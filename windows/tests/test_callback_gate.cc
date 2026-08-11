// windows/tests/test_callback_gate.cc — 「關門的人要等到裡面沒有人」
//
// ⚠ 這裡測的**不是**「指標讀起來乾不乾淨」。把 service/engine.cc 那個
//   `Engine* g_deploy_engine` 換成 `std::atomic<Engine*>` 之後,下面第三條
//   仍然是紅的 —— 而它就是那個缺陷本身:回呼讀到一個非空的指標之後,
//   那個物件還是可能在它正在用的時候被解構。
//
//   實測(改之前先跑過):把 CallbackGate 的鎖拿掉、改成一個 atomic 指標,
//   `close_waits_for_the_callback_that_is_already_inside` 立刻紅
//   (Close() 在回呼還在裡面的時候就返回了),而
//   `after_close_the_target_can_be_deleted` 在 --asan 之下是
//   heap-use-after-free —— 那正是這個缺陷在真機上的樣子。
//
// ⚠ 「用一堆執行緒猛敲、期待 ASan 剛好撞上那個窗口」是**測不到**這件事的
//   (試過:天真版在 --asan 之下一路綠)。窗口由排程決定,而排程不會配合。
//   所以下面兩條都用一個閂把回呼**釘在 Run() 裡面**,再讓 Close() 去撞它 ——
//   要證明的性質本來就是「裡面有人的時候 Close() 會不會等」,那就直接讓
//   裡面有人。
//
// ⚠ 這一支不去測引擎 —— 引擎在 Ubuntu 上編不起來。測的是那條路上唯一
//   難寫對的機制,而它現在是一個自成一體、與 windows.h 無關的類別。
//
#include "../common/callback_gate.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "check.h"

using namespace rimewin;

