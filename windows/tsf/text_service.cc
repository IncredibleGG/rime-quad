#include "text_service.h"

#include <functional>
#include <new>
#include <vector>

#include "../common/ime_policy.h"
#include "../winshared/winutil.h"
#include "guids.h"
#include "lang_bar.h"
#include "trace.h"

// DllMain 存下來的模組控制代碼。用來找到與 DLL 同目錄的 rime_service.exe。
extern HMODULE g_rime_module;
extern LONG g_rime_dll_refs;

namespace rimewin {
namespace {

// ⚠ C++ 例外穿過 COM 邊界是未定義行為,而在這裡「未定義」的實際長相
//   就是宿主進程崩潰 —— 而宿主可能是使用者正在編輯的文件。
//   每一個 COM 介面方法都必須擋一次。
#define RIME_GUARD_BEGIN try {
#define RIME_GUARD_END_HR                                                    \
  }                                                                          \
  catch (...) { return E_FAIL; }

// 跑一段同步的 edit session。
//
// final 不是裝飾:COM 介面沒有虛擬解構子,而這裡是 `delete this`。
// 標成 final 才能讓編譯器確定 `this` 就是最終型別 —— 否則某天有人繼承它,
// 就會變成從基底指標刪除衍生物件的未定義行為。
class FnEditSession final : public ITfEditSession {
 public:
  using Fn = std::function<HRESULT(TfEditCookie)>;
  explicit FnEditSession(Fn fn) : fn_(std::move(fn)) {}

  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession))
      *ppv = static_cast<ITfEditSession*>(this);
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
  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    try {
      return fn_(ec);
    } catch (...) {
      return E_FAIL;
    }
  }

 private:
  ~FnEditSession() = default;
  LONG ref_ = 1;
  Fn fn_;
};

// 由 TSF 給的 wParam / lParam 加一次 GetKeyboardState 組出 KeyEvent。
// OnTestKeyDown 與 OnKeyDown 必須用**同一份**組法,否則兩者對「這顆鍵映不映得出
// keysym」的判斷會不一致 —— 症狀是某些鍵在特定修飾鍵下悄悄失效。
KeyEvent BuildKeyEvent(WPARAM w, LPARAM l, bool key_up) {
  BYTE state[256] = {0};
  ::GetKeyboardState(state);
  KeyEvent e;
  e.vk = static_cast<uint32_t>(w);
  e.scan_code = static_cast<uint32_t>((l >> 16) & 0xFF);
  e.extended = (l & (1 << 24)) != 0;
  e.key_up = key_up;
  e.shift = (state[VK_SHIFT] & 0x80) != 0;
  e.ctrl = (state[VK_CONTROL] & 0x80) != 0;
  e.alt = (state[VK_MENU] & 0x80) != 0;
  e.win = ((state[VK_LWIN] | state[VK_RWIN]) & 0x80) != 0;
  e.caps_lock = (state[VK_CAPITAL] & 0x01) != 0;
  e.num_lock = (state[VK_NUMLOCK] & 0x01) != 0;
  e.right_alt = (state[VK_RMENU] & 0x80) != 0;
  return e;
}

// ── 背景把服務叫起來 ──────────────────────────────────────────────
//
// ⚠ 這條路徑修的是使用者實際回報的「**沒有任何 UI**」。
//
// 系統匣圖示與設定視窗都住在**服務進程**裡,而在這之前,服務唯一的啟動
// 時機是「第一顆按鍵走到 EnsureReady()」。於是只要按鍵那條路上任何一段
// 斷掉(例如佈局問不出 keysym,見 win32_oracle.h 檔頭),服務就**永遠不會
// 被啟動** —— 使用者看到的是「打不出字」**加上**「沒有任何 UI」,
// 兩個看起來無關的症狀,同一個根因。
//
// 把啟動搬到 ActivateEx(使用者切到這個輸入法的當下)之後:
//   · 系統匣圖示與設定視窗在切過去幾秒內就會出現,不必先打字;
//   · 首次部署(要編譯詞庫,好幾分鐘)提早開始,而不是等到使用者
//     第一次按鍵才開始 —— 那時他會以為輸入法壞了;
//   · 而「打不出字」如果還在,就**只剩**按鍵那條路可以查了。
//     把兩個症狀解耦,比同時修兩件事重要。
//
// ⚠ 一定要在**背景執行緒**上做。ActivateEx 跑在宿主的 UI 執行緒上,
//   在那裡 CreateProcess(甚至只是開一次登錄檔)都會讓切換輸入法卡一下,
//   而那是使用者每天做很多次的動作。
//
// ⚠ 執行緒活著的期間必須抓住 DLL 的參考計數(g_rime_dll_refs)。
//   少了它,宿主可能在 DllCanUnloadNow 回 S_OK 之後把 DLL 卸載掉,
//   而執行緒還在那段已經不存在的程式碼裡 —— 症狀是「切換輸入法時偶爾當掉」。
DWORD WINAPI ServiceStarterThread(LPVOID param) {
  std::wstring* path = static_cast<std::wstring*>(param);
  if (path) {
    if (!ServiceIsRunning()) {
      Trace("ActivateEx:服務沒在跑,背景啟動");
      LaunchService(*path);
    } else {
      Trace("ActivateEx:服務已經在跑");
    }
    delete path;
  }
  ::InterlockedDecrement(&g_rime_dll_refs);
  return 0;
}

