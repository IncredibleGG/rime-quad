// windows/tests/test_status_line.cc — 狀態行「誰寫的」與「誰可以收回」
//
// ⚠ 這幾條守的是**訊息消失**這一類缺陷,而它有一個很難處理的性質:
//   使用者不會回報「我沒看到一句話」。所以它只能靠測試抓,不能靠
//   實機驗證抓 —— 實機上看起來就是一切正常。
//
#include "../common/status_line.h"

#include "check.h"

using namespace rimewin;

// ── 1. 沒有寫過的票永遠不算「還在畫面上」 ───────────────────────
//
// 呼叫端用 kNone 表示「我這一輪沒有排任何要收回的東西」。
// 這一條擋的是「預設值剛好等於某一則訊息」那種錯。
TEST(status_line_a_ticket_that_was_never_written_shows_nothing) {
  StatusLine s;
  CHECK(!s.StillShowing(StatusLine::kNone));
  const StatusLine::Ticket t = s.Write();
  CHECK(t != StatusLine::kNone);
  CHECK(s.StillShowing(t));
  // 別人的票(還沒發出去的那一號)不算數。
  CHECK(!s.StillShowing(t + 1));
}

// ── 2. 4 秒的自動清空不可以抹掉後來寫的紅字 ─────────────────────
//
// 這就是那個缺陷本身。時序:
//   使用者改了簡繁 → 「已套用」+ 起一個 4 秒的計時器
//   3 秒後他按了別的東西,存檔失敗 → 紅字
//   第 4 秒,計時器到 → 舊版無條件清空 → **紅字消失**
// 而使用者從頭到尾沒有機會知道存檔失敗過。
TEST(status_line_the_four_second_timer_does_not_wipe_a_later_message) {
  StatusLine s;
  const StatusLine::Ticket applied = s.Write();  // 「已套用」
  CHECK(s.StillShowing(applied));

  const StatusLine::Ticket failed = s.Write();  // 「設定存不起來」
  // 計時器醒來時要問的就是這一句 —— 而答案是「不是我那一則了」。
  CHECK(!s.StillShowing(applied));
  CHECK(s.StillShowing(failed));
}

// ── 3. 心跳解除時只能收回**自己**寫的那一句 ─────────────────────
//
// 反方向的同一個缺陷。舊版:
//   `if (stalled) SetStatus(kStatusEngineBusy); else SetStatus(L"");`
// 那個 else 是無條件的,而「引擎不忙了」與「使用者剛拿到一行紅字」
// 在時間上完全獨立 —— 兩者相撞的時候紅字就沒了。
TEST(status_line_the_heartbeat_only_takes_back_its_own_sentence) {
  StatusLine s;
  const StatusLine::Ticket busy = s.Write();  // 心跳:「還在處理剛才那一下」
  // 引擎還在忙的時候,使用者按了一顆會失敗的按鈕。
  const StatusLine::Ticket save_failed = s.Write();
  // 引擎終於回來了 —— 心跳想把自己那一句收回去。
  CHECK(!s.StillShowing(busy));  // 不可以清:畫面上已經不是它那一則
  CHECK(s.StillShowing(save_failed));

  // 反過來:沒有人蓋過去的話,心跳當然收得回自己那一句。
  StatusLine s2;
  const StatusLine::Ticket busy2 = s2.Write();
  CHECK(s2.StillShowing(busy2));
}

// ── 4. 序號只增不減 ─────────────────────────────────────────────
//
// 回收序號(例如清空時歸零)會讓一張很舊的票意外變成「還在畫面上」,
// 而那個錯誤只在跑很久的進程上、而且只在特定次數之後才出現。
TEST(status_line_serials_are_never_reused) {
  StatusLine s;
  StatusLine::Ticket first = s.Write();
  for (int i = 0; i < 1000; ++i) {
    const StatusLine::Ticket t = s.Write();
    CHECK(t != first);
    CHECK(s.StillShowing(t));
    CHECK(!s.StillShowing(first));
  }
  CHECK_INT(static_cast<int>(first), 1);
  CHECK_INT(static_cast<int>(s.current()), 1001);
}
