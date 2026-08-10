#include "settings_window.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>

#include "../common/net_ui.h"
#include "../common/schema_choice.h"
#include "../common/schema_list_patch.h"
#include "../common/ui_dip.h"
#include "../common/ui_layout.h"
#include "../winshared/winutil.h"
#include "cand_window.h"
#include "status_bar.h"
#include "ui_confirm.h"
#include "ui_listview.h"
#include "update_check.h"

namespace rimewin {
namespace {

constexpr wchar_t kClass[] = L"LuminaKeySettingsWindow";
constexpr UINT WM_RIME_OPEN = WM_APP + 1;
constexpr UINT WM_RIME_TRAY = WM_APP + 2;
constexpr UINT WM_RIME_SET_VARIANT = WM_APP + 3;
// 更新的工作執行緒做完了。⚠ 它只是「回來了」的訊號 ——
//   結果放在成員變數裡,而且只有 UI 執行緒讀得到。
constexpr UINT WM_RIME_UPDATE_DONE = WM_APP + 4;
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kDeployTimer = 1;
constexpr UINT_PTR kStatusTimer = 2;
// ⚠ 側欄底部那一行要自己更新(見 OnServiceStateTick)。
//   半秒問一次,問的是兩個 atomic,而且只有狀態真的變了才重畫。
constexpr UINT_PTR kServiceStateTimer = 3;
constexpr UINT kServiceStatePollMs = 500;

enum : int {
  IDM_TRAY_SETTINGS = 900,
  IDM_TRAY_REDEPLOY,
  IDM_TRAY_QUIT,
  IDM_TRAY_VAR_FOLLOW,
  IDM_TRAY_VAR_HANT,
  IDM_TRAY_VAR_HANS,
};

// ── 控制項 id ───────────────────────────────────────────────────
//
// ⚠ 它們**搬到 common/ui_layout.h 了**。理由不是整理:id 留在這裡的話,
//   每一頁的版面也只能留在這裡,而這個檔案在 Ubuntu 上編不起來 ——
//   於是「哪一顆控制項在哪裡」永遠沒有單元測試看得到。
//   外觀頁的深淺色三態排在視窗底部以外整整一輪沒有被發現,
//   就是因為 W18 量的是 ui_layout.cc 裡手工造的一份假骨架。

// ── ⚠ 這一張表就是「第六個看得到但摸不到」的結構性修法 ────────────
//
// 控制項的**建立**與**顯示**走同一份資料。舊版是兩份:一處 CreateWindow、
// 另一處是每頁的 id 陣列 —— 而 IDC_FOLLOW_MODE 只出現在前者,
// 於是它被建立了、有處理常式、有讀寫設定的完整程式碼,卻從來沒有被
// ShowWindow 過。畫面上根本沒有那顆核取方塊,而程式碼看起來完全正常。
//
// 現在「建了但不屬於任何一頁」在結構上不可能發生。
//
// ── 2026-08-10:那一格「哪一頁」也不在這裡了 ────────────────────
//
// 這張表原本自己帶一個 page 欄位,而**每一頁上有哪些控制項**因此有了
// 兩份來源:這裡一份、LayoutUi() 的 switch 一份。兩份會漂移,而漂移
// 的樣子是「頁上有一顆沒有被擺過位置的控制項,停在 (0,0) 10×10」。
//
// 現在頁的歸屬**只由 common/ui_layout.h 的 LayoutSettingsPageDip()
// 決定** —— 它擺得到就顯示,擺不到就隱藏。表上只剩「怎麼建」。
// 兩邊的 id 集合由 check_ui_spec.sh 的 W24 兩個方向比對(多一顆、
// 少一顆都紅),所以「建了但沒有頁」與「有頁但沒有建」都會被擋下。
struct ControlDef {
  int id;
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
    {IDC_STATUS, L"STATIC", ST, kNoText},
    {IDC_CLOSE, L"BUTTON", BTN, UiString::kClose},

    // ── 輸入方案 ──
    {IDC_SCHEMAS_TITLE, L"STATIC", ST, UiString::kSchemasTitle},
    {IDC_SCHEMAS_SUB, L"STATIC", ST, UiString::kSchemasSubtitle},
    {IDC_SCHEMAS_LIST_HEAD, L"STATIC", ST, UiString::kSchemasListHeading},
    {IDC_SCHEMAS_LIST_BLURB, L"STATIC", ST, UiString::kSchemasListBlurb},
    {IDC_SCHEMA_LIST, WC_LISTVIEWW,
     LVS_REPORT | LVS_SINGLESEL | LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS |
         WS_TABSTOP | WS_BORDER,
     kNoText},
    {IDC_UP, L"BUTTON", BTN, UiString::kSchemasMoveUp},
    {IDC_DOWN, L"BUTTON", BTN, UiString::kSchemasMoveDown},
    {IDC_APPLY_ORDER, L"BUTTON", BTN, UiString::kSchemasApplyOrder},
    {IDC_SCHEMAS_DEFAULT_LINE, L"STATIC", ST, kNoText},
    // §12.5.2:開關 = BUTTON + BS_AUTOCHECKBOX | BS_RIGHTBUTTON。
    // BS_RIGHTBUTTON 把方塊移到**右邊**,滿足 §4.1「開關在右、標題說明在左」。
    // ⚠ **不可以** owner-draw 它:BS_OWNERDRAW 與 BS_AUTOCHECKBOX 互斥
    //   (兩者都佔 BS_TYPEMASK 的低 4 位元),自繪之後螢幕閱讀器會念成
    //   「按鈕」而不是「核取方塊,已勾選」。
    {IDC_FOLLOW_MODE, L"BUTTON",
     BS_AUTOCHECKBOX | BS_RIGHTBUTTON | BS_MULTILINE | WS_TABSTOP,
     UiString::kSchemasFollowTitle},
    {IDC_FOLLOW_BLURB, L"STATIC", ST, UiString::kSchemasFollowBlurb},
    {IDC_SCHEMAS_EMPTY, L"STATIC", ST, kNoText},

    // ── 外觀 ──
    {IDC_APPEAR_TITLE, L"STATIC", ST, UiString::kAppearanceTitle},
    {IDC_APPEAR_SUB, L"STATIC", ST, UiString::kAppearanceSubtitle},
    {IDC_COUNT_HEAD, L"STATIC", ST, UiString::kCountHeading},
    {IDC_COUNT_BLURB, L"STATIC", ST, UiString::kCountBlurb},
    // ⚠ 「跟著○○」永遠是單選群組的第一格(§4.2)。
    {IDC_COUNT_0, L"BUTTON", RADIO1, UiString::kValueFollowSchema},
    {IDC_COUNT_1, L"BUTTON", RADIO, UiString::kCountThree},
    {IDC_COUNT_2, L"BUTTON", RADIO, UiString::kCountFive},
    {IDC_COUNT_3, L"BUTTON", RADIO, UiString::kCountSeven},
    {IDC_COUNT_4, L"BUTTON", RADIO, UiString::kCountNine},
    {IDC_SCALE_HEAD, L"STATIC", ST, UiString::kScaleHeading},
    {IDC_SCALE_BLURB, L"STATIC", ST, UiString::kScaleBlurb},
    {IDC_SCALE_0, L"BUTTON", RADIO1, UiString::kValueFollowSchema},
    {IDC_SCALE_1, L"BUTTON", RADIO, UiString::kScaleSmall},
    {IDC_SCALE_2, L"BUTTON", RADIO, UiString::kScaleNormal},
    {IDC_SCALE_3, L"BUTTON", RADIO, UiString::kScaleLarge},
    {IDC_SCALE_4, L"BUTTON", RADIO, UiString::kScaleHuge},
    {IDC_THEME_HEAD, L"STATIC", ST, UiString::kThemeHeading},
    {IDC_THEME_BLURB, L"STATIC", ST, UiString::kThemeBlurb},
    {IDC_THEME_0, L"BUTTON", RADIO1, UiString::kThemeFollowSystem},
    {IDC_THEME_1, L"BUTTON", RADIO, UiString::kThemeLight},
    {IDC_THEME_2, L"BUTTON", RADIO, UiString::kThemeDark},
    {IDC_BAR_HEAD, L"STATIC", ST, UiString::kStatusBarHeading},
    {IDC_BAR_BLURB, L"STATIC", ST, UiString::kStatusBarBlurb},
    {IDC_BAR_SHOW, L"BUTTON",
     BS_AUTOCHECKBOX | BS_RIGHTBUTTON | WS_TABSTOP, UiString::kStatusBarShow},
    {IDC_APPEAR_NOTE, L"STATIC", ST, UiString::kAppearanceHonestNote},

