// windows/tests/test_win32_sidebar.cc — 側欄:選取只有一份、整列都點得到
//
// ── 為什麼需要這一支 ────────────────────────────────────────────
//
// 使用者截圖上側欄有**兩列同時反白**(#80),而且他回報「點得到的地方
// 跟畫出來的地方差 16 DIP」(#75)。兩件事都有一半是 comctl32 的行為,
// 而那一半**只有真的 Windows 答得出來**:
//
//   · LVS_SINGLESEL 保證的是「使用者點不出第二個選取」。它保不保證
//     **程式化的** LVM_SETITEMSTATE 會先取消舊的那一列?讀原始碼讀不出來。
//   · report 模式的 ListView 預設只有第一欄的 **label 矩形**算命中,
//     而 label 的寬度是文字實際的寬度。我們畫的底卻是整列。
//     兩者差多少,取決於那一頁的名字有幾個字。
//
// 以前這兩格在 CI 上完全不存在:windows/ 底下的 UI 單元測試都刻意不
// include windows.h。這一支開真的控制項、走真的 comctl32,而且驅動的是
// **產品碼本身**(service/ui_listview.cc 的 SelectOnlyRow /
// SetRowListExtendedStyle / SetRowListRowHeight),不是一份長得很像的複本。
//
// ⚠ 沒有 SKIP。harness 自己壞掉(控制項建不起來、一次 CDDS_ITEMPREPAINT
//   都沒收到)一律算失敗 —— 這個專案吃過「測試安靜地跳過自己」的虧。
//
// ⚠ 這裡**不送合成的滑鼠訊息**。「按已經選取的那一列會不會換頁」那一條
//   要 WM_LBUTTONDOWN/UP 走完 comctl32 的滑鼠迴圈(它會 SetCapture),
//   在一個離螢幕、沒有輸入焦點的視窗上那是碰運氣 —— 而一條間歇性的
//   測試比沒有那條測試更糟。那一格仍然只有真人驗得到。

#include <windows.h>

#include <commctrl.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../common/ui_layout.h"
#include "../service/ui_listview.h"
#include "check.h"

using namespace rimewin;

