#include "text_service.h"

#include "../common/ui_strings.h"

#include <functional>
#include <new>
#include <vector>

#include "../common/ime_policy.h"
#include "../common/hotkey_policy.h"
#include "../common/key_eat_policy.h"
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

// 這個宿主可不可以啟動服務。判準與理由見 common/elevation_policy.h。
//
// ⚠ 進程的一生中不會變,所以問一次就好 —— 但**不要**在 DllMain 裡問
//   (那裡是載入器鎖)。第一次呼叫發生在 ActivateEx 或語言列重畫,
//   兩者都遠在載入器鎖之外。
HostElevation HostElevationOnce() {
  static const HostElevation e = ClassifyHostElevation();
  return e;
}

void StartServiceInBackground(const std::wstring& service_path) {
  if (service_path.empty()) return;
  if (::InterlockedCompareExchange(&g_service_start_once, 1, 0) != 0) return;
  // ⚠ 這裡以前是 `if (IsProcessElevated())` —— 一句「提權就不啟動」。
  //
  //   理由是對的(提權的服務會把使用者詞庫的擁有者換掉),但涵蓋過頭:
  //   內建 Administrator 帳號與關掉 UAC 的機器上,**每一個進程都是提權的**,
  //   於是服務永遠不會被啟動 —— 沒有系統匣圖示、沒有設定視窗、打不出字,
  //   三個症狀一個原因,而且沒有任何錯誤訊息。實測回報就是這一種。
  //
  //   現在問的是正確的問題:「這個使用者在這個工作階段裡,還有沒有另一份
  //   非提權的身分會被我們弄壞?」見 common/elevation_policy.h。
  const HostElevation elev = HostElevationOnce();
  if (!MayStartUserService(elev)) {
    Trace("ActivateEx:不啟動服務(刻意):%s / %s", HostElevationTag(elev),
          HostElevationZh(elev));
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
    const HRESULT hr = source->AdviseSink(
        IID_ITfThreadMgrEventSink, static_cast<ITfThreadMgrEventSink*>(this),
        &thread_mgr_cookie_);
    if (FAILED(hr)) Trace("!! ThreadMgr sink 掛不上 hr=0x%08lX", (unsigned long)hr);
    source->Release();
  }

  // ══ key event sink ═══════════════════════════════════════════════
  //
  // ⚠⚠ **這裡的回傳值以前沒有人看。** 而它失敗的話,後果是整個輸入法
  //     安靜地不存在:
  //
  //       AdviseKeyEventSink 失敗 → 我們的 OnTestKeyDown / OnKeyDown
  //       **從來不會被呼叫** → 引擎一顆按鍵都收不到 → 連線不建立
  //       → 服務不會被啟動 → 沒有系統匣圖示、沒有設定視窗
  //
  //     也就是與「佈局問不出字」一模一樣的症狀組合,而且一樣沒有錯誤訊息:
  //     `ActivateEx` 照樣回 S_OK,語言列按鈕照樣加得上,使用者看到輸入法
  //     好端端地在清單上、切得過去、然後什麼都不會發生。
  //
  // fForeground = TRUE 要求**呼叫執行緒擁有前景**。文件是這樣寫的,而
  // 微軟自己的 SampleIME 也傳 TRUE —— 問題是那個前提不是永遠成立的
  // (沒有互動式桌面的工作階段、被遠端桌面接管、宿主在背景時被啟用…)。
  // 前景 sink 拿不到就退一步用非前景的:少的是「別的 TIP 也在時的優先權」,
  // 而那遠好過**一顆按鍵都收不到**。
  {
    ITfKeystrokeMgr* keystroke = nullptr;
    const HRESULT qhr =
        thread_mgr_->QueryInterface(IID_ITfKeystrokeMgr, (void**)&keystroke);
    if (SUCCEEDED(qhr) && keystroke) {
      HRESULT ahr = keystroke->AdviseKeyEventSink(
          client_id_, static_cast<ITfKeyEventSink*>(this), TRUE);
      if (FAILED(ahr)) {
        const HRESULT first = ahr;
        ahr = keystroke->AdviseKeyEventSink(
            client_id_, static_cast<ITfKeyEventSink*>(this), FALSE);
        Trace("key sink:前景版失敗 hr=0x%08lX,退成非前景 hr=0x%08lX",
              (unsigned long)first, (unsigned long)ahr);
      } else {
        Trace("key sink:已掛上(前景)");
      }
      key_sink_ok_ = SUCCEEDED(ahr);
      if (!key_sink_ok_)
        Trace("!! key sink 兩種都掛不上 —— 這個宿主裡一顆按鍵都收不到");
      keystroke->Release();
    } else {
      Trace("!! 拿不到 ITfKeystrokeMgr hr=0x%08lX —— 收不到任何按鍵",
            (unsigned long)qhr);
    }
  }

  // ══ 保留鍵:Ctrl+空白鍵切中英 ════════════════════════════════════
  //
  // 使用者原話:「ctrl+ 空格沒辦法切中英文 這個應該是所有輸入法的
  // 基本配置」。做法與**不可以**用的做法,理由都在
  // common/hotkey_policy.h 的檔頭:
  //
  //   · **不掛 WH_KEYBOARD_LL。** 那會看到使用者在每一個程式裡的
  //     每一次按鍵,與「離線、經得起審計」的定位直接衝突。
  //   · **不動 OnTestKeyDown / key_eat_policy。** 那張真值表是
  //     「不要再吃掉別人的鍵」的唯一防線,而 Ctrl+C 曾經真的被吃掉過。
  //
  // PreserveKey 只在**我們自己的文字服務被啟用時**生效,而且只有命中的
  // 那一顆會經由 OnPreservedKey 回來 —— 其餘按鍵我們一個都看不到。
  //
  // ⚠ 失敗不是致命的(別的 TIP 可能已經佔走這個組合)。記下來就好:
  //   使用者仍然點得到那一橫的第一格,而記錄會說明為什麼熱鍵沒反應。
  {
    ITfKeystrokeMgr* keystroke = nullptr;
    if (SUCCEEDED(thread_mgr_->QueryInterface(IID_ITfKeystrokeMgr,
                                              (void**)&keystroke)) &&
        keystroke) {
      TF_PRESERVEDKEY pk{};
      pk.uVKey = VK_SPACE;
      pk.uModifiers = TF_MOD_CONTROL;
      static const WCHAR kDesc[] = L"LuminaKey: Chinese/English";
      const HRESULT hr = keystroke->PreserveKey(
          client_id_, GUID_RimePreservedKeyToggle, &pk, kDesc,
          (ULONG)(sizeof(kDesc) / sizeof(kDesc[0]) - 1));
      preserved_key_ok_ = SUCCEEDED(hr);
      Trace("保留鍵 Ctrl+Space:%s hr=0x%08lX",
            preserved_key_ok_ ? "已註冊" : "註冊失敗(可能被別的輸入法佔走)",
            (unsigned long)hr);
      keystroke->Release();
    }
  }

  // 語言設定檔變更的通知。見 text_service.h 的說明:三份設定檔共用一個
  // CLSID,所以切換語言時**只有**這一則通知會來。
  {
    ITfSource* psrc = nullptr;
    if (SUCCEEDED(thread_mgr_->QueryInterface(IID_ITfSource, (void**)&psrc))) {
      const HRESULT hr = psrc->AdviseSink(
          IID_ITfInputProcessorProfileActivationSink,
          static_cast<ITfInputProcessorProfileActivationSink*>(this),
          &profile_sink_cookie_);
      if (FAILED(hr))
        Trace("!! profile sink 掛不上 hr=0x%08lX —— "
              "使用者從繁體切到簡體會完全沒有效果", (unsigned long)hr);
      psrc->Release();
    }
  }

  // 現在這一刻是哪一份。sink 只在**之後**的切換時才觸發,
  // 所以第一次一定要自己問一次 —— 少了這一次,使用者開機後
  // 第一個 app 裡的預設方案永遠是「不知道語言」的那一種。
  RefreshProfile();

  // 語言列上的設定按鈕。加不上去不是錯誤(某些宿主沒有語言列),
  // 系統匣那條路是獨立的。
  //
  // ⚠ 第二個參數是「拒絕啟動服務」唯一一個使用者看得到的出口。
  //   條件是**兩個都成立**才示警:我們不會啟動它,而且它現在也沒在跑。
  //   少了後半條的話,分裂權杖的機器上(提權視窗 + 一般視窗並存,
  //   而服務由一般視窗那一邊帶起來了)使用者會在提權視窗裡看到一個
  //   「未啟動」的警告,而輸入法明明好端端地能用 —— 那是在製造假警報,
  //   而假警報會讓真警報失去意義。
  lang_bar_ = CreateLangBarButton(
      [this]() { OpenSettings(); },
      []() -> const wchar_t* {
        const HostElevation e = HostElevationOnce();
        if (MayStartUserService(e)) return nullptr;
        if (ServiceIsRunning()) return nullptr;
        return HostElevationTooltipW(e);
      });
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
  // 一行把「這個宿主裡到底能不能用」講完。使用者回報時,這一行就是答案。
  Trace("ActivateEx 完成:key sink=%s 語言列=%s 服務路徑=%s",
        key_sink_ok_ ? "OK" : "**沒掛上,收不到按鍵**",
        lang_bar_ ? "OK" : "沒有",
        service_path_.empty() ? "(算不出來)" : "OK");
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
      // ⚠ 保留鍵一定要還回去。不還的話,這個宿主接下來的 Ctrl+空白鍵
      //   會被一個已經不存在的文字服務攔著 —— 使用者看到的是
      //   「某個程式裡空白鍵偶爾失靈」,而那查不到我們頭上。
      if (preserved_key_ok_) {
        TF_PRESERVEDKEY pk{};
        pk.uVKey = VK_SPACE;
        pk.uModifiers = TF_MOD_CONTROL;
        keystroke->UnpreserveKey(GUID_RimePreservedKeyToggle, &pk);
        preserved_key_ok_ = false;
      }
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
    // 引擎手上的輸入被清掉了 —— Composing() 必須跟著回到 false,
    // 否則退格會繼續被當成「組字中的鍵」而吃掉,使用者就刪不掉字。
    engine_composing_ = false;
  }
  ipc_.SendFocus(focus != nullptr);
  return S_OK;
  RIME_GUARD_END_HR
}

