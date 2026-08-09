// windows/common/ui_dip.h — DIP → 實體像素(§12.3)
//
// ⚠ **規範裡桌面欄的 `pt`,在 Windows 上一律讀作 DIP**(96 DPI 下 1 DIP = 1 px),
//   不是排版學的點。理由見 docs/ui-design.md §12.3 的三條佐證。
//
// ── 為什麼不可以用 `static_cast<int>(dip * dpi_scale_)` ──────────────
//
// 那是舊 settings_window.cc:539 的寫法,而它**無條件捨去**:
// 150% 下 11 DIP → 16.5 → 16(應為 17)。單獨看每一項都只差 1 px,
// 但版面是一項接一項疊出來的,誤差**沿著版面累積** ——
// 症狀是「高 DPI 下右邊那一欄擠在一起」,而沒有任何一行程式碼看起來是錯的。
//
// Win32 的 `MulDiv` 用 64 位元中間值、四捨五入(取最接近,.5 遠離零),
// 是唯一該用的那個。本檔把它實作成**純函式**,理由有兩個:
//
//   1. `windows/common/` 底下不可以 include windows.h(見 CMakeLists 的說明)
//      —— 那是版面能在 Ubuntu 上被測到的前提,而 W18/W19 靠這條。
//   2. 版面計算與真的視窗**必須用同一支**換算。各寫一份的話,
//      單元測試驗的是測試自己那一份。
//
#ifndef RIMEWIN_UI_DIP_H_
#define RIMEWIN_UI_DIP_H_

#include <cstdint>

namespace rimewin {

// Win32 `MulDiv` 的語義:64 位元中間值,四捨五入(.5 遠離零),
// denom == 0 時回 -1(與 Win32 一致)。
inline int MulDivRound(int value, int numer, int denom) {
  if (denom == 0) return -1;
  const int64_t n = static_cast<int64_t>(value) * static_cast<int64_t>(numer);
  const int64_t d = static_cast<int64_t>(denom);
  const int64_t half = (d < 0 ? -d : d) / 2;
  const int64_t r = ((n < 0) != (d < 0)) ? (n - half) / d : (n + half) / d;
  return static_cast<int>(r);
}

// DIP → 實體像素。**版面裡每一個長度都要走這一支。**
inline int Dip(int dip, int dpi) { return MulDivRound(dip, dpi, 96); }

// LOGFONTW.lfHeight。⚠ 負值 = 字元高度,不是儲存格高度 ——
// 正值會讓字比預期小一截(差的是 internal leading)。
inline int FontHeightFromDip(int size_dip, int dpi) {
  return -MulDivRound(size_dip, dpi, 96);
}

}  // namespace rimewin

#endif  // RIMEWIN_UI_DIP_H_
