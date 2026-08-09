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
//  InstallLayoutOrTip —— 試過了,退回來了。這一段是那次的紀錄。
// ══════════════════════════════════════════════════════════════════
//
// 微軟的「IME requirements」把這件事拆成兩步,而且指名第二步用這一支:
// 註冊用 RegisterProfile(並且「不要自己寫登錄檔」),而
// 「若要讓輸入法裝完立刻可用,呼叫 InstallLayoutOrTip 把它加進使用者
// 已啟用的輸入法」,psz 格式 `<LangID>:{CLSID}{profileGUID}`。
// Mozc 走的就是這條。它沒有匯入庫也不在公開標頭裡,要 LoadLibrary
// ("input.dll") + GetProcAddress,旗標(ILOT_UNINSTALL=0x1 …)自己定義。
//
// 我照著接了一版:啟用時 InstallLayoutOrTip + EnableLanguageProfile 兩條都走,
// 停用時 ILOT_UNINSTALL + EnableLanguageProfile(false)。
//
// **CI 上當場壞得比原來更嚴重**:跑完 enable-user 之後,rime_tsf_host
// 連 rime_tsf.dll 都載不進來了(落地記錄檔根本沒被建出來),
// 而在那之前它至少還走得到 ActivateEx。唯一新增的、會動到這台機器的
// 呼叫就是 ILOT_UNINSTALL 那一支 —— 它顯然不只是「從使用者的清單裡拿掉」。
//
// ⚠ 我**沒有**查清楚它到底動了什麼(那要在真 Windows 上逐鍵比對登錄檔,
//   而這一輪的目標是讓使用者的清單只剩一格,不是把 input.dll 的行為
//   逆向出來)。所以這裡誠實地記下:**試過、壞了、退回 EnableLanguageProfile**。
//   下一個人要再碰它的話,請從「ILOT_UNINSTALL 之後 HKLM 的 CTF\TIP
//   子樹還剩什麼」開始量,不要從文件開始猜。
//
// 退回去之後的做法就是 EnableLanguageProfile 一條路，而它是這一輪
// **量得到**的那一條(§4b 的「正好一份」)。

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
