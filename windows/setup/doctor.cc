#include "doctor.h"

#include <msctf.h>
#include <windows.h>
// WIN32_LEAN_AND_MEAN 之下 windows.h 不帶這幾個。
#include <tlhelp32.h>

#include <io.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "../tsf/guids.h"
#include "../tsf/ipc_client.h"
#include "../tsf/registration.h"
#include "../tsf/registration_check.h"
#include "../tsf/trace.h"
#include "../tsf/win32_oracle.h"
#include "../winshared/winutil.h"

namespace rimewin {
namespace {

// 報告先累積成一整份 UTF-8 字串,最後才一次印出來 / 寫檔。
//
// 不邊做邊印的理由:使用者要把它整份貼給我們,而中間穿插著 Windows API
// 可能吐出的東西(例如 CheckRegistration 自己的輸出)會讓那份貼上的東西
// 少掉一半。統一由這裡管,順序與內容就是確定的。
class Report {
 public:
  int fails = 0;
  int warns = 0;

  void Line(const std::string& s) { buf_ += s; buf_ += "\r\n"; }
  void Blank() { buf_ += "\r\n"; }

  void Head(const char* title) {
    Line("");
    Line(std::string("── ") + title + " " +
         std::string(60 > 6 + Len(title) ? 60 - 6 - Len(title) : 3, '-'));
  }

  void Pass(const std::string& s) { Line("  [PASS] " + s); }
  void Info(const std::string& s) { Line("  [INFO] " + s); }
  void Warn(const std::string& s) {
    ++warns;
    Line("  [WARN] " + s);
  }
  // ⚠ FAIL 後面一定要接「所以呢」。一句「註冊失敗」對使用者沒有用,
  //   對接到回報的人也只是換一種方式說「不能用」。
  void Fail(const std::string& s, const std::string& what_now) {
    ++fails;
    Line("  [FAIL] " + s);
    if (!what_now.empty()) Line("         → " + what_now);
  }
  void Note(const std::string& s) { Line("         " + s); }

  const std::string& text() const { return buf_; }

