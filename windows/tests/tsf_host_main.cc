// windows/tests/tsf_host_main.cc — rime_tsf_host.exe
//
// ══ 這支程式是什麼 ═════════════════════════════════════════════════
//
// 一個**假的文字編輯器**。它做的事與記事本對 TSF 做的事一樣:
//
//   建 ITfThreadMgr → Activate → 建一份文件(ITextStoreACP)→ 給它焦點
//   → 啟用**我們的**語言設定檔 → 用 ITfKeystrokeMgr 送按鍵進去
//   → 看文件裡最後長出什麼字
//
// 也就是說:**它逼系統走完真正的那條路** —— 讀登錄檔、CoCreateInstance、
// 把 rime_tsf.dll 載入這個進程、呼叫 ActivateEx、掛 key event sink、
// 把按鍵交給我們、我們開 edit session、組字、上屏。
//
// ══ 為什麼要有它 ═══════════════════════════════════════════════════
//
// 上一輪 windows/README.md 把這一整段列在「只有人在真 Windows 上跑才驗得到」:
//
//     切到這個輸入法之後 ActivateEx 有沒有被呼叫、
//     在記事本裡打不打得出字、組字視窗會不會出現
//
// 而使用者實際回報的正是這一段壞掉。CI 驗到的「經由具名管道打出你好」
// **繞過了 TSF**:它證明的是「引擎 + 資料 + IPC」是好的,
// 完全沒有經過 DLL 被系統載入、ActivateEx 被呼叫、按鍵經 TSF 進來這條路。
//
// 這支程式把那條路補上。它跑得起來的話,「輸入法能不能用」就不再是
// 一句只能靠使用者回報的話。
//
// ══ 誠實說明它**不是**什麼 ═════════════════════════════════════════
//
//   · 它不是記事本。真的宿主會有自己的版面、DPI、視窗、UI 執行緒模型,
//     而 GetTextExt 在很多宿主上會失敗 —— 這裡的實作一定成功。
//     所以「候選窗位置對不對」這一格仍然驗不到。
//   · 它證明不了語言列按鈕與系統匣圖示長什麼樣。
//   · runner 是非互動的工作階段,CTF 的行為與使用者桌面上不保證一樣。
//     所以**每一步的 HRESULT 都印出來** —— 走不通的時候要看得出卡在哪,
//     而不是得到一句「失敗」。
//
// ══ 用法 ═══════════════════════════════════════════════════════════
//
//   rime_tsf_host.exe [--langid 0x0404] [--keys nihao1] [--expect 你好]
//                     [--require-activate] [--require-eaten]
//                     [--trace <檔案>] [--wait-ms 3000]
//
// 結束碼 0 = 要求的每一項都成立。
#include <msctf.h>
#include <windows.h>
// WIN32_LEAN_AND_MEAN 之下 windows.h 不帶 CommandLineToArgvW。
#include <shellapi.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "../tsf/guids.h"
#include "../winshared/winutil.h"

using namespace rimewin;

namespace {

int g_fails = 0;

void Say(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stdout, fmt, ap);
  va_end(ap);
  std::fflush(stdout);
}

void Step(const char* what, HRESULT hr) {
  Say("  %-46s hr=0x%08lX %s\n", what, static_cast<unsigned long>(hr),
      SUCCEEDED(hr) ? "OK" : "**失敗**");
}

void Fail(const char* fmt, ...) {
  ++g_fails;
  std::fprintf(stdout, "  !! ");
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stdout, fmt, ap);
  va_end(ap);
  std::fprintf(stdout, "\n");
  std::fflush(stdout);
}

void Ok(const char* fmt, ...) {
  std::fprintf(stdout, "  ✓ ");
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stdout, fmt, ap);
  va_end(ap);
  std::fprintf(stdout, "\n");
  std::fflush(stdout);
}

