#include "engine.h"

#include <set>

#include <chrono>
#include <cstdio>
#include <cstring>

#include "../common/callback_gate.h"
#include "../common/ime_policy.h"
#include "../common/status_cells.h"
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

// ── 部署回呼要碰的那個 Engine，活多久 ───────────────────
//
// OnDeploy 跑在 **librime 的部署執行緒**上，而 Engine 是 main 那條執行緒
// 上的物件 —— 它隨時可能因為 Stop() → ~Engine() 而不再存在。
//
// ⚠ **把這個指標換成 `std::atomic<Engine*>` 是不夠的。** 那只讓「讀到的
//   是不是 nullptr」變成定義良好；它一個字都沒有說到讀出來之後的事：
//   回呼在 `p = load()` 與 `p->OnDeployTerminal()` 之間可以被排掉任意久，
//   而那段時間足夠 main 跑完整支 Stop() 與 ~Engine()。回呼醒來之後拿的是
//   一段已經還掉的記憶體，而它會照樣寫進去。窗口小只代表難重現，
//   在使用者那一側的樣子是「按下重新整理字詞之後服務偶爾自己消失」。
//
// 所以要的不是「讀得乾淨」，是**生命週期**：關門的人要等到裡面沒有人。
// 那一格在 common/callback_gate.h，由 tests/test_callback_gate.cc 在
// Ubuntu 上直接測（含 --asan；天真的 atomic 版在那裡是 use-after-free）。
//
// ⚠ 鎖序固定：**librime 的全域鎖 → 閘的鎖 → 佇列的鎖**，不可以反向。
//   反向那一步長這樣：某人握著閘的鎖去呼叫 rs_*。所以 Open()/Close()
//   的呼叫點旁邊不可以有任何 rs_*（Stop() 裡 Close() 排在 rs_finalize()
//   之前，兩者不重疊）。
CallbackGate<Engine> g_deploy_gate;

void OnDeploy(rs_deploy_status status, void* /*ud*/) {
  // ⚠ 這個回呼**不在**呼叫端的執行緒上，而且可能在 rs_deploy() 早已返回
  //   之後才觸發（rime_shell.h 檔頭）。
  //
  // IDLE / RUNNING 不是終局 —— 一個位元都不動。
  if (status != RS_DEPLOY_SUCCESS && status != RS_DEPLOY_FAILURE) return;
  const bool ok = (status == RS_DEPLOY_SUCCESS);

  // ── #90：把 session 建回來 ─────────────────────────
  //
  // ⚠ **這件事不可以掛在設定視窗的計時器上。** 那個視窗是可以被關掉的，
  //   而關掉之後就再也沒有人會把 session 建回來 —— 使用者從此打不出
  //   中文，重開機也沒用（每次啟動又走一次同樣的路）。所以觸發點在
  //   這裡：終局有沒有人在看都會到。
  //
  // ⚠ 首次部署也會走到這裡，而那時階段是 kIdle，狀態機會把
  //   kDeployFinished 判成不合法並原地不動 —— 於是不會有任何重建。
  //   那正是要的：首次部署之前本來就一個 session 都沒有。
  //
  // ⚠ 閘裡面只准做兩件事：動 atomic、排一件工作。不加鎖、不碰 session、
  //   不碰 UI、**不呼叫任何 rs_***（rime_shell 呼叫這個回呼時**持有**
  //   g_global_mutex —— 見 #91 —— 所以在這裡直接呼叫 rs_* 是一個真的
  //   死鎖）。而且它跑在閘的鎖裡面：做得越久，Stop() 就陪著等越久。
  g_deploy_gate.Run([ok](Engine* e) { e->OnDeployTerminal(ok); });
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
  // ⚠ 開閘要在 rs_init **之前**。rs_init 自己會呼叫
  //   `start_maintenance(True)`（core/src/rime_shell.cc），而通知回呼在那
  //   之前就掛上去了 —— 首次部署的終局有可能在 rs_init 還沒返回時就到。
  //   開晚了掉的那一發是「第一次整理字詞做完了」，而沒有人會再送一次。
  //
  //   對稱的那一半在 Stop()：它的第一句就是 Close()，而且刻意排在
  //   `if (!started_)` 之前 —— 這一支在 rs_init 失敗時回 false 而
  //   started_ 仍然是 false，閘卻已經開了。
  g_deploy_gate.Open(this);

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
  //
  // ⚠ 佇列本身搬到 common/work_queue.{h,cc} 了。搬家的理由不是整理:
  //   舊版的「丟工作並等它做完」沒有逾時,而**那個沒有逾時同時是
  //   #79 的根因、也是唯一擋住 use-after-free 的東西**(工作捕捉的是
  //   呼叫端堆疊上的 done / out / ok)。要拆掉前者就一定會引爆後者,
  //   所以那一格必須是一個能在 Ubuntu 上單獨測的類別。
  //   見 windows/tests/test_work_queue.cc。
  //
  // 慢工作的門檻由這裡決定:佇列每一件都回報,要不要記是引擎的事。
  queue_.SetSlowReporter(&ReportSlowJob);
  queue_.SetLowPriorityIdleMs(kLowPriorityIdleMs);
  // session 的銷毀必須在**建立它的那條執行緒**上 —— 也就是工作者自己,
  // 而且要在它把佇列排乾之後、離開之前。
  queue_.SetOnWorkerExit([this] {
    for (auto& kv : sessions_) rs_session_destroy(kv.second);
    sessions_.clear();
  });
  queue_.Start();
  started_ = true;
  return true;
}

