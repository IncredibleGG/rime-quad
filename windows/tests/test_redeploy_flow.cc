// windows/tests/test_redeploy_flow.cc — 詞庫檔被改寫的那一刻不可以有 session
//
// 這一組守的是 common/redeploy_flow.h 檔頭那個缺陷:重新部署會在活著的
// session 腳下抽換 mmap,而 librime 那一側**不會報錯**(dict_compiler.cc
// 的 Remove() / Resize() 回傳值沒有人看)。所以「有沒有壞掉」在執行期
// 是看不出來的 —— 只能靠這一層的不變量守住。
//
// ⚠ 這些是**純函式**的測試。真的 mmap 行為(刪不掉、resize 不了)只有
//   Windows 上驗得到,而那一半寫在 commit 訊息裡。這裡驗的是:那條
//   「一定要先收乾淨」的路在階段機上**繞不過去**。

#include "../common/redeploy_flow.h"

#include <string>
#include <vector>

#include "../common/protocol.h"
#include "check.h"

using namespace rimewin;

namespace {

const RedeployPhase kAllPhases[] = {
    RedeployPhase::kIdle, RedeployPhase::kClosingSessions,
    RedeployPhase::kDeploying, RedeployPhase::kRebuilding};

const RedeployEvent kAllEvents[] = {
    RedeployEvent::kRequested,      RedeployEvent::kSessionsClosed,
    RedeployEvent::kDeployFinished, RedeployEvent::kStartRefused,
    RedeployEvent::kTeardownFailed, RedeployEvent::kRebuilt};

RedeployPhase Step(RedeployPhase p, RedeployEvent ev) {
  AdvanceRedeploy(&p, ev);
  return p;
}

}  // namespace

// 階段數與列舉沒有漂移。多一個階段而忘了把它加進下面每一組表,
// 這一條會先紅。
TEST(redeploy_phase_count_matches_table) {
  CHECK_INT(sizeof(kAllPhases) / sizeof(kAllPhases[0]), kRedeployPhaseCount);
}

// ── 這一組是整個檔案的重點 ────────────────────────────────────────
//
// 「可以改寫詞庫檔」與「可以有 session」永遠不可以同時為真。
TEST(rewrite_and_sessions_are_never_both_allowed) {
  int rewriting = 0;
  for (RedeployPhase p : kAllPhases) {
    CHECK(!(WorkspaceMayBeRewritten(p) && SessionsMayExist(p)));
    // 而且不可以同時為假 —— 那代表多了一個誰都不能動的死角。
    CHECK(WorkspaceMayBeRewritten(p) || SessionsMayExist(p));
    if (WorkspaceMayBeRewritten(p)) ++rewriting;
  }
  // 恰好一個階段可以改寫。零個 = 這條線根本不會部署;
  // 兩個 = 有一個階段偷偷放行了。
  CHECK_INT(rewriting, 1);
  CHECK(WorkspaceMayBeRewritten(RedeployPhase::kDeploying));
}

// 通往「可以改寫詞庫檔」的路**只有一條**,而它一定經過收乾淨那一格。
TEST(only_path_into_deploying_is_through_closing_sessions) {
  int edges_into_deploying = 0;
  for (RedeployPhase p : kAllPhases) {
    for (RedeployEvent ev : kAllEvents) {
      RedeployPhase q = p;
      // ⚠ 要問「真的動了嗎」,不是「動完之後是不是 kDeploying」——
      //   後者會把 kDeploying 收到一個不合法事件(原地不動)也算成一條邊。
      if (!AdvanceRedeploy(&q, ev)) continue;
      if (q != RedeployPhase::kDeploying) continue;
      ++edges_into_deploying;
      CHECK(p == RedeployPhase::kClosingSessions);
      CHECK(ev == RedeployEvent::kSessionsClosed);
    }
  }
  CHECK_INT(edges_into_deploying, 1);
}

// 從 kIdle 出發,不先送 kSessionsClosed 就到不了 kDeploying。
TEST(cannot_skip_the_teardown_step) {
  RedeployPhase p = RedeployPhase::kIdle;
  // 直接宣稱部署結束 —— 不合法,原地不動。
  CHECK(!AdvanceRedeploy(&p, RedeployEvent::kDeployFinished));
  CHECK(p == RedeployPhase::kIdle);
  CHECK(!AdvanceRedeploy(&p, RedeployEvent::kSessionsClosed));
  CHECK(p == RedeployPhase::kIdle);

  CHECK(AdvanceRedeploy(&p, RedeployEvent::kRequested));
  CHECK(p == RedeployPhase::kClosingSessions);
  // 還沒收完就不可以改寫。
  CHECK(!WorkspaceMayBeRewritten(p));
  CHECK(AdvanceRedeploy(&p, RedeployEvent::kSessionsClosed));
  CHECK(p == RedeployPhase::kDeploying);
  CHECK(WorkspaceMayBeRewritten(p));
}

