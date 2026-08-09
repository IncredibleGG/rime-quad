// windows/tests/test_key_eat_policy.cc — 「哪些鍵可以宣告吃掉」的逐鍵真值表
//
// 使用者回報:「居然無法刪除字,可以打字,不能刪除。」
// 成因見 common/key_eat_policy.h 的檔頭:退格在**沒有組字**的時候也被
// 宣告吃掉,而引擎不處理它 —— 那顆鍵於是掉進宿主與我們之間的黑洞。
//
// 這份測試的形狀是刻意的:**每一顆鍵,在「組字中」與「沒有組字」兩種狀態下
// 各斷言一次**。舊的測試打的是 `nihao` 六顆字母,所以退格從來沒有被問過。

#include "../common/key_eat_policy.h"

#include "../common/keymap.h"
#include "check.h"

using namespace rimewin;

namespace {

// 直接向 keymap 要 keysym,不要在測試裡自己抄一份常數。
//
// ⚠ 抄一份的話,這份測試會測到「我抄的那個值」而不是「產品真的會送出的
//   那個值」—— 而 keymap.cc 改一個值時它仍然全綠。這個專案抓過那種測試。
int32_t Ks(uint32_t vk, bool extended = false) {
  const int32_t k = LayoutIndependentKeysym(vk, extended, /*num_lock=*/true);
  return k;
}

constexpr uint32_t kVkBack = 0x08, kVkTab = 0x09, kVkReturn = 0x0D,
                   kVkEscape = 0x1B, kVkSpace = 0x20, kVkPrior = 0x21,
                   kVkNext = 0x22, kVkEnd = 0x23, kVkHome = 0x24,
                   kVkLeft = 0x25, kVkUp = 0x26, kVkRight = 0x27,
                   kVkDown = 0x28, kVkInsert = 0x2D, kVkDelete = 0x2E,
                   kVkF1 = 0x70, kVkF4 = 0x73;

// keymap.h 的 Mod 位元。
constexpr uint32_t kCtrl = kModControl, kAlt = kModAlt, kShift = kModShift;

}  // namespace

// ── 這就是使用者回報的那一格 ──────────────────────────────────────
TEST(backspace_is_not_eaten_when_there_is_no_composition) {
  const int32_t bs = Ks(kVkBack);
  CHECK(bs != 0);  // 映得出來 —— 不然下面測的是「映不出所以不吃」,是另一回事
  const KeyKind k = ClassifyKeyKind(bs, 0);
  CHECK(k == KeyKind::kEditing);

  // 沒有組字:**必須放行**。使用者要刪的是他已經上屏的字,那是宿主的事。
  CHECK(!ShouldEatKey(k, /*composing=*/false));
  // 組字中:必須吃,不然刪不掉組字串的最後一個字母。
  CHECK(ShouldEatKey(k, /*composing=*/true));
  // 功能鍵不自己補字元 —— 補了等於我們去改宿主的文件。
  CHECK(!ShouldSelfInsert(k));
}

// 退格不會是唯一一顆。整族一起驗。
TEST(editing_and_navigation_keys_only_eaten_while_composing) {
  struct Row {
    const char* name;
    uint32_t vk;
    bool extended;
    KeyKind want;
  };
  static const Row kRows[] = {
      // ⚠ extended 位元:**編輯區**(方向鍵那一整塊)那幾顆是 extended,
      //   數字鍵台上同名的那幾顆不是(見 keymap.cc 的 LayoutIndependentKeysym)。
      //   兩者的 keysym 完全不同(XK_Left vs XK_KP_Left),所以兩組都要驗 ——
      //   漏掉數字鍵台那一組的話,NumLock 關著的使用者在組字中移不了游標,
      //   而那顆鍵會走到「沒有分類過 → 不吃 → 宿主拿去移它自己的游標」。
      {"BackSpace", kVkBack,   false, KeyKind::kEditing},
      {"Tab",       kVkTab,    false, KeyKind::kFinish},
      {"Return",    kVkReturn, false, KeyKind::kFinish},
      {"Escape",    kVkEscape, false, KeyKind::kFinish},
      {"Delete",    kVkDelete, true,  KeyKind::kEditing},
      {"Left",      kVkLeft,   true,  KeyKind::kNavigation},
      {"Right",     kVkRight,  true,  KeyKind::kNavigation},
      {"Up",        kVkUp,     true,  KeyKind::kNavigation},
      {"Down",      kVkDown,   true,  KeyKind::kNavigation},
      {"Home",      kVkHome,   true,  KeyKind::kNavigation},
      {"End",       kVkEnd,    true,  KeyKind::kNavigation},
      {"PageUp",    kVkPrior,  true,  KeyKind::kNavigation},
      {"PageDown",  kVkNext,   true,  KeyKind::kNavigation},
      // 數字鍵台(NumLock 關著):同樣的 VK,extended = false。
      {"KP_Delete", kVkDelete, false, KeyKind::kEditing},
      {"KP_Left",   kVkLeft,   false, KeyKind::kNavigation},
      {"KP_Right",  kVkRight,  false, KeyKind::kNavigation},
      {"KP_Up",     kVkUp,     false, KeyKind::kNavigation},
      {"KP_Down",   kVkDown,   false, KeyKind::kNavigation},
      {"KP_Home",   kVkHome,   false, KeyKind::kNavigation},
      {"KP_End",    kVkEnd,    false, KeyKind::kNavigation},
      {"KP_PageUp", kVkPrior,  false, KeyKind::kNavigation},
      {"KP_PageDn", kVkNext,   false, KeyKind::kNavigation},
  };
  // 編輯區與數字鍵台真的映到**不同**的 keysym —— 不然上面那二十幾格裡
  // 有一半是重複的,而「兩組都驗過了」是假的。
  CHECK(Ks(kVkLeft, true) != Ks(kVkLeft, false));
  CHECK(Ks(kVkDelete, true) != Ks(kVkDelete, false));
  for (const Row& r : kRows) {
    const int32_t ks = Ks(r.vk, r.extended);
    CHECK(ks != 0);
    const KeyKind k = ClassifyKeyKind(ks, 0);
    CHECK_STR(std::string(r.name) + ":" + KeyKindTag(k),
              std::string(r.name) + ":" + KeyKindTag(r.want));
    CHECK(!ShouldEatKey(k, /*composing=*/false));
    CHECK(ShouldEatKey(k, /*composing=*/true));
    CHECK(!ShouldSelfInsert(k));
  }
}

