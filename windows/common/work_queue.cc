#include "work_queue.h"

#include <chrono>

namespace rimewin {

int64_t WorkQueueNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

namespace {

// 一次有界等待的共享狀態。
//
// ⚠ **這個結構是整個檔案的重點。** 它由工作與呼叫端**各持一份
//   shared_ptr**,所以「呼叫端等到不耐煩走掉了」與「工作終於跑起來」
//   誰先誰後都不會踩到對方。舊版把 `done` 開在呼叫端的堆疊上,
//   而那只有在「呼叫端永遠等到底」的前提下才安全。
struct CallState {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
};

}  // namespace

WorkQueue::~WorkQueue() { Stop(); }

bool WorkQueue::started() const {
  std::lock_guard<std::mutex> lock(mu_);
  return started_;
}

// ── ⚠ Start/Stop 之間不可以有中間態 ────────────────────────────
//
// 舊版是這樣的:
//
//     void Start() {
//       { lock(mu_); if (started_) return; started_ = true; ... }  // 鎖裡
//       thread_ = std::thread(&ThreadMain, this);                  // 鎖外
//     }
//
// 中間那一格是「started_ 已經是 true,而 thread_ 還是空的」。一次 Stop()
// 插進來:看到 started_ → 設 stop_ → `thread_.joinable()` 是 false 所以
// **跳過 join** → 把 started_ 設回 false。Start() 接著才把執行緒建出來,
// 而那是一條**沒有人會 join** 的執行緒。下一次 Start() 對一個 joinable 的
// std::thread 做指派 = `std::terminate`:服務當場消失,沒有訊息、沒有記錄。
//
// 修法有兩層:
//   1. **執行緒在 mu_ 裡建**,所以「started_ 是 true」與「thread_ 有東西」
//      是同一個原子步驟。工作者一起跑就會在 mu_ 上等,不會有事。
//   2. life_mu_ 把 Start 與 Stop **整支**互斥。少了它,Stop 走到「放掉
//      mu_、還沒 join」的時候 Start 會看到 started_ 仍是 true 而直接返回
//      —— 使用者要的那次啟動被靜靜地吃掉,佇列停在停止狀態。
//   ⚠ 不能改用 mu_ 來做第 2 層:join 要等工作者跑完,而工作者要拿 mu_。
//
// ⚠ **老實說:第 2 層自己就夠了。** Start 與 Stop 整支互斥之後,那個中間態
//   根本沒有機會被別人看到,所以第 1 層是多的 —— 而多出來的那一層
//   **沒有自己的守門**:把執行緒搬回鎖外、life_mu_ 留著,測試全綠
//   (實跑過)。留著它的理由只有一個:那個不變式因此**寫在這個函式裡**,
//   不必靠另一把鎖的記性。哪天有人覺得 life_mu_ 是多餘的而拿掉它,
//   會被 work_queue_start_during_a_pending_stop_is_not_swallowed 擋下來,
//   而不是被一次線上的 std::terminate 擋下來。
void WorkQueue::Start() {
  std::lock_guard<std::mutex> life(life_mu_);
  std::lock_guard<std::mutex> lock(mu_);
  if (started_) return;
  stop_ = false;
  last_normal_ms_ = WorkQueueNowMs();
  // ⚠ 先建執行緒再設 started_:std::thread 的建構會丟例外(執行緒建不出來),
  //   而丟出去之後這個物件必須還是「沒有啟動」的樣子,不是半啟動的。
  thread_ = std::thread(&WorkQueue::ThreadMain, this);
  started_ = true;
}

void WorkQueue::Stop() {
  std::lock_guard<std::mutex> life(life_mu_);
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!started_) return;
    stop_ = true;
  }
  cv_.notify_all();
  // ⚠ join **不可以**握著 mu_:工作者要拿它才跑得完。
  //   started_ 到這裡都還是 true,而 life_mu_ 擋著 Start ——
  //   所以「正在停」這段期間沒有第二條工作者被建出來的路。
  if (thread_.joinable()) thread_.join();
  std::lock_guard<std::mutex> lock(mu_);
  started_ = false;
}

