#include "mini_json.h"

#include <cstdlib>

namespace rimewin {
namespace {

const std::vector<Json>& EmptyArray() {
  static const std::vector<Json> kEmpty;
  return kEmpty;
}

class Parser {
 public:
  Parser(const std::string& s) : s_(s) {}

  bool Run(Json* out, std::string* err) {
    SkipWs();
    if (!Value(out, 0)) { *err = err_; return false; }
    SkipWs();
    if (i_ != s_.size()) {
      *err = "trailing bytes after the top-level value at offset " + std::to_string(i_);
      return false;
    }
    return true;
  }

 private:
  bool Fail(const std::string& what) {
    if (err_.empty()) err_ = what + " at offset " + std::to_string(i_);
    return false;
  }

  void SkipWs() {
    while (i_ < s_.size()) {
      const char c = s_[i_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i_;
      else break;
    }
  }

  bool Lit(const char* lit) {
    const size_t n = std::char_traits<char>::length(lit);
    if (s_.compare(i_, n, lit) != 0) return false;
    i_ += n;
    return true;
  }

  bool Value(Json* out, int depth) {
    if (depth > kJsonMaxDepth) return Fail("nesting is deeper than the limit");
    if (i_ >= s_.size()) return Fail("unexpected end of input");
    const char c = s_[i_];
    switch (c) {
      case '{': return Object(out, depth);
      case '[': return Array(out, depth);
      case '"': {
        out->type = Json::Type::kString;
        return String(&out->text);
      }
      case 't':
        if (!Lit("true")) return Fail("bad literal");
        out->type = Json::Type::kBool;
        out->boolean = true;
        return true;
      case 'f':
        if (!Lit("false")) return Fail("bad literal");
        out->type = Json::Type::kBool;
        out->boolean = false;
        return true;
      case 'n':
        if (!Lit("null")) return Fail("bad literal");
        out->type = Json::Type::kNull;
        return true;
      default:
        return Number(out);
    }
  }

  bool Object(Json* out, int depth) {
    out->type = Json::Type::kObject;
    ++i_;  // '{'
    SkipWs();
    if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
    while (true) {
      SkipWs();
      if (i_ >= s_.size() || s_[i_] != '"') return Fail("expected a string key");
      std::string key;
      if (!String(&key)) return false;
      SkipWs();
      if (i_ >= s_.size() || s_[i_] != ':') return Fail("expected ':'");
      ++i_;
      SkipWs();
      Json v;
      if (!Value(&v, depth + 1)) return false;
      out->fields.emplace_back(std::move(key), std::move(v));
      SkipWs();
      if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
      if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
      return Fail("expected ',' or '}'");
    }
  }

  bool Array(Json* out, int depth) {
    out->type = Json::Type::kArray;
    ++i_;  // '['
    SkipWs();
    if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
    while (true) {
      SkipWs();
      Json v;
      if (!Value(&v, depth + 1)) return false;
      out->items.push_back(std::move(v));
      SkipWs();
      if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
      if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
      return Fail("expected ',' or ']'");
    }
  }

  // 把一個 unicode 碼位編成 UTF-8。
  static void AppendUtf8(uint32_t cp, std::string* out) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool Hex4(uint32_t* out) {
    if (i_ + 4 > s_.size()) return Fail("truncated \\u escape");
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = s_[i_ + k];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
      else return Fail("bad hex digit in \\u escape");
    }
    i_ += 4;
    *out = v;
    return true;
  }

  bool String(std::string* out) {
    ++i_;  // 開頭的引號
    out->clear();
    while (true) {
      if (i_ >= s_.size()) return Fail("unterminated string");
      const unsigned char c = static_cast<unsigned char>(s_[i_]);
      if (c == '"') { ++i_; return true; }
      if (c < 0x20) return Fail("raw control character inside a string");
      if (c != '\\') { out->push_back(static_cast<char>(c)); ++i_; continue; }
      ++i_;
      if (i_ >= s_.size()) return Fail("unterminated escape");
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
          uint32_t cp = 0;
          if (!Hex4(&cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            // 代理對。後半沒跟上就當成 U+FFFD —— 索引是別人產生的,
            // 一個壞掉的跳脫不該讓 34 個套件全部消失。
            if (i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
              const size_t save = i_;
              i_ += 2;
              uint32_t lo = 0;
              if (!Hex4(&lo)) return false;
              if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              } else {
                i_ = save;
                cp = 0xFFFD;
              }
            } else {
              cp = 0xFFFD;
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;  // 落單的後半代理
          }
          AppendUtf8(cp, out);
          break;
        }
        default:
          return Fail("unknown escape");
      }
    }
  }

