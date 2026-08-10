// windows/service/net_gate.cc — Windows 端唯一會開 socket 的翻譯單元
//
// ⚠⚠ **這是整個 windows/ 底下唯一准許出現 WinHttp* 的檔案。** ⚠⚠
//
//   windows/audit_offline_win.sh 的 ALLOW 清單裡恰好一個路徑,就是這個檔案
//   (比對的是相對路徑,不是檔名 —— 換個目錄放一份同名的檔案不會被放行)。
//   那支腳本同時會斷言這個檔案**真的**碰得到網路 API:哪天出口搬走了、
//   或是這裡改成不連網了,允許清單就會變成一個沒有人守著的洞,而它會紅。
//
//   要在別的地方開連線之前,請先讀 net_gate.h 的檔頭,那裡寫了為什麼
//   「只有一個出口」這件事本身就是產品的一部分。
//
// 政策與流程都不在這裡:
//   · 「這一跳准不准」→ common/net_policy.h(純函式,Ubuntu 上有測試)
//   · 「先做什麼再做什麼」→ common/net_gate_core.h(純流程,Ubuntu 上有測試)
//   · 這裡只剩下 WinHTTP 的手腳。刻意做到愈笨愈好:唯一驗不到的那一段,
//     行數愈少愈好。
//
#include "net_gate.h"

#include <windows.h>
#include <winhttp.h>

#include <cstdlib>  // _wtoi
#include <vector>

#include "../winshared/winutil.h"

// ⚠ 不用 `#pragma comment(lib, ...)` 自動連結。winhttp 是**只有這一個目標**
//   才准有的相依,寫在 CMakeLists 的 rime_service 那一行才看得見 ——
//   藏在原始碼裡的自動連結,在檢查「誰有網路能力」時是看不到的。

namespace rimewin {
namespace {

// ── 具名常數 ────────────────────────────────────────────────────
//
// 值與 Android 的 NetworkGate 對齊(CONNECT_TIMEOUT_MS / READ_TIMEOUT_MS),
// 讓「同一個索引在手機上逾時、在桌面上不逾時」不會變成一件要查的事。
constexpr int kResolveTimeoutMs = 15000;
constexpr int kConnectTimeoutMs = 15000;
constexpr int kSendTimeoutMs = 15000;
constexpr int kReceiveTimeoutMs = 30000;

// 一次讀多少。與 Android 的 64 KB 相同。
constexpr DWORD kReadChunkBytes = 64u * 1024u;

// User-Agent。**刻意不含本專案的名字。**
//
// 理由逐字照 Android 的 NetworkGate.USER_AGENT:在有審查的網路環境下,
// 一行自報家門的 UA 就足以讓使用者被標記為「正在使用這個輸入法」——
// 被動觀察者連解密都不必,明文的請求標頭裡就寫著。而拿掉不等於沒有:
// WinHTTP 會自己補一個帶 Windows 版本的字串,熵更高不是更低。
// 所以送一個固定、不含任何裝置資訊的常數值。
//
// 這**不是**偽裝成瀏覽器:TLS 握手特徵本來就跟瀏覽器不一樣,硬湊只會
// 讓「UA 與 TLS 指紋對不上」變成新的特徵。目標只有一個 —— 不要自報家門。
constexpr const wchar_t* kUserAgent = L"Mozilla/5.0";

// mingw-w64 的 winhttp.h 版本較舊時缺這幾個常數(CI 上的 Windows SDK 有)。
// 值照 SDK 抄。windows/syntax_check_mingw.sh 會編到這個檔案。
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

int64_t NowMs() {
  FILETIME ft{};
  ::GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER u{};
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  // FILETIME 是 1601 起算的 100 奈秒。轉成 Unix 毫秒,與 Android 的
  // System.currentTimeMillis() 同一個基準 —— 兩端的紀錄要對得起來。
  const int64_t kEpochDelta = 116444736000000000LL;
  return (static_cast<int64_t>(u.QuadPart) - kEpochDelta) / 10000;
}

// 標頭查詢。查不到回 false(不是「回空字串」—— 呼叫端要分得出來)。
bool QueryHeader(HINTERNET req, DWORD info, std::wstring* out) {
  DWORD len = 0;
  ::WinHttpQueryHeaders(req, info, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &len,
                        WINHTTP_NO_HEADER_INDEX);
  if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) return false;
  std::wstring buf(len / sizeof(wchar_t) + 1, L'\0');
  if (!::WinHttpQueryHeaders(req, info, WINHTTP_HEADER_NAME_BY_INDEX, &buf[0],
                             &len, WINHTTP_NO_HEADER_INDEX))
    return false;
  buf.resize(len / sizeof(wchar_t));
  *out = buf;
  return true;
}

// Content-Length。**用字串查再自己解析**,不用 WINHTTP_QUERY_FLAG_NUMBER64:
// 那個旗標在部分 SDK / mingw 標頭上不存在,而為了一個數字讓整個檔案在
// 語法檢查那一關就編不起來不划算。沒宣告或解析不出來回 -1。
int64_t QueryContentLength(HINTERNET req) {
  std::wstring s;
  if (!QueryHeader(req, WINHTTP_QUERY_CONTENT_LENGTH, &s)) return -1;
  int64_t v = 0;
  bool any = false;
  for (wchar_t c : s) {
    if (c < L'0' || c > L'9') {
      if (any) break;
      continue;
    }
    any = true;
    if (v > (INT64_MAX - 9) / 10) return -1;  // 溢位就當成沒宣告
    v = v * 10 + (c - L'0');
  }
  return any ? v : -1;
}

// ═══════════════════════════════════════════════════════════════
//  真的開連線的那一半。**這個類別是全部。**
// ═══════════════════════════════════════════════════════════════
class WinHttpTransport : public NetTransport {
 public:
  ~WinHttpTransport() override {
    CloseHop();
    if (session_) ::WinHttpCloseHandle(session_);
  }