// 一整圈:按下去 → 收乾淨 → 部署 → 重建 → 回到平常。
TEST(happy_path_returns_to_idle) {
  RedeployPhase p = RedeployPhase::kIdle;
  CHECK(AdvanceRedeploy(&p, RedeployEvent::kRequested));
  CHECK(AdvanceRedeploy(&p, RedeployEvent::kSessionsClosed));
  CHECK(AdvanceRedeploy(&p, RedeployEvent::kDeployFinished));
  CHECK(p == RedeployPhase::kRebuilding);
  CHECK(AdvanceRedeploy(&p, RedeployEvent::kRebuilt));
  CHECK(p == RedeployPhase::kIdle);
  CHECK(TypingAllowed(p, true));
}

// ⚠ 部署**失敗**與 rs_deploy() **拒絕啟動**,兩條都要回得到重建。
//
//   少了任何一條,session 就永遠停在「已經收掉、沒有建回來」——
//   使用者從此打不出中文,而且重開機也沒用(每次啟動都會再走一次
//   同樣的失敗)。這是這條線最貴的失敗方式。
TEST(every_way_out_of_deploying_rebuilds_the_sessions) {
  int edges_out = 0;
  for (RedeployEvent ev : kAllEvents) {
    RedeployPhase p = RedeployPhase::kDeploying;
    if (!AdvanceRedeploy(&p, ev)) continue;
    ++edges_out;
    CHECK(p == RedeployPhase::kRebuilding);
  }
  // 終局(成功/失敗共用一條)與「拒絕啟動」。
  CHECK_INT(edges_out, 2);
}

// 收 session 收到一半 rs_deploy() 就拒絕了 —— 一樣要走重建。
TEST(refused_start_while_closing_still_rebuilds) {
  RedeployPhase p = RedeployPhase::kClosingSessions;
  CHECK(AdvanceRedeploy(&p, RedeployEvent::kStartRefused));
  CHECK(p == RedeployPhase::kRebuilding);
}

// 沒有任何一條事件邊可以讓「有 session 的階段」直接跳成「可以改寫」
// 以外的方式繞過去;而且任何階段收到任何事件都不會掉出這四個值。
TEST(transitions_stay_inside_the_four_phases) {
  for (RedeployPhase p : kAllPhases) {
    for (RedeployEvent ev : kAllEvents) {
      const RedeployPhase after = Step(p, ev);
      bool known = false;
      for (RedeployPhase q : kAllPhases)
        if (after == q) known = true;
      CHECK(known);
    }
  }
}

// 不合法的事件一律原地不動,而且回 false。
TEST(illegal_events_do_not_move_the_phase) {
  int legal = 0;
  for (RedeployPhase p : kAllPhases) {
    for (RedeployEvent ev : kAllEvents) {
      RedeployPhase q = p;
      const bool ok = AdvanceRedeploy(&q, ev);
      if (ok) {
        ++legal;
        CHECK(q != p);
      } else {
        CHECK(q == p);
      }
    }
  }
  // kRequested / kSessionsClosed / kDeployFinished / kTeardownFailed /
  // kRebuilt 各一條,kStartRefused 兩條(kClosingSessions 與 kDeploying)。
  CHECK_INT(legal, 7);
}

// ⚠ 「收 session 那一步根本沒跑」與「rs_deploy() 拒絕啟動」是兩件事,
//   而它們的下一格必須不同:前者 session 全部還在(回平常就好),
//   後者 session 已經收掉了(一定要重建)。
//   混成同一條的話,不是多建一批沒有人要的 session,就是把還活著的
//   那一批漏掉 —— 而漏掉的那一種是使用者從此打不出中文。
TEST(teardown_failure_and_refused_start_go_different_ways) {
  RedeployPhase a = RedeployPhase::kClosingSessions;
  CHECK(AdvanceRedeploy(&a, RedeployEvent::kTeardownFailed));
  CHECK(a == RedeployPhase::kIdle);

  RedeployPhase b = RedeployPhase::kClosingSessions;
  CHECK(AdvanceRedeploy(&b, RedeployEvent::kStartRefused));
  CHECK(b == RedeployPhase::kRebuilding);

  CHECK(a != b);

  // 已經開始改寫詞庫檔之後就不可以再說「收 session 沒跑」——
  // 那條路會直接回平常,而此刻一個 session 都沒有。
  RedeployPhase c = RedeployPhase::kDeploying;
  CHECK(!AdvanceRedeploy(&c, RedeployEvent::kTeardownFailed));
  CHECK(c == RedeployPhase::kDeploying);
}

