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
