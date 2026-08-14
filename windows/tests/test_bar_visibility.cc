// windows/tests/test_bar_visibility.cc — 那一橫什麼時候該在
//
// 使用者實機回報:「怎麼就常駐了」。他沒有說「我不要這個東西」——
// 那一橫是他自己要求的(product-gaps.md §4.1:「小小一橫在右下角,
// 可以拖動」)。他要拿掉的只有「他沒在用的時候還在」這一件事。
//
// ⚠ 為什麼這一支存在:顯示時機的缺陷**全部長成時序問題**,而時序問題
//   沒有測試就只能靠人肉試 —— 而人肉試不出「Alt+Tab 到另一個也用
//   LuminaKey 的程式時會不會閃一下」這種事。判準本身是純函式,
//   在 Ubuntu 上跑得完。

#include "bar_visibility.h"

#include "check.h"

using namespace rimewin;

namespace {

// 「使用者此刻正在用我們、開關開著」的那一組輸入。
//
// ⚠ in_use 是 common/bar_owner.h 收斂過的**一個布林**,不是「有幾條連線」。
//   收斂本身(13 個宿主怎麼變成一個答案)由 tests/test_bar_owner.cc 驗,
//   這一支只驗遲滯。
BarVisibilityInputs Using(uint64_t now_ms) {
  BarVisibilityInputs in;
  in.user_enabled = true;
  in.in_use = true;
  in.now_ms = now_ms;
  return in;
}

}  // namespace

// ── V1:總開關關掉 → 永遠不顯示,沒有例外 ─────────────────────────
TEST(bar_visibility_user_switch_wins_over_everything) {
  BarVisibility v;
  int seen = 0;
  for (int in_use = 0; in_use < 2; ++in_use) {
    for (int t = 0; t < 6; ++t) {
      BarVisibilityInputs in;
      in.user_enabled = false;
      in.in_use = in_use != 0;
      in.now_ms = static_cast<uint64_t>(seen) * 1000u;
      CHECK(v.Feed(in) == BarAction::kHide);
      ++seen;
    }
  }
  CHECK_INT(seen, 12);  // 掃描範圍非空(§2-G2)

  // ⚠ 而且**不留下待隱藏狀態**:使用者把開關打開的下一刻要立刻出現,
  //   不是「再等三秒」。
  CHECK(v.Feed(Using(99999)) == BarAction::kShow);
}

// ── V2:fail-visible 已經退場 —— 「沒在用」就是藏,沒有第三種 ────────
//
// ⚠ 這一支以前斷言的是相反的事:焦點不可知時一律顯示。當時白紙黑字的
//   理由是「焦點訊號在使用者打第一個字之前根本不送(ipc_client.cc 的
//   MayEatKey 閘)」,所以「不知道」是開機後的常態。
//
//   那個前提在這一輪消失了:在場連線在 ActivateEx 當下就建立並握手
//   (tsf/text_service.cc 的 PresenceLink),所以「使用者此刻在不在用
//   我們」在第一顆按鍵之前就答得出來。前提沒了,fail-visible 就從
//   保護變成漏洞 —— 它的意思是「不知道的時候顯示」,而使用者回報的
//   S4(切到微軟拼音之後那一橫還自己冒出來)正是「不知道」。
//
// ⚠ 「這一刻的前景答不了這個問題」不走這裡(OS 答不出前景,或前景是
//   **服務自己的設定視窗**):那由呼叫端**維持現狀**處理
//   (common/bar_owner.h 的 undecidable),不是在判準裡多開一個分支。
TEST(bar_visibility_not_in_use_hides_there_is_no_third_answer) {
  BarVisibility v;
  CHECK(v.Feed(Using(0)) == BarAction::kShow);

  BarVisibilityInputs gone = Using(0);
  gone.in_use = false;
  CHECK(v.Feed(gone) == BarAction::kPending);
  gone.now_ms = 3000;
  CHECK(v.Feed(gone) == BarAction::kHide);

  // 從頭開始也一樣:沒在用就是藏,不會因為「還不知道」而先顯示。
  BarVisibility fresh;
  BarVisibilityInputs idle;
  idle.user_enabled = true;
  idle.in_use = false;
  idle.now_ms = 60000;
  CHECK(fresh.Feed(idle) == BarAction::kHide);
}

// ── V3 / V4:遲滯的邊界 ───────────────────────────────────────────
TEST(bar_visibility_hide_waits_for_the_full_delay) {
  CHECK_INT(static_cast<int>(BarHideDelayMs()), 3000);

  BarVisibility v;
  CHECK(v.Feed(Using(0)) == BarAction::kShow);

  BarVisibilityInputs gone = Using(0);
  gone.in_use = false;
  CHECK(v.Feed(gone) == BarAction::kPending);

  // V3:還沒到期 —— 一毫秒都算數。
  gone.now_ms = 2999;
  CHECK(v.Feed(gone) == BarAction::kPending);

  // V4:邊界用 >= 釘死。3000 就是到期,不是 3001。
  gone.now_ms = 3000;
  CHECK(v.Feed(gone) == BarAction::kHide);

  // 到期之後繼續問還是 kHide(不會又跳回 pending)。
  gone.now_ms = 9999;
  CHECK(v.Feed(gone) == BarAction::kHide);
}

