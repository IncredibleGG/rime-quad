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

// ⚠ 版面碼**不用**字串,但「哪一頁叫什麼」屬於「哪一頁上有什麼」的一部分,
//   而本檔是那件事唯一的來源(見 SettingsPageName)。
#include "ui_strings.h"

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

// ── §12.14.4 圓角:Windows 端**只有兩個值** ─────────────────────
//
// ⚠ §3.3 的桌面欄(10/7/6/5)在 Windows 端**不使用**。理由是 §3.3 自己
//   那一條:「按鈕的圓角跟平台走,不跟我們走」。在 Win11 上,**所有**
//   控制項的圓角都是平台給的,不只按鈕 —— 一個 7 DIP 圓角的清單列擺在
//   4 DIP 圓角的系統核取方塊旁邊,看起來是兩套東西拼起來的。
//
//   膠囊(徽章)= 高度 ÷ 2,不是自由參數,所以不在這裡。
//   焦點環 = 元件圓角 + 2,同理。
namespace radius {
constexpr int kControl = 4;  // 卡片、側欄項、清單列、按鈕、狀態列每一格
constexpr int kWindow = 8;   // 懸浮狀態列、方案彈出清單、確認對話框、候選窗
}  // namespace radius

// ── §3.6 / §12.4.2 元件尺寸 ─────────────────────────────────────
namespace metric {
constexpr int kHairline = 1;
constexpr int kFocusRingW = 2;   // §12.14.2:焦點環外圈
constexpr int kIndicatorW = 3;   // §12.14.6.1:側欄選中項左緣指示條的寬
constexpr int kIndicatorH = 16;  // 同上的高
constexpr int kMinTarget = 28;   // 桌面點擊目標最小 28×28;狀態列每一格的高
constexpr int kButtonH = 32;     // §12.14.6.2:按鈕高;懸浮狀態列整條的高
// ⚠ 舊名 kSidebarItemH。§12.14.5:36 有**兩個**意思(側欄項高、清單列高),
//   而那是刻意的 —— 兩者是同一種東西(一列可點的、放一行 t3 的東西),
//   值一樣才對。名字叫 kSidebarItemH 的時候,方案清單用它看起來像
//   「借了側欄的常數」,下一個人會想拆成兩個,然後兩個會漂開。
constexpr int kRowH = 36;
constexpr int kSidebarStatusH = 64;
// §12.14.6.2/.3:按鈕的**下界**寬,不是它的寬。
//
// ⚠ 這個常數是這一輪新加的,而它的存在本身就是那個缺陷的名字:
//   在它之前,一列按鈕的寬度是「內容欄減掉間距再除以顆數」——
//   也就是**每一顆都跟著視窗變寬**。進階頁那張「重新整理字詞」的卡
//   因此是 540×52 而按鈕只佔左邊 180,右邊 360 DIP 整片空白;
//   輸入方案頁的上移/下移/套用這個順序各被拉成 165 寬,
//   讀起來是一列對話框按鈕,不是一排工具列按鈕。
//   現在寬度由**字**決定(ButtonWidthDip),而這一格是它的地板:
//   「確定」「複製」這種兩個字的按鈕不可以縮成一顆藥丸。
constexpr int kButtonMinW = 80;
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

// 側欄底部狀態區的一行有多高。⚠ 它必須 >= TextLineBoxDip(text_size::t5),
// 而且兩行加上留白要放得進 metric::kSidebarStatusH —— 兩條都由 W26 驗。
constexpr int kSidebarStatusLineH = 20;

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
//
// ── ⚠ 這一支回的是**那一列真正在哪裡**,而不是我們希望它在哪裡 ──
//
// 側欄是一個真的 SysListView32(LVS_REPORT + 自繪),而 comctl32 排列的
// 方式是固定的:第一列從清單的 client 頂端開始,之後每一列往下**列高**,
// **列與列之間沒有間隔**。列高由 SetRowListRowHeight() 釘成
// metric::kRowH。
//
// 舊版這裡多加了一個 space::s2 的列距:
//
//     r.y = space::s5 + index * (metric::kRowH + space::s2);
//
// 那個 +4 在畫面上不存在 —— 沒有任何東西畫得出它,comctl32 也不認得它。
// 它只存在於**我們以為列在哪裡**的那一份裡,而命中判定用的正是那一份:
// 到第 5 項(進階)累積差 4 × 4 = **16 DIP**,也就是 #75 使用者回報的
// 「點得到的地方跟畫出來的地方差了半列」。
//
// 現在**只有這一份**。畫在哪裡、點在哪裡,兩件事從同一個算式來,
// 而 test_ui_layout.cc 的 sidebar_hit_and_draw_are_the_same_rect
// 把它與 SidebarListDip() + 列高 算出來的位置逐項比對。
//
// 回的是**整列**(x = 0,寬 = 側欄全寬)—— 那是 comctl32 命中的範圍,
// 而 LVS_EX_FULLROWSELECT 讓它也是使用者按得到的範圍。
RectI SidebarItemDip(int index);

// 那一列上**畫出來的那塊底**(§12.4.2:側欄左右內距 12)。
//
// ⚠ 它一定落在 SidebarItemDip(index) 裡面 —— 這是刻意的,而且有測試釘著:
//   底比列窄是視覺,不是命中範圍。使用者按在底左邊那 12 DIP 上仍然要換頁,
//   不然那一條會變成一道看不見的死區。
RectI SidebarItemFillDip(int index);
// 側欄選中項左緣那條指示條(§12.14.6.1)。**只有選中的那一項有。**
//
// ⚠ 它不是裝飾:§3.4 第 2 條要求「不得只用顏色傳達資訊」,而「現在是
//   哪一頁」的第一個訊號是底色。指示條是**形狀**,色覺障礙下也在。
//
// ⚠ 它與底色、圓角**必須從同一個矩形算**(SidebarItemFillDip)。
//   分開算的樣子是「指示條與底色的圓角對不齊」—— 差一兩個 DIP,
//   看起來只是「做得不精緻」,不像 bug,所以永遠不會有人回報。
RectI SidebarIndicatorDip(int index);

// 側欄底部的狀態區(兩行 t5:「可以打字」/「離線」)。
RectI SidebarStatusDip(int window_h_dip);

// ── ⚠ 側欄清單與底部狀態區**不可以重疊** ────────────────────────
//
// 側欄那條清單是**子視窗**,而設定視窗開著 WS_CLIPCHILDREN ——
// 也就是說,父視窗畫在子視窗底下那一塊的東西**整個被裁掉**,
// 不是被蓋住而是根本沒畫。底部那兩行狀態文字是父視窗在 WM_PAINT 裡畫的。
//
// 實機回報:側欄底部那兩行「可以打字」「離線」是**斷的**,使用者把
// 第一行讀成「⼝以打字」—— 那正是「可」的上緣被裁掉之後剩下的形狀。
// 算式很簡單:清單擺在 y = s5(12),高度給了 H - kSidebarStatusH,
// 所以它的下緣在 H - 52,而狀態區從 H - 64 開始 —— **重疊 12 DIP**,
// 剛好吃掉第一行的上半。
//
// 這件事以前沒有任何自動化看得到,因為位置是在 settings_window.cc 裡算的
// (那個檔案在 Ubuntu 上編不起來)。現在它在這裡,而 W26 就是那個算式。
RectI SidebarListDip(int window_h_dip);

// 狀態區裡的第 line 行(0 = 現在能不能打字,1 = 連線狀態)。
RectI SidebarStatusLineDip(int window_h_dip, int line);

// 一行 size_dip 的文字**至少**要多高才不會被切到。
//
// 漢字的行高(ascent + descent)大約是字級的 4/3,不是字級本身 ——
// 給一個「字級 + 4」的格子看起來剛好夠,實際上是零餘裕:換一套字體、
// 換一個 DPI 的捨入方向,字就被削掉一條。+2 是那條餘裕。
int TextLineBoxDip(int size_dip);

// ── 一段說明文字在寬 w_dip 的欄裡大約要幾行 ─────────────────────
//
// ⚠ **這是估算,不是量測。** 真正的斷行是 GDI 的 DrawTextW(DT_WORDBREAK)
//   做的,而它認得、這裡一個都不認得的東西有:字型真正的字寬表、
//   CJK 逐字斷行 vs 拉丁逐詞斷行、標點禁則、字距調整。
//
//   所以規則是**寧可高一點**:
//     · 全形字算一個字級寬,其餘算 0.6 個字級(對一般的比例字體偏寬)。
//     · **逐詞斷行是算出來的,不是折扣**:空白/連字號/全形字之後可以斷,
//       中間黏在一起的字母是一個不可斷開的詞,而一個詞放不下就整個
//       推到下一行 —— 那正是 DT_WORDBREAK 在做的事。
//     · 在那之上再留 1/16 的欄寬,給字寬表本身的誤差(0.6 是拉丁字母的
//       平均寬度,不是上界;大寫 W、M 接近 0.9)。
//   少算一行的後果是**文字被切掉**(使用者看到半句話,而且沒有任何
//   錯誤);多算一行的後果是下面多一點空白。兩者不對稱,所以往高的抓。
//
// ⚠ **舊版是 6/7 的折扣,而那個折扣不夠。** 它拿 1/7 的欄寬當作「一個
//   詞被推到下一行留下的空白」的替代品,但 440 DIP 的內容欄上 1/7 只有
//   62 DIP —— t5 底下 9.5 個拉丁字母。比它長的詞一出現,估算就低於實際,
//   而低估的方向是切字。使用者看得到的英文文案裡就有 `administrator`
//   (13 個字母)與 `mid-sentence`(12 個)。
//
// ⚠ **「欄越窄行數只會多」是這一支的硬性質,不是描述。** 舊版有一條
//   `if (usable <= size_dip) return 1;`,於是 size=11 時欄寬 <= 13 的每
//   一格都回 1 行,而 14 回 200 行 —— 欄從 14 縮到 13,行數從 200 掉到 1。
//   守它的是 ui_layout_text_lines_is_monotone_at_every_column_width,
//   而那條掃 8..800 的**每一格**:舊的那條只探五個寬度,一格都沒踩到。
//
// ⚠ 為什麼不是「把盒子開大一點就好」:那正是 #76 的成因。每一段各自
//   寫死行數的時候,每一段都被寫得寬鬆一點,而六段加起來就是 195 DIP
//   ——「連網」頁因此長到 1138 DIP,而可視高度只有 506,清除紀錄那顆
//   破壞性按鈕落在摺線下 580 DIP。單獨看每一段都很合理,合起來是缺陷。
//
// 回傳至少 1。text 為空 = 1 行(那一格仍然佔位置)。
int EstimateTextLinesDip(const wchar_t* text, int size_dip, int w_dip);

// 上面那一支 × 行盒高度。說明段的高度一律走這一支。
//
// ⚠ size_dip <= 0 是呼叫端的錯,但版面不可以因此壞掉:以前這裡直接
//   乘 TextLineBoxDip(size_dip),於是 size=-11 回 **-12 DIP**(高度是
//   負的,Stack 往回縮,下一段疊上來)、size=0 回 2 DIP(那一段從畫面上
//   消失,而且沒有任何錯誤)。現在不合法的字級一律當成 t5 ——
//   **回傳保證 >= TextLineBoxDip(text_size::t5)**。
int EstimateTextBoxHeightDip(const wchar_t* text, int size_dip, int w_dip);

// ── 一段文字畫出來大約多寬(DIP)────────────────────────────────
//
// ⚠ **估算,不是量測** —— 同 EstimateTextLinesDip,而且刻意用**同一個**
//   字寬模型(全形算一個字級,其餘算 0.6 個)。兩支各用一套模型的話,
//   「這一行放得下」與「這顆按鈕要多寬」會給出互相矛盾的答案。
//
// ⚠ 錯的方向也一樣:往**寬**的一邊。少算的後果是按鈕上的字被系統
//   截尾(使用者看到「重新整理字…」);多算的後果是按鈕寬幾個 DIP。
//   兩者不對稱,所以再留 1/16 的餘裕(0.6 是拉丁字母的平均寬,
//   不是上界)。
// ⚠ 換行字元在這裡**不換行** —— 這一支回的是「排成一行」的寬度,
//   按鈕上的字本來就只有一行。
int EstimateTextWidthDip(const wchar_t* text, int size_dip);

// ── 一顆按鈕要多寬(§12.14.6.2/.3)──────────────────────────────
//
// = t4 的字寬 + 左右各 `s5`,下界 `metric::kButtonMinW`。
//
// ⚠ **這一支存在的理由是版面在用「填滿寬度」思考。** 舊版一列按鈕的
//   寬是 `(inner_w - 2*s3) / 3` —— 三顆各 165 DIP,橫跨整個內容欄
//   平均分配。那不是工具列,是對話框的按鈕列;而且視窗一拉寬,
//   「上移」這兩個字底下的按鈕就跟著長到 300 DIP。
//
// ⚠ 寬度從**顯示的那一句**算,所以它跟著介面語言變 —— 與說明段的
//   高度(EstimateTextBoxHeightDip)同一條路。
int ButtonWidthDip(UiString label);

// ── 這一顆按鈕上寫的是哪一句 ────────────────────────────────────
//
// ⚠ 這是「寬度從哪一句算」的唯一來源。字本身是
//   service/settings_window.cc 的 kControls 填的,兩邊必須是同一句 ——
//   不然按鈕會照 A 句的寬度畫、上面寫 B 句,而 B 句比較長的時候
//   使用者看到的是被截尾的字。check_ui_spec.sh 的 W35 逐顆比對這兩份。
// ⚠ 不是按鈕的 id 回 UiString::kUiStringCount。
UiString SettingsButtonLabel(int id);

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
  // ⚠ 「連網」排在「進階」前面。它不是進階功能 —— 這個輸入法的定位
  //   就是離線為預設,那顆開關與那份紀錄是使用者**會去看**的東西,
  //   不是藏在最後一頁的旋鈕。順序與 Android 的設定頁對得起來。
  kPageNetwork,
  kPageAdvanced,
  kPageCount,
};

