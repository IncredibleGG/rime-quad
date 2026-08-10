// windows/service/net_gate.h — Windows 端**唯一**的連網出口
//
// ═══════════════════════════════════════════════════════════════════
//  這個檔案(與它的 .cc)是「這個輸入法離線為預設」這句話的兌現處。
// ═══════════════════════════════════════════════════════════════════
//
// 想確認這個輸入法有沒有偷偷連網,不必讀完整個專案 —— 讀
// windows/service/net_gate.cc 一個檔案就夠了,而「只有它一個」這件事
// 由 windows/audit_offline_win.sh 在 CI 上守著(它的 ALLOW 清單裡
// 恰好一個路徑,而且會斷言那個路徑真的存在、真的碰得到網路 API)。
//
// 三層,與 Android 的 net/NetworkGate.kt 同一套:
//
//   1. **單一出口**。整個 windows/ 底下只有 net_gate.cc 碰得到 WinHTTP。
//      另一層在 windows/check_binaries.sh:它看**產物的匯入表** ——
//      rime_tsf.dll 永遠是零,那支住在每一個接受文字輸入的進程裡
//      (含瀏覽器與提權進程),它有網路能力這件事光讀原始碼不會有人發現。
//   2. **開關預設關,而且 fail-closed**。讀不到設定、設定檔壞了、
//      還沒初始化 —— 一律當成關。「不知道」必須等於「關」。
//      判斷只有一處:Settings::NetworkEnabled()。
//   3. **連網紀錄**。每一次**真的送出去**的請求記一筆:時間、主機、
//      原因、結果、位元組數。⚠ 被開關擋下的嘗試**刻意不記** ——
//      記了的話,「開關從沒開過所以紀錄是空的」這句話就不成立,
//      而那正是使用者驗證我們的方式。規則寫在 common/net_policy.h,
//      流程寫在 common/net_gate_core.h(那一份在 Ubuntu 上有單元測試)。
//
// ═══════════════════════════════════════════════════════════════════
//  ⚠ 信任錨:Windows 這一端只有 TLS,沒有程式碼簽章
// ═══════════════════════════════════════════════════════════════════
//
// 這一段是**限制的誠實交代**,不要在文案、註解或 UI 上把它含糊掉。
//
//   · **我們沒有程式碼簽章憑證。** 使用者下載安裝檔時 SmartScreen 會攔,
//     而那個警告是對的 —— 我們確實沒有被驗證過的發行者身分。
//     這一端**不做**任何簽章驗證,也不得宣稱有。
//
//   · 所以**憑證鏈是唯一的信任來源**:我們連得到正確的主機、
//     內容在傳輸中沒有被改,靠的全部是 TLS。因此
//     `net_gate.cc` 絕不放寬憑證檢查(沒有 SECURITY_FLAG_IGNORE_*,
//     一個都沒有),而且 https 被轉去 http 時會中止 ——
//     一次降級轉址就把唯一的錨拿掉了,見 common/net_gate_core.h 的
//     NoDowngrade。
//
//   · **sha256 擋不住惡意的來源。** 下載完之後比對 sha256 是有價值的,
//     但要看清楚它擋的是什麼:那個 sha256 本身來自**同一條連線**的索引。
//     連線被接管的話,對方可以同時換掉套件與它的 sha256,而比對會通過。
//     它擋得住的是**傳輸損壞與截斷**,擋不住一個有能力偽造 TLS 憑證、
//     或是控制了索引來源的對手。
//     真正解決這一段的是索引簽章(docs/offline-threat-model.md §5),
//     那還沒有做。在它做完之前,不要在任何地方寫「已驗證」。
//
//   · 網路上的觀察者仍然看得到我們連了哪個網域(DNS 與 TLS 的 SNI 是
//     明文)。User-Agent 刻意不帶本專案的名字(理由與 Android 相同,
//     見 net_gate.cc),但那改變不了 DNS/SNI 這件事,也不要假裝改得了。
//
// ═══════════════════════════════════════════════════════════════════
//
// ⚠ 本檔**只有**出口、開關、紀錄。更新與方案市集的邏輯**不在這裡**,
//   那是下一階段的事。這裡不做 sha256、不解壓、不碰索引格式 ——
//   出口愈小,「只有這一個地方連得了網」愈容易被相信。
//
// ⚠ 執行緒:FetchText / DownloadFile 都是**同步阻塞**的,一律由呼叫端
//   放到背景執行緒跑。絕不可以在 UI 執行緒上呼叫 —— 服務進程的 UI
//   執行緒同時在跑候選窗,卡住它就是「打字打到一半整個沒反應」。
//
#ifndef RIMEWIN_SERVICE_NET_GATE_H_
#define RIMEWIN_SERVICE_NET_GATE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "../common/net_gate_core.h"
#include "../common/net_policy.h"
#include "settings_store.h"

namespace rimewin {

class NetGate {
 public:
  // store 必須活得比 NetGate 久。開關與紀錄都存在它那裡。
  explicit NetGate(SettingsStore* store);

  // ── 開關 ────────────────────────────────────────────────────
  //
  // ⚠ 預設**關**,而且每一跳都會重新問一次 —— 使用者在下載進行到
  //   一半時把它關掉,那一下必須真的中斷後續連線。
  bool Enabled() const;
  // 回傳 false = 設定沒寫成功,呼叫端要告訴使用者。
  // 安靜地失敗會變成「開關關了,重開又是開的」。
  bool SetEnabled(bool on);

  // ── 出口 ────────────────────────────────────────────────────
  //
  // 取一份文字(方案市集的索引)。out 在失敗時會被清空。
  NetReport FetchText(const std::string& url, NetPurpose purpose,
                      const std::string& label, std::string* out,
                      int64_t max_bytes = kMaxIndexBytes);

  // 下載到檔案。**先寫 <dest>.part,成功才改名成 dest** ——
  // 中途失敗、中途被開關擋下、超過大小上限,都不會在硬碟上留下一個
  // 看起來下載完成的半份檔案。
  //
  // ⚠ 這裡**不算 sha256**。要不要接受這一包是政策問題,不是傳輸問題,
  //   而且 sha256 能擋什麼、不能擋什麼見本檔開頭的信任錨那一段。
  NetReport DownloadFile(const std::string& url, const std::string& dest_path,
                         NetPurpose purpose, const std::string& label,
                         int64_t max_bytes = kMaxPackageBytes);

  // ── 連網紀錄 ────────────────────────────────────────────────
  //
  // 由舊到新。⚠ 讀取**不會建立檔案**:「開關從沒開過 → 紀錄檔根本
  // 不存在」與「紀錄檔存在但是空的」對稽核的人來說不是同一句話。
  std::vector<NetLogEntry> ReadLog() const;
  void ClearLog();
  // 給設定介面的「開啟資料夾」用。
  std::string log_path() const;

 private:
  SettingsStore* store_;
};

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_NET_GATE_H_
