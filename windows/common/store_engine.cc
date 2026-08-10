#include "store_engine.h"

#include <algorithm>

#include "net_policy.h"
#include "schema_list_patch.h"
#include "sha256.h"
#include "zip_reader.h"

namespace rimewin {
namespace {

// 單一套件下載上限。索引宣告的 size 只是參考,這個是硬牆。
// 實測最大的一包(moran)是 31MB。
constexpr int64_t kStoreMaxPackageBytes = 128LL * 1024 * 1024;

const char kDefaultCustom[] = "default.custom.yaml";

std::string Join(const std::vector<std::string>& v, char sep) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out.push_back(sep);
    out += v[i];
  }
  return out;
}

std::vector<std::string> SplitNonEmpty(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t p = s.find(sep, start);
    if (p == std::string::npos) p = s.size();
    if (p > start) out.push_back(s.substr(start, p - start));
    start = p + 1;
  }
  return out;
}

// 紀錄裡的欄位不可以含分隔字元。**這是安全控制,不是排版**:
// 套件 id 與檔名都來自索引(網路),一個 tab 或換行就能在紀錄裡
// 偽造出多一筆看起來無害的安裝。
bool FieldIsClean(const std::string& s) {
  for (char c : s) {
    if (c == '\t' || c == '\n' || c == '\r' || c == ',') return false;
  }
  return true;
}

}  // namespace

const char* StoreFailureToken(StoreFailure f) {
  switch (f) {
    case StoreFailure::kNone: return "NONE";
    case StoreFailure::kSwitchOff: return "SWITCH_OFF";
    case StoreFailure::kNetwork: return "NETWORK";
    case StoreFailure::kIntegrity: return "INTEGRITY";
    case StoreFailure::kArchive: return "ARCHIVE";
    case StoreFailure::kDisk: return "DISK";
    case StoreFailure::kIndex: return "INDEX";
    case StoreFailure::kUnknownPackage: return "UNKNOWN_PACKAGE";
    case StoreFailure::kPreflight: return "PREFLIGHT";
    case StoreFailure::kPatch: return "PATCH";
    case StoreFailure::kDeploy: return "DEPLOY";
    case StoreFailure::kRollbackFailed: return "ROLLBACK_FAILED";
  }
  return "NONE";
}

/* ═══════════════════════════ 安裝紀錄 ═══════════════════════════ */

std::string EncodeInstalledLine(const InstalledPackage& p) {
  return p.id + "\t" + p.upstream_commit + "\t" + p.sha256 + "\t" +
         std::to_string(p.installed_at_ms) + "\t" + Join(p.schemas, ',') + "\t" +
         Join(p.files, ',');
}

bool DecodeInstalledLine(const std::string& line, InstalledPackage* out) {
  std::vector<std::string> f;
  size_t start = 0;
  while (true) {
    const size_t p = line.find('\t', start);
    if (p == std::string::npos) {
      f.push_back(line.substr(start));
      break;
    }
    f.push_back(line.substr(start, p - start));
    start = p + 1;
  }
  if (f.size() != 6) return false;
  if (f[0].empty()) return false;
  // 時間戳必須是數字。
  if (f[3].empty()) return false;
  int64_t at = 0;
  for (char c : f[3]) {
    if (c < '0' || c > '9') return false;
    at = at * 10 + (c - '0');
  }
  out->id = f[0];
  out->upstream_commit = f[1];
  out->sha256 = f[2];
  out->installed_at_ms = at;
  out->schemas = SplitNonEmpty(f[4], ',');
  out->files = SplitNonEmpty(f[5], ',');
  return true;
}

std::vector<InstalledPackage> DecodeInstalled(const std::string& text) {
  std::vector<InstalledPackage> out;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    std::string line = text.substr(pos, nl - pos);
    pos = nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    InstalledPackage p;
    // 壞行安靜丟掉。一行壞掉不可以毀掉整份紀錄。
    if (DecodeInstalledLine(line, &p)) out.push_back(std::move(p));
  }
  return out;
}

std::string EncodeInstalled(const std::vector<InstalledPackage>& v) {
  std::string out;
  for (const auto& p : v) {
    out += EncodeInstalledLine(p);
    out.push_back('\n');
  }
  return out;
}

/* ═══════════════════════════ 流程 ═══════════════════════════════ */

std::string StorePackageUrl(const std::string& index_url,
                            const std::string& base_url, const std::string& file) {
  return ResolvePackageUrl(index_url, base_url, file);
}