// 已經有一場在跑的時候再按一次「重新整理字詞」→ 不行。
TEST(second_request_is_refused) {
  for (RedeployPhase p : kAllPhases) {
    RedeployPhase q = p;
    const bool ok = AdvanceRedeploy(&q, RedeployEvent::kRequested);
    CHECK_INT(ok ? 1 : 0, p == RedeployPhase::kIdle ? 1 : 0);
  }
}

// ── 按鍵那道門 ────────────────────────────────────────────────────
//
// 舊版的判準是 deploy_state_ == 1,而那個值首次部署成功後**永遠**是 1。
// 所以只驗 first_deploy_ok 是不夠的:重新部署的每一個階段都要關門。
TEST(typing_is_closed_during_every_redeploy_phase) {
  for (RedeployPhase p : kAllPhases) {
    const bool idle = (p == RedeployPhase::kIdle);
    CHECK_INT(TypingAllowed(p, true) ? 1 : 0, idle ? 1 : 0);
    // 首次部署還沒成功 —— 任何階段都不准打。
    CHECK(!TypingAllowed(p, false));
  }
}

// 不能打字的時候,回給宿主的快照一定要帶 kStDisabled。
// ⚠ 沒有它的話,狀態列會把那份**預設建構**的快照當成真的:
//   使用者剛切成 En 的那一格會自己跳回「中」,而他沒碰過任何開關。
TEST(closed_gate_always_reports_disabled) {
  for (RedeployPhase p : kAllPhases) {
    for (int ok = 0; ok < 2; ++ok) {
      uint32_t flags = 0xFFFFFFFFu;
      const bool fail_open = ShouldFailOpen(p, ok != 0, &flags);
      CHECK_INT(fail_open ? 1 : 0, TypingAllowed(p, ok != 0) ? 0 : 1);
      if (fail_open) {
        CHECK_INT(flags & kStDisabled, kStDisabled);
        // ⚠ 加上「引擎對這顆鍵一個字都沒說」。這道門是 return !allowed
        //   —— 那顆鍵根本沒交給引擎,所以這個位元照定義成立。
        CHECK_INT(flags & kStKeyNotAnswered, kStKeyNotAnswered);
        // 而且**只有**那兩格 —— 其餘旗標必須是 0,不可以順手說一句
        // 「現在是中文」或「現在是簡體」,那些現在沒有人知道。
        //
        // ⚠ 上一版這裡是 `CHECK_INT(flags, kStDisabled)`,而那條理由
        //   (「不可以聲稱沒有人知道的狀態」)擋的是**輸入狀態**那一族。
        //   kStKeyNotAnswered 不是輸入狀態,它是關於**這顆鍵**的事實,
        //   而這道門正好知道那件事。
        CHECK_INT(flags, kStDisabled | kStKeyNotAnswered);
      } else {
        CHECK_INT(flags, 0);
      }
      CHECK_INT(GateStatusFlags(p, ok != 0), flags);
    }
  }
}

// ══ #116:關著的門一定要說「引擎沒有回答這顆鍵」════════════════════
//
// 這一條與上面那個逐格掃描是**分開**的一條,理由是它守的東西不一樣:
// 上面守「旗標的完整值」,這一條守「那條會出人命的路」,而紅字要直接
// 說出使用者看到的症狀。
//
// ⚠ 它證明得了、也證明不了的:
//   · 證明得到 —— 門關著時,回給 DLL 的快照帶著 kStKeyNotAnswered。
//   · 證明不到 —— DLL 收到之後真的沒有動文件。那一段在 tsf/(這台機器上
//     編不起來),由 tests/test_key_eat_policy.cc 的 DecideKeyOutlet 真值表
//     與真機 / CI 的 §13c 接手。
//
// 把這一格弄丟的樣子:使用者升級之後在訊息框裡打「你好」拿到「ni好」。
TEST(closed_gate_says_the_engine_never_answered_the_key) {
  for (RedeployPhase p : kAllPhases) {
    for (int ok = 0; ok < 2; ++ok) {
      const bool closed = !TypingAllowed(p, ok != 0);
      const uint32_t flags = GateStatusFlags(p, ok != 0);
      // 門關著 ⟺ 有那個位元。**兩個方向都驗** —— 只驗一邊的話,一支
      // 「永遠回 kStKeyNotAnswered」的實作也會綠,而那會讓 DLL 在引擎
      // 健康的時候把每一顆字母都吃掉不動(打字完全沒反應)。
      CHECK_INT((flags & kStKeyNotAnswered) != 0 ? 1 : 0, closed ? 1 : 0);
    }
  }
  // 逐一點名那三個「門是關的」情境,免得哪天 TypingAllowed 自己被改鬆了
  // 之後這條測試跟著一起變成空話。
  //   · 首次部署還沒成功(使用者剛裝完)
  CHECK_INT(GateStatusFlags(RedeployPhase::kIdle, false) & kStKeyNotAnswered,
            kStKeyNotAnswered);
  //   · 使用者按了「重新整理字詞」,正在收 session
  CHECK_INT(GateStatusFlags(RedeployPhase::kClosingSessions, true) &
                kStKeyNotAnswered,
            kStKeyNotAnswered);
  //   · 正在改寫詞庫檔
  CHECK_INT(GateStatusFlags(RedeployPhase::kDeploying, true) &
                kStKeyNotAnswered,
            kStKeyNotAnswered);
  // 而引擎好好的時候一個位元都不准有 —— 這一句就是上面那個「反方向」
  // 在真實情境上的點名:平常打字的每一顆鍵都走這裡。
  CHECK_INT(GateStatusFlags(RedeployPhase::kIdle, true), 0u);
}