// 訊息幫浦。
//
// ⚠ 不可以省。TSF 在 STA 裡有一部分工作是靠視窗訊息推動的
//   (語言列、profile 通知)。不抽訊息的話,那些事件永遠不會送達,
//   而症狀是「某些 sink 從來沒被呼叫」—— 看起來像產品的錯。
void Pump(DWORD ms) {
  const DWORD end = ::GetTickCount() + ms;
  for (;;) {
    MSG msg;
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
    if (::GetTickCount() >= end) return;
    ::Sleep(10);
  }
}

// ───────────────────────── 假的文件 ─────────────────────────
//
// 最小可用的 ITextStoreACP。夠讓 TSF 的 generic context 在上面做:
// 取選取範圍、查可不可以插入、插入文字、改選取範圍、問插入點的螢幕矩形。
// 其餘的一律 E_NOTIMPL —— 而且**要誠實地 E_NOTIMPL**,不要回 S_OK 假裝做了,
// 那會讓失敗延後到一個與原因無關的地方才出現。
class FakeDoc final : public ITextStoreACP {
 public:
  explicit FakeDoc(HWND hwnd) : hwnd_(hwnd) {}

  std::wstring text;
  LONG sel_start = 0;
  LONG sel_end = 0;

  // ── IUnknown ──────────────────────────────────────────────
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITextStoreACP))
      *ppv = static_cast<ITextStoreACP*>(this);
    if (!*ppv) return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(::InterlockedIncrement(&ref_));
  }
  STDMETHODIMP_(ULONG) Release() override {
    const LONG n = ::InterlockedDecrement(&ref_);
    if (n == 0) delete this;
    return static_cast<ULONG>(n);
  }

  // ── sink ──────────────────────────────────────────────────
  STDMETHODIMP AdviseSink(REFIID riid, IUnknown* punk, DWORD mask) override {
    if (!IsEqualIID(riid, IID_ITextStoreACPSink)) return E_INVALIDARG;
    if (sink_) sink_->Release();
    sink_ = nullptr;
    mask_ = mask;
    return punk->QueryInterface(IID_ITextStoreACPSink, (void**)&sink_);
  }
  STDMETHODIMP UnadviseSink(IUnknown*) override {
    if (sink_) {
      sink_->Release();
      sink_ = nullptr;
    }
    return S_OK;
  }

  // ── 鎖 ────────────────────────────────────────────────────
  //
  // ⚠ 一定要**同步**授予。我們的文字服務用的是
  //   RequestEditSession(TF_ES_SYNC | TF_ES_READWRITE)(見 text_service.cc:
  //   非同步的 session 回來時那顆按鍵早就被宿主處理過了,順序全亂)。
  //   這裡若排隊改成非同步,那條路就完全走不到 —— 而 CI 會綠,
  //   因為驗到的是另一條路。
  STDMETHODIMP RequestLock(DWORD flags, HRESULT* result) override {
    if (!sink_ || !result) return E_UNEXPECTED;
    if (lock_ != 0) {
      *result = TS_E_SYNCHRONOUS;
      return S_OK;
    }
    lock_ = flags & (TS_LF_READ | TS_LF_READWRITE);
    *result = sink_->OnLockGranted(lock_);
    lock_ = 0;
    return S_OK;
  }

  STDMETHODIMP GetStatus(TS_STATUS* status) override {
    if (!status) return E_INVALIDARG;
    status->dwDynamicFlags = 0;
    status->dwStaticFlags = TS_SS_NOHIDDENTEXT;
    return S_OK;
  }

  STDMETHODIMP QueryInsert(LONG start, LONG end, ULONG /*cch*/,
                           LONG* out_start, LONG* out_end) override {
    if (!out_start || !out_end) return E_INVALIDARG;
    const LONG len = static_cast<LONG>(text.size());
    *out_start = start < 0 ? 0 : (start > len ? len : start);
    *out_end = end < 0 ? 0 : (end > len ? len : end);
    return S_OK;
  }

  STDMETHODIMP GetSelection(ULONG index, ULONG count, TS_SELECTION_ACP* sel,
                            ULONG* fetched) override {
    if (!fetched) return E_INVALIDARG;
    *fetched = 0;
    if (count == 0) return S_OK;
    if (index != TF_DEFAULT_SELECTION && index != 0) return TS_E_NOSELECTION;
    if (!sel) return E_INVALIDARG;
    sel[0].acpStart = sel_start;
    sel[0].acpEnd = sel_end;
    sel[0].style.ase = TS_AE_END;
    sel[0].style.fInterimChar = FALSE;
    *fetched = 1;
    return S_OK;
  }

  STDMETHODIMP SetSelection(ULONG count, const TS_SELECTION_ACP* sel) override {
    if (count == 0 || !sel) return E_INVALIDARG;
    sel_start = sel[0].acpStart;
    sel_end = sel[0].acpEnd;
    return S_OK;
  }

  STDMETHODIMP GetText(LONG start, LONG end, WCHAR* plain, ULONG plain_req,
                       ULONG* plain_out, TS_RUNINFO* run, ULONG run_req,
                       ULONG* run_out, LONG* next) override {
    const LONG len = static_cast<LONG>(text.size());
    if (start < 0) start = 0;
    if (start > len) start = len;
    LONG stop = (end < 0) ? len : end;
    if (stop > len) stop = len;
    if (stop < start) stop = start;
    ULONG n = static_cast<ULONG>(stop - start);
    if (plain && plain_req < n) n = plain_req;
    if (plain && n > 0) ::memcpy(plain, text.data() + start, n * sizeof(WCHAR));
    if (plain_out) *plain_out = n;
    if (run && run_req > 0) {
      run[0].uCount = n;
      run[0].type = TS_RT_PLAIN;
      if (run_out) *run_out = 1;
    } else if (run_out) {
      *run_out = 0;
    }
    if (next) *next = start + static_cast<LONG>(n);
    return S_OK;
  }

  STDMETHODIMP SetText(DWORD /*flags*/, LONG start, LONG end,
                       const WCHAR* s, ULONG cch, TS_TEXTCHANGE* change) override {
    const LONG len = static_cast<LONG>(text.size());
    if (start < 0) start = 0;
    if (start > len) start = len;
    if (end < 0 || end > len) end = len;
    if (end < start) end = start;
    text.replace(static_cast<size_t>(start), static_cast<size_t>(end - start),
                 s ? std::wstring(s, cch) : std::wstring());
    if (change) {
      change->acpStart = start;
      change->acpOldEnd = end;
      change->acpNewEnd = start + static_cast<LONG>(cch);
    }
    sel_start = sel_end = start + static_cast<LONG>(cch);
    return S_OK;
  }

  STDMETHODIMP InsertTextAtSelection(DWORD flags, const WCHAR* s, ULONG cch,
                                     LONG* out_start, LONG* out_end,
                                     TS_TEXTCHANGE* change) override {
    // TS_IAS_QUERYONLY:只回報「會插在哪裡」,一個字都不可以動。
    // 我們的 StartCompositionIfNeeded 走的正是這一條(它要的是一個
    // 折疊在插入點的 range,不是真的插入)。
    if (flags & TS_IAS_QUERYONLY) {
      if (out_start) *out_start = sel_start;
      if (out_end) *out_end = sel_end;
      return S_OK;
    }
    const LONG start = sel_start;
    const LONG end = sel_end;
    TS_TEXTCHANGE tc{};
    const HRESULT hr = SetText(0, start, end, s, cch, &tc);
    if (FAILED(hr)) return hr;
    if (out_start) *out_start = tc.acpStart;
    if (out_end) *out_end = tc.acpNewEnd;
    if (change) *change = tc;
    return S_OK;
  }

  STDMETHODIMP GetEndACP(LONG* acp) override {
    if (!acp) return E_INVALIDARG;
    *acp = static_cast<LONG>(text.size());
    return S_OK;
  }

  STDMETHODIMP GetActiveView(TsViewCookie* cookie) override {
    if (!cookie) return E_INVALIDARG;
    *cookie = 1;
    return S_OK;
  }

  STDMETHODIMP GetTextExt(TsViewCookie, LONG, LONG, RECT* rc,
                          BOOL* clipped) override {
    if (!rc || !clipped) return E_INVALIDARG;
    // 固定一塊矩形。**這裡與真實宿主最不像**,所以要說清楚:
    // 真的宿主上 GetTextExt 常常失敗或回空矩形(見 README 的「沒被驗證」
    // 第 4 項),而候選窗定位的退路正是為那件事寫的。這支程式驗不到它。
    ::SetRect(rc, 100, 200, 101, 220);
    *clipped = FALSE;
    return S_OK;
  }

  STDMETHODIMP GetScreenExt(TsViewCookie, RECT* rc) override {
    if (!rc) return E_INVALIDARG;
    ::SetRect(rc, 0, 0, 800, 600);
    return S_OK;
  }

  STDMETHODIMP GetWnd(TsViewCookie, HWND* hwnd) override {
    if (!hwnd) return E_INVALIDARG;
    *hwnd = hwnd_;
    return S_OK;
  }

  // ── 用不到的 ──────────────────────────────────────────────
  STDMETHODIMP GetFormattedText(LONG, LONG, IDataObject**) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP GetEmbedded(LONG, REFGUID, REFIID, IUnknown**) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP QueryInsertEmbedded(const GUID*, const FORMATETC*,
                                   BOOL* ok) override {
    if (ok) *ok = FALSE;
    return S_OK;
  }
  STDMETHODIMP InsertEmbedded(DWORD, LONG, LONG, IDataObject*,
                              TS_TEXTCHANGE*) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP InsertEmbeddedAtSelection(DWORD, IDataObject*, LONG*, LONG*,
                                         TS_TEXTCHANGE*) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP RequestSupportedAttrs(DWORD, ULONG, const TS_ATTRID*) override {
    return S_OK;
  }
  STDMETHODIMP RequestAttrsAtPosition(LONG, ULONG, const TS_ATTRID*,
                                      DWORD) override {
    return S_OK;
  }
  STDMETHODIMP RequestAttrsTransitioningAtPosition(LONG, ULONG,
                                                   const TS_ATTRID*,
                                                   DWORD) override {
    return S_OK;
  }
  STDMETHODIMP FindNextAttrTransition(LONG, LONG, ULONG, const TS_ATTRID*, DWORD,
                                      LONG*, BOOL*, LONG*) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP RetrieveRequestedAttrs(ULONG, TS_ATTRVAL*,
                                      ULONG* fetched) override {
    if (fetched) *fetched = 0;
    return S_OK;
  }
  STDMETHODIMP GetACPFromPoint(TsViewCookie, const POINT*, DWORD,
                               LONG*) override {
    return E_NOTIMPL;
  }

 private:
  ~FakeDoc() {
    if (sink_) sink_->Release();
  }
  LONG ref_ = 1;
  HWND hwnd_;
  ITextStoreACPSink* sink_ = nullptr;
  DWORD mask_ = 0;
  DWORD lock_ = 0;
};

