#include "cand_layout.h"

#include <algorithm>

#include "ime_policy.h"

namespace rimewin {
namespace {

double Pick(double v, double fallback) { return v < 0 ? fallback : v; }

}  // namespace

void CandidateStyle::ResolveDefaults() {
  item.padding_h = Pick(item.padding_h, metrics.padding);
  item.padding_v = Pick(item.padding_v, metrics.padding);
  item.spacing = Pick(item.spacing, metrics.spacing);
  item.corner_radius = Pick(item.corner_radius, metrics.corner_radius);
  window.corner_radius = Pick(window.corner_radius, metrics.corner_radius);
  window.padding = Pick(window.padding, metrics.padding);
  window.border_width = Pick(window.border_width, metrics.border_width);
}

WindowLayout ComputeLayout(const std::vector<Candidate>& items,
                           const CandidateStyle& st, const MeasureFn& measure) {
  WindowLayout out;
  if (items.empty()) return out;

  const double frame = st.window.padding + st.window.border_width;
  const bool horizontal = st.EffectiveOrientation() == Orientation::kHorizontal;
  // 標籤／文字／註解之間的間距,規範沒有欄位。暫用 metrics.spacing,已回報。
  const double gap = st.metrics.spacing;

  struct Piece {
    ItemLayout box;
    double content_w = 0, content_h = 0;
  };
  std::vector<Piece> pieces;
  pieces.reserve(items.size());

  for (size_t i = 0; i < items.size(); ++i) {
    const Candidate& c = items[i];
    Piece p;
    p.box.has_label = st.label.show && !c.label.empty();
    p.box.has_comment =
        st.comment.show && st.comment.position != CommentPosition::kHidden &&
        !c.comment.empty();
    if (p.box.has_label)
      p.box.label_display =
          FormatLabel(st.label.format, c.label, static_cast<int32_t>(i));

    const Extent le = p.box.has_label ? measure(p.box.label_display, st.label.size)
                                      : Extent{};
    const Extent te = measure(c.text, st.text.size);
    const Extent ce =
        p.box.has_comment ? measure(c.comment, st.comment.size) : Extent{};

    double line1_w = te.width;
    double line1_h = te.height;
    if (p.box.has_label) {
      line1_w = le.width + gap + te.width;
      line1_h = std::max(line1_h, le.height);
    }

    if (p.box.has_comment && st.comment.position == CommentPosition::kAfter) {
      line1_w += gap + ce.width;
      line1_h = std::max(line1_h, ce.height);
      p.content_w = line1_w;
      p.content_h = line1_h;
    } else if (p.box.has_comment) {  // below
      // 規範明說:position: below 在 orientation: horizontal 下**必須**支援,
      // 候選項變成兩行高。桌面端很常見的排法。
      p.content_w = std::max(line1_w, ce.width);
      p.content_h = line1_h + ce.height;
    } else {
      p.content_w = line1_w;
      p.content_h = line1_h;
    }

    p.box.w = std::max(st.item.min_width, p.content_w + 2 * st.item.padding_h);
    p.box.h = p.content_h + 2 * st.item.padding_v;

    // 內容在 item 內靠左、垂直置中(第一行)。
    double cx = st.item.padding_h;
    if (p.box.has_label) {
      p.box.label_x = cx;
      cx += le.width + gap;
    }
    p.box.text_x = cx;
    cx += te.width;
    if (p.box.has_comment && st.comment.position == CommentPosition::kAfter) {
      p.box.comment_x = cx + gap;
      p.box.comment_y = st.item.padding_v + (line1_h - ce.height) / 2;
    } else if (p.box.has_comment) {
      p.box.comment_x = st.item.padding_h;
      p.box.comment_y = st.item.padding_v + line1_h;
    }
    p.box.label_y = st.item.padding_v + (line1_h - le.height) / 2;
    p.box.text_y = st.item.padding_v + (line1_h - te.height) / 2;

    pieces.push_back(p);
  }

  if (horizontal) {
    double x = frame;
    double max_h = 0;
    size_t placed = 0;
    for (size_t i = 0; i < pieces.size(); ++i) {
      const double need = x + pieces[i].box.w + frame;
      // 第一個候選一定要放進去,就算它自己就超過 max_width ——
      // 否則使用者會看到一個完全空的候選窗,那比太寬更難理解。
      if (i > 0 && need > st.window.max_width) break;
      pieces[i].box.x = x;
      pieces[i].box.y = frame;
      x += pieces[i].box.w + (i + 1 < pieces.size() ? st.item.spacing : 0);
      max_h = std::max(max_h, pieces[i].box.h);
      ++placed;
    }
    out.dropped = static_cast<int32_t>(pieces.size() - placed);
    pieces.resize(placed);
    // 收尾:最後一個 item 後面不該留 spacing。
    double content_right = frame;
    for (size_t i = 0; i < pieces.size(); ++i)
      content_right = std::max(content_right, pieces[i].box.x + pieces[i].box.w);
    // 刻意**不**再夾一次 max_width:上面已經以 max_width 決定放幾個了,
    // 這裡再夾只會把最後一個候選切掉一半 —— 那是「看得到但讀不完」。
    // 唯一會超過 max_width 的情形是單一候選本身就太長,那時寧可窗變寬。
    out.width = std::max(st.window.min_width, content_right + frame);
    out.height = max_h + 2 * frame;
    // 同一列的 item 高度對齊成最高的那一個,免得背景高亮塊參差不齊。
    for (Piece& p : pieces) p.box.h = max_h;
  } else {
    double y = frame;
    double max_w = 0;
    for (size_t i = 0; i < pieces.size(); ++i) {
      pieces[i].box.x = frame;
      pieces[i].box.y = y;
      y += pieces[i].box.h + (i + 1 < pieces.size() ? st.item.spacing : 0);
      max_w = std::max(max_w, pieces[i].box.w);
    }
    // 垂直排列時 max_width 的語意是「候選文字太長要怎麼辦」,而規範把那件事
    // 留給實作(§8.6.7)。目前不截字、不換行,讓窗變寬 —— 已回報。
    out.width = std::max(st.window.min_width, max_w + 2 * frame);
    out.height = y + frame;
    for (Piece& p : pieces) p.box.w = max_w;
  }

  out.items.reserve(pieces.size());
  for (Piece& p : pieces) out.items.push_back(p.box);
  return out;
}

Rect PlaceWindow(const Rect& caret, double w, double h, const Rect& work_area,
                 const CandidateStyle& st) {
  Rect r;

  if (!st.window.follow_caret) {
    // 「固定在螢幕角落」。規範沒說是哪一個角 —— 取右下,並已回報。
    r.left = work_area.right - w;
    r.top = work_area.bottom - h;
    r.right = r.left + w;
    r.bottom = r.top + h;
    return r;
  }

  double x = caret.left + st.window.offset_x;
  // 夾進工作區。右邊放不下就往左推,而不是讓它超出去 ——
  // 超出螢幕的候選窗使用者完全無法自救。
  if (x + w > work_area.right) x = work_area.right - w;
  if (x < work_area.left) x = work_area.left;

  const double below_y = caret.bottom + st.window.offset_y;
  const double above_y = caret.top - st.window.offset_y - h;

  double y;
  switch (st.window.placement) {
    case Placement::kBelow:
      y = below_y;
      break;
    case Placement::kAbove:
      y = above_y;
      break;
    case Placement::kAuto:
    default:
      y = (below_y + h <= work_area.bottom) ? below_y
          : (above_y >= work_area.top)      ? above_y
                                            : below_y;
      break;
  }
  if (y + h > work_area.bottom) y = work_area.bottom - h;
  if (y < work_area.top) y = work_area.top;

  r.left = x;
  r.top = y;
  r.right = x + w;
  r.bottom = y + h;
  return r;
}

}  // namespace rimewin
