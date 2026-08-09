// windows/tsf/user_langs.cc — 說明見 user_langs.h

#include "user_langs.h"

#include <msctf.h>

#include "guids.h"
#include "registration.h"

namespace rimewin {
namespace {

// 與 registration.cc 同一個理由:明著指定 64 位元檢視。
constexpr REGSAM kSam64 = KEY_WOW64_64KEY;

constexpr wchar_t kUserProfileKey[] =
    L"Control Panel\\International\\User Profile";

struct ComScope {
  HRESULT hr;
  ComScope() : hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
  ~ComScope() {
    if (SUCCEEDED(hr)) ::CoUninitialize();
  }
  bool ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

// REG_MULTI_SZ 拆成一個一個字串。尾端的雙 NUL 與空字串一律丟掉。
std::vector<std::wstring> SplitMultiSz(const std::vector<wchar_t>& buf) {
  std::vector<std::wstring> out;
  size_t i = 0;
  while (i < buf.size() && buf[i] != L'\0') {
    const size_t begin = i;
    while (i < buf.size() && buf[i] != L'\0') ++i;
    out.emplace_back(&buf[begin], i - begin);
    if (i < buf.size()) ++i;  // 跳過分隔的 NUL
  }
  return out;
}

}  // namespace

// ══════════════════════════════════════════════════════════════════
//  InstallLayoutOrTip —— 微軟指定的「把輸入法加進使用者的清單」那一支
// ══════════════════════════════════════════════════════════════════
//
// 「IME requirements」那一頁把這件事拆成兩步,而且明著說第二步要用這一支:
//
//   · 安裝時用 ITfInputProcessorProfileMgr::RegisterProfile 註冊
//     (並且「不要自己寫登錄檔」);
//   · **「若要讓輸入法裝完立刻可用,呼叫 InstallLayoutOrTip 把它加進
//     使用者已啟用的輸入法」**,psz 格式 `<LangID>:{CLSID}{profileGUID}`。
//
// 也就是說:「清單上出現幾格」由**這一支**決定,而不是由註冊決定。
// Mozc 走的就是這條(InstallLayoutOrTip → SetDefaultLayoutOrTip)。
//
// ⚠ 它沒有匯入庫,也不在任何公開標頭裡 —— 必須 LoadLibrary("input.dll")
//   + GetProcAddress,旗標也要自己定義。這不是走偏門:微軟自己的文件
//   就是這樣寫的,而它是唯一被文件指名的做法。
//
// ⚠ 拿不到這支函式時(理論上不會,但這支 DLL 的存在不是我們保證得了的)
//   **不可以就這樣算了** —— 退回 EnableLanguageProfile,並且讓呼叫端看得到
//   走的是哪一條。兩條路寫的東西不完全一樣(前者動使用者的語言清單與
//   已啟用的輸入法,後者動 CTF 的 Enable 旗標),所以啟用時**兩條都做**:
//   少做任何一邊,都可能落在「登錄檔看起來對了,清單上卻沒有」那種狀態,
//   而那正是這個專案最貴的失敗形狀。
namespace {

// input.dll 的旗標。公開標頭裡沒有,照文件與 Mozc 的用法定義。
constexpr DWORD kIlotUninstall = 0x00000001;

using InstallLayoutOrTipFn = BOOL(WINAPI*)(LPCWSTR, DWORD);

InstallLayoutOrTipFn LoadInstallLayoutOrTip() {
  static InstallLayoutOrTipFn fn = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    HMODULE m = ::LoadLibraryW(L"input.dll");
    if (m)
      fn = reinterpret_cast<InstallLayoutOrTipFn>(
          ::GetProcAddress(m, "InstallLayoutOrTip"));
    // 刻意不 FreeLibrary:函式指標要留著用。這支程式活不過幾秒。
  }
  return fn;
}

// "0804:{7D02992E-…}{84420A61-…}"
std::wstring ProfileSpec(int index) {
  wchar_t lang[8] = {0};
  ::wsprintfW(lang, L"%04x",
              static_cast<unsigned>(kRimeProfiles[index].langid));
  wchar_t clsid[64] = {0};
  wchar_t guid[64] = {0};
  ::StringFromGUID2(CLSID_RimeTextService, clsid, 64);
  ::StringFromGUID2(*kRimeProfiles[index].guid, guid, 64);
  return std::wstring(lang) + L":" + clsid + guid;
}

}  // namespace

bool InstallOrRemoveLayoutOrTip(int index, bool install) {
  if (index < 0 || index >= kRimeProfileCount) return false;
  InstallLayoutOrTipFn fn = LoadInstallLayoutOrTip();
  if (!fn) return false;
  const std::wstring spec = ProfileSpec(index);
  return fn(spec.c_str(), install ? 0 : kIlotUninstall) != FALSE;
}

std::vector<std::wstring> CurrentUserLanguageTags() {
  std::vector<std::wstring> out;
  HKEY key = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, kUserProfileKey, 0,
                      KEY_READ | kSam64, &key) != ERROR_SUCCESS)
    return out;

  // ── 1. 有序的那一份 ────────────────────────────────────────────
  DWORD type = 0, bytes = 0;
  if (::RegQueryValueExW(key, L"Languages", nullptr, &type, nullptr, &bytes) ==
          ERROR_SUCCESS &&
      type == REG_MULTI_SZ && bytes >= sizeof(wchar_t)) {
    std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 2, L'\0');
    DWORD got = bytes;
    if (::RegQueryValueExW(key, L"Languages", nullptr, &type,
                           reinterpret_cast<BYTE*>(buf.data()),
                           &got) == ERROR_SUCCESS)
      out = SplitMultiSz(buf);
  }

  // ── 2. 退路:列舉子鍵(無序)────────────────────────────────────
  //
  // 只在第一條完全落空時才用。兩條都跑並合併的話,順序就變成
  // 「有序的那幾個 + 一堆亂序的」,而下游是**照順序**選第一個中文的 ——
  // 混進來的亂序項目會讓「他把簡體排第一」這件事失效。
  if (out.empty()) {
    for (DWORD i = 0;; ++i) {
      wchar_t name[256];
      DWORD len = 256;
      if (::RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr,
                          nullptr) != ERROR_SUCCESS)
        break;
      out.emplace_back(name, len);
    }
  }
  ::RegCloseKey(key);
  return out;
}