  bool Number(Json* out) {
    const size_t start = i_;
    if (i_ < s_.size() && s_[i_] == '-') ++i_;
    if (i_ >= s_.size() || s_[i_] < '0' || s_[i_] > '9') return Fail("bad number");
    // JSON 不准前導零。放行的話 "01" 與 "1" 會變成同一個值,而
    // 索引裡的 size / order 是這樣寫出來的話,伺服器那一端已經出問題了。
    if (s_[i_] == '0') {
      ++i_;
    } else {
      while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
    }
    if (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') return Fail("leading zero in a number");
    bool is_int = true;
    if (i_ < s_.size() && s_[i_] == '.') {
      is_int = false;
      ++i_;
      if (i_ >= s_.size() || s_[i_] < '0' || s_[i_] > '9') return Fail("bad fraction");
      while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
    }
    if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
      is_int = false;
      ++i_;
      if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
      if (i_ >= s_.size() || s_[i_] < '0' || s_[i_] > '9') return Fail("bad exponent");
      while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
    }
    const std::string lit = s_.substr(start, i_ - start);
    out->type = Json::Type::kNumber;
    out->number = std::strtod(lit.c_str(), nullptr);
    out->is_integer = is_int;
    if (is_int) {
      errno = 0;
      char* end = nullptr;
      const long long v = std::strtoll(lit.c_str(), &end, 10);
      // 溢位的整數退化成 double。size 欄位溢位時我們寧可拿到一個
      // 明顯不對的大數字,也不要拿到 LLONG_MAX 假裝它是真的。
      if (errno == ERANGE) {
        out->is_integer = false;
      } else {
        out->integer = static_cast<int64_t>(v);
      }
    }
    return true;
  }

  const std::string& s_;
  size_t i_ = 0;
  std::string err_;
};

}  // namespace

const Json* Json::Find(const std::string& key) const {
  if (type != Type::kObject) return nullptr;
  for (const auto& kv : fields) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

std::string Json::Str(const std::string& key, const std::string& dflt) const {
  const Json* v = Find(key);
  if (v == nullptr || v->type != Type::kString) return dflt;
  return v->text;
}

int64_t Json::Int(const std::string& key, int64_t dflt) const {
  const Json* v = Find(key);
  if (v == nullptr || v->type != Type::kNumber) return dflt;
  if (v->is_integer) return v->integer;
  return static_cast<int64_t>(v->number);
}

bool Json::Bool(const std::string& key, bool dflt) const {
  const Json* v = Find(key);
  if (v == nullptr || v->type != Type::kBool) return dflt;
  return v->boolean;
}

const std::vector<Json>& Json::Array(const std::string& key) const {
  const Json* v = Find(key);
  if (v == nullptr || v->type != Type::kArray) return EmptyArray();
  return v->items;
}

std::vector<std::string> Json::Strings(const std::string& key) const {
  std::vector<std::string> out;
  for (const Json& j : Array(key)) {
    if (j.type == Type::kString) out.push_back(j.text);
  }
  return out;
}

bool ParseJson(const std::string& text, Json* out, std::string* err) {
  std::string ignored;
  if (err == nullptr) err = &ignored;
  err->clear();
  *out = Json();
  if (text.empty()) {
    *err = "the document is empty";
    return false;
  }
  // UTF-8 BOM。伺服器不該給,但給了的話整份索引失敗太可惜。
  size_t off = 0;
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF) {
    off = 3;
  }
  const std::string body = off ? text.substr(off) : text;
  Parser p(body);
  return p.Run(out, err);
}

}  // namespace rimewin