// 側欄上第 page 頁的名字。
//
// ⚠ **這是唯一的對應表。** 它以前是 service/settings_window.cc 裡一個與
//   SettingsPage 平行的陣列,而「兩份平行的清單」在這個檔案裡已經害過一次
//   (控制項表的 page 欄位 vs LayoutUi 的 switch)。順序錯開一格的樣子是
//   「側欄寫著『連網』,點下去出現的是進階頁」—— 每一頁都有名字、
//   每一頁都有內容,沒有任何東西看起來不對。
//
// ⚠ 越界回 kNavSchemas,不崩潰:它會在 WM_PAINT 的路徑上被呼叫。
UiString SettingsPageName(int page);

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
  IDC_SHAPE_HEAD,
  IDC_SHAPE_BLURB,
  IDC_SHAPE_0,
  IDC_SHAPE_1,
  IDC_SHAPE_2,
  IDC_SHIFTTAP_HEAD,
  IDC_SHIFTTAP_BLURB,
  IDC_SHIFTTAP_SWITCH,

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
  // ⚠ W2:跑 `rime_ime_setup.exe doctor --report`。與「開始」功能表那個
  //   診斷捷徑**同一行命令**,不是另外寫一份。
  IDC_DIAG_RUN,
  IDC_RESET_HEAD,
  IDC_RESET_BLURB,
  IDC_RESET,

  // 連網
  //
  // ⚠ 這一頁上有兩塊會隨執行期狀態整個換掉(見 PageState):
  //   紀錄是空的時候,清單、欄名、計數與「清除紀錄」那一整個危險區塊
  //   全部不出現,換成一句「一次都沒有連過」。
  IDC_NET_TITLE = 600,
  IDC_NET_SUB,
  IDC_NET_SWITCH,
  IDC_NET_STATE,
  IDC_NET_DETAIL,
  // 更新。⚠ IDC_UPDATE_TRUST(「這支程式沒有數位簽章」那一句)是一個
  //   **永遠顯示**的靜態文字,不是錯誤狀態才出現的東西 —— 而且版面上
  //   它排在按鈕之前,使用者要在按下去**之前**就讀到它。
  IDC_UPDATE_HEAD,
  IDC_UPDATE_BLURB,
  IDC_UPDATE_TRUST,
  IDC_UPDATE_WHAT,
  IDC_UPDATE_STATUS,
  // ⚠ W1:「為什麼下面那幾顆是灰的」。排在按鈕**之前**,理由與
  //   IDC_UPDATE_TRUST 一樣:排在後面的話,他已經點過三次才讀到。
  //   只在連網開關關著的時候出現(見 PageState::net_switch_off)。
  IDC_UPDATE_GATE,
  IDC_UPDATE_CHECK,
  IDC_UPDATE_ACTION,
  IDC_UPDATE_PAGE,
  IDC_NETLOG_HEAD,
  IDC_NETLOG_BLURB,
  IDC_NETLOG_SUMMARY,
  IDC_NETLOG_COLS,
  IDC_NETLOG_LIST,
  IDC_NETLOG_EMPTY,
  IDC_NETLOG_PATH,
  IDC_NETLOG_CLEAR_HEAD,
  IDC_NETLOG_CLEAR_BLURB,
  IDC_NETLOG_CLEAR,
};