void WorkQueue::ThreadMain() {
  for (;;) {
    Job job;
    bool is_low = false;
    bool done_all = false;
    {
      std::unique_lock<std::mutex> lock(mu_);
      for (;;) {
        // ⚠ 收到 stop 之後仍然把兩個佇列**排乾**。每一個已經入列的工作
        //   都保證會被執行 —— Call 的呼叫端可能正在等它,而
        //   「停了所以不做了」會讓那個人等到逾時,即使佇列其實是空的。
        if (stop_ && queue_.empty() && low_queue_.empty()) {
          done_all = true;
          break;
        }
        if (!queue_.empty()) break;
        if (!low_queue_.empty()) {
          // 低優先的工作還要「等佇列閒下來一段時間」才做。
          //
          // 只把它排到後面是不夠的:排序只在工作**還沒開始**時有用。
          // 低優先的工作通常很貴,而且一旦開始就停不下來 —— 在別人
          // 還等著的時候動它們,等於偷走那個人的時間。
          const int64_t idle_ms = WorkQueueNowMs() - last_normal_ms_;
          if (stop_ || idle_ms >= low_idle_ms_) break;
          cv_.wait_for(lock, std::chrono::milliseconds(low_idle_ms_ - idle_ms));
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
    }

    const int64_t t_start = WorkQueueNowMs();
    {
      std::lock_guard<std::mutex> lock(state_mu_);
      running_label_ = job.label;
      running_since_ms_ = t_start;
    }
    job.fn();
    const int64_t t_end = WorkQueueNowMs();
    {
      std::lock_guard<std::mutex> lock(state_mu_);
      running_label_ = nullptr;
      running_since_ms_ = 0;
    }

    // ⚠ 「等了多久」與「跑了多久」一定要分開報。
    //   等很久 = **別人擋在前面**(去看上一行是誰);
    //   跑很久 = 這件事本身就慢(去看它做了什麼)。
    //   併成一個數字的話,兩種完全不同的問題長得一模一樣。
    //
    //   低優先的工作不報「等待」:它是**刻意**被押後的,那個數字沒有意義。
    if (slow_)
      slow_(job.label, is_low ? 0 : t_start - job.enqueued_ms, t_end - t_start);

    if (!is_low) {
      // 「最後一次忙於**有人在等的**工作」是什麼時候。低優先的工作靠它
      // 決定要不要再等一下。低優先的自己不算忙 —— 不然一串連續的收尾
      // 會讓自己一直看起來很忙,永遠等不到安靜期。
      std::lock_guard<std::mutex> lock(mu_);
      last_normal_ms_ = t_end;
    }
    cv_.notify_all();
  }
  if (on_exit_) on_exit_();
}

bool WorkQueue::Post(const char* label, std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    // 沒有工作者可以跑它 —— 說出來,不要靜靜地把工作丟掉(理由見標頭)。
    if (!started_ || stop_) return false;
    Job j;
    j.label = label;
    j.enqueued_ms = WorkQueueNowMs();
    // ⚠ 傳值移入。工作從這一刻起自成一體 —— 呼叫端的框可以立刻消失。
    j.fn = std::move(fn);
    queue_.push_back(std::move(j));
  }
  cv_.notify_all();
  return true;
}

bool WorkQueue::PostLow(const char* label, std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    // 沒有工作者可以跑它 —— 說出來,不要靜靜地把工作丟掉(理由見標頭)。
    if (!started_ || stop_) return false;
    Job j;
    j.label = label;
    j.enqueued_ms = WorkQueueNowMs();
    j.fn = std::move(fn);
    low_queue_.push_back(std::move(j));
  }
  cv_.notify_all();
  return true;
}

WorkQueue::Status WorkQueue::Call(const char* label, std::function<void()> fn,
                                  int timeout_ms) {
  auto st = std::make_shared<CallState>();
  {
    std::lock_guard<std::mutex> lock(mu_);
    // 沒有工作者可以跑它 —— 說出來,不要靜靜地假裝做完了。
    // (舊版在這裡直接 return,而呼叫端讀到的是一個沒有被填過的結果。)
    if (!started_ || stop_) return Status::kStopped;
    Job j;
    j.label = label;
    j.enqueued_ms = WorkQueueNowMs();
    // ⚠ st 與 fn 都**傳值**捕捉。這一行就是這個類別存在的理由:
    //   工作寫的 `done` 屬於那個共享狀態,不屬於任何一個堆疊框。
    j.fn = [st, fn] {
      fn();
      {
        std::lock_guard<std::mutex> l(st->mu);
        st->done = true;
      }
      st->cv.notify_all();
    };
    queue_.push_back(std::move(j));
  }
  cv_.notify_all();

  std::unique_lock<std::mutex> lock(st->mu);
  if (timeout_ms <= 0) {
    st->cv.wait(lock, [&] { return st->done; });
    return Status::kDone;
  }
  const bool ok = st->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                  [&] { return st->done; });
  return ok ? Status::kDone : Status::kTimeout;
}

int64_t WorkQueue::StalledMs() const {
  std::lock_guard<std::mutex> lock(state_mu_);
  if (!running_label_) return 0;
  const int64_t d = WorkQueueNowMs() - running_since_ms_;
  return d > 0 ? d : 0;
}

int64_t WorkQueue::OldestWaitingMs() const {
  std::lock_guard<std::mutex> lock(mu_);
  // queue_ 是 push_back / pop_front,所以 front() 就是最舊的那一件。
  // ⚠ low_queue_ 刻意不看 —— 見標頭。
  if (queue_.empty()) return 0;
  const int64_t d = WorkQueueNowMs() - queue_.front().enqueued_ms;
  return d > 0 ? d : 0;
}

std::string WorkQueue::CurrentLabel() const {
  std::lock_guard<std::mutex> lock(state_mu_);
  return running_label_ ? std::string(running_label_) : std::string();
}

}  // namespace rimewin
