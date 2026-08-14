// windows/common/statusbar_layout.h — 懸浮狀態列的幾何(§12.14.7)
//
// ── 為什麼這一支從 status_bar.cc 搬出來 ────────────────────────────
//
// 規格 §12.15 的 W34 寫得很直白:「格的矩形計算要從 `status_bar.cc` 搬到
// `windows/common/` —— 現在它在 `Relayout()` 裡,Ubuntu 上編不起來,
// 所以**『26 DIP 高』這件事沒有任何自動化看得到**。」
//
// 那不是假設。§12.14.0 第 5 條記著的缺陷就是這個形狀:`kBarH = 28`,
// 而 `Relayout()` 把每一格上下各扣掉 `kBarBorder`(1)→ 每一格 26 DIP,
// 低於 §3.6 的 28 最小點擊目標。W18 量的是設定視窗,量不到這一條;
// 而它在畫面上看起來只是「那一橫有點扁」。
//
// ── ⚠ 字寬是**輸入**,不是這裡量的 ──────────────────────────────
//
// 每一格的寬度來自 `GetTextExtentPoint32W`,而純函式拿不到 HDC。
// 所以呼叫端量好了交進來,測試餵合成的寬度。
// **不要**為了讓這一支「自己算得出來」而在這裡估字寬 —— 估出來的那一份
// 與畫出來的那一份會分岔,而分岔的樣子是「點擊區與看到的格子錯開」。
//
#ifndef RIMEWIN_STATUSBAR_LAYOUT_H_
#define RIMEWIN_STATUSBAR_LAYOUT_H_

#include <vector>

#include "ui_layout.h"

namespace rimewin {

namespace barmetric {
// §12.14.7 的幾何(DIP)。⚠ 整條 32 是為了讓格子滿足 §3.6 的 28:
//   28 + 上下各 s1(2) = 32。
constexpr int kBarH = metric::kButtonH;       // 32
constexpr int kCellH = metric::kMinTarget;    // 28
constexpr int kCellMinW = metric::kMinTarget; // 28
constexpr int kCellPadH = space::s4;          // 10:格內左右內距
constexpr int kCellGap = space::s3;           // 6:格與格之間
constexpr int kInsetV = space::s1;            // 2:外框到格子(上下)
constexpr int kInsetH = space::s2;            // 4:同上(左右)
constexpr int kBarRadius = radius::kWindow;   // 8
constexpr int kCellRadius = radius::kControl; // 4
constexpr int kBorder = metric::kHairline;    // 1
// 第 3 格(方案名)的**內容**寬上限 → 格寬上限 120 + 2*10 = 140。
constexpr int kSchemaContentMaxW = 120;
// 整條的寬上限。
constexpr int kBarMaxW = 320;
// 第 3 格與第 4 格之間那條分隔線,左右各 s3。
constexpr int kSepGap = space::s3;
}  // namespace barmetric

// 一格的輸入:量好的字寬(DIP)。0 或負 = 這一格**整格略過**
// (§8.12:方案名為空時那一格不佔位置,不得畫成一塊看不出用途的空白)。
struct BarCellIn {
  int text_w_dip = 0;
  bool skip = false;
};

struct BarCell {
  RectI rect;      // 空矩形 = 略過
  bool skipped = true;
};

struct BarLayout {
  std::vector<BarCell> cells;
  int total_w_dip = 0;
  // 第 3 格與第 4 格之間那條 1 DIP 分隔線的 x。<0 = 這一次沒有畫
  // (格數不足、或其中一格被略過)。
  //
  // ⚠ 它不是裝飾:1–3 格**改狀態**,第 4 格**開一個視窗**。兩種不同的
  //   後果之間要有一個看得見的界線,否則使用者會以為第四格也是一個開關。
  int separator_x_dip = -1;
  // 第 3 格被截尾了嗎(呼叫端據此加 DT_END_ELLIPSIS)。
  // ⚠ 截尾**不得是死路**:第 3 格點下去開的自繪清單裡是完整的名字。
  bool schema_truncated = false;
};

// 四格模式。cells 的長度必須是 4(1=input_mode / 2=variant /
// 3=schema_name / 4=設定);少於 4 的話多出來的當略過。
BarLayout LayoutStatusBarCellsDip(const std::vector<BarCellIn>& in);

// 「服務沒起來」那一種外觀:四格全不畫,整條一句話、整條可點。
// text_w_dip 是那一句話量出來的寬。
BarLayout LayoutStatusBarSentenceDip(int text_w_dip);

}  // namespace rimewin

#endif  // RIMEWIN_STATUSBAR_LAYOUT_H_
