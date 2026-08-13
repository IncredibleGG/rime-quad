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

// ── 量測寬可以指定的候選 ────────────────────────────────────────
//
// docs/theme-format.md §10 的第 29／30 條是**逐項驗算**的:它們說
// 「量測寬 400 的候選」。要驗那些數字,量測寬就必須是一個我們指定的值,
// 而不是「FakeMeasure 對某個中文字剛好算出來的東西」。
//
// FakeMeasure 是 bytes * size * 0.5,所以 8 個位元組的字串寬 = 4 * size。
// 取 size = want / 4 就得到正好 want 的量測寬。
// padding_h 一併設 0,免得它混進「格寬」裡 —— 規範的 w[i] 已含 padding,
// 這裡讓兩者相等,驗算才對得起來。
constexpr const char* kEightBytes = "12345678";

CandidateStyle ExactWidthStyle(double want) {
  CandidateStyle st;
  st.metrics.padding = 6;
  st.metrics.border_width = 0;
  st.label.show = false;
  st.comment.show = false;
  st.text.size = want / 4.0;
  st.ResolveDefaults();
  st.item.padding_h = 0;
  st.item.padding_v = 0;
  st.item.min_width = 0;
  st.window.min_width = 0;
  return st;
}

std::vector<Candidate> ExactItems(int n) {
  std::vector<Candidate> v;
  for (int i = 0; i < n; ++i) v.push_back({kEightBytes, "", ""});
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
  const WindowLayout l = ComputeLayout({}, st, FakeMeasure, PageHint());
  CHECK_INT(l.items.size(), 0);
  CHECK_NEAR(l.width, 0, 1e-9);
  CHECK_NEAR(l.height, 0, 1e-9);
}

