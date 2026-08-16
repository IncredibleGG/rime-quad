// windows/service/status_bar.h — 懸浮狀態列(docs/ui-design.md §12.10)
//
// 使用者原話:「小小一橫在右下角,可以拖動,上面可以快捷修改」。
//
// ── ⚠ 它不是 §8.12 的 `status_bar` ────────────────────────────────
//
// §8.12 的那一條住在**候選窗裡面**,只有組字時看得見,不能拖,預設關。
// 這一條是螢幕上的獨立 top-level 視窗,一直都在,可以拖,而且**預設開**。
// 規範裡沒有這個表面 —— 已寫進 docs/coordination.md §5 請 macOS 端裁決。
//
// 共用的部分是規範性的:
//   · 第一格 `is_ascii_mode ? En : 中`(§8.12 的 `input_mode`),
//     第二格 `is_simplified ? 简 : 繁`。**兩格都只畫當前那一個** ——
//     使用者實機回報「中/en 應該是現在是什麼輸入法就顯示什麼」,
//     取捨與規範依據見 common/status_cells.h 的檔頭。
//     四個字面**不得**在地化(§12.9.3)—— 所以它們直接寫在 .cc 的
//     繪製碼裡,不進 ui_strings.cc,而 W10 兩個方向都驗。
//   · 空狀態**整項略過**:方案名還沒載入完成時,那一格完全不佔位置,
//     不得畫成一塊看不出用途的空白。
//   · **不得加圖示。** §8.12 記著一個還沒關的缺口,在補上之前
//     「任何『一顆代表當前方案的小圖示』都是各端自己發明的」。
//
// ── ⚠ 為什麼預設是開的 ──────────────────────────────────────────
//
// `ascii_mode` 在這一輪之前,整個 `windows/` 底下只被**讀**過兩次,
// **一次都沒有被設定過**。三條可能的路當時全部不通:Shift、
// 語言列(InitMenu 回 E_NOTIMPL)、系統匣(沒有中英那一項)。
//
// ⚠ **Shift 那一條當時給的理由(「TSF 不交付純修飾鍵」)是假的,已實測
//   推翻。** CI run 31511075812(sha ca97498)logic-x64 的「真的經過 TSF」:
//   送一次左 Shift,SHIFT_TRACE_LINES=1,而多出來的那一行是 OnTestKeyDown
//   自己寫的(vk=0x10 scan=0x2A keysym=0xFFE1 族=host-only 吃掉=0)。
//   **sink 收得到純修飾鍵,不需要低階鍵盤 hook。**
//   ⚠ **[2026-08-12] Shift 那一條已經做了**(#89):輕點偵測是
//   `common/shift_tap.cc` 的純函數狀態機,接在 `OnTestKeyDown` /
//   `OnTestKeyUp`,`*eaten` 一律 FALSE(不吃任何一顆修飾鍵)。
//   也就是說這一橫不再是「那個功能唯一的家」—— 下面那句話已經過期。
//   ⏳ 但真機驗收還沒做(#48),所以這一橫預設仍然是開的。
//   ⚠ 下面那句「唯一的辦法是 Win+空白鍵」**也已經過期**:Ctrl+空白鍵與
//   系統匣兩條後來通了。完整的更新在 docs/ui-design.md §12.10.2 的表格,
//   量測本身見 docs/coordination.md 的 [2026-08-12] [winbar] 那一則。
//
// 也就是說:**Windows 使用者要在句子中間打一個英文字,唯一的辦法是按
// Win+空白鍵把整個輸入法換掉。** 這一橫不是「多一個方便的入口」,
// 它是**那個功能唯一的家**。預設關掉等於預設沒有中英切換。
//
// ⚠ 使用者要關得掉(有人覺得礙眼):設定裡一個開關。
//   **這一橫上不放 X** —— 一顆會讓它消失、而且不知道怎麼找回來的鈕,
//   會被誤點。
//
// ⚠ **不掛低階鍵盤 hook。** WH_KEYBOARD_LL 會看到使用者在每一個程式裡的
//   每一次按鍵,與「離線、經得起審計」的定位直接衝突 ——
//   我們要求別人相信這支程式不偷看,那就不能裝一個看得到全部按鍵的東西。
//   中英切換走這一橫、系統匣、設定,以及 librime 自己的按鍵處理。
//
#ifndef RIMEWIN_SERVICE_STATUS_BAR_H_
#define RIMEWIN_SERVICE_STATUS_BAR_H_

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "../common/protocol.h"
#include "../common/bar_owner.h"
#include "../common/bar_visibility.h"
#include "../common/status_cells.h"
#include "../common/service_state.h"
#include "../common/statusbar_place.h"
#include "ui_font.h"
#include "ui_theme.h"

