// windows/common/ui_palette.h — 色票(§3.4 + §12.6 + §12.14.2)
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
// ── ⚠ 2026-08-14:九個角色不再是常數(§12.14.2 B)──────────────
//
// accent 由**使用者**在系統設定裡選,所以 primary / primaryHover /
// primaryPressed / onPrimary / accentText / accentIndicator /
// primaryContainer{,Hover,Pressed} 九格是**執行期算的**,不是表裡的常數。
// 因此 `PaletteFor()` 現在吃一個 accent 種子並**回傳一份值**,
// 不再回傳指向靜態表的指標。
//
// 常數表裡那九格寫的是一個大聲的哨兵色(洋紅),而
// `palette_no_role_is_left_as_sentinel` 這個測試會抓到任何一個
// 忘了填的角色 —— 忘了填的樣子本來是「那一塊變成別的顏色」,
// 在畫面上看起來只是「配色怪」,不像壞掉。
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

// §3.4 的 11 個語意角色 + §12.6 的狀態層 + §12.14.2 的 11 個新角色。
//
// ⚠ 狀態層是**預先算好的不透明色**,不是「疊 8% 的黑」:
//   GDI 的 FillRect 不做 alpha 混色,寫成不透明色是唯一畫得出來的形式。
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
  // ⚠ 舊名 kRowSelectedHover / kRowSelectedPressed。改名是因為
  //   §12.14.2 把它們定義成 mix(primaryContainer, onSurface, 6%/10%)
  //   —— 它們是「選中底」的狀態層,不是「列」的狀態層,
  //   而側欄與清單以外的地方(將來的卡片內列)也會用到。
  kPrimaryContainerHover,
  kPrimaryContainerPressed,
  kDisabledText,
  kDangerHover,
  kDangerPressed,
  // ── §12.14.2 新增的 11 個 ────────────────────────────────────
  kPrimaryHover,       // accent 衍生
  kPrimaryPressed,     // accent 衍生
  kAccentText,         // accent 衍生
  kAccentIndicator,    // accent 衍生:側欄選中項左緣那條 3×16
  kControlFill,        // 中性:次要按鈕、唯讀 EDIT、輸入框的底
  kControlFillHover,
  kControlFillPressed,
  kControlBorder,      // 中性:控制項與卡片的 1 DIP 外框
  kBadgeFill,          // 中性:「預設」徽章的底
  kFocusOuter,         // 中性:焦點環外圈 2 DIP
  kFocusInner,         // 中性:焦點環內圈 1 DIP
  kRoleCount,
};

// ⚠ 回傳的是**一份值**,不是指標 —— 見標頭:九個角色是算出來的。
struct Palette {
  Rgb role[kRoleCount];
  const Rgb& operator[](int i) const { return role[i]; }
  const Rgb& operator[](Role r) const { return role[static_cast<int>(r)]; }
};

enum class Mode { kLight, kDark, kHighContrast };

// 淺色／深色兩份。高對比那一份由 SysColorFor 決定,見檔頭。
//
// `accent_seed` 是系統 accent(或 AccentFallbackSeed())。
// ⚠ 種子**不是** primary:primary 是種子走過階梯與三道守門之後的結果。
Palette PaletteFor(Mode m, Rgb accent_seed);

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
