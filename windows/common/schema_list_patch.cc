// windows/common/schema_list_patch.cc — 純邏輯,不含任何平台 API。

#include "schema_list_patch.h"

#include <cstddef>

namespace rimewin {
namespace {

struct Line {
  std::string text;   // 不含換行
  size_t indent = 0;  // 前導空白數(tab 一律當成不合法,見下)
  bool blank = false;
  bool comment = false;
  bool has_tab = false;
};

std::vector<Line> Split(const std::string& s) {
  std::vector<Line> out;
  size_t pos = 0;
  while (pos <= s.size()) {
    const size_t nl = s.find('\n', pos);
    std::string t =
        s.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = (nl == std::string::npos) ? s.size() + 1 : nl + 1;
    if (!t.empty() && t.back() == '\r') t.pop_back();
    Line l;
    l.text = t;
    size_t i = 0;
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) {
      if (t[i] == '\t') l.has_tab = true;
      ++i;
    }
    l.indent = i;
    l.blank = (i == t.size());
    l.comment = (!l.blank && t[i] == '#');
    out.push_back(l);
    if (nl == std::string::npos) break;
  }
  // 尾端如果本來就有換行,Split 會多產生一個空行。保留它,重組時才對得回去。
  return out;
}

std::string Join(const std::vector<std::string>& lines) {
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    out += lines[i];
    if (i + 1 < lines.size()) out += "\n";
  }
  return out;
}

// 從 "    - schema: luna_pinyin    # 註解" 取出 id。取不到回空字串。
std::string SchemaIdOf(const std::string& text) {
  size_t i = text.find_first_not_of(" \t");
  if (i == std::string::npos || text[i] != '-') return std::string();
  ++i;
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
  const std::string key = "schema:";
  if (text.compare(i, key.size(), key) != 0) return std::string();
  i += key.size();
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
  size_t e = i;
  while (e < text.size() && text[e] != ' ' && text[e] != '\t' && text[e] != '#')
    ++e;
  return text.substr(i, e - i);
}

bool StartsWithKeyword(const Line& l, const char* kw) {
  const std::string t = l.text.substr(l.indent);
  const size_t n = std::string(kw).size();
  if (t.compare(0, n, kw) != 0) return false;
  // 後面必須是 ':',不然 "patch_something:" 也會中。
  return t.size() > n ? t[n] == ':' : false;
}

}  // namespace

bool IsPlausibleSchemaId(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  for (char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    if (!ok) return false;
  }
  // 開頭是 '.' 或含 ".." 的話,librime 會拿它去組檔名。
  if (id[0] == '.' || id.find("..") != std::string::npos) return false;
  return true;
}

std::vector<std::string> ReadSchemaList(const std::string& yaml, bool* found) {
  std::vector<std::string> out;
  if (found) *found = false;
  const std::vector<Line> lines = Split(yaml);
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].blank || lines[i].comment) continue;
    if (!StartsWithKeyword(lines[i], "schema_list")) continue;
    if (found) *found = true;
    const size_t base = lines[i].indent;
    for (size_t j = i + 1; j < lines.size(); ++j) {
      if (lines[j].blank || lines[j].comment) continue;
      if (lines[j].indent <= base) break;  // 這一段結束了
      const std::string id = SchemaIdOf(lines[j].text);
      if (!id.empty()) out.push_back(id);
    }
    break;
  }
  return out;
}

