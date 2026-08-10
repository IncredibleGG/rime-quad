// windows/tests/test_hotkey_policy.cc — Ctrl+空白鍵,而且**只有**它
//
// 使用者回報「ctrl+ 空格沒辦法切中英文」。補上它的風險在另一邊:
// 這個專案的 `OnTestKeyDown` 曾經宣稱每一個 keysym 都要,結果吃掉了
// Ctrl+C / Ctrl+V、退格、英文字母與 F 系列鍵(見 common/key_eat_policy.h)。
//
// 所以這一支的重點不是「Ctrl+空白鍵會命中」那一行,是下面那張**沒有命中**
// 的表:每一個 Ctrl 組合、每一個帶額外修飾鍵的空白鍵、以及一顆一顆的
// 常用鍵,全部必須是 kNone。

#include "hotkey_policy.h"

#include "check.h"
#include "keymap.h"

using namespace rimewin;

namespace {

bool Toggle(int32_t keysym, uint32_t mods) {
  return IsAsciiToggleHotkey(keysym, mods);
}

}  // namespace

TEST(hotkey_ctrl_space_is_the_ascii_toggle) {
  CHECK(Toggle(0x20, kModControl));
  // CapsLock 開著也要算 —— 它是一個持續的狀態,不是為了這顆熱鍵按的。
  CHECK(Toggle(0x20, kModControl | kModCaps));

  // 正規形式必須與 TSF 那一側註冊的 {VK_SPACE, TF_MOD_CONTROL} 對得上。
  CHECK_INT(AsciiToggleKeysym(), 0x20);
  CHECK_INT(AsciiToggleModifiers(), kModControl);
  CHECK(Toggle(AsciiToggleKeysym(), AsciiToggleModifiers()));
}

// ── 這一條才是重點:別的鍵一顆都不可以被認成熱鍵 ──────────────────
TEST(hotkey_does_not_swallow_anything_else) {
  int checked = 0;

  // 1. 每一個 Ctrl+字母。Ctrl+C / Ctrl+V 被輸入法吃掉是災難。
  for (int32_t ch = 'a'; ch <= 'z'; ++ch) {
    CHECK(!Toggle(ch, kModControl));
    CHECK(!Toggle(ch, kModControl | kModShift));
    checked += 2;
  }
  // 2. 每一個 Ctrl+數字。
  for (int32_t ch = '0'; ch <= '9'; ++ch) {
    CHECK(!Toggle(ch, kModControl));
    ++checked;
  }
  // 3. 空白鍵本身,各種修飾鍵組合 —— **只有**單純的 Ctrl 命中。
  const uint32_t others[] = {
      0,
      kModShift,
      kModAlt,
      kModSuper,
      kModControl | kModShift,   // 很多程式的「不斷行空白」
      kModControl | kModAlt,     // 部分佈局上等於 AltGr+空白
      kModControl | kModSuper,
      kModShift | kModAlt,
      kModControl | kModShift | kModAlt,
  };
  for (uint32_t m : others) {
    CHECK(!Toggle(0x20, m));
    ++checked;
  }
  // 4. 放開事件不算(不然按一次切兩次 = 看起來完全沒反應)。
  CHECK(!Toggle(0x20, kModControl | kModRelease));
  ++checked;

  // 5. 一顆一顆的功能鍵與常用鍵,帶不帶 Ctrl 都不是熱鍵。
  //    ⚠ keysym 用 keymap.h 那一組 X11 值(0xFF00 起)。
  const int32_t keys[] = {
      0xFF08,  // BackSpace
      0xFF09,  // Tab
      0xFF0D,  // Return
      0xFF1B,  // Escape
      0xFF50,  // Home
      0xFF51,  // Left
      0xFF52,  // Up
      0xFF53,  // Right
      0xFF54,  // Down
      0xFF55,  // Prior(PageUp)
      0xFF56,  // Next(PageDown)
      0xFF57,  // End
      0xFFFF,  // Delete
      0xFFBE,  // F1
      0xFFC9,  // F12
      0xFFE1,  // Shift_L
      0xFFE3,  // Control_L
      0xFFE9,  // Alt_L
      0xFF80,  // KP_Space —— ⚠ 數字鍵台的空白,**不是**空白鍵
  };
  for (int32_t k : keys) {
    CHECK(!Toggle(k, 0));
    CHECK(!Toggle(k, kModControl));
    CHECK(!Toggle(k, kModControl | kModShift));
    checked += 3;
  }
  // 6. 一般打字:每一個字母、數字、常見標點,沒有修飾鍵時都不是熱鍵。
  for (int32_t ch = 0x21; ch <= 0x7E; ++ch) {
    CHECK(!Toggle(ch, 0));
    CHECK(!Toggle(ch, kModShift));
    checked += 2;
  }
  // 7. 沒有修飾鍵的空白鍵 —— 它是最常按的一顆鍵,而且在拼音裡是選字。
  CHECK(!Toggle(0x20, 0));
  ++checked;

  // 掃描範圍非空(§2-G2):上面真的跑過這麼多組,不是零組。
  CHECK(checked > 200);
}

// ── 分類本身只有兩種結果,而且映不出 keysym 的鍵不會命中 ─────────
TEST(hotkey_unmappable_keys_are_never_a_hotkey) {
  CHECK(ClassifyHotkey(0, kModControl) == Hotkey::kNone);
  CHECK(ClassifyHotkey(0, 0) == Hotkey::kNone);
  CHECK(ClassifyHotkey(-1, kModControl) == Hotkey::kNone);
  CHECK(ClassifyHotkey(0x20, kModControl) == Hotkey::kToggleAsciiMode);
}
