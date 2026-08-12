// windows/common/cand_layout.h — 候選窗的排版計算(純邏輯)
//
// ⚠ **規範所有權**:候選窗那部分的規範由 macOS 端(第一個桌面端)擴充,
//   Windows 端繼承。本檔只實作 docs/theme-format.md **已經寫下來**的欄位,
//   一個也不多。發現不足一律寫進 docs/coordination.md §5 回報,不自己加。
//   規範一分岔,「一套配置四端共用」這個主張就沒了,而且會安靜地分岔。
//
// ── 2026-08-09:max_width 溢出改成照 §8.6.7.2(規範性)────────────
//
// 舊版在橫排時**丟掉**放不下的候選(`break` + `out.dropped`)。那不是外觀
// 問題:一頁有幾個候選由方案的 page_size 決定,而序號標籤與使用者按下的
// 數字鍵是一一對應的 —— 丟掉第 5 個之後,使用者按 `5` **仍然會選到那個
// 看不見的字**。畫面與行為分岔,而且沒有任何提示。
//
// 現在照 §8.6.7.2:**不得少畫本頁的任何一個候選**,兩種處置都保留全部 n 項:
//   · shrink — 縮欄寬,放不下的項標記 `truncated`,渲染端補 `…`(U+2026)。
//   · clip   — 欄寬不動,超出窗的部分被裁掉。**裁掉的是像素,不是候選**;
//              被裁到一半的項**不**標記 truncated(它沒有被截斷,只是被蓋住)。
// 而 `max_width` 不再是硬上界:9a/9b 兩條例外見 ComputeLayout。
//
// 已知不足(已回報,見 coordination.md §5):
//   · §8.6.7.1 的多行／表格排版(`lines`、`equal_columns`、`item_align`、
//     `max_height`)本檔尚未實作 —— 目前恆為單行(lines: 1),而欄寬是
//     逐項的(等同 `equal_columns: false`)。溢出處置已經照 §8.6.7.2 落地,
//     那是 M1;表格排版是 M3/M4。
//   · 標籤與候選文字之間、候選文字與註解之間的間距沒有欄位。
//     本檔暫用 metrics.spacing(§8.6.4.1 已補上欄位,尚未接 —— M3)。
//   · 狀態列(中/英、簡/繁)的外觀:§8.12 已經補上了,尚未接(M5)。
//
// 為什麼要把排版抽成純函式:候選窗在 CI 上看不到。但「窗有多寬」「翻不翻面」
// 「截斷在第幾個」這些是可以算的,算錯的話使用者看到的是候選窗跑到螢幕外面
// 或是壓在游標上。文字量測(要字型)由外面注入,測試給一個確定性的假量測器。
//
#ifndef RIMEWIN_CAND_LAYOUT_H_
#define RIMEWIN_CAND_LAYOUT_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "protocol.h"

namespace rimewin {

enum class Orientation { kHorizontal, kVertical };
enum class CommentPosition { kAfter, kBelow, kHidden };
enum class Placement { kBelow, kAbove, kAuto };
// §8.6.7.1 的 `overflow`。⚠ 兩者都**保留全部候選**,差別只在「縮」還是「裁」。
enum class Overflow { kShrink, kClip };

struct Rgba {
  uint8_t r = 0, g = 0, b = 0, a = 255;
};

// §8.5。本區塊是「建議值來源」,不直接被畫出來:
// candidates.* 底下沒指定的欄位,預設值取自這裡。
struct Metrics {
  double corner_radius = 8;
  double border_width = 0;
  double padding = 6;
  double spacing = 4;
  double elevation = 2;
};

struct LabelStyle {  // §8.6.1
  bool show = true;
  std::string format = "{label}";
  double size = 12;
  Rgba color{0x80, 0x80, 0x80, 0xFF};
  Rgba highlight_color{0x80, 0x80, 0x80, 0xFF};
};

struct TextStyle {  // §8.6.2
  double size = 20;
  Rgba color{0, 0, 0, 0xFF};
  Rgba highlight_color{0, 0, 0, 0xFF};
};

struct CommentStyle {  // §8.6.3
  bool show = true;
  CommentPosition position = CommentPosition::kAfter;
  double size = 12;
  Rgba color{0x80, 0x80, 0x80, 0xFF};
  Rgba highlight_color{0x80, 0x80, 0x80, 0xFF};
};

struct ItemStyle {  // §8.6.4。-1 代表「未指定,套用 metrics」
  double padding_h = -1;
  double padding_v = -1;
  double spacing = -1;
  double corner_radius = -1;
  double min_width = 0;
  Rgba background{0, 0, 0, 0};
  Rgba highlight_background{0x30, 0x60, 0xC0, 0xFF};
  double border_width = 0;
  Rgba border_color{0, 0, 0, 0};
  double highlight_border_width = 0;
  Rgba highlight_border_color{0, 0, 0, 0};
};

struct WindowStyle {  // §8.6.7
  Rgba background{0xFF, 0xFF, 0xFF, 0xFF};
  bool orientation_set = false;
  Orientation orientation = Orientation::kHorizontal;
  double corner_radius = -1;
  double padding = -1;
  double border_width = -1;
  Rgba border_color{0, 0, 0, 0};
  double min_width = 0;
  // ⚠ **不是硬上界**(§8.6.7.2 第二節)。見 ComputeLayout 的 9a/9b。
  double max_width = 640;
  Overflow overflow = Overflow::kShrink;  // §8.6.7.1
  // §8.6.7.1:預設 = candidates.item.spacing,由 ResolveDefaults 補。
  // -1 = 未指定。
  double column_gap = -1;
  double row_gap = -1;
  Placement placement = Placement::kAuto;
  double offset_x = 0;
  double offset_y = 6;
  bool follow_caret = true;
  double opacity = 1.0;
  bool shadow_show = true;
  double shadow_radius = 18;
  double shadow_offset_x = 0;
  double shadow_offset_y = 4;
  Rgba shadow_color{0, 0, 0, 0x40};
};

struct CandidateStyle {
  Metrics metrics;
  Orientation orientation = Orientation::kHorizontal;  // §8.6
  LabelStyle label;
  TextStyle text;
  CommentStyle comment;
  ItemStyle item;
  WindowStyle window;

