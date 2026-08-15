// windows/tests/test_key_deadline.cc — 一顆按鍵的兩個上限,以及逾時那一份的去處
//
// 這一組守的是覆核者在 winfix-usable 上抓到的那條:按鍵逾時借用了
// kStDisabled,而那一份空快照會走進 pipe_server 的 push_ui ——
// 候選窗當場收掉、那一橫改寫成「正在準備字詞」。
//
// ⚠ 那條路的本體(engine.cc / pipe_server.cc)在開發機上編不起來
//   (要 MSVC)。所以判準搬到 common/key_deadline.h:
//
//   · 「不是現況的那一份不可以碰 UI」 → DecideKeyUiAction(),這裡逐格驗。
//   · 「服務端必須先放棄,而且留得下管道的時間」 → 那個標頭裡的
//     static_assert。它們在**編譯**這個檔案時就會爆,所以這裡再用
//     執行期斷言把同一條寫一次 —— 兩者的分工是:static_assert 讓
//     破壞關係的人編不過,而下面這些讓 run_logic_tests.sh 的報表上
//     看得到「這一條真的被檢查了」(這個框架沒有 SKIP,斷言數為 0
//     就算失敗)。
//   · 「一顆鍵在服務端只有一份預算」 → RemainingKeyBudgetMs()。
//   · 「呼叫端真的有把它接上去」 → 那一格編譯器守不到,由
//     run_logic_tests.sh 的文字守門負責(見那支腳本裡的 #93 那一段)。

#include "../common/key_deadline.h"

#include "check.h"

using namespace rimewin;

TEST(KeyDeadline_OnlyTheCurrentResultTouchesUi) {
  // 逾時的那一份不是快照,是佔位 —— UI 一個像素都不准動。
  CHECK(DecideKeyUiAction(/*timed_out=*/true, /*result_is_current=*/false) ==
        KeyUiAction::kLeaveUiAlone);
  // 沒有逾時、而且真的是引擎跑完之後那一份 —— 這一種才准更新。
  CHECK(DecideKeyUiAction(false, true) == KeyUiAction::kUpdateUi);
  // ⚠ **第三格**:沒有逾時,但那一份 Result 從頭到尾沒有被引擎寫過。
  //   簡繁快捷鍵「什麼都沒做」(ToggleVariantPref 回 false)就是這一格,
  //   而 pipe_server.cc 在 main 上照樣把它 push_ui:items 空的 →
  //   使用者組字到一半候選窗被收掉;旗標全 0 → SnapshotFlagsAreUsable()
  //   仍然是 true,那一橫於是把中/英寫成「中」、簡繁那格整個消失,
  //   而引擎可能正在英數模式、什麼都沒變。
  CHECK_MSG(DecideKeyUiAction(false, false) == KeyUiAction::kLeaveUiAlone,
            "『什麼都沒做』包含**不碰 UI** —— 沒有逾時不代表那一份是現況");
  // 第四格補齊(逾時了卻宣稱是現況:呼叫端寫錯了,一樣不准碰 UI)。
  CHECK(DecideKeyUiAction(true, true) == KeyUiAction::kLeaveUiAlone);
  // ⚠ 四格都要驗。少驗的話,把這支函式改成「永遠回 kLeaveUiAlone」
  //   仍然是綠的 —— 而那會讓候選窗從此不再更新,也就是完全打不出字。
  CHECK(DecideKeyUiAction(false, true) != DecideKeyUiAction(false, false));
}

TEST(KeyDeadline_ServiceGivesUpFirst) {
  // 服務端必須先放棄。反過來 = DLL 先 Fail() → Close(),一顆慢鍵的代價
  // 從「這顆鍵沒打出來」變成「整條連線被丟掉」。
  CHECK_MSG(kKeyDeadlineMs < kKeyTimeoutMs,
            "服務端的上限必須嚴格小於 DLL 端的逾時");
  // 光是「小於」不夠:放棄之後還要把回覆寫進管道並序列化。
  CHECK_MSG(kKeyDeadlineMs + kKeyPipeMarginMs <= kKeyTimeoutMs,
            "服務端放棄之後送回覆的時間沒有留夠 —— 等於沒有先放棄");
  // 餘裕不可以是 0:那樣上面那一條會退化成「小於等於」。
  CHECK(kKeyPipeMarginMs > 0);
}