namespace rimewin {

class Engine;
class SettingsWindow;
class SettingsStore;

// §8.12 的中英字面(中 / En)。
//
// ⚠ 這四個規範性字面全 repo 只住在 service/status_bar.cc 一個地方
//   (§12.9.3 第 1 條:它們是狀態指示,不是介面文字,不進 catalog),
//   而 W7 / W10 兩個方向守著那件事。托盤圖示要畫同一個字,所以走這一支
//   拿 —— 在別的檔案裡再寫一份會多出第二份真相,而「改名改一半」正是
//   這個專案吃過虧的形狀。
const wchar_t* BarModeGlyph(bool ascii_mode);

class StatusBar {
 public:
  StatusBar(Engine* engine, SettingsStore* store);
  ~StatusBar();

  bool Start();
  void Stop();

  void SetSettingsWindow(SettingsWindow* w) { settings_ = w; }

  // 可從任何執行緒。
  void SetVisible(bool on);
  // 重讀狀態(簡繁、方案名、中英)並重畫。
  void Refresh();
  void RefreshTheme();

  // ⚠ 現在到底怎麼了。**不是布林。**
  //   「還在準備 / 準備失敗 / 引擎不在」以前被壓成同一個 false,
  //   於是三種都畫同一句紅字「輸入法沒有在跑」—— 而第一種那句話
  //   是假的:輸入法正在跑,只是還沒準備好,而使用者第一次安裝時
  //   看到的就是那一句。判斷本身在 common/service_state.h(純函式,
  //   單元測試驗得到),這裡只負責去問引擎拿事實。
  ServiceState CurrentServiceState() const;

  // 每一次按鍵的結果都會經過這裡。⚠ 這一橫顯示的是**引擎說的**狀態,
  //   不是我們以為的狀態 —— 使用者用方案自己的按鍵切了中英時,
  //   那一格必須跟著動,否則它會變成一個「說謊的指示器」。
  void OnSnapshot(const Snapshot& snap);

  // ⭐ #119:**只**更新中/英那一格,其他三格一個像素都不動。
  //
  // ⚠ 存在的理由是「一份 Result 裡有兩種可信度不同的事實」:
  //   Ctrl+空格 逾時的時候,候選窗與組字那一段我們手上沒有現況
  //   (那一份是佔位,餵進去會把使用者組字到一半的候選窗收掉),
  //   但**中英模式我們知道** —— SetAsciiModeAll() 那一句是 store +
  //   PostAsync,store 當場就生效,Engine::AsciiMode() 讀得到。
  //
  //   上一輪整份 UI 都被 DecideKeyUiAction() 擋掉,於是使用者按下
  //   Ctrl+空格、中英真的切了,而那一橫一個像素都不動 ——
  //   一個**成功**的操作被演成失敗的。
  void OnAsciiMode(bool ascii);

  /// 在還沒有任何宿主連上來之前,先讓那一橫知道方案叫什麼。
  /// 見底下 schema_name_ 的說明:**不是權威**,第一份快照到就被覆蓋。
  void SeedSchemaName(const std::string& name);

