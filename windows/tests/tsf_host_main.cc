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
//   ── 按鍵矩陣模式(windows/verify_input_matrix.sh 用的)──────────
//
//   --seq "n i h a o {BS} 1"   逐鍵送。`{名字}` 是功能鍵,見 kNamedKeys。
//   --pretext "已經有的字"      先把文件內容設成這樣(游標在最後)。
//                              用來驗「沒有組字時的退格」—— 那需要文件裡
//                              先有東西可以刪。
//   --expect-doc "已經有的"     跑完之後文件必須完全等於這個。
//
//   ⚠ **宿主會做預設處理。** KeyDown 回報沒吃掉的鍵,這支假宿主會像一個
//     真的編輯框那樣自己處理(插入字元、退格刪一個、方向鍵移游標)。
//     少了這一段,「放行」與「掉進黑洞」在文件內容上長得一模一樣 ——
//     而那正是使用者回報的「可以打字,不能刪除」的形狀。
//
//   rime_tsf_host.exe --hold-dll <dll路徑> --hold-ms <毫秒>
//                     只 LoadLibrary 那個 DLL 然後睡著,不碰 TSF。
//                     用來**重現使用者解除安裝時撞到的那個狀態**:
//                     還有程式握著 rime_tsf.dll,所以檔案刪不掉。
//                     見 windows/verify_installer.sh §11。
//
//   ── 兩階段模式:升級發生在**這個進程還活著**的時候 ─────────────
//
//   --phase2-ready-file <檔>   第一階段打完字之後寫出這個檔(對外的訊號:
//                              「我已經載入舊版 DLL 並且打得出字了,
//                                可以開始裝新版」)
//   --phase2-go-file <檔>      然後等這個檔出現(對內的訊號:「裝好了」),
//                              再打一次同樣的字並斷言同樣的結果
//   --phase2-timeout-ms <毫秒> 等 go 檔的上限(預設 600000)
//   --phase2-relink-ms <毫秒>  等「重新連上服務」的上限(預設 240000)
//
//   ⚠ 這個模式驗的是使用者升級之後**真實存在**的狀態:
//     已經開著的程式(檔案總管、瀏覽器)手上是**舊的 DLL 映像**,
//     而服務進程已經換成新的。兩者談不攏的話,症狀是
//     「有些程式能打字、有些不能」——而使用者完全無法理解為什麼。
//     那比全部壞掉更難查,所以它必須有一道關卡。
//     見 windows/verify_installer.sh §13。
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

// ── 具名功能鍵 ────────────────────────────────────────────────────
//
// ⚠ 這張表存在的理由:舊的 --keys 只吃 ASCII 字元(VkKeyScanW),
//   所以 CI **從來沒有送過一顆功能鍵**。使用者回報的退格鍵缺陷因此
//   完全在自動化的射程之外 —— 六顆字母全綠,而鍵盤是壞的。
struct NamedKey {
  const wchar_t* name;
  WPARAM vk;
  bool extended;  // lParam bit 24。方向鍵那一組在主鍵盤上是 extended。
};
const NamedKey kNamedKeys[] = {
    {L"BS", VK_BACK, false},      {L"DEL", VK_DELETE, true},
    {L"LEFT", VK_LEFT, true},     {L"RIGHT", VK_RIGHT, true},
    {L"UP", VK_UP, true},         {L"DOWN", VK_DOWN, true},
    {L"HOME", VK_HOME, true},     {L"END", VK_END, true},
    {L"PGUP", VK_PRIOR, true},    {L"PGDN", VK_NEXT, true},
    {L"ESC", VK_ESCAPE, false},   {L"ENTER", VK_RETURN, false},
    {L"TAB", VK_TAB, false},      {L"SPACE", VK_SPACE, false},
    {L"F1", VK_F1, false},        {L"INS", VK_INSERT, true},
};

// 一顆要送出去的鍵。
struct SeqKey {
  std::wstring label;  // 印出來用
  WPARAM vk = 0;
  LPARAM lparam = 0;
  wchar_t ch = 0;  // 字元鍵才有;宿主的預設處理要拿它插進文件
};

bool NamedToKey(const std::wstring& name, SeqKey* out) {
  for (const NamedKey& n : kNamedKeys) {
    if (name != n.name) continue;
    const UINT scan =
        ::MapVirtualKeyW(static_cast<UINT>(n.vk), MAPVK_VK_TO_VSC);
    out->label = L"{" + name + L"}";
    out->vk = n.vk;
    out->lparam = static_cast<LPARAM>(1 | (static_cast<LPARAM>(scan) << 16) |
                                      (n.extended ? (1L << 24) : 0));
    out->ch = (n.vk == VK_SPACE) ? L' ' : 0;
    return true;
  }
  return false;
}