// ──────────────────── ITfKeyEventSink ────────────────────

STDMETHODIMP TextService::OnSetFocus(BOOL /*foreground*/) { return S_OK; }

STDMETHODIMP TextService::OnPreservedKey(ITfContext* ctx, REFGUID guid,
                                        BOOL* eaten) {
  RIME_GUARD_BEGIN
  if (eaten) *eaten = FALSE;
  if (!IsEqualGUID(guid, GUID_RimePreservedKeyToggle)) return S_OK;

  // ⚠ 連不上服務就**不要**宣稱吃掉這顆鍵。宿主的 Ctrl+空白鍵可能有
  //   它自己的用途(有些編輯器是自動完成),吃掉又不做事就是一顆
  //   壞掉的鍵 —— 這正是 key_eat_policy.h 檔頭那一課。
  if (!ipc_.EnsureReady()) {
    Trace("保留鍵 Ctrl+Space:連不上服務,放行給宿主");
    return S_OK;
  }

  // 送的是**一顆按鍵**,不是一個新的協議操作:服務端用同一份
  // common/hotkey_policy.cc 認出它,所以線路格式一個位元都沒有變
  // (舊的服務配新的 DLL 仍然連得起來,只是那顆鍵會被交給 librime,
  //  而 librime 不處理 Control+space —— 也就是「沒有作用」,不是壞掉)。
  Result r;
  if (!ipc_.SendKey(AsciiToggleKeysym(), AsciiToggleModifiers(), &r)) {
    Trace("保留鍵 Ctrl+Space:送不出去,放行給宿主");
    return S_OK;
  }
  if (eaten) *eaten = r.handled ? TRUE : FALSE;
  if (!r.handled) {
    // 服務沒有處理它:舊版的服務(不認得這個熱鍵,交給了 librime),
    // 或者首次部署還沒做完(engine.cc 的 ToggleAsciiMode 會這樣回)。
    // 上面已經宣告**沒有**吃掉這顆鍵,所以這裡也不可以動文件 ——
    // 動了就是「宿主與我們同時處理同一顆鍵」。
    Trace("保留鍵 Ctrl+Space:服務沒有處理(部署中?舊服務?),放行給宿主");
    return S_OK;
  }

  // ══ ⚠ 這一份快照**不可以**丟掉 ══════════════════════════════════
  //
  // 舊版這裡寫著「中英切換不會產生上屏文字,也不會動組字」。**那句話是
  // 錯的**,而且錯在最要命的地方:common/hotkey_policy.h 的檔頭寫著這顆鍵
  // 存在的理由就是「中英切換發生在句子中間」——
  // 也就是說按下去的當下,引擎手上幾乎一定握著一段組字。
  //
  // librime 切到英數模式時會把那一段**上屏並清掉**(真機實測:
  // preedit="ni hao" → handled=1 has_commit=1 commit="nihao" 組字中=0)。
  // 而共用層的契約是:**commit 在 rs_snapshot_acquire 的當下就被消費**,
  // 不是在 release(service/engine.cc 的 TakeSnapshotLocked 檔頭)。
  // 也就是說這一份快照是那段文字唯一的一次現身。
  //
  // 把它丟掉的後果有三個,而且三個都是靜默的:
  //   (a) 使用者打到一半的字**永久消失**;
  //   (b) 螢幕上那段底線組字留著不收尾;
  //   (c) engine_composing_ 停在 true —— 見 Composing() 的說明,
  //       那之後每一顆退格都會被宣告吃掉又沒有人處理,那一格就是黑洞。
  //
  // 走的是與 HandleKey **完全同一段** ApplyPlan:兩邊各寫一份收尾邏輯
  // 就是兩份真相,而漂移的樣子正是這一類「畫面看起來正常、東西不見了」。
  //
  // ⚠ 目標 context 取 composition_ctx_ 優先:組字是開在**那一份**文件上的,
  //   而 TSF 交進來的 ctx 不保證是同一個。拿錯的話 SetCompositionText 會
  //   在別人的文件上動選取範圍。
  ITfContext* target = composition_ ? composition_ctx_ : ctx;
  if (target) {
    pending_rect_ = false;
    Snapshot snap = r.snap;
    snap.has_commit = false;   // [植入 M1]
    snap.commit_text.clear();  // [植入 M1]
    const HRESULT shr =
        RunSyncSession(target, client_id_,
                       [this, target, &snap](TfEditCookie ec) -> HRESULT {
                         return ApplyPlan(ec, target, snap);
                       });
    if (FAILED(shr)) {
      // edit session 沒跑成(宿主拒絕同步的讀寫鎖)。ApplyPlan 一步都沒做,
      // 而引擎那一側的組字**已經**被切中英清掉了 —— 這裡至少要把
      // engine_composing_ 拉回來,否則退格從此掉進黑洞(見 Composing())。
      engine_composing_ = (r.snap.status_flags & kStComposing) != 0;
      Trace("保留鍵 Ctrl+Space:edit session 失敗 hr=0x%08lX,上屏文字沒寫進文件",
            static_cast<unsigned long>(shr));
    }
    // IPC 放在 edit session **之外**:session 期間持有文件鎖(同 HandleKey)。
    if (pending_rect_) {
      ipc_.SendCaretRect(pending_rect_value_.left, pending_rect_value_.top,
                         pending_rect_value_.right, pending_rect_value_.bottom);
      pending_rect_ = false;
    }
  } else {
    // 沒有文件可以動(TSF 在焦點不在任何輸入框時也會把保留鍵送進來)。
    // 至少讓 engine_composing_ 跟上引擎 —— 少了這一行,上面 (c) 那個
    // 黑洞照樣會發生,而且完全沒有跡象。
    engine_composing_ = (r.snap.status_flags & kStComposing) != 0;
    Trace("保留鍵 Ctrl+Space:沒有 context 可以套用快照(has_commit=%d)",
          r.snap.has_commit ? 1 : 0);
  }

  Trace("保留鍵 Ctrl+Space:已切換 handled=%d 英數=%d 組字中=%d has_commit=%d",
        r.handled ? 1 : 0, (r.snap.status_flags & kStAsciiMode) ? 1 : 0,
        (r.snap.status_flags & kStComposing) ? 1 : 0, r.snap.has_commit ? 1 : 0);
  return S_OK;
  RIME_GUARD_END_HR
}

