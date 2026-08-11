// windows/common/bar_visibility.h — 懸浮狀態列什麼時候該在(§12.10.6)
//
// ── 使用者說了什麼 ──────────────────────────────────────────────
//
// 「怎麼就常駐了」。他**沒有**說「我不要這個東西」—— 那一橫是他自己
// 要求的(product-gaps.md §4.1:「小小一橫在右下角,可以拖動」)。
// 要拿掉的只有「他沒在用的時候還在」這一件事。
//
// ── 規範(§12.10.6,四端一致)────────────────────────────────────
//
// 可見性由三個輸入決定:
//
//   1. **使用者總開關**(appearance.floatingBar,預設開)。
//      關 → 永遠不顯示,沒有例外。
//   2. **這個輸入法目前有沒有被宿主使用。**
//      Windows:至少一條 TSF 連線活著;
//      macOS:至少一個 input client;
//      Android:IME 被系統選中且輸入視窗存在。
//   3. **有沒有輸入焦點。** 平台拿得到才用;**拿不到時一律視為「有」**
//      (fail-visible)。
//
//   顯示:2 成立,且(3 成立 或 3 不可知)→ **立刻**顯示。
//   隱藏:條件不再成立後,等 3000 毫秒才隱藏;期間恢復則取消待隱藏。
//   位置:重新出現時回到使用者拖過的同一個位置,不重新定位。
//
// ⚠ 自動隱藏**不是**關閉。它不改變總開關的值,而且條件恢復時它自己
//   回來。那一橫上仍然**不放 X**(§12.10.2 那一條不變)。
//
// ⚠ 每一端都必須有一個**與這一橫無關、而且在這個輸入法沒被使用時仍然
//   存在**的入口通往設定。Windows = 系統匣圖示 + 開始選單捷徑。
//   那一橫一旦會自己消失,托盤就從「備援入口」升格成「服務活著時唯一
//   必然存在的入口」。
//
// ── 為什麼判準是「有沒有人在用」而不是「有沒有在組字」──────────
//
// 跟著組字走的話,使用者想切中英的那一刻那一橫**不在**(他還沒切,
// 所以還沒觸發顯示),而且每打一段就閃一次。而中英切換正是這一橫
// 存在的理由。
//
// 跟著連線走則與「哪一個輸入框」無關:同一支程式裡跳輸入框、在都用
// LuminaKey 的程式之間 Alt+Tab,都不會觸發。3000 毫秒的遲滯壓得住
// 宿主進程開開關關造成的連線數震盪。
//
// ⚠ 用**連線生死**而不是只用焦點,還有一個硬理由:ipc_client.cc 的
//   `if (!MayEatKey()) return;` —— 焦點訊號在使用者打第一個字之前
//   根本不送。也就是說「還沒收到過焦點訊息」是開機後的常態,不是異常。
//   焦點只當加強條件,而且不可知時 fail-visible。
//
// ── 為什麼是純函式 ──────────────────────────────────────────────
//
// 顯示時機的缺陷**全部長成時序問題**,而時序問題沒有測試就只能靠人肉
// 試 —— 人肉試不出「Alt+Tab 到另一個也用 LuminaKey 的程式時會不會閃
// 一下」。service/ 在 Ubuntu 上編不起來,所以判準必須住在這裡。
//
#ifndef RIMEWIN_COMMON_BAR_VISIBILITY_H_
#define RIMEWIN_COMMON_BAR_VISIBILITY_H_

#include <cstdint>

namespace rimewin {

struct BarVisibilityInputs {
  // appearance.floatingBar != false。關掉 → 永遠 kHide。
  bool user_enabled = true;
  // 目前握著連線的宿主數。**不是**「有幾個輸入框」。
  int active_clients = 0;
  // 收到過任何焦點訊息沒有。為假時**不看** any_focused(fail-visible)。
  bool focus_known = false;
  bool any_focused = false;
  // ⚠ **單調**時鐘(Windows 上是 GetTickCount64)。牆上時鐘會因為對時
  //   或時區調整而跳,而跳一次的後果是那一橫忽然消失或再也藏不起來。
  uint64_t now_ms = 0;
};

enum class BarAction : uint8_t {
  kShow,     // 現在就要顯示
  kHide,     // 現在就要隱藏
  kPending,  // 條件已經不成立,但遲滯還沒到期 —— 維持現狀,繼續問
};

// 條件不再成立之後等多久才真的藏起來。
uint32_t BarHideDelayMs();

class BarVisibility {
 public:
  // 每次狀態可能變了就餵一次(Windows 端是 500 毫秒的 kStateTimer,
  // 加上連線建立/結束與焦點訊息)。
  BarAction Feed(const BarVisibilityInputs& in);

  // 回到初始狀態:**已經藏起來了**,而不是「正在等著藏」。
  void Reset();

 private:
  // 上一次算出來的意圖。初始是「藏著」—— 服務剛起來時本來就沒有人在用,
  // 那一橫不該先出現三秒再消失,那是一次沒有人要求過的閃爍。
  bool shown_ = false;
  bool pending_ = false;
  uint64_t pending_since_ms_ = 0;
};

}  // namespace rimewin

#endif  // RIMEWIN_COMMON_BAR_VISIBILITY_H_
