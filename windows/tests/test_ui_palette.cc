// windows/tests/test_ui_palette.cc — 色票對比度(W22)與兩份色票同構(W11)
//
// §3.4.1 記著一次事故:第一版的 outline 只算了對卡片的比值(1.45 合格),
// 但對畫面底只有 1.34 —— **「只驗一半就宣告過關」**。所以這裡的每一組
// 都把兩個底都算,而且是表驅動的:加一個角色就得回來加一列。

#include "../common/ui_palette.h"

#include "check.h"

using namespace rimewin;

namespace {

struct Pair {
  const char* what;
  Role fg;
  Role bg;
  double threshold;
};

// §12.6.3 的九組。⚠ 停用文字**不在這裡** —— 它是規範性的豁免,
// 由 disabled_text_is_deliberately_exempt 單獨記錄。
const Pair kPairs[] = {
    {"primary text / normal bg", kOnSurface, kSurface, 4.5},
    {"secondary text / normal bg", kOnSurfaceVariant, kSurface, 4.5},
    {"primary text / hover bg", kOnSurface, kRowHover, 4.5},
    {"primary text / pressed bg", kOnSurface, kRowPressed, 4.5},
    {"primary text / selected bg", kOnSurface, kPrimaryContainer, 4.5},
    {"danger / normal bg", kError, kSurface, 4.5},
    {"danger / danger hover bg", kError, kDangerHover, 4.5},
    {"focus ring / hover bg", kPrimary, kRowHover, 3.0},
    {"divider / normal bg", kOutline, kSurface, 1.4},
};
constexpr int kPairCount = sizeof(kPairs) / sizeof(kPairs[0]);

}  // namespace

TEST(palette_every_pair_meets_its_threshold_in_both_modes) {
  int checked = 0;
  for (Mode m : {Mode::kLight, Mode::kDark}) {
    const Rgb* p = PaletteFor(m);
    for (int i = 0; i < kPairCount; ++i) {
      const double r = ContrastRatio(p[kPairs[i].fg], p[kPairs[i].bg]);
      ++checked;
      if (!(r >= kPairs[i].threshold)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s (%s) = %.2f < %.2f", kPairs[i].what,
                      m == Mode::kLight ? "light" : "dark", r,
                      kPairs[i].threshold);
        ::rimewin_test::Fail(__FILE__, __LINE__, buf);
      }
    }
  }
  // ⚠ 範圍斷言(§2-G2):W22 要求配對數 ≥ 18(9 組 × 深淺兩份)。
  //   表寫空了會讓這個測試「全部通過」—— 那正是要堵死的失效方式。
  CHECK_INT(checked, 18);
  CHECK(checked >= 18);
}

TEST(palette_divider_is_checked_against_both_backgrounds) {
  // §3.4.1 的教訓:同一個 outline 既畫在卡片裡,也畫在畫面底上。
  // 只算一個底就宣告合格,是這份規範自己記下來的錯誤。
  for (Mode m : {Mode::kLight, Mode::kDark}) {
    const Rgb* p = PaletteFor(m);
    CHECK(ContrastRatio(p[kOutline], p[kSurface]) >= 1.4);
    CHECK(ContrastRatio(p[kOutline], p[kBackground]) >= 1.4);
  }
}

TEST(palette_disabled_text_is_deliberately_exempt) {
  // §12.6.3:停用文字**刻意**不符合 4.5:1,而且這是規範性的豁免 ——
  // 做到 4.5:1 的結果是它看起來沒有被停用。
  // 這裡把它斷言成「確實低於門檻」:哪天有人「順手修好」它,
  // 這個測試會紅,並把人帶回這段說明。
  // 代價由 §2-D1 補:每一個停用的控制項,同一頁必須有一句全對比的話
  // 說明為什麼。那一條是 W23。
  for (Mode m : {Mode::kLight, Mode::kDark}) {
    const Rgb* p = PaletteFor(m);
    const double r = ContrastRatio(p[kDisabledText], p[kSurface]);
    CHECK(r < 4.5);
    CHECK(r > 1.5);  // 但也不可以低到完全看不見
  }
}

TEST(palette_dark_primary_button_text_is_near_black) {
  // §3.5 第 2 條:亮青底配白字實測只有 2.11:1。深色的 onPrimary
  // 必須是近黑,而不是跟著淺色抄一份白。
  const Rgb* d = PaletteFor(Mode::kDark);
  CHECK(ContrastRatio(d[kOnPrimary], d[kPrimary]) >= 4.5);
  const Rgb white{0xFF, 0xFF, 0xFF};
  CHECK(ContrastRatio(white, d[kPrimary]) < 4.5);  // 記下錯的那一版長什麼樣
}

TEST(palette_two_modes_have_identical_role_sets) {
  // W11 / §2-F4。⚠ 這在 C++ 裡其實是**編譯期**保證(ui_palette.cc 的
  // static_assert),因為兩份都是 Rgb[kRoleCount]。這裡再驗一次執行期的
  // 形狀:每一個角色在兩份裡都取得到,而且不是同一個值(否則等於沒換色票)。
  const Rgb* l = PaletteFor(Mode::kLight);
  const Rgb* d = PaletteFor(Mode::kDark);
  int differ = 0;
  for (int i = 0; i < kRoleCount; ++i) {
    const bool same = l[i].r == d[i].r && l[i].g == d[i].g && l[i].b == d[i].b;
    if (!same) ++differ;
  }
  CHECK(kRoleCount >= 11);       // §3.4 的角色數
  CHECK(differ >= kRoleCount - 1);  // 幾乎每一個角色深淺都不同
}

TEST(palette_high_contrast_maps_every_role_to_a_system_color) {
  // §12.7.4:高對比是第三種模式,**整份色票停用,改走 GetSysColor()**。
  // 每一個角色都要有對應,漏一個的症狀是那一塊畫成黑色。
  for (int i = 0; i < kRoleCount; ++i) {
    const int sys = SysColorFor(static_cast<Role>(i));
    CHECK(sys >= 0);
    CHECK(sys <= 30);  // COLOR_* 的合理範圍
  }
}

TEST(palette_luminance_matches_known_values) {
  // 反向:確認這支公式本身是對的,不然上面每一條都在驗一個壞掉的尺。
  const Rgb white{0xFF, 0xFF, 0xFF};
  const Rgb black{0x00, 0x00, 0x00};
  CHECK_NEAR(RelativeLuminance(white), 1.0, 1e-9);
  CHECK_NEAR(RelativeLuminance(black), 0.0, 1e-9);
  CHECK_NEAR(ContrastRatio(white, black), 21.0, 1e-6);
}