// 每個宿主進程只做一次。
//
// 不是節流,是**正確性**:一個宿主裡可以有很多個 TextService 實例
// (每一條有輸入焦點的執行緒一個),而它們會在使用者每次切回這個輸入法時
// 各自 Activate 一遍。沒有這個旗標的話,瀏覽器切幾次分頁就會排出幾十條
// 執行緒,每一條都去問一次「服務在不在」。
LONG g_service_start_once = 0;

void StartServiceInBackground(const std::wstring& service_path) {
  if (service_path.empty()) return;
  if (::InterlockedCompareExchange(&g_service_start_once, 1, 0) != 0) return;
  // 提權的宿主一律不啟動(理由見 ipc_client.h 的 LaunchService)。
  // 在這裡就先擋掉,連執行緒都不必開。
  if (IsProcessElevated()) {
    Trace("ActivateEx:宿主是提權的,不啟動服務(刻意)");
    return;
  }
  std::wstring* copy = new (std::nothrow) std::wstring(service_path);
  if (!copy) return;
  ::InterlockedIncrement(&g_rime_dll_refs);
  HANDLE th = ::CreateThread(nullptr, 0, &ServiceStarterThread, copy, 0, nullptr);
  if (!th) {
    ::InterlockedDecrement(&g_rime_dll_refs);
    delete copy;
    return;
  }
  ::CloseHandle(th);
}

HRESULT RunSyncSession(ITfContext* ctx, TfClientId id, FnEditSession::Fn fn) {
  if (!ctx) return E_FAIL;
  FnEditSession* s = new (std::nothrow) FnEditSession(std::move(fn));
  if (!s) return E_OUTOFMEMORY;
  HRESULT session_hr = S_OK;
  // TF_ES_SYNC:按鍵處理必須在這一趟裡做完,不能等非同步的 session ——
  // 非同步回來時那顆按鍵早就被宿主處理過了,順序全亂。
  const HRESULT hr = ctx->RequestEditSession(
      id, s, TF_ES_SYNC | TF_ES_READWRITE, &session_hr);
  s->Release();
  if (FAILED(hr)) return hr;
  return session_hr;
}

}  // namespace

TextService::TextService() {
  ::InterlockedIncrement(&g_rime_dll_refs);
  const std::wstring dir = ModuleDirectory(g_rime_module);
  if (dir.empty()) {
    // 走到這裡的話,連服務執行檔的位置都算不出來 —— 輸入法必定完全沒作用,
    // 而且沒有任何其他跡象。一定要留一行。
    Trace("!! 算不出模組目錄,服務永遠不會被啟動");
  } else {
    service_path_ = dir + L"\\rime_service.exe";
    ipc_.SetServicePath(service_path_);
  }
}

TextService::~TextService() {
  if (thread_mgr_) Deactivate();
  ::InterlockedDecrement(&g_rime_dll_refs);
}

// ───────────────────────── IUnknown ─────────────────────────

STDMETHODIMP TextService::QueryInterface(REFIID riid, void** ppv) {
  if (!ppv) return E_INVALIDARG;
  *ppv = nullptr;
  if (IsEqualIID(riid, IID_IUnknown) ||
      IsEqualIID(riid, IID_ITfTextInputProcessor))
    *ppv = static_cast<ITfTextInputProcessor*>(this);
  else if (IsEqualIID(riid, IID_ITfTextInputProcessorEx))
    *ppv = static_cast<ITfTextInputProcessorEx*>(this);
  else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink))
    *ppv = static_cast<ITfThreadMgrEventSink*>(this);
  else if (IsEqualIID(riid, IID_ITfKeyEventSink))
    *ppv = static_cast<ITfKeyEventSink*>(this);
  else if (IsEqualIID(riid, IID_ITfCompositionSink))
    *ppv = static_cast<ITfCompositionSink*>(this);
  else if (IsEqualIID(riid, IID_ITfInputProcessorProfileActivationSink))
    *ppv = static_cast<ITfInputProcessorProfileActivationSink*>(this);
  if (!*ppv) return E_NOINTERFACE;
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) TextService::AddRef() {
  return static_cast<ULONG>(::InterlockedIncrement(&ref_));
}