// ── V5:條件恢復要取消待隱藏 ─────────────────────────────────────
TEST(bar_visibility_pending_is_cancelled_when_it_comes_back) {
  BarVisibility v;
  CHECK(v.Feed(Using(0)) == BarAction::kShow);

  BarVisibilityInputs gone = Using(0);
  gone.in_use = false;
  CHECK(v.Feed(gone) == BarAction::kPending);

  // 1500 毫秒時又有人連上來。
  CHECK(v.Feed(Using(1500)) == BarAction::kShow);

  // ⚠ 核心:4000 毫秒時**不可以**因為「距離第一次離開已經超過 3000」
  //   而隱藏。待隱藏必須被取消,不是被延後。
  CHECK(v.Feed(Using(4000)) == BarAction::kShow);

  // 而且下一次真的離開時,三秒要重新從頭算。
  BarVisibilityInputs gone2 = Using(4000);
  gone2.in_use = false;
  CHECK(v.Feed(gone2) == BarAction::kPending);
  gone2.now_ms = 6999;
  CHECK(v.Feed(gone2) == BarAction::kPending);
  gone2.now_ms = 7000;
  CHECK(v.Feed(gone2) == BarAction::kHide);
}

// ── V6:Alt+Tab 到另一個也用 LuminaKey 的程式,一次都不可以閃 ───────
//
// ⚠ 「兩個宿主都在用我們的時候 in_use 仍然是真」那一半現在由
//   common/bar_owner.h 負責(tests/test_bar_owner.cc 的 O5)。這裡驗的是
//   剩下那一半:in_use 沒變的時候,這支狀態機一次 kPending 都不可以吐。
TEST(bar_visibility_switching_between_our_own_hosts_never_flickers) {
  BarVisibility v;
  CHECK(v.Feed(Using(0)) == BarAction::kShow);

  int seen = 0;
  for (uint64_t t = 100; t <= 10000; t += 100) {
    CHECK(v.Feed(Using(t)) == BarAction::kShow);
    ++seen;
  }
  CHECK_INT(seen, 100);
}

// ── V7:切到別的輸入法 → 遲滯到期後隱藏(#82 那一條不得倒退)────────
TEST(bar_visibility_switching_to_another_ime_hides_after_the_delay) {
  BarVisibility v;
  CHECK(v.Feed(Using(0)) == BarAction::kShow);

  // 使用者按 Win+空白鍵切到微軟拼音。背景那 12 個宿主的連線一條都沒斷,
  // 但 DecideBarOwner() 說前景那條執行緒上啟用中的不是我們。
  BarVisibilityInputs elsewhere = Using(0);
  elsewhere.in_use = false;
  CHECK(v.Feed(elsewhere) == BarAction::kPending);

  elsewhere.now_ms = 3500;
  CHECK(v.Feed(elsewhere) == BarAction::kHide);

  // 切回來 → 立刻(不是三秒後)。
  CHECK(v.Feed(Using(3600)) == BarAction::kShow);
}

// ── V8:Reset 之後不黏住 ─────────────────────────────────────────
TEST(bar_visibility_reset_clears_everything) {
  BarVisibility v;
  CHECK(v.Feed(Using(0)) == BarAction::kShow);
  BarVisibilityInputs gone = Using(0);
  gone.in_use = false;
  CHECK(v.Feed(gone) == BarAction::kPending);

  v.Reset();

  // ⚠ Reset 之後是「已經藏起來了」,不是「正在等著藏」——
  //   所以沒有人在用的時候直接 kHide,不會再吐一次 kPending。
  gone.now_ms = 10;
  CHECK(v.Feed(gone) == BarAction::kHide);
  // 而且顯示那一條照常運作。
  CHECK(v.Feed(Using(20)) == BarAction::kShow);
}

// ── 初始狀態:服務剛起來、還沒有人連上來 → 直接不顯示 ──────────────
TEST(bar_visibility_starts_hidden_without_a_pending_flash) {
  BarVisibility v;
  BarVisibilityInputs nobody;
  nobody.user_enabled = true;
  nobody.in_use = false;
  nobody.now_ms = 0;
  // ⚠ 不是 kPending。服務啟動時本來就沒有人在用,那一橫不該先出現
  //   三秒再消失 —— 那是一次沒有任何人要求過的閃爍。
  CHECK(v.Feed(nobody) == BarAction::kHide);
  nobody.now_ms = 5000;
  CHECK(v.Feed(nobody) == BarAction::kHide);

  // 第一個宿主連上來 → 立刻顯示。
  CHECK(v.Feed(Using(5001)) == BarAction::kShow);
}

// ── 時鐘倒退不可以把它卡住 ───────────────────────────────────────
TEST(bar_visibility_survives_a_backwards_clock) {
  BarVisibility v;
  CHECK(v.Feed(Using(10000)) == BarAction::kShow);
  BarVisibilityInputs gone = Using(10000);
  gone.in_use = false;
  CHECK(v.Feed(gone) == BarAction::kPending);
  // ⚠ 呼叫端餵的是**單調**時鐘,但這一支不該因為一個倒退的時間戳就
  //   永遠卡在 pending(那樣那一橫會再也藏不起來,而且沒有人查得出來)。
  gone.now_ms = 5000;
  CHECK(v.Feed(gone) == BarAction::kPending);
  gone.now_ms = 8001;  // 距離重新起算的 5000 已經超過 3000
  CHECK(v.Feed(gone) == BarAction::kHide);
}
