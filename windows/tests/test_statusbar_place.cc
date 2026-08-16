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

// ── 寬度變了之後必須重走這支純函式 ──────────────────────────────
//
// ⚠ 上面每一條都餵固定的 kBarW,所以「那一橫變寬之後沒有重新定位」
//   一條都碰不到。而那正是實際發生的缺陷:status_bar.cc 的 Relayout
//   用 SWP_NOACTIVATE | SWP_NOMOVE 只改尺寸 —— 左上角釘死、只往右長。

TEST(statusbar_growing_wider_stays_inside_the_work_area) {
  const WorkArea mon = Primary1080();
  // 「未就緒」:一格,約 114 DIP。使用者沒有拖過,走預設的右下角 12/12。
  const PlacedBar narrow = PlaceStatusBar(BarAnchor(), {mon}, 114, kBarH);
  CHECK_INT(narrow.w, 114);
  CHECK_INT(mon.right - (narrow.x + narrow.w), 12);

  // 就緒之後變成四格,約 220 DIP。重走一次這支純函式:
  const BarAnchor a =
      MakeAnchor(mon, narrow.x, narrow.y, narrow.w, narrow.h);
  const PlacedBar wide = PlaceStatusBar(a, {mon}, 220, kBarH);
  CHECK_INT(wide.w, 220);
  CHECK(wide.x >= mon.left);
  CHECK(wide.x + wide.w <= mon.right);
  // 右錨定:右緣仍然離工作區右邊 12,長出來的部分往左跑。
  CHECK_INT(mon.right - (wide.x + wide.w), 12);
  CHECK(wide.x < narrow.x);

  // ── 對照組:只改尺寸不重擺會跑出去多少 ──
  // 這不是裝飾。缺陷的量級寫在測試裡,下一個人才知道「不重擺」
  // 的後果是「設定那一格整格點不到」,而不是「差一點點」。
  const int kept_left = narrow.x;             // SWP_NOMOVE 保留的左上角
  CHECK(kept_left + 220 > mon.right);
  CHECK_INT(kept_left + 220 - mon.right, 94);  // 94 px 在螢幕外面
}

TEST(statusbar_growing_wider_survives_a_narrow_work_area) {
  // 極端:工作區只有 150 寬,而那一橫要 220。先夾尺寸、再擺位置,
  // 不可以產生負的邊距(§8.6.7.3)。
  WorkArea tiny;
  tiny.left = 0;
  tiny.top = 0;
  tiny.right = 150;
  tiny.bottom = 200;
  tiny.primary = true;
  tiny.dpi = 96;
  for (int w = 100; w <= 400; w += 7) {
    const PlacedBar p = PlaceStatusBar(BarAnchor(), {tiny}, w, kBarH);
    CHECK(p.w >= 1);
    CHECK(p.x >= tiny.left);
    CHECK(p.y >= tiny.top);
    CHECK(p.x + p.w <= tiny.right);
    CHECK(p.y + p.h <= tiny.bottom);
  }
}

// ══════════════════════════════════════════════════════════════════
// ⭐ #120:不要壓在別人的浮動橫條上
// ══════════════════════════════════════════════════════════════════
//
// 使用者截圖:他切到第三方輸入法,我們那一橫**壓在人家的橫條上**。
// 位置預設右下角,而上一輪一點避讓都沒有。
namespace {

WorkArea Mon(int l, int t, int r, int b, bool primary = true, int dpi = 96) {
  WorkArea m;
  m.left = l;
  m.top = t;
  m.right = r;
  m.bottom = b;
  m.primary = primary;
  m.dpi = dpi;
  return m;
}

ObstacleRect Obs(int l, int t, int r, int b) {
  ObstacleRect o;
  o.left = l;
  o.top = t;
  o.right = r;
  o.bottom = b;
  return o;
}

// 那一橫已經擺好的樣子:寬 200、高 28,靠工作區右下角(離邊 12)。
PlacedBar AtBottomRight(const WorkArea& m, int w = 200, int h = 28) {
  PlacedBar p;
  p.w = w;
  p.h = h;
  p.x = m.right - 12 - w;
  p.y = m.bottom - 12 - h;
  p.monitor = 0;
  return p;
}

bool Intersects(const PlacedBar& p, const ObstacleRect& o) {
  return p.x < o.right && o.left < p.x + p.w && p.y < o.bottom &&
         o.top < p.y + p.h;
}

}  // namespace

