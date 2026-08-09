#include "settings_window.h"

#include <commctrl.h>
#include <shellapi.h>

#include <cstdio>

#include "../common/schema_choice.h"
#include "../common/schema_list_patch.h"
#include "../common/ui_dip.h"
#include "../common/ui_layout.h"
#include "../winshared/winutil.h"
#include "cand_window.h"
#include "status_bar.h"
#include "ui_confirm.h"

namespace rimewin {
namespace {

constexpr wchar_t kClass[] = L"LuminaKeySettingsWindow";
constexpr UINT WM_RIME_OPEN = WM_APP + 1;
constexpr UINT WM_RIME_TRAY = WM_APP + 2;
constexpr UINT WM_RIME_SET_VARIANT = WM_APP + 3;
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kDeployTimer = 1;
constexpr UINT_PTR kStatusTimer = 2;

enum : int {
  IDM_TRAY_SETTINGS = 900,
  IDM_TRAY_REDEPLOY,
  IDM_TRAY_QUIT,
  IDM_TRAY_VAR_FOLLOW,
  IDM_TRAY_VAR_HANT,
  IDM_TRAY_VAR_HANS,
};

// ── 控制項 id ───────────────────────────────────────────────────
enum : int {
  IDC_SIDEBAR = 100,
  IDC_STATUS,
  IDC_CLOSE,

  // 輸入方案
  IDC_SCHEMAS_TITLE = 200,
  IDC_SCHEMAS_SUB,
  IDC_SCHEMAS_LIST_HEAD,
  IDC_SCHEMAS_LIST_BLURB,
  IDC_SCHEMA_LIST,
  IDC_UP,
  IDC_DOWN,
  IDC_APPLY_ORDER,
  IDC_SCHEMAS_DEFAULT_LINE,
  IDC_FOLLOW_MODE,
  IDC_FOLLOW_BLURB,
  IDC_SCHEMAS_EMPTY,

  // 外觀
  IDC_APPEAR_TITLE = 300,
  IDC_APPEAR_SUB,
  IDC_COUNT_HEAD,
  IDC_COUNT_BLURB,
  IDC_COUNT_0,
  IDC_COUNT_1,
  IDC_COUNT_2,
  IDC_COUNT_3,
  IDC_COUNT_4,
  IDC_SCALE_HEAD,
  IDC_SCALE_BLURB,
  IDC_SCALE_0,
  IDC_SCALE_1,
  IDC_SCALE_2,
  IDC_SCALE_3,
  IDC_SCALE_4,
  IDC_THEME_HEAD,
  IDC_THEME_BLURB,
  IDC_THEME_0,
  IDC_THEME_1,
  IDC_THEME_2,
  IDC_BAR_HEAD,
  IDC_BAR_BLURB,
  IDC_BAR_SHOW,
  IDC_APPEAR_NOTE,

  // 文字
  IDC_TEXT_TITLE = 400,
  IDC_TEXT_SUB,
  IDC_VARIANT_HEAD,
  IDC_VARIANT_BLURB,
  IDC_VARIANT_0,
  IDC_VARIANT_1,
  IDC_VARIANT_2,
  IDC_PUNCT_HEAD,
  IDC_PUNCT_BLURB,
  IDC_PUNCT_0,
  IDC_PUNCT_1,
  IDC_PUNCT_2,