// ── 把一個 ASCII 字元變成 (VK, lParam) ────────────────────────────
bool AsciiToKey(wchar_t ch, WPARAM* vk, LPARAM* lparam) {
  SHORT vs = ::VkKeyScanW(ch);
  if (vs == -1) return false;
  const WPARAM v = static_cast<WPARAM>(vs & 0xFF);
  const UINT scan = ::MapVirtualKeyW(static_cast<UINT>(v), MAPVK_VK_TO_VSC);
  *vk = v;
  // 重複次數 1、掃描碼在 16–23 位。這正是宿主收到 WM_KEYDOWN 時的形狀 ——
  // 我們的 BuildKeyEvent 從這裡取掃描碼。
  *lparam = static_cast<LPARAM>(1 | (static_cast<LPARAM>(scan) << 16));
  return true;
}

std::string Narrow(const std::wstring& s) { return WideToUtf8(s); }

void DumpTrace(const std::wstring& path) {
  Say("\n--- 瘦 DLL 的除錯記錄(%s)---\n", Narrow(path).c_str());
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    Say("  (檔案不存在 —— DLL 從來沒有被載入這個進程)\n");
    return;
  }
  char buf[4096];
  DWORD got = 0;
  std::string all;
  while (::ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0)
    all.append(buf, got);
  ::CloseHandle(h);
  std::fwrite(all.data(), 1, all.size(), stdout);
  std::fflush(stdout);
}