// 底部固定列(狀態文字 + 關閉鈕)佔掉的高度,以及它上面那條 hairline。
// 內容區的可視高度 = 視窗高度 − 這個值。
constexpr int kBottomBarH = metric::kMinTarget + space::s7;          // 48
constexpr int kBottomStripH = kBottomBarH + space::s3;               // 54
// 內容欄從視窗頂端往下 s8 開始。
constexpr int kContentTopDip = space::s8;
// 內容底端留白:最後一個控制項與那條 hairline 之間。
constexpr int kContentPadBottomDip = space::s7;

// ── 摺線那一段:「下面還有東西」要看起來像還有東西 ──────────────
//
// 摺線 = 內容區的下緣,也就是底部固定列上面那條 hairline。它是一條
// **視窗內部**的硬邊,而那正是問題:視窗邊緣把東西切斷沒有人覺得奇怪,
// 視窗**中間**一條線把一張有外框的圓角卡片切斷,看起來是畫錯了。
//
// 實機截圖(796×599,client 763×560,摺線在 y=506)上看得到的兩件事:
//   1. 卡片的左右兩條 1 DIP 外框直直撞上摺線就沒了,沒有下緣、沒有下圓角
//      —— 空白比較多的那一側(右邊)只剩一小截豎線,像沒畫完;
//   2. 剛好跨過摺線的控制項被**攔腰切開**:文字頁的「全形/半形」那個區段
//      標題在 y=497、高 22,可視高度 506 —— 它被裁成 9 DIP,
//      也就是從字的中間切過去。
//
// 這裡的兩個常數把那兩件事分開處理:
//   · kScrollFadeH:摺線上方這一段是**淡出區**。卡片在這一段裡由
//     surface 一階一階混到 background(GDI 沒有 alpha,所以是實色分帶,
//     顏色由 ScrollFadeMix() 算 —— 純函式,測得到)。淡出是「還有更多」
//     全世界都認得的訊號,而且它把那截沒畫完的外框一起吃掉。
//   · ContentClipLineDip():**控制項**裁在淡出區的上緣,不是摺線上。
//     所以淡出區裡永遠只有卡片的底色與外框,沒有字 —— 字上面蓋一層
//     淡出會變成「這一列是不是停用了」,那是另一個缺陷。
constexpr int kScrollFadeH = space::s6;

