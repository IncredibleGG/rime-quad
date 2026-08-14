// windows/setup/setup_main.cc — rime_ime_setup.exe
//
// 安裝程式的手腳。一支很小的執行檔,不連 librime,只做四件事:
//
//   register / unregister    全機註冊(HKLM),需要系統管理員權限
//   enable-user / disable-user  目前使用者的啟用(HKCU)。
//                            ⚠ enable-user 只會留下**一份** ——
//                            清單上出現幾格由這裡決定,不是由註冊決定。
//   user-profiles            印出「這個使用者現在啟用了哪幾份」與判斷依據
//   check                    「真的註冊好了嗎」,CI 靠它斷言
//   stop-service             停掉 rime_service.exe(升級與解除安裝前)
//   doctor                   一頁式自我診斷(給使用者用的,見 setup/doctor.h)
//   user-data-path           印出使用者資料目錄(安裝程式問它,不自己拼)
//   purge-user-data          刪掉使用者的詞典與設定。**不可回復**,
//                            必須帶 --yes-delete-my-dictionary 才會真的動手
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
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../common/profile_choice.h"
#include "../tsf/registration.h"
#include "../tsf/registration_check.h"
#include "../tsf/user_langs.h"
#include "../winshared/winutil.h"
#include "doctor.h"

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
      "  enable-user [--lang <十六進位 langid>]\n"
      "                             把輸入法加進**目前使用者**的清單。\n"
      "                             只會留下一份(其餘一律停用)——\n"
      "                             語言列/Win+空白鍵的清單上只該有一格。\n"
      "                             不給 --lang 就依使用者既有的語言清單自動選。\n"
      "  disable-user               全部停用\n"
      "  user-profiles              印出目前啟用了哪幾份、以及自動判斷會選哪一份\n"
      "  find-window --class <類別名> [--visible]\n"
      "                             那個視窗類別現在存不存在。找到 = 0,沒找到 = 1\n"
      "                             --visible:還要求它是**顯示中**的\n"
      "                             (CI 用:斷言「設定視窗真的開出來了」)\n"
      "  capture-window --class <類別名> --out <bmp 路徑> [--page <0-4>]\n"
      "                             把那個視窗畫一張 24-bit BMP 存下來。\n"
      "                             --page 先把它切到第幾頁(0 起算)。\n"
      "                             它不提權、不改任何狀態、只寫你指名的那個檔;\n"
      "                             支援時請使用者附上這張圖比描述畫面可靠得多。\n"
      "  check [--dll <路徑>] [--user] [--no-enum]\n"
      "                             斷言註冊狀態;不通過就以非零結束\n"
      "                             --no-enum:只驗登錄檔,不問 TSF 的列舉 API\n"
      "                             (剛註冊完的當下 CTF 還看不到,見標頭說明)\n"
      "  paths                      印出所有會被寫到的登錄檔路徑與 GUID\n"
      "  stop-service [--dir <目錄>]  停掉 rime_service.exe\n"
      "  sweep-stale-dlls [--dir <目錄>] [--schedule-if-locked]\n"
      "                             掃掉升級時「改名挪開」留下的 rime_tsf.dll.old-*。\n"
      "                             刪不掉不是錯誤(還有程式握著)——一律以 0 結束。\n"
      "                             --schedule-if-locked:刪不掉的排進開機清除\n"
      "                             (要寫 HKLM,需要系統管理員權限)\n"
      "  dump                       印出登錄檔實況(診斷用)\n"
      "  user-data-path             印出使用者資料目錄(一行,不含其他東西)\n"
      "  purge-user-data --yes-delete-my-dictionary\n"
      "                             刪掉使用者的詞典與設定。**無法復原**。\n"
      "                             沒有帶那個參數就什麼都不做。\n"
      "  doctor [--report] [--no-engine] [--no-scan]\n"
      "                             一頁式自我診斷:檔案、註冊、目前的語言設定檔、\n"
      "                             鍵盤佈局、服務進程、管道、誰載入了 DLL、\n"
      "                             引擎層、以及瘦 DLL 的除錯記錄。\n"
      "                             有任何一格 FAIL 就以非零結束。\n"
      "                             --report:另存一份並用記事本打開\n");
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

