#include "service_state.h"

#include "protocol.h"

namespace rimewin {

ServiceState ServiceStateOf(const EngineFacts& facts) {
  // ⚠ 順序是有意義的,而且每一條都比下一條更明確。
  if (!facts.engine_present) return ServiceState::kNotRunning;
  // 有了終局結果而且是失敗 —— 這一條要排在「還沒有結果」前面,
  // 否則失敗會被說成「還在準備」,而使用者會一直等一件不會發生的事。
  if (facts.deploy_done && !facts.deploy_ok) return ServiceState::kPrepareFailed;
  if (!facts.deploy_done) return ServiceState::kPreparing;
  // 上一次成功了,但引擎現在說自己還沒準備好 —— 使用者剛按了
  // 「重新整理字詞」。deploy_done 不會退回 false,只有這個旗標看得到。
  if (facts.engine_says_not_ready) return ServiceState::kPreparing;
  return ServiceState::kReady;
}

bool SnapshotSaysNotReady(uint32_t status_flags) {
  return (status_flags & kStDisabled) != 0;
}

bool SnapshotFlagsAreUsable(uint32_t status_flags) {
  return !SnapshotSaysNotReady(status_flags);
}

UiString StatusTextFor(ServiceState state) {
  // ⚠ 沒有 default:哪天多一種狀態而忘了給它一句話,編譯器會用 -Wswitch
  //   提醒。回一句別人的話比編不過糟得多 —— 那正是這個缺陷的長相。
  switch (state) {
    case ServiceState::kReady:
      // 就緒時那一橫畫的是四格,沒有句子。
      return UiString::kUiStringCount;
    case ServiceState::kPreparing:
      return UiString::kBarPreparing;
    case ServiceState::kPrepareFailed:
      return UiString::kBarPrepareFailed;
    case ServiceState::kNotRunning:
      return UiString::kBarNotRunning;
  }
  return UiString::kUiStringCount;
}

UiString SidebarStatusTextFor(ServiceState state) {
  switch (state) {
    case ServiceState::kReady:
      return UiString::kNavStatusReady;
    case ServiceState::kPreparing:
      return UiString::kNavStatusPreparing;
    case ServiceState::kPrepareFailed:
      return UiString::kNavStatusPrepareFailed;
    case ServiceState::kNotRunning:
      return UiString::kNavStatusNotRunning;
  }
  return UiString::kNavStatusNotRunning;
}

bool StateIsFailure(ServiceState state) {
  switch (state) {
    case ServiceState::kReady:
    case ServiceState::kPreparing:
      return false;
    case ServiceState::kPrepareFailed:
    case ServiceState::kNotRunning:
      return true;
  }
  return true;
}

bool StateShowsCells(ServiceState state) {
  return state == ServiceState::kReady;
}

}  // namespace rimewin