TEST(KeyDeadline_OneKeyHasOneBudget) {
  // ── 服務端那一份預算是**一顆鍵**的,不是一趟佇列的 ─────────────
  //
  //   簡繁快捷鍵要走兩趟引擎佇列(回讀狀態 → 取快照)。兩趟各給
  //   kKeyDeadlineMs 的話最壞是 200ms,而 DLL 只有 150ms —— 上面那條
  //   「服務端先放棄」在執行期就被破壞了,而 static_assert 一個字都
  //   看不到。所以第二趟要問「還剩多少」。
  CHECK(RemainingKeyBudgetMs(0) == kKeyDeadlineMs);
  CHECK(RemainingKeyBudgetMs(30) == kKeyDeadlineMs - 30);
  // 用完了就是 0,不是負數 —— 負數傳進 WorkQueue::Call 的意思是
  // 「timeout <= 0 = 永遠等」,那正好是這整輪要修掉的東西。
  CHECK_MSG(RemainingKeyBudgetMs(kKeyDeadlineMs) == 0,
            "預算用完必須回 0;回負數會被佇列讀成『永遠等』");
  CHECK_MSG(RemainingKeyBudgetMs(kKeyDeadlineMs + 500) == 0,
            "超支一樣回 0");
  // 兩趟加起來不可以超過一顆鍵的預算。
  const int first = RemainingKeyBudgetMs(0);
  const int second = RemainingKeyBudgetMs(first);
  CHECK_MSG(first + second <= kKeyDeadlineMs,
            "兩趟佇列加起來超過一顆鍵的預算 = 服務端不再先放棄");
}

TEST(KeyDeadline_SlowKeyLogIsBelowTheCap) {
  // KEY_MS 那一行要看得到「快要逾時」的那一段,不能只記已經逾時的。
  CHECK_MSG(kKeySlowLogMs < kKeyDeadlineMs,
            "慢鍵的記錄門檻必須低於上限,否則只有逾時的那些會被記下來");
  CHECK(kKeySlowLogMs > 0);
}

TEST(KeyDeadline_NoLongerTheOldRegressionBand) {
  // ⚠ 這一條是**針對那段迴歸帶**的。
  //
  //   舊的組合是服務端 35 / DLL 50:引擎工作落在 35–50ms 的按鍵,
  //   在加上限之前打得出中文,加上之後變成 fail-open 的英文字母。
  //   放寬之後那一段回來了,而這裡把它釘住 —— 哪天有人想「保守一點」
  //   把服務端調回 50 以下,這一條會紅並且指著理由。
  CHECK_MSG(kKeyDeadlineMs >= 50,
            "服務端的上限低於 50ms 會重新造出一段『本來打得出中文、"
            "現在插英文字母』的迴歸帶");
}

TEST(KeyDeadline_HostStallIsCountedInTrips) {
  // ── ⚠ 這一條是上一輪那個**假保證**的替身 ────────────────────
  //
  //   上一輪這裡寫的是 `CHECK(kKeyTimeoutMs <= 200)`,理由是
  //   「DLL 端的逾時是宿主 UI 執行緒最壞停住的時間」。**那句話是錯的**,
  //   而且低估了一半以上:一顆鍵在 DLL 那側走**兩趟**管道 ——
  //   Exchange(送鍵 + 等回覆)與其後的 SendCaretRect(單向寫),
  //   兩趟各拿一份完整預算。照那條斷言把 kKeyTimeoutMs 調到 200,
  //   真正的最壞會是 400ms,而它會是綠的。
  //
  //   (Exchange 內部本來還更糟:WriteAllTimed 與 ReadFrameTimed 各拿
  //    一份,而 WriteAllTimed 在 while 迴圈裡每一塊都重新給一次 ——
  //    連絕對上限都沒有。那一格已經改成共用一個絕對 deadline,
  //    所以現在「一趟 ≤ kKeyTimeoutMs」是真的。)
  CHECK_MSG(kKeyPipeTripsPerKey >= 2,
            "一顆鍵在 DLL 那側至少走兩趟管道(Exchange + SendCaretRect)");
  CHECK(kKeyHostStallWorstMs == kKeyTimeoutMs * kKeyPipeTripsPerKey);
  CHECK_MSG(kKeyHostStallWorstMs <= 300,
            "宿主 UI 執行緒最壞停住的時間 = 一趟的預算 × 趟數。"
            "超過 300ms 之後使用者會以為程式沒收到而再按一次");
  // 而這一條會在有人「照著舊註解」把 kKeyTimeoutMs 調到 200 時紅:
  // 200 × 2 = 400 > 300。
  CHECK(kKeyTimeoutMs * kKeyPipeTripsPerKey <= 300);
}

