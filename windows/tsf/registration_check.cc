// windows/tsf/registration_check.cc — 註冊狀態的檢查與列舉。說明見標頭。

#include "registration_check.h"

#include <msctf.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "../winshared/winutil.h"
#include "guids.h"
#include "registration.h"

namespace rimewin {
namespace {

constexpr REGSAM kSam64 = KEY_WOW64_64KEY;

// 輸出一律走窄字元 UTF-8,理由同 service/main.cc:在同一個 FILE* 上混用
// 寬字元與窄字元 I/O 是未定義行為,而 CI 是把 stdout 導進檔案再用 bash 比對的。
void Say(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stdout, fmt, ap);
  va_end(ap);
  std::fflush(stdout);
}

void SayW(const char* prefix, const std::wstring& w) {
  Say("%s%s\n", prefix, WideToUtf8(w).c_str());
}

bool ReadRegString(HKEY root, const std::wstring& subkey, const wchar_t* name,
                   std::wstring* out) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | kSam64, &key) !=
      ERROR_SUCCESS)
    return false;
  DWORD type = 0, bytes = 0;
  LONG rc = ::RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
  if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
    ::RegCloseKey(key);
    return false;
  }
  std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 2, L'\0');
  rc = ::RegQueryValueExW(key, name, nullptr, &type,
                          reinterpret_cast<BYTE*>(buf.data()), &bytes);
  ::RegCloseKey(key);
  if (rc != ERROR_SUCCESS) return false;
  *out = std::wstring(buf.data());
  return true;
}

bool KeyExists(HKEY root, const std::wstring& subkey) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | kSam64, &key) !=
      ERROR_SUCCESS)
    return false;
  ::RegCloseKey(key);
  return true;
}

std::vector<std::wstring> SubKeys(HKEY root, const std::wstring& subkey) {
  std::vector<std::wstring> out;
  HKEY key = nullptr;
  if (::RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | kSam64, &key) !=
      ERROR_SUCCESS)
    return out;
  for (DWORD i = 0;; ++i) {
    wchar_t name[256];
    DWORD len = 256;
    if (::RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr,
                        nullptr) != ERROR_SUCCESS)
      break;
    out.push_back(name);
  }
  ::RegCloseKey(key);
  return out;
}

std::wstring ToLower(std::wstring s) {
  for (wchar_t& c : s)
    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
  return s;
}

// LanguageProfile 底下的子鍵名是 langid 的十六進位形式。**不寫死格式。**
//
// 這一條是刻意的:「0x00000404」是實際觀察到的樣子,但那是 CTF 的內部細節,
// 沒有文件保證。寫死格式的話,哪天格式變了,症狀會是「明明註冊成功卻斷言失敗」,
// 而那看起來像產品壞了。改成把子鍵名當十六進位數字解析再比對數值,
// 0x00000404 / 0x0404 / 404 都認得。
bool NameMatchesLangId(const std::wstring& name, LANGID langid) {
  const wchar_t* p = name.c_str();
  if (p[0] == L'0' && (p[1] == L'x' || p[1] == L'X')) p += 2;
  if (!*p) return false;
  unsigned long v = 0;
  for (; *p; ++p) {
    unsigned d;
    if (*p >= L'0' && *p <= L'9') d = static_cast<unsigned>(*p - L'0');
    else if (*p >= L'a' && *p <= L'f') d = static_cast<unsigned>(*p - L'a' + 10);
    else if (*p >= L'A' && *p <= L'F') d = static_cast<unsigned>(*p - L'A' + 10);
    else return false;
    v = v * 16 + d;
    if (v > 0xFFFFFFFFul) return false;
  }
  return v == static_cast<unsigned long>(langid);
}

void DumpTree(HKEY root, const wchar_t* root_name, const std::wstring& subkey,
              int depth) {
  if (depth == 0) {
    Say("  [%s\\%s]%s\n", WideToUtf8(root_name).c_str(),
        WideToUtf8(subkey).c_str(), KeyExists(root, subkey) ? "" : "  ← 不存在");
    if (!KeyExists(root, subkey)) return;
  }
  for (const std::wstring& child : SubKeys(root, subkey)) {
    Say("  %*s%s\n", (depth + 1) * 2, "", WideToUtf8(child).c_str());
    if (depth < 4) DumpTree(root, root_name, subkey + L"\\" + child, depth + 1);
  }
}

