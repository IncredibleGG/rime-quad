// windows/tests/test_keymap.cc — 按鍵映射(四端最難的一格)

#include "../common/keymap.h"

#include "check.h"
#include "fake_layouts.h"

// 直接把共用門面的標頭拉進來對帳。這裡不呼叫任何函式,只用它的列舉值 ——
// keymap.h 為了不把 rime_shell 拖進宿主進程而自己重寫了一份 Mod,
// 兩份一旦漂移,送給引擎的 modifier 就會整組錯位,而且畫面上完全看不出來。
#include "rime_shell.h"

using namespace rimewin;
using rimewin_test::MakeDE;
using rimewin_test::MakeFR;
using rimewin_test::MakeUS;

namespace {

KeyEvent Down(uint32_t vk) {
  KeyEvent e;
  e.vk = vk;
  e.scan_code = 0x10;  // 假的;FakeOracle 不看它
  return e;
}

}  // namespace

TEST(keymap_mod_bits_match_rime_shell_abi) {
  // keymap.h 的 Mod 必須逐位對上 rime_shell.h 的 rs_modifier。
  CHECK_INT(kModShift, RS_MOD_SHIFT);
  CHECK_INT(kModControl, RS_MOD_CONTROL);
  CHECK_INT(kModAlt, RS_MOD_ALT);
  CHECK_INT(kModSuper, RS_MOD_SUPER);
  CHECK_INT(kModCaps, RS_MOD_CAPS);
  CHECK_INT(kModRelease, RS_MOD_RELEASE);
  // 順便釘住門面 ABI:對不上就代表 core/ 動過而 Windows 端沒跟上。
  //
  // 改這個數字之前,必須先確認上面那六個位元還對得上,以及 IPC 握手裡的
  // shell_abi 兩側仍然同源(瘦 DLL 用編譯期常數、服務用 rs_abi_version())。
  //
  // 變更紀錄:
  //   1 -> 2  core/ 新增 rs_sync_user_data() 與 rs_sync_dir()(詞庫匯出要
  //           先讓 librime 把記憶體裡的交易落地)。純新增函式,rs_modifier
  //           的位元與所有既有結構的佈局一個位元都沒動,Windows 端無需改碼。
  //   2 -> 3  core/ 新增 rs_set_input() / rs_get_input()(九宮格的音節消歧
  //           要靠改寫輸入串,librime 沒有「選擇拼寫」的 API)。純新增函式,
  //           rs_modifier 的位元與所有既有結構的佈局一個位元都沒動。
  CHECK_INT(RIME_SHELL_ABI_VERSION, 3);
}

TEST(keymap_char_to_keysym_rules) {
  // X11 的 Latin-1 區段與碼點對齊。
  CHECK_INT(KeysymFromChar(U'a'), 0x61);
  CHECK_INT(KeysymFromChar(U'A'), 0x41);
  CHECK_INT(KeysymFromChar(U' '), 0x20);
  CHECK_INT(KeysymFromChar(U'~'), 0x7E);
  CHECK_INT(KeysymFromChar(0x00FC), 0xFC);  // ü
  CHECK_INT(KeysymFromChar(0x00A3), 0xA3);  // £
  // 其餘走 Unicode 區段。
  CHECK_INT(KeysymFromChar(0x20AC), 0x010020AC);  // €
  CHECK_INT(KeysymFromChar(0x4F60), 0x01004F60);  // 你
  CHECK_INT(KeysymFromChar(0), 0);
  // 0x7F–0x9F 是控制碼,沒有 Latin-1 keysym,必須走 Unicode 區段。
  CHECK_INT(KeysymFromChar(0x80), 0x01000080);
}

TEST(keymap_layout_independent_keys) {
  // 這一類寫死是**正確的**:方向鍵在任何佈局上都是方向鍵。
  CHECK_INT(LayoutIndependentKeysym(0x08, false, true), 0xFF08);  // BackSpace
  CHECK_INT(LayoutIndependentKeysym(0x09, false, true), 0xFF09);  // Tab
  CHECK_INT(LayoutIndependentKeysym(0x1B, false, true), 0xFF1B);  // Escape
  CHECK_INT(LayoutIndependentKeysym(0x20, false, true), 0x0020);  // space
  CHECK_INT(LayoutIndependentKeysym(0x70, false, true), 0xFFBE);  // F1
  CHECK_INT(LayoutIndependentKeysym(0x87, false, true), 0xFFD5);  // F24
  // 字母與 OEM 鍵**不在**這張表裡 —— 它們必須去問佈局。
  CHECK_INT(LayoutIndependentKeysym(0x41, false, true), 0);       // VK_A
  CHECK_INT(LayoutIndependentKeysym(0xBA, false, true), 0);       // VK_OEM_1
  CHECK_INT(LayoutIndependentKeysym(0x32, false, true), 0);       // VK_2
}

