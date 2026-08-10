// windows/common/net_ui.h — 「連網」那一頁的**呈現邏輯**(純函式)
//
// 地基那一層已經有三樣東西:開關(Settings::NetworkEnabled)、單一出口
// (service/net_gate.cc)、連網紀錄(net_policy.h 的 NetLogEntry)。
// 這個檔案是它們與畫面之間的那一段 —— 而它刻意**不在** settings_window.cc
// 裡,理由與 ui_layout.cc / status_cells.cc 完全相同:
//
//   service/ 底下的東西在開發用的 Ubuntu 上編不起來,所以那裡的每一個
//   判斷都只有真人在 Windows 上才驗得到,也就是實際上沒有人驗。
//
// 這一頁上有三件事**寫壞了畫面看起來完全正常**,所以三件都在這裡:
//
//   1. **開關關著時按「檢查更新」不可以連出去。** 判斷是
//      DecideUpdateAction(),它把「開關」排在「已經在跑了」前面 ——
//      順序是規定的,不是實作細節。
//   2. **紀錄是空的時候要說「一次都沒有連過」。** 空白讓人分不出
//      「沒連過」與「壞掉了」,而那正是使用者用來驗證我們的那句話。
//      BuildNetLogView() 一定會給一句 summary,空的時候也給。
//   3. **時間/主機/原因/結果四欄的內容。** 拼字串是最不像會出錯的地方,
//      而它出錯的樣子是「紀錄看起來很正常,只是每一筆都晚了八小時」。
//
// ⚠ 本檔**不得** include windows.h,也不得出現使用者可見的字面值 ——
//   每一個字都從 ui_strings.h 的 catalog 來(W7)。
//
// ⚠ net_policy.cc 裡的 NetPurposeText() / NetOutcomeText() 回的是**窄字串
//   的中文**,那是給紀錄檔與診斷用的,**不是**介面文字:它沒有英文與
//   簡體。畫面上一律走本檔的 NetPurposeUiText() / NetOutcomeUiText()。
//
#ifndef RIMEWIN_NET_UI_H_
#define RIMEWIN_NET_UI_H_

#include <cstdint>
#include <string>
#include <vector>

#include "net_policy.h"
#include "ui_strings.h"

namespace rimewin {

// ── 開關 ────────────────────────────────────────────────────────
//
// 開關底下那一句話。⚠ 開與關**必須是兩句不同的話**:壓成同一句
// (例如只說「連網」)的話,使用者看不出現在到底是哪一種狀態,
// 而這一頁存在的理由就是讓他看得出來。
UiString NetSwitchSummary(bool enabled);
// 按下開關之後,視窗底部那一行說什麼。
UiString NetSwitchStatus(bool enabled);

// ── 連網紀錄 ────────────────────────────────────────────────────

// 一筆紀錄在畫面上的四欄。`line` 是四欄接成一列(單欄清單畫的就是它),
// 四個欄位分開留著是為了讓測試能各驗各的 —— 只驗 line 的話,
// 「主機欄不見了」與「分隔符變了」在斷言上長得一樣。
struct NetLogRow {
  std::wstring when;
  std::wstring host;
  std::wstring reason;
  std::wstring outcome;
  std::wstring line;
};

struct NetLogView {
  // ⚠ 空與不空是**兩件不同的事**,不是「列數等於零」。畫面上要換掉的
  //   不只是清單,還有「清除紀錄」那一整個危險區塊 —— 沒有東西可以清
  //   的時候不該給一顆清除鍵。
  bool empty = true;
  int count = 0;
  // **永遠不是空字串**,空的時候也有一句話。見檔頭第 2 點。
  std::wstring summary;
  // 由**新到舊**(紀錄檔是由舊到新,這裡反過來)。
  std::vector<NetLogRow> rows;
};

// tz_offset_minutes 由呼叫端給(見 LocalTzOffsetMinutes)。傳進來而不是
// 自己去問,是為了讓這一支是純函式 —— 不然測試會跟著跑測試那台機器的
// 時區走,而那種測試在別人的機器上是紅的。
NetLogView BuildNetLogView(const std::vector<NetLogEntry>& entries, UiLang lang,
                           int tz_offset_minutes);

// "YYYY-MM-DD HH:MM:SS"。⚠ 自己算,不走 localtime_r/gmtime ——
// 那兩支吃行程的時區設定,寫出來的測試在別的機器上會是紅的。
std::wstring FormatNetLogTime(int64_t at_ms, int tz_offset_minutes);
// "12 B" / "34 KB" / "5.6 MB"。整數運算,不碰浮點。
std::wstring FormatNetBytes(int64_t bytes);

const wchar_t* NetPurposeUiText(NetPurpose p, UiLang lang);
const wchar_t* NetOutcomeUiText(NetOutcome o, UiLang lang);

// 本地時區與 UTC 的差(分鐘)。⚠ **這一支不是純函式**,是本檔唯一的
// 一支;它問的是這台機器現在的時區。所有格式化都吃它的回傳值當參數,
// 所以格式化本身仍然測得到。
int LocalTzOffsetMinutes();

}  // namespace rimewin

#endif  // RIMEWIN_NET_UI_H_