    // ── 文字 ──
    {IDC_TEXT_TITLE, L"STATIC", ST, UiString::kTextTitle},
    {IDC_TEXT_SUB, L"STATIC", ST, UiString::kTextSubtitle},
    {IDC_VARIANT_HEAD, L"STATIC", ST, UiString::kVariantHeading},
    {IDC_VARIANT_BLURB, L"STATIC", ST, UiString::kVariantBlurb},
    {IDC_VARIANT_0, L"BUTTON", RADIO1, UiString::kVariantFollow},
    {IDC_VARIANT_1, L"BUTTON", RADIO, UiString::kVariantTraditional},
    {IDC_VARIANT_2, L"BUTTON", RADIO, UiString::kVariantSimplified},
    {IDC_PUNCT_HEAD, L"STATIC", ST, UiString::kPunctHeading},
    {IDC_PUNCT_BLURB, L"STATIC", ST, UiString::kPunctBlurb},
    {IDC_PUNCT_0, L"BUTTON", RADIO1, UiString::kPunctFollow},
    {IDC_PUNCT_1, L"BUTTON", RADIO, UiString::kPunctChinese},
    {IDC_PUNCT_2, L"BUTTON", RADIO, UiString::kPunctEnglish},

    // ── 進階 ──
    {IDC_ADV_TITLE, L"STATIC", ST, UiString::kAdvancedTitle},
    {IDC_ADV_SUB, L"STATIC", ST, UiString::kAdvancedSubtitle},
    {IDC_REDEPLOY_HEAD, L"STATIC", ST, UiString::kRedeployHeading},
    {IDC_REDEPLOY_BLURB, L"STATIC", ST, UiString::kRedeployBlurb},
    {IDC_REDEPLOY, L"BUTTON", BTN, UiString::kRedeployButton},
    {IDC_FILES_HEAD, L"STATIC", ST, UiString::kFilesHeading},
    {IDC_FILES_BLURB, L"STATIC", ST, UiString::kFilesBlurb},
    {IDC_OPEN_USER_DIR, L"BUTTON", BTN, UiString::kOpenUserDir},
    {IDC_OPEN_SETTINGS_FILE, L"BUTTON", BTN, UiString::kOpenSettingsFile},
    {IDC_LANG_HEAD, L"STATIC", ST, UiString::kLanguageHeading},
    {IDC_LANG_BLURB, L"STATIC", ST, UiString::kLanguageBlurb},
    {IDC_LANG_0, L"BUTTON", RADIO1, UiString::kLanguageSystem},
    {IDC_LANG_1, L"BUTTON", RADIO, UiString::kLanguageEnglish},
    {IDC_LANG_2, L"BUTTON", RADIO, UiString::kLanguageHant},
    {IDC_LANG_3, L"BUTTON", RADIO, UiString::kLanguageHans},
    // ── 更新(這幾顆住在「連網」那一頁上) ──
    // ⚠ IDC_UPDATE_TRUST(沒有數位簽章那一句)是**永遠顯示**的靜態文字,
    //   不是錯誤時才冒出來的東西。版面上它排在按鈕之前。
    {IDC_UPDATE_HEAD, L"STATIC", ST, UiString::kUpdateHeading},
    {IDC_UPDATE_BLURB, L"STATIC", ST, UiString::kUpdateBlurb},
    {IDC_UPDATE_TRUST, L"STATIC", ST, UiString::kUpdateTrustAnchor},
    {IDC_UPDATE_WHAT, L"STATIC", ST, UiString::kUpdateWhatHappens},
    {IDC_UPDATE_STATUS, L"STATIC", ST, kNoText},
    {IDC_UPDATE_CHECK, L"BUTTON", BTN, UiString::kUpdateCheckButton},
    {IDC_UPDATE_ACTION, L"BUTTON", BTN, UiString::kUpdateInstallButton},
    {IDC_UPDATE_PAGE, L"BUTTON", BTN, UiString::kUpdateOpenPageButton},

    {IDC_DIAG_HEAD, L"STATIC", ST, UiString::kDiagnosticsHeading},
    {IDC_DIAG_NOTE, L"STATIC", ST, UiString::kDiagnosticsNote},
    // §12.5.2:唯讀資訊 = EDIT + ES_READONLY|ES_MULTILINE|WS_VSCROLL。
    // 可整段選取複製,那正是 §4.11 要的。
    {IDC_DIAG, L"EDIT",
     ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP |
         WS_BORDER,
     kNoText},
    {IDC_DIAG_COPY, L"BUTTON", BTN, UiString::kDiagnosticsCopy},
    {IDC_RESET_HEAD, L"STATIC", ST, UiString::kResetHeading},
    {IDC_RESET_BLURB, L"STATIC", ST, UiString::kResetBlurb},
    // ⚠ 危險鍵是這個檔案裡唯一 owner-draw 的**按鈕**。理由不是好看:
    //   啟用視覺樣式後 push button **不吃** WM_CTLCOLORBTN,文字色改不掉,
    //   而 §4.9 要的是「外框 + 危險色文字 + 透明底」。
    //   owner-draw 之後 MSAA 的角色仍然是 push button(它本來就沒有
    //   額外的狀態要維護),所以這一格是六類自繪裡最便宜的一格。
    {IDC_RESET, L"BUTTON", BS_OWNERDRAW | WS_TABSTOP,
     UiString::kResetButton},