// "n i h a o {BS} 1" → 一串 SeqKey。空白與逗號都是分隔符;
// 不在 {} 裡的每一個非空白字元各自是一顆鍵。
// 宿主的預設處理:KeyDown 說「沒吃掉」的鍵,一個真的編輯框會做的事。
//
// ⚠⚠ **這一段是這支程式這一輪最重要的改動。**
//
//   沒有它的話,「輸入法放行了,宿主收到了」與「輸入法吃掉了但什麼都沒做」
//   在文件內容上完全一樣(兩者都是文件沒變)。而後者正是使用者回報的
//   「可以打字,不能刪除」。也就是說:沒有這一段,那個缺陷在 CI 上
//   **不可能**被抓到,不管送多少顆退格。
//
//   所以矩陣的斷言看的是**文件內容**,不是「有沒有被吃掉」。
void HostDefaultAction(FakeDoc* doc, const SeqKey& k) {
  const LONG len = static_cast<LONG>(doc->text.size());
  LONG& s = doc->sel_start;
  LONG& e = doc->sel_end;
  if (s < 0) s = 0;
  if (s > len) s = len;
  switch (k.vk) {
    case VK_BACK:
      if (s > 0) {
        doc->text.erase(static_cast<size_t>(s - 1), 1);
        s = e = s - 1;
      }
      return;
    case VK_DELETE:
      if (s < static_cast<LONG>(doc->text.size()))
        doc->text.erase(static_cast<size_t>(s), 1);
      e = s;
      return;
    case VK_LEFT:  if (s > 0) s = e = s - 1; return;
    case VK_RIGHT: if (s < len) s = e = s + 1; return;
    case VK_HOME:  s = e = 0; return;
    case VK_END:   s = e = len; return;
    // 單行編輯框對這幾顆鍵不動文件。Enter 也一樣 —— 插入換行只會讓
    // 矩陣的預期字串變得難讀,而我們要驗的是「有沒有被放行」。
    case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
    case VK_ESCAPE: case VK_TAB: case VK_RETURN: case VK_F1: case VK_INSERT:
      return;
    default: break;
  }
  if (k.ch != 0) {
    doc->text.insert(static_cast<size_t>(s), 1, k.ch);
    s = e = s + 1;
  }
}

// 送一顆鍵並讓假宿主做它的預設處理。
//
// ⚠ 抽成函式**不是**為了少打字。第二階段(升級之後)要走的必須是
//   與第一階段**完全同一段**程式碼 —— 兩段各寫一份的話,「升級之後
//   還打得出字」驗到的會是另一條路,而那正是這個專案抓過很多次的形狀。
struct SendOutcome {
  BOOL test_eaten = FALSE;
  BOOL eaten = FALSE;
  bool host_did = false;
};

// ── Ctrl+空白鍵那顆保留鍵,TSF 到底收下了沒有 ────────────────────
//
// 使用者回報「ctrl+ 空格沒辦法切中英文」。做法是 ITfKeystrokeMgr::PreserveKey
// (**不是**低階鍵盤 hook,理由見 common/hotkey_policy.h),而那一次呼叫
// 發生在 ActivateEx 裡面 —— 也就是說「有沒有註冊成功」只有在**真的被
// 系統啟用過的那一份文字服務**上問得到。
//
// ⚠ 這一支問的是註冊,不是按下去會怎樣。用 IsPreservedKey 向 TSF 反問:
//   它認不認得 {我們的 GUID, VK_SPACE + TF_MOD_CONTROL}。
//
// ⚠ 第二問同樣重要:{VK_C + TF_MOD_CONTROL} **必須不認得**。這個專案吃過
//   「宣稱每一個 keysym 都要,結果吃掉 Ctrl+C」的虧,而保留鍵是同一種
//   權力 —— 註冊得太寬的話,使用者在每一個程式裡的 Ctrl+C 都會消失,
//   而他不會知道是輸入法幹的。
void CheckPreservedKey(ITfKeystrokeMgr* ks) {
  Say("\n--- 保留鍵(Ctrl+空白鍵)---\n");
  if (!ks) {
    Fail("拿不到 ITfKeystrokeMgr —— 問不了保留鍵");
    return;
  }
  TF_PRESERVEDKEY want{};
  want.uVKey = VK_SPACE;
  want.uModifiers = TF_MOD_CONTROL;
  BOOL registered = FALSE;
  HRESULT hr = ks->IsPreservedKey(GUID_RimePreservedKeyToggle, &want,
                                  &registered);
  Say("  IsPreservedKey(Ctrl+Space) hr=0x%08lX registered=%d\n",
      static_cast<unsigned long>(hr), registered ? 1 : 0);
  if (FAILED(hr) || !registered)
    Fail("Ctrl+空白鍵**沒有**被註冊成保留鍵 —— 使用者按下去不會有任何事發生。\n"
         "     註冊在 TextService::ActivateEx 裡(見 common/hotkey_policy.h);\n"
         "     那裡失敗時記錄會有一行『保留鍵 Ctrl+Space:註冊失敗』。");

  TF_PRESERVEDKEY nope{};
  nope.uVKey = 'C';
  nope.uModifiers = TF_MOD_CONTROL;
  BOOL stolen = FALSE;
  hr = ks->IsPreservedKey(GUID_RimePreservedKeyToggle, &nope, &stolen);
  Say("  IsPreservedKey(Ctrl+C)     hr=0x%08lX registered=%d\n",
      static_cast<unsigned long>(hr), stolen ? 1 : 0);
  if (SUCCEEDED(hr) && stolen)
    Fail("Ctrl+C 也被註冊成保留鍵了 —— 使用者在每一個程式裡的複製都會消失,\n"
         "     而他不會知道是輸入法幹的。保留鍵只准註冊真的要處理的那一顆。");
}

