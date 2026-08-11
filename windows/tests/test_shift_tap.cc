// windows/tests/test_shift_tap.cc — 輕點 Shift 切中英的逐事件真值表
//
// 這一支的重點**不是**「輕點會切」那三行,是下面那一整批**不該切**的:
// 這顆鍵答錯的樣子是「打大寫字母時中英一直亂跳」,而那比沒有這個功能
// 糟得多(common/shift_tap.h 檔頭)。所以每一條產品判準都在這裡有一格。
//
// ⚠ 每一組都從一個**全新的**狀態機開始。共用一個實例的話,上一組留下來的
//   phase 會替下一組回答問題,而那正是這種跨事件狀態機最容易錯的地方。

#include "shift_tap.h"

#include "check.h"
#include "hotkey_policy.h"
#include "keymap.h"
#include "settings.h"

using namespace rimewin;

namespace {

constexpr uint32_t kVkShift = 0x10;
constexpr uint32_t kVkA = 0x41;
constexpr uint32_t kVkCtrl = 0x11;
constexpr uint32_t kVkLeft = 0x25;

// 一顆按鍵事件。mods 是「這一刻**還有哪些修飾鍵按著**」——
// TSF 那一側是 GetKeyboardState 給的,見 tsf/text_service.cc 的 BuildKeyEvent。
KeyEvent Ev(uint32_t vk, uint32_t scan, bool up, bool ctrl = false,
            bool alt = false, bool win = false, bool caps = false) {
  KeyEvent e;
  e.vk = vk;
  e.scan_code = scan;
  e.key_up = up;
  e.ctrl = ctrl;
  e.alt = alt;
  e.win = win;
  e.caps_lock = caps;
  return e;
}

KeyEvent ShiftDown(uint32_t scan) { return Ev(kVkShift, scan, false); }
KeyEvent ShiftUp(uint32_t scan) { return Ev(kVkShift, scan, true); }

bool Toggles(ShiftTap r) { return r == ShiftTap::kToggleAsciiMode; }

}  // namespace

// ══════════════════ 該切換 ══════════════════════════════════════════

// 1
TEST(shift_tap_bare_press_and_release_toggles) {
  ShiftTapState st;
  CHECK(!Toggles(st.OnKey(ShiftDown(kScanShiftL), 1000)));
  CHECK(Toggles(st.OnKey(ShiftUp(kScanShiftL), 1050)));
  // 切完之後回到什麼都沒追蹤 —— 不然下一顆鍵會踩到殘留狀態。
  CHECK(st.phase() == ShiftTapPhase::kIdle);
}

// 2
TEST(shift_tap_left_shift_toggles) {
  ShiftTapState st;
  st.OnKey(ShiftDown(kScanShiftL), 0);
  CHECK_INT(st.armed_scan(), kScanShiftL);
  CHECK(Toggles(st.OnKey(ShiftUp(kScanShiftL), 40)));
}

// 3
TEST(shift_tap_right_shift_toggles) {
  ShiftTapState st;
  // ⚠ 右 Shift 送過來的 wParam 仍然是泛用的 VK_SHIFT(0x10),而
  //   keymap.cc:157 會把它折成 XK_Shift_L —— 分得出左右的只有 scan code。
  st.OnKey(ShiftDown(kScanShiftR), 0);
  CHECK_INT(st.armed_scan(), kScanShiftR);
  CHECK(Toggles(st.OnKey(ShiftUp(kScanShiftR), 40)));
}

// 4
TEST(shift_tap_holding_300ms_still_toggles) {
  // 300ms 在上限(kShiftTapMaxHoldMs = 500,見那裡的根據)以內。
  // 打字時的一次「按一下」常常就是兩三百毫秒,壓太緊會變成
  // 「有時候有反應有時候沒有」—— 最難回報的那一種。
  ShiftTapState st;
  st.OnKey(ShiftDown(kScanShiftL), 10000);
  CHECK(Toggles(st.OnKey(ShiftUp(kScanShiftL), 10300)));
}

// 13(該切:連按兩次要切兩次)
TEST(shift_tap_two_taps_toggle_twice) {
  ShiftTapState st;
  st.OnKey(ShiftDown(kScanShiftL), 0);
  CHECK(Toggles(st.OnKey(ShiftUp(kScanShiftL), 30)));
  st.OnKey(ShiftDown(kScanShiftL), 200);
  CHECK(Toggles(st.OnKey(ShiftUp(kScanShiftL), 230)));
}

