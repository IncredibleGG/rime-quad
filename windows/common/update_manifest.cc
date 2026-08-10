// windows/common/update_manifest.cc — 純解析。不碰網路、不碰檔案。

#include "update_manifest.h"

#include <cstdlib>

#include "net_gate_core.h"
#include "net_policy.h"
#include "sha256.h"

namespace rimewin {
namespace {

// ══════════════════════════════════════════════════════════════════
//  極簡 JSON
//
//  ⚠ 這裡剖的是**網路上抓回來的位元組**,所以它的失敗模式必須是
//    「回 false」,不是「爆掉」。三件事因此是刻意的:
//
//    1. **有深度上限。** 沒有它的話,一份 `[[[[[…` 就能把遞迴下降的
//       剖析器打爆 —— 而且那不需要任何權限,只要能回應一次 HTTP。
//    2. **不接受尾巴的垃圾。** `{...}{...}` 只認第一份的話,發布端
//       產生的壞 JSON 會被我們「修好」,而修好的那一份與別人看到的
//       不一樣。
//    3. **數字保留原文。** version_code 與 size 都是整數語意;
//       走 double 會在十六位數以上安靜地失去精度,而那正好是
//       「把日期塞進版本號」的長度區間。
//
//  為什麼不用現成的函式庫:windows/common/ 刻意零相依 —— 它必須在
//  Ubuntu 上用一行 g++ 編起來,那是這裡每一條測試存在的前提。
// ══════════════════════════════════════════════════════════════════

constexpr int kMaxDepth = 20;

struct JsonValue {
  enum class Kind { kNull, kBool, kNumber, kString, kArray, kObject };
  Kind kind = Kind::kNull;
  bool b = false;
  std::string text;  // 字串的內容,或數字的原文
  std::vector<JsonValue> items;
  std::vector<std::pair<std::string, JsonValue>> members;

  const JsonValue* Member(const std::string& key) const {
    if (kind != Kind::kObject) return nullptr;
    for (const auto& kv : members)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }
};

class JsonParser {
 public:
  explicit JsonParser(const std::string& in) : s_(in) {}

  bool ParseDocument(JsonValue* out) {
    SkipWs();
    if (!ParseValue(out, 0)) return false;
    SkipWs();
    // 尾巴不可以有東西。
    return i_ == s_.size();
  }

 private:
  const std::string& s_;
  size_t i_ = 0;

  void SkipWs() {
    while (i_ < s_.size()) {
      const char c = s_[i_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i_;
      else break;
    }
  }
  bool Eat(char c) {
    if (i_ < s_.size() && s_[i_] == c) {
      ++i_;
      return true;
    }
    return false;
  }
  bool Lit(const char* w) {
    const size_t n = std::string(w).size();
    if (s_.compare(i_, n, w) != 0) return false;
    i_ += n;
    return true;
  }

  bool ParseString(std::string* out) {
    if (!Eat('"')) return false;
    out->clear();
    while (i_ < s_.size()) {
      const unsigned char c = static_cast<unsigned char>(s_[i_]);
      if (c == '"') {
        ++i_;
        return true;
      }
      if (c == '\\') {
        ++i_;
        if (i_ >= s_.size()) return false;
        const char e = s_[i_++];
        switch (e) {
          case '"': out->push_back('"'); break;
          case '\\': out->push_back('\\'); break;
          case '/': out->push_back('/'); break;
          case 'b': out->push_back('\b'); break;
          case 'f': out->push_back('\f'); break;
          case 'n': out->push_back('\n'); break;
          case 'r': out->push_back('\r'); break;
          case 't': out->push_back('\t'); break;
          case 'u': {
            // \uXXXX。我們的欄位全部是 ASCII 或已經是 UTF-8 的字面,
            // 所以這裡只求「不要把剖析弄壞」:認得長度、轉成 UTF-8。
            if (i_ + 4 > s_.size()) return false;
            unsigned cp = 0;
            for (int k = 0; k < 4; ++k) {
              const char h = s_[i_ + k];
              unsigned v;
              if (h >= '0' && h <= '9') v = static_cast<unsigned>(h - '0');
              else if (h >= 'a' && h <= 'f') v = static_cast<unsigned>(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') v = static_cast<unsigned>(h - 'A' + 10);
              else return false;
              cp = cp * 16 + v;
            }
            i_ += 4;
            AppendUtf8(out, cp);
            break;
          }
          default:
            return false;
        }
        continue;
      }
      // ⚠ 未跳脫的控制字元是不合法的 JSON。放行的話,一個換行就能在
      //   我們的欄位裡藏東西 —— 連網紀錄那一側同樣的理由。
      if (c < 0x20) return false;
      out->push_back(static_cast<char>(c));
      ++i_;
    }
    return false;
  }

  static void AppendUtf8(std::string* out, unsigned cp) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool ParseNumber(JsonValue* out) {
    const size_t start = i_;
    if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
    bool any = false;
    while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') {
      ++i_;
      any = true;
    }
    if (i_ < s_.size() && s_[i_] == '.') {
      ++i_;
      while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') {
        ++i_;
        any = true;
      }
    }
    if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
      ++i_;
      if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
      while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
    }
    if (!any) return false;
    out->kind = JsonValue::Kind::kNumber;
    out->text = s_.substr(start, i_ - start);
    return true;
  }