SendOutcome SendKeyThrough(ITfKeystrokeMgr* ks, FakeDoc* doc, const SeqKey& sk) {
  SendOutcome o;
  ks->TestKeyDown(sk.vk, sk.lparam, &o.test_eaten);
  ks->KeyDown(sk.vk, sk.lparam, &o.eaten);
  BOOL up_eaten = FALSE;
  ks->KeyUp(sk.vk, sk.lparam | 0xC0000000, &up_eaten);
  // ⚠ 輸入法沒吃 → 宿主自己處理。真的編輯框就是這樣。
  if (!o.eaten) {
    HostDefaultAction(doc, sk);
    o.host_did = true;
  }
  return o;
}

bool FileThere(const std::wstring& path) {
  return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool TouchFile(const std::wstring& path) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  ::WriteFile(h, "ready\r\n", 7, &written, nullptr);
  ::CloseHandle(h);
  return true;
}

bool ParseSeq(const std::wstring& seq, std::vector<SeqKey>* out) {
  size_t i = 0;
  while (i < seq.size()) {
    const wchar_t c = seq[i];
    if (c == L' ' || c == L',' || c == L'\t') {
      ++i;
      continue;
    }
    if (c == L'{') {
      const size_t close = seq.find(L'}', i);
      if (close == std::wstring::npos) {
        Say("!! --seq 裡有沒收尾的 '{'\n");
        return false;
      }
      const std::wstring name = seq.substr(i + 1, close - i - 1);
      SeqKey k;
      if (!NamedToKey(name, &k)) {
        Say("!! --seq 不認得 {%s} —— 請加進 kNamedKeys\n",
            Narrow(name).c_str());
        return false;
      }
      out->push_back(k);
      i = close + 1;
      continue;
    }
    SeqKey k;
    if (!AsciiToKey(c, &k.vk, &k.lparam)) {
      Say("!! '%s' 在目前的佈局上打不出來\n", Narrow(std::wstring(1, c)).c_str());
      return false;
    }
    k.label = std::wstring(1, c);
    k.ch = c;
    out->push_back(k);
    ++i;
  }
  return true;
}

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