 private:
  static size_t Len(const char* s) {
    size_t n = 0;
    while (s[n]) ++n;
    return n;
  }
  std::string buf_;
};

std::string Fmt(const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  const int n = ::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return std::string();
  return std::string(buf, static_cast<size_t>(n < static_cast<int>(sizeof(buf))
                                                  ? n
                                                  : sizeof(buf) - 1));
}

std::string W(const std::wstring& s) { return WideToUtf8(s); }

bool FileExists(const std::wstring& p) {
  const DWORD a = ::GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool DirExists(const std::wstring& p) {
  const DWORD a = ::GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

long long FileSize(const std::wstring& p) {
  WIN32_FILE_ATTRIBUTE_DATA d{};
  if (!::GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return -1;
  return (static_cast<long long>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

// ── 1. 檔案 ───────────────────────────────────────────────────────
void SectionFiles(Report& r, const std::wstring& dir) {
  r.Head("1. 安裝的檔案");
  r.Info("安裝目錄: " + W(dir));

  struct Item {
    const wchar_t* rel;
    bool required;
    const char* why;
  };
  // ⚠ data\shared 那幾項不是湊數的。少了它們,前面每一步都會成功 ——
  //   服務起得來、輸入法註冊得上、按鍵也有反應 —— 然後一個候選都沒有,
  //   而且沒有任何錯誤訊息。這是這條路徑上最貴的失敗模式。
  static const Item kItems[] = {
      {L"rime_tsf.dll", true, "輸入法本體。少了它,系統載入不了這個輸入法"},
      {L"rime_service.exe", true, "引擎與全部 UI 都在這支裡。少了它,打不出字也沒有系統匣圖示"},
      {L"rime_ime_setup.exe", true, "註冊工具(就是你正在跑的這一支)"},
      {L"rime_console.exe", false, "引擎層自我檢查工具。少了它只是這份報告少一格"},
      {L"data\\shared\\default.yaml", true, "librime 的基礎配置。少了它部署直接失敗"},
      {L"data\\shared\\luna_pinyin_tw.schema.yaml", true, "預設方案"},
      {L"data\\shared\\luna_pinyin.dict.yaml", true, "詞庫。少了它有候選欄但沒有候選"},
      {L"data\\shared\\essay.txt", true, "語言模型。少了它候選排序會爛掉但不會報錯"},
      {L"data\\user\\default.custom.yaml", true,
       "把方案清單限縮成真的有詞庫的那四個。少了它,部分方案切過去沒有候選"},
  };
  for (const Item& it : kItems) {
    const std::wstring p = dir + L"\\" + it.rel;
    if (FileExists(p)) {
      r.Pass(Fmt("%s (%lld bytes)", W(it.rel).c_str(), FileSize(p)));
    } else if (it.required) {
      r.Fail(Fmt("缺少 %s", W(it.rel).c_str()),
             std::string(it.why) + "。請重新安裝一次。");
    } else {
      r.Info(Fmt("沒有 %s —— %s", W(it.rel).c_str(), it.why));
    }
  }
  const std::wstring opencc = dir + L"\\data\\shared\\opencc";
  if (DirExists(opencc))
    r.Pass("data\\shared\\opencc 存在(簡繁與臺灣字形轉換)");
  else
    r.Fail("沒有 data\\shared\\opencc", "簡繁轉換會完全失效。請重新安裝一次。");
}

// ── 2. 註冊 ───────────────────────────────────────────────────────
void SectionRegistration(Report& r, const std::wstring& dll) {
  r.Head("2. 註冊(系統認不認得這個輸入法)");

  // CheckRegistration 自己會印詳細內容到 stdout。這裡只要它的結論 ——
  // 詳細內容對使用者沒有用,而且會把這份報告淹掉。要看細節的人用
  // `rime_ime_setup.exe check` 與 `dump`。
  CheckOptions opt;
  if (FileExists(dll)) opt.expect_dll_path = dll;
  opt.check_enum = true;
  opt.check_user = false;

  // 暫時把 stdout 導到 NUL。
  //
  // ⚠ 用 _dup / _dup2 存回原本那個檔案描述子,**不要**用
  //   freopen("CON", ...) 還原。CI 是 `doctor > report.txt` 這樣跑的,
  //   而 "CON" 會把 stdout 接回主控台 —— 整份報告就寫不進那個檔案了,
  //   而症狀是「CI 上 doctor 的輸出是空的」,看起來像 doctor 壞掉。
  //
  // ⚠ 為什麼要導走:CheckRegistration 自己會印一大片細節。
  //   那些對使用者沒有用,而且會把這份報告淹掉。要看細節的人有
  //   `rime_ime_setup.exe check` 與 `dump` 兩個動詞。
  std::fflush(stdout);
  const int saved_fd = ::_dup(::_fileno(stdout));
  FILE* devnull = nullptr;
  const bool redirected = (::freopen_s(&devnull, "NUL", "w", stdout) == 0);
  const bool machine_ok = CheckRegistration(opt);
  opt.check_user = true;
  opt.check_enum = false;
  const bool user_ok = CheckRegistration(opt);
  std::fflush(stdout);
  if (redirected && saved_fd >= 0) {
    ::_dup2(saved_fd, ::_fileno(stdout));
    ::_close(saved_fd);
  }

  if (machine_ok) {
    r.Pass("全機註冊完整(HKLM 的 COM 與 TSF 鍵、三份語言設定檔、TSF 列舉得到我們)");
  } else {
    r.Fail("全機註冊不完整",
           "系統不認得這個輸入法。以系統管理員身分執行:\n"
           "         \"" + W(ModuleDirectory(nullptr)) +
               "\\rime_ime_setup.exe\" register\n"
           "         再跑一次 `rime_ime_setup.exe check` 看是哪一個鍵缺。");
  }
  if (user_ok) {
    r.Pass("目前這個使用者已啟用(HKCU 底下有語言設定檔)");
  } else {
    r.Fail("目前這個使用者沒有被啟用",
           "輸入法清單裡不會出現它。用**你自己的帳號**(不要提權)執行:\n"
           "         \"" + W(ModuleDirectory(nullptr)) +
               "\\rime_ime_setup.exe\" enable-user\n"
           "         若清單裡還是沒有,到「設定 → 時間與語言 → 語言與地區」\n"
           "         把「中文(繁體,台灣)」或「中文(簡體,中國)」加進語言清單。");
  }
}

// ── 3. 目前啟用的是哪一份 + 鍵盤佈局 ─────────────────────────────
void SectionProfileAndLayout(Report& r) {
  r.Head("3. 目前的語言設定檔與鍵盤佈局");

  ITfInputProcessorProfiles* profiles = nullptr;
  if (SUCCEEDED(::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_ITfInputProcessorProfiles,
                                   (void**)&profiles)) &&
      profiles) {
    ITfInputProcessorProfileMgr* mgr = nullptr;
    if (SUCCEEDED(profiles->QueryInterface(IID_ITfInputProcessorProfileMgr,
                                           (void**)&mgr)) &&
        mgr) {
      TF_INPUTPROCESSORPROFILE prof{};
      if (SUCCEEDED(mgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &prof))) {
        const bool ours = IsEqualCLSID(prof.clsid, CLSID_RimeTextService) != 0;
        r.Info(Fmt("目前啟用的輸入法 langid=0x%04X 是我們=%s",
                   static_cast<unsigned>(prof.langid), ours ? "是" : "否"));
        if (ours)
          r.Pass("這個工作階段目前用的就是本輸入法");
        else
          r.Info("現在用的是別的輸入法 —— 這一格只有在你**先切到本輸入法再跑**"
                 "這支診斷時才有意義。");
      } else {
        r.Warn("問不到目前啟用的輸入法(GetActiveProfile 失敗)");
      }
      mgr->Release();
    } else {
      r.Warn("這個系統沒有 ITfInputProcessorProfileMgr(很舊的 Windows?)");
    }
    profiles->Release();
  } else {
    r.Fail("建不出 TSF 的 InputProcessorProfiles 物件",
           "文字服務框架本身有問題,這台機器上任何 TSF 輸入法都不會正常。");
  }

  // ── 鍵盤佈局 ──────────────────────────────────────────────────
  //
  // ⚠ 這一格是這一輪查到的關鍵,值得把來龍去脈寫在報告裡。
  //
  // 我們是靠 ToUnicodeEx 問「這顆鍵在你的佈局上是什麼字」來把按鍵翻成
  // 引擎看得懂的 keysym(不能寫死,不然 Dvorak / 德文 / 法文使用者打出來
  // 的每一個字都是錯的)。而如果那一份佈局問不出任何字,**每一顆按鍵都會
  // 被原樣放行**:引擎收不到、連線不會建立、服務不會被啟動 ——
  // 「打不出中文」與「沒有任何 UI」同時發生,而且沒有任何錯誤訊息。
  HKL list[64] = {0};
  const UINT n = ::GetKeyboardLayoutList(64, list);
  r.Info(Fmt("這個工作階段掛著 %u 份輸入語言", static_cast<unsigned>(n)));
  int usable = 0;
  for (UINT i = 0; i < n && i < 64; ++i) {
    const unsigned long long v =
        static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(list[i]));
    const bool tip = Win32KeyboardOracle::LooksLikeTextServiceHkl(list[i]);
    const bool answers = Win32KeyboardOracle::LayoutAnswers(list[i]);
    if (answers) ++usable;
    r.Info(Fmt("  hkl=0x%08llX %s 問得出字=%s", v,
               tip ? "(文字服務/IME,不是實體佈局)" : "(鍵盤佈局)",
               answers ? "是" : "否"));
  }
  if (usable > 0) {
    r.Pass(Fmt("有 %d 份佈局問得出字 —— 按鍵翻得成 keysym", usable));
  } else {
    r.Fail("一份佈局都問不出字",
           "每一顆按鍵都會被原樣放行,引擎一顆都收不到,而且系統匣圖示與\n"
           "         設定視窗也不會出現(它們住在服務進程裡,而服務要有按鍵才會被啟動)。\n"
           "         請到「設定 → 時間與語言 → 語言與地區」確認語言底下有一份\n"
           "         實體鍵盤配置(例如「美式鍵盤」)。");
  }

  // 目前這條執行緒看到的那一個。
  //
  // ⚠ 要誠實:這支診斷是主控台程式,它的 HKL 不一定等於記事本裡的那一個。
  //   所以這一行是 INFO 而不是判斷 —— 真正的答案在除錯記錄裡
  //   (瘦 DLL 在 ActivateEx 當下記了一行「鍵盤佈局 hkl=…」)。
  const HKL cur = ::GetKeyboardLayout(0);
  r.Info(Fmt("這支診斷程式自己看到的 hkl=0x%08llX(僅供參考 —— 宿主進程裡的可能不同,"
             "以第 8 節的除錯記錄為準)",
             static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(cur))));
}

// ── 4. 服務進程 ───────────────────────────────────────────────────
std::vector<DWORD> FindProcessesNamed(const wchar_t* exe_name,
                                      std::wstring* first_path) {
  std::vector<DWORD> pids;
  HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return pids;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  if (::Process32FirstW(snap, &pe)) {
    do {
      std::wstring name = pe.szExeFile;
      for (wchar_t& c : name)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
      if (name != exe_name) continue;
      pids.push_back(pe.th32ProcessID);
      if (first_path && first_path->empty()) {
        HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 pe.th32ProcessID);
        if (h) {
          wchar_t path[32768];
          DWORD len = 32768;
          if (::QueryFullProcessImageNameW(h, 0, path, &len))
            first_path->assign(path, len);
          ::CloseHandle(h);
        }
      }
    } while (::Process32NextW(snap, &pe));
  }
  ::CloseHandle(snap);
  return pids;
}

void SectionService(Report& r, const std::wstring& dir) {
  r.Head("4. 服務進程(引擎、候選窗、系統匣圖示、設定視窗都在它裡面)");

  std::wstring path;
  const std::vector<DWORD> pids = FindProcessesNamed(L"rime_service.exe", &path);
  const bool mutex_held = ServiceIsRunning();

  if (!pids.empty()) {
    std::string ids;
    for (DWORD p : pids) ids += Fmt("%lu ", static_cast<unsigned long>(p));
    r.Pass(Fmt("rime_service.exe 在跑(pid %s)", ids.c_str()));
    if (!path.empty()) r.Info("  執行檔: " + W(path));
    // ⚠ 服務在跑 ≠ 使用者看得到系統匣圖示。
    //
    // Windows 11 **預設把新出現的系統匣圖示收進溢位區**(工作列上那個 `^`)。
    // 也就是說「沒有系統匣圖示」這個回報,有一部分其實是「它在 ^ 裡面」。
    // 這一句放在這裡是因為:能看到這一行的人,他的服務是真的在跑 ——
    // 那麼圖示就一定被加過(它在 WM_CREATE 裡加,見 settings_window.cc)。
    r.Note("服務在跑就代表系統匣圖示已經加過了。Windows 11 預設把新的圖示");
    r.Note("收進工作列的溢位區(那個 `^`)—— 看不到的話先點開它看看,");
    r.Note("或到「設定 → 個人化 → 工作列 → 其他系統匣圖示」把它打開。");
    if (pids.size() > 1)
      r.Warn("同時有多支服務在跑 —— 它們會爭同一份使用者詞庫。請重新登入一次。");
  } else {
    r.Fail("rime_service.exe 沒有在跑",
           "這正是「沒有系統匣圖示、沒有設定視窗、打不出中文」的直接原因。\n"
           "         切到本輸入法時瘦 DLL 會自動把它啟動;沒有啟動的話,\n"
           "         多半是這個工作階段裡根本沒有載入到瘦 DLL(見第 6 節),\n"
           "         或宿主是提權的(提權的宿主刻意不啟動服務)。\n"
           "         可以手動啟動一次試試:\n"
           "         \"" + W(dir) + "\\rime_service.exe\"");
  }
  if (mutex_held && pids.empty())
    r.Warn("單一實例的鎖被人持有,但找不到 rime_service.exe —— "
           "有一支剛結束或屬於別的使用者。");
}

// ── 5. 具名管道 ───────────────────────────────────────────────────
void SectionPipe(Report& r) {
  r.Head("5. 具名管道(瘦 DLL 與服務之間那條線)");
  r.Info("管道名稱: " + W(RimePipeName()));

  // ⚠ 刻意**不**設服務路徑:這支診斷不可以順手把服務啟動起來。
  //   啟動了的話,「服務沒在跑」這個症狀會在被觀察的當下消失 ——
  //   而這正是我們要量的東西。
  IpcClient client;
  client.ResetLink();
  const bool ok = client.EnsureReady();
  const ReadyDiagnosis& d = client.diagnosis();
  if (ok) {
    r.Pass(Fmt("連得上、握手過了、session 建得起來(線路版本 %u)",
               static_cast<unsigned>(client.negotiated_proto())));
    return;
  }
  // 三步的修法完全不同,所以不可以併成一句「連不上」。
  switch (d.stage) {
    case ReadyStage::kPipe:
      if (d.os_error == ERROR_FILE_NOT_FOUND) {
        r.Fail("管道不存在(錯誤 2)",
               "服務沒有在監聽。若第 4 節說服務在跑,那它多半還在編譯詞庫\n"
               "         (首次安裝要一到數分鐘),過幾分鐘再跑一次這支診斷。");
      } else if (d.os_error == ERROR_ACCESS_DENIED) {
        r.Fail("管道在,但這個身分開不了(錯誤 5)",
               "多半是你用系統管理員身分跑這支診斷,而服務是你自己那一支。\n"
               "         請不要提權,直接跑一次。");
      } else {
        r.Fail(Fmt("開管道失敗(錯誤 %lu)", d.os_error),
               "服務沒有在監聽,或權限不對。");
      }
      break;
    case ReadyStage::kHandshake:
      r.Fail(Fmt("握手談不攏(我方 proto=%u abi=%u,對方%s proto=%u abi=%u)",
                 static_cast<unsigned>(d.tried_proto),
                 static_cast<unsigned>(d.my_shell_abi),
                 d.peer_replied ? "回了" : "沒回",
                 static_cast<unsigned>(d.peer.proto),
                 static_cast<unsigned>(d.peer.shell_abi)),
             "服務與 DLL 不是同一次建置的產物 —— 多半是升級到一半。\n"
             "         登出再登入一次(或重新開機),讓所有宿主放掉舊的 DLL。");
      break;
    case ReadyStage::kSession:
      r.Fail("連上也握手了,但建不出 session",
             "服務端太慢 —— 幾乎一定是第一次載入方案詞典。\n"
             "         等一兩分鐘再跑一次。一直如此的話,看第 7 節的引擎層。");
      break;
    case ReadyStage::kNone:
      r.Fail("連線沒有成功,但沒有記到失敗階段", "請把這份報告整份回報。");
      break;
  }
}

// ── 6. 誰載入了 rime_tsf.dll ──────────────────────────────────────
//
// ⚠ 這一格回答的是「系統到底有沒有把我們載入宿主進程裡」——
//   regsvr32 成功、登錄檔全綠,都**不代表**這件事會發生。
//   在這之前,要回答它只能請使用者裝 Process Explorer 再教他怎麼看。
void SectionLoadedIn(Report& r, const std::wstring& dll_name) {
  r.Head("6. 哪些程式載入了 rime_tsf.dll");

  HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    r.Warn("列舉不出進程清單,跳過這一格");
    return;
  }
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  int scanned = 0, opaque = 0, hits = 0;
  std::string names;
  if (::Process32FirstW(snap, &pe)) {
    do {
      ++scanned;
      // TH32CS_SNAPMODULE 對「開不起來的進程」(別的使用者、提權的、
      // 受保護的)會失敗。那不是錯誤,只是看不到 —— 數出來就好,
      // 不要假裝掃過了。
      HANDLE ms = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,
                                             pe.th32ProcessID);
      if (ms == INVALID_HANDLE_VALUE) {
        ++opaque;
        continue;
      }
      MODULEENTRY32W me{};
      me.dwSize = sizeof(me);
      bool found = false;
      if (::Module32FirstW(ms, &me)) {
        do {
          std::wstring m = me.szModule;
          for (wchar_t& c : m)
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
          if (m == dll_name) {
            found = true;
            break;
          }
        } while (::Module32NextW(ms, &me));
      }
      ::CloseHandle(ms);
      if (found) {
        ++hits;
        if (hits <= 20) names += W(pe.szExeFile) + " ";
      }
    } while (::Process32NextW(snap, &pe));
  }
  ::CloseHandle(snap);

