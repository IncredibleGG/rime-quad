#include "redeploy_flow.h"

#include "protocol.h"

namespace rimewin {

bool AdvanceRedeploy(RedeployPhase* phase, RedeployEvent ev) {
  if (!phase) return false;
  const RedeployPhase p = *phase;
  // ⚠ 沒有 default,而且每一條邊都寫出來。哪天多一個階段或多一個事件,
  //   -Wswitch 會就地提醒 —— 而「悄悄從一個階段掉進另一個階段」正是
  //   這條線最貴的失敗方式。
  switch (ev) {
    case RedeployEvent::kRequested:
      // ⚠ 只有 kIdle 接得住。已經有一場在跑的時候再按一次,答案是
      //   「不行」,不是「再開一場」—— 兩場同時跑會在第二場收 session 的
      //   當下把第一場的 rs_deploy() 曝露在沒有保護的狀態。
      if (p != RedeployPhase::kIdle) return false;
      *phase = RedeployPhase::kClosingSessions;
      return true;
    case RedeployEvent::kSessionsClosed:
      // ⚠ **唯一**通往 kDeploying 的邊。這是整個檔案的重點:
      //   要改寫詞庫檔,一定得先走過「收乾淨」那一格。
      if (p != RedeployPhase::kClosingSessions) return false;
      *phase = RedeployPhase::kDeploying;
      return true;
    case RedeployEvent::kDeployFinished:
      if (p != RedeployPhase::kDeploying) return false;
      *phase = RedeployPhase::kRebuilding;
      return true;
    case RedeployEvent::kStartRefused:
      // rs_deploy() 拒絕啟動。收 session 那一步已經做了(或做到一半),
      // 所以**一定**要走重建那一格 —— 直接回 kIdle 的話,引擎裡一個
      // session 都沒有而階段卻說「一切正常」,使用者從此打不出中文。
      if (p != RedeployPhase::kClosingSessions && p != RedeployPhase::kDeploying)
        return false;
      *phase = RedeployPhase::kRebuilding;
      return true;
    case RedeployEvent::kTeardownFailed:
      // 收 session 沒有跑 —— session 全部還在,詞庫檔一個位元都沒被動過。
      // 直接回平常,不必重建。
      if (p != RedeployPhase::kClosingSessions) return false;
      *phase = RedeployPhase::kIdle;
      return true;
    case RedeployEvent::kRebuilt:
      if (p != RedeployPhase::kRebuilding) return false;
      *phase = RedeployPhase::kIdle;
      return true;
  }
  return false;
}

bool WorkspaceMayBeRewritten(RedeployPhase p) {
  return p == RedeployPhase::kDeploying;
}

bool SessionsMayExist(RedeployPhase p) {
  // ⚠ 刻意寫成上面那一支的否定,而不是另一張表。兩張表會漂移,
  //   而漂移的那一刻正好就是這個缺陷本身。
  return !WorkspaceMayBeRewritten(p);
}

bool SessionCreationAllowed(RedeployPhase p) {
  switch (p) {
    case RedeployPhase::kIdle:
      return true;
    case RedeployPhase::kClosingSessions:
    case RedeployPhase::kDeploying:
      return false;
    case RedeployPhase::kRebuilding:
      // 部署已經結束,詞庫檔穩定了。重建工作自己就在建 session,
      // 而宿主這時候要一個也沒問題(同一條引擎執行緒,排在重建後面)。
      return true;
  }
  return false;
}

bool TypingAllowed(RedeployPhase p, bool first_deploy_ok) {
  return first_deploy_ok && p == RedeployPhase::kIdle;
}

uint32_t GateStatusFlags(RedeployPhase p, bool first_deploy_ok) {
  return TypingAllowed(p, first_deploy_ok) ? 0u : kStDisabled;
}

bool ShouldFailOpen(RedeployPhase p, bool first_deploy_ok,
                    uint32_t* out_flags) {
  const bool allowed = TypingAllowed(p, first_deploy_ok);
  if (out_flags) *out_flags = GateStatusFlags(p, first_deploy_ok);
  return !allowed;
}

bool PhaseSaysPreparing(RedeployPhase p) { return RedeployInFlight(p); }

bool RedeployInFlight(RedeployPhase p) { return p != RedeployPhase::kIdle; }

const char* SessionRefusedReason(RedeployPhase p, bool first_deploy_ok) {
  // ⚠ 順序:重新部署那一條要排在「還沒部署過」前面。使用者在首次部署
  //   還沒完成時就按下重新整理是做得到的,而那時他真正在等的是後者。
  if (RedeployInFlight(p))
    return "使用者按了重新整理字詞,整理完才建得出 session";
  if (!first_deploy_ok)
    return "引擎還在第一次整理字詞,現在建不出 session";
  return "引擎建不出 session";
}

const char* RedeployPhaseName(RedeployPhase p) {
  switch (p) {
    case RedeployPhase::kIdle: return "idle";
    case RedeployPhase::kClosingSessions: return "closing-sessions";
    case RedeployPhase::kDeploying: return "deploying";
    case RedeployPhase::kRebuilding: return "rebuilding";
  }
  return "?";
}

}  // namespace rimewin