StoreOutcome FetchStoreIndex(const StoreDeps& deps, const std::string& index_url,
                             StoreIndex* out, std::vector<std::string>* warnings) {
  const StoreNetwork::Result r =
      deps.net->Fetch(index_url, /*is_package=*/false, "", kMaxIndexBytes);
  if (!r.ok) {
    return StoreOutcome::Fail(
        r.blocked ? StoreFailure::kSwitchOff : StoreFailure::kNetwork, r.message,
        HostOf(index_url));
  }
  const IndexParseResult parsed = ParseStoreIndex(r.body);
  if (!parsed.ok) {
    return StoreOutcome::Fail(StoreFailure::kIndex, parsed.error);
  }
  *out = parsed.index;
  if (warnings != nullptr) *warnings = parsed.warnings;
  return StoreOutcome::Ok();
}

StoreOutcome InstallPackages(const StoreDeps& deps, const std::string& index_url,
                             const StoreIndex& index, const DependencyPlan& plan,
                             std::vector<InstalledPackage>* registry,
                             const StoreProgressFn& progress) {
  const int total = static_cast<int>(plan.to_download.size());
  int ordinal = 0;
  for (const std::string& pkg_id : plan.to_download) {
    ++ordinal;
    const StorePackage* pkg = index.ById(pkg_id);
    if (pkg == nullptr) {
      return StoreOutcome::Fail(StoreFailure::kUnknownPackage,
                                "the plan names a package that is not in the index",
                                pkg_id);
    }

    StoreProgress pr;
    pr.phase = StoreProgress::Phase::kDownloading;
    pr.package = pkg->name;
    pr.ordinal = ordinal;
    pr.total = total;
    pr.bytes = pkg->size;
    if (progress) progress(pr);

    const std::string url = StorePackageUrl(index_url, index.base_url, pkg->file);
    const StoreNetwork::Result dl =
        deps.net->Fetch(url, /*is_package=*/true, pkg->name, kStoreMaxPackageBytes);
    if (!dl.ok) {
      return StoreOutcome::Fail(
          dl.blocked ? StoreFailure::kSwitchOff : StoreFailure::kNetwork,
          dl.message, pkg->name);
    }

    // ── sha256 ────────────────────────────────────────────────
    //
    // ⚠ 它擋的是傳輸損壞與截斷,**不是**惡意的來源 —— 那個 sha256 本身
    //   來自同一條連線(見 service/net_gate.h 的信任錨)。不要在任何
    //   地方因為這一步而寫「已驗證」。
    pr.phase = StoreProgress::Phase::kVerifying;
    pr.read = static_cast<int64_t>(dl.body.size());
    if (progress) progress(pr);
    const std::string got = Sha256::HexOf(dl.body);
    if (!Sha256HexEqual(got, pkg->sha256)) {
      return StoreOutcome::Fail(
          StoreFailure::kIntegrity,
          "sha256 of the downloaded bytes is " + got + " but the index says " +
              pkg->sha256,
          pkg->name);
    }

    // ── zip 安全檢查 ──────────────────────────────────────────
    pr.phase = StoreProgress::Phase::kExtracting;
    if (progress) progress(pr);
    const ArchiveReport report = InspectArchive(dl.body);
    if (!report.IsSafe()) {
      std::string detail = report.rejections.front().ToString();
      if (report.rejections.size() > 1) {
        detail += " (and " + std::to_string(report.rejections.size() - 1) +
                  " more problem(s))";
      }
      return StoreOutcome::Fail(StoreFailure::kArchive, detail, pkg->name);
    }

    std::vector<ZipEntry> entries;
    std::string err;
    if (!ReadZipCentralDirectory(dl.body, &entries, &err)) {
      // InspectArchive 已經讀過一次,走到這裡代表兩次讀出不同結果。
      return StoreOutcome::Fail(StoreFailure::kArchive, err, pkg->name);
    }

    // 先全部解到記憶體再寫檔:一個寫到一半被截斷的 dict.yaml 會讓之後
    // 每一次部署都失敗,而那不是「無害的半成品」。
    std::vector<std::pair<std::string, std::string>> staged;
    staged.reserve(report.entries.size());
    for (const SafeEntry& se : report.entries) {
      if (se.index >= entries.size()) {
        return StoreOutcome::Fail(StoreFailure::kArchive,
                                  "entry index out of range", pkg->name);
      }
      std::string body;
      if (!ExtractZipEntry(dl.body, entries[se.index], se.uncompressed_size + 1,
                           &body, &err)) {
        return StoreOutcome::Fail(
            err.find("CRC32") != std::string::npos ? StoreFailure::kIntegrity
                                                   : StoreFailure::kArchive,
            se.name + ": " + err, pkg->name);
      }
      staged.emplace_back(se.name, std::move(body));
    }

    std::vector<std::string> written;
    for (const auto& kv : staged) {
      if (!FieldIsClean(kv.first)) {
        return StoreOutcome::Fail(
            StoreFailure::kArchive,
            "the entry name contains a character we will not put in the "
            "installed-packages record",
            kv.first);
      }
      if (!deps.fs->WriteUserFile(kv.first, kv.second)) {
        return StoreOutcome::Fail(StoreFailure::kDisk,
                                  "could not write " + kv.first, pkg->name);
      }
      written.push_back(kv.first);
    }

    InstalledPackage rec;
    rec.id = pkg->id;
    rec.upstream_commit = pkg->upstream_commit;
    rec.sha256 = pkg->sha256;
    rec.installed_at_ms = deps.now_ms ? deps.now_ms() : 0;
    for (const auto& s : pkg->schemas) rec.schemas.push_back(s.id);
    rec.files = written;
    if (!FieldIsClean(rec.id) || !FieldIsClean(rec.upstream_commit)) {
      rec.upstream_commit.clear();
    }
    bool replaced = false;
    for (auto& existing : *registry) {
      if (existing.id == rec.id) {
        existing = rec;
        replaced = true;
        break;
      }
    }
    if (!replaced) registry->push_back(rec);
  }
  return StoreOutcome::Ok();
}

