// windows/tests/test_ui_palette.cc — 色票對比度(W22')、accent 守門(W30')
//
// §3.4.1 記著一次事故:第一版的 outline 只算了對卡片的比值(1.45 合格),
// 但對畫面底只有 1.34 —— **「只驗一半就宣告過關」**。所以這裡的每一組
// 都把兩個底都算,而且是表驅動的:加一個角色就得回來加一列。
//
// ── ⚠ 2026-08-14:W22 改成表驅動,因為 accent 不是我們的了 ────────
//
// §12.14.2 之後,九個角色由**使用者選的** accent 算出來。所以
// 「我算過那九組配對」這句話沒有意義 —— 要算的是「**任何一個** accent
// 都過門檻」。這裡餵 §12.14.1 那八個 accent(含 Windows 挑色盤裡最難的
// #FFB900 黃與 #00CC6A 亮綠)× 深淺兩份 × 十組配對。
//
// 反向(§2-G):把 ui_accent.cc 的 PushForContrastTwo 那一道拿掉,
// 黃色那一格會紅 —— 而畫面上不會有任何東西看起來是錯的。

#include "../common/ui_palette.h"

#include "../common/ui_accent.h"
#include "check.h"

using namespace rimewin;

namespace {

Rgb Hex(uint32_t v) {
  return Rgb{static_cast<uint8_t>((v >> 16) & 0xFF),
             static_cast<uint8_t>((v >> 8) & 0xFF),
             static_cast<uint8_t>(v & 0xFF)};
}

// §12.14.1 那張驗算表的八個 accent。⚠ W30 要求 N ≥ 8,而且
// **每一個都要在測試資料裡** —— 少了黃色,這整條守門就只是裝飾。
struct Seed {
  const char* what;
  uint32_t hex;
};
const Seed kSeeds[] = {
    {"win11 default blue", 0x0078D4}, {"celadon fallback", 0x1F6F63},
    {"yellow (hardest)", 0xFFB900},   {"bright green", 0x00CC6A},
    {"purple", 0x8764B8},             {"red", 0xE81123},
    {"grey", 0x767676},               {"bright red", 0xFF4343},
};
constexpr int kSeedCount = sizeof(kSeeds) / sizeof(kSeeds[0]);

// W30 的十組。⚠ 每一個 accent 的配對數要 ≥ 9。
struct Pair {
  const char* what;
  Role fg;
  Role bg;
  double threshold;
};
const Pair kAccentPairs[] = {
    {"onPrimary / primary", kOnPrimary, kPrimary, 4.5},
    {"onPrimary / primaryHover", kOnPrimary, kPrimaryHover, 4.5},
    {"onPrimary / primaryPressed", kOnPrimary, kPrimaryPressed, 4.5},
    {"accentText / surface", kAccentText, kSurface, 4.5},
    {"accentText / background", kAccentText, kBackground, 4.5},
    {"accentIndicator / surface", kAccentIndicator, kSurface, 3.0},
    {"accentIndicator / background", kAccentIndicator, kBackground, 3.0},
    {"onSurface / primaryContainer", kOnSurface, kPrimaryContainer, 4.5},
    {"onSurface / primaryContainerHover", kOnSurface, kPrimaryContainerHover,
     4.5},
    {"onSurface / primaryContainerPressed", kOnSurface,
     kPrimaryContainerPressed, 4.5},
};
constexpr int kAccentPairCount = sizeof(kAccentPairs) / sizeof(kAccentPairs[0]);

// §12.6.3 + §12.14.2 C 的中性配對(**不隨 accent 變**)。
const Pair kNeutralPairs[] = {
    {"primary text / normal bg", kOnSurface, kSurface, 4.5},
    {"secondary text / normal bg", kOnSurfaceVariant, kSurface, 4.5},
    {"primary text / hover bg", kOnSurface, kRowHover, 4.5},
    {"primary text / pressed bg", kOnSurface, kRowPressed, 4.5},
    {"danger / normal bg", kError, kSurface, 4.5},
    {"danger / danger hover bg", kError, kDangerHover, 4.5},
    {"divider / normal bg", kOutline, kSurface, 1.4},
    // §12.14.2 C 的實算表。
    {"secondary button text / controlFill", kOnSurface, kControlFill, 4.5},
    {"secondary button text / controlFillHover", kOnSurface, kControlFillHover,
     4.5},
    {"secondary button text / controlFillPressed", kOnSurface,
     kControlFillPressed, 4.5},
    {"danger / controlFill", kError, kControlFill, 4.5},
    {"badge text / badgeFill", kOnSurfaceVariant, kBadgeFill, 4.5},
    {"controlBorder / surface", kControlBorder, kSurface, 1.4},
    {"controlBorder / controlFill", kControlBorder, kControlFill, 1.4},
    {"focusOuter / surface", kFocusOuter, kSurface, 3.0},
    {"focusOuter / background", kFocusOuter, kBackground, 3.0},
    {"focusOuter / controlFill", kFocusOuter, kControlFill, 3.0},
    {"focusInner / focusOuter", kFocusInner, kFocusOuter, 3.0},
};
constexpr int kNeutralPairCount =
    sizeof(kNeutralPairs) / sizeof(kNeutralPairs[0]);

void Expect(const Palette& p, const Pair& pr, const char* seed_what,
            const char* mode_what, int line) {
  const double r = ContrastRatio(p[pr.fg], p[pr.bg]);
  if (r >= pr.threshold) return;
  char buf[320];
  std::snprintf(buf, sizeof(buf), "%s / accent=%s / %s = %.2f < %.2f", pr.what,
                seed_what, mode_what, r, pr.threshold);
  ::rimewin_test::Fail(__FILE__, line, buf);
}

}  // namespace

