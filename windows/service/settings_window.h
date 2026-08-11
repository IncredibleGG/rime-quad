// windows/service/settings_window.h — 設定介面
//
// ── 技術選型:純 Win32 + 通用控制項 v6 ──────────────────────────
//
// 硬約束是「不要 Electron、不要打包 Chromium」,而它背後的理由不只是體積:
// 這個專案的定位是離線、經得起審計。使用者要能相信「它不連網」,
// 而說服他的方式是 `dumpbin /imports` 看得出來沒有網路 DLL ——
// 塞一個瀏覽器引擎進來,那句話就再也不可能驗證了。
//
// 被排除的與理由:
//
//   · **Electron / WebView2** — 前者打包 Chromium(上百 MB,而且是一整個
//     網路堆疊);後者相依於系統上的 Edge 執行期,那同樣是 Chromium,
//     而且**不在我們的控制之下**(它會自己更新)。兩者都直接違反約束。
//   · **WinUI 3 / XAML Islands** — 需要 Windows App SDK 的可轉散發套件,
//     而且與貫穿全案的 `/MT` 靜態 CRT 打架。
//   · **WPF / WinForms** — 需要 .NET 執行期。
//   · **本機 HTTP + 系統瀏覽器** — 會開 socket。離線定位下這是最糟的選項。
//
// 剩下的是純 Win32。代價是排版要自己算、控制項醜、無障礙要自己顧;
// 換到的是**零額外相依、與 /MT 相容、二進位增加的大小解釋得清楚**。
//
// ── 2026-08-09:改成側欄式,並修掉四類缺陷 ──────────────────────
//
// 版面從分頁(WC_TABCONTROLW)改成側欄(docs/ui-design.md §12.4)。
// **判準吃的是產品的頁數,不是某一端實作了幾頁** —— 桌面端是 7 頁,
// 所以是側欄;照今天實作的 4 頁做分頁,等市集/連網/自己加的詞上線就要整個重做。
//
// 同時修掉的:
//   1. `dpi_scale_` 只算一次而進程是 per-monitor-v2 → 現在處理 WM_DPICHANGED。
//   2. 無條件捨去的縮放 → 一律走 MulDivRound(common/ui_dip.h)。
//   3. 字型走 SPI_GETNONCLIENTMETRICS(違反 §8.6.0)→ 走 ui_font.h。
//   4. 清單每一列印著方案 id(違反 §6.7 第一層)→ 只印名字。
//   5. MB_YESNO 做確認(按鈕字面由系統決定,必然違反 §2-C3)→ ui_confirm.h。
//   6. 硬編的中文字面值 → common/ui_strings.h,英/繁/簡三語。
//
// ── ⚠ 第六個「看得到但摸不到」,以及它為什麼不會再發生 ──────────
//
// 舊版有一顆 `IDC_FOLLOW_MODE` 核取方塊:建立了、有 WM_COMMAND 處理常式、
// 有讀寫設定的完整程式碼 —— 但它**不在任何一頁的 id 陣列裡**,
// 而控制項是用 `WS_CHILD`(沒有 `WS_VISIBLE`)建的,只有 ShowTab 會去
// 顯示它們。於是那顆核取方塊從來沒有被 ShowWindow 過:
// **它在畫面上根本不存在,而程式碼看起來完全正常。**
//
// 這一輪的修法不是「把它加進陣列」,那只修掉這一次。
// 改成:控制項由**一張表**(kControls)產生,表上每一列都帶著它屬於哪一頁。
// 建立與顯示走同一份資料,所以「建了但沒有頁」在結構上不可能發生 ——
// 一顆控制項要嘛不存在,要嘛屬於某一頁而且會被顯示。
//
#ifndef RIMEWIN_SERVICE_SETTINGS_WINDOW_H_
#define RIMEWIN_SERVICE_SETTINGS_WINDOW_H_

#include <windows.h>

// commctrl.h 要在 windows.h 之後。⚠ 它在**標頭**裡是必要的:
// NM_CUSTOMDRAW 的處理常式簽章帶著 NMLVCUSTOMDRAW,而那個型別在這裡。
#include <commctrl.h>

#include <string>
#include <vector>

