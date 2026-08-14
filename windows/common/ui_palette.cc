#include "ui_palette.h"

#include <cmath>
#include <cstddef>

#include "ui_accent.h"

namespace rimewin {
namespace {

constexpr Rgb H(uint32_t v) {
  return Rgb{static_cast<uint8_t>((v >> 16) & 0xFF),
             static_cast<uint8_t>((v >> 8) & 0xFF),
             static_cast<uint8_t>(v & 0xFF)};
}

// ⚠ accent 衍生的九格在常數表裡是這個哨兵色。忘了填的樣子本來是
//   「那一塊變成別的顏色」—— 在畫面上只像配色怪,不像壞掉。
//   洋紅在我們的色票裡不存在,所以測試抓得到。
constexpr uint32_t kSentinel = 0xFF00FF;

// ── 淺色(§3.4 + §12.6.1 + §12.14.2 C)──────────────────────────
const Rgb kLight[kRoleCount] = {
    H(0xF4F6F5),   // kBackground
    H(0xFFFFFF),   // kSurface
    H(0xF2F5F4),   // kSurfaceVariant
    H(0x14181A),   // kOnSurface
    H(0x454F51),   // kOnSurfaceVariant
    H(kSentinel),  // kPrimary                 ← accent 衍生
    H(kSentinel),  // kOnPrimary               ← accent 衍生
    H(kSentinel),  // kPrimaryContainer        ← accent 衍生
    H(0xB3261E),   // kError            ⚠ 危險按鈕的文字與外框
    H(0xF9DEDC),   // kErrorContainer
    H(0xC4CDCC),   // kOutline          ⚠ §3.4.1 規範性的改動,不是 #E6EAE9
    H(0xECEDED),   // kRowHover
    H(0xE3E3E4),   // kRowPressed
    H(kSentinel),  // kPrimaryContainerHover   ← accent 衍生
    H(kSentinel),  // kPrimaryContainerPressed ← accent 衍生
    H(0xB8BCBD),   // kDisabledText     ⚠ 刻意不合 4.5:1,見 §12.6.3 的豁免
    H(0xF9EEED),   // kDangerHover
    H(0xF6E5E4),   // kDangerPressed
    H(kSentinel),  // kPrimaryHover            ← accent 衍生
    H(kSentinel),  // kPrimaryPressed          ← accent 衍生
    H(kSentinel),  // kAccentText              ← accent 衍生
    H(kSentinel),  // kAccentIndicator         ← accent 衍生
    H(0xF8F8F8),   // kControlFill
    H(0xEFEFEF),   // kControlFillHover
    H(0xE5E6E6),   // kControlFillPressed
    H(0xCECECF),   // kControlBorder
    H(0xF1F1F1),   // kBadgeFill
    H(0x1B1B1B),   // kFocusOuter       ⚠ 焦點環**不用** accent,見 §12.14.2 末段
    H(0xFFFFFF),   // kFocusInner
};

// ── 深色(§3.4 + §12.6.2 + §12.14.2 C)──────────────────────────
const Rgb kDark[kRoleCount] = {
    H(0x0D1012),   // kBackground       ⚠ 不用純黑(§3.5 第 4 條)
    H(0x171B1D),   // kSurface
    H(0x121618),   // kSurfaceVariant
    H(0xECEFEE),   // kOnSurface        ⚠ 不用純白(同上)
    H(0xB9C3C2),   // kOnSurfaceVariant
    H(kSentinel),  // kPrimary
    H(kSentinel),  // kOnPrimary
    H(kSentinel),  // kPrimaryContainer
    H(0xFF8A80),   // kError
    H(0x3A1512),   // kErrorContainer
    H(0x363E40),   // kOutline
    H(0x2C3032),   // kRowHover
    H(0x393D3E),   // kRowPressed
    H(kSentinel),  // kPrimaryContainerHover
    H(kSentinel),  // kPrimaryContainerPressed
    H(0x555B5C),   // kDisabledText
    H(0x2E2627),   // kDangerHover
    H(0x3C2D2D),   // kDangerPressed
    H(kSentinel),  // kPrimaryHover
    H(kSentinel),  // kPrimaryPressed
    H(kSentinel),  // kAccentText
    H(kSentinel),  // kAccentIndicator
    H(0x25292B),   // kControlFill
    H(0x313436),   // kControlFillHover
    H(0x1E2224),   // kControlFillPressed  ⚠ 深色是**變暗**,Win11 就是這樣
    H(0x434648),   // kControlBorder
    H(0x2E3234),   // kBadgeFill
    H(0xFFFFFF),   // kFocusOuter
    H(0x1B1B1B),   // kFocusInner
};

static_assert(sizeof(kLight) / sizeof(kLight[0]) == kRoleCount,
              "淺色色票少了一個角色 —— 兩份色票的鍵名集合必須完全相同(§2-F4)");
static_assert(sizeof(kDark) / sizeof(kDark[0]) == kRoleCount,
              "深色色票少了一個角色 —— 兩份色票的鍵名集合必須完全相同(§2-F4)");
static_assert(kRoleCount == 29,
              "§12.14.2 的角色數。改了就要回來改規格那張表,反過來也是。");

// windows.h 的 COLOR_* 數值。本檔刻意不 include windows.h(見 CMakeLists),
// 所以在這裡寫成常數並註明來源。
enum : int {
  kSysWindow = 5,          // COLOR_WINDOW
  kSysWindowText = 8,      // COLOR_WINDOWTEXT
  kSysHighlight = 13,      // COLOR_HIGHLIGHT
  kSysHighlightText = 14,  // COLOR_HIGHLIGHTTEXT
  kSysBtnFace = 15,        // COLOR_BTNFACE
  kSysGrayText = 17,       // COLOR_GRAYTEXT
  kSysBtnText = 18,        // COLOR_BTNTEXT
  kSysHotlight = 26,       // COLOR_HOTLIGHT
};

}  // namespace

Palette PaletteFor(Mode m, Rgb accent_seed) {
  // ⚠ kHighContrast 不在這裡回答 —— 它的顏色來自 GetSysColor()。
  //   呼叫端拿 SysColorFor() 自己填一份。回淺色是為了讓
  //   「忘了處理高對比」的後果是「淺色」而不是一片黑,但那條路
  //   W13 在守(必須有 SPI_GETHIGHCONTRAST 分支且走 GetSysColor)。
  const bool dark = m == Mode::kDark;
  const Rgb* base = dark ? kDark : kLight;
  Palette p{};
  for (int i = 0; i < kRoleCount; ++i) p.role[i] = base[i];

  const AccentRoles a =
      DeriveAccentRoles(accent_seed, dark, p.role[kSurface],
                        p.role[kBackground], p.role[kOnSurface]);
  p.role[kPrimary] = a.primary;
  p.role[kPrimaryHover] = a.primary_hover;
  p.role[kPrimaryPressed] = a.primary_pressed;
  p.role[kOnPrimary] = a.on_primary;
  p.role[kAccentText] = a.accent_text;
  p.role[kAccentIndicator] = a.accent_indicator;
  p.role[kPrimaryContainer] = a.container;
  p.role[kPrimaryContainerHover] = a.container_hover;
  p.role[kPrimaryContainerPressed] = a.container_pressed;
  return p;
}

int SysColorFor(Role role) {
  switch (role) {
    case kBackground:
    case kSurface:
      return kSysWindow;
    case kSurfaceVariant:
      return kSysBtnFace;
    case kOnSurface:
      return kSysWindowText;
    case kOnSurfaceVariant:
      return kSysBtnText;
    // 高對比下「重點」與「選中」一律走系統的選取色 —— 使用者選的那一組
    // 顏色本來就是為了讓選取狀態看得出來,我們的青瓷綠在這裡只會礙事。
    case kPrimary:
    case kPrimaryHover:
    case kPrimaryPressed:
    case kAccentText:
    case kAccentIndicator:
      return kSysHotlight;
    case kOnPrimary:
    case kPrimaryContainerHover:
    case kPrimaryContainerPressed:
      return kSysHighlightText;
    case kPrimaryContainer:
      return kSysHighlight;
    // 高對比佈景沒有「危險色」這個概念。用一般文字色,並靠外框與
    // 文案表達危險 —— §3.4 第 2 條本來就要求「不得只用顏色傳達資訊」。
    case kError:
      return kSysWindowText;
    case kErrorContainer:
    case kRowHover:
    case kRowPressed:
    case kDangerHover:
    case kDangerPressed:
      return kSysBtnFace;
    // 控制項的底在高對比下是按鈕面,外框是文字色 —— 使用者選的佈景
    // 已經替我們決定了「一塊可按的東西長什麼樣」。
    case kControlFill:
    case kControlFillHover:
    case kControlFillPressed:
    case kBadgeFill:
      return kSysBtnFace;
    case kControlBorder:
    case kOutline:
      return kSysWindowText;
    // ⚠ 高對比下的焦點環仍然是兩圈互為反色。外圈用文字色、內圈用底色
    //   —— 兩者在高對比佈景裡的對比一定是最大的那一組。
    case kFocusOuter:
      return kSysWindowText;
    case kFocusInner:
      return kSysWindow;
    case kDisabledText:
      return kSysGrayText;
    case kRoleCount:
    default:
      return kSysWindowText;
  }
}

// ── WCAG 2.1 相對亮度 ────────────────────────────────────────────

namespace {
double Chan(uint8_t v) {
  const double s = v / 255.0;
  return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
}
}  // namespace

double RelativeLuminance(Rgb c) {
  return 0.2126 * Chan(c.r) + 0.7152 * Chan(c.g) + 0.0722 * Chan(c.b);
}

double ContrastRatio(Rgb a, Rgb b) {
  const double la = RelativeLuminance(a);
  const double lb = RelativeLuminance(b);
  const double hi = la > lb ? la : lb;
  const double lo = la > lb ? lb : la;
  return (hi + 0.05) / (lo + 0.05);
}

Rgb ScrollFadeMix(Rgb from, Rgb to, int band, int bands) {
  if (bands <= 0 || band >= bands) return to;
  if (band <= 0) return from;
  // ⚠ 在 8 位元的分量上做整數內插,四捨五入 —— 截斷的話深色下每一帶
  //   都往暗的一邊偏一格,而八帶累積起來看得出一條階梯。
  auto mix = [&](uint8_t a, uint8_t b) {
    const int num = static_cast<int>(a) * (bands - band) +
                    static_cast<int>(b) * band;
    return static_cast<uint8_t>((num + bands / 2) / bands);
  };
  return Rgb{mix(from.r, to.r), mix(from.g, to.g), mix(from.b, to.b)};
}

}  // namespace rimewin