// ── W30':accent 衍生的每一個角色,在八個 accent 底下都過門檻 ─────
TEST(accent_derived_roles_meet_thresholds_for_every_accent) {
  int checked = 0;
  int accents = 0;
  for (int s = 0; s < kSeedCount; ++s) {
    ++accents;
    for (Mode m : {Mode::kLight, Mode::kDark}) {
      const Palette p = PaletteFor(m, Hex(kSeeds[s].hex));
      for (int i = 0; i < kAccentPairCount; ++i) {
        Expect(p, kAccentPairs[i], kSeeds[s].what,
               m == Mode::kLight ? "light" : "dark", __LINE__);
        ++checked;
      }
    }
  }
  // ⚠ 範圍斷言(§2-G2):accent 數 ≥ 8、每個 accent 的配對數 ≥ 9。
  //   表寫空了會讓這個測試「全部通過」—— 那正是要堵死的失效方式。
  CHECK(accents >= 8);
  CHECK(kAccentPairCount >= 9);
  CHECK_INT(checked, kSeedCount * 2 * kAccentPairCount);
  CHECK(checked >= 8 * 2 * 9);
}

// ── W22':中性配對(不隨 accent 變,但仍然兩份都算)───────────────
TEST(palette_every_neutral_pair_meets_its_threshold_in_both_modes) {
  int checked = 0;
  for (Mode m : {Mode::kLight, Mode::kDark}) {
    const Palette p = PaletteFor(m, AccentFallbackSeed());
    for (int i = 0; i < kNeutralPairCount; ++i) {
      Expect(p, kNeutralPairs[i], "fallback",
             m == Mode::kLight ? "light" : "dark", __LINE__);
      ++checked;
    }
  }
  CHECK(checked >= 18);
  CHECK_INT(checked, 2 * kNeutralPairCount);
}

TEST(palette_divider_is_checked_against_both_backgrounds) {
  // §3.4.1 的教訓:同一個 outline 既畫在卡片裡,也畫在畫面底上。
  // 只算一個底就宣告合格,是這份規範自己記下來的錯誤。
  for (Mode m : {Mode::kLight, Mode::kDark}) {
    const Palette p = PaletteFor(m, AccentFallbackSeed());
    CHECK(ContrastRatio(p[kOutline], p[kSurface]) >= 1.4);
    CHECK(ContrastRatio(p[kOutline], p[kBackground]) >= 1.4);
    CHECK(ContrastRatio(p[kControlBorder], p[kSurface]) >= 1.4);
    CHECK(ContrastRatio(p[kControlBorder], p[kBackground]) >= 1.4);
  }
}