  // ── 那一橫該不該在(§12.10.6)────────────────────────────────
  //
  // ⚠ 兩層都是純函式,而且兩層在 Ubuntu 上都測得到:
  //     · **誰是這一刻的擁有者** → common/bar_owner.h(收斂 13 個宿主)
  //     · **要不要現在改可見性** → common/bar_visibility.h(遲滯)
  //   這裡只負責去問 OS 前景是誰、把兩層串起來、把結果變成 ShowWindow。
  //
  // ⚠ 上一版的訊號是「有幾條連線」,而那是一個沒有產品意義的量:
  //   12 個背景宿主每一條都算一票 —— 使用者切到微軟拼音之後那一橫
  //   照樣自己冒出來(S4)。現在每一條連線只是**一筆註冊**,
  //   投不投票由 bar_owner 拿前景那條執行緒去比。
  //
  // 下面四支都從**連線執行緒**上呼叫(PipeServer::ServeClient)。
  //   Attached   ServeClient 最頂端。此刻我們**還不知道它是誰**,
  //              所以這筆註冊 activated=false,一票都不投。
  //   Identified HELLO 之後。這時才有 pid / tid,才算一筆有效的提案。
  //   Session    SESSION_NEW 之後。那一橫回讀中英狀態時要問這一個。
  //   Detached   ServeClient 離開時(RAII)。
  void OnClientAttached(uint64_t client_id);
  void OnClientIdentified(uint64_t client_id, uint32_t host_pid,
                          uint32_t host_tid);
  void OnClientSession(uint64_t client_id, uint64_t session);
  void OnClientDetached(uint64_t client_id);
  // ⭐ 宿主**說出來**的那句話(protocol.h 的 kProfileState)。
  //
  //   ours_active == false → 這條連線所在的那條執行緒上,啟用中的
  //                          **不再是我們**。這是服務端唯一拿得到的
  //                          「別人的」正面證據(#111)。
  //   ours_active == true  → 又是我們了,作廢那筆紀錄。
  //
  // ⚠ 紀錄鍵是 **tid**、存在 yields_,**不是**掛在連線上 —— TSF 很可能
  //   在讓位之後緊接著 Deactivate 那條執行緒上的 TIP,連線會死,
  //   而證據不可以跟著死。
  void OnClientProfileState(uint64_t client_id, bool ours_active);

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
  static LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
  static DWORD WINAPI ThreadEntry(LPVOID self);
  void ThreadMain();

  void Relayout();
  void Paint(HDC hdc);
  void PaintPopup(HDC hdc);
  int HitCell(POINT pt) const;
  void ClickCell(int cell);
  void OpenSchemaPopup();
  void ClosePopup();
  // 選單現在有幾列(還在讀的時候是 1 —— 那一列是一句話,不是一個方案)。
  int PopupRowCount() const;
  // 依目前的列數把選單擺好。⚠ 開的時候與**讀完之後換內容**都要走它:
  //   列數變了而視窗大小沒變,結果是一個切一半的選單。
  void PlacePopup();
  // ⚠ 寬度由呼叫端給(0 = 用視窗現在的寬度)。Relayout 改了寬度之後
  //   一定要重走這一支 —— 只長寬度不重擺,右端會被推出螢幕。
  void ApplyPlacement(int w_dip);
  // 「哪些窗算障礙」——判準是形狀不是身分,見 .cc 上的說明。
  std::vector<ObstacleRect> CollectFloatingBars(const WorkArea& on) const;
  // 視窗級圓角(§12.14.4)。DWM 或 region,**不得同時** —— 見實作。
  static void ApplyWindowCorners(HWND hwnd, int w_px, int h_px,
                                 int radius_px);
  void SavePlacement();
  // 把三個輸入餵給狀態機,並把結果變成 ShowWindow。只在 UI 執行緒上跑。
  void EvaluateVisibility();
  // 判斷**改變時**往 service.log 寫一行(節流鍵 = 三態 + 最後的可見性)。
  //
  // ⚠ 這一行是使用者下一次回報時我們唯一查得到的東西:它帶著前景的
  //   tid / 行程名 / 視窗類別,以及「為什麼」。⚠ exe / cls 只在真的要寫
  //   的時候才去查 —— 每 500 毫秒那一圈的成本是零。
  void LogOwnerDecision(const BarOwnerForeground& fg,
                        const BarOwnerDecision& owner, size_t n_clients,
                        bool shown);
  // 向引擎回讀一次,把結果貼進 ascii_mode_ / variant_ 並重畫。
  //
  // ⚠ 這一支取代了那三格的**樂觀寫入**。點下去之後畫面要不要變、變成
  //   什麼,由引擎說了算 —— 而不是由「我們剛剛送出去了什麼」說了算。
  void RefreshFromEngine();

  struct Cell {
    RECT rc{};
    // 目前那一格畫什麼。⚠ 空字串 = **整格略過**,不佔位置。
    //
    // ⚠ **一格就是一個字面。** 這裡以前還有 text2 / pair / second_active,
    //   給第一格「中 En」兩段並排用 —— 使用者實機回報看不出哪一個生效。
    //   欄位拿掉是刻意的:要再畫兩段就得先把欄位加回來,而那是看得見的
    //   改動,不是一句話塞進繪製迴圈裡。理由見 common/status_cells.h。
    std::wstring text;
  };

  Engine* engine_;
  SettingsStore* store_;
  SettingsWindow* settings_ = nullptr;

  HANDLE thread_ = nullptr;
  DWORD thread_id_ = 0;
  HANDLE ready_ = nullptr;
  HWND hwnd_ = nullptr;
  HWND popup_ = nullptr;

