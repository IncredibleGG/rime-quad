// windows/tests/test_layout.cc — 候選窗排版與定位
//
// 候選窗在 CI 上看不到,但「窗有多寬」「翻不翻面」「截斷在第幾個」是算得出來的。
// 算錯的症狀是候選窗跑到螢幕外面、壓在游標上、或是少了幾個候選 ——
// 都是使用者一眼看得到而自動化完全看不到的東西。

#include "../common/cand_layout.h"

#include "check.h"

using namespace rimewin;

namespace {

// 確定性的假量測器:每個 UTF-8 位元組算 0.5 * size 寬,高度就是 size。
// 不是真的字型度量,但排版邏輯本來就不該依賴真實字寬。
Extent FakeMeasure(const std::string& s, double size) {
  Extent e;
  e.width = static_cast<double>(s.size()) * size * 0.5;
  e.height = size;
  return e;
}

CandidateStyle DefaultStyle() {
  CandidateStyle st;
  st.ResolveDefaults();
  return st;
}

std::vector<Candidate> Items(int n) {
  std::vector<Candidate> v;
  for (int i = 0; i < n; ++i)
    v.push_back({"字", "", std::to_string(i + 1)});
  return v;
}

}  // namespace

TEST(layout_metrics_defaults_are_inherited) {
  // 規範 §8.5:candidates.* 沒指定的欄位,預設取自 metrics。
  CandidateStyle st;
  st.metrics.padding = 11;
  st.metrics.spacing = 7;
  st.metrics.corner_radius = 3;
  st.metrics.border_width = 2;
  st.ResolveDefaults();
  CHECK_NEAR(st.item.padding_h, 11, 1e-9);
  CHECK_NEAR(st.item.padding_v, 11, 1e-9);
  CHECK_NEAR(st.item.spacing, 7, 1e-9);
  CHECK_NEAR(st.item.corner_radius, 3, 1e-9);
  CHECK_NEAR(st.window.padding, 11, 1e-9);
  CHECK_NEAR(st.window.corner_radius, 3, 1e-9);
  CHECK_NEAR(st.window.border_width, 2, 1e-9);

  // 明確指定的欄位不可以被 metrics 蓋掉。
  CandidateStyle st2;
  st2.metrics.padding = 11;
  st2.item.padding_h = 2;
  st2.ResolveDefaults();
  CHECK_NEAR(st2.item.padding_h, 2, 1e-9);
  CHECK_NEAR(st2.item.padding_v, 11, 1e-9);
}

TEST(layout_empty_menu_gives_empty_window) {
  const auto st = DefaultStyle();
  const WindowLayout l = ComputeLayout({}, st, FakeMeasure);
  CHECK_INT(l.items.size(), 0);
  CHECK_NEAR(l.width, 0, 1e-9);
  CHECK_NEAR(l.height, 0, 1e-9);
}

TEST(layout_horizontal_geometry) {
  auto st = DefaultStyle();
  st.orientation = Orientation::kHorizontal;
  const WindowLayout l = ComputeLayout(Items(3), st, FakeMeasure);
  CHECK_INT(l.items.size(), 3);
  CHECK_INT(l.dropped, 0);
  // 由左而右,不重疊,而且間距就是 item.spacing。
  for (size_t i = 1; i < l.items.size(); ++i) {
    CHECK(l.items[i].x > l.items[i - 1].x);
    CHECK_NEAR(l.items[i].x - (l.items[i - 1].x + l.items[i - 1].w),
               st.item.spacing, 1e-9);
  }
  // 同一列高度要對齊,否則高亮背景塊會參差不齊。
  CHECK_NEAR(l.items[0].h, l.items[2].h, 1e-9);
  // 窗高 = item 高 + 上下的 padding 與框線。
  const double frame = st.window.padding + st.window.border_width;
  CHECK_NEAR(l.height, l.items[0].h + 2 * frame, 1e-9);
}

TEST(layout_vertical_geometry) {
  auto st = DefaultStyle();
  st.orientation = Orientation::kVertical;
  const WindowLayout l = ComputeLayout(Items(4), st, FakeMeasure);
  CHECK_INT(l.items.size(), 4);
  for (size_t i = 1; i < l.items.size(); ++i)
    CHECK(l.items[i].y > l.items[i - 1].y);
  // 垂直排列時所有 item 等寬,否則高亮塊寬度會跳動。
  for (size_t i = 1; i < l.items.size(); ++i)
    CHECK_NEAR(l.items[i].w, l.items[0].w, 1e-9);
}

TEST(layout_window_orientation_overrides_shared_one) {
  // §8.6.7:candidates.window.orientation 繼承 candidates.orientation,
  // 但可以覆寫。桌面端直排、行動端候選列橫排是常見組合。
  auto st = DefaultStyle();
  st.orientation = Orientation::kHorizontal;
  st.window.orientation_set = true;
  st.window.orientation = Orientation::kVertical;
  const WindowLayout l = ComputeLayout(Items(3), st, FakeMeasure);
  CHECK(l.items[1].y > l.items[0].y);
  CHECK_NEAR(l.items[1].x, l.items[0].x, 1e-9);
}

