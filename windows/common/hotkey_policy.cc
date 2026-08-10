#include "hotkey_policy.h"

#include "keymap.h"

namespace rimewin {
namespace {

// 空白鍵的 keysym。keymap.h 檔頭:0x20–0x7E 的 keysym 就是碼點本身。
constexpr int32_t kSpace = 0x20;

// 判斷時要看的修飾鍵。
// ⚠ CapsLock **不看**:它是一個持續的狀態,不是使用者為了這顆熱鍵按的,
//   而「開著大寫鎖定時 Ctrl+空白鍵失效」是一個沒有人猜得到的規則。
constexpr uint32_t kMask = kModShift | kModControl | kModAlt | kModSuper;

}  // namespace

int32_t AsciiToggleKeysym() { return kSpace; }
uint32_t AsciiToggleModifiers() { return kModControl; }

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

  return Hotkey::kNone;
}

}  // namespace rimewin
