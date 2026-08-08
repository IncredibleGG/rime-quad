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

#include "cand_window.h"
#include "engine.h"

namespace rimewin {

class PipeServer {
 public:
  PipeServer(Engine* engine, CandidateUi* ui);
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
  HANDLE stop_event_ = nullptr;
  HANDLE listen_thread_ = nullptr;
  std::atomic<bool> stopping_{false};
  std::mutex mu_;
  std::vector<std::thread> clients_;
};

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_PIPE_SERVER_H_
