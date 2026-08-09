#include "winutil.h"

// StringFromGUID2 住在 objbase.h。WIN32_LEAN_AND_MEAN 之下 windows.h 不會
// 把它帶進來 —— 而 mingw 的標頭會,所以 syntax_check_mingw.sh 是綠的、
// MSVC 才紅。這正是那支腳本檔頭寫的「它綠了不代表 MSVC 會綠」。
#include <objbase.h>

#include <sddl.h>

#include <vector>

namespace rimewin {

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        &out[0], n);
  return out;
}

std::string WideToUtf8(const std::wstring& s) {
  if (s.empty()) return std::string();
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0,
                                      nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        &out[0], n, nullptr, nullptr);
  return out;
}

std::wstring CurrentUserSidString() {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
    return std::wstring();
  DWORD needed = 0;
  ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
  if (needed == 0) {
    ::CloseHandle(token);
    return std::wstring();
  }
  std::vector<BYTE> buf(needed);
  std::wstring result;
  if (::GetTokenInformation(token, TokenUser, buf.data(), needed, &needed)) {
    LPWSTR str = nullptr;
    const TOKEN_USER* tu = reinterpret_cast<const TOKEN_USER*>(buf.data());
    if (::ConvertSidToStringSidW(tu->User.Sid, &str) && str) {
      result = str;
      ::LocalFree(str);
    }
  }
  ::CloseHandle(token);
  return result;
}

std::wstring RimePipeName() {
  std::wstring sid = CurrentUserSidString();
  if (sid.empty()) sid = L"unknown";
  return L"\\\\.\\pipe\\luminakey." + sid + L".v1";
}

std::wstring RimeServiceQuitEventName() {
  std::wstring sid = CurrentUserSidString();
  if (sid.empty()) sid = L"unknown";
  return L"Local\\LuminaKeyServiceQuit." + sid;
}

std::wstring RimeServiceMutexName() {
  std::wstring sid = CurrentUserSidString();
  if (sid.empty()) sid = L"unknown";
  return L"Local\\LuminaKeyService." + sid;
}

std::wstring RimeSettingsEventName() {
  return L"Local\\LuminaKeySettings." + CurrentUserSidString();
}

std::string GuidToUtf8(const GUID& g) {
  wchar_t buf[64] = {0};
  if (::StringFromGUID2(g, buf, 64) == 0) return std::string();
  return WideToUtf8(buf);
}

bool IsProcessElevated() {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
    return true;  // 問不到就當成是。
  TOKEN_ELEVATION elevation{};
  DWORD size = sizeof(elevation);
  bool elevated = true;
  if (::GetTokenInformation(token, TokenElevation, &elevation, size, &size))
    elevated = elevation.TokenIsElevated != 0;
  ::CloseHandle(token);
  return elevated;
}

namespace {

// SID 字串是不是機器帳號。
//
// ⚠ 用字串比對而不是 IsWellKnownSid:這三個值是**寫死在 Windows 裡的常數**,
//   不會在地化、不會隨機器變。而且我們的具名管道名字裡帶的就是這個字串
//   (RimePipeName),所以「名字裡的那個 SID 是不是一個人」與「可不可以
//   啟動服務」問的是同一個東西 —— 用同一個值回答比較不會漂移。
bool SidIsServiceAccount(const std::wstring& sid) {
  return sid == L"S-1-5-18" ||  // LocalSystem
         sid == L"S-1-5-19" ||  // NT AUTHORITY\\LOCAL SERVICE
         sid == L"S-1-5-20";    // NT AUTHORITY\\NETWORK SERVICE
}

// 內建的 Administrator 帳號:SID 的最後一段(RID)是 500。
// 只作診斷用,不參與判定。
bool SidIsBuiltinAdministrator(const std::wstring& sid) {
  const size_t dash = sid.rfind(L'-');
  if (dash == std::wstring::npos) return false;
  return sid.compare(dash + 1, std::wstring::npos, L"500") == 0;
}

}  // namespace

HostElevationFacts QueryHostElevation() {
  HostElevationFacts f;
  f.sid = CurrentUserSidString();
  f.service_account = SidIsServiceAccount(f.sid);
  f.builtin_administrator = SidIsBuiltinAdministrator(f.sid);

  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
    // token_query_ok 留在 false → 判定是 kUnknown → 不啟動。
    f.verdict = ClassifyHostElevation(false, true, TokenSplit::kUnknown,
                                      f.service_account);
    return f;
  }

  bool ok = !f.sid.empty();  // SID 問不到的話,管道名也算不出來 —— 一樣是壞的

  TOKEN_ELEVATION elevation{};
  DWORD size = sizeof(elevation);
  if (::GetTokenInformation(token, TokenElevation, &elevation, size, &size))
    f.is_elevated = elevation.TokenIsElevated != 0;
  else
    ok = false;

  // ⚠ 這一個查詢才是整格的重點:「這個身分有沒有第二份權杖」。
  //   TokenElevation 只說高不高,而高不高不是我們要問的問題。
  TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
  size = sizeof(type);
  if (::GetTokenInformation(token, TokenElevationType, &type, size, &size)) {
    switch (type) {
      case TokenElevationTypeDefault:
        f.split = TokenSplit::kNoLinkedToken;
        break;
      case TokenElevationTypeFull:
        f.split = TokenSplit::kFullWithLinked;
        break;
      case TokenElevationTypeLimited:
        f.split = TokenSplit::kLimitedWithLinked;
        break;
      default:
        f.split = TokenSplit::kUnknown;
        ok = false;
        break;
    }
  } else {
    ok = false;
  }

  ::CloseHandle(token);
  f.token_query_ok = ok;
  f.verdict = ClassifyHostElevation(ok, f.is_elevated, f.split,
                                    f.service_account);
  return f;
}

HostElevation ClassifyHostElevation() { return QueryHostElevation().verdict; }

// ⚠ 資料夾名寫在這一個地方,而且只有這一個地方。
//   產品改名時改這裡;改別處等於製造一個「刪了一個不存在的資料夾然後
//   回報成功」的缺陷。
static const wchar_t kUserDataFolderName[] = L"LuminaKey";

const wchar_t* RimeUserDataFolderName() { return kUserDataFolderName; }

std::wstring RimeUserDataDir() {
  wchar_t buf[32768];
  const DWORD n = ::GetEnvironmentVariableW(L"APPDATA", buf, 32768);
  if (n == 0 || n >= 32768) return std::wstring();
  std::wstring dir(buf, n);
  // %APPDATA% 理論上不會以反斜線結尾,但拼路徑時多防一次是零成本的 ——
  // 而拼錯的結果是一個帶雙反斜線的路徑,某些 API 吃、某些不吃。
  while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
    dir.pop_back();
  if (dir.empty()) return std::wstring();
  return dir + L"\\" + kUserDataFolderName;
}

std::wstring ModuleDirectory(HMODULE module) {
  std::vector<wchar_t> buf(MAX_PATH);
  for (;;) {
    const DWORD n =
        ::GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return std::wstring();
    if (n < buf.size() - 1) break;
    // 路徑可能超過 MAX_PATH(長路徑或很深的安裝目錄)。截斷之後拼出來的
    // 服務路徑會指到不存在的檔案,而症狀是「輸入法沒反應」——查不出原因。
    buf.resize(buf.size() * 2);
    if (buf.size() > 65536) return std::wstring();
  }
  std::wstring path(buf.data());
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return std::wstring();
  return path.substr(0, slash);
}

}  // namespace rimewin
