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

// ── ⚠ 這一支是 #75 的另一半 ─────────────────────────────────────
//
// report 模式的 ListView **預設只有第一欄的 label 矩形算命中**,而 label
// 的寬度是**文字實際的寬度** —— comctl32 從影像清單槽(1 px)之後起算。
// 我們畫的底卻是整列(RowRect() 撐到 client 寬)。兩者差多少取決於那一頁
// 的名字有幾個字:「文字」兩個字的可點範圍不到「輸入方案」的一半,
// 而畫出來的底一樣寬。使用者按在名字右邊的空白上 —— 那裡看起來是那一列,
// 而 comctl32 說沒有命中任何一列。
//
// LVS_EX_FULLROWSELECT 把命中範圍變成整列,也就是我們畫出來的那一塊。
// LVS_EX_DOUBLEBUFFER 順手收掉 comctl32 v6 自繪 + 捲動時的閃爍。
//
// ⚠ 這兩個旗標全樹以前**零命中**（`git grep LVS_EX_` 只有 res/app.manifest
//   裡一句註解）。
//
// ⚠ **hot tracking 沒有開,而且自繪那一側也不再假裝有。**
//   comctl32 的 ListView 只在 LVS_EX_TRACKSELECT / LVS_EX_ONECLICKACTIVATE /
//   LVS_EX_TWOCLICKACTIVATE 或 SetWindowTheme(L"Explorer") 之下才維護
//   hot item,三者這裡都沒有。TRACKSELECT **刻意不補** —— 它會連帶把
//   「滑過就選取」打開,而那不是側欄該有的樣子。
//
//   2026-08-15:settings_window.cc 三處自繪裡讀 CDIS_HOT 的那兩條分支
//   （滑過 / 按下）已經**刪掉**。它們永遠走不到,而留著會讓
//   §12.14.6.6 的「四狀態」在報告上看起來是做完的。
//   要真的做出滑過／按下,要先有辦法看到畫面 —— 而 CI 的截圖
//   （verify_installer.sh §12s）拍不到 hover（runner 上沒有人在動滑鼠）。
//   所以那是另一件事,而且要連同「怎麼驗」一起提。

void SetRowListExtendedStyle(HWND list);

// 把第 row 列設成**唯一**被選取的那一列。row < 0 = 只清不選。
//
// ⚠ **這是整個 windows/ 底下唯一准下 LVM_SETITEMSTATE 的地方**
//   (check_ui_spec.sh 的 W31 守著)。側欄與方案清單都走這一支 ——
//   舊名字叫 SelectSidebarRow,而那個名字讓方案清單那一份複本看起來
//   像是「另一件事」,於是 #80 修好了側欄、方案清單原封不動留著。
//
// ⚠ **先全清再設。** LVS_SINGLESEL 保證的是「使用者點不出第二個選取」;
//   它**不保證**程式化的 LVM_SETITEMSTATE 會先取消舊的那一列,而那正是
//   #80 使用者截圖上兩列同時反白的其中一個可能來源。全清用
//   WPARAM = (WPARAM)-1(所有列)。
//
// ⚠ 抽成一支的理由是**測得到**:CI 上的 rime_tests.exe 開真的控制項、
//   走真的 comctl32,驗的是這一份產品碼本身,不是一份長得很像的複本。
//   「LVS_SINGLESEL 到底管不管程式化設定」這個爭議因此交給 CI 回答,
//   而不是繼續在報告裡互相引用記憶。
void SelectOnlyRow(HWND list, int row);

// 換掉全部的列。文字仍然要寫進 item(自繪畫的是同一份文字,但
// 螢幕閱讀器讀的是 item 上的那一份)。
/// 把一列的高度釘成 `px` 像素(**不是 DIP** —— 呼叫端自己換算)。
///
/// ⚠ 不設的話 comctl32 依字型算,在 t3 下約 20 px,而版面是照 36 算的。
///   後果是「預設」徽章被切掉一半,而且一列低於 28 的最小點擊目標。
///
/// ⚠ **影像清單的高度不等於列高。** CI(Windows run #171)量到的是:
///   掛一個 36 px 的 spacer,`LVM_GETITEMRECT` 回來的是 **37** ——
///   comctl32 自己還會加一格。那 1 px 不是美觀問題:版面
///   (`common/ui_layout.cc` 的 `SidebarListDip`)是照「一列剛好
///   `kSidebarItemH`」算清單有多高的,而側欄是 `LVS_NOSCROLL` ——
///   5 列各多 1 px,最後一列**被裁掉**。
///   所以這一支不猜那一格是多少,而是**掛上去、量回來、再補償**。
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

// 那一列現在是不是被選取的 —— **問控制項本人**。
//
// ⚠ 自繪時**不可以**改讀 `NMCUSTOMDRAW::uItemState` 的 `CDIS_SELECTED`。
//   那件事以前是「不去賭它準不準」,現在是量過的:
//   `tests/test_win32_sidebar.cc` 走一次真的自繪,控制項自己說被選的
//   只有 1 列,而 `uItemState` **5 列裡說 5 列**都被選 ——
//   照它畫的結果是整份清單每一列都反白。
//   (check_ui_spec.sh 的 W31 守著:任何自繪都不准讀那個位元。)
bool RowIsSelected(HWND list, int row);

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_UI_LISTVIEW_H_