// ══════════════════ 不該切換 ════════════════════════════════════════

// 5
TEST(shift_tap_shift_plus_a_does_not_toggle) {
  // 打一個大寫 A。這是這顆鍵最常被誤觸的一格,也是「猜錯的後果比沒有
  // 這個功能糟得多」那句話講的那一格。
  ShiftTapState st;
  st.OnKey(ShiftDown(kScanShiftL), 0);
  st.OnKey(Ev(kVkA, 0x1E, false), 20);
  st.OnKey(Ev(kVkA, 0x1E, true), 60);
  CHECK(!Toggles(st.OnKey(ShiftUp(kScanShiftL), 80)));
}

// 6
TEST(shift_tap_other_key_still_down_when_shift_released) {
  // A 在 Shift 按住時按下,而且 Shift 比 A 先放開(打字打得快時的常態)。
  ShiftTapState st;
  st.OnKey(ShiftDown(kScanShiftL), 0);
  st.OnKey(Ev(kVkA, 0x1E, false), 20);
  CHECK(!Toggles(st.OnKey(ShiftUp(kScanShiftL), 40)));
  // A 的 up 落在 Shift 之後 —— 它不可以「補」一次切換出來。
  CHECK(!Toggles(st.OnKey(Ev(kVkA, 0x1E, true), 60)));
}

// 5b:方向鍵(選取文字)。與字母走同一條規則,但它是使用者最常按住
//     Shift 不放的情境,值得自己一格。
TEST(shift_tap_shift_plus_arrow_does_not_toggle) {
  ShiftTapState st;
  st.OnKey(ShiftDown(kScanShiftL), 0);
  st.OnKey(Ev(kVkLeft, 0x4B, false), 20);
  st.OnKey(Ev(kVkLeft, 0x4B, false), 50);  // 自動重複的方向鍵
  st.OnKey(Ev(kVkLeft, 0x4B, true), 90);
  CHECK(!Toggles(st.OnKey(ShiftUp(kScanShiftL), 120)));
}

// 7
TEST(shift_tap_holding_ten_seconds_does_not_toggle) {
  ShiftTapState st;
  st.OnKey(ShiftDown(kScanShiftL), 1000);
  CHECK(!Toggles(st.OnKey(ShiftUp(kScanShiftL), 11000)));
  // 上限的邊界:剛好在上限上算數,多 1ms 就不算。
  ShiftTapState a;
  a.OnKey(ShiftDown(kScanShiftL), 0);
  CHECK(Toggles(a.OnKey(ShiftUp(kScanShiftL), kShiftTapMaxHoldMs)));
  ShiftTapState b;
  b.OnKey(ShiftDown(kScanShiftL), 0);
  CHECK(!Toggles(b.OnKey(ShiftUp(kScanShiftL), kShiftTapMaxHoldMs + 1)));
}

// 8
TEST(shift_tap_interleaved_left_and_right_does_not_toggle) {
  // 左按下 → 右按下 → 左放開 → 右放開。一次都不可以切。
  ShiftTapState st;
  st.OnKey(ShiftDown(kScanShiftL), 0);
  st.OnKey(ShiftDown(kScanShiftR), 20);
  CHECK(st.phase() == ShiftTapPhase::kPoisoned);
  CHECK(!Toggles(st.OnKey(ShiftUp(kScanShiftL), 40)));
  CHECK(!Toggles(st.OnKey(ShiftUp(kScanShiftR), 60)));
}

// 9
TEST(shift_tap_ctrl_pressed_after_shift_does_not_toggle) {
  ShiftTapState st;
  st.OnKey(ShiftDown(kScanShiftL), 0);
  // Ctrl 的 key-down 自己就是「其他鍵」。
  st.OnKey(Ev(kVkCtrl, 0x1D, false), 100);
  CHECK(st.phase() == ShiftTapPhase::kPoisoned);
  CHECK(!Toggles(st.OnKey(Ev(kVkShift, kScanShiftL, true, /*ctrl=*/true), 140)));
}