// 一顆被擺好的控制項。⚠ y 是**內容座標**(捲動量 0 時等於視窗座標),
// 呼叫端自己減掉捲動量。
struct PlacedControl {
  int id = 0;
  RectI rect;          // 空矩形 = 這一頁上不出現(呼叫端 ShowWindow(SW_HIDE))
  bool clickable = false;
  const char* what = "";  // 診斷用,永遠英文(§4.11)
  // ── ⚠ 這兩格是 W31' 的取材面(§12.15)────────────────────────
  //
  // 「這個矩形放得下它要放的字嗎」這件事,測試沒辦法從 rect 自己看出來
  // —— 它得知道那是幾級的字、幾行。以前那個知識只存在於
  // LayoutSettingsPageDip() 的區域變數裡,所以 §12.14.0 第 4 條那四處
  // (頁標題 28 < 31、區段標題 19 < 22)**沒有任何自動化看得到**。
  //
  // 0 = 這一顆不放文字(清單、EDIT、按鈕的框由控制項自己畫)。
  int text_size_dip = 0;
  int text_lines = 0;
  // 這一顆坐在卡片裡嗎。⚠ 它決定 WM_CTLCOLOR* 要回 surface 還是
  //   background —— 沒有這一格的話,卡片裡的每一顆 STATIC 都會帶著
  //   一塊 background 色的底,而卡片是 surface 色:畫面上是一張卡片
  //   上面浮著幾塊顏色不一樣的長方形。這比沒有卡片還難看。
  bool in_card = false;
};

