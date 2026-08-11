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

// 「有人在用、有焦點、開關開著」的那一組輸入。
BarVisibilityInputs Using(uint64_t now_ms) {
  BarVisibilityInputs in;
  in.user_enabled = true;
  in.active_clients = 1;
  in.focus_known = true;
  in.any_focused = true;
  in.now_ms = now_ms;
  return in;
}

}  // namespace

// ── V1:總開關關掉 → 永遠不顯示,沒有例外 ─────────────────────────
TEST(bar_visibility_user_switch_wins_over_everything) {
  BarVisibility v;
  int seen = 0;
  for (int clients = 0; clients < 3; ++clients) {
    for (int known = 0; known < 2; ++known) {
      for (int focused = 0; focused < 2; ++focused) {
        BarVisibilityInputs in;
        in.user_enabled = false;
        in.active_clients = clients;
        in.focus_known = known != 0;
        in.any_focused = focused != 0;
        in.now_ms = static_cast<uint64_t>(seen) * 1000u;
        CHECK(v.Feed(in) == BarAction::kHide);
        ++seen;
      }
    }
  }
  CHECK_INT(seen, 12);  // 掃描範圍非空(§2-G2)

  // ⚠ 而且**不留下待隱藏狀態**:使用者把開關打開的下一刻要立刻出現,
  //   不是「再等三秒」。
  CHECK(v.Feed(Using(99999)) == BarAction::kShow);
}

// ── V2:拿不到焦點資訊時一律視為「有」(fail-visible)────────────
TEST(bar_visibility_unknown_focus_is_visible) {
  BarVisibility v;
  BarVisibilityInputs in;
  in.user_enabled = true;
  in.active_clients = 1;
  in.focus_known = false;
  in.any_focused = false;  // ⚠ focus_known 為假時**不看**這一格
  in.now_ms = 0;
  CHECK(v.Feed(in) == BarAction::kShow);

  // ipc_client.cc 的 `if (!MayEatKey()) return;` —— 焦點訊號在使用者打
  // 第一個字之前根本不送。所以「還沒收到過焦點訊息」是**開機後的常態**,
  // 不是異常。這時候藏起來,那一橫就永遠等不到第一個字。
  in.now_ms = 60000;
  CHECK(v.Feed(in) == BarAction::kShow);
}

// ── V3 / V4:遲滯的邊界 ───────────────────────────────────────────
TEST(bar_visibility_hide_waits_for_the_full_delay) {
  CHECK_INT(static_cast<int>(BarHideDelayMs()), 3000);

  BarVisibility v;
  CHECK(v.Feed(Using(0)) == BarAction::kShow);

  BarVisibilityInputs gone = Using(0);
  gone.active_clients = 0;
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
  gone.active_clients = 0;
  CHECK(v.Feed(gone) == BarAction::kPending);

  // 1500 毫秒時又有人連上來。
  CHECK(v.Feed(Using(1500)) == BarAction::kShow);

  // ⚠ 核心:4000 毫秒時**不可以**因為「距離第一次離開已經超過 3000」
  //   而隱藏。待隱藏必須被取消,不是被延後。
  CHECK(v.Feed(Using(4000)) == BarAction::kShow);

  // 而且下一次真的離開時,三秒要重新從頭算。
  BarVisibilityInputs gone2 = Using(4000);
  gone2.active_clients = 0;
  CHECK(v.Feed(gone2) == BarAction::kPending);
  gone2.now_ms = 6999;
  CHECK(v.Feed(gone2) == BarAction::kPending);
  gone2.now_ms = 7000;
  CHECK(v.Feed(gone2) == BarAction::kHide);
}

// ── V6:Alt+Tab 到另一個也用 LuminaKey 的程式,一次都不可以閃 ───────
TEST(bar_visibility_switching_between_our_own_hosts_never_flickers) {
  BarVisibility v;
  // 兩個宿主都握著連線。
  BarVisibilityInputs two = Using(0);
  two.active_clients = 2;
  CHECK(v.Feed(two) == BarAction::kShow);

  // Alt+Tab:其中一個關掉了,還有一個在。
  int seen = 0;
  for (uint64_t t = 100; t <= 10000; t += 100) {
    BarVisibilityInputs one = Using(t);
    one.active_clients = 1;
    // ⚠ 一次 kPending 都不可以出現。判準是「這台機器上還有沒有人在用
    //   LuminaKey」,不是「哪一個輸入框」—— 同一支程式裡跳輸入框、
    //   在都用 LuminaKey 的程式之間 Alt+Tab,都不該讓它動。
    CHECK(v.Feed(one) == BarAction::kShow);
    ++seen;
  }
  CHECK_INT(seen, 100);
}

// ── V7:焦點是加強條件 —— 知道而且沒有焦點時才會走遲滯 ────────────
TEST(bar_visibility_known_unfocused_hides_after_the_delay) {
  BarVisibility v;
  CHECK(v.Feed(Using(0)) == BarAction::kShow);

  BarVisibilityInputs blurred = Using(0);
  blurred.focus_known = true;
  blurred.any_focused = false;  // 連線還在,但焦點跑到別的輸入法去了
  CHECK(v.Feed(blurred) == BarAction::kPending);

  blurred.now_ms = 3500;
  CHECK(v.Feed(blurred) == BarAction::kHide);

  // 焦點回來 → 立刻(不是三秒後)。
  CHECK(v.Feed(Using(3600)) == BarAction::kShow);
}

// ── V8:Reset 之後不黏住 ─────────────────────────────────────────
TEST(bar_visibility_reset_clears_everything) {
  BarVisibility v;
  CHECK(v.Feed(Using(0)) == BarAction::kShow);
  BarVisibilityInputs gone = Using(0);
  gone.active_clients = 0;
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
  nobody.active_clients = 0;
  nobody.focus_known = false;
  nobody.any_focused = false;
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
  gone.active_clients = 0;
  CHECK(v.Feed(gone) == BarAction::kPending);
  // ⚠ 呼叫端餵的是**單調**時鐘,但這一支不該因為一個倒退的時間戳就
  //   永遠卡在 pending(那樣那一橫會再也藏不起來,而且沒有人查得出來)。
  gone.now_ms = 5000;
  CHECK(v.Feed(gone) == BarAction::kPending);
  gone.now_ms = 8001;  // 距離重新起算的 5000 已經超過 3000
  CHECK(v.Feed(gone) == BarAction::kHide);
}
