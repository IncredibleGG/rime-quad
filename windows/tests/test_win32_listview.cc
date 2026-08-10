// windows/tests/test_win32_listview.cc — 自繪的 ListView **真的畫得出東西嗎**
//
// ── 為什麼需要這一支 ────────────────────────────────────────────
//
// 使用者實機回報:設定 →「輸入方案」頁,「啟用的方式」底下那個清單
// **一列都沒有**,而它下面那句「現在預設是『朙月拼音·臺灣正體』」是滿的。
// 也就是說 `schemas_` 不是空的(那句話就是拿 `schemas_[0]` 組出來的),
// 版面也把清單與上移／下移／套用三顆鈕都排出來了 —— 資料在、控制項在,
// 而**畫面上一列都沒有**。
//
// 側欄與方案清單是同一種控制項(SysListView32,LVS_REPORT + 自繪),
// 兩邊的 `CDDS_ITEMPREPAINT` 都直接用 `NMCUSTOMDRAW::rc` 當那一列的矩形。
// 這一支問的就是那件事:**report 模式的 custom draw 給的 rc 能不能用。**
//
// ⚠ 這在 Ubuntu 上驗不到,而且**不可以**用讀原始碼的方式驗 ——
//   「有沒有呼叫 DrawTextW」永遠是綠的,而使用者看到的是一片空白。
//   所以這一支開真的 ListView、走真的 comctl32、把畫面渲染進一張點陣圖,
//   然後**數黑色像素**。零 = 使用者回報的那個症狀。
//
// ⚠ 沒有 SKIP。harness 自己壞掉(一次 CDDS_ITEMPREPAINT 都沒收到、
//   點陣圖建不起來)一律算失敗 —— 這個專案吃過「測試安靜地跳過自己」的虧。

#include <windows.h>

#include <commctrl.h>

#include <cstdio>

#include "../service/ui_listview.h"
#include "check.h"

using namespace rimewin;

namespace {

constexpr int kRows = 3;
constexpr int kCanvasW = 420;
constexpr int kCanvasH = 220;

// 畫布刷成洋紅,列底刷成白,字用純黑。三個顏色互不相同,所以
// 「有沒有畫底」與「有沒有畫字」分得開。
constexpr COLORREF kCanvas = RGB(255, 0, 255);
constexpr COLORREF kRowBg = RGB(255, 255, 255);
constexpr COLORREF kRowFg = RGB(0, 0, 0);

constexpr wchar_t kProbeClass[] = L"LuminaKeyListViewProbe";

struct ListProbe {
  HWND list = nullptr;
  int id = 0;
  int prepaint = 0;
  int itemprepaint = 0;
  // custom draw **直接給**的矩形,以及 RowRect() 決定要用的那一份。
  RECT raw[kRows]{};
  RECT used[kRows]{};
  // 產品碼走 RowRect();設成 false 就是「修正被拿掉」的樣子(直接用 rc)。
  bool use_row_rect = true;
};

ListProbe g_sidebar_like;
ListProbe g_schema_like;

bool RectEmpty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

LRESULT DrawRow(ListProbe* p, NMLVCUSTOMDRAW* cd) {
  switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
      ++p->prepaint;
      return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
      const int i = static_cast<int>(cd->nmcd.dwItemSpec);
      ++p->itemprepaint;

      RECT r{};
      const bool ok = p->use_row_rect ? RowRect(p->list, cd, &r)
                                      : (r = cd->nmcd.rc, !RectEmpty(r));
      if (i >= 0 && i < kRows) {
        p->raw[i] = cd->nmcd.rc;
        p->used[i] = r;
      }
      if (!ok) return CDRF_SKIPDEFAULT;  // 沒有矩形可以畫 —— 正是那個症狀

      HDC hdc = cd->nmcd.hdc;
      HBRUSH br = ::CreateSolidBrush(kRowBg);
      ::FillRect(hdc, &r, br);
      ::DeleteObject(br);
      ::SetBkMode(hdc, TRANSPARENT);
      ::SetTextColor(hdc, kRowFg);
      // ⚠ 刻意**不**挑字型:這裡量的是「畫得到畫不到」,不是好不好看,
      //   而 §8.6.0 禁止用系統 UI 字型當預設(W5 掃 DEFAULT_GUI_FONT)。
      //   DC 自己帶的那一份畫出來一樣是黑的,這一支要的就是黑色像素。
      ::DrawTextW(hdc, L"MMMMMMMMMM", -1, &r,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      return CDRF_SKIPDEFAULT;
    }
    default:
      return CDRF_DODEFAULT;
  }
}