// ── 卡片(§12.14.5,這一輪視覺上最大的一個改動)────────────────
//
// Win11 的設定介面**每一個區塊都是一張卡片**,而那正是「看起來像真的
// 軟體」與「看起來像對話框」的分界。現況內容區是「標題 → 說明 → 控制項」
// 一路往下攤平,沒有容器。
//
// ⚠ 卡片**不是**新的控制項:它畫在父視窗的 WM_PAINT 裡,所以不涉及
//   無障礙。卡片內部控制項的位置**照舊由 LayoutSettingsPageDip() 算**,
//   卡片只是多一個「把某幾顆控制項的矩形聯集起來再往外撐 padding」的輸出。
//   這樣 W18 / W31' 量的還是同一批矩形。
struct CardRect {
  RectI rect;
  // 卡內列與列之間的分隔線 y(內容座標)。⚠ 要縮排 s6,不縮排的話
  //   它會碰到卡片的圓角,看起來像裂縫(§12.14.6.7)。
  std::vector<int> divider_ys;
};

struct PageLayout {
  std::vector<PlacedControl> items;
  // 這一頁上的卡片。⚠ 順序 = 由上而下。
  std::vector<CardRect> cards;
  // 內容總高(含底部留白)。**捲動範圍唯一的來源** ——
  // 所以任何一顆沒有推進堆疊的控制項都會落在捲動範圍以外。
  int content_h_dip = 0;
};