void Engine::Stop() {
  // ── ⚠ 關閘要在**最前面**，兩個理由都是實的 ────────────
  //
  //   · 在 `if (!started_)` 之前：Start() 在 rs_init 失敗時回 false 而
  //     started_ 仍然是 false，但閘那時已經開了（它必須開在 rs_init
  //     之前，見 Start()）。這一問若先 return，閘上就留著一個指向即將
  //     被解構的 Engine 的指標，而 librime 的部署執行緒還會照著它呼叫。
  //   · 在 queue_.Stop() 之前：部署的終局回呼會走 RebuildSessionsAsync()，
  //     而它在排不進佇列時會去清 parked_ —— 那是**引擎執行緒的**資料。
  //     佇列正在排乾的那一段裡，工作者可能正在
  //     RebuildSessionsOnEngineThread() 裡讀它，而回呼執行緒同時清它。
  //     先關閘，那條路整條不存在。
  //
  //   Close() 返回之後保證「沒有回呼在用這個 Engine，而且之後也不會有」
  //   （common/callback_gate.h）。那個保證是**等**出來的，所以不可以握著
  //   任何 librime 的東西進來 —— 這裡沒有，rs_finalize() 排在後面。
  g_deploy_gate.Close();
  if (!started_) return;
  // ⚠ Stop() 會把佇列**排乾**再 join：每一件已經入列的工作都保證跑到。
  //   收 session（把使用者詞典寫回去）是用 PostLow 排的，而服務結束時
  //   那些工作還在低優先佇列裡 —— 排不乾就是「關掉輸入法會掉字」。
  queue_.Stop();
  started_ = false;
  // rs_finalize 與 rs_init 在同一條執行緒上。引擎執行緒已經 join,
  // 所以它先前建立的 session 都已經在它身上銷毀了,順序是對的。
  //
  // ⚠ rs_finalize() 會拿 librime 那把全域鎖，而部署回呼是**持有那把鎖**
  //   被呼叫的。所以這一句一定要在閘已經關掉、而且沒有人握著閘的鎖的
  //   時候跑 —— 反過來就是死鎖（見 common/callback_gate.h 的鎖序）。
  rs_finalize();
}

WorkQueue::Status Engine::Post(const char* label, std::function<void()> fn) {
  // timeout <= 0 = 永遠等(舊行為)。這一支的呼叫端沒有訊息迴圈掛在
  // 上面 —— 有的那些走 PostAsync / SchemaListForUi。
  //
  // ⚠ **回傳值要交出去。** 舊版在這裡把 Status 丟掉,而那個 Status 是
  //   「有沒有人會做這件事」唯一的答案:引擎在停的時候工作根本沒有入列,
  //   呼叫端的 out 從頭到尾沒有被寫過,而它讀到的是自己的初始值。
  //   `SchemaList()` 因此回一個與「一個方案都沒有」分不出來的空 vector。
  return queue_.Call(label, std::move(fn), 0);
}

bool Engine::PostAsync(const char* label, std::function<void()> fn) {
  return queue_.Post(label, std::move(fn));
}

void Engine::PostLow(const char* label, std::function<void()> fn) {
  queue_.PostLow(label, std::move(fn));
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
    // ⚠ #90:部署期間**不可以**建 session。建起來的話它會對正在被
    //   改寫的 *.table.bin 開一個 mmap,而我們剛剛才把所有 session
    //   收掉就是為了不要有它。回 0,呼叫端會送一則帶原因的 ERROR。
    if (!SessionCreationAllowed(phase_.load())) return;
    const rs_session s = rs_session_create();
    if (s == RS_INVALID_SESSION) return;
    id = next_id_++;
    sessions_[id] = s;
  });
  return id;
}

