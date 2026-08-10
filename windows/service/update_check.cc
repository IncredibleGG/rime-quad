#include "update_check.h"

#include "net_gate.h"

namespace rimewin {

// ⚠ **這一份是接縫的暫代實作,整支函式由 win-update 那條線換掉。**
//   它一個位元組都不送 —— 見標頭的說明。
//
//   為什麼不是「先不要放這顆按鈕」:那顆按鈕與它旁邊的開關是同一件事的
//   兩半。使用者的要求是「做 windows 的線上更新,然後跟安卓一樣有開關」,
//   而開關的意義正是「按下需要連網的東西時會發生什麼」。少了那顆按鈕,
//   開關旁邊那句話就沒有東西可以指。
//
//   為什麼不是「按下去假裝檢查完說已經最新」:那是宣稱一件沒有發生的事。
UpdateCheckOutcome RunUpdateCheck(NetGate* gate) {
  UpdateCheckOutcome out;
  // ⚠ 再問一次開關。呼叫端(DecideUpdateAction)已經問過,這裡仍然問 ——
  //   兩次之間隔著一次執行緒建立,使用者可以在那個縫裡把開關關掉。
  //   而且 gate 是 null 時也走這一條:「不知道」必須等於「關」。
  if (gate == nullptr || !gate->Enabled()) {
    out.state = UpdateCheckState::kBlocked;
    out.detail = "network switch is off";
    return out;
  }
  out.state = UpdateCheckState::kNotWired;
  out.detail = "no update source is wired up in this build";
  return out;
}

}  // namespace rimewin