// ── 版面上會隨執行期狀態換掉整塊的東西 ─────────────────────────
//
// ⚠ 這裡只放**真的會換掉一整塊**的狀態,不是所有會變的東西。
//   文字變了(例如「現在預設是○○」)不算 —— 那不影響任何一個矩形,
//   所以它不該讓版面多一個分支。多一個 bool 就多一種只有那個狀態下
//   才走得到的版面,而走不到的版面就是沒有人看過的版面。
// ── 「輸入方案」頁那一格說明,現在說的是哪一件事(#62)───────────
//
// ⚠ 走到那一格的路有三條,而它們在畫面上以前是**同一句話**
//   (「目前一種都沒有」)。三條的下一步完全不同:
//
//     kSchemaNoteEmpty       真的一種都沒有
//                            → 去「進階」按「重新整理字詞」
//     kSchemaNoteLoading     快取是冷的,一件查詢正在飛
//                            → 什麼都不用做,讀完會自己出現
//     kSchemaNoteUnavailable 那件查詢**根本沒有入列**(引擎在停 /
//                            沒有工作者)→ **沒有人會回來**,
//                            那句話會永遠停在那裡
//
//   第三條以前不可能被說出來:`Engine::Post` 把 `WorkQueue::Status`
//   整個丟掉,呼叫端只拿到一個空 vector —— 而「引擎沒有回應」與
//   「一個方案都沒有」是同一個空 vector。
enum SchemaListNote : int {
  kSchemaNoteEmpty = 0,
  kSchemaNoteLoading,
  kSchemaNoteUnavailable,
  kSchemaNoteCount,
};

// 那一格要說的三行。
//
// ⚠ **唯一的來源。** service/settings_window.cc 組字串、ui_layout.cc 算
//   高度,兩邊都走這一支 —— 各拿各的話,那一格的高度就會與它的內容
//   分家,而分家的方向是文字被切掉(#76 的形狀)。
// ⚠ note 越界回 kSchemaNoteEmpty 那一組:這一支在 WM_PAINT 路徑上。
struct SchemaNoteText {
  UiString title;
  UiString why;
  UiString next;
};
SchemaNoteText SchemaNoteLines(int note);

struct PageState {
  // 「輸入方案」頁:一種都沒有的時候,清單與三顆按鈕換成一段說明。
  bool schema_list_empty = false;
  // 「連網」頁:一次都沒有連過的時候,清單/欄名/計數與「清除紀錄」
  // 那一整個危險區塊都不出現,換成一句「一次都沒有連過」。
  //
  // ⚠ 預設是 **true**,方向是刻意的:讀不到紀錄(檔案不存在、讀失敗)
  //   時,畫面要說「一次都沒有連過」,而不是給一個空的表格再配一顆
  //   清除鍵。空表格讓人分不出「沒連過」與「壞掉了」,而「開關從沒
  //   開過所以紀錄是空的」正是使用者驗證我們的方式。
  bool net_log_empty = true;
  // 上面那一格說明說的是哪一件事。見 SchemaListNote。
  // ⚠ schema_list_empty 為 false 時它沒有意義(那一格不出現)。
  int schema_note = kSchemaNoteEmpty;
  // 「連網」頁:開關關著的時候,更新那一段上面多一句話說明那幾顆按鈕
  // 為什麼按不動、以及怎麼讓它們不灰(W1)。開著的時候那一句是假的,
  // 所以整格不出現。
  //
  // ⚠ 預設 **true**,方向與 net_log_empty 同一個理由:連網開關本身
  //   預設就是關的(離線為預設是產品定位),所以「第一次打開設定」
  //   看到的必須是**有說明**的那一版。預設 false 等於把這一條修給了
  //   少數人,而漏掉的正好是第一次來的那個人。
  //
  // ⚠ 這一格加在**結構的最後**是刻意的:tests 裡有
  //   `const PageState has_rows{false, false};` 這種位置初始化,
  //   插在中間會把 false 錯位餵給這一格,而症狀是「那句說明無聲無息
  //   地不見了」——畫面上什麼都沒壞,只是又回到 W1 那個狀態。
  bool net_switch_off = true;
};

// 一頁的完整版面。
PageLayout LayoutSettingsPageDip(int page, int window_w_dip, PageState state);

// 內容區的可視高度。⚠ 視窗矮於底部固定列時回 0,不回負數。
int ContentViewportHeightDip(int window_h_dip);

// ── 控制項裁在哪一條線上 ────────────────────────────────────────
//
// 底下**還有內容**(scroll_dip < scroll_max_dip)時,控制項裁在
// 可視高度再往上 kScrollFadeH 的地方 —— 那一段留給淡出區。
// 已經捲到底(或者根本不用捲)時沒有淡出區,裁在可視高度上。
//
// ⚠ 這一支是純函式,理由與 ScrollPlaceControlDip() 一樣:它以前**不存在**,
//   而「裁在哪」是寫在 service/settings_window.cc 裡的一個算式 ——
//   那個檔案在 Ubuntu 上編不起來,所以那個算式沒有任何測試看得到。
int ContentClipLineDip(int window_h_dip, int scroll_dip, int scroll_max_dip);

