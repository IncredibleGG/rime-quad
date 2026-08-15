// windows/tests/test_statusbar_layout.cc — 懸浮狀態列的幾何(W34')
//
// ⚠ 這一份存在的理由寫在 statusbar_layout.h 的檔頭:在它之前,格的矩形
//   住在 service/status_bar.cc::Relayout() 裡,而那個檔案在 Ubuntu 上
//   編不起來 —— 所以「每一格只有 26 DIP 高」這件事**沒有任何自動化
//   看得到**,而它在畫面上看起來只是「那一橫有點扁」。
//
// 反向(§2-G):把 kInsetV 改回 metric::kHairline(回到現況的 26),
// cells_are_at_least_28_tall 會紅。

#include "../common/statusbar_layout.h"

#include "check.h"

using namespace rimewin;

namespace {

// 四格:中 / 繁 / 方案名 / 設定。字寬是**量出來的**,所以測試餵合成值。
std::vector<BarCellIn> Four(int w0, int w1, int w2, int w3) {
  std::vector<BarCellIn> in(4);
  in[0].text_w_dip = w0;
  in[1].text_w_dip = w1;
  in[2].text_w_dip = w2;
  in[3].text_w_dip = w3;
  return in;
}

int Drawn(const BarLayout& l) {
  int n = 0;
  for (const BarCell& c : l.cells)
    if (!c.skipped) ++n;
  return n;
}

}  // namespace

TEST(statusbar_cells_are_at_least_28_by_28) {
  // §3.6 的最小點擊目標。⚠ §12.14.0 第 5 條:現況是 26。
  const BarLayout l = LayoutStatusBarCellsDip(Four(13, 13, 60, 26));
  int checked = 0;
  for (const BarCell& c : l.cells) {
    if (c.skipped) continue;
    CHECK(c.rect.h >= 28);
    CHECK(c.rect.w >= 28);
    ++checked;
  }
  // 範圍斷言(§2-G2):四格 + 整條那一句 = 5,見下一條。
  CHECK_INT(checked, 4);
}

TEST(statusbar_bar_is_32_tall_and_cells_fit_inside_it) {
  const BarLayout l = LayoutStatusBarCellsDip(Four(13, 13, 60, 26));
  CHECK_INT(barmetric::kBarH, 32);
  for (const BarCell& c : l.cells) {
    if (c.skipped) continue;
    CHECK(c.rect.y >= 0);
    CHECK(c.rect.bottom() <= barmetric::kBarH);
    // 上下各 s1(2)。⚠ 不是 kBarBorder(1) —— 那一個字就是 26 DIP。
    CHECK_INT(c.rect.y, space::s1);
    CHECK_INT(c.rect.h, barmetric::kBarH - 2 * space::s1);
  }
}

TEST(statusbar_total_never_exceeds_320_for_realistic_input) {
  // §12.14.7:整條的寬上限 320。第 3 格的內容上限 120 之後,
  // 四個字面的最壞情況仍然放得下 —— 這一條把那件事釘住。
  const BarLayout l = LayoutStatusBarCellsDip(Four(13, 13, 4000, 26));
  CHECK(l.total_w_dip <= barmetric::kBarMaxW);
  CHECK(l.schema_truncated);
  // 只壓第 3 格:1、2、4 格的寬度不受影響。
  const BarLayout base = LayoutStatusBarCellsDip(Four(13, 13, 20, 26));
  CHECK_INT(l.cells[0].rect.w, base.cells[0].rect.w);
  CHECK_INT(l.cells[1].rect.w, base.cells[1].rect.w);
  CHECK_INT(l.cells[3].rect.w, base.cells[3].rect.w);
  CHECK(l.cells[2].rect.w > base.cells[2].rect.w);
}

TEST(statusbar_schema_cell_content_is_capped_at_120) {
  const BarLayout l = LayoutStatusBarCellsDip(Four(13, 13, 500, 26));
  // 格寬上限 = 120 + 2 * s4 = 140。
  CHECK(l.cells[2].rect.w <= barmetric::kSchemaContentMaxW +
                                 2 * barmetric::kCellPadH);
  CHECK(l.schema_truncated);
  // 沒超過就不截。
  const BarLayout ok = LayoutStatusBarCellsDip(Four(13, 13, 60, 26));
  CHECK(!ok.schema_truncated);
}

TEST(statusbar_empty_schema_name_skips_the_whole_cell) {
  // §8.12 規範性:方案名還沒載入完成時,那一格**完全不佔位置**,
  // 不得畫成一塊看不出用途的空白。
  const BarLayout l = LayoutStatusBarCellsDip(Four(13, 13, 0, 26));
  CHECK(l.cells[2].skipped);
  CHECK(l.cells[2].rect.empty());
  CHECK_INT(Drawn(l), 3);
  // 第 3 格不在的時候,第 3/4 格之間那條分隔線也不畫 ——
  // 它分的是「改狀態」與「開視窗」,少了一邊就沒有東西要分。
  CHECK_INT(l.separator_x_dip, -1);
  // 而且第 4 格要接著第 2 格,不是留一個洞。
  CHECK_INT(l.cells[3].rect.x,
            l.cells[1].rect.right() + barmetric::kCellGap);
}

TEST(statusbar_separator_sits_between_cell_three_and_four_only) {
  const BarLayout l = LayoutStatusBarCellsDip(Four(13, 13, 60, 26));
  CHECK(l.separator_x_dip > 0);
  // 左右各 s3。
  CHECK_INT(l.separator_x_dip, l.cells[2].rect.right() + space::s3);
  CHECK_INT(l.cells[3].rect.x,
            l.separator_x_dip + metric::kHairline + space::s3);
  // 1–2–3 之間**沒有**分隔線(它們是同一種東西):格與格之間就是 s3。
  CHECK_INT(l.cells[1].rect.x, l.cells[0].rect.right() + barmetric::kCellGap);
  CHECK_INT(l.cells[2].rect.x, l.cells[1].rect.right() + barmetric::kCellGap);
}

TEST(statusbar_sentence_mode_is_one_clickable_strip) {
  // 第五種外觀:四格全不畫,整條一句話、整條可點,滑過/按下畫**整條**。
  const BarLayout l = LayoutStatusBarSentenceDip(160);
  CHECK_INT(static_cast<int>(l.cells.size()), 1);
  CHECK(!l.cells[0].skipped);
  CHECK_INT(l.cells[0].rect.y, 0);
  CHECK_INT(l.cells[0].rect.h, barmetric::kBarH);
  CHECK_INT(l.cells[0].rect.w, l.total_w_dip);
  // 左右內距 s5。
  CHECK_INT(l.total_w_dip, 160 + 2 * space::s5);
}

TEST(statusbar_radius_is_the_two_windows_values_only) {
  // §12.14.4:Windows 端只有兩個圓角值。狀態列是 top-level → 8;
  // 每一格是控制項 → 4。
  CHECK_INT(barmetric::kBarRadius, 8);
  CHECK_INT(barmetric::kCellRadius, 4);
  CHECK_INT(radius::kWindow, 8);
  CHECK_INT(radius::kControl, 4);
}