PatchResult WriteSchemaList(const std::string& yaml,
                            const std::vector<std::string>& order,
                            std::string* out) {
  if (order.empty()) return PatchResult::kEmptyOrder;
  for (const std::string& id : order)
    if (!IsPlausibleSchemaId(id)) return PatchResult::kBadSchemaId;

  const std::vector<Line> lines = Split(yaml);

  size_t list_at = std::string::npos;
  size_t patch_at = std::string::npos;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].blank || lines[i].comment) continue;
    // ⚠ tab 縮排的 YAML 我們一律不碰。YAML 規範根本不允許 tab 縮排,
    //   而我們的縮排判斷會把它算錯 —— 算錯的結果是把別人的設定吃掉。
    if (lines[i].has_tab) return PatchResult::kNoPatchSection;
    if (list_at == std::string::npos && StartsWithKeyword(lines[i], "schema_list"))
      list_at = i;
    if (patch_at == std::string::npos && lines[i].indent == 0 &&
        StartsWithKeyword(lines[i], "patch"))
      patch_at = i;
  }

  std::vector<std::string> result;
  if (list_at != std::string::npos) {
    const size_t base = lines[list_at].indent;
    // 項目的縮排:沿用原本第一個項目的,沒有項目就 base + 2。
    size_t item_indent = base + 2;
    size_t end = list_at + 1;
    for (size_t j = list_at + 1; j < lines.size(); ++j) {
      if (lines[j].blank || lines[j].comment) {
        // 中間的空行與註解跟著這一段一起被換掉 —— 它們講的是舊的順序。
        end = j + 1;
        continue;
      }
      if (lines[j].indent <= base) break;
      if (!SchemaIdOf(lines[j].text).empty()) item_indent = lines[j].indent;
      end = j + 1;
    }
    // end 可能把尾巴的空行也吃進去了,退回到最後一個真的屬於這一段的行。
    while (end > list_at + 1 &&
           (lines[end - 1].blank ||
            (lines[end - 1].comment && lines[end - 1].indent <= base)))
      --end;

    for (size_t i = 0; i < list_at + 1; ++i) result.push_back(lines[i].text);
    for (const std::string& id : order)
      result.push_back(std::string(item_indent, ' ') + "- schema: " + id);
    for (size_t i = end; i < lines.size(); ++i) result.push_back(lines[i].text);
  } else if (patch_at != std::string::npos) {
    for (size_t i = 0; i < patch_at + 1; ++i) result.push_back(lines[i].text);
    result.push_back("  schema_list:");
    for (const std::string& id : order)
      result.push_back("    - schema: " + id);
    for (size_t i = patch_at + 1; i < lines.size(); ++i)
      result.push_back(lines[i].text);
  } else {
    // 連 patch: 都沒有。檔案的結構我們不認得 —— **不要猜**。
    // 猜錯的代價是使用者一啟動就部署失敗,而且設定介面也打不開。
    return PatchResult::kNoPatchSection;
  }

  *out = Join(result);
  return PatchResult::kOk;
}

namespace {

// "  menu/page_size: 9   # 註解" → key = "menu/page_size", value = "9"
bool SplitScalar(const std::string& text, std::string* key, std::string* value) {
  size_t i = text.find_first_not_of(" \t");
  if (i == std::string::npos || text[i] == '#' || text[i] == '-') return false;
  const size_t colon = text.find(':', i);
  if (colon == std::string::npos) return false;
  *key = text.substr(i, colon - i);
  size_t v = colon + 1;
  while (v < text.size() && (text[v] == ' ' || text[v] == '\t')) ++v;
  size_t e = text.size();
  while (e > v && (text[e - 1] == ' ' || text[e - 1] == '\t')) --e;
  *value = text.substr(v, e - v);
  return !key->empty();
}

}  // namespace

std::string ReadPatchScalar(const std::string& yaml, const std::string& key) {
  const std::vector<Line> lines = Split(yaml);
  bool in_patch = false;
  for (const Line& l : lines) {
    if (l.blank || l.comment) continue;
    if (l.indent == 0) {
      in_patch = StartsWithKeyword(l, "patch");
      continue;
    }
    if (!in_patch) continue;
    std::string k, v;
    if (SplitScalar(l.text, &k, &v) && k == key) return v;
  }
  return std::string();
}

PatchResult UpsertPatchScalar(const std::string& yaml, const std::string& key,
                              const std::string& value, std::string* out) {
  if (key.empty()) return PatchResult::kBadSchemaId;
  // 值會被原樣寫進 YAML。換行就能偽造出別的設定 —— 這裡的值來自
  // 設定介面的下拉選單,但那不是把檢查省掉的理由。
  if (value.find('\n') != std::string::npos ||
      value.find('\r') != std::string::npos || value.find('#') != std::string::npos)
    return PatchResult::kBadSchemaId;

  const std::vector<Line> lines = Split(yaml);
  size_t patch_at = std::string::npos, key_at = std::string::npos;
  size_t key_indent = 2;
  bool in_patch = false;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].blank || lines[i].comment) continue;
    if (lines[i].has_tab) return PatchResult::kNoPatchSection;
    if (lines[i].indent == 0) {
      in_patch = StartsWithKeyword(lines[i], "patch");
      if (in_patch && patch_at == std::string::npos) patch_at = i;
      continue;
    }
    if (!in_patch || key_at != std::string::npos) continue;
    std::string k, v;
    if (SplitScalar(lines[i].text, &k, &v) && k == key) {
      key_at = i;
      key_indent = lines[i].indent;
    }
  }
  if (patch_at == std::string::npos) return PatchResult::kNoPatchSection;

  std::vector<std::string> result;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i == key_at) {
      if (!value.empty())
        result.push_back(std::string(key_indent, ' ') + key + ": " + value);
      // value 為空 = 刪掉這一行(= 回到「沒設過」)
      continue;
    }
    result.push_back(lines[i].text);
    if (i == patch_at && key_at == std::string::npos && !value.empty())
      result.push_back("  " + key + ": " + value);
  }
  *out = Join(result);
  return PatchResult::kOk;
}

}  // namespace rimewin
