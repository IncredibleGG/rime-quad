// windows/service/ui_listview.h — 單欄、自繪的 ListView(側欄與方案清單共用)
//
// ── 為什麼把它從 settings_window.cc 抽出來 ───────────────────────
//
// 設定視窗裡有兩個 SysListView32:側欄(四頁)與方案清單。兩邊的建立、
// 填列、自繪都是各寫一份的複本,而**兩份複本不一樣**:側欄的欄寬每次
// `LayoutUi()` 都重設,方案清單的只在建立時設一次。使用者實機回報的
// 「『啟用的方式』底下是一個空白的 list」正好只發生在後者。
//
// 兩份複本的差異能不能解釋那個症狀,是**只有真的 Windows 答得出來**的
// 問題 —— 而它以前沒有任何自動化碰得到:`windows/` 底下所有 UI 的
// 單元測試都刻意不 include windows.h,所以「畫得出來嗎」整件事在
// CI 上不存在。抽成這一支之後,`tests/test_win32_listview.cc` 可以開
// 真的控制項、走真的 comctl32、把畫面渲染進點陣圖數像素,
// 而且數的是**產品碼本身**,不是一份長得很像的複本。
//
// ⚠ 本檔是 Windows 專屬的(include windows.h / commctrl.h),
//   所以它進不了 run_logic_tests.sh 那一組。它進的是 rime_tests.exe ——
//   那一支在 windows-latest 上跑,tests/test_win32_layouts.cc 已經
//   用同樣的方式驗真的鍵盤佈局。
//
#ifndef RIMEWIN_SERVICE_UI_LISTVIEW_H_
#define RIMEWIN_SERVICE_UI_LISTVIEW_H_

#include <windows.h>

#include <commctrl.h>

#include <string>
#include <vector>

namespace rimewin {

// 建一個單欄、無標頭、自繪的 ListView。
// `extra_style` 是 WS_CHILD 以外的樣式(呼叫端自己決定要不要 WS_BORDER、
// LVS_NOSCROLL…)。**欄一定會被插進去** —— report 模式沒有欄的話,
// 每一列的矩形都是空的,而那不是「醜」,是「什麼都看不到」。
HWND CreateRowList(HWND parent, int id, DWORD extra_style);

// 補上唯一那一欄(已經有欄就什麼都不做)。控制項不是由 CreateRowList
// 建出來的時候(設定視窗那張控制項表統一建)要自己呼叫一次。
void EnsureRowListColumn(HWND list);

// 換掉全部的列。文字仍然要寫進 item(自繪畫的是同一份文字,但
// 螢幕閱讀器讀的是 item 上的那一份)。
/// 把一列的高度釘成 `px` 像素(**不是 DIP** —— 呼叫端自己換算)。
///
/// ⚠ 不設的話 comctl32 依字型算,在 t3 下約 20 px,而版面是照 36 算的。
///   後果是「預設」徽章被切掉一半,而且一列低於 28 的最小點擊目標。
void SetRowListRowHeight(HWND list, int px);

void SetRowListItems(HWND list, const std::vector<std::wstring>& rows);

// ⚠ **控制項一改大小就要呼叫這一支。**
//
//   單欄的 report 清單,那一欄的寬度**不會**跟著控制項走。建立時設 640、
//   之後被擺成 440 寬,那一欄仍然是 640(橫向捲出去);反過來建立時
//   算出 0(例如 DPI 還沒問到),那一欄就永遠是 0,而每一列的矩形
//   都是空的 —— 清單看起來是空的,雖然裡面有列。
void SyncRowListColumn(HWND list);

// 自繪時,這一列該畫在哪。
//
// ⚠ **不要直接用 `NMCUSTOMDRAW::rc`。** report 模式的 ListView 在
//   `CDDS_ITEMPREPAINT` 這一趟給的矩形不保證有意義,而「拿到一個空矩形
//   照樣 FillRect + DrawText」的結果是**一片空白而且沒有任何錯誤**。
//   這一支的順序是:rc 可用就用 rc;不可用就 `LVM_GETITEMRECT` 自己問;
//   問到的矩形寬度是 0(單欄清單的欄寬沒跟上)就撐成 client 的寬度 ——
//   單欄清單裡「一列有多寬」本來就等於控制項有多寬,不是那一欄的事。
//
// 回傳 false = 真的沒有矩形可以畫(控制項不見了)。呼叫端這時**不要**畫。
bool RowRect(HWND list, const NMLVCUSTOMDRAW* cd, RECT* out);

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_UI_LISTVIEW_H_