STDMETHODIMP_(ULONG) TextService::Release() {
  const LONG n = ::InterlockedDecrement(&ref_);
  if (n == 0) delete this;
  return static_cast<ULONG>(n);
}

// ──────────────────── ITfTextInputProcessor(Ex)────────────────────

STDMETHODIMP TextService::Activate(ITfThreadMgr* mgr, TfClientId id) {
  return ActivateEx(mgr, id, 0);
}

STDMETHODIMP TextService::ActivateEx(ITfThreadMgr* mgr, TfClientId id,
                                     DWORD flags) {
  RIME_GUARD_BEGIN
  // ⚠ 這一行是「使用者切到我們的輸入法之後,系統到底有沒有把我們叫起來」
  //   唯一的答案。在這之前,整條路徑完全是紙上的 ——
  //   CI 驗得到註冊、驗得到引擎,就是驗不到這一格。
  //   (現在 windows/verify_tsf.sh 也驗得到了,靠的就是這一行。)
  Trace("ActivateEx 被呼叫 clientid=%lu flags=0x%lX 執行緒=%lu",
        static_cast<unsigned long>(id), static_cast<unsigned long>(flags),
        static_cast<unsigned long>(::GetCurrentThreadId()));
  if (!mgr) return E_INVALIDARG;
  thread_mgr_ = mgr;
  thread_mgr_->AddRef();
  client_id_ = id;

  ITfSource* source = nullptr;
  if (SUCCEEDED(thread_mgr_->QueryInterface(IID_ITfSource, (void**)&source))) {
    source->AdviseSink(IID_ITfThreadMgrEventSink,
                       static_cast<ITfThreadMgrEventSink*>(this),
                       &thread_mgr_cookie_);
    source->Release();
  }

  ITfKeystrokeMgr* keystroke = nullptr;
  if (SUCCEEDED(
          thread_mgr_->QueryInterface(IID_ITfKeystrokeMgr, (void**)&keystroke))) {
    keystroke->AdviseKeyEventSink(client_id_,
                                  static_cast<ITfKeyEventSink*>(this), TRUE);
    keystroke->Release();
  }

  // 語言設定檔變更的通知。見 text_service.h 的說明:三份設定檔共用一個
  // CLSID,所以切換語言時**只有**這一則通知會來。
  {
    ITfSource* psrc = nullptr;
    if (SUCCEEDED(thread_mgr_->QueryInterface(IID_ITfSource, (void**)&psrc))) {
      psrc->AdviseSink(IID_ITfInputProcessorProfileActivationSink,
                       static_cast<ITfInputProcessorProfileActivationSink*>(this),
                       &profile_sink_cookie_);
      psrc->Release();
    }
  }

  // 現在這一刻是哪一份。sink 只在**之後**的切換時才觸發,
  // 所以第一次一定要自己問一次 —— 少了這一次,使用者開機後
  // 第一個 app 裡的預設方案永遠是「不知道語言」的那一種。
  RefreshProfile();

  // 語言列上的設定按鈕。加不上去不是錯誤(某些宿主沒有語言列),
  // 系統匣那條路是獨立的。
  lang_bar_ = CreateLangBarButton([this]() { OpenSettings(); });
  if (lang_bar_ && !AddLangBarButton(thread_mgr_, lang_bar_)) {
    ReleaseLangBarButton(lang_bar_);
    lang_bar_ = nullptr;
  }
  Trace("語言列按鈕:%s", lang_bar_ ? "已加入" : "加不上(宿主沒有語言列?)");

  // 順手把目前的鍵盤佈局問一遍,結果寫進記錄。
  //
  // ⚠ 這一格是這一輪查到的關鍵。文字服務啟用時 GetKeyboardLayout(0) 拿到的
  //   不保證是一份真的鍵盤佈局(見 win32_oracle.h 檔頭),而它若問不出字,
  //   **每一顆按鍵都會被原樣放行、引擎一顆都收不到**,連線也永遠不會建立。
  //   在這裡問一次,是為了讓那件事在記錄裡是一行明確的話,
  //   而不是「使用者說不能打字」。
  {
    const Win32KeyboardOracle& w = Oracle();
    Trace("鍵盤佈局 hkl=0x%08llX%s%s altgr=%d",
          static_cast<unsigned long long>(
              reinterpret_cast<UINT_PTR>(w.requested_hkl())),
          w.used_fallback() ? " (問不出字,已改問 real hkl)" : "",
          w.blind() ? " **完全問不出字 —— 按鍵一顆都進不了引擎**" : "",
          w.HasAltGr() ? 1 : 0);
    if (w.used_fallback())
      Trace("  改用的佈局 hkl=0x%08llX",
            static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(w.hkl())));
  }

  // 刻意**不**在這裡連線服務:開管道與握手要在宿主的 UI 執行緒上等,
  // 那會讓切換輸入法卡住。連線仍然是第一次真的有按鍵時才做。
  //
  // 但**啟動**服務要在這裡做(而且是在背景執行緒上)——
  // 系統匣圖示與設定視窗住在服務進程裡,等到第一顆按鍵才啟動的話,
  // 按鍵那條路一斷,使用者就同時失去輸入與全部 UI。見上面
  // StartServiceInBackground 的說明。
  StartServiceInBackground(service_path_);
  return S_OK;
  RIME_GUARD_END_HR
}

