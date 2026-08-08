#include "settings_window.h"

#include <commctrl.h>
#include <shellapi.h>

#include <cstdio>

#include "../common/schema_choice.h"
#include "../common/schema_list_patch.h"
#include "../winshared/winutil.h"
#include "cand_window.h"

namespace rimewin {
namespace {

constexpr wchar_t kClass[] = L"RimeQuadSettingsWindow";
constexpr UINT WM_RIME_OPEN = WM_APP + 1;
constexpr UINT WM_RIME_TRAY = WM_APP + 2;
constexpr UINT kTrayId = 1;
enum : int { IDM_TRAY_SETTINGS = 900, IDM_TRAY_REDEPLOY, IDM_TRAY_QUIT };
constexpr UINT_PTR kDeployTimer = 1;

// 控制項 id。
enum : int {
  IDC_TAB = 100,
  // 方案
  IDC_SCHEMA_LIST = 200,
  IDC_UP,
  IDC_DOWN,
  IDC_APPLY_ORDER,
  IDC_DEFAULT_COMBO,
  IDC_LBL_ORDER,
  IDC_LBL_DEFAULT,
  IDC_LBL_PRIORITY,
  // 文字
  IDC_LBL_VARIANT = 300,
  IDC_VARIANT_COMBO,
  IDC_LBL_PUNCT,
  IDC_PUNCT_COMBO,
  IDC_LBL_COUNT,
  IDC_COUNT_COMBO,
  IDC_LBL_SCALE,
  IDC_SCALE_COMBO,
  IDC_LBL_TEXT_NOTE,
  // 進階
  IDC_REDEPLOY = 400,
  IDC_OPEN_USER_DIR,
  IDC_OPEN_SETTINGS_FILE,
  IDC_LBL_PATHS,
  IDC_LBL_ABOUT,
  // 共用
  IDC_CLOSE = 500,
  IDC_STATUS,
};

// 每一頁上有哪些控制項。切分頁時整批 Show/Hide ——
// 用 z-order 疊在一起的話,鍵盤 Tab 會走到看不見的控制項上,
// 那是「摸得到但看不到」,同一族的問題。
const int kTabPage0[] = {IDC_LBL_ORDER,   IDC_SCHEMA_LIST, IDC_UP,
                         IDC_DOWN,        IDC_APPLY_ORDER, IDC_LBL_DEFAULT,
                         IDC_DEFAULT_COMBO, IDC_LBL_PRIORITY};
const int kTabPage1[] = {IDC_LBL_VARIANT, IDC_VARIANT_COMBO, IDC_LBL_PUNCT,
                         IDC_PUNCT_COMBO, IDC_LBL_COUNT,     IDC_COUNT_COMBO,
                         IDC_LBL_SCALE,   IDC_SCALE_COMBO,   IDC_LBL_TEXT_NOTE};
const int kTabPage2[] = {IDC_REDEPLOY, IDC_OPEN_USER_DIR, IDC_OPEN_SETTINGS_FILE,
                         IDC_LBL_PATHS, IDC_LBL_ABOUT};

struct PageDef {
  const int* ids;
  int count;
};
const PageDef kPages[] = {
    {kTabPage0, static_cast<int>(sizeof(kTabPage0) / sizeof(int))},
    {kTabPage1, static_cast<int>(sizeof(kTabPage1) / sizeof(int))},
    {kTabPage2, static_cast<int>(sizeof(kTabPage2) / sizeof(int))},
};

// 字形下拉的順序。索引 0 一律是「跟隨」——
// 與 settings.h 的規則一致:第一格是「沒設過」。
const Variant kVariantOrder[] = {Variant::kFollow, Variant::kHant, Variant::kHans,
                                 Variant::kHantHK, Variant::kHantTW};
const wchar_t* const kVariantLabels[] = {L"跟隨語言設定檔", L"傳統漢字",
                                         L"简化字", L"香港字形", L"臺灣字形"};
constexpr int kVariantCount = 5;

const wchar_t* const kPunctLabels[] = {L"跟隨方案", L"全形(。,)", L"半形(. ,)"};
const wchar_t* const kCountLabels[] = {L"跟隨方案", L"3 個", L"5 個", L"7 個",
                                       L"9 個"};
const wchar_t* const kScaleLabels[] = {L"跟隨預設", L"小", L"標準", L"大",
                                       L"很大"};

HWND Ctl(HWND parent, int id) { return ::GetDlgItem(parent, id); }

void SetText(HWND parent, int id, const wchar_t* text) {
  HWND h = Ctl(parent, id);
  if (h) ::SetWindowTextW(h, text);
}

void FillCombo(HWND combo, const wchar_t* const* items, int n, int select) {
  if (!combo) return;
  ::SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  for (int i = 0; i < n; ++i)
    ::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(items[i]));
  ::SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(select), 0);
}

int ComboSel(HWND parent, int id) {
  HWND h = Ctl(parent, id);
  if (!h) return 0;
  const LRESULT r = ::SendMessageW(h, CB_GETCURSEL, 0, 0);
  return r == CB_ERR ? 0 : static_cast<int>(r);
}

}  // namespace

