// windows/tests/test_keypath_ledger.cc — 赦免機制不可以赦免真的失敗
//
// 這一組守的是一條真的被放出去過的假綠(覆核者在 Ubuntu 上把記帳邏輯
// 抄出來另外編一份才抓到 —— 而那份複本不會跟著程式碼走,所以現在改成
// 這裡:tsf_host_main.cc 用的就是被這一組驗的那一份 common/keypath_ledger.h)。
//
//   第一階段拿不到前景 → 黏滯的全域旗標被設成 true 且永不清除
//   第二階段 ForceForeground() 搶贏了 → 真的送了按鍵、真的量到打錯字
//   那一筆失敗仍然被記進「瞎的」那一格 → 被赦免 → 宿主 rc=0
//   腳本讀成「舊的 DLL 映像在升級之後照樣打得出「你好」」
//   而同一份 log 裡宿主自己印著 PHASE2_DOC="nihao"
//
// 底下 four_combinations_* 那四則就是這條 blocker 要求的四種組合。

#include "../common/keypath_ledger.h"

#include "check.h"

using namespace rimewin;

namespace {

// 一趟兩階段的跑法。把它寫成一個小 helper,是為了讓四種組合逐字對得起來:
// 差別只在兩個 bool,而不是四段長得不太一樣的程式碼。
//
// fail = 那一段有沒有量到失敗(打出來的字不對 / 探測沒成功)。
KeypathLedger TwoPhaseRun(bool p1_focus, bool p1_fail, bool p2_focus,
                          bool p2_fail, bool link_ok) {
  KeypathLedger led;
  led.NoteSegment(p1_focus);
  if (p1_fail) led.NoteFail(/*blind=*/!p1_focus);
  led.NoteSegment(p2_focus);
  if (p2_fail) led.NoteFail(/*blind=*/!p2_focus);
  led.set_link_verified(link_ok);
  return led;
}

}  // namespace

// ── 甲:第一階段瞎 / 第二階段瞎 → 赦免 ──────────────────────────
//
// 前提是乙那三句話成立。這是唯一一種赦免得起來的組合:兩段都什麼都
// 沒量到,而不需要前景的那條路整條走通了。
TEST(four_combinations_blind_blind_is_excused) {
  const KeypathLedger led = TwoPhaseRun(/*p1_focus=*/false, /*p1_fail=*/true,
                                        /*p2_focus=*/false, /*p2_fail=*/true,
                                        /*link_ok=*/true);
  CHECK_INT(led.fails_blind(), 2);
  CHECK_INT(led.fails_focused(), 0);
  CHECK_INT(led.excused_fails(), 2);
  CHECK_MSG(led.counted_fails() == 0,
            "兩段都瞎、而乙走通了 —— 這是唯一赦免得起來的組合");
  // 一段都沒量到 → 收尾那行 KEYPATH_MEASURED 必須是 0,
  // 而「這一次沒有量到」那句話准印。
  CHECK(!led.measured());
  CHECK(led.say_nothing_measured());
  CHECK(!led.say_partly_measured());
}

// ── 乙:第一階段瞎 / 第二階段有焦點且失敗 → **不可以赦免** ────────
//
// ⚠ 這一則就是那條 blocker。上一版在這裡回 rc=0。
TEST(four_combinations_blind_then_seen_failure_is_not_excused) {
  const KeypathLedger led = TwoPhaseRun(/*p1_focus=*/false, /*p1_fail=*/true,
                                        /*p2_focus=*/true, /*p2_fail=*/true,
                                        /*link_ok=*/true);
  // 第二階段那一筆是**有焦點時量到的**:它不可以落進「瞎的」那一格。
  CHECK_INT(led.fails_focused(), 1);
  CHECK_INT(led.fails_blind(), 1);
  CHECK_MSG(led.counted_fails() >= 1,
            "第二階段搶贏了前景、真的送了按鍵、真的量到打出來的字不對 —— "
            "那一筆不是「沒有答案」,是「壞掉」。宿主必須以非零結束。");

  // 而且收尾不可以說「這一次沒有量到」—— 明明量到了。
  CHECK_MSG(led.measured(),
            "第二階段有焦點 = PHASE2_KEYPATH_MEASURED=1;"
            "收尾的 KEYPATH_MEASURED 不可以是 0");
  CHECK_MSG(!led.say_nothing_measured(),
            "「按鍵路徑這一次沒有量到」在量到了的時候不准印");
  CHECK(led.say_partly_measured());
}