  UINT dpi_ = 96;
  FontSet fonts_;
  Theme theme_;

  std::mutex mu_;
  // 掃桌面找「別人的浮動橫條」的節流(見 CollectFloatingBars)。
  // ⚠ mutable:那一支是 const 的,而快取是它的實作細節。
  static const DWORD kBarScanMs = 500;
  mutable std::vector<ObstacleRect> bars_cache_;
  mutable DWORD bars_scanned_ms_ = 0;
  // §12.10.5 的避讓上一次把那一橫往下挪了多少像素(負值 = 往上)。
  // ⚠ 存位置時要扣回去,否則偏移會一路累積(見 SavePlacement)。
  int nudge_dy_ = 0;
  bool ascii_mode_ = false;
  // ⚠ 三態,不是布林。kHidden = 引擎沒有回報字形 → **那一格整格不顯示**。
  //   舊版是 bool simplified_,而它的來源是 kStSimplified ←
  //   rs_status.is_simplified ← `simplification` —— 一個本專案打包的方案
  //   通通沒有的開關,讀到的一直是我們自己寫進去的回音。
  VariantCell variant_ = VariantCell::kHidden;
  std::string schema_name_;

  // ⚠ 種子:引擎在管道打開之前就知道方案叫什麼了(WarmUpEngine 選好了),
  //   而這一橫在**第一個宿主連上來、而且使用者真的按了一顆鍵**之前不知道。
  //   `schema_name_` 唯一的寫入路徑是 OnSnapshot ← push_ui ← 連線執行緒,
  //   所以手動啟動服務(或剛裝完還沒去打字)時它一直是空字串,
  //   而空的那一格照 §8.12 整格略過 —— 使用者看到的就是「中 简 設定」三格,
  //   方案名憑空消失。使用者實機回報過這一則。
  //
  //   種子**不是**權威:OnSnapshot 一到就覆蓋它。這裡只解決「還沒有人打過字」
  //   那一段空窗期。
  // 位置錨點的快取。Relayout 在使用者打字的路徑上,不可以每次去讀設定檔。
  BarAnchor anchor_;
  bool anchor_loaded_ = false;
  bool have_snapshot_ = false;
  // ⚠ 引擎自己在線路上說「我還沒準備好」(protocol.h 的 kStDisabled)。
  //   deploy_done() 只會從 false 變成 true 一次,之後永遠是 true ——
  //   使用者按「重新整理字詞」時它不會退回去,只有這個旗標看得到。
  //   OnSnapshot 寫、UI 執行緒讀,所以是 atomic。
  std::atomic<bool> engine_not_ready_{false};

  // 只在 UI 執行緒上碰。
  std::vector<Cell> cells_;
  int hot_ = -1;
  // 第 3/4 格之間那條分隔線的 x(像素)。<0 = 這一次不畫。
  int bar_separator_x_ = -1;
  // 第 3 格被壓到 120 DIP 了嗎(決定要不要 DT_END_ELLIPSIS)。
  bool schema_truncated_ = false;
  int pressed_ = -1;
  ServiceState service_state_ = ServiceState::kReady;
  // ⚠ 這裡以前是一個 visible_,而它同時是兩件事:「使用者要不要這個
  //   東西」與「現在螢幕上有沒有」。混在一起就沒辦法自動隱藏 ——
  //   藏起來會被讀成「使用者關掉了」,再也不會自己回來。
  //
  //   user_enabled_ = 設定檔那一格(appearance.floatingBar),只由
  //   WM_RIME_SHOW 改。shown_ = 狀態機算出來的,ShowWindow 只由它驅動。
  bool user_enabled_ = true;
  bool shown_ = false;
  BarVisibility visibility_;

  // ── 每一條連線一筆註冊(**不是**一張票)────────────────────
  //
  // 連線執行緒寫、UI 執行緒讀,所以要鎖。⚠ 與 mu_ 分開:mu_ 護的是
  // 畫面上那幾個字,而這一份在每一條連線的頭尾都會被碰,兩者混用等於
  // 讓繪製去等連線執行緒。
  std::mutex reg_mu_;
  std::vector<BarOwnerClient> regs_;
  // ── 「這條執行緒說過:啟用中的不再是我們」──────────────────────
  //
  // 由 reg_mu_ 保護(與 regs_ 同一把鎖:兩者永遠一起被讀)。
  //
  // ⚠ **它刻意不隨連線消滅。** OnClientDetached 一個字都不准碰它 ——
  //   連線死掉不是「使用者換了輸入法」的證據,只是「這個宿主不見了」
  //   (宿主被砍掉、宿主凍結、宿主根本沒載入我們,在服務端長得一模一樣)。
  //   把它綁回連線壽命是這個設計最自然、最容易被「順手修好」的一步,
  //   而那一步一做,#111 立刻復活:截圖之後那一橫又會不見。
  //   audit_single_source.sh 有一條負面守門盯著這件事。
  //
  // 上限 64 筆,滿了丟最舊的(使用者機器上是 13 個宿主的量級)。
  std::vector<BarOwnerYield> yields_;

