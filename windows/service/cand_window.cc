#include "cand_window.h"

#include <string>

#include "../common/ime_policy.h"
#include "../winshared/winutil.h"

namespace rimewin {
namespace {

constexpr wchar_t kClassName[] = L"LuminaKeyCandidateWindow";
constexpr UINT WM_RIME_UPDATE = WM_APP + 1;
constexpr UINT WM_RIME_HIDE = WM_APP + 2;
constexpr UINT WM_RIME_QUIT = WM_APP + 3;
// 候選字級改了。單獨一則訊息而不是沿用 UPDATE:UPDATE 會把
// pending_ 讀進 shown_,而設定改的時候 pending_ 可能是空的。
constexpr UINT WM_RIME_RESTYLE = WM_APP + 4;

COLORREF ToColorRef(const Rgba& c) { return RGB(c.r, c.g, c.b); }

HFONT MakeFont(double px) {
  LOGFONTW lf{};
  // 字型家族刻意不寫死也不自己發明欄位:規範還沒有規定桌面候選窗用哪一個
  // 字型欄位。先用系統的 UI 字型,只套規範裡有的**字級**。
  // 等 macOS 端把桌面端的字型規範補上,這裡再照著讀。
  NONCLIENTMETRICSW ncm{};
  ncm.cbSize = sizeof(ncm);
  if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
    lf = ncm.lfMessageFont;
  lf.lfHeight = -static_cast<LONG>(px + 0.5);
  lf.lfWidth = 0;
  lf.lfQuality = CLEARTYPE_QUALITY;
  return ::CreateFontIndirectW(&lf);
}

}  // namespace

CandidateWindow::CandidateWindow() {
  style_.ResolveDefaults();
  ready_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

CandidateWindow::~CandidateWindow() {
  Stop();
  if (ready_) ::CloseHandle(ready_);
}

bool CandidateWindow::Start() {
  thread_ = ::CreateThread(nullptr, 0, &CandidateWindow::UiThreadEntry, this, 0, &thread_id_);
  if (!thread_) return false;
  // 等視窗真的建好。沒等的話,第一次 PostThreadMessage 可能在訊息佇列
  // 存在之前就發出去 —— 那則訊息會被安靜地丟掉,而症狀是「第一次組字
  // 沒有候選窗,第二次才有」。
  return ::WaitForSingleObject(ready_, 5000) == WAIT_OBJECT_0;
}

void CandidateWindow::Stop() {
  if (!thread_) return;
  ::PostThreadMessageW(thread_id_, WM_RIME_QUIT, 0, 0);
  ::WaitForSingleObject(thread_, 3000);
  ::CloseHandle(thread_);
  thread_ = nullptr;
}

void CandidateWindow::Update(const Snapshot& snap, const RECT& caret) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    pending_ = snap;
    pending_caret_ = caret;
  }
  if (thread_id_) ::PostThreadMessageW(thread_id_, WM_RIME_UPDATE, 0, 0);
}

void CandidateWindow::SetTextScale(double scale) {
  // 夾回一個講得通的範圍。值來自設定檔,而設定檔使用者改得到 ——
  // 0 會讓字型高度變成 0(整片空白),100 會讓候選窗比螢幕還大。
  if (!(scale > 0.2)) scale = 0.2;
  if (scale > 4.0) scale = 4.0;
  text_scale_.store(scale);
  // 立刻重畫:下一次有候選時才生效的話,使用者會以為這個選項沒有作用。
  if (thread_id_) ::PostThreadMessageW(thread_id_, WM_RIME_RESTYLE, 0, 0);
}

void CandidateWindow::Hide() {
  if (thread_id_) ::PostThreadMessageW(thread_id_, WM_RIME_HIDE, 0, 0);
}

DWORD WINAPI CandidateWindow::UiThreadEntry(LPVOID self) {
  static_cast<CandidateWindow*>(self)->UiThread();
  return 0;
}

