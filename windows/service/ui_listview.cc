#include "ui_listview.h"

namespace rimewin {
namespace {

bool NotEmpty(const RECT& r) { return r.right > r.left && r.bottom > r.top; }

// 掛一個「只有高度、沒有圖」的影像清單。
//
// ⚠ 舊的那一份要自己銷毀。LVSIL_SMALL 的影像清單**不屬於**控制項
//   (除非設了 LVS_SHAREIMAGELISTS),換掉之後不放的話每次改 DPI 都漏一個。
bool ApplyRowSpacer(HWND list, int cy) {
  if (!list || cy <= 0) return false;
  HIMAGELIST spacer = ::ImageList_Create(1, cy, ILC_COLOR32, 1, 1);
  if (!spacer) return false;
  HIMAGELIST old = reinterpret_cast<HIMAGELIST>(::SendMessageW(
      list, LVM_SETIMAGELIST, LVSIL_SMALL,
      reinterpret_cast<LPARAM>(spacer)));
  if (old && old != spacer) ::ImageList_Destroy(old);
  return true;
}

// 控制項**實際**的列高(像素)。量不到回 0 —— 沒有列可以量的時候
// LVM_GETITEMRECT 是失敗的,而那時候不可以假裝量到了 0。
int MeasuredRowHeight(HWND list) {
  if (!list) return 0;
  RECT r{};
  r.left = LVIR_BOUNDS;
  if (!::SendMessageW(list, LVM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&r)))
    return 0;
  return static_cast<int>(r.bottom - r.top);
}

}  // namespace

void EnsureRowListColumn(HWND list) {
  if (!list) return;
  HWND header = reinterpret_cast<HWND>(
      ::SendMessageW(list, LVM_GETHEADER, 0, 0));
  if (header && ::SendMessageW(header, HDM_GETITEMCOUNT, 0, 0) > 0) return;
  LVCOLUMNW col{};
  col.mask = LVCF_WIDTH;
  col.cx = 10;
  ::SendMessageW(list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&col));
}

void SetRowListExtendedStyle(HWND list) {
  if (!list) return;
  const DWORD mask = LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER;
  // 第一個參數是**遮罩**:只動這兩位元,別人設過的其他旗標不受影響。
  ::SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, mask, mask);
}

void SelectOnlyRow(HWND list, int row) {
  if (!list) return;
  // ⚠ 先全清。見標頭:LVS_SINGLESEL 管的是使用者,不是我們。
  LVITEMW clear{};
  clear.mask = LVIF_STATE;
  clear.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
  clear.state = 0;
  ::SendMessageW(list, LVM_SETITEMSTATE, static_cast<WPARAM>(-1),
                 reinterpret_cast<LPARAM>(&clear));
  if (row < 0) return;
  LVITEMW set{};
  set.mask = LVIF_STATE;
  set.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
  set.state = LVIS_SELECTED | LVIS_FOCUSED;
  ::SendMessageW(list, LVM_SETITEMSTATE, static_cast<WPARAM>(row),
                 reinterpret_cast<LPARAM>(&set));
}

HWND CreateRowList(HWND parent, int id, DWORD extra_style) {
  HWND list = ::CreateWindowExW(
      0, WC_LISTVIEWW, L"", WS_CHILD | extra_style, 0, 0, 10, 10, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      ::GetModuleHandleW(nullptr), nullptr);
  if (!list) return nullptr;
  // 唯一那一欄。寬度之後每次擺位置都由 SyncRowListColumn 重新對齊 ——
  // 建立時的 client 是 10×10,那不是最終的寬度。
  EnsureRowListColumn(list);
  // 整列可點 + 雙緩衝。見標頭 —— 這是「畫出來的地方」與「點得到的地方」
  // 對齊的另一半。
  SetRowListExtendedStyle(list);
  return list;
}

