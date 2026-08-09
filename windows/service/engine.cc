#include "engine.h"

#include <chrono>
#include <cstdio>

#include "../common/ime_policy.h"
#include "rime_shell.h"

namespace rimewin {
namespace {

std::atomic<int>* g_deploy_slot = nullptr;
std::atomic<uint32_t>* g_deploy_seq = nullptr;

void OnDeploy(rs_deploy_status status, void* /*ud*/) {
  // ⚠ 這個回呼**不在**呼叫端的執行緒上,而且可能在 rs_deploy() 早已返回
  //   之後才觸發(rime_shell.h 檔頭)。所以這裡只碰 atomic:
  //   不加鎖、不碰 session、不碰 UI。
  if (!g_deploy_slot) return;
  if (status == RS_DEPLOY_SUCCESS) g_deploy_slot->store(1);
  else if (status == RS_DEPLOY_FAILURE) g_deploy_slot->store(-1);
  else return;  // IDLE / RUNNING 不是終局,不動序號
  // 序號一定要在狀態之後才加:讀的那一邊先看序號再讀狀態,
  // 反過來的話它會看到新序號配舊狀態。
  if (g_deploy_seq) g_deploy_seq->fetch_add(1);
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
  g_deploy_seq = &deploy_seq_;

  // ⚠ rs_init 在**呼叫端的執行緒**上做,不丟給引擎執行緒。
  //
  // rime_shell.h 要求的是「同一 session 的所有呼叫必須在同一執行緒上序列化」。
  // rs_init / rs_finalize 是行程層級的一次性初始化,不屬於任何 session,
  // 所以這條規則管不到它們。
  //
  // 而這裡有一個實測的理由:CI run #16–#18 上,把 rs_init 丟到次要執行緒的
  // 版本每一次都在 **glog 的初始化裡**以 0xC0000005 崩潰
  // (RVA 對到 glog utilities.cc 的 ProgramInvocationShortName 附近),
  // 而同一個 job 裡的 tools/rime_console.cc —— 同一份 rime_shell.cc、
  // 同一批靜態庫、同一份資料 —— 在主執行緒上做同一件事完全正常。
  // 唯一的差別就是那條執行緒。librime 的 SetupLogging 在
  // google::InitGoogleLogging 之前還會呼叫 LogToStderr / SetLogSymlink,
  // 那一段在未初始化狀態下的執行緒安全性沒有保證。
  //
  // 所以:與已經驗證過的那條路走同一條。不在服務進程上開新路。
  Mark("rs_init(呼叫端執行緒)");
  {
    rs_setup setup{};
    setup.shared_data_dir = shared_dir.c_str();
    setup.user_data_dir = user_dir.c_str();
    // 空字串與 NULL 語意不同(rime_shell.h 檔頭):"" = 只寫 stderr,
    // NULL = 交給 librime 決定暫存目錄。rime_console 走的是 "" 那一條。
    setup.log_dir = log_dir.c_str();
    setup.app_name = "rime.windows";
    setup.on_deploy = &OnDeploy;
    if (!rs_init(&setup)) {
      init_error_ = rs_last_error();
  last_error_ = init_error_;
      Mark("rs_init 失敗");
      return false;
    }
  }
  Mark("rs_init OK,啟動引擎執行緒");

  // 引擎執行緒:從這裡開始,**所有** session 相關的呼叫都在它身上,
  // 那條執行緒約定因此變成結構上不可能違反,不必靠人記得。
  thread_ = std::thread(&Engine::ThreadMain, this);
  {
    std::unique_lock<std::mutex> lock(mu_);
    started_ = true;
  }
  return true;
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
  // rs_finalize 與 rs_init 在同一條執行緒上。引擎執行緒已經 join,
  // 所以它先前建立的 session 都已經在它身上銷毀了,順序是對的。
  rs_finalize();
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
  // session 的銷毀必須在建立它的那條執行緒上 —— 也就是這裡。
  // rs_finalize 則留給 Stop()(與 rs_init 同一條執行緒)。
  for (auto& kv : sessions_) rs_session_destroy(kv.second);
  sessions_.clear();
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

void Engine::PostAsync(std::function<void()> fn) {
  {
    std::unique_lock<std::mutex> lock(mu_);
    if (stop_) return;
    // ⚠ **移動進佇列,不可以捕捉參考。** Post 捕捉參考是安全的,因為它
    //   會站在原地等到工作跑完;這一支丟了就走,呼叫端的堆疊在工作真正
    //   執行之前就已經不存在了。這兩支長得很像,而差別是一個懸空參考。
    queue_.push_back(std::move(fn));
  }
  cv_.notify_all();
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
    session_lang_.erase(id);
  });
}

void Engine::EndSessionAsync(uint64_t id) {
  // ⚠ 按值捕捉。這一支不等工作跑完,呼叫端的堆疊會先消失。
  PostAsync([this, id] {
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;
    rs_session_destroy(it->second);
    sessions_.erase(it);
    session_lang_.erase(id);
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

Result Engine::SelectSchema(uint64_t id, const std::string& schema_id) {
  Result r;
  Post([&] {
    const uintptr_t sess = Find(id);
    if (!sess) return;
    r.handled = rs_select_schema(sess, schema_id.c_str());
    r.snap = TakeSnapshotLocked(sess);
  });
  return r;
}

std::vector<std::pair<std::string, std::string>> Engine::SchemaList() {
  std::vector<std::pair<std::string, std::string>> out;
  Post([&] {
    const int32_t n = rs_schema_list(nullptr, nullptr, 0);
    if (n <= 0) return;
    std::vector<const char*> ids(static_cast<size_t>(n), nullptr);
    std::vector<const char*> names(static_cast<size_t>(n), nullptr);
    const int32_t got = rs_schema_list(ids.data(), names.data(), n);
    for (int32_t i = 0; i < got && i < n; ++i) {
      out.emplace_back(ids[i] ? ids[i] : "", names[i] ? names[i] : "");
    }
  });
  return out;
}

bool Engine::SetOption(uint64_t id, const char* option, bool value) {
  bool ok = false;
  Post([&] {
    const uintptr_t sess = Find(id);
    if (!sess) return;
    ok = rs_set_option(sess, option, value);
  });
  return ok;
}

void Engine::SetOptionAll(const char* option, bool value) {
  Post([&] {
    for (const auto& kv : sessions_) rs_set_option(kv.second, option, value);
  });
}

void Engine::SetSessionLangId(uint64_t id, uint32_t langid) {
  Post([&] { session_lang_[id] = langid; });
}

void Engine::ApplyVariantAll(const SchemaPreference& pref) {
  Post([&] {
    for (const auto& kv : sessions_) {
      auto it = session_lang_.find(kv.first);
      const uint32_t lang = (it == session_lang_.end()) ? 0u : it->second;
      bool simplified = false;
      // ⚠ 與建 session 時走的是**同一支** DecideVariant。兩份會漂移,
      //   而漂移的症狀是「改設定當下沒變、換個程式就變了」。
      if (!DecideVariant(lang, pref, &simplified)) continue;
      for (const OptionAssign& a : PlanVariant(simplified, lang))
        rs_set_option(kv.second, a.option, a.value);
    }
  });
}

void Engine::SelectSchemaAll(const std::string& schema_id) {
  Post([&] {
    for (const auto& kv : sessions_) rs_select_schema(kv.second, schema_id.c_str());
  });
}

std::string Engine::SchemaOfSession(uint64_t id) {
  std::string out;
  Post([&] {
    const uintptr_t sess = Find(id);
    if (!sess) return;
    const rs_snapshot* s = rs_snapshot_acquire(sess);
    if (s && s->status.schema_id) out = s->status.schema_id;
    // ⚠ acquire 一定要配對 release。而且 commit 在 acquire 當下就被消費了
    //   (rime_shell.h 檔頭)—— 所以這裡**不可以**在有 commit 待取時被呼叫。
    //   目前的呼叫點只有 session 剛建立之後,那時不可能有 commit。
    if (s) rs_snapshot_release(sess);
  });
  return out;
}

std::string Engine::ApplyChoice(uint64_t id, const std::string& schema_id,
                                const std::vector<OptionAssign>& options) {
  std::string chosen;
  Post([&] {
    const uintptr_t sess = Find(id);
    if (!sess) return;
    if (!schema_id.empty() && rs_select_schema(sess, schema_id.c_str()))
      chosen = schema_id;
    // 字形要在選方案**之後**才設:換方案會重建 context,
    // 先設的話會被換方案那一步洗掉。
    for (const OptionAssign& a : options) rs_set_option(sess, a.option, a.value);
  });
  return chosen;
}


int Engine::AbiVersion() const { return rs_abi_version(); }

bool Engine::BeginDeploy(uint32_t* out_seq) {
  if (out_seq) *out_seq = deploy_seq_.load();
  // rs_deploy 是唯一允許跨執行緒呼叫的函式(rime_shell.h),
  // 所以不必進引擎佇列 —— 而且**不該**進:部署要好幾分鐘,
  // 佔住引擎執行緒等於整個輸入法停擺。
  if (rs_deploy()) return true;
  {
    std::lock_guard<std::mutex> lock(err_mu_);
    const char* e = rs_last_error();
    last_error_ = e ? e : "";
    if (last_error_.empty())
      last_error_ = "引擎沒有給原因(多半是已經有一個部署在進行中)";
  }
  return false;
}

bool Engine::PollDeploy(uint32_t since_seq, int* status) {
  if (deploy_seq_.load() == since_seq) return false;
  const int v = deploy_state_.load();
  if (v == 0) return false;
  if (status) *status = v;
  if (v != 1) {
    std::lock_guard<std::mutex> lock(err_mu_);
    const char* e = rs_last_error();
    last_error_ = e ? e : "";
  }
  return true;
}

std::string Engine::last_error() const {
  std::lock_guard<std::mutex> lock(err_mu_);
  return last_error_.empty() ? init_error_ : last_error_;
}

}  // namespace rimewin
