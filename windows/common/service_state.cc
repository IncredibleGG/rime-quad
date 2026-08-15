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

// ── ⚠ 一份**全 0** 的快照在這裡仍然算「可用」,而那是刻意留著的 ──────
//
//   覆核者問得對:這一支只看 kStDisabled,所以「引擎什麼都沒說」與
//   「引擎說一切正常、而且什麼開關都沒開」在這裡是同一個答案。那一格
//   真的會咬人 —— 簡繁快捷鍵「什麼都沒做」時推一份預設建構的快照上去,
//   那一橫就會把中/英寫成「中」、簡繁那格整個消失。
//
//   **這一輪沒有收緊它,而是把源頭堵掉**(pipe_server 的
//   DecideKeyUiAction:不是引擎現況的那一份根本不進 UI)。理由:
//
//   · 線路上**沒有**「這一份是真的」這個位元(common/protocol.h 的
//     Snapshot)。要在這裡分辨全 0 與真快照,得先加一個欄位,而那是
//     協議變更 —— 舊服務配新 DLL 時它一定是 0,於是收緊的結果會是
//     「升級之後那一橫整排消失」,比現在的缺陷更大。
//   · 拿現有欄位當「正面證據」都不成立:一個健康的引擎在英數模式、
//     用一個沒有字形開關的方案時,status_flags 真的就是全 0。
//     把它判成不可用 = 那一橫永遠不更新。
//
//   ⚠ 所以這裡留著,而**每一個推快照給 UI 的呼叫點**都必須自己回答
//     「這一份是不是引擎的現況」。按鍵那條路已經有守門(#93/#108);
//     其他路徑(SelectCandidate / ChangePage / SelectSchema 找不到
//     session 時同樣回全 0)還沒有,那是下一輪的事,不要以為這一支
//     會替它們擋。
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