    // ── 連網 ──
    {IDC_NET_TITLE, L"STATIC", ST, UiString::kNetworkTitle},
    {IDC_NET_SUB, L"STATIC", ST, UiString::kNetworkSubtitle},
    // §12.5.2:開關 = BUTTON + BS_AUTOCHECKBOX | BS_RIGHTBUTTON。
    // ⚠ 不可以 owner-draw(與 BS_AUTOCHECKBOX 互斥),見 IDC_FOLLOW_MODE。
    {IDC_NET_SWITCH, L"BUTTON",
     BS_AUTOCHECKBOX | BS_RIGHTBUTTON | WS_TABSTOP, UiString::kNetworkSwitch},
    // 開著/關著各一句,文字在執行期填(NetSwitchSummary 決定是哪一句)。
    {IDC_NET_STATE, L"STATIC", ST, kNoText},
    {IDC_NET_DETAIL, L"STATIC", ST, UiString::kNetworkOnDetail},
    {IDC_NETLOG_HEAD, L"STATIC", ST, UiString::kNetLogHeading},
    {IDC_NETLOG_BLURB, L"STATIC", ST, UiString::kNetLogBlurb},
    {IDC_NETLOG_SUMMARY, L"STATIC", ST, kNoText},
    {IDC_NETLOG_COLS, L"STATIC", ST, UiString::kNetLogColumns},
    {IDC_NETLOG_LIST, WC_LISTVIEWW,
     LVS_REPORT | LVS_SINGLESEL | LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS |
         WS_TABSTOP | WS_BORDER,
     kNoText},
    {IDC_NETLOG_EMPTY, L"STATIC", ST, kNoText},
    {IDC_NETLOG_PATH, L"STATIC", ST, kNoText},
    {IDC_NETLOG_CLEAR_HEAD, L"STATIC", ST, UiString::kNetLogClearHeading},
    {IDC_NETLOG_CLEAR_BLURB, L"STATIC", ST, UiString::kNetLogClearBlurb},
    // ⚠ 清除連網紀錄是**破壞性**的:清掉之後,使用者拿來稽核我們的那份
    //   證據就找不回來了。所以它與「把設定回復成預設」同一種樣子 ——
    //   owner-draw 的危險鍵(§4.9),而且排在該頁最後、隔一條分隔線。
    {IDC_NETLOG_CLEAR, L"BUTTON", BS_OWNERDRAW | WS_TABSTOP,
     UiString::kNetLogClearButton},
};
constexpr int kControlCount =
    static_cast<int>(sizeof(kControls) / sizeof(kControls[0]));

#undef ST
#undef BTN
#undef RADIO
#undef RADIO1

// ⚠ 側欄上的頁名**不在這裡**。它在 common/ui_layout.h 的
//   SettingsPageName() —— 一份與 SettingsPage 平行的陣列住在這個檔案裡的話,
//   順序錯開一格就是「側欄寫著『連網』,點下去出現的是進階頁」,
//   而那件事在 Ubuntu 上沒有任何東西看得到。

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

// 交給背景執行緒的那一份。⚠ 它**不帶** SettingsWindow* ——
// 視窗可能在檢查跑完之前就被關掉,而背景執行緒不可以碰一個可能已經
// 不在的物件。它只帶 HWND(PostMessage 對死掉的 HWND 是安全的失敗)
// 與 NetGate*(它活得跟服務進程一樣久)。
struct UpdateJob {
  HWND hwnd;
  NetGate* gate;
};

}  // namespace

// ────────────────────────────────────────────────────────────────

SettingsWindow::SettingsWindow(Engine* engine, SettingsStore* store,
                               const std::string& shared_dir)
    : engine_(engine),
      store_(store),
      shared_dir_(shared_dir),
      net_gate_(store),
      update_(&net_gate_, store, ModuleDirectory(nullptr)) {
  // ── 交棒之後的和解 ────────────────────────────────────────
  //
  // ⚠ 為什麼在**建構子**而不是視窗開起來的時候:安裝程式停掉的是整個
  //   服務進程,回來的時候使用者不一定會去開設定視窗。這裡讀一次
  //   交棒單,結果留著,等他下次打開設定就看得到。
  //   (真的沒有更新過的話,ReconcileHandoff 回 kNoHandoff,一個字都不說。)
  UpdateFailure why = UpdateFailure::kNone;
  std::string name;
  const UpdateOutcome out = update_.Reconcile(&why, &name);
  if (out == UpdateOutcome::kInstalled) {
    wchar_t buf[512];
    std::swprintf(buf, 512, UiText(UiString::kUpdateStatusInstalled),
                  Utf8ToWide(name).c_str());
    update_note_ = buf;
  } else if (out == UpdateOutcome::kNotInstalled) {
    // ⚠ 這裡分得出「檔案被鎖住」與「不知道為什麼」,而那兩句話不一樣:
    //   前者使用者做得到一件事(把握著檔案的程式關掉),後者只能回報。
    update_failure_ = why;
    update_note_ = UiText(UpdateFailureText(why));
  }
}

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
      // ⚠ WS_VSCROLL 不只是「能捲」:它是**唯一的提示**。少了那條捲軸,
      //   內容被切在底部那條 hairline 上,看起來像「這頁就這麼長」。
      // ⚠ WS_CLIPCHILDREN:沒有它,父視窗每一次重畫都會先把每一顆控制項
      //   底下那塊塗成背景色,子控制項再自己畫回去 —— 捲動時整頁在閃。
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
          WS_THICKFRAME | WS_VSCROLL | WS_CLIPCHILDREN,
      CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, wc.hInstance,
      this);
  if (ready_) ::SetEvent(ready_);
  if (!hwnd_) return;
  ::SetTimer(hwnd_, kServiceStateTimer, kServiceStatePollMs, nullptr);

  MSG msg;
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (::IsDialogMessageW(hwnd_, &msg)) {
      // ⚠ Win32 **不會**自動把鍵盤焦點捲進可見範圍。少了這兩行,
      //   Tab 走到視窗外的控制項時畫面完全不動 —— 焦點環在看不到的
      //   地方,使用者在盲按。
      if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN)
        EnsureFocusVisible();
    } else {
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
        self->RefreshNetworkAndUpdateCard();
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
          id == IDC_UPDATE_BLURB || id == IDC_UPDATE_TRUST ||
          id == IDC_UPDATE_WHAT || id == IDC_UPDATE_STATUS ||
          id == IDC_SCHEMAS_DEFAULT_LINE ||
          id == IDC_SCHEMAS_EMPTY ||
          id == IDC_NET_SUB || id == IDC_NET_STATE || id == IDC_NET_DETAIL ||
          id == IDC_NETLOG_BLURB ||
          id == IDC_NETLOG_SUMMARY || id == IDC_NETLOG_COLS ||
          id == IDC_NETLOG_EMPTY || id == IDC_NETLOG_PATH ||
          id == IDC_NETLOG_CLEAR_BLURB;
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
      if (self && di &&
          (di->CtlID == IDC_RESET || di->CtlID == IDC_NETLOG_CLEAR)) {
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
      // ⚠ AdjustWindowRectEx **不算捲軸**。不補這一格的話,最小尺寸下的
      //   client 會比 kWindowMinW 少 17 DIP,而內容欄的 440 下界會開始
      //   往左吃側欄(ContentXDip 在 W < 640 時回小於 200 的值)。
      r.right += ::GetSystemMetrics(SM_CXVSCROLL);
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
    // ── 捲動 ────────────────────────────────────────────────
    // ⚠ 這兩則以前**一則都沒有**,而外觀頁有 384 DIP 在視窗外面。
    //   子控制項(STATIC / BUTTON)不處理滾輪,DefWindowProc 會把
    //   WM_MOUSEWHEEL 轉給父視窗,所以接在這裡就夠。
    case WM_VSCROLL:
      if (self) self->OnVScroll(LOWORD(w), HIWORD(w));
      return 0;
    case WM_MOUSEWHEEL:
      if (self) self->OnMouseWheel(GET_WHEEL_DELTA_WPARAM(w));
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
      if (self && w == kServiceStateTimer) self->OnServiceStateTick();
      if (self && w == kDeployTimer) self->OnDeployTick();
      if (self && w == kStatusTimer) {
        ::KillTimer(hwnd, kStatusTimer);
        self->SetStatus(std::wstring());
      }
      return 0;
    case WM_RIME_UPDATE_DONE:
      if (self) self->OnUpdateWorkerDone();
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
  sidebar_ = CreateRowList(
      hwnd, IDC_SIDEBAR,
      WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_NOCOLUMNHEADER |
          LVS_SHOWSELALWAYS | LVS_NOSCROLL | WS_TABSTOP);
  if (sidebar_) {
    std::vector<std::wstring> pages;
    for (int i = 0; i < kPageCount; ++i)
      pages.push_back(UiText(SettingsPageName(i)));
    SetRowListItems(sidebar_, pages);
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
    if (d.id == IDC_NETLOG_LIST) net_log_list_ = h;
  }

  // ⚠ report 模式沒有欄的話,每一列的矩形都是空的 —— 清單裡有列而畫面
  //   是空白的。寬度在這裡給不準(此刻的 client 還是 10×10),
  //   真正的對齊在 LayoutUi() 的 place():控制項一改大小就跟著改。
  EnsureRowListColumn(schema_list_);
  // ⚠ 同一個理由:report 模式沒有欄的話,每一列的矩形都是空的 ——
  //   紀錄裡有列而畫面是一片空白,而那正是這一頁最不該出現的樣子。
  EnsureRowListColumn(net_log_list_);

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
  // ⚠ 列高與字型一起設,因為它們一起隨 DPI 變。
  //
  //   列高從來沒有被設定過 —— comctl32 依字型自己算,在 t3 下約 20 px。
  //   而版面(ui_layout.cc 的 SidebarListDip)是照 **一列 36 DIP** 算的,
  //   「預設」徽章的高度也是從列高扣出來的(列高 − 12),於是徽章只剩
  //   8 px 而字要 16 —— 使用者看到的是被切掉一半的那兩個字。
  //   一列 20 px 同時也低於 §3.6 的 28 最小點擊目標。
  //
  //   放在 ApplyFonts 裡是刻意的:這支函式在建立時、WM_DPICHANGED、
  //   換介面語言、換主題四條路上都會被呼叫,而列高在那四種情況下都要重算。
  //   放在建立處的話,換一次 DPI 就退回 comctl32 的預設值。
  const int row_px = Dip(metric::kSidebarItemH, dpi_);
  SetRowListRowHeight(sidebar_, row_px);
  SetRowListRowHeight(schema_list_, row_px);

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
                 IDC_NET_TITLE, IDC_ADV_TITLE})
    set(id, title);
  for (int id : {IDC_SCHEMAS_LIST_HEAD, IDC_COUNT_HEAD, IDC_SCALE_HEAD,
                 IDC_THEME_HEAD, IDC_BAR_HEAD, IDC_VARIANT_HEAD,
                 IDC_PUNCT_HEAD, IDC_REDEPLOY_HEAD, IDC_FILES_HEAD,
                 IDC_LANG_HEAD, IDC_DIAG_HEAD, IDC_RESET_HEAD,
                 IDC_UPDATE_HEAD, IDC_NETLOG_HEAD, IDC_NETLOG_CLEAR_HEAD})
    set(id, head);
  for (int id : {IDC_SCHEMAS_SUB, IDC_APPEAR_SUB, IDC_TEXT_SUB, IDC_ADV_SUB,
                 IDC_SCHEMAS_LIST_BLURB, IDC_FOLLOW_BLURB, IDC_COUNT_BLURB,
                 IDC_SCALE_BLURB, IDC_THEME_BLURB, IDC_BAR_BLURB,
                 IDC_APPEAR_NOTE, IDC_VARIANT_BLURB, IDC_PUNCT_BLURB,
                 IDC_REDEPLOY_BLURB, IDC_FILES_BLURB, IDC_LANG_BLURB,
                 IDC_DIAG_NOTE, IDC_RESET_BLURB, IDC_STATUS,
                 IDC_UPDATE_BLURB,
                 IDC_UPDATE_TRUST, IDC_UPDATE_WHAT, IDC_UPDATE_STATUS,
                 IDC_SCHEMAS_DEFAULT_LINE,
                 IDC_SCHEMAS_EMPTY,
                 IDC_NET_SUB, IDC_NET_STATE, IDC_NET_DETAIL,
                 IDC_NETLOG_BLURB, IDC_NETLOG_SUMMARY,
                 IDC_NETLOG_COLS, IDC_NETLOG_EMPTY, IDC_NETLOG_CLEAR_BLURB})
    set(id, small_f);
  set(IDC_DIAG, mono);
  // 紀錄檔的路徑是給人抄去查的,不是介面文案 —— 等寬(§4.11 的形狀)。
  set(IDC_NETLOG_PATH, mono);
  if (sidebar_)
    ::SendMessageW(sidebar_, WM_SETFONT, reinterpret_cast<WPARAM>(body), TRUE);
}

