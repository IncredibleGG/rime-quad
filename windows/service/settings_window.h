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

#include <functional>
#include <string>
#include <map>
#include <vector>

#include "../common/schema_choice.h"
#include "../common/service_state.h"
#include "../common/status_line.h"
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
  // may_query = false:只讀快取,**不准**再排一次非同步查詢。
  // WM_RIME_SCHEMAS_READY 的處理常式用它 —— 不然引擎回一份空清單時
  // 「查完 → 還是空的 → 再查一次」會變成一個永遠打不完的訊息迴圈。
  void ReloadSchemaList(bool may_query = true);
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
  // 開關列(BS_AUTOCHECKBOX)的**字**由我們畫 —— uxtheme 畫的那一份
  // 在深色下是 1.21:1。見 .cc 檔裡那一段。
  LRESULT DrawSwitchRowText(NMCUSTOMDRAW* cd);
  void DrawDangerButton(DRAWITEMSTRUCT* di);

  // ── §12.14.4:自己畫的每一塊都是圓角 ────────────────────────
  //
  // ⚠ 「先 FillRect 再 RoundRect(NULL_BRUSH)」是**方角**:第二步只畫線,
  //   沒有把四個角外面那塊底色挖掉。§12.14.0 第 2 條記的就是那個形狀。
  //   這一支先把角外那塊填成 under 色,再用 RoundRect 填實。
  void FillRoundRect(HDC hdc, const RECT& r, int radius_px, Role fill,
                     Role under);
  // 只描一圈邊(不填)。
  void StrokeRoundRect(HDC hdc, const RECT& r, int radius_px, Role pen,
                       int width_px);
  // §12.14.2 的雙色焦點環:外 2 DIP focusOuter + 內 1 DIP focusInner。
  // ⚠ **不用 kPrimary。** accent 是使用者選的,一個淺黃色的焦點環畫在
  //   白底的清單列上是看不見的,而焦點看不見就是鍵盤使用者走不下去。
  //   兩圈互為反色,所以不管底下是什麼顏色,一定有一圈看得見。
  void DrawFocusRing(HDC hdc, const RECT& r, int radius_px);
  // 危險鍵的 hover:WM_DRAWITEM 不給 hot 狀態,要自己追。
  void TrackDangerHover(HWND ctl, int id);
  void ClearDangerHover();
  // 這一頁的主要按鈕(BS_DEFPUSHBUTTON)。⚠ 一個視窗只能有一顆,
  // 切頁時要把上一頁那顆的樣式拿掉,否則 Enter 會按到看不見的那一顆。
  void ApplyDefaultButtonForPage(int page);
  // 兩顆自繪危險鍵的子類別化。⚠ 存在的理由只有一個:滑鼠訊息**不會**
  // 走到父視窗 —— 子控制項自己收 WM_MOUSEMOVE。沒有這一支就追不到
  // hover,而 §12.14.6.4 的四個狀態就會少一個。
  static LRESULT CALLBACK DangerProc(HWND h, UINT m, WPARAM w, LPARAM l,
                                     UINT_PTR id, DWORD_PTR data);

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
  void ApplyShapeNow();
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

  // ── ⚠ 「已送出」→「已套用 / 套用失敗」 ──────────────────────
  //
  //   引擎那一頭是非同步的(見 engine.h 的 SetOptionAll / ApplyVariantAll),
  //   所以按下的當下唯一為真的句子是「已送出」。真正的結果由工作者
  //   PostMessage(WM_RIME_APPLY_DONE)回來,再把那句話換掉。
  //
  //   為什麼選這一條、而不是「等工作回來才說話」:引擎正常時那一趟是
  //   幾毫秒,兩條路看起來一樣;引擎卡住時,「等回來才說話」的畫面是
  //   **按了完全沒反應**,而那正是 #79 使用者的原話。按下去一定要有
  //   東西動,而那個東西必須說實話。
  //
  //   ⚠ seq 是為了「連按三下」:只有**最後一次**送出的結果可以寫進
  //     那一行。少了它,前一次的失敗會蓋掉後一次的成功。
  unsigned BeginApply(UiString ok_status);
  // 給引擎的完成通知。⚠ 回傳的 lambda 跑在**工作者執行緒**上,
  //   所以它只捕捉 HWND 與 seq(都是傳值),而且只做 PostMessageW。
  std::function<void(bool)> ApplyDoneNotifier(unsigned seq);
  void OnApplyDone(unsigned seq, bool ok);

  // ── ⚠ 方案清單的「哪一列被選」只有一份 ──────────────────────
  //
  //   這是 #80 的孿生兄弟。側欄那一半上一輪修好了(單一寫入點 +
  //   先全清 + 反白從 page_ 畫),而方案清單原封不動:兩處裸的
  //   LVM_SETITEMSTATE、反白從 comctl32 的 CDIS_SELECTED 畫。
  //   同樣是兩份真相、同樣沒有地方對帳,而分岔的樣子一樣是
  //   **兩列同時反白**。
  //
  //   真相現在是 `schema_sel_`(對應側欄的 `page_`),
  //   而 SelectSchemaRow() 是**唯一**的寫入點。
  void SelectSchemaRow(int row);
  int SelectedSchemaRow() const { return schema_sel_; }
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

  // 上一次心跳看到的引擎狀態(見 OnServiceStateTick)。只有它**變了**
  // 才寫底下那一行 —— 每半秒無條件寫一次會把使用者剛看到的訊息蓋掉。
  bool engine_stalled_ = false;

  // ── 底下那一行狀態訊息:誰寫的、誰可以收回 ────────────────────
  //
  // ⚠ **收回**(清空)與**說話**不一樣:清空只有在「收的是自己寫的
  //   那一則」時才是對的。舊版兩處都是無條件 SetStatus(L""),於是
  //   4 秒的計時器與心跳解除都會在隨機的時間點抹掉使用者剛拿到的紅字。
  //   判斷邏輯在 common/status_line.h(那裡測得到)。
  StatusLine status_line_;
  StatusLine::Ticket transient_ticket_ = StatusLine::kNone;
  StatusLine::Ticket engine_busy_ticket_ = StatusLine::kNone;

  // 送出去給引擎的第幾次套用。只有最新的那一次的結果可以寫進狀態行。
  unsigned apply_seq_ = 0;
  // 這一次成功要說的那一句(各個呼叫點不一樣:已套用 / 自動挑開了 /
  // 已還原成預設…)。
  UiString apply_ok_status_ = UiString::kStatusApplied;

  // 內容區的捲動量與上限,單位 DIP(不是像素 —— 換螢幕時 DPI 會變)。
  int scroll_ = 0;
  int scroll_max_ = 0;
  // ⚠ SetScrollInfo 讓捲軸出現/消失時會送 WM_SIZE,而 WM_SIZE 又叫
  //   LayoutUi。沒有這個旗標就是無限遞迴。
  bool in_layout_ = false;
  // ⚠ 同一個形狀,換一顆控制項:ShowPage 對側欄下 LVM_SETITEMSTATE,
  //   comctl32 **同步**送 LVN_ITEMCHANGED 回來,而那則通知又叫 ShowPage。
  //   (比照 in_layout_。)
  bool in_show_page_ = false;
  // 現在正在處理側欄的通知。ShowPage 據此決定重排要不要延後 ——
  // 在 comctl32 更新自己選取範圍的中途對同一顆控制項下 SetWindowPos +
  // LVM_SETCOLUMNWIDTH,是在重入它。
  bool in_sidebar_notify_ = false;
  // 方案清單目前選中的是第幾列(-1 = 一列都沒有,清單是空的)。
  // ⚠ **這是那個問題唯一的答案。** 自繪從它畫,IDC_UP/IDC_DOWN 從它讀,
  //   使用者的點選由 LVN_ITEMCHANGED / NM_CLICK 寫進它。
  int schema_sel_ = -1;
  // 與 in_show_page_ 同一個形狀:SelectSchemaRow 對清單下 LVM_SETITEMSTATE,
  // comctl32 **同步**送 LVN_ITEMCHANGED 回來,而那則通知又叫 SelectSchemaRow。
  bool in_schema_select_ = false;
  // 每一顆控制項目前被裁掉之後剩下的高度(DIP)。-1 = 沒有裁。
  // ⚠ 存著是為了**只在變動時**才呼叫 SetWindowRgn:那一支會重畫,
  //   每次 LayoutUi 都無條件呼叫的話,捲動時整頁會閃。
  std::vector<int> clip_h_;
  // 這一頁上的卡片矩形(內容座標,DIP)。OnPaint 畫它們。
  // ⚠ 由 LayoutSettingsPageDip() 產生 —— 這裡只是把結果存下來,
  //   **不得**自己算。自己算的那一份會與控制項的位置漂開。
  std::vector<CardRect> cards_;
  // 這一顆控制項坐在卡片裡嗎(id → bool)。WM_CTLCOLOR* 靠它決定
  // 回 surface 還是 background。
  std::map<int, bool> in_card_;
  // 現在滑鼠指著哪一顆危險鍵(-1 = 沒有)。
  int danger_hot_ = -1;
  HWND danger_tracked_ = nullptr;
  // 現在哪一顆是 BS_DEFPUSHBUTTON(0 = 沒有)。
  int default_button_ = 0;

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
  // 清單是空的時候,那一格說明說的是**哪一件事**(見 common/ui_layout.h
  // 的 SchemaListNote)。三種:真的一種都沒有 / 還在問引擎 / 問不到。
  // ⚠ 三種以前在畫面上是同一句話(#62),而下一步完全不同。
  int schema_note_ = kSchemaNoteEmpty;

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