SettingsWindow::SettingsWindow(Engine* engine, SettingsStore* store,
                               const std::string& shared_dir)
    : engine_(engine), store_(store), shared_dir_(shared_dir) {}

SettingsWindow::~SettingsWindow() { Stop(); }

DWORD WINAPI SettingsWindow::ThreadEntry(LPVOID self) {
  static_cast<SettingsWindow*>(self)->ThreadMain();
  return 0;
}

bool SettingsWindow::Start() {
  if (thread_) return true;
  ready_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  // 不用 lambda 轉函式指標:LPTHREAD_START_ROUTINE 是 __stdcall,
  // 而 captureless lambda 轉出來的是 __cdecl。x64 上兩者一樣,
  // arm64 上就不是了 —— 而 arm64 是下一個要做的架構。
  thread_ = ::CreateThread(nullptr, 0, &SettingsWindow::ThreadEntry, this, 0,
                           &thread_id_);
  if (!thread_) return false;
  if (ready_) ::WaitForSingleObject(ready_, 5000);
  return hwnd_ != nullptr;
}

void SettingsWindow::Stop() {
  if (thread_id_) ::PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
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

void SettingsWindow::Open() {
  // 從別的執行緒呼叫。**不可以**在這裡直接碰視窗:Win32 的視窗屬於
  // 建立它的執行緒,跨執行緒 ShowWindow 會有各種難查的行為。
  if (hwnd_) ::PostMessageW(hwnd_, WM_RIME_OPEN, 0, 0);
}

void SettingsWindow::ThreadMain() {
  INITCOMMONCONTROLSEX icc{};
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_TAB_CLASSES | ICC_STANDARD_CLASSES;
  ::InitCommonControlsEx(&icc);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &SettingsWindow::WndProc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  wc.lpszClassName = kClass;
  ::RegisterClassExW(&wc);

  // 一開始就建好但不顯示。使用者按下語言列按鈕時要**立刻**看到窗,
  // 而建視窗 + 讀設定 + 問方案清單要花一點時間 ——
  // 「按了之後過一秒才出現」與「按了沒反應」在使用者眼裡是同一件事。
  hwnd_ = ::CreateWindowExW(0, kClass, L"RIME 四端輸入法 設定",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr,
                            nullptr, wc.hInstance, this);
  if (ready_) ::SetEvent(ready_);
  if (!hwnd_) return;

  MSG msg;
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    // IsDialogMessage 讓 Tab / 方向鍵 / Esc 在控制項之間正常運作。
    // 少了它,使用者只能用滑鼠 —— 而那對只用鍵盤的人等於整個介面不存在。
    if (!::IsDialogMessageW(hwnd_, &msg)) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
  }
  if (font_) ::DeleteObject(font_);
  hwnd_ = nullptr;
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM w,
                                         LPARAM l) {
  SettingsWindow* self =
      reinterpret_cast<SettingsWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE: {
      CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(l);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      break;
    }
    case WM_CREATE:
      if (self) {
        self->CreateUi(hwnd);
        self->AddTray();
      }
      return 0;
    case WM_RIME_TRAY:
      if (self) self->OnTray(w, l);
      return 0;
    case WM_RIME_OPEN:
      if (self) {
        self->ReloadFromSettings();
        ::ShowWindow(hwnd, SW_SHOW);
        // 已經開著時只是帶到最前面。再開一個視窗是「按了沒反應」
        // 的另一種長相 —— 使用者以為沒開,其實開在別的地方。
        ::SetForegroundWindow(hwnd);
        ::SetActiveWindow(hwnd);
      }
      return 0;
    case WM_COMMAND:
      if (self) self->OnCommand(LOWORD(w), HIWORD(w));
      return 0;
    case WM_NOTIFY: {
      NMHDR* nm = reinterpret_cast<NMHDR*>(l);
      if (self && nm && nm->idFrom == IDC_TAB && nm->code == TCN_SELCHANGE)
        self->ShowTab(static_cast<int>(
            ::SendMessageW(nm->hwndFrom, TCM_GETCURSEL, 0, 0)));
      return 0;
    }
    case WM_TIMER:
      if (self && w == kDeployTimer) self->OnDeployTick();
      return 0;
    case WM_CLOSE:
      // 關閉 = 隱藏。服務進程還要繼續跑,而重建視窗會讓下一次開啟變慢。
      ::ShowWindow(hwnd, SW_HIDE);
      return 0;
    case WM_DESTROY:
      if (self) self->RemoveTray();
      ::PostQuitMessage(0);
      return 0;
    default:
      // explorer 重啟之後系統匣清空,它會廣播 "TaskbarCreated"。
      if (self && self->taskbar_created_ != 0 && msg == self->taskbar_created_) {
        self->tray_added_ = false;
        self->AddTray();
        return 0;
      }
      break;
  }
  return ::DefWindowProcW(hwnd, msg, w, l);
}

// ─────────────────────────── 系統匣 ───────────────────────────