STDMETHODIMP TextService::Deactivate() {
  Trace("Deactivate 被呼叫");
  if (lang_bar_) {
    RemoveLangBarButton(thread_mgr_, lang_bar_);
    ReleaseLangBarButton(lang_bar_);
    lang_bar_ = nullptr;
  }
  if (profile_sink_cookie_ != TF_INVALID_COOKIE && thread_mgr_) {
    ITfSource* psrc = nullptr;
    if (SUCCEEDED(thread_mgr_->QueryInterface(IID_ITfSource, (void**)&psrc))) {
      psrc->UnadviseSink(profile_sink_cookie_);
      psrc->Release();
    }
    profile_sink_cookie_ = TF_INVALID_COOKIE;
  }
  RIME_GUARD_BEGIN
  if (composition_ && composition_ctx_) {
    ITfContext* ctx = composition_ctx_;
    RunSyncSession(ctx, client_id_, [this](TfEditCookie ec) -> HRESULT {
      EndComposition(ec);
      return S_OK;
    });
  }
  // session 沒收乾淨的話,服務端會留著一個永遠不會再被用到的 librime session。
  ipc_.Close();

  if (thread_mgr_) {
    ITfKeystrokeMgr* keystroke = nullptr;
    if (SUCCEEDED(thread_mgr_->QueryInterface(IID_ITfKeystrokeMgr,
                                              (void**)&keystroke))) {
      keystroke->UnadviseKeyEventSink(client_id_);
      keystroke->Release();
    }
    if (thread_mgr_cookie_ != TF_INVALID_COOKIE) {
      ITfSource* source = nullptr;
      if (SUCCEEDED(thread_mgr_->QueryInterface(IID_ITfSource, (void**)&source))) {
        source->UnadviseSink(thread_mgr_cookie_);
        source->Release();
      }
      thread_mgr_cookie_ = TF_INVALID_COOKIE;
    }
    thread_mgr_->Release();
    thread_mgr_ = nullptr;
  }
  client_id_ = TF_CLIENTID_NULL;
  return S_OK;
  RIME_GUARD_END_HR
}

// ──────────────────── ITfThreadMgrEventSink ────────────────────

STDMETHODIMP TextService::OnInitDocumentMgr(ITfDocumentMgr*) { return S_OK; }
STDMETHODIMP TextService::OnUninitDocumentMgr(ITfDocumentMgr*) { return S_OK; }
STDMETHODIMP TextService::OnPushContext(ITfContext*) { return S_OK; }
STDMETHODIMP TextService::OnPopContext(ITfContext*) { return S_OK; }

STDMETHODIMP TextService::OnSetFocus(ITfDocumentMgr* focus,
                                     ITfDocumentMgr* /*prev*/) {
  RIME_GUARD_BEGIN
  // 換到別的輸入框:一定要把上一段組字收掉,否則它會留在原本的文件裡,
  // 而使用者接下來打的字會接在一段看起來已經沒有主人的 preedit 後面。
  if (composition_ && composition_ctx_) {
    ITfContext* ctx = composition_ctx_;
    RunSyncSession(ctx, client_id_, [this](TfEditCookie ec) -> HRESULT {
      SetCompositionText(ec, composition_ctx_, L"");
      EndComposition(ec);
      return S_OK;
    });
    Result r;
    ipc_.SendClear(&r);
  }
  ipc_.SendFocus(focus != nullptr);
  return S_OK;
  RIME_GUARD_END_HR
}

