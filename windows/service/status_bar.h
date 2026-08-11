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

  /// 在還沒有任何宿主連上來之前,先讓那一橫知道方案叫什麼。
  /// 見底下 schema_name_ 的說明:**不是權威**,第一份快照到就被覆蓋。
  void SeedSchemaName(const std::string& name);

  // ── 那一橫該不該在(§12.10.6)────────────────────────────────
  //
  // ⚠ 判準本身在 common/bar_visibility.h(純函式,Ubuntu 上測得到)。
  //   這裡只負責把三個輸入餵給它,以及把結果變成 ShowWindow。
  //
  // ⚠ 用**連線生死**當主要訊號,而不是只用焦點:ipc_client.cc 的
  //   `if (!MayEatKey()) return;` —— 焦點訊號在使用者打第一個字之前
  //   根本不送。連線生死沒有這道閘。
  //
  // 這兩支從**連線執行緒**上呼叫(PipeServer::ServeClient 的頭尾)。
  void OnClientAttached();
  void OnClientDetached();
  // 宿主說焦點來了/走了。從連線執行緒上呼叫(Op::kFocus)。
  void OnHostFocus(bool focused);

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
  void SavePlacement();
  // 把三個輸入餵給狀態機,並把結果變成 ShowWindow。只在 UI 執行緒上跑。
  void EvaluateVisibility();
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

  // 目前握著連線的宿主數。連線執行緒寫、UI 執行緒讀,所以是 atomic。
  // ⚠ **不是**「有幾個輸入框」。同一支程式裡跳輸入框不會動到它,
  //   而那正是我們要的:那種時候那一橫不該閃。
  std::atomic<int> clients_{0};
  // 收到過任何焦點訊息沒有。為假時**不看** any_focused_(fail-visible)。
  std::atomic<bool> focus_known_{false};
  std::atomic<bool> any_focused_{false};

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
