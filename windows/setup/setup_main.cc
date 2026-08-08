// windows/setup/setup_main.cc — rime_ime_setup.exe
//
// 安裝程式的手腳。一支很小的執行檔,不連 librime,只做四件事:
//
//   register / unregister    全機註冊(HKLM),需要系統管理員權限
//   enable-user / disable-user  目前使用者的啟用(HKCU)
//   check                    「真的註冊好了嗎」,CI 靠它斷言
//   stop-service             停掉 rime_service.exe(升級與解除安裝前)
//
// ── 為什麼是一支獨立的 exe,而不是叫 regsvr32 ──────────────────
//
// 1. **使用者不該看到 regsvr32。** 這一輪的目標是「下載、雙擊、下一步、
//    裝好、能用」。regsvr32 是給開發者的。
// 2. **regsvr32 失敗時只給一個 HRESULT。** 少了哪一個子鍵、路徑指到哪、
//    TSF 收不收得下 —— 一個都問不到。而「註冊看起來成功、其實輸入法
//    在市集 App 裡不存在」正是這一類問題的長相。
// 3. **CI 要能斷言。** check 這個動詞讓「輸入法有沒有被系統接受」從
//    windows/README.md 的「驗不了」那一欄搬到「驗得了」那一欄。
//
// 進入點是 main 而不是 wmain:全專案一致(理由見 service/main.cc 檔頭的
// glog / __argv 那一段)。參數仍然走 CommandLineToArgvW —— 窄字元 argv
// 走系統 ANSI 代碼頁,安裝路徑裡有中文時會被換成 '?'。

#include <windows.h>
// WIN32_LEAN_AND_MEAN 之下 windows.h 不帶這兩個。
#include <shellapi.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "../tsf/registration.h"
#include "../tsf/registration_check.h"
#include "../winshared/winutil.h"

using namespace rimewin;

namespace {

void Say(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stdout, fmt, ap);
  va_end(ap);
  std::fflush(stdout);
}

// 自己寫,不用 _wcsicmp / CompareStringOrdinal:路徑與檔名只會是 ASCII 的
// 大小寫問題,而 CRT 的那幾支在不同標頭裡的可見性各版本不一樣,
// 為了一個小寫轉換去追標頭不值得。
std::wstring LowerW(std::wstring s) {
  for (wchar_t& c : s)
    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
  return s;
}

void Usage() {
  Say(
      "用法: rime_ime_setup.exe <動詞> [選項]\n"
      "\n"
      "  register [--dll <路徑>]    全機註冊(需要系統管理員權限)\n"
      "  unregister                 全機反註冊\n"
      "  enable-user                把輸入法加進**目前使用者**的清單\n"
      "  disable-user               反之\n"
      "  check [--dll <路徑>] [--user]\n"
      "                             斷言註冊狀態;不通過就以非零結束\n"
      "  paths                      印出所有會被寫到的登錄檔路徑與 GUID\n"
      "  stop-service [--dir <目錄>]  停掉 rime_service.exe\n"
      "  dump                       印出登錄檔實況(診斷用)\n");
}

std::wstring DefaultDllPath() {
  const std::wstring dir = ModuleDirectory(nullptr);
  if (dir.empty()) return std::wstring();
  return dir + L"\\rime_tsf.dll";
}

// ── stop-service ──────────────────────────────────────────────────
//
// 兩段式,而且順序不能反過來:
//
//   1. **先好好地請它結束。** 服務進程持有使用者詞庫的 LevelDB;
//      直接 TerminateProcess 等於在寫入中途拔電源,而 LevelDB 壞掉的
//      症狀是「輸入法還能打字,但學過的詞全沒了」——使用者只會覺得
//      「升級之後我的詞不見了」,不會聯想到解除安裝程序。
//   2. 沒反應才動手。這一步限定在**安裝目錄底下的**那支執行檔,
//      不會誤殺同名的別支程式。
bool SignalQuitEvent() {
  const std::wstring name = RimeServiceQuitEventName();
  HANDLE ev = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
  if (!ev) {
    Say("  (沒有 %s —— 服務多半沒在跑)\n", WideToUtf8(name).c_str());
    return false;
  }
  ::SetEvent(ev);
  ::CloseHandle(ev);
  Say("  已送出結束訊號: %s\n", WideToUtf8(name).c_str());
  return true;
}

// 回傳仍在跑、且執行檔位於 dir 底下的 rime_service.exe 的 pid。
std::vector<DWORD> FindServiceProcesses(const std::wstring& dir) {
  std::vector<DWORD> pids;
  HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return pids;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  const std::wstring dir_lower = LowerW(dir);
  if (::Process32FirstW(snap, &pe)) {
    do {
      if (LowerW(pe.szExeFile) != L"rime_service.exe") continue;
      HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                               pe.th32ProcessID);
      if (!h) continue;
      wchar_t path[32768];
      DWORD n = 32768;
      bool match = false;
      if (::QueryFullProcessImageNameW(h, 0, path, &n)) {
        const std::wstring p = LowerW(std::wstring(path, n));
        // 限定在安裝目錄底下。沒有這一條的話,開發機上正在跑的建置樹版本
        // 也會被殺掉,而那是別人的東西。
        match = dir_lower.empty() ||
                (p.size() >= dir_lower.size() &&
                 p.compare(0, dir_lower.size(), dir_lower) == 0);
      }
      ::CloseHandle(h);
      if (match) pids.push_back(pe.th32ProcessID);
    } while (::Process32NextW(snap, &pe));
  }
  ::CloseHandle(snap);
  return pids;
}

