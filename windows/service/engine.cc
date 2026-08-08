#include "engine.h"

#include <chrono>
#include <cstdio>

#include "../common/ime_policy.h"
#include "rime_shell.h"

namespace rimewin {
namespace {

std::atomic<int>* g_deploy_slot = nullptr;

void OnDeploy(rs_deploy_status status, void* /*ud*/) {
  // ⚠ 這個回呼**不在**呼叫端的執行緒上,而且可能在 rs_deploy() 早已返回
  //   之後才觸發(rime_shell.h 檔頭)。所以這裡只碰一個 atomic:
  //   不加鎖、不碰 session、不碰 UI。
  if (!g_deploy_slot) return;
  if (status == RS_DEPLOY_SUCCESS) g_deploy_slot->store(1);
  else if (status == RS_DEPLOY_FAILURE) g_deploy_slot->store(-1);
}

uint32_t FlagsOf(const rs_status& s) {
  uint32_t f = 0;
  if (s.is_composing) f |= kStComposing;
  if (s.is_ascii_mode) f |= kStAsciiMode;
  if (s.is_full_shape) f |= kStFullShape;
  if (s.is_simplified) f |= kStSimplified;
  if (s.is_ascii_punct) f |= kStAsciiPunct;
  if (s.is_disabled) f |= kStDisabled;
  return f;
}

Snapshot Convert(const rs_snapshot* s) {
  Snapshot out;
  if (!s) return out;
  if (s->commit_text && *s->commit_text) {
    out.has_commit = true;
    out.commit_text = s->commit_text;
  }
  out.preedit = s->composition.preedit ? s->composition.preedit : "";
  out.sel_start = s->composition.sel_start;
  out.sel_end = s->composition.sel_end;
  out.caret = s->composition.caret;
  out.items.reserve(static_cast<size_t>(s->menu.count > 0 ? s->menu.count : 0));
  for (int32_t i = 0; i < s->menu.count; ++i) {
    Candidate c;
    c.text = s->menu.items[i].text ? s->menu.items[i].text : "";
    c.comment = s->menu.items[i].comment ? s->menu.items[i].comment : "";
    c.label = s->menu.items[i].label ? s->menu.items[i].label : "";
    out.items.push_back(std::move(c));
  }
  out.page_no = s->menu.page_no;
  out.highlighted = s->menu.highlighted;
  out.is_last_page = s->menu.is_last_page;
  out.schema_id = s->status.schema_id ? s->status.schema_id : "";
  out.schema_name = s->status.schema_name ? s->status.schema_name : "";
  out.status_flags = FlagsOf(s->status);
  return out;
}

// 啟動階段的標記。只在啟動時印幾行,不是熱路徑。
//
// 存在的理由很具體:CI run #16/#17 上 rime_service.exe 在 rs_init 附近
// 以 0xC0000005 崩潰,而同一個 job 裡的 rime_console(同一份 rime_shell.cc、
// 同一批靜態庫)是綠的。沒有這幾行的話,「崩在我的執行緒機制裡」與
// 「崩在 librime 裡」分不開,而那兩者的修法完全不同。
void Mark(const char* what) {
  std::fprintf(stderr, "[engine] %s\n", what);
  std::fflush(stderr);
}

}  // namespace

Engine::Engine() {}

Engine::~Engine() { Stop(); }

bool Engine::Start(const std::string& shared_dir, const std::string& user_dir,
                   const std::string& log_dir) {
  g_deploy_slot = &deploy_state_;
  bool ok = false;
  thread_ = std::thread(&Engine::ThreadMain, this);
  {
    std::unique_lock<std::mutex> lock(mu_);
    started_ = true;
  }
  Mark("Post 之前(主執行緒)");
  Post([&] {
    Mark("進入引擎執行緒");
    rs_setup setup{};
    setup.shared_data_dir = shared_dir.c_str();
    setup.user_data_dir = user_dir.c_str();
    // ⚠ 空字串與 NULL 語意不同(rime_shell.h 檔頭):"" = 只寫 stderr,
    //   NULL = 交給 librime 決定暫存目錄。這裡一律傳字串,空的就是 ""。
    //   tools/rime_console.cc 走的正是 "" 那一條,而那條在這個 runner 上
    //   已經驗證過很多次;NULL 那條沒有人走過。不要在服務進程上開新路。
    setup.log_dir = log_dir.c_str();
    setup.app_name = "rime.windows";
    setup.on_deploy = &OnDeploy;
    Mark("呼叫 rs_init");
    ok = rs_init(&setup);
    Mark(ok ? "rs_init 回傳 true" : "rs_init 回傳 false");
    if (!ok) init_error_ = rs_last_error();
    Mark("rs_last_error 讀取完畢");
  });
  Mark("Post 返回");
  return ok;
}

void Engine::Stop() {
  {
    std::unique_lock<std::mutex> lock(mu_);
    if (!started_) return;
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  started_ = false;
  g_deploy_slot = nullptr;
}

void Engine::ThreadMain() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
      if (stop_ && queue_.empty()) break;
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    job();
    cv_.notify_all();
  }
  // 收尾也在同一條執行緒上 —— session 的銷毀同樣受那條執行緒約定管。
  for (auto& kv : sessions_) rs_session_destroy(kv.second);
  sessions_.clear();
  rs_finalize();
}

