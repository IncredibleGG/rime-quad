// windows/common/statusbar_place.h — 懸浮狀態列的位置記憶與還原(§12.10.5)
//
// ── ⚠ 為什麼不可以只存螢幕座標 ──────────────────────────────────
//
// 使用者把那一橫拖到第二顆螢幕上,然後拔掉那顆螢幕。存下來的座標
// 現在落在螢幕外面,而症狀是「**那一橫不見了**」——
// 使用者不會把「我拔了螢幕」和「輸入法的狀態列消失了」聯想在一起,
// 他會以為輸入法壞了,而且沒有任何自救途徑(那一橫上刻意沒有 X,
// 所以他也不會覺得是自己關掉的)。
//
// 所以存的是**當時那顆螢幕的工作區矩形**加上**相對右下角的偏移**,
// 還原時三段回落:
//
//   1. 找工作區矩形與快照**完全相同**的那一顆螢幕 → 用它 + dx/dy
//   2. 找不到 → **主螢幕**工作區的右下角 + 預設偏移 12/12
//   3. 無論走哪一條,最後一律把視窗矩形**夾進**該螢幕的工作區
//
// 第 3 條沿用 §8.6.7.3 已經寫下的規則:「窗**必須**完整落在可用區內。
// 先夾窗的尺寸,再擺位置 —— 位置的計算不得產生負的邊距」。
//
// ⚠ 用**工作區**(MONITORINFO.rcWork)不是整個螢幕矩形:整個矩形會讓
//   這一橫被工作列蓋住,而 §8.6.7.3 註明那是實測會發生的事,不是理論問題。
//
// 全部是純函式:輸入是「快照 + 目前的螢幕清單」,輸出是一個矩形。
// **在 Ubuntu 上測得到,不需要真的 Windows** —— W20 靠這條。
//
#ifndef RIMEWIN_STATUSBAR_PLACE_H_
#define RIMEWIN_STATUSBAR_PLACE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace rimewin {

struct WorkArea {
  int left = 0, top = 0, right = 0, bottom = 0;
  bool primary = false;
  int dpi = 96;

  int width() const { return right - left; }
  int height() const { return bottom - top; }
  bool operator==(const WorkArea& o) const {
    return left == o.left && top == o.top && right == o.right &&
           bottom == o.bottom;
  }
};

// 存進設定檔的東西。⚠ 偏移是 **DIP**,不是像素:使用者把它拖到
// 150% 的螢幕上再拖回來,存像素的話位置會跑掉。
// 讓開之後與障礙物之間留多少(DIP)。§12.10.3 那一橫本身高 28,
// 8 是「看得出是兩條」而不是「中間空一格」的距離。
constexpr int kBarAvoidGapDip = 8;
// 最多讓幾步。⚠ 一堆互相重疊的橫條不可以讓它繞不完 —— 這一支跑在
//   那一橫的 UI 執行緒上(Relayout → ApplyPlacement)。
constexpr int kBarAvoidMaxSteps = 8;

// 一塊「不要壓上去」的螢幕矩形。右下為排他(與 Win32 的 RECT 一致)。
struct ObstacleRect {
  int left = 0, top = 0, right = 0, bottom = 0;
};

struct BarAnchor {
  bool valid = false;
  int work_left = 0, work_top = 0, work_right = 0, work_bottom = 0;
  int dx_dip = 12;  // 相對工作區**右下角**的偏移,正值 = 向左上
  int dy_dip = 12;
};

struct PlacedBar {
  int x = 0, y = 0, w = 0, h = 0;
  // 走了三段回落的哪一段。診斷用 —— 使用者回報「它跑回右下角了」時,
  // 這個值直接說明是螢幕不見了還是設定壞了。
  // kNudgedAside = 位置算完之後被**避讓**挪過(見 AvoidObstacles)。
  enum class How {
    kRestoredExact,
    kFellBackToPrimary,
    kClamped,
    kNudgedAside
  } how = How::kRestoredExact;
  // 用了 monitors 裡的哪一顆(索引)。-1 = 一顆都沒有。
  // ⚠ 避讓要在**同一顆螢幕的工作區內**做,所以這一格不可以省 ——
  //   呼叫端沒有別的辦法知道三段回落最後挑了哪一顆。
  int monitor = -1;
  // 避讓把它往下移了多少像素(負值 = 往上)。0 = 沒有避讓。
  // ⚠ 存位置的時候要把它**扣回去**:使用者拖到的那個點才是他選的位置,
  //   把避讓後的位置存起來的話,每一次重排都會再挪一次,而那個偏移
  //   會一路累積到螢幕邊緣。
  int nudge_dy = 0;
};

