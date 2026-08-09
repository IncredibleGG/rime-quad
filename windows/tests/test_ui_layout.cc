// windows/tests/test_ui_layout.cc — 設定視窗版面(W18 / W19)與單位換算(W1)
//
// 這幾條在 docs/ui-design.md §12.12 裡是「真的單元測試」那一組:
// 驗的是邏輯,不是字串。**一條都不需要真的 Windows。**

#include "../common/ui_dip.h"
#include "../common/ui_layout.h"

#include "check.h"

using namespace rimewin;

// ── W1:單位換算 ─────────────────────────────────────────────────

TEST(dip_muldiv_rounds_instead_of_truncating) {
  // §12.3 的例子:150% 下 11 DIP → 16.5 → **17**,不是 16。
  // 舊寫法 static_cast<int>(11 * 1.5) 給的是 16,而誤差沿著版面累積。
  CHECK_INT(Dip(11, 144), 17);
  CHECK_INT(static_cast<int>(11 * 1.5), 16);  // 記下舊寫法錯在哪

  // 100% 下恆等。
  CHECK_INT(Dip(200, 96), 200);
  CHECK_INT(Dip(0, 96), 0);

  // 175%(168 DPI)那一組,逐項算過:
  CHECK_INT(Dip(20, 168), 35);   // 35.0
  CHECK_INT(Dip(36, 168), 63);   // 63.0
  CHECK_INT(Dip(13, 168), 23);   // 22.75 → 23
  CHECK_INT(Dip(200, 168), 350);

  // 125%(120 DPI)。
  CHECK_INT(Dip(11, 120), 14);   // 13.75 → 14
  CHECK_INT(Dip(6, 120), 8);     // 7.5 → 8(.5 遠離零)
}

TEST(dip_font_height_is_negative) {
  // ⚠ lfHeight 負值 = 字元高度。正值是儲存格高度,字會小一截,
  //   而那個差別在截圖上看起來只是「字有點小」。
  CHECK_INT(FontHeightFromDip(13, 96), -13);
  CHECK_INT(FontHeightFromDip(22, 144), -33);
  CHECK(FontHeightFromDip(11, 168) < 0);
}

TEST(dip_muldiv_denominator_zero_is_not_a_crash) {
  CHECK_INT(MulDivRound(10, 10, 0), -1);
}

// ── W19:§12.4.2 的三組驗算數字 ──────────────────────────────────
//
// ⚠ 規格自承這三組是**手算的**,而且「那個測試還不存在」(§12.13 第 7 條)。
//   現在它存在了,而且三組全部對得上。

TEST(ui_layout_content_column_three_worked_cases) {
  // W=780 → content_w=540, content_x=220
  CHECK_INT(ContentWidthDip(780), 540);
  CHECK_INT(ContentXDip(780), 220);

  // W=660(最小尺寸)→ content_w=440, content_x=210
  // 左右內距被壓到 10 —— 內容欄的 440 下界優先於邊距,這是刻意的。
  CHECK_INT(ContentWidthDip(660), 440);
  CHECK_INT(ContentXDip(660), 210);

  // W=1000 → content_w=640, content_x=280(兩側留白)
  CHECK_INT(ContentWidthDip(1000), 640);
  CHECK_INT(ContentXDip(1000), 280);
}

TEST(ui_layout_content_column_never_leaves_the_window) {
  // 掃一遍合理的寬度區間:內容欄不可以超出視窗,也不可以壓到側欄。
  for (int w = kWindowMinW; w <= 2400; w += 7) {
    const int cx = ContentXDip(w);
    const int cw = ContentWidthDip(w);
    CHECK(cx >= metric::kSidebarW);
    CHECK(cw >= kContentMinW);
    CHECK(cw <= kContentMaxW);
    // ⚠ 這一條是重點:視窗比 840 寬時內容欄置中,不可以因為
    //   (avail - cw) 是奇數而多算一格跑出右邊界。
    CHECK(cx + cw <= w);
  }
}

TEST(ui_layout_sidebar_items_do_not_overlap_the_status_area) {
  // 側欄最多列七頁(§5.3 的桌面端頁數)。第七頁的底不可以蓋到狀態區。
  const RectI last = SidebarItemDip(6);
  const RectI status = SidebarStatusDip(kWindowDefaultH);
  CHECK(last.bottom() <= status.y);
  // 最小高度下也要成立 —— 那是最容易撞到的情形。
  const RectI status_min = SidebarStatusDip(kWindowMinH);
  CHECK(last.bottom() <= status_min.y);
}

TEST(ui_layout_sidebar_items_are_evenly_spaced) {
  const RectI a = SidebarItemDip(0);
  const RectI b = SidebarItemDip(1);
  CHECK_INT(b.y - a.bottom(), space::s2);
  CHECK_INT(a.h, metric::kSidebarItemH);
  CHECK_INT(a.w, metric::kSidebarW - 2 * space::s5);
}

// ── W18:所有可點矩形 ≥ 28×28 DIP ───────────────────────────────

TEST(ui_layout_every_clickable_target_meets_the_minimum) {
  const int sizes[][2] = {{660, 460}, {780, 560}, {1000, 700}, {1600, 1000}};
  int measured = 0;
  for (const auto& s : sizes) {
    const std::vector<HitTarget> targets = ClickableTargetsDip(s[0], s[1], 7);
    // ⚠ 範圍斷言:一頁上至少要有這麼多可點的東西。掃到零個而報「全部合格」
    //   正是 §2-G 講的那個失效方式。
    CHECK(targets.size() >= 12);
    for (const HitTarget& t : targets) {
      ++measured;
      CHECK(t.rect.w >= metric::kMinTarget);
      CHECK(t.rect.h >= metric::kMinTarget);
    }
  }
  CHECK(measured >= 12);
}

TEST(ui_layout_stack_puts_danger_last_behind_a_divider) {
  // §2-C2:破壞性動作在該頁最後一個區塊,與上面隔一條 hairline + s7。
  Stack st(0, 0, 400);
  const RectI a = st.Push(30, space::s3);
  const RectI line = st.PushDivider();
  const RectI danger = st.Push(30, 0);
  CHECK_INT(line.h, metric::kHairline);
  CHECK(line.y >= a.bottom() + space::s7);
  CHECK(danger.y >= line.bottom() + space::s7);
}