TEST(layout_horizontal_geometry) {
  auto st = DefaultStyle();
  st.orientation = Orientation::kHorizontal;
  const WindowLayout l = ComputeLayout(Items(3), st, FakeMeasure, PageHint());
  CHECK_INT(l.items.size(), 3);
  CHECK_INT(l.truncated_count, 0);
  // 由左而右,不重疊,而且間距就是 column_gap(預設 = item.spacing)。
  for (size_t i = 1; i < l.items.size(); ++i) {
    CHECK(l.items[i].x > l.items[i - 1].x);
    CHECK_NEAR(l.items[i].x - (l.items[i - 1].x + l.items[i - 1].w),
               st.window.column_gap, 1e-9);
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
  const WindowLayout l = ComputeLayout(Items(4), st, FakeMeasure, PageHint());
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
  const WindowLayout l = ComputeLayout(Items(3), st, FakeMeasure, PageHint());
  CHECK(l.items[1].y > l.items[0].y);
  CHECK_NEAR(l.items[1].x, l.items[0].x, 1e-9);
}

// ── W15 / §8.6.7.2:不得丟棄候選 ──────────────────────────────────
//
// ⚠ 這一組取代了舊的 layout_max_width_truncates_and_reports 與
//   layout_first_candidate_always_placed_even_if_too_wide。那兩個測試
//   **斷言的正是那個 bug**:「放不下就少給幾項」。它們是綠的,而使用者
//   按 `5` 會選到看不見的字 —— 一個測試把規範反過來寫,比沒有測試更糟。

TEST(layout_never_drops_candidates_horizontal_shrink) {
  auto st = DefaultStyle();
  st.window.max_width = 120;
  st.window.overflow = Overflow::kShrink;
  const WindowLayout l = ComputeLayout(Items(20), st, FakeMeasure, PageHint());
  // 規範性:輸出項數 == 輸入項數。序號標籤與數字鍵是一一對應的。
  CHECK_INT(l.items.size(), 20);
  CHECK(l.truncated_count > 0);   // 縮到底了,所以有項要補 `…`
  // 每一項都必須有正的寬度:縮成 0 的候選「看得見卻讀不到」。
  for (const auto& it : l.items) CHECK(it.w > 0);
}

TEST(layout_never_drops_candidates_horizontal_clip) {
  auto st = DefaultStyle();
  st.window.max_width = 120;
  st.window.overflow = Overflow::kClip;
  const WindowLayout l = ComputeLayout(Items(20), st, FakeMeasure, PageHint());
  CHECK_INT(l.items.size(), 20);
  // clip 裁的是像素,不是候選 —— 被窗蓋住的項**不**算截斷。
  CHECK_INT(l.truncated_count, 0);
  CHECK(l.width <= st.window.max_width + 1e-9);
}

TEST(layout_never_drops_candidates_vertical_both) {
  // §8.6.7.2 第三節:直排與橫排走同一套,orientation 不決定溢出處置。
  for (int mode = 0; mode < 2; ++mode) {
    auto st = DefaultStyle();
    st.orientation = Orientation::kVertical;
    st.window.max_width = 40;
    st.window.overflow = mode == 0 ? Overflow::kShrink : Overflow::kClip;
    std::vector<Candidate> v = {{"非常長的一個候選字串", "", "1"},
                               {"也很長的另外一個候選", "", "2"},
                               {"短", "", "3"}};
    const WindowLayout l = ComputeLayout(v, st, FakeMeasure, PageHint());
    CHECK_INT(l.items.size(), 3);
    for (const auto& it : l.items) CHECK(it.w > 0);
  }
}

TEST(layout_max_width_is_not_a_hard_bound_single_item) {
  // docs/theme-format.md §10 第 29 條前半,逐項驗算:
  // 單一量測寬 400 的候選、max_width 300、padding 6、min_width 0、
  // item.min_width 0。
  auto st = ExactWidthStyle(400);
  st.window.max_width = 300;

  st.window.overflow = Overflow::kShrink;
  const WindowLayout a = ComputeLayout(ExactItems(1), st, FakeMeasure, PageHint());
  CHECK_INT(a.items.size(), 1);
  CHECK_NEAR(a.items[0].w, 288, 1e-9);   // 欄寬縮成 288
  CHECK_NEAR(a.width, 300, 1e-9);        // 窗寬 300
  CHECK(a.items[0].truncated);           // 該項被標記為需要截斷
  CHECK_INT(a.truncated_count, 1);

  st.window.overflow = Overflow::kClip;
  const WindowLayout b = ComputeLayout(ExactItems(1), st, FakeMeasure, PageHint());
  CHECK_INT(b.items.size(), 1);
  CHECK_NEAR(b.items[0].w, 400, 1e-9);   // 欄寬維持 400
  CHECK_NEAR(b.width, 412, 1e-9);        // 9b 抬高了上界
  CHECK(!b.items[0].truncated);          // clip 不標記截斷
  CHECK_INT(b.truncated_count, 0);
}

TEST(layout_shrink_floor_widens_the_window_9a) {
  // §10 第 29 條後半:item.min_width 150、3 個各寬 200、column_gap 4、
  // max_width 300、padding 6。
  // shrink 縮到 93⅓ 後被 min_width 夾回 150,content_w = 3*150 + 2*4 = 458
  // 仍超出 avail_w(288)→ 9a 把窗寬抬成 458 + 12 = 470,
  // 三項全部標記為需要截斷(量測寬 200 > 格寬 150)。
  auto st = ExactWidthStyle(200);
  st.window.max_width = 300;
  st.window.column_gap = 4;
  st.window.overflow = Overflow::kShrink;
  st.item.min_width = 150;
  const WindowLayout l = ComputeLayout(ExactItems(3), st, FakeMeasure, PageHint());
  CHECK_INT(l.items.size(), 3);
  for (const auto& it : l.items) CHECK_NEAR(it.w, 150, 1e-9);
  CHECK_NEAR(l.width, 470, 1e-9);
  CHECK_INT(l.truncated_count, 3);

  // 對照第 21 條:item.min_width 為 0 時同樣的輸入是窗寬 300。
  auto st0 = st;
  st0.item.min_width = 0;
  const WindowLayout l0 = ComputeLayout(ExactItems(3), st0, FakeMeasure, PageHint());
  CHECK_INT(l0.items.size(), 3);
  CHECK_NEAR(l0.width, 300, 1e-9);
}

TEST(layout_clip_keeps_the_computed_positions_case30) {
  // §10 第 30 條:3 個各寬 200 的候選、max_width 300、overflow clip →
  // 排版結果**必須含有三項**,落點依序 x = 0 / 204 / 408(內容區相對),
  // 窗寬 300;需要截斷的項數為 0(它們是被窗蓋住,不是被截斷)。
  auto st = ExactWidthStyle(200);
  st.window.max_width = 300;
  st.window.column_gap = 4;
  st.window.overflow = Overflow::kClip;
  const WindowLayout l = ComputeLayout(ExactItems(3), st, FakeMeasure, PageHint());
  const double frame = st.window.padding + st.window.border_width;
  CHECK_INT(l.items.size(), 3);
  CHECK_NEAR(l.items[0].x - frame, 0, 1e-9);
  CHECK_NEAR(l.items[1].x - frame, 204, 1e-9);
  CHECK_NEAR(l.items[2].x - frame, 408, 1e-9);
  CHECK_NEAR(l.width, 300, 1e-9);
  CHECK_INT(l.truncated_count, 0);
}

TEST(layout_single_over_wide_candidate_still_visible) {
  // 舊測試斷言「第二項被丟掉」。現在改成:兩項都在,而且第一項看得見。
  auto st = DefaultStyle();
  st.window.max_width = 10;
  std::vector<Candidate> v = {{"非常長的一個候選字串", "", "1"}, {"短", "", "2"}};
  for (int mode = 0; mode < 2; ++mode) {
    st.window.overflow = mode == 0 ? Overflow::kShrink : Overflow::kClip;
    const WindowLayout l = ComputeLayout(v, st, FakeMeasure, PageHint());
    CHECK_INT(l.items.size(), 2);
    CHECK(l.width > st.window.max_width);  // 9a 或 9b 抬高了上界
  }
}

TEST(layout_label_can_be_hidden) {
  auto st = DefaultStyle();
  const WindowLayout with = ComputeLayout(Items(3), st, FakeMeasure, PageHint());
  st.label.show = false;
  const WindowLayout without = ComputeLayout(Items(3), st, FakeMeasure, PageHint());
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
  const WindowLayout after = ComputeLayout(v, st, FakeMeasure, PageHint());
  st.comment.position = CommentPosition::kBelow;
  const WindowLayout below = ComputeLayout(v, st, FakeMeasure, PageHint());
  CHECK(below.height > after.height);
  CHECK(below.width < after.width);
  // below 時註解在文字下方。
  CHECK(below.items[0].comment_y > below.items[0].text_y);

  st.comment.position = CommentPosition::kHidden;
  const WindowLayout hidden = ComputeLayout(v, st, FakeMeasure, PageHint());
  CHECK(!hidden.items[0].has_comment);
  CHECK(hidden.width < after.width);
}

TEST(layout_item_min_width) {
  auto st = DefaultStyle();
  st.item.min_width = 200;
  const WindowLayout l = ComputeLayout(Items(1), st, FakeMeasure, PageHint());
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

// ══════════════════════════════════════════════════════════════════
//  滾輪要翻幾頁(G71)
// ══════════════════════════════════════════════════════════════════
//
// ⚠ 這一組守的是「精密觸控板」。滑鼠的滾輪一格固定送 ±120,寫成
//   `delta > 0 ? 上一頁 : 下一頁` 在滑鼠上完全正常 —— 而精密觸控板
//   一次輕撥會送出一連串很小的增量(十幾則 ±8),於是使用者輕輕一撥
//   就翻掉十幾頁,再也回不到他原本看的那一頁。
//   這個缺陷在 Ubuntu 上驗得到,在真 Windows 上要有觸控板才碰得到。

TEST(wheel_one_full_notch_is_exactly_one_page) {
  int32_t acc = 0;
  // 捲輪往上 = 往**前**一頁(與閱讀方向一致,四端要一樣)。
  CHECK_INT(WheelPageSteps(&acc, 120), -1);
  CHECK_INT(acc, 0);
  CHECK_INT(WheelPageSteps(&acc, -120), 1);
  CHECK_INT(acc, 0);
}

TEST(wheel_small_touchpad_increments_accumulate_into_one_page) {
  int32_t acc = 0;
  // 四則 -30(往下輕撥一次)只可以翻**一**頁,不是四頁。
  CHECK_INT(WheelPageSteps(&acc, -30), 0);
  CHECK_INT(WheelPageSteps(&acc, -30), 0);
  CHECK_INT(WheelPageSteps(&acc, -30), 0);
  CHECK_INT(WheelPageSteps(&acc, -30), 1);
  CHECK_INT(acc, 0);
}

TEST(wheel_reversing_direction_drops_the_leftover) {
  int32_t acc = 0;
  CHECK_INT(WheelPageSteps(&acc, -90), 0);  // 往下撥了一點點
  CHECK_INT(acc, -90);
  // 改成往上撥:那 -90 的殘值**不可以**留著。留著的話,往上撥
  // 只要 30 就翻一頁,而使用者感覺是「有時候一撥就跳、有時候撥不動」。
  CHECK_INT(WheelPageSteps(&acc, 30), 0);
  CHECK_INT(acc, 30);
}

TEST(wheel_a_hard_flick_can_turn_more_than_one_page) {
  int32_t acc = 0;
  CHECK_INT(WheelPageSteps(&acc, -360), 3);
  CHECK_INT(acc, 0);
}

TEST(wheel_zero_delta_does_nothing_and_null_is_safe) {
  int32_t acc = 55;
  CHECK_INT(WheelPageSteps(&acc, 0), 0);
  CHECK_INT(acc, 55);  // 連累積器都不可以動
  CHECK_INT(WheelPageSteps(nullptr, 120), 0);
}

// ══════════════════════════════════════════════════════════════════
//  頁碼(G72)—— 字面照 docs/theme-format.md §8.12,一個字都沒有自己發明
// ══════════════════════════════════════════════════════════════════
//
// §8.12 的規則(規範性):
//     page_no == 0 且 is_last_page  → 空(整項不顯示)
//     否則                          → "<page_no + 1>",非最後一頁時後綴 "+"
//
// ⚠ 「只有一頁就不顯示」不是省事,是規範明文:候選只有一頁是最常見的
//   情形,每次組字都掛一個「1」是純粹的噪音。
// ⚠ 後綴用 `+` 而不是 `n/m`,因為 librime **不提供總頁數**
//   (`rs_menu` 只有 is_last_page)。寫成 `1/3` 就得靠猜。

TEST(page_indicator_is_empty_on_a_single_page) {
  CHECK_STR(PageIndicatorText(0, true), "");
}

TEST(page_indicator_says_there_is_more_on_the_first_page) {
  // 這一格就是 G72 的全部:使用者在第一頁時要知道**後面還有**。
  CHECK_STR(PageIndicatorText(0, false), "1+");
}

TEST(page_indicator_counts_from_one_not_zero) {
  // page_no 是 0-based,畫出來是 1-based。差一格的話,使用者按 PageDown
  // 一次會看到「1」,而他明明已經翻過去了。
  CHECK_STR(PageIndicatorText(1, true), "2");
  CHECK_STR(PageIndicatorText(1, false), "2+");
  CHECK_STR(PageIndicatorText(9, false), "10+");
}

TEST(layout_leaves_no_room_for_the_page_hint_on_a_single_page) {
  // §8.12「空狀態必須整項略過」:不佔位置,不是畫一塊看不出用途的空白。
  CandidateStyle st;
  st.ResolveDefaults();
  const std::vector<Candidate> items = Items(3);
  const WindowLayout one = ComputeLayout(items, st, FakeMeasure, PageHint());
  CHECK_STR(one.page_text, "");
  PageHint more;
  more.page_no = 0;
  more.is_last_page = false;
  const WindowLayout multi = ComputeLayout(items, st, FakeMeasure, more);
  CHECK_STR(multi.page_text, "1+");
  // 有頁碼的那一份**必須**比較高 —— 否則那一行會壓在候選上面。
  CHECK(multi.height > one.height);
  // 而候選本身一項都不能少(§8.6.7.2 第一條)。
  CHECK_INT(static_cast<int>(multi.items.size()),
            static_cast<int>(items.size()));
  // 頁碼要落在候選底下,不是壓在它們身上。
  CHECK(multi.page_y >= one.height - 1e-9);
}

TEST(layout_widens_the_window_rather_than_cutting_the_page_hint) {
  // 一兩個字元被裁掉之後,使用者看到的是「有一頁」而不是「後面還有」——
  // 那比不畫更糟。窄窗時寧可讓窗變寬。
  CandidateStyle st;
  st.ResolveDefaults();
  st.window.min_width = 0;
  const std::vector<Candidate> items = Items(1);
  PageHint more;
  more.page_no = 9;
  more.is_last_page = false;
  const WindowLayout w = ComputeLayout(items, st, FakeMeasure, more);
  CHECK_STR(w.page_text, "10+");
  CHECK(w.page_x + FakeMeasure(w.page_text, st.window.status_size).width <=
        w.width + 1e-9);
}