// ─────────────────────────── 版面 ───────────────────────────

void SettingsWindow::LayoutUi() {
  // ⚠ SetScrollInfo 讓捲軸出現/消失時會送 WM_SIZE,而 WM_SIZE 又叫這裡。
  if (!hwnd_ || in_layout_) return;
  in_layout_ = true;

  RECT rc{};
  ::GetClientRect(hwnd_, &rc);
  // 版面在 DIP 上算(純函式),最後才換成像素。
  int W = MulDivRound(rc.right - rc.left, 96, static_cast<int>(dpi_));
  int H = MulDivRound(rc.bottom - rc.top, 96, static_cast<int>(dpi_));

  // ── 內容區:整頁的版面由 common/ui_layout.cc 算 ────────────────
  //
  // ⚠ 這個函式**不得**自己算任何一個 y。以前它算,而它算出來的東西
  //   單元測試看不到 —— 外觀頁的深淺三態排在 574/604/634,
  //   可視高度 506,那三顆在畫面上不存在而 W18 全綠。
  //   check_ui_spec.sh 的 W24 會擋下任何一個回到這裡的 Stack。
  const PageLayout page_layout = LayoutSettingsPageDip(page_, W,
                                                      PageStateNow());
  int viewport_h = ContentViewportHeightDip(H);
  scroll_max_ = std::max(0, page_layout.content_h_dip - viewport_h);
  if (scroll_ > scroll_max_) scroll_ = scroll_max_;
  if (scroll_ < 0) scroll_ = 0;

  {
    // 捲軸要先設好:它出現或消失會改變 client 的寬度。
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = page_layout.content_h_dip > 0 ? page_layout.content_h_dip - 1 : 0;
    si.nPage = static_cast<UINT>(viewport_h > 0 ? viewport_h : 0);
    si.nPos = scroll_;
    // ⚠ 不加 SIF_DISABLENOSCROLL:內容放得下時捲軸要**消失**,
    //   一條永遠灰著的捲軸會被讀成「這頁還有東西,只是壞了」。
    ::SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
  }

  // 捲軸出現/消失之後 client 會變窄,重讀一次再擺。
  ::GetClientRect(hwnd_, &rc);
  W = MulDivRound(rc.right - rc.left, 96, static_cast<int>(dpi_));
  H = MulDivRound(rc.bottom - rc.top, 96, static_cast<int>(dpi_));
  viewport_h = ContentViewportHeightDip(H);
  const int dpi = static_cast<int>(dpi_);

  auto place = [&](int id, const RectI& r) {
    HWND c = Ctl(hwnd_, id);
    if (!c) return;
    ::SetWindowPos(c, nullptr, Dip(r.x, dpi), Dip(r.y, dpi), Dip(r.w, dpi),
                   Dip(r.h, dpi), SWP_NOZORDER);
    // ⚠ 單欄的 report 清單,欄寬**不會**跟著控制項走。不補這一句的話,
    //   建立時那個寬度會一直用下去,而使用者看到的是一個空白的清單。
    // ⚠ 兩個單欄清單都要。少一句的話,那個清單的欄寬會停在建立時的值,
    //   而使用者看到的是一個**空白的清單**(實機回報過一次了)。
    if (id == IDC_SCHEMA_LIST || id == IDC_NETLOG_LIST) SyncRowListColumn(c);
  };

  // 側欄:整條左邊,狀態區留在下面。
  // ⚠ 位置由 common/ui_layout.cc 算(純函式,單元測試看得到)。以前這裡
  //   自己算,而它算出來的清單**壓在底部狀態區上面 12 DIP** ——
  //   WS_CLIPCHILDREN 會把父視窗畫在那一塊的東西整個裁掉,
  //   於是「可以打字」那一行的上緣被切掉,使用者讀成「⼝以打字」。
  if (sidebar_) {
    const RectI list = SidebarListDip(H);
    ::SetWindowPos(sidebar_, nullptr, Dip(list.x, dpi), Dip(list.y, dpi),
                   Dip(list.w, dpi), Dip(list.h, dpi), SWP_NOZORDER);
    SyncRowListColumn(sidebar_);
  }

  const int cx = ContentXDip(W);
  const int cw = ContentWidthDip(W);

  // ⚠ -2 =「不知道現在裁到哪」,-1 =「確定沒有裁」。兩個混用的話,
  //   換 DPI 之後那些**應該解除裁切**的控制項會保留舊的區域,
  //   而症狀是「調了縮放之後有幾顆控制項被切掉一半」。
  if (static_cast<int>(clip_h_.size()) != kControlCount)
    clip_h_.assign(kControlCount, -2);

  for (int i = 0; i < kControlCount; ++i) {
    const int id = kControls[i].id;
    HWND c = Ctl(hwnd_, id);
    if (!c) continue;

    // 底部固定列:不捲動,永遠看得見。
    if (id == IDC_STATUS) {
      place(id, RectI{cx, H - kBottomBarH, cw - 120 - space::s3,
                      metric::kMinTarget});
      ClipToViewport(i, c, 0, -1);
      ::ShowWindow(c, SW_SHOW);
      continue;
    }
    if (id == IDC_CLOSE) {
      place(id, RectI{W - space::s7 - 100, H - kBottomBarH, 100,
                      metric::kMinTarget});
      ClipToViewport(i, c, 0, -1);
      ::ShowWindow(c, SW_SHOW);
      continue;
    }

    const PlacedControl* p = nullptr;
    for (const PlacedControl& q : page_layout.items)
      if (q.id == id) { p = &q; break; }
    if (!p || p->rect.empty()) {
      ::ShowWindow(c, SW_HIDE);
      continue;
    }
    // ── 捲動後的位置、裁切、顯示與否 ─────────────────
    //
    // ⚠ 這三件事的**決定權不在這裡**,在 common/ui_layout.cc 的
    //   ScrollPlaceControlDip()。理由是這個檔案在 Ubuntu 上編不起來,
    //   單元測試看不到它 —— 上一輪那行
    //   `const int y = p->rect.y - scroll_;` 就是寫在這裡,而把
    //   `- scroll_` 拿掉(捲軸拖得動、內容一動也不動)之後,
    //   206 個單元測試與守門腳本全綠。
    //
    //   現在這裡只做一件事:把算好的三個值接到 Win32 上。
    //   三條接線都不得寫死(y 不得是 p->rect.y、ShowWindow 的引數
    //   不得是字面的 SW_SHOW/SW_HIDE)—— check_ui_spec.sh 的 W25
    //   驗的就是這三條。
    const ScrolledPlacement sp =
        ScrollPlaceControlDip(p->rect, scroll_, viewport_h);
    place(id, RectI{p->rect.x, sp.y_dip, p->rect.w, p->rect.h});
    ClipToViewport(i, c, p->rect.w, sp.clip_h_dip);
    // 捲出可視範圍的控制項**不隱藏**,只裁成空的(sp.visible
    // 永遠是 true,而那是被單元測試釘住的規定)。隱藏會讓它退出
    // Tab 順序,於是鍵盤使用者再也捲不到它 —— 而捲動的存在正是
    // 為了讓那些控制項碰得到。
    ::ShowWindow(c, sp.visible ? SW_SHOW : SW_HIDE);
  }

  ::InvalidateRect(hwnd_, nullptr, TRUE);
  in_layout_ = false;
}