void SetRowListRowHeight(HWND list, int px) {
  // ⚠ **列高從來沒有被設定過**,而這造成了一個使用者看得到的缺陷。
  //
  //   comctl32 依字型自己算列高,在 t3(13 DIP)下約 20 px。而
  //   settings_window.cc 的「預設」徽章高度 = 列高 − 2×space::s3 = 列高 − 12,
  //   於是徽章只剩 **8 px**,而 TextLineBoxDip(t5) 要 **16** ——
  //   `DrawTextW` 沒有 DT_NOCLIP,所以那兩個字**剛好被切掉一半**。
  //   使用者實機回報:「清單右上角那個徽章畫壞了」,而他讀到的是
  //   殘存的那一條橫帶。
  //
  //   同一個根因還讓一列只有 20 px —— 低於 §3.6 的 28 最小點擊目標,
  //   而那是可點的一列;`ui_layout.cc` 的版面又是照「一列 36」算高度的,
  //   所以那個框實際裝得下約 7 列而不是它以為的 4 列。
  //
  // ⚠ 為什麼用影像清單而不是 LVS_OWNERDRAWFIXED:
  //   OWNERDRAWFIXED 會讓整列改走 WM_DRAWITEM,而這個專案的列是用
  //   NM_CUSTOMDRAW 畫的(兩者不能並存)。掛一個「只有高度、沒有圖」的
  //   影像清單是 Win32 上調列高的標準做法,而且不影響現有的繪製路徑。
  //   寬度給 1 而不是 0:0 在某些版本上會被當成「沒有影像清單」。
  //
  // ⚠ **spacer 的高度不是列高。** 掛 36 px 上去,CI(run #171)上的
  //   comctl32 給的是 **37** —— 它自己還會加一格。而版面
  //   (ui_layout.cc 的 SidebarListDip)是照「一列剛好 36」算清單高度的,
  //   側欄又是 LVS_NOSCROLL:5 列各多 1 px,最後一列**被裁掉**。
  //   那一格是多少不必知道也不該猜(換一版 comctl32、換一種字型都可能
  //   不一樣)—— **掛上去、量回來、把差補掉**就好。
  if (!list || px <= 0) return;
  // ⚠ 量得到列高需要至少一列,而 schema_list_ 在 ApplyFonts 跑的時候
  //   還是空的。空的話暫時借一列,量完刪掉:插入與清空都不會產生帶
  //   LVIS_SELECTED 的 LVN_ITEMCHANGED,選取那條線碰不到
  //   (OnNotify 只在「沒選 → 有選」的上升緣才動作)。
  const bool borrowed = ::SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 0;
  if (borrowed) {
    wchar_t blank[] = L"";
    LVITEMW probe{};
    probe.mask = LVIF_TEXT;
    probe.iItem = 0;
    probe.pszText = blank;
    ::SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&probe));
  }
  int cy = px;
  // 收斂通常一輪就到(差是個常數)。上限擺著是防呆:量不到、或某一版
  // comctl32 讓它來回震盪的話,寧可停在「至少掛上去了」的舊行為。
  for (int i = 0; i < 4; ++i) {
    if (!ApplyRowSpacer(list, cy)) break;
    const int got = MeasuredRowHeight(list);
    if (got <= 0 || got == px) break;
    const int next = cy - (got - px);
    if (next <= 0 || next == cy) break;
    cy = next;
  }
  if (borrowed) ::SendMessageW(list, LVM_DELETEALLITEMS, 0, 0);
}

void SetRowListItems(HWND list, const std::vector<std::wstring>& rows) {
  if (!list) return;
  ::SendMessageW(list, LVM_DELETEALLITEMS, 0, 0);
  for (size_t i = 0; i < rows.size(); ++i) {
    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = static_cast<int>(i);
    // 自繪畫的是同一份文字,但 item 上這一份仍然要設:螢幕閱讀器
    // 讀的是它,而自繪不會產生任何可朗讀的東西。
    it.pszText = const_cast<wchar_t*>(rows[i].c_str());
    ::SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it));
  }
}

bool RowIsSelected(HWND list, int row) {
  if (!list || row < 0) return false;
  // ⚠ 見標頭:**不讀 NMCUSTOMDRAW::uItemState**。這一支問的是控制項
  //   自己那一份,而那一份是權威的。
  return (::SendMessageW(list, LVM_GETITEMSTATE, static_cast<WPARAM>(row),
                         static_cast<LPARAM>(LVIS_SELECTED)) &
          LVIS_SELECTED) != 0;
}

void SyncRowListColumn(HWND list) {
  if (!list) return;
  RECT rc{};
  ::GetClientRect(list, &rc);
  int w = rc.right - rc.left;
  if (w <= 0) return;
  ::SendMessageW(list, LVM_SETCOLUMNWIDTH, 0, MAKELPARAM(w, 0));
}

bool RowRect(HWND list, const NMLVCUSTOMDRAW* cd, RECT* out) {
  if (!out) return false;
  *out = RECT{};
  if (!cd) return false;

  RECT r = cd->nmcd.rc;
  if (!NotEmpty(r)) {
    // custom draw 沒有給有意義的矩形 —— 自己問控制項。
    if (!list) return false;
    RECT q{};
    q.left = LVIR_BOUNDS;
    if (!::SendMessageW(list, LVM_GETITEMRECT,
                        static_cast<WPARAM>(cd->nmcd.dwItemSpec),
                        reinterpret_cast<LPARAM>(&q)))
      return false;
    r = q;
  }
  if (r.bottom <= r.top) return false;

  // 單欄清單:一列的寬度就是控制項的寬度。欄寬沒跟上時(建立時算的那個
  // 值早就過期了)這裡把它撐回來 —— 不撐的話,清單裡有列而畫面是空的。
  if (list) {
    RECT client{};
    ::GetClientRect(list, &client);
    if (client.right > client.left) {
      if (r.left < client.left) r.left = client.left;
      if (r.right < client.right) r.right = client.right;
    }
  }
  if (!NotEmpty(r)) return false;
  *out = r;
  return true;
}

}  // namespace rimewin
