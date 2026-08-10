// windows/service/update_check.h — 「檢查更新」的接縫
//
// ═══════════════════════════════════════════════════════════════════
//  ⚠ **這個接縫是給 win-update 那條線填的。** 本輪(win-netui)交出來的
//    是介面與呼叫點:背景執行緒、完成訊息、五種結果各自的那一句話,
//    以及「開關關著時根本不進來」那一層。真的去問版本的那一段還沒有,
//    所以 update_check.cc 目前回的是 kNotWired ——
//    **不是** kUpToDate。把「還沒接上」講成「已經是最新的」是在宣稱
//    一件沒有發生的事,而那是這個專案的硬約束之一。
// ═══════════════════════════════════════════════════════════════════
//
// ── 合併時要對的三件事 ──────────────────────────────────────────
//
//   1. **函式簽名**(下面那一支)。呼叫端在 service/settings_window.cc 的
//      SettingsWindow::UpdateThreadEntry 裡,已經跑在背景執行緒上,
//      結果用 WM_RIME_UPDATE_DONE 送回 UI 執行緒。
//   2. **連線只能走 gate。** 這裡拿到的是 NetGate*,那是 Windows 端唯一的
//      連網出口(windows/audit_offline_win.sh 守著「只有 service/net_gate.cc
//      碰得到網路 API」)。**不要在這個檔案裡開第二個出口** ——
//      單一出口是硬約束,不是慣例。
//   3. **五種結果不可以合併。** UpdateCheckState 的五格各對應一句不同的
//      話(common/net_ui.h 的 UpdateStateText),而
//      tests/test_net_ui.cc 的 net_ui_update_states_are_five_different_sentences
//      會擋下任何兩格說同一句。特別是 kBlocked(開關是關的,一個位元組
//      都沒送)與 kFailed(送了但沒成功)——那兩件事要給使用者的
//      下一步完全不同。
//
#ifndef RIMEWIN_SERVICE_UPDATE_CHECK_H_
#define RIMEWIN_SERVICE_UPDATE_CHECK_H_

#include <string>

#include "../common/net_ui.h"

namespace rimewin {

class NetGate;

struct UpdateCheckOutcome {
  UpdateCheckState state = UpdateCheckState::kNotWired;
  // kAvailable 時才有意義。⚠ 這是**版本字串**,不是網址 ——
  // 網址不進畫面,也不進連網紀錄(紀錄裡只有主機名)。
  std::string version;
  // 診斷用,**永遠英文**(§4.11)。不進畫面上的那句話。
  std::string detail;
};

// ⚠ **同步阻塞。必須在背景執行緒上呼叫。**
//   服務進程的 UI 執行緒同時在跑候選窗,卡住它就是
//   「打字打到一半整個沒反應」(見 service/net_gate.h 檔頭)。
//
// ⚠ gate 為 null 或開關是關的 → 回 kBlocked,而且**不送出任何東西**。
//   「不知道」必須等於「關」。
UpdateCheckOutcome RunUpdateCheck(NetGate* gate);

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_UPDATE_CHECK_H_