void SettingsWindow::AddTray() {
  if (tray_added_ || !hwnd_) return;
  if (taskbar_created_ == 0)
    taskbar_created_ = ::RegisterWindowMessageW(L"TaskbarCreated");
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd_;
  nid.uID = kTrayId;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = WM_RIME_TRAY;
  // ⚠ 沒有自己的圖示檔。用系統的應用程式圖示 —— 醜,但**看得到**,
  //   而看不到的圖示等於沒有這個入口。要換成自己的圖示得加一份 .rc,
  //   那會把資源編譯器拉進建置,留給有美術資源的時候再做。
  nid.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
  ::lstrcpynW(nid.szTip, L"RIME 四端輸入法", 128);
  tray_added_ = ::Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

void SettingsWindow::RemoveTray() {
  if (!tray_added_) return;
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd_;
  nid.uID = kTrayId;
  ::Shell_NotifyIconW(NIM_DELETE, &nid);
  tray_added_ = false;
}

void SettingsWindow::OnTray(WPARAM /*w*/, LPARAM l) {
  const UINT ev = static_cast<UINT>(LOWORD(l));
  if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK) {
    Open();
    return;
  }
  if (ev != WM_RBUTTONUP && ev != WM_CONTEXTMENU) return;

  HMENU menu = ::CreatePopupMenu();
  if (!menu) return;
  ::AppendMenuW(menu, MF_STRING, IDM_TRAY_SETTINGS, L"設定(&S)…");
  ::AppendMenuW(menu, MF_STRING, IDM_TRAY_REDEPLOY, L"重新整理字詞(&R)");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, IDM_TRAY_QUIT, L"結束輸入法服務(&X)");
  POINT pt{};
  ::GetCursorPos(&pt);
  // SetForegroundWindow 是必要的:少了它,使用者點到選單外面時選單不會關,
  // 會一直留在畫面上(這是 Shell_NotifyIcon 的既知行為)。
  ::SetForegroundWindow(hwnd_);
  const int cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x,
                                   pt.y, 0, hwnd_, nullptr);
  ::DestroyMenu(menu);
  ::PostMessageW(hwnd_, WM_NULL, 0, 0);

  if (cmd == IDM_TRAY_SETTINGS) {
    Open();
  } else if (cmd == IDM_TRAY_REDEPLOY) {
    // 先把視窗叫出來:進度與結果都顯示在它上面,不然按下去真的會
    // 「什麼都沒發生」——而部署要十幾秒。
    //
    // ⚠ 這裡用 SendMessage 而不是 Open()。Open() 是 PostMessage(給別的
    //   執行緒用的),而我們**已經在** UI 執行緒上 —— 用它的話
    //   WM_RIME_OPEN 會排在 StartRedeploy **之後**才處理,
    //   而它結尾的 ReloadFromSettings 會把「正在整理字詞…」那行狀態擦掉。
    //   使用者按下去看到的是一片空白,直到 200ms 後計時器把它寫回來。
    ::SendMessageW(hwnd_, WM_RIME_OPEN, 0, 0);
    ShowTab(2);
    ::SendMessageW(Ctl(hwnd_, IDC_TAB), TCM_SETCURSEL, 2, 0);
    StartRedeploy(L"重新整理字詞");
  } else if (cmd == IDM_TRAY_QUIT) {
    if (::MessageBoxW(hwnd_,
                      L"結束之後,下一次打字時服務會自動再啟動。\r\n"
                      L"要現在結束嗎?",
                      L"結束輸入法服務", MB_YESNO | MB_ICONQUESTION) != IDYES)
      return;
    // ⚠ 不可以直接結束進程:這支進程持有使用者詞庫的 LevelDB,
    //   從中途拔掉詞庫會壞,而症狀是「我學過的詞全沒了」。
    //   送具名事件,由 main.cc 那條路正常收尾。
    HANDLE ev2 = ::OpenEventW(EVENT_MODIFY_STATE, FALSE,
                              RimeServiceQuitEventName().c_str());
    if (ev2) {
      ::SetEvent(ev2);
      ::CloseHandle(ev2);
    }
  }
}

// ─────────────────────────── 建立控制項 ───────────────────────────

