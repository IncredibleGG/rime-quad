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
//   · `中`/`En` 兩態**同時顯示**,`简`/`繁` 依 is_simplified。
//     **不得**只顯示一個字(「中」有兩種讀法,Android 被真機回報過),
//     **不得**在地化(§12.9.3)—— 所以那四個字面直接寫在 .cc 的繪製碼裡,
//     不進 ui_strings.cc,而 W10 兩個方向都驗。
//   · 空狀態**整項略過**:方案名還沒載入完成時,那一格完全不佔位置,
//     不得畫成一塊看不出用途的空白。
//   · **不得加圖示。** §8.12 記著一個還沒關的缺口,在補上之前
//     「任何『一顆代表當前方案的小圖示』都是各端自己發明的」。
//
// ── ⚠ 為什麼預設是開的 ──────────────────────────────────────────
//
// `ascii_mode` 在這一輪之前,整個 `windows/` 底下只被**讀**過兩次,
// **一次都沒有被設定過**。三條可能的路全部不通:Shift(TSF 不交付純修飾鍵)、
// 語言列(InitMenu 回 E_NOTIMPL)、系統匣(沒有中英那一項)。
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
#include "../common/service_state.h"
#include "../common/statusbar_place.h"
#include "ui_font.h"
#include "ui_theme.h"

namespace rimewin {

class Engine;
class SettingsWindow;
class SettingsStore;

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
  // ⚠ 寬度由呼叫端給(0 = 用視窗現在的寬度)。Relayout 改了寬度之後
  //   一定要重走這一支 —— 只長寬度不重擺,右端會被推出螢幕。
  void ApplyPlacement(int w_dip);
  void SavePlacement();

  struct Cell {
    RECT rc{};
    // 目前那一格畫什麼。⚠ 空字串 = **整格略過**,不佔位置。
    std::wstring text;
    // 兩態同時顯示的那一格(中/En)才用得到:第二段的文字與哪一段是當前態。
    std::wstring text2;
    bool pair = false;
    bool second_active = false;
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
  bool simplified_ = false;
  std::string schema_name_;
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
  bool visible_ = true;

  // 拖動。⚠ 不用 WM_NCHITTEST 回 HTCAPTION —— 那會讓整條都變成拖動區,
  //   四格就點不到了;留一小塊「握把」又要使用者去找它。
  bool dragging_ = false;
  bool drag_moved_ = false;
  POINT drag_start_{};
  POINT drag_origin_{};

  std::vector<std::pair<std::string, std::string>> popup_items_;
  int popup_hot_ = -1;
};

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_STATUS_BAR_H_