// 捲動上限(DIP)。內容放得下時是 0。
// ⚠ 這一支**必須**吃 window_h_dip —— 舊版的 ClickableTargetsDip 把它
//   `(void)` 掉,於是「排到視窗底部以外」對測試而言不存在。
int ScrollMaxDip(int page, int window_w_dip, int window_h_dip,
                 PageState state);

// ── 捲動之後,一顆內容區控制項該擺在哪、露出多少、藏不藏 ─────────
//
// ⚠ 這一支存在的理由只有一個:**讓那三件事變成測得到的東西**。
//   在它之前,三件事都寫在 service/settings_window.cc::LayoutUi() 裡,
//   而那個檔案在 Ubuntu 上編不起來 —— 於是覆核者把
//   `y = p->rect.y - scroll_` 改成 `y = p->rect.y`(捲軸拖得動、
//   內容一動也不動,也就是這一輪剛修掉的那個 BLOCKER 原封不動回來)
//   之後,206 個單元測試與 check_ui_spec.sh 全綠。
//
//   所以現在:決定權在這裡,LayoutUi 只負責把結果接到 Win32 上,
//   而 W25 驗的是**那三條接線**(y 從哪來、clip 從哪來、
//   ShowWindow 的引數從哪來),不是「檔案裡有沒有 scroll_ 這個字」。
struct ScrolledPlacement {
  // 視窗座標 = 內容座標 - 捲動量。**捲動量一定要參與**,
  // 否則捲軸會動而內容不動。
  int y_dip = 0;
  // ── ⚠ 只有兩個值:-1(整顆都畫)或 0(一個像素都不畫)──────────
  //
  // 舊版回的是「露出這麼高」,也就是**半顆控制項**:預設尺寸下文字頁的
  // 「全形/半形」區段標題在 y=497、高 22,可視高度 506 —— 它被裁成 9,
  // 從字的中間橫著切過去。使用者回報的是「摺線那裡看起來像壞掉」,
  // 而那正是它:被攔腰切開的字沒有任何辦法讀成「還有東西可以捲」。
  //
  // 現在一顆控制項要嘛整顆在裁切線以上,要嘛完全不畫。摺線因此永遠
  // 落在**空白**上(卡片的內距或頁面底色),而「還有更多」交給
  // 淡出區(kScrollFadeH)與捲軸去說 —— 它們說得清楚,半個字說不清楚。
  //
  // ⚠ 0 **不等於**隱藏:控制項仍然存在、仍然在 Tab 順序上(見 visible)。
  int clip_h_dip = -1;
  // ⚠ 永遠 true,而且**這是規定,不是實作細節**:捲出可視範圍的控制項
  //   只裁不藏。ShowWindow(SW_HIDE) 會讓它退出 Tab 順序,於是鍵盤
  //   使用者再也走不到它 —— 而捲動的存在正是為了讓那些控制項碰得到。
  //   呼叫端必須把這個欄位接到 ShowWindow 的引數上(而不是寫死
  //   SW_SHOW/SW_HIDE),這樣「藏起來」才會是一個測得到的行為改變。
  bool visible = true;
  // ── ⚠ **真的送進 SetWindowPos 的那一個矩形**(視窗座標)────────
  //
  // 2026-08-15:在它之前,呼叫端拿的是 `y_dip` + `clip_h_dip` 兩個值,
  // 而「一個像素都不畫」是靠 `SetWindowRgn()` 給那顆控制項套一個空區域
  // 達成的。那個作法有一個結構性的問題:**區域不在版面模型裡**。
  // 版面說「這顆在 y=507」,而 y=507 已經壓在底部固定列(H−48=512)上;
  // 畫面上它到底有沒有被畫出來,取決於一個沒有任何純函式看得到的
  // Win32 呼叫。實機截圖(796×599)量到的就是這個:外觀頁的
  // 「標準」那一列被畫在 y=507..543,把「關閉」鈕蓋掉只剩右緣十幾個
  // 像素;文字頁的區段說明被畫在 y=521..553,壓在同一條列上。
  //
  // 所以現在「不畫」是一個**尺寸**,不是一個區域:w = h = 0。
  // 空矩形畫不出任何東西是幾何,不是 Win32 的某一條語意 ——
  // 而且 `RectI::empty()` 讓它在版面模型上**量得到**
  // (見 DrawnRectsDip 與 test_ui_layout.cc 的
  //  nothing_is_ever_drawn_on_the_bottom_fixed_bar)。
  //
  // ⚠ 位置(x/y)仍然是真的位置,不是把它挪到畫面外:控制項還在
  //   Tab 順序上,焦點一到 EnsureFocusVisible() 就會捲到它 ——
  //   那條路徑讀的是**內容座標**,不是這裡的 rect。
  RectI rect;
};
// ⚠ 第三個引數是**裁切線**(ContentClipLineDip()),不是可視高度。
//   兩者在「還有更多」的時候差 kScrollFadeH -- 傳可視高度進去的話,
//   控制項會畫進淡出區裡,而字上面蓋一層淡出看起來像那一列被停用了。
ScrolledPlacement ScrollPlaceControlDip(const RectI& content_rect,
                                        int scroll_dip, int clip_line_dip);

