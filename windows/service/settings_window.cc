#include "settings_window.h"

#include "status_bar.h"  // BarModeGlyph:§8.12 那四個字面的唯一出口
#include "tray_icon.h"

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

namespace rimewin {
namespace {

constexpr wchar_t kClass[] = L"LuminaKeySettingsWindow";
constexpr UINT WM_RIME_OPEN = WM_APP + 1;
constexpr UINT WM_RIME_TRAY = WM_APP + 2;
constexpr UINT WM_RIME_SET_VARIANT = WM_APP + 3;
// 更新的工作執行緒做完了。⚠ 它只是「回來了」的訊號 ——
//   結果放在成員變數裡,而且只有 UI 執行緒讀得到。
constexpr UINT WM_RIME_UPDATE_DONE = WM_APP + 4;
// 引擎那一頭把方案清單問回來了(ReloadSchemaList 的非同步那一半)。
//
// ⚠ 它是從**引擎執行緒**用 PostMessageW 送過來的。PostMessageW 是
//   Win32 少數保證跨執行緒安全的呼叫,而且它不等 —— 這正是重點:
//   引擎那一頭不可以為了通知我們而卡住,而我們也不可以為了等它而卡住。
constexpr UINT WM_RIME_SCHEMAS_READY = WM_APP + 5;
// 「等這一則通知處理完之後再重排一次版面」。
//
// ⚠ 它存在的理由很窄:ShowPage 走在 LVN_ITEMCHANGED 途中時,直接
//   LayoutUi() 會同步對 sidebar_ 下 SetWindowPos + LVM_SETCOLUMNWIDTH,
//   而那是在 comctl32 更新自己選取範圍的中途重入同一顆控制項。
constexpr UINT WM_RIME_RELAYOUT = WM_APP + 6;

// 引擎把一件「套用」做完了(wParam = 第幾次送出,lParam = 成不成功)。
// ⚠ 由**工作者執行緒** PostMessage 過來,所以它只帶得動兩個純量;
//   任何要碰成員的事都在這一頭做。見 SettingsWindow::ApplyDoneNotifier。
constexpr UINT WM_RIME_APPLY_DONE = WM_APP + 7;
// 中英模式變了 —— 托盤那一格要重畫。
// ⚠ 一定要 Post 而不是直接呼叫:通知來源是那一橫的 UI 執行緒
//   (StatusBar::RefreshFromEngine),而 Shell_NotifyIcon 要在擁有那個
//   HWND 的執行緒上做。
// ⚠ 合併時從 WM_APP + 5 改成 + 8:win-next 那一側已經把 +5 / +6 / +7
//   用掉了(方案清單回來、延後重排版面、套用做完),而這四則訊息
//   **送到同一個 HWND**。撞號的話 WndProc 會把托盤重畫當成
//   「方案清單回來了」—— 而那個 switch 裡兩個一樣的 case 值
//   在 MSVC 上是編譯錯誤,在別處則是靜默地走錯分支。
constexpr UINT WM_RIME_TRAY_ICON = WM_APP + 8;
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kDeployTimer = 1;
constexpr UINT_PTR kStatusTimer = 2;
// ⚠ 側欄底部那一行要自己更新(見 OnServiceStateTick)。
//   半秒問一次,問的是兩個 atomic,而且只有狀態真的變了才重畫。
constexpr UINT_PTR kServiceStateTimer = 3;
// W2:等那支診斷程式跑完。半秒問一次,與狀態列那顆同一個節奏 ——
// 期間底下那一行要一直在動(「已經 N 秒」),不然按下去看起來像沒反應。
constexpr UINT_PTR kDoctorTimer = 4;
constexpr UINT kDoctorPollMs = 500;
constexpr UINT kServiceStatePollMs = 500;
// 引擎的同一件工作跑超過這麼久,就在底下那一行說出來。
//
// ⚠ 2000 毫秒不是保險起見挑的:量到的最慢的一件工作是「建 session」
//   442~753 毫秒(見 engine.h),所以低於一秒會在正常情況下亂叫。
//   而使用者按下一顆單選鈕之後願意等的上限遠低於此 —— 兩秒還沒動靜,
//   他要的已經不是「再等等」,是「到底有沒有生效」。
constexpr int64_t kEngineStallWarnMs = 2000;

enum : int {
  IDM_TRAY_SETTINGS = 900,
  IDM_TRAY_REDEPLOY,
  IDM_TRAY_QUIT,
  IDM_TRAY_VAR_FOLLOW,
  IDM_TRAY_VAR_HANT,
  IDM_TRAY_VAR_HANS,
  // 中英兩項。⚠ 它們與簡繁那三項**沒有關係**,走的是
  //   Engine::SetAsciiModeAll(),不碰 CommitVariantPref。
  IDM_TRAY_MODE_CN,
  IDM_TRAY_MODE_EN,
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

// 色票的 Rgb → GDI 的 COLORREF。⚠ RGB() 的參數順序與 Rgb 的欄位
//   順序一樣,但 COLORREF 內部是 BGR —— 自己拼位元的話會把紅藍
//   對調,而深色下那個錯誤看起來只像「顏色怪怪的」。
inline COLORREF RgbToColorRef(Rgb c) { return RGB(c.r, c.g, c.b); }

// 這一顆的**字**由我們畫嗎(§12.5.2 / §12.14.6.6)。
//
// ⚠ **這是唯一的一份名單,而且它涵蓋核取方塊與單選鈕兩種。**
//
//   啟用視覺樣式之後,BUTTON 這個 theme class 底下的核取方塊與單選鈕,
//   **字**是 uxtheme 畫的:`WM_CTLCOLOR*` 的 SetTextColor 對它沒有作用。
//   而它們的**底**是我們畫的(WM_CTLCOLORBTN 回卡片色)。一半我們的、
//   一半系統的,深色下的結果是近黑字壓在 #171B1D 的卡片上 = **1.21:1**,
//   比 kDisabledText 的 2.51:1 還低。
//
// ⚠ 上一輪只補了核取方塊,而**讀不到字的那三頁全是單選鈕**:
//   一次顯示幾個字(IDC_COUNT_*)、那個小窗的字大小(IDC_SCALE_*)、
//   打出繁體字還是簡體字(IDC_VARIANT_*)、這個視窗的語言(IDC_LANG_*)。
//   實機截圖量出來的三個點(p1/p2 的 y=205、p4 的 y=397)一個都不是
//   核取方塊。所以判準不是「是不是開關列」,是
//   **「它的底是我們畫的、字卻是系統畫的」** —— 而那正好等於
//   「BS_AUTOCHECKBOX 或 BS_AUTORADIOBUTTON」這一組。
//
// ⚠ push button 不在這裡,而那是有理由的、不是漏掉:push button 的
//   **底也是 uxtheme 畫的**(整顆按鈕都是它的),所以字與底一致、讀得到。
//   危險鍵是 BS_OWNERDRAW,字與底都是我們的。這三類把 kControls 蓋滿,
//   而 check_ui_spec.sh 的 W49 逐顆對帳:出現第四類就紅。
//
// ⚠ 每一顆 id 都**寫出來**,不用 `first + i` 的範圍判斷 —— 理由與
//   ui_layout.cc 的 radios() 一樣:守門逐字比對這裡與 kControls,
//   而範圍寫法讓一半的 id 在文字上不存在。
// 方塊(核取方塊/單選鈕的字形)佔掉的那一欄有多寬(DIP)。
// ⚠ 兩個數字不一樣的理由寫在 DrawRowButtonText 的檔頭 ——
//   一邊只要「夠寬」,另一邊必須「不多不少」。
constexpr int kGlyphColRightDip = 24;          // BS_RIGHTBUTTON:方塊在右
constexpr int kGlyphColLeftDip = space::s6;    // 預設:方塊在左(16)

bool WeDrawTheText(int id) {
  return
      // BS_AUTOCHECKBOX(開關列)
      id == IDC_FOLLOW_MODE || id == IDC_BAR_SHOW ||
      id == IDC_SHIFTTAP_SWITCH || id == IDC_NET_SWITCH ||
      // BS_AUTORADIOBUTTON(單選群組)
      id == IDC_COUNT_0 || id == IDC_COUNT_1 || id == IDC_COUNT_2 ||
      id == IDC_COUNT_3 || id == IDC_COUNT_4 ||
      id == IDC_SCALE_0 || id == IDC_SCALE_1 || id == IDC_SCALE_2 ||
      id == IDC_SCALE_3 || id == IDC_SCALE_4 ||
      id == IDC_THEME_0 || id == IDC_THEME_1 || id == IDC_THEME_2 ||
      id == IDC_VARIANT_0 || id == IDC_VARIANT_1 || id == IDC_VARIANT_2 ||
      id == IDC_PUNCT_0 || id == IDC_PUNCT_1 || id == IDC_PUNCT_2 ||
      id == IDC_SHAPE_0 || id == IDC_SHAPE_1 || id == IDC_SHAPE_2 ||
      id == IDC_LANG_0 || id == IDC_LANG_1 || id == IDC_LANG_2 ||
      id == IDC_LANG_3;
}

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
    {IDC_SHAPE_HEAD, L"STATIC", ST, UiString::kShapeHeading},
    {IDC_SHAPE_BLURB, L"STATIC", ST, UiString::kShapeBlurb},
    {IDC_SHAPE_0, L"BUTTON", RADIO1, UiString::kShapeFollow},
    {IDC_SHAPE_1, L"BUTTON", RADIO, UiString::kShapeHalf},
    {IDC_SHAPE_2, L"BUTTON", RADIO, UiString::kShapeFull},
    {IDC_SHIFTTAP_HEAD, L"STATIC", ST, UiString::kShiftTapHeading},
    {IDC_SHIFTTAP_BLURB, L"STATIC", ST, UiString::kShiftTapBlurb},
    // §12.5.2:開關 = BUTTON + BS_AUTOCHECKBOX | BS_RIGHTBUTTON。
    // ⚠ 不可以 owner-draw(與 BS_AUTOCHECKBOX 互斥),見 IDC_FOLLOW_MODE。
    {IDC_SHIFTTAP_SWITCH, L"BUTTON",
     BS_AUTOCHECKBOX | BS_RIGHTBUTTON | WS_TABSTOP, UiString::kShiftTapSwitch},

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
    // ⚠ W1:為什麼下面那幾顆是灰的。**這一顆的文字是固定的** ——
    //   它只在連網開關關著的時候被排進版面(見 ui_layout.cc),
    //   所以不必在執行期換句子。
    {IDC_UPDATE_GATE, L"STATIC", ST, UiString::kUpdateSwitchGate},
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
    // ⚠ W2:跑 `rime_ime_setup.exe doctor --report`。
    {IDC_DIAG_RUN, L"BUTTON", BTN, UiString::kDiagnosticsRunButton},
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

// ⚠ 只是把 kClass 讓外面問得到;27 行那個字面值不動(見標頭的說明)。
const wchar_t* SettingsWindowClassName() { return kClass; }

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
  // ── D:標題列左上角那一格(§12.14.6 沒有寫,因為沒有人想到它是空的)──
  //
  // ⚠ 不填 hIcon / hIconSm 的後果**不是**「沒有圖示」,是
  //   Win32 給一個系統預設的方塊 —— 而系統匣那一顆是我們自己畫的。
  //   同一支程式的兩個入口長得不一樣,而標題列那一格是使用者
  //   在工作列上、在 Alt-Tab 上、在視窗左上角看到的那一顆。
  //
  // ⚠ **拿同一份**:tray_icon.cc 的那一支。全 repo 沒有 .rc / .ico
  //   (見 tray_icon.h 的檔頭),所以「拿同一份」在這裡的意思是
  //   同一個繪製函式,不是同一個資源 id。
  //
  // ⚠ 字面固定用中文模式那一個,**不跟著中英模式變**:
  //   系統匣那一顆是狀態指示(它就是要跟著變),標題列那一顆是身分。
  //   跟著變的話,使用者在別的視窗打字時,設定視窗在工作列上的圖示
  //   會自己跳動。
  //
  // ⚠ 這兩顆**不 DestroyIcon**:視窗類別在整個進程生命週期裡
  //   註冊一次、不註銷,類別活多久它們就要活多久。放掉的樣子是
  //   「某一次重畫之後標題列的圖示變成空白」。
  wc.hIcon = MakeModeIconPx(BarModeGlyph(false), ::GetSystemMetrics(SM_CXICON));
  wc.hIconSm =
      MakeModeIconPx(BarModeGlyph(false), ::GetSystemMetrics(SM_CXSMICON));
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
          id == IDC_PUNCT_BLURB || id == IDC_SHAPE_BLURB ||
          id == IDC_REDEPLOY_BLURB ||
          id == IDC_FILES_BLURB || id == IDC_LANG_BLURB ||
          id == IDC_DIAG_NOTE || id == IDC_RESET_BLURB ||
          id == IDC_UPDATE_BLURB || id == IDC_UPDATE_TRUST ||
          id == IDC_UPDATE_WHAT || id == IDC_UPDATE_STATUS ||
          id == IDC_UPDATE_GATE ||
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
      // ── ⚠ 卡片裡的控制項要回 surface,不是 background ────────────
      //
      //   沒有這一格的話,卡片是 surface 色而卡片裡每一顆 STATIC 都帶著
      //   一塊 background 色的底 —— 畫面上是一張卡片上面浮著幾塊顏色
      //   不一樣的長方形,比沒有卡片還難看。
      //   「哪一顆在卡片裡」由 LayoutSettingsPageDip() 回報(in_card),
      //   **不在這裡判斷** —— 判斷在這裡的話它會與版面漂開。
      auto it = self->in_card_.find(id);
      const bool in_card = it != self->in_card_.end() && it->second;
      const Role bg = id == IDC_DIAG ? kControlFill
                                     : in_card ? kSurface : kBackground;
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
    // ⚠ §12.14.1 末段:**兩則都要接。**
    //   換 accent 時 WM_SETTINGCHANGE 不一定來;換深淺時這一則不一定來。
    //   只接一則的症狀是「換了顏色要重開設定視窗才會變」。
    case Theme::kDwmColorizationChanged:
      if (self) self->RefreshTheme();
      return 0;
    // ⚠ 危險鍵的 hover **不在這裡**。子控制項的滑鼠訊息不會走到父視窗,
    //   所以父視窗永遠收不到那兩顆按鈕的 WM_MOUSEMOVE / WM_MOUSELEAVE。
    //   追蹤在 DangerProc(子類別化)裡 —— 在這裡放一個 case 只會是
    //   一段永遠走不到、卻看起來在做事的碼。
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
    case WM_RIME_TRAY_ICON:
      if (self) self->RefreshTrayIcon(/*modify=*/true);
      return 0;
    case WM_RIME_TRAY:
      if (self) self->OnTray(w, l);
      return 0;
    case WM_RIME_OPEN:
      if (self) {
        self->ReloadFromSettings();
        if (w > 0) self->ShowPage(static_cast<int>(w) - 1);
        // ⚠ 最小化過的視窗,SW_SHOW **不會**把它還原 —— 它只是把一個
        //   已經在工作列上的圖示再「顯示」一次,畫面上什麼都不會發生。
        //   症狀與「按了設定沒反應」一模一樣,而且只在使用者最小化過一次
        //   之後才出現,所以很容易被當成偶發。
        //   (這不是 check_ui_spec.sh 的 W25 守的那一行。W25 守的是本檔案
        //    下面 LayoutUi() 尾端那一句:捲出可視範圍的控制項只裁成空的、
        //    **不隱藏**,所以它們留在 Tab 順序裡,鍵盤使用者捲得到。
        //
        //    ⚠ 那一句的原文刻意**不抄在這裡**。W25 的反向測試是「把那段
        //    字面值換掉」,而它只換**第一處** —— 在它前面抄一份,反向測試
        //    從此打在註解上,那道守門就空了而且完全看不出來。
        //    這一棵樹已經因為這件事紅過一次,別再抄第二次。)
        ::ShowWindow(hwnd, ::IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
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
      if (self && w == kDoctorTimer) self->OnDoctorTick();
      if (self && w == kStatusTimer) {
        ::KillTimer(hwnd, kStatusTimer);
        // ⚠ **只清自己那一則。** 這個計時器是為了收回 4 秒前那句成功
        //   訊息而設的,但它收的曾經是「那一刻畫面上的任何東西」——
        //   使用者在這 4 秒裡按了別的東西、拿到一行紅字,那行紅字會
        //   在他還沒讀完的時候消失,而且不留任何痕跡。
        if (self->status_line_.StillShowing(self->transient_ticket_)) {
          self->transient_ticket_ = StatusLine::kNone;
          self->SetStatus(std::wstring());
        }
      }
      return 0;
    case WM_RIME_UPDATE_DONE:
      if (self) self->OnUpdateWorkerDone();
      return 0;
    // ⚠ may_query = false:這一則**就是**那次查詢的結果。再排一次的話,
    //   引擎回空清單(暖機還沒完)時會變成無限的訊息迴圈。
    case WM_RIME_SCHEMAS_READY:
      if (self) self->ReloadSchemaList(false);
      return 0;
    case WM_RIME_RELAYOUT:
      if (self) self->LayoutUi();
      return 0;
    case WM_RIME_APPLY_DONE:
      if (self) self->OnApplyDone(static_cast<unsigned>(w), l != 0);
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
      // ⚠ 先把診斷那顆計時器與 handle 收掉。留著的話,計時器會打在一個
      //   正在拆的視窗上,而那支 handle 會漏到進程結束 ——
      //   服務是常駐的,「進程結束會清掉」在這裡不是一個答案。
      if (self) self->StopDoctorWatch();
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
    // 兩顆自繪危險鍵要自己追 hover(§12.14.6.4)。子控制項的滑鼠訊息
    // 不會走到父視窗,所以子類別化它們 —— 這是 Win32 追 hot 狀態的
    // 既有慣用法,不是我們的發明。
    if (h && (d.id == IDC_RESET || d.id == IDC_NETLOG_CLEAR))
      ::SetWindowSubclass(h, &SettingsWindow::DangerProc,
                          static_cast<UINT_PTR>(d.id),
                          reinterpret_cast<DWORD_PTR>(this));
  }

  // ⚠ report 模式沒有欄的話,每一列的矩形都是空的 —— 清單裡有列而畫面
  //   是空白的。寬度在這裡給不準(此刻的 client 還是 10×10),
  //   真正的對齊在 LayoutUi() 的 place():控制項一改大小就跟著改。
  EnsureRowListColumn(schema_list_);
  // ⚠ 同一個理由:report 模式沒有欄的話,每一列的矩形都是空的 ——
  //   紀錄裡有列而畫面是一片空白,而那正是這一頁最不該出現的樣子。
  EnsureRowListColumn(net_log_list_);
  // ⚠ 整列可點 + 雙緩衝。這兩顆是從 kControls 那張表建出來的,
  //   沒有走 CreateRowList,所以要自己補一次(側欄那一顆在 CreateRowList
  //   裡已經有了)。少了 FULLROWSELECT,使用者按在方案名字右邊的空白上
  //   選不到那一列 —— 而那裡看起來就是那一列。
  SetRowListExtendedStyle(schema_list_);
  SetRowListExtendedStyle(net_log_list_);

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
  const int row_px = Dip(metric::kRowH, dpi_);
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
                 IDC_PUNCT_HEAD, IDC_SHAPE_HEAD,
                 IDC_REDEPLOY_HEAD, IDC_FILES_HEAD,
                 IDC_LANG_HEAD, IDC_DIAG_HEAD, IDC_RESET_HEAD,
                 IDC_UPDATE_HEAD, IDC_NETLOG_HEAD, IDC_NETLOG_CLEAR_HEAD})
    set(id, head);
  for (int id : {IDC_SCHEMAS_SUB, IDC_APPEAR_SUB, IDC_TEXT_SUB, IDC_ADV_SUB,
                 IDC_SCHEMAS_LIST_BLURB, IDC_FOLLOW_BLURB, IDC_COUNT_BLURB,
                 IDC_SCALE_BLURB, IDC_THEME_BLURB, IDC_BAR_BLURB,
                 IDC_APPEAR_NOTE, IDC_VARIANT_BLURB, IDC_PUNCT_BLURB,
                 IDC_SHAPE_BLURB,
                 IDC_REDEPLOY_BLURB, IDC_FILES_BLURB, IDC_LANG_BLURB,
                 IDC_DIAG_NOTE, IDC_RESET_BLURB, IDC_STATUS,
                 IDC_UPDATE_BLURB,
                 IDC_UPDATE_TRUST, IDC_UPDATE_WHAT, IDC_UPDATE_STATUS,
                 IDC_UPDATE_GATE,
                 IDC_SCHEMAS_DEFAULT_LINE,
                 IDC_SCHEMAS_EMPTY,
                 IDC_NET_SUB, IDC_NET_STATE, IDC_NET_DETAIL,
                 IDC_NETLOG_BLURB, IDC_NETLOG_SUMMARY,
                 IDC_NETLOG_COLS, IDC_NETLOG_EMPTY, IDC_NETLOG_CLEAR_BLURB})
    set(id, small_f);
  // ── 按鈕文字是 t4(§12.14.3 的級距表)────────────────────────
  //
  // ⚠ 以前所有系統按鈕吃 body(t3 = 13),而自繪的危險鍵是 t4(12)——
  //   同一頁上兩種按鈕的字級不一樣,差 1 DIP 看不出是錯的,但整排按鈕
  //   放在一起就是「有一顆怪怪的」。
  {
    HFONT btn = fonts_.Get(text_size::t4);
    for (int id : {IDC_CLOSE, IDC_UP, IDC_DOWN, IDC_APPLY_ORDER,
                   IDC_UPDATE_CHECK, IDC_UPDATE_ACTION, IDC_UPDATE_PAGE,
                   IDC_REDEPLOY, IDC_OPEN_USER_DIR, IDC_OPEN_SETTINGS_FILE,
                   IDC_DIAG_COPY, IDC_DIAG_RUN, IDC_RESET, IDC_NETLOG_CLEAR})
      set(id, btn);
  }
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
  // 卡片與「哪一顆在卡片裡」都由版面回報。⚠ **這裡不算任何一個** ——
  //   自己算的那一份會與控制項的位置漂開,而漂開的樣子是
  //   「卡片的邊框從控制項中間穿過去」。
  cards_ = page_layout.cards;
  in_card_.clear();
  for (const PlacedControl& p : page_layout.items)
    in_card_[p.id] = p.in_card;
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
  // ⚠ 控制項裁在**裁切線**上,不是可視高度上。底下還有內容的時候
  //   兩者差 kScrollFadeH —— 那一段是淡出區,而淡出區裡不可以有字:
  //   字上面蓋一層淡出看起來像那一列被停用了(這是 §12.14.6.3 的
  //   「停用時必須有一句話說明為什麼」正好擋不住的那一種誤讀)。
  //   算式在 common/ui_layout.cc(純函式,單元測試看得到)——
  //   寫在這裡的話它又會變成一個沒有人量得到的數字。
  const int clip_line = ContentClipLineDip(H, scroll_, scroll_max_);
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
      // §12.14.6.8:狀態文字靠左,**離側欄右緣 s7** —— 不是內容欄的 x。
      // 內容欄是置中的,所以視窗一寬,那一行字就會跟著飄到中間,
      // 而它旁邊什麼都沒有,看起來像放錯位置。
      const int sx = metric::kSidebarW + space::s7;
      place(id, RectI{sx, H - kBottomBarH,
                      std::max(0, W - space::s7 - 100 - space::s3 - sx),
                      metric::kButtonH});
      ClipToViewport(i, c, 0, -1);
      ::ShowWindow(c, SW_SHOW);
      continue;
    }
    if (id == IDC_CLOSE) {
      // §12.14.6.8:寬 100、高 **32**(不是 28)。⚠ 它是**次要按鈕** ——
      // 這一頁的主要動作是使用者正在改的那一項,不是離開。
      place(id, RectI{W - space::s7 - 100, H - kBottomBarH, 100,
                      metric::kButtonH});
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
        ScrollPlaceControlDip(p->rect, scroll_, clip_line);
    // ⚠ **位置與尺寸都是 sp.rect,一個字都不自己拼。**
    //   2026-08-15 之前這裡寫的是
    //     place(id, RectI{p->rect.x, sp.y_dip, p->rect.w, p->rect.h});
    //   也就是「位置聽純函式的、尺寸自己給」。於是跨過裁切線的控制項
    //   仍然被擺成**滿尺寸**,只靠下面那一句 SetWindowRgn() 把它變不見
    //   —— 而區域不在版面模型裡,沒有任何純函式量得到它。實機截圖上
    //   量到的就是這個:外觀頁的「標準」那一列畫在 y=507..543,
    //   壓在底部固定列(H−48=512)上,把「關閉」鈕蓋掉只剩右緣十幾點。
    //   現在「不畫」是 sp.rect 的 w = h = 0,而空矩形畫不出東西是幾何。
    //   守門:test_ui_layout.cc 的
    //   nothing_is_ever_drawn_on_the_bottom_fixed_bar(五頁 × 每一個
    //   捲動位置 × 四種視窗尺寸)。
    place(id, sp.rect);
    // ⚠ SetWindowRgn 留著,而且是**第二道**:尺寸已經是 0 了,這一道
    //   只是不讓「有人把 sp.rect 換回滿尺寸」變成一次靜悄悄的回歸。
    ClipToViewport(i, c, sp.rect.w, sp.clip_h_dip);
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
  // ⚠ 先換主要按鈕,再擺版面。一個視窗只能有一顆 BS_DEFPUSHBUTTON,
  //   而它吃 Enter —— 留著上一頁那顆的話,使用者在這一頁按 Enter,
  //   被按下去的是**看不見的**那一顆(§12.14.6.2 末段:這是行為缺陷,
  //   不只是外觀)。
  ApplyDefaultButtonForPage(page_);
  // 換頁一律回到頂端。留著上一頁的捲動量,新的一頁會從半空中開始。
  scroll_ = 0;
  // ⚠ 先全清再設(見 ui_listview.h 的 SelectOnlyRow),而且用重入旗標
  //   擋住自己寫出來的 LVN_ITEMCHANGED —— 那一次全清會產生好幾則。
  if (sidebar_ && !in_show_page_) {
    in_show_page_ = true;
    SelectOnlyRow(sidebar_, page);
    in_show_page_ = false;
  }
  // 反白現在從 page_ 畫(見 DrawSidebar),所以換頁一定要重畫側欄。
  // FALSE = 不擦背景:每一列的自繪自己會先 FillRect,擦了只是多閃一次。
  if (sidebar_) ::InvalidateRect(sidebar_, nullptr, FALSE);
  // ⚠ 這裡**不再**自己決定誰看得見。誰在這一頁上,由版面說了算 ——
  //   兩份來源會漂移,而漂移的樣子是一顆停在 (0,0) 的控制項。
  //
  // ⚠ 但走在側欄通知途中的時候要**延後**重排:LayoutUi() 會對 sidebar_
  //   下 SetWindowPos + LVM_SETCOLUMNWIDTH,而此刻 comctl32 正在自己的
  //   LVM_SETITEMSTATE / 滑鼠處理裡面。排一則訊息,等它做完再擺。
  if (in_sidebar_notify_ && hwnd_)
    ::PostMessageW(hwnd_, WM_RIME_RELAYOUT, 0, 0);
  else
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

void SettingsWindow::FillRoundRect(HDC hdc, const RECT& r, int radius_px,
                                   Role fill, Role under) {
  // ⚠ **先把角外那塊填成底色**。§12.14.0 第 2 條:少了這一步,圓角只是
  //   一條畫在方塊裡面的弧,而那個方塊仍然是方的。
  ::FillRect(hdc, const_cast<RECT*>(&r), theme_.Brush(under));
  if (radius_px < 1) {
    ::FillRect(hdc, const_cast<RECT*>(&r), theme_.Brush(fill));
    return;
  }
  HGDIOBJ oldb = ::SelectObject(hdc, theme_.Brush(fill));
  // 外框與填色同色 —— RoundRect 會用目前的畫筆描邊,不換的話會描出
  // 一圈黑線(GDI 預設是 BLACK_PEN)。
  HGDIOBJ oldp = ::SelectObject(hdc, theme_.Pen(fill, 1));
  ::RoundRect(hdc, r.left, r.top, r.right, r.bottom, radius_px * 2,
              radius_px * 2);
  ::SelectObject(hdc, oldp);
  ::SelectObject(hdc, oldb);
}

void SettingsWindow::StrokeRoundRect(HDC hdc, const RECT& r, int radius_px,
                                     Role pen, int width_px) {
  HGDIOBJ oldp = ::SelectObject(hdc, theme_.Pen(pen, width_px));
  HGDIOBJ oldb = ::SelectObject(hdc, ::GetStockObject(NULL_BRUSH));
  ::RoundRect(hdc, r.left, r.top, r.right, r.bottom, radius_px * 2,
              radius_px * 2);
  ::SelectObject(hdc, oldb);
  ::SelectObject(hdc, oldp);
}

void SettingsWindow::DrawFocusRing(HDC hdc, const RECT& r, int radius_px) {
  // §12.6.4 第 2 條:**不要用 DrawFocusRect** —— 它是 XOR 的點線框,
  // 在我們的色票上會變成不可預測的顏色。
  //
  // §12.14.2 末段:兩圈互為反色。外圈 2 DIP kFocusOuter(淺色近黑 /
  // 深色純白)+ 內圈 1 DIP kFocusInner(相反)。焦點環的圓角 =
  // 元件圓角 + 2(§3.6)。
  const int outer = Dip(metric::kFocusRingW, dpi_);
  const int inner = Dip(metric::kHairline, dpi_);
  StrokeRoundRect(hdc, r, radius_px + Dip(2, dpi_), kFocusOuter, outer);
  RECT in = r;
  ::InflateRect(&in, -outer, -outer);
  if (in.right > in.left && in.bottom > in.top)
    StrokeRoundRect(hdc, in, radius_px, kFocusInner, inner);
}

void SettingsWindow::TrackDangerHover(HWND ctl, int id) {
  if (danger_hot_ == id) return;
  ClearDangerHover();
  danger_hot_ = id;
  danger_tracked_ = ctl;
  // WM_DRAWITEM 不給 hot 狀態,所以 hover 要自己追(§12.14.6.4)。
  // ⚠ 沒有 TME_LEAVE 的話,滑鼠移開之後那一顆會一直停在 hover ——
  //   而「按下去之前不知道自己指到了它」與「指開了還亮著」是同一種
  //   缺陷的兩半。
  TRACKMOUSEEVENT tme{};
  tme.cbSize = sizeof(tme);
  tme.dwFlags = TME_LEAVE;
  tme.hwndTrack = ctl;
  ::TrackMouseEvent(&tme);
  ::InvalidateRect(ctl, nullptr, TRUE);
}

void SettingsWindow::ClearDangerHover() {
  if (danger_hot_ < 0) return;
  HWND old = danger_tracked_;
  danger_hot_ = -1;
  danger_tracked_ = nullptr;
  if (old) ::InvalidateRect(old, nullptr, TRUE);
}

void SettingsWindow::ApplyDefaultButtonForPage(int page) {
  // §12.14.6.2:「主要按鈕」= 給它 BS_DEFPUSHBUTTON,**不要自繪**。
  // Win11 的 push button 在 BS_DEFPUSHBUTTON 下本來就是 accent 實心,
  // 而 manifest 已經在了(windows/res/app.manifest)。自繪它會拿走
  // §12.5.1 說的那兩樣東西,換到的只是「顏色跟我們算的一模一樣」。
  //
  // ⚠ **一個視窗只能有一顆**(它吃 Enter)。這是行為,不只是外觀:
  //   留著兩顆的話 Enter 會按到看不見的那一顆 —— 使用者在「連網」頁
  //   按 Enter,結果是「輸入方案」頁的「套用這個順序」被按下去。
  int want = 0;
  switch (page) {
    case kPageSchemas:
      want = IDC_APPLY_ORDER;
      break;
    case kPageAdvanced:
      want = IDC_REDEPLOY;
      break;
    default:
      // ⚠ 其餘三頁**刻意沒有**主要按鈕。§3.4 第 1 條「一頁最多一個彩色
      //   重點」的另一半是「沒有主要動作的頁就不要指一個」——
      //   外觀/文字頁上使用者改的是單選鈕,沒有一顆需要他按下去確認。
      want = 0;
      break;
  }
  if (want == default_button_) return;
  auto set_style = [&](int id, bool def) {
    HWND h = Ctl(hwnd_, id);
    if (!h) return;
    LONG st = ::GetWindowLongW(h, GWL_STYLE);
    const LONG next = def ? ((st & ~static_cast<LONG>(BS_TYPEMASK)) |
                             BS_DEFPUSHBUTTON)
                          : ((st & ~static_cast<LONG>(BS_TYPEMASK)) |
                             BS_PUSHBUTTON);
    if (next == st) return;
    ::SetWindowLongW(h, GWL_STYLE, next);
    ::InvalidateRect(h, nullptr, TRUE);
  };
  if (default_button_) set_style(default_button_, false);
  default_button_ = want;
  if (want) set_style(want, true);
  // ⚠ **不送 DM_SETDEFID。** 這是一個一般視窗類別,不是對話框 ——
  //   DM_SETDEFID 由 DefDlgProc 處理,而我們走的是 DefWindowProc,
  //   送過去等於什麼都沒做。真正決定 Enter 按到哪一顆的是訊息迴圈裡的
  //   IsDialogMessageW():它問不到 DM_GETDEFID 就去找**帶
  //   BS_DEFPUSHBUTTON 樣式的那一顆**。所以上面那一次 SetWindowLongW
  //   就是全部,而「一個視窗只能有一顆」也因此是真的行為約束,
  //   不只是外觀 —— 留著兩顆的話,IsDialogMessageW 找到的是**先建的**
  //   那一顆,也就是使用者這一頁上看不見的那一顆。
}

LRESULT SettingsWindow::DrawSidebar(NMLVCUSTOMDRAW* cd) {
  switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
      return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
      const int i = static_cast<int>(cd->nmcd.dwItemSpec);
      // ── #80:反白從 page_ 畫,**不從控制項的狀態畫** ────────────
      //
      // 側欄以前有兩份「現在是哪一頁」:page_ 驅動內容,comctl32 的
      // LVIS_SELECTED 驅動反白。兩份沒有任何地方對帳,分岔之後也沒有人
      // 會發現 —— 而 WM_CLOSE 只 SW_HIDE(視窗不銷毀),髒狀態跟著進程
      // 活著,關掉再開一模一樣。使用者截圖上是**兩列同時反白**。
      //
      // ⚠ 這一行**不去賭** NMCUSTOMDRAW::uItemState 對 ListView 的
      //   CDIS_SELECTED 準不準 —— 那件事讀原始碼讀不出來,而且不需要
      //   知道:畫面只要從 page_ 畫,分岔就不可能存在。這是把賭注拿掉,
      //   不是賭贏。
      //
      // ⚠ 另一條路(移除 LVS_SHOWSELALWAYS、整條側欄改成自己開矩形)
      //   **刻意不選**:自繪的矩形沒有 UIA 元素,方向鍵巡覽與螢幕閱讀器
      //   要自己補一份 provider(§12.5.1 的判準是無障礙,不是省事)。
      //   保留 ListView、只把「畫什麼」的來源收成一份,兩邊都拿得到。
      const bool selected = (i == page_);
      // ── ⚠ hover / pressed:**這裡沒有這兩個狀態,而且是故意的** ──
      //
      // §12.14.6.6 的表有四欄（一般 / 滑過 / 按下 / 停用）。這三個
      // ListView 上**只做得到兩欄**,理由寫在 ui_listview.h:comctl32 的
      // ListView 只在 LVS_EX_TRACKSELECT / LVS_EX_ONECLICKACTIVATE /
      // LVS_EX_TWOCLICKACTIVATE 或 SetWindowTheme(L"Explorer") 之下才會
      // 維護 hot item,而我們三者都沒有。
      //
      // 上一版讀了 `cd->nmcd.uItemState & CDIS_HOT`,並且用它同時算出
      // 「按下」（hot && 左鍵）。那兩條分支**永遠走不到** —— 也就是說
      // kRowHover / kRowPressed / kPrimaryContainerHover /
      // kPrimaryContainerPressed 四個角色在這條路上一次都沒有亮過,
      // 而報告上寫著四狀態 ✅。
      //
      // ⚠ 拿掉,不是接上。接上要 SetWindowTheme(L"Explorer"),而這台
      //   建置機沒有 Windows、沒有 wine —— 接上去只會把「死碼」換成
      //   「沒有人看過的碼」,那正是這條線已經燒掉三輪的形狀。
      //   要接的話先有辦法看到畫面（見 verify_installer.sh §12s 的截圖）,
      //   而截圖拍不到 hover（CI 上沒有滑鼠）。所以它是下一輪、
      //   而且要連同「怎麼驗」一起提。
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
      // ⚠ 底、圓角、指示條三者**從同一個純函式出**(SidebarItemFillDip /
      //   SidebarIndicatorDip)。以前這裡自己拿 RowRect 左右各縮 s5,而
      //   ui_layout.cc 另有一份 SidebarItemFillDip() —— 兩份會漂開,
      //   而漂開的樣子是「指示條與底色的圓角對不齊」。
      // ⚠ 三個矩形都用**同一個原點**換算:純函式給的是視窗座標,
      //   而 r 是 comctl32 給的那一列。原點取列的左上角,三者的相對位置
      //   就與純函式算的一模一樣 —— 各自從不同的地方換算是這一格最容易
      //   出錯的地方(第一版的指示條就因此往上跑了 2 DIP,而 2 DIP
      //   在畫面上看起來只是「有點沒對齊」)。
      const RectI row_dip = SidebarItemDip(i);
      const RectI fill_dip = SidebarItemFillDip(i);
      const RectI ind_dip = SidebarIndicatorDip(i);
      RECT item{r.left + Dip(fill_dip.x, dpi_),
                r.top + Dip(fill_dip.y - row_dip.y, dpi_),
                r.left + Dip(fill_dip.x + fill_dip.w, dpi_),
                r.top + Dip(fill_dip.y - row_dip.y + fill_dip.h, dpi_)};

      const int rad = Dip(radius::kControl, dpi_);
      ::FillRect(hdc, &r, theme_.Brush(kBackground));
      const Role bg = selected ? kPrimaryContainer : kBackground;
      if (bg != kBackground) FillRoundRect(hdc, item, rad, bg, kBackground);

      // 左緣指示條(§12.14.6.1)。**選中才有。**
      // ⚠ 它是「哪一頁」的第二個訊號(第一個是底色)—— §3.4 第 2 條:
      //   不得只用顏色傳達資訊。指示條是形狀,色覺障礙下也在。
      if (selected) {
        RECT ind{item.left, r.top + Dip(ind_dip.y - row_dip.y, dpi_),
                 item.left + Dip(ind_dip.w, dpi_),
                 r.top + Dip(ind_dip.y - row_dip.y + ind_dip.h, dpi_)};
        FillRoundRect(hdc, ind, Dip(2, dpi_), kAccentIndicator, bg);
      }

      if (focused) DrawFocusRing(hdc, item, rad);

      ::SetBkMode(hdc, TRANSPARENT);
      // ⚠ 只有「選中」會換文字色。滑過／按下那兩格拿掉了 ——
      //   見上面 hover / pressed 那一段。
      ::SetTextColor(hdc,
                     theme_.Color(selected ? kOnSurface : kOnSurfaceVariant));
      // ⚠ **選中不換字重**(§12.14.3 末段)。中文堆疊底下那是
      //   Regular ↔ Bold,字寬會變,切頁時側欄那幾行會互相跳。
      //   選中由底色 + 指示條 + 文字色表達,三個都不改變排版。
      HGDIOBJ oldf = ::SelectObject(hdc, fonts_.Get(text_size::t3, false));
      RECT tr = item;
      // 文字左緣 = 項目左緣 + s6(16),**選中與未選中一樣**。
      // 16 剛好讓文字清開 3 DIP 的指示條還有 13 DIP 的餘裕。
      tr.left += Dip(space::s6, dpi_);
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
      // ── #80 的孿生兄弟:反白從 schema_sel_ 畫,**不從控制項的狀態畫**
      //
      //   理由與 DrawSidebar 那一段逐字相同,只是換一顆控制項:
      //   這一行**不去賭** NMCUSTOMDRAW::uItemState 對 ListView 的
      //   CDIS_SELECTED 準不準 —— 畫面只要從我們自己的那一份畫,
      //   分岔就不可能存在。這是把賭注拿掉,不是賭贏。
      //
      //   ⚠ 上一輪只修了側欄。方案清單用的是同一組樣式
      //     (LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS)、同一個
      //     兩處裸 LVM_SETITEMSTATE 的形狀,所以「兩列同時反白」
      //     在這裡照樣會發生 —— 而使用者截圖指的正好是清單。
      const bool selected = (i == schema_sel_);
      // ⚠ 沒有 hover / pressed。理由與 DrawSidebar 那一段逐字相同
      //   （comctl32 不維護 hot item,那兩條分支永遠走不到）。
      const bool focused =
          show_focus_ && (cd->nmcd.uItemState & CDIS_FOCUS) != 0;

      HDC hdc = cd->nmcd.hdc;
      // ⚠ 見 ui_listview.h 的 RowRect()。這一格就是使用者回報的
      //   「『啟用的方式』底下是一個空白的 list」。
      RECT r{};
      if (!RowRect(schema_list_, cd, &r)) return CDRF_DODEFAULT;
      const int rad = Dip(radius::kControl, dpi_);
      const Role bg = selected ? kPrimaryContainer : kSurface;
      FillRoundRect(hdc, r, rad, bg, kSurface);

      if (focused) DrawFocusRing(hdc, r, rad);

      ::SetBkMode(hdc, TRANSPARENT);
      RECT tr = r;
      // §12.14.6.5:清單列左右內距 s5(12),不是 s4。
      tr.left += Dip(space::s5, dpi_);
      tr.right -= Dip(space::s5, dpi_);

      // 「預設」徽章:順序的第一個就是預設。⚠ 這一句是規範性的
      // (§6.7 第一層)—— 這一列上**不可以**出現方案 id,只有名字。
      // 舊版印的是「名字  (id)」,而 id 是引擎的內部識別字。
      if (i == 0) {
        const wchar_t* badge = UiText(UiString::kSchemasDefaultBadge);
        SIZE bs{};
        HGDIOBJ oldf2 = ::SelectObject(hdc, fonts_.Get(text_size::t5));
        ::GetTextExtentPoint32W(hdc, badge, ::lstrlenW(badge), &bs);
        // ── ⚠ 位置由純函式算(§12.14.6.5 / §12.14.0 第 3 條)──────
        //
        // 舊版把徽章的右緣釘在 tr.right,而 tr 來自 RowRect() ——
        // 那一支會把列寬**撐到 GetClientRect 的寬度**。清單帶 WS_BORDER
        // 而且沒有 LVS_NOSCROLL,所以項目一多就出現直捲軸:client 變窄、
        // 欄寬沒跟著變窄,於是那一列比看得見的範圍寬,徽章被排到可視區
        // 外面,被 DC 的裁剪切掉。使用者實機回報過。
        //
        // 現在算式吃的是**控制項自己的寬度**,而且明確扣掉捲軸寬。
        RECT lc{};
        ::GetClientRect(schema_list_, &lc);
        const int list_w_dip =
            MulDivRound(lc.right - lc.left, 96, static_cast<int>(dpi_));
        const bool has_vs =
            (::GetWindowLongW(schema_list_, GWL_STYLE) & WS_VSCROLL) != 0;
        const int sb_dip = MulDivRound(
            static_cast<int>(::GetSystemMetrics(SM_CXVSCROLL)), 96,
            static_cast<int>(dpi_));
        const BadgePlacement bp = BadgePlacementDip(
            list_w_dip, /*row_top_dip=*/0, metric::kRowH,
            MulDivRound(bs.cx, 96, static_cast<int>(dpi_)),
            metric::kHairline, sb_dip, has_vs);
        RECT br{r.left + Dip(bp.badge.x, dpi_),
                r.top + Dip(bp.badge.y, dpi_),
                r.left + Dip(bp.badge.right(), dpi_),
                r.top + Dip(bp.badge.bottom(), dpi_)};
        // 膠囊:圓角 = 高 ÷ 2。⚠ 外框**沒有** —— 試算過 1 DIP 外框對
        // 徽章底只有 1.24:1,過不了 §3.4.1 的 1.4。
        FillRoundRect(hdc, br, Dip(bp.radius_dip, dpi_), kBadgeFill, bg);
        ::SetTextColor(hdc, theme_.Color(kOnSurfaceVariant));
        ::DrawTextW(hdc, badge, -1, &br,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        ::SelectObject(hdc, oldf2);
        // 名稱的截尾點與徽章的左緣**由同一個算式產生**,所以長方案名
        // 不會壓到徽章上。
        tr.right = r.left + Dip(bp.name_right, dpi_);
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
      // ⚠ **不讀 NMCUSTOMDRAW::uItemState 的 CDIS_SELECTED。**
      //   側欄與方案清單早就不讀它了,而那時寫下的理由是
      //   「不去賭它準不準」。CI(Windows run #171)把答案量出來了:
      //   tests/test_win32_sidebar.cc 走一次真的自繪,控制項自己說
      //   被選的只有 1 列,而 uItemState **5 列裡說 5 列**都被選。
      //
      //   連網紀錄沒有第二份真相可以分岔,所以它不在 #80 的形狀裡 ——
      //   它壞的方式不一樣:照 uItemState 畫,**整份紀錄每一列都反白**。
      //   問控制項本人。
      const bool selected = RowIsSelected(net_log_list_, static_cast<int>(i));
      // ⚠ 沒有 hover / pressed。理由與 DrawSidebar 那一段逐字相同
      //   （comctl32 不維護 hot item,那兩條分支永遠走不到）。
      const bool focused =
          show_focus_ && (cd->nmcd.uItemState & CDIS_FOCUS) != 0;

      HDC hdc = cd->nmcd.hdc;
      // ⚠ 見 ui_listview.h 的 RowRect():report 模式下 nmcd.rc 是
      //   (0,0,0,0),拿它去畫的結果是一片空白而且沒有任何錯誤。
      RECT r{};
      if (!RowRect(net_log_list_, cd, &r)) return CDRF_DODEFAULT;
      const int rad = Dip(radius::kControl, dpi_);
      const Role bg = selected ? kPrimaryContainer : kSurface;
      FillRoundRect(hdc, r, rad, bg, kSurface);

      if (focused) DrawFocusRing(hdc, r, rad);

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
  // ⚠ 同一條理由:那一格說明的高度是從它自己的字算出來的,而「它自己的
  //   字」由 schema_note_ 決定。寫死一個 note 的話,另外兩種說法會被
  //   照第一種的高度裁掉 —— 而那正是 #76 的形狀。
  s.schema_note = schema_note_;
  // ⚠ W1:開關關著的時候,更新那一段上面多一句話說明那幾顆按鈕為什麼
  //   按不動。真相與別的地方一樣**只有 NetGate 一個來源** ——
  //   這裡不讀 settings_,兩份會漂移,而漂移的樣子是
  //   「開關看起來是開的,而畫面上還寫著『開關是關的』」。
  s.net_switch_off = !net_gate_.Enabled();
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
    RefreshNetworkAndUpdateCard();
    return;
  }
  // 這一份設定被別人(出口)改過了,重讀一次免得後面覆寫掉。
  settings_ = store_->Load();
  // ── ⚠ 這裡本來只叫 RefreshNetworkPage(),而那是一個真的缺陷 ────────
  //
  //   更新那三顆按鈕的 EnableWindow **只在** RefreshNetworkAndUpdateCard()
  //   裡下,而整個檔案裡叫得到它的地方全部在更新那一串動作上,以及
  //   WM_CREATE。也就是說:使用者把連網開關打開之後,那三顆**還是灰的**
  //   —— 要關掉設定視窗再打開一次才會亮。
  //
  //   在 W1 之前這件事沒有人看得出來(反正三顆本來就一直是灰的,而且
  //   畫面上沒有一句話說為什麼)。W1 補上那句話之後它就變成**說謊**:
  //   「把它打開,前兩顆就會亮」,而他打開了,那句話消失了,按鈕還是灰的
  //   —— 比原本更難懂,因為連解釋都跟著不見了。
  //
  //   RefreshNetworkAndUpdateCard() 是 RefreshNetworkPage() 的超集
  //   (它自己第一行就叫那一支),所以這裡改叫它,不會少做任何事。
  RefreshNetworkAndUpdateCard();
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

// ───────────────────── W2:從設定裡跑得到診斷 ─────────────────────
//
// ⚠ 跑的是**與「開始」功能表那個捷徑完全同一行**的命令:
//     {app}\rime_ime_setup.exe doctor --report
//   不另外拼一份參數。兩份會漂移,而漂移的症狀是「他貼給我們的東西
//   和捷徑產出來的不一樣」——而那正是這份報告存在的理由被抵消掉。
//
// ⚠ **不提權。** rime_ime_setup.exe 沒有 requestedExecutionLevel
//   (res/app.manifest 只掛在有視窗的三支上),所以這一下不會跳 UAC,
//   而診斷跑起來的身分就是使用者自己 —— 那是對的:提權之後看到的 HKCU
//   與具名管道都是**另一個帳號的**,報告自己第一段就在講這件事。
//
// ⚠ **這不是連網出口。** ShellExecuteEx 叫的是同目錄的一支本機執行檔。
void SettingsWindow::StartDoctorReport() {
  // 已經在跑就什麼都不做。按鈕那時是停用的,但鍵盤與螢幕閱讀器仍然
  // 送得進 BN_CLICKED —— 「按鈕看起來灰的」不是一道防線。
  if (doctor_proc_) return;

  const std::wstring exe = ModuleDirectory(nullptr) + L"\\rime_ime_setup.exe";
  SHELLEXECUTEINFOW ei{};
  ei.cbSize = sizeof(ei);
  // ⚠ SEE_MASK_NOCLOSEPROCESS:要拿到 hProcess 才問得出「跑完了沒」。
  //   不拿的話畫面上就只能寫一句「已經送出」然後永遠不更新 ——
  //   而使用者不會知道記事本什麼時候才會跳出來。
  ei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
  ei.hwnd = hwnd_;
  ei.lpVerb = L"open";
  ei.lpFile = exe.c_str();
  ei.lpParameters = L"doctor --report";
  // 主控台視窗不要跳出來:報告最後會自己用記事本打開,而一個黑框
  // 閃過去只會讓人以為出了事。
  ei.nShow = SW_HIDE;
  if (!::ShellExecuteExW(&ei) || !ei.hProcess) {
    // ⚠ 一句「失敗」對一個東西已經壞掉的人沒有用 —— 要說得出還有哪一條路。
    // ⚠ 而且走 SetStatus **不是** SetTransientStatus:失敗訊息不准 4 秒後
    //   自己消失,消失之後畫面上是一片空白,而空白跟「成功了」長得一樣。
    //   (check_ui_spec.sh 的 W35 就是守這一條 —— 第一版寫成 transient,
    //    被它當場攔下來。)
    SetStatus(UiString::kDiagnosticsRunFailed);
    return;
  }
  doctor_proc_ = ei.hProcess;
  doctor_start_ = ::GetTickCount();
  // ⚠ W23:停用的控制項,同一頁必須有一句說明為什麼。這裡那句話就是
  //   底下狀態行的「正在檢查…已經 N 秒」,而它一直在動。
  ::EnableWindow(Ctl(hwnd_, IDC_DIAG_RUN), FALSE);
  wchar_t buf[240];
  ::swprintf(buf, 240, UiText(UiString::kDiagnosticsRunning), 0u);
  SetStatus(buf);
  ::SetTimer(hwnd_, kDoctorTimer, kDoctorPollMs, nullptr);
}

void SettingsWindow::OnDoctorTick() {
  if (!doctor_proc_) return;
  const DWORD elapsed = ::GetTickCount() - doctor_start_;
  if (::WaitForSingleObject(doctor_proc_, 0) == WAIT_TIMEOUT) {
    // 不畫假的進度條 —— 它會停在某個數字然後不動,而那比什麼都不畫更糟。
    // 只說實話:已經跑了多久。(與「重新整理字詞」那一條同一個做法。)
    wchar_t buf[240];
    ::swprintf(buf, 240, UiText(UiString::kDiagnosticsRunning),
               static_cast<unsigned>(elapsed / 1000));
    SetStatus(buf);
    return;
  }
  StopDoctorWatch();
  // ⚠ 不看結束碼。doctor 的結束碼是**失敗的格數**,而「有幾格紅」正是
  //   使用者跑它的理由 —— 把它當成「這一下失敗了」會對一個剛剛成功
  //   拿到診斷的人說「失敗」。報告在記事本裡,結論寫在報告最後。
  SetTransientStatus(UiString::kDiagnosticsRunDone);
}

void SettingsWindow::StopDoctorWatch() {
  if (hwnd_) ::KillTimer(hwnd_, kDoctorTimer);
  if (doctor_proc_) {
    ::CloseHandle(doctor_proc_);
    doctor_proc_ = nullptr;
  }
  HWND b = Ctl(hwnd_, IDC_DIAG_RUN);
  if (b) ::EnableWindow(b, TRUE);
}

LRESULT CALLBACK SettingsWindow::DangerProc(HWND h, UINT m, WPARAM w,
                                           LPARAM l, UINT_PTR id,
                                           DWORD_PTR data) {
  SettingsWindow* self = reinterpret_cast<SettingsWindow*>(data);
  switch (m) {
    case WM_MOUSEMOVE:
      if (self) self->TrackDangerHover(h, static_cast<int>(id));
      break;
    case WM_MOUSELEAVE:
      if (self) self->ClearDangerHover();
      break;
    case WM_NCDESTROY:
      // ⚠ 一定要拿掉 —— 留著的話下一次視窗銷毀時會呼叫到已經不在的
      //   物件上。SetWindowSubclass 的契約就是這一條。
      ::RemoveWindowSubclass(h, &SettingsWindow::DangerProc, id);
      break;
    default:
      break;
  }
  return ::DefSubclassProc(h, m, w, l);
}

void SettingsWindow::DrawDangerButton(DRAWITEMSTRUCT* di) {
  // §4.9 / §2-C1:**外框 + 危險色文字 + 透明底**。不得用危險色實心底 ——
  // 實心的紅底看起來像「這是主要動作」,而它正好相反。
  HDC hdc = di->hDC;
  RECT r = di->rcItem;
  const bool pressed = (di->itemState & ODS_SELECTED) != 0;
  const bool disabled = (di->itemState & ODS_DISABLED) != 0;
  const bool focused = show_focus_ && (di->itemState & ODS_FOCUS) != 0;
  // ⚠ hover 現在**是真的 hover**:WM_DRAWITEM 不給 hot 狀態,所以由
  //   TrackDangerHover()(TME_LEAVE + WM_MOUSEMOVE)自己追。
  //   舊版拿按下狀態代替,少的那一階是「使用者按下去之前不知道自己
  //   指到了它」——§12.14.6.4:四個狀態就是四個狀態。
  const bool hot = danger_hot_ == static_cast<int>(di->CtlID);
  const int rad = Dip(radius::kControl, dpi_);
  // 底 = surface(等於卡片底,看起來透明)。⚠ **不得用危險色實心底**
  //   (§4.9):實心紅底看起來像「這是主要動作」,而它正好相反。
  const Role fill = disabled ? kSurface
                             : pressed ? kDangerPressed
                                       : hot ? kDangerHover : kSurface;
  FillRoundRect(hdc, r, rad, fill, kSurface);

  const Role fg = disabled ? kDisabledText : kError;
  StrokeRoundRect(hdc, r, rad, fg, Dip(metric::kHairline, dpi_));

  if (focused) DrawFocusRing(hdc, r, rad);

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

  // ── 卡片(§12.14.5)────────────────────────────────────────
  //
  // ⚠ 卡片畫在父視窗的 WM_PAINT 裡,所以不涉及無障礙(§12.5.3 末列
  //   「卡片／分隔線／空狀態／徽章」那一格已經涵蓋)。
  // ⚠ 矩形由 LayoutSettingsPageDip() 給,這裡只換算成像素。
  //   內容座標要減掉捲動量 —— 少了這一句,捲動時卡片會定在原地,
  //   而控制項會從卡片裡滑出去。
  {
    const int card_r = Dip(radius::kControl, dpi_);
    const int viewport_h = H - Dip(kBottomStripH, dpi_);
    // ── ⚠ 卡片也要裁在摺線上 ────────────────────────────────────
    //
    //   控制項由 ScrollPlaceControlDip() 裁(那一支有單元測試),
    //   而卡片是父視窗自己畫的 —— 沒有人裁它。捲到一半時,最後一張卡
    //   會**畫進底部固定列**,蓋在「關閉」鈕與狀態文字那一條上面。
    //   子控制項還在(它們是視窗,畫在最上層),所以症狀是
    //   「關閉鈕底下多了一塊白色的方塊」,而不是按鈕不見了 ——
    //   那種東西不會被回報成缺陷,只會被記成「這個設定畫面怪怪的」。
    ::IntersectClipRect(hdc, sb, 0, W, viewport_h);
    for (const CardRect& c : cards_) {
      RECT cr{Dip(c.rect.x, dpi_), Dip(c.rect.y - scroll_, dpi_),
              Dip(c.rect.x + c.rect.w, dpi_),
              Dip(c.rect.y - scroll_ + c.rect.h, dpi_)};
      if (cr.bottom <= 0 || cr.top >= viewport_h) continue;
      // 底 surface + 1 DIP controlBorder + 圓角 4。⚠ **沒有陰影** ——
      //   §3.5 第 3 條:卡片與底只差一階,靠 1px 分隔線切開,不靠陰影。
      //   深色下用陰影的結果是一坨黑,而 GDI 沒有模糊,自畫的陰影只能
      //   是幾條漸深的邊線 —— 那在 2026 年看起來比沒有陰影更舊。
      FillRoundRect(hdc, cr, card_r, kSurface, kBackground);
      StrokeRoundRect(hdc, cr, card_r, kControlBorder,
                      Dip(metric::kHairline, dpi_));
      // 卡內列與列之間的分隔線。⚠ **要縮排 s6** —— 不縮排的話它會碰到
      //   卡片的圓角,看起來像裂縫(§12.14.6.7)。
      for (int y : c.divider_ys) {
        const int py = Dip(y - scroll_, dpi_);
        if (py <= cr.top || py >= cr.bottom) continue;
        RECT dl{cr.left + Dip(space::s6, dpi_), py,
                cr.right - Dip(space::s6, dpi_),
                py + Dip(metric::kHairline, dpi_)};
        ::FillRect(hdc, &dl, theme_.Brush(kOutline));
      }
    }
    // ── 摺線上方那一段:淡出區(B)────────────────────────────
    //
    // ⚠ **這一段存在的理由是十張截圖上看得到的那件事。**
    //   摺線是一條**視窗內部**的硬邊:視窗邊緣把東西切斷沒有人覺得
    //   奇怪,視窗中間一條線把一張有 1 DIP 外框的圓角卡片切斷,
    //   看起來是畫錯了 —— 卡片的左右兩條外框直直撞上摺線就沒了,
    //   沒有下緣也沒有下圓角,空白比較多的那一側只剩一小截豎線。
    //   使用者的原話:「那不像『還有內容可以捲』,像畫錯了。」
    //
    // ⚠ 淡出是「還有更多」全世界都認得的訊號,而且它把那截
    //   沒畫完的外框一起吃掉。GDI 的 FillRect 不做 alpha
    //   (§12.14.2 開頭:狀態層是**預先算好的不透明色**),所以這是
    //   一疊實色橫帶,顏色由 common/ui_palette.cc 的 ScrollFadeMix()
    //   算 —— 純函式,單元測試看得到。
    //
    // ⚠ 實色行得通的**前提**是:淡出區裡只有卡片的底與外框,
    //   沒有字。那是 ContentClipLineDip() 保證的(控制項裁在淡出區的
    //   上緣)。少了那個前提,實色就變成「把一行字塗掉」。
    //
    // ⚠ 捲到底(或者根本不用捲)的時候 clip_line == vp,
    //   這一整段不執行 —— 下面沒有東西了還畫一層淡出等於騙人。
    {
      const int clip_line = ContentClipLineDip(Hdip, scroll_, scroll_max_);
      const int vp_dip = ContentViewportHeightDip(Hdip);
      const int bands = vp_dip - clip_line;
      if (bands > 0) {
        auto role_rgb = [&](Role r) {
          const COLORREF c = theme_.Color(r);
          return Rgb{GetRValue(c), GetGValue(c), GetBValue(c)};
        };
        const Rgb to = role_rgb(kBackground);
        const Rgb card_from = role_rgb(kSurface);
        const Rgb border_from = role_rgb(kControlBorder);
        const int hair = Dip(metric::kHairline, dpi_);
        // 先把整條抹成頁面底色 —— 卡片的外框與底都畫過了,
        // 沒有這一下的話卡片以外那幾格會留著上一次的東西。
        // ⚠ 左緣從側欄那條 hairline 的**右邊**開始。從 sb 開始的話
        //   會把那條分隔線在這 16 DIP 裡擦掉一截,而症狀是
        //   「側欄與內容之間那條線在靠近底部的地方斷了」。
        RECT strip{sb + hair, Dip(clip_line, dpi_), W, viewport_h};
        ::FillRect(hdc, &strip, theme_.Brush(kBackground));
        for (int i = 0; i < bands; ++i) {
          const int y0 = Dip(clip_line + i, dpi_);
          const int y1 = Dip(clip_line + i + 1, dpi_);
          if (y1 <= y0) continue;
          HBRUSH fill = ::CreateSolidBrush(
              RgbToColorRef(ScrollFadeMix(card_from, to, i, bands)));
          HBRUSH edge = ::CreateSolidBrush(
              RgbToColorRef(ScrollFadeMix(border_from, to, i, bands)));
          for (const CardRect& c : cards_) {
            const int top = c.rect.y - scroll_;
            const int bot = top + c.rect.h;
            if (clip_line + i < top || clip_line + i >= bot) continue;
            const int l = Dip(c.rect.x, dpi_);
            const int r = Dip(c.rect.x + c.rect.w, dpi_);
            RECT band{l, y0, r, y1};
            ::FillRect(hdc, &band, fill);
            RECT le{l, y0, l + hair, y1};
            RECT re{r - hair, y0, r, y1};
            ::FillRect(hdc, &le, edge);
            ::FillRect(hdc, &re, edge);
          }
          ::DeleteObject(fill);
          ::DeleteObject(edge);
        }
      }
    }
    // 裁切區還原 —— 底下那條 hairline 畫在摺線**上**,留著會被裁掉。
    ::SelectClipRgn(hdc, nullptr);
  }

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

// ── 方案清單的選取:**唯一**的寫入點 ────────────────────────────
//
// 與 ShowPage 對側欄做的事逐字相同(見那裡的說明):
//   ① 先寫我們自己的那一份(schema_sel_ / page_)
//   ② 再走 SelectOnlyRow —— 先全清再設,而且用重入旗標擋住自己寫出來的
//      LVN_ITEMCHANGED(那一次全清會同步產生好幾則)
//   ③ 最後重畫,因為反白是從我們自己的那一份畫的
//
// ⚠ 舊版**沒有這一支**:ReloadSchemaList 與 IDC_UP/IDC_DOWN 各自裸下一次
//   LVM_SETITEMSTATE,兩處都沒有先全清,而「哪一列被選」的答案要去問
//   comctl32(LVM_GETNEXTITEM)。也就是兩份真相 + 兩個寫入點 —— #80。
void SettingsWindow::SelectSchemaRow(int row) {
  const int n = static_cast<int>(schemas_.size());
  if (n <= 0)
    row = -1;  // 清單是空的:一列都不選,而不是「選第 0 列」
  else if (row < 0 || row >= n)
    row = 0;
  schema_sel_ = row;
  if (!schema_list_) return;
  if (!in_schema_select_) {
    in_schema_select_ = true;
    SelectOnlyRow(schema_list_, row);
    in_schema_select_ = false;
  }
  // FALSE = 不擦背景:每一列的自繪自己會先 FillRect,擦了只是多閃一次。
  ::InvalidateRect(schema_list_, nullptr, FALSE);
}

void SettingsWindow::ReloadSchemaList(bool may_query) {
  // ── ⚠ 這條路上不可以有任何**同步**的引擎呼叫 ────────────────
  //
  //   它掛在 WM_RIME_OPEN 上(→ ReloadFromSettings → 這裡),也就是
  //   「打開設定視窗」這條路。舊版是 `schemas_ = engine_->SchemaList();`,
  //   而那一支會同步等引擎執行緒 —— 引擎在忙(首次部署、一次慢的
  //   建 session)的時候,設定視窗的訊息迴圈就停在那裡。
  //
  //   那條執行緒同時是**系統匣圖示的擁有者**(AddTray 在 WM_CREATE 裡),
  //   所以它一停,連「重開設定」這條退路也一起沒了 —— 這與使用者說的
  //   「就卡死了」是同一個形狀(#79)。
  //
  //   現在:快取命中就直接畫(純記憶體);冷的話**非同步**問一次,
  //   回來用 WM_RIME_SCHEMAS_READY 叫自己重畫。冷快取那一瞬間畫面上
  //   是「輸入方案」頁的空狀態,而那句話說的正好是實話。
  //
  // ── ⚠ 清單是空的時候,**空的理由有三種**(#62)────────────────
  //
  //   快取有效        → 那就是答案(空也是答案):kSchemaNoteEmpty
  //   快取是冷的,查詢排進去了 → kSchemaNoteLoading(等一下就出來)
  //   查詢**排不進去**(引擎在停/沒有工作者)→ kSchemaNoteUnavailable
  //     ⚠ 這一種沒有人會回來。以前這裡把 RefreshSchemaListAsync 的
  //       回傳值丟掉,於是畫面上說的是「目前一種都沒有,到進階按重新
  //       整理字詞」—— 一句對這個情況完全沒有用的話,而且它永遠不會變。
  if (engine_->SchemaListFromCache(&schemas_)) {
    schema_note_ = kSchemaNoteEmpty;  // 有答案了(空的話就是真的空)
  } else if (may_query) {
    schemas_.clear();
    HWND h = hwnd_;
    // ⚠ 回傳 false = **沒有入列**,那個 lambda 永遠不會被呼叫。
    schema_note_ = engine_->RefreshSchemaListAsync([h] {
                     // ⚠ 這個 lambda 跑在**引擎執行緒**上。它只能做跨
                     //   執行緒安全的事,所以裡面只有 PostMessageW ——
                     //   不碰任何成員,那些屬於 UI 執行緒。
                     if (h) ::PostMessageW(h, WM_RIME_SCHEMAS_READY, 0, 0);
                   })
                       ? kSchemaNoteLoading
                       : kSchemaNoteUnavailable;
  } else {
    // 不准查(這一次只是重畫),而快取還是冷的 —— 上一次排的那件
    // 查詢還在飛。
    schemas_.clear();
    schema_note_ = kSchemaNoteLoading;
  }
  order_.clear();
  for (const auto& kv : schemas_) order_.push_back(kv.first);

  if (schema_list_) {
    std::vector<std::wstring> rows;
    for (size_t i = 0; i < schemas_.size(); ++i)
      rows.push_back(SchemaDisplayName(i));
    SetRowListItems(schema_list_, rows);
  }
  // ⚠ 換過內容之後一定要重設選取,而且要走那個唯一的寫入點:
  //   舊的 schema_sel_ 可能指到一列已經不存在的東西(重新整理字詞之後
  //   方案會少)。空清單 = -1,不是 0。
  SelectSchemaRow(schemas_.empty() ? -1 : 0);

  // ② 「現在預設是『注音 · 臺灣正體』」(§12.4.3)。
  if (!schemas_.empty()) {
    std::wstring line = UiText(UiString::kSchemasCurrentDefaultPrefix);
    line += L"「";
    line += SchemaDisplayName(0);
    line += L"」";
    SetText(hwnd_, IDC_SCHEMAS_DEFAULT_LINE, line.c_str());
  } else {
    // §4.7 的空狀態要說三件事:為什麼是空的、這是不是正常、下一步按哪裡。
    // ⚠ 那三句話從 **common/ui_layout.h 的 SchemaNoteLines()** 來,不在
    //   這裡挑。版面用同一支算那一格的高度 —— 兩邊各挑各的,高度就會
    //   與內容分家,而分家的方向是文字被切掉(#76 的形狀)。
    const SchemaNoteText note = SchemaNoteLines(schema_note_);
    std::wstring empty = UiText(note.title);
    empty += L"\r\n";
    empty += UiText(note.why);
    empty += L"\r\n";
    empty += UiText(note.next);
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

  // 全／半形(G70)。⚠ 三態的索引順序與 ApplyShapeNow 是同一份對照,
  //   對不上的症狀是「選了寬的,回來看是窄的」——Android 端踩過同一格。
  const Tri shape = settings_.Shape();
  CheckRadio(hwnd_, IDC_SHAPE_0, 3,
             shape == Tri::kUnset ? 0 : (shape == Tri::kFalse ? 1 : 2));

  // 輕點 Shift 切中英,預設**開**(業界慣例,理由見 common/settings.h)。
  // ⚠ 問的是 ShiftTapToggle() 而不是自己比一次 Tri —— 那個判斷只有一份。
  ::SendMessageW(Ctl(hwnd_, IDC_SHIFTTAP_SWITCH), BM_SETCHECK,
                 settings_.ShiftTapToggle() ? BST_CHECKED : BST_UNCHECKED, 0);

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
  // ⚠ 每一次寫都要記一筆。**收回**的那兩處(4 秒的計時器、心跳解除)
  //   靠它分辨「畫面上還是不是我寫的那一則」——
  //   判斷邏輯在 common/status_line.h,那裡有單元測試。
  status_line_.Write();
  SetText(hwnd_, IDC_STATUS, text.c_str());
}

void SettingsWindow::SetTransientStatus(UiString s) {
  // §12.5.3 末列:Win32 沒有 toast,而系統匣氣球使用者可以整個關掉
  // (等於這則訊息可能永遠不出現)。**不要做浮層** —— 用視窗底部
  // 已經有的那一行,4 秒後清掉。成功訊息不值得一個新表面。
  SetStatus(UiText(s));
  transient_ticket_ = status_line_.current();
  ::SetTimer(hwnd_, kStatusTimer, 4000, nullptr);
}

// ── 「已送出」→「已套用 / 套用失敗」 ────────────────────────────

unsigned SettingsWindow::BeginApply(UiString ok_status) {
  apply_ok_status_ = ok_status;
  ++apply_seq_;
  // ⚠ 這一句沒有 4 秒的計時器:它要一直留到引擎回來為止。
  //   反過來也要把上一輪留下來的計時器殺掉,不然它會在半路把
  //   「正在套用…」清成空白,而使用者看到的是「按了之後什麼都沒有」。
  ::KillTimer(hwnd_, kStatusTimer);
  transient_ticket_ = StatusLine::kNone;
  SetStatus(UiString::kStatusApplyQueued);
  return apply_seq_;
}

std::function<void(bool)> SettingsWindow::ApplyDoneNotifier(unsigned seq) {
  HWND h = hwnd_;
  // ⚠ **只捕捉傳值的純量。** 這一份會被引擎執行緒拿去跑,而那時這個
  //   物件的框可能早就不在了(見 common/work_queue.h 的檔頭)。
  //   裡面唯一做的事是 PostMessageW —— 所有 UI 狀態只在 UI 執行緒上動。
  return [h, seq](bool ok) {
    if (h) ::PostMessageW(h, WM_RIME_APPLY_DONE, static_cast<WPARAM>(seq),
                          ok ? 1 : 0);
  };
}

void SettingsWindow::OnApplyDone(unsigned seq, bool ok) {
  // ⚠ 連按三下的時候,前兩次的結果不可以寫進那一行:使用者現在關心的
  //   是最後那一下。舊的通知安靜地丟掉。
  if (seq != apply_seq_) return;
  if (!ok) {
    // ⚠ **失敗不走 transient。** 這曾經是整個檔案裡唯一一句會自己
    //   消失的失敗訊息 —— 其他每一句(kStatusSaveFailed ×10、
    //   kStatusRedeployFailed、kStatusOrderNotApplied)都走 SetStatus,
    //   寫上去就留著。而它偏偏是最長的那一句(英文兩行),又正好
    //   出現在使用者剛被告知「已送出,正在套用…」、最可能把視線
    //   移開的那一刻:4 秒之後回頭看,那一行是空的,而空白跟成功
    //   長得一模一樣。判準在 check_ui_spec.sh 的 W35。
    SetStatus(UiString::kStatusApplyFailed);
    return;
  }
  SetTransientStatus(apply_ok_status_);
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
  //
  // ⚠ 這一支是**非同步**的(#79)。所以這裡先說「已送出」,真正的
  //   「已套用」由 WM_RIME_APPLY_DONE 換上去 —— 舊版在這裡無條件說
  //   「已套用」,而那是在替一件還躺在佇列裡的工作背書。
  const unsigned seq = BeginApply(UiString::kStatusApplied);
  engine_->ApplyVariantAll(settings_.SchemaPref(), ApplyDoneNotifier(seq));
  int vsel = 0;
  for (int i = 0; i < kVariantCount; ++i)
    if (kVariantOrder[i] == v) vsel = i;
  CheckRadio(hwnd_, IDC_VARIANT_0, kVariantCount, vsel);
  if (bar_) bar_->Refresh();
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
  if (t == Tri::kUnset) {
    // 沒有任何東西被送進佇列,所以這一句當場就是真的。
    SetTransientStatus(UiString::kStatusPunctFollow);
    return;
  }
  const unsigned seq = BeginApply(UiString::kStatusApplied);
  engine_->SetOptionAll("ascii_punct", t == Tri::kTrue,
                        ApplyDoneNotifier(seq));
}

void SettingsWindow::ApplyShapeNow() {
  // ⚠ 與 ApplyPunctNow 逐字同構,而那是刻意的:兩者是同一種三態偏好,
  //   走同一條「存檔 → 套進每一個活著的 session」的路。
  const int sel = RadioSel(hwnd_, IDC_SHAPE_0, 3);
  const Tri t = sel == 0 ? Tri::kUnset : (sel == 1 ? Tri::kFalse : Tri::kTrue);
  settings_.SetShape(t);
  if (!store_->Save(settings_)) {
    SetStatus(UiString::kStatusSaveFailed);
    return;
  }
  // ⚠ kUnset(不干預)= **完全不呼叫 rs_set_option**。送 false 不是同一
  //   件事:方案沒有這個開關時,送進去會被原樣記下、原樣回讀,而那正是
  //   status_cells.h 記著的那個「畫面替沒發生的事作證」的形狀。
  if (t == Tri::kUnset) {
    SetTransientStatus(UiString::kStatusShapeFollow);
    return;
  }
  const unsigned seq = BeginApply(UiString::kStatusApplied);
  engine_->SetOptionAll("full_shape", t == Tri::kTrue, ApplyDoneNotifier(seq));
}

void SettingsWindow::ApplyShiftTapToggle() {
  const bool on =
      ::SendMessageW(Ctl(hwnd_, IDC_SHIFTTAP_SWITCH), BM_GETCHECK, 0, 0) ==
      BST_CHECKED;
  // 預設是**開**,所以「開」= 沒表示過意見 = 刪掉那個鍵(settings.h 檔頭)。
  if (on)
    settings_.Unset(keys::kTextShiftTapToggle);
  else
    settings_.SetTri(keys::kTextShiftTapToggle, Tri::kFalse);
  if (!store_->Save(settings_)) {
    SetStatus(UiString::kStatusSaveFailed);
    return;
  }
  // ⚠ 這裡**不必**通知任何人。偵測在瘦 DLL 裡,而決定切不切的那一格
  //   (service/pipe_server.cc)是在**收到那顆鍵的當下**才讀設定檔的 ——
  //   所以按下去就生效,連下一顆按鍵都不用等。這是刻意的:一顆
  //   「要重開才生效」的開關,使用者會以為它壞了。
  SetTransientStatus(UiString::kStatusApplied);
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
  // ⚠ 順序:ReloadFromSettings() 最後會 SetStatus(L"") 把那一行清空,
  //   所以「已送出」一定要在它**之後**才寫。
  ReloadFromSettings();
  const unsigned seq = BeginApply(UiString::kStatusResetDone);
  engine_->ApplyVariantAll(settings_.SchemaPref(), ApplyDoneNotifier(seq));
  if (cand_) cand_->SetTextScale(1.0);
  if (bar_) {
    bar_->SetVisible(true);
    bar_->Refresh();
  }
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
  //
  // ⚠ deploying_ 一個人不夠(#90):它在 PollDeploy 回報終局的那一刻就
  //   變成 false,而那時 session 還沒建回來 —— 中間那一段仍然打不出
  //   中文,側欄不可以在那時候就說「可以打字」。
  facts.engine_says_not_ready =
      deploying_ ||
      (engine_ && PhaseSaysPreparing(engine_->redeploy_phase()));
  return ServiceStateOf(facts);
}

void SettingsWindow::OnServiceStateTick() {
  // ── 引擎心跳 ──────────────────────────────────────────────
  //
  // ⚠ 這一段**不碰引擎的工作佇列** —— EngineBusyMs() / CurrentJobLabel()
  //   讀的是兩個小鎖底下的純資料,而那兩個鎖都只被握 O(1) 的時間
  //   (見 common/work_queue.h 的 OldestWaitingMs)。掛在這個 500 毫秒的
  //   計時器上是刻意的:它是這個視窗唯一保證不會被引擎拖住的週期性路徑。
  //
  // ⚠ **量的是佇列,不是正在跑的那一件。** 舊版問的是 StalledMs(),
  //   而那只回報「現在跑的那一件跑了多久」。引擎只有一條執行緒,所以
  //   使用者按下去之後沒有動靜的原因幾乎一定是**別人擋在前面**:
  //   20 件各 500 毫秒排在他前面 = 他等 10 秒,而 StalledMs() 從不超過
  //   500 —— 底下那個 2000 毫秒的門檻一次都不會亮。
  //
  // 為什麼要有這一格:改成非同步之後,引擎真的卡住時使用者看到的是
  // 「按了沒反應,但視窗還會動」。那比死掉的視窗好,但它仍然沒有回答
  // 他心裡的那個問題 —— **到底生效了沒有**。這一行回答它。
  //
  // ⚠ **不把工作的名字寫進畫面。** 那些標籤(「建 session」「套用方案與
  //   選項」)是給日誌看的內部字眼,而 §6.7 第一層硬禁引擎詞彙出現在
  //   使用者看得到的地方。名字寫 stderr,那裡才是查問題的人在看的。
  {
    const bool stalled =
        engine_ != nullptr && engine_->EngineBusyMs() > kEngineStallWarnMs;
    if (stalled != engine_stalled_) {
      engine_stalled_ = stalled;
      if (stalled) {
        // ⚠ 日誌兩個數字都印:「跑很久」與「等很久」要修的地方完全不同
        //   (見 common/work_queue.cc 的 SlowReporter)。畫面上只說一句話,
        //   查問題的人在看 stderr。
        std::fprintf(stderr,
                     "[settings] 引擎沒有回應,卡在:%s"
                     "(正在跑 %lld ms,佇列最舊的等了 %lld ms)\n",
                     engine_->CurrentJobLabel().c_str(),
                     static_cast<long long>(engine_->StalledMs()),
                     static_cast<long long>(engine_->OldestWaitingMs()));
        std::fflush(stderr);
        SetStatus(UiString::kStatusEngineBusy);
        engine_busy_ticket_ = status_line_.current();
      } else if (status_line_.StillShowing(engine_busy_ticket_)) {
        // ⚠ **只清自己那一則。** 舊版這裡是無條件清空,而「引擎不忙了」
        //   與「使用者剛拿到一行紅字」在時間上完全獨立 —— 兩者相撞時
        //   紅字就沒了,而且沒有任何痕跡。
        engine_busy_ticket_ = StatusLine::kNone;
        SetStatus(std::wstring());
      } else {
        // 別人蓋過去了。那一則不是我的,不動它;但要放掉自己的票,
        // 免得序號繞回來時誤判(status_line.h:序號只增不減)。
        engine_busy_ticket_ = StatusLine::kNone;
      }
    }
  }

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
      // ⚠ 自己寫出來的選取不要繞回來。ShowPage 現在先全清再設,
      //   那一次全清會同步產生好幾則 LVN_ITEMCHANGED。
      if (in_show_page_) return;
      NMLISTVIEW* lv = reinterpret_cast<NMLISTVIEW*>(nm);
      if ((lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED)) {
        in_sidebar_notify_ = true;
        ShowPage(lv->iItem);
        in_sidebar_notify_ = false;
      }
      return;
    }
    // ⚠ 換頁**不可以只吃上升緣**。已經被選取的那一列再按一次不會產生
    //   「沒有選取 → 有選取」的變化,所以 LVN_ITEMCHANGED 不會來 ——
    //   而使用者的期待是「按了就去那一頁」。這在正常情況下看不出來
    //   (那一頁已經在眼前),但只要選取與 page_ 曾經分岔過,
    //   使用者就會遇到「反白在這一列,按它卻什麼都不會發生」。
    //   ShowPage 現在是冪等的,所以這裡直接叫它。
    if (nm->code == NM_CLICK) {
      const NMITEMACTIVATE* ia = reinterpret_cast<const NMITEMACTIVATE*>(nm);
      if (ia && ia->iItem >= 0) {
        in_sidebar_notify_ = true;
        ShowPage(ia->iItem);
        in_sidebar_notify_ = false;
      }
      return;
    }
  }
  if (nm->idFrom == IDC_SCHEMA_LIST) {
    if (nm->code == NM_CUSTOMDRAW) {
      *result = DrawSchemaList(reinterpret_cast<NMLVCUSTOMDRAW*>(nm));
      return;
    }
    // ⚠ 反白現在從 schema_sel_ 畫,所以**使用者自己的點選也要寫進它**。
    //   少了這兩則,滑鼠與方向鍵選出來的那一列不會反白,而 IDC_UP /
    //   IDC_DOWN 會去搬另一列 —— 那比兩列反白更糟。
    if (nm->code == LVN_ITEMCHANGED) {
      // 自己寫出來的選取不要繞回來(比照側欄的 in_show_page_)。
      if (in_schema_select_) return;
      NMLISTVIEW* lv = reinterpret_cast<NMLISTVIEW*>(nm);
      if ((lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED))
        SelectSchemaRow(lv->iItem);
      return;
    }
    // 已經被選取的那一列再按一次不會產生「沒有選取 → 有選取」的變化,
    // 所以 LVN_ITEMCHANGED 不會來。SelectSchemaRow 是冪等的,直接叫它。
    if (nm->code == NM_CLICK) {
      const NMITEMACTIVATE* ia = reinterpret_cast<const NMITEMACTIVATE*>(nm);
      if (ia && ia->iItem >= 0) SelectSchemaRow(ia->iItem);
      return;
    }
    return;
  }
  if (nm->idFrom == IDC_NETLOG_LIST && nm->code == NM_CUSTOMDRAW) {
    *result = DrawNetLogList(reinterpret_cast<NMLVCUSTOMDRAW*>(nm));
    return;
  }
  if (WeDrawTheText(static_cast<int>(nm->idFrom)) &&
      nm->code == NM_CUSTOMDRAW) {
    *result = DrawRowButtonText(reinterpret_cast<NMCUSTOMDRAW*>(nm));
    return;
  }
}

// ── 開關列的**字**由我們自己畫(C)────────────────────────────────
//
// ⚠ **這一支存在的理由是一個算得出來的數字:1.21:1。**
//
//   覆核者在深色截圖上看到「輸入方案」頁第二張卡的標題
//   (「跟著我選的輸入法語言,自動挑一種」)暗到看起來像停用,
//   而**同一張卡的說明文字反而比它清楚**。那一句是決定性的:
//   說明走的是 kOnSurfaceVariant(深色下 9.62:1),標題走的應該是
//   kOnSurface(14.99:1)—— 標題比說明暗,代表它根本不是 kOnSurface。
//   它也不是 kDisabledText:那一顆從來沒有被 EnableWindow(FALSE) 過
//   (整個檔案裡碰 EnableWindow 的只有 IDC_REDEPLOY / IDC_APPLY_ORDER /
//    更新那三顆)。**所以它的顏色根本不是從我們的色票來的。**
//
//   啟用視覺樣式之後,BS_AUTOCHECKBOX 的字是 uxtheme 用 BUTTON 這個
//   theme class 畫的,而 WM_CTLCOLORSTATIC/BTN 的 SetTextColor 對它
//   **沒有作用**(§12.5.3 對 push button 記了同一件事,而
//   ui_theme.h 的檔頭把核取方塊的「文字」列在**做得到**那一欄 ——
//   那一格是錯的,這一輪一起改掉)。系統畫出來的是淺色佈景的近黑字:
//   #000000 對深色卡片底 #171B1D = **1.21:1**,比 kDisabledText 的
//   2.51:1 還低。「看起來像停用」是客氣的說法。
//
// ⚠ **為什麼 §12.14.1 那三道對比守門沒有擋住它。**
//   那三道跑的是 `Palette` —— 也就是**我們挑的**顏色,而它們每一個都
//   合格(kOnSurface/kSurface 深色 14.99、淺色 17.87)。畫面上那個
//   顏色是 uxtheme 挑的,不在 Palette 裡。**一道跑在自己色票上的守門,
//   對一個不屬於自己的顏色是結構性地看不見的。** 修法因此不是
//   「再加一個色票配對」,是**把那個顏色搬回我們手上**——
//   搬回來之後它才進得了守門的定義域。
//
// ⚠ 為什麼不 owner-draw:BS_OWNERDRAW 與 BS_AUTOCHECKBOX 互斥
//   (共用 BS_TYPEMASK 的低 4 位元),自繪之後螢幕閱讀器會念成「按鈕」
//   而不是「核取方塊,已勾選」。NM_CUSTOMDRAW **不動樣式位元**,
//   所以無障礙角色、自動勾選、鍵盤操作全部原封不動。
//
// ⚠ 為什麼只重畫**字**、不碰方塊:方塊要跟系統 accent 走
//   (§12.14.6.6),那是刻意的。我們擦掉的是方塊那一欄以外的區域。
//
// ── ⚠ 方塊在左邊還是右邊,由**控制項自己的樣式位元**回答 ──────────
//
//   開關列帶 BS_RIGHTBUTTON(§4.1「開關在右、標題說明在左」),
//   單選鈕不帶(§4.2 的群組,方塊在左)。這兩件事以前只有前者被處理,
//   而**深色下讀不到字的三頁全是單選鈕**。
//
//   ⚠ 這裡**不再**開第二份名單說「哪幾顆的方塊在左邊」——
//     那種名單會與 kControls 漂開,而漂開的樣子正好是這一次的缺陷:
//     一顆控制項的字回去給 uxtheme 畫,而沒有人發現。樣式位元是
//     **同一份事實**:kControls 建它的時候寫的就是這一個。
//
// ── ⚠ 那一欄有多寬:兩邊的數字不一樣,而那不是疏忽 ────────────
//
//   · 方塊在右(BS_RIGHTBUTTON):字在左、方塊在右。擦寬一點只是多擦
//     一塊本來就是卡片底色的地方,所以取**寬鬆的那一邊** 24 DIP。
//   · 方塊在左:字**在方塊右邊**,而它從哪裡開始是 comctl32 決定的
//     —— 方塊 13 DIP 寬,加上 BUTTON 這個 theme class 的內容內距
//     (約 3 DIP),字大約從 16 DIP 開始。所以這一邊**不可以**取 24:
//     取 24 的話,系統畫的那份字最左邊那 7、8 個像素會留在下面,
//     而深色下那是一小截近黑的殘影 —— 覆核者掃描線量到的就會是它。
//     取 space::s6(16)是為了對上同一個數字:方塊(≤16)完整保留,
//     字(≥16)整段被擦掉,而我們自己的字也從 16 開始畫,
//     所以左緣與系統原本的位置一致。
//
//   ⚠ 這一格是這一輪**我最沒有把握**的數字:它是從 comctl32 的行為
//     推出來的,不是量出來的(這台機器沒有 Windows)。量得到的判準
//     寫在報告的預測表裡:深色下那一列的**最暗像素必須是卡片底
//     #171B1D**,不得再出現接近 #000000 的像素。
LRESULT SettingsWindow::DrawRowButtonText(NMCUSTOMDRAW* cd) {
  if (!cd) return CDRF_DODEFAULT;
  // 先讓系統畫完(方塊、焦點、hot/pressed 的底),再把字換掉。
  if (cd->dwDrawStage == CDDS_PREPAINT) {
    // ⚠ 這一句在**舊版的假設下**就是修法本身。留著它是因為
    //   它零成本:comctl32 若真的採用 DC 的文字色,下面那一段畫出來的
    //   位置與顏色與它一致,看不出差別;若不採用(這正是缺陷),
    //   下面那一段才是真正生效的那一份。
    ::SetTextColor(cd->hdc, theme_.Color(kOnSurface));
    return CDRF_NOTIFYPOSTPAINT;
  }
  if (cd->dwDrawStage != CDDS_POSTPAINT) return CDRF_DODEFAULT;

  const int id = static_cast<int>(cd->hdr.idFrom);
  HWND ctl = cd->hdr.hwndFrom;
  if (!ctl) return CDRF_DODEFAULT;

  // 方塊在哪一邊 —— 見上面那一段。**唯一的來源是樣式位元。**
  const LONG style = ::GetWindowLongW(ctl, GWL_STYLE);
  const bool glyph_right = (style & BS_RIGHTBUTTON) != 0;
  RECT text = cd->rc;
  if (glyph_right)
    text.right -= Dip(kGlyphColRightDip, dpi_);
  else
    text.left += Dip(kGlyphColLeftDip, dpi_);
  if (text.right <= text.left) return CDRF_DODEFAULT;

  // 底:卡片裡是 surface。⚠ 與 WM_CTLCOLOR* 走**同一份** in_card_,
  //   兩邊各判一次的話會出現「字的底比卡片淺一階」的方塊。
  auto it = in_card_.find(id);
  const bool in_card = it != in_card_.end() && it->second;
  const Role bg = in_card ? kSurface : kBackground;
  ::FillRect(cd->hdc, &text, theme_.Brush(bg));

  wchar_t buf[512];
  const int n = ::GetWindowTextW(ctl, buf, 512);
  if (n > 0) {
    const bool disabled = ::IsWindowEnabled(ctl) == FALSE;
    ::SetBkMode(cd->hdc, TRANSPARENT);
    ::SetTextColor(cd->hdc,
                   theme_.Color(disabled ? kDisabledText : kOnSurface));
    HGDIOBJ oldf = ::SelectObject(cd->hdc, fonts_.Get(text_size::t3));
    // §12.14.6.6:整列高 36、字級 t3。文字左緣 = 擦除區的左緣 ——
    // 方塊在右時那就是控制項左緣,方塊在左時是方塊那一欄的右緣
    // (也就是 comctl32 自己會用的那個位置)。
    ::DrawTextW(cd->hdc, buf, n, &text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
    ::SelectObject(cd->hdc, oldf);
  }
  // 焦點環:擦掉的那一塊裡本來有系統畫的點線框。§12.6.4 第 2 條不准用
  // DrawFocusRect(它是 XOR 的),所以補我們自己那一圈。
  if (cd->uItemState & CDIS_FOCUS)
    DrawFocusRing(cd->hdc, text, Dip(radius::kControl, dpi_));
  // ⚠ POSTPAINT 的回傳值系統不看(它已經畫完了)。
  //   接管是靠 PREPAINT 回 CDRF_NOTIFYPOSTPAINT + 這裡把那一欄
  //   擦掉重畫 —— 不是靠 CDRF_SKIPDEFAULT。寫成 SKIPDEFAULT 的話
  //   下一個人會以為系統沒有畫過,然後把上面那一句 FillRect 拿掉。
  return CDRF_DODEFAULT;
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
  if (id >= IDC_SHAPE_0 && id <= IDC_SHAPE_2 && clicked) {
    ApplyShapeNow();
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
      }
      // 選取跟著那一列一起走 —— 走唯一的寫入點,它自己會重畫。
      SelectSchemaRow(to);
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
      // ── ⚠ P0:排到第一 ≠ 引擎會用它 ────────────────────────────
      //
      // ChooseSchema()(common/schema_choice.cc)由高到低是:
      //   1. pinnedHant / pinnedHans
      //   2. pinnedGlobal
      //   3. 清單中第一個字集相符的
      //   4. 清單第一個
      //
      // 而 **pinnedHant 是自動寫的,使用者不知道**:pipe_server.cc 的
      // note_schema 只要看到快照上的 schema_id 變了(也就是使用者按了
      // Ctrl + ` 或 F4)就把它記下來。而這一頁的說明文字**自己在教
      // 使用者去按那顆鍵**(「打字時按 Ctrl + ` 或 F4 可以隨時換」)。
      //
      // 於是實跑得出來的缺陷是:按過一次 Ctrl + ` 之後,再用這一頁把
      // 另一個排到第一、按「套用這個順序」—— 落地是真的(default.custom.yaml
      // 真的被改了),但**引擎仍然用 pinnedHant 那一個**。而畫面上有
      // 三個綠訊號同時說相反的話:
      //   · 「現在預設是『○○』」取的是 order_[0]
      //   · 第 0 列掛著「預設」徽章
      //   · 重新整理跑完會報「整理完成」
      // 這正是狀態欄今晚撞過四次的同一個病:顯示的真相與引擎的真相是
      // 兩份,而且會分岔。
      //
      // 舊碼**只清第 2 層**,而 pinnedGlobal 沒有任何 UI 會設它
      //(全 repo 只有這一處清除、pipe_server.cc 一處讀)—— 也就是說
      // 那一行等於空跑。
      //
      // 修法:使用者**親手排的順序**,比我們背著他記下來的那一次按鍵
      // 權威。所以三層一起放掉。
      bool pinned_changed = false;
      if (!settings_.Raw(keys::kSchemasPinnedGlobal).empty()) {
        settings_.SetPinnedGlobal(std::string());
        pinned_changed = true;
      }
      if (!settings_.Raw(keys::kSchemasPinnedHant).empty()) {
        settings_.SetPinnedForCharSet(CharSet::kHant, std::string());
        pinned_changed = true;
      }
      if (!settings_.Raw(keys::kSchemasPinnedHans).empty()) {
        settings_.SetPinnedForCharSet(CharSet::kHans, std::string());
        pinned_changed = true;
      }
      if (pinned_changed) store_->Save(settings_);
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
      const unsigned seq = BeginApply(on ? UiString::kStatusFollowOn
                                         : UiString::kStatusFollowOff);
      engine_->ApplyVariantAll(settings_.SchemaPref(), ApplyDoneNotifier(seq));
      return;
    }
    case IDC_BAR_SHOW:
      ApplyStatusBarVisibility();
      return;
    case IDC_SHIFTTAP_SWITCH:
      ApplyShiftTapToggle();
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
    case IDC_DIAG_RUN:
      StartDoctorReport();
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

namespace {
// 托盤圖示要用**主視窗**的 DPI。托盤自己的 DPI 拿不到(它屬於 explorer),
// 而同一台機器上兩者幾乎總是一致 —— 不一致時大一階好過小一階,
// 縮小比放大清楚。
UINT TrayDpiOf(HWND hwnd) {
  UINT dpi = 96;
  using GetDpiFn = UINT(WINAPI*)(HWND);
  HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
  GetDpiFn fn = u32 ? reinterpret_cast<GetDpiFn>(reinterpret_cast<void*>(
                          ::GetProcAddress(u32, "GetDpiForWindow")))
                    : nullptr;
  if (fn && hwnd) dpi = fn(hwnd);
  return dpi ? dpi : 96;
}
}  // namespace

UINT SettingsWindow::TrayDpi() const { return TrayDpiOf(hwnd_); }

// 可從任何執行緒呼叫。只是 Post 一則訊息 —— 真正的重畫在擁有 hwnd_
// 的那條執行緒上做(Shell_NotifyIcon 的要求)。
void SettingsWindow::NotifyModeChanged() {
  if (hwnd_) ::PostMessageW(hwnd_, WM_RIME_TRAY_ICON, 0, 0);
}

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
  // ⚠ 圖示由 tray_icon.cc 自繪(中 / En),而不是
  //   LoadIconW(nullptr, IDI_APPLICATION)。
  //
  //   那一橫本來是常駐的,所以托盤只是備援入口,醜一點沒關係。現在
  //   那一橫會**自己消失**(§12.10.6),托盤就升格成「服務活著時唯一
  //   必然存在的入口」—— 而一顆與別的東西一模一樣的通用圖示等於
  //   沒有入口:使用者找不出哪一顆是輸入法。
  //
  //   順便解掉第二件事:中/英在系統托盤有了一個「那一橫以外」的家。
  //   三家競品(微軟、搜狗、小狼毫)的交集正是「中/英屬於托盤那一格」。
  tray_ascii_ = engine_ && engine_->AsciiMode();
  tray_icon_ = MakeModeTrayIcon(BarModeGlyph(tray_ascii_), TrayDpi());
  // ⚠ 自繪失敗就退回系統圖示 —— 一顆醜圖示仍然比沒有圖示好。
  nid.hIcon = tray_icon_ ? tray_icon_ : ::LoadIconW(nullptr, IDI_APPLICATION);
  ::lstrcpynW(nid.szTip, UiText(UiString::kTrayTip), 128);
  tray_added_ = ::Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

// 中英模式變了就重畫托盤那一格。
//
// ⚠ 只在真的變了的時候動(tray_ascii_ 擋住)。托盤重畫在某些佈景主題下
//   會閃一下,而每半秒閃一次比圖示不更新更礙眼。
void SettingsWindow::RefreshTrayIcon(bool modify) {
  if (!tray_added_ || !hwnd_ || !engine_) return;
  const bool now = engine_->AsciiMode();
  if (modify && now == tray_ascii_) return;
  HICON fresh = MakeModeTrayIcon(BarModeGlyph(now), TrayDpi());
  if (!fresh) return;  // 畫不出來就留著舊的,不要換成空的
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd_;
  nid.uID = kTrayId;
  nid.uFlags = NIF_ICON;
  nid.hIcon = fresh;
  ::Shell_NotifyIconW(NIM_MODIFY, &nid);
  // ⚠ 先換再放。Shell_NotifyIcon 只是**引用**那個 HICON,不會複製 ——
  //   反過來的話托盤上會出現一顆空的。
  if (tray_icon_) ::DestroyIcon(tray_icon_);
  tray_icon_ = fresh;
  tray_ascii_ = now;
}

void SettingsWindow::RemoveTray() {
  if (!tray_added_) return;
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd_;
  nid.uID = kTrayId;
  ::Shell_NotifyIconW(NIM_DELETE, &nid);
  tray_added_ = false;
  // 自繪的圖示要自己銷毀。刪掉那一格之後才放,順序反了會是
  // use-after-free(托盤還引用著它)。
  if (tray_icon_) {
    ::DestroyIcon(tray_icon_);
    tray_icon_ = nullptr;
  }
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
    // ── 中 / 英 ────────────────────────────────────────────────
    //
    // ⚠ 為什麼要在托盤上再放一次:那一橫現在會**自己消失**
    //   (§12.10.6),而 ui-design.md §12.10.2 整節的論證是
    //   「這一橫是中英切換唯一的家」。那個前提要被徹底拆掉才敢讓它消失。
    //
    //   現在中英有三個家:Ctrl + 空白鍵(tsf 的 PreserveKey)、
    //   那一橫的第一格、以及這裡。這一個的成本是零 TSF ——
    //   它在服務活著時**必然存在**,與有沒有宿主在用無關。
    //
    // ⚠ 這兩項**不碰** CommitVariantPref(那是簡繁那三項的路,
    //   而且另一條線正在改那一段)。它們走 Engine::SetAsciiModeAll()。
    const bool ascii = engine_ && engine_->AsciiMode();
    ::AppendMenuW(menu, MF_STRING | (ascii ? MF_UNCHECKED : MF_CHECKED),
                  IDM_TRAY_MODE_CN, UiText(UiString::kTrayModeChinese));
    ::AppendMenuW(menu, MF_STRING | (ascii ? MF_CHECKED : MF_UNCHECKED),
                  IDM_TRAY_MODE_EN, UiText(UiString::kTrayModeEnglish));
  }
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
  } else if (cmd == IDM_TRAY_MODE_CN || cmd == IDM_TRAY_MODE_EN) {
    // 已經在 UI 執行緒上,直接做 —— 與簡繁那三項同一個理由:
    // 「按下去」與「生效」中間隔一次訊息迴圈的話,使用者立刻去打字
    // 會發現還沒切。
    if (engine_) engine_->SetAsciiModeAll(cmd == IDM_TRAY_MODE_EN);
    // 托盤那一格自己就是指示器,要跟著變。
    RefreshTrayIcon(/*modify=*/true);
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
