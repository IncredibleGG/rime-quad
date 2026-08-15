// windows/service/pipe_server.h — 具名管道伺服器
//
// ⚠ 這條管道上流的是**使用者的每一次按鍵**。所以 DACL 不是可以之後再補的
//   細節:預設的具名管道 DACL 允許同一台機器上的其他使用者連進來。
//   這裡明確只授權目前這個使用者的 SID。
#ifndef RIMEWIN_SERVICE_PIPE_SERVER_H_
#define RIMEWIN_SERVICE_PIPE_SERVER_H_

#include <windows.h>

#include <atomic>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <functional>

#include "../common/log_rate.h"
#include "cand_window.h"
#include "engine.h"
#include "settings_store.h"

namespace rimewin {

class StatusBar;
class SettingsWindow;

class PipeServer {
 public:
  PipeServer(Engine* engine, CandidateUi* ui, SettingsStore* settings);

  // ⚠ 懸浮狀態列顯示的是**引擎說的**中英／簡繁,不是我們以為的 ——
  //   使用者用方案自己的按鍵切了模式時,那一橫必須跟著動,
  //   否則它會變成一個說謊的指示器,而那比沒有指示器更糟。
  void SetStatusBar(StatusBar* b) { bar_ = b; }

  // ⚠ 簡繁快捷鍵(Ctrl+Shift+F,G76)要**走與狀態列第二格、設定視窗
  //   完全同一支寫入**(SettingsWindow::CommitVariantPref)。各寫一份
  //   會漂移,而漂移的樣子是「從這裡切有效、從那裡切無效」——
  //   使用者猜不到差別在哪,而畫面上兩邊看起來都正常。
  //   ⚠ nullptr 是合法的(--no-ui 的 CI 模式),那時那顆鍵不做事、
  //     交回宿主,而不是假裝切了。
  void SetSettingsWindow(SettingsWindow* w) { settings_window_ = w; }

  // 收到 Op::kOpenSettings 時呼叫。可以是 nullptr(--no-ui 的 CI 模式)。
  // ⚠ 這個回呼會在**連線執行緒**上跑,實作必須只是 PostMessage。
  void SetOpenSettingsHandler(std::function<void()> fn) {
    on_open_settings_ = std::move(fn);
  }
  // 監聽迴圈在**沒有人要求停止**的情況下結束時呼叫。
  //
  // ⚠ 這不是可有可無的通知。監聽迴圈死掉之後,這支服務會變成一具空殼:
  //   它還活著、還佔著單一實例的 mutex,但沒有管道 —— 於是 DLL 那邊
  //   永遠連不上(fail-open,中文輸入整個消失),而且**再也啟動不了
  //   第二支服務**,因為 mutex 在這一支手上。使用者要重開機才會好。
  //   所以主程式收到這個回呼時應該讓服務結束,把位置讓出來。
  void SetFatalHandler(std::function<void()> fn) { on_fatal_ = std::move(fn); }

  // ── 記錄檔長太大時的兜底 ────────────────────────────────────
  //
  // ⚠ 這不是節流(節流是下面的 conn_log_)。service/main.cc 的大小檢查
  //   **一輩子只在行程啟動時做一次**,所以跑的期間 service.log 只會長。
  //   服務沒有閒置離開的路徑 —— 一台被開著好幾天的機器上,那個檔案會
  //   一路長過 1 MiB,而那才是「檔案無限長」這個風險的本體。
  //
  // ⚠ 掛在**每一條新連線**上,不是每一顆按鍵:一次 GetFileAttributesExW
  //   是磁碟 I/O,而按鍵那條路上多一次磁碟 I/O 正是 #108 的第二名成因。
  //   連線是稀有事件(登入那一波 26 條,之後零星),量級剛好。
  void SetLogCheckpoint(std::function<void()> fn) {
    on_log_checkpoint_ = std::move(fn);
  }

