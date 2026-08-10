// windows/common/net_policy.cc — 純邏輯,不含任何網路 API。
//
// ⚠ 這個檔案的名字裡有 net,但它**不會**開任何連線。
//   真的連線的只有 windows/service/net_gate.cc 一個檔案。

#include "net_policy.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace rimewin {
namespace {

std::string Lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

// 一行一筆是 TSV 唯一的不變式。tab / CR / LF 全部換成空白再 trim 再截斷。
std::string Clean(const std::string& in, size_t max) {
  std::string s;
  s.reserve(in.size());
  for (char c : in) s.push_back((c == '\t' || c == '\n' || c == '\r') ? ' ' : c);
  size_t b = 0, e = s.size();
  while (b < e && s[b] == ' ') ++b;
  while (e > b && s[e - 1] == ' ') --e;
  s = s.substr(b, e - b);
  if (s.size() > max) s.resize(max);
  return s;
}

bool ParseI64(const std::string& s, int64_t* out) {
  if (s.empty() || s.size() > 19) return false;
  int64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  *out = v;
  return true;
}

// scheme://host[:port]/path 的極簡拆解。刻意不做完整的 RFC3986:
// 我們只需要 scheme 與 host,而多一分解析就多一分「兩邊看法不同」的空間。
bool SplitUrl(const std::string& url, std::string* scheme, std::string* authority,
              std::string* rest) {
  const size_t c = url.find("://");
  if (c == std::string::npos || c == 0) return false;
  *scheme = Lower(url.substr(0, c));
  const size_t astart = c + 3;
  size_t aend = url.size();
  for (size_t i = astart; i < url.size(); ++i) {
    if (url[i] == '/' || url[i] == '?' || url[i] == '#') {
      aend = i;
      break;
    }
  }
  *authority = url.substr(astart, aend - astart);
  *rest = url.substr(aend);
  // authority 允許為空(`file:///x` 就是),由呼叫端決定那算不算數 ——
  // CheckHop 要先看 scheme 才看 authority,否則 `file:///etc/passwd`
  // 會被判成「網址壞掉」而不是「scheme 不准」,而那兩者要對使用者
  // 說的話不一樣。
  return true;
}

}  // namespace

const char* NetPurposeToken(NetPurpose p) {
  switch (p) {
    case NetPurpose::kStoreIndex: return "STORE_INDEX";
    case NetPurpose::kStorePackage: return "STORE_PACKAGE";
    case NetPurpose::kUpdateManifest: return "UPDATE_MANIFEST";
    case NetPurpose::kUpdateSetup: return "UPDATE_SETUP";
  }
  return "STORE_INDEX";
}

const char* NetPurposeText(NetPurpose p) {
  switch (p) {
    case NetPurpose::kStoreIndex: return "瀏覽方案市集(取索引)";
    case NetPurpose::kStorePackage: return "下載方案套件";
    case NetPurpose::kUpdateManifest: return "查有沒有新版本";
    case NetPurpose::kUpdateSetup: return "下載新版本的安裝程式";
  }
  return "?";
}

bool NetPurposeFromToken(const std::string& s, NetPurpose* out) {
  if (s == "STORE_INDEX") { *out = NetPurpose::kStoreIndex; return true; }
  if (s == "STORE_PACKAGE") { *out = NetPurpose::kStorePackage; return true; }
  if (s == "UPDATE_MANIFEST") { *out = NetPurpose::kUpdateManifest; return true; }
  if (s == "UPDATE_SETUP") { *out = NetPurpose::kUpdateSetup; return true; }
  return false;
}

const char* NetOutcomeToken(NetOutcome o) {
  switch (o) {
    case NetOutcome::kOk: return "OK";
    case NetOutcome::kFailed: return "FAILED";
    case NetOutcome::kRedirected: return "REDIRECTED";
  }
  return "FAILED";
}

const char* NetOutcomeText(NetOutcome o) {
  switch (o) {
    case NetOutcome::kOk: return "成功";
    case NetOutcome::kFailed: return "失敗";
    case NetOutcome::kRedirected: return "轉址";
  }
  return "?";
}

bool NetOutcomeFromToken(const std::string& s, NetOutcome* out) {
  if (s == "OK") { *out = NetOutcome::kOk; return true; }
  if (s == "FAILED") { *out = NetOutcome::kFailed; return true; }
  if (s == "REDIRECTED") { *out = NetOutcome::kRedirected; return true; }
  return false;
}

std::string EncodeLogLine(const NetLogEntry& e) {
  char num[32];
  std::string out;
  std::snprintf(num, sizeof(num), "%lld", static_cast<long long>(e.at_ms));
  out += num;
  out += '\t';
  out += Clean(e.host, kMaxLogHost);
  out += '\t';
  out += NetPurposeToken(e.purpose);
  out += '\t';
  out += Clean(e.label, kMaxLogLabel);
  out += '\t';
  out += NetOutcomeToken(e.outcome);
  out += '\t';
  std::snprintf(num, sizeof(num), "%lld",
                static_cast<long long>(e.bytes < 0 ? 0 : e.bytes));
  out += num;
  out += '\t';
  out += Clean(e.detail, kMaxLogDetail);
  return out;
}

