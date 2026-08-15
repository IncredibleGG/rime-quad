// windows/tests/test_work_queue.cc — 有界等待,以及「遲到的工作寫進誰的記憶體」
//
// ⚠ 這五條每一條在 common/work_queue.{h,cc} 出現**之前**都是紅的,
//   而且是被建構保證的紅:第 1 條要的那個逾時參數在舊的
//   `Engine::Post(const char*, std::function<void()>)` 上根本不存在,
//   所以那一版連編都不會過。
//
// ⚠ 這裡刻意**不**去測「引擎會不會卡住」——引擎在 Ubuntu 上編不起來。
//   測的是那個卡住之所以會讓 UI 整個死掉的那一格機制,而它現在是
//   一個自成一體、與 windows.h 無關的類別。
//
#include "../common/work_queue.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "check.h"

using namespace rimewin;

namespace {

// 一個「永遠不開,除非我們去開」的閘。用它把工作者釘在一件工作裡,
// 就能重現「引擎正在做一件很久的事」而不必真的有一個引擎。
class Gate {
 public:
  void Wait() {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [&] { return open_; });
  }
  void Open() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      open_ = true;
    }
    cv_.notify_all();
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  bool open_ = false;
};

// 把工作者卡在一件工作裡,並且**確定它真的進去了**才返回。
// 不等這一下的話,測試會與「工作者還沒撿到那件工作」賽跑,
// 而輸的那一次會是隨機的紅 —— 這個專案不接受間歇性的測試。
void BlockWorker(WorkQueue* q, Gate* gate, std::atomic<bool>* entered) {
  q->Post("測試:卡住工作者", [gate, entered] {
    entered->store(true);
    gate->Wait();
  });
  for (int i = 0; i < 2000 && !entered->load(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(entered->load());
}

}  // namespace

// ── 1. 有界等待:逾時就回來,不是永遠等下去 ─────────────────────
//
// 這是 #79 的那一格。舊版的 `Engine::Post` 只有
// `cv_.wait(lock, [&]{return done;})` —— 沒有逾時、沒有 stop 條件,
// 而設定視窗的 UI 執行緒就掛在它上面。引擎那一頭一慢,
// `GetMessageW` 就再也不會被呼叫,整個視窗(連同系統匣圖示)停住。
TEST(work_queue_call_times_out_instead_of_waiting_forever) {
  WorkQueue q;
  q.Start();
  Gate gate;
  std::atomic<bool> entered{false};
  BlockWorker(&q, &gate, &entered);

  const int64_t t0 = WorkQueueNowMs();
  const WorkQueue::Status s =
      q.Call("測試:排在後面的那一件", [] {}, 300);
  const int64_t waited = WorkQueueNowMs() - t0;

  CHECK(s == WorkQueue::Status::kTimeout);
  // 下界:真的等了(不是一進去就放棄)。
  CHECK(waited >= 250);
  // 上界:真的**回來了**。900 毫秒是給忙碌的 CI 機留的餘裕,
  // 而重點不是這個數字準不準 —— 是它有沒有上界。
  CHECK(waited < 900);

  gate.Open();
  q.Stop();
}

// ── 2. 逾時之後,遲到的工作不可以碰呼叫端已經放棄的那份結果 ──────
//
// 舊版的 job 捕捉的是 `[&fn, &done, this]`,而 SchemaList / SetOption
// 這些包裝把結果寫進**呼叫端的堆疊**(`out`、`ok`)。今天安全只因為
// 呼叫端一定等到底;逾時一回傳,遲到的 job 就會寫進一個死掉的框。
//
// ⚠ 這一條在 `--asan` 下才有全部的價值:任何一次退步回「工作直接寫
//   呼叫端的變數」,ASan 會把它報成 heap/stack-use-after-free 而不是
//   一個安靜的錯值。CI 上兩種都跑。
TEST(work_queue_late_job_does_not_touch_the_abandoned_frame) {
  WorkQueue q;
  q.Start();
  Gate gate;
  std::atomic<bool> entered{false};
  BlockWorker(&q, &gate, &entered);

  std::string result = "呼叫端還沒拿到東西";
  std::atomic<bool> job_ran{false};
  const WorkQueue::Status s = q.CallFor<std::string>(
      "測試:遲到的工作", &result,
      [&job_ran] {
        job_ran.store(true);
        return std::string("引擎終於回來了");
      },
      200);
  CHECK(s == WorkQueue::Status::kTimeout);
  // 逾時的當下,呼叫端那一份**沒有**被動過。
  CHECK(result == "呼叫端還沒拿到東西");
  CHECK(!job_ran.load());

  // 現在放行,讓那件遲到的工作真的跑完。
  gate.Open();
  for (int i = 0; i < 2000 && !job_ran.load(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(job_ran.load());
  // 工作跑完之後,呼叫端那一份**仍然**一個位元組都沒被改。
  // 它寫進去的是與工作共同持有的那個盒子。
  CHECK(result == "呼叫端還沒拿到東西");

  q.Stop();
}

// ── 3. Post 真的不 rendezvous ───────────────────────────────────
//
// 「改成非同步」很容易做成「還是同步,只是名字改了」。這一條量時間:
// 工作者被釘死的時候,Post 仍然要立刻回來。
TEST(work_queue_post_returns_without_running) {
  WorkQueue q;
  q.Start();
  Gate gate;
  std::atomic<bool> entered{false};
  BlockWorker(&q, &gate, &entered);

  std::atomic<int> ran{0};
  const int64_t t0 = WorkQueueNowMs();
  for (int i = 0; i < 5; ++i)
    q.Post("測試:丟了就走", [&ran] { ran.fetch_add(1); });
  const int64_t cost = WorkQueueNowMs() - t0;

  CHECK(cost < 50);
  // 而且它們**還沒跑**——閘還關著。
  CHECK_INT(ran.load(), 0);

  gate.Open();
  q.Stop();  // Stop 會把佇列排乾
  CHECK_INT(ran.load(), 5);
}

// ── 4. 非同步的工作必須自己擁有它的參數 ────────────────────────
//
// 這一條專門守 `Engine::ApplyVariantAll` 的那個 `[&]`:改成非同步之後,
// 那個 lambda 捕捉的 `pref` 屬於一個已經離開的堆疊框。
// 症狀不是崩潰,是「使用者選了簡體,套下去的是一段亂掉的偏好」。
TEST(work_queue_async_job_owns_its_arguments) {
  WorkQueue q;
  q.Start();
  Gate gate;
  std::atomic<bool> entered{false};
  BlockWorker(&q, &gate, &entered);

  std::atomic<bool> ok{false};
  std::atomic<bool> ran{false};
  {
    // 這個字串在工作跑到之前就會死掉。
    std::string pref = "臺灣正體;簡繁=繁";
    q.Post("測試:自己擁有參數", [pref, &ok, &ran] {
      ok.store(pref == "臺灣正體;簡繁=繁");
      ran.store(true);
    });
  }

  gate.Open();
  for (int i = 0; i < 2000 && !ran.load(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(ran.load());
  CHECK(ok.load());
  q.Stop();
}

// ── 5. 卡住的工作者要說得出「我卡在哪一件」 ─────────────────────
//
// 側欄那行狀態讀的就是這兩個。沒有它們的話,使用者面對的是一個
// 什麼都不說的視窗,而「引擎在忙」與「這個程式當掉了」在畫面上
// 長得一模一樣 —— 而那兩件事的下一步完全不同。
TEST(work_queue_stalled_worker_is_reported) {
  WorkQueue q;
  q.Start();
  // 閒著的時候不可以謊報有東西在跑。
  CHECK_INT(static_cast<int>(q.StalledMs()), 0);
  CHECK(q.CurrentLabel().empty());

  Gate gate;
  std::atomic<bool> entered{false};
  q.Post("測試:一件很久的工作", [&gate, &entered] {
    entered.store(true);
    gate.Wait();
  });
  for (int i = 0; i < 2000 && !entered.load(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(entered.load());

  CHECK(q.CurrentLabel() == "測試:一件很久的工作");
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  const int64_t stalled = q.StalledMs();
  CHECK(stalled >= 100);
  // 上界只是為了證明它量的是**這一件**已經跑了多久,不是進程活了多久。
  CHECK(stalled < 5000);

  gate.Open();
  q.Stop();
  CHECK(q.CurrentLabel().empty());
  CHECK_INT(static_cast<int>(q.StalledMs()), 0);
}

// ── 6. 慢工作的記錄仍然把「等」與「跑」分開 ──────────────────
//
// 這一格是從 engine.cc 搬過來的,而搬家最容易掉的就是它 ——
// 掉了不會紅,只會讓下一次「為什麼慢」失去唯一的線索。
TEST(work_queue_slow_report_separates_waiting_from_running) {
  WorkQueue q;
  std::vector<std::string> labels;
  std::vector<int64_t> waited, ran;
  std::mutex log_mu;
  q.SetSlowReporter([&](const char* label, int64_t w, int64_t r) {
    std::lock_guard<std::mutex> lock(log_mu);
    labels.push_back(label);
    waited.push_back(w);
    ran.push_back(r);
  });
  q.Start();

  Gate gate;
  std::atomic<bool> entered{false};
  q.Post("測試:擋在前面的那一件", [&gate, &entered] {
    entered.store(true);
    gate.Wait();
  });
  for (int i = 0; i < 2000 && !entered.load(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(entered.load());

  q.Post("測試:排在後面的那一件", [] {});
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  gate.Open();
  q.Stop();

  std::lock_guard<std::mutex> lock(log_mu);
  CHECK_INT(static_cast<int>(labels.size()), 2);
  CHECK(labels[0] == "測試:擋在前面的那一件");
  CHECK(labels[1] == "測試:排在後面的那一件");
  // 第一件:自己跑很久,沒有等。
  CHECK(ran[0] >= 100);
  // 第二件:自己幾乎不花時間,**等**了很久。
  //   這兩個數字要修的地方完全不同,所以它們不可以被併成一個。
  CHECK(waited[1] >= 100);
  CHECK(ran[1] < 100);
}

// ── 7. 沒有工作者的時候要說「沒有」,不要假裝做完了 ────────────
//
// 舊版的 Post 在 stop_ 之後**直接 return**,而呼叫端讀到的是一個
// 從來沒有被填過的結果 —— 一份空的方案清單看起來就像「一個方案都
// 沒有」,而那句話是假的。
TEST(work_queue_call_says_stopped_when_there_is_no_worker) {
  WorkQueue q;  // 沒有 Start()
  std::atomic<int> ran{0};
  const WorkQueue::Status s = q.Call("測試:沒有工作者", [&] { ran.fetch_add(1); }, 100);
  CHECK(s == WorkQueue::Status::kStopped);
  CHECK_INT(ran.load(), 0);

  q.Start();
  CHECK(q.Call("測試:有工作者", [&] { ran.fetch_add(1); }, 1000) ==
        WorkQueue::Status::kDone);
  CHECK_INT(ran.load(), 1);
  q.Stop();
  CHECK(q.Call("測試:停了之後", [&] { ran.fetch_add(1); }, 100) ==
        WorkQueue::Status::kStopped);
  CHECK_INT(ran.load(), 1);
}

// ── 8. 停止之前入列的工作一定會被跑完 ──────────────────────────
//
// 引擎靠這一條:session 的銷毀(把使用者詞典寫回去)是用 PostLow 排的,
// 而服務結束時那些工作還在佇列裡。排不乾就是「關掉輸入法會掉字」。
TEST(work_queue_drains_everything_before_it_stops) {
  WorkQueue q;
  std::atomic<int> normal{0}, low{0}, exited{0};
  q.SetOnWorkerExit([&] { exited.fetch_add(1); });
  q.SetLowPriorityIdleMs(1000000);  // 正常情況下低優先的永遠等不到
  q.Start();
  for (int i = 0; i < 20; ++i) q.Post("測試:一般", [&] { normal.fetch_add(1); });
  for (int i = 0; i < 20; ++i) q.PostLow("測試:低優先", [&] { low.fetch_add(1); });
  q.Stop();
  CHECK_INT(normal.load(), 20);
  CHECK_INT(low.load(), 20);
  // 收尾只跑一次,而且是在**排乾之後**。
  CHECK_INT(exited.load(), 1);
}

// ── 9. 「引擎沒有回應」要量佇列,不是量正在跑的那一件 ────────────
//
// StalledMs() 只回報**正在跑的那一件**已經跑了多久。而 work_queue.h /
// engine.h 自己寫著:引擎只有一條執行緒,所以「我按下去的東西為什麼
// 沒有動靜」的答案幾乎一定是「**別人**擋在前面」。
//
// 20 件各 500 毫秒排在使用者前面 = 他等 10 秒,而 StalledMs() 從頭到尾
// 沒有超過 500 —— 設定視窗那個 2000 毫秒的心跳門檻**一次都不會亮**,
// 使用者面對的是一個什麼都不說的視窗。
//
// 要量的是**最舊那件在佇列裡躺了多久**。
TEST(work_queue_oldest_waiting_measures_the_queue_not_the_running_job) {
  WorkQueue q;
  q.Start();
  // 閒著的時候不可以謊報有人在等。
  CHECK_INT(static_cast<int>(q.OldestWaitingMs()), 0);

  Gate gate1, gate2;
  std::atomic<bool> in1{false}, in2{false};
  q.Post("測試:擋在最前面的那一件", [&gate1, &in1] {
    in1.store(true);
    gate1.Wait();
  });
  for (int i = 0; i < 2000 && !in1.load(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(in1.load());

  // 第二件與第三件現在都在佇列裡躺著,沒有人碰得到它們。
  q.Post("測試:第二件", [&gate2, &in2] {
    in2.store(true);
    gate2.Wait();
  });
  q.Post("測試:第三件", [] {});
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(q.OldestWaitingMs() >= 250);

  // ── 這一段才是把 StalledMs() 的語意排除掉的那一格 ────────────────
  //
  //   放行第一件:第二件立刻開始跑,於是「正在跑的那一件」的計時
  //   **歸零**。舊的心跳在這一刻會說「引擎沒事」。
  //   而第三件已經躺了 300 毫秒以上,而且還要繼續等第二件跑完 ——
  //   使用者按下的那個東西到現在一點動靜都沒有。
  gate1.Open();
  for (int i = 0; i < 2000 && !in2.load(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(in2.load());
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  CHECK(q.StalledMs() < 250);        // 正在跑的那一件剛開始跑
  CHECK(q.OldestWaitingMs() >= 300);  // 而佇列尾巴已經等了這麼久

  gate2.Open();
  q.Stop();
  // 排乾之後沒有人在等。
  CHECK_INT(static_cast<int>(q.OldestWaitingMs()), 0);
}

// ── 10. 低優先的工作不算「在等」 ────────────────────────────────
//
// 低優先的工作是**刻意**被押後的(SetLowPriorityIdleMs),沒有人在等它。
// 把它算進「最舊那件等了多久」的話,心跳會在每一次收尾工作排隊時
// 謊報引擎卡住 —— 而一句常駐的假警告等於沒有警告。
// 這與 SlowReporter 對低優先工作不報「等待」是同一條理由。
TEST(work_queue_oldest_waiting_ignores_low_priority) {
  WorkQueue q;
  q.SetLowPriorityIdleMs(1000000);  // 正常情況下低優先的永遠等不到
  q.Start();
  Gate gate;
  std::atomic<bool> entered{false};
  BlockWorker(&q, &gate, &entered);

  q.PostLow("測試:沒有人在等的收尾", [] {});
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  CHECK_INT(static_cast<int>(q.OldestWaitingMs()), 0);

  gate.Open();
  q.Stop();
}

// ── 11. 丟了就走的那一支也要說得出「沒有人會做這件事」 ────────────
//
// 第 7 條已經替 Call 守住這件事(「沒有工作者要說出來」)。Post 沒有 ——
// 它在 `!started_ || stop_` 時直接 return void,呼叫端拿不到任何線索。
//
// 這在介面上有一個很具體的後果:設定視窗現在會在按下去的當下說
// 「正在套用…」,然後**等工作回來**才把那句話換成「已套用」或
// 「套用失敗」。工作根本沒有入列的話,那個回來永遠不會發生,
// 而使用者面對的是一句永遠停在「正在套用…」的話 ——
// 比舊版那句假的「已套用」好不了多少。
TEST(work_queue_post_says_whether_the_job_was_actually_queued) {
  WorkQueue q;  // 沒有 Start()
  std::atomic<int> ran{0};
  CHECK(!q.Post("測試:沒有工作者", [&] { ran.fetch_add(1); }));
  CHECK(!q.PostLow("測試:沒有工作者(低優先)", [&] { ran.fetch_add(1); }));
  CHECK_INT(ran.load(), 0);

  q.Start();
  CHECK(q.Post("測試:有工作者", [&] { ran.fetch_add(1); }));
  CHECK(q.PostLow("測試:有工作者(低優先)", [&] { ran.fetch_add(1); }));
  q.Stop();  // Stop 會把佇列排乾
  CHECK_INT(ran.load(), 2);

  // 停了之後也要說「沒有」,不要靜靜地把工作丟掉。
  CHECK(!q.Post("測試:停了之後", [&] { ran.fetch_add(1); }));
  CHECK_INT(ran.load(), 2);
}

// ── 12. Start() 與 Stop() 交錯:不可以有中間態 ───────────────────
//
// 出事的形狀(這一輪之前的碼):
//
//     void WorkQueue::Start() {
//       { lock(mu_); if (started_) return; started_ = true; ... }   // ← 鎖裡
//       thread_ = std::thread(&WorkQueue::ThreadMain, this);        // ← 鎖外
//     }
//
// 中間那一格是「`started_` 已經是 true,而 `thread_` 還是空的」。
// 一次 Stop() 插進來就會:看到 started_ → 設 stop_ → `thread_.joinable()`
// 是 **false** 所以**跳過 join** → 把 started_ 設回 false。
// Start() 接著才把執行緒建出來 —— 一條**沒有人會 join** 的執行緒。
//
// 代價不是漏掉一次停止。下一次 Start() 會對一個 joinable 的 std::thread
// 做指派,而那是 `std::terminate`:整個服務當場消失,沒有訊息、沒有記錄。
//
// ⚠ 今天 Start/Stop 都在 main 上,所以它構不成 —— 但那是**呼叫端**的
//   性質,不是這個類別的。這一支是給別人拿去用的執行緒工具,
//   而「只要沒有人從兩條執行緒碰它就沒事」不是一個執行緒工具該有的合約。
//
// ⚠ 這一條刻意用**壓**的(200 輪 × 50 次交錯)而不是插樁:那個窗口是
//   兩行程式碼寬,插樁進去就等於把要驗的東西自己寫一遍。
//   壓不出來的那一次是漏網,不是綠燈 —— 所以下面還接了一段
//   「壓完之後這個佇列必須還是好的」,它是確定性的。
TEST(work_queue_start_and_stop_interleaved_never_terminates) {
  for (int round = 0; round < 200; ++round) {
    WorkQueue q;
    std::thread a([&q] {
      for (int i = 0; i < 50; ++i) q.Start();
    });
    std::thread b([&q] {
      for (int i = 0; i < 50; ++i) q.Stop();
    });
    a.join();
    b.join();
    q.Stop();
    // 壓完之後不可以還有人自稱啟動著。
    CHECK(!q.started());

    // ── 確定性的那一半 ────────────────────────────────────────
    // 壓過之後這個佇列必須還是**完好的**:再 Start 一次要拿得到一個
    // 真的在跑的工作者,而不是一條沒人 join 的殘骸(對 joinable 的
    // std::thread 指派 = std::terminate),也不是一個永遠不收工作的空殼。
    std::atomic<int> ran{0};
    q.Start();
    CHECK(q.started());
    CHECK(q.Post("測試:壓完之後還收得下工作", [&ran] { ran.fetch_add(1); }));
    q.Stop();  // Stop 保證把佇列排乾
    CHECK_INT(ran.load(), 1);
    CHECK(!q.started());
  }
}

// ── 13. 一次 Start/Stop 循環只會有一條工作者 ─────────────────────
//
// 上面那一條驗的是「不會炸」,這一條驗的是「不會多」。
// 工作者離開時會呼叫 SetOnWorkerExit,所以離開的次數數得出來 ——
// 而它必須剛好等於**成功啟動過幾次**。多了 = 有人被重複建出來,
// 少了 = 有人沒有離開(那條執行緒還握著 session,而服務以為它結束了)。
//
// ⚠ 重複的 Start() 必須是 no-op(第二次不可以再建一條)。
TEST(work_queue_start_is_idempotent_and_every_worker_exits) {
  std::atomic<int> exits{0};
  {
    WorkQueue q;
    q.SetOnWorkerExit([&exits] { exits.fetch_add(1); });
    for (int i = 0; i < 20; ++i) {
      q.Start();
      q.Start();  // 第二次是 no-op
      q.Start();
      CHECK(q.started());
      q.Stop();
      q.Stop();  // 第二次是 no-op
      CHECK(!q.started());
    }
  }
  CHECK_INT(exits.load(), 20);
}

// ── 14. 正在停的時候有人要啟動,那次啟動不可以被靜靜吃掉 ──────────
//
// 第 12 條壓的是「會不會炸」。這一條驗的是同一個中間態的**另一面**,
// 而它不會炸、只會安靜地錯:
//
//   Stop() 放掉 mu_ 之後要 join,而 join 要等工作者跑完手上那件事
//   (可能是好幾秒的部署)。那段期間 `started_` 仍然是 true,所以一次
//   Start() 會**看到 true 而直接返回** —— 呼叫端以為啟動了。等 Stop()
//   join 完,它把 started_ 設回 false:佇列停在停止狀態,而沒有人知道。
//   之後每一件 Post 都回 false,設定視窗那一頭是一句永遠停在
//   「正在套用…」的話。
//
// 修法是 life_mu_:Start 與 Stop **整支**互斥,所以這次 Start 會等 Stop
// 做完,然後真的啟動一條工作者。
//
// ⚠ 這一條是確定性的,不是壓出來的:工作者被閘門釘死,所以 Stop() 一定
//   卡在 join 裡,而主執行緒那次 Start() 一定落在那個窗口內。
TEST(work_queue_start_during_a_pending_stop_is_not_swallowed) {
  WorkQueue q;
  q.Start();
  Gate gate;
  std::atomic<bool> entered{false};
  BlockWorker(&q, &gate, &entered);

  std::atomic<bool> stopping{false};
  std::thread stopper([&] {
    stopping.store(true);
    q.Stop();  // 卡在 join 裡,直到閘門打開
  });
  // 開閘的人是第三條執行緒 —— 主執行緒等一下要進 Start(),不能由它來開。
  std::thread opener([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    gate.Open();
  });

  for (int i = 0; i < 2000 && !stopping.load(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(stopping.load());
  // 這 50 毫秒之後 Stop() 一定已經在 join 裡(閘門還要 250 毫秒才開)。
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  q.Start();  // ← 要的就是這一次:它不可以被那個中間態吃掉

  stopper.join();
  opener.join();

  // 啟動過了就得真的啟動著,而且收得下工作。
  CHECK(q.started());
  std::atomic<int> ran{0};
  CHECK(q.Post("測試:停止之後重新啟動", [&ran] { ran.fetch_add(1); }));
  q.Stop();
  CHECK_INT(ran.load(), 1);
}


// ── 15. 逾時之後,遲到的工作不可以再執行本體 ─────────────────────
//
// ★ 這一條守的是「加上限」這件事本身的代價。
//
//   service/engine.cc 的按鍵改成有上限的等待之後,逾時的那一顆鍵會被
//   回報成 handled=false —— 而 tsf/text_service.cc 收到 handled=false
//   會**自己把那個字元補進宿主的文件**(SelfInsertChar)。
//   如果那件遲到的工作稍後又把同一顆鍵送進 librime,結果是
//   **引擎組了字、宿主也打了字,兩邊分岔**。
//
//   engine.cc 在 Ubuntu 上編不起來(要 MSVC),所以這道閘只能住在
//   common/ 才守得住 —— 與 bar_visibility.h:72-76 同一句話。
//
// ⚠ 不是「逾時就取消」:工作沒有被取消,它照樣會被撿走、照樣會跑完
//   那個包裝。這一條要的是**本體**(那個會呼叫 rs_process_key 的
//   lambda)一次都沒有被進入。
TEST(work_queue_abandoned_key_job_never_runs_its_body) {
  WorkQueue q;
  q.Start();
  Gate gate;
  std::atomic<bool> entered{false};
  // 工作者被釘在一件慢工作裡 —— 也就是「引擎正在收 session / 部署」。
  BlockWorker(&q, &gate, &entered);

  std::atomic<int> body_ran{0};
  bool abandoned = false;
  const WorkQueue::Status s = q.CallAbandonable(
      "測試:按鍵", [&body_ran] { body_ran.fetch_add(1); }, 20, &abandoned);

  CHECK(s == WorkQueue::Status::kTimeout);
  // 呼叫端搶到了作廢權。搶到 = 從這一刻起本體保證不會跑。
  CHECK(abandoned);
  CHECK_INT(body_ran.load(), 0);

  // 放行。那件遲到的工作現在會被撿走 —— Stop() 保證已入列的工作
  // 都會被執行(見 ThreadMain),所以下面那一行不是在賽跑。
  gate.Open();
  q.Stop();

  // ★ 全條最重要的一行:那件工作**跑過了**,而本體一次都沒進去。
  CHECK_INT(body_ran.load(), 0);
}

// ── 16. 沒有逾時的時候,本體照樣要跑 ───────────────────────────
//
// 上面那一條單獨存在時,一個「永遠作廢」的實作可以全綠 ——
// 而那個實作的意思是「按鍵從此永遠不進引擎」。兩條要一起看。
TEST(work_queue_bounded_key_job_runs_when_it_makes_the_deadline) {
  WorkQueue q;
  q.Start();
  std::atomic<int> body_ran{0};
  bool abandoned = true;
  const WorkQueue::Status s = q.CallAbandonable(
      "測試:按鍵", [&body_ran] { body_ran.fetch_add(1); }, 1000, &abandoned);
  CHECK(s == WorkQueue::Status::kDone);
  CHECK(!abandoned);
  CHECK_INT(body_ran.load(), 1);
  q.Stop();
}