  // 候選窗上的滾輪。⚠ **在候選窗自己的 UI 執行緒上跑**,不是連線執行緒。
  //   由建構子掛上、解構子拿掉(候選窗比我們晚死,見 service/main.cc 的
  //   宣告順序)。
  void OnCandidateWheel(int32_t steps);

  ~PipeServer();

  // ⚠ 回傳 true 時,管道**確定已經接得起連線** —— 不是「大概快好了」。
  //
  //   舊版不是這樣:它建一個實例、關掉、開一條執行緒去重建,然後在
  //   CreateThread 成功的當下就回 true。那條執行緒可能還一步都沒跑,
  //   而呼叫端(service/main.cc)緊接著就寫 ready 檔並印出 ready。
  //   於是「ready 檔存在」不代表「連得上」,而 CI 的驗證腳本正是等
  //   ready 檔才跑 probe。
  //
  //   現在 Start() 會等監聽執行緒親自回報「ConnectNamedPipe 已經掛上」
  //   才返回。這不是睡一下賭它好了 —— 是一個真的訊號。
  bool Start();
  void Stop();

 private:
  static DWORD WINAPI ListenEntry(LPVOID self);
  void ListenLoop();
  void ServeClient(HANDLE pipe);
  // err 收 CreateNamedPipeW 失敗時的 GetLastError()。失敗的原因必須傳得出來:
  // ERROR_ACCESS_DENIED(有人佔著同名管道)與 ERROR_INVALID_PARAMETER
  // 的修法完全不同,而「建不起來」三個字對兩者都成立。
  HANDLE CreateInstance(bool first, DWORD* err);

  // 「這個語言該用哪個方案、要套哪些選項」。
  //
  // ⚠ **兩個呼叫點共用這一支**:宿主的 SESSION_NEW,以及部署之後把
  //   session 建回來時的重套(engine.h 的 SessionPlanner / #85)。
  //   寫成兩份的話會漂移,而漂移的症狀是「按一次重新整理字詞,
  //   釘好的方案與簡繁悄悄回到預設」——使用者不會把那兩件事聯想在一起。
  //
  // ⚠ 方案清單由呼叫端傳進來,不在這裡問。重建那條路是在**引擎執行緒**
  //   上跑的,在那裡再丟一件工作進引擎佇列就是自己等自己。
  // 簡繁切一下。回傳 false = **什麼都沒做**(沒有設定視窗、引擎沒有
  // 回報字形)——呼叫端據此宣告沒有吃掉那顆鍵,而且**不碰 UI**。
  //
  // ⚠ 它在按鍵那條路上,而且要走一趟引擎佇列(ReadBackStatus),所以
  //   要吃這顆鍵剩下的預算。逾時 = 什麼都沒做,與回 false 同一個處置。
  bool ToggleVariantPref(int deadline_ms, Engine::KeyWait* wait);

  Engine::SessionPlan PlanForLang(
      uint32_t langid,
      const std::vector<std::pair<std::string, std::string>>& schemas) const;

