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

#include "../common/key_deadline.h"
#include "../common/protocol.h"
#include "../common/redeploy_flow.h"
#include "../common/schema_choice.h"
#include "../common/work_queue.h"

namespace rimewin {

class Engine {
 public:
  Engine();
  ~Engine();

  // 先在呼叫端執行緒上做 rs_init,成功後再啟動引擎執行緒。
  // 部署是非同步的,本函式不等它完成 —— 那段時間裡使用者已經在打字了
  // (服務會對每一顆按鍵立刻回「沒處理」)。
  //
  // ⚠ 「要多久」有**兩個不同的答案**,而它們都在 common/first_run_timing.h:
  //   出貨預設方案是十幾秒(kFirstDeployTypical*,**使用者讀得到的句子
  //   一律用這一組**),而使用者自己灌了很大的詞典時要留到分鐘級
  //   (kDeployWaitBudgetSec,那是**等待預算**不是期待值)。
  //   這裡不再複述任何一個數字 —— 四份碼各講一個數字正是 W3 那個缺陷,
  //   而它的代價是完成頁對一個只要等十幾秒的人說「一到數分鐘」。
  bool Start(const std::string& shared_dir, const std::string& user_dir,
             const std::string& log_dir);
  void Stop();

  bool deploy_done() const { return deploy_state_.load() != 0; }
  bool deploy_ok() const { return deploy_state_.load() == 1; }

  // ── 重新部署現在走到哪一格(#90)────────────────────────────
  //
  // ⚠ deploy_done() / deploy_ok() **答不了這個問題**:deploy_state_ 首次
  //   部署成功之後永遠是 1,使用者按「重新整理字詞」時它不會退回去。
  //   「現在能不能打字」「現在能不能建 session」「詞庫檔現在能不能被
  //   改寫」三個問題全部由這一格 + common/redeploy_flow.h 的純函式回答。
  RedeployPhase redeploy_phase() const { return phase_.load(); }

  // ── 部署之後 session 要照什麼重建(#90 / #85)──────────────
  //
  // 重建時要重套方案 / 簡繁 / 標點 / 中英。那些是由設定檔 + 語言設定檔
  // 決定的,而只有 pipe_server 讀得到它們 —— 所以由它注入。
  //
  // ⚠ **必須與建 session 時走同一支**。兩份會漂移,而漂移的症狀是
  //   「重新整理字詞之後,設定悄悄回到預設」,使用者不會把那兩件事
  //   聯想在一起。
  //
  // ⚠ 這個回呼在**引擎執行緒**上被呼叫,所以它**不可以**再丟工作進引擎
  //   佇列(那是自己等自己)。方案清單由引擎先問好再傳進去。
  struct SessionPlan {
    std::string schema_id;
    std::vector<OptionAssign> options;
  };
  using SessionPlanner = std::function<SessionPlan(
      uint32_t langid,
      const std::vector<std::pair<std::string, std::string>>& schemas)>;
  // ⚠ **走引擎執行緒設定,不是就地指派。** planner_ 是在那條執行緒上被讀
  //   的(重建 session 那一件工作),從別條執行緒直接寫就是一個資料競爭,
  //   而它的形狀是「服務關閉時偶爾崩一次」—— 那種崩潰查起來最貴。
  //   傳一個空的 std::function 進來就是解除註冊(PipeServer 的解構子)。
  void SetSessionPlanner(SessionPlanner p);