void Engine::EndSession(uint64_t id) {
  Post("收 session", [&] {
    // ⚠ #90:部署期間這個 id 在 sessions_ 裡是不存在的(它被收走了),
    //   但它還在 parked_ 裡等著被建回來。宿主這時候關掉了 —— 那就
    //   不要再建了,不然重建之後會多出一個沒有人認得的 session。
    ForgetParked(id);
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
  // ⚠ #90:低優先那條路是「等引擎閒下來 1.5 秒」,而部署期間引擎正好
  //   一直是閒的 —— 少了這一格,補充備用 session 會在部署進行到一半時
  //   醒過來,對正在被改寫的詞庫檔開一個 mmap。
  if (!SessionCreationAllowed(phase_.load())) return;
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
  //   而那正是要修的東西(理由寫在 common/work_queue.h 的 PostLow)。
  PostLow("收 session", [this, id] {
    ForgetParked(id);  // 見 EndSession 的說明。
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
  //
  // ⚠ #90:判準不是 `deploy_state_ == 1`。那個值首次部署成功之後**永遠**
  //   是 1,所以重新部署期間這道門是開的。要問的是 common/redeploy_flow.h
  //   的 ShouldFailOpen(階段 + 有沒有能用的詞庫),而它在 Ubuntu 上驗得到。
  //
  // ⚠ 這道門與 ProcessKey 一樣是在**呼叫端執行緒**上答的，不可以搬進
  //   Post() 裡 —— 理由（50 毫秒的按鍵預算 vs. 收 session 那一包）寫在
  //   ProcessKey 那一支上面。
  if (ShouldFailOpen(phase_.load(), deploy_state_.load() == 1,
                     &r.snap.status_flags)) {
    r.handled = false;
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
  // ── ⚠ 這道門在**呼叫端執行緒**上答，不排進佇列 ──────────
  //
  //   機制要寫對，因為它決定的不是「按鍵有沒有被吃掉」，是**連線活不活**：
  //
  //   · 這一支跑在**連線執行緒**上（pipe_server 每個 client 一條）。
  //     Engine::Post() 是 `queue_.Call(label, fn, 0)` —— timeout 0 =
  //     **永遠等**，而引擎只有一條 FIFO。
  //   · 使用者按下「重新整理字詞」時排進去的是「收乾淨 session 再開始
  //     部署」一整包，而收 session 是這條路上最慢的一步（每一個
  //     rs_session_destroy 都要把使用者詞典寫回去）。
  //   · 而 DLL 那一側每一顆按鍵的預算是 **50 毫秒**
  //     （tsf/ipc_client.cc 的 kKeyTimeoutMs）。
  //
  //   所以門若是排在 Post() **裡面**，整理期間的第一顆鍵會是：吃滿 50 ms
  //   → DLL 那側 Fail(kTimeout) → Close()、session_ = 0 ——
  //   **整條連線被丟掉**。使用者看到的結果仍然是 fail-open（宿主自己收下、
  //   打出英文），但代價藏在後面：凡是在那段時間打過字的宿主都已經斷線，
  //   於是部署後「保住原 id、重套方案 / 簡繁 / 英數」那一套（#85）對它
  //   **不生效** —— 它得重新 SESSION_NEW，而那在回到 kIdle 之前是被擋的。
  //
  //   門本身只讀兩個 atomic（phase_ / deploy_state_），不碰 sessions_、
  //   不呼叫任何 rs_*，所以在哪一條執行緒上答都是同一個答案 —— 而在這裡
  //   答是微秒級的，一顆鍵都不會排到那一包後面。
  //
  //   ⚠ 這樣仍然**不是**「按鍵不會逾時」。剩下的是一個真的窗口：一顆鍵
  //     剛好在 BeginDeploy 寫下階段之前讀到 kIdle、又排在那一包後面，那一顆
  //     還是會逾時。要連它也收掉，得讓按鍵的等待有上限，而且遲到的工作
  //     不可以把那顆鍵打進 librime（不然引擎組了字、宿主也打了字，兩邊
  //     分岔）—— 那是另一件事，開在 task #93。
  //
  // ⚠ 而且這道門必須在 Find() **之前**（#90）：重新部署期間 session 是
  //   **真的不見了**，先 Find() 就會走進下面那條「找不到」—— 回給宿主的是
  //   handled=false 配一份 status_flags **全 0** 的快照，而狀態列會把它當成
  //   「一切正常，而且什麼都沒開」，把使用者剛切好的中英打回預設，
  //   同時一句「還沒好」都不說。
  if (ShouldFailOpen(phase_.load(), deploy_state_.load() == 1,
                     &r.snap.status_flags)) {
    r.handled = false;
    return r;
  }
  Post("按鍵", [&] {
    const uintptr_t sess = Find(id);
    if (!sess) {
      // 引擎不認得這個 id(部署後重建那一個失敗了,或宿主拿著過期的 id)。
      // ⚠ 同樣不可以回一份全 0 的快照 —— 理由與上面那一段一模一樣。
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

Result Engine::CurrentResult(uint64_t id) {
  Result r;
  Post("取快照", [&] {
    const uintptr_t sess = Find(id);
    if (!sess) return;
    r.handled = true;
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

std::vector<std::pair<std::string, std::string>> Engine::ListSchemasOnWorker() {
  std::vector<std::pair<std::string, std::string>> out;
  const int32_t n = rs_schema_list(nullptr, nullptr, 0);
  if (n > 0) {
    std::vector<const char*> ids(static_cast<size_t>(n), nullptr);
    std::vector<const char*> names(static_cast<size_t>(n), nullptr);
    const int32_t got = rs_schema_list(ids.data(), names.data(), n);
    for (int32_t i = 0; i < got && i < n; ++i)
      out.emplace_back(ids[i] ? ids[i] : "", names[i] ? names[i] : "");
  }
  // ⚠ 快取由**工作者自己**填。三個入口(同步、有界、非同步)共用這一份,
  //   所以「問過了但沒填快取」在結構上不可能發生。
  {
    std::lock_guard<std::mutex> lock(cache_mu_);
    schema_cache_ = out;
    schema_cache_valid_ = true;
  }
  return out;
}

WorkQueue::Status Engine::SchemaList(
    std::vector<std::pair<std::string, std::string>>* out) {
  // ⚠ 這一支永遠等(timeout 0),所以 `[&]` 是安全的 —— 呼叫端的框在
  //   工作跑完之前不會消失(理由見 common/work_queue.h 的檔頭)。
  //   有上限的那一條走 SchemaListForUi,而它用 CallFor 的共享盒子。
  std::vector<std::pair<std::string, std::string>> got;
  const WorkQueue::Status st =
      Post("列方案", [&] { got = ListSchemasOnWorker(); });
  // ⚠ 沒跑就**不要動 out**:呼叫端拿到的必須是它自己的初始值,而不是
  //   一個看起來像答案的空清單。
  if (st == WorkQueue::Status::kDone && out) *out = std::move(got);
  return st;
}

bool Engine::SchemaListFromCache(
    std::vector<std::pair<std::string, std::string>>* out) const {
  std::lock_guard<std::mutex> lock(cache_mu_);
  // ⚠ 判準是「問過了沒有」,不是「是不是空的」。見標頭。
  if (!schema_cache_valid_) return false;
  if (out) *out = schema_cache_;
  return true;
}

bool Engine::RefreshSchemaListAsync(std::function<void()> on_ready) {
  // ⚠ on_ready **傳值**捕捉進工作。這一支返回時工作通常還沒開始跑,
  //   呼叫端的框隨時會消失(見 common/work_queue.h 的檔頭)。
  return queue_.Post("列方案(介面,非同步)", [this, on_ready] {
    ListSchemasOnWorker();
    if (on_ready) on_ready();
  });
}

bool Engine::SchemaListForUi(
    int timeout_ms, std::vector<std::pair<std::string, std::string>>* out) {
  {
    std::lock_guard<std::mutex> lock(cache_mu_);
    if (schema_cache_valid_) {
      if (out) *out = schema_cache_;
      return true;
    }
  }
  // 快取是冷的(還沒暖機完,或剛部署完被清掉)。量到的成本是 46~99 ms,
  // 所以 1500 ms 這個上限**平常一次都不會被用到** —— 它是為了「引擎
  // 真的卡住了」那一次而存在的,而那一次現在只會慢一秒半,
  // 不會讓整個視窗死掉。
  using Pairs = std::vector<std::pair<std::string, std::string>>;
  Pairs got;
  const WorkQueue::Status st = queue_.CallFor<Pairs>(
      "列方案(介面,有界)", &got, [this] { return ListSchemasOnWorker(); },
      timeout_ms);
  if (st != WorkQueue::Status::kDone) return false;
  if (out) *out = std::move(got);
  return true;
}

WorkQueue::Status Engine::SchemaListCached(
    std::vector<std::pair<std::string, std::string>>* out) {
  {
    std::lock_guard<std::mutex> lock(cache_mu_);
    if (schema_cache_valid_) {
      if (out) *out = schema_cache_;
      return WorkQueue::Status::kDone;
    }
  }
  // 快取無效(還沒問過,或剛部署完被清掉)—— 真的問一次,
  // 上面那一支會順手把快取填好。
  return SchemaList(out);
}

void Engine::InvalidateSchemaCache() {
  std::lock_guard<std::mutex> lock(cache_mu_);
  schema_cache_.clear();
  schema_cache_valid_ = false;
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

void Engine::SetOptionAll(const char* option, bool value,
                         std::function<void(bool)> on_done) {
  // ⚠ **非同步。** 這一支的呼叫端是設定視窗的 UI 執行緒。同步等的話,
  //   引擎慢一次就是視窗死一次(#79)。
  // ⚠ option 複製成 std::string:工作跑起來的時候呼叫端的框可能已經
  //   不在了。今天的呼叫端都傳字面值,但這個保證不該靠呼叫端記得。
  const std::string opt = option ? option : "";
  const bool queued =
      PostAsync("對所有 session 設選項", [this, opt, value, on_done] {
        if (opt.empty()) {
          if (on_done) on_done(false);
          return;
        }
        // ⚠ 一個 session 沒設成功就是失敗。「有幾個成功」對使用者沒有
        //   意義 —— 他問的是「我剛才那一下算不算數」。
        bool ok = true;
        for (const auto& kv : sessions_)
          if (!rs_set_option(kv.second, opt.c_str(), value)) ok = false;
        if (on_done) on_done(ok);
      });
  // 沒有入列 = 這件事永遠不會發生。**一定要說**,不然畫面上那句
  // 「正在套用…」會永遠停在那裡。
  if (!queued && on_done) on_done(false);
}

void Engine::SetAsciiModeAll(bool on) {
  // 先記下來:新的 session 由 pipe_server 從這裡讀(它在 options 裡),
  // 而備用 session 的計畫比對也吃同一個值。
  ascii_mode_.store(on);
  // ⚠ 非同步。呼叫端是懸浮狀態列的 UI 執行緒(Ctrl+空白鍵那一顆),
  //   而那條執行緒停住的樣子是「那一橫還在畫面上,但點不動也拖不動」。
  PostAsync("對所有 session 設中英", [this, on] {
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
  PostAsync("對備用 session 設中英", [this, on] {
    std::lock_guard<std::mutex> lock(spare_mu_);
    for (const auto& kv : spare_) {
      const uintptr_t sess = Find(kv.second.session);
      if (sess) rs_set_option(sess, "ascii_mode", on);
    }
  });
}

void Engine::SetSessionPlanner(SessionPlanner p) {
  Post("記下重建 session 的計畫", [&] { planner_ = std::move(p); });
}

void Engine::SetSessionLangId(uint64_t id, uint32_t langid) {
  Post("記下 session 的語言", [&] { session_lang_[id] = langid; });
}

void Engine::ApplyVariantAll(const SchemaPreference& pref,
                            std::function<void(bool)> on_done) {
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
  //
  // ⚠ 這一段留在**呼叫端**執行緒上(不搬進下面那件非同步工作裡),
  //   理由與拿掉第二個 Post 是同一條:spare_mu_ 一旦在引擎執行緒上被
  //   拿住,SESSION_NEW 那一趟(TakeSpareSession / RequestSpareSession)
  //   就在呼叫端執行緒上等它。這裡只是幾筆純記憶體運算,沒有 rs_*。
  //
  //   代價是計畫先更新、session 的選項晚一步(下面那件工作)才更新。
  //   那個空窗**不會留下錯的狀態**:備用 session 從建立起就一直在
  //   sessions_ 裡(MakeSpareOnEngineThread 的第一件事就是
  //   `sessions_[id] = s;`),所以下面那個迴圈照樣會套到它,
  //   不論它在空窗期間有沒有被 TakeSpareSession 交出去。
  {
    std::lock_guard<std::mutex> lock(spare_mu_);
    for (auto& kv : spare_) {
      bool simplified = false;
      const bool set_variant = DecideVariant(kv.first, pref, &simplified);
      // ⚠ **整組換掉,不是逐項覆蓋 value。**
      //
      //   這裡原本是「逐項就地覆蓋 value、保留舊順序」,而那在它唯一
      //   針對的情境下**恆定失效**:PlanVariant 的名字順序隨 simplified
      //   而變(先關再開會把被跳過的那一個排到最後),SameOptions 是
      //   逐項比對 —— 簡繁只要真的變了,順序就一定變,備用池照樣整池
      //   報廢。而且 !set_variant 那一條路以前直接 continue,計畫裡會
      //   留著一組 SESSION_NEW 根本不會送的選項,長度就對不上。
      //
      //   兩件事現在都由 common/schema_choice.cc 的 UpdateVariantInPlan
      //   處理,而它是純函式 —— tests/test_schema_choice.cc 驗得到,
      //   不必等一輪 CI,也不必有一台 Windows。
      kv.second.options = UpdateVariantInPlan(kv.second.options, set_variant,
                                              simplified, kv.first);
    }
  }
  // ⚠ **非同步,而且 pref 要傳值。**
  //
  //   這一支就是 #79 使用者按下去的那一顆:設定視窗的「簡體字」單選鈕
  //   → OnCommand → ApplyVariantNow → CommitVariantPref → 這裡。
  //   BS_AUTORADIOBUTTON 的那一顆勾是 BUTTON 控制項在送出 BN_CLICKED
  //   **之前**自己畫上去的,所以症狀正好是「勾畫上去了,然後一切靜止」。
  //
  //   改非同步之後,原本的 `[&]` 就變成一個 use-after-free:pref 是
  //   呼叫端 CommitVariantPref 框上的一個暫時值。傳值捕捉不是保險,
  //   是必須(見 tests/test_work_queue.cc 的 async_job_owns_its_arguments)。
  const bool queued =
      PostAsync("對所有 session 套簡繁", [this, pref, on_done] {
        // SelectAndApply 之後要用它重算 —— 少了這一行,使用者改完設定再
        // 換一次方案,換回去的是**改設定之前**那一份簡繁。
        //
        // ⚠ 改成非同步之後這一格晚了一拍,但**順序仍然對**:佇列是先進
        //   先出,而換方案那條路(SelectSchemaAll / SelectSchema)也走
        //   同一條佇列,一定排在這件工作後面。
        variant_pref_ = pref;
        bool ok = true;
        for (const auto& kv : sessions_) {
          auto it = session_lang_.find(kv.first);
          const uint32_t lang = (it == session_lang_.end()) ? 0u : it->second;
          bool simplified = false;
          // ⚠ 與建 session 時走的是**同一支** DecideVariant。兩份會漂移,
          //   而漂移的症狀是「改設定當下沒變、換個程式就變了」。
          //
          // ⚠ DecideVariant 回 false = 「這一個 session 刻意不動」
          //   (例如使用者關掉了自動挑)。那**不是失敗** —— 把它算成
          //   失敗的話,關掉自動挑的人每按一次都會拿到一行紅字。
          if (!DecideVariant(lang, pref, &simplified)) continue;
          for (const OptionAssign& a : PlanVariant(simplified, lang))
            if (!rs_set_option(kv.second, a.option, a.value)) ok = false;
        }
        if (on_done) on_done(ok);
      });
  if (!queued && on_done) on_done(false);
  // ⚠ 這裡以前還有第二個 Post(「對備用 session 套簡繁」),註解寫著
  //   「備用 session 不在 sessions_ 的迴圈裡(它們在 spare_ 底下)」。
  //   **那句話是假的。** MakeSpareOnEngineThread 建完備用 session 的
  //   第一件事就是 `sessions_[id] = s;`,spare_ 存的只是「哪一個 id
  //   備著、配了什麼計畫」。備用 session 從頭到尾都在 sessions_ 裡,
  //   所以上面那個迴圈已經套過它們了,而且套的是同一份 ——
  //   session_lang_[id] 就是 langid,與 spare_ 的鍵相同。
  //
  //   那段註解自己就推翻自己:它的迴圈 body 呼叫 Find(),而 Find 查的
  //   **就是 sessions_**。如果備用 session 真的不在 sessions_ 裡,
  //   Find 會回 0、continue,整個 Post 一個選項都不會設 —— 它宣稱
  //   在防的那個「靜默錯誤」它根本防不到。
  //
  //   拿掉它不是整理:它會在引擎執行緒上**再拿一次 spare_mu_**,而那把
  //   鎖同時擋著呼叫端執行緒的 TakeSpareSession / RequestSpareSession
  //   ——SESSION_NEW 那一趟正在等它。
}

void Engine::SelectSchemaAll(const std::string& schema_id) {
  // ⚠ 非同步 + 傳值。呼叫端是懸浮狀態列的方案選單(UI 執行緒),
  //   而 schema_id 是那個選單的一個成員的複本 —— 選單在這一支返回之後
  //   就關掉了。
  PostAsync("對所有 session 換方案", [this, schema_id] {
    // ⚠ 這是懸浮狀態列第三格那個方案選單走的路。它以前是裸呼叫,
    //   於是「換一次方案」就把使用者選的簡繁洗掉,而畫面上那一格
    //   還畫著舊的 —— 使用者回報的兩件事(那一橫、簡繁說謊)在這裡
    //   是同一個缺陷。
    //
    // ⚠ **不可以**改回裸 rs_select_schema:engine.cc 裡只准有一處,
    //   而那一處必須在 SelectAndApply 裡(audit_single_source.sh 規則 2)。
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

Engine::StatusReadback Engine::ReadBackStatus(uint64_t session_id) {
  StatusReadback out;
  Post("回讀狀態", [&] {
    // ⚠ 指定了就問指定的那一個。呼叫端(懸浮那一橫)知道使用者此刻
    //   正在打字的是哪一個宿主(common/bar_owner.h 算出來的
    //   focused_session),而 13 個宿主的 ascii_mode 是各自獨立的 ——
    //   方案自己的按鍵與 ascii_composer 都會只翻其中一個。
    //   挑 sessions_.begin() 等於擲骰子,而那正是使用者回報的
    //   「點那一格沒反應,而它始終顯示『中』」。
    uintptr_t sess = session_id ? Find(session_id) : 0;
    // 沒指定、或指定的那一個已經不在了 → 退而求其次挑一個活著的,
    // 再退到備用池(它們的選項是照同一份計畫配的)。
    if (!sess && !sessions_.empty()) sess = sessions_.begin()->second;
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

void Engine::ForgetParked(uint64_t id) {
  for (size_t i = 0; i < parked_.size(); ++i) {
    if (parked_[i].id != id) continue;
    parked_.erase(parked_.begin() + static_cast<long>(i));
    return;
  }
}

// ⚠ 只能在引擎執行緒上呼叫。
void Engine::CloseAllSessionsOnEngineThread() {
  // 備用池先處理:它的 session 也在 sessions_ 裡,但沒有任何宿主認得
  // 它們 —— 建回來是浪費,下一個 SESSION_NEW 會自己要一個。
  std::set<uint64_t> spares;
  {
    std::lock_guard<std::mutex> lock(spare_mu_);
    for (const auto& kv : spare_) spares.insert(kv.second.session);
    spare_.clear();
    // ⚠ 清 spare_pending_ 只是不要讓補充工作被永久擋住;真正擋住
    //   「部署期間醒過來的那一件」的是 MakeSpareOnEngineThread 裡的
    //   SessionCreationAllowed()。
    spare_pending_.clear();
  }
  parked_.clear();
  int host_sessions = 0;
  for (const auto& kv : sessions_) {
    if (spares.find(kv.first) == spares.end()) {
      ParkedSession ps;
      ps.id = kv.first;
      auto it = session_lang_.find(kv.first);
      ps.langid = (it == session_lang_.end()) ? 0u : it->second;
      parked_.push_back(ps);
      ++host_sessions;
    }
    // ⚠ 這一句同時做兩件事,而第二件在這一輪之前是**壞的**:
    //   (1) 放掉這個 session 對 *.table.bin / *.prism.bin 的 mmap ——
    //       沒有它,librime 重編時刪不掉舊檔(而且不會說),
    //       最後把新表寫進一段還有人在讀的記憶體;
    //   (2) rs_session_destroy 走 librime 的 destroy_session,而**那才是
    //       使用者剛學到的詞落地的時機**。舊版是拿一份缺了最近學習
    //       成果的詞庫去重編。
    rs_session_destroy(kv.second);
  }
  const int total = static_cast<int>(sessions_.size());
  sessions_.clear();
  session_lang_.clear();
  std::fprintf(stderr,
               "[engine] 部署前收乾淨 session:共 %d 個(其中 %d 個要建回來)\n",
               total, host_sessions);
  std::fflush(stderr);
}

// ⚠ 只能在引擎執行緒上呼叫。
void Engine::RebuildSessionsOnEngineThread() {
  // ⚠ **一個例外都不留。** 每一個 rs_session_create() 的呼叫點都先問過
  //   這道門 —— 在這裡答案本來就是 true(kRebuilding 允許建)。留著這
  //   一問是為了讓守門掃得到「呼叫點的形狀」:有例外就得寫一份「這幾個
  //   不算」的名單,而名單會過期,然後下一個呼叫點就沒有人守。
  //   真的答 false 的話代表階段被誰推歪了 —— 那時什麼都不要建,
  //   把門打開讓使用者至少打得出字比較要緊。
  if (!SessionCreationAllowed(phase_.load())) {
    parked_.clear();
    std::fprintf(stderr, "[engine] 部署後重建:階段是 %s,不建了\n",
                 RedeployPhaseName(phase_.load()));
    std::fflush(stderr);
    phase_.store(RedeployPhase::kIdle);
    return;
  }
  // ⚠ 方案清單要在**這裡**重新問一次,而不是叫 planner_ 自己去問:
  //   planner_ 在引擎執行緒上跑,再丟一件工作進佇列就是自己等自己。
  //   順帶把快取填回去 —— 部署很可能剛改過那份清單。
  const std::vector<std::pair<std::string, std::string>> schemas =
      ListSchemasOnWorker();
  int made = 0;
  int failed = 0;
  for (const ParkedSession& ps : parked_) {
    const rs_session s = rs_session_create();
    if (s == RS_INVALID_SESSION) {
      ++failed;
      continue;
    }
    // ⚠ 用**原來那個 id**。宿主手上拿的就是它,而它是我們自己發的
    //   (next_id_),不是 librime 的 —— 所以底下那個 session 可以在
    //   宿主完全不知情的情況下被換掉一次。
    sessions_[ps.id] = s;
    session_lang_[ps.id] = ps.langid;
    // ── #85:重套方案 / 簡繁 / 標點 / 中英 ────────────────────
    //
    // ⚠ 不重套的話,使用者按一次「重新整理字詞」就會靜靜地回到預設:
    //   他釘的方案、他選的簡繁、他剛切到的英數全部不見,而畫面上
    //   寫的是「完成」。
    if (planner_) {
      const SessionPlan plan = planner_(ps.langid, schemas);
      // 字形要在選方案**之後**才設(換方案會重建 context)。
      //
      // ⚠ 這裡走 SelectAndApply,不是裸的 rs_select_schema —— 與
      //   MakeSpareOnEngineThread 同一條規矩:engine.cc 裡只准有一個
      //   rs_select_schema 呼叫點,而它在 SelectAndApply 裡
      //   (windows/audit_single_source.sh 規則 2)。
      //   這一格是**合併時新長出來的**呼叫點:#90 的重建路徑在 winbar
      //   立那條規矩的時候還不存在,而它正是規矩要防的那個形狀 ——
      //   換方案會把 switches 重設回方案宣告的值。
      //
      //   順序與備用 session 那一支完全一樣:「選方案 → SelectAndApply
      //   用 variant_pref_ 保底 → plan.options 覆蓋」。最後生效的一定是
      //   planner_ 算出來的那一份(它含簡繁、標點、中英,而且是照
      //   **設定檔**算的)。SelectAndApply 只是讓「選了方案卻沒重套簡繁」
      //   這條路在原始碼裡不存在。
      SelectAndApply(ps.id, s, plan.schema_id);
      for (const OptionAssign& a : plan.options)
        rs_set_option(s, a.option, a.value);
    }
    ++made;
  }
  parked_.clear();
  std::fprintf(stderr, "[engine] 部署後重建 session:成功 %d 個,失敗 %d 個\n",
               made, failed);
  std::fflush(stderr);
  // ⚠ 這一句是使用者重新打得出中文的那一刻。放在最後 —— 中間任何一步
  //   多花時間,門就多關一下,而關著的門有話說(kStDisabled → 「正在準備」)。
  RedeployPhase p = phase_.load();
  if (AdvanceRedeploy(&p, RedeployEvent::kRebuilt)) phase_.store(p);
}

void Engine::RebuildSessionsAsync() {
  if (PostAsync("部署後重建 session",
                [this] { RebuildSessionsOnEngineThread(); }))
    return;
  // ⚠ 排不進去 = 引擎在停。那時 session 本來就會被工作者的收尾清掉,
  //   但階段不能留在 kRebuilding —— 留著的話,只要引擎再起來,
  //   那道門就是關的而沒有任何人會去開它。
  parked_.clear();
  RedeployPhase p = phase_.load();
  if (AdvanceRedeploy(&p, RedeployEvent::kRebuilt)) phase_.store(p);
}

void Engine::OnDeployTerminal(bool deploy_ok) {
  // ⚠ 從**部署回呼的執行緒**上被呼叫，而且是在 g_deploy_gate 的鎖**裡面**
  //   （rime_shell 那時還持有它自己的全域鎖，見 #91）。所以這裡只准動
  //   atomic 與排一件工作：呼叫任何 rs_* 是死鎖，做得久是讓 Stop() 陪等。
  deploy_state_.store(deploy_ok ? 1 : -1);
  // ⚠ 序號一定要在狀態之後才加：讀的那一邊（PollDeploy）先看序號再讀
  //   狀態，反過來的話它會看到新序號配舊狀態。
  deploy_seq_.fetch_add(1);
  RedeployPhase p = phase_.load();
  if (!AdvanceRedeploy(&p, RedeployEvent::kDeployFinished)) return;
  phase_.store(p);
  // ⚠ 成功**與失敗**都要建回來。失敗那一條尤其重要:使用者手上是一個
  //   一個 session 都沒有的引擎,不建回去他從此打不出中文。
  RebuildSessionsAsync();
}

// ⚠ 只能在引擎執行緒上呼叫。
//
// ⚠ **收乾淨與開始部署在同一件工作裡**,而那是刻意的:兩者分成兩件的話
//   中間就有一段引擎執行緒閒著、session 已經沒了、詞庫檔還沒被鎖住的
//   空檔,而一個剛開起來的程式正好會在那時候要 session —— 建起來的話
//   它會活著撐過整場部署,而我們收掉所有 session 就是為了不要有它。
//   (門也關著,但那是第二道;第一道是「中間插不進任何東西」。)
void Engine::CloseThenDeployOnEngineThread() {
  CloseAllSessionsOnEngineThread();
  {
    RedeployPhase p = phase_.load();
    if (AdvanceRedeploy(&p, RedeployEvent::kSessionsClosed)) phase_.store(p);
  }
  // ⚠ 這一句**不會**佔住引擎執行緒:rs_deploy() 自己開一條 detached
  //   執行緒(core/src/rime_shell.cc:360),馬上就返回。
  //   ——「部署會佔住引擎好幾分鐘」那個說法是錯的,查證過了。
  if (rs_deploy()) return;

  // ── 拒絕啟動:session 已經收掉了,一定要建回來 ────────────────
  {
    std::lock_guard<std::mutex> lock(err_mu_);
    const char* e = rs_last_error();
    last_error_ = e ? e : "";
    if (last_error_.empty())
      last_error_ = "引擎沒有給原因(多半是已經有一次同步或整理在進行中)";
  }
  {
    RedeployPhase p = phase_.load();
    if (AdvanceRedeploy(&p, RedeployEvent::kStartRefused)) phase_.store(p);
  }
  // 已經在引擎執行緒上了,直接跑,不要再排一件。
  RebuildSessionsOnEngineThread();
  // ⚠ **一定要讓畫面知道這一場結束了。** 設定視窗只看 deploy_seq_,
  //   少了這兩句它會永遠停在「正在整理字詞…已耗時 N 秒」,而那個數字
  //   會一直往上跳 —— 使用者面前是一個永遠不會結束的進度。
  //   順序與 OnDeploy 一樣:先寫結果,後推序號。
  redeploy_start_failed_.store(true);
  deploy_seq_.fetch_add(1);
}

bool Engine::BeginDeploy(uint32_t* out_seq) {
  if (out_seq) *out_seq = deploy_seq_.load();
  // 部署會改寫方案清單(使用者可能剛勾掉一個方案)。清掉快取,
  // 下一次問的時候會退回真的問一次引擎 —— 寧可慢一次,
  // 也不要拿一份舊清單去替使用者挑方案。
  InvalidateSchemaCache();

  // ── 1. 先把門關上(#90)──────────────────────────────────────
  //
  // 關門要在收 session 之前:從這一刻起,建 session 的每一條路都會被
  // SessionCreationAllowed() 擋下來。
  {
    RedeployPhase p = phase_.load();
    if (!AdvanceRedeploy(&p, RedeployEvent::kRequested)) {
      std::lock_guard<std::mutex> lock(err_mu_);
      last_error_ = "已經有一次整理字詞在進行中";
      return false;
    }
    phase_.store(p);
  }

  // ── 2. 收乾淨 + 開始部署,丟給引擎執行緒 ──────────────────────
  //
  // ⚠ **非同步。** 呼叫端是設定視窗的 UI 執行緒,而收 session 要把
  //   使用者剛學到的詞寫回去(rs_session_destroy 才是它落地的時機)——
  //   在那條執行緒上同步等就是 #79 的形狀,而且這裡等的東西比誰都久。
  //   engine.h 的 Post() 上面寫著「設定視窗與懸浮狀態列不可以用它」。
  //
  // ⚠ 而 rs_deploy() 也在那一件工作裡跑,理由見上面那一支的說明。
  if (PostAsync("收乾淨 session 再開始部署",
                [this] { CloseThenDeployOnEngineThread(); }))
    return true;

  // 沒有入列 = 那件事永遠不會發生(引擎在停)。session 一個都沒被收掉,
  // 所以**不必重建**,但門一定要打開 —— 留著關的話,引擎再起來時
  // 沒有任何人會去開它。
  {
    RedeployPhase p = phase_.load();
    if (AdvanceRedeploy(&p, RedeployEvent::kTeardownFailed)) phase_.store(p);
  }
  std::lock_guard<std::mutex> lock(err_mu_);
  last_error_ = "引擎沒有回應,session 沒有收掉 —— 不開始整理字詞";
  return false;
}

bool Engine::PollDeploy(uint32_t since_seq, int* status) {
  if (deploy_seq_.load() == since_seq) return false;
  // ⚠ 「這一次整理連開始都失敗了」要問在讀 deploy_state_ **之前**:
  //   那一格還停在上一次成功的 1,照著它讀會把失敗說成完成。
  if (redeploy_start_failed_.exchange(false)) {
    if (status) *status = -1;
    InvalidateSchemaCache();
    return true;
  }
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
    // ⚠ **不要用空字串蓋掉已經有的原因。** rs_last_error() 是 thread_local
    //   (rime_shell.h 檔頭),而這一支跑在設定視窗的執行緒上 —— 失敗的
    //   原因(如果有)留在**引擎執行緒**的那一份裡。舊版無條件覆寫,
    //   連我們自己記下來的那一句也一起被抹掉,畫面上就變成「沒有給原因」。
    if (e && *e) last_error_ = e;
  }
  return true;
}

std::string Engine::last_error() const {
  std::lock_guard<std::mutex> lock(err_mu_);
  return last_error_.empty() ? init_error_ : last_error_;
}

}  // namespace rimewin