  bool ParseValue(JsonValue* out, int depth) {
    if (depth > kMaxDepth) return false;
    if (i_ >= s_.size()) return false;
    const char c = s_[i_];
    if (c == '"') {
      out->kind = JsonValue::Kind::kString;
      return ParseString(&out->text);
    }
    if (c == '{') {
      ++i_;
      out->kind = JsonValue::Kind::kObject;
      SkipWs();
      if (Eat('}')) return true;
      for (;;) {
        SkipWs();
        std::string key;
        if (!ParseString(&key)) return false;
        SkipWs();
        if (!Eat(':')) return false;
        SkipWs();
        JsonValue v;
        if (!ParseValue(&v, depth + 1)) return false;
        // ⚠ 重複的鍵:**留第一個**。JSON 沒有規定,而「留最後一個」
        //   讓一份 {"size":10,"size":99999} 的宣告值取決於剖析器的選擇。
        //   兩邊的選擇不同就是兩份不同的清單。
        if (!out->Member(key)) out->members.emplace_back(key, v);
        SkipWs();
        if (Eat(',')) continue;
        return Eat('}');
      }
    }
    if (c == '[') {
      ++i_;
      out->kind = JsonValue::Kind::kArray;
      SkipWs();
      if (Eat(']')) return true;
      for (;;) {
        SkipWs();
        JsonValue v;
        if (!ParseValue(&v, depth + 1)) return false;
        out->items.push_back(v);
        SkipWs();
        if (Eat(',')) continue;
        return Eat(']');
      }
    }
    if (c == 't') {
      if (!Lit("true")) return false;
      out->kind = JsonValue::Kind::kBool;
      out->b = true;
      return true;
    }
    if (c == 'f') {
      if (!Lit("false")) return false;
      out->kind = JsonValue::Kind::kBool;
      out->b = false;
      return true;
    }
    if (c == 'n') {
      if (!Lit("null")) return false;
      out->kind = JsonValue::Kind::kNull;
      return true;
    }
    return ParseNumber(out);
  }
};

std::string Trim(const std::string& in) {
  size_t b = 0, e = in.size();
  while (b < e && (in[b] == ' ' || in[b] == '\t' || in[b] == '\r' || in[b] == '\n')) ++b;
  while (e > b && (in[e - 1] == ' ' || in[e - 1] == '\t' || in[e - 1] == '\r' ||
                   in[e - 1] == '\n'))
    --e;
  return in.substr(b, e - b);
}

std::string Lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

// 頂層字串欄位。型別不對(數字、物件、陣列)一律當成沒有。
std::string TopString(const JsonValue& root, const char* key) {
  const JsonValue* v = root.Member(key);
  if (!v || v->kind != JsonValue::Kind::kString) return std::string();
  return v->text;
}

// 頂層整數欄位。⚠ 只接受**整數**的原文:`1e9` 與 `1.0` 一律不算。
// 版本號與位元組數沒有小數,而接受它們等於接受一個我們沒有定義過的四捨五入。
bool TopInt(const JsonValue& root, const char* key, int64_t* out) {
  const JsonValue* v = root.Member(key);
  if (!v || v->kind != JsonValue::Kind::kNumber) return false;
  const std::string& t = v->text;
  if (t.empty() || t.size() > 19) return false;
  size_t i = 0;
  bool neg = false;
  if (t[0] == '-') {
    neg = true;
    i = 1;
  }
  if (i >= t.size()) return false;
  int64_t val = 0;
  for (; i < t.size(); ++i) {
    if (t[i] < '0' || t[i] > '9') return false;
    val = val * 10 + (t[i] - '0');
  }
  *out = neg ? -val : val;
  return true;
}

}  // namespace