  // 部署有了終局(成功或失敗)時由部署回呼呼叫。
  //
  // ⚠ **從部署回呼的執行緒**上呼叫,而 rime_shell 那時還持有它的全域鎖
  //   (見 #91)。所以這一支只准動一個 atomic 與排一件工作 ——
  //   在這裡直接呼叫任何 rs_* 都是一個真的死鎖。
  //
  // ⚠ 觸發點在這裡而不是設定視窗的計時器上,是刻意的:那個視窗可以被
  //   關掉,而關掉之後就再也沒有人會把 session 建回來。
  //
  // ⚠ 而且它是在 engine.cc 那個 CallbackGate 的鎖**裡面**跑的：那把鎖是
  //   「Stop() 返回時保證沒有回呼還在用這個 Engine」唯一的來源
  //   （common/callback_gate.h）。所以這一支做得越久，Stop() 就陪著等越久
  //   —— 它只該動 atomic 與排工作，不可以在裡面等任何東西。
  //
  // deploy_ok = 這一場部署成功還是失敗。兩者都要把 session 建回來；
  // 失敗那一條尤其重要（見 .cc）。
  void OnDeployTerminal(bool deploy_ok);
  // 等待首次部署完成。只給 --selftest 用;正常執行不等。
  bool WaitDeploy(int seconds);

  uint64_t NewSession();
  void EndSession(uint64_t id);

  // ── ⚠ 引擎只有一條執行緒,而客戶端有兩個很緊的預算 ──────────
  //
  //   建立 session 的往返:300 毫秒(ipc_client.cc 的 kConnectTimeoutMs)
  //   每一顆按鍵的往返  : **150 毫秒**(kKeyTimeoutMs)
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
  // ⚠ 補一個回去也要 442~753 毫秒,而那會擋住按鍵(預算只有 150 毫秒)。
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

  // ── 一顆按鍵的等待上限 ────────────────────────────────────────
  //
  // ⚠ 數字**不在這裡** —— 它與 DLL 那一側的逾時是一組,兩個一起才有
  //   意義,所以兩個都住在 common/key_deadline.h,由那裡的 static_assert
  //   守住「服務端必須先放棄,而且留得下管道的時間」這條關係。
  //   上一輪它們各寫一份、關係只寫在註解裡 —— 而註解守不住東西。

  // 一顆按鍵等待的結果。給 pipe_server 記錄用 ——
  // 「使用者說間歇打不出中文」要變成一個數字,就是從這裡出去的。
  struct KeyWait {
    // 在 kKeyDeadlineMs 內沒有等到(或工作根本沒入列)。
    bool timed_out = false;
    // 本體**確定一步都沒跑**(我們先搶到作廢權)。
    //
    // ⚠ timed_out 而 abandoned 為 false 是那個誠實的殘留窗口:
    //   工作剛好在我們放棄的同一瞬間進到 rs_process_key 裡了。
    //   那一顆鍵有可能引擎組了字、而宿主也自己打了字。
    //   記下來,不要假裝沒有。
    bool abandoned = false;
    // ⭐ #119:**這顆鍵已經對引擎或設定做過不可逆的事。**
    //
    // ⚠ 它與 timed_out / abandoned 都無關,而那正是要點:
    //   ToggleAsciiMode() 的 SetAsciiModeAll() 是 store + PostAsync,
    //   **不等** —— 呼叫過就已經切了,而它排在那趟有上限的取快照
    //   **前面**。逾時的時候「沒等到」是真的,「什麼都沒發生」是假的。
    //
    // 呼叫端(pipe_server.cc 的 case Op::kKey)拿它餵
    // common/key_deadline.h 的 PlanKeyExit() —— 那是 `case Op::kKey` 上
    // 唯一算得出 Result::handled 的地方,而第一條硬性要求就是
    // 「副作用發生了 ⇒ 那顆鍵一定算被我們吃掉」。
    bool side_effect_done = false;
  };