// ──────────────────── ITfKeyEventSink ────────────────────

STDMETHODIMP TextService::OnSetFocus(BOOL /*foreground*/) { return S_OK; }

STDMETHODIMP TextService::OnPreservedKey(ITfContext*, REFGUID, BOOL* eaten) {
  if (eaten) *eaten = FALSE;
  return S_OK;
}

STDMETHODIMP TextService::OnTestKeyDown(ITfContext* ctx, WPARAM w, LPARAM l,
                                        BOOL* eaten) {
  RIME_GUARD_BEGIN
  if (!eaten) return E_INVALIDARG;
  *eaten = FALSE;
  // OnTestKeyDown 不可以有副作用,所以這裡只做「有沒有可能是我們的」判斷:
  // 映射得出 keysym,而且連線是通的。
  //
  // 回 TRUE 但稍後 OnKeyDown 回 FALSE 是合法的 —— TSF 會把那顆鍵交回宿主。
  // 反過來(這裡回 FALSE)則 OnKeyDown 根本不會被呼叫,那顆鍵就永遠進不到引擎。
  // 所以這裡寧可寬鬆。
  const MappedKey mapped = MapKey(BuildKeyEvent(w, l, false), Oracle());
  const bool ready = mapped.keysym != 0 && ipc_.EnsureReady();
  if (key_trace_budget_ > 0) {
    --key_trace_budget_;
    // 三個欄位就足以指出斷在哪一段:
    //   keysym == 0        → 佈局那一段(按鍵根本沒進引擎,見 win32_oracle.h)
    //   keysym != 0, !ready → IPC 那一段(上面 EnsureReady 已經記了原因)
    //   兩者都好           → 這一顆真的進了引擎
    Trace("按鍵 vk=0x%02X scan=0x%02X keysym=0x%X mods=0x%X 吃掉=%d",
          static_cast<unsigned>(w),
          static_cast<unsigned>((l >> 16) & 0xFF),
          static_cast<unsigned>(mapped.keysym),
          static_cast<unsigned>(mapped.modifiers), ready ? 1 : 0);
    if (key_trace_budget_ == 0)
      Trace("(按鍵記錄額度用完,之後不再記 —— 它在宿主的 UI 執行緒上)");
  }
  if (!ready) return S_OK;
  *eaten = TRUE;
  (void)ctx;
  return S_OK;
  RIME_GUARD_END_HR
}

STDMETHODIMP TextService::OnKeyDown(ITfContext* ctx, WPARAM w, LPARAM l,
                                    BOOL* eaten) {
  RIME_GUARD_BEGIN
  if (!eaten) return E_INVALIDARG;
  *eaten = HandleKey(ctx, w, l, /*key_up=*/false) ? TRUE : FALSE;
  return S_OK;
  RIME_GUARD_END_HR
}

STDMETHODIMP TextService::OnTestKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* eaten) {
  // key-up 一律不吃。
  //
  // TSF 本來就不會把純修飾鍵(Shift / Ctrl)的事件交給 key event sink,
  // 所以 librime 那套「按一下 Shift 切中英」在這條路徑上做不到 ——
  // 那需要另外掛低階鍵盤 hook,不在本輪範圍。已列在 README 的已知缺口。
  if (eaten) *eaten = FALSE;
  return S_OK;
}

STDMETHODIMP TextService::OnKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* eaten) {
  if (eaten) *eaten = FALSE;
  return S_OK;
}

STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie /*ec*/,
                                                  ITfComposition* composition) {
  RIME_GUARD_BEGIN
  // 宿主自己把組字結束掉了(例如使用者用滑鼠點到別的位置)。
  // 引擎那邊也要跟著清掉,否則下一次按鍵會接在一段已經不存在的組字後面。
  if (composition_ == composition) {
    composition_->Release();
    composition_ = nullptr;
    if (composition_ctx_) {
      composition_ctx_->Release();
      composition_ctx_ = nullptr;
    }
    Result r;
    ipc_.SendClear(&r);
  }
  return S_OK;
  RIME_GUARD_END_HR
}

// ──────────────────── 內部 ────────────────────

const Win32KeyboardOracle& TextService::Oracle() {
  // ⚠ 每次都問一次目前執行緒的 HKL。使用者可以在任何時候切換鍵盤佈局
  //   (Win+空白鍵),而快取住第一次看到的那個,等於從此用錯的佈局解讀按鍵。
  const HKL hkl = ::GetKeyboardLayout(0);
  if (!oracle_ || oracle_hkl_ != hkl) {
    oracle_.reset(new Win32KeyboardOracle(hkl));
    oracle_hkl_ = hkl;
  }
  return *oracle_;
}

