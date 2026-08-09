#include "lang_bar.h"

// ⚠ ctfutb.h 只在這一個 .cc 裡。標頭上只有四個函式與一個不透明的類別名 ——
//   理由見 lang_bar.h:mingw-w64 的 ctfutb.h 缺 ITfLangBarItemButton,
//   而 windows/syntax_check_mingw.sh 是推 CI 之前唯一的語法關卡。
//   型別露在標頭上的話,text_service.cc(全案最大的檔案)也會一起被跳過。
#include <ctfutb.h>
#include <oleauto.h>
#include <olectl.h>
#include <strsafe.h>

#include <new>

#include "guids.h"

#include "../common/ui_strings.h"

namespace rimewin {
namespace {

// 語言列上顯示的字。刻意短:那一格只有幾個字寬。
//
// ⚠ 它們原本硬編在這裡,只有繁體中文。現在走 ui_strings.cc ——
//   `windows/` 底下只有那一個檔案可以有使用者可見字串(W7)。
//
// ⚠ **這支 DLL 讀不到使用者的設定**(它住在每一個宿主進程裡,而設定
//   在服務進程那一側)。所以介面語言在這裡是由**這個輸入法註冊在哪個
//   語言設定檔底下**決定的,由 text_service.cc 在解析出 langid 之後
//   呼叫 SetUiLang() —— 語言列上的這一格本來就屬於那一份設定檔,
//   所以那是對的訊號,而且不必為了一顆按鈕多走一趟 IPC。
const wchar_t* ButtonText() { return UiText(UiString::kLangBarButton); }
const wchar_t* Tooltip() { return UiText(UiString::kLangBarTooltip); }

// 服務沒有(而且不會)起來時顯示的字。
//
// ⚠ 一個**刻意的**拒絕不該長得跟壞掉一樣。以前它只寫進一個使用者不知道
//   存在的記錄檔,於是「我們決定不啟動」與「它壞了」在使用者眼裡完全相同。
//   這幾個字是那個決定唯一的可見痕跡。
const wchar_t* ButtonTextWarn() { return UiText(UiString::kLangBarNotRunning); }

// 這個 cookie 只要不是 TF_INVALID_COOKIE 就好。用一個固定值:
// 我們只接受一個 sink,而 TSF 也只會掛一個。
constexpr DWORD kSinkCookie = 0x52494D45;  // 'RIME'

}  // namespace

class LangBarButton final : public ITfLangBarItemButton, public ITfSource {
 public:
  LangBarButton(std::function<void()> on_click, LangBarStatusFn status)
      : on_click_(std::move(on_click)), status_(std::move(status)) {}

  // 現在的狀態。nullptr = 正常。呼叫端在 UI 執行緒上,見 lang_bar.h。
  const wchar_t* Warning() const {
    if (!status_) return nullptr;
    try {
      return status_();
    } catch (...) {
      // 例外穿過 COM 邊界 = 宿主崩潰。狀態顯示不值得那個代價。
      return nullptr;
    }
  }

  // 語言列重新問一次。sink 沒掛上時 TSF 會在下一次重畫時自己問。
  void NotifyUpdate() {
    if (!sink_) return;
    sink_->OnUpdate(TF_LBI_TEXT | TF_LBI_TOOLTIP | TF_LBI_STATUS);
  }