std::vector<uint32_t> InstalledInputLangIds() {
  std::vector<uint32_t> out;
  ComScope com;
  if (!com.ok()) return out;
  ITfInputProcessorProfiles* profiles = nullptr;
  if (FAILED(::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_ITfInputProcessorProfiles,
                                (void**)&profiles)))
    return out;
  LANGID* langs = nullptr;
  ULONG count = 0;
  if (SUCCEEDED(profiles->GetLanguageList(&langs, &count)) && langs) {
    for (ULONG i = 0; i < count; ++i) out.push_back(langs[i]);
    ::CoTaskMemFree(langs);
  }
  profiles->Release();
  return out;
}

std::vector<int> EnabledProfileIndexesForCurrentUser() {
  std::vector<int> out;
  ComScope com;
  if (!com.ok()) return out;
  ITfInputProcessorProfiles* profiles = nullptr;
  if (FAILED(::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_ITfInputProcessorProfiles,
                                (void**)&profiles)))
    return out;
  for (int i = 0; i < kRimeProfileCount; ++i) {
    BOOL on = FALSE;
    // 失敗 = 那一份根本沒註冊過,當成沒啟用。**不要**因此把整個查詢
    // 判定失敗:升級時舊版的三份裡可能有一份已經被清掉了。
    if (SUCCEEDED(profiles->IsEnabledLanguageProfile(
            CLSID_RimeTextService, kRimeProfiles[i].langid,
            *kRimeProfiles[i].guid, &on)) &&
        on)
      out.push_back(i);
  }
  profiles->Release();
  return out;
}

HRESULT KeepOnlyProfileEnabled(int keep_index, int* enabled, int* disabled) {
  if (enabled) *enabled = 0;
  if (disabled) *disabled = 0;
  if (keep_index < 0 || keep_index >= kRimeProfileCount) return E_INVALIDARG;

  HRESULT last_bad = S_OK;

  // 1. 先開要留的那一份。順序見標頭。
  //    **兩條路都走**:InstallLayoutOrTip 動的是使用者的語言清單與
  //    已啟用的輸入法(= 清單上看得到),EnableLanguageProfile 動的是
  //    CTF 的 Enable 旗標(= IsEnabledLanguageProfile 讀得到)。
  //    只做其中一邊,可能落在「登錄檔對了但清單上沒有」或反過來。
  InstallOrRemoveLayoutOrTip(keep_index, true);
  const HRESULT hr_on = SetProfileEnabledForCurrentUser(keep_index, true);
  if (SUCCEEDED(hr_on)) {
    if (enabled) *enabled = 1;
  } else {
    last_bad = hr_on;
  }

  // 2. 再關其餘的。**每一份都要關,不因為其中一個失敗就停** ——
  //    停在一半留下的正是使用者回報的那個狀態(清單上兩格)。
  //    ⚠ 這一圈同時是**升級時的清理**:舊版啟用了三份,新版跑到這裡
  //    會把多的兩份收回去。少了它,使用者會看到「新版說只有一格,
  //    但清單上還是三格」。
  for (int i = 0; i < kRimeProfileCount; ++i) {
    if (i == keep_index) continue;
    InstallOrRemoveLayoutOrTip(i, false);
    const HRESULT hr = SetProfileEnabledForCurrentUser(i, false);
    if (SUCCEEDED(hr)) {
      if (disabled) ++*disabled;
    } else {
      last_bad = hr;
    }
  }
  return last_bad;
}

}  // namespace rimewin
