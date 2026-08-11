#include "engine.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include "../common/ime_policy.h"
#include "rime_shell.h"

namespace rimewin {
namespace {

// 低優先的工作(收 session、補充備用 session)在引擎閒下來多久之後才做。
//
// ⚠ 1500 毫秒不是「保險起見」挑的,是被量出來的兩件事夾出來的:
//
//   · 低優先的工作**很貴**:收 session 要把使用者詞典寫回去,補一個備用
//     session 量到 442~753 毫秒。而它們一旦開始就停不下來。
//   · 按鍵的預算只有 **50 毫秒**。所以只要在使用者還在打字的空檔裡插進去
//     一件,那一顆字就打不出來。
//
//   打字時的按鍵間隔是十分之幾秒,「想一下」的停頓才是一兩秒 ——
//   所以門檻要明顯大於前者。取 1500 毫秒:連續打字時這條路自然不會被
//   走到,而使用者一停下來就會被補上。
//
//   代價只有「收尾晚一點做」與「連開兩個程式時第二個可能沒有備用的」,
//   兩者都退回原本的行為,**不會比現在差**。
constexpr int64_t kLowPriorityIdleMs = 1500;

// 一件工作慢到多少毫秒就值得記一行。
//
// 40 毫秒是刻意訂在**按鍵預算(50ms)之下**的:任何一件慢到 40 毫秒的
// 工作,都已經有能力讓下一顆按鍵逾時。記多一點沒關係 —— 這幾行只在
// 出事時才有人看,而出事時它們是唯一的線索。
constexpr int64_t kSlowJobMs = 40;

void ReportSlowJob(const char* label, int64_t waited_ms, int64_t ran_ms) {
  if (waited_ms + ran_ms < kSlowJobMs) return;
  std::fprintf(stderr, "[engine] 慢工作 %s 等待=%lld ms 執行=%lld ms\n", label,
               static_cast<long long>(waited_ms),
               static_cast<long long>(ran_ms));
  std::fflush(stderr);
}

int64_t NowSteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

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
  // ⚠ **不是** s.is_simplified。那個欄位只反映 `simplification`,而本專案
  //   打包的方案通通沒有那個開關 —— rs_set_option 對不存在的選項不會
  //   失敗、會原樣記下並回讀,所以讀它等於把我們自己寫進去的偏好當成
  //   引擎的狀態回報。使用者實機回報過:設定裡選簡體、狀態列畫「简」、
  //   打出來是繁體。真相在 s.variant(core/include/rime_shell.h)。
  if (s.variant != RS_VARIANT_UNKNOWN) f |= kStVariantKnown;
  if (s.variant == RS_VARIANT_HANS) f |= kStSimplified;
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
    Job job;
    bool have_job = false;
    bool is_low = false;
    bool done_all = false;
    {
      std::unique_lock<std::mutex> lock(mu_);
      for (;;) {
        if (stop_ && queue_.empty() && low_queue_.empty()) {
          done_all = true;
          break;
        }
        if (!queue_.empty()) break;
        if (!low_queue_.empty()) {
          // ── ⚠ 低優先的工作還要「等引擎閒下來一段時間」才做 ──────────
          //
          // 只把它排到後面是不夠的:排序只在工作**還沒開始**時有用。
          // 低優先的兩件事都很貴 —— 收 session 要把使用者詞典寫回去,
          // 補一個備用 session 要 442~753 毫秒(量到的)—— 而它們一旦
          // 開始就停不下來。在引擎還忙的時候動它們,等於用一顆打不出來
          // 的字(按鍵預算只有 50 毫秒)去換別的東西。
          //
          // 使用者連續打字時引擎一直是忙的,所以這條路自然不會被走到;
          // 他停下來想事情的時候才會。而這兩件事晚幾秒**沒有任何人在等**。
          const int64_t idle_ms = NowSteadyMs() - last_normal_ms_;
          if (stop_ || idle_ms >= kLowPriorityIdleMs) break;
          cv_.wait_for(lock,
                       std::chrono::milliseconds(kLowPriorityIdleMs - idle_ms));
          continue;
        }
        cv_.wait(lock);
      }
      if (done_all) break;
      if (!queue_.empty()) {
        job = std::move(queue_.front());
        queue_.pop_front();
      } else {
        job = std::move(low_queue_.front());
        low_queue_.pop_front();
        is_low = true;
      }
      have_job = true;
    }
    if (!have_job) continue;
    const int64_t t_start = NowSteadyMs();
    job.fn();
    const int64_t t_end = NowSteadyMs();
    // ⚠ 「等了多久」與「跑了多久」一定要分開報。
    //   等很久 = **別人擋在前面**(去看上一行是誰);
    //   跑很久 = 這件事本身就慢(去看它做了什麼)。
    //   併成一個數字的話,兩種完全不同的問題長得一模一樣 ——
    //   而引擎只有一條執行緒,所以前者才是常態。
    //
    //   低優先的工作不報「等待」:它是**刻意**被押後的,那個數字沒有意義。
    ReportSlowJob(job.label, is_low ? 0 : t_start - job.enqueued_ms,
                  t_end - t_start);
    if (!is_low) {
      // 「引擎最後一次忙於**有人在等的**工作」是什麼時候。低優先的工作
      // 靠它決定要不要再等一下(見上面)。低優先的自己不算忙 ——
      // 不然一串連續的收尾會讓自己一直看起來很忙,永遠等不到安靜期。
      std::lock_guard<std::mutex> lock(mu_);
      last_normal_ms_ = t_end;
    }
    cv_.notify_all();
  }
  // session 的銷毀必須在建立它的那條執行緒上 —— 也就是這裡。
  // rs_finalize 則留給 Stop()(與 rs_init 同一條執行緒)。
  for (auto& kv : sessions_) rs_session_destroy(kv.second);
  sessions_.clear();
}

