// windows/common/shift_tap.h — 「這一串按鍵算不算輕點了一下 Shift」
//
// ══ 使用者要的是什麼 ═══════════════════════════════════════════════
//
// 微軟拼音、搜狗、macOS 內建注音的預設都是「按一下 Shift 切中英」。
// 使用者實機回報過 Windows 這一側切不動,而 docs/product-gaps.md 把它
// 排在 P1(工單 #89)。
//
// ══ 為什麼可以做了 ═════════════════════════════════════════════════
//
// 這件事被一句假話擋了很久:「TSF 不會把純修飾鍵交給 key event sink,
// 所以要另外掛低階鍵盤 hook」。上一輪在真的 TSF 上量掉了它 ——
// CI run 31511075812(sha ca97498)logic-x64「真的經過 TSF」那一步:
//
//     SHIFT_SCAN_SENT=0x2A  SHIFT_TRACE_LINES=1
//     按鍵 vk=0x10 scan=0x2A keysym=0xFFE1 mods=0x0 族=host-only 吃掉=0
//
// 也就是**收得到**,而且 scan code 有正確帶進來。既然收得到,整件事就
// 可以在四支 sink 裡用一個純函式的狀態機做完,WH_KEYBOARD_LL 那條紅線
// 一步都不用碰(理由見 common/hotkey_policy.h 與 service/status_bar.h)。
//
// ⚠ 仍然沒有量到的:真實宿主(記事本 / Chrome / Word)的訊息迴圈會不會
//   把 VK_SHIFT 送進 TSF。上一輪那支 harness 走的是 ITfKeystrokeMgr::KeyDown
//   ——**那是宿主呼叫的入口**,它證明 sink 收得到,證不到宿主會送。
//   那一格在 #48(只有真人驗得到)。這個檔案本身不依賴那一格:它不對就是
//   「這顆鍵沒有反應」,不會弄壞別的東西。
//
// ══ 判準:一次切換是什麼 ═══════════════════════════════════════════
//
// **按下 Shift → 放開 Shift,而且中間什麼都沒發生。** 下面每一條都不算:
//
//   · Shift 按住時按了任何其他鍵(Shift+A、Shift+方向鍵…)
//   · 按下 Shift 的當下 Ctrl / Alt / Win 已經按著了(Ctrl+Shift…)
//   · 兩顆 Shift 交錯(左按下 → 右按下 → 左放開)
//   · 按住超過 kShiftTapMaxHoldMs
//   · 自動重複(同一顆 Shift 連續多次 down 沒有 up)
//   · 中途失焦 / Deactivate(呼叫端要呼叫 Reset())
//
// ══ ⚠ 為什麼是跨事件的狀態機,不是逐事件的判斷 ═══════════════════
//
// 四端共用的模型在 apple/LuminaKey/.../InputModeSwitch.swift 的 `ModifierGate`,
// 但**這一支不可以照抄它**。那一份是逐事件、無狀態的:每次事件看一眼當下
// 的 flags 決定擋不擋。它自己的測試
// `Fix4MacModTests.testUnforwardedModifiersStillUpdateTheTrackerState`
// 就把洞演出來了 ——
//
//     ⌘按下 → ⇧按下(當下有 ⌘,擋掉)→ ⌘放開 → ⇧放開(flags 已經空了)
//
// 最後那一下看起來是一次乾淨的放開,於是誤切。那是待辦 #88。
//
// 這裡的作法是:**按下的那一刻決定這一段算不算數,並且把答案記著**。
// 記著的那個位元(kPoisoned)就是 #88 缺的東西。macOS 要修 #88 可以直接
// 抄這張表 —— 它是純函式,沒有一行與 Windows 綁定。
//
// ══ ⚠ 刻意不看 CapsLock ════════════════════════════════════════════
//
// 與 macOS 的 `blockingFlags` 同一條理由,而且那一條是踩出來的:CapsLock
// 那個位元代表「燈亮著」,不是「鍵按著」。把它算進擋鍵裡,開著大寫鎖定的
// 人輕點 Shift 會**完全沒有反應**,而他永遠不會把兩件事聯想在一起。
//
// ══ 這個檔案為什麼是純邏輯 ═════════════════════════════════════════
//
// 不 include windows.h,所以 windows/run_logic_tests.sh 在 Ubuntu 上就跑得到
// (tests/test_shift_tap.cc 是一張逐事件的真值表)。整個判斷放在這裡而不是
// tsf/text_service.cc 的理由,與 key_eat_policy.cc 同一條:留在那個檔案裡的話
// **只有真人在 Windows 上才驗得到,也就是實際上沒有人驗** —— 而這顆鍵答錯的
// 樣子是「打大寫字母時中英一直亂跳」,比沒有這個功能糟得多。
//
#ifndef RIMEWIN_COMMON_SHIFT_TAP_H_
#define RIMEWIN_COMMON_SHIFT_TAP_H_

