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
// ── 2026-08-10:整頁的版面搬進來,連同控制項 id ──────────────────
//
// 在這之前,**每一頁的實際版面住在 service/settings_window.cc 的
// LayoutUi() 裡**,而本檔的 ClickableTargetsDip 手工造了一份
// 「輸入方案頁」的假骨架當代表,還把 window_h_dip 整個丟掉
// (`(void)window_h_dip;`)。後果不是「測試弱了一點」:
//
//   · 外觀頁的深淺三態排在 y=574/604/634,狀態列開關在 754,
//     說明在 810(頁底 870);底部固定列在 H-48,而預設 client 高 560。
//     也就是**那三顆單選鈕在畫面上根本不存在**,而視窗沒有捲動,
//     150% 的 1080p 筆電拉到最大也碰不到。
//   · W18 量的是那份假骨架 —— 一頁沒壞的頁 —— 所以它一直是綠的。
//
// 「守門者自己在該紅的時候安靜地不跑」只有一種結構性的修法:
// **把被守的東西搬到守得到的地方**。現在每一頁的每一個矩形都由
// LayoutSettingsPageDip() 產生,settings_window.cc 只負責把它們
// 貼到 HWND 上;它自己不再有 Stack,也算不出任何一個 y。
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

// ── 設定視窗的頁 ────────────────────────────────────────────────
//
// ⚠ 順序 = 側欄由上而下的順序。
enum SettingsPage : int {
  kPageSchemas = 0,
  kPageAppearance,
  kPageText,
  kPageAdvanced,
  kPageCount,
};

// ── 設定視窗的控制項 id ─────────────────────────────────────────
//
// ⚠ 它們住在**這裡**、不在 settings_window.cc,理由只有一個:守門。
//   一顆控制項的位置只要有一格是在 service/ 那一側算的,單元測試就
//   量不到它;而量不到的那一格,正好是最新、最沒被看過的那一個。
//   本檔是唯一知道「哪一頁上有哪些控制項、各在哪裡」的地方,
//   check_ui_spec.sh 的 W24 兩個方向都比對過 settings_window.cc
//   的建立表與這裡的版面(多一顆、少一顆都紅)。
enum SettingsControlId : int {
  IDC_SIDEBAR = 100,
  IDC_STATUS,
  IDC_CLOSE,

  // 輸入方案
  IDC_SCHEMAS_TITLE = 200,
  IDC_SCHEMAS_SUB,
  IDC_SCHEMAS_LIST_HEAD,
  IDC_SCHEMAS_LIST_BLURB,
  IDC_SCHEMA_LIST,
  IDC_UP,
  IDC_DOWN,
  IDC_APPLY_ORDER,
  IDC_SCHEMAS_DEFAULT_LINE,
  IDC_FOLLOW_MODE,
  IDC_FOLLOW_BLURB,
  IDC_SCHEMAS_EMPTY,

  // 外觀
  IDC_APPEAR_TITLE = 300,
  IDC_APPEAR_SUB,
  IDC_COUNT_HEAD,
  IDC_COUNT_BLURB,
  IDC_COUNT_0,
  IDC_COUNT_1,
  IDC_COUNT_2,
  IDC_COUNT_3,
  IDC_COUNT_4,
  IDC_SCALE_HEAD,
  IDC_SCALE_BLURB,
  IDC_SCALE_0,
  IDC_SCALE_1,
  IDC_SCALE_2,
  IDC_SCALE_3,
  IDC_SCALE_4,
  IDC_THEME_HEAD,
  IDC_THEME_BLURB,
  IDC_THEME_0,
  IDC_THEME_1,
  IDC_THEME_2,
  IDC_BAR_HEAD,
  IDC_BAR_BLURB,
  IDC_BAR_SHOW,
  IDC_APPEAR_NOTE,

  // 文字
  IDC_TEXT_TITLE = 400,
  IDC_TEXT_SUB,
  IDC_VARIANT_HEAD,
  IDC_VARIANT_BLURB,
  IDC_VARIANT_0,
  IDC_VARIANT_1,
  IDC_VARIANT_2,
  IDC_PUNCT_HEAD,
  IDC_PUNCT_BLURB,
  IDC_PUNCT_0,
  IDC_PUNCT_1,
  IDC_PUNCT_2,