// ── 丙:第一階段有焦點且失敗 / 第二階段瞎 → 不可以赦免 ───────────
//
// 少了「兩個數字」這一分,一趟「第一階段有焦點、第二階段掉了」會把第一
// 階段**真的量到**的缺陷一起赦免掉。
TEST(four_combinations_seen_failure_then_blind_is_not_excused) {
  const KeypathLedger led = TwoPhaseRun(/*p1_focus=*/true, /*p1_fail=*/true,
                                        /*p2_focus=*/false, /*p2_fail=*/true,
                                        /*link_ok=*/true);
  CHECK_INT(led.fails_focused(), 1);
  CHECK_MSG(led.counted_fails() >= 1,
            "第一階段是有焦點時量到的失敗 —— 第二階段掉了前景不會讓它消失");
  CHECK(led.measured());
  CHECK(!led.say_nothing_measured());
}

// ── 丁:乙沒走通 → 一律不赦免 ──────────────────────────────────
//
// 一個什麼都沒驗到的段落綠起來,比它紅起來危險得多。
TEST(four_combinations_link_not_verified_excuses_nothing) {
  const KeypathLedger led = TwoPhaseRun(/*p1_focus=*/false, /*p1_fail=*/true,
                                        /*p2_focus=*/false, /*p2_fail=*/true,
                                        /*link_ok=*/false);
  CHECK_INT(led.excused_fails(), 0);
  CHECK_INT(led.counted_fails(), 2);
  CHECK(led.say_nothing_measured());
}

// ── 覆核者那一趟的逐字重現 ─────────────────────────────────────
//
// 他實測印出來的是:
//   第一階段瞎 + 第二階段有焦點且打錯字:
//     BLIND=2 SEEN=0 EXCUSED=1 -> rc=0
//     宿主結束碼 = 0
//
// 第一階段那兩筆的來源:搶不到前景本身記一筆,後面「文件裡是「」,
// 預期「你好」」再記一筆。第二階段打錯字記第三筆 —— 而第三筆是有焦點的。
TEST(reviewer_repro_phase2_wrong_text_must_be_red) {
  KeypathLedger led;
  led.NoteSegment(/*had_focus=*/false);
  led.NoteFail(/*blind=*/true);  // 送按鍵之前仍然拿不到執行緒焦點
  led.NoteFail(/*blind=*/true);  // 文件裡是「」,預期「你好」
  led.NoteSegment(/*had_focus=*/true);  // ForceForeground() 搶贏了
  led.NoteFail(/*blind=*/false);        // PHASE2_DOC="nihao",預期「你好」
  led.set_link_verified(true);          // 管道 / 握手 / session 都成立

  CHECK_INT(led.fails_blind(), 2);
  CHECK_INT(led.fails_focused(), 1);
  CHECK_INT(led.excused_fails(), 2);
  // 上一版這裡是 0(rc=0)。現在必須是 1。
  CHECK_INT(led.counted_fails(), 1);
  CHECK_MSG(led.measured(),
            "宿主自己印了 PHASE2_KEYPATH_MEASURED=1 —— "
            "收尾不可以在同一份 log 裡印 KEYPATH_MEASURED=0");
}