TEST(layout_max_width_truncates_and_reports) {
  auto st = DefaultStyle();
  st.window.max_width = 120;
  const WindowLayout l = ComputeLayout(Items(20), st, FakeMeasure);
  CHECK(l.items.size() < 20);
  // 沒放進去的候選數必須算得出來 —— 不然「候選少了幾個」查不出原因。
  CHECK_INT(l.dropped, (int)(20 - l.items.size()));
  CHECK(l.width <= st.window.max_width + 1e-9);
}

TEST(layout_first_candidate_always_placed_even_if_too_wide) {
  // 單一候選就超過 max_width 時,寧可窗變寬,也不要給一個空窗 ——
  // 空的候選窗使用者完全無法理解,而且看起來像輸入法壞了。
  auto st = DefaultStyle();
  st.window.max_width = 10;
  std::vector<Candidate> v = {{"非常長的一個候選字串", "", "1"}, {"短", "", "2"}};
  const WindowLayout l = ComputeLayout(v, st, FakeMeasure);
  CHECK_INT(l.items.size(), 1);
  CHECK_INT(l.dropped, 1);
  CHECK(l.width > st.window.max_width);
}

TEST(layout_label_can_be_hidden) {
  auto st = DefaultStyle();
  const WindowLayout with = ComputeLayout(Items(3), st, FakeMeasure);
  st.label.show = false;
  const WindowLayout without = ComputeLayout(Items(3), st, FakeMeasure);
  CHECK(without.width < with.width);
  CHECK(!without.items[0].has_label);
  CHECK(with.items[0].has_label);
  CHECK_STR(with.items[0].label_display, "1");
}

TEST(layout_comment_below_makes_item_two_lines) {
  // 規範 §8.6.3:position: below 在 horizontal 下**必須**支援,候選項變兩行高。
  std::vector<Candidate> v = {{"你好", "ni hao", "1"}};
  auto st = DefaultStyle();
  st.comment.position = CommentPosition::kAfter;
  const WindowLayout after = ComputeLayout(v, st, FakeMeasure);
  st.comment.position = CommentPosition::kBelow;
  const WindowLayout below = ComputeLayout(v, st, FakeMeasure);
  CHECK(below.height > after.height);
  CHECK(below.width < after.width);
  // below 時註解在文字下方。
  CHECK(below.items[0].comment_y > below.items[0].text_y);

  st.comment.position = CommentPosition::kHidden;
  const WindowLayout hidden = ComputeLayout(v, st, FakeMeasure);
  CHECK(!hidden.items[0].has_comment);
  CHECK(hidden.width < after.width);
}

TEST(layout_item_min_width) {
  auto st = DefaultStyle();
  st.item.min_width = 200;
  const WindowLayout l = ComputeLayout(Items(1), st, FakeMeasure);
  CHECK_NEAR(l.items[0].w, 200, 1e-9);
}

// ── 定位(§8.6.7)────────────────────────────────────────────────

namespace {
Rect Screen() { return Rect{0, 0, 1920, 1080}; }
}  // namespace

TEST(place_below_by_default_with_offset) {
  auto st = DefaultStyle();
  st.window.placement = Placement::kBelow;
  st.window.offset_y = 6;
  const Rect caret{100, 200, 102, 220};
  const Rect r = PlaceWindow(caret, 300, 40, Screen(), st);
  CHECK_NEAR(r.left, 100, 1e-9);
  CHECK_NEAR(r.top, 226, 1e-9);  // caret.bottom + offset_y
  CHECK_NEAR(r.width(), 300, 1e-9);
}

TEST(place_auto_flips_when_no_room_below) {
  auto st = DefaultStyle();
  st.window.placement = Placement::kAuto;
  // 游標在螢幕最底:下面放不下,必須翻到上面。
  const Rect caret{100, 1040, 102, 1060};
  const Rect r = PlaceWindow(caret, 300, 200, Screen(), st);
  CHECK(r.bottom <= caret.top);
  CHECK(r.top >= 0);
}

TEST(place_auto_stays_below_when_it_fits) {
  auto st = DefaultStyle();
  st.window.placement = Placement::kAuto;
  const Rect caret{100, 200, 102, 220};
  const Rect r = PlaceWindow(caret, 300, 200, Screen(), st);
  CHECK(r.top >= caret.bottom);
}

TEST(place_never_leaves_the_work_area) {
  auto st = DefaultStyle();
  // 螢幕外的候選窗使用者完全無法自救,所以無論如何都要夾回來。
  const Rect caret{1900, 1070, 1902, 1078};
  const Rect r = PlaceWindow(caret, 600, 400, Screen(), st);
  CHECK(r.left >= 0);
  CHECK(r.top >= 0);
  CHECK(r.right <= 1920);
  CHECK(r.bottom <= 1080);
}

TEST(place_handles_negative_screen_origin) {
  // 多螢幕:第二個螢幕在主螢幕左邊時,座標是負的。
  auto st = DefaultStyle();
  const Rect work{-1920, 0, 0, 1080};
  const Rect caret{-1000, 500, -998, 520};
  const Rect r = PlaceWindow(caret, 300, 100, work, st);
  CHECK(r.left >= -1920);
  CHECK(r.right <= 0);
}

TEST(place_follow_caret_false_pins_to_corner) {
  auto st = DefaultStyle();
  st.window.follow_caret = false;
  const Rect caret{100, 200, 102, 220};
  const Rect r = PlaceWindow(caret, 300, 100, Screen(), st);
  CHECK_NEAR(r.right, 1920, 1e-9);
  CHECK_NEAR(r.bottom, 1080, 1e-9);
}
