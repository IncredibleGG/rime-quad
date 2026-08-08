// windows/common/keymap.cc — VK_* + 鍵盤狀態 → X11 keysym(純邏輯)
//
// 不 include windows.h。需要的 VK_* 常數在下面自己定義,值抄自 WinUser.h。
// 這樣做的唯一理由是**可測試性**:本檔連同 tests/test_keymap.cc 可以在
// 開發用的 Ubuntu 上編譯並執行,不必等 CI 的 Windows runner。

#include "keymap.h"

namespace rimewin {
namespace {

// ── VK_*(WinUser.h)──────────────────────────────────────────────
constexpr uint32_t VK_BACK_ = 0x08, VK_TAB_ = 0x09, VK_CLEAR_ = 0x0C,
                   VK_RETURN_ = 0x0D, VK_SHIFT_ = 0x10, VK_CONTROL_ = 0x11,
                   VK_MENU_ = 0x12, VK_PAUSE_ = 0x13, VK_CAPITAL_ = 0x14,
                   VK_ESCAPE_ = 0x1B, VK_SPACE_ = 0x20, VK_PRIOR_ = 0x21,
                   VK_NEXT_ = 0x22, VK_END_ = 0x23, VK_HOME_ = 0x24,
                   VK_LEFT_ = 0x25, VK_UP_ = 0x26, VK_RIGHT_ = 0x27,
                   VK_DOWN_ = 0x28, VK_SELECT_ = 0x29, VK_PRINT_ = 0x2A,
                   VK_EXECUTE_ = 0x2B, VK_SNAPSHOT_ = 0x2C, VK_INSERT_ = 0x2D,
                   VK_DELETE_ = 0x2E, VK_HELP_ = 0x2F, VK_LWIN_ = 0x5B,
                   VK_RWIN_ = 0x5C, VK_APPS_ = 0x5D, VK_NUMPAD0_ = 0x60,
                   VK_NUMPAD9_ = 0x69, VK_MULTIPLY_ = 0x6A, VK_ADD_ = 0x6B,
                   VK_SEPARATOR_ = 0x6C, VK_SUBTRACT_ = 0x6D, VK_DECIMAL_ = 0x6E,
                   VK_DIVIDE_ = 0x6F, VK_F1_ = 0x70, VK_F24_ = 0x87,
                   VK_NUMLOCK_ = 0x90, VK_SCROLL_ = 0x91, VK_LSHIFT_ = 0xA0,
                   VK_RSHIFT_ = 0xA1, VK_LCONTROL_ = 0xA2, VK_RCONTROL_ = 0xA3,
                   VK_LMENU_ = 0xA4, VK_RMENU_ = 0xA5, VK_PROCESSKEY_ = 0xE5,
                   VK_PACKET_ = 0xE7;

// ── X11 keysym(keysymdef.h)──────────────────────────────────────
constexpr int32_t XK_BackSpace = 0xFF08, XK_Tab = 0xFF09, XK_Clear = 0xFF0B,
                  XK_Return = 0xFF0D, XK_Pause = 0xFF13, XK_Scroll_Lock = 0xFF14,
                  XK_Escape = 0xFF1B, XK_Home = 0xFF50, XK_Left = 0xFF51,
                  XK_Up = 0xFF52, XK_Right = 0xFF53, XK_Down = 0xFF54,
                  XK_Page_Up = 0xFF55, XK_Page_Down = 0xFF56, XK_End = 0xFF57,
                  XK_Begin = 0xFF58, XK_Select = 0xFF60, XK_Print = 0xFF61,
                  XK_Execute = 0xFF62, XK_Insert = 0xFF63, XK_Menu = 0xFF67,
                  XK_Help = 0xFF6A, XK_Num_Lock = 0xFF7F, XK_KP_Enter = 0xFF8D,
                  XK_KP_Home = 0xFF95, XK_KP_Left = 0xFF96, XK_KP_Up = 0xFF97,
                  XK_KP_Right = 0xFF98, XK_KP_Down = 0xFF99, XK_KP_Prior = 0xFF9A,
                  XK_KP_Next = 0xFF9B, XK_KP_End = 0xFF9C, XK_KP_Begin = 0xFF9D,
                  XK_KP_Insert = 0xFF9E, XK_KP_Delete = 0xFF9F,
                  XK_KP_Multiply = 0xFFAA, XK_KP_Add = 0xFFAB,
                  XK_KP_Separator = 0xFFAC, XK_KP_Subtract = 0xFFAD,
                  XK_KP_Decimal = 0xFFAE, XK_KP_Divide = 0xFFAF, XK_KP_0 = 0xFFB0,
                  XK_F1 = 0xFFBE, XK_Shift_L = 0xFFE1, XK_Shift_R = 0xFFE2,
                  XK_Control_L = 0xFFE3, XK_Control_R = 0xFFE4,
                  XK_Caps_Lock = 0xFFE5, XK_Alt_L = 0xFFE9, XK_Alt_R = 0xFFEA,
                  XK_Super_L = 0xFFEB, XK_Super_R = 0xFFEC, XK_Delete = 0xFFFF;

}  // namespace

int32_t KeysymFromChar(char32_t ch) {
  if (ch == 0) return 0;
  // X11 的 Latin-1 區段(0x20–0xFF)刻意與 Unicode 碼點對齊,直接用。
  // 0x7F–0x9F 是控制碼,沒有對應的 keysym,走 Unicode 區段。
  if (ch >= 0x20 && ch <= 0x7E) return static_cast<int32_t>(ch);
  if (ch >= 0xA0 && ch <= 0xFF) return static_cast<int32_t>(ch);
  return static_cast<int32_t>(0x01000000u | static_cast<uint32_t>(ch));
}

int32_t DeadKeysymFromChar(char32_t ch) {
  // ToUnicodeEx 對死鍵回傳的通常是「間隔形式」(spacing form,例如 U+00B4),
  // 但有些佈局給的是組合形式(combining,U+0301)。兩種都收。
  switch (ch) {
    case 0x0060: case 0x0300: return 0xFE50;  // dead_grave
    case 0x00B4: case 0x0301: return 0xFE51;  // dead_acute
    case 0x005E: case 0x0302: return 0xFE52;  // dead_circumflex
    case 0x007E: case 0x0303: return 0xFE53;  // dead_tilde
    case 0x00AF: case 0x0304: return 0xFE54;  // dead_macron
    case 0x02D8: case 0x0306: return 0xFE55;  // dead_breve
    case 0x02D9: case 0x0307: return 0xFE56;  // dead_abovedot
    case 0x00A8: case 0x0308: return 0xFE57;  // dead_diaeresis
    case 0x02DA: case 0x030A: return 0xFE58;  // dead_abovering
    case 0x02DD: case 0x030B: return 0xFE59;  // dead_doubleacute
    case 0x02C7: case 0x030C: return 0xFE5A;  // dead_caron
    case 0x00B8: case 0x0327: return 0xFE5B;  // dead_cedilla
    case 0x02DB: case 0x0328: return 0xFE5C;  // dead_ogonek
    default: return 0;
  }
}

int32_t LayoutIndependentKeysym(uint32_t vk, bool extended, bool num_lock) {
  // 數字鍵台。extended 位元是分辨的關鍵:編輯區(Insert/Home/方向鍵…)
  // 那幾顆是 extended,數字鍵台上同名的那幾顆不是。NumLock 關掉時
  // Windows 送的是 VK_INSERT / VK_END 這一組,靠 extended 才分得出來源。
  if (!extended) {
    switch (vk) {
      case VK_INSERT_: return XK_KP_Insert;
      case VK_DELETE_: return XK_KP_Delete;
      case VK_HOME_:   return XK_KP_Home;
      case VK_END_:    return XK_KP_End;
      case VK_PRIOR_:  return XK_KP_Prior;
      case VK_NEXT_:   return XK_KP_Next;
      case VK_LEFT_:   return XK_KP_Left;
      case VK_RIGHT_:  return XK_KP_Right;
      case VK_UP_:     return XK_KP_Up;
      case VK_DOWN_:   return XK_KP_Down;
      case VK_CLEAR_:  return XK_KP_Begin;  // NumLock 關掉時的數字鍵台 5
      default: break;
    }
  }
  if (vk >= VK_NUMPAD0_ && vk <= VK_NUMPAD9_)
    return XK_KP_0 + static_cast<int32_t>(vk - VK_NUMPAD0_);
  if (vk >= VK_F1_ && vk <= VK_F24_)
    return XK_F1 + static_cast<int32_t>(vk - VK_F1_);

  switch (vk) {
    case VK_BACK_:      return XK_BackSpace;
    case VK_TAB_:       return XK_Tab;
    case VK_CLEAR_:     return XK_Clear;
    case VK_RETURN_:    return extended ? XK_KP_Enter : XK_Return;
    case VK_PAUSE_:     return XK_Pause;
    case VK_CAPITAL_:   return XK_Caps_Lock;
    case VK_ESCAPE_:    return XK_Escape;
    // 空白鍵在所有佈局上都是 U+0020。放進固定表是為了讓它不受
    // ToUnicodeEx 在某些佈局下的怪異行為影響(例如把它當成死鍵的解除鍵)。
    case VK_SPACE_:     return 0x0020;
    case VK_PRIOR_:     return XK_Page_Up;
    case VK_NEXT_:      return XK_Page_Down;
    case VK_END_:       return XK_End;
    case VK_HOME_:      return XK_Home;
    case VK_LEFT_:      return XK_Left;
    case VK_UP_:        return XK_Up;
    case VK_RIGHT_:     return XK_Right;
    case VK_DOWN_:      return XK_Down;
    case VK_SELECT_:    return XK_Select;
    case VK_PRINT_:
    case VK_SNAPSHOT_:  return XK_Print;
    case VK_EXECUTE_:   return XK_Execute;
    case VK_INSERT_:    return XK_Insert;
    case VK_DELETE_:    return XK_Delete;
    case VK_HELP_:      return XK_Help;
    case VK_LWIN_:      return XK_Super_L;
    case VK_RWIN_:      return XK_Super_R;
    case VK_APPS_:      return XK_Menu;
    case VK_MULTIPLY_:  return XK_KP_Multiply;
    case VK_ADD_:       return XK_KP_Add;
    case VK_SEPARATOR_: return XK_KP_Separator;
    case VK_SUBTRACT_:  return XK_KP_Subtract;
    // ⚠ VK_DECIMAL 是本表唯一一顆**其實受佈局影響**的鍵:德文等佈局的
    //   數字鍵台小數點印的是逗號。X11 有 KP_Separator 表示逗號版本,
    //   但 Windows 兩者都送 VK_DECIMAL,要分辨得回頭問佈局。
    //   目前一律當 KP_Decimal —— 已知的不足,列在 README 的「沒被驗證」。
    case VK_DECIMAL_:   return XK_KP_Decimal;
    case VK_DIVIDE_:    return XK_KP_Divide;
    case VK_NUMLOCK_:   return XK_Num_Lock;
    case VK_SCROLL_:    return XK_Scroll_Lock;
    case VK_LSHIFT_:    return XK_Shift_L;
    case VK_RSHIFT_:    return XK_Shift_R;
    case VK_LCONTROL_:  return XK_Control_L;
    case VK_RCONTROL_:  return XK_Control_R;
    case VK_LMENU_:     return XK_Alt_L;
    case VK_RMENU_:     return XK_Alt_R;
    // 泛用的 VK_SHIFT / VK_CONTROL / VK_MENU:分不出左右時取左。
    case VK_SHIFT_:     return XK_Shift_L;
    case VK_CONTROL_:   return XK_Control_L;
    case VK_MENU_:      return XK_Alt_L;
    default: return 0;
  }
}

MappedKey MapKey(const KeyEvent& e, const KeyboardOracle& oracle) {
  MappedKey out;

  // VK_PROCESSKEY 是「這顆鍵已經被某個 IME 處理掉了」的佔位符,
  // VK_PACKET 是 SendInput 注入 Unicode 用的。兩者都不是真的按鍵,
  // 拿去查表只會得到垃圾。
  if (e.vk == VK_PROCESSKEY_ || e.vk == VK_PACKET_ || e.vk == 0) return out;

  // ── AltGr ────────────────────────────────────────────────────
  //
  // Windows 沒有獨立的 AltGr 鍵碼:按下右 Alt 時,有 AltGr 的佈局會**同時**
  // 合成一個左 Ctrl。所以「Ctrl 被按著」這件事,在德文/法文/波蘭文等佈局上
  // 可能根本不是使用者按了 Ctrl。
  //
  // 判斷成立時要做兩件事,少做任何一件都是壞的:
  //   1. 問佈局時**帶著** AltGr,才拿得到第三層的字元(德文 AltGr+Q = @)。
  //   2. 回報給引擎的 modifier **不帶** Control/Alt。帶了的話 librime 會把
  //      AltGr+Q 當成 Ctrl+Alt+@ 這種快捷鍵,那顆鍵就永遠打不出字。
  const bool altgr = oracle.HasAltGr() && e.right_alt && e.ctrl;

  if (e.shift) out.modifiers |= kModShift;
  if (e.caps_lock) out.modifiers |= kModCaps;
  if (e.ctrl && !altgr) out.modifiers |= kModControl;
  if (e.alt && !altgr) out.modifiers |= kModAlt;
  if (e.win) out.modifiers |= kModSuper;
  if (e.key_up) out.modifiers |= kModRelease;

  // ── 第一類:與佈局無關的鍵 ───────────────────────────────────
  const int32_t fixed = LayoutIndependentKeysym(e.vk, e.extended, e.num_lock);
  if (fixed != 0) {
    out.keysym = fixed;
    return out;
  }

  // ── 第二類:會產生字元的鍵 —— 一律問佈局 ─────────────────────
  //
  // ⚠ 這裡**不可以**寫成 `if (vk >= 'A' && vk <= 'Z') keysym = vk + 32;`。
  //   那樣寫在 QWERTY 上看起來完全正確,而 Dvorak 使用者打出來的每一個字
  //   都是錯的 —— 沒有崩潰、沒有錯誤訊息,只是輸入法沒用。
  //
  // 問的時候刻意**不帶 Ctrl / Alt**(AltGr 除外):Ctrl+A 在 Windows 上
  // 產生的是控制碼 0x01,不是 'a'。引擎要的是「哪一顆鍵」加「按著什麼修飾鍵」,
  // 這也是 ibus / fcitx 的慣例。
  bool is_dead = false;
  char32_t ch =
      oracle.Translate(e.vk, e.scan_code, e.shift, e.caps_lock, altgr, &is_dead);

  if (ch != 0 && is_dead) {
    const int32_t dk = DeadKeysymFromChar(ch);
    // 認不得的死鍵退回它的間隔字元。比丟掉好:使用者至少打得出東西。
    out.keysym = dk ? dk : KeysymFromChar(ch);
    return out;
  }
  if (ch != 0) {
    out.keysym = KeysymFromChar(ch);
    return out;
  }

  // 佈局什麼都沒給。到這裡通常代表這顆鍵在這個佈局上沒有字元
  // (例如某些佈局的 VK_OEM_8)。回傳 0 = 不送進引擎、原樣放行給宿主。
  //
  // 刻意**不做** 「VK_A 就當 'a'」的退路:那正是這一整個檔案要避免的東西。
  // 寧可少送一顆鍵,也不要在別的佈局上送出錯的鍵。
  return out;
}

}  // namespace rimewin
