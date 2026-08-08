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
//   rime_service.exe --print-dirs           只印出解析出來的三個目錄就結束
//
// ── 資料目錄:安裝到 Program Files 之後,這一格是最容易錯的 ────────
//
//   shared  <安裝目錄>\data\shared    唯讀。方案、詞庫、opencc 詞典。
//   seed    <安裝目錄>\data\user      唯讀**範本**,首次執行時複製過去。
//   user    %APPDATA%\RimeQuad        可寫。使用者詞典、設定、編譯產物。
//
// 三件必須講清楚的事:
//
// 1. **使用者資料絕對不可以放在安裝目錄底下。** 裝在 C:\Program Files 之下的
//    話,一般權限的進程寫不進去 —— 而 librime 寫不進去時不會停,它會繼續跑,
//    然後使用者學過的詞一個都留不住。沒有錯誤訊息。所以 user 一律走 %APPDATA%,
//    而且底下有一道明確的檢查擋住「user 落在安裝目錄裡」這件事。
//
// 2. **範本要複製,不是直接當使用者目錄用。** core/data/user 裡的
//    default.custom.yaml 把 schema_list 限縮成實際打包的四個方案;
//    少了它,librime 會照上游 default.yaml 去部署 cangjie5 / quick5 等
//    我們根本沒有詞庫的方案。複製時**只補不覆蓋**,使用者改過的不能被裝回去。
//
// 3. librime 的編譯產物(.bin)進的是 <user>\build,不是 shared ——
//    所以唯讀的安裝目錄是成立的。CI 有一道斷言在守這件事:跑完一輪之後
//    安裝目錄底下的檔案清單與時間戳必須一個位元都沒變。

#include <windows.h>
// WIN32_LEAN_AND_MEAN 之下 windows.h 不會帶進 shellapi.h,而我們要
// CommandLineToArgvW。
#include <shellapi.h>

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

// 首次執行要複製過去的範本(core/data/user 的內容)。
std::string DefaultSeedDir() {
  const std::wstring dir = rimewin::ModuleDirectory(nullptr);
  if (dir.empty()) return std::string();
  return rimewin::WideToUtf8(dir + L"\\data\\user");
}

// %APPDATA%\RimeQuad(Roaming)。
//
// 為什麼是 Roaming 而不是 Local:這裡放的是**使用者的資料** —— 自訂短語、
// 學習過的詞、設定。跟著人走是對的,而 RIME 生態(Weasel 用 %APPDATA%\Rime)
// 也是這個慣例,使用者搬移或備份時找得到。
// 代價是編譯產物(<user>\build)也跟著漫遊,在有漫遊設定檔的網域環境裡會變重。
// 那是已知的取捨,不是沒想過。
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

std::wstring LowerW(std::wstring s) {
  for (wchar_t& c : s)
    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
  return s;
}

// 使用者目錄不可以落在安裝目錄底下。
//
// 這不是潔癖。裝在 C:\Program Files 之下時,一般權限的進程對那棵樹只有讀取權;
// 而 librime 寫不進使用者目錄時**不會停下來**,它會照常給候選、照常上屏,
// 只是一個學過的詞都留不住,而且完全沒有錯誤訊息。等使用者發現「它從來沒學會
// 我的詞」已經是好幾天以後,而那時沒有任何線索指向權限。
bool UserDirInsideInstallDir(const std::string& user_utf8) {
  const std::wstring install = LowerW(rimewin::ModuleDirectory(nullptr));
  if (install.empty()) return false;
  const std::wstring user = LowerW(rimewin::Utf8ToWide(user_utf8));
  return user.size() >= install.size() &&
         user.compare(0, install.size(), install) == 0;
}