// ── 畫面上**真的畫得出來**的每一塊(視窗座標)────────────────────
//
// ⚠ 這一支存在的理由是一次量錯的宣告。上一輪的報告說摺線那一段修好了,
//   而十張實機截圖(796×599)量出來的是:外觀頁與文字頁的底部固定列
//   仍然被蓋掉 —— 「關閉」鈕只剩右緣十幾個像素,而 SetStatus() 寫的
//   「已套用」「正在套用…」那一行完全看不到。
//
//   成因不是那一段裁切沒寫,是**沒有人量得到它**:卡片裁在父視窗的
//   DC 上(IntersectClipRect),控制項裁在 SetWindowRgn 上,兩者都是
//   Win32 的呼叫,而版面模型只認得「這顆在 y=507」。y=507 + 高 36
//   壓在 H−kBottomBarH=512 那一列上,所以只要那一個 Win32 呼叫沒有
//   生效(或者像截圖那條路徑一樣看不見它),畫面就是壞的,
//   而每一條純函式的測試都是綠的。
//
//   這一支把「畫得出來的是哪些矩形」變成一個**算得出來的東西**:
//   卡片照 OnPaint 的裁切線裁,控制項照 ScrollPlaceControlDip() 的
//   結果取,兩者合起來就是畫面。test_ui_layout.cc 的
//   nothing_is_ever_drawn_on_the_bottom_fixed_bar 對五頁 ×
//   每一個捲動位置斷言:**底部固定列那一帶不得有任何一塊**。
struct DrawnRect {
  int id = 0;            // 控制項 id;卡片是 0
  bool is_card = false;
  const char* what = "";  // 診斷用,永遠英文(§4.11)
  RectI rect;             // 視窗座標(已經減過捲動量、已經裁過)
};
std::vector<DrawnRect> DrawnRectsDip(int page, int window_w_dip,
                                     int window_h_dip, int scroll_dip,
                                     PageState state);

// ── 「預設」徽章擺哪裡(§12.14.6.5)─────────────────────────────
//
// ⚠ **這一支存在的理由是 §12.14.0 第 3 條**:徽章的位置以前從**列矩形**
//   往回推,而列矩形會被 RowRect() 撐到 GetClientRect 的寬度。清單帶
//   WS_BORDER、沒有 LVS_NOSCROLL,所以項目一多就出現直捲軸,client 變窄
//   而欄寬沒跟著變窄 —— 那一列比看得見的範圍寬,徽章被排到可視區外面,
//   被 DC 的裁剪切掉。使用者實機回報過:「清單右上角那個徽章畫壞了」。
//
//   修法與那個因果對不對無關:**位置不該從列矩形往回推**。這裡用控制項
//   自己的寬度,而且**明確扣掉捲軸寬** —— 這樣它在有沒有捲軸兩種情況下
//   都是對的,也才測得到。
//
// ⚠ has_vscroll 要從 `GetWindowLongW(list, GWL_STYLE) & WS_VSCROLL` 來,
//   **不要**用「項目數 > 可視列數」自己推 —— 那會在捲軸剛出現的那一幀算錯。
//
// badge_text_w_dip 是量出來的字寬(GetTextExtentPoint32W)。純函式量不了字,
// 所以它是**輸入** —— 測試餵合成的寬度,不假裝量得到。
struct BadgePlacement {
  RectI badge;      // 膠囊本體(相對清單控制項的 client 座標)
  int radius_dip;   // = 高 ÷ 2。膠囊不是自由參數
  int name_right;   // 名稱截尾的右界 —— 與徽章左緣**同一個算式**
};
BadgePlacement BadgePlacementDip(int list_w_dip, int row_top_dip,
                                 int row_h_dip, int badge_text_w_dip,
                                 int border_dip, int scrollbar_w_dip,
                                 bool has_vscroll);

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
                                           int page, PageState state);

}  // namespace rimewin

#endif  // RIMEWIN_UI_LAYOUT_H_