#include "../common/schema_choice.h"
#include "../common/service_state.h"
// ⚠ 頁、控制項 id 與**每一頁的版面**都住在這裡面。本檔不再自己算矩形。
#include "../common/ui_layout.h"
#include "../common/ui_strings.h"
#include "engine.h"
// ⚠ 連網出口。設定視窗**不自己讀寫** `network.enabled`,一律走 NetGate ——
//   直接寫 settings_ 的話,出口那一側讀到的仍然是舊值,而那個差別的樣子是
//   「開關看起來開了,按下去卻說被擋下」。
#include "net_gate.h"
#include "settings_store.h"
#include "update_service.h"
#include "ui_font.h"
#include "ui_theme.h"

namespace rimewin {

class CandidateWindow;
class StatusBar;

class SettingsWindow {
 public:
  SettingsWindow(Engine* engine, SettingsStore* store,
                 const std::string& shared_dir);
  ~SettingsWindow();

  bool Start();
  void Stop();

  // 可從任何執行緒呼叫。已經開著就把它帶到最前面。
  void Open();
  // 開在某一頁上(懸浮狀態列的「輸入法沒有在跑」會直接帶到「進階」)。
  void OpenAt(int page);

  void SetCandidateWindow(CandidateWindow* w) { cand_ = w; }
  void SetStatusBar(StatusBar* b) { bar_ = b; }

  // ── 簡繁:設定視窗**以外**的入口 ────────────────────────────
  // 語言設定檔收斂成一份之後,使用者再也不能用 Win+空白鍵切簡繁,
  // 所以那件事必須在我們自己的 UI 裡做得到。
  void SetVariantPref(VariantPref v);
  VariantPref CurrentVariantPref();

  // 懸浮狀態列要用的:目前的介面語言與外觀,好讓兩個表面長得一樣。
  UiLang ui_lang() const { return ui_lang_; }

 private:
  // ⚠ 頁的順序(kPageSchemas … kPageCount)與控制項 id 都在
  //   common/ui_layout.h。它們**不可以**住在這裡:住在這裡就表示
  //   版面測不到,而「外觀頁的深淺三態排到視窗外而 W18 全綠」
  //   就是那樣發生的。

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
  static DWORD WINAPI ThreadEntry(LPVOID self);
  void ThreadMain();

  void CreateUi(HWND hwnd);
  void LayoutUi();
  void ShowPage(int page);
  void ReloadFromSettings();
  void ReloadSchemaList();
  void OnCommand(int id, int code);
  void OnNotify(NMHDR* nm, LRESULT* result);
  void OnDeployTick();

  // ── 連網那一頁 ────────────────────────────────────────────
  //
  // ⚠ 「該不該開始檢查更新」「開關現在說哪一句話」「紀錄的四欄長什麼樣」
  //   三件事的**決定權都不在這裡**,在 common/net_ui.h(純函式,有單元
  //   測試)。這裡只負責把結果接到 Win32 上 —— 理由與捲動那三條接線
  //   完全相同:這個檔案在 Ubuntu 上編不起來,寫在這裡就沒有人驗得到。
  void RefreshNetworkPage();
  void OnNetSwitchToggled();
  void DoClearNetLog();
  // 版面上那兩個執行期分支的**唯一**來源。⚠ 兩格都得是真的狀態:
  // net_log_empty 寫死的話,一次都沒有連過的使用者會看到一個空表格
  // 加一顆清除鍵,而那正是這一頁最不該有的樣子。
  PageState PageStateNow() const;
  // ⚠ 側欄底部那一行以前只在 WM_PAINT 時算,而 WM_PAINT 要等使用者
  //   去碰視窗。首次安裝的人正好就坐在這一頁上等,於是「還沒好」
  //   會一直寫在那裡,直到他去點別的東西。
  void OnServiceStateTick();
  // 現在到底怎麼了。判斷在 common/service_state.h,與懸浮狀態列
  // **共用同一份** —— 各寫一份會漂移,而漂移的症狀是
  // 「側欄說沒在跑,那一橫說在準備」。
  ServiceState SidebarServiceState() const;
  void OnPaint(HDC hdc);
  void OnDpiChanged(UINT dpi, const RECT* suggested);
  void RefreshTheme();
  void ApplyFonts();