TEST(StatusBarPlace_NothingInTheWayMeansNoMove) {
  const std::vector<WorkArea> mons{Mon(0, 0, 1920, 1040)};
  const PlacedBar in = AtBottomRight(mons[0]);
  // 沒有障礙物 —— 一個像素都不准動。避讓不可以變成「每次重排都跳一下」。
  const PlacedBar out = AvoidObstacles(in, mons, {});
  CHECK_INT(out.y, in.y);
  CHECK_INT(out.nudge_dy, 0);
  // 有障礙物但不在路上(x 差得遠)—— 一樣不動。
  const PlacedBar out2 =
      AvoidObstacles(in, mons, {Obs(0, 1000, 300, 1040)});
  CHECK_INT(out2.y, in.y);
  CHECK_INT(out2.nudge_dy, 0);
}

TEST(StatusBarPlace_MovesOffAnotherFloatingBar) {
  const std::vector<WorkArea> mons{Mon(0, 0, 1920, 1040)};
  const PlacedBar in = AtBottomRight(mons[0]);
  // 別人的橫條停在同一個角:1000..1040 的最後 40 像素。
  const ObstacleRect other = Obs(1600, 990, 1908, 1030);
  CHECK_MSG(Intersects(in, other), "前提:沒有避讓時它們真的疊在一起");

  const PlacedBar out = AvoidObstacles(in, mons, {other});
  CHECK_MSG(!Intersects(out, other), "避讓之後不可以再疊在一起");
  CHECK_MSG(out.nudge_dy < 0, "在工作區下半 → 往**上**讓");
  CHECK(out.how == PlacedBar::How::kNudgedAside);
  // 讓開之後留得下 §12.10.5 的空隙,而且 x 一個像素都沒動
  // (那一橫是右錨定的,左右挪會離開使用者選的那個角)。
  CHECK_INT(out.x, in.x);
  CHECK(out.y + out.h <= other.top - kBarAvoidGapDip);
  // ⚠ 而且仍然在工作區裡面。
  CHECK(out.y >= mons[0].top);
  CHECK(out.y + out.h <= mons[0].bottom);
}

TEST(StatusBarPlace_TaskbarOnTopMeansMoveDown) {
  // 工作列在**上面**:工作區的 top 被推下來,而那一橫如果落在上半,
  // 往上讓就是往螢幕外面讓。方向要由它自己的位置決定,不是寫死。
  const std::vector<WorkArea> mons{Mon(0, 48, 1920, 1080)};
  PlacedBar in;
  in.w = 200;
  in.h = 28;
  in.x = 1920 - 12 - 200;
  in.y = 48 + 12;          // 靠工作區**上**緣
  in.monitor = 0;
  const ObstacleRect other = Obs(1600, 48, 1908, 90);
  CHECK_MSG(Intersects(in, other), "前提:真的疊在一起");

  const PlacedBar out = AvoidObstacles(in, mons, {other});
  CHECK_MSG(!Intersects(out, other), "避讓之後不可以再疊在一起");
  CHECK_MSG(out.nudge_dy > 0, "在工作區上半 → 往**下**讓");
  CHECK(out.y >= mons[0].top);
  CHECK(out.y + out.h <= mons[0].bottom);
}

TEST(StatusBarPlace_OtherMonitorsObstaclesAreNotOurs) {
  // 多螢幕:第二顆上的橫條與我們無關。
  // ⚠ 少了這一關,別顆螢幕上一條「座標剛好對得上」的橫條會把這一條
  //   趕出工作區 —— 而症狀是「它不見了」,使用者不會聯想到避讓。
  const std::vector<WorkArea> mons{Mon(0, 0, 1920, 1040),
                                   Mon(1920, 0, 3840, 1040, false)};
  const PlacedBar in = AtBottomRight(mons[0]);
  const PlacedBar out =
      AvoidObstacles(in, mons, {Obs(3500, 990, 3830, 1030)});
  CHECK_INT(out.y, in.y);
  CHECK_INT(out.nudge_dy, 0);
}