TEST(keymap_extended_bit_splits_numpad_from_editing_cluster) {
  // NumLock 關掉時,數字鍵台送的是 VK_INSERT / VK_END 這一組,
  // 與編輯區同名。唯一分得出來的是 lParam 的 extended 位元。
  CHECK_INT(LayoutIndependentKeysym(0x2D, /*extended=*/true, false), 0xFF63);   // Insert
  CHECK_INT(LayoutIndependentKeysym(0x2D, /*extended=*/false, false), 0xFF9E);  // KP_Insert
  CHECK_INT(LayoutIndependentKeysym(0x23, true, false), 0xFF57);                // End
  CHECK_INT(LayoutIndependentKeysym(0x23, false, false), 0xFF9C);               // KP_End
  CHECK_INT(LayoutIndependentKeysym(0x25, true, false), 0xFF51);                // Left
  CHECK_INT(LayoutIndependentKeysym(0x25, false, false), 0xFF96);               // KP_Left
  // 主鍵盤 Enter 與數字鍵台 Enter 是兩顆不同的鍵。
  CHECK_INT(LayoutIndependentKeysym(0x0D, false, true), 0xFF0D);  // Return
  CHECK_INT(LayoutIndependentKeysym(0x0D, true, true), 0xFF8D);   // KP_Enter
  // NumLock 開著時是 VK_NUMPAD0..9。
  CHECK_INT(LayoutIndependentKeysym(0x60, false, true), 0xFFB0);  // KP_0
  CHECK_INT(LayoutIndependentKeysym(0x69, false, true), 0xFFB9);  // KP_9
}

TEST(keymap_us_basics) {
  const auto us = MakeUS();
  CHECK_INT(MapKey(Down(rimewin_test::kVkA), us).keysym, 0x61);  // a
  {
    KeyEvent e = Down(rimewin_test::kVkA);
    e.shift = true;
    const MappedKey m = MapKey(e, us);
    CHECK_INT(m.keysym, 0x41);  // A
    CHECK_INT(m.modifiers, kModShift);
  }
  {
    KeyEvent e = Down(rimewin_test::kVk2);
    e.shift = true;
    CHECK_INT(MapKey(e, us).keysym, U'@');
  }
  CHECK_INT(MapKey(Down(rimewin_test::kVkOem1), us).keysym, U';');
  CHECK_INT(MapKey(Down(rimewin_test::kVkOemComma), us).keysym, U',');
}

// ★ 這一條是整份測試的重點。
//
// 它斷言的是:**同一個 VK,在不同佈局下必須映到不同的 keysym。**
// 只要有人把 MapKey 改回一張寫死的 VK→字元表,這一條就會紅。
// 而寫死的那種錯誤,在美式鍵盤上跑起來完全正常 —— CI 也是美式鍵盤。
TEST(keymap_same_vk_differs_across_layouts) {
  const auto us = MakeUS();
  const auto de = MakeDE();
  const auto fr = MakeFR();

  // VK_OEM_1:美式 ';'、德文 'ü'、法文 '$'。Rime 的 punctuator 用得到這顆。
  CHECK_INT(MapKey(Down(rimewin_test::kVkOem1), us).keysym, U';');
  CHECK_INT(MapKey(Down(rimewin_test::kVkOem1), de).keysym, 0xFC);  // ü
  CHECK_INT(MapKey(Down(rimewin_test::kVkOem1), fr).keysym, U'$');

  // 數字列:法文不按 Shift 是 'é'。
  CHECK_INT(MapKey(Down(rimewin_test::kVk2), us).keysym, U'2');
  CHECK_INT(MapKey(Down(rimewin_test::kVk2), fr).keysym, 0xE9);  // é
  {
    KeyEvent e = Down(rimewin_test::kVk2);
    e.shift = true;
    CHECK_INT(MapKey(e, fr).keysym, U'2');  // 法文要按 Shift 才是 2
    CHECK_INT(MapKey(e, us).keysym, U'@');
    CHECK_INT(MapKey(e, de).keysym, U'"');
  }

  // 位置式佈局(VK 不跟著字元走)也必須正確。
  CHECK_INT(MapKey(Down(rimewin_test::kVkA), fr).keysym, U'q');
  CHECK_INT(MapKey(Down(rimewin_test::kVkQ), fr).keysym, U'a');
}