  // 進階
  IDC_ADV_TITLE = 500,
  IDC_ADV_SUB,
  IDC_REDEPLOY_HEAD,
  IDC_REDEPLOY_BLURB,
  IDC_REDEPLOY,
  IDC_FILES_HEAD,
  IDC_FILES_BLURB,
  IDC_OPEN_USER_DIR,
  IDC_OPEN_SETTINGS_FILE,
  IDC_LANG_HEAD,
  IDC_LANG_BLURB,
  IDC_LANG_0,
  IDC_LANG_1,
  IDC_LANG_2,
  IDC_LANG_3,
  IDC_DIAG_HEAD,
  IDC_DIAG_NOTE,
  IDC_DIAG,
  IDC_DIAG_COPY,
  IDC_RESET_HEAD,
  IDC_RESET_BLURB,
  IDC_RESET,
};

// ── ⚠ 這一張表就是「第六個看得到但摸不到」的結構性修法 ────────────
//
// 控制項的**建立**與**顯示**走同一份資料。舊版是兩份:一處 CreateWindow、
// 另一處是每頁的 id 陣列 —— 而 IDC_FOLLOW_MODE 只出現在前者,
// 於是它被建立了、有處理常式、有讀寫設定的完整程式碼,卻從來沒有被
// ShowWindow 過。畫面上根本沒有那顆核取方塊,而程式碼看起來完全正常。
//
// 現在「建了但不屬於任何一頁」在結構上不可能發生。
// 不屬於任何一頁:永遠看得見。
constexpr int kAlways = 99;

struct ControlDef {
  int id;
  int page;
  const wchar_t* cls;
  DWORD style;
  UiString label;  // kUiStringCount = 執行期才填
};

constexpr UiString kNoText = UiString::kUiStringCount;

#define ST (SS_LEFT | SS_NOPREFIX)
#define BTN (BS_PUSHBUTTON | WS_TABSTOP)
#define RADIO (BS_AUTORADIOBUTTON | WS_TABSTOP)
#define RADIO1 (BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP)

const ControlDef kControls[] = {
    // 永遠看得見
    {IDC_STATUS, kAlways, L"STATIC", ST, kNoText},
    {IDC_CLOSE, kAlways, L"BUTTON", BTN, UiString::kClose},

    // ── 輸入方案 ──
    {IDC_SCHEMAS_TITLE, 0, L"STATIC", ST, UiString::kSchemasTitle},
    {IDC_SCHEMAS_SUB, 0, L"STATIC", ST, UiString::kSchemasSubtitle},
    {IDC_SCHEMAS_LIST_HEAD, 0, L"STATIC", ST, UiString::kSchemasListHeading},
    {IDC_SCHEMAS_LIST_BLURB, 0, L"STATIC", ST, UiString::kSchemasListBlurb},
    {IDC_SCHEMA_LIST, 0, WC_LISTVIEWW,
     LVS_REPORT | LVS_SINGLESEL | LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS |
         WS_TABSTOP | WS_BORDER,
     kNoText},
    {IDC_UP, 0, L"BUTTON", BTN, UiString::kSchemasMoveUp},
    {IDC_DOWN, 0, L"BUTTON", BTN, UiString::kSchemasMoveDown},
    {IDC_APPLY_ORDER, 0, L"BUTTON", BTN, UiString::kSchemasApplyOrder},
    {IDC_SCHEMAS_DEFAULT_LINE, 0, L"STATIC", ST, kNoText},
    // §12.5.2:開關 = BUTTON + BS_AUTOCHECKBOX | BS_RIGHTBUTTON。
    // BS_RIGHTBUTTON 把方塊移到**右邊**,滿足 §4.1「開關在右、標題說明在左」。
    // ⚠ **不可以** owner-draw 它:BS_OWNERDRAW 與 BS_AUTOCHECKBOX 互斥
    //   (兩者都佔 BS_TYPEMASK 的低 4 位元),自繪之後螢幕閱讀器會念成
    //   「按鈕」而不是「核取方塊,已勾選」。
    {IDC_FOLLOW_MODE, 0, L"BUTTON",
     BS_AUTOCHECKBOX | BS_RIGHTBUTTON | BS_MULTILINE | WS_TABSTOP,
     UiString::kSchemasFollowTitle},
    {IDC_FOLLOW_BLURB, 0, L"STATIC", ST, UiString::kSchemasFollowBlurb},
    {IDC_SCHEMAS_EMPTY, 0, L"STATIC", ST, kNoText},

    // ── 外觀 ──
    {IDC_APPEAR_TITLE, 1, L"STATIC", ST, UiString::kAppearanceTitle},
    {IDC_APPEAR_SUB, 1, L"STATIC", ST, UiString::kAppearanceSubtitle},
    {IDC_COUNT_HEAD, 1, L"STATIC", ST, UiString::kCountHeading},
    {IDC_COUNT_BLURB, 1, L"STATIC", ST, UiString::kCountBlurb},
    // ⚠ 「跟著○○」永遠是單選群組的第一格(§4.2)。
    {IDC_COUNT_0, 1, L"BUTTON", RADIO1, UiString::kValueFollowSchema},
    {IDC_COUNT_1, 1, L"BUTTON", RADIO, UiString::kCountThree},
    {IDC_COUNT_2, 1, L"BUTTON", RADIO, UiString::kCountFive},
    {IDC_COUNT_3, 1, L"BUTTON", RADIO, UiString::kCountSeven},
    {IDC_COUNT_4, 1, L"BUTTON", RADIO, UiString::kCountNine},
    {IDC_SCALE_HEAD, 1, L"STATIC", ST, UiString::kScaleHeading},
    {IDC_SCALE_BLURB, 1, L"STATIC", ST, UiString::kScaleBlurb},
    {IDC_SCALE_0, 1, L"BUTTON", RADIO1, UiString::kValueFollowSchema},
    {IDC_SCALE_1, 1, L"BUTTON", RADIO, UiString::kScaleSmall},
    {IDC_SCALE_2, 1, L"BUTTON", RADIO, UiString::kScaleNormal},
    {IDC_SCALE_3, 1, L"BUTTON", RADIO, UiString::kScaleLarge},
    {IDC_SCALE_4, 1, L"BUTTON", RADIO, UiString::kScaleHuge},
    {IDC_THEME_HEAD, 1, L"STATIC", ST, UiString::kThemeHeading},
    {IDC_THEME_BLURB, 1, L"STATIC", ST, UiString::kThemeBlurb},
    {IDC_THEME_0, 1, L"BUTTON", RADIO1, UiString::kThemeFollowSystem},
    {IDC_THEME_1, 1, L"BUTTON", RADIO, UiString::kThemeLight},
    {IDC_THEME_2, 1, L"BUTTON", RADIO, UiString::kThemeDark},
    {IDC_BAR_HEAD, 1, L"STATIC", ST, UiString::kStatusBarHeading},
    {IDC_BAR_BLURB, 1, L"STATIC", ST, UiString::kStatusBarBlurb},
    {IDC_BAR_SHOW, 1, L"BUTTON",
     BS_AUTOCHECKBOX | BS_RIGHTBUTTON | WS_TABSTOP, UiString::kStatusBarShow},
    {IDC_APPEAR_NOTE, 1, L"STATIC", ST, UiString::kAppearanceHonestNote},

    // ── 文字 ──
    {IDC_TEXT_TITLE, 2, L"STATIC", ST, UiString::kTextTitle},
    {IDC_TEXT_SUB, 2, L"STATIC", ST, UiString::kTextSubtitle},
    {IDC_VARIANT_HEAD, 2, L"STATIC", ST, UiString::kVariantHeading},
    {IDC_VARIANT_BLURB, 2, L"STATIC", ST, UiString::kVariantBlurb},
    {IDC_VARIANT_0, 2, L"BUTTON", RADIO1, UiString::kVariantFollow},
    {IDC_VARIANT_1, 2, L"BUTTON", RADIO, UiString::kVariantTraditional},
    {IDC_VARIANT_2, 2, L"BUTTON", RADIO, UiString::kVariantSimplified},
    {IDC_PUNCT_HEAD, 2, L"STATIC", ST, UiString::kPunctHeading},
    {IDC_PUNCT_BLURB, 2, L"STATIC", ST, UiString::kPunctBlurb},
    {IDC_PUNCT_0, 2, L"BUTTON", RADIO1, UiString::kPunctFollow},
    {IDC_PUNCT_1, 2, L"BUTTON", RADIO, UiString::kPunctChinese},
    {IDC_PUNCT_2, 2, L"BUTTON", RADIO, UiString::kPunctEnglish},

    // ── 進階 ──
    {IDC_ADV_TITLE, 3, L"STATIC", ST, UiString::kAdvancedTitle},
    {IDC_ADV_SUB, 3, L"STATIC", ST, UiString::kAdvancedSubtitle},
    {IDC_REDEPLOY_HEAD, 3, L"STATIC", ST, UiString::kRedeployHeading},
    {IDC_REDEPLOY_BLURB, 3, L"STATIC", ST, UiString::kRedeployBlurb},
    {IDC_REDEPLOY, 3, L"BUTTON", BTN, UiString::kRedeployButton},
    {IDC_FILES_HEAD, 3, L"STATIC", ST, UiString::kFilesHeading},
    {IDC_FILES_BLURB, 3, L"STATIC", ST, UiString::kFilesBlurb},
    {IDC_OPEN_USER_DIR, 3, L"BUTTON", BTN, UiString::kOpenUserDir},
    {IDC_OPEN_SETTINGS_FILE, 3, L"BUTTON", BTN, UiString::kOpenSettingsFile},
    {IDC_LANG_HEAD, 3, L"STATIC", ST, UiString::kLanguageHeading},
    {IDC_LANG_BLURB, 3, L"STATIC", ST, UiString::kLanguageBlurb},
    {IDC_LANG_0, 3, L"BUTTON", RADIO1, UiString::kLanguageSystem},
    {IDC_LANG_1, 3, L"BUTTON", RADIO, UiString::kLanguageEnglish},
    {IDC_LANG_2, 3, L"BUTTON", RADIO, UiString::kLanguageHant},
    {IDC_LANG_3, 3, L"BUTTON", RADIO, UiString::kLanguageHans},
    {IDC_DIAG_HEAD, 3, L"STATIC", ST, UiString::kDiagnosticsHeading},
    {IDC_DIAG_NOTE, 3, L"STATIC", ST, UiString::kDiagnosticsNote},
    // §12.5.2:唯讀資訊 = EDIT + ES_READONLY|ES_MULTILINE|WS_VSCROLL。
    // 可整段選取複製,那正是 §4.11 要的。
    {IDC_DIAG, 3, L"EDIT",
     ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP |
         WS_BORDER,
     kNoText},
    {IDC_DIAG_COPY, 3, L"BUTTON", BTN, UiString::kDiagnosticsCopy},
    {IDC_RESET_HEAD, 3, L"STATIC", ST, UiString::kResetHeading},
    {IDC_RESET_BLURB, 3, L"STATIC", ST, UiString::kResetBlurb},
    // ⚠ 危險鍵是這個檔案裡唯一 owner-draw 的**按鈕**。理由不是好看:
    //   啟用視覺樣式後 push button **不吃** WM_CTLCOLORBTN,文字色改不掉,
    //   而 §4.9 要的是「外框 + 危險色文字 + 透明底」。
    //   owner-draw 之後 MSAA 的角色仍然是 push button(它本來就沒有
    //   額外的狀態要維護),所以這一格是六類自繪裡最便宜的一格。
    {IDC_RESET, 3, L"BUTTON", BS_OWNERDRAW | WS_TABSTOP,
     UiString::kResetButton},
};
constexpr int kControlCount =
    static_cast<int>(sizeof(kControls) / sizeof(kControls[0]));

#undef ST
#undef BTN
#undef RADIO
#undef RADIO1

// 側欄上的頁名(順序 = 由上而下)。
const UiString kPageNames[] = {UiString::kNavSchemas, UiString::kNavAppearance,
                               UiString::kNavText, UiString::kNavAdvanced};

const VariantPref kVariantOrder[] = {VariantPref::kFollowInputMode,
                                     VariantPref::kTraditional,
                                     VariantPref::kSimplified};
constexpr int kVariantCount = 3;

const UiString kVariantLabels[] = {UiString::kVariantFollow,
                                   UiString::kVariantTraditional,
                                   UiString::kVariantSimplified};

HWND Ctl(HWND parent, int id) { return ::GetDlgItem(parent, id); }

void SetText(HWND parent, int id, const wchar_t* text) {
  HWND h = Ctl(parent, id);
  if (h) ::SetWindowTextW(h, text);
}

void CheckRadio(HWND parent, int first, int count, int sel) {
  for (int i = 0; i < count; ++i) {
    HWND h = Ctl(parent, first + i);
    if (h)
      ::SendMessageW(h, BM_SETCHECK, i == sel ? BST_CHECKED : BST_UNCHECKED, 0);
  }
}

int RadioSel(HWND parent, int first, int count) {
  for (int i = 0; i < count; ++i) {
    HWND h = Ctl(parent, first + i);
    if (h && ::SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED) return i;
  }
  return 0;
}

}  // namespace

// ────────────────────────────────────────────────────────────────

SettingsWindow::SettingsWindow(Engine* engine, SettingsStore* store,
                               const std::string& shared_dir)
    : engine_(engine), store_(store), shared_dir_(shared_dir) {}

SettingsWindow::~SettingsWindow() { Stop(); }

DWORD WINAPI SettingsWindow::ThreadEntry(LPVOID self) {
  static_cast<SettingsWindow*>(self)->ThreadMain();
  return 0;
}

bool SettingsWindow::Start() {
  if (thread_) return true;
  ready_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  thread_ = ::CreateThread(nullptr, 0, &SettingsWindow::ThreadEntry, this, 0,
                           &thread_id_);
  if (!thread_) return false;
  if (ready_) ::WaitForSingleObject(ready_, 5000);
  return hwnd_ != nullptr;
}

void SettingsWindow::Stop() {
  if (thread_id_) ::PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
  if (thread_) {
    ::WaitForSingleObject(thread_, 3000);
    ::CloseHandle(thread_);
    thread_ = nullptr;
  }
  if (ready_) {
    ::CloseHandle(ready_);
    ready_ = nullptr;
  }
}

void SettingsWindow::Open() {
  if (hwnd_) ::PostMessageW(hwnd_, WM_RIME_OPEN, 0, 0);
}

void SettingsWindow::OpenAt(int page) {
  if (hwnd_) ::PostMessageW(hwnd_, WM_RIME_OPEN, static_cast<WPARAM>(page + 1),
                            0);
}

void SettingsWindow::ThreadMain() {
  INITCOMMONCONTROLSEX icc{};
  icc.dwSize = sizeof(icc);
  // ⚠ 不再需要 ICC_TAB_CLASSES(分頁換成側欄了),但要 LISTVIEW。
  icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
  ::InitCommonControlsEx(&icc);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &SettingsWindow::WndProc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  // ⚠ nullptr,不是 COLOR_BTNFACE + 1:底由我們自己在 WM_ERASEBKGND 畫。
  //   用系統色的話,深色模式下每次重畫都會先閃一片淺灰。
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kClass;
  ::RegisterClassExW(&wc);

  hwnd_ = ::CreateWindowExW(
      0, kClass, UiText(UiString::kWindowTitle),
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME,
      CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, wc.hInstance,
      this);
  if (ready_) ::SetEvent(ready_);
  if (!hwnd_) return;

  MSG msg;
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!::IsDialogMessageW(hwnd_, &msg)) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
  }
  fonts_.Clear();
  theme_.Clear();
  hwnd_ = nullptr;
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM w,
                                         LPARAM l) {
  SettingsWindow* self = reinterpret_cast<SettingsWindow*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE: {
      CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(l);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      break;
    }
    case WM_CREATE:
      if (self) {
        self->CreateUi(hwnd);
        self->AddTray();
      }
      return 0;
    case WM_ERASEBKGND: {
      if (!self) break;
      RECT rc{};
      ::GetClientRect(hwnd, &rc);
      ::FillRect(reinterpret_cast<HDC>(w), &rc,
                 self->theme_.Brush(kBackground));
      return 1;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = ::BeginPaint(hwnd, &ps);
      if (self) self->OnPaint(hdc);
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    // ⚠ 唯讀 EDIT 送的是 WM_CTLCOLORSTATIC,不是 WM_CTLCOLOREDIT。
    //   核取方塊與單選鈕送的也是這一則(只影響**文字與底**,
    //   方塊本身由 uxtheme 畫,改不掉 —— 見 ui_theme.h)。
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
      if (!self) break;
      HDC hdc = reinterpret_cast<HDC>(w);
      const HWND ctl = reinterpret_cast<HWND>(l);
      const int id = ::GetDlgCtrlID(ctl);
      const bool secondary =
          id == IDC_SCHEMAS_SUB || id == IDC_APPEAR_SUB || id == IDC_TEXT_SUB ||
          id == IDC_ADV_SUB || id == IDC_STATUS ||
          id == IDC_SCHEMAS_LIST_BLURB || id == IDC_FOLLOW_BLURB ||
          id == IDC_COUNT_BLURB || id == IDC_SCALE_BLURB ||
          id == IDC_THEME_BLURB || id == IDC_BAR_BLURB ||
          id == IDC_APPEAR_NOTE || id == IDC_VARIANT_BLURB ||
          id == IDC_PUNCT_BLURB || id == IDC_REDEPLOY_BLURB ||
          id == IDC_FILES_BLURB || id == IDC_LANG_BLURB ||
          id == IDC_DIAG_NOTE || id == IDC_RESET_BLURB ||
          id == IDC_SCHEMAS_DEFAULT_LINE ||
          id == IDC_SCHEMAS_EMPTY;
      const bool disabled = ::IsWindowEnabled(ctl) == FALSE;
      ::SetTextColor(hdc, self->theme_.Color(disabled ? kDisabledText
                                             : secondary ? kOnSurfaceVariant
                                                         : kOnSurface));
      const Role bg = id == IDC_DIAG ? kSurfaceVariant : kBackground;
      ::SetBkColor(hdc, self->theme_.Color(bg));
      return reinterpret_cast<LRESULT>(self->theme_.Brush(bg));
    }
    case WM_DRAWITEM: {
      DRAWITEMSTRUCT* di = reinterpret_cast<DRAWITEMSTRUCT*>(l);
      if (self && di && di->CtlID == IDC_RESET) {
        self->DrawDangerButton(di);
        return TRUE;
      }
      break;
    }
    // 鍵盤使用時才畫焦點環(§12.6.4 第 1 條)。
    case WM_UPDATEUISTATE:
      if (self) {
        const UINT action = LOWORD(w);
        const UINT flags = HIWORD(w);
        if (flags & UISF_HIDEFOCUS) {
          if (action == UIS_CLEAR) self->show_focus_ = true;
          if (action == UIS_SET) self->show_focus_ = false;
        }
        ::RedrawWindow(hwnd, nullptr, nullptr,
                       RDW_INVALIDATE | RDW_ALLCHILDREN);
      }
      break;
    case WM_GETMINMAXINFO: {
      // §12.4.2:最小尺寸靠這一則**強制**,不是只設初始大小。
      MINMAXINFO* mm = reinterpret_cast<MINMAXINFO*>(l);
      const UINT dpi = self ? self->dpi_ : 96;
      RECT r{0, 0, Dip(kWindowMinW, dpi), Dip(kWindowMinH, dpi)};
      ::AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
      mm->ptMinTrackSize.x = r.right - r.left;
      mm->ptMinTrackSize.y = r.bottom - r.top;
      return 0;
    }
    case WM_DPICHANGED:
      if (self) self->OnDpiChanged(HIWORD(w), reinterpret_cast<RECT*>(l));
      return 0;
    case WM_SETTINGCHANGE:
      // 跟著系統即時切換深淺(§12.7.1 末列)。
      if (self && (Theme::IsColorSetChange(l) || w == SPI_SETHIGHCONTRAST))
        self->RefreshTheme();
      break;
    case WM_THEMECHANGED:
      if (self) self->RefreshTheme();
      break;
    case WM_SIZE:
      if (self) self->LayoutUi();
      return 0;
    case WM_RIME_TRAY:
      if (self) self->OnTray(w, l);
      return 0;
    case WM_RIME_OPEN:
      if (self) {
        self->ReloadFromSettings();
        if (w > 0) self->ShowPage(static_cast<int>(w) - 1);
        ::ShowWindow(hwnd, SW_SHOW);
        ::SetForegroundWindow(hwnd);
        ::SetActiveWindow(hwnd);
      }
      return 0;
    case WM_COMMAND:
      if (self) self->OnCommand(LOWORD(w), HIWORD(w));
      return 0;
    case WM_NOTIFY: {
      LRESULT r = 0;
      if (self) self->OnNotify(reinterpret_cast<NMHDR*>(l), &r);
      return r;
    }
    case WM_TIMER:
      if (self && w == kDeployTimer) self->OnDeployTick();
      if (self && w == kStatusTimer) {
        ::KillTimer(hwnd, kStatusTimer);
        self->SetStatus(std::wstring());
      }
      return 0;
    case WM_RIME_SET_VARIANT: {
      const int i = static_cast<int>(w);
      if (self && i >= 0 && i < kVariantCount)
        self->CommitVariantPref(kVariantOrder[i]);
      return 0;
    }
    case WM_CLOSE:
      ::ShowWindow(hwnd, SW_HIDE);
      return 0;
    case WM_DESTROY:
      if (self) self->RemoveTray();
      ::PostQuitMessage(0);
      return 0;
    default:
      if (self && self->taskbar_created_ != 0 && msg == self->taskbar_created_) {
        self->tray_added_ = false;
        self->AddTray();
        return 0;
      }
      break;
  }
  return ::DefWindowProcW(hwnd, msg, w, l);
}