// 依快照與目前的螢幕清單決定那一橫該畫在哪。
// w_dip/h_dip 是那一橫的固定尺寸(§12.10.3:28 高)。
// monitors 為空時回傳一個原點在 0,0 的矩形 —— 那是不可能發生的情形
// (沒有螢幕就沒有人在看),但**不可以**因此回傳未初始化的東西。
PlacedBar PlaceStatusBar(const BarAnchor& anchor,
                         const std::vector<WorkArea>& monitors, int w_dip,
                         int h_dip);

// ══ §12.10.5 的避讓:不要壓在別人的浮動橫條上(#120)══════════════
//
// ── 使用者看到的 ────────────────────────────────────────────────
//
// 他切到第三方輸入法,那一家也有一條浮動狀態列,也停在右下角。
// 兩條疊在一起 —— 而我們是 HWND_TOPMOST 且**後畫**的那一條,所以蓋住
// 的是人家的。截圖上看起來像我們把別人的介面弄壞了。
//
// ── 為什麼不用「認得出特定輸入法」的白名單 ──────────────────────
//
// 那會過期。使用者可能裝的是明年才出的那一版、或一個我們沒聽過的牌子,
// 而白名單長得跟綠燈一模一樣:它不會報錯,只會安靜地不避讓。
//
// 所以判準是**形狀**,不是身分:呼叫端交進來一組矩形,這裡只做幾何。
// 「哪些窗算障礙」由呼叫端用結構上的性質決定(永遠在最上層、而且不搶
// 焦點 —— 那正是一條浮動工具列必須有的樣子,見 service/status_bar.cc)。
//
// ── 往哪邊挪 ────────────────────────────────────────────────────
//
// 只動 y,不動 x:那一橫是**右錨定**的(§12.10.5),左右挪會離開使用者
// 選的那個角;而會跟它撞在一起的東西幾乎一定也是一條沿著同一條邊排的
// 橫條,所以垂直方向才有空位。
//
// 方向由**它自己在工作區裡的位置**決定,不是寫死「往上」:
//   · 在工作區的下半 → 往上讓(工作列在下面時的常態);
//   · 在上半         → 往下讓(工作列在上面、或使用者把它拖到頂了)。
// 工作列在左 / 右時工作區本來就已經把它扣掉了,所以同一條規則適用。
//
// ⚠ 三件事一定要守住,而且三件都在 tests/test_statusbar_place.cc 裡:
//   1. **絕不移出工作區。** 讓不開就維持原位 —— 「壓在別人上面」難看,
//      「跑到螢幕外面」是消失,而使用者不會把它跟避讓聯想在一起。
//   2. **只在同一顆螢幕上讓。** 多螢幕時障礙物可能落在別顆上,那與
//      我們無關。
//   3. **步數有上限。** 一堆互相重疊的障礙物不可以讓它繞不完。
//
// obstacles 是**螢幕座標**的矩形,右下為排他(與 RECT 一致)。
PlacedBar AvoidObstacles(const PlacedBar& placed,
                         const std::vector<WorkArea>& monitors,
                         const std::vector<ObstacleRect>& obstacles);

// 使用者拖完之後,把「現在在哪」變成可以存起來的快照。
BarAnchor MakeAnchor(const WorkArea& on, int x_px, int y_px, int w_px,
                     int h_px);

// 設定檔的字串形式。⚠ 壞掉的字串一律回 `valid = false`(= 回到預設位置),
// 絕不讓一行壞掉的設定變成一個畫在螢幕外的視窗。
std::string SerializeAnchor(const BarAnchor& a);
BarAnchor ParseAnchor(const std::string& s);

}  // namespace rimewin

#endif  // RIMEWIN_STATUSBAR_PLACE_H_