namespace {

constexpr wchar_t kProbeClass[] = L"LuminaKeySidebarProbe";
constexpr int kCanvasW = 260;
constexpr int kCanvasH = 300;
// 側欄的四~五頁。名字**長短刻意差很多**:「文字」兩個字的 label 矩形
// 不到「輸入方案」的一半,而 #75 的一半正是「label 有多寬」。
// ⚠ 頁名用**碼點**組,不寫中文字面值:check_ui_spec.sh 的 W7 要求
//   使用者可見的中日韓字串只能住在 common/ui_strings.cc,而那條規則
//   對測試檔一視同仁(一個為了測規則而自己違規的測試,會讓那條檢查
//   要嘛紅、要嘛得為自己開一個例外,而例外正是規則死掉的方式)。
//
//   長短刻意差很多:第三個只有兩個字,它的 label 矩形不到第一個的一半
//   —— 而 #75 的一半正是「label 有多寬」。
constexpr int kPageRows = 5;

std::wstring PageName(int i) {
  // 輸入方案 / 外觀 / 文字 / 連網 / 進階
  static const int cps[kPageRows][4] = {{0x8F38, 0x5165, 0x65B9, 0x6848},
                                        {0x5916, 0x89C0, 0, 0},
                                        {0x6587, 0x5B57, 0, 0},
                                        {0x9023, 0x7DB2, 0, 0},
                                        {0x9032, 0x968E, 0, 0}};
  std::wstring s;
  for (int k = 0; k < 4; ++k)
    if (cps[i][k]) s.push_back(static_cast<wchar_t>(cps[i][k]));
  return s;
}

// 自繪那一趟,每一列的兩個來源各說了什麼。
//
//   · state_*     控制項自己那一份(LVM_GETITEMSTATE)—— **權威的那一份**,
//                 也是 service/ui_listview.cc 的 RowIsSelected() 走的路。
//   · uistate_*   NMCUSTOMDRAW::uItemState 的 CDIS_SELECTED。
//
// 兩份都收,是因為「它們一不一樣」正是這一支要回答的爭議。
struct DrawLog {
  int itemprepaint = 0;
  int uistate_rows = 0;
  int uistate_last = -1;
  int state_rows = 0;
  int state_last = -1;
};

DrawLog g_log;
HWND g_sidebar = nullptr;
int g_sidebar_id = 5001;

LRESULT CALLBACK ProbeProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  if (msg == WM_NOTIFY) {
    NMHDR* nm = reinterpret_cast<NMHDR*>(l);
    if (nm && nm->code == NM_CUSTOMDRAW &&
        nm->idFrom == static_cast<UINT_PTR>(g_sidebar_id)) {
      NMLVCUSTOMDRAW* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(nm);
      if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
      if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
        ++g_log.itemprepaint;
        const int row = static_cast<int>(cd->nmcd.dwItemSpec);
        if ((cd->nmcd.uItemState & CDIS_SELECTED) != 0) {
          ++g_log.uistate_rows;
          g_log.uistate_last = row;
        }
        // 權威的那一份 —— 產品碼(RowIsSelected)走的就是這一條。
        if (RowIsSelected(g_sidebar, row)) {
          ++g_log.state_rows;
          g_log.state_last = row;
        }
        // ⚠ 不畫任何東西:這一支量的是**狀態**,不是像素。
        //   「畫得出來嗎」有 test_win32_listview.cc 在管。
        return CDRF_SKIPDEFAULT;
      }
    }
    return 0;
  }
  if (msg == WM_DPICHANGED) {
    // 進程是 per-monitor-v2,系統**會**送這一則。照建議矩形重擺 ——
    // 這支探針的視窗離螢幕,但「收到了就不理」在別處是缺陷,
    // 不該因為是測試就寫成別的樣子(check_ui_spec.sh 的 W2 守著)。
    const RECT* sug = reinterpret_cast<const RECT*>(l);
    if (sug)
      ::SetWindowPos(hwnd, nullptr, sug->left, sug->top, sug->right - sug->left,
                     sug->bottom - sug->top, SWP_NOZORDER | SWP_NOACTIVATE);
    return 0;
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

// 側欄的樣式與 settings_window.cc 的 CreateUi 完全一樣。
constexpr DWORD kSidebarStyle = LVS_REPORT | LVS_SINGLESEL |
                                LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS |
                                LVS_NOSCROLL | WS_TABSTOP;

struct Harness {
  HWND parent = nullptr;
  HWND list = nullptr;
};

// with_ext = false 時**刻意不掛**擴充樣式 —— 那就是修正被拿掉的樣子,
// 用來證明下面那條命中測試真的分得出兩者(反向對照)。
Harness Build(bool with_ext) {
  EnsureClass();
  Harness hn;
  hn.parent = ::CreateWindowExW(
      WS_EX_TOOLWINDOW, kProbeClass, L"", WS_POPUP | WS_CLIPCHILDREN, -4000,
      -4000, kCanvasW, kCanvasH, nullptr, nullptr,
      ::GetModuleHandleW(nullptr), nullptr);
  if (!hn.parent) return hn;

  g_log = DrawLog();
  // ⚠ 不走 CreateRowList:那一支現在自己會掛擴充樣式,而這裡要能做出
  //   「沒有掛」的那一份。其餘每一步都與產品碼相同。
  hn.list = ::CreateWindowExW(
      0, WC_LISTVIEWW, L"", WS_CHILD | kSidebarStyle, 0, 0, 10, 10, hn.parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(g_sidebar_id)),
      ::GetModuleHandleW(nullptr), nullptr);
  if (!hn.list) return hn;
  EnsureRowListColumn(hn.list);
  if (with_ext) SetRowListExtendedStyle(hn.list);

  std::vector<std::wstring> rows;
  for (int i = 0; i < kPageRows; ++i) rows.push_back(PageName(i));
  SetRowListItems(hn.list, rows);

  // 側欄在 96 DPI 下的實際尺寸:寬 kSidebarW,列高 kSidebarItemH。
  ::SetWindowPos(hn.list, nullptr, 0, 0, metric::kSidebarW,
                 kPageRows * metric::kSidebarItemH + 8, SWP_NOZORDER);
  SyncRowListColumn(hn.list);
  SetRowListRowHeight(hn.list, metric::kSidebarItemH);

  g_sidebar = hn.list;
  ::ShowWindow(hn.parent, SW_SHOWNA);
  ::UpdateWindow(hn.parent);
  return hn;
}

void Destroy(Harness& hn) {
  g_sidebar = nullptr;
  if (hn.parent) ::DestroyWindow(hn.parent);
  hn.parent = nullptr;
  hn.list = nullptr;
}

