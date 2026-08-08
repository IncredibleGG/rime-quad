// windows/tests/fake_layouts.h — 幾份真實鍵盤佈局的離線複本
//
// 用途:讓「按鍵映射受實體佈局影響」這件事在**沒有 Windows** 的機器上也測得到。
// 表的內容抄自各佈局的實際鍵位,不是編出來的。
//
// ── 一個必須講清楚的事實 ──────────────────────────────────────
//
// Windows 的佈局檔同時包含 scancode→VK 的對照表,所以**字母鍵的 VK 會跟著
// 字元走**:德文 QWERTZ 上產生 'z' 的那顆鍵回報的就是 VK_Z,Dvorak 上產生
// 'o' 的那顆鍵回報 VK_O。也就是說,對純字母而言,一張寫死的
// 「VK_A → 'a'」表在多數佈局上**碰巧**是對的。
//
// 真正會壞的是這三類,而它們都是輸入法天天用到的:
//
//   1. **OEM 標點鍵。** VK_OEM_1 在美式是 ';',德文是 'ü',法文是 '$'。
//      Rime 的 punctuator 大量使用 , . / ; ' 這些鍵 —— 寫死等於在非美式
//      鍵盤上把標點全部對錯。
//   2. **數字列。** 法文 AZERTY 不按 Shift 打出的是 'é',按了才是 '2'。
//      寫死 VK_2 → '2' 在法國使用者手上是直接錯的。
//   3. **AltGr 層。** 德文 AltGr+Q = '@',而 Windows 把 AltGr 回報成
//      Ctrl+Alt。認不出來的話,那顆鍵會被當成快捷鍵而永遠打不出字。
//
// 所以測試針對的是這三類,不是「Dvorak 的字母」。
//
#ifndef RIMEWIN_TESTS_FAKE_LAYOUTS_H_
#define RIMEWIN_TESTS_FAKE_LAYOUTS_H_

#include <map>
#include <string>

#include "../common/keymap.h"

namespace rimewin_test {

struct LayoutEntry {
  char32_t base = 0;
  char32_t shifted = 0;
  char32_t altgr = 0;
  bool dead_base = false;
};

class FakeOracle : public rimewin::KeyboardOracle {
 public:
  FakeOracle(std::string name, bool has_altgr) : name_(std::move(name)), altgr_(has_altgr) {}

  FakeOracle& Add(uint32_t vk, char32_t base, char32_t shifted,
                  char32_t altgr = 0, bool dead_base = false) {
    map_[vk] = LayoutEntry{base, shifted, altgr, dead_base};
    return *this;
  }

  char32_t Translate(uint32_t vk, uint32_t /*scan*/, bool shift, bool caps,
                     bool altgr, bool* out_is_dead) const override {
    *out_is_dead = false;
    auto it = map_.find(vk);
    if (it == map_.end()) return 0;
    const LayoutEntry& e = it->second;
    if (altgr) return e.altgr;  // AltGr 層沒東西就是沒東西
    // CapsLock 只作用在字母上,且與 Shift 互斥 —— 這是 Windows 的行為。
    const bool upper = IsLetter(e.base) ? (shift != caps) : shift;
    const char32_t ch = upper ? e.shifted : e.base;
    if (ch != 0 && e.dead_base && !upper) *out_is_dead = true;
    return ch;
  }

  bool HasAltGr() const override { return altgr_; }
  const std::string& name() const { return name_; }

 private:
  static bool IsLetter(char32_t c) {
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') ||
           (c >= 0xE0 && c <= 0xFF);  // 拉丁附加字母(ä ö ü é …)
  }
  std::string name_;
  bool altgr_;
  std::map<uint32_t, LayoutEntry> map_;
};

// ── VK 常數(只列測試用得到的)──────────────────────────────────
constexpr uint32_t kVkA = 0x41, kVkQ = 0x51, kVkZ = 0x5A, kVk2 = 0x32,
                   kVk0 = 0x30, kVkOem1 = 0xBA, kVkOem2 = 0xBF,
                   kVkOemComma = 0xBC, kVkOemPeriod = 0xBE, kVkOem3 = 0xC0,
                   kVkOem7 = 0xDE, kVkOemPlus = 0xBB, kVkOemMinus = 0xBD;

// 美式 QWERTY(00000409)。沒有 AltGr。
inline FakeOracle MakeUS() {
  FakeOracle o("us", /*has_altgr=*/false);
  o.Add(kVkA, U'a', U'A')
      .Add(kVkQ, U'q', U'Q')
      .Add(kVkZ, U'z', U'Z')
      .Add(kVk2, U'2', U'@')
      .Add(kVk0, U'0', U')')
      .Add(kVkOem1, U';', U':')
      .Add(kVkOem2, U'/', U'?')
      .Add(kVkOemComma, U',', U'<')
      .Add(kVkOemPeriod, U'.', U'>')
      .Add(kVkOem3, U'`', U'~')
      .Add(kVkOem7, U'\'', U'"')
      .Add(kVkOemPlus, U'=', U'+')
      .Add(kVkOemMinus, U'-', U'_');
  return o;
}

// 德文 QWERTZ(00000407)。有 AltGr。
inline FakeOracle MakeDE() {
  FakeOracle o("de", /*has_altgr=*/true);
  o.Add(kVkA, U'a', U'A')
      .Add(kVkQ, U'q', U'Q', U'@')      // AltGr+Q = @
      .Add(kVkZ, U'z', U'Z')            // VK 跟著字元走,德文的 z 仍是 VK_Z
      .Add(kVk2, U'2', U'"', 0x00B2)   // AltGr+2 = ²
      .Add(kVkOem1, U'ü', U'Ü')         // ← 美式是 ';'
      .Add(kVkOem3, U'ö', U'Ö')         // ← 美式是 '`'
      .Add(kVkOem7, U'ä', U'Ä')         // ← 美式是 '\''
      .Add(kVkOem2, U'#', U'\'')
      .Add(kVkOemComma, U',', U';')
      .Add(kVkOemPeriod, U'.', U':')
      .Add(kVkOemPlus, U'+', U'*', U'~');
  return o;
}

// 法文 AZERTY(0000040C)。有 AltGr,且數字列不按 Shift 是字母。
inline FakeOracle MakeFR() {
  FakeOracle o("fr", /*has_altgr=*/true);
  o.Add(kVkA, U'q', U'Q')            // AZERTY:VK_A 這顆位置在法文上是 q?
      .Add(kVkQ, U'a', U'A')         //  ——(見下方註)
      .Add(kVk2, U'é', U'2', U'~')   // ⚠ 不按 Shift 是 é,按了才是 2
      .Add(kVk0, U'à', U'0', U'@')   // AltGr+0 = @
      .Add(kVkOem1, U'$', U'£', 0x00A4)
      .Add(kVkOemComma, U',', U'?')
      .Add(kVkOemPeriod, U';', U'.');
  return o;
}
// 註:法文佈局在 Windows 上其實也會重排 VK(產生 'a' 的鍵回報 VK_A)。
// 這裡刻意把 VK_A/VK_Q 對調,是要當作「位置式佈局」的代表,
// 用來測 MapKey 完全不看 VK 猜字元 —— 見 test_keymap.cc 的
// keymap_same_vk_differs_across_layouts。

}  // namespace rimewin_test

#endif  // RIMEWIN_TESTS_FAKE_LAYOUTS_H_