// ══════════════════════════════════════════════════════════════════
//  「連我的資料一起刪」
// ══════════════════════════════════════════════════════════════════
//
// 使用者的原話:「增加一個刪除乾淨,就是用戶確實不需要了,我們就直接刪除」。
//
// ⚠ 這是整個產品裡**唯一一個不可回復**的動作。使用者的詞典是他用了幾個月
//   累積出來的東西,刪錯了沒有任何辦法補救 —— 重裝補不回來、CI 補不回來。
//   所以這一段的每一個決定都往「寧可少刪」那一邊倒:
//
//   1. **只有明確要求才刪。** 這支程式的預設行為是什麼都不做:
//      沒有帶那個確認參數就直接以 2 結束並印用法。
//      安裝程式那一側同樣是明確勾選 / 明確回答「是」才會叫到這裡,
//      而且靜默解除安裝**永遠不會**走到這裡(除非另外傳 /PURGEUSERDATA)。
//   2. **路徑不自己拼。** 走 winshared 的 RimeUserDataDir() ——
//      唯一的決定處。自己拼一份的話,資料夾名一改(例如產品改名)
//      這裡就會去刪一個不存在的目錄然後回報成功。
//   3. **刪之前逐條檢查**(見 SafeToDelete)。任何一條不過就拒絕,
//      而且明著說是哪一條 —— 「拒絕刪除」永遠比「刪錯」好。
//   4. **不跟著符號連結走。** 目錄若是 reparse point(junction / symlink),
//      只刪那個連結本身,不遞迴進去 —— 不然一個指向 C:\ 的 junction
//      會把整台機器帶走。

// 這個路徑可不可以刪。回傳 nullptr = 可以;否則回傳拒絕的理由。
const char* SafeToDelete(const std::wstring& dir) {
  if (dir.empty()) return "算不出使用者資料目錄(%APPDATA% 是空的?)";
  // 「C:\x」這種長度的東西一定不是我們的資料目錄。
  if (dir.size() < 12) return "路徑短得不合理";
  if (dir.find(L"..") != std::wstring::npos) return "路徑裡有 ..";

  wchar_t appdata[32768];
  const DWORD n = ::GetEnvironmentVariableW(L"APPDATA", appdata, 32768);
  if (n == 0 || n >= 32768) return "讀不到 %APPDATA%";
  std::wstring base(appdata, n);
  while (!base.empty() && (base.back() == L'\\' || base.back() == L'/'))
    base.pop_back();
  if (base.empty()) return "%APPDATA% 是空的";
  // 必須**真的**在 %APPDATA% 底下,而且不是 %APPDATA% 自己。
  if (dir.size() <= base.size() + 1) return "路徑不在 %APPDATA% 底下";
  if (LowerW(dir).compare(0, base.size(), LowerW(base)) != 0)
    return "路徑不在 %APPDATA% 底下";

  // 最後一段必須正好是我們的資料夾名。這一條擋的是「路徑算錯成
  // %APPDATA% 底下的別的東西」——例如少接了一段,結果指到 Microsoft\。
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return "路徑裡沒有反斜線";
  if (LowerW(dir.substr(slash + 1)) != LowerW(RimeUserDataFolderName()))
    return "最後一段不是我們的資料夾名";
  return nullptr;
}

// 遞迴刪除。回傳刪掉的檔案數;失敗的計進 *failed。
int DeleteTree(const std::wstring& dir, int* failed) {
  int removed = 0;
  WIN32_FIND_DATAW fd{};
  HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      const std::wstring name = fd.cFileName;
      if (name == L"." || name == L"..") continue;
      const std::wstring full = dir + L"\\" + name;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        // ⚠ reparse point(junction / symlink)**不可以遞迴進去**。
        //   一個指向 C:\ 的 junction 會讓這個函式把整台機器刪掉。
        //   只刪那個連結本身。
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
          if (::RemoveDirectoryW(full.c_str())) ++removed;
          else if (failed) ++*failed;
          continue;
        }
        removed += DeleteTree(full, failed);
      } else {
        // 唯讀屬性會讓 DeleteFileW 失敗。先拿掉 —— 使用者自己設成唯讀的
        // 檔案,在「我要刪乾淨」這個明確要求底下也該被刪掉。
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
          ::SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (::DeleteFileW(full.c_str())) ++removed;
        else if (failed) ++*failed;
      }
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
  }
  if (!::RemoveDirectoryW(dir.c_str()) && failed) ++*failed;
  return removed;
}