  void Begin(const std::string& url, Head* out) override {
    CloseHop();

    if (!session_) {
      // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY:照系統的 Proxy 設定走。
      // ⚠ 誠實交代:走 proxy 代表 proxy 的經營者看得到我們連了哪個主機。
      //   那與 DNS / TLS SNI 本來就看得到是同一件事(見 net_gate.h 檔頭),
      //   而**不照系統設定走**只會讓公司網路裡的使用者連不上,不會讓他更隱密。
      session_ = ::WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
      if (!session_) {
        out->sent = false;
        out->error = "無法建立連線工作階段";
        return;
      }
      ::WinHttpSetTimeouts(session_, kResolveTimeoutMs, kConnectTimeoutMs,
                           kSendTimeoutMs, kReceiveTimeoutMs);
      // 只准 TLS 1.2 / 1.3。舊版 SSL/TLS 在這裡沒有相容性上的理由 ——
      // 我們連的是自己的發布路徑,不是任意的舊伺服器。
      DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 |
                        WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
      ::WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols,
                         sizeof(protocols));
      // ⚠⚠ 這裡**沒有**、而且永遠不會有 WINHTTP_OPTION_SECURITY_FLAGS 搭配
      //     SECURITY_FLAG_IGNORE_*。Windows 端沒有程式碼簽章,憑證鏈是
      //     唯一的信任錨(見 net_gate.h 檔頭)。放寬它等於把最後一道拿掉,
      //     而畫面上看不出任何差別。
    }

    const std::wstring wurl = Utf8ToWide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = static_cast<DWORD>(-1);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!::WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || uc.dwHostNameLength == 0) {
      out->sent = false;
      out->error = "網址解析失敗";
      return;
    }
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    const bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    conn_ = ::WinHttpConnect(session_, host.c_str(), uc.nPort, 0);
    if (!conn_) {
      out->sent = false;
      out->error = "無法建立連線";
      return;
    }
    req_ = ::WinHttpOpenRequest(conn_, L"GET", path.c_str(), nullptr,
                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                secure ? WINHTTP_FLAG_SECURE : 0u);
    if (!req_) {
      out->sent = false;
      out->error = "無法建立請求";
      return;
    }