bool TextService::HandleKey(ITfContext* ctx, WPARAM w, LPARAM l, bool key_up) {
  const MappedKey mapped = MapKey(BuildKeyEvent(w, l, key_up), Oracle());
  if (mapped.keysym == 0) return false;

  if (!ipc_.EnsureReady()) return false;

  Result result;
  if (!ipc_.SendKey(mapped.keysym, mapped.modifiers, &result)) {
    // ⚠ 這裡是「按鍵永久變灰」的分岔點。拿不到結果就**放行**,
    //   不可以吃掉。使用者打不出中文會抱怨,打不出任何字則是電腦壞了。
    return false;
  }
  if (!result.handled) return false;

  pending_rect_ = false;
  const Snapshot snap = result.snap;
  RunSyncSession(ctx, client_id_, [this, ctx, &snap](TfEditCookie ec) -> HRESULT {
    return ApplyPlan(ec, ctx, snap);
  });

  // IPC 放在 edit session **之外**:session 期間持有文件鎖,在那裡等別的
  // 進程回話,等於把宿主的文件鎖交給另一個進程的排程。
  if (pending_rect_) {
    ipc_.SendCaretRect(pending_rect_value_.left, pending_rect_value_.top,
                       pending_rect_value_.right, pending_rect_value_.bottom);
    pending_rect_ = false;
  }
  return true;
}

HRESULT TextService::ApplyPlan(TfEditCookie ec, ITfContext* ctx,
                               const Snapshot& snap) {
  const HostPlan plan = PlanFromSnapshot(composition_ != nullptr, snap);
  const std::wstring commit = Utf8ToWide(plan.commit_text);
  const std::wstring preedit = Utf8ToWide(plan.preedit);

  switch (plan.action) {
    case DocAction::kNothing:
      break;
    case DocAction::kUpdate:
      if (FAILED(StartCompositionIfNeeded(ec, ctx))) return E_FAIL;
      SetCompositionText(ec, ctx, preedit);
      break;
    case DocAction::kCommitAndEnd:
    case DocAction::kCommitAndUpdate:
      if (composition_) {
        SetCompositionText(ec, ctx, commit);
        EndComposition(ec);
      } else {
        InsertText(ec, ctx, commit);
      }
      if (plan.action == DocAction::kCommitAndUpdate) {
        if (FAILED(StartCompositionIfNeeded(ec, ctx))) return E_FAIL;
        SetCompositionText(ec, ctx, preedit);
      }
      break;
    case DocAction::kEnd:
      if (composition_) {
        // 先把組字文字清空再結束。少了這一步,最後那一段 preedit 會
        // **留在文件裡**變成使用者沒打過的字。
        SetCompositionText(ec, ctx, L"");
        EndComposition(ec);
      }
      break;
  }

  if (plan.show_candidates) ReportCaretRect(ec, ctx);
  return S_OK;
}

HRESULT TextService::StartCompositionIfNeeded(TfEditCookie ec, ITfContext* ctx) {
  if (composition_) return S_OK;
  if (!ctx) return E_FAIL;

  ITfInsertAtSelection* ias = nullptr;
  if (FAILED(ctx->QueryInterface(IID_ITfInsertAtSelection, (void**)&ias)))
    return E_FAIL;
  ITfRange* range = nullptr;
  HRESULT hr = ias->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, &range);
  ias->Release();
  if (FAILED(hr) || !range) return E_FAIL;

  ITfContextComposition* cc = nullptr;
  hr = ctx->QueryInterface(IID_ITfContextComposition, (void**)&cc);
  if (SUCCEEDED(hr)) {
    hr = cc->StartComposition(ec, range, static_cast<ITfCompositionSink*>(this),
                              &composition_);
    cc->Release();
  }
  range->Release();
  if (FAILED(hr) || !composition_) return E_FAIL;

  composition_ctx_ = ctx;
  composition_ctx_->AddRef();
  return S_OK;
}

HRESULT TextService::SetCompositionText(TfEditCookie ec, ITfContext* ctx,
                                        const std::wstring& text) {
  if (!composition_ || !ctx) return E_FAIL;
  ITfRange* range = nullptr;
  if (FAILED(composition_->GetRange(&range)) || !range) return E_FAIL;

  HRESULT hr = range->SetText(ec, 0, text.c_str(), static_cast<LONG>(text.size()));
  if (SUCCEEDED(hr)) {
    ITfRange* caret = nullptr;
    if (SUCCEEDED(range->Clone(&caret)) && caret) {
      caret->Collapse(ec, TF_ANCHOR_END);
      TF_SELECTION sel{};
      sel.range = caret;
      sel.style.ase = TF_AE_NONE;
      sel.style.fInterimChar = FALSE;
      ctx->SetSelection(ec, 1, &sel);
      caret->Release();
    }
  }
  range->Release();
  return hr;
}

