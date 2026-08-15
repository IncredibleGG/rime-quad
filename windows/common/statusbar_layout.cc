#include "statusbar_layout.h"

#include <algorithm>

namespace rimewin {
namespace {

// 第 index 格在四格模式下的內容寬上限。只有第 3 格(index 2,方案名)有上限。
int ContentCapFor(size_t index) {
  return index == 2 ? barmetric::kSchemaContentMaxW : -1;
}

}  // namespace

BarLayout LayoutStatusBarCellsDip(const std::vector<BarCellIn>& in) {
  BarLayout out;
  out.cells.assign(in.size(), BarCell{});

  // 步驟 1:第 3 格的內容寬上限 120。
  std::vector<int> content(in.size(), 0);
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i].skip || in[i].text_w_dip <= 0) continue;
    int w = in[i].text_w_dip;
    const int cap = ContentCapFor(i);
    if (cap > 0 && w > cap) {
      w = cap;
      out.schema_truncated = true;
    }
    content[i] = w;
  }

  // 步驟 2:擺位置。
  //
  // ⚠ 上下各內縮 s1(2),**不是** kBarBorder(1)。那一個字就是
  //   §12.14.0 第 5 條:每一格 26 DIP 高,低於 §3.6 的 28。
  const int top = barmetric::kInsetV;
  const int cell_h = barmetric::kBarH - 2 * barmetric::kInsetV;  // 28
  int x = barmetric::kInsetH;
  int last_right = x;
  bool any = false;
  // 略過的那幾格不佔位置,所以「上一格畫過了嗎」要自己記 —— 直接用
  // index 判斷「前面那一格是不是第 3 格」在第 3 格被略過時會畫錯線。
  int drawn_index = -1;
  for (size_t i = 0; i < in.size(); ++i) {
    if (content[i] <= 0) {
      out.cells[i].skipped = true;
      out.cells[i].rect = RectI{};
      continue;
    }
    if (any) {
      // 第 3 格與第 4 格之間多一條分隔線(左右各 s3)。
      if (drawn_index == 2 && i == 3) {
        x += barmetric::kSepGap;
        out.separator_x_dip = x;
        x += metric::kHairline + barmetric::kSepGap;
      } else {
        x += barmetric::kCellGap;
      }
    }
    int w = content[i] + 2 * barmetric::kCellPadH;
    if (w < barmetric::kCellMinW) w = barmetric::kCellMinW;
    out.cells[i].rect = RectI{x, top, w, cell_h};
    out.cells[i].skipped = false;
    x += w;
    last_right = x;
    any = true;
    drawn_index = static_cast<int>(i);
  }
  out.total_w_dip = any ? last_right + barmetric::kInsetH
                        : barmetric::kCellMinW;

  // 步驟 3:整條的寬上限 320。超過時**只壓第 3 格**(1、2、4 格都是
  // 固定的短字串,壓它們沒有用)。
  //
  // ⚠ 步驟 4:壓到 120 還是超過 320 → **讓它超過**,不得再往下壓。
  //   第 3 格是使用者辨認「現在是哪一種打字方式」的唯一依據,
  //   壓成兩個字等於沒有。所以這裡的下界就是 kSchemaContentMaxW。
  if (out.total_w_dip > barmetric::kBarMaxW && content.size() > 2 &&
      content[2] > 0) {
    const int over = out.total_w_dip - barmetric::kBarMaxW;
    const int floor_w = std::min(content[2], barmetric::kSchemaContentMaxW);
    const int want = content[2] - over;
    const int next = std::max(floor_w, want);
    if (next < content[2]) {
      std::vector<BarCellIn> again = in;
      again[2].text_w_dip = next;
      BarLayout retry = LayoutStatusBarCellsDip(again);
      retry.schema_truncated = true;
      return retry;
    }
  }
  return out;
}

BarLayout LayoutStatusBarSentenceDip(int text_w_dip) {
  BarLayout out;
  BarCell c;
  // 整條一句話:左右內距 s5,整條可點(§12.14.7 的第五種外觀)。
  int w = (text_w_dip > 0 ? text_w_dip : 0) + 2 * space::s5;
  if (w < barmetric::kCellMinW) w = barmetric::kCellMinW;
  // ⚠ 滑過／按下畫的是**整條**,不是那一格的矩形 —— 所以這一格的
  //   矩形就是整條(扣掉外框那一圈)。現況畫的是格子,圓角也不對。
  c.rect = RectI{0, 0, w, barmetric::kBarH};
  c.skipped = false;
  out.cells.push_back(c);
  out.total_w_dip = w;
  return out;
}

}  // namespace rimewin
