// windows/common/callback_gate.h — 把一個「別人的執行緒會回頭呼叫的指標」
//                                   關到有生命週期保證為止
//
// ── 這個檔案存在的理由 ──────────────────────────────────────────────
//
//   service/engine.cc 有一個全域指標,由 `Engine::Start()` / `Engine::Stop()`
//   寫,由 **librime 的部署執行緒**讀:
//
//       Engine* g_deploy_engine = nullptr;          // Start 設,Stop 清
//       void OnDeploy(rs_deploy_status s, void*) {
//         if (g_deploy_engine)                      // ← check
//           g_deploy_engine->OnDeployTerminal();    // ← use
//       }
//
//   ⚠ **把它換成 `std::atomic<Engine*>` 是不夠的。** 那只讓「讀到的是不是
//     nullptr」這一問變成定義良好(不再是資料競賽,不再是未定義行為)。
//     它一個字都沒有說到讀出來**之後**的事:
//
//         Engine* p = g.load();   // 非空
//         ── 這裡回呼可以被作業系統排掉任意久 ──
//         ── 而那段時間足夠 main 跑完 Stop() 與 ~Engine() ──
//         p->OnDeployTerminal();  // ← 一段已經還掉的記憶體,而且它會寫進去
//
//     check 與 use 之間的窗口不是「小到可以忽略」,它是**被排程決定的**。
//     窗口小只代表它很難重現,不代表它不會發生;而這一條路上發生的樣子是
//     服務進程在使用者按下「重新整理字詞」之後隨機消失。
//
//   所以要的不是「讀得乾淨」,是**生命週期**:關門的人必須等到裡面沒有人。
//   這一支給的保證只有兩句,而兩句都是被鎖保證的、不是被時序猜的:
//
//     · `Close()` 返回之後,**沒有任何一個 `Run()` 還在裡面**;
//     · `Close()` 返回之後,之後的每一個 `Run()` 都直接回 false,
//       一個位元都不會碰那個物件。
//
//   於是「Close() 之後才解構那個物件」就是安全的,而那正是 Stop() 在做的事。
//
// ── ⚠ 用它的人要守的三條 ────────────────────────────────────────────
//
//   1. `fn` 在**閘的鎖裡面**跑。做得越久,`Close()` 就陪著等越久 ——
//      而 Close() 通常在關機路徑上。所以 fn 要短:動 atomic、排一件工作,
//      到此為止。
//   2. `fn` 裡面**不可以**再呼叫這個閘的 `Open()` / `Close()` / `Run()`。
//      鎖不是遞迴的,那是自己鎖自己。
//   3. **鎖序**:呼叫 `Run()` 的那條執行緒通常已經握著別人的鎖(engine 那
//      一處是 librime 的全域鎖),所以順序固定為「外面的鎖 → 閘的鎖」。
//      反過來 —— 握著閘的鎖再去拿那把外面的鎖 —— 就是死鎖。實務上這句話
//      的意思是:`Open()` / `Close()` 的呼叫點旁邊不可以有任何會進到那個
//      外部函式庫的呼叫。
//
// ⚠ 本檔**不含 windows.h**,而且不可以含。它與 work_queue.h 同一個理由:
//   這一格在 service/ 底下沒有任何自動化碰得到(那裡編不起來),
//   放在 common/ 才有 windows/tests/test_callback_gate.cc 逐條驗得到,
//   包含 `run_logic_tests.sh --asan`——「Close() 之後才 delete」是不是真的
//   安全,那才是會抓到的地方。
//
#ifndef RIMEWIN_COMMON_CALLBACK_GATE_H_
#define RIMEWIN_COMMON_CALLBACK_GATE_H_

#include <mutex>
#include <utility>

namespace rimewin {

template <typename T>
class CallbackGate {
 public:
  CallbackGate() = default;
  CallbackGate(const CallbackGate&) = delete;
  CallbackGate& operator=(const CallbackGate&) = delete;

  // 開門。之後 Run() 會拿到 target。重複開 = 換一個目標。
  void Open(T* target) {
    std::lock_guard<std::mutex> lock(mu_);
    target_ = target;
  }

  // 關門,**並且等到裡面沒有人**。見檔頭:這一句返回之後才可以解構那個
  // 物件,而這是整個類別唯一的重點。
  void Close() {
    std::lock_guard<std::mutex> lock(mu_);
    target_ = nullptr;
  }

  // 回呼端。門開著就以目標呼叫 fn 並回 true;門關著就什麼都不做並回 false。
  //
  // ⚠ 「check 然後 use」整段都在鎖裡面 —— 這正是與 `std::atomic<T*>` 的
  //   差別所在。fn 跑的期間 Close() 進不來,所以 fn 手上那個指標在 fn
  //   結束之前不會有人去解構它。
  template <typename F>
  bool Run(F&& fn) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!target_) return false;
    fn(target_);
    return true;
  }

  // 只給測試與診斷用。⚠ 拿它去做 if (open()) { ... 用那個物件 ... }
  // 就是把這個類別解決掉的那個缺陷原封不動搬回來 —— 要用就用 Run()。
  bool open() const {
    std::lock_guard<std::mutex> lock(mu_);
    return target_ != nullptr;
  }

 private:
  mutable std::mutex mu_;
  T* target_ = nullptr;
};

}  // namespace rimewin

#endif  // RIMEWIN_COMMON_CALLBACK_GATE_H_