void CandidateWindow::UiThread() {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &CandidateWindow::WndProc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.lpszClassName = kClassName;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  ::RegisterClassExW(&wc);

  hwnd_ = ::CreateWindowExW(
      // NOACTIVATE 是重點:候選窗絕不可以搶走宿主的焦點,
      // 搶走的話宿主會認為使用者離開了輸入框,組字當場被取消。
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
      kClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
      ::GetModuleHandleW(nullptr), this);
  ::SetEvent(ready_);
  if (!hwnd_) return;

  MSG msg;
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.hwnd == nullptr) {
      switch (msg.message) {
        case WM_RIME_UPDATE:
          Relayout();
          continue;
        case WM_RIME_RESTYLE:
          // 候選窗現在可能是隱藏的(沒有候選)。Relayout 對空清單
          // 會直接 Hide,所以這裡不必分情況。
          Relayout();
          continue;
        case WM_RIME_HIDE:
          ::ShowWindow(hwnd_, SW_HIDE);
          continue;
        case WM_RIME_QUIT:
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
}

LRESULT CALLBACK CandidateWindow::WndProc(HWND hwnd, UINT msg, WPARAM w,
                                          LPARAM l) {
  CandidateWindow* self =
      reinterpret_cast<CandidateWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      break;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = ::BeginPaint(hwnd, &ps);
      if (self) self->Paint(hdc);
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    // 點擊候選窗不可以啟動它 —— 見上面 WS_EX_NOACTIVATE 的說明。
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_ERASEBKGND:
      return 1;  // 全部在 WM_PAINT 裡畫,避免閃爍
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, w, l);
}

Extent CandidateWindow::MeasureWithFont(const std::string& utf8, double size) {
  Extent e;
  const std::wstring w = Utf8ToWide(utf8);
  HDC hdc = ::GetDC(hwnd_);
  if (!hdc) return e;
  HFONT font = MakeFont(size * dpi_scale_);
  HGDIOBJ old = ::SelectObject(hdc, font);
  SIZE sz{};
  ::GetTextExtentPoint32W(hdc, w.c_str(), static_cast<int>(w.size()), &sz);
  e.width = sz.cx;
  e.height = sz.cy;
  ::SelectObject(hdc, old);
  ::DeleteObject(font);
  ::ReleaseDC(hwnd_, hdc);
  return e;
}

