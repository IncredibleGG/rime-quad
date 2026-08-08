// windows/service/engine.h — 服務進程裡的引擎(rime_shell + librime)
//
// ⚠ rime_shell.h 檔頭:「除 rs_deploy() 外,同一 session 的所有呼叫必須在
//   同一執行緒上序列化。」而這個服務會有多條連線執行緒(每個宿主進程一條)。
//
//   所以這裡的做法是**唯一一條引擎執行緒**:所有連線把工作丟進佇列,
//   引擎執行緒逐一執行,呼叫端等結果。連 session 的建立與銷毀也走同一條。
//   代價是所有輸入被序列化 —— 按鍵處理是次毫秒等級的事,不成問題;
//   而好處是那條執行緒約定變成結構上不可能違反,不必靠人記得。
//
//   rs_deploy_callback 是唯一的例外:它來自 librime 的維護執行緒,
//   而且可能在 rs_deploy() 早就返回之後才觸發。所以它只碰一個 atomic。
//
// ⚠ rs_init / rs_finalize **不在**引擎執行緒上,在呼叫端(main)那一條。
//   它們是行程層級的一次性初始化,不屬於任何 session,那條執行緒約定管不到。
//   而且這是實測的結果:丟給次要執行緒的版本每一次都在 glog 的初始化裡崩潰
//   (CI run #16–#18),詳見 engine.cc 裡 Start() 的說明。
#ifndef RIMEWIN_SERVICE_ENGINE_H_
#define RIMEWIN_SERVICE_ENGINE_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "../common/protocol.h"

namespace rimewin {

class Engine {
 public:
  Engine();
  ~Engine();

  // 先在呼叫端執行緒上做 rs_init,成功後再啟動引擎執行緒。
  // 部署是非同步的,本函式不等它完成 —— 首次部署要編譯詞庫,可能好幾分鐘,
  // 而那段時間裡使用者已經在打字了(服務會對每一顆按鍵立刻回「沒處理」)。
  bool Start(const std::string& shared_dir, const std::string& user_dir,
             const std::string& log_dir);
  void Stop();

  bool deploy_done() const { return deploy_state_.load() != 0; }
  bool deploy_ok() const { return deploy_state_.load() == 1; }
  // 等待首次部署完成。只給 --selftest 用;正常執行不等。
  bool WaitDeploy(int seconds);

  uint64_t NewSession();
  void EndSession(uint64_t id);

  Result ProcessKey(uint64_t id, int32_t keysym, uint32_t mods);
  Result SelectCandidate(uint64_t id, int32_t index);
  Result CommitComposition(uint64_t id);
  Result Clear(uint64_t id);
  Result ChangePage(uint64_t id, bool backward);
  Result SelectSchema(uint64_t id, const std::string& schema_id);

  std::string last_error() const;

 private:
  void ThreadMain();
  void Post(std::function<void()> fn);  // 丟工作並等它做完

  // 以下三個只在引擎執行緒上呼叫。
  Snapshot TakeSnapshot(uint64_t id);
  Snapshot TakeSnapshotLocked(uintptr_t sess);
  uintptr_t Find(uint64_t id) const;

  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
  bool stop_ = false;
  bool started_ = false;

  std::map<uint64_t, uintptr_t> sessions_;
  uint64_t next_id_ = 1;

  // 0 = 還沒有結果, 1 = 成功, -1 = 失敗。只由部署回呼寫,別處只讀。
  std::atomic<int> deploy_state_{0};
  std::string init_error_;
};

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_ENGINE_H_
