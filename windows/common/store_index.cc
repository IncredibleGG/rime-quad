#include "store_index.h"

#include <algorithm>
#include <cstdio>
#include <set>

#include "mini_json.h"
#include "schema_list_patch.h"
#include "sha256.h"

namespace rimewin {
namespace {

std::string LowerAscii(const std::string& s) {
  std::string o = s;
  for (char& c : o) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return o;
}

}  // namespace

const StorePackage* StoreIndex::ById(const std::string& id) const {
  for (const auto& p : packages) {
    if (p.id == id) return &p;
  }
  return nullptr;
}

const StorePackage* StoreIndex::ProvidingSchema(const std::string& sid) const {
  for (const auto& p : packages) {
    for (const auto& s : p.schemas) {
      if (s.id == sid) return &p;
    }
  }
  return nullptr;
}

std::vector<const StoreCategory*> StoreIndex::VisibleCategories() const {
  std::vector<const StoreCategory*> out;
  for (const auto& c : categories) {
    if (!c.hidden) out.push_back(&c);
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const StoreCategory* a, const StoreCategory* b) {
                     return a->order < b->order;
                   });
  return out;
}

std::vector<const StorePackage*> StoreIndex::PackagesIn(
    const std::string& category) const {
  std::vector<const StorePackage*> out;
  for (const auto& p : packages) {
    if (p.category == category) out.push_back(&p);
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const StorePackage* a, const StorePackage* b) {
                     if (a->recommended != b->recommended) return a->recommended;
                     return a->name < b->name;
                   });
  return out;
}

IndexParseResult ParseStoreIndex(const std::string& json_text) {
  IndexParseResult r;
  Json root;
  std::string err;
  if (!ParseJson(json_text, &root, &err)) {
    r.error = "the index is not valid JSON: " + err;
    return r;
  }
  if (!root.IsObject()) {
    r.error = "the top level of the index must be an object";
    return r;
  }
  const Json* fv = root.Find("format_version");
  if (fv == nullptr || fv->type != Json::Type::kNumber) {
    r.error = "the index has no format_version";
    return r;
  }
  const int version = static_cast<int>(root.Int("format_version", 0));
  if (version != kStoreIndexFormatVersion) {
    r.error = "the index is format_version " + std::to_string(version) +
              " but this build only understands " +
              std::to_string(kStoreIndexFormatVersion);
    return r;
  }

  StoreIndex idx;
  idx.format_version = version;
  idx.generated_at = root.Str("generated_at");
  idx.base_url = root.Str("base_url");

  int ci = 0;
  for (const Json& node : root.Array("categories")) {
    const std::string id = node.Str("id");
    if (id.empty()) {
      r.warnings.push_back("categories[" + std::to_string(ci) +
                           "] has no id; skipped");
      ++ci;
      continue;
    }
    StoreCategory c;
    c.id = id;
    c.name = node.Str("name", id);
    c.order = static_cast<int>(node.Int("order", 100 + ci));
    c.hidden = node.Bool("hidden", false);
    idx.categories.push_back(std::move(c));
    ++ci;
  }
  std::set<std::string> category_ids;
  for (const auto& c : idx.categories) category_ids.insert(c.id);

  std::set<std::string> seen;
  int pi = -1;
  for (const Json& node : root.Array("packages")) {
    ++pi;
    const std::string where = "packages[" + std::to_string(pi) + "]";
    const std::string id = node.Str("id");
    if (id.empty()) {
      r.warnings.push_back(where + " has no id; skipped");
      continue;
    }
    // ⚠ id 會被寫進 default.custom.yaml 與檔案路徑。它從網路來。
    if (!IsPlausibleSchemaId(id)) {
      r.warnings.push_back("package '" + id +
                           "' has an id with characters we will not write to "
                           "a path or a YAML file; skipped");
      continue;
    }
    if (!seen.insert(id).second) {
      r.warnings.push_back(where + " repeats the id '" + id +
                           "'; the later one is skipped");
      continue;
    }
    const std::string file = node.Str("file");
    if (file.empty()) {
      r.warnings.push_back("package '" + id + "' has no file; skipped");
      continue;
    }
    const std::string sha = LowerAscii(node.Str("sha256"));
    if (sha.empty()) {
      // 規範:必須驗 sha256。沒有它就沒有辦法驗。
      r.warnings.push_back("package '" + id +
                           "' has no sha256, so its integrity cannot be "
                           "checked; skipped");
      continue;
    }
    if (!LooksLikeSha256Hex(sha)) {
      r.warnings.push_back("package '" + id +
                           "' has a sha256 that is not 64 hex characters; skipped");
      continue;
    }

    StorePackage p;
    p.id = id;
    p.name = node.Str("name", id);
    p.category = node.Str("category", "other");
    if (!category_ids.empty() && category_ids.count(p.category) == 0) {
      r.warnings.push_back("package '" + id + "' has category '" + p.category +
                           "', which is not in categories; moved to other");
      p.category = "other";
    }
    p.description = node.Str("description");
    p.upstream = node.Str("upstream");
    p.upstream_commit = node.Str("upstream_commit");
    p.license = node.Str("license");
    p.file = file;
    p.size = node.Int("size", 0);
    p.sha256 = sha;
    for (const Json& s : node.Array("schemas")) {
      const std::string sid = s.Str("id");
      if (sid.empty()) continue;
      if (!IsPlausibleSchemaId(sid)) {
        r.warnings.push_back("package '" + id + "' lists a schema id '" + sid +
                             "' we will not write to a path; that schema is skipped");
        continue;
      }
      StoreSchemaRef ref;
      ref.id = sid;
      ref.name = s.Str("name", sid);
      ref.language = s.Str("language");
      p.schemas.push_back(std::move(ref));
    }
    p.requires_ids = node.Strings("requires");
    const Json* verified = node.Find("verified");
    p.verified_deployed = verified != nullptr && verified->Bool("deployed", false);
    p.recommended_layout = node.Str("recommended_layout");
    p.layout_note = node.Str("layout_note");
    p.recommended = node.Bool("recommended", false);
    idx.packages.push_back(std::move(p));
  }

  // requires 指向不存在的套件:不在這裡淘汰(相依可能是留給之後的索引版本),
  // 但要在解析階段就留下警告,否則使用者按下「安裝」才第一次看到問題。
  std::set<std::string> ids;
  for (const auto& p : idx.packages) ids.insert(p.id);
  for (const auto& p : idx.packages) {
    for (const auto& req : p.requires_ids) {
      if (ids.count(req) == 0) {
        r.warnings.push_back("package '" + p.id + "' requires '" + req +
                             "', which is not in the index");
      }
    }
  }

  if (idx.packages.empty()) {
    r.error = "the index has no usable package";
    return r;
  }

  r.ok = true;
  r.index = std::move(idx);
  return r;
}

