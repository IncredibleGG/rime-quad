#include "ui_layout.h"

#include <algorithm>

namespace rimewin {

int ContentWidthDip(int window_w_dip) {
  const int avail = window_w_dip - metric::kSidebarW;
  const int want = avail - 2 * space::s7;
  return std::max(kContentMinW, std::min(kContentMaxW, want));
}

int ContentXDip(int window_w_dip) {
  const int avail = window_w_dip - metric::kSidebarW;
  const int cw = ContentWidthDip(window_w_dip);
  // 置中。W > 840 時兩側留白;W = 660 時 (460-440)/2 = 10,
  // 也就是內距被壓到 10 —— 刻意的,見標頭。
  return metric::kSidebarW + (avail - cw) / 2;
}

RectI SidebarItemDip(int index) {
  RectI r;
  r.x = space::s5;
  r.y = space::s5 + index * (metric::kSidebarItemH + space::s2);
  r.w = metric::kSidebarW - 2 * space::s5;
  r.h = metric::kSidebarItemH;
  return r;
}

RectI SidebarStatusDip(int window_h_dip) {
  RectI r;
  r.x = space::s5;
  r.y = window_h_dip - metric::kSidebarStatusH;
  r.w = metric::kSidebarW - 2 * space::s5;
  r.h = metric::kSidebarStatusH;
  return r;
}

RectI Stack::Push(int h_dip, int gap_dip) {
  RectI r;
  r.x = x_;
  r.y = y_;
  r.w = w_;
  r.h = h_dip;
  y_ += h_dip + gap_dip;
  return r;
}

RectI Stack::PushDivider() {
  y_ += space::s7;
  RectI r;
  r.x = x_;
  r.y = y_;
  r.w = w_;
  r.h = metric::kHairline;
  y_ += metric::kHairline + space::s7;
  return r;
}

std::vector<HitTarget> ClickableTargetsDip(int window_w_dip, int window_h_dip,
                                           int page_count) {
  std::vector<HitTarget> out;
  // 側欄的每一頁。
  for (int i = 0; i < page_count; ++i)
    out.push_back({"sidebar_item", SidebarItemDip(i)});

  // 內容區:用一頁最擁擠的骨架當代表 —— 「輸入方案」頁,
  // 它同時有清單列、上移/下移、開關列與一顆危險鍵。
  const int cx = ContentXDip(window_w_dip);
  const int cw = ContentWidthDip(window_w_dip);
  Stack st(cx, space::s8, cw);
  st.Push(text_size::t1 + space::s3, space::s1);  // 頁標題(不可點)
  st.Push(text_size::t5 + space::s3, space::s7);  // 副標(不可點)

  // 可排序清單:四列。每一列都是可點的(勾選 + 選取)。
  for (int i = 0; i < 4; ++i) {
    const RectI row = st.Push(metric::kSidebarItemH, 0);
    out.push_back({"schema_row", row});
    // 列內的拖曳把手與核取方塊也是可點的小方塊。
    RectI handle{row.x + space::s4, row.y + (row.h - metric::kMinTarget) / 2,
                 metric::kMinTarget, metric::kMinTarget};
    out.push_back({"drag_handle", handle});
  }
  st.Skip(space::s7);

  // 上移 / 下移。
  const RectI up = st.Push(metric::kMinTarget + space::s2, space::s3);
  out.push_back({"move_up", RectI{up.x, up.y, 96, up.h}});
  out.push_back({"move_down", RectI{up.x + 96 + space::s3, up.y, 96, up.h}});

  // 開關列(整列可點,§4.1)。
  const RectI sw = st.Push(metric::kSidebarItemH + space::s4, space::s7);
  out.push_back({"follow_input_mode_switch", sw});

  // 危險區塊:一條 hairline 之後,該頁最後一個。
  st.PushDivider();
  const RectI danger = st.Push(metric::kMinTarget + space::s2, 0);
  out.push_back({"danger_button", RectI{danger.x, danger.y, 160, danger.h}});

  (void)window_h_dip;
  return out;
}

}  // namespace rimewin
