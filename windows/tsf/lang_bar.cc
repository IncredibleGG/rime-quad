#include "lang_bar.h"

#include <oleauto.h>
#include <olectl.h>
#include <strsafe.h>

#include "guids.h"

namespace rimewin {
namespace {

// 語言列與系統匣上顯示的字。刻意短:那一格只有幾個字寬。
constexpr wchar_t kButtonText[] = L"設定";
constexpr wchar_t kTooltip[] = L"RIME 四端輸入法設定";

// 這個 cookie 只要不是 TF_INVALID_COOKIE 就好。用一個固定值:
// 我們只接受一個 sink,而 TSF 也只會掛一個。
constexpr DWORD kSinkCookie = 0x52494D45;  // 'RIME'

}  // namespace

LangBarButton::LangBarButton(std::function<void()> on_click)
    : on_click_(std::move(on_click)) {}

LangBarButton::~LangBarButton() {
  if (sink_) {
    sink_->Release();
    sink_ = nullptr;
  }
}

bool LangBarButton::AddTo(ITfThreadMgr* mgr, LangBarButton* item) {
  if (!mgr || !item) return false;
  ITfLangBarItemMgr* bar = nullptr;
  if (FAILED(mgr->QueryInterface(IID_ITfLangBarItemMgr, (void**)&bar)) || !bar)
    return false;
  const HRESULT hr = bar->AddItem(item);
  bar->Release();
  return SUCCEEDED(hr);
}

void LangBarButton::RemoveFrom(ITfThreadMgr* mgr, LangBarButton* item) {
  if (!mgr || !item) return;
  ITfLangBarItemMgr* bar = nullptr;
  if (FAILED(mgr->QueryInterface(IID_ITfLangBarItemMgr, (void**)&bar)) || !bar)
    return;
  bar->RemoveItem(item);
  bar->Release();
}

STDMETHODIMP LangBarButton::QueryInterface(REFIID riid, void** ppv) {
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

STDMETHODIMP_(ULONG) LangBarButton::AddRef() {
  return static_cast<ULONG>(::InterlockedIncrement(&ref_));
}

STDMETHODIMP_(ULONG) LangBarButton::Release() {
  const LONG n = ::InterlockedDecrement(&ref_);
  if (n == 0) delete this;
  return static_cast<ULONG>(n);
}

STDMETHODIMP LangBarButton::GetInfo(TF_LANGBARITEMINFO* info) {
  if (!info) return E_INVALIDARG;
  info->clsidService = CLSID_RimeTextService;
  info->guidItem = GUID_RimeLangBarButton;
  // SHOWNINTRAY:Windows 8 以後語言列多半是收在工作列的輸入指示器裡,
  // 沒有這個旗標的話這一項在多數機器上根本不會出現。
  info->dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_SHOWNINTRAY;
  info->ulSort = 0;
  // szDescription 是固定長度陣列(TF_LBI_DESC_MAXLEN),不是指標。
  ::StringCchCopyW(info->szDescription, TF_LBI_DESC_MAXLEN, kTooltip);
  return S_OK;
}

STDMETHODIMP LangBarButton::GetStatus(DWORD* flags) {
  if (!flags) return E_INVALIDARG;
  *flags = 0;  // 不隱藏、不停用
  return S_OK;
}

STDMETHODIMP LangBarButton::Show(BOOL /*show*/) {
  // 按鈕型的項目由系統決定顯不顯示。回 S_OK,不要回 E_NOTIMPL ——
  // 部分宿主拿到 E_NOTIMPL 會把整個項目丟掉。
  return S_OK;
}

STDMETHODIMP LangBarButton::GetTooltipString(BSTR* tooltip) {
  if (!tooltip) return E_INVALIDARG;
  *tooltip = ::SysAllocString(kTooltip);
  return *tooltip ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP LangBarButton::OnClick(TfLBIClick /*click*/, POINT /*pt*/,
                                    const RECT* /*area*/) {
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

STDMETHODIMP LangBarButton::InitMenu(ITfMenu* /*menu*/) {
  // 按鈕型沒有選單。
  //
  // ⚠ 這裡刻意不做選單。做了就要維護「選單上每一項都真的有作用」,
  //   而選單項目正是這個專案抓過四次的「看得到但摸不到」最容易長出來
  //   的地方。一顆按鈕、一件事,少一個出錯的面。
  return E_NOTIMPL;
}

STDMETHODIMP LangBarButton::OnMenuSelect(UINT /*id*/) { return E_NOTIMPL; }

STDMETHODIMP LangBarButton::GetIcon(HICON* icon) {
  if (!icon) return E_INVALIDARG;
  // 沒有圖示。回 S_OK 配 nullptr 會讓某些宿主畫一個空白方塊,
  // 回 E_FAIL 才會退回顯示文字(GetText)。
  *icon = nullptr;
  return E_FAIL;
}

STDMETHODIMP LangBarButton::GetText(BSTR* text) {
  if (!text) return E_INVALIDARG;
  *text = ::SysAllocString(kButtonText);
  return *text ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP LangBarButton::AdviseSink(REFIID riid, IUnknown* punk,
                                       DWORD* cookie) {
  if (!punk || !cookie) return E_INVALIDARG;
  if (!IsEqualIID(riid, IID_ITfLangBarItemSink)) return CONNECT_E_CANNOTCONNECT;
  if (sink_) return CONNECT_E_ADVISELIMIT;
  if (FAILED(punk->QueryInterface(IID_ITfLangBarItemSink, (void**)&sink_)))
    return E_NOINTERFACE;
  sink_cookie_ = kSinkCookie;
  *cookie = sink_cookie_;
  return S_OK;
}

STDMETHODIMP LangBarButton::UnadviseSink(DWORD cookie) {
  if (cookie != sink_cookie_ || !sink_) return CONNECT_E_NOCONNECTION;
  sink_->Release();
  sink_ = nullptr;
  sink_cookie_ = 0;
  return S_OK;
}

}  // namespace rimewin
