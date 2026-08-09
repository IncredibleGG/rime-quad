#include "status_bar.h"

// GET_X_LPARAM / GET_Y_LPARAM。⚠ 不要自己用 LOWORD/HIWORD 拆 ——
// 多螢幕時滑鼠座標**會是負的**,而 LOWORD 是無號的,
// 左邊那顆螢幕上的每一次點擊都會落在一個荒謬的座標上。
#include <windowsx.h>

#include <algorithm>

#include "../common/settings.h"
#include "../common/ui_dip.h"
#include "../common/ui_layout.h"
#include "../common/ui_strings.h"
#include "../winshared/winutil.h"
#include "engine.h"
#include "settings_store.h"
#include "settings_window.h"

namespace rimewin {
namespace {

constexpr wchar_t kClass[] = L"LuminaKeyStatusBar";
constexpr wchar_t kPopupClass[] = L"LuminaKeyStatusBarPopup";
constexpr UINT WM_RIME_REFRESH = WM_APP + 1;
constexpr UINT WM_RIME_SHOW = WM_APP + 2;
constexpr UINT WM_RIME_QUIT = WM_APP + 3;
constexpr UINT WM_RIME_THEME = WM_APP + 4;

// §12.10.3 的尺寸(DIP)。
constexpr int kBarH = 28;        // t4 字高 12 + 上下 padding 各 6 + 邊框 2
constexpr int kCellMinW = metric::kMinTarget;  // 28
constexpr int kCellPadH = space::s4;           // 10
constexpr int kCellGap = space::s3;            // 6
constexpr int kBarRadius = radius::kMedium;    // 7
constexpr int kBarBorder = metric::kHairline;  // 1

// ── ⚠ §8.12 的規範性字面,四端一致,**不得在地化** ────────────────
//
// 它們刻意**不**進 ui_strings.cc(§12.9.3 第 1 條):進了 catalog 就會有人
// 把簡體語系的「简」翻成別的寫法,而那正是規範要避免的事 ——
// 這四個字是**狀態指示**,不是介面文字。
//
// W10 兩個方向都驗:它們必須出現在這裡,而且不得出現在 catalog 裡。
// 「中」與「En」**兩態同時顯示**:只顯示一個字的話,「中」有兩種讀法
// (「現在是中文」還是「按下去變中文」),Android 被真機回報過。
constexpr wchar_t kGlyphChinese[] = L"中";
constexpr wchar_t kGlyphAscii[] = L"En";
constexpr wchar_t kGlyphSimplified[] = L"简";
constexpr wchar_t kGlyphTraditional[] = L"繁";

enum CellIndex { kCellMode = 0, kCellVariant, kCellSchema, kCellSettings };

}  // namespace

StatusBar::StatusBar(Engine* engine, SettingsStore* store)
    : engine_(engine), store_(store) {}

StatusBar::~StatusBar() { Stop(); }

DWORD WINAPI StatusBar::ThreadEntry(LPVOID self) {
  static_cast<StatusBar*>(self)->ThreadMain();
  return 0;
}

bool StatusBar::Start() {
  if (thread_) return true;
  ready_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  thread_ = ::CreateThread(nullptr, 0, &StatusBar::ThreadEntry, this, 0,
                           &thread_id_);
  if (!thread_) return false;
  if (ready_) ::WaitForSingleObject(ready_, 5000);
  return hwnd_ != nullptr;
}

void StatusBar::Stop() {
  if (thread_id_) ::PostThreadMessageW(thread_id_, WM_RIME_QUIT, 0, 0);
  if (thread_) {
    ::WaitForSingleObject(thread_, 3000);
    ::CloseHandle(thread_);
    thread_ = nullptr;
  }
  if (ready_) {
    ::CloseHandle(ready_);
    ready_ = nullptr;
  }
}

void StatusBar::SetVisible(bool on) {
  if (thread_id_)
    ::PostThreadMessageW(thread_id_, WM_RIME_SHOW, on ? 1 : 0, 0);
}

void StatusBar::Refresh() {
  if (thread_id_) ::PostThreadMessageW(thread_id_, WM_RIME_REFRESH, 0, 0);
}

void StatusBar::RefreshTheme() {
  if (thread_id_) ::PostThreadMessageW(thread_id_, WM_RIME_THEME, 0, 0);
}

void StatusBar::OnSnapshot(const Snapshot& snap) {
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    const bool a = (snap.status_flags & kStAsciiMode) != 0;
    const bool s = (snap.status_flags & kStSimplified) != 0;
    if (a != ascii_mode_ || s != simplified_ ||
        snap.schema_name != schema_name_ || !have_snapshot_) {
      ascii_mode_ = a;
      simplified_ = s;
      if (!snap.schema_name.empty()) schema_name_ = snap.schema_name;
      have_snapshot_ = true;
      changed = true;
    }
  }
  if (changed) Refresh();
}