// 走一次真的繪製,把每一列拿到的 uItemState 收集起來。
// ⚠ 用 WM_PRINTCLIENT 而不是螢幕擷取:runner 上沒有人在看畫面。
bool Paint(HWND list) {
  g_log = DrawLog();
  HDC screen = ::GetDC(nullptr);
  if (!screen) return false;
  HDC mem = ::CreateCompatibleDC(screen);
  HBITMAP bmp = ::CreateCompatibleBitmap(screen, kCanvasW, kCanvasH);
  ::ReleaseDC(nullptr, screen);
  if (!mem || !bmp) {
    if (bmp) ::DeleteObject(bmp);
    if (mem) ::DeleteDC(mem);
    return false;
  }
  HGDIOBJ oldb = ::SelectObject(mem, bmp);
  ::SendMessageW(list, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(mem),
                 PRF_CLIENT | PRF_ERASEBKGND);
  ::SelectObject(mem, oldb);
  ::DeleteObject(bmp);
  ::DeleteDC(mem);
  return true;
}

int SelectedCount(HWND list) {
  return static_cast<int>(::SendMessageW(list, LVM_GETSELECTEDCOUNT, 0, 0));
}

int FirstSelected(HWND list) {
  return static_cast<int>(::SendMessageW(list, LVM_GETNEXTITEM,
                                         static_cast<WPARAM>(-1),
                                         LVNI_SELECTED));
}

int HitRow(HWND list, int x, int y) {
  LVHITTESTINFO ht{};
  ht.pt.x = x;
  ht.pt.y = y;
  ::SendMessageW(list, LVM_HITTEST, 0, reinterpret_cast<LPARAM>(&ht));
  return ht.iItem;
}

}  // namespace

// ── W1:程式化換頁之後,**恰好一列**被選取 ───────────────────────
//
// 這一條把「LVS_SINGLESEL 管不管程式化設定」這個爭議交給 CI 回答,
// 而不是繼續在報告裡互相引用記憶。
//
// ⚠ 它驗的是 service/ui_listview.cc 的 SelectOnlyRow 本人。把那一支
//   裡的「先全清」拿掉,這一條就是它會不會紅的唯一裁判。
TEST(sidebar_select_leaves_exactly_one_row_selected) {
  Harness hn = Build(true);
  CHECK(hn.parent != nullptr);
  CHECK(hn.list != nullptr);
  if (!hn.list) return;

  SelectOnlyRow(hn.list, 0);
  CHECK_INT(SelectedCount(hn.list), 1);
  CHECK_INT(FirstSelected(hn.list), 0);

  // 換到第 2 頁(「文字」)—— 使用者截圖上內容顯示的正是這一頁。
  SelectOnlyRow(hn.list, 2);
  CHECK_INT(SelectedCount(hn.list), 1);
  CHECK_INT(FirstSelected(hn.list), 2);

  // 再換幾次,確認它不會慢慢累積。
  for (int i = 0; i < kPageRows; ++i) {
    SelectOnlyRow(hn.list, i);
    CHECK_INT(SelectedCount(hn.list), 1);
    CHECK_INT(FirstSelected(hn.list), i);
  }
  Destroy(hn);
}

// ── W2:自繪那一趟,權威的那一份也只有一列 selected ──────────────
//
// 上一條問的是換完頁之後控制項的**狀態**,這一條問的是**繪製途中**
// 拿得到什麼 —— 而使用者看到的是後者。兩列同時反白的直接條件就是
// 繪製時有兩列說自己被選。
//
// ⚠ 這一條**斷言的是 LVM_GETITEMSTATE,不是 NMCUSTOMDRAW::uItemState**,
//   而那不是為了讓它好過。它第一次上 CI(Windows run #171)斷言的正是
//   uItemState,結果是:`g_log.selected_rows == 1 → 5`、
//   `g_log.last_selected == 2 → 4` —— **5 列裡 5 列**都被回報成 selected,
//   而同一時刻控制項自己(上一條的 LVM_GETSELECTEDCOUNT)說只有 1 列。
//
//   所以那個爭議有答案了,而且答案是「uItemState 這個位元不能用」:
//   settings_window.cc 的側欄與方案清單本來就不讀它(它們從 page_ /
//   schema_sel_ 畫),當時的措辭是「不去賭它準不準」—— 現在不是賭,
//   是量過的。連網紀錄那一個當時仍在讀,照它畫的結果是**每一列都反白**;
//   這一輪一併改成問控制項本人(service/ui_listview.cc 的 RowIsSelected),
//   而 check_ui_spec.sh 的 W31 從此不准任何一個自繪處理常式讀那個位元。
//
//   兩份都收、只斷言權威的那一份,並把對照印出來:哪一天 comctl32 把
//   uItemState 修好了,下面那一行會看得出來,而不必為此讓測試變紅
//   (它變不變好都不改變「產品碼不讀它」這個結論)。
TEST(sidebar_custom_draw_reports_exactly_one_selected_row) {
  Harness hn = Build(true);
  CHECK(hn.list != nullptr);
  if (!hn.list) return;

  SelectOnlyRow(hn.list, 0);
  SelectOnlyRow(hn.list, 2);
  CHECK(Paint(hn.list));
  // harness 自己壞掉(一次 ITEMPREPAINT 都沒收到)算失敗,不算通過。
  CHECK_INT(g_log.itemprepaint, kPageRows);
  CHECK_INT(g_log.state_rows, 1);
  CHECK_INT(g_log.state_last, 2);
  std::printf(
      "    [對照] 繪製途中:控制項說 %d 列被選(最後一列 %d);"
      "NMCUSTOMDRAW::uItemState 說 %d 列(最後一列 %d)\n",
      g_log.state_rows, g_log.state_last, g_log.uistate_rows,
      g_log.uistate_last);
  Destroy(hn);
}

