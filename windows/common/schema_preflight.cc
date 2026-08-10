#include "schema_preflight.h"

#include <algorithm>

namespace rimewin {
namespace {

std::string TrimBoth(const std::string& s) {
  size_t a = 0;
  size_t b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
  return s.substr(a, b - a);
}

// 去掉行尾註解。**只認「空白 + #」** —— `#` 出現在值裡面
// (例如顏色 `0xff0000` 沒有 #,但 patch 的值有可能)不該被當成註解起點。
std::string StripInlineComment(const std::string& s) {
  bool in_single = false, in_double = false;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '\'' && !in_double) in_single = !in_single;
    else if (c == '"' && !in_single) in_double = !in_double;
    else if (c == '#' && !in_single && !in_double) {
      if (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t') return s.substr(0, i);
    }
  }
  return s;
}

std::string Unquote(const std::string& s) {
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

bool StartsWith(const std::string& s, const char* p) {
  return s.compare(0, std::char_traits<char>::length(p), p) == 0;
}

}  // namespace

std::vector<YamlPair> ScanSchemaYaml(const std::string& text) {
  std::vector<YamlPair> out;
  // (縮排, 鍵)。用來回答「這個 `- 項目` 屬於哪一個鍵」。
  std::vector<std::pair<int, std::string>> stack;
  std::string top_key;

  size_t pos = 0;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    const std::string raw = text.substr(pos, nl - pos);
    pos = nl + 1;

    int indent = 0;
    while (indent < static_cast<int>(raw.size()) &&
           (raw[indent] == ' ' || raw[indent] == '\t')) {
      ++indent;
    }
    const std::string body = StripInlineComment(raw.substr(indent));
    const std::string line = TrimBoth(body);
    if (line.empty()) continue;
    if (line[0] == '#') continue;
    if (line == "---" || line == "...") continue;

    // ── 序列項目 ────────────────────────────────────────────
    if (line[0] == '-' && (line.size() == 1 || line[1] == ' ')) {
      const std::string item = TrimBoth(line.substr(1));
      while (!stack.empty() && stack.back().first > indent) stack.pop_back();
      const std::string parent = stack.empty() ? std::string() : stack.back().second;
      if (item.empty()) continue;
      // `- key: value` 這種序列裡的對映。
      const size_t colon = item.find(": ");
      const bool ends_colon = item.size() > 1 && item.back() == ':';
      if (colon != std::string::npos || ends_colon) {
        const size_t c = (colon != std::string::npos) ? colon : item.size() - 1;
        const std::string k = TrimBoth(item.substr(0, c));
        const std::string v = TrimBoth(item.substr(c + 1));
        out.push_back({top_key, k, Unquote(TrimBoth(v))});
        continue;
      }
      out.push_back({top_key, parent, Unquote(item)});
      continue;
    }

    // ── `鍵: 值` ────────────────────────────────────────────
    //
    // ⚠ 分界是**第一個「冒號 + 空白」**或行尾的冒號,不是第一個冒號 ——
    //   `__include: symbols.yaml:/punctuator` 的值裡本來就有冒號。
    size_t c = std::string::npos;
    const size_t cs = line.find(": ");
    if (cs != std::string::npos) c = cs;
    else if (line.back() == ':') c = line.size() - 1;
    if (c == std::string::npos) continue;

    const std::string key = TrimBoth(line.substr(0, c));
    std::string value = TrimBoth(line.substr(c + 1));
    if (key.empty()) continue;

    if (indent == 0) top_key = key;

    while (!stack.empty() && stack.back().first >= indent) stack.pop_back();
    stack.push_back({indent, key});

    if (value.empty()) continue;

    // 流式序列 `dependencies: [stroke, luna_pinyin]`
    if (value.front() == '[') {
      std::string inner = value.substr(1);
      const size_t close = inner.rfind(']');
      if (close != std::string::npos) inner = inner.substr(0, close);
      size_t s = 0;
      while (s <= inner.size()) {
        size_t comma = inner.find(',', s);
        if (comma == std::string::npos) comma = inner.size();
        const std::string item = Unquote(TrimBoth(inner.substr(s, comma - s)));
        if (!item.empty()) out.push_back({top_key, key, item});
        s = comma + 1;
      }
      continue;
    }
    out.push_back({top_key, key, Unquote(value)});
  }
  return out;
}

bool ParseIncludeTarget(const std::string& raw, IncludeRef* out) {
  std::string v = TrimBoth(raw);
  const bool optional = !v.empty() && v.back() == '?';
  if (optional) v = TrimBoth(v.substr(0, v.size() - 1));
  const size_t sep = v.find(':');
  // 沒有冒號 → 同一個檔案裡的節點;冒號在開頭 → 同上。
  if (sep == std::string::npos || sep == 0) return false;
  std::string head = TrimBoth(v.substr(0, sep));
  if (head.empty()) return false;
  if (head.size() < 5 || head.compare(head.size() - 5, 5, ".yaml") != 0) {
    head += ".yaml";
  }
  out->file_name = head;
  out->optional = optional;
  return true;
}

