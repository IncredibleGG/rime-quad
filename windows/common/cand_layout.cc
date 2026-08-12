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
  // §8.6.7.1:column_gap / row_gap 預設 = candidates.item.spacing。
  // ⚠ 要在 item.spacing 解析完之後才取,否則會拿到 -1。
  window.column_gap = Pick(window.column_gap, item.spacing);
  window.row_gap = Pick(window.row_gap, item.spacing);
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

  // ── §8.6.7.1 第 6–9 步 + §8.6.7.2 ────────────────────────────────
  //
  // ⚠ **每一條路徑都必須輸出全部 n 項**(§8.6.7.2 第一節)。這一段沒有
  //   任何 `break`、沒有 `resize`、沒有 `continue` —— 那是刻意的,
  //   而 W15 會餵一組必定溢出的輸入來守它。
  //
  // 目前恆為單行(§8.6.7.1 的 lines: 1):
  //   horizontal → columns = n、rows = 1,第 c 欄就是第 c 個候選
  //   vertical   → columns = 1、rows = n,唯一那一欄取全頁最寬
  // 表格排版(lines >= 2)還沒做,見標頭的「已知不足」。

  const size_t n = pieces.size();
  const double axis_gap = horizontal ? st.window.column_gap : st.window.row_gap;

  // 第 6 步:欄寬。同時記下每一項的「量測寬」——第 8 步要拿它比對格寬。
  std::vector<double> natural(n);
  for (size_t i = 0; i < n; ++i) natural[i] = pieces[i].box.w;

  std::vector<double> colw;
  if (horizontal) {
    colw = natural;  // 逐欄取該欄唯一那一項(等同 equal_columns: false)
  } else {
    double max_w = 0;
    for (double v : natural) max_w = std::max(max_w, v);
    colw.assign(1, max_w);
  }
  const size_t columns = colw.size();

  auto sum_cols = [&]() {
    double s = 0;
    for (double v : colw) s += v;
    return s;
  };
  auto content_width = [&]() {
    return sum_cols() + static_cast<double>(columns - 1) * axis_gap;
  };

  // 第 2 步:可用內容寬。max_width <= 0 = 不限。
  const bool bounded = st.window.max_width > 0;
  const double avail_w =
      bounded ? st.window.max_width - 2 * frame : 0;  // bounded 時才有意義

  // 第 8 步:超出 avail_w 時的處置。**兩者都保留全部 n 項。**
  bool shrunk_to_floor = false;
  if (bounded && content_width() > avail_w) {
    if (st.window.overflow == Overflow::kShrink) {
      const double room =
          std::max(0.0, avail_w - static_cast<double>(columns - 1) * axis_gap);
      const double total = sum_cols();
      const double scale = total > 0 ? room / total : 1.0;
      for (double& v : colw) {
        // ⚠ **不得**把任何一欄縮成 0(§8.6.7.1 第 8 步):那會產生一個
        //   看得見卻讀不到的候選 —— 與丟掉它幾乎一樣糟。
        v = std::max(st.item.min_width, v * scale);
      }
      // 夾到 item.min_width 之後仍然超出 → 接受超出,由 9a 讓窗變寬。
      shrunk_to_floor = content_width() > avail_w;
    }
    // clip:欄寬不動,窗寬由第 9 步夾住,超出的部分被裁掉。
  }

  // 第 8 步末:量測寬 > 拿到的格寬 → 標記需要截斷,渲染端補 `…`。
  // ⚠ clip 底下不標記:那些項是被窗蓋住,不是被截短(§8.6.7.2 第一節)。
  if (st.window.overflow == Overflow::kShrink) {
    for (size_t i = 0; i < n; ++i) {
      const size_t c = horizontal ? i : 0;
      if (natural[i] > colw[c] + 1e-9) {
        pieces[i].box.truncated = true;
        ++out.truncated_count;
      }
    }
  }

  // 落點。欄寬定了才擺,這樣 shrink 的位移是對的。
  double item_h = 0;
  for (const Piece& p : pieces) item_h = std::max(item_h, p.box.h);

  if (horizontal) {
    double x = frame;
    for (size_t i = 0; i < n; ++i) {
      pieces[i].box.x = x;
      pieces[i].box.y = frame;
      pieces[i].box.w = colw[i];
      // 同一列高度對齊成最高的那一個,免得高亮背景塊參差不齊。
      pieces[i].box.h = item_h;
      x += colw[i] + axis_gap;  // 最後一項多加的 axis_gap 不影響 content_w(下面自己算)
    }
    out.height = item_h + 2 * frame;
  } else {
    // §8.6.7.1 第 1 步:列高一律相同(comment.position: below 會讓部分項
    // 高一倍;用最大值才不會互相蓋住)。
    for (size_t i = 0; i < n; ++i) {
      pieces[i].box.x = frame;
      pieces[i].box.y = frame + static_cast<double>(i) * (item_h + axis_gap);
      pieces[i].box.w = colw[0];
      pieces[i].box.h = item_h;
    }
    // 最後一項後面不留 axis_gap。
    out.height = static_cast<double>(n) * item_h +
                 static_cast<double>(n - 1) * axis_gap + 2 * frame;
  }

  // 第 9 步 + §8.6.7.2 第二節的兩條例外。
  const double content_w = content_width();
  double effective_max = bounded ? st.window.max_width : 0;  // 0 = ∞
  if (bounded) {
    if (shrunk_to_floor) {
      // 9a:shrink 的承諾是「縮,不裁」。縮到 item.min_width 還是放不下時,
      //     守住承諾的唯一辦法是讓窗變寬。
      effective_max = content_w + 2 * frame;
    } else if (colw[0] + 2 * frame > effective_max) {
      // 9b:第一個候選一定要看得見 —— 一個空的(或只看得到半個字的)
      //     候選窗比一個太寬的候選窗更難理解,而 max_width 寫得太小
      //     是主題的筆誤,不是使用者的錯。
      effective_max = colw[0] + 2 * frame;
    }
  }

  double w = content_w + 2 * frame;
  // min_width 是下界,而且**優先於** effective_max(clamp 的既有語義)。
  if (bounded && w > effective_max) w = effective_max;
  out.width = std::max(st.window.min_width, w);

  out.items.reserve(n);
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

int32_t WheelPageSteps(int32_t* accumulator, int32_t delta) {
  // Win32 的 WHEEL_DELTA。這裡寫死是刻意的:本檔不得 include windows.h
  // (它要在 Ubuntu 上編得起來),而這個值是 API 契約的一部分,不會變。
  const int32_t kWheelDelta = 120;
  if (!accumulator) return 0;
  if (delta == 0) return 0;
  // 換方向 → 舊的餘數作廢。見標頭。
  if ((*accumulator > 0) != (delta > 0)) *accumulator = 0;
  *accumulator += delta;
  const int32_t notches = *accumulator / kWheelDelta;
  *accumulator -= notches * kWheelDelta;
  // 捲輪往上(delta > 0)= 往**前**一頁。
  return -notches;
}

}  // namespace rimewin