LRESULT CALLBACK ProbeProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  if (msg == WM_NOTIFY) {
    NMHDR* nm = reinterpret_cast<NMHDR*>(l);
    if (nm && nm->code == NM_CUSTOMDRAW) {
      NMLVCUSTOMDRAW* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(nm);
      if (nm->idFrom == static_cast<UINT_PTR>(g_sidebar_like.id))
        return DrawRow(&g_sidebar_like, cd);
      if (nm->idFrom == static_cast<UINT_PTR>(g_schema_like.id))
        return DrawRow(&g_schema_like, cd);
    }
    return 0;
  }
  if (msg == WM_DPICHANGED) {
    // 進程是 per-monitor-v2,系統**會**送這一則。照建議矩形重擺 ——
    // 這支探針的視窗離螢幕,但「收到了就不理」在別處是缺陷,
    // 不該因為是測試就寫成別的樣子。
    const RECT* sug = reinterpret_cast<const RECT*>(l);
    if (sug)
      ::SetWindowPos(hwnd, nullptr, sug->left, sug->top, sug->right - sug->left,
                     sug->bottom - sug->top, SWP_NOZORDER | SWP_NOACTIVATE);
    return 0;
  }
  if (msg == WM_ERASEBKGND) {
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    HBRUSH br = ::CreateSolidBrush(kCanvas);
    ::FillRect(reinterpret_cast<HDC>(w), &rc, br);
    ::DeleteObject(br);
    return 1;
  }
  return ::DefWindowProcW(hwnd, msg, w, l);
}

void EnsureClass() {
  static bool done = false;
  if (done) return;
  done = true;
  INITCOMMONCONTROLSEX icc{};
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
  ::InitCommonControlsEx(&icc);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &ProbeProc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kProbeClass;
  ::RegisterClassExW(&wc);
}

// 把控制項畫進一張點陣圖,回傳「列的區域裡有幾個黑色像素」。
// ⚠ 用 WM_PRINTCLIENT 而不是螢幕擷取:runner 上沒有人在看畫面,
//   離螢幕的視窗有沒有有效的內容取決於 DWM,那不是可以依賴的東西。
long BlackPixels(ListProbe* p, int w, int h) {
  HWND ctl = p->list;
  if (!ctl) return -1;
  // ⚠ 計數器在這裡歸零:下面斷言的 itemprepaint 必須是**這一趟**收到的。
  //   不歸零的話,「WM_PRINTCLIENT 根本沒有走繪製」會被稍早那次正常
  //   重畫的數字蓋掉,而 harness 壞掉會偽裝成產品壞掉。
  p->prepaint = 0;
  p->itemprepaint = 0;
  HDC screen = ::GetDC(nullptr);
  if (!screen) return -1;
  HDC mem = ::CreateCompatibleDC(screen);
  HBITMAP bmp = ::CreateCompatibleBitmap(screen, w, h);
  ::ReleaseDC(nullptr, screen);
  if (!mem || !bmp) {
    if (bmp) ::DeleteObject(bmp);
    if (mem) ::DeleteDC(mem);
    return -1;
  }
  HGDIOBJ oldb = ::SelectObject(mem, bmp);
  RECT all{0, 0, w, h};
  HBRUSH br = ::CreateSolidBrush(kCanvas);
  ::FillRect(mem, &all, br);
  ::DeleteObject(br);

  ::SendMessageW(ctl, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(mem),
                 PRF_CLIENT | PRF_ERASEBKGND | PRF_CHILDREN);

  long black = 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      if (::GetPixel(mem, x, y) == kRowFg) ++black;

  ::SelectObject(mem, oldb);
  ::DeleteObject(bmp);
  ::DeleteDC(mem);
  return black;
}