  // ⚠ wait **不是選用的**。逾時時回的那個 Result 是一個佔位,不是
  //   引擎的現況(見 KeyWait 上面那一段與 common/key_deadline.h);
  //   呼叫端只能靠 wait->timed_out 分辨它,所以不給預設值 ——
  //   少了它,那一份佔位會被當成快照餵進候選窗與那一橫。
  // ⚠ deadline_ms 是**這顆鍵**剩下的預算,不是「一趟佇列的預算」。
  //   呼叫端在收到這一格之前可能已經花掉一部分(pipe_server 那道
  //   ClassifyHotkey 要讀設定檔,實測會慢),而 DLL 那側的時鐘從送出
  //   那一刻就在跑。算法在 common/key_deadline.h 的 RemainingKeyBudgetMs()。
  Result ProcessKey(uint64_t id, int32_t keysym, uint32_t mods,
                    int deadline_ms, KeyWait* wait);
  Result SelectCandidate(uint64_t id, int32_t index);
  Result CommitComposition(uint64_t id);
  Result Clear(uint64_t id);
  Result ChangePage(uint64_t id, bool backward);

  // ── 只取一份當下的快照,什麼都不動 ────────────────────────────
  //
  // ⚠ 給「這顆鍵我們自己處理掉了,但文件不該有任何變化」的路徑用
  //   (簡繁快捷鍵 Ctrl+Shift+F)。**不可以回一份空的 Result** ——
  //   DLL 那一側收到 handled=1 之後會把 snap 套進文件
  //   (tsf/text_service.cc 的 SendAsciiToggle,那裡有一整段 ⚠ 記著
  //   「這一份快照不可以丟掉」),而空快照的意思是「沒有組字、沒有
  //   候選」:使用者打到一半的那一段會當場消失。
  //
  // ⚠ 它在**按鍵那條路上**(簡繁快捷鍵的第二趟),所以與 ProcessKey
  //   同一套:有上限、有作廢權、逾時回一份預設建構的 Result 而呼叫端
  //   **不可以**拿它碰 UI(common/key_deadline.h 的 DecideKeyUiAction)。
  //   上一輪只把 ProcessKey 收掉,而這一支仍然是 Post() = 永遠等 ——
  //   同一條路上的同一個缺陷,守門看不到而已。
  Result CurrentResult(uint64_t id, int deadline_ms, KeyWait* wait);
  Result SelectSchema(uint64_t id, const std::string& schema_id);

  // ── 設定介面要用的 ──────────────────────────────────────────
  //
  // rs_schema_list 不吃 session(方案清單是全域的),但仍然走引擎執行緒:
  // 它回傳的字串有生命週期,而別的執行緒同時在呼叫 rs_* 的話那份緩衝
  // 會被踩掉。這裡在引擎執行緒上把字串複製出來再回來。
  // ── ⚠ 回傳的是**狀態**,不是一個分不出來的空 vector ──────────
  //
  //   舊版是 `std::vector<...> SchemaList()`,而它把 `queue_.Call()` 的
  //   `WorkQueue::Status` 整個丟掉。於是「引擎在停,這件工作根本沒有
  //   入列」與「一個方案都沒有」在呼叫端是**同一個空 vector** ——
  //   這個專案反覆吃虧的形狀:三種不同的失敗在畫面上是同一句話。
  //
  //   kDone    = out 是答案(可能真的是空的,那也是答案)
  //   kStopped = 工作沒有入列也沒有跑,**out 一個位元組都沒有被動過**
  //   (timeout 是 0 = 永遠等,所以這一支不會回 kTimeout。)
  [[nodiscard]] WorkQueue::Status SchemaList(
      std::vector<std::pair<std::string, std::string>>* out);

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
  // 回傳值的意思與 SchemaList 相同(快取命中一律是 kDone)。
  [[nodiscard]] WorkQueue::Status SchemaListCached(
      std::vector<std::pair<std::string, std::string>>* out);
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

