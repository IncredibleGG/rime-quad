#include "ui_theme.h"

#include <cstring>

namespace rimewin {
namespace {

// 讀 HKCU\...\Themes\Personalize 的 AppsUseLightTheme。
// 0 = 深色。鍵不存在 → 視為淺色(Win10 1803 之前沒有這個鍵)。
bool SystemPrefersDark() {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(
          HKEY_CURRENT_USER,
          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
          0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
    return false;
  DWORD v = 1;
  DWORD size = sizeof(v);
  DWORD type = 0;
  const LONG r = ::RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                                    reinterpret_cast<LPBYTE>(&v), &size);
  ::RegCloseKey(key);
  if (r != ERROR_SUCCESS || type != REG_DWORD) return false;
  return v == 0;
}

bool HighContrastOn() {
  HIGHCONTRASTW hc{};
  hc.cbSize = sizeof(hc);
  if (!::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0))
    return false;
  return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

}  // namespace

const char* AppearancePrefValue(AppearancePref p) {
  switch (p) {
    case AppearancePref::kLight:
      return "light";
    case AppearancePref::kDark:
      return "dark";
    default:
      return "followSystem";
  }
}

AppearancePref AppearancePrefFromValue(const char* v) {
  if (!v) return AppearancePref::kFollowSystem;
  if (std::strcmp(v, "light") == 0) return AppearancePref::kLight;
  if (std::strcmp(v, "dark") == 0) return AppearancePref::kDark;
  return AppearancePref::kFollowSystem;
}

bool Theme::Refresh(AppearancePref pref) {
  const bool hc = HighContrastOn();
  Mode next;
  if (hc) {
    // §12.7.4:高對比**壓過**使用者在我們這裡選的深淺。他開高對比是因為
    // 看不清楚,而我們的色票(不管哪一份)都不是為那件事設計的。
    next = Mode::kHighContrast;
  } else {
    switch (pref) {
      case AppearancePref::kLight:
        next = Mode::kLight;
        break;
      case AppearancePref::kDark:
        next = Mode::kDark;
        break;
      default:
        next = SystemPrefersDark() ? Mode::kDark : Mode::kLight;
        break;
    }
  }
  if (next == mode_ && hc == high_contrast_) return false;
  mode_ = next;
  high_contrast_ = hc;
  // 換了模式就把 GDI 物件全部丟掉重建 —— 留著的是舊色票的顏色。
  Clear();
  return true;
}

COLORREF Theme::Color(Role r) const {
  if (mode_ == Mode::kHighContrast) {
    // 整份色票停用,改走系統的。
    return ::GetSysColor(SysColorFor(r));
  }
  const Rgb* p = PaletteFor(mode_);
  const Rgb c = p[r];
  return RGB(c.r, c.g, c.b);
}

HBRUSH Theme::Brush(Role r) {
  const int key = static_cast<int>(r);
  auto it = brushes_.find(key);
  if (it != brushes_.end()) return it->second;
  HBRUSH b = ::CreateSolidBrush(Color(r));
  brushes_[key] = b;
  return b;
}

HPEN Theme::Pen(Role r, int width_px) {
  if (width_px < 1) width_px = 1;
  const int key = static_cast<int>(r) * 64 + (width_px & 63);
  auto it = pens_.find(key);
  if (it != pens_.end()) return it->second;
  HPEN p = ::CreatePen(PS_SOLID, width_px, Color(r));
  pens_[key] = p;
  return p;
}

void Theme::Clear() {
  for (auto& kv : brushes_)
    if (kv.second) ::DeleteObject(kv.second);
  brushes_.clear();
  for (auto& kv : pens_)
    if (kv.second) ::DeleteObject(kv.second);
  pens_.clear();
}

void Theme::ApplyTitleBar(HWND hwnd) const {
  if (!hwnd) return;
  // dwmapi 動態載入。不為了一個函式多一個匯入 —— 而且它在很舊的
  // Windows 上不存在,靜態連結會讓整支程式載不起來。
  using DwmSetFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
  static HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll");
  static DwmSetFn fn =
      dwm ? reinterpret_cast<DwmSetFn>(reinterpret_cast<void*>(
                ::GetProcAddress(dwm, "DwmSetWindowAttribute")))
          : nullptr;
  if (!fn) return;

  const BOOL dark = mode_ == Mode::kDark ? TRUE : FALSE;
  // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE(Win10 1903+/Win11,已公開文件,
  // Ubuntu 上的 mingw dwmapi.h 也是 20)。Win10 1809 是 19。
  // 先試 20,回傳失敗再試 19,再失敗就放著。
  if (FAILED(fn(hwnd, 20, &dark, sizeof(dark))))
    fn(hwnd, 19, &dark, sizeof(dark));
}

bool Theme::IsColorSetChange(LPARAM l) {
  const wchar_t* s = reinterpret_cast<const wchar_t*>(l);
  return s && ::lstrcmpiW(s, L"ImmersiveColorSet") == 0;
}

}  // namespace rimewin
