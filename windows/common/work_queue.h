// windows/common/work_queue.h — 一條工作者執行緒 + 一個工作佇列
//
// ── 為什麼把它從 service/engine.cc 抽出來 ────────────────────────
//
// engine.cc 裡原本就有這一套(佇列、低優先佇列、慢工作記錄、
// 「丟工作並等它做完」)。抽出來的理由**不是整理**,是它有一格
// 真的很難寫對,而那一格在 service/ 裡沒有任何自動化碰得到:
//
//   **有逾時的等待,一旦逾時,遲到的工作會寫進誰的記憶體?**
//
// 舊版的 `Engine::Post` 是這樣的:
//
//     void Engine::Post(const char* label, std::function<void()> fn) {
//       bool done = false;                       // ← 呼叫端的堆疊
//       ... j.fn = [&fn, &done, this] { fn(); ... done = true; };
//       cv_.wait(lock, [&] { return done; });    // ← 沒有逾時
//     }
//
// 它今天是安全的,而且**只因為那個 wait 永遠不會提早返回**:呼叫端
// 保證等到工作跑完才離開,所以 `&done`(以及 SchemaList / SetOption
// 那些包裝寫進去的 `out`、`ok`)指到的框一定還活著。
//
// 也就是說:「沒有逾時」既是那個缺陷(#79:UI 執行緒被引擎拖死),
// **也是**目前唯一擋住 use-after-free 的東西。加上逾時的那一刻,
// 第二個缺陷就會被引爆 —— 而它是靜默的、時序相關的、只在引擎剛好
// 很慢的時候才發生的那一種。
//
// 所以這一支的共享狀態一律是 `std::shared_ptr`:工作與呼叫端**各持
// 一份**,誰先走都不影響另一邊。而它住在 common/ 是為了能在 Ubuntu 上
// 用 windows/run_logic_tests.sh 單獨測 —— 包含 `--asan`,那才是真的
// 會抓到「又有人回頭捕捉呼叫端堆疊」的東西。
//
// ⚠ 本檔**不含 windows.h**,而且不可以含。它進 rime_common,
//   而 rime_common 同時被 Windows 上編不起來的測試用到。
//
#ifndef RIMEWIN_COMMON_WORK_QUEUE_H_
#define RIMEWIN_COMMON_WORK_QUEUE_H_

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace rimewin {

class WorkQueue {
 public:
  enum class Status {
    kDone,     // 工作跑完了
    kTimeout,  // 等到逾時,工作**可能還在跑**(見下面的警告)
    kStopped,  // 佇列沒有啟動或已經停了,工作**沒有**入列
  };

  // 一件工作慢到值得記一行時被呼叫(label / 等了多久 / 跑了多久)。
  // ⚠ 在**工作者執行緒**上呼叫。
  using SlowReporter =
      std::function<void(const char* label, int64_t waited_ms, int64_t ran_ms)>;

  WorkQueue() = default;
  ~WorkQueue();
  WorkQueue(const WorkQueue&) = delete;
  WorkQueue& operator=(const WorkQueue&) = delete;

  // 下面三支要在 Start() 之前設定。
  void SetSlowReporter(SlowReporter r) { slow_ = std::move(r); }
  // 佇列排乾、工作者要離開之前,在**工作者執行緒上**跑一次。
  // 引擎用它銷毀 session —— 那件事只能在建立它的那條執行緒上做。
  void SetOnWorkerExit(std::function<void()> fn) { on_exit_ = std::move(fn); }
  // 低優先的工作要等佇列閒下來多久才做。見 PostLow。
  void SetLowPriorityIdleMs(int64_t ms) { low_idle_ms_ = ms; }

  void Start();
  // 停止並 join。**已經入列的工作保證都會被執行**(見 ThreadMain)。
  void Stop();
  bool started() const;

  // ── 丟了就走 ──────────────────────────────────────────────────
  //
  // ⚠ `fn` 捕捉的每一樣東西都必須是**它自己擁有的**(傳值)。
  //   這一支返回時工作通常還沒開始跑,呼叫端的框隨時會消失。
  //   `[&]` 在這裡是一個 use-after-free,而且它不會當場崩 ——
  //   它會在幾百毫秒之後,拿一段已經被別人用掉的記憶體當設定值。
  void Post(const char* label, std::function<void()> fn);

  // 「有空再做」:優先權比 Post 低,而且要等佇列**真的閒下來**
  // (SetLowPriorityIdleMs)才會被撿走。給那些沒有人在等、但是很貴的
  // 收尾工作用 —— 在別人還等著的時候動它們,等於偷走那個人的時間。
  void PostLow(const char* label, std::function<void()> fn);

  // ── 丟了並等它做完,**有上限** ──────────────────────────────
  //
  // timeout_ms <= 0 = 永遠等(舊行為)。
  //
  // ⚠ 回 kTimeout 的時候,那件工作**沒有被取消** —— 它還在佇列裡,
  //   或正在跑。這一支能保證的只有一件事:**這個呼叫端不再等它**,
  //   而且工作者之後碰的每一樣東西都不屬於這個呼叫端。
  //   所以 `fn` 一樣必須自己擁有它讀寫的東西,理由與 Post 相同。
  //
  // ⚠ 逾時之後要拿工作的結果,用 CallFor —— 不要自己開一個變數讓
  //   工作寫進去。那正是這個檔案存在的理由。
  Status Call(const char* label, std::function<void()> fn, int timeout_ms);

  // 有回傳值的有界呼叫。結果放在一個與工作**共同持有**的盒子裡:
  // 逾時之後遲到的工作寫進盒子(還活著),而呼叫端的 `*out` 一個位元組
  // 都不會被動到。
  //
  //   std::vector<Pair> list;
  //   if (q.CallFor<std::vector<Pair>>("列方案", &list, [this] {...}, 1500)
  //       != WorkQueue::Status::kDone) { /* 說實話,不要假裝拿到了 */ }
  template <typename T>
  Status CallFor(const char* label, T* out, std::function<T()> fn,
                 int timeout_ms) {
    // ⚠ box 與 fn 都是**傳值**捕捉進工作的。工作因此自成一體:
    //   呼叫端在它跑完之前整個消失也不影響它。
    auto box = std::make_shared<T>();
    const Status s = Call(label, [box, fn] { *box = fn(); }, timeout_ms);
    if (s == Status::kDone && out) *out = std::move(*box);
    return s;
  }

  // ── 工作者的活體狀態 ────────────────────────────────────────
  //
  // 給介面用:「這個視窗還在動,是引擎沒有回應」與「這個程式當掉了」
  // 在畫面上長得一模一樣,而使用者的下一步完全不同。
  //
  // 正在跑的那件工作已經跑了幾毫秒。沒有工作在跑 = 0。
  //
  // ⚠ **這一支回答的不是「使用者等了多久」。** 它只看正在跑的那一件。
  //   工作者一路啃過一長串各自都不算慢的工作時,它從頭到尾都很小,
  //   而排在最後面的那個人已經等了十秒。那條路要問 OldestWaitingMs()。
  int64_t StalledMs() const;

  // 佇列裡**最舊那件**已經躺了幾毫秒。佇列空 = 0。
  //
  // 這一支才是「我按下去的東西為什麼沒有動靜」的答案。工作者只有一條,
  // 所以那個答案幾乎一定是「別人擋在前面」——(見本檔開頭與 engine.h)。
  // 而擋在前面的那些人各自跑多久,對等待的人來說沒有差別:
  // 他量到的只有自己躺了多久。
  //
  // ⚠ **低優先佇列不算。** 那些工作是刻意被押後的(SetLowPriorityIdleMs),
  //   沒有人在等它們。算進去的話,每一次收尾工作排隊都會讓介面謊報
  //   「引擎沒有回應」,而一句常駐的假警告等於沒有警告。
  //   (與 SlowReporter 對低優先工作不報「等待」是同一條理由。)
  //
  // ⚠ 這一支拿的是 `mu_`(佇列鎖),不是 StalledMs() 的 `state_mu_`。
  //   可以這樣做的**唯一**理由是:ThreadMain 從來不在持有 mu_ 的情況下
  //   跑工作本體(`job.fn()` 在那個鎖的作用域之外,cv_.wait 也會放掉鎖),
  //   所以 mu_ 每一次都只被握住 O(1) 的時間。哪一天有人把工作搬進鎖裡,
  //   這一行就會變成「介面被引擎拖住」—— 也就是 #79 本人。
  int64_t OldestWaitingMs() const;

  // 正在跑的那件工作叫什麼。沒有工作在跑 = 空字串。
  std::string CurrentLabel() const;

 private:
  struct Job {
    std::function<void()> fn;
    const char* label = "(沒有標籤)";  // 一律是字面值,不必管生命週期
    int64_t enqueued_ms = 0;
  };

  void ThreadMain();

  std::thread thread_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> queue_;
  std::deque<Job> low_queue_;
  int64_t last_normal_ms_ = 0;
  int64_t low_idle_ms_ = 1500;
  bool stop_ = false;
  bool started_ = false;

  // 正在跑的那一件。與 mu_ 分開:讀它的是介面執行緒,而它**不該**為了
  // 問一個顯示用的數字去搶佇列的鎖。
  mutable std::mutex state_mu_;
  const char* running_label_ = nullptr;
  int64_t running_since_ms_ = 0;

  SlowReporter slow_;
  std::function<void()> on_exit_;
};

// steady clock 的毫秒。放在標頭是因為測試也要量時間,而且要與佇列
// 用同一支 —— 兩支不同的時鐘量出來的差值沒有意義。
int64_t WorkQueueNowMs();

}  // namespace rimewin

#endif  // RIMEWIN_COMMON_WORK_QUEUE_H_