TEST(keymap_altgr_is_not_ctrl_alt) {
  const auto de = MakeDE();

  // 德文 AltGr+Q = '@'。Windows 回報的是「右 Alt + Ctrl」。
  KeyEvent e = Down(rimewin_test::kVkQ);
  e.ctrl = true;
  e.alt = true;
  e.right_alt = true;
  const MappedKey m = MapKey(e, de);
  CHECK_INT(m.keysym, U'@');
  // 關鍵:回報給引擎的 modifier **不可以**帶 Control / Alt。
  // 帶了的話 librime 會把它當成 Ctrl+Alt+@ 這種快捷鍵,那顆鍵永遠打不出字。
  CHECK_INT(m.modifiers & kModControl, 0u);
  CHECK_INT(m.modifiers & kModAlt, 0u);

  // 沒有 AltGr 的佈局上,同樣的按鍵狀態就真的是 Ctrl+Alt。
  const auto us = MakeUS();
  KeyEvent e2 = Down(rimewin_test::kVkQ);
  e2.ctrl = true;
  e2.alt = true;
  e2.right_alt = true;
  const MappedKey m2 = MapKey(e2, us);
  CHECK_INT(m2.keysym, U'q');
  CHECK_INT(m2.modifiers & kModControl, (uint32_t)kModControl);
  CHECK_INT(m2.modifiers & kModAlt, (uint32_t)kModAlt);

  // 左 Alt + Ctrl(不是 AltGr)在德文佈局上也還是 Ctrl+Alt。
  KeyEvent e3 = Down(rimewin_test::kVkQ);
  e3.ctrl = true;
  e3.alt = true;
  e3.right_alt = false;
  const MappedKey m3 = MapKey(e3, de);
  CHECK_INT(m3.modifiers & kModControl, (uint32_t)kModControl);
}

TEST(keymap_ctrl_reports_base_char_not_control_code) {
  // Ctrl+A 在 Windows 上產生的是 0x01。引擎要的是 keysym 'a' + Control。
  const auto us = MakeUS();
  KeyEvent e = Down(rimewin_test::kVkA);
  e.ctrl = true;
  const MappedKey m = MapKey(e, us);
  CHECK_INT(m.keysym, U'a');
  CHECK_INT(m.modifiers, (uint32_t)kModControl);
}

TEST(keymap_caps_lock) {
  const auto us = MakeUS();
  KeyEvent e = Down(rimewin_test::kVkA);
  e.caps_lock = true;
  const MappedKey m = MapKey(e, us);
  CHECK_INT(m.keysym, U'A');
  CHECK_INT(m.modifiers, (uint32_t)kModCaps);

  // CapsLock + Shift 在字母上互相抵銷。
  KeyEvent e2 = Down(rimewin_test::kVkA);
  e2.caps_lock = true;
  e2.shift = true;
  CHECK_INT(MapKey(e2, us).keysym, U'a');

  // 但 CapsLock 不作用在標點上。
  KeyEvent e3 = Down(rimewin_test::kVkOem1);
  e3.caps_lock = true;
  CHECK_INT(MapKey(e3, us).keysym, U';');
}

TEST(keymap_release_flag) {
  const auto us = MakeUS();
  KeyEvent e = Down(rimewin_test::kVkA);
  e.key_up = true;
  const MappedKey m = MapKey(e, us);
  CHECK_INT(m.keysym, U'a');
  CHECK_INT(m.modifiers & kModRelease, (uint32_t)kModRelease);
}

TEST(keymap_unmappable_keys_return_zero) {
  const auto us = MakeUS();
  // VK_PROCESSKEY:已經被別的 IME 吃掉的佔位符。
  CHECK_INT(MapKey(Down(0xE5), us).keysym, 0);
  // VK_PACKET:SendInput 注入 Unicode 用,不是真的按鍵。
  CHECK_INT(MapKey(Down(0xE7), us).keysym, 0);
  CHECK_INT(MapKey(Down(0), us).keysym, 0);
  // 這個佈局上沒有字元的鍵:回 0 = 原樣放行,而不是猜一個。
  CHECK_INT(MapKey(Down(0xE2 /* VK_OEM_102,美式鍵盤上沒有 */), us).keysym, 0);
}

TEST(keymap_dead_keys) {
  // 法文的 ^ 是死鍵。
  rimewin_test::FakeOracle fr("fr-dead", true);
  fr.Add(0xDD /* VK_OEM_6 */, 0x005E, 0x00A8, 0, /*dead_base=*/true);
  KeyEvent e = Down(0xDD);
  CHECK_INT(MapKey(e, fr).keysym, 0xFE52);  // dead_circumflex

  // 組合形式(U+0300 系)也要認得。
  rimewin_test::FakeOracle x("combining", false);
  x.Add(0xDD, 0x0301, 0, 0, true);
  CHECK_INT(MapKey(Down(0xDD), x).keysym, 0xFE51);  // dead_acute

  // 認不得的死鍵退回它的間隔字元,而不是丟掉。
  rimewin_test::FakeOracle y("odd-dead", false);
  y.Add(0xDD, 0x02BC, 0, 0, true);
  CHECK_INT(MapKey(Down(0xDD), y).keysym, 0x010002BC);
}

TEST(keymap_space_is_layout_independent) {
  // 空白鍵不去問佈局:某些佈局會把它當成死鍵的解除鍵而回傳奇怪的東西,
  // 而空白鍵在 Rime 裡是「上屏 / 選第一個候選」,不能有意外。
  rimewin_test::FakeOracle weird("weird", false);
  weird.Add(0x20, U'x', U'X');
  CHECK_INT(MapKey(Down(0x20), weird).keysym, 0x20);
}