TEST(KeyDeadline_VariantKeySideEffectGoesLast) {
  // ── ① 副作用發生了 ⇒ 那顆鍵一定算被我們吃掉 ──────────────────
  //
  //   ToggleVariantPref() 回 true 的當下簡繁**已經真的換掉了**。這時回
  //   handled=false 的話,tsf/text_service.cc 的 `if (!r.handled) return
  //   false` → `*eaten = FALSE` → TSF 把 Ctrl+Shift+F **也**交給宿主:
  //   使用者按一下同時得到「簡繁換了」與「VS Code 的跨檔搜尋開了」。
  CHECK_MSG(PlanVariantKey(/*snapshot_is_current=*/true, /*toggled=*/true)
                .eat_key,
            "副作用已經發生,那顆鍵必須算被我們吃掉");

  // ── ② 那顆鍵算被吃掉 ⇒ 手上那一份一定是引擎的現況 ──────────────
  //
  //   handled=1 配一份全 0 的快照,DLL 會走 RunSyncSession(ApplyPlan(…))
  //   把它套進文件 —— 空快照的意思是「沒有組字」,使用者打到一半的那一段
  //   當場消失,而且**沒有上屏**。
  for (int i = 0; i < 4; ++i) {
    const bool cur = (i & 1) != 0;
    const bool tog = (i & 2) != 0;
    const VariantKeyPlan p = PlanVariantKey(cur, tog);
    CHECK_MSG(!p.eat_key || p.ui_is_current,
              "吃掉了那顆鍵卻拿不出現況 —— DLL 會把空快照套進文件");
    CHECK_MSG(!p.eat_key || cur,
              "第一趟沒跑成(或引擎不認得這個 session)就不可以吃掉那顆鍵");
    // ③ 沒有現況快照就不准去做那件不可逆的事。這一格是 ①② 成立的理由:
    //   副作用排在最後一趟,前面每一趟失敗都退回「一步都沒做」。
    CHECK_MSG(p.may_toggle == cur,
              "那件不可逆的事必須排在**握著現況快照之後**");
    // 編譯期那一組(common/key_deadline.h 的 static_assert)在執行期再說
    // 一次 —— 這個框架沒有 SKIP,斷言數為 0 就算失敗。
    CHECK(VariantKeyPlanIsSound(cur, tog));
  }

  // ⚠ 四格都要驗。少驗的話,把 PlanVariantKey 改成「永遠吃掉」仍然是綠的
  //   —— 而那正是這一輪要修掉的那個迴歸(引擎切了、宿主也開了)。
  CHECK(!PlanVariantKey(true, false).eat_key);
  CHECK(!PlanVariantKey(false, true).eat_key);
  CHECK(!PlanVariantKey(false, false).eat_key);
  CHECK(PlanVariantKey(true, true).eat_key !=
        PlanVariantKey(true, false).eat_key);
}

TEST(KeyDeadline_VariantKeyNeverPredictsTheBudget) {
  // ⚠ 覆核者對「先確認預算夠走兩趟」提的陷阱:第一趟要花多久**事前不知道**,
  //   所以在最前面查一次不算數。這裡把那條性質釘住:兩趟各自問一次
  //   RemainingKeyBudgetMs(),而第一趟花掉多少都行 —— 第二趟拿到 0 時,
  //   ToggleVariantPref 的 `deadline_ms <= 0` 那道門回「什麼都沒做」,
  //   於是 toggled=false → 不吃那顆鍵 → 交回宿主,兩邊一致。
  for (int spent = 0; spent <= kKeyDeadlineMs + 50; ++spent) {
    const int left = RemainingKeyBudgetMs(spent);
    CHECK(left >= 0);
    CHECK(left <= kKeyDeadlineMs);
    // 預算用完的那一格必須表達得出「不做」,而不是退化成「永遠等」。
    if (left == 0) {
      CHECK_MSG(!PlanVariantKey(true, /*toggled=*/false).eat_key,
                "第二趟沒做成就不可以吃掉那顆鍵");
    }
  }
}