  // ── 只在那一橫自己的 UI 執行緒上碰 ──────────────────────────
  //
  // 上一次裁決的結果。⚠ 前景答不了這個問題時**維持這個值**:
  //   OS 答不出前景(UAC 提示、安全桌面),或前景是**服務自己的
  //   視窗**(設定視窗 / 托盤選單)—— 見 bar_owner.h 的 undecidable。
  bool in_use_ = false;
  // 使用者此刻正在打字的那個 session。0 = 他還沒打過字(切過來而已),
  // 或前景那條執行緒不是我們的。回讀中英狀態時問這一個,**不是**
  // sessions_.begin() —— 13 個宿主各有自己的 ascii_mode,挑第一個
  // 等於擲骰子,而那正是使用者回報的「點了那一格沒反應」。
  uint64_t focused_session_ = 0;
  // ── 記錄節流(只在那一橫自己的 UI 執行緒上碰)────────────────
  //
  // ⚠ 節流鍵刻意**不含 tid**:含了的話使用者每點一次視窗就多一行,
  //   貼上來又是一面牆。而三態從 kOurs 變 kHold 的那一刻本來就會寫
  //   一行,那一行裡就帶著截圖工具的 tid 與檔名 —— 一格就指得出來。
  // ⚠ ever_logged_ 讓服務起來之後**第一次**判斷必定寫一行,否則初始值
  //   剛好等於現況時整份記錄是空的,而使用者貼不出任何東西。
  BarOwnerVerdict last_logged_verdict_ = BarOwnerVerdict::kHold;
  bool last_logged_shown_ = false;
  bool ever_logged_ = false;
  // 上一次「切一下」真正落地的時刻(GetTickCount)。⚠ 在它之前**產生**
  //   的滑鼠訊息一律丟掉 —— 回讀會擋住這條 UI 執行緒(引擎佇列可以被
  //   佔住好幾秒,見 #93 / #103),那段期間使用者會一直點,而每一下都
  //   是一次翻轉。N 下 = N 次翻轉,奇數次結束時引擎在 ASCII。
  //   冪等的對象是**意圖**,不是「點擊」。
  DWORD toggle_settled_ms_ = 0;

  // 拖動。⚠ 不用 WM_NCHITTEST 回 HTCAPTION —— 那會讓整條都變成拖動區,
  //   四格就點不到了;留一小塊「握把」又要使用者去找它。
  bool dragging_ = false;
  bool drag_moved_ = false;
  POINT drag_start_{};
  POINT drag_origin_{};

  std::vector<std::pair<std::string, std::string>> popup_items_;
  int popup_hot_ = -1;
  // ── ⚠ 這一格以前是「擋 UI 執行緒 1.5 秒」 ──────────────────────
  //
  //   舊版:`if (!engine_->SchemaListForUi(1500, &popup_items_)) return;`
  //   跑在懸浮那一橫自己的 UI 執行緒上,而那條執行緒停住的樣子是
  //   「那一橫還在畫面上,但點不動也拖不動」。逾時之後什麼都不做 ——
  //   使用者按了一下沒事發生。
  //
  //   而 BeginDeploy 會 InvalidateSchemaCache(),所以**整個部署期間**
  //   每一次按都走冷快取:1.5 秒凍結 + 選單不開 + 佇列多一件沒有人讀的
  //   工作,可以無限累積。
  //
  //   現在:快取有就直接開;沒有就**立刻**開一個說得出話的選單
  //   (kStatusBarSchemaLoading),背景查完再換成真的清單。
  bool popup_loading_ = false;
  // 已經有一件查詢在飛。⚠ 沒有這個閘就是「按 20 下 = 佇列裡 20 件」。
  //   只在那一橫自己的執行緒上讀寫(開選單、收 WM_RIME_SCHEMAS_READY),
  //   所以不需要 atomic。
  bool schema_query_inflight_ = false;
};

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_STATUS_BAR_H_
