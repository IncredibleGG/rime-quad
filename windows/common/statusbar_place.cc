#include "statusbar_place.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "ui_dip.h"

namespace rimewin {
namespace {

const WorkArea* FindExact(const BarAnchor& a,
                          const std::vector<WorkArea>& monitors) {
  if (!a.valid) return nullptr;
  for (const WorkArea& m : monitors) {
    if (m.left == a.work_left && m.top == a.work_top &&
        m.right == a.work_right && m.bottom == a.work_bottom)
      return &m;
  }
  return nullptr;
}

const WorkArea* FindPrimary(const std::vector<WorkArea>& monitors) {
  for (const WorkArea& m : monitors)
    if (m.primary) return &m;
  return monitors.empty() ? nullptr : &monitors.front();
}

}  // namespace

namespace {

int IndexOf(const std::vector<WorkArea>& monitors, const WorkArea* m) {
  if (!m) return -1;
  for (size_t i = 0; i < monitors.size(); ++i)
    if (&monitors[i] == m) return static_cast<int>(i);
  return -1;
}

}  // namespace

PlacedBar PlaceStatusBar(const BarAnchor& anchor,
                         const std::vector<WorkArea>& monitors, int w_dip,
                         int h_dip) {
  PlacedBar out;
  const WorkArea* mon = FindExact(anchor, monitors);
  int dx = anchor.dx_dip;
  int dy = anchor.dy_dip;

  if (mon) {
    out.how = PlacedBar::How::kRestoredExact;
  } else {
    // 第 2 段:主螢幕右下角 + 預設偏移。**不是**沿用快照裡的偏移 ——
    // 那個偏移是相對另一顆螢幕算的,套到這一顆上可能又落在外面。
    mon = FindPrimary(monitors);
    dx = 12;
    dy = 12;
    out.how = PlacedBar::How::kFellBackToPrimary;
  }

  if (!mon) {
    // 沒有任何螢幕。不可能發生,但不可以因此回傳未初始化的東西。
    out.x = 0;
    out.y = 0;
    out.w = w_dip;
    out.h = h_dip;
    out.how = PlacedBar::How::kClamped;
    return out;
  }

  // 尺寸先換算成該螢幕的像素。
  int w = Dip(w_dip, mon->dpi);
  int h = Dip(h_dip, mon->dpi);
  // 第 3 段前半:**先夾窗的尺寸**。工作區比那一橫還小的話(極小的
  // 虛擬螢幕、或工作列吃掉大半),先讓它縮到放得下,再談位置 ——
  // 順序反過來的話,位置的計算會產生負的邊距。
  w = std::min(w, mon->width());
  h = std::min(h, mon->height());
  w = std::max(w, 1);
  h = std::max(h, 1);

  int x = mon->right - Dip(dx, mon->dpi) - w;
  int y = mon->bottom - Dip(dy, mon->dpi) - h;

  // 第 3 段後半:一律夾進工作區。
  const int max_x = mon->right - w;
  const int max_y = mon->bottom - h;
  const int cx = std::min(std::max(x, mon->left), max_x);
  const int cy = std::min(std::max(y, mon->top), max_y);
  if (cx != x || cy != y) {
    if (out.how == PlacedBar::How::kRestoredExact)
      out.how = PlacedBar::How::kClamped;
  }
  x = cx;
  y = cy;

  out.x = x;
  out.y = y;
  out.w = w;
  out.h = h;
  // ⚠ 交出用了哪一顆 —— 避讓要在同一顆的工作區內做,而三段回落之後
  //   呼叫端自己算不出來(它不知道我們走的是第 1 段還是第 2 段)。
  out.monitor = IndexOf(monitors, mon);
  return out;
}

namespace {

bool Overlaps(int x, int y, int w, int h, const ObstacleRect& o) {
  return x < o.right && o.left < x + w && y < o.bottom && o.top < y + h;
}

}  // namespace

PlacedBar AvoidObstacles(const PlacedBar& placed,
                         const std::vector<WorkArea>& monitors,
                         const std::vector<ObstacleRect>& obstacles) {
  PlacedBar out = placed;
  if (placed.monitor < 0 ||
      static_cast<size_t>(placed.monitor) >= monitors.size())
    return out;
  if (obstacles.empty()) return out;
  const WorkArea& on = monitors[static_cast<size_t>(placed.monitor)];
  if (placed.w <= 0 || placed.h <= 0) return out;

  // 讓開之後留多少空隙。⚠ DIP,依那顆螢幕的 DPI 換算 —— 150% 的螢幕上
  //   8 個像素幾乎貼在一起。
  const int gap = Dip(kBarAvoidGapDip, on.dpi);

  // 只認**跟我們同一顆螢幕**的障礙物,而且要真的擋在路上(x 有重疊)。
  // ⚠ 沒有這一關的話,第二顆螢幕上的一條橫條會把這一條趕到工作區外面。
  std::vector<ObstacleRect> hits;
  for (const ObstacleRect& o : obstacles) {
    if (o.right <= o.left || o.bottom <= o.top) continue;
    if (o.right <= on.left || o.left >= on.right) continue;
    if (o.bottom <= on.top || o.top >= on.bottom) continue;
    if (o.right <= placed.x || o.left >= placed.x + placed.w) continue;
    hits.push_back(o);
  }
  if (hits.empty()) return out;

  auto blocked = [&](int y) -> bool {
    for (const ObstacleRect& o : hits)
      if (Overlaps(placed.x, y, placed.w, placed.h, o)) return true;
    return false;
  };
  if (!blocked(placed.y)) return out;

  // ⚠ 方向由**它自己在工作區裡的位置**決定,不是寫死往上:工作列在
  //   上面(或使用者把那一橫拖到頂了)的時候,往上讓就是往螢幕外面讓。
  const bool in_lower_half =
      (placed.y + placed.h / 2) * 2 >= on.top + on.bottom;

  // 一個方向試到底;讓不開就換另一個方向從原位再試一次。
  auto slide = [&](bool up) -> bool {
    int y = placed.y;
    for (int step = 0; step < kBarAvoidMaxSteps; ++step) {
      // 擋路的那些裡面,挑一個決定下一個落腳點。
      int next = y;
      bool moved = false;
      for (const ObstacleRect& o : hits) {
        if (!Overlaps(placed.x, y, placed.w, placed.h, o)) continue;
        const int cand = up ? o.top - gap - placed.h : o.bottom + gap;
        if (!moved || (up ? cand < next : cand > next)) {
          next = cand;
          moved = true;
        }
      }
      if (!moved) {
        out.y = y;
        out.nudge_dy = y - placed.y;
        if (out.nudge_dy != 0) out.how = PlacedBar::How::kNudgedAside;
        return true;
      }
      // 讓出工作區就是**消失**,而使用者不會把它跟避讓聯想在一起。
      if (next < on.top || next + placed.h > on.bottom) return false;
      y = next;
    }
    return false;
  };

  if (slide(in_lower_half)) return out;
  if (slide(!in_lower_half)) return out;
  // 讓不開 —— 維持原位。壓在別人上面難看,跑到螢幕外面是消失。
  return out;
}

BarAnchor MakeAnchor(const WorkArea& on, int x_px, int y_px, int w_px,
                     int h_px) {
  BarAnchor a;
  a.valid = true;
  a.work_left = on.left;
  a.work_top = on.top;
  a.work_right = on.right;
  a.work_bottom = on.bottom;
  // 相對右下角,而且存 DIP。⚠ 反過來(存左上角)的話,使用者把它靠在
  // 右下角、然後改了螢幕解析度,那一橫會離開它靠著的那個角 ——
  // 而「靠右下」正是他選那個位置的理由。
  const int dx_px = on.right - (x_px + w_px);
  const int dy_px = on.bottom - (y_px + h_px);
  a.dx_dip = MulDivRound(dx_px, 96, on.dpi);
  a.dy_dip = MulDivRound(dy_px, 96, on.dpi);
  return a;
}

std::string SerializeAnchor(const BarAnchor& a) {
  if (!a.valid) return std::string();
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%d,%d,%d,%d,%d,%d", a.work_left, a.work_top,
                a.work_right, a.work_bottom, a.dx_dip, a.dy_dip);
  return std::string(buf);
}