Script SettingsWindow::script() const {
  switch (ui_lang_) {
    case UiLang::kZhHans:
      return Script::kHans;
    case UiLang::kEnUs:
      return Script::kLatin;
    default:
      return Script::kHant;
  }
}

// ─────────────────────────── 建立 ───────────────────────────

void SettingsWindow::CreateUi(HWND hwnd) {
  hwnd_ = hwnd;

  {
    UINT dpi = 96;
    using GetDpiFn = UINT(WINAPI*)(HWND);
    HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
    GetDpiFn fn = u32 ? reinterpret_cast<GetDpiFn>(reinterpret_cast<void*>(
                            ::GetProcAddress(u32, "GetDpiForWindow")))
                      : nullptr;
    if (fn) dpi = fn(hwnd);
    dpi_ = dpi ? dpi : 96;
  }

  // 介面語言要在建任何控制項**之前**決定 —— 控制項的文字是建立時給的。
  settings_ = store_->Load();
  ui_lang_ = ResolveUiLang(settings_.Raw(keys::kAdvancedLanguage), 0);
  SetUiLang(ui_lang_);
  theme_.Refresh(AppearancePrefFromValue(
      settings_.Raw(keys::kAppearanceAppearance).c_str()));
  fonts_.Reset(dpi_, script());
  ::SetWindowTextW(hwnd, UiText(UiString::kWindowTitle));
  theme_.ApplyTitleBar(hwnd);

  HINSTANCE inst = ::GetModuleHandleW(nullptr);

  // 側欄。§12.5.3:SysListView32 單欄 + LVS_SINGLESEL + custom-draw ——
  // 方向鍵巡覽與選取狀態是免費的,而自己畫一排矩形要自己補一份 UIA provider。
  sidebar_ = ::CreateWindowExW(
      0, WC_LISTVIEWW, L"",
      WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_NOCOLUMNHEADER |
          LVS_SHOWSELALWAYS | LVS_NOSCROLL | WS_TABSTOP,
      0, 0, 10, 10, hwnd,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SIDEBAR)), inst,
      nullptr);
  if (sidebar_) {
    LVCOLUMNW col{};
    col.mask = LVCF_WIDTH;
    col.cx = Dip(metric::kSidebarW, dpi_);
    ::SendMessageW(sidebar_, LVM_INSERTCOLUMNW, 0,
                   reinterpret_cast<LPARAM>(&col));
    for (int i = 0; i < kPageCount; ++i) {
      LVITEMW it{};
      it.mask = LVIF_TEXT;
      it.iItem = i;
      it.pszText = const_cast<wchar_t*>(UiText(kPageNames[i]));
      ::SendMessageW(sidebar_, LVM_INSERTITEMW, 0,
                     reinterpret_cast<LPARAM>(&it));
    }
  }

  // ⚠ 每一顆控制項都從 kControls 產生 —— 見那張表上面的說明。
  for (int i = 0; i < kControlCount; ++i) {
    const ControlDef& d = kControls[i];
    const wchar_t* text =
        d.label == kNoText ? L"" : UiText(d.label);
    HWND h = ::CreateWindowExW(0, d.cls, text, WS_CHILD | d.style, 0, 0, 10, 10,
                               hwnd,
                               reinterpret_cast<HMENU>(
                                   static_cast<INT_PTR>(d.id)),
                               inst, nullptr);
    if (d.id == IDC_SCHEMA_LIST) schema_list_ = h;
  }

  if (schema_list_) {
    LVCOLUMNW col{};
    col.mask = LVCF_WIDTH;
    col.cx = Dip(kContentMaxW, dpi_);
    ::SendMessageW(schema_list_, LVM_INSERTCOLUMNW, 0,
                   reinterpret_cast<LPARAM>(&col));
  }

  ApplyFonts();

  const int w = Dip(kWindowDefaultW, dpi_);
  const int h = Dip(kWindowDefaultH, dpi_);
  RECT r{0, 0, w, h};
  ::AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
  ::SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER);
  LayoutUi();
  ShowPage(kPageSchemas);
  // ⚠ 這裡**不**去問引擎要方案清單。CreateUi 跑在 WM_CREATE 裡,而
  //   Engine::SchemaList 會同步等引擎執行緒 —— 服務剛啟動時那條執行緒
  //   可能正在做首次部署前的準備。真的卡住的話 Start() 會逾時,
  //   而症狀是「設定視窗有時候叫不出來」,間歇性又難查。
}

void SettingsWindow::ApplyFonts() {
  HFONT body = fonts_.Get(text_size::t3);
  HFONT small_f = fonts_.Get(text_size::t5);
  HFONT title = fonts_.Get(text_size::t1, true);
  HFONT head = fonts_.Get(text_size::t2, true);
  HFONT mono = fonts_.Get(text_size::t6, false, FontRole::kMono);

  auto set = [&](int id, HFONT f) {
    HWND h = Ctl(hwnd_, id);
    if (h) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
  };
  for (int i = 0; i < kControlCount; ++i) set(kControls[i].id, body);

  for (int id : {IDC_SCHEMAS_TITLE, IDC_APPEAR_TITLE, IDC_TEXT_TITLE,
                 IDC_ADV_TITLE})
    set(id, title);
  for (int id : {IDC_SCHEMAS_LIST_HEAD, IDC_COUNT_HEAD, IDC_SCALE_HEAD,
                 IDC_THEME_HEAD, IDC_BAR_HEAD, IDC_VARIANT_HEAD,
                 IDC_PUNCT_HEAD, IDC_REDEPLOY_HEAD, IDC_FILES_HEAD,
                 IDC_LANG_HEAD, IDC_DIAG_HEAD, IDC_RESET_HEAD})
    set(id, head);
  for (int id : {IDC_SCHEMAS_SUB, IDC_APPEAR_SUB, IDC_TEXT_SUB, IDC_ADV_SUB,
                 IDC_SCHEMAS_LIST_BLURB, IDC_FOLLOW_BLURB, IDC_COUNT_BLURB,
                 IDC_SCALE_BLURB, IDC_THEME_BLURB, IDC_BAR_BLURB,
                 IDC_APPEAR_NOTE, IDC_VARIANT_BLURB, IDC_PUNCT_BLURB,
                 IDC_REDEPLOY_BLURB, IDC_FILES_BLURB, IDC_LANG_BLURB,
                 IDC_DIAG_NOTE, IDC_RESET_BLURB, IDC_STATUS,
                 IDC_SCHEMAS_DEFAULT_LINE,
                 IDC_SCHEMAS_EMPTY})
    set(id, small_f);
  set(IDC_DIAG, mono);
  if (sidebar_)
    ::SendMessageW(sidebar_, WM_SETFONT, reinterpret_cast<WPARAM>(body), TRUE);
}

// ─────────────────────────── 版面 ───────────────────────────

