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
#include <thread>
#include <vector>

#include <functional>

#include "cand_window.h"
#include "engine.h"
#include "settings_store.h"

namespace rimewin {

class StatusBar;

class PipeServer {
 public:
  PipeServer(Engine* engine, CandidateUi* ui, SettingsStore* settings);

  // ⚠ 懸浮狀態列顯示的是**引擎說的**中英／簡繁,不是我們以為的 ——
  //   使用者用方案自己的按鍵切了模式時,那一橫必須跟著動,
  //   否則它會變成一個說謊的指示器,而那比沒有指示器更糟。
  void SetStatusBar(StatusBar* b) { bar_ = b; }

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
  Engine::SessionPlan PlanForLang(
      uint32_t langid,
      const std::vector<std::pair<std::string, std::string>>& schemas) const;

  Engine* engine_;
  CandidateUi* ui_;
  StatusBar* bar_ = nullptr;
  SettingsStore* settings_;
  std::function<void()> on_open_settings_;
  std::function<void()> on_fatal_;
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
  std::mutex mu_;
  std::vector<std::thread> clients_;
};

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_PIPE_SERVER_H_