void Engine::Post(const char* label, std::function<void()> fn) {
  bool done = false;
  {
    std::unique_lock<std::mutex> lock(mu_);
    if (stop_) return;
    Job j;
    j.label = label;
    j.enqueued_ms = NowSteadyMs();
    j.fn = [&fn, &done, this] {
      fn();
      std::unique_lock<std::mutex> l2(mu_);
      done = true;
    };
    queue_.push_back(std::move(j));
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
  Post("建 session", [&] {
    const rs_session s = rs_session_create();
    if (s == RS_INVALID_SESSION) return;
    id = next_id_++;
    sessions_[id] = s;
  });
  return id;
}

void Engine::EndSession(uint64_t id) {
  Post("收 session", [&] {
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;
    rs_session_destroy(it->second);
    sessions_.erase(it);
    session_lang_.erase(id);
  });
}

namespace {
bool SameOptions(const std::vector<OptionAssign>& a,
                 const std::vector<OptionAssign>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].value != b[i].value) return false;
    const char* x = a[i].option ? a[i].option : "";
    const char* y = b[i].option ? b[i].option : "";
    if (std::strcmp(x, y) != 0) return false;
  }
  return true;
}
}  // namespace

// 丟一件「有空再做」的工作。與 Post 不同:不等它、而且優先權比一般工作低。
void Engine::PostLow(const char* label, std::function<void()> fn) {
  {
    std::unique_lock<std::mutex> lock(mu_);
    if (stop_) return;
    Job j;
    j.label = label;
    j.enqueued_ms = NowSteadyMs();
    // ⚠ 按值移動進佇列。這一支丟了就走,呼叫端的堆疊在工作真正執行之前
    //   就已經不存在了 —— 捕捉參考等於一個懸空參考。
    j.fn = std::move(fn);
    low_queue_.push_back(std::move(j));
  }
  cv_.notify_all();
}