struct Harness {
  HWND parent = nullptr;
  int w = 0, h = 0;
};

// 建一個父視窗 + 兩個清單:一個照側欄的做法(欄寬每次版面都重設),
// 一個照方案清單的做法(欄寬只在建立時設一次)。兩邊其餘完全相同。
Harness Build(bool schema_like_resets_column) {
  EnsureClass();
  Harness hn;
  hn.w = kCanvasW;
  hn.h = kCanvasH;
  hn.parent = ::CreateWindowExW(
      WS_EX_TOOLWINDOW, kProbeClass, L"", WS_POPUP | WS_CLIPCHILDREN, -4000,
      -4000, kCanvasW, kCanvasH, nullptr, nullptr,
      ::GetModuleHandleW(nullptr), nullptr);
  if (!hn.parent) return hn;

  g_sidebar_like = ListProbe();
  g_schema_like = ListProbe();
  g_sidebar_like.id = 4001;
  g_schema_like.id = 4002;

  // 側欄:LVS_NOSCROLL、沒有外框。
  g_sidebar_like.list = CreateRowList(
      hn.parent, g_sidebar_like.id,
      LVS_REPORT | LVS_SINGLESEL | LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS |
          LVS_NOSCROLL | WS_TABSTOP);
  // 方案清單:有外框、可捲動。
  g_schema_like.list = CreateRowList(
      hn.parent, g_schema_like.id,
      LVS_REPORT | LVS_SINGLESEL | LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS |
          WS_TABSTOP | WS_BORDER);

  const std::vector<std::wstring> rows{L"row-one", L"row-two", L"row-three"};
  SetRowListItems(g_sidebar_like.list, rows);
  SetRowListItems(g_schema_like.list, rows);

  ::SetWindowPos(g_sidebar_like.list, nullptr, 0, 0, 200, 100, SWP_NOZORDER);
  ::SetWindowPos(g_schema_like.list, nullptr, 210, 0, 200, 100, SWP_NOZORDER);
  SyncRowListColumn(g_sidebar_like.list);
  if (schema_like_resets_column) SyncRowListColumn(g_schema_like.list);

  ::ShowWindow(hn.parent, SW_SHOWNA);
  ::UpdateWindow(hn.parent);
  return hn;
}

void Destroy(Harness& hn) {
  if (hn.parent) ::DestroyWindow(hn.parent);
  hn.parent = nullptr;
}

void Report(const char* what, const ListProbe& p, long black) {
  RECT client{};
  if (p.list) ::GetClientRect(p.list, &client);
  const long colw =
      p.list ? static_cast<long>(::SendMessageW(p.list, LVM_GETCOLUMNWIDTH, 0, 0))
             : -1;
  const long count =
      p.list ? static_cast<long>(::SendMessageW(p.list, LVM_GETITEMCOUNT, 0, 0))
             : -1;
  std::printf(
      "    [%s] prepaint=%d itemprepaint=%d black=%ld items=%ld colw=%ld "
      "client=(%ld,%ld,%ld,%ld)\n",
      what, p.prepaint, p.itemprepaint, black, count, colw, client.left,
      client.top, client.right, client.bottom);
  for (int i = 0; i < kRows; ++i) {
    std::printf(
        "      row%d  custom_draw_rc=(%ld,%ld,%ld,%ld)  used=(%ld,%ld,%ld,%ld)\n",
        i, p.raw[i].left, p.raw[i].top, p.raw[i].right, p.raw[i].bottom,
        p.used[i].left, p.used[i].top, p.used[i].right, p.used[i].bottom);
  }
}

}  // namespace