BarAnchor ParseAnchor(const std::string& s) {
  BarAnchor a;
  if (s.empty()) return a;
  long v[6] = {0, 0, 0, 0, 0, 0};
  size_t pos = 0;
  for (int i = 0; i < 6; ++i) {
    if (pos > s.size()) return BarAnchor();
    size_t comma = s.find(',', pos);
    const std::string tok =
        s.substr(pos, comma == std::string::npos ? std::string::npos
                                                 : comma - pos);
    if (tok.empty()) return BarAnchor();
    char* end = nullptr;
    v[i] = std::strtol(tok.c_str(), &end, 10);
    if (!end || *end != '\0') return BarAnchor();  // 有非數字 → 整份丟掉
    if (comma == std::string::npos) {
      if (i != 5) return BarAnchor();  // 欄位不足
      pos = s.size() + 1;
    } else {
      pos = comma + 1;
    }
  }
  // ⚠ **多出來的欄位也要紅。** 少了這一條,"1,2,3,4,5,6,7" 會被當成合法,
  //   而它幾乎一定代表格式換過了 —— 照舊格式解讀一份新格式的字串,
  //   得到的是一個看起來合理、其實指向別處的座標。
  if (pos <= s.size()) return BarAnchor();
  // 工作區矩形必須是正的。反過來的話 FindExact 永遠比不中,
  // 那還算安全(退回主螢幕);但明確擋掉比較誠實。
  if (v[2] <= v[0] || v[3] <= v[1]) return BarAnchor();
  a.valid = true;
  a.work_left = static_cast<int>(v[0]);
  a.work_top = static_cast<int>(v[1]);
  a.work_right = static_cast<int>(v[2]);
  a.work_bottom = static_cast<int>(v[3]);
  a.dx_dip = static_cast<int>(v[4]);
  a.dy_dip = static_cast<int>(v[5]);
  return a;
}

}  // namespace rimewin