HRESULT TextService::InsertText(TfEditCookie ec, ITfContext* ctx,
                                const std::wstring& text) {
  if (!ctx || text.empty()) return S_OK;
  ITfInsertAtSelection* ias = nullptr;
  if (FAILED(ctx->QueryInterface(IID_ITfInsertAtSelection, (void**)&ias)))
    return E_FAIL;
  ITfRange* range = nullptr;
  const HRESULT hr = ias->InsertTextAtSelection(
      ec, 0, text.c_str(), static_cast<LONG>(text.size()), &range);
  ias->Release();
  if (SUCCEEDED(hr) && range) {
    range->Collapse(ec, TF_ANCHOR_END);
    TF_SELECTION sel{};
    sel.range = range;
    sel.style.ase = TF_AE_NONE;
    sel.style.fInterimChar = FALSE;
    ctx->SetSelection(ec, 1, &sel);
  }
  if (range) range->Release();
  return hr;
}

void TextService::EndComposition(TfEditCookie ec) {
  if (composition_) {
    composition_->EndComposition(ec);
    composition_->Release();
    composition_ = nullptr;
  }
  if (composition_ctx_) {
    composition_ctx_->Release();
    composition_ctx_ = nullptr;
  }
}

void TextService::ReportCaretRect(TfEditCookie ec, ITfContext* ctx) {
  if (!ctx) return;
  ITfContextView* view = nullptr;
  if (FAILED(ctx->GetActiveView(&view)) || !view) return;

  ITfRange* range = nullptr;
  if (composition_) {
    composition_->GetRange(&range);
  } else {
    TF_SELECTION sel{};
    ULONG fetched = 0;
    if (SUCCEEDED(ctx->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched)) &&
        fetched == 1) {
      range = sel.range;  // 所有權轉移過來
    }
  }

  RECT rc{};
  BOOL clipped = FALSE;
  bool got = false;
  if (range && SUCCEEDED(view->GetTextExt(ec, range, &rc, &clipped)) &&
      (rc.right != rc.left || rc.bottom != rc.top)) {
    got = true;
  }
  if (!got) {
    // GetTextExt 在不少宿主上會失敗或給出空矩形(尤其是還沒有任何文字時)。
    // 退回宿主視窗的左上角:候選窗位置不理想,總比跑到螢幕原點好。
    HWND hwnd = nullptr;
    if (SUCCEEDED(view->GetWnd(&hwnd)) && hwnd) {
      RECT wr{};
      if (::GetWindowRect(hwnd, &wr)) {
        rc.left = wr.left + 8;
        rc.top = wr.top + 8;
        rc.right = rc.left + 1;
        rc.bottom = rc.top + 20;
        got = true;
      }
    }
  }
  if (got) {
    pending_rect_ = true;
    pending_rect_value_ = rc;
  }
  if (range) range->Release();
  view->Release();
}

// ── 語言設定檔 ────────────────────────────────────────────────────

STDMETHODIMP TextService::OnActivated(DWORD /*profile_type*/, LANGID langid,
                                      REFCLSID clsid, REFGUID /*catid*/,
                                      REFGUID guid_profile, HKL /*hkl*/,
                                      DWORD flags) {
  RIME_GUARD_BEGIN
  // 只理會**我們自己**被啟用的那一則。別的輸入法被啟用時我們什麼都不做:
  // 那時使用者根本沒在用這個輸入法,改自己的狀態沒有意義,
  // 而且會讓「他上次用哪個方案」被別人的切換覆蓋掉。
  if (!IsEqualCLSID(clsid, CLSID_RimeTextService)) return S_OK;
  if (!(flags & TF_IPSINK_FLAG_ACTIVE)) return S_OK;
  // 這條通知是「使用者從繁體切到簡體」唯一的管道(三份設定檔共用一個 CLSID,
  // 所以 TSF 不會重新 Activate)。在這之前它整條是紙上的。
  Trace("profile sink:啟用 langid=0x%04X", static_cast<unsigned>(langid));
  ipc_.SetProfile(static_cast<uint32_t>(langid), GuidToUtf8(guid_profile));
  return S_OK;
  RIME_GUARD_END_HR
}