  // ── IUnknown ────────────────────────────────────────────────

  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfLangBarItem) ||
        IsEqualIID(riid, IID_ITfLangBarItemButton))
      *ppv = static_cast<ITfLangBarItemButton*>(this);
    else if (IsEqualIID(riid, IID_ITfSource))
      *ppv = static_cast<ITfSource*>(this);
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

  // ── ITfLangBarItem ──────────────────────────────────────────

  STDMETHODIMP GetInfo(TF_LANGBARITEMINFO* info) override {
    if (!info) return E_INVALIDARG;
    info->clsidService = CLSID_RimeTextService;
    info->guidItem = GUID_RimeLangBarButton;
    // SHOWNINTRAY:Windows 8 以後語言列多半收在工作列的輸入指示器裡,
    // 沒有這個旗標的話這一項在多數機器上根本不會出現。
    info->dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_SHOWNINTRAY;
    info->ulSort = 0;
    // szDescription 是固定長度陣列(TF_LBI_DESC_MAXLEN),不是指標。
    const wchar_t* warn = Warning();
    ::StringCchCopyW(info->szDescription, TF_LBI_DESC_MAXLEN,
                     warn ? warn : Tooltip());
    return S_OK;
  }

  STDMETHODIMP GetStatus(DWORD* flags) override {
    if (!flags) return E_INVALIDARG;
    *flags = 0;  // 不隱藏、不停用
    return S_OK;
  }

  STDMETHODIMP Show(BOOL /*show*/) override {
    // 按鈕型的項目由系統決定顯不顯示。回 S_OK,不要回 E_NOTIMPL ——
    // 部分宿主拿到 E_NOTIMPL 會把整個項目丟掉。
    return S_OK;
  }

  STDMETHODIMP GetTooltipString(BSTR* tooltip) override {
    if (!tooltip) return E_INVALIDARG;
    const wchar_t* warn = Warning();
    *tooltip = ::SysAllocString(warn ? warn : Tooltip());
    return *tooltip ? S_OK : E_OUTOFMEMORY;
  }

  // ── ITfLangBarItemButton ────────────────────────────────────

  STDMETHODIMP OnClick(TfLBIClick /*click*/, POINT /*pt*/,
                       const RECT* /*area*/) override {
    // ⚠ 這裡在宿主的 UI 執行緒上。on_click_ 必須是**單向、不等回覆**的,
    //   不然使用者按一下設定,他正在用的程式就卡住了。
    try {
      if (on_click_) on_click_();
    } catch (...) {
      // 例外穿過 COM 邊界 = 宿主崩潰。這顆按鈕不值得那個代價。
      return E_FAIL;
    }
    return S_OK;
  }

  STDMETHODIMP InitMenu(ITfMenu* /*menu*/) override {
    // 按鈕型沒有選單。
    //
    // ⚠ 刻意不做選單。做了就要維護「選單上每一項都真的有作用」,
    //   而選單項目正是這個專案抓過四次的「看得到但摸不到」最容易長出來
    //   的地方。一顆按鈕、一件事,少一個出錯的面。要多做幾件事的入口是
    //   系統匣的右鍵選單(服務進程那一側)—— 那裡改壞了不會把宿主帶走。
    return E_NOTIMPL;
  }

  STDMETHODIMP OnMenuSelect(UINT /*id*/) override { return E_NOTIMPL; }

  STDMETHODIMP GetIcon(HICON* icon) override {
    if (!icon) return E_INVALIDARG;
    // 沒有圖示。回 S_OK 配 nullptr 會讓某些宿主畫一個空白方塊,
    // 回 E_FAIL 才會退回顯示文字(GetText)。
    *icon = nullptr;
    return E_FAIL;
  }

  STDMETHODIMP GetText(BSTR* text) override {
    if (!text) return E_INVALIDARG;
    // 服務不會起來時,這一格是使用者唯一看得到的訊號。
    *text = ::SysAllocString(Warning() ? ButtonTextWarn() : ButtonText());
    return *text ? S_OK : E_OUTOFMEMORY;
  }

  // ── ITfSource ───────────────────────────────────────────────
  //
  // 我們沒有變更要通知,但**不可以**回 E_NOTIMPL:部分宿主會因此
  // 整個不顯示這一項。

  STDMETHODIMP AdviseSink(REFIID riid, IUnknown* punk, DWORD* cookie) override {
    if (!punk || !cookie) return E_INVALIDARG;
    if (!IsEqualIID(riid, IID_ITfLangBarItemSink))
      return CONNECT_E_CANNOTCONNECT;
    if (sink_) return CONNECT_E_ADVISELIMIT;
    if (FAILED(punk->QueryInterface(IID_ITfLangBarItemSink, (void**)&sink_)))
      return E_NOINTERFACE;
    sink_cookie_ = kSinkCookie;
    *cookie = sink_cookie_;
    return S_OK;
  }

  STDMETHODIMP UnadviseSink(DWORD cookie) override {
    if (cookie != sink_cookie_ || !sink_) return CONNECT_E_NOCONNECTION;
    sink_->Release();
    sink_ = nullptr;
    sink_cookie_ = 0;
    return S_OK;
  }

 private:
  // final + 私有解構子:這裡是 `delete this`,而 COM 介面沒有虛擬解構子。
  ~LangBarButton() {
    if (sink_) {
      sink_->Release();
      sink_ = nullptr;
    }
  }

  LONG ref_ = 1;
  std::function<void()> on_click_;
  LangBarStatusFn status_;
  ITfLangBarItemSink* sink_ = nullptr;
  DWORD sink_cookie_ = 0;
};

// ───────────────────────── 對外的四個函式 ─────────────────────────

LangBarButton* CreateLangBarButton(std::function<void()> on_click,
                                   LangBarStatusFn status) {
  return new (std::nothrow)
      LangBarButton(std::move(on_click), std::move(status));
}

void RefreshLangBarButton(LangBarButton* item) {
  if (item) item->NotifyUpdate();
}

void ReleaseLangBarButton(LangBarButton* item) {
  if (item) item->Release();
}

bool AddLangBarButton(ITfThreadMgr* mgr, LangBarButton* item) {
  if (!mgr || !item) return false;
  ITfLangBarItemMgr* bar = nullptr;
  if (FAILED(mgr->QueryInterface(IID_ITfLangBarItemMgr, (void**)&bar)) || !bar)
    return false;
  const HRESULT hr = bar->AddItem(item);
  bar->Release();
  return SUCCEEDED(hr);
}

void RemoveLangBarButton(ITfThreadMgr* mgr, LangBarButton* item) {
  if (!mgr || !item) return;
  ITfLangBarItemMgr* bar = nullptr;
  if (FAILED(mgr->QueryInterface(IID_ITfLangBarItemMgr, (void**)&bar)) || !bar)
    return;
  bar->RemoveItem(item);
  bar->Release();
}

}  // namespace rimewin