void SettingsWindow::CreateUi(HWND hwnd) {
  hwnd_ = hwnd;
  {
    UINT dpi = 96;
    using GetDpiFn = UINT(WINAPI*)(HWND);
    HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
    GetDpiFn fn = u32 ? reinterpret_cast<GetDpiFn>(reinterpret_cast<void*>(
                            ::GetProcAddress(u32, "GetDpiForWindow")))
                      : nullptr;
    if (fn) dpi = fn(hwnd);
    if (dpi == 0) dpi = 96;
    dpi_scale_ = dpi / 96.0;
  }

  // 系統的 UI 字型。**不要用 DEFAULT_GUI_FONT** —— 那是 System 字型,
  // 在中文系統上畫出來是點陣的,而且不一定有我們要顯示的字。
  NONCLIENTMETRICSW ncm{};
  ncm.cbSize = sizeof(ncm);
  if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = static_cast<LONG>(lf.lfHeight * dpi_scale_);
    font_ = ::CreateFontIndirectW(&lf);
  }

  HINSTANCE inst = ::GetModuleHandleW(nullptr);
  auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    HWND h = ::CreateWindowExW(0, cls, text, WS_CHILD | style, 0, 0, 10, 10,
                               hwnd, reinterpret_cast<HMENU>(
                                         static_cast<INT_PTR>(id)),
                               inst, nullptr);
    if (h && font_)
      ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return h;
  };

  HWND tab = ::CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE,
                               0, 0, 10, 10, hwnd,
                               reinterpret_cast<HMENU>(
                                   static_cast<INT_PTR>(IDC_TAB)),
                               inst, nullptr);
  if (tab && font_)
    ::SendMessageW(tab, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
  const wchar_t* names[] = {L"方案", L"文字", L"進階"};
  for (int i = 0; i < 3; ++i) {
    TCITEMW it{};
    it.mask = TCIF_TEXT;
    it.pszText = const_cast<wchar_t*>(names[i]);
    ::SendMessageW(tab, TCM_INSERTITEMW, static_cast<WPARAM>(i),
                   reinterpret_cast<LPARAM>(&it));
  }

  // ── 方案 ──
  mk(L"STATIC", L"方案順序(第一項是預設;改完要按「套用順序」)", SS_LEFT,
     IDC_LBL_ORDER);
  mk(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP,
     IDC_SCHEMA_LIST);
  mk(L"BUTTON", L"上移", BS_PUSHBUTTON | WS_TABSTOP, IDC_UP);
  mk(L"BUTTON", L"下移", BS_PUSHBUTTON | WS_TABSTOP, IDC_DOWN);
  mk(L"BUTTON", L"套用順序", BS_PUSHBUTTON | WS_TABSTOP, IDC_APPLY_ORDER);
  mk(L"STATIC", L"預設方案", SS_LEFT, IDC_LBL_DEFAULT);
  mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
     IDC_DEFAULT_COMBO);
  mk(L"STATIC",
     L"「跟隨語言設定檔」的意思是:從繁體中文那一份進來就用繁體方案,\r\n"
     L"從簡體中文那一份進來就用簡體方案。在這裡選了具體的方案之後,\r\n"
     L"它對每一個語言都成立 —— 選它就是不要我們替你猜。\r\n"
     L"用 Ctrl+` 或 F4 臨時換方案不會被這裡蓋掉,而且會被記住。",
     SS_LEFT, IDC_LBL_PRIORITY);

  // ── 文字 ──
  mk(L"STATIC", L"字形(簡繁)", SS_LEFT, IDC_LBL_VARIANT);
  mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
     IDC_VARIANT_COMBO);
  mk(L"STATIC", L"標點", SS_LEFT, IDC_LBL_PUNCT);
  mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
     IDC_PUNCT_COMBO);
  mk(L"STATIC", L"一次顯示幾個候選", SS_LEFT, IDC_LBL_COUNT);
  mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
     IDC_COUNT_COMBO);
  mk(L"STATIC", L"候選字大小", SS_LEFT, IDC_LBL_SCALE);
  mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
     IDC_SCALE_COMBO);
  mk(L"STATIC",
     L"字形與標點改了立刻生效。候選個數改的是引擎的 menu/page_size,\r\n"
     L"要重新整理字詞之後才會變 —— 按下去時會問你。",
     SS_LEFT, IDC_LBL_TEXT_NOTE);

  // ── 進階 ──
  mk(L"BUTTON", L"重新整理字詞", BS_PUSHBUTTON | WS_TABSTOP, IDC_REDEPLOY);
  mk(L"BUTTON", L"開啟使用者資料夾", BS_PUSHBUTTON | WS_TABSTOP,
     IDC_OPEN_USER_DIR);
  mk(L"BUTTON", L"開啟設定檔", BS_PUSHBUTTON | WS_TABSTOP,
     IDC_OPEN_SETTINGS_FILE);
  mk(L"STATIC", L"", SS_LEFT, IDC_LBL_PATHS);
  mk(L"STATIC", L"", SS_LEFT, IDC_LBL_ABOUT);

  mk(L"BUTTON", L"關閉", BS_PUSHBUTTON | WS_TABSTOP, IDC_CLOSE);
  mk(L"STATIC", L"", SS_LEFT, IDC_STATUS);
  ::ShowWindow(Ctl(hwnd, IDC_CLOSE), SW_SHOW);
  ::ShowWindow(Ctl(hwnd, IDC_STATUS), SW_SHOW);

  const int w = static_cast<int>(660 * dpi_scale_);
  const int h = static_cast<int>(470 * dpi_scale_);
  ::SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER);
  LayoutUi();
  ShowTab(0);
  // ⚠ 這裡**不**去問引擎要方案清單。CreateUi 跑在 WM_CREATE 裡,
  //   而 Engine::SchemaList 會同步等引擎執行緒 —— 服務剛啟動時那條執行緒
  //   可能正在做首次部署前的準備。真的卡住的話 Start() 會逾時,
  //   而症狀是「設定視窗有時候叫不出來」,間歇性又難查。
  //   內容由 WM_RIME_OPEN(真的要顯示的時候)才載入。
}