// 把範本目錄裡的檔案補進使用者目錄。**只補不覆蓋。**
//
// bFailIfExists = TRUE 是這件事的全部重點:使用者改過的 default.custom.yaml
// 不可以在每次啟動(或每次升級)時被裝回原樣。
// 只做一層,不遞迴 —— core/data/user 目前就是平的一層,而「遞迴複製到使用者
// 資料目錄」是一個哪天資料變複雜就會出事的動作,要做的時候應該是明著決定的。
int SeedUserDir(const std::string& seed_utf8, const std::string& user_utf8) {
  if (seed_utf8.empty() || !DirExists(seed_utf8)) return 0;
  const std::wstring seed = rimewin::Utf8ToWide(seed_utf8);
  const std::wstring user = rimewin::Utf8ToWide(user_utf8);
  WIN32_FIND_DATAW fd{};
  HANDLE h = ::FindFirstFileW((seed + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  int copied = 0;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    if (::CopyFileW((seed + L"\\" + fd.cFileName).c_str(),
                    (user + L"\\" + fd.cFileName).c_str(), TRUE))
      ++copied;
  } while (::FindNextFileW(h, &fd));
  ::FindClose(h);
  return copied;
}

}  // namespace

// ⚠⚠ 進入點必須是 **main**,不可以是 wmain。⚠⚠
//
// glog 的 ProgramInvocationShortName() 在 Windows/MSVC 上走這一條
// (deps/glog/src/utilities.cc,HAVE___ARGV 分支):
//
//     return const_basename(__argv[0]);
//
// 而 `__argv` **只有在 CRT 以窄字元進入點啟動時才會被填**。用 wmain 的話
// CRT 只填 __wargv,__argv 是 NULL —— 於是 librime 一呼叫
// google::InitGoogleLogging(SetupLogging 裡)就是空指標解參考。
//
// 這不是理論。CI run #16–#19 每一次都在同一個位置崩:0xC0000005,
// RVA 對到 glog utilities.cc 的 ProgramInvocationShortName+0x22。
// 而同一個 job 裡的 tools/rime_console.cc —— 同一份 rime_shell.cc、
// 同一批靜態庫、同一份資料 —— 完全正常,因為**它用的是 main**。
// 「差在哪」就差在這裡,而症狀(服務進程一啟動就死)看起來與進入點毫無關聯。
//
// 參數仍然取寬字元版本:窄字元的 argv 走 ANSI 代碼頁,使用者名稱或安裝
// 路徑裡有非 ANSI 字元(中文使用者目錄很常見)時會被換成 '?',而那正好是
// %APPDATA% 底下的使用者資料目錄。
static int RunService(int argc, wchar_t** argv);

int main(int /*narrow_argc*/, char** /*narrow_argv*/) {
  int argc = 0;
  LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (!argv) {
    std::fprintf(stderr, "CommandLineToArgvW 失敗\n");
    return 2;
  }
  const int rc = RunService(argc, argv);
  ::LocalFree(argv);
  return rc;
}