struct ComScope {
  HRESULT hr;
  ComScope() : hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
  ~ComScope() {
    if (SUCCEEDED(hr)) ::CoUninitialize();
  }
  bool ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

// ── TSF 的 API 到底看不看得到我們 ────────────────────────────────
//
// 這是本輪最有價值的一項驗證。登錄檔長出東西只證明「我們寫進去了」;
// 由 TSF 自己的列舉 API 把我們列出來,才證明「**系統接受了**這個輸入法」。
// 兩者不是同一件事:鍵值格式錯、CLSID 註冊在錯的檢視、類別沒註冊,
// 都會讓登錄檔看起來很像對的,而系統一個字都不認。
// 每一個註冊的 langid 都要被列舉得出來。
//
// ⚠ 「至少有一個」是不夠的。這一輪修的正是「只註冊了 0x0404,而簡體中文的
//   使用者在自己的語言底下找不到它」—— 那個狀態下「至少有一個」照樣成立,
//   斷言照樣全綠,而使用者照樣找不到。所以是逐一比對。
bool EnumeratesItself() {
  ITfInputProcessorProfiles* profiles = nullptr;
  HRESULT hr = ::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_ITfInputProcessorProfiles,
                                  (void**)&profiles);
  if (FAILED(hr)) {
    Say("  !! CoCreateInstance(CLSID_TF_InputProcessorProfiles) 失敗 hr=0x%08lX\n",
        static_cast<unsigned long>(hr));
    return false;
  }

  int found = 0;
  for (int i = 0; i < ProfileCount(); ++i) {
    const LANGID lang = ProfileLangId(i);
    IEnumTfLanguageProfiles* e = nullptr;
    hr = profiles->EnumLanguageProfiles(lang, &e);
    if (FAILED(hr) || !e) {
      Say("  !! EnumLanguageProfiles(0x%04X) 失敗 hr=0x%08lX\n",
          static_cast<unsigned>(lang), static_cast<unsigned long>(hr));
      continue;
    }
    TF_LANGUAGEPROFILE p{};
    ULONG fetched = 0;
    int total = 0;
    bool hit = false;
    while (e->Next(1, &p, &fetched) == S_OK && fetched == 1) {
      ++total;
      if (IsEqualCLSID(p.clsid, CLSID_RimeTextService) && IsOurProfile(p.guidProfile)) {
        hit = true;
        Say("  ✓ EnumLanguageProfiles(0x%04X) 列舉到本輸入法 %s(fActive=%d)\n",
            static_cast<unsigned>(lang), WideToUtf8(ProfileGuidString(i)).c_str(),
            p.fActive ? 1 : 0);
      }
    }
    e->Release();
    if (hit)
      ++found;
    else
      Say("  !! langid 0x%04X 底下有 %d 個設定檔,但沒有我們的\n",
          static_cast<unsigned>(lang), total);
  }
  profiles->Release();

  // 第二條路徑:ProfileMgr。它給的是包含旗標(啟用/使用中)的完整結構,
  // 而且是「輸入法清單」那一份資料的來源。兩條都問是刻意的 ——
  // 只問一條的話,問到的可能正好是有快取的那一條。
  ITfInputProcessorProfileMgr* mgr = nullptr;
  hr = ::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                          CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfileMgr,
                          (void**)&mgr);
  if (SUCCEEDED(hr) && mgr) {
    IEnumTfInputProcessorProfiles* e2 = nullptr;
    if (SUCCEEDED(mgr->EnumProfiles(0, &e2)) && e2) {
      TF_INPUTPROCESSORPROFILE p{};
      ULONG fetched = 0;
      while (e2->Next(1, &p, &fetched) == S_OK && fetched == 1) {
        if (IsEqualCLSID(p.clsid, CLSID_RimeTextService) &&
            IsOurProfile(p.guidProfile)) {
          Say("  ✓ EnumProfiles:langid=0x%04X dwFlags=0x%08lX dwCaps=0x%08lX\n",
              static_cast<unsigned>(p.langid),
              static_cast<unsigned long>(p.dwFlags),
              static_cast<unsigned long>(p.dwCaps));
        }
      }
      e2->Release();
    }
    mgr->Release();
  }

  if (found != ProfileCount()) {
    Say("  !! %d / %d 個語言被 TSF 列舉出來\n", found, ProfileCount());
    return false;
  }
  Say("  ✓ %d 個語言全部列舉得到\n", ProfileCount());
  return true;
}

}  // namespace

void DumpRegistration() {
  Say("--- 登錄檔實況 ---\n");
  DumpTree(HKEY_LOCAL_MACHINE, L"HKLM", ClsidRegPath(), 0);
  DumpTree(HKEY_LOCAL_MACHINE, L"HKLM", CtfTipRegPath(), 0);
  DumpTree(HKEY_CURRENT_USER, L"HKCU", CtfTipRegPath(), 0);
}