void SettingsWindow::LayoutUi() {
  if (!hwnd_) return;
  RECT rc{};
  ::GetClientRect(hwnd_, &rc);
  const int W_px = rc.right - rc.left;
  const int H_px = rc.bottom - rc.top;
  // 版面在 DIP 上算(純函式),最後才換成像素。
  const int W = MulDivRound(W_px, 96, static_cast<int>(dpi_));
  const int H = MulDivRound(H_px, 96, static_cast<int>(dpi_));
  const int dpi = static_cast<int>(dpi_);

  auto place = [&](int id, const RectI& r) {
    HWND c = Ctl(hwnd_, id);
    if (!c) return;
    ::SetWindowPos(c, nullptr, Dip(r.x, dpi), Dip(r.y, dpi), Dip(r.w, dpi),
                   Dip(r.h, dpi), SWP_NOZORDER);
  };

  // 側欄:整條左邊,狀態區留在下面。
  if (sidebar_) {
    const int sb_h = H - metric::kSidebarStatusH;
    ::SetWindowPos(sidebar_, nullptr, 0, Dip(space::s5, dpi),
                   Dip(metric::kSidebarW, dpi), Dip(sb_h, dpi), SWP_NOZORDER);
    ::SendMessageW(sidebar_, LVM_SETCOLUMNWIDTH, 0,
                   MAKELPARAM(Dip(metric::kSidebarW, dpi), 0));
  }

  const int cx = ContentXDip(W);
  const int cw = ContentWidthDip(W);
  const int t3h = text_size::t3 + space::s3;
  const int t5h = text_size::t5 + space::s2;
  const int btn_h = metric::kMinTarget + space::s2;
  const int bottom_h = metric::kMinTarget + space::s7;

  Stack st(cx, space::s8, cw);

  auto title_block = [&](int title_id, int sub_id) {
    place(title_id, st.Push(text_size::t1 + space::s3, space::s1));
    place(sub_id, st.Push(t5h, space::s7));
  };
  auto heading = [&](int head_id, int blurb_id, int blurb_lines) {
    place(head_id, st.Push(text_size::t2 + space::s2, space::s1));
    if (blurb_id) place(blurb_id, st.Push(t5h * blurb_lines, space::s3));
  };
  // 單選群組:一列一個(桌面欄的密度)。
  auto radios = [&](int first, int count) {
    for (int i = 0; i < count; ++i)
      place(first + i, st.Push(metric::kMinTarget, space::s1));
    st.Skip(space::s7 - space::s1);
  };

  switch (page_) {
    case kPageSchemas: {
      title_block(IDC_SCHEMAS_TITLE, IDC_SCHEMAS_SUB);
      heading(IDC_SCHEMAS_LIST_HEAD, IDC_SCHEMAS_LIST_BLURB, 2);
      const bool empty = order_.empty();
      const int list_h = 4 * metric::kSidebarItemH + space::s3;
      if (empty) {
        place(IDC_SCHEMA_LIST, RectI{});
        place(IDC_SCHEMAS_EMPTY, st.Push(t5h * 4, space::s7));
      } else {
        place(IDC_SCHEMAS_EMPTY, RectI{});
        place(IDC_SCHEMA_LIST, st.Push(list_h, space::s3));
        const RectI row = st.Push(btn_h, space::s3);
        const int bw = (cw - 2 * space::s3) / 3;
        place(IDC_UP, RectI{row.x, row.y, bw, row.h});
        place(IDC_DOWN, RectI{row.x + bw + space::s3, row.y, bw, row.h});
        place(IDC_APPLY_ORDER,
              RectI{row.x + 2 * (bw + space::s3), row.y, bw, row.h});
        place(IDC_SCHEMAS_DEFAULT_LINE, st.Push(t5h, space::s7));
      }
      place(IDC_FOLLOW_MODE, st.Push(metric::kSidebarItemH, space::s1));
      place(IDC_FOLLOW_BLURB, st.Push(t5h * 3, space::s7));
      break;
    }
    case kPageAppearance: {
      title_block(IDC_APPEAR_TITLE, IDC_APPEAR_SUB);
      heading(IDC_COUNT_HEAD, IDC_COUNT_BLURB, 2);
      radios(IDC_COUNT_0, 5);
      heading(IDC_SCALE_HEAD, IDC_SCALE_BLURB, 1);
      radios(IDC_SCALE_0, 5);
      heading(IDC_THEME_HEAD, IDC_THEME_BLURB, 1);
      radios(IDC_THEME_0, 3);
      heading(IDC_BAR_HEAD, IDC_BAR_BLURB, 3);
      place(IDC_BAR_SHOW, st.Push(metric::kSidebarItemH, space::s7));
      place(IDC_APPEAR_NOTE, st.Push(t5h * 4, 0));
      break;
    }
    case kPageText: {
      title_block(IDC_TEXT_TITLE, IDC_TEXT_SUB);
      heading(IDC_VARIANT_HEAD, IDC_VARIANT_BLURB, 1);
      radios(IDC_VARIANT_0, 3);
      heading(IDC_PUNCT_HEAD, IDC_PUNCT_BLURB, 2);
      radios(IDC_PUNCT_0, 3);
      break;
    }
    case kPageAdvanced: {
      title_block(IDC_ADV_TITLE, IDC_ADV_SUB);
      heading(IDC_REDEPLOY_HEAD, IDC_REDEPLOY_BLURB, 2);
      place(IDC_REDEPLOY, RectI{cx, st.y(), 180, btn_h});
      st.Skip(btn_h + space::s7);
      heading(IDC_FILES_HEAD, IDC_FILES_BLURB, 2);
      {
        const RectI row = st.Push(btn_h, space::s7);
        const int bw = (cw - space::s3) / 2;
        place(IDC_OPEN_USER_DIR, RectI{row.x, row.y, bw, row.h});
        place(IDC_OPEN_SETTINGS_FILE,
              RectI{row.x + bw + space::s3, row.y, bw, row.h});
      }
      heading(IDC_LANG_HEAD, IDC_LANG_BLURB, 1);
      radios(IDC_LANG_0, 4);
      heading(IDC_DIAG_HEAD, IDC_DIAG_NOTE, 2);
      place(IDC_DIAG, st.Push(t5h * 6, space::s3));
      place(IDC_DIAG_COPY, RectI{cx, st.y(), 120, btn_h});
      st.Skip(btn_h);
      // ⚠ 危險操作一律是該頁最後一個區塊,上面隔一條 hairline + s7
      //   (§4.9 / §2-C2)。PushDivider 就是那條線。
      st.PushDivider();
      heading(IDC_RESET_HEAD, IDC_RESET_BLURB, 2);
      place(IDC_RESET, RectI{cx, st.y(), 220, btn_h});
      break;
    }
    default:
      break;
  }

  // 永遠看得見的兩個。
  place(IDC_STATUS, RectI{cx, H - bottom_h, cw - 120 - space::s3,
                          metric::kMinTarget});
  place(IDC_CLOSE, RectI{W - space::s7 - 100, H - bottom_h, 100,
                         metric::kMinTarget});
  ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void SettingsWindow::ShowPage(int page) {
  if (page < 0 || page >= kPageCount) page = 0;
  page_ = page;
  for (int i = 0; i < kControlCount; ++i) {
    const ControlDef& d = kControls[i];
    const bool visible =
        d.page == kAlways || d.page == page;
    HWND h = Ctl(hwnd_, d.id);
    if (h) ::ShowWindow(h, visible ? SW_SHOW : SW_HIDE);
  }
  if (sidebar_) {
    LVITEMW it{};
    it.mask = LVIF_STATE;
    it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    it.state = LVIS_SELECTED | LVIS_FOCUSED;
    ::SendMessageW(sidebar_, LVM_SETITEMSTATE, static_cast<WPARAM>(page),
                   reinterpret_cast<LPARAM>(&it));
  }
  LayoutUi();
}


// ─────────────────────────── 自繪 ───────────────────────────
//
// §12.5.1 的判準只有一條:**只有在系統控制項無法表達規範要求的某個狀態時,
// 才 owner-draw。外觀不夠好看不是理由。** 理由是無障礙,不是省事 ——
// owner-draw 會同時拿走 MSAA/UIA 的角色與狀態,以及高對比佈景的自動適配。
//
// 所以下面三處都用 **NM_CUSTOMDRAW / WM_DRAWITEM**,而不是自己開一個
// 沒有控制項的矩形:ListView 保留逐列的 UIA 元素與選取狀態,
// owner-draw 的 push button 仍然是 push button。

LRESULT SettingsWindow::DrawSidebar(NMLVCUSTOMDRAW* cd) {
  switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
      return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
      const int i = static_cast<int>(cd->nmcd.dwItemSpec);
      const bool selected = (cd->nmcd.uItemState & CDIS_SELECTED) != 0;
      const bool hot = (cd->nmcd.uItemState & CDIS_HOT) != 0;
      // ⚠ 焦點環只在**鍵盤**使用時畫(§12.6.4 第 1 條)。滑鼠使用者身上
      //   到處是框,是 Win32 自繪最常見的破綻。show_focus_ 由
      //   WM_UPDATEUISTATE 維護。
      const bool focused =
          show_focus_ && (cd->nmcd.uItemState & CDIS_FOCUS) != 0;

      HDC hdc = cd->nmcd.hdc;
      RECT r = cd->nmcd.rc;
      // 側欄項的內距(§12.4.2:側欄左右內距 12)。
      RECT item = r;
      item.left += Dip(space::s5, dpi_);
      item.right -= Dip(space::s5, dpi_);

      ::FillRect(hdc, &r, theme_.Brush(kBackground));
      const Role bg = selected ? (hot ? kRowSelectedHover : kPrimaryContainer)
                               : (hot ? kRowHover : kBackground);
      if (bg != kBackground) ::FillRect(hdc, &item, theme_.Brush(bg));

      if (focused) {
        // §12.6.4 第 2 條:**不要用 DrawFocusRect** —— 它是 XOR 的點線框,
        // 在我們的色票上會變成不可預測的顏色。自己畫 2 DIP 的框。
        HPEN pen = theme_.Pen(kPrimary, Dip(2, dpi_));
        HGDIOBJ oldp = ::SelectObject(hdc, pen);
        HGDIOBJ oldb = ::SelectObject(hdc, ::GetStockObject(NULL_BRUSH));
        ::Rectangle(hdc, item.left, item.top, item.right, item.bottom);
        ::SelectObject(hdc, oldb);
        ::SelectObject(hdc, oldp);
      }

      ::SetBkMode(hdc, TRANSPARENT);
      ::SetTextColor(hdc, theme_.Color(selected ? kOnSurface
                                                : kOnSurfaceVariant));
      HGDIOBJ oldf = ::SelectObject(hdc, fonts_.Get(text_size::t3, selected));
      RECT tr = item;
      tr.left += Dip(space::s4, dpi_);
      const wchar_t* label =
          (i >= 0 && i < kPageCount) ? UiText(kPageNames[i]) : L"";
      ::DrawTextW(hdc, label, -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
      ::SelectObject(hdc, oldf);
      return CDRF_SKIPDEFAULT;
    }
    default:
      return CDRF_DODEFAULT;
  }
}