#include <cstdint>

#include "keymap.h"

namespace rimewin {

// 左右 Shift 的 scan code(set 1)。
//
// ⚠ **只有 scan code 分得出左右。** 宿主送過來的 wParam 是泛用的
//   VK_SHIFT(0x10),而 keymap.cc:157 又把泛用的那顆一律折成
//   XK_Shift_L(0xFFE1)—— 到了 keysym 那一層左右已經是同一顆了。
//   上一輪的實測(見檔頭)確認 scan code 有正確帶進來。
inline constexpr uint32_t kScanShiftL = 0x2A;
inline constexpr uint32_t kScanShiftR = 0x36;

// 按住超過這麼久就不算「輕點」。
//
// ── 這個數字的根據 ─────────────────────────────────────────────
//
// Windows 自動重複的**起始延遲**(SPI_GETKEYBOARDDELAY)預設是設定值 1,
// 也就是約 500 毫秒。超過它,Windows 自己就開始把這顆鍵當成「按住」而不是
// 「按一下」—— 修飾鍵同樣會重複。所以 500 不是憑手感挑的:它是這台機器上
// 「按住」的官方定義,而我們只是照它。
//
// ⚠ 使用者可以把那個延遲調到 250–1000ms,而我們**刻意不去問它**:
//   問了的話這條規則會每台機器不一樣,也就不可能有一張測得到的真值表。
//   真的把延遲調長的人由另一條擋著 —— 自動重複本身就是獨立的作廢條件
//   (同一顆 Shift 連續兩次 down)。
inline constexpr uint32_t kShiftTapMaxHoldMs = 500;

enum class ShiftTap {
  // 什麼都沒發生。**絕大多數事件都落在這裡。**
  kNothing,
  // 這一串構成一次「輕點 Shift」= 一次中英切換。
  kToggleAsciiMode,
};

// 狀態機的三個處境。露出來是為了診斷與測試 —— 呼叫端只該看 OnKey 的回傳值。
enum class ShiftTapPhase {
  // 沒有在追蹤任何東西。
  kIdle,
  // 有一顆 Shift 按著,而且到目前為止是乾淨的。
  kArmed,
  // 有一顆 Shift 按著,但這一段**已經作廢**了。
  //
  // ⚠ 這個處境不能省成「回到 kIdle」。省掉的話,自動重複
  //   (down down down up)會在第二個 down 作廢、第三個 down **重新開始**,
  //   然後那個 up 就切了 —— 而自動重複正是「使用者按著沒放」最常見的樣子。
  kPoisoned,
};

// 判斷「這一顆是不是 Shift」。
//
// vk 決定是不是 Shift(泛用 VK_SHIFT 與明確的左右都算),
// scan 決定是**哪一顆**(見 kScanShiftL / kScanShiftR)。
bool IsShiftVk(uint32_t vk);

class ShiftTapState {
 public:
  // 餵一顆按鍵事件進來。
  //
  // e 用的是 keymap.h 既有的 KeyEvent(TSF 那一側本來就在組它,
  // 見 tsf/text_service.cc 的 BuildKeyEvent)—— 再定義一份自己的事件型別
  // 就是兩份「一顆按鍵長什麼樣」,而漂移的地方會是 scan code 或 key_up。
  //
  // time_ms:單調遞增的毫秒數(TSF 那側用 GetTickCount)。
  // ⚠ 它會在 49.7 天後繞回 0,所以內部一律用無號減法算差值 ——
  //   繞回的那一刻最壞的結果是那一次輕點不算數,不會變成負的巨大值。
  ShiftTap OnKey(const KeyEvent& e, uint32_t time_ms);

  // 把狀態機歸零。
  //
  // ⚠ 呼叫端**必須**在失焦與 Deactivate 時呼叫。少了它,「按著 Shift 切到
  //   別的視窗、在那邊放開」會被算成一次乾淨的輕點 —— 而那一顆 up 我們
  //   根本沒有看到,是下一次的 up 被誤算。
  void Reset();

  ShiftTapPhase phase() const { return phase_; }
  // 目前追蹤的是哪一顆(kScanShiftL / kScanShiftR)。kIdle 時沒有意義。
  uint32_t armed_scan() const { return armed_scan_; }

 private:
  ShiftTapPhase phase_ = ShiftTapPhase::kIdle;
  uint32_t armed_scan_ = 0;
  uint32_t armed_time_ms_ = 0;
};

}  // namespace rimewin

#endif  // RIMEWIN_COMMON_SHIFT_TAP_H_
