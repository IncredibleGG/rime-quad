// windows/common/keymap.h — VK_* + 鍵盤狀態 → X11 keysym
//
// 四端中最難的一格(docs/handoff-windows.md §4)。難點不是 keysym 表,
// 是這件事:
//
//   ⚠ **同一顆實體按鍵在不同鍵盤佈局下產生不同的字元。**
//     使用者裝的是 Dvorak,VK_S 那顆鍵印的是 'o';德文鍵盤上 VK_OEM_1 是 'ü';
//     法文 AZERTY 的 VK_A 是 'q'。TSF 給的是 VK_*,而 VK_* 是**按位置**編的,
//     不是按字元編的。所以「VK_A → keysym 'a'」這種常數表,對 QWERTY 以外的
//     使用者一律是錯的,而且錯得很安靜 —— 打出來的是別的字,不會崩潰、
//     不會報錯,只是輸入法完全沒用。
//
// 因此本檔把映射切成兩半:
//
//   1. **與佈局無關的鍵**(方向鍵、F1–F24、Backspace、數字鍵台、修飾鍵……)
//      用固定表。這些鍵在任何佈局下都是同一件事,寫死是正確的。
//   2. **會產生字元的鍵**(字母、數字、OEM 標點)**一律問佈局**。
//      問的方式抽象成 KeyboardOracle;Win32 的實作是 ToUnicodeEx(..., hkl),
//      測試用的實作是幾張真實佈局的表(QWERTY / Dvorak / 德文 / AZERTY)。
//
// 這個切法同時解決了驗證問題:第 2 類的邏輯完全不碰 Windows API,
// 可以在 Ubuntu 上編譯並跑測試。而「有人把它改回常數表」這件事,
// 由 tests/test_keymap.cc 裡那條「同一個 VK 在 QWERTY 與 Dvorak 下
// 必須給出不同 keysym」的斷言擋住。
//
#ifndef RIMEWIN_KEYMAP_H_
#define RIMEWIN_KEYMAP_H_

#include <cstdint>

namespace rimewin {

// rime_shell.h 的 rs_modifier。這裡重寫一份,是為了讓本檔不必 include
// rime_shell.h —— DLL 側完全不連結 rime_shell,不該為了幾個常數把整個
// 門面標頭拉進宿主進程。值必須與 rime_shell.h 一致,tests 裡有斷言對帳。
//
// 注意:這**不是** librime 的遮罩。librime 的 kSuperMask 是 1<<26,
// 轉換由 core/src/rime_shell.cc 的 to_rime_mask() 負責,我們不碰。
enum Mod : uint32_t {
  kModShift = 1u << 0,
  kModControl = 1u << 1,
  kModAlt = 1u << 2,
  kModSuper = 1u << 3,
  kModCaps = 1u << 4,
  kModRelease = 1u << 5,
};

// TSF 的 OnKeyDown/OnKeyUp 給的原始資料,加上一次 GetKeyboardState 的結果。
// 刻意用純量而不是 BYTE[256]:讓測試不必模擬整張鍵盤狀態表。
struct KeyEvent {
  uint32_t vk = 0;         // wParam
  uint32_t scan_code = 0;  // (lParam >> 16) & 0xFF
  bool extended = false;   // lParam bit 24。分辨主鍵盤 Enter 與數字鍵台 Enter
  bool key_up = false;

  bool shift = false;
  bool ctrl = false;
  bool alt = false;
  bool win = false;       // 任一 Win 鍵
  bool caps_lock = false; // toggle 狀態(不是按著)
  bool num_lock = false;  // toggle 狀態

  // AltGr 偵測需要:Windows 把 AltGr 回報成「左 Ctrl + 右 Alt」。
  bool right_alt = false;
};

// 佈局問答。實作必須是**無狀態的查詢** —— 特別是 Win32 的 ToUnicodeEx
// 會動到核心裡的 dead key 狀態,實作有責任不讓它外洩(見 win32_oracle.cc)。
class KeyboardOracle {
 public:
  virtual ~KeyboardOracle() = default;

  // 回傳 (vk, scan_code) 在指定的 shift / caps / altgr 狀態下產生的字元;
  // 不產生字元時回傳 0。
  //
  // out_is_dead 置位代表這是一顆死鍵(dead key,例如法文的 ^),
  // 此時回傳值是它所代表的附加符號字元。
  virtual char32_t Translate(uint32_t vk, uint32_t scan_code, bool shift,
                             bool caps, bool altgr, bool* out_is_dead) const = 0;

  // 這個佈局有沒有 AltGr 層。沒有的話,Ctrl+Alt 就只是 Ctrl+Alt。
  virtual bool HasAltGr() const = 0;
};

struct MappedKey {
  // 0 = 無法映射,**不可**送進引擎。
  // (rs_keysym_by_name 查不到時也回 0,兩者語意一致。)
  int32_t keysym = 0;
  uint32_t modifiers = 0;  // Mod 位元
};

// 主要入口。純函式:同樣的 KeyEvent 加同樣的 oracle 一定給同樣的結果。
MappedKey MapKey(const KeyEvent& e, const KeyboardOracle& oracle);

// 只做「與佈局無關」那一半的查表,測試與診斷用。0 = 不是這一類的鍵。
int32_t LayoutIndependentKeysym(uint32_t vk, bool extended, bool num_lock);

// Unicode 字元 → X11 keysym。規則來自 X11:
//   0x20–0x7E 與 0xA0–0xFF 的 keysym 就是碼點本身(Latin-1 區段是刻意對齊的);
//   其餘一律是 0x01000000 | 碼點。
int32_t KeysymFromChar(char32_t ch);

// 死鍵字元 → dead_* keysym。查不到回傳 0,呼叫端應退回一般字元處理。
int32_t DeadKeysymFromChar(char32_t ch);

}  // namespace rimewin

#endif  // RIMEWIN_KEYMAP_H_