  r.Info(Fmt("掃了 %d 個進程,其中 %d 個看不到模組清單(權限)", scanned, opaque));
  if (hits > 0) {
    r.Pass(Fmt("%d 個進程載入了 rime_tsf.dll: %s", hits, names.c_str()));
    r.Note("表示註冊是好的,而且系統真的把輸入法載進了宿主。");
  } else {
    r.Warn("沒有任何看得到的進程載入 rime_tsf.dll");
    r.Note("這**不一定**是壞事:只有「目前正接受文字輸入、而且選了本輸入法」");
    r.Note("的程式才會載入它。正確的量法是:先開記事本、按 Win+空白鍵切到");
    r.Note("本輸入法、在記事本裡按幾個字,**不要關掉記事本**,再跑一次這支診斷。");
    r.Note("那樣做之後這裡還是 0 的話,就是系統沒有載入這個輸入法 ——");
    r.Note("而那是第 2 節(註冊)的問題,不是輸入法邏輯的問題。");
  }
}

// ── 7. 引擎層 ─────────────────────────────────────────────────────
//
// 分層診斷的第一刀:rime_console.exe 完全不經過 TSF、不經過管道,
// 直接驅動 librime + 資料。它打得出「你好」就代表引擎、資料、方案都是好的,
// 問題必定在 TSF 或 IPC 那一側 —— 反過來也一樣。
void SectionEngine(Report& r, const std::wstring& dir) {
  r.Head("7. 引擎層(不經 TSF、不經管道,直接問 librime)");

  const std::wstring console = dir + L"\\rime_console.exe";
  const std::wstring shared = dir + L"\\data\\shared";
  // ⚠ **不要**在這裡拼 %APPDATA% + 資料夾名。唯一的決定處是
  //   winshared/winutil.cc 的 RimeUserDataDir() —— 這裡自己拼一份的話,
  //   產品改名時這一格會去讀一個不存在的目錄,然後 rime_console 會在
  //   一棵空的使用者目錄上重新部署一次(要好幾分鐘),而報告會說
  //   「引擎層逾時」。診斷工具給出錯的診斷,比沒有診斷更糟。
  const std::wstring user = RimeUserDataDir();

  if (!FileExists(console)) {
    r.Info("這個安裝裡沒有 rime_console.exe,跳過引擎層檢查");
    return;
  }
  if (user.empty()) {
    r.Fail("解析不出 %APPDATA%", "使用者資料目錄算不出來,輸入法無法運作。");
    return;
  }
  ::CreateDirectoryW(user.c_str(), nullptr);

  // ⚠ 使用者資料目錄裡有沒有非 ASCII 字元。
  //
  // 這一格 CI **永遠看不到** —— runner 的帳號叫 runneradmin。而中文使用者的
  // Windows 帳號名稱是中文是很常見的事,於是 %APPDATA% 會長成
  // C:\Users\王小明\AppData\Roaming\RimeQuad。
  //
  // 我們自己這一側是乾淨的(全程寬字元 + UTF-8),但路徑要交給 librime,
  // 而它在 Windows 上怎麼解讀那串位元組是它的事。萬一它當成系統 ANSI 代碼頁,
  // 結果會是「建不出目錄 / 部署失敗 / 一個候選都沒有」,而且沒有錯誤訊息。
  // 所以先把這個事實印出來 —— 它是 WARN 不是 FAIL:我們**還沒有證據**說它
  // 一定會壞,而把一件沒被證實的事印成 FAIL,會讓使用者去修一個不存在的問題。
  {
    bool non_ascii = false;
    for (wchar_t c : user)
      if (c > 0x7F) non_ascii = true;
    if (non_ascii) {
      r.Warn("使用者資料目錄裡有非 ASCII 字元: " + W(user));
      r.Note("多半是 Windows 帳號名稱是中文。這一格 CI 永遠測不到,");
      r.Note("而它是「部署失敗、一個候選都沒有」的已知嫌疑之一。");
      r.Note("下面的引擎層檢查若在這種路徑下失敗,請把這一段一起回報。");
    }
  }

  wchar_t tmp_dir[MAX_PATH] = {0};
  ::GetTempPathW(MAX_PATH, tmp_dir);
  const std::wstring out_path =
      std::wstring(tmp_dir) + L"rimequad-doctor-engine.txt";

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE out = ::CreateFileW(out_path.c_str(), GENERIC_WRITE | GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (out == INVALID_HANDLE_VALUE) {
    r.Warn("建不出暫存檔,跳過引擎層檢查");
    return;
  }

  std::wstring cmd = L"\"" + console + L"\" \"" + shared + L"\" \"" + user +
                     L"\" nihao 1 luna_pinyin_tw";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdOutput = out;
  si.hStdError = out;
  si.hStdInput = nullptr;  // rime_console 不讀 stdin
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');

  r.Info("執行: " + W(cmd));
  if (!::CreateProcessW(console.c_str(), buf.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    ::CloseHandle(out);
    r.Fail(Fmt("啟動 rime_console.exe 失敗(錯誤 %lu)",
               static_cast<unsigned long>(::GetLastError())),
           "執行檔在,但跑不起來。防毒軟體?");
    return;
  }
  // ⚠ 逾時不是 FAIL。首次安裝時 librime 要編譯詞庫,那是**好幾分鐘**的事,
  //   而在那之前引擎本來就給不出候選。把它報成 FAIL 會讓一台其實正常的
  //   機器看起來壞掉,而使用者接下來會做的事(重裝)只會讓計時器重來。
  const DWORD wait = ::WaitForSingleObject(pi.hProcess, 180000);
  DWORD rc = 1;
  if (wait == WAIT_TIMEOUT) {
    ::TerminateProcess(pi.hProcess, 1);
  } else {
    ::GetExitCodeProcess(pi.hProcess, &rc);
  }
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);

  // 讀回輸出。
  std::string text;
  {
    ::SetFilePointer(out, 0, nullptr, FILE_BEGIN);
    char chunk[8192];
    DWORD got = 0;
    while (::ReadFile(out, chunk, sizeof(chunk), &got, nullptr) && got > 0)
      text.append(chunk, got);
    ::CloseHandle(out);
  }
  ::DeleteFileW(out_path.c_str());

  const bool inited = text.find("rs_init OK") != std::string::npos;

  if (wait == WAIT_TIMEOUT) {
    // ⚠ 逾時要分成兩種說法,不可以併成一句「逾時」。
    //
    //   rs_init 都沒過去   → 引擎層是真的壞的(資料缺、路徑不對)
    //   rs_init 過了       → 引擎起得來,只是還在等部署回報。
    //                        rime_console 自己會等 600 秒,而 librime 在
    //                        **已經部署過**的目錄上不一定會再發一次通知 ——
    //                        那時它就是坐在那裡等,而引擎其實好得很。
    if (!inited) {
      r.Fail("引擎層連 rs_init 都沒有完成(等了 180 秒)",
             "librime 起不來。多半是 data\\shared 缺東西或路徑不對。\n"
             "         重新安裝一次;仍然如此的話請把這份報告整份回報。");
    } else {
      r.Warn("引擎層起得來,但這一次沒有打完(180 秒)");
      r.Note("rs_init 過了,所以 librime 與資料是好的 —— 它在等部署回報。");
      r.Note("首次安裝時編譯詞庫要一到數分鐘;過幾分鐘再跑一次。");
    }
    return;
  }

  // 原始碼是 UTF-8 而且編譯時帶 /utf-8(見 windows/CMakeLists.txt),
  // 所以這個字面值在二進位裡就是 UTF-8 的位元組 —— 與 rime_console 印出來的
  // 一模一樣。不要為此寫成 \xE4\xBD\xA0 那種手動編碼:抄錯了不會有人發現。
  const bool commit_ok =
      text.find(">>> COMMIT: \"你好\"") != std::string::npos;
  const bool deploy_ok = text.find("[deploy] SUCCESS") != std::string::npos;

  if (deploy_ok)
    r.Pass("詞庫部署成功");
  else
    r.Warn("沒有看到部署成功 —— 詞庫可能還在編譯,或資料有問題");

  if (rc == 0 && commit_ok) {
    r.Pass("引擎層打得出「你好」—— librime、資料、方案全部正常");
    r.Note("所以問題必定在 TSF(第 2/3/6 節)或管道(第 5 節)那一側。");
  } else if (inited) {
    // 起得來但沒打出字:仍然是壞的,但壞在別的地方(方案、詞庫、選字),
    // 而不是「librime 根本起不來」。這兩者要查的東西不同。
    r.Fail(Fmt("引擎層起得來,但打不出「你好」(結束碼 %lu)",
               static_cast<unsigned long>(rc)),
           "librime 起來了,所以問題在方案或詞庫那一層,不在路徑。\n"
           "         請連同下面這幾行一起回報。");
  } else {
    r.Fail(Fmt("引擎層連 rs_init 都沒有完成(結束碼 %lu)",
               static_cast<unsigned long>(rc)),
           "librime 起不來。多半是 data\\shared 缺東西或路徑不對。\n"
           "         重新安裝一次;仍然如此的話請把這份報告整份回報。");
    // 只挑我們自己的輸出。glog 的部署警告有好幾百行,貼過來只會把
    // 真正的訊息淹掉。
    size_t pos = 0;
    int printed = 0;
    while (pos < text.size() && printed < 25) {
      size_t nl = text.find('\n', pos);
      if (nl == std::string::npos) nl = text.size();
      std::string line = text.substr(pos, nl - pos);
      pos = nl + 1;
      while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        line.pop_back();
      if (line.empty()) continue;
      // glog 的行長這樣:W0809 01:24:33.123456 ...
      if (line.size() > 6 && (line[0] == 'W' || line[0] == 'I' ||
                              line[0] == 'E' || line[0] == 'F') &&
          line[1] >= '0' && line[1] <= '9')
        continue;
      r.Note(line);
      ++printed;
    }
  }
}

// ── 8. 除錯記錄 ───────────────────────────────────────────────────
void SectionTrace(Report& r) {
  r.Head("8. 瘦 DLL 的除錯記錄(它在宿主進程裡發生的事)");

  wchar_t path[MAX_PATH] = {0};
  if (!TraceFilePath(path, MAX_PATH)) {
    r.Info("除錯記錄是關掉的(環境變數 RIME_TSF_TRACE)");
    return;
  }
  r.Info("記錄檔: " + W(path));
  HANDLE h = ::CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    r.Warn("記錄檔不存在");
    r.Note("代表**這台機器上從來沒有任何進程載入過 rime_tsf.dll** ——");
    r.Note("第一行是在 DllMain 裡寫的,只要系統載入過一次就會有。");
    r.Note("先切到本輸入法、在記事本裡按幾個鍵,再跑一次這支診斷。");
    r.Note("那樣做之後檔案還是不存在的話,就是註冊那一段的問題(第 2 節)。");
    return;
  }
  LARGE_INTEGER size{};
  ::GetFileSizeEx(h, &size);
  // 只取尾巴。整個檔案可能到 1 MiB,而診斷要看的是最近這一次。
  const LONGLONG kTail = 16384;
  if (size.QuadPart > kTail) {
    LARGE_INTEGER off;
    off.QuadPart = size.QuadPart - kTail;
    ::SetFilePointerEx(h, off, nullptr, FILE_BEGIN);
  }
  std::string text;
  char chunk[8192];
  DWORD got = 0;
  while (::ReadFile(h, chunk, sizeof(chunk), &got, nullptr) && got > 0)
    text.append(chunk, got);
  ::CloseHandle(h);

  // 只印最後 40 行。
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    std::string line = text.substr(pos, nl - pos);
    pos = nl + 1;
    while (!line.empty() && (line.back() == '\r')) line.pop_back();
    if (!line.empty()) lines.push_back(line);
  }
  const size_t start = lines.size() > 40 ? lines.size() - 40 : 0;
  if (lines.empty()) {
    r.Warn("記錄檔是空的");
  } else {
    r.Info(Fmt("最後 %d 行(共 %d 行在尾段緩衝裡):",
               static_cast<int>(lines.size() - start),
               static_cast<int>(lines.size())));
    for (size_t i = start; i < lines.size(); ++i) r.Note(lines[i]);

    // 把最重要的兩個結論直接講出來,不要求讀的人自己看懂上面那些行。
    const bool activated =
        text.find("ActivateEx 被呼叫") != std::string::npos;
    if (activated)
      r.Pass("記錄裡有 ActivateEx —— 系統確實把這個輸入法叫起來過");
    else
      r.Warn("記錄裡沒有 ActivateEx —— DLL 載入過,但系統沒有啟用這個文字服務");
    if (text.find("keysym=0x0 ") != std::string::npos)
      r.Fail("記錄裡有 keysym=0x0 的按鍵",
             "那顆鍵在目前的鍵盤佈局上問不出字,所以被原樣放行、沒進引擎。\n"
             "         見第 3 節的鍵盤佈局那一格。");
  }
}

