#include "ui_confirm.h"

#include "../common/ui_dip.h"
#include "../common/ui_layout.h"

namespace rimewin {
namespace {

constexpr wchar_t kClass[] = L"LuminaKeyConfirmDialog";
enum : int { kIdConfirm = 1, kIdCancel = 2 };

struct DlgState {
  Theme* theme = nullptr;
  Script script = Script::kHant;
  FontSet fonts;
  UINT dpi = 96;
  bool result = false;
  bool done = false;
  HWND label = nullptr;
  HWND confirm = nullptr;
  HWND cancel = nullptr;
  std::wstring body;
  bool two_buttons = true;
};

// 版面(DIP)。內容欄的下界沿用 §12.4.2 的 440,好讓對話框與設定視窗
// 的行寬一致 —— 兩個表面的文字寬度差很多的話,看起來像兩個程式。
constexpr int kDlgW = 420;
constexpr int kPad = space::s8;      // 32
constexpr int kBtnH = metric::kMinTarget + space::s2;  // 32
constexpr int kBtnMinW = 96;
constexpr int kBtnGap = space::s3;   // 6

void Relayout(HWND hwnd, DlgState* st) {
  RECT rc{};
  ::GetClientRect(hwnd, &rc);
  const int W = rc.right - rc.left;
  const int H = rc.bottom - rc.top;
  const int pad = Dip(kPad, st->dpi);
  const int bh = Dip(kBtnH, st->dpi);
  const int bw = Dip(kBtnMinW, st->dpi);
  const int gap = Dip(kBtnGap, st->dpi);

  if (st->label)
    ::SetWindowPos(st->label, nullptr, pad, pad, W - 2 * pad,
                   H - 2 * pad - bh - Dip(space::s7, st->dpi), SWP_NOZORDER);

  int x = W - pad - bw;
  if (st->cancel) {
    ::SetWindowPos(st->cancel, nullptr, x, H - pad - bh, bw, bh, SWP_NOZORDER);
    x -= bw + gap;
  }
  if (st->confirm)
    ::SetWindowPos(st->confirm, nullptr, x, H - pad - bh, bw, bh, SWP_NOZORDER);
}

LRESULT CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  DlgState* st =
      reinterpret_cast<DlgState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      break;
    }
    case WM_ERASEBKGND: {
      if (!st) break;
      RECT rc{};
      ::GetClientRect(hwnd, &rc);
      ::FillRect(reinterpret_cast<HDC>(w), &rc, st->theme->Brush(kSurface));
      return 1;
    }
    // 唯讀的 STATIC 與 EDIT 都送這一則(不是 WM_CTLCOLOREDIT)。
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
      if (!st) break;
      HDC hdc = reinterpret_cast<HDC>(w);
      ::SetTextColor(hdc, st->theme->Color(kOnSurface));
      ::SetBkColor(hdc, st->theme->Color(kSurface));
      return reinterpret_cast<LRESULT>(st->theme->Brush(kSurface));
    }
    case WM_COMMAND: {
      if (!st) break;
      const int id = LOWORD(w);
      if (id == kIdConfirm) {
        st->result = true;
        st->done = true;
      } else if (id == kIdCancel || id == IDCANCEL) {
        st->result = false;
        st->done = true;
      }
      return 0;
    }
    case WM_CLOSE:
      // 關掉視窗 = 取消。**不可以**等於確認 —— 一個破壞性動作
      // 不該因為使用者按了右上角的 X 就發生。
      if (st) {
        st->result = false;
        st->done = true;
      }
      return 0;
    case WM_SIZE:
      if (st) Relayout(hwnd, st);
      return 0;
    case WM_DPICHANGED: {
      if (!st) break;
      st->dpi = HIWORD(w);
      st->fonts.Reset(st->dpi, st->script);
      RECT* sug = reinterpret_cast<RECT*>(l);
      if (sug)
        ::SetWindowPos(hwnd, nullptr, sug->left, sug->top,
                       sug->right - sug->left, sug->bottom - sug->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
      HFONT f = st->fonts.Get(text_size::t4);
      for (HWND h : {st->label, st->confirm, st->cancel})
        if (h) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
      Relayout(hwnd, st);
      ::RedrawWindow(hwnd, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
      return 0;
    }
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, w, l);
}

