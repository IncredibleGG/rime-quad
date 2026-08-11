// windows/common/shift_tap.cc — 純邏輯,不含任何平台 API。

#include "shift_tap.h"

namespace rimewin {
namespace {

// keymap.cc 沒有把這三個 VK 露出來(它只在自己的匿名命名空間裡用),
// 而這裡只需要這三個值。⚠ 兩邊都必須是這三個數字,tests 裡有斷言對帳。
constexpr uint32_t kVkShift = 0x10;   // 泛用:宿主實際送過來的就是它
constexpr uint32_t kVkLShift = 0xA0;
constexpr uint32_t kVkRShift = 0xA1;

// 「按下 Shift 的當下,有沒有別的修飾鍵按著」。
//
// ⚠ **不含 CapsLock,也不含 Shift 自己。** CapsLock 的理由見檔頭
//   (那個位元是「燈亮著」不是「鍵按著」);Shift 自己不算是因為
//   我們正在判斷的就是它。
//
// ⚠ AltGr 在 Windows 上會同時合成左 Ctrl + 右 Alt(見 keymap.cc),
//   所以 AltGr 按著時這裡是 true —— 那是對的:AltGr 按著時的 Shift
//   一定是某個組合的一部分。
bool BlockingMods(const KeyEvent& e) { return e.ctrl || e.alt || e.win; }

}  // namespace

bool IsShiftVk(uint32_t vk) {
  return vk == kVkShift || vk == kVkLShift || vk == kVkRShift;
}

void ShiftTapState::Reset() {
  phase_ = ShiftTapPhase::kIdle;
  armed_scan_ = 0;
  armed_time_ms_ = 0;
}

// 滑鼠 / 觸控 / 手寫筆動了宿主的文件。與「Shift 按住期間出現任何其他
// 按鍵事件」走同一格 —— 那一段就是不再是「什麼都沒發生」了。
//
// ⚠ 與下面 OnKey 裡「不是 Shift 的鍵」那一段**逐字相同**,而且必須相同。
//   兩者是同一條產品判準的兩半:一半看得到(按鍵),一半看不到(滑鼠)。
void ShiftTapState::OnOtherInput() {
  if (phase_ == ShiftTapPhase::kArmed) phase_ = ShiftTapPhase::kPoisoned;
}

ShiftTap ShiftTapState::OnKey(const KeyEvent& e, uint32_t time_ms) {
  // ── 不是 Shift 的鍵 ──────────────────────────────────────────
  //
  // 按下或放開都一樣,只要在 Shift 按住期間出現,這一段就作廢。
  //
  // ⚠ 為什麼**放開**也算:那是「Shift 按住之前就按著的鍵在中途放開了」,
  //   也就是這一段並不是「什麼都沒發生」。判準寫成「出現過任何其他按鍵
  //   事件」而不是「按下過任何其他鍵」,是因為前者一句話講得完,而且
  //   逐條測得出來。
  //
  // ⚠ 反過來,「Shift 按下**之前**就有別的鍵按著」我們**不追蹤**,
  //   於是「按著 A 不放、輕點一下 Shift、再放開 A」會切一次中英。
  //   這是**刻意的 fail-open**:要擋它就得跨事件數「現在有幾顆鍵按著」,
  //   而那個計數只要漏掉一顆 key-up(Alt+Tab 走掉時本來就會漏)就會
  //   **永遠**卡住,症狀是「這顆鍵有時候整個不會動」——
  //   而多切一次中英使用者按一下就回來了。這個專案的規矩是
  //   「壞掉的鍵比缺功能嚴重」,所以往可回復的那一邊倒。
  if (!IsShiftVk(e.vk)) {
    if (phase_ == ShiftTapPhase::kArmed) phase_ = ShiftTapPhase::kPoisoned;
    return ShiftTap::kNothing;
  }

  // ── Shift 按下 ───────────────────────────────────────────────
  if (!e.key_up) {
    switch (phase_) {
      case ShiftTapPhase::kArmed:
        // 已經有一顆 Shift 按著,又來一個 down。兩種可能,兩種都作廢:
        //   · 同一顆 → **自動重複**(使用者按著沒放);
        //   · 另一顆 → 兩顆 Shift 交錯。
        // 分不分得出來不重要,因為答案一樣。
        phase_ = ShiftTapPhase::kPoisoned;
        return ShiftTap::kNothing;
      case ShiftTapPhase::kPoisoned:
        // 已經作廢,維持作廢直到有一顆 Shift 放開(見下面)。
        return ShiftTap::kNothing;
      case ShiftTapPhase::kIdle:
        break;
    }
    if (BlockingMods(e)) {
      // Ctrl / Alt / Win 已經按著了 —— 這一下 Shift 是某個組合的一部分。
      //
      // ⚠ **這一行就是 #88。** macOS 那一份在這裡只是「擋掉不轉送」,
      //   沒有把「這一段作廢了」記下來,於是等 ⌘ 先放開之後,⇧ 的放開
      //   看起來乾乾淨淨。記成 kPoisoned 之後,那個 up 只會把狀態機
      //   收回 kIdle,一次都不會切。
      phase_ = ShiftTapPhase::kPoisoned;
      return ShiftTap::kNothing;
    }
    phase_ = ShiftTapPhase::kArmed;
    armed_scan_ = e.scan_code;
    armed_time_ms_ = time_ms;
    return ShiftTap::kNothing;
  }

  // ── Shift 放開 ───────────────────────────────────────────────
  const ShiftTapPhase was = phase_;
  const uint32_t armed_scan = armed_scan_;
  const uint32_t armed_time = armed_time_ms_;
  // 不管算不算數,一顆 Shift 放開就代表這一段結束了。
  Reset();

  if (was != ShiftTapPhase::kArmed) return ShiftTap::kNothing;

  // 放開的必須是**同一顆**。
  //
  // ⚠ 宿主給不出 scan code(兩邊都是 0)時,左右在這裡也是同一顆 ——
  //   那時「左按下 → 右按下」會走上面 kArmed 的自動重複那一格而作廢,
  //   仍然不會誤切。fail-safe 的方向是對的。
  if (e.scan_code != armed_scan) return ShiftTap::kNothing;

  // 放開的當下又有別的修飾鍵按著 = 這是一個組合鍵的一半。
  //
  // ⚠ 這一條在正常的事件流裡是多餘的(那顆 Ctrl 自己的 key-down 早就
  //   把這一段毒掉了)。留著是為了「宿主沒有把那顆 Ctrl 的 down 交給
  //   我們」的情況 —— 而我們對宿主到底交不交付哪些鍵的認識,上一輪
  //   才剛被自己的 CI 推翻過一次。
  if (BlockingMods(e)) return ShiftTap::kNothing;

  // 按住太久 = 使用者在按著它做別的事,不是輕點。
  // ⚠ 無號減法:time_ms 繞回 0 時差值會是一個很大的數,於是不算數 ——
  //   那是這裡想要的方向(寧可少切一次)。
  if (static_cast<uint32_t>(time_ms - armed_time) > kShiftTapMaxHoldMs)
    return ShiftTap::kNothing;

  return ShiftTap::kToggleAsciiMode;
}

}  // namespace rimewin
