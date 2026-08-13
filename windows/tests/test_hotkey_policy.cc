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

// ══════════════════════════════════════════════════════════════════
//  簡繁快捷鍵 Ctrl+Shift+F(G76)—— 而且**只有**它
// ══════════════════════════════════════════════════════════════════
//
// 微軟拼音的預設就是這一顆,而我們兩端都沒有。
//
// ⚠ 這一支的重點與 Ctrl+空白鍵那一支完全相同:不是「它會命中」那一行,
//   是**三顆熱鍵互不干擾**那張表。Ctrl+F 是每一個程式的「尋找」,
//   Ctrl+Shift+空白鍵在很多程式裡是不斷行空白 —— 多吃一顆就是一顆
//   壞掉的鍵,而那比缺一個功能嚴重(common/key_eat_policy.h 的那一課)。

TEST(hotkey_ctrl_shift_f_is_the_variant_toggle) {
  CHECK(ClassifyHotkey(0x46, kModControl | kModShift) ==
        Hotkey::kToggleVariant);
  // CapsLock 開著照樣算 —— 它是一個持續的狀態,不是為了這顆熱鍵按的。
  CHECK(ClassifyHotkey(0x46, kModControl | kModShift | kModCaps) ==
        Hotkey::kToggleVariant);

  // 正規形式必須與 TSF 那一側註冊的 {'F', TF_MOD_CONTROL | TF_MOD_SHIFT}
  // 對得上。對不上就是「註冊了一顆永遠不會被認得的鍵」——
  // 症狀是按下去完全沒反應,而每一層都回報成功。
  CHECK_INT(VariantToggleKeysym(), 0x46);
  CHECK_INT(VariantToggleModifiers(), kModControl | kModShift);
  CHECK(ClassifyHotkey(VariantToggleKeysym(), VariantToggleModifiers()) ==
        Hotkey::kToggleVariant);

  // ⚠ 這一顆**沒有**使用者開關(同 Ctrl+空白鍵),所以輕點 Shift 那個
  //   開關關著時它照樣要動。
  CHECK(DecideKeyAction(VariantToggleKeysym(), VariantToggleModifiers(),
                        true) == KeyAction::kToggleVariant);
  CHECK(DecideKeyAction(VariantToggleKeysym(), VariantToggleModifiers(),
                        false) == KeyAction::kToggleVariant);
}

TEST(hotkey_three_hotkeys_do_not_interfere) {
  // 三顆各自命中。
  CHECK(ClassifyHotkey(0x20, kModControl) == Hotkey::kToggleAsciiMode);
  CHECK(ClassifyHotkey(0xFFE1, 0) == Hotkey::kToggleAsciiModeShiftTap);
  CHECK(ClassifyHotkey(0x46, kModControl | kModShift) ==
        Hotkey::kToggleVariant);

  // ── 交叉:任何一顆的修飾鍵配另一顆的鍵,一個都不可以命中 ──────
  // Ctrl+Shift+空白鍵:很多程式的「不斷行空白」。
  CHECK(ClassifyHotkey(0x20, kModControl | kModShift) == Hotkey::kNone);
  // Ctrl+F:每一個程式的「尋找」。
  CHECK(ClassifyHotkey(0x46, kModControl) == Hotkey::kNone);
  // Shift+F / 裸 F:使用者只是在打一個大寫 F。
  CHECK(ClassifyHotkey(0x46, kModShift) == Hotkey::kNone);
  CHECK(ClassifyHotkey(0x46, 0) == Hotkey::kNone);
  // 小寫 f 不是它(TSF 那一側送的是 VK_F,正規形式是大寫)。
  CHECK(ClassifyHotkey(0x66, kModControl | kModShift) == Hotkey::kNone);
  // 帶第三顆修飾鍵一律不算(AltGr、Win 組合)。
  CHECK(ClassifyHotkey(0x46, kModControl | kModShift | kModAlt) ==
        Hotkey::kNone);
  CHECK(ClassifyHotkey(0x46, kModControl | kModShift | kModSuper) ==
        Hotkey::kNone);
  // 帶修飾鍵的 Shift_L 不是輕點。
  CHECK(ClassifyHotkey(0xFFE1, kModControl | kModShift) == Hotkey::kNone);
  // ⚠ 放開事件一律不算。少了這一條,按一次會切兩次 ——
  //   而「切了兩次」在使用者眼裡就是「按了完全沒反應」。
  CHECK(ClassifyHotkey(0x46, kModControl | kModShift | kModRelease) ==
        Hotkey::kNone);

  // ── 使用者把輕點 Shift 關掉,不可以連另外兩顆一起關掉 ───────────
  CHECK(DecideKeyAction(0xFFE1, 0, false) == KeyAction::kIgnore);
  CHECK(DecideKeyAction(0x20, kModControl, false) ==
        KeyAction::kToggleAsciiMode);
  CHECK(DecideKeyAction(0x46, kModControl | kModShift, false) ==
        KeyAction::kToggleVariant);
}

TEST(hotkey_variant_key_does_not_swallow_the_other_ctrl_shift_letters) {
  // Ctrl+Shift+字母是一大票程式的快捷鍵(Ctrl+Shift+T 復原分頁、
  // Ctrl+Shift+N 無痕視窗…)。**只有 F 那一顆**是我們的。
  int checked = 0;
  for (int32_t ch = 'A'; ch <= 'Z'; ++ch) {
    const Hotkey h = ClassifyHotkey(ch, kModControl | kModShift);
    if (ch == 'F') {
      CHECK(h == Hotkey::kToggleVariant);
    } else {
      CHECK(h == Hotkey::kNone);
    }
    // 小寫的一顆都不是。
    CHECK(ClassifyHotkey(ch + 32, kModControl | kModShift) == Hotkey::kNone);
    checked += 2;
  }
  CHECK_INT(checked, 52);  // 掃描範圍非空(§2-G2)
}