  // 把所有 "預設 = metrics.X" 的欄位補實。載入主題之後、排版之前呼叫一次。
  // 分成獨立的一步,是為了讓「有沒有正確繼承 metrics」本身可以被測 ——
  // 規範 §10 的一致性檢核清單就有這一條。
  void ResolveDefaults();

  double ItemPaddingH() const { return item.padding_h; }
  Orientation EffectiveOrientation() const {
    return window.orientation_set ? window.orientation : orientation;
  }
};

// 文字量測。由外面注入 —— 服務進程用 GDI,測試用確定性的假量測器。
struct Extent {
  double width = 0;
  double height = 0;
};
using MeasureFn = std::function<Extent(const std::string& utf8, double size)>;

struct ItemLayout {
  double x = 0, y = 0, w = 0, h = 0;   // 相對候選窗左上角
  double label_x = 0, label_y = 0;
  double text_x = 0, text_y = 0;
  double comment_x = 0, comment_y = 0;
  bool has_label = false;
  bool has_comment = false;
  std::string label_display;  // 已套用 §8.6.1 的 format
  // §8.6.7.1 第 8 步:量測寬度大於它拿到的格寬 → 渲染端**必須**在尾端
  // 補 `…`(U+2026)。⚠ 只有 `shrink` 會產生它;`clip` 底下被窗蓋住的項
  // **不**算截斷(它沒有被截短,只是被遮住),所以那時恆為 false。
  bool truncated = false;
};

struct WindowLayout {
  double width = 0;
  double height = 0;
  // ⚠ **恆等於輸入的候選數**(§8.6.7.2 第一節,規範性)。
  //   少一項就代表使用者按那個數字鍵會選到看不見的字。W15 在守這件事。
  std::vector<ItemLayout> items;
  // 需要補 `…` 的項數。診斷用;不是「少畫了幾個」。
  int32_t truncated_count = 0;
};

// ── 滾輪要翻幾頁(G71)──────────────────────────────────────────
//
// ⚠ 為什麼這需要一個純函式,而不是 `delta > 0 ? 上一頁 : 下一頁`:
//   滑鼠的滾輪一格固定送 ±WHEEL_DELTA(120),那樣寫在滑鼠上完全正常。
//   **精密觸控板不是** —— 它一次輕撥會送出一連串很小的增量,於是
//   使用者輕輕一撥就翻掉十幾頁,而且再也找不回原本看的那一頁。
//   累積器由呼叫端持有(候選窗一個成員),本函式只負責「加進去、
//   取出整數頁、餘數留著」。
//
// 回傳**有號的頁數**:負 = 往前(捲輪往上),正 = 往後(捲輪往下)。
// 方向與閱讀方向一致,四端要一樣。
//
// ⚠ 換方向時餘數要丟掉。留著的話「往下撥一點點、再往上撥一點點」會在
//   中間留下一個永遠用不完的殘值,而使用者感覺到的是「有時候一撥就跳、
//   有時候撥半天不動」—— 那種手感問題沒有人回報得清楚。
int32_t WheelPageSteps(int32_t* accumulator, int32_t delta);

WindowLayout ComputeLayout(const std::vector<Candidate>& items,
                           const CandidateStyle& st, const MeasureFn& measure);

struct Rect {
  double left = 0, top = 0, right = 0, bottom = 0;
  double width() const { return right - left; }
  double height() const { return bottom - top; }
};

// §8.6.7 的 placement / offset。caret 是宿主給的插入點矩形(螢幕座標),
// work_area 是該螢幕的可用區域。
//
// auto 的語意是「空間不足時翻面」。翻面之後仍然放不下時一律夾進 work_area:
// 候選窗跑到螢幕外面是使用者完全無法自救的狀態。
Rect PlaceWindow(const Rect& caret, double w, double h, const Rect& work_area,
                 const CandidateStyle& st);

}  // namespace rimewin

#endif  // RIMEWIN_CAND_LAYOUT_H_