// ── 字元鍵:兩種狀態都吃,而且引擎不處理時我們自己補 ────────────────
//
// 為什麼不能照「有沒有組字」判斷:**哪些字元會起頭是方案決定的**。
// 朗月拼音的數字不起頭,注音的數字起頭(1 是ㄅ)。DLL 不知道方案,
// 所以一律吃 + 自己補,才不會有任何一種方案的使用者打不出第一個字。
TEST(character_keys_are_always_eaten_and_self_inserted) {
  const int32_t letters[] = {'a', 'z', 'A', 'Z'};
  const int32_t digits[] = {'0', '5', '9'};
  const int32_t punct[] = {',', '.', ';', '/', '-', '\''};
  for (int32_t ks : letters) {
    const KeyKind k = ClassifyKeyKind(ks, 0);
    CHECK(k == KeyKind::kCharacter);
    CHECK(ShouldEatKey(k, false));
    CHECK(ShouldEatKey(k, true));
    CHECK(ShouldSelfInsert(k));
  }
  for (int32_t ks : digits) {
    const KeyKind k = ClassifyKeyKind(ks, 0);
    CHECK(k == KeyKind::kCharacter);
    CHECK(ShouldEatKey(k, false));  // ← 注音的 1 = ㄅ,不吃就打不出第一個字
    CHECK(ShouldSelfInsert(k));
  }
  for (int32_t ks : punct) {
    const KeyKind k = ClassifyKeyKind(ks, 0);
    CHECK(k == KeyKind::kCharacter);
    CHECK(ShouldEatKey(k, false));
  }
  // 空白鍵。keymap 把它映成 0x20,所以它是字元 —— 沒有組字時使用者按空白
  // 就是要一個空白,而引擎不處理它,於是由我們補進文件。
  const int32_t space = Ks(kVkSpace);
  CHECK_INT(space, 0x20);
  CHECK(ClassifyKeyKind(space, 0) == KeyKind::kCharacter);
  CHECK(ShouldEatKey(ClassifyKeyKind(space, 0), false));
  // Shift 是字元的一部分,不會把字元鍵變成宿主的鍵。
  CHECK(ClassifyKeyKind('A', kShift) == KeyKind::kCharacter);
}

// ── Ctrl / Alt / Win 的組合鍵一律不吃 ──────────────────────────────
//
// ⚠ Ctrl+C 的 keysym 就是 'c'。舊規則(「映得出 keysym 就吃」)會把它吃掉,
//   然後引擎不處理 —— 使用者複製不了東西,而且完全不知道為什麼。
//   這一條與退格是同一個根因的兩個出口。
TEST(modifier_combos_belong_to_the_host) {
  CHECK(ClassifyKeyKind('c', kCtrl) == KeyKind::kHostOnly);
  CHECK(ClassifyKeyKind('v', kCtrl) == KeyKind::kHostOnly);
  CHECK(ClassifyKeyKind('a', kCtrl) == KeyKind::kHostOnly);
  CHECK(ClassifyKeyKind('z', kCtrl) == KeyKind::kHostOnly);
  CHECK(ClassifyKeyKind(Ks(kVkF4), kAlt) == KeyKind::kHostOnly);
  CHECK(ClassifyKeyKind(Ks(kVkBack), kCtrl) == KeyKind::kHostOnly);
  CHECK(ClassifyKeyKind(Ks(kVkLeft), kCtrl) == KeyKind::kHostOnly);
  for (bool composing : {false, true}) {
    CHECK(!ShouldEatKey(ClassifyKeyKind('c', kCtrl), composing));
    CHECK(!ShouldEatKey(ClassifyKeyKind(Ks(kVkF4), kAlt), composing));
  }
}