// ── W3:畫出來的那一列,**每一個像素**都點得到 ───────────────────
//
// #75 的另一半。report 模式預設只有 label 矩形算命中,而 label 是文字
// 實際的寬度 —— 使用者按在頁名右邊的空白上,那裡看起來是那一列,
// 而 comctl32 說沒有命中任何一列。
//
// 五個探點:左緣+1 / 中心 / 右緣−1 / 上緣+1 / 下緣−1。
TEST(sidebar_every_pixel_of_a_drawn_row_is_clickable) {
  Harness hn = Build(true);
  CHECK(hn.list != nullptr);
  if (!hn.list) return;

  int probed = 0;
  for (int i = 0; i < kPageRows; ++i) {
    RECT r{};
    r.left = LVIR_BOUNDS;
    CHECK(::SendMessageW(hn.list, LVM_GETITEMRECT, static_cast<WPARAM>(i),
                         reinterpret_cast<LPARAM>(&r)) != 0);
    // 列高真的被釘成 36 了(SetRowListRowHeight 是產品碼)。
    CHECK_INT(static_cast<int>(r.bottom - r.top), metric::kSidebarItemH);
    const int cx = (r.left + r.right) / 2;
    const int cy = (r.top + r.bottom) / 2;
    const int pts[5][2] = {{static_cast<int>(r.left) + 1, cy},
                           {cx, cy},
                           {static_cast<int>(r.right) - 1, cy},
                           {cx, static_cast<int>(r.top) + 1},
                           {cx, static_cast<int>(r.bottom) - 1}};
    for (const auto& pt : pts) {
      CHECK_INT(HitRow(hn.list, pt[0], pt[1]), i);
      ++probed;
    }
  }
  // ⚠ 範圍斷言:掃到零個探點而報「全部合格」正是 §2-G 的那個失效方式。
  CHECK_INT(probed, kPageRows * 5);
  Destroy(hn);
}

// ── W3 的反向對照:沒有掛擴充樣式時,同一組探點**分得出來** ──────
//
// 少了這一條,上面那條綠燈可能只是因為命中判定本來就寬 ——
// 而那樣的話它守不住任何東西。
//
// ⚠ 斷言刻意寬鬆(「至少有一個探點落空」)而不是「右緣一定落空」:
//   前者證明這條測試分得出有沒有修正,後者是在賭 comctl32 某一版
//   label 矩形的精確寬度。哪一天 comctl32 把整列命中變成預設,
//   這一條會紅,而那時該做的事是把 LVS_EX_FULLROWSELECT 拿掉 ——
//   紅得有道理,不是雜訊。
TEST(sidebar_without_full_row_select_some_probes_miss) {
  Harness hn = Build(false);
  CHECK(hn.list != nullptr);
  if (!hn.list) return;

  int misses = 0;
  int probed = 0;
  for (int i = 0; i < kPageRows; ++i) {
    RECT r{};
    r.left = LVIR_BOUNDS;
    if (!::SendMessageW(hn.list, LVM_GETITEMRECT, static_cast<WPARAM>(i),
                        reinterpret_cast<LPARAM>(&r)))
      continue;
    const int cy = (r.top + r.bottom) / 2;
    if (HitRow(hn.list, static_cast<int>(r.right) - 1, cy) != i) ++misses;
    ++probed;
  }
  CHECK_INT(probed, kPageRows);
  CHECK(misses > 0);
  Destroy(hn);
}