  // 快取**有效**就填 out 並回 true。**純記憶體,不碰工作者。**
  //
  // ⚠ 「有效」不等於「不是空的」。舊版的判準是 `schema_cache_.empty()`,
  //   於是「真的一種方案都沒有」永遠被當成「還沒問過」:設定視窗每次
  //   打開都再排一件查詢,懸浮狀態列那個選單永遠停在「正在讀方案…」,
  //   而使用者永遠看不到「目前一種都沒有」那句實話。
  //   現在空與無效是兩件事,由 schema_cache_valid_ 分開。
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
  //
  // ⚠ **Ctrl+空白是中文輸入法上最常按的那顆鍵**,所以它與 ProcessKey
  //   一樣要有上限:上一輪這一支還是 Post("切中英後取快照") = 永遠等。
  //   使用者 Alt+Tab 到另一個程式(那會排一次 SESSION_NEW,實測
  //   442~753ms)之後按下去 —— 服務端無上限地等,DLL 150ms 就 Fail()
  //   → Close(),整條連線被丟掉,那個程式接下來只能打英文。
  //   前面那道 ShouldFailOpen 擋不住它:它只認「部署中」,而 SESSION_NEW
  //   的 ApplyChoice、EndSessionAsync 寫回詞典、補備用 session 這些排在
  //   同一條 FIFO 前面的慢工作它一件都看不到。
  //
  // ⚠ 逾時的處置與 ProcessKey 一致:回 handled=false 讓宿主自己收尾,
  //   而且**不碰 UI**。中英模式本身仍然會晚一點才切(SetAsciiModeAll 是
  //   PostAsync,已經在佇列裡了)—— 那是刻意的:使用者按 Ctrl+空白 要的
  //   就是切,而 handled=false 交回宿主的 Ctrl+空白 不會往文件裡寫字元。
  Result ToggleAsciiMode(uint64_t id, int deadline_ms, KeyWait* wait);
  bool AsciiMode() const { return ascii_mode_.load(); }
  // ⚠ 只記下行程層級那一格,**不動任何 session**。
  //
  //   用途只有一個:懸浮那一橫回讀到「使用者正在用的那個 session 現在
  //   是 X」之後,把 X 記回來,好讓下一次「切一下」的方向與畫面上那個
  //   字是同一個來源。兩邊分岔的樣子是「點了沒反應」——
  //   畫面說中、方向從行程層級算(已經是 En)→ 再點送的還是同一個值。
  //
  //   ⚠ 不可以拿它代替 SetAsciiModeAll:那一支要真的去改每一個 session,
  //     而這一支刻意什麼都不改(它記的是**觀察到的事實**,不是命令)。
  void NoteAsciiModeFromSession(bool on) { ascii_mode_.store(on); }
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

