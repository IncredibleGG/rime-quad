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

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../winshared/winutil.h"
#include "cand_window.h"
#include "engine.h"
#include "pipe_server.h"
#include "rime_shell.h"

namespace {

// 全部輸出走**窄字元 UTF-8**,不用 fwprintf。
//
// 兩個理由,而第二個是實際踩到的:
//   1. tools/rime_console.cc 已經證明這條路在 Windows 上是對的 ——
//      輸出導進檔案,拿到的是原始 UTF-8 位元組,不經過主控台的代碼頁轉換。
//   2. 在同一個 FILE* 上混用寬字元與窄字元 I/O 是未定義行為,而 librime
//      與 glog 用的是窄字元。我們先 fwprintf 過的話,之後它們寫同一條串流
//      的行為就沒有保證了。
void Say(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stdout, fmt, ap);
  va_end(ap);
  std::fflush(stdout);
}

void Err(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fflush(stderr);
}

// 服務進程崩掉時,使用者看到的只是「輸入法忽然沒反應了」——
// 而 DLL 那邊會 fail-open,連錯誤訊息都沒有。至少把例外碼與位址印出來,
// 讓「服務崩了」與「服務沒起來」變成兩件分得開的事。
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
  if (info && info->ExceptionRecord) {
    // 位址本身沒有用:ASLR 每次都不一樣。真正查得動的是 **RVA**
    // (位址減掉模組基底),因為那個可以拿去對連結器產生的 .map 檔。
    // rime_service 連結時帶了 /MAP,而 CI 會把 .map 一起上傳。
    const unsigned char* base =
        reinterpret_cast<const unsigned char*>(::GetModuleHandleW(nullptr));
    const unsigned char* at = reinterpret_cast<const unsigned char*>(
        info->ExceptionRecord->ExceptionAddress);
    Err("[service] **崩潰** 例外碼 0x%08lX 位址 %p 基底 %p",
        static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode),
        static_cast<const void*>(at), static_cast<const void*>(base));
    if (base && at > base)
      Err(" RVA 0x%08llX", static_cast<unsigned long long>(at - base));
    Err("\n");
  } else {
    Err("[service] **崩潰**(沒有例外資訊)\n");
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

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
  ::SetUnhandledExceptionFilter(&CrashFilter);

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
      Err("未知參數: %s\n", rimewin::WideToUtf8(a).c_str());
      return 2;
    }
  }

  if (shared.empty()) shared = DefaultSharedDir();
  if (user.empty()) user = DefaultUserDir();
  EnsureDir(user);
  // log 為空時傳 ""(只寫 stderr)而**不是** NULL(交給 librime 決定暫存目錄)。
  // rime_shell.h 明說兩者語意不同,而 tools/rime_console.cc 走的是 "" 那條 ——
  // 那條已經在這個 runner 上驗證過很多次了。服務進程的 stderr 本來就會被
  // 導進檔案,沒有必要另外開一份 glog 的檔案日誌。

  if (!DirExists(shared)) {
    // 明確報出來。少了共用資料目錄的話,後面每一步都會「成功」,
    // 而使用者看到的是「輸入法有反應但一個候選都沒有」—— 最難查的失敗。
    Err("找不到共用資料目錄: %s\n", shared.c_str());
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

  Say("[service] shell ABI = %d\n", rs_abi_version());
  Say("[service] shared = %s\n", shared.c_str());
  Say("[service] user   = %s\n", user.c_str());
  Say("[service] log    = %s\n", log.empty() ? "(stderr)" : log.c_str());

  rimewin::Engine engine;
  Say("[service] rs_init ...\n");
  if (!engine.Start(shared, user, log)) {
    Err("rs_init 失敗: %s\n", engine.last_error().c_str());
    return 1;
  }
  Say("[service] rs_init OK\n");
  if (wait_deploy > 0 && !engine.WaitDeploy(wait_deploy)) {
    // 部署失敗時 rs_last_error() 是空字串 —— librime 的 C API 不提供原因
    // (docs/handoff-windows.md §5)。所以這裡只能說「失敗了」,
    // 不要假裝知道為什麼。
    Err("部署未在 %d 秒內成功\n", wait_deploy);
    return 1;
  }
  if (wait_deploy > 0) Say("[service] 部署完成\n");

  rimewin::NullCandidateUi null_ui;
  rimewin::CandidateWindow window;
  rimewin::CandidateUi* ui = &null_ui;
  if (!no_ui) {
    if (window.Start())
      ui = &window;
    else
      Err("候選窗建立失敗,改以無 UI 模式繼續\n");
  }

  Say("[service] 候選窗 %s\n", no_ui ? "(--no-ui,不建)" : "已就緒");

  rimewin::PipeServer server(&engine, ui);
  if (!server.Start()) {
    Err("具名管道建立失敗(可能已經有一支服務在跑)\n");
    return 1;
  }
  Say("[service] 管道 = %s\n",
      rimewin::WideToUtf8(rimewin::RimePipeName()).c_str());

  if (!ready_file.empty()) {
    HANDLE f = ::CreateFileW(ready_file.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      ::WriteFile(f, "ready", 5, &written, nullptr);
      ::CloseHandle(f);
    }
  }
  Say("[service] ready\n");

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