STDMETHODIMP TextService::OnTestKeyDown(ITfContext* ctx, WPARAM w, LPARAM l,
                                        BOOL* eaten) {
  RIME_GUARD_BEGIN
  if (!eaten) return E_INVALIDARG;
  *eaten = FALSE;
  // OnTestKeyDown 不可以有副作用,所以這裡只做「這顆鍵是不是我們的」判斷。
  //
  // ⚠⚠ **這一趟說「吃」,就等於承諾了 OnKeyDown 會處理它。**
  //
  //   舊版的註解寫「回 TRUE 但稍後 OnKeyDown 回 FALSE 是合法的 —— TSF 會把
  //   那顆鍵交回宿主」。**那句話是錯的。** 宿主(以及 Windows 給非 TSF
  //   感知程式用的 CUAS 相容層)在這一趟聽到「吃」的當下就放棄了自己的
  //   預設處理;事後改口說沒吃時,那顆鍵已經回不去了。
  //
  //   使用者回報的「可以打字,不能刪除」就是這樣來的:退格映得出 keysym、
  //   連線也是通的,於是這裡說吃;引擎在沒有組字時不處理退格,於是
  //   OnKeyDown 說不吃 —— 那顆鍵掉進兩邊中間的黑洞。
  //
  //   所以現在多問一個問題:**這顆鍵在目前的狀態下,真的是我們的嗎?**
  //   規則(以及為什麼字元鍵與功能鍵的規則不一樣)在
  //   common/key_eat_policy.h 的檔頭。
  //
  // 走到這裡就代表 key event sink 是好的 —— 它沒掛上的話這個函式
  // 根本不會被呼叫。所以「記錄裡完全沒有按鍵行」與「按鍵行的 keysym 是 0」
  // 是兩件完全不同的事,前者要查的是 ActivateEx 那一段。
  const KeyPlan plan = PlanKey(w, l, /*key_up=*/false);
  const bool ready = plan.eat && ipc_.EnsureReady();
  if (key_trace_budget_ > 0) {
    --key_trace_budget_;
    // 五個欄位就足以指出斷在哪一段:
    //   keysym == 0         → 佈局那一段(按鍵根本沒進引擎,見 win32_oracle.h)
    //   族別 + 組字中 = 不吃 → 刻意放行給宿主(退格在沒有組字時就走這裡)
    //   吃了但 !ready       → IPC 那一段(上面 EnsureReady 已經記了原因)
    Trace("按鍵 vk=0x%02X scan=0x%02X keysym=0x%X mods=0x%X 族=%s 組字中=%d 吃掉=%d",
          static_cast<unsigned>(w),
          static_cast<unsigned>((l >> 16) & 0xFF),
          static_cast<unsigned>(plan.mapped.keysym),
          static_cast<unsigned>(plan.mapped.modifiers), KeyKindTag(plan.kind),
          Composing() ? 1 : 0, ready ? 1 : 0);
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
    // 引擎手上的輸入被清掉了 —— Composing() 必須跟著回到 false,
    // 否則退格會繼續被當成「組字中的鍵」而吃掉,使用者就刪不掉字。
    engine_composing_ = false;
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

TextService::KeyPlan TextService::PlanKey(WPARAM w, LPARAM l, bool key_up) {
  KeyPlan p;
  p.mapped = MapKey(BuildKeyEvent(w, l, key_up), Oracle());
  p.kind = ClassifyKeyKind(p.mapped.keysym, p.mapped.modifiers);
  p.eat = ShouldEatKey(p.kind, Composing());
  return p;
}

bool TextService::SelfInsertChar(ITfContext* ctx, char32_t ch) {
  if (!ctx || ch == 0) return false;
  // char32_t → UTF-16。BMP 以外要拆成代理對,不然使用者按出來的字會變成
  // 一個問號 —— 而那種字元真的存在於某些佈局上。
  std::wstring text;
  if (ch < 0x10000) {
    text.push_back(static_cast<wchar_t>(ch));
  } else {
    const char32_t v = ch - 0x10000;
    text.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
    text.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
  }
  RunSyncSession(ctx, client_id_,
                 [this, ctx, &text](TfEditCookie ec) -> HRESULT {
                   return InsertText(ec, ctx, text);
                 });
  return true;
}

bool TextService::HandleKey(ITfContext* ctx, WPARAM w, LPARAM l, bool key_up) {
  // ⚠ 與 OnTestKeyDown 用**同一份**判斷。兩邊分岔 = 黑洞(見那裡的說明)。
  const KeyPlan plan = PlanKey(w, l, key_up);
  if (!plan.eat) return false;

  // ⚠ 連線斷掉時一定要把 engine_composing_ 歸零。
  //
  //   它是「退格算不算我們的」的依據之一(見 Composing())。連線斷在
  //   組字進行到一半的時候,這個旗標會**永遠停在 true** —— 於是使用者
  //   從此每一顆退格都被吃掉、而引擎又收不到,那顆鍵就再也沒有作用了。
  //   一個「暫時的連線失敗」不該留下一顆永久壞掉的鍵。
  if (!ipc_.EnsureReady()) {
    engine_composing_ = false;
    return false;
  }

  Result result;
  if (!ipc_.SendKey(plan.mapped.keysym, plan.mapped.modifiers, &result)) {
    // ⚠ 這裡是「按鍵永久變灰」的分岔點。拿不到結果就**放行**,
    //   不可以吃掉。使用者打不出中文會抱怨,打不出任何字則是電腦壞了。
    engine_composing_ = false;
    return false;
  }
  if (!result.handled) {
    // 引擎不處理這顆鍵 —— 但我們在 OnTestKeyDown 已經宣告吃掉它了,
    // 宿主不會再處理它。所以這裡必須有人負責。
    //
    // ── 沒有組字時:字元鍵由我們自己把字寫進文件 ─────────────────
    //
    //   這條路每天都會走到:英數模式底下每一顆字母、朗月拼音底下的數字,
    //   引擎都不處理。(為什麼不乾脆不吃它們:哪些字元會起頭是**方案**
    //   決定的,注音的 alphabet 含數字。見 key_eat_policy.h。)
    //
    // ── 組字進行中:吃掉並且什麼都不做 ───────────────────────────
    //
    //   ⚠ 這一段是刻意的,不是偷懶。組字進行中,輸入法擁有這個輸入脈絡:
    //
    //     · 自己插字元 → 會插進組字的 range 裡,把 preedit 弄壞。
    //     · 放行給宿主 → 宿主會拿 Tab / 方向鍵去動**它自己的**游標,
    //       而那個游標正壓在一段進行中的組字上。使用者看到的是組字
    //       突然跳到別的地方、或文件裡多出半截 preedit。
    //
    //   兩條都比「什麼都不做」糟。而且這樣一來
    //   「OnTestKeyDown 說吃 ⟹ OnKeyDown 也說吃」是**結構上**成立的,
    //   不是碰運氣 —— verify_input_matrix.sh 的 MISMATCH 那一格量的就是它。
    //   (唯一的例外是連線在兩趟之間斷掉,而那條路上面已經放行了。)
    if (ShouldSelfInsert(plan.kind) && !composition_) {
      const char32_t ch = CharForSelfInsert(plan.mapped.keysym);
      if (ch != 0 && SelfInsertChar(ctx, ch)) {
        // 這條路每天都會走到(英數模式的每一顆字母),所以額度要與按鍵那一行
        // 共用 —— 不然它會變成一條每顆按鍵一次磁碟寫入的路徑,而這裡是
        // 宿主的 UI 執行緒。
        if (key_trace_budget_ > 0) {
          --key_trace_budget_;
          Trace("引擎不吃這顆字元鍵,由我們補進文件 keysym=0x%X",
                static_cast<unsigned>(plan.mapped.keysym));
        }
        return true;
      }
    }
    // 組字進行中 → 吃掉並且什麼都不做(理由見上面那段)。
    //
    // ⚠ 用 Composing() 而不是 composition_:引擎說它還在組字、而我們這一側
    //   的組字沒開起來(StartComposition 失敗)時,這顆鍵在 OnTestKeyDown
    //   已經被宣告吃掉了 —— 這裡要用**同一個**條件回答,不然那一格就是黑洞。
    if (Composing()) return true;

    // 走到這裡 = 沒有組字、而且這顆鍵不是字元鍵(或字元補不出來)。
    // 依 key_eat_policy 的規則,功能鍵在沒有組字時根本不會被宣告吃掉,
    // 所以正常情況到不了這裡。真的到了就放行 ——
    // 放行最壞是「這顆鍵沒作用」,吃掉最壞是「這顆鍵永遠壞了」。
    return false;
  }

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
  // 引擎自己說的「我還在組字」。下一顆按鍵要靠它決定退格是誰的
  // (見 Composing())。**一定要在這裡更新**:少了它,StartComposition
  // 失敗的那個宿主上引擎會一直卡著上一段輸入,而使用者按退格會被放行給宿主,
  // 於是他刪的是自己文件裡的字,而組字串原封不動。
  engine_composing_ = (snap.status_flags & kStComposing) != 0;
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
  // 語言列按鈕上的字跟著這一份語言設定檔走(見 lang_bar.cc 的說明)。
  SetUiLang(ResolveUiLang("system", static_cast<uint32_t>(langid)));
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
  SetUiLang(ResolveUiLang("system", langid));
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
  // ⚠ 判準與 ActivateEx 那一條**必須是同一個**,不然會出現「按鈕按得動
  //   但服務起不來」或反過來。見 common/elevation_policy.h。
  if (!MayStartUserService(HostElevationOnce())) {
    Trace("設定按鈕:不啟動服務(刻意):%s",
          HostElevationTag(HostElevationOnce()));
    return;
  }
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