  // ── 內容區的捲動(§12.4 A2)────────────────────────────────
  //
  // ⚠ 為什麼一定要有:外觀頁的內容高 890 DIP,而 150% 的 1080p 筆電
  //   client 高約 667 DIP —— **視窗拉到最大也碰不到深淺色三態**,
  //   而那三顆是整個 UI 上唯一的入口。加大最小尺寸解決不了,
  //   因為使用者的螢幕就那麼高。
  void SetScroll(int dip);
  void OnVScroll(int code, int track_pos);
  void OnMouseWheel(int delta);
  // Win32 **不會**自動把鍵盤焦點捲進可見範圍。少了這一支,Tab 走到
  // 視窗外的控制項時畫面完全不動,使用者在盲按。
  void EnsureFocusVisible();
  // 把控制項裁掉伸進底部固定列的那一截(子視窗只會被父視窗的 client
  // 矩形裁掉,而底部那 54 DIP 仍在 client 裡面)。
  // 裁掉底部固定列蓋到的那一截。**clip_h_dip < 0 = 不裁**;
  // 它由 ScrollPlaceControlDip() 算出來,這裡不再自己判斷 ——
  // 自己判斷的那一版單元測試看不到。
  void ClipToViewport(int index, HWND c, int w_dip, int clip_h_dip);

  // 自繪(§12.5.3 的六類裡的四類在這個檔案)。
  LRESULT DrawSidebar(NMLVCUSTOMDRAW* cd);
  LRESULT DrawSchemaList(NMLVCUSTOMDRAW* cd);
  LRESULT DrawNetLogList(NMLVCUSTOMDRAW* cd);
  void DrawDangerButton(DRAWITEMSTRUCT* di);

 public:
  // 中英模式變了。可從任何執行緒呼叫。
  void NotifyModeChanged();

 private:
  void AddTray();
  void RemoveTray();
  // 依目前的中英模式重畫托盤圖示(NIM_ADD / NIM_MODIFY 共用)。
  void RefreshTrayIcon(bool modify);
  UINT TrayDpi() const;
  void OnTray(WPARAM w, LPARAM l);

  void ApplyVariantNow();
  void CommitVariantPref(VariantPref v);
  void ApplyPunctNow();
  void ApplyAppearancePref();
  void ApplyUiLanguage();
  void ApplyScaleNow();
  void ApplyStatusBarVisibility();
  // 輕點 Shift 切中英(#89)。存的是設定,不必通知任何人 ——
  // 決定切不切的那一格在收到那顆鍵的當下才讀設定檔。
  void ApplyShiftTapToggle();
  void DoResetSettings();
  bool ApplyOrderAndPageSize(std::string* error);
  // ── 連上網路 / 更新 ──────────────────────────────────────
  //
  // ⚠ 檢查與下載都是**阻塞**的(見 net_gate.h),所以它們跑在自己的
  //   執行緒上。UI 執行緒同時在跑候選窗 —— 卡住它就是
  //   「打字打到一半整個沒反應」。
  //
  // 執行緒回來的方式是 PostMessage(WM_RIME_UPDATE_DONE),不是直接改狀態:
  // 所有 UI 狀態一律只在 UI 執行緒上動。
  // ⚠ 這一支把**整頁**重畫一次(連網那半走 RefreshNetworkPage)——
  //   更新卡片與紀錄在同一頁上,而按下更新之後紀錄裡會多幾筆,
  //   只重畫卡片的話使用者要換頁再換回來才看得到那幾筆。
  void RefreshNetworkAndUpdateCard();
  void StartUpdateCheck();
  void StartUpdateDownload();
  void DoUpdateHandOff();
  void OnUpdateWorkerDone();
  void ToggleNetwork();
  void OpenNetLog();
  void OpenDownloadPage();
  static DWORD WINAPI UpdateWorkerEntry(LPVOID self);

  void StartRedeploy(UiString why);
  void SetStatus(const std::wstring& text);
  void SetStatus(UiString s) { SetStatus(UiText(s)); }
  // 成功訊息 4 秒後自己清掉(§12.5.3:不做浮層,成功不值得一個新表面)。
  void SetTransientStatus(UiString s);

  int SelectedSchemaRow() const;
  std::wstring SchemaDisplayName(size_t index) const;
  Script script() const;

  Engine* engine_;
  SettingsStore* store_;
  std::string shared_dir_;
  // ⚠ 宣告順序 = 初始化順序,而它吃 store_ —— 所以它排在 store_ 後面。
  NetGate net_gate_;
  CandidateWindow* cand_ = nullptr;
  StatusBar* bar_ = nullptr;