void CandidateWindow::Relayout() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    shown_ = pending_;
    caret_ = pending_caret_;
  }
  if (shown_.items.empty()) {
    ::ShowWindow(hwnd_, SW_HIDE);
    return;
  }

  // 依候選窗**會出現的那個螢幕**取 DPI。多螢幕混合 DPI 時,拿主螢幕的
  // 值去算會讓候選窗在另一顆螢幕上大小完全不對。
  HMONITOR mon = ::MonitorFromPoint(POINT{caret_.left, caret_.bottom},
                                    MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  ::GetMonitorInfoW(mon, &mi);
  UINT dpi_x = 96, dpi_y = 96;
  {
    // GetDpiForMonitor 在 shcore.dll。動態載入,不為了一個函式多一個相依。
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static HMODULE shcore = ::LoadLibraryW(L"shcore.dll");
    // 經過 void* 再轉:直接把 FARPROC 轉成具體的函式型別是
    // -Wcast-function-type 會抓的寫法,而那個警告在別的地方是真的有意義的。
    static GetDpiForMonitorFn fn =
        shcore ? reinterpret_cast<GetDpiForMonitorFn>(reinterpret_cast<void*>(
                     ::GetProcAddress(shcore, "GetDpiForMonitor")))
               : nullptr;
    if (fn) fn(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dpi_x, &dpi_y);
  }
  dpi_scale_ = static_cast<double>(dpi_y) / 96.0;

  CandidateStyle scaled = style_;
  scaled.item.padding_h *= dpi_scale_;
  scaled.item.padding_v *= dpi_scale_;
  scaled.item.spacing *= dpi_scale_;
  scaled.window.padding *= dpi_scale_;
  scaled.window.border_width *= dpi_scale_;
  scaled.window.max_width *= dpi_scale_;
  scaled.metrics.spacing *= dpi_scale_;
  // 使用者選的候選字級。**排版與繪製要用同一個值**:量測用一個、
  // 畫用另一個的話,字會畫到框外面,而畫面看起來只是「有點擠」。
  const double ts = text_scale_.load();
  scaled.label.size *= ts;
  scaled.text.size *= ts;
  scaled.comment.size *= ts;

  layout_ = ComputeLayout(shown_.items, scaled, [this](const std::string& s,
                                                       double size) {
    return MeasureWithFont(s, size);
  });

  Rect work{static_cast<double>(mi.rcWork.left), static_cast<double>(mi.rcWork.top),
            static_cast<double>(mi.rcWork.right),
            static_cast<double>(mi.rcWork.bottom)};
  Rect caret{static_cast<double>(caret_.left), static_cast<double>(caret_.top),
             static_cast<double>(caret_.right), static_cast<double>(caret_.bottom)};
  const Rect where =
      PlaceWindow(caret, layout_.width, layout_.height, work, scaled);

  ::SetWindowPos(hwnd_, HWND_TOPMOST, static_cast<int>(where.left),
                 static_cast<int>(where.top), static_cast<int>(layout_.width),
                 static_cast<int>(layout_.height),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
  ::InvalidateRect(hwnd_, nullptr, TRUE);
  ::UpdateWindow(hwnd_);
}

void CandidateWindow::Paint(HDC hdc) {
  RECT client{};
  ::GetClientRect(hwnd_, &client);

  // 雙緩衝。直接畫在螢幕上的話,每次候選更新使用者都會看到一次閃爍,
  // 而候選是每一顆按鍵都會更新的東西。
  HDC mem = ::CreateCompatibleDC(hdc);
  HBITMAP bmp = ::CreateCompatibleBitmap(hdc, client.right, client.bottom);
  HGDIOBJ old_bmp = ::SelectObject(mem, bmp);

  HBRUSH bg = ::CreateSolidBrush(ToColorRef(style_.window.background));
  ::FillRect(mem, &client, bg);
  ::DeleteObject(bg);

  if (style_.window.border_width > 0) {
    HBRUSH border = ::CreateSolidBrush(ToColorRef(style_.window.border_color));
    ::FrameRect(mem, &client, border);
    ::DeleteObject(border);
  }

  ::SetBkMode(mem, TRANSPARENT);

  // 與 Relayout 用同一個倍率(見那裡的說明)。
  const double ts = text_scale_.load();
  HFONT f_label = MakeFont(style_.label.size * dpi_scale_ * ts);
  HFONT f_text = MakeFont(style_.text.size * dpi_scale_ * ts);
  HFONT f_comment = MakeFont(style_.comment.size * dpi_scale_ * ts);

  for (size_t i = 0; i < layout_.items.size(); ++i) {
    const ItemLayout& box = layout_.items[i];
    const bool hl = (static_cast<int32_t>(i) == shown_.highlighted);
    RECT r{static_cast<LONG>(box.x), static_cast<LONG>(box.y),
           static_cast<LONG>(box.x + box.w), static_cast<LONG>(box.y + box.h)};
    if (hl) {
      HBRUSH b = ::CreateSolidBrush(ToColorRef(style_.item.highlight_background));
      ::FillRect(mem, &r, b);
      ::DeleteObject(b);
    } else if (style_.item.background.a != 0) {
      HBRUSH b = ::CreateSolidBrush(ToColorRef(style_.item.background));
      ::FillRect(mem, &r, b);
      ::DeleteObject(b);
    }

    if (box.has_label) {
      ::SelectObject(mem, f_label);
      ::SetTextColor(mem, ToColorRef(hl ? style_.label.highlight_color
                                        : style_.label.color));
      const std::wstring s = Utf8ToWide(box.label_display);
      ::TextOutW(mem, static_cast<int>(box.x + box.label_x),
                 static_cast<int>(box.y + box.label_y), s.c_str(),
                 static_cast<int>(s.size()));
    }
    {
      ::SelectObject(mem, f_text);
      ::SetTextColor(mem,
                     ToColorRef(hl ? style_.text.highlight_color : style_.text.color));
      const std::wstring s = Utf8ToWide(shown_.items[i].text);
      ::TextOutW(mem, static_cast<int>(box.x + box.text_x),
                 static_cast<int>(box.y + box.text_y), s.c_str(),
                 static_cast<int>(s.size()));
    }
    if (box.has_comment) {
      ::SelectObject(mem, f_comment);
      ::SetTextColor(mem, ToColorRef(hl ? style_.comment.highlight_color
                                        : style_.comment.color));
      const std::wstring s = Utf8ToWide(shown_.items[i].comment);
      ::TextOutW(mem, static_cast<int>(box.x + box.comment_x),
                 static_cast<int>(box.y + box.comment_y), s.c_str(),
                 static_cast<int>(s.size()));
    }
  }

  ::BitBlt(hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);

  ::DeleteObject(f_label);
  ::DeleteObject(f_text);
  ::DeleteObject(f_comment);
  ::SelectObject(mem, old_bmp);
  ::DeleteObject(bmp);
  ::DeleteDC(mem);
}

}  // namespace rimewin
