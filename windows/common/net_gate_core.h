// windows/common/net_gate_core.h — 連網出口的**流程**(純邏輯,不開任何連線)
//
// net_policy.h 是「這一跳准不准」的判斷,net_gate.cc 是「真的開 socket」的
// 那一半。中間還有一段東西,而它才是最容易寫壞的:
//
//   問開關 → 檢查網址 → 送出請求 → 看狀態碼 → 跟轉址 → 邊收邊數 → 記一筆
//
// 這一段以前會直接長在 net_gate.cc 裡,而那個檔案 include winhttp.h,
// 於是它在開發用的 Ubuntu 上編不起來,也就是**沒有任何自動化碰得到它**。
// 「開關關著時真的一個位元組都沒出去」這句話,會變成只有真人在 Windows 上
// 拔網路線才驗得到 —— 也就是實際上沒有人驗。
//
// 所以流程搬到這裡,連線動作抽成 NetTransport 介面。這個檔案不 include
// windows.h、不 include winhttp.h,windows/run_logic_tests.sh 在 Ubuntu 上
// 直接對它下斷言(windows/tests/test_net_gate_core.cc)。
//
// ── ⚠ 這個檔案存在的唯一理由:兩條規則要用單元測試守住 ──────────
//
//   1. **開關關著時,NetTransport 一次都不會被呼叫。**
//      不是「呼叫了但失敗」,是根本沒走到那裡。
//   2. **只有真的送出請求之後才會產生紀錄。**
//      被開關擋下、網址壞掉、scheme 不准 —— 一個字都不記。
//      理由見 net_policy.h 檔頭:記了的話,「開關從沒開過所以紀錄是空的」
//      這句話就不成立,而那正是使用者驗證我們的方式。
//
//   ⚠ 這兩條各自對應一種**植入違規**的反向測試(見 test_net_gate_core.cc
//     檔尾)。改這個檔案的人請先去讀那兩段。
//
// ── NetReport::hops 是什麼,為什麼它是關鍵 ──────────────────────
//
// hops = **真的送出去的請求數**。它同時就是「這次呼叫應該產生幾筆紀錄」。
// 於是「只有真的連線才記」這條規則變成一行可以斷言的等式:
//
//     記錄到的筆數 == report.hops
//
// 測試在**每一個**情境都斷言這一條。有人讓被擋下的嘗試也記一筆,
// 等式左邊會多一;有人讓真的連線漏記,右邊會多一。兩邊都會紅。
//
#ifndef RIMEWIN_NET_GATE_CORE_H_
#define RIMEWIN_NET_GATE_CORE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "net_policy.h"

namespace rimewin {

// 這次呼叫的結果。**kBlocked 一定要與其他失敗分得開** ——
// 「連不上伺服器」要給重試,「開關是關的」要給一顆開啟開關的按鈕,
// 而把兩者壓成同一句紅字正是這個專案在 Windows 端犯過的錯。
enum class NetResult {
  kOk = 0,
  kBlocked,           // 總開關是關的。**不曾連線,不曾記錄**
  kBadUrl,            // 網址解析不出來(還沒連線,不記錄)
  kBadScheme,         // 不是 http/https(還沒連線,不記錄)
  kDowngraded,        // https 被轉去 http(還沒連上新的那一跳,不記錄)
  kTooManyRedirects,
  kTooLarge,
  kHttpError,         // 連上了,但不是 200
  kTransportError,    // 連線本身失敗或中斷
};

const char* NetResultText(NetResult r);
// kBlocked 以外都是「網路那邊的問題」。UI 用這一條決定要不要給重試按鈕。
bool NetResultIsBlocked(NetResult r);

// 收下來的位元組往哪裡去。實作:記憶體字串(取索引)或檔案(下載套件)。
class NetSink {
 public:
  virtual ~NetSink() = default;
  // 回傳 false = 寫不下去(磁碟滿、權限)。核心會中止這次下載。
  virtual bool Write(const char* data, size_t n) = 0;
  // 這次不算數,把已經寫進去的東西丟掉。
  // ⚠ 中途被開關擋下時也會呼叫它:使用者按下「關閉連網」之後,
  //   硬碟上不該留著半個下載到一半的檔案。
  virtual void Discard() = 0;
};

// 真的開連線的那一半。**唯一的實作在 windows/service/net_gate.cc**,
// 而那是整個 windows/ 底下唯一准許出現 WinHttp* 的翻譯單元
// (由 windows/audit_offline_win.sh 守住)。
class NetTransport {
 public:
  virtual ~NetTransport() = default;

  struct Head {
    // ⚠ **這個欄位就是「要不要記錄」的分界線。**
    //   true = 連線 API 真的被呼叫了(DNS 查詢已經送出去了),
    //   不論後面成不成功,網路上都已經留下痕跡 → 必須記一筆。
    //   false = 連 handle 都沒開起來,一個封包都沒出去 → 不記。
    bool sent = false;
    bool ok = false;              // 拿到回應標頭了嗎
    int status = 0;               // HTTP 狀態碼
    std::string location;         // 3xx 的 Location,可能是相對網址
    int64_t declared_bytes = -1;  // Content-Length,-1 = 沒宣告
    std::string error;            // 一句話,會進紀錄的 detail 欄
  };