bool LooksLikeAppId(const std::string& s) {
  // {8-4-4-4-12}
  if (s.size() != 38) return false;
  if (s.front() != '{' || s.back() != '}') return false;
  static const int kDash[] = {9, 14, 19, 24};
  for (size_t i = 1; i + 1 < s.size(); ++i) {
    const int pos = static_cast<int>(i);
    bool is_dash = false;
    for (int d : kDash) is_dash = is_dash || (pos == d);
    if (is_dash) {
      if (s[i] != '-') return false;
      continue;
    }
    const char c = s[i];
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F');
    if (!hex) return false;
  }
  return true;
}

std::string NormalizeAppId(const std::string& s) {
  std::string t = Trim(s);
  for (char& c : t)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  return t;
}

ManifestParseResult ParseWinUpdateManifest(const std::string& text,
                                           const std::string& manifest_url) {
  ManifestParseResult r;
  if (text.size() > static_cast<size_t>(kMaxUpdateManifestBytes)) {
    r.error = "版本資訊太大,不像是我們發出去的那一份。";
    return r;
  }
  JsonValue root;
  JsonParser p(text);
  if (!p.ParseDocument(&root)) {
    r.error = "版本資訊不是合法的 JSON。";
    return r;
  }
  if (root.kind != JsonValue::Kind::kObject) {
    r.error = "版本資訊的頂層必須是物件。";
    return r;
  }

  WinUpdateManifest m;

  if (!TopInt(root, "version_code", &m.version_code)) {
    r.error =
        "版本資訊沒有 version_code,無從判斷新舊。"
        "(這是舊格式的版本資訊,發布端更新後就會有。)";
    return r;
  }
  if (m.version_code <= 0) {
    r.error = "version_code 必須是正整數。";
    return r;
  }

  m.version_name = Trim(TopString(root, "version_name"));
  if (m.version_name.empty()) {
    r.error = "版本資訊缺少 version_name。";
    return r;
  }

  if (!TopInt(root, "size", &m.size)) {
    r.error = "版本資訊缺少 size。";
    return r;
  }
  if (m.size <= 0 || m.size > kMaxSetupBytes) {
    r.error = "size 不合理(超出允許的上限)。";
    return r;
  }

  m.sha256 = Lower(Trim(TopString(root, "sha256")));
  if (!LooksLikeSha256Hex(m.sha256)) {
    r.error = "版本資訊的 sha256 不是 64 字元的十六進位字串。";
    return r;
  }

  // url 缺席時退回 file —— 與 Android 端同一條退路,理由也一樣:
  // 整個目錄搬到別的主機時不必改內容,本機測試也靠這一條。
  std::string raw_url = Trim(TopString(root, "url"));
  if (raw_url.empty()) raw_url = Trim(TopString(root, "file"));
  if (raw_url.empty()) {
    r.error = "版本資訊沒有 url 也沒有 file,不知道要下載什麼。";
    return r;
  }
  // ⚠ **先驗它自己的 scheme,再解析。** ResolveUrl 只認得 http(s):// 開頭
  //   的絕對網址,其餘一律當成相對路徑接在 base 後面 —— 於是
  //   file:///C:/x.exe 會被拼成 https://<原主機>/file:///C:/x.exe,
  //   一個合法的 https 網址,SchemeAllowed 看了說可以。實測就是這樣:
  //   這一條測試原本是紅的。判斷用的是連網那一側同一支
  //   LocationSchemeAllowed(相對網址回 true),不另外寫第二份。
  if (!LocationSchemeAllowed(raw_url)) {
    r.error = "下載網址只接受 http 或 https。";
    return r;
  }
  m.url = ResolveUrl(manifest_url, raw_url);
  // ⚠ **https,不是 http-or-https。** 這個網址指向的是一支會提權執行的
  //   安裝程式,而這一端沒有程式碼簽章 —— TLS 是唯一的信任錨
  //   (見 service/net_gate.h 開頭)。走明文的話,對方換得掉安裝程式
  //   就換得掉下面那個 sha256,那是同一個動作。
  if (!HttpsOnly(m.url)) {
    r.error = "安裝程式的網址必須是 https。";
    return r;
  }

  // ── 以下全部選用。格式不對 = 當成沒有,不 return。 ──────────
  m.commit = Trim(TopString(root, "commit"));
  m.file = Trim(TopString(root, "file"));
  m.notes = Trim(TopString(root, "notes"));

  const std::string app_id = Trim(TopString(root, "app_id"));
  if (LooksLikeAppId(app_id)) m.app_id = app_id;

  // 字串或字串陣列都收。換過一次名字時寫字串最自然,換過兩次就得是陣列 ——
  // 兩種都認得,省掉一次「格式要改了」的協調。
  {
    const std::string one = Trim(TopString(root, "replaces_app_id"));
    if (LooksLikeAppId(one)) m.replaces_app_ids.push_back(one);
    const JsonValue* arr = root.Member("replaces_app_id");
    if (arr && arr->kind == JsonValue::Kind::kArray) {
      for (const JsonValue& v : arr->items) {
        if (v.kind != JsonValue::Kind::kString) continue;
        const std::string s = Trim(v.text);
        if (!LooksLikeAppId(s)) continue;
        bool dup = false;
        for (const auto& e : m.replaces_app_ids)
          dup = dup || (NormalizeAppId(e) == NormalizeAppId(s));
        if (!dup) m.replaces_app_ids.push_back(s);
      }
    }
  }

  const std::string page = Trim(TopString(root, "page_url"));
  if (!page.empty() && LocationSchemeAllowed(page)) {
    const std::string abs = ResolveUrl(manifest_url, page);
    // 這一個是拿去開瀏覽器的,同樣只收 https:一個明文的下載頁,
    // 使用者在上面按下的每一顆鍵都可以被路上的人換掉。
    // ⚠ 格式不對 = 當成沒有(選用欄位不整份拒收),與上面那幾條一致。
    if (HttpsOnly(abs)) m.page_url = abs;
  }

  r.ok = true;
  r.manifest = m;
  return r;
}