TEST(palette_no_role_is_left_as_sentinel) {
  // ⚠ accent 衍生的九格在常數表裡是洋紅哨兵。忘了填一格的樣子是
  //   「那一塊變成別的顏色」—— 在畫面上只像配色怪,不像壞掉。
  const Rgb sentinel{0xFF, 0x00, 0xFF};
  for (int s = 0; s < kSeedCount; ++s)
    for (Mode m : {Mode::kLight, Mode::kDark}) {
      const Palette p = PaletteFor(m, Hex(kSeeds[s].hex));
      for (int i = 0; i < kRoleCount; ++i) {
        const bool is_sentinel = p[i].r == sentinel.r && p[i].g == sentinel.g &&
                                 p[i].b == sentinel.b;
        CHECK(!is_sentinel);
      }
    }
}

TEST(palette_disabled_text_is_deliberately_exempt) {
  // §12.6.3:停用文字**刻意**不符合 4.5:1,而且這是規範性的豁免 ——
  // 做到 4.5:1 的結果是它看起來沒有被停用。
  for (Mode m : {Mode::kLight, Mode::kDark}) {
    const Palette p = PaletteFor(m, AccentFallbackSeed());
    const double r = ContrastRatio(p[kDisabledText], p[kSurface]);
    CHECK(r < 4.5);
    CHECK(r > 1.5);  // 但也不可以低到完全看不見
  }
}

TEST(palette_two_modes_have_identical_role_sets) {
  // W11 / §2-F4。⚠ 這在 C++ 裡其實是**編譯期**保證(ui_palette.cc 的
  // static_assert)。這裡再驗一次執行期的形狀。
  const Palette l = PaletteFor(Mode::kLight, AccentFallbackSeed());
  const Palette d = PaletteFor(Mode::kDark, AccentFallbackSeed());
  int differ = 0;
  for (int i = 0; i < kRoleCount; ++i) {
    const bool same = l[i].r == d[i].r && l[i].g == d[i].g && l[i].b == d[i].b;
    if (!same) ++differ;
  }
  CHECK_INT(kRoleCount, 29);         // §12.14.2 的角色數
  CHECK(differ >= kRoleCount - 2);   // 幾乎每一個角色深淺都不同
}

TEST(palette_high_contrast_maps_every_role_to_a_system_color) {
  // §12.7.4:高對比是第三種模式,**整份色票停用,改走 GetSysColor()**。
  // 每一個角色都要有對應,漏一個的症狀是那一塊畫成黑色。
  for (int i = 0; i < kRoleCount; ++i) {
    const int sys = SysColorFor(static_cast<Role>(i));
    CHECK(sys >= 0);
    CHECK(sys <= 30);  // COLOR_* 的合理範圍
  }
  // 焦點環兩圈在高對比下也必須是**不同**的系統色,否則兩圈疊成一圈,
  // 而「不管底下是什麼顏色一定有一圈看得見」這條保證就沒了。
  CHECK(SysColorFor(kFocusOuter) != SysColorFor(kFocusInner));
}

TEST(palette_luminance_matches_known_values) {
  // 反向:確認這支公式本身是對的,不然上面每一條都在驗一個壞掉的尺。
  const Rgb white{0xFF, 0xFF, 0xFF};
  const Rgb black{0x00, 0x00, 0x00};
  CHECK_NEAR(RelativeLuminance(white), 1.0, 1e-9);
  CHECK_NEAR(RelativeLuminance(black), 0.0, 1e-9);
  CHECK_NEAR(ContrastRatio(white, black), 21.0, 1e-6);
}

// ── accent 的取得:三段回落的**解析**那一半 ─────────────────────

TEST(accent_palette_blob_must_be_exactly_32_bytes) {
  uint8_t good[32] = {0};
  // index 4 = AccentDark1(淺色用)。
  good[16] = 0x11; good[17] = 0x22; good[18] = 0x33; good[19] = 0xFF;
  // index 1 = AccentLight2(深色用)。
  good[4] = 0xAA; good[5] = 0xBB; good[6] = 0xCC; good[7] = 0xFF;
  Rgb out{};
  CHECK(AccentFromPalette(good, 32, /*dark=*/false, &out));
  CHECK_INT(out.r, 0x11);
  CHECK_INT(out.g, 0x22);
  CHECK_INT(out.b, 0x33);
  CHECK(AccentFromPalette(good, 32, /*dark=*/true, &out));
  CHECK_INT(out.r, 0xAA);
  CHECK_INT(out.g, 0xBB);
  CHECK_INT(out.b, 0xCC);

  // ⚠ 位元組數不是 32 就**整段跳過**(規格明文)。那個佈局是流傳的
  //   知識,不是契約 —— 賭它的下場是「顏色變成隨機的一組位元組」。
  CHECK(!AccentFromPalette(good, 31, false, &out));
  CHECK(!AccentFromPalette(good, 33, false, &out));
  CHECK(!AccentFromPalette(good, 0, false, &out));
  CHECK(!AccentFromPalette(nullptr, 32, false, &out));
}