  HANDLE thread_ = nullptr;
  DWORD thread_id_ = 0;
  HANDLE ready_ = nullptr;
  HWND hwnd_ = nullptr;
  HWND sidebar_ = nullptr;
  HWND schema_list_ = nullptr;
  HWND net_log_list_ = nullptr;

  // ⚠ **沒有 dpi_scale_。** 那個 double 正是舊版每一項各少 0~1 px、
  //   而誤差沿著版面累積的來源。一律 MulDivRound(dip, dpi_, 96)。
  UINT dpi_ = 96;
  FontSet fonts_;
  Theme theme_;
  UiLang ui_lang_ = UiLang::kZhHant;
  int page_ = kPageSchemas;
  // 上一次畫在側欄底部的是哪一種狀態。只有它變了才重畫那一小塊。
  ServiceState sidebar_state_ = ServiceState::kReady;
  // 鍵盤使用時才畫焦點環(§12.6.4 第 1 條)。滑鼠使用者身上到處是框,
  // 是 Win32 自繪最常見的破綻。WM_UPDATEUISTATE 維護它。
  bool show_focus_ = false;

  // 內容區的捲動量與上限,單位 DIP(不是像素 —— 換螢幕時 DPI 會變)。
  int scroll_ = 0;
  int scroll_max_ = 0;
  // ⚠ SetScrollInfo 讓捲軸出現/消失時會送 WM_SIZE,而 WM_SIZE 又叫
  //   LayoutUi。沒有這個旗標就是無限遞迴。
  bool in_layout_ = false;
  // 每一顆控制項目前被裁掉之後剩下的高度(DIP)。-1 = 沒有裁。
  // ⚠ 存著是為了**只在變動時**才呼叫 SetWindowRgn:那一支會重畫,
  //   每次 LayoutUi 都無條件呼叫的話,捲動時整頁會閃。
  std::vector<int> clip_h_;

  // ⚠ **只有一個 NetGate**,而且是整個進程唯一的那一個 —— 出口只有一個,
  //   這裡不新開第二條路(見 net_gate.h 的「單一出口」)。它宣告在上面
  //   store_ 後面的 net_gate_;update_ 吃它的位址,所以 update_ 必須排在
  //   它後面(宣告順序 = 初始化順序)。
  UpdateService update_;
  // 0 = 沒有在跑;1 = 查;2 = 下載。UI 執行緒讀、工作執行緒不寫。
  int update_job_ = 0;
  HANDLE update_thread_ = nullptr;
  UpdateStage update_stage_ = UpdateStage::kIdle;
  UpdateFailure update_failure_ = UpdateFailure::kNone;
  // 上一次交棒的結果(啟動時和解出來的)。只顯示一次。
  std::wstring update_note_;

  bool deploying_ = false;
  uint32_t deploy_seq_ = 0;
  DWORD deploy_start_ = 0;
  UiString deploy_why_ = UiString::kRedeployButton;
  bool has_rollback_ = false;
  std::string rollback_yaml_;

  bool tray_added_ = false;
  // 目前托盤上畫的是哪一邊。避免每半秒重畫一次 —— 托盤重畫會在某些
  // 佈景主題下閃一下,而那比圖示不更新更礙眼。
  bool tray_ascii_ = false;
  // ⚠ 自繪的圖示要自己銷毀。Shell_NotifyIcon 只是**引用**它,
  //   不會複製 —— 換掉之後才可以放,順序反了會畫出一顆空的。
  HICON tray_icon_ = nullptr;
  UINT taskbar_created_ = 0;

  Settings settings_;
  std::vector<std::pair<std::string, std::string>> schemas_;  // id, name
  std::vector<std::string> order_;   // 目前清單上的順序(id)

  // ── 連網那一頁的狀態 ──────────────────────────────────────
  //
  // ⚠ 預設 true(= 一次都沒有連過)。方向是刻意的:還沒讀到紀錄之前,
  //   畫面要說「一次都沒有連過」,不是給一個空表格。
  bool net_log_empty_ = true;
  // 畫紀錄那一列時要用的文字。與 SetRowListItems 餵進去的是同一份 ——
  // 兩份會漂移,而漂移的症狀是「螢幕閱讀器念的與畫面上的不一樣」。
  std::vector<std::wstring> net_log_lines_;
  // 有一次檢查正在跑。⚠ 只在 UI 執行緒上讀寫(按鈕與完成訊息都在那裡),
  // 所以不需要 atomic;背景執行緒**不碰它**。
};

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_SETTINGS_WINDOW_H_