UpdateVerdict CompareVersion(int64_t installed_code,
                             const WinUpdateManifest& remote) {
  if (remote.version_code > installed_code) return UpdateVerdict::kUpdateAvailable;
  if (remote.version_code == installed_code) return UpdateVerdict::kUpToDate;
  return UpdateVerdict::kDowngrade;
}

AppIdVerdict CompareAppId(const std::string& installed,
                          const std::string& remote) {
  // ⚠ 「本機不知道自己是誰」也是 kUnknown。把它當成 kChanged 會讓一個
  //   讀不到 version.txt 的安裝從此再也更新不了。
  if (Trim(remote).empty() || Trim(installed).empty()) return AppIdVerdict::kUnknown;
  if (NormalizeAppId(remote) == NormalizeAppId(installed)) return AppIdVerdict::kSame;
  return AppIdVerdict::kChanged;
}

bool DeclaresReplacing(const std::string& installed,
                       const WinUpdateManifest& remote) {
  if (Trim(installed).empty()) return false;
  const std::string want = NormalizeAppId(installed);
  for (const auto& e : remote.replaces_app_ids)
    if (NormalizeAppId(e) == want) return true;
  return false;
}

InstalledVersion ParseInstalledVersion(const std::string& text) {
  InstalledVersion v;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    const std::string line = Trim(text.substr(pos, nl - pos));
    pos = nl + 1;
    if (line.empty() || line[0] == '#') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = Trim(line.substr(0, eq));
    const std::string val = Trim(line.substr(eq + 1));
    if (key == "version_code") {
      int64_t n = 0;
      bool ok = !val.empty() && val.size() <= 19;
      for (char c : val) ok = ok && c >= '0' && c <= '9';
      if (ok) {
        for (char c : val) n = n * 10 + (c - '0');
        if (n > 0) v.version_code = n;
      }
    } else if (key == "version_name") {
      v.version_name = val;
    } else if (key == "app_id") {
      if (LooksLikeAppId(val)) v.app_id = val;
    } else if (key == "commit") {
      v.commit = val;
    }
    if (nl == text.size()) break;
  }
  return v;
}

}  // namespace rimewin