void SettingsWindow::ClipToViewport(int index, HWND c, int w_dip,
                                    int clip_h_dip) {
  // 裁到多高由 ScrollPlaceControlDip() 決定(而那一支有單元測試)。
  // 這裡只負責把它換成像素、並且**只在變動時才動**:
  // SetWindowRgn 會重畫,每次 LayoutUi 都無條件呼叫的話,捲動時整頁會閃。
  const int dpi = static_cast<int>(dpi_);
  const int visible = clip_h_dip < 0 ? -1 : clip_h_dip;  // -1 = 不裁
  if (index >= 0 && index < static_cast<int>(clip_h_.size())) {
    if (clip_h_[index] == visible) return;
    clip_h_[index] = visible;
  }
  if (visible < 0) {
    ::SetWindowRgn(c, nullptr, TRUE);
    return;
  }
  const int w_px = Dip(w_dip > 0 ? w_dip : 1, dpi);
  // CreateRectRgn 之後所有權交給視窗 —— **不可以**自己 DeleteObject。
  ::SetWindowRgn(c, ::CreateRectRgn(0, 0, w_px, Dip(visible, dpi)), TRUE);
}

// ── 捲動 ────────────────────────────────────────────────────────

void SettingsWindow::SetScroll(int dip) {
  if (dip > scroll_max_) dip = scroll_max_;
  if (dip < 0) dip = 0;
  if (dip == scroll_) return;
  scroll_ = dip;
  LayoutUi();
}

void SettingsWindow::OnMouseWheel(int delta) {
  UINT lines = 3;
  if (!::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0)) lines = 3;
  // 0 = 使用者把滾輪捲動關掉了,那是他的選擇。
  if (lines == 0) return;
  // WHEEL_PAGESCROLL(0xFFFFFFFF)= 一次一頁。
  const int line = text_size::t3 + space::s3;
  RECT rc{};
  ::GetClientRect(hwnd_, &rc);
  const int H = MulDivRound(rc.bottom - rc.top, 96, static_cast<int>(dpi_));
  const int step = lines > 100 ? std::max(1, ContentViewportHeightDip(H))
                               : static_cast<int>(lines) * line;
  SetScroll(scroll_ - delta * step / WHEEL_DELTA);
}