DependencyResult ResolveDependencies(const StoreIndex& index,
                                     const std::vector<std::string>& selected,
                                     const std::vector<std::string>& installed) {
  DependencyResult result;
  const std::set<std::string> have(installed.begin(), installed.end());
  std::set<std::string> done;
  std::vector<std::string> on_stack;
  std::vector<std::string> skipped;

  // 遞迴用明確的 lambda,深度由套件圖決定(34 個節點),不會失控。
  struct Visitor {
    const StoreIndex& index;
    const std::set<std::string>& have;
    std::set<std::string>& done;
    std::vector<std::string>& on_stack;
    std::vector<std::string>& skipped;
    DependencyResult& result;

    bool Visit(const std::string& id, const std::string* required_by) {
      if (have.count(id) != 0) {
        if (std::find(skipped.begin(), skipped.end(), id) == skipped.end()) {
          skipped.push_back(id);
        }
        return true;
      }
      if (done.count(id) != 0) return true;
      if (std::find(on_stack.begin(), on_stack.end(), id) != on_stack.end()) {
        // 循環:記下來給畫面顯示,然後就地收手。這個套件在遞迴回捲時
        // 一定會被加進 to_download,不會漏掉。
        if (std::find(result.plan.cycles.begin(), result.plan.cycles.end(), id) ==
            result.plan.cycles.end()) {
          result.plan.cycles.push_back(id);
        }
        return true;
      }
      const StorePackage* pkg = index.ById(id);
      if (pkg == nullptr) {
        if (required_by == nullptr) {
          result.status = DependencyResult::Status::kUnknownPackage;
          result.missing = id;
        } else {
          result.status = DependencyResult::Status::kMissingDependency;
          result.missing = id;
          result.required_by = *required_by;
        }
        return false;
      }
      on_stack.push_back(id);
      for (const auto& dep : pkg->requires_ids) {
        if (!Visit(dep, &id)) return false;
      }
      on_stack.pop_back();
      done.insert(id);
      result.plan.to_download.push_back(id);
      result.plan.total_bytes += pkg->size;
      return true;
    }
  };

  Visitor v{index, have, done, on_stack, skipped, result};
  for (const auto& id : selected) {
    if (!v.Visit(id, nullptr)) {
      result.plan = DependencyPlan();
      return result;
    }
  }
  result.plan.already_installed = skipped;
  return result;
}

std::vector<std::string> DependentsOf(const StoreIndex& index,
                                      const std::string& candidate,
                                      const std::vector<std::string>& installed) {
  std::vector<std::string> out;
  for (const auto& id : installed) {
    if (id == candidate) continue;
    const StorePackage* p = index.ById(id);
    if (p == nullptr) continue;
    if (std::find(p->requires_ids.begin(), p->requires_ids.end(), candidate) !=
        p->requires_ids.end()) {
      out.push_back(id);
    }
  }
  return out;
}

std::string FormatBytes(int64_t bytes) {
  char buf[64];
  if (bytes >= 1024LL * 1024 * 1024) {
    std::snprintf(buf, sizeof(buf), "%.2f GB",
                  static_cast<double>(bytes) / 1024.0 / 1024.0 / 1024.0);
  } else if (bytes >= 1024LL * 1024) {
    std::snprintf(buf, sizeof(buf), "%.1f MB",
                  static_cast<double>(bytes) / 1024.0 / 1024.0);
  } else if (bytes >= 1024) {
    std::snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    std::snprintf(buf, sizeof(buf), "%lld B", static_cast<long long>(bytes));
  }
  return buf;
}

}  // namespace rimewin
