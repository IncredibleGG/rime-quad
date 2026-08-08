#include "text_service.h"

#include <functional>
#include <new>

#include "../common/ime_policy.h"
#include "../winshared/winutil.h"
#include "guids.h"

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
  if (!dir.empty()) ipc_.SetServicePath(dir + L"\\rime_service.exe");
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
                                     DWORD /*flags*/) {
  RIME_GUARD_BEGIN
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

  // 刻意**不**在這裡連線服務。Activate 發生在使用者切到這個輸入法的當下,
  // 在宿主的 UI 執行緒上;那時去開管道(甚至啟動服務)會讓切換卡住。
  // 連線是第一次真的有按鍵時才做,而且有逾時與退避。
  return S_OK;
  RIME_GUARD_END_HR
}

STDMETHODIMP TextService::Deactivate() {
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
  if (MapKey(BuildKeyEvent(w, l, false), Oracle()).keysym == 0) return S_OK;
  if (!ipc_.EnsureReady()) return S_OK;
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

const KeyboardOracle& TextService::Oracle() {
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

}  // namespace rimewin