void WriteAndOpenReport(const std::string& text) {
  wchar_t tmp_dir[MAX_PATH] = {0};
  ::GetTempPathW(MAX_PATH, tmp_dir);
  const std::wstring path = std::wstring(tmp_dir) + L"rimequad-doctor.txt";
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  // UTF-8 BOM:沒有它的話,記事本在某些 Windows 版本上會用系統 ANSI 代碼頁
  // 讀這個檔,而整份報告裡的中文會變成亂碼 —— 使用者看到亂碼只會以為
  // 「連診斷工具都壞了」。
  ::WriteFile(h, "\xEF\xBB\xBF", 3, &written, nullptr);
  ::WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
  ::CloseHandle(h);

  std::wstring cmd = L"notepad.exe \"" + path + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');
  // 用 CreateProcess 而不是 ShellExecute:後者在 shell32 裡,而多背一個 DLL
  // 就多一種「在某台機器上載入失敗」的可能 —— 對一支**專門用來診斷
  // 「為什麼載入失敗」**的工具來說格外諷刺。
  if (::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                       nullptr, &si, &pi)) {
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
  }
}

}  // namespace

int RunDoctor(const DoctorOptions& opt) {
  // STA:TSF 的物件是 Apartment 模型的。
  const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  const std::wstring dir = ModuleDirectory(nullptr);
  const std::wstring dll = dir + L"\\rime_tsf.dll";

  Report r;
  r.Line("════════════════════════════════════════════════════════════");
  r.Line("  RIME 輸入法 自我診斷");
  r.Line("════════════════════════════════════════════════════════════");
  {
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    r.Line(Fmt("  時間        : %04u-%02u-%02u %02u:%02u:%02u", st.wYear,
               st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond));
  }
  r.Line("  使用者 SID  : " + W(CurrentUserSidString()));
  r.Line(std::string("  這支診斷是提權的嗎: ") +
         (IsProcessElevated() ? "是 ⚠(提權時看到的 HKCU 與管道都是**另一個"
                                "帳號的**,請改用一般身分再跑一次)"
                              : "否(正確)"));

  SectionFiles(r, dir);
  SectionRegistration(r, dll);
  SectionProfileAndLayout(r);
  SectionService(r, dir);
  SectionPipe(r);
  if (opt.scan_processes) SectionLoadedIn(r, L"rime_tsf.dll");
  if (opt.check_engine) SectionEngine(r, dir);
  SectionTrace(r);

  r.Head("結論");
  if (r.fails == 0 && r.warns == 0) {
    r.Line("  全部通過。");
    r.Line("  若仍然打不出中文,請照第 6 節的說明:先開記事本、切到本輸入法、");
    r.Line("  按幾個鍵、**不要關掉記事本**,再跑一次這支診斷 —— 那樣才量得到");
    r.Line("  真正在使用時的狀態。");
  } else {
    r.Line(Fmt("  %d 項失敗、%d 項需要注意。上面每一個 [FAIL] 後面的 →",
               r.fails, r.warns));
    r.Line("  就是接下來要做的事。");
  }
  r.Line("");
  r.Line("把這整份貼給我們就夠了,不需要再問任何問題。");
  r.Line("(這份報告裡沒有你打過的字、沒有候選字、沒有視窗標題 ——");
  r.Line(" 只有程式檔名與註冊狀態。)");

  std::fwrite(r.text().data(), 1, r.text().size(), stdout);
  std::fflush(stdout);
  if (opt.open_report) WriteAndOpenReport(r.text());

  if (SUCCEEDED(com)) ::CoUninitialize();
  return r.fails;
}

}  // namespace rimewin
