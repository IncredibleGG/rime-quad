// windows/common/settings.cc — 純邏輯,不含任何平台 API。

#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace rimewin {
namespace {

struct KnownKey {
  const char* key;
  const char* section;  // nullptr = 沿用上一個
};

// 序列化的順序。與檔頭的說明一致,分區只是給人看的。
const KnownKey kKnownKeys[] = {
    {keys::kSchemasFollowInputMode, "輸入方案"},
    {keys::kSchemasPinnedGlobal, nullptr},
    {keys::kSchemasPinnedHant, nullptr},
    {keys::kSchemasPinnedHans, nullptr},
    {keys::kAppearanceCandidateScale, "外觀"},
    {keys::kAppearanceAppearance, nullptr},
    {keys::kAppearanceFloatingBar, nullptr},
    {keys::kAppearanceFloatingBarPos, nullptr},
    {keys::kTextVariant, "文字"},
    {keys::kTextPunctuation, nullptr},
    {keys::kTextShape, nullptr},
    {keys::kTextShiftTapToggle, nullptr},
    {keys::kAdvancedLanguage, "進階"},
    {keys::kNetworkEnabled, "連網"},
    {keys::kStoreIndexUrl, nullptr},
};
constexpr int kKnownKeyCount = static_cast<int>(sizeof(kKnownKeys) /
                                                sizeof(kKnownKeys[0]));

std::string Trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
  return s.substr(b, e - b);
}

// 鍵名允許的字元。限制的目的不是潔癖:值來自使用者可編輯的檔案,
// 而序列化時我們會把它寫回去 —— 鍵名裡混進換行就能偽造出多一行設定。
bool ValidKey(const std::string& k) {
  if (k.empty() || k.size() > 128) return false;
  for (char c : k) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

// 值不得含換行。同上:一行一筆是這個格式唯一的不變式。
std::string CleanValue(const std::string& v) {
  std::string out;
  out.reserve(v.size());
  for (char c : v) {
    if (c == '\n' || c == '\r') continue;
    out.push_back(c);
  }
  if (out.size() > 4096) out.resize(4096);
  return out;
}

bool ParseIntStrict(const std::string& s, int* out) {
  if (s.empty()) return false;
  size_t i = 0;
  bool neg = false;
  if (s[0] == '-') {
    neg = true;
    i = 1;
    if (s.size() == 1) return false;
  }
  long v = 0;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
    if (v > 1000000) return false;
  }
  *out = static_cast<int>(neg ? -v : v);
  return true;
}

int IndexOf(const int* allowed, int n, int value) {
  for (int i = 0; i < n; ++i)
    if (allowed[i] == value) return i;
  return 0;
}

}  // namespace

const int kCandCountValues[] = {0, 3, 5, 7, 9};
const int kCandCountCount = 5;
const int kCandScaleValues[] = {0, 85, 100, 120, 145};
const int kCandScaleCount = 5;

int IndexOfCandCount(int v) { return IndexOf(kCandCountValues, kCandCountCount, v); }
int IndexOfCandScale(int v) { return IndexOf(kCandScaleValues, kCandScaleCount, v); }

int CandCountAtIndex(int i) {
  if (i < 0 || i >= kCandCountCount) return kCandCountValues[0];
  return kCandCountValues[i];
}
int CandScaleAtIndex(int i) {
  if (i < 0 || i >= kCandScaleCount) return kCandScaleValues[0];
  return kCandScaleValues[i];
}

Settings Settings::Parse(const std::string& text) {
  Settings s;
  size_t pos = 0;
  while (pos <= text.size()) {
    const size_t nl = text.find('\n', pos);
    const std::string line =
        Trim(text.substr(pos, nl == std::string::npos ? std::string::npos
                                                      : nl - pos));
    pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
    if (line.empty() || line[0] == '#') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;  // 壞行:丟掉,不讓整份設定失效
    const std::string k = Trim(line.substr(0, eq));
    const std::string v = Trim(line.substr(eq + 1));
    if (!ValidKey(k)) continue;
    s.kv_[k] = CleanValue(v);
  }
  return s;
}

std::string Settings::Serialize() const {
  std::string out =
      "# LuminaKey 設定。由設定介面寫入,也可以自己改。\n"
      "# 規則:**沒有的鍵 = 沒設過 = 跟隨預設**。想回到預設就把那一行刪掉,\n"
      "#       不要寫一個「預設值」進來 —— 那兩件事在這裡是不同的意思。\n";
  // 分區標題只在該區真的有東西時才印 —— 空的標題會讓使用者以為
  // 那一區的設定不見了。
  const char* last_section = nullptr;
  bool section_open = false;
  for (int i = 0; i < kKnownKeyCount; ++i) {
    const KnownKey& kk = kKnownKeys[i];
    if (kk.section) {
      last_section = kk.section;
      section_open = false;
    }
    auto it = kv_.find(kk.key);
    if (it == kv_.end()) continue;
    if (!section_open && last_section) {
      out += "\n# ";
      out += last_section;
      out += "\n";
      section_open = true;
    }
    out += kk.key;
    out += " = ";
    out += it->second;
    out += "\n";
  }
  // 未知的鍵(含 schema.last.*)原樣保留,排序後輸出以求穩定。
  std::vector<std::string> rest;
  for (const auto& kv : kv_) {
    bool known = false;
    for (int i = 0; i < kKnownKeyCount; ++i)
      if (kv.first == kKnownKeys[i].key) { known = true; break; }
    if (!known) rest.push_back(kv.first);
  }
  if (!rest.empty()) {
    std::sort(rest.begin(), rest.end());
    out += "\n# 其他(含上次用過的方案;本版本不認得的鍵也會留在這裡)\n";
    for (const std::string& k : rest) {
      out += k;
      out += " = ";
      out += kv_.at(k);
      out += "\n";
    }
  }
  return out;
}

