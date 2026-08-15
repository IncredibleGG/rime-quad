#include "ui_accent.h"

namespace rimewin {
namespace {

constexpr Rgb kWhite{0xFF, 0xFF, 0xFF};
constexpr Rgb kBlack{0x00, 0x00, 0x00};
// §12.14.1 第 1 道守門的兩個候選。近黑不是純黑 —— 與 §3.5 第 4 條
// 「深色下不用純黑」同一個理由。
constexpr Rgb kNearBlack{0x10, 0x10, 0x10};

// n / d,四捨六入五成雙(banker's rounding)。d > 0、n >= 0。
// ⚠ 「五成雙」不是品味:規格 §12.14.2 那張對照表是拿 Python 的 round()
//   算出來的,而它就是這一種。用「五入」會讓表與程式在三格上對不起來,
//   而那三格會被下一個人當成 bug 追。
long long RoundHalfEven(long long n, long long d) {
  long long q = n / d;
  const long long r = n - q * d;
  const long long twice = 2 * r;
  if (twice > d || (twice == d && (q & 1) != 0)) ++q;
  return q;
}

uint8_t Chan(int a, int b, int num, int den) {
  const long long v = RoundHalfEven(
      static_cast<long long>(a) * den + static_cast<long long>(b - a) * num,
      den);
  if (v <= 0) return 0;
  if (v >= 255) return 255;
  return static_cast<uint8_t>(v);
}

}  // namespace

Rgb Mix(Rgb a, Rgb b, int num, int den) {
  if (den <= 0) return a;
  return Rgb{Chan(a.r, b.r, num, den), Chan(a.g, b.g, num, den),
             Chan(a.b, b.b, num, den)};
}

AccentLadder LadderOf(Rgb base) {
  AccentLadder l;
  l.light3 = Mix(base, kWhite, 70, 100);
  l.light2 = Mix(base, kWhite, 50, 100);
  l.light1 = Mix(base, kWhite, 30, 100);
  l.base = base;
  l.dark1 = Mix(base, kBlack, 20, 100);
  l.dark2 = Mix(base, kBlack, 40, 100);
  l.dark3 = Mix(base, kBlack, 55, 100);
  return l;
}

bool AccentFromPalette(const uint8_t* bytes, size_t n, bool dark, Rgb* out) {
  // ⚠ 位元組數不是 32 就整段跳過。見標頭:那個佈局是流傳的知識,
  //   不是契約。錯的樣子若是「拿到半組 RGBA」,顏色會是隨機的;
  //   而「整段跳過」錯的樣子只是「用 DWM 那一段」。
  if (!bytes || n != 32 || !out) return false;
  // 8 組 RGBA,每組 4 位元組。index 4 = AccentDark1(淺色用),
  // index 1 = AccentLight2(深色用)。
  const size_t idx = dark ? 1u : 4u;
  const uint8_t* p = bytes + idx * 4u;
  out->r = p[0];
  out->g = p[1];
  out->b = p[2];
  return true;
}

Rgb AccentFromDword(uint32_t v) {
  // 0x00BBGGRR。⚠ 位元組序與 hex 直覺相反 —— 寫錯的樣子是
  //   「顏色的紅藍互換」,那在藍色 accent(Win11 預設)上看起來
  //   像一個合理的橘色,不像壞掉。
  return Rgb{static_cast<uint8_t>(v & 0xFFu),
             static_cast<uint8_t>((v >> 8) & 0xFFu),
             static_cast<uint8_t>((v >> 16) & 0xFFu)};
}

Rgb AccentFallbackSeed() { return Rgb{0x1F, 0x6F, 0x63}; }

Rgb PushForContrast(Rgb base, Rgb fg, double want) {
  if (ContrastRatio(base, fg) >= want) return base;
  // 「與字相反」:字是亮的就往黑推,字是暗的就往白推。
  const Rgb target = RelativeLuminance(fg) > 0.5 ? kBlack : kWhite;
  // 8-bit 量化的二分搜尋。t = 255/255 是純黑/純白,對另一端的字
  // 一定過門檻,所以 hi 這一側恆為真,搜尋不會失敗。
  int lo = 0, hi = 255;
  while (lo < hi) {
    const int mid = (lo + hi) / 2;
    const Rgb c = Mix(base, target, mid, 255);
    if (ContrastRatio(c, fg) >= want)
      hi = mid;
    else
      lo = mid + 1;
  }
  return Mix(base, target, lo, 255);
}

Rgb PushForContrastTwo(Rgb base, Rgb bg_a, Rgb bg_b, double want,
                       bool toward_dark) {
  auto ok = [&](Rgb c) {
    return ContrastRatio(c, bg_a) >= want && ContrastRatio(c, bg_b) >= want;
  };
  if (ok(base)) return base;
  const Rgb target = toward_dark ? kBlack : kWhite;
  int lo = 0, hi = 255;
  while (lo < hi) {
    const int mid = (lo + hi) / 2;
    if (ok(Mix(base, target, mid, 255)))
      hi = mid;
    else
      lo = mid + 1;
  }
  return Mix(base, target, lo, 255);
}

AccentRoles DeriveAccentRoles(Rgb seed, bool dark, Rgb surface, Rgb background,
                              Rgb on_surface) {
  const AccentLadder l = LadderOf(seed);
  AccentRoles a;

  // ── 1. 實心底 + 底上的字 ────────────────────────────────────
  // 淺色用 Dark1、深色用 Light2(§12.16 第 4 條:來自 WinUI 的
  // AccentFillColorDefaultBrush,沒有讀過那份資源字典 —— 錯了的樣子是
  // 「顏色比系統的深/淺一階」,不是壞掉)。
  Rgb fill = dark ? l.light2 : l.dark1;
  // 守門一:兩個候選取對比高的那一個。淺 accent 自動拿到近黑字。
  const bool white_wins =
      ContrastRatio(fill, kWhite) >= ContrastRatio(fill, kNearBlack);
  a.on_primary = white_wins ? kWhite : kNearBlack;
  // 守門二:仍然不到 4.5 就把底往「與字相反」推。
  fill = PushForContrast(fill, a.on_primary, 4.5);
  a.primary = fill;

  // hover / 按下一律往「與字相反」的方向推 10% / 18%。
  // ⚠ Win11 自己是反過來的(accent 按鈕 hover 會變**淡**)。
  //   我們選相反的方向,理由是「按下去更沉」保證對比只會更高,
  //   而我們事先不知道使用者的 accent 有多亮。
  const Rgb away =
      RelativeLuminance(a.on_primary) > 0.5 ? kBlack : kWhite;
  a.primary_hover = Mix(a.primary, away, 10, 100);
  a.primary_pressed = Mix(a.primary, away, 18, 100);

  // ── 2. accent 當文字 ────────────────────────────────────────
  // 淺 Dark2 / 深 Light3,對 surface **與** background 兩個底都要 ≥ 4.5。
  a.accent_text = PushForContrastTwo(dark ? l.light3 : l.dark2, surface,
                                     background, 4.5, /*toward_dark=*/!dark);

  // ── 3. 側欄指示條 ───────────────────────────────────────────
  // = primary,對兩個底都要 ≥ 3.0(非文字門檻)。
  // ⚠ 八個 accent 裡只有黃色真的會動到這一格。拿掉這一道守門,
  //   黃色 accent 的使用者會拿到一條對背景 2.8:1 的指示條,
  //   而畫面上不會有任何東西看起來是錯的。
  a.accent_indicator = PushForContrastTwo(a.primary, surface, background, 3.0,
                                          /*toward_dark=*/!dark);

  // ── 4. 選中底 ───────────────────────────────────────────────
  a.container = Mix(surface, a.primary, dark ? 24 : 14, 100);
  // 選中底上放的是 onSurface(§12.14.6.1 / .5 的表),所以它也要過 4.5。
  a.container =
      PushForContrast(a.container, on_surface, 4.5);
  a.container_hover = Mix(a.container, on_surface, 6, 100);
  a.container_pressed = Mix(a.container, on_surface, 10, 100);
  return a;
}

}  // namespace rimewin