void SettingsWindow::LayoutUi() {
  if (!hwnd_) return;
  RECT rc{};
  ::GetClientRect(hwnd_, &rc);
  auto px = [&](int v) { return static_cast<int>(v * dpi_scale_); };
  const int pad = px(10);
  const int W = rc.right - rc.left;
  const int H = rc.bottom - rc.top;
  const int bottom_h = px(34);
  const int tab_h = H - pad * 2 - bottom_h;

  ::SetWindowPos(Ctl(hwnd_, IDC_TAB), nullptr, pad, pad, W - pad * 2, tab_h,
                 SWP_NOZORDER);
  // 分頁內容區:tab 控制項底下往內縮。
  const int x0 = pad + px(12);
  const int y0 = pad + px(34);
  const int cw = W - pad * 2 - px(24);

  auto place = [&](int id, int x, int y, int w, int h) {
    HWND c = Ctl(hwnd_, id);
    if (c) ::SetWindowPos(c, nullptr, x, y, w, h, SWP_NOZORDER);
  };

  // 方案
  place(IDC_LBL_ORDER, x0, y0, cw, px(18));
  place(IDC_SCHEMA_LIST, x0, y0 + px(22), cw - px(110), px(150));
  place(IDC_UP, x0 + cw - px(100), y0 + px(22), px(100), px(28));
  place(IDC_DOWN, x0 + cw - px(100), y0 + px(56), px(100), px(28));
  place(IDC_APPLY_ORDER, x0 + cw - px(100), y0 + px(144), px(100), px(28));
  place(IDC_LBL_DEFAULT, x0, y0 + px(184), px(90), px(20));
  place(IDC_DEFAULT_COMBO, x0 + px(96), y0 + px(180), cw - px(96), px(240));
  place(IDC_LBL_PRIORITY, x0, y0 + px(216), cw, px(90));

  // 文字
  const int rows[] = {0, 34, 68, 102};
  place(IDC_LBL_VARIANT, x0, y0 + px(rows[0] + 4), px(140), px(20));
  place(IDC_VARIANT_COMBO, x0 + px(150), y0 + px(rows[0]), px(230), px(240));
  place(IDC_LBL_PUNCT, x0, y0 + px(rows[1] + 4), px(140), px(20));
  place(IDC_PUNCT_COMBO, x0 + px(150), y0 + px(rows[1]), px(230), px(240));
  place(IDC_LBL_COUNT, x0, y0 + px(rows[2] + 4), px(140), px(20));
  place(IDC_COUNT_COMBO, x0 + px(150), y0 + px(rows[2]), px(230), px(240));
  place(IDC_LBL_SCALE, x0, y0 + px(rows[3] + 4), px(140), px(20));
  place(IDC_SCALE_COMBO, x0 + px(150), y0 + px(rows[3]), px(230), px(240));
  place(IDC_LBL_TEXT_NOTE, x0, y0 + px(150), cw, px(60));

  // 進階
  place(IDC_REDEPLOY, x0, y0, px(180), px(30));
  place(IDC_OPEN_USER_DIR, x0 + px(190), y0, px(180), px(30));
  place(IDC_OPEN_SETTINGS_FILE, x0 + px(380), y0, px(150), px(30));
  place(IDC_LBL_PATHS, x0, y0 + px(44), cw, px(110));
  place(IDC_LBL_ABOUT, x0, y0 + px(160), cw, px(120));

  // 共用
  place(IDC_STATUS, pad, H - pad - px(26), W - pad * 2 - px(110), px(22));
  place(IDC_CLOSE, W - pad - px(100), H - pad - px(30), px(100), px(28));
}

void SettingsWindow::ShowTab(int index) {
  if (index < 0 || index > 2) index = 0;
  tab_ = index;
  for (int p = 0; p < 3; ++p)
    for (int i = 0; i < kPages[p].count; ++i)
      ::ShowWindow(Ctl(hwnd_, kPages[p].ids[i]), p == index ? SW_SHOW : SW_HIDE);
}

// ─────────────────────────── 讀 / 寫 ───────────────────────────

