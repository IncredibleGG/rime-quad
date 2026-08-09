// windows/service/ui_theme.h — 深淺色、高對比、GDI 物件快取(§12.7)
//
// ── Win32 沒有深色模式 ──────────────────────────────────────────
//
// 這個檔案把「做得到」與「做不到」分開:
//
//   做得到:標題列(DwmSetWindowAttribute)、視窗底色、STATIC/核取方塊/
//           單選鈕/唯讀 EDIT 的**文字與底**(WM_CTLCOLORSTATIC)、
//           以及我們自己畫的每一塊。
//   做不到:核取方塊與單選鈕的**方塊字形**、ListView 與 EDIT 的**捲軸**、
//           ComboBox 的下拉鈕與外框。它們在啟用視覺樣式後由 uxtheme 畫,
//           `WM_CTLCOLOR*` 只影響文字與底。
//
// ⚠ **裁決:禁止使用 uxtheme.dll 的未公開序數 API**
//   (SetPreferredAppMode / AllowDarkModeForWindow / ShouldAppsUseDarkMode /
//    RefreshImmersiveColorPolicyState —— 它們沒有名字,只有序數 132/133/135/104)。
//
//   理由與這個專案的定位直接相關,不是潔癖:
//   1. **審計。** 一個外人讀到 `GetProcAddress(uxtheme, MAKEINTRESOURCEA(135))`
//      看不出它在做什麼。我們要求別人相信「這支程式不連網」,
//      靠的就是「每一個匯入都解釋得清楚」。
//   2. **序數會變。** 它們不在任何契約裡,一次 Windows 更新就可能挪位,
//      而失敗的樣子是**呼叫到別的函式**,不是回傳錯誤。
//   3. 它換到的東西很小:方塊字形、捲軸、下拉鈕。
//
//   所以留下來的兩處已知瑕疵(**核取方塊與單選鈕的方塊字形**、
//   **ListView 的捲軸**)是**刻意接受**的。寫在這裡是為了讓它不要被
//   當成 bug 反覆回報。W14 在守這條。
//
// ── 高對比是第三種模式,不是深色的一種(§12.7.4)──────────────
//
// 開啟時**整份色票停用,改走 GetSysColor()**。使用者開高對比是因為他
// 看不清楚,這時我們的青瓷綠不是幫忙。
//
#ifndef RIMEWIN_SERVICE_UI_THEME_H_
#define RIMEWIN_SERVICE_UI_THEME_H_

#include <windows.h>

#include <map>

#include "../common/ui_palette.h"

namespace rimewin {

// 使用者在設定裡選的。三態,不是兩態(§12.7.2 / §3.5 第 1 條)——
// 只有兩態的話,使用者按過一次就回不到「讓系統決定」。
enum class AppearancePref { kFollowSystem = 0, kLight, kDark };

const char* AppearancePrefValue(AppearancePref p);
AppearancePref AppearancePrefFromValue(const char* v);

class Theme {
 public:
  ~Theme() { Clear(); }

  // 重讀系統偏好與高對比狀態。回傳 true = 有變化(呼叫端要重畫)。
  bool Refresh(AppearancePref pref);

  Mode mode() const { return mode_; }

  COLORREF Color(Role r) const;
  // ⚠ 筆刷要**快取**。每次訊息 CreateSolidBrush 是 GDI 物件洩漏,
  //   而症狀是「開幾小時之後畫面開始畫不出來」—— 那時已經很難聯想回來。
  HBRUSH Brush(Role r);
  HPEN Pen(Role r, int width_px);

  void Clear();

  // 標題列。屬性值 20(Win10 1903+/Win11)。Win10 1809 是 19。
  // 先試 20,失敗再試 19,再失敗就放著(淺色標題列 + 深色內容,
  // 難看但不會壞)—— **這個退化路徑本身是規格的一部分**,
  // 不是我們對版本號有把握。
  void ApplyTitleBar(HWND hwnd) const;

  // WM_SETTINGCHANGE 的 lParam 是不是 "ImmersiveColorSet"。
  static bool IsColorSetChange(LPARAM l);

 private:
  Mode mode_ = Mode::kLight;
  bool high_contrast_ = false;
  std::map<int, HBRUSH> brushes_;
  std::map<int, HPEN> pens_;
};

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_UI_THEME_H_