void SettingsWindow::OnVScroll(int code, int track_pos) {
  RECT rc{};
  ::GetClientRect(hwnd_, &rc);
  const int H = MulDivRound(rc.bottom - rc.top, 96, static_cast<int>(dpi_));
  const int page = std::max(1, ContentViewportHeightDip(H));
  const int line = text_size::t3 + space::s3;
  int v = scroll_;
  switch (code) {
    case SB_LINEUP: v -= line; break;
    case SB_LINEDOWN: v += line; break;
    case SB_PAGEUP: v -= page; break;
    case SB_PAGEDOWN: v += page; break;
    case SB_TOP: v = 0; break;
    case SB_BOTTOM: v = scroll_max_; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: {
      // ⚠ HIWORD 只有 16 位元 —— 內容超過 65535 DIP 時會截斷。
      //   走 SCROLLINFO 拿 32 位元的 nTrackPos。
      SCROLLINFO si{};
      si.cbSize = sizeof(si);
      si.fMask = SIF_TRACKPOS;
      v = ::GetScrollInfo(hwnd_, SB_VERT, &si) ? si.nTrackPos : track_pos;
      break;
    }
    default:
      return;
  }
  SetScroll(v);
}

void SettingsWindow::EnsureFocusVisible() {
  if (!hwnd_ || scroll_max_ <= 0) return;
  HWND f = ::GetFocus();
  if (!f || ::GetParent(f) != hwnd_) return;
  const int id = ::GetDlgCtrlID(f);
  RECT rc{};
  ::GetClientRect(hwnd_, &rc);
  const int W = MulDivRound(rc.right - rc.left, 96, static_cast<int>(dpi_));
  const int H = MulDivRound(rc.bottom - rc.top, 96, static_cast<int>(dpi_));
  const int viewport_h = ContentViewportHeightDip(H);
  const PageLayout pl = LayoutSettingsPageDip(page_, W, PageStateNow());
  for (const PlacedControl& p : pl.items) {
    if (p.id != id || p.rect.empty()) continue;
    if (p.rect.y < scroll_ + space::s3)
      SetScroll(p.rect.y - space::s3);
    else if (p.rect.bottom() > scroll_ + viewport_h - space::s3)
      SetScroll(p.rect.bottom() - viewport_h + space::s3);
    return;
  }
}

void SettingsWindow::ShowPage(int page) {
  if (page < 0 || page >= kPageCount) page = 0;
  page_ = page;
  // 換頁一律回到頂端。留著上一頁的捲動量,新的一頁會從半空中開始。
  scroll_ = 0;
  if (sidebar_) {
    LVITEMW it{};
    it.mask = LVIF_STATE;
    it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    it.state = LVIS_SELECTED | LVIS_FOCUSED;
    ::SendMessageW(sidebar_, LVM_SETITEMSTATE, static_cast<WPARAM>(page),
                   reinterpret_cast<LPARAM>(&it));
  }
  // ⚠ 這裡**不再**自己決定誰看得見。誰在這一頁上,由版面說了算 ——
  //   兩份來源會漂移,而漂移的樣子是一顆停在 (0,0) 的控制項。
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
      // ⚠ **不要直接用 cd->nmcd.rc** —— 見 ui_listview.h 的 RowRect()。
      //   拿不到矩形時交回控制項自己畫(item 上有文字),
      //   不是畫一片空白:一列看不見比一列不好看嚴重得多。
      RECT r{};
      if (!RowRect(sidebar_, cd, &r)) return CDRF_DODEFAULT;
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
      const wchar_t* label = UiText(SettingsPageName(i));
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
      // ⚠ 見 ui_listview.h 的 RowRect()。這一格就是使用者回報的
      //   「『啟用的方式』底下是一個空白的 list」。
      RECT r{};
      if (!RowRect(schema_list_, cd, &r)) return CDRF_DODEFAULT;
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

LRESULT SettingsWindow::DrawNetLogList(NMLVCUSTOMDRAW* cd) {
  switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
      return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
      const size_t i = static_cast<size_t>(cd->nmcd.dwItemSpec);
      const bool selected = (cd->nmcd.uItemState & CDIS_SELECTED) != 0;
      const bool hot = (cd->nmcd.uItemState & CDIS_HOT) != 0;
      const bool focused =
          show_focus_ && (cd->nmcd.uItemState & CDIS_FOCUS) != 0;

      HDC hdc = cd->nmcd.hdc;
      // ⚠ 見 ui_listview.h 的 RowRect():report 模式下 nmcd.rc 是
      //   (0,0,0,0),拿它去畫的結果是一片空白而且沒有任何錯誤。
      RECT r{};
      if (!RowRect(net_log_list_, cd, &r)) return CDRF_DODEFAULT;
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
      ::SetTextColor(hdc, theme_.Color(kOnSurface));
      // 等寬:四欄靠對齊才讀得出來。
      HGDIOBJ oldf =
          ::SelectObject(hdc, fonts_.Get(text_size::t6, false, FontRole::kMono));
      RECT tr = r;
      tr.left += Dip(space::s4, dpi_);
      tr.right -= Dip(space::s4, dpi_);
      // ⚠ 畫的是與 SetRowListItems 餵進去的**同一份**字串。另外拼一份的話,
      //   螢幕閱讀器念的與畫面上的會不一樣。
      const wchar_t* line =
          i < net_log_lines_.size() ? net_log_lines_[i].c_str() : L"";
      ::DrawTextW(hdc, line, -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
      ::SelectObject(hdc, oldf);
      return CDRF_SKIPDEFAULT;
    }
    default:
      return CDRF_DODEFAULT;
  }
}

// ───────────────────────── 連網那一頁 ─────────────────────────

PageState SettingsWindow::PageStateNow() const {
  PageState s;
  s.schema_list_empty = order_.empty();
  // ⚠ 這一格**一定要**從真的紀錄來(net_log_empty_ 由 RefreshNetworkPage
  //   從 NetGate::ReadLog() 填)。寫死 false 的話,一次都沒有連過的使用者
  //   看到的是一個空表格加一顆清除鍵 —— 而「開關從沒開過所以紀錄是空的」
  //   正是使用者驗證我們的方式,那句話必須在畫面上說得出來。
  s.net_log_empty = net_log_empty_;
  return s;
}