namespace {

struct Target {
  std::atomic<int> hits{0};
};

// 一個「永遠不開,除非我們去開」的閂。用它把回呼釘在 Run() 裡面,
// 就能重現「回呼正在用那個物件」而不必去猜時序。
class Latch {
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

// 等一個旗標變成 true,**並且確定它真的變了**才返回。
// 不等這一下的話,測試會與「那條執行緒還沒被排到」賽跑,而輸的那一次
// 會是隨機的紅 —— 這個專案不接受間歇性的測試。
void WaitFlag(const std::atomic<bool>& flag) {
  for (int i = 0; i < 5000 && !flag.load(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(flag.load());
}

}  // namespace

// ── 1. 沒開門的時候,回呼一個位元都不會碰到那個物件 ──────────────────
TEST(gate_that_was_never_opened_runs_nothing) {
  CallbackGate<Target> gate;
  Target t;
  CHECK(!gate.open());
  bool ran = false;
  CHECK(!gate.Run([&](Target* p) {
    ran = true;
    p->hits.fetch_add(1);
  }));
  CHECK(!ran);
  CHECK_INT(0, t.hits.load());
}

// ── 2. 開了門就送得到,關了門就送不到 ───────────────────────────────
TEST(open_then_close_switches_delivery_on_and_off) {
  CallbackGate<Target> gate;
  Target t;
  gate.Open(&t);
  CHECK(gate.open());
  CHECK(gate.Run([](Target* p) { p->hits.fetch_add(1); }));
  CHECK(gate.Run([](Target* p) { p->hits.fetch_add(1); }));
  CHECK_INT(2, t.hits.load());

  gate.Close();
  CHECK(!gate.open());
  CHECK(!gate.Run([](Target* p) { p->hits.fetch_add(1); }));
  CHECK_INT(2, t.hits.load());

  // 關過再開仍然要能用(Start 失敗之後重試一次,走的就是這條)。
  gate.Open(&t);
  CHECK(gate.Run([](Target* p) { p->hits.fetch_add(1); }));
  CHECK_INT(3, t.hits.load());
  gate.Close();
}

// ── 3. ⚠ 這一條是整個檔案存在的理由 ─────────────────────────────────
//
//   Close() 不可以在回呼還在裡面用著那個物件的時候返回。返回了就代表
//   呼叫端(Engine::Stop → ~Engine)會在那個回呼**正在寫**的時候把物件
//   拆掉。只換成 std::atomic<T*> 的版本會在這裡紅:它的 Close() 只是
//   store(nullptr),而 store 不會等任何人。
TEST(close_waits_for_the_callback_that_is_already_inside) {
  CallbackGate<Target> gate;
  Target t;
  gate.Open(&t);

  Latch may_leave;
  std::atomic<bool> inside{false};
  std::atomic<bool> left{false};

  std::thread cb([&] {
    gate.Run([&](Target* p) {
      p->hits.fetch_add(1);
      inside.store(true);
      may_leave.Wait();
      // ⚠ 這一行代表「回呼還在用那個物件」的最後一刻。
      left.store(true);
    });
  });

  WaitFlag(inside);

  // 100 毫秒之後才放行。正確的閘會讓 Close() 等滿這 100 毫秒;
  // 只有一個 atomic 的版本會立刻返回,而那時 left 還是 false。
  std::thread releaser([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    may_leave.Open();
  });

  gate.Close();
  CHECK(left.load());  // ← 就是這一行

  cb.join();
  releaser.join();
  CHECK_INT(1, t.hits.load());
}

// ── 4. Close() 之後就可以拆掉那個物件 ───────────────────────────────
//
//   這是第 3 條的用途本身,也是這條路上真正的後果:Engine::Stop() 關門,
//   ~Engine() 拆物件,而 librime 的部署執行緒完全不知道發生了這件事。
//
//   ⚠ 這一條在 --asan 之下才看得到全貌:天真版的 delete 會落在回呼還在
//     裡面的時候,而回呼接下來那一句 `p->hits.fetch_add(1)` 就是
//     heap-use-after-free。沒有 --asan 時它只是靜靜地寫進一段還回去的
//     記憶體 —— 也就是使用者那一側「服務偶爾自己消失」的來源。
TEST(after_close_the_target_can_be_deleted) {
  CallbackGate<Target> gate;
  Target* t = new Target();
  gate.Open(t);

  Latch may_leave;
  std::atomic<bool> inside{false};
  std::atomic<bool> left{false};

  std::thread cb([&] {
    gate.Run([&](Target* p) {
      inside.store(true);
      may_leave.Wait();
      // ⚠ 這一句在 Close() 返回**之後**才跑得到的話,它寫的就是一段
      //   已經被 delete 掉的記憶體。
      p->hits.fetch_add(1);
      left.store(true);
    });
  });

  WaitFlag(inside);
  std::thread releaser([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    may_leave.Open();
  });

  gate.Close();
  CHECK(left.load());
  delete t;  // ← 只有「Close() 返回時裡面沒有人」才做得起這一句

  cb.join();
  releaser.join();
}

// ── 5. 關門之後,一個 Run() 都不可以再跑進去 ─────────────────────────
//
//   第 3、4 條守的是「正在裡面的那一個」;這一條守的是「後面來的那些」。
//   兩者都不成立的話,關門就只是一句話而不是一個保證。
TEST(no_run_gets_in_after_close_returns) {
  CallbackGate<Target> gate;
  Target* t = new Target();
  gate.Open(t);

  std::atomic<bool> stop{false};
  std::atomic<bool> closed{false};
  std::atomic<long> ran{0};
  std::atomic<long> ran_after_close{0};

  std::vector<std::thread> callers;
  for (int i = 0; i < 4; ++i) {
    callers.emplace_back([&] {
      while (!stop.load()) {
        // ⚠ 順序:先讀 closed 再 Run。反過來的話,一條在 Close() **之前**
        //   就已經跑完的 Run 會被記成「關門之後還跑得進去」,而那是假紅。
        const bool was_closed = closed.load();
        if (gate.Run([](Target* p) { p->hits.fetch_add(1); })) {
          ran.fetch_add(1);
          if (was_closed) ran_after_close.fetch_add(1);
        }
      }
    });
  }

  // 讓它們真的敲進去幾次,不然這條測試什麼都沒證明。
  for (int i = 0; i < 500 && ran.load() == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(ran.load() > 0);

  gate.Close();
  closed.store(true);
  delete t;  // ← 只有「Close() 返回時裡面沒有人」才做得起這一句

  // 拆掉之後再讓它們敲一陣子。
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  stop.store(true);
  for (std::thread& th : callers) th.join();

  CHECK_INT(0, static_cast<int>(ran_after_close.load()));
}

// ── 6. 兩個閘互不相干(同一個行程裡不只一個回呼來源時不會互卡)────────
TEST(two_gates_are_independent) {
  CallbackGate<Target> a;
  CallbackGate<Target> b;
  Target ta;
  Target tb;
  a.Open(&ta);
  b.Open(&tb);
  a.Close();
  CHECK(!a.Run([](Target* p) { p->hits.fetch_add(1); }));
  CHECK(b.Run([](Target* p) { p->hits.fetch_add(1); }));
  CHECK_INT(0, ta.hits.load());
  CHECK_INT(1, tb.hits.load());
  b.Close();
}