LRESULT SettingsWindow::DrawSchemaList(NMLVCUSTOMDRAW* cd) {
  switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
      return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
      const int i = static_cast<int>(cd->nmcd.dwItemSpec);
      const bool selected = (cd->nmcd.uItemState & CDIS_SELECTED) != 0;
      const bool hot = (cd->nmcd.uItemState & CDIS_HOT) != 0;
      const bool focused =
          show_focus_ && (cd->nmcd.uItemState & CDIS_FOCUS) != 0;

      HDC hdc = cd->nmcd.hdc;
      RECT r = cd->nmcd.rc;
      const Role bg = selected ? (hot ? kRowSelectedHover : kPrimaryContainer)
                               : (hot ? kRowHover : kSurface);
      ::FillRect(hdc, &r, theme_.Brush(bg));

      if (focused) {
        HPEN pen = theme_.Pen(kPrimary, Dip(2, dpi_));
        HGDIOBJ oldp = ::SelectObject(hdc, pen);
        HGDIOBJ oldb = ::SelectObject(hdc, ::GetStockObject(NULL_BRUSH));
        ::Rectangle(hdc, r.left, r.top, r.right, r.bottom);
        ::SelectObject(hdc, oldb);
        ::SelectObject(hdc, oldp);
      }

      ::SetBkMode(hdc, TRANSPARENT);
      RECT tr = r;
      tr.left += Dip(space::s4, dpi_);
      tr.right -= Dip(space::s4, dpi_);

      // 「預設」徽章:順序的第一個就是預設。⚠ 這一句是規範性的
      // (§6.7 第一層)—— 這一列上**不可以**出現方案 id,只有名字。
      // 舊版印的是「名字  (id)」,而 id 是引擎的內部識別字。
      if (i == 0) {
        const wchar_t* badge = UiText(UiString::kSchemasDefaultBadge);
        SIZE bs{};
        HGDIOBJ oldf2 = ::SelectObject(hdc, fonts_.Get(text_size::t5));
        ::GetTextExtentPoint32W(hdc, badge, ::lstrlenW(badge), &bs);
        RECT br{tr.right - bs.cx - 2 * Dip(space::s4, dpi_),
                r.top + Dip(space::s3, dpi_), tr.right,
                r.bottom - Dip(space::s3, dpi_)};
        ::FillRect(hdc, &br, theme_.Brush(kSurfaceVariant));
        ::SetTextColor(hdc, theme_.Color(kOnSurfaceVariant));
        ::DrawTextW(hdc, badge, -1, &br,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        ::SelectObject(hdc, oldf2);
        tr.right = br.left - Dip(space::s3, dpi_);
      }

      ::SetTextColor(hdc, theme_.Color(kOnSurface));
      HGDIOBJ oldf = ::SelectObject(hdc, fonts_.Get(text_size::t3));
      const std::wstring name = SchemaDisplayName(static_cast<size_t>(i));
      ::DrawTextW(hdc, name.c_str(), -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
      ::SelectObject(hdc, oldf);
      return CDRF_SKIPDEFAULT;
    }
    default:
      return CDRF_DODEFAULT;
  }
}