TEST(accent_dword_is_bbggrr_not_rrggbb) {
  // ⚠ 寫錯的樣子是「紅藍互換」—— 藍色 accent 會變成一個看起來
  //   很合理的橘色,不像壞掉。
  const Rgb c = AccentFromDword(0x00D47800u);  // BB=D4 GG=78 RR=00
  CHECK_INT(c.r, 0x00);
  CHECK_INT(c.g, 0x78);
  CHECK_INT(c.b, 0xD4);
}

TEST(accent_ladder_matches_the_spec_worked_example) {
  // §12.14.1 的階梯,拿 Win11 預設藍實算。
  const AccentLadder l = LadderOf(Hex(0x0078D4));
  CHECK_INT(l.dark1.r, 0x00);
  CHECK_INT(l.dark1.g, 0x60);
  CHECK_INT(l.dark1.b, 0xAA);
  CHECK_INT(l.light2.r, 0x80);
  CHECK_INT(l.light2.g, 0xBC);
  CHECK_INT(l.light2.b, 0xEA);
}

TEST(accent_fallback_seed_is_the_celadon_and_derives_a_darker_primary) {
  // §12.14.2:退路那一組走的是**同一條公式**,不是 #1F6F63 本身。
  // 所以淺色的 primary 是 #19594F —— 一條路徑比兩條路徑重要。
  const Rgb seed = AccentFallbackSeed();
  CHECK_INT(seed.r, 0x1F);
  CHECK_INT(seed.g, 0x6F);
  CHECK_INT(seed.b, 0x63);
  const Palette p = PaletteFor(Mode::kLight, seed);
  CHECK_INT(p[kPrimary].r, 0x19);
  CHECK_INT(p[kPrimary].g, 0x59);
  CHECK_INT(p[kPrimary].b, 0x4F);
}

TEST(accent_hover_and_pressed_only_ever_raise_contrast) {
  // §12.14.1:hover／按下一律往「與字相反」的方向推,所以按下去的
  // 對比**只會更高**。Win11 自己是反過來的(變淡),而變淡的那一版
  // 在亮 accent 上會掉到門檻以下 —— 那正是我們不跟的理由。
  for (int s = 0; s < kSeedCount; ++s)
    for (Mode m : {Mode::kLight, Mode::kDark}) {
      const Palette p = PaletteFor(m, Hex(kSeeds[s].hex));
      const double base = ContrastRatio(p[kOnPrimary], p[kPrimary]);
      const double hov = ContrastRatio(p[kOnPrimary], p[kPrimaryHover]);
      const double prs = ContrastRatio(p[kOnPrimary], p[kPrimaryPressed]);
      CHECK(hov >= base - 1e-9);
      CHECK(prs >= hov - 1e-9);
    }
}

TEST(accent_yellow_indicator_really_needed_the_gate) {
  // §12.14.1 的原話:「只有黃色需要真的動一格」。這一條把那句話釘住
  // —— 拿掉守門的迴圈,這裡會紅,而畫面上不會有任何東西看起來是錯的。
  const Rgb yellow = Hex(0xFFB900);
  const Palette p = PaletteFor(Mode::kLight, yellow);
  // 沒有守門的話,指示條就是 primary 本身。
  CHECK(ContrastRatio(p[kPrimary], p[kBackground]) < 3.0);
  // 有守門之後兩個底都過。
  CHECK(ContrastRatio(p[kAccentIndicator], p[kSurface]) >= 3.0);
  CHECK(ContrastRatio(p[kAccentIndicator], p[kBackground]) >= 3.0);
}