  // 開連線、送出 GET、讀回應標頭。
  virtual void Begin(const std::string& url, Head* out) = 0;
  // 邊收邊寫進 sink,邊數位元組。超過 max_bytes 立刻中止並把 over_limit 設 true。
  virtual bool ReadBody(NetSink* sink, int64_t max_bytes, int64_t* read,
                        bool* over_limit, std::string* error) = 0;
  // 收掉這一跳的 handle。每一次 Begin 之後**一定**會有一次 End。
  virtual void End() = 0;
};

struct NetRequest {
  std::string url;
  NetPurpose purpose = NetPurpose::kStoreIndex;
  std::string label;                  // 例如套件名。會進紀錄,可為空
  int64_t max_bytes = kMaxIndexBytes;
};

struct NetReport {
  NetResult result = NetResult::kTransportError;
  std::string message;   // 給使用者看的一句話
  int64_t bytes = 0;     // 只有 kOk 有意義
  std::string host;      // 最後一跳的主機。**永不為空**(見 HostOf)
  // 真的送出去的請求數 = 這次呼叫應該產生的紀錄筆數。見檔頭。
  int hops = 0;
};

// 「現在可以連網嗎」。**每一跳都會重新問一次** ——
// 使用者可能在下載進行到一半時把開關關掉,那一下必須真的中斷後續連線。
using NetSwitchFn = std::function<bool()>;
using NetRecordFn = std::function<void(const NetLogEntry&)>;
using NetClockFn = std::function<int64_t()>;

// 走完一次「取一份東西回來」。transport / sink 不可為 null。
//
// ⚠ is_enabled 讀不到、丟例外、沒接上 → 呼叫端必須傳一個回 false 的函式。
//   「不知道」等於「關」。這裡不替呼叫端猜:傳空的 std::function 進來,
//   下面會當成 false。
NetReport RunFetch(const NetRequest& req, const NetSwitchFn& is_enabled,
                   NetTransport* transport, NetSink* sink,
                   const NetRecordFn& record, const NetClockFn& now_ms);

// ── 轉址的兩條規則 ──────────────────────────────────────────────
//
// net_policy.h 明著說**不做主機白名單**:索引來源是使用者可以改的,
// 轉址可以落在任何主機,設計上讓目的地看得見(每一跳都記)而不是限制它。
// 這裡照辦 —— 不比對主機。
//
// ⚠ 但有一件事 net_policy.h 沒有涵蓋,而它不是白名單問題:
//   **https 轉去 http**。Windows 端沒有程式碼簽章(見 net_gate.h 檔頭),
//   TLS 憑證是**唯一**的信任錨。一次降級轉址就把那個唯一的錨拿掉了,
//   而使用者在畫面上看不出差別 —— 紀錄裡會有一筆主機名一樣的連線。
//   Android 端的 NetworkGate.redirectAllowed 本來就擋這一條。
//
//   反過來 http → https 是允許的(那是升級),http → http 也允許
//   (一開始就沒有錨可以掉)。
//
//   ⚠ 這個函式**只**對 http 說話。https → file:// / ftp:// 一樣走不通,
//     但那是「scheme 不准」而不是「降級」,由下一跳的 CheckHop 回答。
//     分開的理由與 net_policy.cc 把 scheme 排在 authority 之前一樣:
//     兩者要對使用者說的那句話不同,而說錯話比不說更糟。
bool NoDowngrade(const std::string& from, const std::string& to);

// Location 標頭自己的 scheme 准不准。相對網址一律回 true(它沒有 scheme,
// 由 ResolveUrl 接上 base 的)。
//
// ⚠ **這一條看起來多餘,實際上不是。** net_policy.h 的 ResolveUrl 只認得
//   `http://` / `https://` 開頭的絕對網址,其餘一律當成**相對路徑**接在
//   base 後面。於是 `Location: file:///etc/passwd` 會被拼成
//
//       https://<原主機>/file:///etc/passwd
//
//   —— 一個合法的 https 網址,CheckHop 看了說 kProceed。實際危害不大
//   (它連不到本機檔案,只是對原主機發一個沒有意義的請求),但
//   「**每一跳的 scheme 都驗過**」這句話會變成假的,而那句話正是我們
//   在稽核時要交代的東西。守門要守的是那句話,不是它的近似值。
//
//   不改 ResolveUrl 的理由:它是 net_policy.h 的一部分,四端共用,
//   而且已經有測試釘住現有行為。這裡多驗一次,成本是一個純函式。
bool LocationSchemeAllowed(const std::string& location);

}  // namespace rimewin

#endif  // RIMEWIN_NET_GATE_CORE_H_