void StatusBar::ThreadMain() {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &StatusBar::WndProc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;  // 全自繪
  wc.lpszClassName = kClass;
  ::RegisterClassExW(&wc);

  WNDCLASSEXW pc{};
  pc.cbSize = sizeof(pc);
  pc.lpfnWndProc = &StatusBar::PopupProc;
  pc.hInstance = ::GetModuleHandleW(nullptr);
  pc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  pc.hbrBackground = nullptr;
  pc.lpszClassName = kPopupClass;
  ::RegisterClassExW(&pc);

  // §12.10.3:TOOLWINDOW 讓它不出現在 Alt+Tab 與工作列;
  // NOACTIVATE 讓點它不搶焦點 —— 而「在句子中間切中英」正是它存在的理由,
  // 搶了焦點就會讓使用者正在打字的輸入框失去插入點。
  hwnd_ = ::CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClass, L"",
      WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, ::GetModuleHandleW(nullptr),
      this);
  if (ready_) ::SetEvent(ready_);
  if (!hwnd_) return;

  {
    UINT dpi = 96;
    using GetDpiFn = UINT(WINAPI*)(HWND);
    HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
    GetDpiFn fn = u32 ? reinterpret_cast<GetDpiFn>(reinterpret_cast<void*>(
                            ::GetProcAddress(u32, "GetDpiForWindow")))
                      : nullptr;
    if (fn) dpi = fn(hwnd_);
    dpi_ = dpi ? dpi : 96;
  }
  const Settings st = store_ ? store_->Load() : Settings();
  theme_.Refresh(AppearancePrefFromValue(
      st.Raw(keys::kAppearanceAppearance).c_str()));
  fonts_.Reset(dpi_, Script::kHant);
  visible_ = st.GetTri(keys::kAppearanceFloatingBar) != Tri::kFalse;

  Relayout();
  ApplyPlacement();
  if (visible_) ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

  MSG msg;
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.hwnd == nullptr) {
      switch (msg.message) {
        case WM_RIME_REFRESH:
          Relayout();
          ::InvalidateRect(hwnd_, nullptr, TRUE);
          continue;
        case WM_RIME_SHOW:
          visible_ = msg.wParam != 0;
          ::ShowWindow(hwnd_, visible_ ? SW_SHOWNOACTIVATE : SW_HIDE);
          continue;
        case WM_RIME_THEME: {
          const Settings s2 = store_ ? store_->Load() : Settings();
          theme_.Refresh(AppearancePrefFromValue(
              s2.Raw(keys::kAppearanceAppearance).c_str()));
          ::InvalidateRect(hwnd_, nullptr, TRUE);
          continue;
        }
        case WM_RIME_QUIT:
          ClosePopup();
          ::DestroyWindow(hwnd_);
          hwnd_ = nullptr;
          return;
        default:
          break;
      }
    }
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }
  fonts_.Clear();
  theme_.Clear();
}

// ─────────────────────────── 版面 ───────────────────────────