bool CheckRegistration(const CheckOptions& opt) {
  ComScope com;
  if (!com.ok()) {
    Say("!! CoInitializeEx 失敗\n");
    return false;
  }

  int bad = 0;
  auto fail = [&bad](const char* msg) {
    Say("  !! %s\n", msg);
    ++bad;
  };

  Say("=== 註冊檢查 ===\n");
  SayW("CLSID   = ", ClsidString());
  for (int i = 0; i < ProfileCount(); ++i)
    Say("Profile = 0x%04X %s\n", static_cast<unsigned>(ProfileLangId(i)),
        WideToUtf8(ProfileGuidString(i)).c_str());

  // ── 1. COM in-proc server ──────────────────────────────────────
  std::wstring v;
  if (!KeyExists(HKEY_LOCAL_MACHINE, ClsidRegPath()))
    fail("HKLM\\SOFTWARE\\Classes\\CLSID\\{…} 不存在 —— COM 類別根本沒註冊");
  else
    Say("  ✓ HKLM\\%s\n", WideToUtf8(ClsidRegPath()).c_str());

  if (!ReadRegString(HKEY_LOCAL_MACHINE, InprocRegPath(), nullptr, &v) ||
      v.empty()) {
    fail("InprocServer32 沒有預設值 —— COM 不知道該載入哪個 DLL");
  } else {
    Say("  ✓ InprocServer32 = %s\n", WideToUtf8(v).c_str());
    if (::GetFileAttributesW(v.c_str()) == INVALID_FILE_ATTRIBUTES)
      fail("InprocServer32 指到的檔案不存在 —— 註冊留在機器上,DLL 卻已經不在了");
    if (!opt.expect_dll_path.empty() &&
        ToLower(v) != ToLower(opt.expect_dll_path)) {
      Say("     預期 = %s\n", WideToUtf8(opt.expect_dll_path).c_str());
      fail("InprocServer32 指到的不是這一次安裝的那份 DLL");
    }
  }

  if (!ReadRegString(HKEY_LOCAL_MACHINE, InprocRegPath(), L"ThreadingModel",
                     &v) ||
      v != L"Apartment")
    fail("ThreadingModel 不是 Apartment —— COM 會替 TSF 的介面做 proxy,而它們不是為此設計的");
  else
    Say("  ✓ ThreadingModel = Apartment\n");

  // ── 2. TSF 的設定檔 ────────────────────────────────────────────
  if (!KeyExists(HKEY_LOCAL_MACHINE, CtfTipRegPath())) {
    fail("HKLM\\SOFTWARE\\Microsoft\\CTF\\TIP\\{…} 不存在 —— TSF 沒有收下這個文字服務");
  } else {
    Say("  ✓ HKLM\\%s\n", WideToUtf8(CtfTipRegPath()).c_str());
    // 逐一比對每一個語言。**不可以寫成「至少有一個」** —— 這一輪修的正是
    // 「只註冊了 0x0404,簡體中文的使用者找不到它」,而那個狀態下
    // 「至少有一個」照樣成立。
    const std::wstring lp = CtfTipRegPath() + L"\\LanguageProfile";
    for (int i = 0; i < ProfileCount(); ++i) {
      bool profile_ok = false;
      for (const std::wstring& lang : SubKeys(HKEY_LOCAL_MACHINE, lp)) {
        if (!NameMatchesLangId(lang, ProfileLangId(i))) continue;
        for (const std::wstring& g :
             SubKeys(HKEY_LOCAL_MACHINE, lp + L"\\" + lang)) {
          if (ToLower(g) != ToLower(ProfileGuidString(i))) continue;
          profile_ok = true;
          Say("  ✓ LanguageProfile\\%s\\%s\n", WideToUtf8(lang).c_str(),
              WideToUtf8(g).c_str());
        }
      }
      if (!profile_ok) {
        Say("     缺:langid 0x%04X 的 %s\n",
            static_cast<unsigned>(ProfileLangId(i)),
            WideToUtf8(ProfileGuidString(i)).c_str());
        fail("CTF 底下缺一份語言設定檔 —— 使用該語言的人在自己的語言底下找不到這個輸入法");
      }
    }

    const std::vector<std::wstring> cats =
        SubKeys(HKEY_LOCAL_MACHINE, CtfCategoryRegPath());
    Say("  Category\\Category 底下 %d 個:\n", static_cast<int>(cats.size()));
    int items = 0;
    for (const std::wstring& c : cats) {
      const std::vector<std::wstring> kids =
          SubKeys(HKEY_LOCAL_MACHINE, CtfCategoryRegPath() + L"\\" + c);
      items += static_cast<int>(kids.size());
      Say("      %s (%d)\n", WideToUtf8(c).c_str(), static_cast<int>(kids.size()));
    }
    // 刻意比對**數量**而不是逐一比對 GUID 值。GUID_TFCAT_* 的字面值只有
    // Windows SDK 說了算,在腳本或這裡再抄一份等於憑印象抄 —— 抄錯的話
    // 這道檢查會在「其實註冊正確」時失敗,而那看起來像產品壞了。
    // 少一個能力類別的症狀是「在某些程式裡輸入法整個不存在」,
    // 而數量對不上就抓得到它。
    if (static_cast<int>(cats.size()) != RegisteredCategoryCount()) {
      Say("     預期 %d 個(GUID_TFCAT_TIP_KEYBOARD + 5 個能力類別)\n",
          RegisteredCategoryCount());
      fail("能力類別註冊不齊 —— 症狀會是「在市集 App / Edge / 提權視窗裡沒有這個輸入法」");
    } else if (items != ExpectedCategoryItemCount()) {
      // 這一條抓的是半套狀態:類別在,但只替其中一份 profile 註冊了。
      // 症狀是「簡體那一份在市集 App 裡不存在,繁體那一份正常」。
      Say("     類別底下共 %d 筆,預期 %d 筆(1 + 5 × %d 個語言)\n", items,
          ExpectedCategoryItemCount(), ProfileCount());
      fail("能力類別沒有替每一份語言設定檔註冊");
    } else {
      Say("  ✓ 能力類別 %d 類 / %d 筆齊全\n", RegisteredCategoryCount(), items);
    }
  }

  // ── 3. 系統看不看得到(TSF 的 API)────────────────────────────
  if (opt.check_enum) {
    Say("--- TSF 列舉 ---\n");
    if (!EnumeratesItself())
      fail("TSF 的列舉 API 看不到這個輸入法(或看不到全部語言)—— 登錄檔寫進去了,系統卻沒有收下");
  } else {
    // 明著說跳過了。「安靜地少驗一項」與「驗過了」在報表上長得一樣,
    // 而這個專案最貴的失敗一直是那一種。
    Say("--- TSF 列舉:**跳過**(--no-enum)---\n");
    Say("  註冊完的當下 CTF 還看不到新的設定檔(實測 0.12 秒時看不到、22 秒後看得到),\n");
    Say("  所以安裝程式只驗登錄檔。「系統接受了嗎」由事後的 check(不帶 --no-enum)回答。\n");
  }

  // ── 4. 目前使用者有沒有被啟用 ──────────────────────────────────
  if (opt.check_user) {
    const std::wstring lp = CtfTipRegPath() + L"\\LanguageProfile";
    int user_ok = 0;
    for (int i = 0; i < ProfileCount(); ++i) {
      for (const std::wstring& lang : SubKeys(HKEY_CURRENT_USER, lp)) {
        if (!NameMatchesLangId(lang, ProfileLangId(i))) continue;
        for (const std::wstring& g : SubKeys(HKEY_CURRENT_USER, lp + L"\\" + lang)) {
          if (ToLower(g) != ToLower(ProfileGuidString(i))) continue;
          ++user_ok;
          Say("  ✓ HKCU LanguageProfile\\%s\\%s\n", WideToUtf8(lang).c_str(),
              WideToUtf8(g).c_str());
        }
      }
    }
    // 這裡要求的是「至少一份」,與 HKLM 那一段不同,而且是刻意的:
    // HKCU 這一側取決於**使用者的語言清單裡有哪幾種中文**,
    // 那不是我們決定得了的。三種全中才算過的話,這道檢查會在
    // 完全正常的機器上失敗。
    if (user_ok == 0)
      fail("HKCU 底下一份設定檔都沒有 —— 目前這個使用者沒有被啟用");
    else
      Say("  ✓ HKCU 底下 %d 份已啟用(取決於使用者的語言清單,不要求三份全中)\n",
          user_ok);
  }

  if (bad != 0) {
    Say("\n=== 註冊檢查失敗:%d 項 ===\n", bad);
    // 失敗時一定要把實況印出來。沒有它的話,「路徑不對」與「根本沒註冊」
    // 在報表上長得一模一樣,而每問一次都要等一整輪 CI。
    DumpRegistration();
    return false;
  }
  Say("\n=== 註冊檢查通過 ===\n");
  return true;
}

}  // namespace rimewin
