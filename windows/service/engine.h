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
#include "../common/work_queue.h"

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

  // ── ⚠ 預先建好的備用 session ──────────────────────────────────
  //
  // 量到的(CI run 31316116994,引擎的慢工作記錄):
  //
  //     [engine] 慢工作 建 session       等待=0 ms 執行=442 / 530 / 603 / 753 ms
  //     [engine] 慢工作 套用方案與選項   等待=0 ms 執行=196 / 382 / 492 ms
  //
  //   而正常的時候它們是 46~72 ms。**「等待」全部是 0** —— 沒有人擋在
  //   前面,是 librime 這兩個呼叫本身偶爾就要花掉半秒到四分之三秒。
  //
  // 這件事我們改不掉,而 SESSION_NEW 的預算是 300 毫秒。所以唯一的辦法是
  // **不要在使用者等著的時候建 session**:平常就先建好一個放著,
  // 宿主連上來時直接交出去(只是一次上鎖,不進引擎佇列)。
  //
  // ⚠ 補一個回去也要 442~753 毫秒,而那會擋住按鍵(預算只有 50 毫秒)。
  //   所以補充走的是低優先那條路,而且要等引擎**真的閒下來**
  //   (kLowPriorityIdleMs)。使用者連續打字時引擎一直是忙的 → 不補 →
  //   池子空了就退回當場建立,也就是**最壞情況不比現在差**。
  //
  // ⚠ 計畫不合就不能用:交出去之前要比對「方案 + 選項」與這一次算出來的
  //   是不是同一組。不比對的話,使用者剛改完設定的第一個程式會拿到一個
  //   照舊設定配好的 session,而那種錯誤是靜默的。
  uint64_t TakeSpareSession(uint32_t langid, const std::string& schema_id,
                            const std::vector<OptionAssign>& options);
  void RequestSpareSession(uint32_t langid, const std::string& schema_id,
                           const std::vector<OptionAssign>& options);
  // 暖機專用:**同步**先建好一個。暖機跑在管道打開之前,那時「等引擎閒
  // 下來」既沒有意義也來不及 —— 管道一開就可能有人連上來要它。
  void PrimeSpareSession(uint32_t langid, const std::string& schema_id,
                         const std::vector<OptionAssign>& options);

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

  // ── ⚠ 建 session 那條路徑要用**這一支**,不是上面那一支 ──────────
  //
  // 量到的(CI run 31315693513,引擎的慢工作記錄):
  //
  //     [engine] 慢工作 列方案 等待=0 ms 執行=46~99 ms   ×26
  //     [engine] 慢工作 建 session 等待=0 ms 執行=40~57 ms ×4
  //
  // 也就是說:SESSION_NEW 那 94~110 毫秒裡,**有一半是 rs_schema_list**,
  // 而它問的是一件**全域而且幾乎不變**的事 —— 方案清單只有在重新部署
  // 之後才會變。每一個宿主連上來都重問一次,是把一個常數當成變數。
  //
  // (另外注意「等待」全都是 0:那一輪引擎根本沒有排隊,所以慢的不是
  //  別人擋著,是這件事本身。這也是為什麼答案是快取而不是排程。)
  //
  // 快取由 SchemaList() 自己填,並在部署開始/結束時清掉 ——
  // 清掉之後下一次呼叫會退回真的問一次,所以最壞情況只是回到原本的成本。
  std::vector<std::pair<std::string, std::string>> SchemaListCached();
  void InvalidateSchemaCache();

  // ── ⚠ 有介面掛在上面的那三支 ────────────────────────────────
  //
  // #79 的根因是「UI 執行緒把自己的存活押在引擎的工作何時回來」。
  // 設定視窗與懸浮狀態列各有自己的訊息迴圈,而它們一停,
  // GetMessageW 就再也不會被呼叫 —— 畫面停在最後一格合成的樣子,
  // 任何點擊都沒有人處理。**系統匣圖示也掛在設定視窗那條執行緒上**,
  // 所以連「重開設定」這條退路都會一起沒了。
  //
  // 所以介面走的是下面這三支,而不是上面那兩支:

  // 快取命中就填 out 並回 true。**純記憶體,不碰工作者。**
  bool SchemaListFromCache(
      std::vector<std::pair<std::string, std::string>>* out) const;

  // 非同步問一次方案清單,填好快取之後呼叫 on_ready。
  //
  // ⚠ on_ready 跑在**工作者執行緒**上,不是 UI 執行緒。它裡面只能做
  //   跨執行緒安全的事 —— 設定視窗傳進來的那一份只做一件:PostMessageW。
  // ⚠ on_ready 捕捉的每一樣東西都必須傳值(見 common/work_queue.h)。
  // ⚠ 回傳 false = 沒有入列,on_ready **不會**被呼叫。呼叫端如果用一個
  //   「已經有一件在飛」的旗標擋重複排隊,那個旗標就會永遠卡在 true。
  bool RefreshSchemaListAsync(std::function<void()> on_ready);

  // 有上限的一次方案清單查詢。逾時回 false,而 out 保持不動 ——
  // 呼叫端**必須**據此收手,不可以把一份沒被填過的清單當成
  // 「一個方案都沒有」。
  bool SchemaListForUi(int timeout_ms,
                       std::vector<std::pair<std::string, std::string>>* out);

  // 工作者現在卡在哪一件、卡了多久(毫秒;沒有工作在跑 = 0 / 空字串)。
  //
  // ⚠ 給介面判斷用,**不要把 label 寫進畫面**:那些是內部字眼,
  //   §6.7 第一層硬禁它們出現在使用者看得到的地方。寫進日誌。
  int64_t StalledMs() const { return queue_.StalledMs(); }
  std::string CurrentJobLabel() const { return queue_.CurrentLabel(); }

  // ── ⚠ 介面要判斷「引擎沒有回應」時,要問的是這一支 ────────────
  //
  // 上面那一句「引擎只有一條執行緒,所以『我的請求為什麼慢』的答案
  // 幾乎一定是『**別人**擋在前面』」不是修辭,它有一個直接的後果:
  //
  //   20 件各 500 毫秒排在使用者前面 = 他等 10 秒,
  //   而 StalledMs() 從頭到尾沒有超過 500。
  //
  // 心跳如果只看 StalledMs(),那 10 秒裡一次都不會亮 —— 而那正是
  // 使用者最需要有人跟他說話的 10 秒。要看的是「最舊那件躺了多久」。
  int64_t OldestWaitingMs() const { return queue_.OldestWaitingMs(); }

  // 介面該不該說「引擎正在忙」,看這一個數字就夠:
  // 兩種卡法(自己這件很慢 / 別人擋在前面)使用者感受到的是同一件事。
  int64_t EngineBusyMs() const {
    const int64_t a = queue_.StalledMs();
    const int64_t b = queue_.OldestWaitingMs();
    return a > b ? a : b;
  }

  // 對**目前存在的每一個 session** 套用。設定介面改了字形之後,
  // 使用者不該還要換一個程式才看得到效果。
  // ── ⚠ on_done:非同步的東西必須說得出「做完了沒有、成不成功」 ──
  //
  //   這兩支改成非同步之後,設定視窗那一頭原本是**無條件**在按下的
  //   當下就說「已套用」—— 它在替一件還躺在佇列裡的工作背書。引擎
  //   卡著的時候那句話是假的,而工作失敗時畫面上什麼都沒有。
  //
  //   所以它們現在收一個完成通知。⚠ `on_done` 跑在**工作者執行緒**上,
  //   不是 UI 執行緒;裡面只能做跨執行緒安全的事(設定視窗傳進來的
  //   那一份只做一件:PostMessageW)。捕捉的每一樣東西都要傳值。
  //
  //   ⚠ 工作**沒有入列**時(引擎在停)也一定要叫一次 on_done(false)——
  //     不叫的話,畫面上那句「正在套用…」會永遠停在那裡。
  void SetOptionAll(const char* option, bool value,
                    std::function<void(bool)> on_done = {});

  // ── ⚠ 中英切換:這一輪之前 Windows 端**完全沒有**這個功能 ──────
  //
  //   `ascii_mode` 在整個 windows/ 底下只被讀過兩次(engine.cc 的狀態
  //   打包、protocol.h 的旗標定義),**一次都沒有被設定過**。
  //   使用者要在句子中間打一個英文字,唯一的辦法是按 Win+空白鍵
  //   把整個輸入法換掉。
  //
  //   這裡把它變成一個**行程層級**的狀態:懸浮狀態列切它,現有的每一個
  //   session 立刻跟著變,而**新建的 session 也要跟著** ——
  //   少了後面那一半,使用者切成英文之後開一個新程式又會變回中文,
  //   而那看起來像「這個開關會自己跳回去」。
  //
  //   新 session 那一半靠 pipe_server 把它放進 options(見那裡的說明):
  //   options 同時是備用 session 的「計畫」,所以比對自然會涵蓋它 ——
  //   不然使用者切完中英之後拿到的第一個 session 會是照舊狀態配好的,
  //   而那種錯誤是靜默的。
  //
  //   ⚠ 這是**模式**不是偏好,所以刻意**不落地**:重開機回到中文,
  //     與每一家中文輸入法的行為一致。
  void SetAsciiModeAll(bool on);

  // Ctrl+空白鍵走這一條:切掉行程層級的中英模式,並回一份**新的**快照。
  //
  // ⚠ 一定要回快照,而且要在切換**之後**才取。兩個理由:
  //
  //   1. 懸浮狀態列那一格顯示的是快照上的旗標(status_bar.h:「顯示的是
  //      引擎說的狀態,不是我們以為的」),回一份切換前的快照就是一個
  //      說謊的指示器 —— 使用者按了鍵、模式真的變了,而畫面上還是舊的。
  //   2. **更要緊的**:這顆鍵存在的理由是「中英切換發生在句子中間」
  //      (common/hotkey_policy.h 的檔頭),而 librime 在切到英數的當下會
  //      把手上那一段組字上屏。那份 commit 在 rs_snapshot_acquire 的當下
  //      就被消費(見 TakeSnapshotLocked 的檔頭),所以它只會現身一次 ——
  //      取早了,它落在下一次 acquire,而那一次沒有人在等。
  //
  // ⚠ 這一支是在**管道執行緒**上被呼叫的(pipe_server.cc 每個 client 一條
  //   std::thread),所以裡面碰 sessions_ / rs_* 的每一步都必須包在 Post()
  //   裡。不包的話是無鎖讀 sessions_,而且與引擎執行緒上的 rs_process_key
  //   並行呼叫 rs_*。
  //
  // ⚠ 部署還沒完成時回 handled=false(與 ProcessKey 同一條規則)——
  //   宣稱切過了卻拿不出東西,狀態列就會說謊,而那顆鍵已經被吃掉了。
  Result ToggleAsciiMode(uint64_t id);
  bool AsciiMode() const { return ascii_mode_.load(); }
  void SelectSchemaAll(const std::string& schema_id);

  // 記住這個 session 是從哪一個語言設定檔來的。
  // 設定介面改了簡繁之後要對**現有的**每一個 session 重套一次,
  // 而每個 session 的語言不一樣(每個宿主進程各有一個)。
  // 少了這一格,使用者改完設定要換一個程式才看得到效果 ——
  // 而他當下看到的是「這個下拉沒有作用」。
  void SetSessionLangId(uint64_t id, uint32_t langid);
  // on_done 的約定見 SetOptionAll。
  void ApplyVariantAll(const SchemaPreference& pref,
                       std::function<void(bool)> on_done = {});

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
  // 丟工作並**等它做完,沒有上限**。
  //
  // ⚠ 這一支只留給**沒有訊息迴圈掛在上面**的路徑:管道執行緒(宿主那一側
  //   自己有 50 毫秒的預算,逾時就 fail-open)、暖機、--selftest。
  //   設定視窗與懸浮狀態列**不可以**用它 —— 那就是 #79。
  //
  // ⚠ label 不是裝飾。引擎只有一條執行緒,所以「我的請求為什麼慢」的答案
  //   幾乎一定是「**別人**擋在前面」,而以前記錄裡完全看不出那個別人是誰:
  //   2026-08-09 CI 上有一次 SESSION_NEW 花了 1328 ms(建 session 1234 ms),
  //   旁邊那幾次是 15~47 ms,而沒有任何線索指出那 1.2 秒引擎在做什麼。
  //   現在每一件慢工作都會自己report:等了多久、跑了多久、叫什麼名字。
  //
  // ⚠ 沒有標籤的那個多載**故意拿掉了**。它讓「哪一件工作卡住」在記錄裡
  //   變成「(沒有標籤)」,而那正是出事時唯一有用的一格。
  void Post(const char* label, std::function<void()> fn);
  // 丟了就走,**不等**。⚠ fn 捕捉的東西一律傳值(見 common/work_queue.h):
  //   這一支返回時工作通常還沒開始跑,呼叫端的框隨時會消失。
  // ⚠ 回傳 false = **沒有入列**,那件工作永遠不會跑。等完成通知的
  //   呼叫端一定要看它(見 common/work_queue.h 的 Post)。
  bool PostAsync(const char* label, std::function<void()> fn);
  // 丟一件「有空再做」的工作:不等它,而且**優先權比一般工作低**
  // (要等引擎閒下來 kLowPriorityIdleMs 才會被撿走)。
  void PostLow(const char* label, std::function<void()> fn);
  // ⚠ 只能在工作者執行緒上呼叫:真的問一次 rs_schema_list 並填快取。
  std::vector<std::pair<std::string, std::string>> ListSchemasOnWorker();

  // ⚠ 只能在引擎執行緒上呼叫(直接碰 sessions_ / next_id_ 與 rs_*)。
  void MakeSpareOnEngineThread(uint32_t langid, const std::string& schema_id,
                               const std::vector<OptionAssign>& options);

  // 以下三個只在引擎執行緒上呼叫。
  Snapshot TakeSnapshot(uint64_t id);
  Snapshot TakeSnapshotLocked(uintptr_t sess);
  uintptr_t Find(uint64_t id) const;

  // 只由 Start() / Stop() 寫,而那兩支都在呼叫端(main)那一條執行緒上。
  bool started_ = false;

  std::map<uint64_t, uintptr_t> sessions_;
  std::map<uint64_t, uint32_t> session_lang_;
  uint64_t next_id_ = 1;

  // 0 = 還沒有結果, 1 = 成功, -1 = 失敗。只由部署回呼寫,別處只讀。
  // 中英模式。行程層級,不落地(見 SetAsciiModeAll)。
  std::atomic<bool> ascii_mode_{false};
  std::atomic<int> deploy_state_{0};
  // 每收到一次**終局**的部署結果就加一。見 BeginDeploy 的說明:
  // 沒有這個序號的話,上一輪的結果會被讀成這一輪的。
  std::atomic<uint32_t> deploy_seq_{0};
  // 方案清單的快取。與 mu_ 分開:填快取的人剛從引擎執行緒回來,
  // 而讀快取的人(連線執行緒)完全不該碰引擎的佇列鎖。
  mutable std::mutex cache_mu_;
  std::vector<std::pair<std::string, std::string>> schema_cache_;

  // 預先建好的備用 session,一個 langid 一個。
  //
  // ⚠ 為什麼一個就夠:一個 langid 只在「使用者開了一個新程式」時被取走,
  //   而兩個程式在同一個 kLowPriorityIdleMs 之內接連開起來是少見的;
  //   真的撞上就退回當場建立(不比現在差)。**先用量到的東西決定要不要
  //   加大**,不要憑感覺放三個 —— 每一個都是一份常駐的 librime session。
  struct SparePlan {
    uint64_t session = 0;
    std::string schema_id;
    std::vector<OptionAssign> options;
  };
  mutable std::mutex spare_mu_;
  std::map<uint32_t, SparePlan> spare_;
  // 這個 langid 已經排了一件補充工作,不要重複排。
  std::map<uint32_t, bool> spare_pending_;

  std::string init_error_;
  mutable std::mutex err_mu_;
  std::string last_error_;

  // ── 工作佇列 ────────────────────────────────────────────────
  //
  // ⚠ **宣告在最後一個是刻意的。** 成員的解構順序與宣告順序相反,
  //   所以它會**最先**被銷毀 —— 而它的解構子會把工作者 join 掉。
  //   反過來的話,工作者還在跑的時候 sessions_ / spare_ 已經死了。
  //   (~Engine() 也會先呼叫 Stop(),這裡是第二道。)
  //
  // 佇列本身 —— 含有界等待,以及「逾時之後遲到的工作寫進誰的記憶體」
  // 那一格 —— 在 common/work_queue.h,由 tests/test_work_queue.cc 在
  // Ubuntu 上直接測(含 --asan)。那一格以前住在這個檔案裡,
  // 而這個檔案在開發機上編不起來 = 沒有人驗得到。
  WorkQueue queue_;
};

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_ENGINE_H_
