// windows/tsf/registration.cc — 註冊/反註冊的唯一實作。DLL 與安裝程式共用。
//
// 說明見 registration.h。這裡只放**寫入**那一半;檢查與列舉在
// registration_check.cc(那一份不連進 DLL,因為它要用到 oleaut32)。

#include "registration.h"

#include <msctf.h>

#include "guids.h"

namespace rimewin {
namespace {

std::wstring GuidToString(REFGUID guid) {
  wchar_t buf[64] = {0};
  ::StringFromGUID2(guid, buf, 64);
  return std::wstring(buf);
}

// KEY_WOW64_64KEY 明著加上。這支 DLL 與安裝程式都是 64 位元,本來就會落在
// 64 位元檢視;但 Inno Setup 的安裝主程式是 32 位元的,它若哪天改成直接
// 呼叫這裡的函式(而不是啟動我們自己的 64 位元 exe),沒有這個旗標就會
// 安靜地寫進 WOW6432Node —— 註冊「成功」,而 64 位元的宿主一個都找不到它。
constexpr REGSAM kSam64 = KEY_WOW64_64KEY;

bool WriteRegString(HKEY root, const std::wstring& subkey, const wchar_t* name,
                    const std::wstring& value) {
  HKEY key = nullptr;
  if (::RegCreateKeyExW(root, subkey.c_str(), 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE | kSam64, nullptr,
                        &key, nullptr) != ERROR_SUCCESS)
    return false;
  const LONG rc = ::RegSetValueExW(
      key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  ::RegCloseKey(key);
  return rc == ERROR_SUCCESS;
}

// 遞迴刪除。RegDeleteKeyExW 只刪得掉沒有子鍵的鍵,而 CTF 那一棵有好幾層 ——
// 少了這個,反除安裝會留下一半的登錄檔,而下一次安裝會在那堆殘骸上疊起來。
LONG DeleteKeyTree(HKEY root, const std::wstring& subkey) {
  HKEY key = nullptr;
  LONG rc = ::RegOpenKeyExW(root, subkey.c_str(), 0,
                            KEY_READ | KEY_WRITE | kSam64, &key);
  if (rc != ERROR_SUCCESS) return rc;
  for (;;) {
    wchar_t name[256];
    DWORD len = 256;
    // 一律讀第 0 個:每刪掉一個,後面的索引就位移了。
    if (::RegEnumKeyExW(key, 0, name, &len, nullptr, nullptr, nullptr,
                        nullptr) != ERROR_SUCCESS)
      break;
    DeleteKeyTree(key, name);
  }
  ::RegCloseKey(key);
  return ::RegDeleteKeyExW(root, subkey.c_str(), kSam64, 0);
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

std::wstring ClsidString() { return GuidToString(CLSID_RimeTextService); }

int ProfileCount() { return kRimeProfileCount; }

LANGID ProfileLangId(int i) { return kRimeProfiles[i].langid; }

std::wstring ProfileGuidString(int i) {
  return GuidToString(*kRimeProfiles[i].guid);
}

bool IsOurProfile(REFGUID guid) {
  for (int i = 0; i < kRimeProfileCount; ++i)
    if (IsEqualGUID(guid, *kRimeProfiles[i].guid)) return true;
  return false;
}

std::wstring ClsidRegPath() {
  return L"SOFTWARE\\Classes\\CLSID\\" + ClsidString();
}
std::wstring InprocRegPath() { return ClsidRegPath() + L"\\InprocServer32"; }

std::wstring CtfTipRegPath() {
  return L"SOFTWARE\\Microsoft\\CTF\\TIP\\" + ClsidString();
}
std::wstring CtfCategoryRegPath() {
  return CtfTipRegPath() + L"\\Category\\Category";
}

int RegisteredCategoryCount() {
  // Category\Category 底下的子鍵是**類別 id**,每個類別一個,
  // 與註冊了幾份語言設定檔無關。
  // +1 是 GUID_TFCAT_TIP_KEYBOARD(「這是一個鍵盤類的文字服務」),
  // 它與底下那五個能力類別是分開註冊的。
  return 1 + static_cast<int>(sizeof(kTipCapCategories) /
                              sizeof(kTipCapCategories[0]));
}

int ExpectedCategoryItemCount() {
  // 再深一層的總數:TIP_KEYBOARD 底下掛的是 CLSID(一筆),
  // 五個能力類別底下各掛**每一份 profile 的 GUID**。
  // 這個數字是從註冊迴圈本身推出來的,不是手寫的常數 ——
  // 哪天多註冊一個語言,它自己會跟著變。
  return 1 + static_cast<int>(sizeof(kTipCapCategories) /
                              sizeof(kTipCapCategories[0])) *
                 kRimeProfileCount;
}

HRESULT RegisterTextService(const std::wstring& dll_path) {
  if (dll_path.empty()) return E_INVALIDARG;
  ComScope com;
  if (!com.ok()) return E_FAIL;

  if (!WriteRegString(HKEY_LOCAL_MACHINE, ClsidRegPath(), nullptr,
                      RIME_TEXT_SERVICE_DESC))
    return E_ACCESSDENIED;
  if (!WriteRegString(HKEY_LOCAL_MACHINE, InprocRegPath(), nullptr, dll_path))
    return E_ACCESSDENIED;
  // Apartment:TSF 的文字服務跑在宿主的 UI 執行緒上,那是個 STA。
  // 寫成 Both/Free 會讓 COM 幫我們建 proxy,而 TSF 的介面不是為此設計的。
  if (!WriteRegString(HKEY_LOCAL_MACHINE, InprocRegPath(), L"ThreadingModel",
                      L"Apartment"))
    return E_ACCESSDENIED;

  ITfInputProcessorProfileMgr* profiles = nullptr;
  HRESULT hr = ::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_ITfInputProcessorProfileMgr,
                                  (void**)&profiles);
  if (FAILED(hr)) return hr;
  // 每一個中文語言各註冊一份。少了這個迴圈的第二圈,系統語言是簡體中文的
  // 使用者會在「繁体中文(中国台湾)」底下看到這個輸入法 —— 或者根本找不到。
  //
  // ══ 2026-08-09:bEnabledByDefault 由 TRUE 改成 FALSE ═══════════
  //
  // 倒數第二個參數。它的意思是「**替所有使用者**把這一份預設設成啟用」
  // (對照組:EnableLanguageProfileByDefault 的文件寫的是 for all users,
  //  EnableLanguageProfile 才是 for the current user)。
  //
  // 為什麼非改不可:使用者回報他的 Win + 空白鍵清單上我們佔三格,
  // 而微软拼音、小狼毫各佔一格。三格的來源只可能是「有三份是啟用的」,
  // 而這一輪之前有**兩條**路會讓它們變成啟用:
  //   1. 這個 TRUE(全機、對每一個使用者、三份全開);
  //   2. 安裝程式的 enable-user 對三份各呼叫一次 EnableLanguageProfile。
  // 只堵第 2 條而留著第 1 條,等於修了一半 —— 而修一半的症狀與沒修
  // 完全一樣(清單上還是三格),只是要再等一輪使用者回報才知道。
  //
  // 改成 FALSE 之後,「這台機器上這個使用者看到幾格」只由**一條**路決定:
  // rime_ime_setup.exe enable-user(見 tsf/user_langs.cc)。
  // 一條路才斷言得了,而 CI 現在斷言的正是「正好一份」。
  //
  // ⚠ 代價寫在這裡:同一台機器上**第二個使用者**登入時不會自動拿到
  //   這個輸入法 —— 因為沒有人替他跑過 enable-user。這是刻意的取捨:
  //   自動給他的話,給的會是三份(我們無從得知他的語言清單),
  //   而那正是要修的問題。他要用的話,「開始」功能表裡的
  //   「LuminaKey 設定」旁邊那支 rime_ime_setup.exe enable-user 補得回來。
  //   (參考:上游 Weasel 也不把 HK / MO / SG 設成 enabled-by-default。)
  //
  // 最後一個參數 dwFlags 仍然是 0。TF_RP_HIDDENINSETTINGUI(0x2)是存在的,
  // 但**刻意不用**:微軟對它的全部說明只有一句「不會出現在設定 UI 裡」,
  // 沒有 Remarks、沒有說「設定 UI」涵蓋哪幾個介面、TF_INPUTPROCESSORPROFILE
  // 也讀不回這個旗標(它只露出 ACTIVE / ENABLED / SUBSTITUTEDBY…),
  // 而且找不到任何一個公開的輸入法在用它(Weasel、Mozc、chewing 都沒有)。
  // 拿一個沒有人走過、而且驗證不了的旗標去修一個我們已經知道怎麼修的問題,
  // 是把「清單上有幾格」交給一個猜測。所以走啟用那一條 —— 那一條量得到。
  for (int i = 0; i < kRimeProfileCount && SUCCEEDED(hr); ++i) {
    const std::wstring desc = kRimeProfiles[i].description;
    hr = profiles->RegisterProfile(
        CLSID_RimeTextService, kRimeProfiles[i].langid, *kRimeProfiles[i].guid,
        desc.c_str(), static_cast<ULONG>(desc.size()), dll_path.c_str(),
        static_cast<ULONG>(dll_path.size()), 0, nullptr, 0,
        /*bEnabledByDefault=*/FALSE, /*dwFlags=*/0);
  }
  profiles->Release();
  if (FAILED(hr)) return hr;

  ITfCategoryMgr* categories = nullptr;
  hr = ::CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ITfCategoryMgr, (void**)&categories);
  if (FAILED(hr)) return hr;
  hr = categories->RegisterCategory(CLSID_RimeTextService, GUID_TFCAT_TIP_KEYBOARD,
                                    CLSID_RimeTextService);
  // 能力類別要對**每一份** profile 各註冊一次:少了哪一份,那個語言底下的
  // 輸入法就會在市集 App / Edge / 提權視窗裡整個不存在,而其他語言底下正常。
  for (const GUID* cat : kTipCapCategories) {
    for (int i = 0; i < kRimeProfileCount && SUCCEEDED(hr); ++i)
      hr = categories->RegisterCategory(CLSID_RimeTextService, *cat,
                                        *kRimeProfiles[i].guid);
    if (FAILED(hr)) break;
  }
  categories->Release();
  return hr;
}

HRESULT UnregisterTextService() {
  ComScope com;
  if (!com.ok()) return E_FAIL;

  ITfCategoryMgr* categories = nullptr;
  if (SUCCEEDED(::CoCreateInstance(CLSID_TF_CategoryMgr, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
                                   (void**)&categories))) {
    categories->UnregisterCategory(CLSID_RimeTextService, GUID_TFCAT_TIP_KEYBOARD,
                                   CLSID_RimeTextService);
    for (const GUID* cat : kTipCapCategories)
      for (int i = 0; i < kRimeProfileCount; ++i)
        categories->UnregisterCategory(CLSID_RimeTextService, *cat,
                                       *kRimeProfiles[i].guid);
    categories->Release();
  }

  ITfInputProcessorProfileMgr* profiles = nullptr;
  if (SUCCEEDED(::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_ITfInputProcessorProfileMgr,
                                   (void**)&profiles))) {
    for (int i = 0; i < kRimeProfileCount; ++i)
      profiles->UnregisterProfile(CLSID_RimeTextService, kRimeProfiles[i].langid,
                                  *kRimeProfiles[i].guid, 0);
    profiles->Release();
  }