uint64_t Engine::TakeSpareSession(uint32_t langid, const std::string& schema_id,
                                  const std::vector<OptionAssign>& options) {
  uint64_t stale = 0;
  uint64_t id = 0;
  {
    std::lock_guard<std::mutex> lock(spare_mu_);
    auto it = spare_.find(langid);
    if (it == spare_.end()) return 0;
    if (it->second.schema_id == schema_id &&
        SameOptions(it->second.options, options)) {
      id = it->second.session;
    } else {
      // 計畫變了(使用者改過設定,或方案清單變了)。這一個不能用 ——
      // 交出去的話,他改完設定開的第一個程式會拿到一個照舊設定配好的
      // session,而那種錯誤是**靜默**的。收掉它,這一次當場建一個。
      stale = it->second.session;
    }
    spare_.erase(it);
  }
  if (stale != 0) EndSessionAsync(stale);
  return id;
}

void Engine::RequestSpareSession(uint32_t langid, const std::string& schema_id,
                                 const std::vector<OptionAssign>& options) {
  {
    std::lock_guard<std::mutex> lock(spare_mu_);
    if (spare_.count(langid)) return;          // 已經有一個備著
    if (spare_pending_.count(langid)) return;  // 已經排了一件補充工作
    spare_pending_[langid] = true;
  }
  // ⚠ 走低優先那條路。補一個 session 本身要 442~753 毫秒(量到的),
  //   而按鍵的預算只有 50 毫秒 —— 在引擎還忙著的時候補,等於用一顆
  //   打不出來的字去換下一個程式開得快一點。低優先那條路要等引擎
  //   真的閒下來才動,而使用者連續打字時引擎一直是忙的。
  PostLow("補充備用 session", [this, langid, schema_id, options] {
    {
      std::lock_guard<std::mutex> lock(spare_mu_);
      spare_pending_.erase(langid);
      if (spare_.count(langid)) return;
    }
    MakeSpareOnEngineThread(langid, schema_id, options);
  });
}

// ⚠ **engine.cc 裡唯一允許出現 rs_select_schema 的地方**(見 engine.h)。
//   windows/audit_single_source.sh 在原始碼層面守這一條。
bool Engine::SelectAndApply(uint64_t id, uintptr_t sess,
                            const std::string& schema_id) {
  if (!sess) return false;
  bool selected = false;
  if (!schema_id.empty()) {
    selected = rs_select_schema(sess, schema_id.c_str());
    // 換方案失敗就不要再套簡繁 —— 那個 session 現在是什麼方案我們不知道,
    // 而對一個沒選成功的方案套字形只會讓狀態更難解釋。
    if (!selected) return false;
  }
  // 換方案會重建 context 並把 switches 重設回方案宣告的值,所以簡繁
  // 一定要在**選完之後**才套。順序反過來的話,設的那一份會被洗掉,
  // 而那是**靜默**的:畫面照舊、打出來的字變了。
  auto it = session_lang_.find(id);
  const uint32_t lang = (it == session_lang_.end()) ? 0u : it->second;
  bool simplified = false;
  // ⚠ 與建 session 時走的是**同一支** DecideVariant / PlanVariant。
  //   兩份會漂移,而漂移的症狀是「改設定當下沒變、換個程式就變了」。
  if (DecideVariant(lang, variant_pref_, &simplified)) {
    for (const OptionAssign& a : PlanVariant(simplified, lang))
      rs_set_option(sess, a.option, a.value);
  }
  return selected;
}

// ⚠ **只能在引擎執行緒上呼叫**(它直接碰 sessions_ / next_id_ 與 rs_*)。
void Engine::MakeSpareOnEngineThread(uint32_t langid,
                                     const std::string& schema_id,
                                     const std::vector<OptionAssign>& options) {
  const rs_session s = rs_session_create();
  if (s == RS_INVALID_SESSION) return;
  const uint64_t id = next_id_++;
  sessions_[id] = s;
  session_lang_[id] = langid;
  // ⚠ 這裡走 SelectAndApply,而不是裸的 rs_select_schema —— 備用 session
  //   也是 session,它一樣會被換方案洗掉簡繁。
  //
  //   接著才套 options:它是**呼叫端算好的完整計畫**(BuildOptionPlan),
  //   含簡繁、標點、中英,而且 TakeSpareSession 會拿它比對。所以順序是
  //   「選方案 → SelectAndApply 保底 → options 覆蓋」,最後生效的一定是
  //   計畫裡那一份。
  SelectAndApply(id, s, schema_id);
  for (const OptionAssign& a : options) rs_set_option(s, a.option, a.value);
  std::lock_guard<std::mutex> lock(spare_mu_);
  SparePlan p;
  p.session = id;
  p.schema_id = schema_id;
  p.options = options;
  spare_[langid] = std::move(p);
}

