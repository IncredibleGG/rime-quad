#include "ui_listview.h"

namespace rimewin {
namespace {

bool NotEmpty(const RECT& r) { return r.right > r.left && r.bottom > r.top; }

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

HWND CreateRowList(HWND parent, int id, DWORD extra_style) {
  HWND list = ::CreateWindowExW(
      0, WC_LISTVIEWW, L"", WS_CHILD | extra_style, 0, 0, 10, 10, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      ::GetModuleHandleW(nullptr), nullptr);
  if (!list) return nullptr;
  // 唯一那一欄。寬度之後每次擺位置都由 SyncRowListColumn 重新對齊 ——
  // 建立時的 client 是 10×10,那不是最終的寬度。
  EnsureRowListColumn(list);
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
  if (!list || px <= 0) return;
  HIMAGELIST spacer = ::ImageList_Create(1, px, ILC_COLOR32, 1, 1);
  if (!spacer) return;
  // ⚠ 舊的那一份要自己銷毀。LVSIL_SMALL 的影像清單**不屬於**控制項
  //   (除非設了 LVS_SHAREIMAGELISTS),換掉之後不放的話每次改 DPI 都漏一個。
  HIMAGELIST old = reinterpret_cast<HIMAGELIST>(::SendMessageW(
      list, LVM_SETIMAGELIST, LVSIL_SMALL,
      reinterpret_cast<LPARAM>(spacer)));
  if (old && old != spacer) ::ImageList_Destroy(old);
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