StoreOutcome EnableSchemas(const StoreDeps& deps,
                           const std::vector<std::string>& schema_ids,
                           const StoreProgressFn& progress) {
  if (schema_ids.empty()) {
    return StoreOutcome::Fail(StoreFailure::kPatch,
                              "no schema was selected to enable");
  }

  // ── 1. 預檢。**在動任何檔案之前。** ────────────────────────
  StoreProgress pr;
  pr.phase = StoreProgress::Phase::kPreflight;
  if (progress) progress(pr);

  FileExistsFn exists = [&deps](const std::string& name) {
    return deps.fs->DataFileExists(name);
  };
  std::vector<std::string> notes;
  for (const std::string& sid : schema_ids) {
    std::string text;
    const std::string file = sid + ".schema.yaml";
    if (!deps.fs->ReadDataFile(file, &text)) {
      return StoreOutcome::Fail(StoreFailure::kPreflight,
                                "the schema file " + file + " is not on disk", sid);
    }
    const PreflightReport rep = PreflightSchemaText(sid, text, exists);
    for (const auto& w : rep.Warnings()) {
      notes.push_back(rep.schema_id + " refers to " + w.name +
                      ", which is not on disk");
    }
    const std::vector<PreflightMissing> blocking = rep.Blocking();
    if (!blocking.empty()) {
      const PreflightMissing& m = blocking.front();
      std::string detail;
      if (m.kind == PreflightKind::kLua) {
        detail = "this schema declares the lua component " + m.name +
                 ", and this Windows build has no librime-lua";
      } else {
        detail = "the file " + m.name + " it needs is not on disk";
      }
      if (blocking.size() > 1) {
        detail += " (and " + std::to_string(blocking.size() - 1) + " more)";
      }
      // ⚠ 一個檔案都沒動過,所以 rolled_back 是 false ——
      //   那不是「回滾成功」,是「根本不需要回滾」。
      return StoreOutcome::Fail(StoreFailure::kPreflight, detail, rep.schema_id);
    }
  }

  // ── 2. 改 schema_list ─────────────────────────────────────
  std::string yaml;
  if (!deps.fs->ReadUserFile(kDefaultCustom, &yaml)) {
    return StoreOutcome::Fail(StoreFailure::kPatch,
                              "could not read default.custom.yaml");
  }
  bool found = false;
  std::vector<std::string> order = ReadSchemaList(yaml, &found);
  for (const std::string& sid : schema_ids) {
    if (std::find(order.begin(), order.end(), sid) == order.end()) {
      order.push_back(sid);
    }
  }
  std::string patched;
  const PatchResult pres = WriteSchemaList(yaml, order, &patched);
  if (pres != PatchResult::kOk) {
    const char* why = "default.custom.yaml is not in a shape we dare to rewrite";
    if (pres == PatchResult::kEmptyOrder) why = "the resulting schema list is empty";
    if (pres == PatchResult::kBadSchemaId) why = "one of the schema ids is not a plausible id";
    return StoreOutcome::Fail(StoreFailure::kPatch, why);
  }
  if (!deps.fs->WriteUserFile(kDefaultCustom, patched)) {
    return StoreOutcome::Fail(StoreFailure::kPatch,
                              "could not write default.custom.yaml");
  }

  // ── 3. 部署,失敗就改回去 ─────────────────────────────────
  pr.phase = StoreProgress::Phase::kDeploying;
  if (progress) progress(pr);
  const StoreDeployer::Outcome dep = deps.deployer->DeployAndWait(progress);
  if (!dep.started) {
    // 引擎沒在跑。**這不是部署失敗** —— 檔案已經寫好了,下次啟動就會生效,
    // 所以不回滾,但也不可以說成功。
    StoreOutcome o = StoreOutcome::Fail(
        StoreFailure::kDeploy,
        "the engine is not running, so the new schema list has not been built yet");
    o.notes = notes;
    return o;
  }
  if (!dep.ok) {
    pr.phase = StoreProgress::Phase::kRollingBack;
    if (progress) progress(pr);
    StoreOutcome o;
    o.failure = StoreFailure::kDeploy;
    o.detail = dep.last_error.empty() ? "librime did not say why" : dep.last_error;
    o.notes = notes;
    if (!deps.fs->WriteUserFile(kDefaultCustom, yaml)) {
      // 回滾本身也失敗:使用者現在有一份會讓部署失敗的設定,而我們改不回去。
      o.failure = StoreFailure::kRollbackFailed;
      o.detail += " (and restoring the previous schema list also failed)";
      return o;
    }
    o.rolled_back = true;
    // 把舊的清單建回去。這一步失敗也只能算了 —— 檔案已經是舊的了。
    deps.deployer->DeployAndWait(progress);
    return o;
  }

  StoreOutcome ok = StoreOutcome::Ok();
  ok.notes = notes;
  return ok;
}