void Engine::PrimeSpareSession(uint32_t langid, const std::string& schema_id,
                               const std::vector<OptionAssign>& options) {
  // ⚠ 這一支是**同步**的,而且只有暖機會用它。
  //
  //   補充備用 session 平常走低優先那條路(要等引擎閒下來 1.5 秒),
  //   但暖機是在**管道還沒打開之前**做的 —— 那時沒有任何宿主連得上來,
  //   所以「等閒下來」既沒有意義也來不及:管道一開就可能有人連上來,
  //   而他要的正是這個備用 session。
  Post("預先建好備用 session", [&] {
    {
      std::lock_guard<std::mutex> lock(spare_mu_);
      if (spare_.count(langid)) return;
    }
    MakeSpareOnEngineThread(langid, schema_id, options);
  });
}

void Engine::EndSessionAsync(uint64_t id) {
  // ⚠ **不進一般佇列。** 進去的話它就排在下一個宿主的 SESSION_NEW 前面,
  //   而那正是要修的東西(理由寫在 ThreadMain 裡)。
  PostLow("收 session", [this, id] {
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

Result Engine::ToggleAsciiMode(uint64_t id) {
  Result r;
  // ── ⚠ 部署還沒做完就不要宣稱切過了 ──────────────────────────────
  //
  //   與 ProcessKey 同一條規則(見下面)。少了這一格,使用者在首次部署
  //   那幾分鐘按下 Ctrl+空白鍵會拿到 handled=true 配一份**空快照** ——
  //   瘦 DLL 於是宣告吃掉那顆鍵,而懸浮狀態列那一格讀的就是快照上的旗標,
  //   它會顯示「中」而引擎其實什麼都沒做。按鍵被吃掉、畫面說謊、
  //   而使用者只會覺得「這個開關壞了」。
  //
  //   回 handled=false 的話,瘦 DLL 會把那顆鍵放行給宿主(key_eat_policy 的
  //   同一條規矩:做不到的事就不要吃掉那顆鍵)。
  if (deploy_state_.load() != 1) {
    r.handled = false;
    r.snap.status_flags = kStDisabled;
    return r;
  }
  const bool now = ascii_mode_.load();
  SetAsciiModeAll(!now);
  r.handled = true;
  // ── ⚠ 快照要在切換**之後**取,而且必須在**引擎執行緒**上取 ──────
  //
  //   順序:狀態列那一格顯示的是快照上的旗標(status_bar.h:「顯示的是
  //   引擎說的狀態」),取早了就是一個說謊的指示器。更要緊的是,librime
  //   在切到英數的當下會把手上那段組字上屏,而那份 commit 在
  //   rs_snapshot_acquire 的當下就被消費 —— 取早了它會落在下一次 acquire,
  //   而下一次沒有人在等,使用者打到一半的字就永久消失。
  //
  //   執行緒:TakeSnapshot 直接碰 sessions_ 與 rs_*,而 engine.h 在它的
  //   宣告上面寫著「以下三個只在引擎執行緒上呼叫」。這個函式是在**管道
  //   執行緒**上跑的(pipe_server.cc 每個 client 一條 std::thread),所以
  //   直接呼叫等於無鎖讀 sessions_,並與引擎執行緒上的 rs_process_key
  //   並行呼叫 rs_*。SetAsciiModeAll 內部是 Post 進佇列的,這一句不是 ——
  //   「兩者都排在同一條佇列上」那句舊註解是錯的,只有前者是。
  Post("切中英後取快照", [&] { r.snap = TakeSnapshot(id); });
  return r;
}

Result Engine::ProcessKey(uint64_t id, int32_t keysym, uint32_t mods) {
  Result r;
  Post("按鍵", [&] {
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
  Post("選候選", [&] {
    const uintptr_t sess = Find(id);
    if (!sess) return;
    r.handled = rs_select_candidate(sess, index);
    r.snap = TakeSnapshotLocked(sess);
  });
  return r;
}

Result Engine::CommitComposition(uint64_t id) {
  Result r;
  Post("上屏", [&] {
    const uintptr_t sess = Find(id);
    if (!sess) return;
    r.handled = rs_commit_composition(sess);
    r.snap = TakeSnapshotLocked(sess);
  });
  return r;
}

Result Engine::Clear(uint64_t id) {
  Result r;
  Post("清除組字", [&] {
    const uintptr_t sess = Find(id);
    if (!sess) return;
    r.handled = rs_clear_composition(sess);
    r.snap = TakeSnapshotLocked(sess);
  });
  return r;
}

Result Engine::ChangePage(uint64_t id, bool backward) {
  Result r;
  Post("翻頁", [&] {
    const uintptr_t sess = Find(id);
    if (!sess) return;
    r.handled = rs_change_page(sess, backward);
    r.snap = TakeSnapshotLocked(sess);
  });
  return r;
}

Result Engine::SelectSchema(uint64_t id, const std::string& schema_id) {
  Result r;
  Post("換方案", [&] {
    const uintptr_t sess = Find(id);
    if (!sess) return;
    // ⚠ 換完一定要重套簡繁 —— 這條路徑是 SendSelectSchema 進來的,
    //   而它以前是裸呼叫:使用者從設定裡換一次方案,他選的簡體就沒了。
    r.handled = SelectAndApply(id, sess, schema_id);
    r.snap = TakeSnapshotLocked(sess);
  });
  return r;
}

std::vector<std::pair<std::string, std::string>> Engine::SchemaList() {
  std::vector<std::pair<std::string, std::string>> out;
  Post("列方案", [&] {
    const int32_t n = rs_schema_list(nullptr, nullptr, 0);
    if (n <= 0) return;
    std::vector<const char*> ids(static_cast<size_t>(n), nullptr);
    std::vector<const char*> names(static_cast<size_t>(n), nullptr);
    const int32_t got = rs_schema_list(ids.data(), names.data(), n);
    for (int32_t i = 0; i < got && i < n; ++i) {
      out.emplace_back(ids[i] ? ids[i] : "", names[i] ? names[i] : "");
    }
  });
  {
    std::lock_guard<std::mutex> lock(cache_mu_);
    schema_cache_ = out;
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> Engine::SchemaListCached() {
  {
    std::lock_guard<std::mutex> lock(cache_mu_);
    if (!schema_cache_.empty()) return schema_cache_;
  }
  // 快取是空的(還沒問過,或剛部署完被清掉)—— 真的問一次,
  // 上面那一支會順手把快取填好。
  return SchemaList();
}

void Engine::InvalidateSchemaCache() {
  std::lock_guard<std::mutex> lock(cache_mu_);
  schema_cache_.clear();
}

bool Engine::SetOption(uint64_t id, const char* option, bool value) {
  bool ok = false;
  Post("設選項", [&] {
    const uintptr_t sess = Find(id);
    if (!sess) return;
    ok = rs_set_option(sess, option, value);
  });
  return ok;
}

void Engine::SetOptionAll(const char* option, bool value) {
  Post("對所有 session 設選項", [&] {
    for (const auto& kv : sessions_) rs_set_option(kv.second, option, value);
  });
}

void Engine::SetAsciiModeAll(bool on) {
  // 先記下來:新的 session 由 pipe_server 從這裡讀(它在 options 裡),
  // 而備用 session 的計畫比對也吃同一個值。
  ascii_mode_.store(on);
  Post("對所有 session 設中英", [&] {
    for (const auto& kv : sessions_) rs_set_option(kv.second, "ascii_mode", on);
  });
  // ⚠ 預先建好的備用 session 是照**舊**狀態配的,現在不合了。
  //   留著的話,使用者切成英文之後開的第一個程式會拿到一個中文的 session
  //   —— 而那種錯誤是靜默的。直接讓計畫比對去淘汰它(options 變了),
  //   這裡不必動它,但要把 spare 也一起設,免得它被判成「計畫相同」。
  {
    std::lock_guard<std::mutex> lock(spare_mu_);
    for (auto& kv : spare_) {
      bool found = false;
      for (OptionAssign& a : kv.second.options) {
        if (std::string(a.option) == "ascii_mode") {
          a.value = on;
          found = true;
        }
      }
      if (!found) kv.second.options.push_back({"ascii_mode", on});
    }
  }
  Post("對備用 session 設中英", [&] {
    std::lock_guard<std::mutex> lock(spare_mu_);
    for (const auto& kv : spare_) {
      const uintptr_t sess = Find(kv.second.session);
      if (sess) rs_set_option(sess, "ascii_mode", on);
    }
  });
}

void Engine::SetSessionLangId(uint64_t id, uint32_t langid) {
  Post("記下 session 的語言", [&] { session_lang_[id] = langid; });
}

void Engine::ApplyVariantAll(const SchemaPreference& pref) {
  // ⚠ 先把備用池的**計畫**改掉,再去改真的 session。
  //
  //   TakeSpareSession 用 SameOptions 比對計畫,計畫不合就把那個備用
  //   session 當場丟掉、當場重建。所以改完簡繁設定之後,如果不同步
  //   更新備用池的計畫,**池子裡每一個 session 都會被判成過期** ——
  //   正確性沒事,但 442~753 毫秒的 session 建立成本又回到使用者
  //   等待的那一趟裡(SESSION_NEW 的預算是 300 毫秒)。
  //
  //   症狀:改完簡繁設定之後開的第一個程式,第一顆按鍵明顯變慢,
  //   而使用者不會把「我剛改了設定」與那件事聯想在一起。
  //
  //   這與 SetAsciiModeAll 是同一個形狀,做法照抄它。
  {
    std::lock_guard<std::mutex> lock(spare_mu_);
    for (auto& kv : spare_) {
      bool simplified = false;
      if (!DecideVariant(kv.first, pref, &simplified)) continue;
      const std::vector<OptionAssign> plan = PlanVariant(simplified, kv.first);
      // ⚠ 逐項就地覆蓋,不是 push_back。計畫的**順序**是契約的一部分
      //   (SameOptions 是逐項比對),而 BuildOptionPlan 把簡繁那一組
      //   放在最前面 —— 附加在尾巴會讓長度與順序都對不上,等於白做。
      for (const OptionAssign& a : plan) {
        for (OptionAssign& have : kv.second.options) {
          if (std::string(have.option) == std::string(a.option))
            have.value = a.value;
        }
      }
    }
  }
  Post("對所有 session 套簡繁", [&] {
    // SelectAndApply 之後要用它重算 —— 少了這一行,使用者改完設定再
    // 換一次方案,換回去的是**改設定之前**那一份簡繁。
    variant_pref_ = pref;
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
  // 備用 session 不在 sessions_ 的迴圈裡(它們在 spare_ 底下),
  // 所以真的那一份也要另外設一次 —— 只改計畫不改 session,交出去的
  // 會是一個計畫說簡體、實際還是繁體的 session,而那種錯誤是靜默的。
  Post("對備用 session 套簡繁", [&] {
    std::lock_guard<std::mutex> lock(spare_mu_);
    for (const auto& kv : spare_) {
      const uintptr_t sess = Find(kv.second.session);
      if (!sess) continue;
      bool simplified = false;
      if (!DecideVariant(kv.first, pref, &simplified)) continue;
      for (const OptionAssign& a : PlanVariant(simplified, kv.first))
        rs_set_option(sess, a.option, a.value);
    }
  });
}

void Engine::SelectSchemaAll(const std::string& schema_id) {
  Post("對所有 session 換方案", [&] {
    // ⚠ 這是懸浮狀態列第三格那個方案選單走的路。它以前是裸呼叫,
    //   於是「換一次方案」就把使用者選的簡繁洗掉,而畫面上那一格
    //   還畫著舊的 —— 使用者回報的兩件事(那一橫、簡繁說謊)在這裡
    //   是同一個缺陷。
    for (const auto& kv : sessions_)
      SelectAndApply(kv.first, kv.second, schema_id);
  });
}

std::string Engine::SchemaOfSession(uint64_t id) {
  std::string out;
  Post("問 session 的方案", [&] {
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

Engine::StatusReadback Engine::ReadBackStatus() {
  StatusReadback out;
  Post("回讀狀態", [&] {
    // 挑一個活著的 session。沒有就退而求其次用備用池裡的 ——
    // 它們的選項是照同一份計畫配的,回讀出來的是同一件事。
    uintptr_t sess = 0;
    if (!sessions_.empty()) sess = sessions_.begin()->second;
    if (!sess) {
      std::lock_guard<std::mutex> lock(spare_mu_);
      if (!spare_.empty()) sess = Find(spare_.begin()->second.session);
    }
    // ⚠ 一個都沒有 → ok 維持 false,呼叫端**什麼都不要改**。
    //   在這裡回一份預設值等於宣稱「現在是中文、繁體」,而那又是
    //   一次沒有證據的宣稱。
    if (!sess) return;
    out.ok = true;
    if (rs_get_option(sess, "ascii_mode")) out.status_flags |= kStAsciiMode;
    // ⚠ 簡繁**不讀 `simplification`**。理由見 common/status_cells.h:
    //   本專案打包的方案通通沒有那個開關,而 rs_set_option 對不存在的
    //   選項不會失敗、會原樣記下並回讀 —— 讀它等於讀自己寫進去的回音。
    // ⚠ 用字面名字而不是 kVariantOptions[i]:那個陣列的順序是
    //   PlanVariant 的「先關再開」用的,拿索引對應欄位等於把兩件無關的
    //   東西綁在一起 —— 哪天順序調了,這裡會靜默地讀錯欄位。
    VariantOptions vo;
    vo.zh_hans = rs_get_option(sess, "zh_hans");
    vo.zh_hant = rs_get_option(sess, "zh_hant");
    vo.zh_hant_hk = rs_get_option(sess, "zh_hant_hk");
    vo.zh_hant_tw = rs_get_option(sess, "zh_hant_tw");
    const VariantCell v = VariantCellFromOptions(vo);
    if (v != VariantCell::kHidden) out.status_flags |= kStVariantKnown;
    if (v == VariantCell::kSimplified) out.status_flags |= kStSimplified;
  });
  return out;
}

std::string Engine::ApplyChoice(uint64_t id, const std::string& schema_id,
                                const std::vector<OptionAssign>& options) {
  std::string chosen;
  Post("套用方案與選項", [&] {
    const uintptr_t sess = Find(id);
    if (!sess) return;
    // ⚠ 走 SelectAndApply(而不是裸的 rs_select_schema)。
    //   它保底把簡繁套一次;接著 options 是呼叫端算好的完整計畫
    //   (BuildOptionPlan:簡繁 + 標點 + 中英),覆蓋在上面。
    //   兩者的簡繁部分一致 —— 都走 DecideVariant / PlanVariant。
    if (SelectAndApply(id, sess, schema_id)) chosen = schema_id;
    // 字形要在選方案**之後**才設:換方案會重建 context,
    // 先設的話會被換方案那一步洗掉。
    for (const OptionAssign& a : options) rs_set_option(sess, a.option, a.value);
  });
  return chosen;
}


int Engine::AbiVersion() const { return rs_abi_version(); }

bool Engine::BeginDeploy(uint32_t* out_seq) {
  if (out_seq) *out_seq = deploy_seq_.load();
  // 部署會改寫方案清單(使用者可能剛勾掉一個方案)。清掉快取,
  // 下一次問的時候會退回真的問一次引擎 —— 寧可慢一次,
  // 也不要拿一份舊清單去替使用者挑方案。
  InvalidateSchemaCache();
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
  // 部署結束了 —— 清單可能已經不一樣。再清一次:BeginDeploy 那一次清的是
  // 「部署開始前」的舊清單,而部署**期間**若有人問過,快取又被填成
  // 一份半路的清單了。
  InvalidateSchemaCache();
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