void EnsureClass() {
  static bool done = false;
  if (done) return;
  done = true;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &DlgProc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  // ⚠ hbrBackground 留 nullptr:底由 WM_ERASEBKGND 自己畫。
  //   設 COLOR_BTNFACE+1 的話,深色模式下會先閃一片淺灰。
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kClass;
  ::RegisterClassExW(&wc);
}

bool RunDialog(HWND parent, Theme* theme, Script script,
               const std::wstring& title, const std::wstring& body,
               const std::wstring& confirm_text,
               const std::wstring& cancel_text, bool two_buttons) {
  EnsureClass();
  DlgState st;
  st.theme = theme;
  st.script = script;
  st.two_buttons = two_buttons;
  st.body = body;

  UINT dpi = 96;
  {
    using GetDpiFn = UINT(WINAPI*)(HWND);
    HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
    GetDpiFn fn = u32 ? reinterpret_cast<GetDpiFn>(reinterpret_cast<void*>(
                            ::GetProcAddress(u32, "GetDpiForWindow")))
                      : nullptr;
    if (fn && parent) dpi = fn(parent);
    if (!dpi) dpi = 96;
  }
  st.dpi = dpi;
  st.fonts.Reset(dpi, script);

  // 高度依內文行數估。⚠ 不做精確量測是刻意的:量錯的後果是多一點空白,
  //   而做精確量測要在建視窗之前先有 HDC 與字型,那條路更容易錯。
  int lines = 2;
  for (wchar_t c : body)
    if (c == L'\n') ++lines;
  lines += static_cast<int>(body.size() / 34);
  if (lines > 12) lines = 12;
  const int h_dip = kPad * 2 + kBtnH + space::s7 + lines * (text_size::t5 + 6);

  const int w_px = Dip(kDlgW, dpi);
  const int h_px = Dip(h_dip, dpi);

  RECT pr{};
  if (parent)
    ::GetWindowRect(parent, &pr);
  else
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &pr, 0);
  const int x = pr.left + ((pr.right - pr.left) - w_px) / 2;
  const int y = pr.top + ((pr.bottom - pr.top) - h_px) / 2;

  HWND hwnd = ::CreateWindowExW(
      WS_EX_DLGMODALFRAME, kClass, title.c_str(),
      WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w_px, h_px, parent, nullptr,
      ::GetModuleHandleW(nullptr), &st);
  if (!hwnd) return false;
  theme->ApplyTitleBar(hwnd);

  HINSTANCE inst = ::GetModuleHandleW(nullptr);
  st.label = ::CreateWindowExW(0, L"STATIC", body.c_str(),
                               WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10,
                               hwnd, nullptr, inst, nullptr);
  if (two_buttons)
    st.confirm = ::CreateWindowExW(
        0, L"BUTTON", confirm_text.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdConfirm)), inst,
        nullptr);
  st.cancel = ::CreateWindowExW(
      0, L"BUTTON", cancel_text.c_str(),
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 10, 10,
      hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCancel)), inst,
      nullptr);

  HFONT f = st.fonts.Get(text_size::t4);
  for (HWND h : {st.label, st.confirm, st.cancel})
    if (h) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);

  Relayout(hwnd, &st);
  if (parent) ::EnableWindow(parent, FALSE);
  ::ShowWindow(hwnd, SW_SHOW);
  // ⚠ §2-C4:預設焦點在**取消**。
  ::SetFocus(st.cancel);

  MSG msg;
  while (!st.done && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!::IsDialogMessageW(hwnd, &msg)) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
  }

  if (parent) ::EnableWindow(parent, TRUE);
  ::DestroyWindow(hwnd);
  // 父視窗要拿回焦點。少了這一步,對話框關掉之後鍵盤焦點會落在
  // 桌面上 —— 使用者按 Tab 沒有反應,而畫面看起來完全正常。
  if (parent) ::SetActiveWindow(parent);
  st.fonts.Clear();
  return st.result;
}

}  // namespace

bool ConfirmDialog(HWND parent, Theme* theme, Script script,
                   const std::wstring& title, const std::wstring& body,
                   const std::wstring& confirm_text,
                   const std::wstring& cancel_text) {
  return RunDialog(parent, theme, script, title, body, confirm_text,
                   cancel_text, true);
}

void MessageDialog(HWND parent, Theme* theme, Script script,
                   const std::wstring& title, const std::wstring& body,
                   const std::wstring& dismiss_text) {
  RunDialog(parent, theme, script, title, body, std::wstring(), dismiss_text,
            false);
}

}  // namespace rimewin
