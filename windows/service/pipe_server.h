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

class PipeServer {
 public:
  PipeServer(Engine* engine, CandidateUi* ui, SettingsStore* settings);

  // 收到 Op::kOpenSettings 時呼叫。可以是 nullptr(--no-ui 的 CI 模式)。
  // ⚠ 這個回呼會在**連線執行緒**上跑,實作必須只是 PostMessage。
  void SetOpenSettingsHandler(std::function<void()> fn) {
    on_open_settings_ = std::move(fn);
  }
  ~PipeServer();

  bool Start();
  void Stop();

 private:
  static DWORD WINAPI ListenEntry(LPVOID self);
  void ListenLoop();
  void ServeClient(HANDLE pipe);
  HANDLE CreateInstance(bool first);

  Engine* engine_;
  CandidateUi* ui_;
  SettingsStore* settings_;
  std::function<void()> on_open_settings_;
  HANDLE stop_event_ = nullptr;
  HANDLE listen_thread_ = nullptr;
  std::atomic<bool> stopping_{false};
  std::mutex mu_;
  std::vector<std::thread> clients_;
};

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_PIPE_SERVER_H_