// out_flags 傳 nullptr 不可以炸。
TEST(should_fail_open_tolerates_null_out) {
  CHECK(ShouldFailOpen(RedeployPhase::kDeploying, true, nullptr));
  CHECK(!ShouldFailOpen(RedeployPhase::kIdle, true, nullptr));
}

// AdvanceRedeploy 收到 nullptr 不可以炸。
TEST(advance_tolerates_null_phase) {
  CHECK(!AdvanceRedeploy(nullptr, RedeployEvent::kRequested));
}

// ── 建 session 那道門 ─────────────────────────────────────────────
//
// ⚠ kClosingSessions 必須是 false:收乾淨與 rs_deploy() 之間有一段空檔,
//   而剛開的程式正好會在那時候要 session。
TEST(session_creation_is_closed_until_the_rewrite_is_over) {
  CHECK(SessionCreationAllowed(RedeployPhase::kIdle));
  CHECK(!SessionCreationAllowed(RedeployPhase::kClosingSessions));
  CHECK(!SessionCreationAllowed(RedeployPhase::kDeploying));
  CHECK(SessionCreationAllowed(RedeployPhase::kRebuilding));
  // 允許建的階段,一定也是允許 session 存在的階段。
  for (RedeployPhase p : kAllPhases)
    if (SessionCreationAllowed(p)) CHECK(SessionsMayExist(p));
}

// ── 畫面上必須說出來 ──────────────────────────────────────────────
//
// 部署期間打不出中文是刻意的(與首次部署一致),但使用者不可以
// 因此以為輸入法壞了。所以「不能打字」的每一個階段都得讓畫面說
// 「正在準備」。
TEST(every_closed_phase_tells_the_user) {
  for (RedeployPhase p : kAllPhases) {
    if (!TypingAllowed(p, true)) CHECK(PhaseSaysPreparing(p));
    if (PhaseSaysPreparing(p)) CHECK(!TypingAllowed(p, true));
    CHECK_INT(PhaseSaysPreparing(p) ? 1 : 0, RedeployInFlight(p) ? 1 : 0);
  }
}

// ── 建不出 session 時給的原因要分得開 ─────────────────────────────
TEST(session_refused_reasons_are_three_different_sentences) {
  const std::string first_run =
      SessionRefusedReason(RedeployPhase::kIdle, false);
  const std::string redeploying =
      SessionRefusedReason(RedeployPhase::kDeploying, true);
  const std::string other = SessionRefusedReason(RedeployPhase::kIdle, true);
  CHECK(first_run != redeploying);
  CHECK(first_run != other);
  CHECK(redeploying != other);
  CHECK(!first_run.empty());
  CHECK(!redeploying.empty());
  CHECK(!other.empty());
  // 首次部署還沒好的時候按下重新整理 —— 他真正在等的是重新整理那一場。
  CHECK_STR(SessionRefusedReason(RedeployPhase::kDeploying, false),
            redeploying);
}

// 階段名字四個都不一樣(日誌裡分得出來)。
TEST(phase_names_are_distinct) {
  std::vector<std::string> seen;
  for (RedeployPhase p : kAllPhases) {
    const std::string n = RedeployPhaseName(p);
    CHECK(!n.empty());
    CHECK(n != "?");
    for (const std::string& s : seen) CHECK(s != n);
    seen.push_back(n);
  }
  CHECK_INT(static_cast<int>(seen.size()), kRedeployPhaseCount);
}