int PurgeUserData(bool confirmed) {
  const std::wstring dir = RimeUserDataDir();
  Say("使用者資料目錄: %s\n", WideToUtf8(dir).c_str());
  if (!confirmed) {
    Say("!! 沒有帶 --yes-delete-my-dictionary,什麼都不做。\n"
        "   這個動作會刪掉使用者的詞典與設定,而且**無法復原** ——\n"
        "   所以它必須被明確要求,不會因為打錯一個字就發生。\n");
    return 2;
  }
  const char* why = SafeToDelete(dir);
  if (why) {
    Say("!! 拒絕刪除:%s\n"
        "   (寧可什麼都不刪,也不要刪錯一個目錄。)\n", why);
    return 1;
  }
  if (::GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
    // 不是失敗:使用者可能已經自己刪過了,或從來沒有真的用過。
    Say("目錄不存在,沒有東西要刪。\n");
    return 0;
  }
  int failed = 0;
  const int removed = DeleteTree(dir, &failed);
  Say("已刪除 %d 個檔案,%d 個失敗\n", removed, failed);
  if (::GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) {
    // 目錄還在 = 有東西被佔用著(服務還沒完全結束、或使用者開著檔案)。
    // 這要明著說:安裝程式那一側會據此告訴使用者「有些東西沒刪掉」,
    // 而不是宣布刪乾淨了然後留下半棵樹。
    Say("!! 目錄仍然存在 —— 有檔案被佔用著(服務還在跑?)\n");
    return 1;
  }
  Say("使用者資料已完全刪除。\n");
  return 0;
}

// ══════════════════════════════════════════════════════════════════
//  語言設定檔:清單上只留一格
// ══════════════════════════════════════════════════════════════════
//
// 為什麼會有這一段,見 common/profile_choice.h 的檔頭。一句話:
// **註冊(HKLM)不會讓清單上多一格,啟用(HKCU)才會**,而舊版對三份
// profile 都做了啟用,於是使用者的 Win + 空白鍵清單上我們佔三格,
// 而微软拼音、小狼毫各佔一格。