// ── 把 DLL 抓在手上不放 ────────────────────────────────────────────
//
// 這不是測試 TSF,是**重現使用者的處境**:瀏覽器、檔案總管、Office 這些
// 程式在解除安裝的當下仍然握著 rime_tsf.dll,於是那個檔案刪不掉,
// Inno 只好把它排進「開機時刪除」的佇列,然後問使用者要不要重新啟動。
//
// 刻意用最笨的方式(LoadLibrary + 睡)——不註冊、不啟用 profile、不建 TSF 情境。
// 要重現的就只是「有一個進程握著這個檔案」這件事本身,多做任何一步都是
// 在測別的東西。
int HoldDll(const std::wstring& path, DWORD ms) {
  Say("LoadLibrary %s\n", Narrow(path).c_str());
  HMODULE h = ::LoadLibraryW(path.c_str());
  if (!h) {
    Say("!! 載入不了 err=%lu\n", static_cast<unsigned long>(::GetLastError()));
    return 1;
  }
  Say("已載入,握住 %lu 毫秒\n", static_cast<unsigned long>(ms));
  std::fflush(stdout);
  ::Sleep(ms);
  ::FreeLibrary(h);
  Say("已放開\n");
  return 0;
}

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
  std::wstring seq;
  std::wstring pretext;
  // ⚠ Ctrl+空白鍵那顆保留鍵有沒有真的被 TSF 收下。見下面 CheckPreservedKey()。
  bool check_preserved = false;
  std::wstring expect_doc;
  bool have_expect_doc = false;
  std::wstring expect;
  std::wstring trace_path;
  bool require_activate = false;
  bool require_eaten = false;
  DWORD wait_ms = 3000;
  std::wstring hold_dll;
  DWORD hold_ms = 0;
  std::wstring phase2_ready;
  std::wstring phase2_go;
  DWORD phase2_timeout_ms = 600000;
  DWORD phase2_relink_ms = 240000;

  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    if (a == L"--langid" && i + 1 < argc)
      langid = static_cast<LANGID>(::wcstol(argv[++i], nullptr, 0));
    else if (a == L"--keys" && i + 1 < argc) keys = argv[++i];
    else if (a == L"--seq" && i + 1 < argc) seq = argv[++i];
    else if (a == L"--pretext" && i + 1 < argc) pretext = argv[++i];
    else if (a == L"--expect-doc" && i + 1 < argc) {
      expect_doc = argv[++i];
      have_expect_doc = true;
    }
    else if (a == L"--expect" && i + 1 < argc) expect = argv[++i];
    else if (a == L"--trace" && i + 1 < argc) trace_path = argv[++i];
    else if (a == L"--check-preserved-key") check_preserved = true;
    else if (a == L"--require-activate") require_activate = true;
    else if (a == L"--require-eaten") require_eaten = true;
    else if (a == L"--wait-ms" && i + 1 < argc)
      wait_ms = static_cast<DWORD>(::wcstol(argv[++i], nullptr, 0));
    else if (a == L"--hold-dll" && i + 1 < argc) hold_dll = argv[++i];
    else if (a == L"--hold-ms" && i + 1 < argc)
      hold_ms = static_cast<DWORD>(::wcstol(argv[++i], nullptr, 0));
    else if (a == L"--phase2-ready-file" && i + 1 < argc) phase2_ready = argv[++i];
    else if (a == L"--phase2-go-file" && i + 1 < argc) phase2_go = argv[++i];
    else if (a == L"--phase2-timeout-ms" && i + 1 < argc)
      phase2_timeout_ms = static_cast<DWORD>(::wcstol(argv[++i], nullptr, 0));
    else if (a == L"--phase2-relink-ms" && i + 1 < argc)
      phase2_relink_ms = static_cast<DWORD>(::wcstol(argv[++i], nullptr, 0));
    else {
      Say("未知參數: %s\n", Narrow(a).c_str());
      return 2;
    }
  }

  // --hold-dll 是一條完全獨立的路徑:不碰 COM、不碰 TSF、不寫除錯記錄。
  if (!hold_dll.empty()) return HoldDll(hold_dll, hold_ms ? hold_ms : 30000);

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
  // 「沒有組字時按退格」需要文件裡先有東西可以刪 —— 不然刪成功與
  // 刪失敗都是空字串,斷言等於沒有斷言。
  if (!pretext.empty()) {
    doc->text = pretext;
    doc->sel_start = doc->sel_end = static_cast<LONG>(pretext.size());
  }
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
  //
  // --seq 是逐鍵模式(功能鍵寫成 {BS});--keys 是舊的 ASCII 字串模式。
  // 兩者最後都變成同一串 SeqKey,走同一條路 —— 不然「矩陣測到的」與
  // 「§6c 測到的」會是兩段不同的程式碼。
  std::vector<SeqKey> plan;
  if (!seq.empty()) {
    if (!ParseSeq(seq, &plan)) return 2;
  } else {
    for (wchar_t ch : keys) {
      SeqKey k;
      if (!AsciiToKey(ch, &k.vk, &k.lparam)) {
        Fail("'%c' 在目前的佈局上打不出來", static_cast<char>(ch));
        continue;
      }
      k.label = std::wstring(1, ch);
      k.ch = ch;
      plan.push_back(k);
    }
  }

  int eaten_count = 0;
  int mismatch_count = 0;
  int host_handled = 0;
  // ⚠ ks 提到迴圈外面:第二階段(升級之後)要用**同一個**介面再送一次鍵。
  //   在第一階段就 Release 掉的話,第二階段得自己再 QueryInterface 一次 ——
  //   那是另一條路,而「升級之後還能不能打字」要驗的正是原本那一條。
  ITfKeystrokeMgr* ks = nullptr;
  if (!plan.empty()) {
    hr = thread_mgr->QueryInterface(IID_ITfKeystrokeMgr, (void**)&ks);
    Step("QueryInterface(ITfKeystrokeMgr)", hr);
    if (SUCCEEDED(hr) && ks) {
      // ⚠ 送按鍵之前先把「TSF 覺得誰有焦點」印出來。
      //
      // ITfKeystrokeMgr 只會把按鍵交給**有執行緒焦點**的那一份文字服務。
      // IsThreadFocus 是 FALSE 的話,KeyDown 會乖乖回 S_OK / pfEaten=FALSE
      // 而完全不呼叫任何人 —— 看起來就像「輸入法不吃這顆鍵」,
      // 但其實它根本沒被問過。這兩件事要查的地方完全不同。
      BOOL thread_focus = FALSE;
      const HRESULT fhr = thread_mgr->IsThreadFocus(&thread_focus);
      Say("  IsThreadFocus = %d (hr=0x%08lX)\n", thread_focus ? 1 : 0,
          static_cast<unsigned long>(fhr));
      ITfDocumentMgr* focused = nullptr;
      const HRESULT ghr = thread_mgr->GetFocus(&focused);
      Say("  ThreadMgr::GetFocus = %p (hr=0x%08lX)%s\n",
          static_cast<void*>(focused), static_cast<unsigned long>(ghr),
          focused == docmgr ? " —— 就是我們那一份" : " —— **不是我們那一份**");
      if (focused) focused->Release();
      // 焦點不在我們身上就再要一次。有些系統要在 profile 啟用**之後**
      // 才吃得下 SetFocus。
      if (focused != docmgr && docmgr) {
        const HRESULT shr = thread_mgr->SetFocus(docmgr);
        Say("  重新 SetFocus hr=0x%08lX\n", static_cast<unsigned long>(shr));
        Pump(200);
      }
      // ── ⚠ 送按鍵之前把視窗前景搶回來 ────────────────────────────
      //
      // ActivateEx 到這裡之間會發生別的事:最重要的是**服務進程可能剛被
      // 我們自己啟動起來**,而它會建自己的視窗(設定視窗、系統匣)。
      // 那些視窗一出現就可能把前景拿走,而 TSF 只把按鍵交給擁有前景的
      // 那條執行緒 —— 沒有前景時 KeyDown 回 S_OK、pfEaten=FALSE,
      // 一顆鍵都不會到達文字服務,**而且不報錯**。
      //
      // 實測(CI run 31314468397 的 §5d):`前景視窗 = …030136
      // (我們的是 …05018E)`、`IsThreadFocus = 0`,六顆鍵一顆都沒到,
      // 而報表寫的是「第一次打字失敗」—— 一個看起來像產品壞掉的測試台問題。
      if (!thread_focus && hwnd) {
        ::ShowWindow(hwnd, SW_SHOWNORMAL);
        ::SetForegroundWindow(hwnd);
        ::SetActiveWindow(hwnd);
        ::SetFocus(hwnd);
        if (docmgr) thread_mgr->SetFocus(docmgr);
        Pump(300);
        BOOL again = FALSE;
        thread_mgr->IsThreadFocus(&again);
        Say("  重新搶前景之後 IsThreadFocus = %d(前景視窗 %p,我們的是 %p)\n",
            again ? 1 : 0, static_cast<void*>(::GetForegroundWindow()),
            static_cast<void*>(hwnd));
        if (!again)
          Fail("送按鍵之前仍然拿不到執行緒焦點 —— TSF 不會把任何按鍵交給\n"
               "     文字服務,所以底下量到的東西**不能拿來判斷產品好壞**。\n"
               "     這是測試台/工作階段的問題,不是輸入法的問題。");
      }
      if (check_preserved) CheckPreservedKey(ks);
      Say("\n--- 送按鍵 ---\n");
      for (const SeqKey& sk : plan) {
        const SendOutcome o = SendKeyThrough(ks, doc, sk);
        if (o.host_did) ++host_handled;
        // TestKeyDown 說吃、KeyDown 說不吃 = **這顆鍵在真實宿主上會消失**。
        // 這支假宿主因為有 HostDefaultAction 所以看不出來,但真的程式不會
        // 補救它 —— 所以在這裡明著數出來。
        // 這正是「可以打字,不能刪除」的形狀。
        if (o.test_eaten && !o.eaten) ++mismatch_count;

        // 這一行是給 verify_input_matrix.sh 解析的。欄位順序不要動。
        Say("  KEY %-8s vk=0x%02X test=%d down=%d host=%d doc=\"%s\"\n",
            Narrow(sk.label).c_str(), static_cast<unsigned>(sk.vk),
            o.test_eaten ? 1 : 0, o.eaten ? 1 : 0, o.host_did ? 1 : 0,
            Narrow(doc->text).c_str());
        if (o.eaten) ++eaten_count;
        Pump(60);
      }
    }
    Say("\n  文件內容 = \"%s\"\n", Narrow(doc->text).c_str());
    Say("  被吃掉的按鍵 = %d / %d(宿主自己處理了 %d 顆)\n", eaten_count,
        static_cast<int>(plan.size()), host_handled);
    Say("  DOC=\"%s\"\n", Narrow(doc->text).c_str());
    Say("  MISMATCH=%d\n", mismatch_count);
    if (mismatch_count > 0)
      Fail("有 %d 顆鍵是 TestKeyDown 說「吃」、KeyDown 說「不吃」。\n"
           "     在真的宿主裡那幾顆鍵會**直接消失**:宿主在測試那一趟就\n"
           "     放棄了自己的預設處理,而我們事後改口它收不到。\n"
           "     使用者回報的「可以打字,不能刪除」就是這個形狀。\n"
           "     修的地方是 windows/common/key_eat_policy.cc —— \n"
           "     沒有實作的鍵就不要在 OnTestKeyDown 宣告吃掉。",
           mismatch_count);
  }

  // 「一顆按鍵都沒有到達 OnTestKeyDown」與「到達了但沒吃」是兩件完全不同的事。
  // 前者要查 ActivateEx 那一段(key event sink 掛上了沒有),
  // 後者要查佈局或連線。把它們併成一句話,就是把人往錯的方向送。
  //
  // ⚠ 一定要**重新讀一次**記錄檔。上面那個 trace 是在送按鍵**之前**讀的
  //   (它要回答「ActivateEx 有沒有被呼叫」),按鍵行還沒寫進去。
  //   拿它來判斷「按鍵有沒有到達」會永遠說沒有 —— 實測 CI run #59 就是這樣:
  //   記錄檔裡明明有兩行 keysym=0x6E / 0x69,而這支程式報「一顆都沒到達」。
  //   一個讀舊資料的斷言比沒有斷言更糟:它會把人送去查一段其實是好的程式碼。
  const std::string trace_after_keys = ReadAll(trace_path);
  const bool saw_any_key =
      trace_after_keys.find("按鍵 vk=") != std::string::npos;
  if (!keys.empty() && !saw_any_key) {
    Fail("按鍵**一顆都沒有到達 OnTestKeyDown** —— 不是「不吃」,是根本沒收到。\n"
         "     要查的是 ActivateEx 裡的 AdviseKeyEventSink,不是佈局也不是連線。\n"
         "     記錄裡的 key sink 那一行會說掛上了沒有。");
    if (key_sink_bad || trace_after_keys.find("**沒掛上,收不到按鍵**") !=
                            std::string::npos)
      Fail("記錄明說 key event sink 兩種都掛不上");
  }
  if (require_eaten && eaten_count == 0 && saw_any_key)
    Fail("按鍵到達了 OnTestKeyDown,但一顆都沒有被吃掉。\n"
         "     照除錯記錄裡的 keysym 判斷是哪一段:\n"
         "       keysym=0x0  → 鍵盤佈局問不出字(win32_oracle.h)\n"
         "       keysym!=0   → 連不上服務(上面會有一行「連線失敗」)");

  if (have_expect_doc) {
    if (doc->text == expect_doc)
      Ok("文件內容 = \"%s\"(與預期相同)", Narrow(expect_doc).c_str());
    else
      Fail("文件內容 = \"%s\",預期 \"%s\"\n"
           "     ⚠ 斷言看的是**文件內容**,不是「有沒有被吃掉」——\n"
           "     「吃掉了但什麼都沒做」正是這一輪要抓的缺陷。",
           Narrow(doc->text).c_str(), Narrow(expect_doc).c_str());
  }

  if (!expect.empty()) {
    if (doc->text == expect)
      Ok("文件裡真的是「%s」—— 整條 TSF 路徑走通了", Narrow(expect).c_str());
    else
      Fail("文件裡是「%s」,預期「%s」", Narrow(doc->text).c_str(),
           Narrow(expect).c_str());
  }

  // ── 這個進程到底載入了哪一份 DLL ──────────────────────────────
  //
  // 升級之後同一台機器上會同時有兩份:新的在 rime_tsf.dll,舊的被改名成
  // rime_tsf.dll.old-<時間戳>(見 docs/decisions/no-restart.md)。
  // 「新開的進程有沒有用到新版」這個問題,只有在進程**裡面**答得出來。
  {
    HMODULE m = ::GetModuleHandleW(L"rime_tsf.dll");
    wchar_t p[MAX_PATH] = {0};
    if (m && ::GetModuleFileNameW(m, p, MAX_PATH))
      Say("  LOADED_TSF_DLL=%s\n", Narrow(p).c_str());
    else
      Say("  LOADED_TSF_DLL=(沒有載入)\n");
  }

  // ══════════════════════════════════════════════════════════════
  //  第二階段:升級發生在**這個進程還活著**的時候
  // ══════════════════════════════════════════════════════════════
  //
  // 使用者實測回報「我沒重啟也能用」。他機器上的真實狀態是:
  // 已經開著的程式握著**舊的** DLL 映像,而服務進程已經是**新的**。
  // 這一段就是把那個狀態做出來,然後問一句話:那樣還打得出字嗎?
  //
  // 走不通的症狀不是「壞掉」,是「有些程式能打字、有些不能」——
  // 而使用者完全無法理解為什麼。那比全部壞掉更難查。
  if (!phase2_go.empty()) {
    Say("\n══ 第二階段:舊的 DLL 映像 + 新的服務 ══\n");
    if (!ks || plan.empty()) {
      Fail("第二階段需要 --keys 或 --seq(要有東西可以打)");
    } else if (expect.empty()) {
      Fail("第二階段需要 --expect —— 沒有它就沒有斷言,那一格等於沒在測");
    } else {
      bool go = false;
      if (!phase2_ready.empty()) {
        if (TouchFile(phase2_ready))
          Say("  已寫出就緒訊號 %s(外面可以開始裝新版了)\n",
              Narrow(phase2_ready).c_str());
        else
          Fail("寫不出就緒訊號 %s —— 外面的腳本會一直等下去",
               Narrow(phase2_ready).c_str());
      }
      Say("  等 %s 出現(上限 %lu 毫秒)…\n", Narrow(phase2_go).c_str(),
          static_cast<unsigned long>(phase2_timeout_ms));
      const DWORD go_deadline = ::GetTickCount() + phase2_timeout_ms;
      for (;;) {
        if (FileThere(phase2_go)) {
          go = true;
          break;
        }
        if (::GetTickCount() >= go_deadline) break;
        // ⚠ 這裡要抽訊息,不能單純 Sleep。這個進程仍然是一個 TSF 宿主,
        //   而 TSF 有一部分工作靠視窗訊息推動 —— 幾分鐘不抽訊息的話,
        //   等升級結束之後它已經不是一個正常的宿主了,
        //   而後面量到的「連不回去」就會是我們自己造成的。
        Pump(200);
      }
      if (!go) {
        Fail("等不到 %s —— 外面那一步沒有完成,第二階段什麼都沒驗到",
             Narrow(phase2_go).c_str());
      } else {
        Say("  收到訊號:新版已經裝好,而這個進程手上的 DLL 映像還是舊的。\n");

        // ── ⚠ 先把前景搶回來 ──────────────────────────────────
        //
        // 等升級的那幾分鐘裡,別的東西可能拿走了前景(安裝程式、服務、
        // 系統的通知)。而 TSF 只把按鍵交給**擁有前景**的那條執行緒:
        // 沒有前景時 KeyDown 回 S_OK、pfEaten=FALSE,一顆鍵都不會到達
        // 文字服務,**而且不報錯**。
        //
        // 少了這一段,一次前景被搶走會被記成「舊的 DLL 連不回新的服務」——
        // 一個把人送去查版本協商、而其實那一段完全是好的的結論。
        // (實測過:第一版把這支宿主丟到背景跑,量到的正是那個假結論。)
        if (hwnd) {
          ::ShowWindow(hwnd, SW_SHOWNORMAL);
          ::SetForegroundWindow(hwnd);
          ::SetActiveWindow(hwnd);
          ::SetFocus(hwnd);
        }
        if (docmgr) thread_mgr->SetFocus(docmgr);
        Pump(300);
        {
          BOOL tf = FALSE;
          thread_mgr->IsThreadFocus(&tf);
          const HWND fg = ::GetForegroundWindow();
          Say("  PHASE2_THREAD_FOCUS=%d(前景視窗 %p,我們的是 %p)\n",
              tf ? 1 : 0, static_cast<void*>(fg), static_cast<void*>(hwnd));
          if (!tf)
            Fail("第二階段拿不到執行緒焦點 —— TSF 不會把任何按鍵交給文字服務,\n"
                 "     所以下面量到的東西**不能拿來判斷相容性**。\n"
                 "     這是測試台的問題,不是產品的問題。");
        }

        // ── 重新連上服務 ────────────────────────────────────────
        //
        // 升級把舊的服務進程停掉了。舊的 DLL 必須自己:
        //   開管道失敗 → 啟動新的 rime_service.exe → 重新握手
        //   (版本協商,必要時降級到 v1)→ 重新開 session。
        //
        // 用「送一顆 n 看它吃不吃」來問,而不是去讀什麼內部狀態:
        // 使用者感覺得到的就是這一件事。吃掉了代表整條路都通了。
        SeqKey probe_key;
        probe_key.label = L"n";
        probe_key.ch = L'n';
        SeqKey esc_key;
        const bool have_probe =
            AsciiToKey(L'n', &probe_key.vk, &probe_key.lparam);
        const bool have_esc = NamedToKey(L"ESC", &esc_key);
        if (!have_probe) Fail("這個佈局上打不出 'n',第二階段沒辦法探測");
        bool relinked = false;
        int probes = 0;
        const DWORD t0 = ::GetTickCount();
        const DWORD relink_deadline = t0 + phase2_relink_ms;
        while (have_probe && ::GetTickCount() < relink_deadline) {
          doc->text.clear();
          doc->sel_start = doc->sel_end = 0;
          ++probes;
          const SendOutcome o = SendKeyThrough(ks, doc, probe_key);
          if (o.eaten) {
            // 吃掉了 = 連上、握手過、session 開好、正在組字。
            // 把組字清掉再開始真正的那一趟。
            if (have_esc) SendKeyThrough(ks, doc, esc_key);
            relinked = true;
            break;
          }
          Pump(500);
        }
        doc->text.clear();
        doc->sel_start = doc->sel_end = 0;
        const DWORD relink_ms = ::GetTickCount() - t0;
        Say("  PHASE2_RELINK_MS=%lu\n", static_cast<unsigned long>(relink_ms));
        Say("  PHASE2_PROBES=%d\n", probes);

        if (!relinked) {
          Fail("**舊的 DLL 連不回新的服務**(試了 %d 次,%lu 毫秒)。\n"
               "     這正是使用者升級之後的狀態:已經開著的程式手上是舊的\n"
               "     DLL 映像,服務卻已經換成新的。連不回去的話,他看到的是\n"
               "     「有些程式能打字、有些不能」,而且完全無法理解為什麼。\n"
               "     要查三件事,而且順序就是這個:\n"
               "       1. 管道名有沒有跟著版本走(winshared/winutil.cc\n"
               "          的 RimePipeName —— 跟著版本走的話,舊 DLL 會去敲\n"
               "          一條不存在的管道,而錯誤碼是 2「找不到檔案」)\n"
               "       2. 握手的版本協商與降級重試(common/protocol.h 的\n"
               "          kMinProtocolVersion、tsf/ipc_client.cc 的 EnsureReady)\n"
               "       3. rime_shell 的 ABI —— 那一格**沒有降級的路**,\n"
               "          不合就是不合(ipc_client.cc 的 Handshake)\n"
               "     瘦 DLL 的除錯記錄裡那幾行「連線失敗」會直接說是哪一個。",
               probes, static_cast<unsigned long>(relink_ms));
        } else {
          Ok("舊的 DLL 重新連上了新的服務(%lu 毫秒、%d 次嘗試)",
             static_cast<unsigned long>(relink_ms), probes);
          Say("\n--- 第二階段送按鍵(同一段程式碼、同一個 ks)---\n");
          for (const SeqKey& sk : plan) {
            const SendOutcome o = SendKeyThrough(ks, doc, sk);
            Say("  KEY %-8s vk=0x%02X test=%d down=%d host=%d doc=\"%s\"\n",
                Narrow(sk.label).c_str(), static_cast<unsigned>(sk.vk),
                o.test_eaten ? 1 : 0, o.eaten ? 1 : 0, o.host_did ? 1 : 0,
                Narrow(doc->text).c_str());
            Pump(60);
          }
          Say("  PHASE2_DOC=\"%s\"\n", Narrow(doc->text).c_str());
          if (doc->text == expect)
            Ok("**升級之後,同一個進程用舊的 DLL 照樣打得出「%s」**",
               Narrow(expect).c_str());
          else
            Fail("第二階段打出來的是「%s」,預期「%s」——\n"
                 "     舊的 DLL 連上了新的服務,但打出來的東西不對。\n"
                 "     那比連不上更糟:使用者不會發現。",
                 Narrow(doc->text).c_str(), Narrow(expect).c_str());
        }
      }
    }
  }

  if (ks) ks->Release();

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