  Engine* engine_;
  CandidateUi* ui_;
  StatusBar* bar_ = nullptr;
  SettingsWindow* settings_window_ = nullptr;
  SettingsStore* settings_;
  std::function<void()> on_open_settings_;
  std::function<void()> on_fatal_;
  std::function<void()> on_log_checkpoint_;
  HANDLE stop_event_ = nullptr;
  HANDLE listen_thread_ = nullptr;
  // Start() 建好的第一個實例。**不關掉**,直接交給監聽執行緒 ——
  // 「建了再關、關了再建」中間那一段時間裡管道名根本不存在,
  // 而用戶端在那一瞬間拿到的 ERROR_FILE_NOT_FOUND 與「服務沒跑」
  // 一模一樣。
  HANDLE first_instance_ = INVALID_HANDLE_VALUE;
  // 監聽執行緒掛上 ConnectNamedPipe 之後設它。Start() 等的就是這個。
  HANDLE listening_event_ = nullptr;
  std::atomic<bool> stopping_{false};
  // ── 「現在螢幕上那一頁是誰的」──────────────────────────────
  //
  // ⚠ 快照本身**沒有** session id(protocol.h 的 Snapshot),而候選窗
  //   是服務自己的視窗、不屬於任何一條連線。滾輪要翻頁就得知道翻誰的,
  //   所以由 push_ui 在推畫面的同時記下來。
  // ⚠ 沒有候選時清成 0:留著的話,候選窗收起來之後滾輪仍然會去翻一個
  //   看不見的 session —— 使用者在別的地方捲網頁,我們在背後動他的
  //   組字狀態,而畫面上什麼都看不出來。
  std::mutex ui_mu_;
  uint64_t ui_session_ = 0;
  RECT ui_caret_{};
  // ── 「這個宿主抱著升級前的那一份 DLL」只記一次 ────────────────
  //
  // ⚠ 節流的鍵是 **host_pid**,不是連線 —— 連線會斷會重連,而使用者
  //   機器上有 13 個宿主。上限 64 筆(超過就不再記):那個量級之後,
  //   那一行的診斷價值已經被稀釋掉了,而記錄的長度是使用者的成本。
  std::mutex old_proto_mu_;
  std::set<uint32_t> old_proto_seen_;
  // ── 連線進場 / 離場那兩行的節流 ────────────────────────────────
  //
  // ⚠ **上一版寫的理由是錯的**,而且錯的方向剛好讓這條線在使用者
  //   真正需要的時候是空的。原話是「service.log 是 1 MiB 的環形檔…
  //   先被捲掉的正好是稀有的那一種」。查證:service/main.cc 的大小檢查
  //   **一輩子只在行程啟動時做一次**(too_big → freopen "w",否則 "a")。
  //   跑的期間那個檔案只會長,**沒有任何一行會被捲掉**;超過 1 MiB 時
  //   被丟掉的是整個檔案,而且發生在下一次啟動。
  //
  // ⚠ 於是「一輩子 64 條」這道額度擋掉的不是雜訊,**是最近的訊號**。
  //   服務沒有閒置離開的路徑,所以 64 用完之後,從登入到關機再也不會有
  //   第二行連線記錄 —— 而 64 很快就沒:13 個宿主 × 每個兩條連線,
  //   光登入就 26 條。使用者下午再撞到 #108、照慣例被請去撈 service.log
  //   時,唯一能在服務端分辨「重連迴圈」與「引擎佇列塞住」的那組行
  //   (存活=300ms、按鍵=1、逾時=1 連續好幾行)早就不寫了。
  //
  // 所以換成**速率限制**:令牌桶(common/log_rate.h)。
  //   · 桶滿 = 登入那一波寫得出 10 條,「連續好幾行」這個樣式讀得出來。
  //   · 之後每分鐘回補一條 —— 八小時之後那組行**還在寫**,而這是這一輪
  //     對這條線唯一的硬要求(tests/test_log_rate.cc 逐條驗)。
  //   · 被壓掉的則數不會消失,由下一行帶出去。那個數字本身就是重連
  //     迴圈最強的訊號:一分鐘 137 條連線,比 10 行「存活=300ms」還清楚。
  //
  // ⚠ 節流的單位是「多少**條連線**」而不是「多少行」:進場與離場必須
  //   成對,只有離場那一行的話,「存活多久」對不到任何一條連線。
  //   同一條連線的兩行由 ServeClient 開頭一次決定(見 .cc)。
  //
  // ⚠ 「檔案無限長」這個風險仍然是真的(一台開著好幾天的機器),
  //   但那一半由 SetLogCheckpoint() 的執行期大小檢查兜底,不是靠
  //   「再也不寫」。
  std::mutex conn_log_mu_;
  LogTokenBucket conn_log_{kConnLogBurst, kConnLogRefillMs};
  std::mutex mu_;
  std::vector<std::thread> clients_;
};

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_PIPE_SERVER_H_
