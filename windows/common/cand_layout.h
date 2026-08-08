// windows/common/cand_layout.h — 候選窗的排版計算(純邏輯)
//
// ⚠ **規範所有權**:候選窗那部分的規範由 macOS 端(第一個桌面端)擴充,
//   Windows 端繼承。本檔只實作 docs/theme-format.md **已經寫下來**的欄位,
//   一個也不多。發現不足一律寫進 docs/coordination.md §5 回報,不自己加。
//   規範一分岔,「一套配置四端共用」這個主張就沒了,而且會安靜地分岔。
//
// 已知不足(已回報,見 coordination.md §5):
//   · §8.6.7 的 max_width 說「超出則換行／截斷,**由實作決定**」——
//     那正好是最需要一致的地方,兩端會做出兩種結果。本檔目前是截斷。
//   · 多欄／表格排版(§11 自承未定義)。
//   · 標籤與候選文字之間、候選文字與註解之間的間距沒有欄位。
//     本檔暫用 metrics.spacing。
//   · 狀態列(中/英、簡/繁)的外觀完全沒有規範。本輪不畫。
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
  double max_width = 640;
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
};

struct WindowLayout {
  double width = 0;
  double height = 0;
  std::vector<ItemLayout> items;
  // 因為 max_width 而沒放進來的候選數。> 0 時代表使用者看不到全部候選 ——
  // 服務進程要記進日誌,不然「候選少了幾個」查不出原因。
  int32_t dropped = 0;
};

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