  // ── 向引擎回讀「現在到底是什麼狀態」──────────────────────────
  //
  // ⚠ 這一支存在的理由是一個真的缺陷:懸浮狀態列那三格以前是**樂觀
  //   寫入** —— 點下去就自己翻,不等任何引擎的證據。理由當時很實際
  //   (那三格唯一的更新路徑是 OnSnapshot,而 OnSnapshot 要等使用者
  //   真的打一個字),但代價是那一橫可以顯示一個從來沒有發生過的狀態。
  //   使用者回報的「畫面說简、打出來是繁」就是那個形狀的一種。
  //
  //   回讀是**證據**,而且一樣不需要使用者先打一個字。
  //
  // ⚠ 刻意用 rs_get_option 而不是 rs_snapshot_acquire:acquire 會在當下
  //   消費掉待取的 commit(rime_shell.h 檔頭),而這一支是使用者點那一橫
  //   時呼叫的 —— 那時完全可能有一個還沒送到宿主的 commit。
  //   吃掉它的症狀是「打到一半的字消失了」,而且查不出來。
  //
  // 回傳 protocol.h 的 status_flags 形式,與 OnSnapshot 吃的是同一種
  // 東西 —— 兩條路徑產生兩種格式的話,那一橫就會有兩套解讀。
  struct StatusReadback {
    bool ok = false;  // 一個活著的 session 都沒有 → 什麼都不要改
    uint32_t status_flags = 0;
  };
  // ⚠ session_id = 0 代表「沒有指定」—— 那時退回去挑一個活著的,
  //   再退到備用池。**指定的時候一定要問指定的那一個**:13 個宿主各有
  //   自己的 ascii_mode,挑第一個等於擲骰子。
  //
  // ⚠ deadline_ms > 0 = 有上限 + 作廢權(**按鍵那條路一律要給**:
  //   簡繁快捷鍵 Ctrl+Shift+F 的第一趟就是它)。deadline_ms <= 0 =
  //   舊行為,永遠等 —— 只留給沒有訊息迴圈掛在上面、也不在按鍵路徑上的
  //   呼叫端(那一橫自己的重整)。逾時時 ok 維持 false,呼叫端照既有
  //   約定「什麼都不要改」。
  StatusReadback ReadBackStatus(uint64_t session_id = 0, int deadline_ms = 0,
                                KeyWait* wait = nullptr);

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
  // ⚠ 這一支只留給**沒有訊息迴圈掛在上面、而且不在按鍵那條路上**的
  //   路徑:暖機、--selftest、以及管道執行緒上那些不是按鍵的訊息。
  //   設定視窗與懸浮狀態列**不可以**用它 —— 那就是 #79。
  //
  // ⚠ **「宿主那一側自己有 150 毫秒的預算,逾時就 fail-open」是錯的,
  //   而它在這裡掛了兩輪。** 逾時**不是** fail-open:tsf/ipc_client.cc 的
  //   Fail() 第一句就是 Close() —— 整條連線被丟掉、session_ 歸零。那個
  //   宿主接下來要重連、重建 session(實測 442~753ms)、重套方案,而
  //   期間每一顆鍵都是 fail-open 的英文。一顆慢鍵的代價不是「這顆鍵」,
  //   是一整段打不出中文,而且它會自己餵自己(#93/#108 的正回饋迴圈)。
  //
  // ⚠ 所以**按鍵那條路上一條都不准走這一支**。那條路用
  //   CallKeyBounded()(見下面),而守門在 run_logic_tests.sh 的
  //   「按鍵那條路」那一段 —— 它從 pipe_server.cc 的 case Op::kKey
  //   把出口數出來,不是認 Post("按鍵") 那個字串。
  //
  // ⚠ label 不是裝飾。引擎只有一條執行緒,所以「我的請求為什麼慢」的答案
  //   幾乎一定是「**別人**擋在前面」,而以前記錄裡完全看不出那個別人是誰:
  //   2026-08-09 CI 上有一次 SESSION_NEW 花了 1328 ms(建 session 1234 ms),
  //   旁邊那幾次是 15~47 ms,而沒有任何線索指出那 1.2 秒引擎在做什麼。
  //   現在每一件慢工作都會自己report:等了多久、跑了多久、叫什麼名字。
  //
  // ⚠ 沒有標籤的那個多載**故意拿掉了**。它讓「哪一件工作卡住」在記錄裡
  //   變成「(沒有標籤)」,而那正是出事時唯一有用的一格。
  // ⚠ 回傳 kStopped = 引擎在停(或還沒起來),**工作沒有入列也沒有跑**。
  //   呼叫端讀到的 out 是它自己初始化的那個值,不是答案 —— 丟掉這個
  //   回傳值就是把「引擎沒有回應」變成「答案是空的」。
  WorkQueue::Status Post(const char* label, std::function<void()> fn);

  // ── 按鍵那條路唯一的入口:有上限 + 作廢權 ──────────────────────
  //
  // ⚠ `case Op::kKey` 上**每一個**會進引擎佇列的出口都要走這一支,
  //   不是只有 ProcessKey。三個出口都在:ProcessKey、ToggleAsciiMode
  //   (Ctrl+空白 / 輕點 Shift)、以及簡繁快捷鍵那兩趟
  //   (ReadBackStatus → CurrentResult)。少一個,那一個就是無上限的
  //   Post = 永遠等,而症狀與其他兩個一模一樣:整條連線被丟掉。
  //
  // ⚠ deadline_ms <= 0 代表「這顆鍵的預算已經用完」——
  //   **直接當成逾時,不入列**。入列只會讓那件工作遲到之後撞上一個
  //   已經放棄的呼叫端,而 0 傳進 WorkQueue::Call 的意思正好是
  //   「永遠等」(work_queue.cc)。
  //
  // ⚠ fn 一律要自己擁有它讀寫的東西(shared_ptr 的盒子),不可以 [&]:
  //   逾時之後這個函式就返回了,而那件工作可能還在佇列裡。
  //   理由整段寫在 common/work_queue.h 的檔頭。
  //
  // 回傳 true = 工作真的跑完了,盒子裡是引擎的現況。
  // 回傳 false = 沒等到;*wait 帶出 timed_out / abandoned,而那一份
  //   結果**不是現況**,呼叫端不可以拿它碰 UI。
  bool CallKeyBounded(const char* label, std::function<void()> fn,
                      int deadline_ms, KeyWait* wait);
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