void SettingsWindow::DrawDangerButton(DRAWITEMSTRUCT* di) {
  // §4.9 / §2-C1:**外框 + 危險色文字 + 透明底**。不得用危險色實心底 ——
  // 實心的紅底看起來像「這是主要動作」,而它正好相反。
  HDC hdc = di->hDC;
  RECT r = di->rcItem;
  const bool pressed = (di->itemState & ODS_SELECTED) != 0;
  const bool disabled = (di->itemState & ODS_DISABLED) != 0;
  const bool focused = show_focus_ && (di->itemState & ODS_FOCUS) != 0;
  // hover 要自己追(WM_DRAWITEM 不給 hot 狀態)。這裡用按下狀態代替,
  // 少一階視覺回饋,但不會騙人。
  ::FillRect(hdc, &r, theme_.Brush(kBackground));
  if (pressed) ::FillRect(hdc, &r, theme_.Brush(kDangerPressed));

  const Role fg = disabled ? kDisabledText : kError;
  HPEN pen = theme_.Pen(fg, Dip(1, dpi_));
  HGDIOBJ oldp = ::SelectObject(hdc, pen);
  HGDIOBJ oldb = ::SelectObject(hdc, ::GetStockObject(NULL_BRUSH));
  ::Rectangle(hdc, r.left, r.top, r.right, r.bottom);
  ::SelectObject(hdc, oldb);
  ::SelectObject(hdc, oldp);

  if (focused) {
    HPEN fp = theme_.Pen(kPrimary, Dip(2, dpi_));
    HGDIOBJ o1 = ::SelectObject(hdc, fp);
    HGDIOBJ o2 = ::SelectObject(hdc, ::GetStockObject(NULL_BRUSH));
    const int in = Dip(2, dpi_);
    ::Rectangle(hdc, r.left + in, r.top + in, r.right - in, r.bottom - in);
    ::SelectObject(hdc, o2);
    ::SelectObject(hdc, o1);
  }

  wchar_t buf[128] = {0};
  ::GetWindowTextW(di->hwndItem, buf, 128);
  ::SetBkMode(hdc, TRANSPARENT);
  ::SetTextColor(hdc, theme_.Color(fg));
  HGDIOBJ oldf = ::SelectObject(hdc, fonts_.Get(text_size::t4));
  ::DrawTextW(hdc, buf, -1, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  ::SelectObject(hdc, oldf);
}

void SettingsWindow::OnPaint(HDC hdc) {
  // 容器裝飾:側欄與內容區之間的分隔線,以及側欄底部的狀態區。
  // 它們不是控制項,所以不涉及無障礙 —— 在父視窗的 WM_PAINT 裡畫。
  RECT rc{};
  ::GetClientRect(hwnd_, &rc);
  const int W = rc.right - rc.left;
  const int H = rc.bottom - rc.top;
  const int sb = Dip(metric::kSidebarW, dpi_);

  RECT side{0, 0, sb, H};
  ::FillRect(hdc, &side, theme_.Brush(kSurface));
  RECT line{sb, 0, sb + Dip(metric::kHairline, dpi_), H};
  ::FillRect(hdc, &line, theme_.Brush(kOutline));

  // ── 側欄底部的狀態區(§7.2,每一頁都在)────────────────────
  //
  // ⚠ 第一行不是裝飾。`product-gaps.md` §4.2:「未啟動」目前唯一的訊號是
  //   語言列上那四個字,而它在 Win11 上多半收在輸入指示器裡要點兩下才看得到。
  //   這裡與懸浮狀態列是同一個訊號的兩個家。
  ::SetBkMode(hdc, TRANSPARENT);
  HGDIOBJ oldf = ::SelectObject(hdc, fonts_.Get(text_size::t5));
  const int pad = Dip(space::s5, dpi_);
  const int top = H - Dip(metric::kSidebarStatusH, dpi_);
  RECT r1{pad, top + Dip(space::s3, dpi_), sb - pad,
          top + Dip(space::s3 + text_size::t5 + 4, dpi_)};
  const bool ready = engine_ && engine_->deploy_done() && engine_->deploy_ok();
  ::SetTextColor(hdc, theme_.Color(ready ? kOnSurfaceVariant : kError));
  ::DrawTextW(hdc,
              UiText(ready ? UiString::kNavStatusReady
                           : UiString::kNavStatusNotRunning),
              -1, &r1, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  RECT r2 = r1;
  r2.top = r1.bottom + Dip(space::s1, dpi_);
  r2.bottom = r2.top + Dip(text_size::t5 + 4, dpi_);
  ::SetTextColor(hdc, theme_.Color(kOnSurfaceVariant));
  ::DrawTextW(hdc, UiText(UiString::kNavStatusOffline), -1, &r2,
              DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  ::SelectObject(hdc, oldf);

  // 底部狀態行上面那一條 hairline。
  RECT bl{Dip(metric::kSidebarW, dpi_),
          H - Dip(metric::kMinTarget + space::s7 + space::s3, dpi_), W,
          H - Dip(metric::kMinTarget + space::s7 + space::s3, dpi_) +
              Dip(metric::kHairline, dpi_)};
  ::FillRect(hdc, &bl, theme_.Brush(kOutline));
}

// ─────────────────────────── DPI 與佈景 ───────────────────────────

void SettingsWindow::OnDpiChanged(UINT dpi, const RECT* suggested) {
  // §12.3 的三條硬規則之一:進程是 per-monitor-v2,所以系統**會**送這則
  // 訊息;不處理的結果不是「維持原樣」,是**版面與系統的期待脫節**。
  dpi_ = dpi ? dpi : 96;
  // 字型是像素單位的 —— 不重建就是模糊或錯大小。
  fonts_.Reset(dpi_, script());
  ApplyFonts();
  if (suggested)
    ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                   suggested->right - suggested->left,
                   suggested->bottom - suggested->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
  if (sidebar_) {
    // ⚠ 子控制項**不會**各自收到 WM_DPICHANGED,一律由父視窗重新配置。
    ::SendMessageW(sidebar_, LVM_SETCOLUMNWIDTH, 0,
                   MAKELPARAM(Dip(metric::kSidebarW, dpi_), 0));
  }
  LayoutUi();
  ::RedrawWindow(hwnd_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void SettingsWindow::RefreshTheme() {
  if (!theme_.Refresh(AppearancePrefFromValue(
          settings_.Raw(keys::kAppearanceAppearance).c_str())))
    return;
  theme_.ApplyTitleBar(hwnd_);
  // ⚠ 標題列不一定跟著重畫,補一發 SWP_FRAMECHANGED。
  ::SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
  ::RedrawWindow(hwnd_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
  if (bar_) bar_->RefreshTheme();
}

// ─────────────────────────── 讀 / 寫 ───────────────────────────

std::wstring SettingsWindow::SchemaDisplayName(size_t index) const {
  if (index >= schemas_.size()) return std::wstring();
  const auto& kv = schemas_[index];
  // ⚠ **只有名字。** 舊版是 `名字  (id)`,而 schema id 是引擎的內部識別字
  //   —— §6.7 第一層硬禁它出現在使用者看得到的地方(W17)。
  //   名字為空時才退回 id:那時沒有別的東西可以顯示,而一列空白
  //   比一個看不懂的字串更糟。
  return Utf8ToWide(kv.second.empty() ? kv.first : kv.second);
}

int SettingsWindow::SelectedSchemaRow() const {
  if (!schema_list_) return -1;
  return static_cast<int>(::SendMessageW(schema_list_, LVM_GETNEXTITEM,
                                         static_cast<WPARAM>(-1),
                                         LVNI_SELECTED));
}

void SettingsWindow::ReloadSchemaList() {
  schemas_ = engine_->SchemaList();
  order_.clear();
  for (const auto& kv : schemas_) order_.push_back(kv.first);

  if (schema_list_) {
    ::SendMessageW(schema_list_, LVM_DELETEALLITEMS, 0, 0);
    for (size_t i = 0; i < schemas_.size(); ++i) {
      LVITEMW it{};
      it.mask = LVIF_TEXT;
      it.iItem = static_cast<int>(i);
      // 文字由 custom-draw 畫,但仍然要設 —— 螢幕閱讀器讀的是這一份。
      std::wstring name = SchemaDisplayName(i);
      it.pszText = const_cast<wchar_t*>(name.c_str());
      ::SendMessageW(schema_list_, LVM_INSERTITEMW, 0,
                     reinterpret_cast<LPARAM>(&it));
    }
    if (!schemas_.empty()) {
      LVITEMW it{};
      it.mask = LVIF_STATE;
      it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
      it.state = LVIS_SELECTED | LVIS_FOCUSED;
      ::SendMessageW(schema_list_, LVM_SETITEMSTATE, 0,
                     reinterpret_cast<LPARAM>(&it));
    }
  }

  // ② 「現在預設是『注音 · 臺灣正體』」(§12.4.3)。
  if (!schemas_.empty()) {
    std::wstring line = UiText(UiString::kSchemasCurrentDefaultPrefix);
    line += L"「";
    line += SchemaDisplayName(0);
    line += L"」";
    SetText(hwnd_, IDC_SCHEMAS_DEFAULT_LINE, line.c_str());
  } else {
    // §4.7 的空狀態要說三件事:為什麼是空的、這是不是正常、下一步按哪裡。
    std::wstring empty = UiText(UiString::kSchemasEmptyTitle);
    empty += L"\r\n";
    empty += UiText(UiString::kSchemasEmptyWhy);
    empty += L"\r\n";
    empty += UiText(UiString::kSchemasEmptyNext);
    SetText(hwnd_, IDC_SCHEMAS_EMPTY, empty.c_str());
  }
  LayoutUi();
}

void SettingsWindow::ReloadFromSettings() {
  if (!hwnd_) return;
  settings_ = store_->Load();

  const UiLang want = ResolveUiLang(settings_.Raw(keys::kAdvancedLanguage), 0);
  if (want != ui_lang_) {
    ui_lang_ = want;
    SetUiLang(want);
    fonts_.Reset(dpi_, script());
    for (int i = 0; i < kControlCount; ++i)
      if (kControls[i].label != kNoText)
        SetText(hwnd_, kControls[i].id, UiText(kControls[i].label));
    ::SetWindowTextW(hwnd_, UiText(UiString::kWindowTitle));
    ApplyFonts();
  }
  RefreshTheme();
  ReloadSchemaList();

  const SchemaPreference pref = settings_.SchemaPref();
  int vsel = 0;
  for (int i = 0; i < kVariantCount; ++i)
    if (kVariantOrder[i] == pref.variant) vsel = i;
  CheckRadio(hwnd_, IDC_VARIANT_0, kVariantCount, vsel);
  ::SendMessageW(Ctl(hwnd_, IDC_FOLLOW_MODE), BM_SETCHECK,
                 pref.follow_input_mode ? BST_CHECKED : BST_UNCHECKED, 0);

  const Tri punct = settings_.Punctuation();
  CheckRadio(hwnd_, IDC_PUNCT_0, 3,
             punct == Tri::kUnset ? 0 : (punct == Tri::kFalse ? 1 : 2));

  // ⚠ 一次顯示幾個字是 **A 層**(librime 的一頁候選數),不在設定檔裡 ——
  //   所以它從 default.custom.yaml 讀。
  {
    int n = 0;
    const std::string raw =
        ReadPatchScalar(store_->ReadDefaultCustom(), "menu/page_size");
    for (char c : raw) {
      if (c < '0' || c > '9') { n = 0; break; }
      n = n * 10 + (c - '0');
      if (n > 100) { n = 0; break; }
    }
    CheckRadio(hwnd_, IDC_COUNT_0, kCandCountCount, IndexOfCandCount(n));
  }
  CheckRadio(hwnd_, IDC_SCALE_0, kCandScaleCount,
             IndexOfCandScale(settings_.GetEnumInt(
                 keys::kAppearanceCandidateScale, kCandScaleValues,
                 kCandScaleCount)));

  {
    const AppearancePref ap = AppearancePrefFromValue(
        settings_.Raw(keys::kAppearanceAppearance).c_str());
    CheckRadio(hwnd_, IDC_THEME_0, 3, static_cast<int>(ap));
  }
  {
    const std::string lang = settings_.Raw(keys::kAdvancedLanguage);
    int sel = 0;
    if (lang == "en") sel = 1;
    else if (lang == "zh-Hant") sel = 2;
    else if (lang == "zh-Hans") sel = 3;
    CheckRadio(hwnd_, IDC_LANG_0, 4, sel);
  }
  // 懸浮狀態列預設**開**(§12.10.2:它是中英切換唯一的家)。
  ::SendMessageW(Ctl(hwnd_, IDC_BAR_SHOW), BM_SETCHECK,
                 settings_.GetTri(keys::kAppearanceFloatingBar) == Tri::kFalse
                     ? BST_UNCHECKED
                     : BST_CHECKED,
                 0);

  // ── 診斷(§4.11:永遠英文、等寬、不進 catalog)──────────────
  {
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  "kDiagPlatform: Windows x64\r\n"
                  "kDiagShellAbi: %d\r\n"
                  "kDiagWireVersion: %u\r\n"
                  "kDiagDpi: %u\r\n"
                  "kDiagUserDir: %s\r\n"
                  "kDiagSharedDir: %s\r\n"
                  "kDiagSettingsFile: %s\r\n"
                  "kDiagDeployDone: %d\r\n"
                  "kDiagDeployOk: %d\r\n",
                  engine_->AbiVersion(),
                  static_cast<unsigned>(kProtocolVersion),
                  static_cast<unsigned>(dpi_), store_->user_dir().c_str(),
                  shared_dir_.c_str(), store_->settings_path().c_str(),
                  engine_->deploy_done() ? 1 : 0, engine_->deploy_ok() ? 1 : 0);
    SetText(hwnd_, IDC_DIAG, Utf8ToWide(buf).c_str());
  }
  SetStatus(std::wstring());
}

void SettingsWindow::SetStatus(const std::wstring& text) {
  SetText(hwnd_, IDC_STATUS, text.c_str());
}

void SettingsWindow::SetTransientStatus(UiString s) {
  // §12.5.3 末列:Win32 沒有 toast,而系統匣氣球使用者可以整個關掉
  // (等於這則訊息可能永遠不出現)。**不要做浮層** —— 用視窗底部
  // 已經有的那一行,4 秒後清掉。成功訊息不值得一個新表面。
  SetStatus(UiText(s));
  ::SetTimer(hwnd_, kStatusTimer, 4000, nullptr);
}

// ─────────────────────────── 套用 ───────────────────────────

void SettingsWindow::ApplyVariantNow() {
  const int sel = RadioSel(hwnd_, IDC_VARIANT_0, kVariantCount);
  CommitVariantPref(kVariantOrder[(sel >= 0 && sel < kVariantCount) ? sel : 0]);
}

void SettingsWindow::CommitVariantPref(VariantPref v) {
  settings_.SetVariantPref(v);
  if (!store_->Save(settings_)) {
    SetStatus(UiString::kStatusSaveFailed);
    return;
  }
  // 立刻對現有的每一個輸入視窗套用。少了這一步,使用者改了之後要換一個
  // 程式才會生效 —— 而他當下看到的是「這個選項沒有作用」。
  engine_->ApplyVariantAll(settings_.SchemaPref());
  int vsel = 0;
  for (int i = 0; i < kVariantCount; ++i)
    if (kVariantOrder[i] == v) vsel = i;
  CheckRadio(hwnd_, IDC_VARIANT_0, kVariantCount, vsel);
  if (bar_) bar_->Refresh();
  SetTransientStatus(UiString::kStatusApplied);
}

void SettingsWindow::SetVariantPref(VariantPref v) {
  int idx = 0;
  for (int i = 0; i < kVariantCount; ++i)
    if (kVariantOrder[i] == v) idx = i;
  if (hwnd_)
    ::PostMessageW(hwnd_, WM_RIME_SET_VARIANT, static_cast<WPARAM>(idx), 0);
}

VariantPref SettingsWindow::CurrentVariantPref() {
  if (settings_.size() == 0) settings_ = store_->Load();
  return settings_.SchemaPref().variant;
}

void SettingsWindow::ApplyPunctNow() {
  const int sel = RadioSel(hwnd_, IDC_PUNCT_0, 3);
  const Tri t = sel == 0 ? Tri::kUnset : (sel == 1 ? Tri::kFalse : Tri::kTrue);
  settings_.SetPunctuation(t);
  if (!store_->Save(settings_)) {
    SetStatus(UiString::kStatusSaveFailed);
    return;
  }
  // ⚠ kUnset(不干預)= **完全不呼叫 rs_set_option**。設成 false 不是
  //   同一件事:很多方案根本沒有這個開關,而有些方案的預設是 true。
  if (t != Tri::kUnset) engine_->SetOptionAll("ascii_punct", t == Tri::kTrue);
  SetTransientStatus(t == Tri::kUnset ? UiString::kStatusPunctFollow
                                      : UiString::kStatusApplied);
}

void SettingsWindow::ApplyAppearancePref() {
  const int sel = RadioSel(hwnd_, IDC_THEME_0, 3);
  settings_.SetRaw(keys::kAppearanceAppearance,
                   AppearancePrefValue(static_cast<AppearancePref>(sel)));
  // 「跟著系統」= 沒表示過意見 = **刪掉那個鍵**(settings.h 檔頭)。
  if (sel == 0) settings_.Unset(keys::kAppearanceAppearance);
  if (!store_->Save(settings_)) {
    SetStatus(UiString::kStatusSaveFailed);
    return;
  }
  RefreshTheme();
  SetTransientStatus(UiString::kStatusApplied);
}

void SettingsWindow::ApplyUiLanguage() {
  const int sel = RadioSel(hwnd_, IDC_LANG_0, 4);
  const char* v = sel == 1 ? "en" : sel == 2 ? "zh-Hant" : sel == 3 ? "zh-Hans"
                                                                   : "system";
  if (sel == 0)
    settings_.Unset(keys::kAdvancedLanguage);
  else
    settings_.SetRaw(keys::kAdvancedLanguage, v);
  if (!store_->Save(settings_)) {
    SetStatus(UiString::kStatusSaveFailed);
    return;
  }
  ui_lang_ = ResolveUiLang(settings_.Raw(keys::kAdvancedLanguage), 0);
  SetUiLang(ui_lang_);
  // 字型跟著語言換(§8.4.2 的 script_of(run))—— 中文介面配一個沒有漢字的
  // 字體,結果是整片交給 GDI 的 font linking,字形不是我們選的。
  fonts_.Reset(dpi_, script());
  for (int i = 0; i < kControlCount; ++i)
    if (kControls[i].label != kNoText)
      SetText(hwnd_, kControls[i].id, UiText(kControls[i].label));
  ::SetWindowTextW(hwnd_, UiText(UiString::kWindowTitle));
  ApplyFonts();
  ReloadSchemaList();
  if (bar_) bar_->Refresh();
  ::RedrawWindow(hwnd_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
  SetTransientStatus(UiString::kStatusApplied);
}

void SettingsWindow::ApplyScaleNow() {
  const int v = CandScaleAtIndex(RadioSel(hwnd_, IDC_SCALE_0, kCandScaleCount));
  settings_.SetEnumInt(keys::kAppearanceCandidateScale, v, kCandScaleValues,
                       kCandScaleCount);
  if (!store_->Save(settings_)) {
    SetStatus(UiString::kStatusSaveFailed);
    return;
  }
  if (cand_) cand_->SetTextScale(v <= 0 ? 1.0 : v / 100.0);
  SetTransientStatus(UiString::kStatusScaleApplied);
}

void SettingsWindow::ApplyStatusBarVisibility() {
  const bool on = ::SendMessageW(Ctl(hwnd_, IDC_BAR_SHOW), BM_GETCHECK, 0, 0) ==
                  BST_CHECKED;
  // 預設是**開**,所以「開」= 沒表示過意見 = 刪掉那個鍵。
  if (on)
    settings_.Unset(keys::kAppearanceFloatingBar);
  else
    settings_.SetTri(keys::kAppearanceFloatingBar, Tri::kFalse);
  if (!store_->Save(settings_)) {
    SetStatus(UiString::kStatusSaveFailed);
    return;
  }
  if (bar_) bar_->SetVisible(on);
  SetTransientStatus(UiString::kStatusApplied);
}

void SettingsWindow::DoResetSettings() {
  // §2-C5:不可逆的動作要同時點名「會消失的是什麼」與「不會消失的是什麼」。
  // 兩句都在 kResetConfirmBody 裡。
  if (!ConfirmDialog(hwnd_, &theme_, script(), UiText(UiString::kResetHeading),
                     UiText(UiString::kResetConfirmBody),
                     UiText(UiString::kResetButton), UiText(UiString::kCancel)))
    return;
  // ⚠ 只清 B 層。**不碰** default.custom.yaml(方案順序與一頁幾個字),
  //   也不碰使用者詞典 —— 那正是確認文案承諾不會消失的東西。
  settings_ = Settings();
  if (!store_->Save(settings_)) {
    SetStatus(UiString::kStatusSaveFailed);
    return;
  }
  ReloadFromSettings();
  engine_->ApplyVariantAll(settings_.SchemaPref());
  if (cand_) cand_->SetTextScale(1.0);
  if (bar_) {
    bar_->SetVisible(true);
    bar_->Refresh();
  }
  SetTransientStatus(UiString::kStatusResetDone);
}

bool SettingsWindow::ApplyOrderAndPageSize(std::string* error) {
  std::string yaml = store_->ReadDefaultCustom();
  if (yaml.empty() && !store_->DefaultCustomExists()) {
    *error = "missing";
    return false;
  }
  // 快照。失敗時整份寫回去。
  rollback_yaml_ = yaml;
  has_rollback_ = true;
  std::string next;
  PatchResult r = WriteSchemaList(yaml, order_, &next);
  if (r != PatchResult::kOk) {
    *error = r == PatchResult::kNoPatchSection ? "unreadable" : "invalid";
    return false;
  }
  const int count = CandCountAtIndex(RadioSel(hwnd_, IDC_COUNT_0,
                                              kCandCountCount));
  std::string next2;
  char num[16] = {0};
  if (count > 0) std::snprintf(num, sizeof(num), "%d", count);
  r = UpsertPatchScalar(next, "menu/page_size", count > 0 ? num : "", &next2);
  if (r != PatchResult::kOk) {
    *error = "write";
    return false;
  }
  if (!store_->WriteDefaultCustom(next2)) {
    *error = "write";
    return false;
  }
  return true;
}

void SettingsWindow::StartRedeploy(UiString why) {
  if (deploying_) return;
  if (!engine_->BeginDeploy(&deploy_seq_)) {
    // rs_deploy() 拒絕啟動,多半是已經有一個在跑。
    // ⚠ 這裡**一定**要說話。Android 端踩過的原話是「使用者只能猜它成功了沒」。
    std::wstring msg = UiText(UiString::kRedeployRefused);
    msg += Utf8ToWide(engine_->last_error());
    MessageDialog(hwnd_, &theme_, script(), UiText(UiString::kRedeployFailTitle),
                  msg, UiText(UiString::kCancel));
    return;
  }
  deploying_ = true;
  deploy_why_ = why;
  deploy_start_ = ::GetTickCount();
  // ⚠ W23:停用的控制項,同一頁必須有一句說明為什麼。
  //   這裡那句話就是底下狀態行的「正在整理字詞…已耗時 N 秒」,
  //   而它是全對比的、就在同一個視窗裡、而且一直在動。
  ::EnableWindow(Ctl(hwnd_, IDC_REDEPLOY), FALSE);
  ::EnableWindow(Ctl(hwnd_, IDC_APPLY_ORDER), FALSE);
  wchar_t buf[160];
  ::swprintf(buf, 160, UiText(UiString::kStatusRedeployRunning), 0u);
  SetStatus(buf);
  ::SetTimer(hwnd_, kDeployTimer, 200, nullptr);
}

void SettingsWindow::OnDeployTick() {
  if (!deploying_) return;
  const DWORD elapsed = ::GetTickCount() - deploy_start_;
  int status = 0;
  if (!engine_->PollDeploy(deploy_seq_, &status)) {
    // librime 不給百分比,所以我們只說實話:已經跑了多久。
    // 畫一條假的進度條比什麼都不畫更糟 —— 它會停在某個數字然後不動。
    wchar_t buf[160];
    ::swprintf(buf, 160, UiText(UiString::kStatusRedeployRunning),
               static_cast<unsigned>(elapsed / 1000));
    SetStatus(buf);
    return;
  }
  ::KillTimer(hwnd_, kDeployTimer);
  deploying_ = false;
  ::EnableWindow(Ctl(hwnd_, IDC_REDEPLOY), TRUE);
  ::EnableWindow(Ctl(hwnd_, IDC_APPLY_ORDER), TRUE);
  if (status == 1) {
    wchar_t buf[200];
    ::swprintf(buf, 200, UiText(UiString::kStatusRedeployDone),
               UiText(deploy_why_), elapsed / 1000.0);
    SetStatus(buf);
    ::SetTimer(hwnd_, kStatusTimer, 4000, nullptr);
    has_rollback_ = false;
    ReloadSchemaList();
  } else {
    // 部署失敗時 rs_last_error() 常常是空字串 —— librime 的 C API 不給原因。
    // **不要假裝知道為什麼。**
    const std::string err = engine_->last_error();
    std::wstring msg = err.empty() ? UiText(UiString::kRedeployFailNoReason)
                                   : Utf8ToWide(err);
    // ⚠ 失敗時把 default.custom.yaml 整份還原。不還原的話,使用者會卡在
    //   「每次啟動都整理失敗」的狀態,而且沒有自救途徑。
    if (has_rollback_) {
      msg += L"\r\n\r\n";
      msg += store_->WriteDefaultCustom(rollback_yaml_)
                 ? UiText(UiString::kRedeployFailRolledBack)
                 : UiText(UiString::kRedeployFailRollbackFailed);
      has_rollback_ = false;
    }
    MessageDialog(hwnd_, &theme_, script(),
                  UiText(UiString::kRedeployFailTitle), msg,
                  UiText(UiString::kCancel));
    SetStatus(UiString::kStatusRedeployFailed);
  }
}

// ─────────────────────────── 事件 ───────────────────────────

void SettingsWindow::OnNotify(NMHDR* nm, LRESULT* result) {
  if (!nm) return;
  if (nm->idFrom == IDC_SIDEBAR) {
    if (nm->code == NM_CUSTOMDRAW) {
      *result = DrawSidebar(reinterpret_cast<NMLVCUSTOMDRAW*>(nm));
      return;
    }
    if (nm->code == LVN_ITEMCHANGED) {
      NMLISTVIEW* lv = reinterpret_cast<NMLISTVIEW*>(nm);
      if ((lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED))
        ShowPage(lv->iItem);
      return;
    }
  }
  if (nm->idFrom == IDC_SCHEMA_LIST && nm->code == NM_CUSTOMDRAW) {
    *result = DrawSchemaList(reinterpret_cast<NMLVCUSTOMDRAW*>(nm));
    return;
  }
}

void SettingsWindow::OnCommand(int id, int code) {
  // 單選鈕:BN_CLICKED 才算。少了這一層,設定時的 BM_SETCHECK 會被當成
  // 使用者按的,於是「載入設定」本身就會寫一次設定檔。
  const bool clicked = code == BN_CLICKED;

  if (id >= IDC_COUNT_0 && id <= IDC_COUNT_4 && clicked) {
    // ⚠ 這一項是 **A 層**:它就是 librime 的一頁候選數,要重新整理字詞
    //   才會變。只截掉畫面上的那幾個是不行的 —— 數字鍵仍然選得到
    //   看不見的那幾個。(而候選窗那一側的同一個錯誤這一輪已經修掉了,
    //   見 common/cand_layout.cc。)
    if (ConfirmDialog(hwnd_, &theme_, script(),
                      UiText(UiString::kApplyCountTitle),
                      UiText(UiString::kApplyCountBody),
                      UiText(UiString::kApplyCountConfirm),
                      UiText(UiString::kApplyCountLater))) {
      std::string err;
      if (!ApplyOrderAndPageSize(&err)) {
        UiString s = err == "unreadable" ? UiString::kOrderPatchUnreadable
                     : err == "invalid"  ? UiString::kOrderPatchInvalid
                     : err == "missing"  ? UiString::kOrderPatchMissing
                                         : UiString::kOrderPatchWriteFailed;
        MessageDialog(hwnd_, &theme_, script(),
                      UiText(UiString::kApplyCountTitle), UiText(s),
                      UiText(UiString::kCancel));
        return;
      }
      StartRedeploy(UiString::kApplyCountTitle);
    } else {
      SetStatus(UiString::kSchemasOrderChangedHint);
    }
    return;
  }
  if (id >= IDC_SCALE_0 && id <= IDC_SCALE_4 && clicked) {
    ApplyScaleNow();
    return;
  }
  if (id >= IDC_THEME_0 && id <= IDC_THEME_2 && clicked) {
    ApplyAppearancePref();
    return;
  }
  if (id >= IDC_VARIANT_0 && id <= IDC_VARIANT_2 && clicked) {
    ApplyVariantNow();
    return;
  }
  if (id >= IDC_PUNCT_0 && id <= IDC_PUNCT_2 && clicked) {
    ApplyPunctNow();
    return;
  }
  if (id >= IDC_LANG_0 && id <= IDC_LANG_3 && clicked) {
    ApplyUiLanguage();
    return;
  }

  switch (id) {
    case IDC_CLOSE:
    case IDCANCEL:
      ::ShowWindow(hwnd_, SW_HIDE);
      return;
    case IDC_UP:
    case IDC_DOWN: {
      const int sel = SelectedSchemaRow();
      const int n = static_cast<int>(order_.size());
      if (sel < 0 || sel >= n) return;
      const int to = (id == IDC_UP) ? sel - 1 : sel + 1;
      if (to < 0 || to >= n) return;
      std::swap(order_[sel], order_[to]);
      std::swap(schemas_[sel], schemas_[to]);
      if (schema_list_) {
        for (int i = 0; i < n; ++i) {
          LVITEMW it{};
          it.mask = LVIF_TEXT;
          it.iItem = i;
          std::wstring name = SchemaDisplayName(static_cast<size_t>(i));
          it.pszText = const_cast<wchar_t*>(name.c_str());
          ::SendMessageW(schema_list_, LVM_SETITEMW, 0,
                         reinterpret_cast<LPARAM>(&it));
        }
        LVITEMW st{};
        st.mask = LVIF_STATE;
        st.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        st.state = LVIS_SELECTED | LVIS_FOCUSED;
        ::SendMessageW(schema_list_, LVM_SETITEMSTATE, static_cast<WPARAM>(to),
                       reinterpret_cast<LPARAM>(&st));
        ::InvalidateRect(schema_list_, nullptr, TRUE);
      }
      SetStatus(UiString::kSchemasOrderChangedHint);
      return;
    }
    case IDC_APPLY_ORDER: {
      std::string err;
      if (!ApplyOrderAndPageSize(&err)) {
        UiString s = err == "unreadable" ? UiString::kOrderPatchUnreadable
                     : err == "invalid"  ? UiString::kOrderPatchInvalid
                     : err == "missing"  ? UiString::kOrderPatchMissing
                                         : UiString::kOrderPatchWriteFailed;
        MessageDialog(hwnd_, &theme_, script(),
                      UiText(UiString::kSchemasApplyOrder), UiText(s),
                      UiText(UiString::kCancel));
        SetStatus(UiString::kStatusOrderNotApplied);
        return;
      }
      // 順序決定了預設,所以「一律使用某一個」的舊設定要一起放掉 ——
      // 留著的話,使用者把另一個排到最前面卻看不到任何變化,
      // 而畫面上沒有任何東西解釋得了那件事。
      if (!settings_.Raw(keys::kSchemasPinnedGlobal).empty()) {
        settings_.SetPinnedGlobal(std::string());
        store_->Save(settings_);
      }
      StartRedeploy(UiString::kSchemasApplyOrder);
      return;
    }
    case IDC_FOLLOW_MODE: {
      const bool on = ::SendMessageW(Ctl(hwnd_, IDC_FOLLOW_MODE), BM_GETCHECK,
                                     0, 0) == BST_CHECKED;
      settings_.SetFollowInputMode(on);
      if (!store_->Save(settings_)) {
        SetStatus(UiString::kStatusSaveFailed);
        return;
      }
      // ⚠ 關掉「自動挑」時**連簡繁都不碰**:使用者要的是「我自己管」,
      //   半套更難理解。
      engine_->ApplyVariantAll(settings_.SchemaPref());
      SetTransientStatus(on ? UiString::kStatusFollowOn
                            : UiString::kStatusFollowOff);
      return;
    }
    case IDC_BAR_SHOW:
      ApplyStatusBarVisibility();
      return;
    case IDC_REDEPLOY:
      StartRedeploy(UiString::kRedeployButton);
      return;
    case IDC_RESET:
      DoResetSettings();
      return;
    case IDC_DIAG_COPY: {
      // 診斷要能被貼進回報。整段選起來再複製,不另外造一份字串 ——
      // 兩份會漂移,而漂移的症狀是「他貼給我的和他螢幕上的不一樣」。
      HWND e = Ctl(hwnd_, IDC_DIAG);
      if (!e) return;
      ::SendMessageW(e, EM_SETSEL, 0, -1);
      ::SendMessageW(e, WM_COPY, 0, 0);
      ::SendMessageW(e, EM_SETSEL, static_cast<WPARAM>(-1), 0);
      SetTransientStatus(UiString::kDiagnosticsCopied);
      return;
    }
    case IDC_OPEN_USER_DIR:
      ::ShellExecuteW(hwnd_, L"open", Utf8ToWide(store_->user_dir()).c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
      return;
    case IDC_OPEN_SETTINGS_FILE: {
      // 檔案可能還不存在(全部都是預設值時我們刻意不寫檔)。
      // 直接開一個不存在的檔案,記事本會說「找不到」——
      // 那對使用者是個沒有用的答案,所以先確保它存在。
      if (!store_->Save(settings_)) {
        SetStatus(UiString::kStatusSaveFailed);
        return;
      }
      ::ShellExecuteW(hwnd_, L"open", L"notepad.exe",
                      Utf8ToWide(store_->settings_path()).c_str(), nullptr,
                      SW_SHOWNORMAL);
      return;
    }
    default:
      return;
  }
}

// ─────────────────────────── 系統匣 ───────────────────────────

void SettingsWindow::AddTray() {
  if (tray_added_ || !hwnd_) return;
  if (taskbar_created_ == 0)
    taskbar_created_ = ::RegisterWindowMessageW(L"TaskbarCreated");
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd_;
  nid.uID = kTrayId;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = WM_RIME_TRAY;
  // ⚠ 沒有自己的圖示檔。用系統的應用程式圖示 —— 醜,但**看得到**,
  //   而看不到的圖示等於沒有這個入口。
  nid.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
  ::lstrcpynW(nid.szTip, UiText(UiString::kTrayTip), 128);
  tray_added_ = ::Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

void SettingsWindow::RemoveTray() {
  if (!tray_added_) return;
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd_;
  nid.uID = kTrayId;
  ::Shell_NotifyIconW(NIM_DELETE, &nid);
  tray_added_ = false;
}

void SettingsWindow::OnTray(WPARAM /*w*/, LPARAM l) {
  const UINT ev = static_cast<UINT>(LOWORD(l));
  if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK) {
    Open();
    return;
  }
  if (ev != WM_RBUTTONUP && ev != WM_CONTEXTMENU) return;

  HMENU menu = ::CreatePopupMenu();
  if (!menu) return;
  ::AppendMenuW(menu, MF_STRING, IDM_TRAY_SETTINGS,
                UiText(UiString::kTraySettings));
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  {
    // ⚠ 打勾用 MF_CHECKED 而不是自己畫。使用者要看得出**現在是哪一個**,
    //   而一個沒有狀態的選單就是「按了、切了、下次打開還是不知道自己在哪」。
    const VariantPref cur = CurrentVariantPref();
    const int ids[3] = {IDM_TRAY_VAR_FOLLOW, IDM_TRAY_VAR_HANT,
                        IDM_TRAY_VAR_HANS};
    for (int i = 0; i < kVariantCount; ++i)
      ::AppendMenuW(
          menu,
          MF_STRING | (kVariantOrder[i] == cur ? MF_CHECKED : MF_UNCHECKED),
          ids[i], UiText(kVariantLabels[i]));
  }
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, IDM_TRAY_REDEPLOY,
                UiText(UiString::kTrayRedeploy));
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, IDM_TRAY_QUIT, UiText(UiString::kTrayQuit));
  POINT pt{};
  ::GetCursorPos(&pt);
  // SetForegroundWindow 是必要的:少了它,使用者點到選單外面時選單不會關。
  ::SetForegroundWindow(hwnd_);
  const int cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x,
                                   pt.y, 0, hwnd_, nullptr);
  ::DestroyMenu(menu);
  ::PostMessageW(hwnd_, WM_NULL, 0, 0);

  if (cmd == IDM_TRAY_SETTINGS) {
    Open();
  } else if (cmd == IDM_TRAY_VAR_FOLLOW || cmd == IDM_TRAY_VAR_HANT ||
             cmd == IDM_TRAY_VAR_HANS) {
    // 已經在 UI 執行緒上,直接做。**不要**經過 PostMessage ——
    // 那會讓「按下去」與「生效」中間隔著一次訊息迴圈,
    // 而使用者立刻就會去打字驗證。
    CommitVariantPref(cmd == IDM_TRAY_VAR_FOLLOW ? VariantPref::kFollowInputMode
                      : cmd == IDM_TRAY_VAR_HANT ? VariantPref::kTraditional
                                                 : VariantPref::kSimplified);
  } else if (cmd == IDM_TRAY_REDEPLOY) {
    // 先把視窗叫出來:進度與結果都顯示在它上面,不然按下去真的會
    // 「什麼都沒發生」—— 而整理要十幾秒。
    ::SendMessageW(hwnd_, WM_RIME_OPEN, 0, 0);
    ShowPage(kPageAdvanced);
    StartRedeploy(UiString::kRedeployButton);
  } else if (cmd == IDM_TRAY_QUIT) {
    // §2-C3:確認鍵寫出它會做什麼(「現在結束它」),不是「是」。
    if (!ConfirmDialog(hwnd_, &theme_, script(),
                       UiText(UiString::kQuitServiceTitle),
                       UiText(UiString::kQuitServiceBody),
                       UiText(UiString::kQuitServiceConfirm),
                       UiText(UiString::kCancel)))
      return;
    // ⚠ 不可以直接結束進程:這支進程持有使用者自己加的詞的資料庫,
    //   從中途拔掉會壞,而症狀是「我學過的詞全沒了」。
    //   送具名事件,由 main.cc 那條路正常收尾。
    HANDLE ev2 = ::OpenEventW(EVENT_MODIFY_STATE, FALSE,
                              RimeServiceQuitEventName().c_str());
    if (ev2) {
      ::SetEvent(ev2);
      ::CloseHandle(ev2);
    }
  }
}

}  // namespace rimewin