  // 進階
  IDC_ADV_TITLE = 500,
  IDC_ADV_SUB,
  IDC_REDEPLOY_HEAD,
  IDC_REDEPLOY_BLURB,
  IDC_REDEPLOY,
  IDC_FILES_HEAD,
  IDC_FILES_BLURB,
  IDC_OPEN_USER_DIR,
  IDC_OPEN_SETTINGS_FILE,
  IDC_LANG_HEAD,
  IDC_LANG_BLURB,
  IDC_LANG_0,
  IDC_LANG_1,
  IDC_LANG_2,
  IDC_LANG_3,
  IDC_DIAG_HEAD,
  IDC_DIAG_NOTE,
  IDC_DIAG,
  IDC_DIAG_COPY,
  IDC_RESET_HEAD,
  IDC_RESET_BLURB,
  IDC_RESET,
};

// 底部固定列(狀態文字 + 關閉鈕)佔掉的高度,以及它上面那條 hairline。
// 內容區的可視高度 = 視窗高度 − 這個值。
constexpr int kBottomBarH = metric::kMinTarget + space::s7;          // 48
constexpr int kBottomStripH = kBottomBarH + space::s3;               // 54
// 內容欄從視窗頂端往下 s8 開始。
constexpr int kContentTopDip = space::s8;
// 內容底端留白:最後一個控制項與那條 hairline 之間。
constexpr int kContentPadBottomDip = space::s7;

// 一顆被擺好的控制項。⚠ y 是**內容座標**(捲動量 0 時等於視窗座標),
// 呼叫端自己減掉捲動量。
struct PlacedControl {
  int id = 0;
  RectI rect;          // 空矩形 = 這一頁上不出現(呼叫端 ShowWindow(SW_HIDE))
  bool clickable = false;
  const char* what = "";  // 診斷用,永遠英文(§4.11)
};

struct PageLayout {
  std::vector<PlacedControl> items;
  // 內容總高(含底部留白)。**捲動範圍唯一的來源** ——
  // 所以任何一顆沒有推進堆疊的控制項都會落在捲動範圍以外。
  int content_h_dip = 0;
};

// 一頁的完整版面。schema_list_empty 是「輸入方案」頁唯一的執行期分支
// (清單是空的時候換成一句說明)。
PageLayout LayoutSettingsPageDip(int page, int window_w_dip,
                                 bool schema_list_empty);

// 內容區的可視高度。⚠ 視窗矮於底部固定列時回 0,不回負數。
int ContentViewportHeightDip(int window_h_dip);

// 捲動上限(DIP)。內容放得下時是 0。
// ⚠ 這一支**必須**吃 window_h_dip —— 舊版的 ClickableTargetsDip 把它
//   `(void)` 掉,於是「排到視窗底部以外」對測試而言不存在。
int ScrollMaxDip(int page, int window_w_dip, int window_h_dip,
                 bool schema_list_empty);

// ── W18 的取材面 ────────────────────────────────────────────────
//
// 「所有可點矩形 ≥ 28×28 DIP」與「每一個都碰得到」要驗得到,前提是
// 有人說得出**哪些是可點的、在哪裡**。版面自己回報,而不是靠測試去猜 ——
// 猜的那一份會漏掉新加的控制項。
struct HitTarget {
  const char* what;  // 診斷用,永遠英文(§4.11)
  RectI rect;
  int id = 0;
  // true = 在會捲動的內容區裡,rect.y 是內容座標;
  // false = 側欄或底部固定列,rect.y 是視窗座標。
  bool scrolls = false;
};

// **某一頁**上所有可點的矩形(含側欄與底部固定列)。
// ⚠ 第三個參數是**頁**,不是頁數 —— 舊版收的是 page_count,而那正是
//   它從來沒有真的走過任何一頁版面的原因。
std::vector<HitTarget> ClickableTargetsDip(int window_w_dip, int window_h_dip,
                                           int page, bool schema_list_empty);

}  // namespace rimewin

#endif  // RIMEWIN_UI_LAYOUT_H_