    // ⚠ 自動轉址一律關掉,自己一跳一跳走。
    //   交給 WinHTTP 自動跟的話,中間那幾跳**進不了連網紀錄** ——
    //   而「它到底連了哪些主機」正是使用者最想知道的那一件事。
    //   同時這也是 https→http 降級擋得住的前提。
    DWORD disable = WINHTTP_DISABLE_REDIRECTS;
    ::WinHttpSetOption(req_, WINHTTP_OPTION_DISABLE_FEATURE, &disable,
                       sizeof(disable));
    // identity:不要壓縮。我們數的位元組數必須就是線路上的位元組數,
    // 否則「超過大小上限就中止」那道牆會被一個高壓縮比的回應繞過去。
    ::WinHttpAddRequestHeaders(req_, L"Accept-Encoding: identity",
                               static_cast<DWORD>(-1),
                               WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    // ══════════════════════════════════════════════════════════════
    //  ⚠ 分界線。下一行之後,DNS 查詢與 TCP/TLS 握手就要出去了 ——
    //    網路上會留下痕跡,所以從這裡開始**這一跳一定要進連網紀錄**,
    //    不論它成不成功。sent 在呼叫**之前**就設 true 是刻意的:
    //    「送出去了嗎」問的是我們有沒有動手,不是對方有沒有回應。
    // ══════════════════════════════════════════════════════════════
    out->sent = true;
    if (!::WinHttpSendRequest(req_, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
      out->ok = false;
      out->error = "連線失敗";
      return;
    }
    if (!::WinHttpReceiveResponse(req_, nullptr)) {
      out->ok = false;
      out->error = "沒有收到回應";
      return;
    }

    std::wstring status;
    if (!QueryHeader(req_, WINHTTP_QUERY_STATUS_CODE, &status)) {
      out->ok = false;
      out->error = "讀不到狀態碼";
      return;
    }
    out->status = ::_wtoi(status.c_str());
    out->ok = true;

    std::wstring location;
    if (QueryHeader(req_, WINHTTP_QUERY_LOCATION, &location))
      out->location = WideToUtf8(location);
    out->declared_bytes = QueryContentLength(req_);
  }

  bool ReadBody(NetSink* sink, int64_t max_bytes, int64_t* read, bool* over,
                std::string* error) override {
    *read = 0;
    *over = false;
    if (!req_) {
      *error = "沒有回應";
      return false;
    }
    std::vector<char> buf(kReadChunkBytes);
    for (;;) {
      DWORD avail = 0;
      if (!::WinHttpQueryDataAvailable(req_, &avail)) {
        *error = "讀取中斷";
        return false;
      }
      if (avail == 0) return true;  // 收完了
      while (avail > 0) {
        const DWORD want =
            avail < kReadChunkBytes ? avail : kReadChunkBytes;
        DWORD got = 0;
        if (!::WinHttpReadData(req_, buf.data(), want, &got)) {
          *error = "讀取中斷";
          return false;
        }
        if (got == 0) return true;
        *read += got;
        // ⚠ 這才是真正的牆。Content-Length 是對方給的,它可以撒謊,
        //   也可以整個不給(chunked)。
        if (*read > max_bytes) {
          *over = true;
          return false;
        }
        if (!sink->Write(buf.data(), got)) {
          *error = "寫入失敗";
          return false;
        }
        avail -= got;
      }
    }
  }

  void End() override { CloseHop(); }

 private:
  void CloseHop() {
    if (req_) {
      ::WinHttpCloseHandle(req_);
      req_ = nullptr;
    }
    if (conn_) {
      ::WinHttpCloseHandle(conn_);
      conn_ = nullptr;
    }
  }

  HINTERNET session_ = nullptr;
  HINTERNET conn_ = nullptr;
  HINTERNET req_ = nullptr;
};

// ── 收下來的位元組往哪去 ────────────────────────────────────────

class StringSink : public NetSink {
 public:
  std::string data;
  bool Write(const char* p, size_t n) override {
    data.append(p, n);
    return true;
  }
  void Discard() override { data.clear(); }
};

// 先寫 <dest>.part,成功才改名。中途失敗不會在硬碟上留下一個看起來
// 下載完成的半份檔案 —— 那個症狀是「裝了方案但是壞的」,而且看不出原因。
class FileSink : public NetSink {
 public:
  explicit FileSink(const std::string& dest_utf8)
      : dest_(Utf8ToWide(dest_utf8)), part_(Utf8ToWide(dest_utf8) + L".part") {
    h_ = ::CreateFileW(part_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  ~FileSink() override { Close(); }

  bool opened() const { return h_ != INVALID_HANDLE_VALUE; }

  bool Write(const char* p, size_t n) override {
    if (h_ == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    if (!::WriteFile(h_, p, static_cast<DWORD>(n), &written, nullptr))
      return false;
    return written == static_cast<DWORD>(n);
  }

  void Discard() override {
    Close();
    ::DeleteFileW(part_.c_str());
  }

  // 只有 RunFetch 回 kOk 才呼叫。
  bool Commit() {
    if (h_ != INVALID_HANDLE_VALUE) ::FlushFileBuffers(h_);
    Close();
    if (!::MoveFileExW(part_.c_str(), dest_.c_str(), MOVEFILE_REPLACE_EXISTING)) {
      ::DeleteFileW(part_.c_str());
      return false;
    }
    return true;
  }

 private:
  void Close() {
    if (h_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(h_);
      h_ = INVALID_HANDLE_VALUE;
    }
  }
  std::wstring dest_;
  std::wstring part_;
  HANDLE h_ = INVALID_HANDLE_VALUE;
};

}  // namespace

NetGate::NetGate(SettingsStore* store) : store_(store) {}

bool NetGate::Enabled() const {
  // ⚠ fail-closed。store 沒接上 = 不知道 = 關。
  //   判斷只有一處:Settings::NetworkEnabled()。不要在這裡另寫一份。
  if (!store_) return false;
  return store_->Load().NetworkEnabled();
}

bool NetGate::SetEnabled(bool on) {
  if (!store_) return false;
  Settings s = store_->Load();
  // ⚠ 關掉時寫明確的 false,**不是**把鍵刪掉。
  //   settings.h 的規矩是「設成預設 = 刪掉那個鍵」,但這一顆的預設本來
  //   就是關,而「使用者親手關掉」與「從來沒碰過」是兩件值得分開的事
  //   （前者出現在稽核對話裡:他關過,而紀錄自那之後是空的)。
  //   兩者讀回來都是關,所以行為上沒有差別 —— NetworkEnabled() 只認 kTrue。
  s.SetTri(keys::kNetworkEnabled, on ? Tri::kTrue : Tri::kFalse);
  return store_->Save(s);
}

std::vector<NetLogEntry> NetGate::ReadLog() const {
  if (!store_) return std::vector<NetLogEntry>();
  return store_->ReadNetLog();
}

void NetGate::ClearLog() {
  if (store_) store_->ClearNetLog();
}

std::string NetGate::log_path() const {
  return store_ ? store_->net_log_path() : std::string();
}

namespace {

// 開關與紀錄的接線。兩個都刻意做成「每次重問 / 失敗不致命」:
//   · 開關每一跳重問一次,使用者中途關掉會立刻生效。
//   · 紀錄寫不進去不該讓下載失敗(與 Android 的 record() catch 同一個取捨)。
NetSwitchFn MakeSwitch(SettingsStore* store) {
  return [store]() -> bool {
    if (!store) return false;
    return store->Load().NetworkEnabled();
  };
}

NetRecordFn MakeRecorder(SettingsStore* store) {
  return [store](const NetLogEntry& e) {
    if (store) store->AppendNetLog(e);
  };
}

}  // namespace

NetReport NetGate::FetchText(const std::string& url, NetPurpose purpose,
                             const std::string& label, std::string* out,
                             int64_t max_bytes) {
  if (out) out->clear();
  NetRequest req;
  req.url = url;
  req.purpose = purpose;
  req.label = label;
  req.max_bytes = max_bytes;

  WinHttpTransport transport;
  StringSink sink;
  const NetReport r = RunFetch(req, MakeSwitch(store_), &transport, &sink,
                               MakeRecorder(store_), &NowMs);
  if (r.result == NetResult::kOk && out) *out = sink.data;
  return r;
}

NetReport NetGate::DownloadFile(const std::string& url,
                                const std::string& dest_path, NetPurpose purpose,
                                const std::string& label, int64_t max_bytes) {
  NetRequest req;
  req.url = url;
  req.purpose = purpose;
  req.label = label;
  req.max_bytes = max_bytes;

  FileSink sink(dest_path);
  if (!sink.opened()) {
    // ⚠ 連暫存檔都開不起來的時候什麼都不做 —— 尤其**不要**先連網再發現
    //   寫不下去。開關關著時一個封包都不該出去,這一條同理。
    NetReport r;
    r.result = NetResult::kTransportError;
    r.message = "無法建立暫存檔";
    r.host = HostOf(url);
    return r;
  }

  WinHttpTransport transport;
  NetReport r = RunFetch(req, MakeSwitch(store_), &transport, &sink,
                         MakeRecorder(store_), &NowMs);
  if (r.result == NetResult::kOk) {
    if (!sink.Commit()) {
      r.result = NetResult::kTransportError;
      r.message = "下載完成但存檔失敗";
    }
  } else {
    sink.Discard();
  }
  return r;
}

}  // namespace rimewin