// 9b:**#88 的那條迴歸**。macOS 的 ModifierGate 在這一格會誤切。
TEST(shift_tap_modifier_held_before_shift_does_not_toggle) {
  // ⌘/Ctrl 先按 → ⇧ 按下(當下有 Ctrl)→ Ctrl 放開 → ⇧ 放開。
  // 最後那一下的 flags 已經空了 —— 逐事件無狀態的判斷會在這裡誤切,
  // 而這正是 apple/.../Fix4MacModTests.swift 自己演出來的那個洞(工單 #88)。
  ShiftTapState st;
  st.OnKey(Ev(kVkCtrl, 0x1D, false), 0);
  st.OnKey(Ev(kVkShift, kScanShiftL, false, /*ctrl=*/true), 20);
  CHECK(st.phase() == ShiftTapPhase::kPoisoned);
  st.OnKey(Ev(kVkCtrl, 0x1D, true, /*ctrl=*/false), 40);
  // 這一顆的 ctrl 是 false —— 只看當下的話它是一次乾淨的放開。
  CHECK(!Toggles(st.OnKey(Ev(kVkShift, kScanShiftL, true), 60)));
}

// 10
TEST(shift_tap_focus_change_while_held_does_not_toggle) {
  ShiftTapState st;
  st.OnKey(ShiftDown(kScanShiftL), 0);
  st.Reset();  // Deactivate / OnSetFocus 會呼叫這個
  CHECK(st.phase() == ShiftTapPhase::kIdle);
  CHECK(!Toggles(st.OnKey(ShiftUp(kScanShiftL), 40)));
}

// 11
TEST(shift_tap_key_after_the_release_does_not_add_a_toggle) {
  ShiftTapState st;
  st.OnKey(ShiftDown(kScanShiftL), 0);
  CHECK(Toggles(st.OnKey(ShiftUp(kScanShiftL), 30)));
  // 放開之後的每一顆鍵都不可以再切一次。
  CHECK(!Toggles(st.OnKey(Ev(kVkA, 0x1E, false), 50)));
  CHECK(!Toggles(st.OnKey(Ev(kVkA, 0x1E, true), 70)));
  CHECK(!Toggles(st.OnKey(ShiftUp(kScanShiftL), 90)));  // 多出來的一顆 up
}

// 12
TEST(shift_tap_auto_repeat_does_not_toggle) {
  // 按著 Shift 不放:Windows 會一直送 key-down,沒有 key-up。
  //
  // ⚠ 這一組就是 kPoisoned 存在的理由。作廢之後如果回到 kIdle,
  //   第三個 down 會**重新開始**,然後那個 up 就切了。
  ShiftTapState st;
  st.OnKey(ShiftDown(kScanShiftL), 0);
  for (uint32_t t = 30; t <= 300; t += 30) {
    CHECK(!Toggles(st.OnKey(ShiftDown(kScanShiftL), t)));
    CHECK(st.phase() == ShiftTapPhase::kPoisoned);
  }
  CHECK(!Toggles(st.OnKey(ShiftUp(kScanShiftL), 330)));
}

// ══════════════════ 邊界與對帳 ══════════════════════════════════════

TEST(shift_tap_caps_lock_does_not_block_it) {
  // ⚠ 開著大寫鎖定的人輕點 Shift **必須**照樣切。
  //   CapsLock 那個位元是「燈亮著」不是「鍵按著」——
  //   算進擋鍵裡的話,那些人會覺得這顆鍵整個不存在,而且永遠查不到原因。
  ShiftTapState st;
  st.OnKey(Ev(kVkShift, kScanShiftL, false, false, false, false, /*caps=*/true),
           0);
  CHECK(Toggles(st.OnKey(
      Ev(kVkShift, kScanShiftL, true, false, false, false, /*caps=*/true), 40)));
}

TEST(shift_tap_only_shift_vks_are_shift) {
  // 掃全部 256 個 VK:只有這三個算 Shift。多認一個就是「別的鍵也會切中英」。
  int shifts = 0;
  for (uint32_t vk = 0; vk < 256; ++vk) {
    const bool is = IsShiftVk(vk);
    if (is) ++shifts;
    CHECK(is == (vk == 0x10 || vk == 0xA0 || vk == 0xA1));
  }
  CHECK_INT(shifts, 3);
}

