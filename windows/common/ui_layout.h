// windows/common/ui_layout.h — 設定視窗的版面(§12.4,純函式)
//
// ── 為什麼是純函式 ──────────────────────────────────────────────
//
// 純 Win32 沒有宣告式版面,每一個矩形都要自己算(§12.1 的約束 1)。
// 而「算錯」的樣子是使用者一眼看得到、自動化完全看不到的東西:
// 側欄壓在內容上、按鈕跑出視窗、高 DPI 下右邊那一欄擠在一起。
//
// 所以版面**不碰 HWND**:輸入是「視窗大小 + DPI」,輸出是一堆矩形。
// 這樣 W18(所有可點矩形 ≥ 28×28 DIP)與 W19(§12.4.2 的三組驗算數字)
// 就是真的單元測試,在 Ubuntu 上跑得到 —— 不需要一台 Windows。
//
// ⚠ **本檔不得出現 `is_dark` / `dark_mode` 分支**(§2-F4 / W11)。
//   深淺的差別全部收在 ui_palette.h 的色票表裡。版面拿不到那個資訊,
//   所以「深色模式只換色票」不是紀律,是結構上唯一做得到的事。
//
#ifndef RIMEWIN_UI_LAYOUT_H_
#define RIMEWIN_UI_LAYOUT_H_

#include <cstdint>
#include <vector>

namespace rimewin {

// ── §3.1 間距階梯(桌面欄)──────────────────────────────────────
// ⚠ W3 掃的就是這一組。**間距、圓角、元件尺寸是三個不同的集合**,
//   混在一起掃會把 1 DIP 的分隔線判成違規,而一條永遠紅的檢查會被關掉。
namespace space {
constexpr int s1 = 2;
constexpr int s2 = 4;
constexpr int s3 = 6;
constexpr int s4 = 10;
constexpr int s5 = 12;
constexpr int s6 = 16;
constexpr int s7 = 20;
constexpr int s8 = 32;
}  // namespace space

// ── §3.3 圓角(桌面欄)。按鈕不在這裡 —— 它跟平台走。
namespace radius {
constexpr int kLarge = 10;
constexpr int kMedium = 7;
constexpr int kMediumInner = 6;
constexpr int kSmall = 5;
}  // namespace radius

// ── §3.6 / §12.4.2 元件尺寸 ─────────────────────────────────────
namespace metric {
constexpr int kHairline = 1;
constexpr int kMinTarget = 28;   // 桌面點擊目標最小 28×28
constexpr int kSidebarItemH = 36;
constexpr int kSidebarStatusH = 64;
constexpr int kSidebarW = 200;   // §5.1「不可拖曳」→ 固定,不是 splitter
}  // namespace metric

// ── §3.2 字級階梯(桌面欄)。W4 掃這一組。
namespace text_size {
constexpr int t1 = 22;  // 頁標題,一頁只有一個
constexpr int t2 = 15;  // 區段標題、對話框標題
constexpr int t3 = 13;  // 列標題
constexpr int t4 = 12;  // 現值、按鈕文字
constexpr int t5 = 11;  // 白話說明、小標題
constexpr int t6 = 11;  // 診斷(等寬)
}  // namespace text_size

// 視窗尺寸(DIP)。§5.1。
constexpr int kWindowDefaultW = 780;
constexpr int kWindowDefaultH = 560;
constexpr int kWindowMinW = 660;
constexpr int kWindowMinH = 460;
constexpr int kContentMinW = 440;
constexpr int kContentMaxW = 640;

struct RectI {
  int x = 0, y = 0, w = 0, h = 0;
  int right() const { return x + w; }
  int bottom() const { return y + h; }
  bool empty() const { return w <= 0 || h <= 0; }
};

// ── 內容欄 ──────────────────────────────────────────────────────
//
//   avail     = W - 200
//   content_w = clamp(avail - 2*s7, 440, 640)
//   content_x = 200 + (avail - content_w) / 2      // 置中
//
// ⚠ 最小尺寸(660)下左右內距被壓到 10,這是刻意的:
//   內容欄的 440 下界**優先於**邊距。W19 的三組數字驗的就是這件事。
// 單位是 DIP;呼叫端自己用 Dip() 換成像素。
int ContentWidthDip(int window_w_dip);
int ContentXDip(int window_w_dip);

// ── 側欄 ────────────────────────────────────────────────────────
//
// 側欄上**只列已經實作的頁**(§2-D1:做不到就整頁拿掉)。
// 頁數由呼叫端給 —— 本檔不知道有哪幾頁,那是 ui_strings 那一側的事。
RectI SidebarItemDip(int index);
// 側欄底部的狀態區(兩行 t5:「可以打字」/「離線」)。
RectI SidebarStatusDip(int window_h_dip);

// ── 內容區的直向堆疊 ────────────────────────────────────────────
//
// 每一頁的骨架相同:t1 頁標題 → t5 副標 → hairline → 依序的區塊。
// 區塊之間 s7,區塊內同組控制項之間 s3。
// ⚠ **危險操作一律是該頁最後一個區塊**(§4.9 / §2-C2),
//   上面隔一條 hairline + s7 —— 那條 hairline 由 PushDivider 產生。
class Stack {
 public:
  Stack(int x_dip, int y_dip, int w_dip)
      : x_(x_dip), y_(y_dip), w_(w_dip), top_(y_dip) {}

  // 配一塊高 h 的矩形,並往下推進 gap。
  RectI Push(int h_dip, int gap_dip);
  // 一條 hairline,上下各留 s7。危險區塊前面用它。
  RectI PushDivider();
  void Skip(int dip) { y_ += dip; }

  int y() const { return y_; }
  int width() const { return w_; }
  int height_used() const { return y_ - top_; }

 private:
  int x_, y_, w_, top_;
};

// ── W18 的取材面 ────────────────────────────────────────────────
//
// 「所有可點矩形 ≥ 28×28 DIP」要驗得到,前提是有人說得出**哪些是可點的**。
// 版面自己回報,而不是靠測試去猜 —— 猜的那一份會漏掉新加的控制項,
// 而漏掉的正好是最新、最沒被看過的那一個。
struct HitTarget {
  const char* what;  // 診斷用,永遠英文(§4.11)
  RectI rect;
};

// 一頁上所有可點的矩形。page_count 是側欄上的頁數。
std::vector<HitTarget> ClickableTargetsDip(int window_w_dip, int window_h_dip,
                                           int page_count);

}  // namespace rimewin

#endif  // RIMEWIN_UI_LAYOUT_H_
