// windows/tsf/dll.cc — COM in-proc server 的外殼:DllMain、類別工廠、註冊
//
// 匯出的四個進入點就是 TSF 對這支 DLL 的全部要求:
//   DllGetClassObject / DllCanUnloadNow / DllRegisterServer / DllUnregisterServer
// 名單同時寫在 rime_tsf.def 裡,CI 有一道 dumpbin /exports 在對帳 ——
// 少一個的話 regsvr32 會失敗,而失敗訊息看不出少了哪一個。

#include <msctf.h>
#include <windows.h>

#include <new>
#include <string>

#include "../winshared/winutil.h"
#include "guids.h"
#include "registration.h"
#include "text_service.h"
#include "trace.h"

HMODULE g_rime_module = nullptr;
LONG g_rime_dll_refs = 0;

namespace {

// ── 類別工廠 ────────────────────────────────────────────────────
// 沒有狀態,所以做成靜態單例:DLL 裡少一次配置就少一個失敗點。
class ClassFactory : public IClassFactory {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory))
      *ppv = static_cast<IClassFactory*>(this);
    if (!*ppv) return E_NOINTERFACE;
    return S_OK;
  }
  // 靜態單例,計數只是為了讓 COM 開心。
  STDMETHODIMP_(ULONG) AddRef() override { return 2; }
  STDMETHODIMP_(ULONG) Release() override { return 1; }

  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;
    rimewin::TextService* ts = nullptr;
    try {
      ts = new (std::nothrow) rimewin::TextService();
    } catch (...) {
      return E_FAIL;
    }
    if (!ts) return E_OUTOFMEMORY;
    const HRESULT hr = ts->QueryInterface(riid, ppv);
    ts->Release();
    return hr;
  }

  STDMETHODIMP LockServer(BOOL lock) override {
    if (lock)
      ::InterlockedIncrement(&g_rime_dll_refs);
    else
      ::InterlockedDecrement(&g_rime_dll_refs);
    return S_OK;
  }
};

ClassFactory g_factory;

}  // namespace

// ───────────────────────── DllMain ─────────────────────────

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      g_rime_module = instance;
      // 載入器鎖底下能做的事極少。這裡只記一個控制代碼,
      // 並關掉執行緒通知 —— 宿主可能有幾十條執行緒,每條都通知一次是白費的。
      ::DisableThreadLibraryCalls(instance);
      // ⚠ 這一行是整個診斷鏈的第一格,而且**只有在這裡記得到**。
      //
      // 「系統根本沒有載入這支 DLL」與「載入了但 ActivateEx 沒被呼叫」
      // 在使用者眼裡完全一樣(不能打字、沒有 UI),而要修的地方一個在
      // 註冊、一個在文字服務本身。少了這一行,那兩件事分不開。
      //
      // 在 DllMain 裡做檔案 I/O 通常是要避免的 —— 這裡可以做,是因為
      // Trace() 只用 kernel32 裡本來就已經載入的函式,不會觸發任何
      // LoadLibrary(見 tsf/trace.h 的規矩 2 與 3)。
      rimewin::Trace("DLL 載入(DLL_PROCESS_ATTACH)");
      break;
    default:
      break;
  }
  return TRUE;
}

// ───────────────────────── 匯出 ─────────────────────────

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
  if (!ppv) return E_INVALIDARG;
  *ppv = nullptr;
  if (!IsEqualCLSID(rclsid, CLSID_RimeTextService)) {
    rimewin::Trace("DllGetClassObject:不是我們的 CLSID");
    return CLASS_E_CLASSNOTAVAILABLE;
  }
  // 第二格:COM 真的來要我們的類別工廠了。走到這裡就代表登錄檔的
  // InprocServer32 是對的、DLL 也載得起來。
  rimewin::Trace("DllGetClassObject:CLSID 相符");
  return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow(void) {
  return ::InterlockedCompareExchange(&g_rime_dll_refs, 0, 0) == 0 ? S_OK : S_FALSE;
}

// 註冊的實作在 tsf/registration.cc,DLL 與 rime_ime_setup.exe 共用同一份。
//
// 兩邊各寫一份的話會漂移,而漂移的症狀是「用安裝程式裝的能用,自己
// regsvr32 的不能用」—— 兩種註冊狀態長得幾乎一樣,只差一個子鍵。
//
// 一般使用者**不會走這條路徑**:安裝程式呼叫的是 rime_ime_setup.exe。
// 這兩個匯出留著是因為 regsvr32 是 COM in-proc server 的既定介面,
// 開發時仍然用得到,而且 windows/check_binaries.sh 要求匯出正好那四個。
STDAPI DllRegisterServer(void) {
  wchar_t module_path[MAX_PATH] = {0};
  if (::GetModuleFileNameW(g_rime_module, module_path, MAX_PATH) == 0)
    return E_FAIL;
  return rimewin::RegisterTextService(module_path);
}

STDAPI DllUnregisterServer(void) { return rimewin::UnregisterTextService(); }