  // ── #90:部署前後的 session 生命週期 ────────────────────────
  //
  // ⚠ 兩支都**只能在引擎執行緒上呼叫**。
  //
  // 收乾淨:把每一個 session(含備用池)銷毀,並把**宿主的**那些記進
  // parked_。銷毀同時是使用者剛學到的詞落地的唯一時機。
  void CloseAllSessionsOnEngineThread();
  // 建回來:照 parked_ 重建,並用 planner_ 重套方案 / 簡繁 / 標點 / 中英。
  void RebuildSessionsOnEngineThread();
  // 收乾淨 → 開始部署。**兩件事在同一件工作裡**,所以中間插不進任何
  // 東西 —— 這是「詞庫檔被改寫時一個 session 都不在」唯一的保證。
  // ⚠ 只能在引擎執行緒上呼叫。
  void CloseThenDeployOnEngineThread();
  // 把重建排進佇列。部署的終局回呼走它。
  void RebuildSessionsAsync();
  // 宿主在部署期間關掉了 —— 把它從「等著被建回來」的名單上劃掉。
  void ForgetParked(uint64_t id);

  // ── 選方案 + 套簡繁,一個不可分割的動作 ──────────────────────
  //
  // ⚠ **engine.cc 裡唯一允許出現 rs_select_schema 的地方。**
  //   windows/audit_single_source.sh 在原始碼層面守這一條。
  //
  // 為什麼必須綁在一起:librime 的 ConcreteEngine::InitializeOptions()
  // 在每一次載入方案時都會把 switches 重設回方案宣告的值(有 `reset:`
  // 的那些)。luna_pinyin_tw 的 __patch 把 switches/@2/reset 設成 3,
  // 所以換一次方案,zh_hant_tw 就被設回真、zh_hans 被設回假 ——
  // 使用者剛選的簡體被悄悄洗掉。
  //
  // 這條路徑產品裡真的會走到:懸浮狀態列第三格的方案選單 →
  // SelectSchemaAll → 之後沒有任何人重套簡繁。而使用者回報的「狀態列
  // 說简、打出來是繁」與「那一橫」是同一個畫面上的兩件事。
  //
  // (emulator-5558 上實測過,scripts/verify_variant_persistence.sh 的
  //  情境 B 與 C:裸選一次 → 候選變回「逆號 擬好」;選完立刻重套 →
  //  仍然是「逆号 拟好」。)
  //
  // 回傳「真的換了方案」(schema_id 非空而且 rs_select_schema 成功)。
  // schema_id 為空時不換方案,但**仍然重套一次簡繁** —— 保底不花錢。
  //
  // ⚠ 只能在引擎執行緒上呼叫。sess 是已經解出來的 rs_session。
  bool SelectAndApply(uint64_t id, uintptr_t sess,
                      const std::string& schema_id);

  // 目前的簡繁偏好。SelectAndApply 要用它替換方案之後重算一次,
  // 而 langid 由 session_lang_ 提供。
  //
  // ⚠ 它是**設定的複本**,不是真相的來源 —— 真相在設定檔。存一份是
  //   因為 SelectAndApply 跑在引擎執行緒上,而讀設定檔要碰磁碟。
  //   ApplyVariantAll 與設定存檔時更新它。
  SchemaPreference variant_pref_;

