#include "bar_visibility.h"

namespace rimewin {
namespace {

// 3000 毫秒。挑這個數字的理由不是手感,是它要壓住的東西:宿主進程
// 開開關關造成的連線數震盪(關掉一個分頁、開一個新視窗),以及
// TSF 在切換輸入焦點時短暫的 0 連線空窗。太短會閃,太長則使用者切到
// 別的輸入法之後那一橫還賴在畫面上 —— 而那正是他抱怨的事。
constexpr uint32_t kHideDelayMs = 3000;

}  // namespace

uint32_t BarHideDelayMs() { return kHideDelayMs; }

void BarVisibility::Reset() {
  shown_ = false;
  pending_ = false;
  pending_since_ms_ = 0;
}

BarAction BarVisibility::Feed(const BarVisibilityInputs& in) {
  // 1. 總開關。關掉 → 永遠不顯示,**而且不留下待隱藏狀態** ——
  //    使用者把它打開的下一刻要立刻出現,不是「再等三秒」。
  if (!in.user_enabled) {
    shown_ = false;
    pending_ = false;
    return BarAction::kHide;
  }

  // 2. 有沒有人在用 + 3. 有沒有焦點(不可知一律視為「有」)。
  //
  // ⚠ focus_known 為假時**不看** any_focused。焦點訊號在使用者打第一個
  //   字之前根本不送(ipc_client.cc 的 MayEatKey 閘),所以「還沒收到過」
  //   是開機後的常態。在那裡藏起來,那一橫就永遠等不到第一個字。
  const bool in_use = in.active_clients > 0;
  const bool focused = !in.focus_known || in.any_focused;
  const bool wanted = in_use && focused;

  if (wanted) {
    // 立刻顯示,而且取消待隱藏 —— 是**取消**,不是延後。
    shown_ = true;
    pending_ = false;
    return BarAction::kShow;
  }

  // 已經藏起來了就不必再等三秒(服務剛啟動時就是這一支)。
  if (!shown_) {
    pending_ = false;
    return BarAction::kHide;
  }

  if (!pending_) {
    pending_ = true;
    pending_since_ms_ = in.now_ms;
    return BarAction::kPending;
  }

  // ⚠ 時鐘倒退時重新起算,不是永遠卡在 pending。呼叫端餵的是單調時鐘,
  //   但一個卡住的狀態機會讓那一橫再也藏不起來,而沒有人查得出來為什麼。
  if (in.now_ms < pending_since_ms_) {
    pending_since_ms_ = in.now_ms;
    return BarAction::kPending;
  }

  // 邊界用 >=:3000 就是到期,不是 3001。
  if (in.now_ms - pending_since_ms_ >= kHideDelayMs) {
    shown_ = false;
    pending_ = false;
    return BarAction::kHide;
  }
  return BarAction::kPending;
}

}  // namespace rimewin