bool DecodeLogLine(const std::string& line, NetLogEntry* out) {
  std::vector<std::string> f;
  size_t pos = 0;
  while (true) {
    const size_t t = line.find('\t', pos);
    if (t == std::string::npos) {
      f.push_back(line.substr(pos));
      break;
    }
    f.push_back(line.substr(pos, t - pos));
    pos = t + 1;
    if (f.size() > 8) return false;
  }
  if (f.size() != 7) return false;
  NetLogEntry e;
  if (!ParseI64(f[0], &e.at_ms)) return false;
  e.host = f[1];
  if (!NetPurposeFromToken(f[2], &e.purpose)) return false;
  e.label = f[3];
  if (!NetOutcomeFromToken(f[4], &e.outcome)) return false;
  if (!ParseI64(f[5], &e.bytes)) return false;
  e.detail = f[6];
  *out = e;
  return true;
}

std::vector<NetLogEntry> DecodeLog(const std::string& text) {
  std::vector<NetLogEntry> out;
  size_t pos = 0;
  while (pos <= text.size()) {
    const size_t nl = text.find('\n', pos);
    std::string line =
        text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    NetLogEntry e;
    if (DecodeLogLine(line, &e)) out.push_back(e);  // 壞行安靜丟掉
  }
  return out;
}

std::string EncodeLog(const std::vector<NetLogEntry>& entries) {
  std::string out;
  for (const NetLogEntry& e : entries) {
    out += EncodeLogLine(e);
    out += '\n';
  }
  return out;
}

std::vector<NetLogEntry> AppendCapped(const std::vector<NetLogEntry>& cur,
                                      const NetLogEntry& add,
                                      size_t max_entries) {
  std::vector<NetLogEntry> out = cur;
  out.push_back(add);
  if (max_entries > 0 && out.size() > max_entries)
    out.erase(out.begin(), out.begin() + (out.size() - max_entries));
  return out;
}

HopVerdict CheckHop(bool enabled, const std::string& url) {
  // ⚠ 順序不可以換。開關要在**任何**解析之前問 ——
  //   開關是關的時候,連「這個網址長什麼樣」都不必知道。
  if (!enabled) return HopVerdict::kBlockedBySwitch;
  std::string scheme, authority, rest;
  if (!SplitUrl(url, &scheme, &authority, &rest)) return HopVerdict::kBadUrl;
  // ⚠ scheme 先於 authority。`file:///etc/passwd` 的 authority 是空的,
  //   先檢查 authority 的話它會被判成 kBadUrl —— 而我們要說的是
  //   「這個 scheme 不准」,那是使用者(與稽核的人)看得懂的答案。
  if (scheme != "http" && scheme != "https") return HopVerdict::kBadScheme;
  if (authority.empty()) return HopVerdict::kBadUrl;
  return HopVerdict::kProceed;
}

std::string HostOf(const std::string& url) {
  std::string scheme, authority, rest;
  if (!SplitUrl(url, &scheme, &authority, &rest) || authority.empty())
    return url.size() > 60 ? url.substr(0, 60) : url;
  // 去掉 userinfo 與 port。userinfo 可能含密碼,絕不進紀錄。
  const size_t at = authority.rfind('@');
  std::string host = (at == std::string::npos) ? authority
                                               : authority.substr(at + 1);
  if (!host.empty() && host[0] == '[') {  // IPv6 字面值
    const size_t rb = host.find(']');
    if (rb != std::string::npos) host = host.substr(0, rb + 1);
  } else {
    const size_t colon = host.find(':');
    if (colon != std::string::npos) host = host.substr(0, colon);
  }
  if (host.empty()) return url.size() > 60 ? url.substr(0, 60) : url;
  return host;
}

bool SchemeAllowed(const std::string& url) {
  return CheckHop(true, url) == HopVerdict::kProceed;
}

bool HttpsOnly(const std::string& url) {
  // 先走同一條 CheckHop(scheme 先於 authority、空 authority 算壞網址),
  // 免得這裡多出第二份「什麼算合法網址」的判斷。
  if (CheckHop(true, url) != HopVerdict::kProceed) return false;
  std::string scheme, authority, rest;
  if (!SplitUrl(url, &scheme, &authority, &rest)) return false;
  return scheme == "https";
}

std::string ResolveUrl(const std::string& base, const std::string& rel) {
  if (rel.empty()) return base;
  const std::string lower = Lower(rel);
  if (lower.compare(0, 7, "http://") == 0 || lower.compare(0, 8, "https://") == 0)
    return rel;
  std::string scheme, authority, rest;
  if (!SplitUrl(base, &scheme, &authority, &rest) || authority.empty()) return rel;
  if (rel.compare(0, 2, "//") == 0) return scheme + ":" + rel;
  const std::string root = scheme + "://" + authority;
  if (rel[0] == '/') return root + rel;
  // 相對路徑:接在 base 路徑的最後一個 '/' 之後。
  std::string path = rest;
  const size_t q = path.find_first_of("?#");
  if (q != std::string::npos) path = path.substr(0, q);
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos) return root + "/" + rel;
  return root + path.substr(0, slash + 1) + rel;
}

std::string ResolvePackageUrl(const std::string& index_url,
                              const std::string& base_url,
                              const std::string& file) {
  const std::string lower = Lower(file);
  if (lower.compare(0, 7, "http://") == 0 || lower.compare(0, 8, "https://") == 0)
    return file;
  if (!base_url.empty()) {
    std::string b = base_url;
    if (b.back() != '/') b += '/';
    const std::string abs = ResolveUrl(index_url, b);
    return ResolveUrl(abs, file);
  }
  return ResolveUrl(index_url, file);
}

}  // namespace rimewin