// ── 兩個欄位不可以互相矛盾(窮舉) ──────────────────────────────
//
// 這一則不是重複上面四則:它把兩段的四種焦點組合 × 失敗組合全部走一次,
// 對每一種都要求同一組不變式成立。上面四則說的是「這幾種要紅」,
// 這一則說的是「不管哪一種,這幾件事都不准發生」。
TEST(measured_and_say_lines_never_contradict) {
  for (int p1f = 0; p1f < 2; ++p1f)
    for (int p2f = 0; p2f < 2; ++p2f)
      for (int p1x = 0; p1x < 2; ++p1x)
        for (int p2x = 0; p2x < 2; ++p2x)
          for (int lk = 0; lk < 2; ++lk) {
            const KeypathLedger led = TwoPhaseRun(p1f != 0, p1x != 0, p2f != 0,
                                                  p2x != 0, lk != 0);
            // 1. 量到了就不准說沒量到。
            if (led.measured())
              CHECK_MSG(!led.say_nothing_measured(),
                        "measured() 為真時「這一次沒有量到」不准印");
            // 2. 那兩句話互斥。
            CHECK_MSG(!(led.say_nothing_measured() && led.say_partly_measured()),
                      "「完全沒量到」與「部分量到」不可以同時為真");
            // 3. 有焦點時記下的失敗**永遠**計入 —— 沒有任何組合赦得掉。
            CHECK_MSG(led.counted_fails() >= led.fails_focused(),
                      "有焦點時量到的失敗被赦免掉了");
            // 4. 有任何一段有焦點,measured() 就必須為真 ——
            //    它是數出來的,不是從「有沒有瞎過」推的。
            CHECK_MSG(led.measured() == (led.segments_focused() > 0),
                      "measured() 必須由量到的段落數決定");
            // 5. 赦免不可以超過瞎的那些。
            CHECK(led.excused_fails() <= led.fails_blind());
          }
}

// ── 單階段的跑法(§5c / §5d / §6c:沒有第二階段) ─────────────────
//
// 這些跑法永遠走不到「兩段」,但它們一樣會拿不到前景。
// 乙走通了才赦免,走不通就紅 —— 與兩階段同一條規矩。
TEST(single_phase_blind_needs_link_to_be_excused) {
  KeypathLedger red;
  red.NoteSegment(/*had_focus=*/false);
  red.NoteFail(/*blind=*/true);
  red.set_link_verified(false);
  CHECK_INT(red.counted_fails(), 1);
  CHECK(red.say_nothing_measured());

  KeypathLedger green;
  green.NoteSegment(/*had_focus=*/false);
  green.NoteFail(/*blind=*/true);
  green.set_link_verified(true);
  CHECK_INT(green.counted_fails(), 0);
  CHECK(green.say_nothing_measured());
}

TEST(single_phase_with_focus_is_never_excused) {
  KeypathLedger led;
  led.NoteSegment(/*had_focus=*/true);
  led.NoteFail(/*blind=*/false);
  led.set_link_verified(true);
  CHECK_INT(led.counted_fails(), 1);
  CHECK(led.measured());
  CHECK(!led.say_nothing_measured());
  CHECK(!led.say_partly_measured());
}

// ── 一段都沒走的跑法 ────────────────────────────────────────────
//
// --connect-only 之類的跑法根本不送按鍵。那時 KEYPATH_MEASURED=0,
// 但「這一次沒有量到(拿不到前景)」**不准印** —— 沒量到的原因不是前景,
// 是根本沒有要量。
TEST(no_segment_at_all_says_nothing_about_foreground) {
  KeypathLedger led;
  led.set_link_verified(true);
  CHECK(!led.measured());
  CHECK(!led.any_blind_segment());
  CHECK_MSG(!led.say_nothing_measured(),
            "一段都沒走的跑法不可以印「拿不到前景所以沒量到」");
  CHECK_INT(led.counted_fails(), 0);
}

// 記帳自己壞了(有瞎的失敗、卻沒有任何一段是瞎的)時不可以赦免。
// 壞掉的記帳拿來赦免東西,比沒有記帳更糟。
TEST(blind_fail_without_a_blind_segment_is_not_excused) {
  KeypathLedger led;
  led.NoteFail(/*blind=*/true);
  led.set_link_verified(true);
  CHECK_INT(led.excused_fails(), 0);
  CHECK_INT(led.counted_fails(), 1);
}