TEST(shift_tap_tick_count_wraparound_does_not_toggle) {
  // GetTickCount 每 49.7 天繞回 0。繞回的那一刻寧可少切一次,
  // 也不可以因為算出一個巨大的負值而變成「永遠算數」。
  ShiftTapState st;
  st.OnKey(ShiftDown(kScanShiftL), 0xFFFFFF00u);
  // 距離繞回還有 0x100,放開時已經繞過去 0x64 —— 相隔 0x164 = 356,
  // 仍然在上限內 → 該切。無號減法算出來就是它;有號的話會是一個負數。
  CHECK(Toggles(st.OnKey(ShiftUp(kScanShiftL), 0x00000064u)));
  ShiftTapState b;
  b.OnKey(ShiftDown(kScanShiftL), 0x00000100u);
  // 時間倒退(不該發生,但宿主給的數字不是我們控制的)→ 差值巨大 → 不切。
  CHECK(!Toggles(b.OnKey(ShiftUp(kScanShiftL), 0x00000000u)));
}

// 14:開關關掉時一次都不切。
//
// ⚠ 判斷是誰做的很重要:狀態機**照樣跑**(它在瘦 DLL 裡,不知道設定),
//   決定權在服務端那一格 —— common/hotkey_policy.cc 的 DecideKeyAction。
//   關掉時它必須回 kIgnore,**不是** kEngine:一顆裸的 XK_Shift_L 交給
//   librime 會去動 ascii_composer 的內部狀態,而那不是我們要的。
TEST(shift_tap_switch_off_never_toggles) {
  const int32_t k = ShiftTapKeysym();
  const uint32_t m = ShiftTapModifiers();

  CHECK(DecideKeyAction(k, m, /*shift_tap_enabled=*/true) ==
        KeyAction::kToggleAsciiMode);
  CHECK(DecideKeyAction(k, m, /*shift_tap_enabled=*/false) ==
        KeyAction::kIgnore);

  // Ctrl+空白鍵**不受這個開關影響** —— 兩顆鍵是兩件事,
  // 而「關掉輕點 Shift 順便把 Ctrl+空白鍵也關掉了」是使用者猜不到的規則。
  CHECK(DecideKeyAction(AsciiToggleKeysym(), AsciiToggleModifiers(), false) ==
        KeyAction::kToggleAsciiMode);

  // 別的鍵一律交給引擎,開關開著關著都一樣。
  CHECK(DecideKeyAction('a', 0, true) == KeyAction::kEngine);
  CHECK(DecideKeyAction('a', 0, false) == KeyAction::kEngine);

  // 設定那一側:**未設 == 開**(微軟拼音預設就是這顆鍵)。
  Settings s;
  CHECK(s.ShiftTapToggle());
  s.SetTri(keys::kTextShiftTapToggle, Tri::kFalse);
  CHECK(!s.ShiftTapToggle());
  s.SetTri(keys::kTextShiftTapToggle, Tri::kUnset);
  CHECK(s.ShiftTapToggle());
}

// 正規形式對帳:瘦 DLL 送的那一組必須就是服務端認得的那一組。
// ⚠ 兩邊對不上的症狀是「按下去完全沒有反應」,而且沒有任何記錄看得出來。
TEST(shift_tap_wire_form_round_trips) {
  CHECK_INT(ShiftTapKeysym(), 0xFFE1);  // XK_Shift_L,見 keymap.cc:157
  CHECK_INT(ShiftTapModifiers(), 0u);
  CHECK(ClassifyHotkey(ShiftTapKeysym(), ShiftTapModifiers()) ==
        Hotkey::kToggleAsciiModeShiftTap);
  // ⚠ 它**不可以**被 IsAsciiToggleHotkey 認成 Ctrl+空白鍵:那樣開關就
  //   關不掉了(兩顆鍵會走同一格)。
  CHECK(!IsAsciiToggleHotkey(ShiftTapKeysym(), ShiftTapModifiers()));
  // 帶任何修飾鍵的 Shift_L 都不算 —— 那是別的東西。
  CHECK(ClassifyHotkey(ShiftTapKeysym(), kModControl) == Hotkey::kNone);
  CHECK(ClassifyHotkey(ShiftTapKeysym(), kModAlt) == Hotkey::kNone);
  CHECK(ClassifyHotkey(ShiftTapKeysym(), kModShift) == Hotkey::kNone);
  CHECK(ClassifyHotkey(ShiftTapKeysym(), kModRelease) == Hotkey::kNone);
  // 右 Shift 的 keysym 沒有被指派意義(左右在 keysym 層分不出來,
  // 我們一律送左的那一個)。
  CHECK(ClassifyHotkey(0xFFE2, 0) == Hotkey::kNone);
}