  // 反註冊要盡量做完,不因為中途某一步失敗就停。
  //
  // ⚠ 這裡不只刪 CLSID,也把 CTF\TIP 底下整棵刪掉。UnregisterProfile 只拿掉
  //   語言設定檔那一支,類別註冊(Category\Category、Category\Item)是留著的 ——
  //   而「解除安裝之後登錄檔還有殘骸」正是 CI 這一輪要斷言的事情之一。
  DeleteKeyTree(HKEY_LOCAL_MACHINE, CtfTipRegPath());
  DeleteKeyTree(HKEY_LOCAL_MACHINE, ClsidRegPath());
  // 使用者那一側的殘骸也一起清。這一支多半是以提權身分跑的,所以它清掉的是
  // 「提權那個帳號」的 HKCU;真正在用電腦的那個人由 disable-user 負責
  // (安裝程式以 runasoriginaluser 呼叫)。兩邊都做,才不會有人被留下。
  DeleteKeyTree(HKEY_CURRENT_USER, CtfTipRegPath());
  return S_OK;
}

HRESULT SetProfileEnabledForCurrentUser(int index, bool enable) {
  if (index < 0 || index >= kRimeProfileCount) return E_INVALIDARG;
  ComScope com;
  if (!com.ok()) return E_FAIL;
  ITfInputProcessorProfiles* profiles = nullptr;
  HRESULT hr = ::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_ITfInputProcessorProfiles,
                                  (void**)&profiles);
  if (FAILED(hr)) return hr;
  hr = profiles->EnableLanguageProfile(CLSID_RimeTextService,
                                       kRimeProfiles[index].langid,
                                       *kRimeProfiles[index].guid,
                                       enable ? TRUE : FALSE);
  profiles->Release();
  return hr;
}

}  // namespace rimewin