bool ReferencesLuaComponent(const std::string& text, std::string* first) {
  static const char* const kComponents[] = {
      "lua_translator", "lua_filter", "lua_processor", "lua_segmentor",
  };
  for (const YamlPair& p : ScanSchemaYaml(text)) {
    for (const char* c : kComponents) {
      if (StartsWith(p.value, c)) {
        if (first != nullptr) *first = p.value;
        return true;
      }
    }
  }
  return false;
}

std::vector<PreflightMissing> PreflightReport::Blocking() const {
  std::vector<PreflightMissing> out;
  for (const auto& m : missing) {
    if (m.severity == PreflightSeverity::kBlocking) out.push_back(m);
  }
  return out;
}

std::vector<PreflightMissing> PreflightReport::Warnings() const {
  std::vector<PreflightMissing> out;
  for (const auto& m : missing) {
    if (m.severity == PreflightSeverity::kWarning) out.push_back(m);
  }
  return out;
}

bool PreflightReport::Ok() const { return Blocking().empty(); }

PreflightReport PreflightSchemaText(const std::string& schema_id,
                                    const std::string& text,
                                    const FileExistsFn& exists,
                                    bool lua_supported) {
  PreflightReport report;
  report.schema_id = schema_id;

  const std::vector<YamlPair> pairs = ScanSchemaYaml(text);

  // 宣告的 schema_id(訊息裡用它,而不是檔名)。
  std::string declared = schema_id;
  for (const YamlPair& p : pairs) {
    if (p.top_key == "schema" && p.key == "schema_id" && !p.value.empty()) {
      declared = p.value;
      break;
    }
  }
  report.schema_id = declared;

  // 想要的檔案 → (種類, 嚴重度)。同一個檔案被兩處引用時取比較嚴的。
  std::vector<PreflightMissing> wanted;
  auto put = [&wanted, &declared](const std::string& name, PreflightKind kind,
                                  PreflightSeverity sev) {
    for (auto& w : wanted) {
      if (w.name == name) {
        if (w.severity == PreflightSeverity::kWarning &&
            sev == PreflightSeverity::kBlocking) {
          w.severity = sev;
          w.kind = kind;
        }
        return;
      }
    }
    PreflightMissing m;
    m.name = name;
    m.kind = kind;
    m.severity = sev;
    m.referenced_by = declared;
    wanted.push_back(m);
  };

  for (const YamlPair& p : pairs) {
    if (p.value.empty()) continue;
    if (p.key == "dictionary") {
      // ⚠ 只有 `translator/dictionary` 是部署期真的會編的那一本。
      //   別處的 dictionary(reverse_lookup、custom_phrase、
      //   translator@xxx……)librime 在部署期連讀都沒讀。
      put(p.value + ".dict.yaml", PreflightKind::kDictionary,
          p.top_key == "translator" ? PreflightSeverity::kBlocking
                                    : PreflightSeverity::kWarning);
    } else if (p.key == "import_preset") {
      put(p.value + ".yaml", PreflightKind::kConfig, PreflightSeverity::kBlocking);
    } else if (p.key == "__include" || p.key == "__patch") {
      IncludeRef ref;
      if (ParseIncludeTarget(p.value, &ref)) {
        put(ref.file_name, PreflightKind::kConfig,
            ref.optional ? PreflightSeverity::kWarning : PreflightSeverity::kBlocking);
      }
    } else if (p.key == "language") {
      if (p.value.find('-') != std::string::npos) {
        put(p.value + ".gram", PreflightKind::kGrammar, PreflightSeverity::kWarning);
      }
    } else if (p.key == "dependencies") {
      put(p.value + ".schema.yaml", PreflightKind::kSchema,
          PreflightSeverity::kWarning);
    }
  }

  for (const PreflightMissing& m : wanted) {
    if (!exists(m.name)) report.missing.push_back(m);
  }

  // lua。見標頭:沒有 librime-lua 時,這種方案會「部署成功但沒有候選」。
  if (!lua_supported) {
    std::string component;
    if (ReferencesLuaComponent(text, &component)) {
      PreflightMissing m;
      m.kind = PreflightKind::kLua;
      m.name = component;
      m.referenced_by = declared;
      m.severity = PreflightSeverity::kBlocking;
      report.missing.push_back(m);
    }
  }
  return report;
}

}  // namespace rimewin
