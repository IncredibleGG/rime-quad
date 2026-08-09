// windows/tests/test_statusbar_place.cc — 懸浮狀態列的位置還原(W20)
//
// W20 要求的三個案例:**螢幕消失、DPI 變、工作區縮小**。
// 反向測試(「移掉『夾進工作區』那一步」)由 windows/check_ui_spec.sh
// 的 --self-check 植入,見那支腳本。

#include "../common/statusbar_place.h"

#include "check.h"

using namespace rimewin;

namespace {

WorkArea Primary1080() {
  WorkArea w;
  w.left = 0;
  w.top = 0;
  w.right = 1920;
  w.bottom = 1040;  // 工作列吃掉 40
  w.primary = true;
  w.dpi = 96;
  return w;
}

WorkArea SecondRight150() {
  WorkArea w;
  w.left = 1920;
  w.top = 0;
  w.right = 4480;  // 2560 寬
  w.bottom = 1400;
  w.primary = false;
  w.dpi = 144;  // 150%
  return w;
}

constexpr int kBarW = 220;  // 四格大約這麼寬
constexpr int kBarH = 28;   // §12.10.3

}  // namespace

TEST(statusbar_restores_onto_the_same_monitor) {
  const WorkArea second = SecondRight150();
  // 使用者把它拖到第二顆螢幕的右下角附近。
  const int x = second.right - 300;
  const int y = second.bottom - 100;
  const BarAnchor a = MakeAnchor(second, x, y, 200, 42);

  const std::vector<WorkArea> mons = {Primary1080(), second};
  const PlacedBar p = PlaceStatusBar(a, mons, kBarW, kBarH);
  CHECK(p.how == PlacedBar::How::kRestoredExact);
  // 回到第二顆螢幕上。
  CHECK(p.x >= second.left);
  CHECK(p.x + p.w <= second.right);
  CHECK(p.y + p.h <= second.bottom);
  // 150% 下尺寸要跟著放大 —— 不放大的話那一橫在高 DPI 螢幕上小到點不到。
  CHECK_INT(p.w, 330);  // 220 DIP @144dpi
  CHECK_INT(p.h, 42);   // 28 DIP @144dpi
}

TEST(statusbar_falls_back_when_the_monitor_disappears) {
  // ⚠ W20 的第一個案例:**螢幕消失**。
  //   使用者拔掉外接螢幕之後,存下來的座標落在螢幕外,
  //   而症狀是「那一橫不見了」—— 使用者不會把兩件事聯想在一起。
  const WorkArea second = SecondRight150();
  const BarAnchor a =
      MakeAnchor(second, second.right - 300, second.bottom - 100, 200, 42);

  const std::vector<WorkArea> only_primary = {Primary1080()};
  const PlacedBar p = PlaceStatusBar(a, only_primary, kBarW, kBarH);
  CHECK(p.how == PlacedBar::How::kFellBackToPrimary);
  // 一定要落在主螢幕的工作區裡,而且靠右下(那是預設位置)。
  const WorkArea m = Primary1080();
  CHECK(p.x >= m.left);
  CHECK(p.y >= m.top);
  CHECK(p.x + p.w <= m.right);
  CHECK(p.y + p.h <= m.bottom);
  CHECK_INT(p.x + p.w, m.right - 12);
  CHECK_INT(p.y + p.h, m.bottom - 12);
}

TEST(statusbar_survives_a_dpi_change_on_the_same_geometry) {
  // ⚠ W20 的第二個案例:**DPI 變**(使用者改了縮放比例,工作區矩形不變)。
  //   偏移存的是 DIP,所以那一橫與角落的**視覺**距離不變。
  WorkArea m = Primary1080();
  const BarAnchor a = MakeAnchor(m, m.right - 240, m.bottom - 60, 200, 28);
  CHECK_INT(a.dx_dip, 40);
  CHECK_INT(a.dy_dip, 32);

  m.dpi = 144;  // 使用者改成 150%
  const PlacedBar p = PlaceStatusBar(a, {m}, kBarW, kBarH);
  CHECK(p.how == PlacedBar::How::kRestoredExact);
  // 邊距換算成像素之後是 40*1.5 = 60、32*1.5 = 48。
  CHECK_INT(m.right - (p.x + p.w), 60);
  CHECK_INT(m.bottom - (p.y + p.h), 48);
}