void StatusBar::Relayout() {
  bool ascii, simp;
  std::string name;
  {
    std::lock_guard<std::mutex> lock(mu_);
    ascii = ascii_mode_;
    simp = simplified_;
    name = schema_name_;
  }
  service_ready_ = engine_ && engine_->deploy_done() && engine_->deploy_ok();

  cells_.clear();
  if (!service_ready_) {
    // 第五種外觀:服務沒連上。四格在這個狀態下**全部不畫**
    // (它們此刻都是假的),整條改成一句話,而且整條可點。
    Cell c;
    c.text = UiText(UiString::kBarNotRunning);
    cells_.push_back(c);
  } else {
    Cell mode;
    mode.pair = true;
    mode.text = kGlyphChinese;
    mode.text2 = kGlyphAscii;
    mode.second_active = ascii;
    cells_.push_back(mode);

    Cell variant;
    variant.text = simp ? kGlyphSimplified : kGlyphTraditional;
    cells_.push_back(variant);

    Cell schema;
    // ⚠ 空狀態**整項略過**(§8.12 規範性):方案名還沒載入完成時,
    //   那一格完全不佔位置,不得畫成一塊看不出用途的空白。
    schema.text = name.empty() ? std::wstring() : Utf8ToWide(name);
    cells_.push_back(schema);

    Cell settings;
    settings.text = UiText(UiString::kBarSettings);
    cells_.push_back(settings);
  }

  HDC hdc = ::GetDC(hwnd_);
  HGDIOBJ oldf = hdc ? ::SelectObject(hdc, fonts_.Get(text_size::t4)) : nullptr;
  const int pad = Dip(kCellPadH, dpi_);
  const int gap = Dip(kCellGap, dpi_);
  const int minw = Dip(kCellMinW, dpi_);
  const int h = Dip(kBarH, dpi_);
  int x = Dip(kBarBorder + space::s2, dpi_);

  for (Cell& c : cells_) {
    if (c.text.empty() && !c.pair) {
      c.rc = RECT{0, 0, 0, 0};  // 略過:不佔位置
      continue;
    }
    std::wstring measure = c.text;
    if (c.pair) measure += L" " + c.text2;
    SIZE sz{};
    if (hdc)
      ::GetTextExtentPoint32W(hdc, measure.c_str(),
                              static_cast<int>(measure.size()), &sz);
    int w = sz.cx + 2 * pad;
    if (w < minw) w = minw;
    c.rc = RECT{x, Dip(kBarBorder, dpi_), x + w, h - Dip(kBarBorder, dpi_)};
    x += w + gap;
  }
  if (hdc) {
    if (oldf) ::SelectObject(hdc, oldf);
    ::ReleaseDC(hwnd_, hdc);
  }
  const int total = x - gap + Dip(kBarBorder + space::s2, dpi_);

  RECT cur{};
  ::GetWindowRect(hwnd_, &cur);
  ::SetWindowPos(hwnd_, HWND_TOPMOST, cur.left, cur.top, std::max(total, minw),
                 h, SWP_NOACTIVATE | SWP_NOMOVE);
}

void StatusBar::ApplyPlacement() {
  // §12.10.5 的三段回落。**全部是純函式**(common/statusbar_place.h),
  // 所以在 Ubuntu 上測得到 —— W20 靠這條。
  BarAnchor anchor;
  if (store_) {
    const Settings st = store_->Load();
    anchor = ParseAnchor(st.Raw(keys::kAppearanceFloatingBarPos));
  }

  std::vector<WorkArea> monitors;
  struct Ctx {
    std::vector<WorkArea>* out;
  } ctx{&monitors};
  ::EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR mon, HDC, LPRECT, LPARAM p) -> BOOL {
        Ctx* c = reinterpret_cast<Ctx*>(p);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!::GetMonitorInfoW(mon, &mi)) return TRUE;
        WorkArea w;
        // ⚠ 用**工作區**(rcWork)不是整個螢幕矩形:整個矩形會讓這一橫
        //   被工作列蓋住,而 §8.6.7.3 註明那是實測會發生的事。
        w.left = mi.rcWork.left;
        w.top = mi.rcWork.top;
        w.right = mi.rcWork.right;
        w.bottom = mi.rcWork.bottom;
        w.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        w.dpi = 96;
        using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        static HMODULE shcore = ::LoadLibraryW(L"shcore.dll");
        static GetDpiForMonitorFn fn =
            shcore ? reinterpret_cast<GetDpiForMonitorFn>(
                         reinterpret_cast<void*>(
                             ::GetProcAddress(shcore, "GetDpiForMonitor")))
                   : nullptr;
        if (fn) {
          UINT dx = 96, dy = 96;
          if (SUCCEEDED(fn(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy)))
            w.dpi = static_cast<int>(dy);
        }
        c->out->push_back(w);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));

  RECT rc{};
  ::GetWindowRect(hwnd_, &rc);
  const int w_dip = MulDivRound(rc.right - rc.left, 96, static_cast<int>(dpi_));
  const PlacedBar p = PlaceStatusBar(anchor, monitors, w_dip, kBarH);
  ::SetWindowPos(hwnd_, HWND_TOPMOST, p.x, p.y, p.w, p.h,
                 SWP_NOACTIVATE);
}

