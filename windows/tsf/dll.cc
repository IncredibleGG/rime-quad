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
#include "text_service.h"

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

std::wstring GuidToString(REFGUID guid) {
  wchar_t buf[64] = {0};
  ::StringFromGUID2(guid, buf, 64);
  return std::wstring(buf);
}

bool WriteRegString(HKEY root, const std::wstring& subkey, const wchar_t* name,
                    const std::wstring& value) {
  HKEY key = nullptr;
  if (::RegCreateKeyExW(root, subkey.c_str(), 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
    return false;
  const LONG rc = ::RegSetValueExW(
      key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  ::RegCloseKey(key);
  return rc == ERROR_SUCCESS;
}

// 這幾個「能力」類別決定了輸入法能不能在現代的宿主裡活著。
// 少了 IMMERSIVESUPPORT,市集 App 與 Edge 裡就完全用不了;
// 少了 SECUREMODE,提權的視窗上打不了字。
// 兩者的症狀都是「在某些程式裡沒反應」,而且與程式碼完全無關 —— 只是沒註冊。
const GUID* const kTipCapCategories[] = {
    &GUID_TFCAT_TIPCAP_SECUREMODE,
    &GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
    &GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT,
    &GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
    &GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,
};

struct ComScope {
  HRESULT hr;
  ComScope() : hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
  ~ComScope() {
    // RPC_E_CHANGED_MODE 代表這個執行緒早就以別的模式初始化過了 ——
    // 那不是我們初始化的,不可以去 Uninitialize 它。
    if (SUCCEEDED(hr)) ::CoUninitialize();
  }
  bool ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

}  // namespace

// ───────────────────────── DllMain ─────────────────────────

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      g_rime_module = instance;
      // 載入器鎖底下能做的事極少。這裡只記一個控制代碼,
      // 並關掉執行緒通知 —— 宿主可能有幾十條執行緒,每條都通知一次是白費的。
      ::DisableThreadLibraryCalls(instance);
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
  if (!IsEqualCLSID(rclsid, CLSID_RimeTextService)) return CLASS_E_CLASSNOTAVAILABLE;
  return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow(void) {
  return ::InterlockedCompareExchange(&g_rime_dll_refs, 0, 0) == 0 ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer(void) {
  ComScope com;
  if (!com.ok()) return E_FAIL;

  wchar_t module_path[MAX_PATH] = {0};
  if (::GetModuleFileNameW(g_rime_module, module_path, MAX_PATH) == 0)
    return E_FAIL;

  const std::wstring clsid = GuidToString(CLSID_RimeTextService);
  const std::wstring base = L"CLSID\\" + clsid;
  if (!WriteRegString(HKEY_CLASSES_ROOT, base, nullptr, RIME_TEXT_SERVICE_DESC))
    return E_ACCESSDENIED;
  if (!WriteRegString(HKEY_CLASSES_ROOT, base + L"\\InprocServer32", nullptr,
                      module_path))
    return E_ACCESSDENIED;
  // Apartment:TSF 的文字服務跑在宿主的 UI 執行緒上,那是個 STA。
  // 寫成 Both/Free 會讓 COM 幫我們建 proxy,而 TSF 的介面不是為此設計的。
  if (!WriteRegString(HKEY_CLASSES_ROOT, base + L"\\InprocServer32",
                      L"ThreadingModel", L"Apartment"))
    return E_ACCESSDENIED;

  ITfInputProcessorProfileMgr* profiles = nullptr;
  HRESULT hr = ::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_ITfInputProcessorProfileMgr,
                                  (void**)&profiles);
  if (FAILED(hr)) return hr;
  const std::wstring desc = RIME_TEXT_SERVICE_DESC;
  hr = profiles->RegisterProfile(
      CLSID_RimeTextService, RIME_PROFILE_LANGID, GUID_RimeProfile, desc.c_str(),
      static_cast<ULONG>(desc.size()), module_path,
      static_cast<ULONG>(wcslen(module_path)), 0, nullptr, 0, TRUE, 0);
  profiles->Release();
  if (FAILED(hr)) return hr;

  ITfCategoryMgr* categories = nullptr;
  hr = ::CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ITfCategoryMgr, (void**)&categories);
  if (FAILED(hr)) return hr;
  hr = categories->RegisterCategory(CLSID_RimeTextService, GUID_TFCAT_TIP_KEYBOARD,
                                    CLSID_RimeTextService);
  for (const GUID* cat : kTipCapCategories) {
    if (FAILED(hr)) break;
    hr = categories->RegisterCategory(CLSID_RimeTextService, *cat, GUID_RimeProfile);
  }
  categories->Release();
  return hr;
}

STDAPI DllUnregisterServer(void) {
  ComScope com;
  if (!com.ok()) return E_FAIL;

  ITfCategoryMgr* categories = nullptr;
  if (SUCCEEDED(::CoCreateInstance(CLSID_TF_CategoryMgr, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
                                   (void**)&categories))) {
    categories->UnregisterCategory(CLSID_RimeTextService, GUID_TFCAT_TIP_KEYBOARD,
                                   CLSID_RimeTextService);
    for (const GUID* cat : kTipCapCategories)
      categories->UnregisterCategory(CLSID_RimeTextService, *cat, GUID_RimeProfile);
    categories->Release();
  }

  ITfInputProcessorProfileMgr* profiles = nullptr;
  if (SUCCEEDED(::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_ITfInputProcessorProfileMgr,
                                   (void**)&profiles))) {
    profiles->UnregisterProfile(CLSID_RimeTextService, RIME_PROFILE_LANGID,
                                GUID_RimeProfile, 0);
    profiles->Release();
  }

  // 反註冊要盡量做完,不因為中途某一步失敗就停 ——
  // 停在一半留下的殘骸,使用者只能自己動登錄檔。
  const std::wstring clsid = GuidToString(CLSID_RimeTextService);
  const std::wstring base = L"CLSID\\" + clsid;
  ::RegDeleteKeyW(HKEY_CLASSES_ROOT, (base + L"\\InprocServer32").c_str());
  ::RegDeleteKeyW(HKEY_CLASSES_ROOT, base.c_str());
  return S_OK;
}