 public:
  // 啟動時把設定檔裡那一份種進來。少了它,服務剛起來到使用者第一次改
  // 設定之間,SelectAndApply 用的是**預設值**而不是他存過的偏好 ——
  // 症狀是「換一次方案,簡繁跳回預設」,而他沒有碰過簡繁。
  void SetVariantPref(const SchemaPreference& pref) {
    Post("記下簡繁偏好", [&] { variant_pref_ = pref; });
  }

  // 取一份複本出來。⚠ 走 Post 而不是直接回傳 variant_pref_ 的參考:
  //   它只屬於引擎執行緒,而呼叫者(pipe_server 的 kSelectSchema)在
  //   連線的執行緒上。Post 是同步的,所以拿到的是當下那一份。
  //
  // 它的用途**只有診斷**:common/schema_choice.h 的
  // PickVariantPrefForSchemaSwitch 拿它來判斷「這一份過期了沒」,
  // 而那個答案只進日誌。判斷本身一律以設定檔為準。
  //
  // ⚠ 合併 win-next 之後 Post() 會回 WorkQueue::Status,而這兩支刻意
  //   不看它:引擎在停(kStopped)時工作根本沒跑,SetVariantPref 等於
  //   沒記、VariantPrefCopy 回的是預設值。兩件都可以接受 ——
  //   前者在 Start() 之後才有人呼叫,後者的答案**只進日誌**
  //   (判斷一律以設定檔為準,見 PickVariantPrefForSchemaSwitch)。
  //   要是哪天有人拿它去做決定,那時就得先把回傳值接起來。
  SchemaPreference VariantPrefCopy() {
    SchemaPreference out;
    Post("讀簡繁偏好", [&] { out = variant_pref_; });
    return out;
  }

 private:

  // 以下三個只在引擎執行緒上呼叫。
  Snapshot TakeSnapshot(uint64_t id);
  Snapshot TakeSnapshotLocked(uintptr_t sess);
  uintptr_t Find(uint64_t id) const;

  // 只由 Start() / Stop() 寫,而那兩支都在呼叫端(main)那一條執行緒上。
  bool started_ = false;

  std::map<uint64_t, uintptr_t> sessions_;
  std::map<uint64_t, uint32_t> session_lang_;
  uint64_t next_id_ = 1;

  // ── #90 ─────────────────────────────────────────────────────
  //
  // 重新部署的階段。判斷全部在 common/redeploy_flow.h(純函式,
  // Ubuntu 上驗得到);這裡只存那一格值。
  std::atomic<RedeployPhase> phase_{RedeployPhase::kIdle};
  // 部署前收掉、部署後要建回來的 session。
  //
  // ⚠ 只收**宿主的**那些。備用池裡那幾個沒有任何人認得,建回來是浪費;
  //   下一個 SESSION_NEW 會自己要一個。
  //
  // ⚠ 這裡存的是我們自己的 id(next_id_ 發的),不是 librime 的 ——
  //   所以 session 可以在宿主完全不知情的情況下被換掉一次。
  struct ParkedSession {
    uint64_t id = 0;
    uint32_t langid = 0;
  };
  std::vector<ParkedSession> parked_;
  SessionPlanner planner_;
  // 這一次「重新整理字詞」連開始都失敗了(rs_deploy() 拒絕啟動)。
  //
  // ⚠ **不可以**拿 deploy_state_ 來表示這件事。那一格回答的是「有沒有
  //   一份能用的詞庫」,而按鍵那道門讀它 —— 把它寫成 -1,使用者就
  //   **永久**打不出中文了,而失敗的其實只是這一次整理。
  std::atomic<bool> redeploy_start_failed_{false};

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
  // ⚠ 「問過了」與「答案不是空的」是兩件事。見 SchemaListFromCache。
  bool schema_cache_valid_ = false;

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