static int RunService(int argc, wchar_t** argv) {
  ::SetUnhandledExceptionFilter(&CrashFilter);

#ifdef _MSC_VER
  // 上面那段註解說的事,在這裡變成一道會講人話的檢查。
  // 進入點若哪天被改回 wchar_t 版本,這一行會直接說出原因;
  // 沒有它的話,症狀是 librime 深處的一個空指標解參考,
  // 而那要花掉五輪 CI 才查得到(#16–#19 就是)。
  if (__argv == nullptr || __argv[0] == nullptr) {
    Err("[service] __argv 是空的 —— 進入點必須是 main 而不是 wmain。\n"
        "  glog 的 ProgramInvocationShortName() 會走 const_basename(__argv[0]),\n"
        "  用寬字元進入點時 CRT 只填 __wargv,那裡就會是空指標解參考。\n");
    return 1;
  }
#endif

  std::string shared = EnvUtf8(L"RIME_SHARED_DATA_DIR");
  std::string user = EnvUtf8(L"RIME_USER_DATA_DIR");
  std::string log = EnvUtf8(L"RIME_LOG_DIR");
  std::string seed;
  bool no_ui = false;
  bool print_dirs = false;
  int wait_deploy = 0;
  int quit_after = 0;
  std::wstring ready_file;

  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    auto next = [&](std::string* out) {
      if (i + 1 < argc) *out = rimewin::WideToUtf8(argv[++i]);
    };
    if (a == L"--no-ui") no_ui = true;
    else if (a == L"--print-dirs") print_dirs = true;
    else if (a == L"--shared") next(&shared);
    else if (a == L"--user") next(&user);
    else if (a == L"--seed") next(&seed);
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
  if (seed.empty()) seed = DefaultSeedDir();

  if (user.empty()) {
    Err("解析不出使用者資料目錄(%%APPDATA%% 是空的?)\n");
    return 1;
  }
  if (UserDirInsideInstallDir(user)) {
    // 見上面 UserDirInsideInstallDir 的說明。寧可現在就大聲停下來,
    // 也不要讓使用者用了三天才發現輸入法從來沒記住過任何東西。
    Err("使用者資料目錄落在安裝目錄底下: %s\n"
        "  安裝目錄(通常是 C:\\Program Files\\RimeQuad)對一般權限的進程是唯讀的,\n"
        "  而 librime 寫不進去時**不會報錯**,只是一個學過的詞都留不住。\n"
        "  使用者資料應該在 %%APPDATA%%\\RimeQuad。\n",
        user.c_str());
    return 1;
  }

  // --print-dirs 放在這裡:三個目錄都解析完、但還沒動任何狀態(沒有建目錄、
  // 沒有搶單一實例的 mutex、沒有起引擎)。CI 靠它在安裝完成後**立刻**斷言
  // 「shared 指到 Program Files、user 指到 %APPDATA%」,不必等好幾分鐘的詞庫編譯。
  if (print_dirs) {
    Say("shared=%s\n", shared.c_str());
    Say("user=%s\n", user.c_str());
    Say("seed=%s\n", seed.c_str());
    Say("log=%s\n", log.c_str());
    return 0;
  }

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

  // 首次執行:把 <安裝目錄>\data\user 的範本補進使用者目錄。
  // 少了 default.custom.yaml 的話,librime 會照上游 default.yaml 去部署
  // cangjie5 / quick5 等我們沒有詞庫的方案 —— 部署會噴錯,而使用者看到的是
  // 「有些方案切過去就一個候選都沒有」。
  {
    const int copied = SeedUserDir(seed, user);
    if (copied > 0) Say("[service] 從範本補上 %d 個檔案\n", copied);
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

  // 結束事件在這裡就建好,不等到最後才建。
  //
  // 首次部署要編好幾分鐘的詞庫,而使用者很可能正好在那段時間裡按下解除安裝。
  // 事件晚建的話,那幾分鐘裡 stop-service 找不到東西可以通知,只能強制結束 ——
  // 而那正是最不該強制結束的一段(詞庫正在寫)。
  HANDLE quit = ::CreateEventW(nullptr, TRUE, FALSE,
                               rimewin::RimeServiceQuitEventName().c_str());
  if (!quit)
    Err("[service] 結束事件建立失敗 —— 停止服務只能靠結束進程\n");

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

  // 等結束訊號,而不是無止境地 Sleep。
  //
  // 升級與解除安裝之前必須停掉這支進程,否則 rime_service.exe 與
  // rime_tsf.dll 都被佔用著換不掉。而**不可以**直接 TerminateProcess:
  // 這支進程持有使用者詞庫的 LevelDB,從中途拔掉的話詞庫會壞,
  // 而症狀是「升級之後我學過的詞全沒了」——使用者不會把它跟安裝程序聯想在一起。
  // rime_ime_setup.exe stop-service 送的就是這個事件(在上面建好)。
  if (quit) {
    const DWORD wait_ms =
        quit_after > 0 ? static_cast<DWORD>(quit_after) * 1000 : INFINITE;
    ::WaitForSingleObject(quit, wait_ms);
    ::CloseHandle(quit);
  } else {
    // 建不出事件不該讓服務起不來 —— 退回舊行為,只是停不掉而已。
    if (quit_after > 0)
      ::Sleep(static_cast<DWORD>(quit_after) * 1000);
    else
      for (;;) ::Sleep(60000);
  }
  Say("[service] 結束,收尾中\n");

  server.Stop();
  window.Stop();
  engine.Stop();
  ::ReleaseMutex(single);
  ::CloseHandle(single);
  return 0;
}
