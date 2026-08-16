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

#include <type_traits>

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

  // ── ② 拿得去碰 UI 的條件比「吃掉那顆鍵」嚴格 ────────────────────
  //
  //   handled=1 配一份全 0 的快照,DLL 會走 RunSyncSession(ApplyPlan(…))
  //   把它套進文件 —— 空快照的意思是「沒有組字」,使用者打到一半的那一段
  //   當場消失,而且**沒有上屏**。所以 ui_is_current 只有在「真的切了、
  //   而且握著現況」那一格才是 true。
  //
  //   ⚠ 上一輪這裡寫的是「吃掉了 ⇒ 一定是現況」,而那是 #119 之前的
  //     規矩。#119 之後正確的形狀是「吃掉了、但不是現況 ⇒ 文件與 UI 都
  //     不准動,而且線路上要說出來」—— 兩者不是同一句話。
  for (int i = 0; i < 4; ++i) {
    const bool cur = (i & 1) != 0;
    const bool tog = (i & 2) != 0;
    const VariantKeyPlan p = PlanVariantKey(cur, tog);
    CHECK_MSG(p.ui_is_current == (cur && tog),
              "只有『真的切了、而且握著現況』那一格才准拿去碰 UI");
    CHECK_MSG(!p.ui_is_current || p.eat_key,
              "拿得去碰 UI 卻不吃那顆鍵 —— 宿主會同時做它自己的事");
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
  CHECK_MSG(!PlanVariantKey(true, false).eat_key,
            "什麼都沒做(toggled=false)就不可以吃掉那顆鍵");
  CHECK_MSG(!PlanVariantKey(false, false).eat_key,
            "什麼都沒做就不可以吃掉那顆鍵");
  // ⚠ (false, true) 這一格在呼叫端到不了(may_toggle 擋著),但答案仍然
  //   要對:**真的切了就一定要吃掉那顆鍵**,即使手上那一份不是現況。
  //   上一輪它回 false —— 那是 #119 那個錯的另一個化身,而
  //   VariantKeyPlanIsSound 的 ① 在那一格是空的(may_toggle 為 false),
  //   所以沒有人抓得到。
  CHECK_MSG(PlanVariantKey(false, true).eat_key,
            "切了就一定要吃掉那顆鍵,即使手上那一份不是現況");
  CHECK_MSG(!PlanVariantKey(false, true).ui_is_current,
            "而它一個像素都不准碰");
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

// ══════════════════════════════════════════════════════════════════
// ⭐ #119:`case Op::kKey` 上每一個會產生副作用的出口共用的那道門
// ══════════════════════════════════════════════════════════════════
//
// 使用者實測回報:「重開之後 Ctrl+空格 仍然切不動中英。」
//
// 真相是它**切動了**,只是同時發生了兩件他看得到的事:
//   · service/engine.cc 的 SetAsciiModeAll() 是 store + PostAsync(不等),
//     它排在那趟有上限的「切中英後取快照」**前面** —— 中英當場就切了;
//   · 那一趟逾時,上一輪照樣回 handled=false → TSF 的 OnPreservedKey 回
//     FALSE → **宿主也吃到那顆 Ctrl+空格**,而服務端那側
//     key_result_is_current 為 false → 那一橫一個像素都不動。
//
// 也就是說畫面說他沒切、宿主又做了自己的事,而引擎其實切了。
TEST(KeyDeadline_EveryKeyExitGoesThroughTheSameDoor) {
  // ── ① 副作用發生了 ⇒ 那顆鍵一定算被我們吃掉 ─────────────────
  //   #119 本身就是這一格:切了卻回 handled=false。
  CHECK_MSG(PlanKeyExit(/*side_effect_done=*/true,
                        /*snapshot_is_current=*/false)
                .eat_key,
            "中英已經切了而快照逾時 —— 這顆鍵必須算被我們吃掉,"
            "不然宿主的 Ctrl+空格 也會動作一次");
  CHECK_MSG(PlanKeyExit(true, true).eat_key,
            "切了、而且拿得到現況 —— 一樣要吃掉");

  // ── ② 吃掉 ≠ 可以動文件 ──────────────────────────────────────
  //   這一格是 ① 的代價,少了它 ① 就變成另一個缺陷:handled=1 配一份
  //   佔位,tsf/text_service.cc 的 SendAsciiToggle 會走 ApplyPlan 把它
  //   套進文件 —— 空快照的意思是「沒有組字」,使用者打到一半的那一段
  //   當場消失,而且沒有上屏。
  CHECK_MSG(PlanKeyExit(true, false).doc == KeyDocAction::kLeaveDocAlone,
            "不是現況的那一份不可以套進文件");
  CHECK_MSG(PlanKeyExit(true, true).doc == KeyDocAction::kApplySnapshot,
            "是現況就照舊套進文件,不然每一次成功的切換都不收尾");

  // ── ③ 什麼都沒做就不可以吃掉那顆鍵 ────────────────────────────
  //   key_eat_policy.h 檔頭那一課:做不到的事不要吃掉那顆鍵。
  CHECK(!PlanKeyExit(false, false).eat_key);
  CHECK(!PlanKeyExit(false, true).eat_key);

  // ── ④ 線路上的位元:極性只有一個方向 ──────────────────────────
  CHECK(KeyExitNeedsNotAnsweredBit(true, false));
  CHECK(KeyExitNeedsNotAnsweredBit(false, false));
  CHECK(!KeyExitNeedsNotAnsweredBit(true, true));
  CHECK(!KeyExitNeedsNotAnsweredBit(false, true));

  // 四格全跑一次,而且與 DecideKeyUiAction 是同一個答案 ——
  // 兩份真相就是兩個會漂移的畫面。
  for (int i = 0; i < 4; ++i) {
    const bool side = (i & 1) != 0;
    const bool cur = (i & 2) != 0;
    const KeyExitPlan p = PlanKeyExit(side, cur);
    CHECK(KeyExitPlanIsSound(side, cur));
    CHECK_MSG(!side || p.eat_key, "① 副作用發生了就一定要吃掉那顆鍵");
    CHECK_MSG(p.result_is_current || p.doc == KeyDocAction::kLeaveDocAlone,
              "② 不是現況就不准動文件");
    CHECK_MSG((DecideKeyUiAction(!p.result_is_current, p.result_is_current) ==
               KeyUiAction::kUpdateUi) == p.result_is_current,
              "③ 服務端那側的 UI 判準必須給出同一個答案");
    CHECK_MSG(!(p.eat_key && !p.result_is_current) ||
                  KeyExitNeedsNotAnsweredBit(side, cur),
              "④ 吃掉而且不是現況 ⇒ 線路上一定要說『引擎沒有回答』");
  }
}

TEST(KeyDeadline_VariantDoorDelegatesEatKeyToTheGeneralOne) {
  // ── ⚠ 上一輪這裡有一條**比不到東西**的斷言 ──────────────────────
  //
  //   它長這樣:VariantPlanAgreesWithKeyExit(s, t) 把 `s && t` **同時**
  //   餵給 PlanKeyExit 的兩個參數,所以四格只比到 (false,false) 與
  //   (true,true) —— 混合的那兩格從來沒有被比過,而
  //   (side_effect_done=true, snapshot_is_current=false) 正是 #119 那一格。
  //   覆核者實測:把 PlanKeyExit 的 eat_key 改回
  //   `side_effect_done && snapshot_is_current`(等於把 #119 反悔),
  //   那條斷言四格照樣過。
  //
  //   現在 PlanVariantKey 的 eat_key **就是** PlanKeyExit 的答案
  //   (common/key_deadline.h),所以「一份真相」是結構上的,不必靠比對。
  //   這裡改成釘那個**對應關係**:side_effect_done ← toggled,
  //   snapshot_is_current ← snapshot_is_current。有人把對應改成別的
  //   (例如又寫回 `cur && tog`),下面就會紅。
  for (int i = 0; i < 4; ++i) {
    const bool cur = (i & 1) != 0;
    const bool tog = (i & 2) != 0;
    CHECK_INT(PlanVariantKey(cur, tog).eat_key ? 1 : 0,
              PlanKeyExit(/*side_effect_done=*/tog,
                          /*snapshot_is_current=*/cur)
                      .eat_key
                  ? 1
                  : 0);
  }
  // ⚠ 而這一格證明那個對應**不是**退化的:把兩個參數餵成同一個值
  //   (上一輪那個寫法)會在這裡得到不同的答案。
  CHECK_MSG(PlanVariantKey(false, true).eat_key !=
                PlanKeyExit(false && true, false && true).eat_key,
            "混合的那一格必須真的被比到 —— 上一輪的斷言把兩個參數餵成"
            "同一個值,於是 #119 那一格從來沒有被比過");
}

TEST(KeyDeadline_GeneralDoorEatsOnSideEffectAlone) {
  // ⚠ 這一條是上面那條假斷言真正該做的事:**單獨**釘住
  //   「eat_key 只看 side_effect_done」。把它改回
  //   `side_effect_done && snapshot_is_current`(#119 反悔)的話,
  //   下面第一行就紅。
  CHECK_MSG(PlanKeyExit(/*side_effect_done=*/true,
                        /*snapshot_is_current=*/false)
                .eat_key,
            "#119:中英切了而快照逾時 —— 這一格必須吃掉那顆鍵");
  CHECK(PlanKeyExit(true, true).eat_key);
  CHECK(!PlanKeyExit(false, false).eat_key);
  CHECK(!PlanKeyExit(false, true).eat_key);
  // eat_key 與 snapshot_is_current **沒有**關係:同一個 side_effect_done
  // 下,兩格的答案必須一樣。
  CHECK_INT(PlanKeyExit(true, false).eat_key ? 1 : 0,
            PlanKeyExit(true, true).eat_key ? 1 : 0);
  CHECK_INT(PlanKeyExit(false, false).eat_key ? 1 : 0,
            PlanKeyExit(false, true).eat_key ? 1 : 0);
}

// ══════════════════════════════════════════════════════════════════
// ⭐ #119 在**執行期**沒有修好的那一格:副作用標記在半路被抹掉
// ══════════════════════════════════════════════════════════════════
//
// 上一顆 commit 宣稱修好了 #119。覆核者照 service/engine.cc 那兩支的
// 形狀寫最小重現、g++ 實跑,拿到 `timed_out=1 side_effect_done=0`:
//
//   ToggleAsciiMode()   SetAsciiModeAll(!now);
//                       wait->side_effect_done = true;   ← 標記
//                       CallKeyBounded(..., wait);
//   CallKeyBounded()    *wait = KeyWait();               ← 進門第一句抹掉
//
// ProcessKey() 沒事只是因為它填在 CallKeyBounded **之後** —— 那不是設計,
// 是運氣。這一組把約定釘成執行期斷言,而型別本身還多釘一層:
// copy assignment 被 = delete,`*wait = KeyWait();` 是編譯錯誤。
TEST(KeyDeadline_SideEffectMarkSurvivesTheTrip) {
  KeyWait kw;
  kw.ResetForNewCall();
  CHECK(!kw.side_effect_done());
  CHECK(!kw.timed_out);

  // ── ToggleAsciiMode() 的形狀:先做那件不可逆的事,再進門 ──────────
  kw.MarkSideEffectDone();
  CHECK(kw.side_effect_done());

  // ── CallKeyBounded() 進門第一句 ──────────────────────────────────
  kw.BeginTrip();
  CHECK_MSG(kw.side_effect_done(),
            "⭐ #119:一趟新的佇列等待**不可以**抹掉『那件不可逆的事已經"
            "做過了』—— 中英已經切了,而『重來一趟』不會把它切回去");

  // ── 逾時 ────────────────────────────────────────────────────────
  kw.timed_out = true;
  kw.abandoned = true;
  CHECK_MSG(kw.side_effect_done(),
            "逾時之後那個事實還在:本體沒跑,但 SetAsciiModeAll() 早就"
            "跑完了(它是 store + PostAsync,不等)");

  // ── 這一格就是使用者看到的那件事 ────────────────────────────────
  //   側邊那條鏈:PlanKeyExit → r.handled → TSF 的 OnPreservedKey。
  //   side_effect_done 被抹掉的話這裡拿到 (false, false) → eat_key=false
  //   → 宿主也吃到那顆 Ctrl+空格,與 65a4165 一模一樣。
  const KeyExitPlan p = PlanKeyExit(kw.side_effect_done(), !kw.timed_out);
  CHECK_MSG(p.eat_key,
            "#119 的整條鏈:標記活著 ⇒ eat_key ⇒ handled=true ⇒ "
            "OnPreservedKey 回 TRUE ⇒ 宿主不會再吃一次那顆 Ctrl+空格");
  CHECK(!p.result_is_current);
  CHECK(p.doc == KeyDocAction::kLeaveDocAlone);
  CHECK(KeyExitNeedsNotAnsweredBit(kw.side_effect_done(), !kw.timed_out));

  // ── 只進不出 ────────────────────────────────────────────────────
  kw.BeginTrip();
  kw.BeginTrip();
  CHECK_MSG(kw.side_effect_done(), "MarkSideEffectDone() 沒有反向的那一支");
  // 只有「一顆新的按鍵開始」才清得掉。
  kw.ResetForNewCall();
  CHECK_MSG(!kw.side_effect_done(),
            "而下一支按鍵入口的第一句話要把它歸零 —— 跨兩趟累計是"
            "呼叫端的責任(pipe_server.cc 的簡繁那一格)");
  CHECK(!kw.timed_out && !kw.abandoned);
}

// ⚠ 型別層那一道:`*wait = KeyWait();`(#119 的成因)必須是**編譯錯誤**。
//   這一條與上面那個執行期測試是兩件事 —— 上面守的是 BeginTrip() 的
//   內容,這一條守的是「沒有人能繞過 BeginTrip() 整份指派一次」。
static_assert(!std::is_copy_assignable<rimewin::KeyWait>::value,
              "KeyWait 的 copy assignment 必須是 = delete —— "
              "`*wait = KeyWait();` 正是 #119 在執行期沒有修好的那一行");
static_assert(!std::is_move_assignable<rimewin::KeyWait>::value,
              "move assignment 一樣不可以 —— `*wait = KeyWait();` 會挑它");
static_assert(!std::is_copy_constructible<rimewin::KeyWait>::value,
              "連複製一份出來再指派回去都不可以");
static_assert(std::is_default_constructible<rimewin::KeyWait>::value,
              "呼叫端要建得出來(pipe_server.cc 的 `Engine::KeyWait kw;`)");

TEST(KeyDeadline_AsciiToggleTimeoutIsNotAFailure) {
  // ⚠ 這一條釘的是 #119 的**另一半**:逾時的時候那一橫不可以整份凍住。
  //
  //   中英模式是服務端不必問引擎就知道的一件事(Engine::AsciiMode() 是
  //   一個 atomic,SetAsciiModeAll 那一句的 store 當場生效),而候選窗
  //   與組字那一段才是真的拿不到。所以「這一份 Result 能不能碰 UI」與
  //   「這顆鍵成功了沒有」是兩件事 —— 上一輪把它們混成一件,於是一個
  //   **成功**的操作被演成失敗的。
  const KeyExitPlan p = PlanKeyExit(/*side_effect_done=*/true,
                                    /*snapshot_is_current=*/false);
  CHECK_MSG(p.eat_key, "操作成功了(中英真的切了)");
  CHECK_MSG(!p.result_is_current, "而手上這一份不是引擎的現況");
  // 兩者同時成立,正是 pipe_server.cc 那一格走 bar_->OnAsciiMode()
  // 而不是 push_ui() 的判準。
  CHECK(p.eat_key && !p.result_is_current);
}