bool Settings::Has(const std::string& key) const {
  return kv_.find(key) != kv_.end();
}

void Settings::Unset(const std::string& key) { kv_.erase(key); }

std::string Settings::Raw(const std::string& key) const {
  auto it = kv_.find(key);
  return it == kv_.end() ? std::string() : it->second;
}

void Settings::SetRaw(const std::string& key, const std::string& value) {
  if (!ValidKey(key)) return;
  kv_[key] = CleanValue(value);
}

Tri Settings::GetTri(const std::string& key) const {
  auto it = kv_.find(key);
  if (it == kv_.end()) return Tri::kUnset;
  if (it->second == "true") return Tri::kTrue;
  if (it->second == "false") return Tri::kFalse;
  return Tri::kUnset;  // 認不得的字面值 = 沒設過
}

void Settings::SetTri(const std::string& key, Tri v) {
  if (v == Tri::kUnset) {
    kv_.erase(key);
    return;
  }
  kv_[key] = (v == Tri::kTrue) ? "true" : "false";
}

int Settings::GetEnumInt(const std::string& key, const int* allowed, int n) const {
  auto it = kv_.find(key);
  if (it == kv_.end() || n <= 0) return n > 0 ? allowed[0] : 0;
  int v = 0;
  if (!ParseIntStrict(it->second, &v)) return allowed[0];
  for (int i = 0; i < n; ++i)
    if (allowed[i] == v) return v;
  return allowed[0];
}

void Settings::SetEnumInt(const std::string& key, int value, const int* allowed,
                          int n) {
  // allowed[0] 是「跟隨」,寫進去等於把「沒設過」變成「設過」——
  // 見檔頭。所以那一格的正確動作是刪掉。
  if (n > 0 && value == allowed[0]) {
    kv_.erase(key);
    return;
  }
  for (int i = 0; i < n; ++i) {
    if (allowed[i] == value) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%d", value);
      kv_[key] = buf;
      return;
    }
  }
  // 不在允許清單裡:當成沒設過,不要偷偷存一個沒有人認得的值。
  kv_.erase(key);
}

SchemaPreference Settings::SchemaPref() const {
  SchemaPreference p;
  // ⚠ 預設是 true。**「沒有這個鍵」= 沒設過 = 跟隨輸入模式**,
  //   所以這裡是「只有明著寫了 false 才關」,不是 GetTri() == kTrue。
  p.follow_input_mode = GetTri(keys::kSchemasFollowInputMode) != Tri::kFalse;
  p.pinned_global = Raw(keys::kSchemasPinnedGlobal);
  p.pinned_hant = Raw(keys::kSchemasPinnedHant);
  p.pinned_hans = Raw(keys::kSchemasPinnedHans);
  p.variant = VariantPrefFromToken(Raw(keys::kTextVariant));
  return p;
}

void Settings::SetFollowInputMode(bool on) {
  // 預設值是 true,所以 true = 刪掉那個鍵(見檔頭:不要把預設值寫進檔案)。
  if (on)
    kv_.erase(keys::kSchemasFollowInputMode);
  else
    SetTri(keys::kSchemasFollowInputMode, Tri::kFalse);
}

void Settings::SetPinnedGlobal(const std::string& schema_id) {
  if (schema_id.empty())
    kv_.erase(keys::kSchemasPinnedGlobal);
  else
    SetRaw(keys::kSchemasPinnedGlobal, schema_id);
}

void Settings::SetPinnedForCharSet(CharSet cs, const std::string& schema_id) {
  const char* key = cs == CharSet::kHans ? keys::kSchemasPinnedHans
                                         : keys::kSchemasPinnedHant;
  // kUnspecified 沒有對應的桶。硬塞進繁體那一桶會讓「不知道是哪一種」
  // 變成「他選了繁體」,而使用者從來沒說過那句話。
  if (cs == CharSet::kUnspecified) return;
  if (schema_id.empty())
    kv_.erase(key);
  else
    SetRaw(key, schema_id);
}

void Settings::SetVariantPref(VariantPref v) {
  if (v == VariantPref::kFollowInputMode)
    kv_.erase(keys::kTextVariant);
  else
    SetRaw(keys::kTextVariant, VariantPrefToken(v));
}

void Settings::SetPunctuation(Tri t) { SetTri(keys::kTextPunctuation, t); }

void Settings::SetShape(Tri t) { SetTri(keys::kTextShape, t); }

}  // namespace rimewin