void StatusBar::SavePlacement() {
  if (!store_) return;
  RECT rc{};
  ::GetWindowRect(hwnd_, &rc);
  HMONITOR mon = ::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!::GetMonitorInfoW(mon, &mi)) return;
  WorkArea on;
  on.left = mi.rcWork.left;
  on.top = mi.rcWork.top;
  on.right = mi.rcWork.right;
  on.bottom = mi.rcWork.bottom;
  on.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
  on.dpi = static_cast<int>(dpi_);
  const BarAnchor a = MakeAnchor(on, rc.left, rc.top, rc.right - rc.left,
                                 rc.bottom - rc.top);
  Settings st = store_->Load();
  st.SetRaw(keys::kAppearanceFloatingBarPos, SerializeAnchor(a));
  store_->Save(st);
}

// ─────────────────────────── 繪製 ───────────────────────────

void StatusBar::Paint(HDC hdc) {
  RECT client{};
  ::GetClientRect(hwnd_, &client);

  // 雙緩衝(§12.6.4 第 3 條)。少了它,狀態一變就閃。
  HDC mem = ::CreateCompatibleDC(hdc);
  HBITMAP bmp = ::CreateCompatibleBitmap(hdc, client.right, client.bottom);
  HGDIOBJ old_bmp = ::SelectObject(mem, bmp);

  ::FillRect(mem, &client, theme_.Brush(kSurface));

  // 外框:一般是 outline 色,服務沒在跑時是 error 色。
  {
    const Role edge = service_ready_ ? kOutline : kError;
    HPEN pen = theme_.Pen(edge, Dip(kBarBorder, dpi_));
    HGDIOBJ oldp = ::SelectObject(mem, pen);
    HGDIOBJ oldb = ::SelectObject(mem, ::GetStockObject(NULL_BRUSH));
    const int r = Dip(kBarRadius, dpi_);
    ::RoundRect(mem, client.left, client.top, client.right, client.bottom, r,
                r);
    ::SelectObject(mem, oldb);
    ::SelectObject(mem, oldp);
  }

  ::SetBkMode(mem, TRANSPARENT);
  HGDIOBJ oldf = ::SelectObject(mem, fonts_.Get(text_size::t4));

  for (size_t i = 0; i < cells_.size(); ++i) {
    const Cell& c = cells_[i];
    if (c.rc.right <= c.rc.left) continue;  // 略過的那一格
    RECT r = c.rc;
    const bool hot = static_cast<int>(i) == hot_;
    const bool down = static_cast<int>(i) == pressed_;
    if (down)
      ::FillRect(mem, &r, theme_.Brush(kRowPressed));
    else if (hot)
      ::FillRect(mem, &r, theme_.Brush(kRowHover));

    if (!service_ready_) {
      ::SetTextColor(mem, theme_.Color(kError));
      ::DrawTextW(mem, c.text.c_str(), -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      continue;
    }

    if (c.pair) {
      // 兩態同時顯示:當前那一態用「當前態文字」色,另一態用次要色。
      // ⚠ 兩段之間留一個空白,而且**兩段都畫** —— 只畫一個字的話,
      //   使用者分不出那是「現在的狀態」還是「按下去會變成的狀態」。
      std::wstring first = c.text;
      std::wstring second = c.text2;
      SIZE s1{}, ssp{};
      ::GetTextExtentPoint32W(mem, first.c_str(),
                              static_cast<int>(first.size()), &s1);
      ::GetTextExtentPoint32W(mem, L" ", 1, &ssp);
      const int total_w = r.right - r.left;
      SIZE s2{};
      ::GetTextExtentPoint32W(mem, second.c_str(),
                              static_cast<int>(second.size()), &s2);
      int x = r.left + (total_w - (s1.cx + ssp.cx + s2.cx)) / 2;
      const int y = r.top + ((r.bottom - r.top) - s1.cy) / 2;
      ::SetTextColor(mem, theme_.Color(c.second_active ? kOnSurfaceVariant
                                                       : kOnSurface));
      ::TextOutW(mem, x, y, first.c_str(), static_cast<int>(first.size()));
      x += s1.cx + ssp.cx;
      ::SetTextColor(mem, theme_.Color(c.second_active ? kOnSurface
                                                       : kOnSurfaceVariant));
      ::TextOutW(mem, x, y, second.c_str(), static_cast<int>(second.size()));
    } else {
      ::SetTextColor(mem, theme_.Color(hot || down ? kOnSurface
                                                   : kOnSurfaceVariant));
      ::DrawTextW(mem, c.text.c_str(), -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
  }

  ::SelectObject(mem, oldf);
  ::BitBlt(hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
  ::SelectObject(mem, old_bmp);
  ::DeleteObject(bmp);
  ::DeleteDC(mem);
}

int StatusBar::HitCell(POINT pt) const {
  for (size_t i = 0; i < cells_.size(); ++i) {
    const RECT& r = cells_[i].rc;
    if (r.right <= r.left) continue;
    if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom)
      return static_cast<int>(i);
  }
  // 服務沒在跑時**整條**可點(§12.10.4 末段)。
  if (!service_ready_ && !cells_.empty()) return 0;
  return -1;
}

void StatusBar::ClickCell(int cell) {
  if (!service_ready_) {
    // §4.10 公式的第三段:一個**使用者做得到的動作**,
    // 不是「請聯絡開發者」。帶他到「進階」,那裡有「重新整理字詞」。
    if (settings_) settings_->OpenAt(3);
    return;
  }
  switch (cell) {
    case kCellMode: {
      // ⚠ 這一格是這一輪最重要的一顆鍵。在它之前,Windows 使用者
      //   **完全沒有**中英切換 —— ascii_mode 從來沒有被設定過。
      bool now;
      {
        std::lock_guard<std::mutex> lock(mu_);
        now = ascii_mode_;
        ascii_mode_ = !now;
      }
      if (engine_) engine_->SetAsciiModeAll(!now);
      Relayout();
      ::InvalidateRect(hwnd_, nullptr, TRUE);
      return;
    }
    case kCellVariant: {
      bool now;
      {
        std::lock_guard<std::mutex> lock(mu_);
        now = simplified_;
      }
      // 走設定視窗那一支,三條路(狀態列、系統匣、設定)共用同一份寫入 ——
      // 各寫一份會漂移,而漂移的症狀是「從這裡切有效、從那裡切無效」。
      if (settings_)
        settings_->SetVariantPref(now ? VariantPref::kTraditional
                                      : VariantPref::kSimplified);
      return;
    }
    case kCellSchema:
      OpenSchemaPopup();
      return;
    case kCellSettings:
      if (settings_) settings_->Open();
      return;
    default:
      return;
  }
}

// ── 方案清單:自繪的 top-level 小視窗 ──────────────────────────
//
// ⚠ **不是 TrackPopupMenu。** 它要先 SetForegroundWindow 才會在點外面時
//   正常關閉,而那就是搶焦點 —— 使用者正在打字的輸入框會失去插入點,
//   而「在句子中間改東西」正是這一橫存在的理由。用自己的小窗 + 滑鼠捕捉。

void StatusBar::OpenSchemaPopup() {
  ClosePopup();
  if (!engine_) return;
  popup_items_ = engine_->SchemaListCached();
  if (popup_items_.empty()) return;

  const int row_h = Dip(metric::kSidebarItemH, dpi_);
  const int w = Dip(200, dpi_);
  const int h = row_h * static_cast<int>(popup_items_.size()) +
                2 * Dip(space::s2, dpi_);
  RECT bar{};
  ::GetWindowRect(hwnd_, &bar);
  int x = bar.left + cells_[kCellSchema].rc.left;
  int y = bar.top - h - Dip(space::s2, dpi_);
  if (y < 0) y = bar.bottom + Dip(space::s2, dpi_);

  popup_ = ::CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kPopupClass, L"",
      WS_POPUP, x, y, w, h, hwnd_, nullptr, ::GetModuleHandleW(nullptr), this);
  if (!popup_) return;
  popup_hot_ = -1;
  ::ShowWindow(popup_, SW_SHOWNOACTIVATE);
  // 滑鼠捕捉:點到外面就關。SetForegroundWindow 那條路會搶焦點,不能用。
  ::SetCapture(popup_);
}

void StatusBar::ClosePopup() {
  if (!popup_) return;
  if (::GetCapture() == popup_) ::ReleaseCapture();
  ::DestroyWindow(popup_);
  popup_ = nullptr;
  popup_hot_ = -1;
}

void StatusBar::PaintPopup(HDC hdc) {
  RECT client{};
  ::GetClientRect(popup_, &client);
  HDC mem = ::CreateCompatibleDC(hdc);
  HBITMAP bmp = ::CreateCompatibleBitmap(hdc, client.right, client.bottom);
  HGDIOBJ old_bmp = ::SelectObject(mem, bmp);

  ::FillRect(mem, &client, theme_.Brush(kSurface));
  {
    HPEN pen = theme_.Pen(kOutline, Dip(kBarBorder, dpi_));
    HGDIOBJ oldp = ::SelectObject(mem, pen);
    HGDIOBJ oldb = ::SelectObject(mem, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(mem, client.left, client.top, client.right, client.bottom);
    ::SelectObject(mem, oldb);
    ::SelectObject(mem, oldp);
  }
  ::SetBkMode(mem, TRANSPARENT);
  HGDIOBJ oldf = ::SelectObject(mem, fonts_.Get(text_size::t3));

  const int row_h = Dip(metric::kSidebarItemH, dpi_);
  const int top = Dip(space::s2, dpi_);
  for (size_t i = 0; i < popup_items_.size(); ++i) {
    RECT r{Dip(space::s2, dpi_), top + static_cast<int>(i) * row_h,
           client.right - Dip(space::s2, dpi_),
           top + static_cast<int>(i + 1) * row_h};
    if (static_cast<int>(i) == popup_hot_)
      ::FillRect(mem, &r, theme_.Brush(kRowHover));
    ::SetTextColor(mem, theme_.Color(kOnSurface));
    RECT tr = r;
    tr.left += Dip(space::s4, dpi_);
    // ⚠ 只印名字,不印 id(§6.7 第一層)。名字為空才退回 id。
    const std::wstring name =
        Utf8ToWide(popup_items_[i].second.empty() ? popup_items_[i].first
                                                  : popup_items_[i].second);
    ::DrawTextW(mem, name.c_str(), -1, &tr,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
  }
  ::SelectObject(mem, oldf);
  ::BitBlt(hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
  ::SelectObject(mem, old_bmp);
  ::DeleteObject(bmp);
  ::DeleteDC(mem);
}

LRESULT CALLBACK StatusBar::PopupProc(HWND hwnd, UINT msg, WPARAM w,
                                      LPARAM l) {
  StatusBar* self =
      reinterpret_cast<StatusBar*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      break;
    }
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = ::BeginPaint(hwnd, &ps);
      if (self) self->PaintPopup(hdc);
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (!self) break;
      RECT c{};
      ::GetClientRect(hwnd, &c);
      const int y = GET_Y_LPARAM(l);
      const int row_h = Dip(metric::kSidebarItemH, self->dpi_);
      const int top = Dip(space::s2, self->dpi_);
      int hot = -1;
      if (GET_X_LPARAM(l) >= 0 && GET_X_LPARAM(l) < c.right && y >= top) {
        const int i = (y - top) / (row_h > 0 ? row_h : 1);
        if (i >= 0 && i < static_cast<int>(self->popup_items_.size())) hot = i;
      }
      if (hot != self->popup_hot_) {
        self->popup_hot_ = hot;
        ::InvalidateRect(hwnd, nullptr, TRUE);
      }
      return 0;
    }
    case WM_LBUTTONUP: {
      if (!self) break;
      POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      RECT c{};
      ::GetClientRect(hwnd, &c);
      const bool inside = pt.x >= 0 && pt.x < c.right && pt.y >= 0 &&
                          pt.y < c.bottom;
      const int pick = self->popup_hot_;
      const std::string id =
          (inside && pick >= 0 &&
           pick < static_cast<int>(self->popup_items_.size()))
              ? self->popup_items_[pick].first
              : std::string();
      self->ClosePopup();
      if (!id.empty() && self->engine_) self->engine_->SelectSchemaAll(id);
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (self && self->popup_) self->ClosePopup();
      return 0;
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, w, l);
}

// ─────────────────────────── 訊息 ───────────────────────────

LRESULT CALLBACK StatusBar::WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  StatusBar* self =
      reinterpret_cast<StatusBar*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      break;
    }
    // ⚠ 點它不可以啟動它:搶焦點會讓使用者正在打字的輸入框失去插入點 ——
    //   而「在句子中間切中英」正是這一橫存在的理由。
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = ::BeginPaint(hwnd, &ps);
      if (self) self->Paint(hdc);
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (!self) break;
      POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      if (self->dragging_) {
        // §12.10.3 的拖動:位移超過系統的拖動門檻才算拖,
        // 否則當作點擊那一格。⚠ SM_CXDRAG 是使用者可調的,不要寫死 4 px。
        const int dx = pt.x - self->drag_start_.x;
        const int dy = pt.y - self->drag_start_.y;
        if (!self->drag_moved_ &&
            (std::abs(dx) > ::GetSystemMetrics(SM_CXDRAG) ||
             std::abs(dy) > ::GetSystemMetrics(SM_CYDRAG)))
          self->drag_moved_ = true;
        if (self->drag_moved_) {
          POINT screen = pt;
          ::ClientToScreen(hwnd, &screen);
          RECT rc{};
          ::GetWindowRect(hwnd, &rc);
          ::SetWindowPos(hwnd, HWND_TOPMOST, screen.x - self->drag_start_.x,
                         screen.y - self->drag_start_.y, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;
      }
      const int hot = self->HitCell(pt);
      if (hot != self->hot_) {
        self->hot_ = hot;
        ::InvalidateRect(hwnd, nullptr, TRUE);
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        ::TrackMouseEvent(&tme);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      if (self && self->hot_ != -1) {
        self->hot_ = -1;
        ::InvalidateRect(hwnd, nullptr, TRUE);
      }
      return 0;
    case WM_LBUTTONDOWN: {
      if (!self) break;
      self->drag_start_ = POINT{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      self->dragging_ = true;
      self->drag_moved_ = false;
      self->pressed_ = self->HitCell(self->drag_start_);
      ::SetCapture(hwnd);
      ::InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }
    case WM_LBUTTONUP: {
      if (!self) break;
      const bool moved = self->drag_moved_;
      const int cell = self->pressed_;
      self->dragging_ = false;
      self->drag_moved_ = false;
      self->pressed_ = -1;
      if (::GetCapture() == hwnd) ::ReleaseCapture();
      ::InvalidateRect(hwnd, nullptr, TRUE);
      if (moved) {
        // 拖過就存位置,**不觸發那一格** —— 拖完順便切了中英,
        // 使用者會以為輸入法自己亂跳。
        self->SavePlacement();
      } else if (cell >= 0) {
        self->ClickCell(cell);
      }
      return 0;
    }
    case WM_DPICHANGED: {
      if (!self) break;
      self->dpi_ = HIWORD(w);
      self->fonts_.Reset(self->dpi_, Script::kHant);
      // 取建議矩形的**位置**,尺寸自己依新 DPI 重算 —— 它是固定 DIP
      // 尺寸的小窗,建議尺寸對它沒有意義。
      RECT* sug = reinterpret_cast<RECT*>(l);
      if (sug)
        ::SetWindowPos(hwnd, HWND_TOPMOST, sug->left, sug->top, 0, 0,
                       SWP_NOSIZE | SWP_NOACTIVATE);
      self->Relayout();
      ::InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }
    case WM_SETTINGCHANGE:
      if (self && (Theme::IsColorSetChange(l) || w == SPI_SETHIGHCONTRAST)) {
        self->RefreshTheme();
        // 工作區可能變了(工作列改大小)—— 重新夾一次位置。
        self->ApplyPlacement();
      }
      break;
    case WM_DISPLAYCHANGE:
      // ⚠ 螢幕拔掉了。不重新定位的話,那一橫會留在一個不存在的座標上,
      //   而症狀是「它不見了」。
      if (self) self->ApplyPlacement();
      return 0;
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, w, l);
}

}  // namespace rimewin