TEST(StatusBarPlace_NeverLeavesTheWorkArea) {
  // 讓不開的時候**維持原位**。壓在別人上面難看,跑到螢幕外面是消失
  // —— 而那一橫上刻意沒有 X,使用者不會覺得是自己關掉的。
  const std::vector<WorkArea> mons{Mon(0, 0, 1920, 1040)};
  const PlacedBar in = AtBottomRight(mons[0]);
  // 整個工作區的右半邊從上到下都被佔住。
  const std::vector<ObstacleRect> wall{Obs(1500, 0, 1920, 1040)};
  const PlacedBar out = AvoidObstacles(in, mons, wall);
  CHECK_INT(out.y, in.y);
  CHECK_INT(out.x, in.x);
  CHECK_INT(out.nudge_dy, 0);
  CHECK(out.y >= mons[0].top && out.y + out.h <= mons[0].bottom);
}

TEST(StatusBarPlace_StacksPastSeveralBars) {
  // 兩條疊著的橫條 —— 要一路讓過去,而且步數有上限(不可以繞不完)。
  const std::vector<WorkArea> mons{Mon(0, 0, 1920, 1040)};
  const PlacedBar in = AtBottomRight(mons[0]);
  const std::vector<ObstacleRect> two{Obs(1600, 990, 1908, 1030),
                                      Obs(1500, 940, 1908, 985)};
  const PlacedBar out = AvoidObstacles(in, mons, two);
  for (const ObstacleRect& o : two)
    CHECK_MSG(!Intersects(out, o), "兩條都要讓過去");
  CHECK(out.y >= mons[0].top);
  CHECK(out.y + out.h <= mons[0].bottom);
}

TEST(StatusBarPlace_AvoidanceIsNotStoredAsThePosition) {
  // ⚠ 存進設定檔的必須是**使用者選的**那個位置。存避讓後的位置,
  //   下一次重排會從那裡再讓一次 —— 偏移會一路累積,最後那一橫自己
  //   爬到螢幕另一頭。service/status_bar.cc 的 SavePlacement 靠
  //   nudge_dy 把它扣回去,所以這一格必須說得出「挪了多少」。
  const std::vector<WorkArea> mons{Mon(0, 0, 1920, 1040)};
  const PlacedBar in = AtBottomRight(mons[0]);
  const PlacedBar out =
      AvoidObstacles(in, mons, {Obs(1600, 990, 1908, 1030)});
  CHECK_INT(out.y - out.nudge_dy, in.y);
  // 扣回去之後與 MakeAnchor 之前那一份完全一樣 —— 也就是「使用者的位置」。
  const BarAnchor a =
      MakeAnchor(mons[0], out.x, out.y - out.nudge_dy, out.w, out.h);
  CHECK_INT(a.dy_dip, 12);
  CHECK_INT(a.dx_dip, 12);
}

TEST(StatusBarPlace_PlacementSaysWhichMonitor) {
  // 避讓要在同一顆螢幕的工作區內做,而三段回落之後呼叫端自己算不出來。
  const std::vector<WorkArea> mons{Mon(0, 0, 1920, 1040, false),
                                   Mon(1920, 0, 3840, 1040, true)};
  BarAnchor a;
  a.valid = true;
  a.work_left = 0;
  a.work_top = 0;
  a.work_right = 1920;
  a.work_bottom = 1040;
  a.dx_dip = 12;
  a.dy_dip = 12;
  CHECK_INT(PlaceStatusBar(a, mons, 200, 28).monitor, 0);
  // 那顆螢幕不見了 → 退回主螢幕,而 monitor 要跟著指到主螢幕。
  const std::vector<WorkArea> only_second{Mon(1920, 0, 3840, 1040, true)};
  const PlacedBar p2 = PlaceStatusBar(a, only_second, 200, 28);
  CHECK_INT(p2.monitor, 0);
  CHECK(p2.how == PlacedBar::How::kFellBackToPrimary);
  // 一顆螢幕都沒有 —— 不可以回一個假的索引。
  CHECK_INT(PlaceStatusBar(a, {}, 200, 28).monitor, -1);
}