TEST(statusbar_is_clamped_when_the_work_area_shrinks) {
  // ⚠ W20 的第三個案例:**工作區縮小**(使用者把工作列變成兩排、
  //   或接了一台解析度更小的螢幕)。存下來的偏移現在會把那一橫推出去。
  WorkArea m = Primary1080();
  const BarAnchor a = MakeAnchor(m, 40, 40, 200, 28);  // 靠左上,偏移很大
  CHECK(a.dx_dip > 1000);

  WorkArea small = m;
  small.right = 640;
  small.bottom = 400;
  const PlacedBar p = PlaceStatusBar(a, {small}, kBarW, kBarH);
  // 完全落在工作區內,而且沒有負的邊距。
  CHECK(p.x >= small.left);
  CHECK(p.y >= small.top);
  CHECK(p.x + p.w <= small.right);
  CHECK(p.y + p.h <= small.bottom);
}

TEST(statusbar_clamps_size_before_position) {
  // §8.6.7.3:「先夾窗的尺寸,再擺位置 —— 位置的計算不得產生負的邊距」。
  // 工作區比那一橫還小的極端情形。
  WorkArea tiny;
  tiny.left = 0;
  tiny.top = 0;
  tiny.right = 100;
  tiny.bottom = 20;
  tiny.primary = true;
  tiny.dpi = 96;
  const PlacedBar p = PlaceStatusBar(BarAnchor(), {tiny}, kBarW, kBarH);
  CHECK(p.w <= tiny.width());
  CHECK(p.h <= tiny.height());
  CHECK(p.x >= 0);
  CHECK(p.y >= 0);
  CHECK(p.x + p.w <= tiny.right);
  CHECK(p.y + p.h <= tiny.bottom);
}

TEST(statusbar_default_anchor_goes_bottom_right_of_primary) {
  // 從來沒拖過的使用者。預設偏移 12/12,右下角。
  const std::vector<WorkArea> mons = {Primary1080(), SecondRight150()};
  const PlacedBar p = PlaceStatusBar(BarAnchor(), mons, kBarW, kBarH);
  const WorkArea m = Primary1080();
  CHECK_INT(p.x + p.w, m.right - 12);
  CHECK_INT(p.y + p.h, m.bottom - 12);
}

TEST(statusbar_no_monitors_does_not_return_garbage) {
  const PlacedBar p = PlaceStatusBar(BarAnchor(), {}, kBarW, kBarH);
  CHECK_INT(p.w, kBarW);
  CHECK_INT(p.h, kBarH);
  CHECK_INT(p.x, 0);
  CHECK_INT(p.y, 0);
}

// ── 設定檔的字串形式 ────────────────────────────────────────────

TEST(statusbar_anchor_round_trips) {
  const WorkArea m = SecondRight150();
  const BarAnchor a = MakeAnchor(m, 3000, 1200, 200, 42);
  const std::string s = SerializeAnchor(a);
  CHECK(!s.empty());
  const BarAnchor b = ParseAnchor(s);
  CHECK(b.valid);
  CHECK_INT(b.work_left, a.work_left);
  CHECK_INT(b.work_right, a.work_right);
  CHECK_INT(b.dx_dip, a.dx_dip);
  CHECK_INT(b.dy_dip, a.dy_dip);
}

TEST(statusbar_broken_anchor_strings_fall_back_instead_of_going_off_screen) {
  // ⚠ 一行壞掉的設定不可以變成一個畫在螢幕外的視窗。
  const char* bad[] = {
      "",
      "1,2,3",                  // 欄位不足
      "1,2,3,4,5",              // 少一個
      "1,2,3,4,5,6,7",          // 多一個
      "a,b,c,d,e,f",            // 非數字
      "0,0,-10,-10,12,12",      // 矩形是反的
      ",,,,,",                  // 空欄位
      "0,0,100,100,12,12x",     // 尾巴有垃圾
  };
  for (const char* s : bad) {
    const BarAnchor a = ParseAnchor(s);
    CHECK(!a.valid);
    // 而且拿它去定位仍然得到一個落在螢幕內的矩形。
    const PlacedBar p = PlaceStatusBar(a, {Primary1080()}, kBarW, kBarH);
    CHECK(p.x >= 0);
    CHECK(p.y >= 0);
    CHECK(p.x + p.w <= 1920);
    CHECK(p.y + p.h <= 1040);
  }
}