void Engine::Post(std::function<void()> fn) {
  bool done = false;
  {
    std::unique_lock<std::mutex> lock(mu_);
    if (stop_) return;
    queue_.push_back([&fn, &done, this] {
      fn();
      std::unique_lock<std::mutex> l2(mu_);
      done = true;
    });
  }
  cv_.notify_all();
  std::unique_lock<std::mutex> lock(mu_);
  // 只等 done,不等 stop_。ThreadMain 在收到 stop 之後仍會把佇列排乾,
  // 所以每一個**已經入列**的工作都保證會被執行。若這裡順便等 stop_,
  // Post 會在工作還沒跑之前就返回,而那個工作捕捉的是這個堆疊上的變數。
  cv_.wait(lock, [&] { return done; });
}

bool Engine::WaitDeploy(int seconds) {
  for (int i = 0; i < seconds * 10 && deploy_state_.load() == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  return deploy_state_.load() == 1;
}

uintptr_t Engine::Find(uint64_t id) const {
  auto it = sessions_.find(id);
  return it == sessions_.end() ? 0 : it->second;
}

uint64_t Engine::NewSession() {
  uint64_t id = 0;
  Post([&] {
    const rs_session s = rs_session_create();
    if (s == RS_INVALID_SESSION) return;
    id = next_id_++;
    sessions_[id] = s;
  });
  return id;
}

void Engine::EndSession(uint64_t id) {
  Post([&] {
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;
    rs_session_destroy(it->second);
    sessions_.erase(it);
  });
}

Snapshot Engine::TakeSnapshotLocked(uintptr_t sess) {
  // ⚠ commit 在 acquire **當下**就被消費,不是在 release。
  //   所以每一個輸入事件只可以 acquire 一次,並在那一次就把 commit 收走。
  const rs_snapshot* raw = rs_snapshot_acquire(sess);
  Snapshot out = Convert(raw);
  rs_snapshot_release(sess);

  // 「選字」不等於「上屏」。判別條件是 menu.count,不是 is_composing。
  const bool composing = (out.status_flags & kStComposing) != 0;
  if (ShouldCommitComposition(composing, static_cast<int32_t>(out.items.size()))) {
    if (rs_commit_composition(sess)) {
      // 這是一次**新的**事件(我們主動要求上屏),所以可以再 acquire 一次。
      const rs_snapshot* raw2 = rs_snapshot_acquire(sess);
      Snapshot after = Convert(raw2);
      rs_snapshot_release(sess);
      if (out.has_commit && after.has_commit) {
        // 先前那一段先上屏,順序不能反 —— 反了使用者看到的是字序顛倒。
        after.commit_text = out.commit_text + after.commit_text;
      } else if (out.has_commit) {
        after.has_commit = true;
        after.commit_text = out.commit_text;
      }
      return after;
    }
  }
  return out;
}

Snapshot Engine::TakeSnapshot(uint64_t id) {
  const uintptr_t sess = Find(id);
  if (!sess) return Snapshot();
  return TakeSnapshotLocked(sess);
}

Result Engine::ProcessKey(uint64_t id, int32_t keysym, uint32_t mods) {
  Result r;
  Post([&] {
    const uintptr_t sess = Find(id);
    if (!sess) return;
    // 部署還沒完成時 librime 給不出任何候選。這時**立刻**回「沒處理」,
    // 讓宿主自己收下那顆鍵 —— 不要讓 DLL 那邊逾時。
    // 逾時會讓連線被關掉重建,而首次部署要好幾分鐘,那段時間會一直重連。
    if (deploy_state_.load() != 1) {
      r.handled = false;
      r.snap.status_flags = kStDisabled;
      return;
    }
    r.handled = rs_process_key(sess, keysym, mods);
    r.snap = TakeSnapshotLocked(sess);
  });
  return r;
}

Result Engine::SelectCandidate(uint64_t id, int32_t index) {
  Result r;
  Post([&] {
    const uintptr_t sess = Find(id);
    if (!sess) return;
    r.handled = rs_select_candidate(sess, index);
    r.snap = TakeSnapshotLocked(sess);
  });
  return r;
}

Result Engine::CommitComposition(uint64_t id) {
  Result r;
  Post([&] {
    const uintptr_t sess = Find(id);
    if (!sess) return;
    r.handled = rs_commit_composition(sess);
    r.snap = TakeSnapshotLocked(sess);
  });
  return r;
}

Result Engine::Clear(uint64_t id) {
  Result r;
  Post([&] {
    const uintptr_t sess = Find(id);
    if (!sess) return;
    r.handled = rs_clear_composition(sess);
    r.snap = TakeSnapshotLocked(sess);
  });
  return r;
}

Result Engine::ChangePage(uint64_t id, bool backward) {
  Result r;
  Post([&] {
    const uintptr_t sess = Find(id);
    if (!sess) return;
    r.handled = rs_change_page(sess, backward);
    r.snap = TakeSnapshotLocked(sess);
  });
  return r;
}

std::string Engine::last_error() const { return init_error_; }

}  // namespace rimewin
