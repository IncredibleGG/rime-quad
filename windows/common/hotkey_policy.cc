#include "hotkey_policy.h"

#include "keymap.h"

namespace rimewin {
namespace {

// 空白鍵的 keysym。keymap.h 檔頭:0x20–0x7E 的 keysym 就是碼點本身。
constexpr int32_t kSpace = 0x20;

// 左 Shift 的 keysym(X11 的 XK_Shift_L)。見 keymap.cc 的同一個常數。
constexpr int32_t kShiftL = 0xFFE1;

// 判斷時要看的修飾鍵。
// ⚠ CapsLock **不看**:它是一個持續的狀態,不是使用者為了這顆熱鍵按的,
//   而「開著大寫鎖定時 Ctrl+空白鍵失效」是一個沒有人猜得到的規則。
constexpr uint32_t kMask = kModShift | kModControl | kModAlt | kModSuper;

}  // namespace

int32_t AsciiToggleKeysym() { return kSpace; }
uint32_t AsciiToggleModifiers() { return kModControl; }

int32_t ShiftTapKeysym() { return kShiftL; }
uint32_t ShiftTapModifiers() { return 0; }

Hotkey ClassifyHotkey(int32_t keysym, uint32_t modifiers) {
  // 放開事件不算。⚠ 少了這一條,按一次會切兩次 —— 而「切了兩次」
  //   在使用者那裡看起來就是「按了完全沒反應」,最難查的那一種。
  if (modifiers & kModRelease) return Hotkey::kNone;

  // ⚠ 用**等於**,不是「有沒有含 Ctrl」。Ctrl+Shift+空白鍵在很多程式裡
  //   是不斷行空白;Ctrl+Alt+空白鍵在部分佈局上是 AltGr+空白。
  //   把它們一起吃掉,就是這個專案在 key_eat_policy.h 記著的那個錯誤
  //   換一個地方再犯一次。
  if (keysym == kSpace && (modifiers & kMask) == kModControl)
    return Hotkey::kToggleAsciiMode;

  // 輕點 Shift 的暗號:一顆**裸的**左 Shift。
  //
  // ⚠ 同樣用等於,不是「有沒有含」:帶著任何修飾鍵的 Shift_L 都不是它。
  //   而 CapsLock 一樣不看(kMask 裡本來就沒有它)—— 開著大寫鎖定的人
  //   輕點 Shift 必須照樣切,理由見 common/shift_tap.h 檔頭。
  if (keysym == kShiftL && (modifiers & kMask) == 0)
    return Hotkey::kToggleAsciiModeShiftTap;

  return Hotkey::kNone;
}

KeyAction DecideKeyAction(int32_t keysym, uint32_t modifiers,
                          bool shift_tap_enabled) {
  switch (ClassifyHotkey(keysym, modifiers)) {
    case Hotkey::kToggleAsciiMode:
      return KeyAction::kToggleAsciiMode;
    case Hotkey::kToggleAsciiModeShiftTap:
      // ⚠ 關掉時是 kIgnore,不是 kEngine。理由見標頭 KeyAction 的說明:
      //   librime 自己也認得 Shift_L。
      return shift_tap_enabled ? KeyAction::kToggleAsciiMode
                               : KeyAction::kIgnore;
    case Hotkey::kNone:
      break;
  }
  return KeyAction::kEngine;
}

}  // namespace rimewin