void TextService::RefreshProfile() {
  // 首選:ITfInputProcessorProfileMgr::GetActiveProfile —— 它同時給
  // langid 與 profile GUID,而且問的是「現在真的啟用的是哪一份」。
  ITfInputProcessorProfiles* profiles = nullptr;
  if (FAILED(::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_ITfInputProcessorProfiles,
                                (void**)&profiles)) ||
      !profiles) {
    // ⚠ 退路:HKL 的低 16 位就是目前輸入語言的 langid。
    //   拿不到 profile GUID,但 langid 已經足夠決定方案 ——
    //   而「拿不到就什麼都不做」等於讓簡體使用者繼續看到繁體字。
    const HKL hkl = ::GetKeyboardLayout(0);
    const uint32_t lang =
        static_cast<uint32_t>(reinterpret_cast<UINT_PTR>(hkl) & 0xFFFFu);
    Trace("語言設定檔:第3層(HKL 低位字)langid=0x%04X —— "
          "連 CLSID_TF_InputProcessorProfiles 都建不出來",
          static_cast<unsigned>(lang));
    ipc_.SetProfile(lang, std::string());
    return;
  }

  uint32_t langid = 0;
  std::string guid;
  // 走到哪一層。README 記著「三層退路一層都沒被執行過一次」——
  // 少了這一行,那句話下一輪還是一樣。
  const char* layer = "?";
  ITfInputProcessorProfileMgr* mgr = nullptr;
  if (SUCCEEDED(profiles->QueryInterface(IID_ITfInputProcessorProfileMgr,
                                         (void**)&mgr)) &&
      mgr) {
    TF_INPUTPROCESSORPROFILE prof{};
    if (SUCCEEDED(mgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &prof)) &&
        IsEqualCLSID(prof.clsid, CLSID_RimeTextService)) {
      langid = static_cast<uint32_t>(prof.langid);
      guid = GuidToUtf8(prof.guidProfile);
      layer = "第1層 ProfileMgr::GetActiveProfile";
    }
    mgr->Release();
  }
  if (langid == 0) {
    // 沒有 ProfileMgr(較舊的系統),或啟用的不是我們 —— 後者在
    // Activate 的當下是可能的,系統的快取還沒更新(見 README:
    // 註冊完 0.12 秒時列舉不到自己,22 秒後才看得到)。
    LANGID cur = 0;
    if (SUCCEEDED(profiles->GetCurrentLanguage(&cur))) {
      langid = static_cast<uint32_t>(cur);
      layer = "第2層 GetCurrentLanguage(啟用的還不是我們 / CTF 快取沒跟上)";
    }
  }
  profiles->Release();
  if (langid == 0) {
    const HKL hkl = ::GetKeyboardLayout(0);
    langid = static_cast<uint32_t>(reinterpret_cast<UINT_PTR>(hkl) & 0xFFFFu);
    layer = "第3層 HKL 低位字";
  }
  Trace("語言設定檔:%s langid=0x%04X guid=%s", layer,
        static_cast<unsigned>(langid), guid.empty() ? "(沒有)" : guid.c_str());
  ipc_.SetProfile(langid, guid);
}

void TextService::OpenSettings() {
  // 路 1:管道已經連上 → 走 IPC。UWP / 市集 App 的宿主跑在 AppContainer 裡,
  // 開不了 Local\ 底下別人建立的具名物件,那時只有這一條走得通。
  if (ipc_.SendOpenSettings()) {
    Trace("設定按鈕:路1(IPC)");
    return;
  }

  // 路 2:具名事件。使用者還沒打過任何一個字時管道沒連,只有這一條走得通。
  HANDLE ev = ::OpenEventW(EVENT_MODIFY_STATE, FALSE,
                           RimeSettingsEventName().c_str());
  if (ev) {
    ::SetEvent(ev);
    ::CloseHandle(ev);
    Trace("設定按鈕:路2(具名事件)");
    return;
  }
  Trace("設定按鈕:路3(CreateProcess)");

  // 路 3:服務根本沒在跑 → 把它叫起來,直接開設定視窗。
  // ⚠ 提權的宿主不可以啟動服務(見 ipc_client.cc 的說明:那會產生一支
  //   提權的服務,把使用者詞庫檔案的擁有者換掉)。所以這裡什麼都不做 ——
  //   使用者在提權視窗裡本來就沒有輸入法。
  if (IsProcessElevated()) return;
  std::wstring exe = ModuleDirectory(g_rime_module);
  if (exe.empty()) return;
  exe += L"\\rime_service.exe";
  std::wstring cmd = L"\"" + exe + L"\" --settings";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');
  if (::CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
  }
}

}  // namespace rimewin