StoreOutcome UninstallPackage(const StoreDeps& deps, const std::string& package_id,
                              std::vector<InstalledPackage>* registry,
                              const StoreProgressFn& progress) {
  auto it = std::find_if(registry->begin(), registry->end(),
                         [&package_id](const InstalledPackage& p) {
                           return p.id == package_id;
                         });
  if (it == registry->end()) {
    return StoreOutcome::Fail(StoreFailure::kUnknownPackage,
                              "that package is not in the installed record",
                              package_id);
  }
  const InstalledPackage rec = *it;

  // 先把方案從 schema_list 拿掉(不然刪完檔案下一次部署就失敗)。
  std::string yaml;
  if (!deps.fs->ReadUserFile(kDefaultCustom, &yaml)) {
    return StoreOutcome::Fail(StoreFailure::kPatch,
                              "could not read default.custom.yaml");
  }
  bool found = false;
  std::vector<std::string> order = ReadSchemaList(yaml, &found);
  std::vector<std::string> kept;
  for (const std::string& sid : order) {
    if (std::find(rec.schemas.begin(), rec.schemas.end(), sid) == rec.schemas.end()) {
      kept.push_back(sid);
    }
  }
  if (found && kept.empty()) {
    // ⚠ schema_list 空掉 = 一個方案都沒有 = 使用者連設定介面都救不回來。
    return StoreOutcome::Fail(
        StoreFailure::kPatch,
        "removing this package would leave the schema list empty", package_id);
  }
  if (found && kept.size() != order.size()) {
    std::string patched;
    if (WriteSchemaList(yaml, kept, &patched) != PatchResult::kOk) {
      return StoreOutcome::Fail(
          StoreFailure::kPatch,
          "default.custom.yaml is not in a shape we dare to rewrite");
    }
    if (!deps.fs->WriteUserFile(kDefaultCustom, patched)) {
      return StoreOutcome::Fail(StoreFailure::kPatch,
                                "could not write default.custom.yaml");
    }
  }

  std::vector<std::string> failed;
  for (const std::string& rel : rec.files) {
    if (!deps.fs->RemoveUserFile(rel)) failed.push_back(rel);
  }
  registry->erase(std::find_if(registry->begin(), registry->end(),
                               [&package_id](const InstalledPackage& p) {
                                 return p.id == package_id;
                               }));

  StoreProgress pr;
  pr.phase = StoreProgress::Phase::kDeploying;
  if (progress) progress(pr);
  const StoreDeployer::Outcome dep = deps.deployer->DeployAndWait(progress);

  StoreOutcome out = StoreOutcome::Ok();
  for (const auto& f : failed) out.notes.push_back("could not delete " + f);
  if (dep.started && !dep.ok) {
    // 檔案已經刪了,回滾沒有意義(要回滾就得重新下載)。誠實說出來。
    return StoreOutcome::Fail(
        StoreFailure::kDeploy,
        dep.last_error.empty() ? "librime did not say why" : dep.last_error,
        package_id);
  }
  return out;
}

}  // namespace rimewin
