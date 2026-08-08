// windows/tests/test_policy.cc — 組字政策與標籤格式
//
// 這裡測的兩件事都屬於「畫面看起來完全正常、自動化全過」的那一族:
// 選字之後該不該上屏、組字視窗該不該收掉。錯了的話使用者會說
// 「打完字沒出來」或「上一句黏在下一句前面」,而截圖看不出任何異狀。

#include "../common/ime_policy.h"

#include "check.h"

using namespace rimewin;

TEST(policy_commit_rule_is_menu_count_not_is_composing) {
  // 這條規則是 rime_shell.h 檔頭與 coordination.md §4 共同要求的,四端同一條。
  //
  //   count > 0                    → 還有段落待選,不可 commit
  //   count == 0 && is_composing   → 轉換完成待確認 → commit
  //   count == 0 && !is_composing  → 已結束
  CHECK(!ShouldCommitComposition(/*is_composing=*/true, /*count=*/5));
  CHECK(!ShouldCommitComposition(true, 1));
  CHECK(ShouldCommitComposition(true, 0));
  CHECK(!ShouldCommitComposition(false, 0));
  CHECK(!ShouldCommitComposition(false, 3));

  // 注音的真實情境:ㄋㄧˇㄏㄠˇ 選了只覆蓋第一音節的「你」之後,
  // preedit 變成「你ㄏㄠˇ」而 count = 4 —— 此時 commit 會吃掉後半段。
  CHECK(!ShouldCommitComposition(true, 4));
}

TEST(policy_plan_commit_and_end) {
  Snapshot s;
  s.has_commit = true;
  s.commit_text = "你好";
  s.preedit = "";
  const HostPlan p = PlanFromSnapshot(/*had_composition=*/true, s);
  CHECK(p.action == DocAction::kCommitAndEnd);
  CHECK_STR(p.commit_text, "你好");
  CHECK(!p.show_candidates);
}

TEST(policy_plan_commit_with_remaining_preedit) {
  // 選了句子的前半段,librime 立刻把前半段 commit,後半段仍在組字。
  // 若這裡判成 kCommitAndEnd,後半段會從畫面上消失,而使用者接著打的字
  // 全部接在錯的地方。
  Snapshot s;
  s.has_commit = true;
  s.commit_text = "你";
  s.preedit = "ㄏㄠˇ";
  s.items = {{"好", "", "1"}};
  const HostPlan p = PlanFromSnapshot(true, s);
  CHECK(p.action == DocAction::kCommitAndUpdate);
  CHECK_STR(p.commit_text, "你");
  CHECK_STR(p.preedit, "ㄏㄠˇ");
  CHECK(p.show_candidates);
}

TEST(policy_plan_update_and_end_and_nothing) {
  {
    Snapshot s;
    s.preedit = "ni";
    s.items = {{"你", "", "1"}, {"尼", "", "2"}};
    const HostPlan p = PlanFromSnapshot(false, s);
    CHECK(p.action == DocAction::kUpdate);
    CHECK(p.show_candidates);
  }
  {
    // 清空組字:必須明確收掉組字視窗,否則上一段 preedit 會留在畫面上。
    Snapshot s;
    const HostPlan p = PlanFromSnapshot(/*had_composition=*/true, s);
    CHECK(p.action == DocAction::kEnd);
    CHECK(!p.show_candidates);
  }
  {
    // 本來就沒有組字,什麼都不必做 —— 不可以無中生有開一個空的組字。
    Snapshot s;
    const HostPlan p = PlanFromSnapshot(/*had_composition=*/false, s);
    CHECK(p.action == DocAction::kNothing);
  }
}

TEST(policy_plan_empty_commit_text_is_not_a_commit) {
  // has_commit 為真但字串是空的:librime 不會這樣,但線路上可能被塞進來。
  // 當成沒有 commit 處理,不要對文件送出空字串(那會結束組字)。
  Snapshot s;
  s.has_commit = true;
  s.commit_text = "";
  s.preedit = "ni";
  const HostPlan p = PlanFromSnapshot(true, s);
  CHECK(p.action == DocAction::kUpdate);
}

TEST(policy_label_format_placeholders) {
  CHECK_STR(FormatLabel("{label}", "1", 0), "1");
  CHECK_STR(FormatLabel("{label}.", "3", 2), "3.");
  CHECK_STR(FormatLabel("{index}", "x", 0), "1");
  CHECK_STR(FormatLabel("{index}", "x", 8), "9");
  CHECK_STR(FormatLabel("{index0}", "x", 0), "0");
  CHECK_STR(FormatLabel("[{index}] {label}", "①", 4), "[5] ①");
  CHECK_STR(FormatLabel("", "1", 0), "");
  CHECK_STR(FormatLabel("no placeholder", "1", 0), "no placeholder");
}

TEST(policy_label_format_preserves_unknown_placeholders) {
  // 規範 §8.6.1:未知佔位符**必須**原樣保留,不得丟棄也不得報錯。
  // 實作成「丟掉」的話,主題作者看到的是標籤莫名變短,而且四端不一致。
  CHECK_STR(FormatLabel("{unknown}", "1", 0), "{unknown}");
  CHECK_STR(FormatLabel("{label}{nope}", "2", 1), "2{nope}");
  CHECK_STR(FormatLabel("{}", "1", 0), "{}");
  // 沒有配對的右大括號:剩下的原樣輸出,不報錯。
  CHECK_STR(FormatLabel("{label", "1", 0), "{label");
  CHECK_STR(FormatLabel("a{b", "1", 0), "a{b");
  // 大小寫敏感 —— {LABEL} 不是 {label}。
  CHECK_STR(FormatLabel("{LABEL}", "1", 0), "{LABEL}");
}