// ── 我們一件事都沒實作的鍵,不可以吃 ───────────────────────────────
TEST(keys_we_never_implemented_are_never_eaten) {
  const int32_t never[] = {
      Ks(kVkF1), Ks(kVkF4), Ks(kVkInsert),
      0xFF61 /* Print */, 0xFF7F /* NumLock */, 0xFFE1 /* Shift_L */,
  };
  for (int32_t ks : never) {
    CHECK(ks != 0);
    const KeyKind k = ClassifyKeyKind(ks, 0);
    CHECK(k == KeyKind::kHostOnly);
    for (bool composing : {false, true}) CHECK(!ShouldEatKey(k, composing));
  }
  // 映不出 keysym 的鍵。
  CHECK(ClassifyKeyKind(0, 0) == KeyKind::kUnmappable);
  CHECK(!ShouldEatKey(KeyKind::kUnmappable, false));
  CHECK(!ShouldEatKey(KeyKind::kUnmappable, true));
}

// ── 反向守門:「沒有分類過」的預設方向必須是不吃 ────────────────────
//
// 日後 keymap 多支援一顆鍵而這裡忘了分類時,那顆鍵應該是「進不了引擎」
// (看得見、查得出來),不是「被吃掉但沒人做事」(壞掉的鍵盤)。
TEST(unclassified_keysyms_default_to_not_eaten) {
  // X11 裡的一段功能鍵區,我們沒有分類過。
  const int32_t unclassified[] = {0xFF20 /* Multi_key 附近 */, 0xFF6B,
                                  0xFF7A, 0xFFD8 /* F27 */};
  for (int32_t ks : unclassified) {
    const KeyKind k = ClassifyKeyKind(ks, 0);
    CHECK(k == KeyKind::kHostOnly);
    CHECK(!ShouldEatKey(k, false));
    CHECK(!ShouldEatKey(k, true));
  }
}

// ── 自己補字元時,補的必須是**佈局算出來的那個字**,不是猜的 ─────────
TEST(self_inserted_character_matches_the_keysym) {
  CHECK_INT(CharForSelfInsert('a'), (int32_t)U'a');
  CHECK_INT(CharForSelfInsert('Z'), (int32_t)U'Z');
  CHECK_INT(CharForSelfInsert('5'), (int32_t)U'5');
  CHECK_INT(CharForSelfInsert(0x20), (int32_t)U' ');
  CHECK_INT(CharForSelfInsert(0xFC), (int32_t)U'\u00FC');  // 德文 ü
  // X11 對非 Latin-1 的規則:0x01000000 | 碼點。
  CHECK_INT(CharForSelfInsert(0x01000100), 0x0100);
  // 數字鍵台:keysym 不是 '5',但使用者要的是 5。
  CHECK_INT(CharForSelfInsert(0xFFB5), (int32_t)U'5');
  CHECK_INT(CharForSelfInsert(0xFFAE), (int32_t)U'.');
  // 功能鍵沒有字元可以補 —— 補了就是憑空多出一個字。
  CHECK_INT(CharForSelfInsert(0xFF08), 0);  // BackSpace
  CHECK_INT(CharForSelfInsert(0xFF51), 0);  // Left
  CHECK_INT(CharForSelfInsert(0), 0);
  // 每一顆被分類成 kCharacter 的鍵都補得出字元 —— 不然它會變成
  // 「吃掉了、引擎不處理、也補不出東西」,也就是黑洞。
  const int32_t chars[] = {'a', 'Z', '0', '9', ',', '.', 0x20, 0xFC,
                           0xFFB0, 0xFFB9, 0xFFAA, 0xFFAF, 0x01000100};
  for (int32_t ks : chars) {
    CHECK(ClassifyKeyKind(ks, 0) == KeyKind::kCharacter);
    CHECK(CharForSelfInsert(ks) != 0);
  }
}

// 每一族都要有標籤(記錄檔與 verify_input_matrix.sh 比對的是標籤)。
TEST(every_key_kind_has_a_tag) {
  const KeyKind all[] = {KeyKind::kUnmappable, KeyKind::kCharacter,
                         KeyKind::kEditing,    KeyKind::kNavigation,
                         KeyKind::kFinish,     KeyKind::kHostOnly};
  for (KeyKind k : all) {
    CHECK(KeyKindTag(k) != nullptr);
    CHECK(std::string(KeyKindTag(k)) != "?");
  }
}