void SettingsWindow::ReloadSchemaList() {
  schemas_ = engine_->SchemaList();
  order_.clear();
  for (const auto& kv : schemas_) order_.push_back(kv.first);

  HWND lb = Ctl(hwnd_, IDC_SCHEMA_LIST);
  if (lb) {
    ::SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    for (const auto& kv : schemas_) {
      const std::wstring line =
          Utf8ToWide(kv.second.empty() ? kv.first : kv.second) + L"  (" +
          Utf8ToWide(kv.first) + L")";
      ::SendMessageW(lb, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    }
    ::SendMessageW(lb, LB_SETCURSEL, 0, 0);
  }

  HWND cb = Ctl(hwnd_, IDC_DEFAULT_COMBO);
  if (cb) {
    ::SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    ::SendMessageW(cb, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(L"跟隨語言設定檔(建議)"));
    for (const auto& kv : schemas_) {
      const std::wstring name = Utf8ToWide(kv.second.empty() ? kv.first : kv.second);
      ::SendMessageW(cb, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(name.c_str()));
    }
    const std::string forced = settings_.Raw(keys::kSchemaForced);
    int sel = 0;
    for (size_t i = 0; i < schemas_.size(); ++i)
      if (schemas_[i].first == forced) sel = static_cast<int>(i) + 1;
    ::SendMessageW(cb, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
  }
}

void SettingsWindow::ReloadFromSettings() {
  if (!hwnd_) return;
  settings_ = store_->Load();
  ReloadSchemaList();

  int vsel = 0;
  const Variant cur = settings_.SchemaPref().forced_variant;
  for (int i = 0; i < kVariantCount; ++i)
    if (kVariantOrder[i] == cur) vsel = i;
  FillCombo(Ctl(hwnd_, IDC_VARIANT_COMBO), kVariantLabels, kVariantCount, vsel);

  const Tri punct = settings_.GetTri(keys::kTextAsciiPunct);
  FillCombo(Ctl(hwnd_, IDC_PUNCT_COMBO), kPunctLabels, 3,
            punct == Tri::kUnset ? 0 : (punct == Tri::kFalse ? 1 : 2));

  FillCombo(Ctl(hwnd_, IDC_COUNT_COMBO), kCountLabels, kCandCountCount,
            IndexOfCandCount(settings_.GetEnumInt(keys::kCandCount,
                                                  kCandCountValues,
                                                  kCandCountCount)));
  FillCombo(Ctl(hwnd_, IDC_SCALE_COMBO), kScaleLabels, kCandScaleCount,
            IndexOfCandScale(settings_.GetEnumInt(keys::kCandScale,
                                                  kCandScaleValues,
                                                  kCandScaleCount)));

  {
    std::wstring paths = L"使用者資料(詞典、設定):\r\n  " +
                         Utf8ToWide(store_->user_dir()) + L"\r\n" +
                         L"隨附資料(方案、詞庫,唯讀):\r\n  " +
                         Utf8ToWide(shared_dir_) + L"\r\n" + L"設定檔:\r\n  " +
                         Utf8ToWide(store_->settings_path());
    SetText(hwnd_, IDC_LBL_PATHS, paths.c_str());
  }
  {
    wchar_t buf[512];
    ::swprintf(buf, 512,
               L"RIME 四端輸入法(Windows,x64)\r\n"
               L"引擎門面 ABI %d,線路版本 %u\r\n"
               L"\r\n"
               L"這個版本**完全不連網**:兩個二進位都沒有相依任何網路 DLL,\r\n"
               L"CI 會用 dumpbin /imports 斷言這件事。方案市集還沒做,\r\n"
               L"所以現在也沒有東西需要一個連網開關。",
               engine_->AbiVersion(), static_cast<unsigned>(kProtocolVersion));
    SetText(hwnd_, IDC_LBL_ABOUT, buf);
  }
  SetStatus(L"");
}

void SettingsWindow::SetStatus(const std::wstring& text) {
  SetText(hwnd_, IDC_STATUS, text.c_str());
}

// ─────────────────────────── 套用 ───────────────────────────

void SettingsWindow::ApplyVariantNow() {
  const int sel = ComboSel(hwnd_, IDC_VARIANT_COMBO);
  const Variant want = kVariantOrder[(sel >= 0 && sel < kVariantCount) ? sel : 0];
  settings_.SetForcedVariant(want);
  if (!store_->Save(settings_)) {
    SetStatus(L"設定寫不進去 —— 改了但不會留到下次啟動。");
    return;
  }
  // 立刻對現有的每一個 session 套用。少了這一步,使用者改了字形之後
  // 要換一個程式才會生效 —— 而他當下看到的是「這個下拉沒有作用」。
  const std::vector<OptionAssign> plan = PlanVariant(want, Variant::kFollow);
  for (const OptionAssign& a : plan) engine_->SetOptionAll(a.option, a.value);
  SetStatus(want == Variant::kFollow ? L"字形改為跟隨語言設定檔。"
                                     : L"字形已套用。");
}

void SettingsWindow::ApplyPunctNow() {
  const int sel = ComboSel(hwnd_, IDC_PUNCT_COMBO);
  const Tri t = sel == 0 ? Tri::kUnset : (sel == 1 ? Tri::kFalse : Tri::kTrue);
  settings_.SetTri(keys::kTextAsciiPunct, t);
  if (!store_->Save(settings_)) {
    SetStatus(L"設定寫不進去。");
    return;
  }
  if (t != Tri::kUnset) engine_->SetOptionAll("ascii_punct", t == Tri::kTrue);
  SetStatus(t == Tri::kUnset ? L"標點改為跟隨方案(下次啟動生效)。"
                             : L"標點已套用。");
}

void SettingsWindow::ApplyDefaultSchemaNow() {
  const int sel = ComboSel(hwnd_, IDC_DEFAULT_COMBO);
  std::string id;
  if (sel > 0 && static_cast<size_t>(sel - 1) < schemas_.size())
    id = schemas_[sel - 1].first;
  settings_.SetForcedSchema(id);
  if (!store_->Save(settings_)) {
    SetStatus(L"設定寫不進去。");
    return;
  }
  if (!id.empty()) {
    engine_->SelectSchemaAll(id);
    SetStatus(L"預設方案已套用到目前所有的輸入視窗。");
  } else {
    // 「跟隨語言設定檔」不立刻改現有 session:那會把使用者剛剛用
    // Ctrl+` 選的方案打回去。下一個 session 才套用。
    SetStatus(L"預設方案改為跟隨語言設定檔,切換到下一個程式時生效。");
  }
}

bool SettingsWindow::ApplyOrderAndPageSize(std::string* error) {
  std::string yaml = store_->ReadDefaultCustom();
  if (yaml.empty() && !store_->DefaultCustomExists()) {
    *error =
        "找不到 default.custom.yaml。它應該在使用者資料夾裡,首次啟動時"
        "會從安裝目錄複製一份過去。";
    return false;
  }
  std::string next;
  PatchResult r = WriteSchemaList(yaml, order_, &next);
  if (r != PatchResult::kOk) {
    *error = r == PatchResult::kNoPatchSection
                 ? "你的 default.custom.yaml 我看不懂(找不到 patch: 那一段,"
                   "或它用 tab 縮排)。為了不弄壞你改過的設定,這次什麼都沒動 ——"
                   "請手動編輯那個檔案。"
                 : "方案清單裡有不合法的項目,這次什麼都沒動。";
    return false;
  }
  const int count =
      settings_.GetEnumInt(keys::kCandCount, kCandCountValues, kCandCountCount);
  std::string next2;
  char num[16] = {0};
  if (count > 0) std::snprintf(num, sizeof(num), "%d", count);
  r = UpsertPatchScalar(next, "menu/page_size", count > 0 ? num : "", &next2);
  if (r != PatchResult::kOk) {
    *error = "寫入候選個數失敗,這次什麼都沒動。";
    return false;
  }
  if (!store_->WriteDefaultCustom(next2)) {
    *error = "default.custom.yaml 寫不進去。";
    return false;
  }
  return true;
}

void SettingsWindow::StartRedeploy(const wchar_t* why) {
  if (deploying_) return;
  if (!engine_->BeginDeploy(&deploy_seq_)) {
    // rs_deploy() 拒絕啟動,多半是已經有一個部署在跑。
    // ⚠ 這裡**一定**要說話。Android 端踩過的原話是「使用者只能猜它
    //   成功了沒」——而那個 bug 是真機回報的。
    std::wstring msg = L"沒有開始整理:" + Utf8ToWide(engine_->last_error());
    ::MessageBoxW(hwnd_, msg.c_str(), L"重新整理字詞", MB_OK | MB_ICONWARNING);
    return;
  }
  deploying_ = true;
  deploy_why_ = why ? why : L"";
  deploy_start_ = ::GetTickCount();
  ::EnableWindow(Ctl(hwnd_, IDC_REDEPLOY), FALSE);
  ::EnableWindow(Ctl(hwnd_, IDC_APPLY_ORDER), FALSE);
  SetStatus(L"正在整理字詞…已耗時 0 秒");
  ::SetTimer(hwnd_, kDeployTimer, 200, nullptr);
}

void SettingsWindow::OnDeployTick() {
  if (!deploying_) return;
  const DWORD elapsed = ::GetTickCount() - deploy_start_;
  int status = 0;
  if (!engine_->PollDeploy(deploy_seq_, &status)) {
    // 還沒好。librime 不給百分比,所以我們只說實話:已經跑了多久。
    // 畫一條假的進度條比什麼都不畫更糟 —— 它會停在某個數字然後不動。
    wchar_t buf[128];
    ::swprintf(buf, 128, L"正在整理字詞…已耗時 %u 秒(大方案要數十秒)",
               static_cast<unsigned>(elapsed / 1000));
    SetStatus(buf);
    return;
  }
  ::KillTimer(hwnd_, kDeployTimer);
  deploying_ = false;
  ::EnableWindow(Ctl(hwnd_, IDC_REDEPLOY), TRUE);
  ::EnableWindow(Ctl(hwnd_, IDC_APPLY_ORDER), TRUE);
  if (status == 1) {
    wchar_t buf[128];
    ::swprintf(buf, 128, L"%s完成(耗時 %.1f 秒)。", deploy_why_.c_str(),
               elapsed / 1000.0);
    SetStatus(buf);
    ReloadSchemaList();
  } else {
    // 部署失敗時 rs_last_error() 常常是空字串 —— librime 的 C API 不給原因
    // (docs/coordination.md §4)。**不要假裝知道為什麼。**
    const std::string err = engine_->last_error();
    std::wstring msg = L"整理字詞沒有成功。\r\n\r\n";
    msg += err.empty() ? L"引擎沒有給原因(librime 的 C API 不提供)。\r\n"
                         L"常見成因是某個方案的詞庫檔案缺席。"
                       : Utf8ToWide(err);
    ::MessageBoxW(hwnd_, msg.c_str(), L"重新整理字詞", MB_OK | MB_ICONERROR);
    SetStatus(L"整理字詞失敗。");
  }
}

// ─────────────────────────── 事件 ───────────────────────────

void SettingsWindow::OnCommand(int id, int code) {
  switch (id) {
    case IDC_CLOSE:
    case IDCANCEL:
      ::ShowWindow(hwnd_, SW_HIDE);
      return;
    case IDC_UP:
    case IDC_DOWN: {
      HWND lb = Ctl(hwnd_, IDC_SCHEMA_LIST);
      if (!lb) return;
      const int sel = static_cast<int>(::SendMessageW(lb, LB_GETCURSEL, 0, 0));
      const int n = static_cast<int>(order_.size());
      if (sel < 0 || sel >= n) return;
      const int to = (id == IDC_UP) ? sel - 1 : sel + 1;
      if (to < 0 || to >= n) return;
      std::swap(order_[sel], order_[to]);
      // 清單重畫。schemas_ 的順序也跟著換,不然標籤會對錯位。
      for (int i = 0; i < n; ++i) {
        if (schemas_[i].first == order_[i]) continue;
        for (int j = i + 1; j < n; ++j) {
          if (schemas_[j].first == order_[i]) {
            std::swap(schemas_[i], schemas_[j]);
            break;
          }
        }
      }
      ::SendMessageW(lb, LB_RESETCONTENT, 0, 0);
      for (const auto& kv : schemas_) {
        const std::wstring line =
            Utf8ToWide(kv.second.empty() ? kv.first : kv.second) + L"  (" +
            Utf8ToWide(kv.first) + L")";
        ::SendMessageW(lb, LB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(line.c_str()));
      }
      ::SendMessageW(lb, LB_SETCURSEL, static_cast<WPARAM>(to), 0);
      SetStatus(L"順序改了,還沒生效 —— 按「套用順序」。");
      return;
    }
    case IDC_APPLY_ORDER: {
      std::string err;
      if (!ApplyOrderAndPageSize(&err)) {
        ::MessageBoxW(hwnd_, Utf8ToWide(err).c_str(), L"套用順序",
                      MB_OK | MB_ICONERROR);
        SetStatus(L"順序沒有套用。");
        return;
      }
      StartRedeploy(L"套用順序");
      return;
    }
    case IDC_DEFAULT_COMBO:
      if (code == CBN_SELCHANGE) ApplyDefaultSchemaNow();
      return;
    case IDC_VARIANT_COMBO:
      if (code == CBN_SELCHANGE) ApplyVariantNow();
      return;
    case IDC_PUNCT_COMBO:
      if (code == CBN_SELCHANGE) ApplyPunctNow();
      return;
    case IDC_COUNT_COMBO: {
      if (code != CBN_SELCHANGE) return;
      const int v = CandCountAtIndex(ComboSel(hwnd_, IDC_COUNT_COMBO));
      settings_.SetEnumInt(keys::kCandCount, v, kCandCountValues,
                           kCandCountCount);
      if (!store_->Save(settings_)) {
        SetStatus(L"設定寫不進去。");
        return;
      }
      // ⚠ 這一項改的是引擎的 menu/page_size,要重新部署才會變。
      //   只截掉畫面上的候選是不行的:數字鍵仍然選得到看不見的那幾個。
      if (::MessageBoxW(hwnd_,
                        L"候選個數要重新整理字詞之後才會變(約十幾秒)。\r\n"
                        L"現在就做嗎?",
                        L"一次顯示幾個候選", MB_YESNO | MB_ICONQUESTION) ==
          IDYES) {
        std::string err;
        if (!ApplyOrderAndPageSize(&err)) {
          ::MessageBoxW(hwnd_, Utf8ToWide(err).c_str(), L"候選個數",
                        MB_OK | MB_ICONERROR);
          return;
        }
        StartRedeploy(L"套用候選個數");
      } else {
        SetStatus(L"已記住,下次「重新整理字詞」時生效。");
      }
      return;
    }
    case IDC_SCALE_COMBO: {
      if (code != CBN_SELCHANGE) return;
      const int v = CandScaleAtIndex(ComboSel(hwnd_, IDC_SCALE_COMBO));
      settings_.SetEnumInt(keys::kCandScale, v, kCandScaleValues,
                           kCandScaleCount);
      if (!store_->Save(settings_)) {
        SetStatus(L"設定寫不進去。");
        return;
      }
      if (cand_) cand_->SetTextScale(v <= 0 ? 1.0 : v / 100.0);
      SetStatus(L"候選字大小已套用(下一次出現候選時看得到)。");
      return;
    }
    case IDC_REDEPLOY:
      StartRedeploy(L"重新整理字詞");
      return;
    case IDC_OPEN_USER_DIR:
      ::ShellExecuteW(hwnd_, L"open", Utf8ToWide(store_->user_dir()).c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
      return;
    case IDC_OPEN_SETTINGS_FILE: {
      // 檔案可能還不存在(全部都是預設值時我們刻意不寫檔)。
      // 直接開一個不存在的檔案,記事本會說「找不到」——
      // 那對使用者是個沒有用的答案,所以先確保它存在。
      if (!store_->Save(settings_)) {
        SetStatus(L"設定檔寫不進去。");
        return;
      }
      ::ShellExecuteW(hwnd_, L"open", L"notepad.exe",
                      Utf8ToWide(store_->settings_path()).c_str(), nullptr,
                      SW_SHOWNORMAL);
      return;
    }
    default:
      return;
  }
}

}  // namespace rimewin