// ── 這一條就是使用者看到的那件事:清單上有沒有東西 ────────────────
TEST(win32_listview_custom_draw_actually_paints_rows) {
  Harness hn = Build(/*schema_like_resets_column=*/true);
  CHECK(hn.parent != nullptr);
  if (!hn.parent) return;
  CHECK(g_sidebar_like.list != nullptr);
  CHECK(g_schema_like.list != nullptr);

  const long black_side = BlackPixels(&g_sidebar_like, 200, 100);
  const long black_schema = BlackPixels(&g_schema_like, 200, 100);
  Report("sidebar-like", g_sidebar_like, black_side);
  Report("schema-like", g_schema_like, black_schema);

  // harness 自己要先是好的:一次 item 通知都沒有 = 這支測試什麼都沒測。
  CHECK(g_sidebar_like.itemprepaint >= kRows);
  CHECK(g_schema_like.itemprepaint >= kRows);

  // 使用者看得到的那一條。0 = 清單是空白的。
  CHECK(black_side > 0);
  CHECK(black_schema > 0);
  Destroy(hn);
}

// ── 欄寬沒有被重設的那一份(方案清單原本的樣子)────────────────────
//
// 側欄的欄寬每次 LayoutUi 都重設,方案清單只在建立時設一次。
// 這一條把兩者的差別單獨拉出來,免得日後又把它們判成「一樣的東西」。
TEST(win32_listview_paints_even_without_a_column_width_refresh) {
  Harness hn = Build(/*schema_like_resets_column=*/false);
  CHECK(hn.parent != nullptr);
  if (!hn.parent) return;
  const long black = BlackPixels(&g_schema_like, 200, 100);
  Report("schema-like(欄寬未重設)", g_schema_like, black);
  CHECK(g_schema_like.itemprepaint >= kRows);
  CHECK(black > 0);
  Destroy(hn);
}

// ── 前 / 後對照:直接用 custom draw 的 rc,對上走 RowRect() ────────
//
// **2026-08-10 在 windows-latest 上實測**(CI run #137,job logic-x64):
// report 模式的 ListView 在 `CDDS_ITEMPREPAINT` 給的 `NMCUSTOMDRAW::rc`
// 是 **(0,0,0,0)** —— 側欄與方案清單兩邊、每一列都一樣。
// 拿它去 FillRect + DrawTextW 什麼都不會畫,而 `CDRF_SKIPDEFAULT` 又
// 把控制項自己的繪製擋掉了,所以結果是**一整片空白而且沒有任何錯誤**。
// 那就是使用者截圖裡的那個空清單。
//
// 這一條把兩條路的像素數並排印出來,讓紀錄自己說話;
// 斷言只有一條:**RowRect() 不管 custom draw 給什麼,都要畫得出東西。**
TEST(win32_listview_row_rect_is_usable_whatever_custom_draw_hands_over) {
  Harness hn = Build(/*schema_like_resets_column=*/true);
  CHECK(hn.parent != nullptr);
  if (!hn.parent) return;

  g_schema_like.use_row_rect = false;
  const long raw_black = BlackPixels(&g_schema_like, 200, 100);
  Report("schema-like(直接用 custom draw 的 rc)", g_schema_like, raw_black);
  const int raw_items = g_schema_like.itemprepaint;

  g_schema_like.use_row_rect = true;
  const long fixed_black = BlackPixels(&g_schema_like, 200, 100);
  Report("schema-like(走 RowRect)", g_schema_like, fixed_black);

  std::printf(
      "    對照:rc 直用 black=%ld(itemprepaint=%d) / RowRect black=%ld\n",
      raw_black, raw_items, fixed_black);

  CHECK(g_schema_like.itemprepaint >= kRows);
  for (int i = 0; i < kRows; ++i) CHECK(!RectEmpty(g_schema_like.used[i]));
  CHECK(fixed_black > 0);
  Destroy(hn);
}
