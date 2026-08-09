// windows/common/ui_palette.h — 色票(§3.4 + §12.6)
//
// ── 為什麼是一個 enum 索引的陣列,而不是三個具名結構 ────────────────
//
// §2-F4(以及 W11)要求:**深淺兩份色票的鍵名集合完全相同**,
// 而且**版面程式碼裡不得出現 `is_dark` 分支**。
//
// 用 `enum` 當索引 + 每一份色票一個 `Rgb[kRoleCount]` 陣列,加上
// `static_assert(std::size(kLight) == kRoleCount)`,這件事就從
// 「有一支腳本在掃」升級成**編譯期保證**:少一個角色編不過。
// 那比任何掃描都早、都硬,而且不會因為掃描範圍寫錯而變成綠的。
//
// 版面拿到的是 `const Palette&`,它不知道自己是哪一份 —— 所以
// 「深色模式只換色票,不動版面」不是一條紀律,是型別上唯一做得到的事。
//
// ── 高對比是第三份色票,不是深色的一種(§12.7.4)────────────────
//
// 使用者開高對比是因為他**看不清楚**,這時我們的青瓷綠不是幫忙。
// 所以 `Mode::kHighContrast` 的那一份不寫死顏色,由呼叫端用
// `GetSysColor()` 填 —— 那是唯一尊重使用者選的佈景的作法。
// 本檔只負責「哪一個角色該對到哪一個 COLOR_* 」的對照(SysColorFor)。
//
#ifndef RIMEWIN_UI_PALETTE_H_
#define RIMEWIN_UI_PALETTE_H_

#include <cstdint>

namespace rimewin {

struct Rgb {
  uint8_t r = 0, g = 0, b = 0;
};

// §3.4 的 11 個語意角色 + §12.6 的狀態層。
//
// ⚠ 狀態層是**預先算好的不透明色**,不是「疊 8% 的黑」:
//   GDI 的 FillRect 不做 alpha 混色,寫成不透明色是唯一畫得出來的形式。
//   產生它們的公式留在 §12.6(淺 hover 8%/pressed 12%、深 10%/16%),
//   日後改色票時照那個公式重算。
enum Role : int {
  // §3.4
  kBackground = 0,
  kSurface,
  kSurfaceVariant,
  kOnSurface,
  kOnSurfaceVariant,
  kPrimary,
  kOnPrimary,
  kPrimaryContainer,
  kError,
  kErrorContainer,
  kOutline,
  // §12.6 狀態層
  kRowHover,
  kRowPressed,
  kRowSelectedHover,
  kRowSelectedPressed,
  kDisabledText,
  kDangerHover,
  kDangerPressed,
  kRoleCount,
};

using Palette = Rgb[kRoleCount];

enum class Mode { kLight, kDark, kHighContrast };

// 淺色／深色兩份。高對比那一份由 SysColorFor 決定,見檔頭。
const Rgb* PaletteFor(Mode m);

// 高對比模式下,這個角色該讀哪一個 `GetSysColor()` 索引。
// 回傳的是 COLOR_* 的**數值**(本檔不 include windows.h,所以不能用巨集名)。
// 對照表寫在 .cc 裡,並附上每一格的理由。
int SysColorFor(Role role);

// ── 對比度(§3.4.1 / §12.6.3)────────────────────────────────────
//
// WCAG 2.1 的相對亮度。寫成純函式是為了讓 W22 成為**單元測試**而不是
// 一句「我算過了」—— §3.4.1 記著一次「只驗一半就宣告過關」的事故
// (第一版只算了對卡片的比值,對畫面底不合格),那正是這種東西該被測的理由。
double RelativeLuminance(Rgb c);
double ContrastRatio(Rgb a, Rgb b);

}  // namespace rimewin

#endif  // RIMEWIN_UI_PALETTE_H_
