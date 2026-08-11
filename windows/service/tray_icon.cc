#include "tray_icon.h"

#include <vector>

#include "ui_font.h"

namespace rimewin {
namespace {

int IconSizeForDpi(UINT dpi) {
  // 托盤圖示的標準尺寸。挑最接近而不超過的那一階 —— 放大一顆小圖示
  // 在高 DPI 上會糊，而糊掉的圖示與認不出來的圖示是同一個問題。
  if (dpi >= 192) return 32;
  if (dpi >= 144) return 24;
  if (dpi >= 120) return 20;
  return 16;
}

}  // namespace

HICON MakeModeTrayIcon(const wchar_t* text, UINT dpi) {
  if (!text || !*text) return nullptr;
  const int len = static_cast<int>(::lstrlenW(text));
  const int n = IconSizeForDpi(dpi ? dpi : 96);

  HDC screen = ::GetDC(nullptr);
  if (!screen) return nullptr;
  HDC mem = ::CreateCompatibleDC(screen);
  if (!mem) {
    ::ReleaseDC(nullptr, screen);
    return nullptr;
  }

  // ⚠ 32 位元 top-down DIB。托盤在 Win10/11 上會依主題把圖示放在深色或
  //   淺色底上,所以圖示本身必須有 alpha —— 用單色遮罩畫出來的東西在
  //   深色工作列上會出現一圈黑邊,而那看起來就像「壞掉的圖示」。
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = n;
  bi.bmiHeader.biHeight = -n;  // 負 = top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP color =
      ::CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!color || !bits) {
    if (color) ::DeleteObject(color);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);
    return nullptr;
  }
  HGDIOBJ old_bmp = ::SelectObject(mem, color);

  // ⚠ 全透明起手。CreateDIBSection 不保證清空。
  ::ZeroMemory(bits, static_cast<size_t>(n) * static_cast<size_t>(n) * 4);

  // 字。⚠ 用**白色**畫,再把 alpha 補成不透明 —— 托盤會依主題自己
  //   反色處理單色圖示;而我們畫的是有 alpha 的彩色圖示,所以要挑一個
  //   在深淺兩種工作列上都看得見的顏色。白字 + 一圈深色描邊太細會糊,
  //   所以改用「深色圓底 + 白字」:那是三家競品(微軟、搜狗、小狼毫)
  //   在托盤那一格的共同做法,而且與工作列主題無關。
  const int pad = n >= 24 ? 1 : 0;
  RECT disc{pad, pad, n - pad, n - pad};
  HBRUSH bg = ::CreateSolidBrush(RGB(0x33, 0x33, 0x33));
  HGDIOBJ old_brush = ::SelectObject(mem, bg);
  HGDIOBJ old_pen = ::SelectObject(mem, ::GetStockObject(NULL_PEN));
  ::Ellipse(mem, disc.left, disc.top, disc.right, disc.bottom);
  ::SelectObject(mem, old_pen);
  ::SelectObject(mem, old_brush);
  ::DeleteObject(bg);

  // ⚠ 兩個字元的字面(En)與一個漢字的字級不能一樣,否則兩個字母會
  //   撐出圓底之外。按**字數**挑高度,不是按 DPI 算一個通用值。
  const int cell = n - pad * 2;
  const int h = (len > 1) ? (cell * 55 / 100) : (cell * 78 / 100);
  // ⚠ **不自己 CreateFontIndirectW。** 那一支給一個不存在的字體時會回一個
  //   有效的 HFONT(靜默 fallback),所以字體的存在性檢查全 repo 只在
  //   service/ui_font.cc 做一次(它用 EnumFontFamiliesExW),而 W6 守著
  //   「只有那一個檔案碰得到 CreateFontIndirectW」。
  //
  //   走 FontSet 還順便拿到中文字體的解析 —— 這一顆圖示上畫的是一個漢字,
  //   而挑錯字體的症狀是畫出一個豆腐方塊,那與「認不出來的圖示」是同一
  //   個問題。
  //
  // ⚠ FontSet 吃的是 DIP,而 h 是像素。
  FontSet fonts;
  fonts.Reset(dpi ? dpi : 96, Script::kHant);
  const int size_dip = MulDiv(h, 96, static_cast<int>(dpi ? dpi : 96));
  HFONT font = fonts.Get(size_dip > 0 ? size_dip : 1, /*semibold=*/true);
  HGDIOBJ old_font = font ? ::SelectObject(mem, font) : nullptr;

  ::SetBkMode(mem, TRANSPARENT);
  ::SetTextColor(mem, RGB(0xFF, 0xFF, 0xFF));
  RECT tr{pad, pad, n - pad, n - pad};
  ::DrawTextW(mem, text, len, &tr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  if (old_font) ::SelectObject(mem, old_font);
  // ⚠ **不 DeleteObject。** 那個 HFONT 屬於 FontSet 的快取,
  //   它會在 fonts 解構時一起收 —— 這裡刪掉等於 double free。

  // ⚠ GDI 的文字與 Ellipse 都**不寫 alpha 通道**。畫完之後圓底範圍內
  //   的 alpha 仍然是 0,而托盤會把 alpha 0 當成全透明 ——
  //   結果是一顆什麼都看不到的圖示。所以這裡逐像素把「有畫到東西的
  //   地方」補成不透明。
  //
  //   判準是「這個像素不是我們一開始清成的全 0」—— 圓底是 0x333333、
  //   字是 0xFFFFFF,兩者都不是 0。
  unsigned char* px = static_cast<unsigned char*>(bits);
  const size_t count = static_cast<size_t>(n) * static_cast<size_t>(n);
  for (size_t i = 0; i < count; ++i) {
    unsigned char* p = px + i * 4;
    if (p[0] || p[1] || p[2]) p[3] = 0xFF;
  }

  ::SelectObject(mem, old_bmp);

  // 遮罩點陣圖:32 位元 alpha 圖示仍然要給一張,而且要全 0
  // (0 = 不遮蔽)—— 給 nullptr 的話 CreateIconIndirect 會失敗。
  // ⚠ 內容必須是全 0(0 = 不遮蔽)。CreateBitmap 給 nullptr 的話內容
  //   **未定義** —— 隨機的 1 會在圖示上打出隨機的洞,而那種缺陷在
  //   不同機器上長得不一樣。自己配一塊清零的緩衝。
  const size_t mask_stride = static_cast<size_t>((n + 15) / 16) * 2;
  std::vector<unsigned char> mask_bits(mask_stride * static_cast<size_t>(n), 0);
  HBITMAP mask = ::CreateBitmap(n, n, 1, 1, mask_bits.data());
  HICON icon = nullptr;
  if (mask) {
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    icon = ::CreateIconIndirect(&ii);
    ::DeleteObject(mask);
  }

  ::DeleteObject(color);
  ::DeleteDC(mem);
  ::ReleaseDC(nullptr, screen);
  return icon;
}

}  // namespace rimewin
