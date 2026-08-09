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

#include <utility>
#include <vector>

#include "../common/protocol.h"
#include "../common/schema_choice.h"

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

  // ── ⚠ 引擎只有一條執行緒,而客戶端有兩個很緊的預算 ──────────
  //
  //   建立 session 的往返:300 毫秒(ipc_client.cc 的 kConnectTimeoutMs)
  //   每一顆按鍵的往返  : **50 毫秒**(kKeyTimeoutMs)
  //
  // 兩個都跑在宿主的 UI 執行緒上,所以都不能調大 —— 調大等於讓使用者
  // 按鍵時整個程式卡住那麼久。超過就 fail-open:那個宿主打不出中文,
  // 而使用者只看到英文,沒有任何錯誤訊息。
  //
  // ⚠ **不要用「丟到佇列裡非同步做」來解這件事。** 試過,而且量到它更糟:
  //   把 rs_select_schema 從 SESSION_NEW 移出去之後,成本並沒有消失,
  //   它只是從 300 毫秒的預算搬進了 50 毫秒的預算,於是第一顆按鍵變成
  //   「TestKeyDown 說吃、KeyDown 說不吃」—— 那顆鍵在真的宿主裡會直接
  //   消失,比原本的症狀更糟。完整的量測記錄在 pipe_server.cc 的
  //   kSessionNew 那一段。
  //
  //   貴的工作要嘛在**服務暖機時**做完(main.cc 的 WarmUpEngine),
  //   要嘛就留在同步路徑上讓 §6e 量得到它。
  //
  // 下面這一支是安全的那一種:它移走的是**等待**,不是工作。
  // 離開的宿主不需要陪著詞典寫回去;工作本身仍然在引擎執行緒上、順序不變。
  void EndSessionAsync(uint64_t id);

  Result ProcessKey(uint64_t id, int32_t keysym, uint32_t mods);
  Result SelectCandidate(uint64_t id, int32_t index);
  Result CommitComposition(uint64_t id);
  Result Clear(uint64_t id);
  Result ChangePage(uint64_t id, bool backward);
  Result SelectSchema(uint64_t id, const std::string& schema_id);

  // ── 設定介面要用的 ──────────────────────────────────────────
  //
  // rs_schema_list 不吃 session(方案清單是全域的),但仍然走引擎執行緒:
  // 它回傳的字串有生命週期,而別的執行緒同時在呼叫 rs_* 的話那份緩衝
  // 會被踩掉。這裡在引擎執行緒上把字串複製出來再回來。
  std::vector<std::pair<std::string, std::string>> SchemaList();

  // 對**目前存在的每一個 session** 套用。設定介面改了字形之後,
  // 使用者不該還要換一個程式才看得到效果。
  void SetOptionAll(const char* option, bool value);
  void SelectSchemaAll(const std::string& schema_id);

  // 記住這個 session 是從哪一個語言設定檔來的。
  // 設定介面改了簡繁之後要對**現有的**每一個 session 重套一次,
  // 而每個 session 的語言不一樣(每個宿主進程各有一個)。
  // 少了這一格,使用者改完設定要換一個程式才看得到效果 ——
  // 而他當下看到的是「這個下拉沒有作用」。
  void SetSessionLangId(uint64_t id, uint32_t langid);
  void ApplyVariantAll(const SchemaPreference& pref);

  bool SetOption(uint64_t id, const char* option, bool value);
  std::string SchemaOfSession(uint64_t id);

  // 把「這個語言該用什麼」套到一個 session 上。回傳實際選中的方案 id
  // (沒有選就回空字串)。
  std::string ApplyChoice(uint64_t id, const std::string& schema_id,
                          const std::vector<OptionAssign>& options);

  int AbiVersion() const;

  // ── 部署 ────────────────────────────────────────────────────
  //
  // ⚠ rs_deploy_callback 不在呼叫端的執行緒上,而且可能在 rs_deploy()
  //   早就返回之後才觸發。所以「呼叫過了所以做完了」是錯的。
  //
  // ⚠ 更陰險的是**上一輪的結果**:直接讀一個 atomic 狀態的話,
  //   剛啟動時那一次首次部署的 SUCCESS 會被當成這一次的結果,
  //   於是使用者按下按鈕的瞬間就看到「完成」。所以這裡用序號:
  //   BeginDeploy 記下當下的序號,PollDeploy 只認**比它新**的終局狀態。
  //   (Android 端用 AtomicBoolean armed 解同一個問題。)
  //
  // BeginDeploy 回傳 false = rs_deploy() 拒絕啟動(多半是已經有一個
  // 部署在進行中)。呼叫端**必須**把這件事說出來,不可以靜靜地什麼都不做。
  bool BeginDeploy(uint32_t* out_seq);
  // 回傳 false = 還沒結束。true 時 *status:1 = 成功,-1 = 失敗。
  bool PollDeploy(uint32_t since_seq, int* status);

  std::string last_error() const;

 private:
  void ThreadMain();
  // 丟工作並**等它做完**。
  //
  // ⚠ label 不是裝飾。引擎只有一條執行緒,所以「我的請求為什麼慢」的答案
  //   幾乎一定是「**別人**擋在前面」,而以前記錄裡完全看不出那個別人是誰:
  //   2026-08-09 CI 上有一次 SESSION_NEW 花了 1328 ms(建 session 1234 ms),
  //   旁邊那幾次是 15~47 ms,而沒有任何線索指出那 1.2 秒引擎在做什麼。
  //   現在每一件慢工作都會自己report:等了多久、跑了多久、叫什麼名字。
  void Post(const char* label, std::function<void()> fn);
  void Post(std::function<void()> fn) { Post("(沒有標籤)", std::move(fn)); }

  // 以下三個只在引擎執行緒上呼叫。
  Snapshot TakeSnapshot(uint64_t id);
  Snapshot TakeSnapshotLocked(uintptr_t sess);
  uintptr_t Find(uint64_t id) const;

  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  // 一件排隊中的工作。帶著標籤與入列時間,好把「等了多久」與「跑了多久」
  // 分開 —— 那兩個數字要修的地方完全不同(前者是別人擋著,後者是自己慢)。
  struct Job {
    std::function<void()> fn;
    const char* label = "(沒有標籤)";  // 一律是字面值,不必管生命週期
    int64_t enqueued_ms = 0;
  };
  std::deque<Job> queue_;
  // 「有空再收」的 session。與 queue_ 分開,而且**優先權比它低** ——
  // 收掉一個已經走掉的 session 沒有人在等,建立一個新的才有人在等。
  // 完整的理由見 ThreadMain。
  std::deque<uint64_t> pending_destroy_;
  // 引擎最後一次跑「有人在等的」工作是什麼時候(steady clock,毫秒)。
  int64_t last_normal_ms_ = 0;
  bool stop_ = false;
  bool started_ = false;

  std::map<uint64_t, uintptr_t> sessions_;
  std::map<uint64_t, uint32_t> session_lang_;
  uint64_t next_id_ = 1;

  // 0 = 還沒有結果, 1 = 成功, -1 = 失敗。只由部署回呼寫,別處只讀。
  std::atomic<int> deploy_state_{0};
  // 每收到一次**終局**的部署結果就加一。見 BeginDeploy 的說明:
  // 沒有這個序號的話,上一輪的結果會被讀成這一輪的。
  std::atomic<uint32_t> deploy_seq_{0};
  std::string init_error_;
  mutable std::mutex err_mu_;
  std::string last_error_;
};

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_ENGINE_H_
