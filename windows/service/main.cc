// windows/service/main.cc — 服務進程
//
// 這支執行檔是架構裡「重」的那一半:librime、rime_shell、詞庫、候選窗。
// 它**不**被載入到宿主進程裡,所以它可以慢、可以吃記憶體、可以崩 ——
// 崩了只是輸入法沒作用,不會把使用者正在編輯的文件一起帶走。
// 瘦 DLL 那一半在 windows/tsf/。
//
// 用法:
//   rime_service.exe                        正常執行(由 DLL 自動啟動)
//   rime_service.exe --no-ui                不建候選窗(CI 用)
//   rime_service.exe --shared <dir> --user <dir>
//   rime_service.exe --wait-deploy <秒>     等首次部署完成才開始服務
//   rime_service.exe --ready-file <path>    就緒後寫一個檔案(CI 用來等它起來)
//   rime_service.exe --quit-after <秒>      到時自己結束(CI 用,避免殘留進程)

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../winshared/winutil.h"
#include "cand_window.h"
#include "engine.h"
#include "pipe_server.h"
#include "rime_shell.h"

namespace {

std::string EnvUtf8(const wchar_t* name) {
  wchar_t buf[32768];
  const DWORD n = ::GetEnvironmentVariableW(name, buf, 32768);
  if (n == 0 || n >= 32768) return std::string();
  return rimewin::WideToUtf8(std::wstring(buf, n));
}

std::string DefaultSharedDir() {
  // 與 DLL / 服務同目錄底下的 data\shared。安裝包會把 core/data/shared 放在那裡。
  const std::wstring dir = rimewin::ModuleDirectory(nullptr);
  if (dir.empty()) return std::string();
  return rimewin::WideToUtf8(dir + L"\\data\\shared");
}

std::string DefaultUserDir() {
  const std::string appdata = EnvUtf8(L"APPDATA");
  if (appdata.empty()) return std::string();
  return appdata + "\\RimeQuad";
}

bool DirExists(const std::string& utf8) {
  if (utf8.empty()) return false;
  const DWORD attr = ::GetFileAttributesW(rimewin::Utf8ToWide(utf8).c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

void EnsureDir(const std::string& utf8) {
  if (utf8.empty()) return;
  ::CreateDirectoryW(rimewin::Utf8ToWide(utf8).c_str(), nullptr);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::string shared = EnvUtf8(L"RIME_SHARED_DATA_DIR");
  std::string user = EnvUtf8(L"RIME_USER_DATA_DIR");
  std::string log = EnvUtf8(L"RIME_LOG_DIR");
  bool no_ui = false;
  int wait_deploy = 0;
  int quit_after = 0;
  std::wstring ready_file;

  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    auto next = [&](std::string* out) {
      if (i + 1 < argc) *out = rimewin::WideToUtf8(argv[++i]);
    };
    if (a == L"--no-ui") no_ui = true;
    else if (a == L"--shared") next(&shared);
    else if (a == L"--user") next(&user);
    else if (a == L"--log") next(&log);
    else if (a == L"--wait-deploy" && i + 1 < argc) wait_deploy = _wtoi(argv[++i]);
    else if (a == L"--quit-after" && i + 1 < argc) quit_after = _wtoi(argv[++i]);
    else if (a == L"--ready-file" && i + 1 < argc) ready_file = argv[++i];
    else {
      std::fwprintf(stderr, L"未知參數: %s\n", a.c_str());
      return 2;
    }
  }

  if (shared.empty()) shared = DefaultSharedDir();
  if (user.empty()) user = DefaultUserDir();
  EnsureDir(user);

  if (!DirExists(shared)) {
    // 明確報出來。少了共用資料目錄的話,後面每一步都會「成功」,
    // 而使用者看到的是「輸入法有反應但一個候選都沒有」—— 最難查的失敗。
    std::fwprintf(stderr, L"找不到共用資料目錄: %hs\n", shared.c_str());
    return 1;
  }

  // 單一實例。每個宿主進程都會嘗試啟動服務(DLL 那邊有節流,但仍會撞在一起),
  // 沒有這道鎖的話會有好幾支服務同時開著同一份使用者詞庫。
  const std::wstring mutex_name =
      L"Local\\RimeQuadService." + rimewin::CurrentUserSidString();
  HANDLE single = ::CreateMutexW(nullptr, TRUE, mutex_name.c_str());
  if (!single || ::GetLastError() == ERROR_ALREADY_EXISTS) {
    // 已經有一支在跑,這不是錯誤。
    return 0;
  }

  // 候選窗的座標是螢幕座標,而宿主(TSF 的 GetTextExt)給的是實體像素。
  // 沒有宣告 DPI 感知的話,系統會對我們的視窗做縮放,候選窗在高 DPI 螢幕上
  // 會位移並且模糊。動態載入:這個 API 在較舊的系統上不存在。
  {
    using SetCtxFn = BOOL(WINAPI*)(HANDLE);
    HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
    SetCtxFn set_ctx =
        u32 ? reinterpret_cast<SetCtxFn>(reinterpret_cast<void*>(
                  ::GetProcAddress(u32, "SetProcessDpiAwarenessContext")))
            : nullptr;
    // -4 = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    if (set_ctx) set_ctx(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
  }

  std::fwprintf(stdout, L"[service] shell ABI = %d\n", rs_abi_version());
  std::fwprintf(stdout, L"[service] shared = %hs\n", shared.c_str());
  std::fwprintf(stdout, L"[service] user   = %hs\n", user.c_str());
  std::fflush(stdout);

  rimewin::Engine engine;
  if (!engine.Start(shared, user, log)) {
    std::fwprintf(stderr, L"rs_init 失敗: %hs\n", engine.last_error().c_str());
    return 1;
  }
  if (wait_deploy > 0 && !engine.WaitDeploy(wait_deploy)) {
    // 部署失敗時 rs_last_error() 是空字串 —— librime 的 C API 不提供原因
    // (docs/handoff-windows.md §5)。所以這裡只能說「失敗了」,
    // 不要假裝知道為什麼。
    std::fwprintf(stderr, L"部署未在 %d 秒內成功\n", wait_deploy);
    return 1;
  }

  rimewin::NullCandidateUi null_ui;
  rimewin::CandidateWindow window;
  rimewin::CandidateUi* ui = &null_ui;
  if (!no_ui) {
    if (window.Start())
      ui = &window;
    else
      std::fwprintf(stderr, L"候選窗建立失敗,改以無 UI 模式繼續\n");
  }

  rimewin::PipeServer server(&engine, ui);
  if (!server.Start()) {
    std::fwprintf(stderr, L"具名管道建立失敗(可能已經有一支服務在跑)\n");
    return 1;
  }

  if (!ready_file.empty()) {
    HANDLE f = ::CreateFileW(ready_file.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      ::WriteFile(f, "ready", 5, &written, nullptr);
      ::CloseHandle(f);
    }
  }
  std::fwprintf(stdout, L"[service] ready\n");
  std::fflush(stdout);

  if (quit_after > 0) {
    ::Sleep(static_cast<DWORD>(quit_after) * 1000);
  } else {
    // 沒有結束條件時就一直跑。真正的結束是使用者登出或手動結束進程 ——
    // 本輪還沒有系統匣圖示,那是下一輪的事(已列在 README)。
    for (;;) ::Sleep(60000);
  }

  server.Stop();
  window.Stop();
  engine.Stop();
  ::ReleaseMutex(single);
  ::CloseHandle(single);
  return 0;
}