std::string ReadAll(const std::wstring& path) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::string();
  char buf[4096];
  DWORD got = 0;
  std::string all;
  while (::ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0)
    all.append(buf, got);
  ::CloseHandle(h);
  return all;
}

}  // namespace

static int Run(int argc, wchar_t** argv);

int main(int, char**) {
  int argc = 0;
  LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (!argv) return 2;
  const int rc = Run(argc, argv);
  ::LocalFree(argv);
  return rc;
}

static int Run(int argc, wchar_t** argv) {
  LANGID langid = 0x0404;
  std::wstring keys;
  std::wstring expect;
  std::wstring trace_path;
  bool require_activate = false;
  bool require_eaten = false;
  DWORD wait_ms = 3000;

  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    if (a == L"--langid" && i + 1 < argc)
      langid = static_cast<LANGID>(::wcstol(argv[++i], nullptr, 0));
    else if (a == L"--keys" && i + 1 < argc) keys = argv[++i];
    else if (a == L"--expect" && i + 1 < argc) expect = argv[++i];
    else if (a == L"--trace" && i + 1 < argc) trace_path = argv[++i];
    else if (a == L"--require-activate") require_activate = true;
    else if (a == L"--require-eaten") require_eaten = true;
    else if (a == L"--wait-ms" && i + 1 < argc)
      wait_ms = static_cast<DWORD>(::wcstol(argv[++i], nullptr, 0));
    else {
      Say("未知參數: %s\n", Narrow(a).c_str());
      return 2;
    }
  }

  if (trace_path.empty()) {
    wchar_t tmp[MAX_PATH] = {0};
    ::GetTempPathW(MAX_PATH, tmp);
    trace_path = std::wstring(tmp) + L"rime-tsf-host.log";
  }
  ::DeleteFileW(trace_path.c_str());
  // DLL 在**這個進程裡**讀這個環境變數,所以在載入它之前設好就行。
  ::SetEnvironmentVariableW(L"RIME_TSF_TRACE", trace_path.c_str());

  Say("=== rime_tsf_host:用真的 TSF 走一遍 ===\n");
  Say("  langid = 0x%04X\n", static_cast<unsigned>(langid));
  Say("  按鍵   = %s\n", keys.empty() ? "(不送)" : Narrow(keys).c_str());
  Say("  記錄   = %s\n\n", Narrow(trace_path).c_str());

  const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Step("CoInitializeEx(STA)", com);
  if (FAILED(com)) return 2;

  // 一個隱藏視窗。GetWnd 要回傳一個真的 HWND —— 有些 TSF 的路徑會拿它
  // 去問 DPI 與螢幕。
  WNDCLASSW wc{};
  wc.lpfnWndProc = ::DefWindowProcW;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.lpszClassName = L"RimeTsfHostWnd";
  ::RegisterClassW(&wc);
  HWND hwnd = ::CreateWindowExW(0, L"RimeTsfHostWnd", L"rime tsf host",
                                WS_OVERLAPPEDWINDOW, 0, 0, 400, 200, nullptr,
                                nullptr, wc.hInstance, nullptr);
  if (!hwnd) {
    Fail("建不出視窗 err=%lu", ::GetLastError());
  } else {
    // ⚠ 一定要真的顯示並搶到前景。
    //
    // TSF 的 key event sink 分「前景」與「非前景」兩種,而
    // AdviseKeyEventSink(..., fForeground = TRUE) 要求**呼叫執行緒擁有前景**。
    // 視窗藏著不顯示的話,文字服務掛前景 sink 會失敗,於是
    // ITfKeystrokeMgr::KeyDown 送進去的按鍵**不會被交給任何人** ——
    // 而回傳值仍然是 S_OK、pfEaten 仍然是 FALSE,看起來就像「輸入法不吃這顆鍵」。
    // 實測(CI run #58 的 install job)就是這樣:ActivateEx 過了、
    // 語言列按鈕也加上了,而六顆按鍵一顆都沒有到達 OnTestKeyDown。
    ::ShowWindow(hwnd, SW_SHOWNORMAL);
    ::UpdateWindow(hwnd);
    ::SetForegroundWindow(hwnd);
    ::SetActiveWindow(hwnd);
    ::SetFocus(hwnd);
    Pump(200);
    HWND fg = ::GetForegroundWindow();
    Say("  前景視窗 = %p(我們的是 %p)%s\n", static_cast<void*>(fg),
        static_cast<void*>(hwnd),
        fg == hwnd ? " —— 搶到了" : " —— **沒搶到**(非互動的工作階段?)");
  }

  ITfThreadMgr* thread_mgr = nullptr;
  HRESULT hr = ::CoCreateInstance(CLSID_TF_ThreadMgr, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
                                  (void**)&thread_mgr);
  Step("CoCreateInstance(CLSID_TF_ThreadMgr)", hr);
  if (FAILED(hr) || !thread_mgr) {
    Fail("這台機器上建不出 ITfThreadMgr —— TSF 在這個工作階段裡不可用。\n"
         "     (GitHub runner 是非互動的工作階段,這一步走不通是可能的。\n"
         "      走不通的話,這支程式驗不到任何東西,但**它必須明說**,\n"
         "      不可以靜靜地以 0 結束。)");
    return 3;
  }

  TfClientId client_id = TF_CLIENTID_NULL;
  hr = thread_mgr->Activate(&client_id);
  Step("ITfThreadMgr::Activate", hr);
  if (FAILED(hr)) {
    Fail("Activate 失敗,後面每一步都沒有意義");
    return 3;
  }

  ITfDocumentMgr* docmgr = nullptr;
  hr = thread_mgr->CreateDocumentMgr(&docmgr);
  Step("CreateDocumentMgr", hr);

  FakeDoc* doc = new FakeDoc(hwnd);
  ITfContext* ctx = nullptr;
  TfEditCookie ec = 0;
  if (docmgr) {
    hr = docmgr->CreateContext(client_id, 0, static_cast<ITextStoreACP*>(doc),
                               &ctx, &ec);
    Step("CreateContext(帶 ITextStoreACP)", hr);
    if (SUCCEEDED(hr)) {
      hr = docmgr->Push(ctx);
      Step("ITfDocumentMgr::Push", hr);
    }
  }
  hr = thread_mgr->SetFocus(docmgr);
  Step("ITfThreadMgr::SetFocus", hr);
  Pump(200);

  // ── 啟用我們的語言設定檔 ────────────────────────────────────
  //
  // ⚠ 這一步是整支程式的重點,而且**旗標的組合會決定它成不成功**。
  //   所以這裡不是「試一種然後宣布結論」,是把幾種都試過去,
  //   並且把每一種的 HRESULT 都印出來 —— 走不通的時候,
  //   下一輪才知道是哪一種組合的問題,而不是「ActivateProfile 失敗」。
  ITfInputProcessorProfiles* profiles = nullptr;
  hr = ::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                          CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfiles,
                          (void**)&profiles);
  Step("CoCreateInstance(InputProcessorProfiles)", hr);

  const GUID* profile_guid = nullptr;
  for (int i = 0; i < kRimeProfileCount; ++i)
    if (kRimeProfiles[i].langid == langid) profile_guid = kRimeProfiles[i].guid;
  if (!profile_guid) {
    Fail("langid 0x%04X 不在我們註冊的三個語言裡", static_cast<unsigned>(langid));
    return 2;
  }

  bool activated = false;
  if (profiles) {
    ITfInputProcessorProfileMgr* mgr = nullptr;
    if (SUCCEEDED(profiles->QueryInterface(IID_ITfInputProcessorProfileMgr,
                                           (void**)&mgr)) &&
        mgr) {
      struct Attempt {
        const char* name;
        DWORD flags;
      };
      static const Attempt kAttempts[] = {
          {"FORPROCESS|DONTCARECURRENTINPUTLANGUAGE",
           TF_IPPMF_FORPROCESS | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE},
          {"FORSESSION|DONTCARECURRENTINPUTLANGUAGE",
           TF_IPPMF_FORSESSION | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE},
          {"DONTCARECURRENTINPUTLANGUAGE", TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE},
          {"0", 0},
      };
      for (const Attempt& a : kAttempts) {
        const HRESULT ar = mgr->ActivateProfile(
            TF_PROFILETYPE_INPUTPROCESSOR, langid, CLSID_RimeTextService,
            *profile_guid, nullptr, a.flags);
        Say("  ActivateProfile(%-42s) hr=0x%08lX\n", a.name,
            static_cast<unsigned long>(ar));
        if (SUCCEEDED(ar)) {
          activated = true;
          break;
        }
      }
      mgr->Release();
    } else {
      Fail("拿不到 ITfInputProcessorProfileMgr");
    }
    // 舊介面再試一次。ActivateLanguageProfile 在某些系統組態下是唯一走得通的。
    if (!activated) {
      const HRESULT ar = profiles->ActivateLanguageProfile(
          CLSID_RimeTextService, langid, *profile_guid);
      Step("ITfInputProcessorProfiles::ActivateLanguageProfile", ar);
      activated = SUCCEEDED(ar);
    }
    profiles->Release();
  }
  if (!activated) Fail("三種旗標與舊介面都啟用不了我們的語言設定檔");

  // 啟用之後再給一次焦點:有些系統要焦點事件才會真的把 TIP 叫起來。
  if (docmgr) thread_mgr->SetFocus(docmgr);
  Pump(wait_ms);

  // ── ActivateEx 到底有沒有被呼叫 ────────────────────────────
  const std::string trace = ReadAll(trace_path);
  const bool saw_load = trace.find("DLL 載入") != std::string::npos;
  const bool saw_activate = trace.find("ActivateEx 被呼叫") != std::string::npos;
  const bool key_sink_bad = trace.find("**沒掛上,收不到按鍵**") != std::string::npos;
  Say("\n--- 分層結論 ---\n");
  if (saw_load)
    Ok("系統把 rime_tsf.dll 載入了這個進程");
  else
    Fail("rime_tsf.dll **沒有被載入** —— 註冊那一段有問題(登錄檔 / CLSID / 路徑)");
  if (saw_activate)
    Ok("ActivateEx 被呼叫了 —— 文字服務真的被啟用");
  else if (require_activate)
    Fail("ActivateEx **沒有被呼叫** —— DLL 載入了,但系統沒有把它當成輸入法叫起來");
  else
    Say("  (ActivateEx 沒有被呼叫)\n");

  // ── 送按鍵 ─────────────────────────────────────────────────
  int eaten_count = 0;
  if (!keys.empty()) {
    ITfKeystrokeMgr* ks = nullptr;
    hr = thread_mgr->QueryInterface(IID_ITfKeystrokeMgr, (void**)&ks);
    Step("QueryInterface(ITfKeystrokeMgr)", hr);
    if (SUCCEEDED(hr) && ks) {
      Say("\n--- 送按鍵 ---\n");
      for (wchar_t ch : keys) {
        WPARAM vk = 0;
        LPARAM lp = 0;
        if (!AsciiToKey(ch, &vk, &lp)) {
          Fail("'%c' 在目前的佈局上打不出來", static_cast<char>(ch));
          continue;
        }
        BOOL test_eaten = FALSE;
        BOOL eaten = FALSE;
        const HRESULT t = ks->TestKeyDown(vk, lp, &test_eaten);
        const HRESULT k = ks->KeyDown(vk, lp, &eaten);
        BOOL up_eaten = FALSE;
        ks->KeyUp(vk, lp | 0xC0000000, &up_eaten);
        Say("  '%c' vk=0x%02X test=%d(hr=0x%08lX) down=%d(hr=0x%08lX)\n",
            static_cast<char>(ch), static_cast<unsigned>(vk),
            test_eaten ? 1 : 0, static_cast<unsigned long>(t), eaten ? 1 : 0,
            static_cast<unsigned long>(k));
        if (eaten) ++eaten_count;
        Pump(60);
      }
      ks->Release();
    }
    Say("\n  文件內容 = \"%s\"\n", Narrow(doc->text).c_str());
    Say("  被吃掉的按鍵 = %d / %d\n", eaten_count,
        static_cast<int>(keys.size()));
  }

  // 「一顆按鍵都沒有到達 OnTestKeyDown」與「到達了但沒吃」是兩件完全不同的事。
  // 前者要查 ActivateEx 那一段(key event sink 掛上了沒有),
  // 後者要查佈局或連線。把它們併成一句話,就是把人往錯的方向送。
  const bool saw_any_key = trace.find("按鍵 vk=") != std::string::npos;
  if (!keys.empty() && !saw_any_key) {
    Fail("按鍵**一顆都沒有到達 OnTestKeyDown** —— 不是「不吃」,是根本沒收到。\n"
         "     要查的是 ActivateEx 裡的 AdviseKeyEventSink,不是佈局也不是連線。\n"
         "     記錄裡的 key sink 那一行會說掛上了沒有。");
    if (key_sink_bad)
      Fail("記錄明說 key event sink 兩種都掛不上");
  }
  if (require_eaten && eaten_count == 0 && saw_any_key)
    Fail("按鍵到達了 OnTestKeyDown,但一顆都沒有被吃掉。\n"
         "     照除錯記錄裡的 keysym 判斷是哪一段:\n"
         "       keysym=0x0  → 鍵盤佈局問不出字(win32_oracle.h)\n"
         "       keysym!=0   → 連不上服務(上面會有一行「連線失敗」)");

  if (!expect.empty()) {
    if (doc->text == expect)
      Ok("文件裡真的是「%s」—— 整條 TSF 路徑走通了", Narrow(expect).c_str());
    else
      Fail("文件裡是「%s」,預期「%s」", Narrow(doc->text).c_str(),
           Narrow(expect).c_str());
  }

  DumpTrace(trace_path);

  // 收尾。Deactivate 走了才會看到 DLL 的 Deactivate 記錄。
  if (ctx) ctx->Release();
  if (docmgr) {
    docmgr->Pop(TF_POPF_ALL);
    docmgr->Release();
  }
  thread_mgr->Deactivate();
  thread_mgr->Release();
  doc->Release();
  if (hwnd) ::DestroyWindow(hwnd);
  ::CoUninitialize();

  Say("\n=== %s(%d 項失敗)===\n", g_fails == 0 ? "通過" : "失敗", g_fails);
  return g_fails == 0 ? 0 : 1;
}