// "0x0804" / "0804" / "2052" 都收。認不得回 0(= 沒有指定)。
uint32_t ParseLangId(const std::wstring& text) {
  if (text.empty()) return 0;
  int base = 10;
  size_t i = 0;
  if (text.size() > 2 && text[0] == L'0' && (text[1] == L'x' || text[1] == L'X')) {
    base = 16;
    i = 2;
  } else {
    // 沒有 0x 前綴但含有 a-f → 當十六進位。langid 實務上一律寫十六進位,
    // 而 "0804" 當十進位解會變成 804,那不是任何一個語言 —— 靜靜地選錯
    // 比明著失敗糟得多。所以:四位數字一律當十六進位。
    bool hexish = false;
    for (wchar_t c : text)
      if ((c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F')) hexish = true;
    if (hexish || text.size() == 4) base = 16;
  }
  uint32_t v = 0;
  for (; i < text.size(); ++i) {
    const wchar_t c = text[i];
    int d;
    if (c >= L'0' && c <= L'9') d = c - L'0';
    else if (c >= L'a' && c <= L'f') d = c - L'a' + 10;
    else if (c >= L'A' && c <= L'F') d = c - L'A' + 10;
    else return 0;
    if (d >= base) return 0;
    v = v * static_cast<uint32_t>(base) + static_cast<uint32_t>(d);
  }
  return v;
}

// kRimeProfiles 的 langid,給純邏輯層當「我們實際註冊了哪幾個」。
// ⚠ 不要在 profile_choice.cc 裡另寫一份常數清單:那一份與這裡漂移的話,
//   會選到一個沒有註冊的 langid,而症狀是「裝完了,清單上什麼都沒有」。
std::vector<uint16_t> RegisteredLangIds() {
  std::vector<uint16_t> v;
  for (int i = 0; i < ProfileCount(); ++i)
    v.push_back(static_cast<uint16_t>(ProfileLangId(i)));
  return v;
}

int IndexOfLangId(uint16_t langid) {
  for (int i = 0; i < ProfileCount(); ++i)
    if (static_cast<uint16_t>(ProfileLangId(i)) == langid) return i;
  return -1;
}

// 蒐集事實 → 交給純邏輯層 → 回傳 kRimeProfiles 的索引(-1 = 算不出來)。
// 過程一律印出來:**選了哪一個**與**為什麼**是兩個問題,只印前者的話,
// 選錯時要再等一輪 CI 才知道是哪一層做的決定。
int DecideProfileIndex(uint32_t explicit_langid, bool verbose) {
  const std::vector<std::wstring> tags = CurrentUserLanguageTags();
  std::vector<std::string> tags_utf8;
  for (const std::wstring& t : tags) tags_utf8.push_back(WideToUtf8(t));
  const std::vector<uint32_t> installed = InstalledInputLangIds();
  const uint32_t ui = ::GetUserDefaultUILanguage();

  if (verbose) {
    Say("  使用者的語言清單(依序):");
    if (tags_utf8.empty()) Say("(讀不到)");
    for (const std::string& t : tags_utf8) Say(" %s", t.c_str());
    Say("\n  已安裝的輸入語言:");
    if (installed.empty()) Say("(讀不到)");
    for (uint32_t l : installed) Say(" 0x%04X", static_cast<unsigned>(l));
    Say("\n  系統顯示語言:0x%04X\n", static_cast<unsigned>(ui));
  }

  const rimewin::ProfileChoice c = rimewin::ChooseUserProfileLangId(
      tags_utf8, installed, ui, explicit_langid, RegisteredLangIds());
  if (c.langid == 0) {
    Say("!! 算不出要啟用哪一份(%s)\n", c.reason);
    return -1;
  }
  const int idx = IndexOfLangId(c.langid);
  if (verbose)
    Say("  → 選 0x%04X(%s)\n", static_cast<unsigned>(c.langid), c.reason);
  return idx;
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

// ── capture-window:把一個視窗畫成 BMP ────────────────────────────
//
// ⚠ **這條線最大的問題是沒有人看過畫面。** windows/service/settings_window.cc
//   這一路的改動全部在 Linux 上建置與驗證,而「好不好看」在那個條件下
//   是驗不了的 —— 每一次都只能靠讀碼說服自己。
//
//   .github/workflows/windows.yml 有三個 windows-latest 的 job,而
//   verify_installer.sh §12b 已經在 runner 上把設定視窗**開出來並斷言
//   它是顯示中的**。所以截圖不是新能力,是把已經在那裡的一步接出一張圖。
//
// ── 為什麼是 PrintWindow 而不是螢幕擷取 ──────────────────────────
//
//   PW_RENDERFULLCONTENT 直接叫視窗把自己畫進一張記憶體 DC:
//     · 不需要顯示裝置(runner 有沒有真的螢幕不影響)
//     · 不需要把視窗弄到前景(跨進程搶前景在 Windows 上會被拒絕)
//     · 被別的視窗蓋住也拍得到
//   ⚠ 已知風險:BS_OWNERDRAW 的兩顆危險鍵走不走得到 WM_PRINTCLIENT,
//     **我沒有證據**。所以失敗或整張全黑時退回螢幕擷取,而且
//     **在日誌裡印出用了哪一條** —— 「抓到黑畫面」與「視窗沒開出來」
//     在 artifact 上長得一模一樣,不印的話沒有人分得出來。
//
// ⚠ **這個動詞不寫進產品的主進程。** rime_service.exe 是握著使用者詞庫的
//   那一支,不該為了 CI 長出一個測試旗標。PrintWindow 對別的進程的 HWND
//   一樣有效,所以 rime_service.exe 一行都不用改。
//
// ⚠ 它對使用者無害:不提權、不改任何狀態、只寫一個他指名的檔案。
//   rime_ime_setup.exe 本來就是使用者機器上的診斷工具(「開始」功能表裡
//   那一項「診斷:輸入法為什麼不能用」),多一個「把你的設定視窗截一張圖」
//   對支援是正資產。
namespace {

// 24-bit BMP。⚠ 不用任何編碼器 —— BITMAPFILEHEADER + BITMAPINFOHEADER
//   直接落地,不引入新相依(§12.1:每一個匯入都要解釋得清楚)。
bool WriteBmp24(const std::wstring& path, HBITMAP bmp, HDC dc, int w, int h) {
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;  // 負 = top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 24;
  bi.bmiHeader.biCompression = BI_RGB;
  const int stride = ((w * 3) + 3) & ~3;
  std::vector<BYTE> pixels(static_cast<size_t>(stride) * h, 0);
  if (!::GetDIBits(dc, bmp, 0, h, pixels.data(), &bi, DIB_RGB_COLORS))
    return false;

  BITMAPFILEHEADER fh{};
  fh.bfType = 0x4D42;  // 'BM'
  fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
  fh.bfSize = fh.bfOffBits + static_cast<DWORD>(pixels.size());
  BITMAPINFOHEADER ih = bi.bmiHeader;
  ih.biHeight = h;  // 檔案裡寫正的,像素改成 bottom-up
  ih.biSizeImage = static_cast<DWORD>(pixels.size());

  HANDLE f = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD wrote = 0;
  bool ok = ::WriteFile(f, &fh, sizeof(fh), &wrote, nullptr) != 0 &&
            ::WriteFile(f, &ih, sizeof(ih), &wrote, nullptr) != 0;
  // bottom-up:一列一列倒著寫。
  for (int y = h - 1; ok && y >= 0; --y)
    ok = ::WriteFile(f, pixels.data() + static_cast<size_t>(y) * stride,
                     static_cast<DWORD>(stride), &wrote, nullptr) != 0;
  ::CloseHandle(f);
  return ok;
}

// 整張圖是不是同一個顏色(全黑/全白)。
// ⚠ 這一格就是「抓到什麼都沒有」與「抓到畫面」的分界,而它在 artifact 上
//   看起來一樣 —— 所以要在**這裡**分辨,不是等人打開圖檔。
bool LooksBlank(HBITMAP bmp, HDC dc, int w, int h) {
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 24;
  bi.bmiHeader.biCompression = BI_RGB;
  const int stride = ((w * 3) + 3) & ~3;
  std::vector<BYTE> px(static_cast<size_t>(stride) * h, 0);
  if (!::GetDIBits(dc, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS)) return true;
  const BYTE r0 = px[0], g0 = px[1], b0 = px[2];
  for (int y = 0; y < h; ++y) {
    const BYTE* row = px.data() + static_cast<size_t>(y) * stride;
    for (int x = 0; x < w; ++x) {
      if (row[x * 3] != b0 || row[x * 3 + 1] != g0 || row[x * 3 + 2] != r0)
        return false;
    }
  }
  return true;
}

}  // namespace

int CaptureWindow(const std::wstring& window_class, const std::wstring& out,
                  int page) {
  HWND h = ::FindWindowW(window_class.c_str(), nullptr);
  if (!h) {
    Say("!! capture-window:找不到類別「%s」的視窗\n",
        WideToUtf8(window_class).c_str());
    return 1;
  }
  if (!::IsWindowVisible(h)) {
    // ⚠ 不是「拍不到」,是**拍到的東西不代表使用者看到的**。設定視窗在
    //   服務啟動時就被建好但不顯示,所以隱藏的那一份可能還沒擺過版面。
    Say("!! capture-window:視窗存在但不是顯示中的 —— 不拍\n");
    return 1;
  }
  if (page >= 0) {
    // WM_RIME_OPEN(WM_APP + 1),wParam = page + 1。
    // ⚠ 這個編碼與 service/settings_window.cc 的 OpenAt() 是同一份約定。
    //   跨進程送訊息,所以 rime_service.exe **一行都不用改**。
    ::PostMessageW(h, WM_APP + 1, static_cast<WPARAM>(page + 1), 0);
    // 換頁會重排版面與重畫,給它一點時間 —— 沒有可以等的事件。
    ::Sleep(1200);
  }

  RECT wr{};
  ::GetWindowRect(h, &wr);
  const int w = wr.right - wr.left;
  const int hgt = wr.bottom - wr.top;
  if (w <= 0 || hgt <= 0) {
    Say("!! capture-window:視窗矩形是空的\n");
    return 1;
  }

  HDC screen = ::GetDC(nullptr);
  HDC mem = ::CreateCompatibleDC(screen);
  HBITMAP bmp = ::CreateCompatibleBitmap(screen, w, hgt);
  HGDIOBJ old = ::SelectObject(mem, bmp);

  const char* how = "PrintWindow";
  // 2 = PW_RENDERFULLCONTENT(mingw 的 winuser.h 不一定有這個巨集)。
  BOOL ok = ::PrintWindow(h, mem, 2);
  if (!ok || LooksBlank(bmp, mem, w, hgt)) {
    // 退回螢幕擷取。⚠ 這一條需要真的有顯示裝置,而 runner 有沒有
    //   **我沒有證據** —— 所以它是退路,不是主路。
    how = "BitBlt(screen)";
    ::BitBlt(mem, 0, 0, w, hgt, screen, wr.left, wr.top, SRCCOPY);
  }
  const bool blank = LooksBlank(bmp, mem, w, hgt);
  const bool wrote = WriteBmp24(out, bmp, mem, w, hgt);

  ::SelectObject(mem, old);
  ::DeleteObject(bmp);
  ::DeleteDC(mem);
  ::ReleaseDC(nullptr, screen);

  // ⚠ **一定要印出用了哪一條、以及是不是一片空白。** 沒有這兩行的話,
  //   「PrintWindow 對自繪按鈕沒作用」與「視窗真的長這樣」在 artifact 上
  //   長得一模一樣。
  Say("capture-window:%dx%d 用 %s%s → %s(%s)\n", w, hgt, how,
      blank ? "(⚠ 整張同一個顏色)" : "", WideToUtf8(out).c_str(),
      wrote ? "寫檔成功" : "寫檔失敗");
  if (!wrote) return 1;
  // ⚠ 一片空白**不當成失敗**:它仍然是證據(而且是最需要被看到的那一種)。
  //   把它變成紅的會讓人去關掉這一步,而不是去看那張圖。
  return 0;
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
  bool want_enum = true;
  bool purge_confirmed = false;
  bool schedule_if_locked = false;
  uint32_t explicit_langid = 0;
  std::wstring window_class;
  bool require_visible = false;
  std::wstring capture_out;
  int capture_page = -1;
  DoctorOptions doctor;

  for (int i = 2; i < argc; ++i) {
    const std::wstring a = argv[i];
    if (a == L"--class" && i + 1 < argc) window_class = argv[++i];
    else if (a == L"--visible") require_visible = true;
    else if (a == L"--out" && i + 1 < argc) capture_out = argv[++i];
    else if (a == L"--page" && i + 1 < argc)
      capture_page = ::_wtoi(argv[++i]);
    else if (a == L"--lang" && i + 1 < argc) explicit_langid = ParseLangId(argv[++i]);
    else if (a == L"--dll" && i + 1 < argc) dll = argv[++i];
    else if (a == L"--dir" && i + 1 < argc) dir = argv[++i];
    else if (a == L"--user") want_user = true;
    else if (a == L"--no-enum") want_enum = false;
    else if (a == L"--report") doctor.open_report = true;
    else if (a == L"--no-engine") doctor.check_engine = false;
    else if (a == L"--no-scan") doctor.scan_processes = false;
    else if (a == L"--schedule-if-locked") schedule_if_locked = true;
    // 參數名字刻意又長又白話。`--force` / `-y` 那種東西會被人習慣性地帶上,
    // 而這是唯一一個帶錯就救不回來的動作。
    else if (a == L"--yes-delete-my-dictionary") purge_confirmed = true;
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

  if (verb == L"enable-user" || verb == L"disable-user" ||
      verb == L"user-profiles" || verb == L"enable-user-legacy-all") {
    // 提權時的 HKCU 是**提權那個帳號的**。安裝程式必須以 ExecAsOriginalUser
    // 呼叫這一支,否則「用系統管理員帳號提權裝完,登入的那個人清單裡什麼都沒有」。
    if (IsProcessElevated() && verb != L"user-profiles")
      Say("  ⚠ 這支目前是提權的 —— 寫進去的會是提權帳號的 HKCU,不是登入者的。\n");
    Say("  SID = %s\n", WideToUtf8(CurrentUserSidString()).c_str());

    // ── 目前實況 ────────────────────────────────────────────────
    //
    // ⚠ 答案向 TSF 要(IsEnabledLanguageProfile),不是數 HKCU 的子鍵:
    //   停用之後那個鍵可能還在,數子鍵的話「停用了」與「還開著」
    //   長得一模一樣 —— 而這一輪要斷言的正好是「只剩一份」。
    {
      const std::vector<int> on = EnabledProfileIndexesForCurrentUser();
      Say("  目前已啟用 %d 份:", static_cast<int>(on.size()));
      for (int i : on) Say(" 0x%04X", static_cast<unsigned>(ProfileLangId(i)));
      Say("\n");
    }

    if (verb == L"user-profiles") {
      // 機器可讀的一段,給 windows/verify_installer.sh 斷言用。
      // 格式與 `paths` 一致:一行一筆 KEY=值。
      const std::vector<std::wstring> tags = CurrentUserLanguageTags();
      Say("USER_LANGS=");
      for (size_t k = 0; k < tags.size(); ++k)
        Say("%s%s", k ? "," : "", WideToUtf8(tags[k]).c_str());
      Say("\n");
      const std::vector<int> on = EnabledProfileIndexesForCurrentUser();
      Say("ENABLED_COUNT=%d\n", static_cast<int>(on.size()));
      for (int i : on)
        Say("ENABLED=0x%04X=%s\n", static_cast<unsigned>(ProfileLangId(i)),
            WideToUtf8(ProfileGuidString(i)).c_str());
      const int want = DecideProfileIndex(explicit_langid, false);
      if (want >= 0)
        Say("WOULD_CHOOSE=0x%04X\n",
            static_cast<unsigned>(ProfileLangId(want)));
      else
        Say("WOULD_CHOOSE=0x0000\n");
      return 0;
    }

    if (verb == L"enable-user-legacy-all") {
      // ⚠ **這個動詞只有 CI 在用,而且它刻意重現一個已經修掉的 bug。**
      //
      //   windows/verify_installer.sh §4c 要驗的是「升級時會把舊版多開的
      //   語言設定檔收回去」。要驗那件事,得先有一台**處於舊狀態**的機器 ——
      //   而製造那個狀態最誠實的方法,是跑一次舊版真的跑過的那段程式碼,
      //   不是去猜舊版在登錄檔裡留下什麼然後手寫進去。
      //
      //   直接連跑兩次 enable-user 驗不到這件事:第二次面對的已經是
      //   「只有一份」的狀態,清理那條路從來不會被走到 ——
      //   而一個前提不成立的斷言必定通過。這個專案抓過那種測試。
      //
      //   它不會出現在 Usage 裡,安裝程式也不會呼叫它。
      int okc = 0;
      for (int i = 0; i < ProfileCount(); ++i) {
        const HRESULT hr = SetProfileEnabledForCurrentUser(i, true);
        Say("  0x%04X -> %s (hr=0x%08lX)\n",
            static_cast<unsigned>(ProfileLangId(i)),
            SUCCEEDED(hr) ? "OK" : "失敗", static_cast<unsigned long>(hr));
        if (SUCCEEDED(hr)) ++okc;
      }
      Say("(測試用)重現舊版:%d / %d 份被啟用\n", okc, ProfileCount());
      return okc == ProfileCount() ? 0 : 1;
    }

    if (verb == L"disable-user") {
      // 全部停用。解除安裝走這條 —— 那時我們要從清單上完全消失。
      int okc = 0;
      for (int i = 0; i < ProfileCount(); ++i) {
        const HRESULT hr = SetProfileEnabledForCurrentUser(i, false);
        Say("  0x%04X %s -> %s (hr=0x%08lX)\n",
            static_cast<unsigned>(ProfileLangId(i)),
            WideToUtf8(ProfileGuidString(i)).c_str(),
            SUCCEEDED(hr) ? "OK" : "失敗", static_cast<unsigned long>(hr));
        if (SUCCEEDED(hr)) ++okc;
      }
      Say("停用:%d / %d 份成功\n", okc, ProfileCount());
      return okc == 0 ? 1 : 0;
    }

    // ── enable-user:**只留一份** ────────────────────────────────
    //
    // 舊版是「三份各啟用一次」,而那正是使用者回報的問題:
    // 語言清單上我們佔三格,微软拼音與小狼毫各佔一格。
    // 更糟的是,對一個他清單裡**沒有**的語言做啟用,Windows 會順手
    // 把那個語言加進他的語言清單 —— 截圖上那兩欄繁體中文底下只有
    // LuminaKey 一個輸入法,就是這麼來的。
    const int keep = DecideProfileIndex(explicit_langid, true);
    if (keep < 0) {
      Say("!! 沒有可以啟用的語言設定檔 —— 這個使用者的清單上不會出現這個輸入法\n");
      return 1;
    }
    int on = 0, off = 0;
    const HRESULT hr = KeepOnlyProfileEnabled(keep, &on, &off);
    Say("  啟用 0x%04X %s\n", static_cast<unsigned>(ProfileLangId(keep)),
        WideToUtf8(ProfileGuidString(keep)).c_str());
    Say("  停用其餘 %d 份(升級時把舊版多開的那幾份收回來)\n", off);
    if (on == 0) {
      Say("!! 啟用失敗 hr=0x%08lX —— 這個使用者的清單上不會出現這個輸入法\n",
          static_cast<unsigned long>(hr));
      return 1;
    }
    // 收斂之後再問一次 TSF。**不要相信自己剛才做了什麼** ——
    // 這一步抓的是「呼叫都成功了,但實際上還是兩份」那種狀態,
    // 而那正是使用者看到的東西。
    const std::vector<int> now = EnabledProfileIndexesForCurrentUser();
    Say("  收斂後已啟用 %d 份:", static_cast<int>(now.size()));
    for (int i : now) Say(" 0x%04X", static_cast<unsigned>(ProfileLangId(i)));
    Say("\n");
    if (now.size() != 1) {
      Say("!! 預期正好 1 份,實際 %d 份 —— 使用者的語言清單上會出現 %d 格\n",
          static_cast<int>(now.size()), static_cast<int>(now.size()));
      return 1;
    }
    Say("啟用:1 份(清單上只會有一格)\n");
    return 0;
  }

  if (verb == L"check") {
    CheckOptions opt;
    if (::GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES)
      opt.expect_dll_path = dll;
    opt.check_user = want_user;
    opt.check_enum = want_enum;
    return CheckRegistration(opt) ? 0 : 1;
  }

  if (verb == L"find-window") {
    // ⚠ 這個動詞存在的理由是**編碼**,不是功能。
    //
    //   原本這一格是 verify_installer.sh 用 PowerShell 的 FindWindowW 做的,
    //   而類別名與「開始」功能表的路徑都含中文。Git Bash → powershell.exe
    //   的命令列會經過一次代碼頁轉換,中文在那裡被換掉之後,
    //   FindWindow 找的是一個不存在的類別 —— 於是它**永遠回報找不到**,
    //   而那看起來與「設定視窗真的沒開出來」一模一樣。
    //
    //   類別名由呼叫端傳進來(腳本從 scripts/lib/product.env 推導),
    //   這裡刻意**不寫死** —— 寫死就變成產品名的第二份。
    if (window_class.empty()) {
      Say("!! find-window 需要 --class <類別名>\n");
      return 2;
    }
    const HWND h = ::FindWindowW(window_class.c_str(), nullptr);
    const bool visible = h && ::IsWindowVisible(h);
    Say("類別「%s」:%s\n", WideToUtf8(window_class).c_str(),
        h ? (visible ? "找到了(顯示中)" : "找到了(隱藏)") : "找不到");
    if (h) {
      DWORD pid = 0;
      ::GetWindowThreadProcessId(h, &pid);
      Say("  hwnd=%p pid=%lu visible=%d\n", static_cast<void*>(h),
          static_cast<unsigned long>(pid), visible ? 1 : 0);
    }
    // ⚠ --visible 不是可有可無的。設定視窗在**服務一啟動時就被建好但不顯示**
    //   (見 service/settings_window.cc:按下去要立刻看到窗,不能等它建)。
    //   所以「視窗存在」對任何一支跑著的服務都恆真 —— 拿它當斷言的話,
    //   「按下去有反應」與「按下去沒反應」會一起通過。使用者看得到的是
    //   **顯示出來**,不是存在。
    if (require_visible) return visible ? 0 : 1;
    return h ? 0 : 1;
  }

  if (verb == L"capture-window") {
    if (window_class.empty() || capture_out.empty()) {
      Say("!! capture-window 需要 --class <類別名> --out <bmp 路徑>\n");
      return 2;
    }
    return CaptureWindow(window_class, capture_out, capture_page);
  }

  if (verb == L"dump") {
    DumpRegistration();
    return 0;
  }

  if (verb == L"doctor") {
    // 結束碼 = 失敗的格數。0 = 全綠。
    //
    // ⚠ 這一點讓 doctor 變成一個**可以被 CI 斷言**的東西,而不是一份
    //   只會印綠字的報告。windows/verify_installer.sh 對它有正反兩面的
    //   斷言:安裝前必須紅、安裝後必須綠、把服務停掉之後必須再度紅。
    //   一支只會印綠字的診斷工具比沒有更糟 —— 它讓人以為有人在看。
    const int fails = RunDoctor(doctor);
    return fails == 0 ? 0 : 1;
  }

  if (verb == L"user-data-path") {
    // 只印一行,不印別的 —— 安裝程式會把它整個讀進去當路徑用。
    // ⚠ 這個動詞存在的唯一理由是「路徑只有一份」:安裝程式若自己拼
    //   {userappdata}\<資料夾名>,產品改名時就會漏掉那一處。
    Say("%s\n", WideToUtf8(RimeUserDataDir()).c_str());
    return 0;
  }

  if (verb == L"purge-user-data") return PurgeUserData(purge_confirmed);

  if (verb == L"stop-service") return StopService(dir);

  if (verb == L"sweep-stale-dlls") {
    // ⚠ **這個動詞永遠以 0 結束,而且那是刻意的。**
    //
    //   它掃的是升級時被改名挪開的舊 DLL。掃不掉的唯一原因是「還有進程
    //   握著那份映像」——瀏覽器可以開好幾天,那是**完全正常**的狀態,
    //   不是故障。把它報成失敗的話,呼叫端(解除安裝程式)就得決定
    //   要不要因此中止或要求重新開機,而那正是這一輪要消滅的東西。
    //
    //   真的出問題時看得出來:下面三個數字是機器可讀的,
    //   windows/verify_installer.sh §13 拿它們斷言。
    const StaleDllSweep s = SweepStaleTsfDlls(dir, schedule_if_locked);
    Say("目錄: %s\n", WideToUtf8(dir).c_str());
    Say("樣式: %s\n", WideToUtf8(RimeStaleTsfDllPattern()).c_str());
    Say("SWEPT_DELETED=%d\n", s.deleted);
    Say("SWEPT_LOCKED=%d\n", s.locked);
    Say("SWEPT_SCHEDULED=%d\n", s.scheduled);
    if (s.locked > 0 && s.scheduled == 0 && !schedule_if_locked)
      Say("  (%d 個還被握著 —— 留著就好,下次啟動時再掃一次)\n", s.locked);
    return 0;
  }

  Say("未知動詞: %s\n", WideToUtf8(verb).c_str());
  Usage();
  return 2;
}