int StopService(const std::wstring& dir) {
  Say("停止 rime_service.exe(目錄 %s)\n", WideToUtf8(dir).c_str());
  SignalQuitEvent();

  // 給它 5 秒好好收尾(關管道、關詞庫)。
  for (int i = 0; i < 50; ++i) {
    if (FindServiceProcesses(dir).empty()) {
      Say("  服務已結束 ✓\n");
      return 0;
    }
    ::Sleep(100);
  }

  const std::vector<DWORD> pids = FindServiceProcesses(dir);
  for (DWORD pid : pids) {
    HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) {
      Say("  !! 打不開 pid %lu(權限不足)\n", static_cast<unsigned long>(pid));
      continue;
    }
    Say("  沒有回應,強制結束 pid %lu\n", static_cast<unsigned long>(pid));
    ::TerminateProcess(h, 0);
    ::CloseHandle(h);
  }
  // 刻意**不**因為停不掉就讓安裝/解除安裝失敗。停不掉的最壞後果是
  // 檔案被佔用,而那由 Inno 的 restartreplace 接手(重開機時換掉);
  // 為了它讓整個安裝失敗,對使用者是更差的結果。
  return 0;
}

int Report(const char* what, HRESULT hr) {
  if (SUCCEEDED(hr)) {
    Say("%s 成功\n", what);
    return 0;
  }
  Say("!! %s 失敗 hr=0x%08lX\n", what, static_cast<unsigned long>(hr));
  if (hr == E_ACCESSDENIED)
    Say("   (寫 HKLM 需要系統管理員權限 —— 這支程式應該由安裝程式提權後呼叫)\n");
  return 1;
}

}  // namespace

static int Run(int argc, wchar_t** argv);

int main(int, char**) {
  int argc = 0;
  LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (!argv) {
    std::fprintf(stderr, "CommandLineToArgvW 失敗\n");
    return 2;
  }
  const int rc = Run(argc, argv);
  ::LocalFree(argv);
  return rc;
}

static int Run(int argc, wchar_t** argv) {
  if (argc < 2) {
    Usage();
    return 2;
  }
  const std::wstring verb = argv[1];
  std::wstring dll = DefaultDllPath();
  std::wstring dir = ModuleDirectory(nullptr);
  bool want_user = false;

  for (int i = 2; i < argc; ++i) {
    const std::wstring a = argv[i];
    if (a == L"--dll" && i + 1 < argc) dll = argv[++i];
    else if (a == L"--dir" && i + 1 < argc) dir = argv[++i];
    else if (a == L"--user") want_user = true;
    else {
      Say("未知參數: %s\n", WideToUtf8(a).c_str());
      Usage();
      return 2;
    }
  }

  if (verb == L"paths") {
    // CI 的斷言腳本靠這一段確認自己查的是**產品自己宣稱**的那些鍵,
    // 而不是腳本作者憑印象抄的。GUID 仍然在腳本裡另外寫死一份做交叉比對:
    // GUID 變了就是換了一個輸入法,那時 CI 該紅。
    Say("CLSID=%s\n", WideToUtf8(ClsidString()).c_str());
    Say("HKLM_CLSID=%s\n", WideToUtf8(ClsidRegPath()).c_str());
    Say("HKLM_INPROC=%s\n", WideToUtf8(InprocRegPath()).c_str());
    Say("HKLM_CTF=%s\n", WideToUtf8(CtfTipRegPath()).c_str());
    Say("HKLM_CTF_CATEGORY=%s\n", WideToUtf8(CtfCategoryRegPath()).c_str());
    Say("CATEGORY_COUNT=%d\n", RegisteredCategoryCount());
    Say("CATEGORY_ITEMS=%d\n", ExpectedCategoryItemCount());
    Say("PROFILE_COUNT=%d\n", ProfileCount());
    // 一行一個語言,格式 PROFILE=<langid 十六進位>=<GUID>。
    // CI 的斷言腳本逐行讀,對每一個 langid 各查一次登錄檔 ——
    // 「至少有一個」不算過,那正是這次被使用者回報的問題的形狀。
    for (int i = 0; i < ProfileCount(); ++i)
      Say("PROFILE=0x%04X=%s\n", static_cast<unsigned>(ProfileLangId(i)),
          WideToUtf8(ProfileGuidString(i)).c_str());
    Say("DLL=%s\n", WideToUtf8(dll).c_str());
    return 0;
  }

  if (verb == L"register") {
    if (::GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
      Say("!! 找不到 %s\n", WideToUtf8(dll).c_str());
      return 1;
    }
    Say("註冊 %s\n", WideToUtf8(dll).c_str());
    return Report("全機註冊", RegisterTextService(dll));
  }

  if (verb == L"unregister") return Report("全機反註冊", UnregisterTextService());

  if (verb == L"enable-user") {
    // 提權時的 HKCU 是**提權那個帳號的**。安裝程式必須以 ExecAsOriginalUser
    // 呼叫這一支,否則「用系統管理員帳號提權裝完,登入的那個人清單裡什麼都沒有」。
    if (IsProcessElevated())
      Say("  ⚠ 這支目前是提權的 —— 寫進去的會是提權帳號的 HKCU,不是登入者的。\n");
    Say("  SID = %s\n", WideToUtf8(CurrentUserSidString()).c_str());
    return Report("啟用(目前使用者)", EnableForCurrentUser());
  }

  if (verb == L"disable-user")
    return Report("停用(目前使用者)", DisableForCurrentUser());

  if (verb == L"check") {
    CheckOptions opt;
    if (::GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES)
      opt.expect_dll_path = dll;
    opt.check_user = want_user;
    return CheckRegistration(opt) ? 0 : 1;
  }

  if (verb == L"dump") {
    DumpRegistration();
    return 0;
  }

  if (verb == L"stop-service") return StopService(dir);

  Say("未知動詞: %s\n", WideToUtf8(verb).c_str());
  Usage();
  return 2;
}