void SettingsWindow::RefreshNetworkPage() {
  if (!hwnd_) return;

  // ⚠ 開關的真相只有一個來源:NetGate。設定視窗不自己讀 settings_ ——
  //   兩份真相會漂移,而漂移的樣子是「開關看起來開了,按下去卻說被擋下」。
  const bool on = net_gate_.Enabled();
  HWND sw = Ctl(hwnd_, IDC_NET_SWITCH);
  if (sw) ::SendMessageW(sw, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
  // 開著一句、關著一句。哪一句由純函式決定(common/net_ui.h),
  // 而「兩種狀態說同一句話」有單元測試擋著。
  SetText(hwnd_, IDC_NET_STATE, UiText(NetSwitchSummary(on)));

  // ⚠ 讀紀錄**不會**建立檔案(見 settings_store.h):「開關從沒開過 →
  //   紀錄檔根本不存在」與「檔案存在但是空的」對稽核的人不是同一句話。
  const NetLogView view =
      BuildNetLogView(net_gate_.ReadLog(), ui_lang_, LocalTzOffsetMinutes());
  net_log_empty_ = view.empty;
  net_log_lines_.clear();
  for (const NetLogRow& row : view.rows) net_log_lines_.push_back(row.line);
  if (net_log_list_) SetRowListItems(net_log_list_, net_log_lines_);

  if (view.empty) {
    // §4.7 的空狀態:為什麼是空的、這是不是正常。
    std::wstring text = view.summary;
    text += L"\r\n";
    text += UiText(UiString::kNetLogEmptyWhy);
    SetText(hwnd_, IDC_NETLOG_EMPTY, text.c_str());
  } else {
    SetText(hwnd_, IDC_NETLOG_SUMMARY, view.summary.c_str());
  }
  {
    std::wstring line = UiText(UiString::kNetLogFileLine);
    line += Utf8ToWide(net_gate_.log_path());
    SetText(hwnd_, IDC_NETLOG_PATH, line.c_str());
  }
  // 空/不空會換掉整塊版面(清單 ↔ 說明、清除鍵在不在),所以要重排。
  LayoutUi();
}

void SettingsWindow::OnNetSwitchToggled() {
  HWND sw = Ctl(hwnd_, IDC_NET_SWITCH);
  const bool on =
      sw && ::SendMessageW(sw, BM_GETCHECK, 0, 0) == BST_CHECKED;
  // ⚠ **只能**走 NetGate::SetEnabled。這裡不碰 settings_ ——
  //   出口那一側每一跳都會重問 NetGate,兩邊必須是同一個值。
  if (!net_gate_.SetEnabled(on)) {
    // 安靜地失敗會變成「開關關了,重開又是開的」。撥回真正的狀態,
    // 不要在畫面上留一個假的開關。
    SetStatus(UiString::kStatusSaveFailed);
    RefreshNetworkPage();
    return;
  }
  // 這一份設定被別人(出口)改過了,重讀一次免得後面覆寫掉。
  settings_ = store_->Load();
  RefreshNetworkPage();
  SetTransientStatus(NetSwitchStatus(on));
}

void SettingsWindow::DoClearNetLog() {
  // §2-C3:確認鍵寫出它會做什麼(「清除連網紀錄」),不是「是」。
  // ⚠ 這一步一定要有確認:清掉之後,使用者拿來稽核我們的那份證據
  //   就找不回來了。
  if (!ConfirmDialog(hwnd_, &theme_, script(),
                     UiText(UiString::kNetLogClearHeading),
                     UiText(UiString::kNetLogClearBlurb),
                     UiText(UiString::kNetLogClearButton),
                     UiText(UiString::kCancel)))
    return;
  net_gate_.ClearLog();
  RefreshNetworkPage();
  SetTransientStatus(UiString::kNetLogCleared);
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
  // ⚠ 這兩行的矩形由 common/ui_layout.cc 算(純函式,單元測試看得到)。
  //   以前是在這裡算的,而算出來的行高剛好等於漢字的行高、一點餘裕
  //   都沒有,再加上側欄清單壓在狀態區上面 12 DIP —— 使用者實機看到的
  //   是兩行斷掉的字,第一行「可以打字」被讀成「⼝以打字」。
  const int Hdip = MulDivRound(H, 96, static_cast<int>(dpi_));
  const RectI l1 = SidebarStatusLineDip(Hdip, 0);
  const RectI l2 = SidebarStatusLineDip(Hdip, 1);
  RECT r1{Dip(l1.x, dpi_), Dip(l1.y, dpi_), Dip(l1.x + l1.w, dpi_),
          Dip(l1.y + l1.h, dpi_)};
  // ⚠ 這裡以前與那一橫犯同一個錯:一個布林,而「還在準備 /
  //   準備失敗 / 引擎不在」三種處境共用同一句紅字「輸入法沒有在跑」。
  //   第一種那句話是假的,而第一次安裝的人看到的就是它。
  const ServiceState state = SidebarServiceState();
  sidebar_state_ = state;
  ::SetTextColor(hdc, theme_.Color(StateIsFailure(state)
                                       ? kError
                                       : kOnSurfaceVariant));
  // DT_VCENTER:行高比字高多出來的那幾格留在上下兩側,不是全留在下面。
  ::DrawTextW(hdc, UiText(SidebarStatusTextFor(state)), -1, &r1,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
  RECT r2{Dip(l2.x, dpi_), Dip(l2.y, dpi_), Dip(l2.x + l2.w, dpi_),
          Dip(l2.y + l2.h, dpi_)};
  ::SetTextColor(hdc, theme_.Color(kOnSurfaceVariant));
  ::DrawTextW(hdc, UiText(UiString::kNavStatusOffline), -1, &r2,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
  ::SelectObject(hdc, oldf);

  // 底部狀態行上面那一條 hairline。
  // ⚠ 與 ContentViewportHeightDip() 用**同一個**常數:內容就是裁在
  //   這一條上,兩邊各寫一份的話,捲動時會露出半格。
  RECT bl{Dip(metric::kSidebarW, dpi_), H - Dip(kBottomStripH, dpi_), W,
          H - Dip(kBottomStripH, dpi_) + Dip(metric::kHairline, dpi_)};
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
  // 區域是像素單位的 —— DPI 換了就全部作廢,一律重算(見 clip_h_ 的說明)。
  clip_h_.assign(clip_h_.size(), -2);
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
    std::vector<std::wstring> rows;
    for (size_t i = 0; i < schemas_.size(); ++i)
      rows.push_back(SchemaDisplayName(i));
    SetRowListItems(schema_list_, rows);
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
  // ⚠ 連網那一頁**每次打開設定都要重讀**:開關可能被別的地方改過,
  //   而紀錄在使用者上一次看之後又長了幾筆。
  RefreshNetworkAndUpdateCard();
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

ServiceState SettingsWindow::SidebarServiceState() const {
  EngineFacts facts;
  facts.engine_present = engine_ != nullptr;
  facts.deploy_done = engine_ && engine_->deploy_done();
  facts.deploy_ok = engine_ && engine_->deploy_ok();
  // ⚠ 設定視窗收不到快照(那是狀態列的路),所以線路上那個旗標
  //   這裡拿不到。它在「按下重新整理字詞之後」那一段,而**那一段
  //   這個視窗自己知道**:底下那行狀態訊息正在數秒數。
  facts.engine_says_not_ready = deploying_;
  return ServiceStateOf(facts);
}

void SettingsWindow::OnServiceStateTick() {
  const ServiceState now = SidebarServiceState();
  if (now == sidebar_state_) return;
  sidebar_state_ = now;
  // ⚠ 只重畫側欄底部那一小塊。整頁 InvalidateRect 會讓一個開著的
  //   設定視窗每次狀態變動都閃一下,而這一行本來就只佔那麼大。
  RECT rc{};
  ::GetClientRect(hwnd_, &rc);
  RECT strip{0, rc.bottom - Dip(metric::kSidebarStatusH, dpi_),
             Dip(metric::kSidebarW, dpi_), rc.bottom};
  ::InvalidateRect(hwnd_, &strip, TRUE);
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
  if (nm->idFrom == IDC_NETLOG_LIST && nm->code == NM_CUSTOMDRAW) {
    *result = DrawNetLogList(reinterpret_cast<NMLVCUSTOMDRAW*>(nm));
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
    case IDC_NET_SWITCH:
      OnNetSwitchToggled();
      return;
    case IDC_NETLOG_CLEAR:
      DoClearNetLog();
      return;
    case IDC_REDEPLOY:
      StartRedeploy(UiString::kRedeployButton);
      return;
    case IDC_UPDATE_CHECK:
      StartUpdateCheck();
      return;
    case IDC_UPDATE_ACTION:
      // 一顆按鈕、兩下:還沒下載就下載並核對,核對過了才交棒。
      // 為什麼不是一下做完 —— 交棒等於這個視窗與輸入法會消失一下,
      // 那不該是一顆「下載」按鈕的副作用。
      if (update_stage_ == UpdateStage::kReady && update_.verified())
        DoUpdateHandOff();
      else
        StartUpdateDownload();
      return;
    case IDC_UPDATE_PAGE:
      OpenDownloadPage();
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

// ──────────────────── 連上網路 / 線上更新 ────────────────────
//
// ⚠ 這一整段**不碰任何網路 API**。它只呼叫 NetGate(整個 windows/ 底下
//   唯一開得了連線的地方)與 UpdateService。稽核腳本
//   windows/audit_offline_win.sh 守著這件事。

void SettingsWindow::OpenDownloadPage() {
  std::string url = update_.have_manifest() ? update_.latest().page_url
                                            : std::string();
  if (url.empty() && update_.have_manifest()) url = update_.latest().url;
  if (url.empty()) url = kWinUpdateManifestUrl;
  ::ShellExecuteW(hwnd_, L"open", Utf8ToWide(url).c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
}

DWORD WINAPI SettingsWindow::UpdateWorkerEntry(LPVOID p) {
  SettingsWindow* self = static_cast<SettingsWindow*>(p);
  UpdateFailure why = UpdateFailure::kNone;
  if (self->update_job_ == 1)
    self->update_.Check(&why);
  else
    self->update_.DownloadAndVerify(&why);
  self->update_failure_ = why;
  ::PostMessageW(self->hwnd_, WM_RIME_UPDATE_DONE, 0, 0);
  return 0;
}

void SettingsWindow::StartUpdateCheck() {
  if (update_job_ != 0) return;
  // 開關關著就**不要連**,而且要說 —— 他剛剛主動按了一顆按鈕,
  // 什麼都不發生會被當成壞掉。
  if (!net_gate_.Enabled()) {
    update_failure_ = UpdateFailure::kSwitchOff;
    update_stage_ = UpdateStage::kIdle;
    RefreshNetworkAndUpdateCard();
    return;
  }
  update_failure_ = UpdateFailure::kNone;
  update_stage_ = UpdateStage::kChecking;
  update_job_ = 1;
  RefreshNetworkAndUpdateCard();
  if (update_thread_) ::CloseHandle(update_thread_);
  update_thread_ = ::CreateThread(nullptr, 0, &UpdateWorkerEntry, this, 0,
                                  nullptr);
  if (!update_thread_) {
    update_job_ = 0;
    update_stage_ = UpdateStage::kIdle;
    update_failure_ = UpdateFailure::kHandoffFailed;
    RefreshNetworkAndUpdateCard();
  }
}

void SettingsWindow::StartUpdateDownload() {
  if (update_job_ != 0) return;
  if (!net_gate_.Enabled()) {
    update_failure_ = UpdateFailure::kSwitchOff;
    RefreshNetworkAndUpdateCard();
    return;
  }
  if (!update_.have_manifest()) {
    StartUpdateCheck();
    return;
  }
  update_failure_ = UpdateFailure::kNone;
  update_stage_ = UpdateStage::kDownloading;
  update_job_ = 2;
  RefreshNetworkAndUpdateCard();
  if (update_thread_) ::CloseHandle(update_thread_);
  update_thread_ = ::CreateThread(nullptr, 0, &UpdateWorkerEntry, this, 0,
                                  nullptr);
  if (!update_thread_) {
    update_job_ = 0;
    update_stage_ = UpdateStage::kIdle;
    update_failure_ = UpdateFailure::kHandoffFailed;
    RefreshNetworkAndUpdateCard();
  }
}

void SettingsWindow::OnUpdateWorkerDone() {
  const int job = update_job_;
  update_job_ = 0;
  if (update_failure_ != UpdateFailure::kNone) {
    update_stage_ = UpdateStage::kIdle;
  } else if (job == 2 && update_.verified()) {
    update_stage_ = UpdateStage::kReady;
  } else {
    update_stage_ = UpdateStage::kIdle;
  }
  RefreshNetworkAndUpdateCard();
}

void SettingsWindow::DoUpdateHandOff() {
  UpdateFailure why = UpdateFailure::kNone;
  // ⚠ deploying_ 就是「服務現在停不下來」那一格:交棒之後安裝程式
  //   第一件事就是停掉這個進程,而整理字詞做到一半被停掉會留下
  //   半份資料。使用者看到的會是一句「等它做完再更新」。
  if (!update_.HandOff(deploying_, &why)) {
    update_failure_ = why;
    RefreshNetworkAndUpdateCard();
    return;
  }
  update_failure_ = UpdateFailure::kNone;
  update_stage_ = UpdateStage::kHandedOff;
  RefreshNetworkAndUpdateCard();
}

void SettingsWindow::RefreshNetworkAndUpdateCard() {
  if (!hwnd_) return;

  // ⚠ 開關、狀態句、紀錄清單、空狀態**都在同一頁上**,而按下更新之後
  //   紀錄裡會多幾筆 —— 只重畫卡片的話,使用者得換頁再換回來才看得到
  //   那幾筆,而那幾筆正是這一頁存在的理由。
  RefreshNetworkPage();
  const bool on = net_gate_.Enabled();

  UpdateUiState st;
  st.stage = update_stage_;
  st.failure = update_failure_;
  st.network_enabled = on;
  st.have_manifest = update_.have_manifest();
  st.verdict = update_.verdict();
  st.app_id = update_.app_id_verdict();
  st.file_verified = update_.verified();
  st.installed_version_known = update_.installed().valid();
  const UpdateCard card = DescribeUpdateCard(st);

  // 狀態那一行。⚠ 帶格式符的三條各自補上它的值 —— 直接把 %s 畫上去
  // 是這個專案抓過的那種「看起來有做」。
  std::wstring text;
  if (card.status != UiString::kUiStringCount) {
    wchar_t buf[1024];
    if (card.status == UiString::kUpdateStatusAvailable) {
      std::swprintf(buf, 1024, UiText(card.status),
                    Utf8ToWide(update_.latest().version_name).c_str());
      text = buf;
    } else if (card.status == UiString::kUpdateStatusDownloading) {
      wchar_t mb[64];
      std::swprintf(mb, 64, L"%lld MB",
                    static_cast<long long>(update_.latest().size / (1024 * 1024)));
      std::swprintf(buf, 1024, UiText(card.status), mb);
      text = buf;
    } else {
      text = UiText(card.status);
    }
  }
  // 上一次交棒的結果比目前的狀態更值得說一次。
  if (!update_note_.empty()) {
    text = update_note_;
    update_note_.clear();
  }
  SetText(hwnd_, IDC_UPDATE_STATUS, text.c_str());

  HWND chk = Ctl(hwnd_, IDC_UPDATE_CHECK);
  if (chk) {
    ::EnableWindow(chk, card.show_check_button && on ? TRUE : FALSE);
    ::ShowWindow(chk, SW_SHOW);
  }
  HWND act = Ctl(hwnd_, IDC_UPDATE_ACTION);
  if (act) {
    if (card.has_action()) ::SetWindowTextW(act, UiText(card.action));
    // ⚠ 沒有動作時**停用**而不是隱藏:版面是靜態的(見 ui_layout.cc),
    //   藏起來會在頁上留一個洞,而使用者會以為是畫壞了。
    ::EnableWindow(act, card.has_action() ? TRUE : FALSE);
  }
  HWND pg = Ctl(hwnd_, IDC_UPDATE_PAGE);
  if (pg) ::EnableWindow(pg, card.show_page_button ? TRUE : FALSE);
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
