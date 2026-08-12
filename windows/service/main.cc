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
//   rime_service.exe --settings             把設定視窗叫出來。已經有一支服務
//                                           在跑的話就通知它並立刻結束 ——
//                                           不會起第二支服務。
//   rime_service.exe --print-choice <langid>  只印出「這個語言會選哪個方案」
//                                           就結束(給 CI 斷言用,不啟動引擎)
//
// ── 資料目錄:安裝到 Program Files 之後,這一格是最容易錯的 ────────
//
//   shared  <安裝目錄>\data\shared    唯讀。方案、詞庫、opencc 詞典。
//   seed    <安裝目錄>\data\user      唯讀**範本**,首次執行時複製過去。
//   user    %APPDATA%\LuminaKey       可寫。使用者詞典、設定、編譯產物。
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

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "../winshared/winutil.h"
#include "cand_window.h"
#include "engine.h"
#include "pipe_server.h"
#include "rime_shell.h"
#include "settings_store.h"
#include "settings_window.h"
#include "status_bar.h"

namespace {

// 全部輸出走**窄字元 UTF-8**,不用 fwprintf。
//
// 兩個理由,而第二個是實際踩到的:
//   1. tools/rime_console.cc 已經證明這條路在 Windows 上是對的 ——
//      輸出導進檔案,拿到的是原始 UTF-8 位元組,不經過主控台的代碼頁轉換。
//   2. 在同一個 FILE* 上混用寬字元與窄字元 I/O 是未定義行為,而 librime
//      與 glog 用的是窄字元。我們先 fwprintf 過的話,之後它們寫同一條串流
//      的行為就沒有保證了。
// ── ⚠ 服務進程的記錄要落地,不能只寫 stderr ──────────────────────
//
// 瘦 DLL 啟動服務時用的是 CREATE_NO_WINDOW | DETACHED_PROCESS,而且**沒有**
// 重導向 stdout/stderr(見 tsf/ipc_client.cc 的 LaunchService)。也就是說:
// **使用者機器上跑的那一支服務,它印的每一個字都掉進黑洞。**
//
// 代價是實際付過的:2026-08-09 冷啟動那一格紅了,而它是 fail-open,
// 使用者只看到英文。要查「為什麼建 session 花了 300 毫秒以上」時,
// 唯一會寫下答案的那一支的輸出**根本不存在** —— 只有 CI 用
// `rime_service.exe > service.log` 自己啟動的那一支看得到,
// 而那正好不是使用者走的那條路。
//
// 所以這裡多寫一份到 %LOCALAPPDATA%\<資料夾名>\diagnostics\service.log,
// 與瘦 DLL 的 tsf.log 放在一起(doctor 已經會去看那個目錄)。
// 失敗一律安靜:記錄壞掉不該讓輸入法跟著壞掉。
// 只有在 stdout/stderr **真的沒有去處**時才接到檔案上。
//
// ⚠ 條件寫成這樣是刻意的:CI 用 `rime_service.exe > service.log` 啟動時
//   控制代碼是有效的,那一條路必須原封不動 —— §6e 靠它讀 SESSION_NEW_MS。
//   要補的只有「瘦 DLL 啟動的那一支」,而它是 DETACHED_PROCESS、
//   沒有任何重導向,所以控制代碼是空的。
//
// 走 freopen 而不是自己包一層 tee,是為了**連 pipe_server.cc 的 Log()
// 也一起接住** —— SESSION_NEW_MS 那一行正是它印的。自己包一層的話,
// 那一行還是會掉進黑洞,而它就是最需要看到的那一行。
void RedirectStdIoIfDetached() {
  const HANDLE herr = ::GetStdHandle(STD_ERROR_HANDLE);
  if (herr != nullptr && herr != INVALID_HANDLE_VALUE) return;

  wchar_t local[MAX_PATH] = {0};
  const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return;
  std::wstring dir = std::wstring(local) + L"\\" +
                     rimewin::RimeUserDataFolderName();
  ::CreateDirectoryW(dir.c_str(), nullptr);
  dir += L"\\diagnostics";
  ::CreateDirectoryW(dir.c_str(), nullptr);
  const std::wstring path = dir + L"\\service.log";

  // 超過 1 MiB 就從頭來過 —— 舊的是更舊的故障,而診斷要看最近這一次。
  // (與 tsf/trace.cc 同一個規矩,不做輪替:兩個檔案只會讓「最近那次在
  //  哪一個裡面」變成一個新問題。)
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  const bool too_big =
      ::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) &&
      fad.nFileSizeHigh == 0 && fad.nFileSizeLow > (1u << 20);
  if (!::_wfreopen(path.c_str(), too_big ? L"w" : L"a", stderr)) return;
  ::_wfreopen(path.c_str(), L"a", stdout);
}

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

// %APPDATA%\LuminaKey(Roaming)。
//
// 為什麼是 Roaming 而不是 Local:這裡放的是**使用者的資料** —— 自訂短語、
// 學習過的詞、設定。跟著人走是對的,而 RIME 生態(Weasel 用 %APPDATA%\Rime)
// 也是這個慣例,使用者搬移或備份時找得到。
// 代價是編譯產物(<user>\build)也跟著漫遊,在有漫遊設定檔的網域環境裡會變重。
// 那是已知的取捨,不是沒想過。
std::string DefaultUserDir() {
  // ⚠ 路徑本身由 winshared/winutil.cc 的 RimeUserDataDir() 決定,
  //   **這裡不再自己拼**。呼叫者已經有三個(服務、rime_ime_setup 的
  //   doctor 與「解除安裝時一起刪資料」、以及安裝程式),
  //   各拼一份的話,資料夾名一改就會漏掉一處 ——
  //   而那一處會去讀 / 去刪一個不存在的資料夾然後回報成功。
  return rimewin::WideToUtf8(rimewin::RimeUserDataDir());
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

// ── 引擎預熱 ────────────────────────────────────────────────────────
//
// ⚠ 這個函式存在的理由,是一個會讓使用者說「有時候打不出字」的缺陷。
//
// 第一次 rs_select_schema() 不是幾毫秒的事:它要開詞典(Table / Prism 的
// mmap)、開使用者詞庫(LevelDB)、載入文法。冷的時候是**幾百毫秒到幾秒**。
//
// 而 DLL 那一側建 session 的預算只有 300ms(tsf/ipc_client.cc 的
// kConnectTimeoutMs),因為 EnsureReady() 是在**宿主的 UI 執行緒**上跑的,
// 不能久等 —— 等下去使用者感覺到的是整個應用程式卡住。
//
// 兩件事湊在一起就是:服務端處理 SESSION_NEW 時正好會做那一次昂貴的
// rs_select_schema(替使用者的語言挑預設方案),於是**第一個連上來的宿主
// 幾乎必定超時**。連線被丟掉、進退避、fail-open。使用者看到的是
// 「剛開機 / 剛裝好的時候,第一個程式裡打不出中文,過一下才好」。
// 而且每一次超時的請求仍然會在引擎執行緒上跑完(用戶端已經走了),
// 下一次重試又排一份新的 —— 佇列會越積越長,可能一直追不上。
//
// 修法不是把預算調大:那只是把卡頓從「連不上」搬到「宿主的 UI 執行緒卡 3 秒」。
// 修法是**在對外服務之前先把那一次昂貴的載入做完**,做在 server.Start()
// 之前,所以「管道開了」就等於「答得動 SESSION_NEW」。
//
// 涵蓋範圍要講清楚:這裡只預熱**預設方案**。使用者手動切到別的方案
// (Ctrl+`)時仍然會付一次載入成本,但那條路不在連線的關鍵路徑上,
// 超時了也只是那一顆鍵沒作用,不會讓整條連線被丟掉。
// ⚠⚠ 預熱要暖的是**「方案 + 選項」那一整組**,不是只有方案。
//
// 舊版只用 langid 0 暖一次,理由寫的是「三份語言設定檔選到的都是同一個
// 方案,所以涵蓋得到全部三種使用者」。**那句話是對的,但它不相干** ——
// 貴的東西不在方案那一格:
//
//   langid 0     → CharSetOfLangId(0) = kUnspecified
//                → DecideVariant 直接回 false → set_variant = false
//                → ApplyChoice(sess, "luna_pinyin_tw", **{}**)  ← 一個選項都沒有
//
//   langid 0x0804 → kHans → set_variant = true, simplified = true
//                → PlanVariant → { simplification:true, zh_hans:true, … }
//
// 方案 id 一樣,但**選項完全不同**。而 simplification / zh_hant / zh_hans
// 這一組要載入 OpenCC 的 .ocd2 字典,那是第一次用到才發生的檔案載入 ——
// 於是真正的第一個使用者在 SESSION_NEW 的 300 毫秒預算裡付掉那筆錢,
// 逾時、fail-open,打出一串英文。
//
// **這就是 2026-08-09 §5c 冷啟動那一格紅掉的原因**,而冷啟動 = 每一個新
// 使用者的第一次。它不是隨機的,是第一次一定會發生。
//
// 所以現在對**每一種使用者真的會帶進來的 langid** 各暖一組。
// 涵蓋的是 CharSetOfLangId 的每一種結果,以及 PlanVariant 會分岔的每一種
// 字形(臺灣 / 香港 / 簡體)—— 也就是 ChooseSchema + PlanVariant 的整個值域。
//
// ⚠ 那份 langid 清單在 common/schema_choice.h(純邏輯層),不在這裡 ——
//   放在那裡,tests/test_schema_choice.cc 才驗得到「暖的那一組與真的用的
//   那一組是同一組」,而那是 Ubuntu 上跑得動的純邏輯,不必等一輪 CI。
void WarmUpEngine(rimewin::Engine* engine, rimewin::SettingsStore* store,
                  rimewin::StatusBar* bar) {
  const ULONGLONG t0 = ::GetTickCount64();
  std::vector<std::string> ids;
  // ⚠ 「引擎沒有回應」與「一個方案都沒有」不是同一件事。分不出來的話,
  //   ChooseSchema 會在**空清單**上挑,挑不到就什麼都不套 —— 使用者
  //   拿到的是一個沒有方案的 session,而記錄裡看起來像正常暖機。
  std::vector<std::pair<std::string, std::string>> schemas;
  if (engine->SchemaList(&schemas) != rimewin::WorkQueue::Status::kDone)
    Say("[service] 預熱:引擎沒有回應,這一輪拿不到方案清單\n");
  for (const auto& kv : schemas) ids.push_back(kv.first);
  const rimewin::Settings st = store ? store->Load() : rimewin::Settings();
  const rimewin::SchemaPreference pref = st.SchemaPref();
  const rimewin::Tri punct = st.Punctuation();
  const rimewin::Tri shape = st.Shape();
  // ⚠ 把簡繁偏好種進引擎。Engine::SelectAndApply 每次換方案之後要用它
  //   重算一次,而它在使用者第一次改設定之前都是**預設值** ——
  //   少了這一行,症狀是「換一次方案,簡繁跳回預設」,而他沒有碰過簡繁。
  if (engine) engine->SetVariantPref(pref);

  int warmed = 0;
  for (int wi = 0; wi < rimewin::kWarmUpLangIdCount; ++wi) {
    const uint32_t langid = rimewin::kWarmUpLangIds[wi];
    // ⚠ 暖的與真的用的必須是**同一組**選項,否則等於沒暖。
    //
    //   這裡以前是手拉的一段,上面寫著一句「必須與 pipe_server 逐字
    //   相同」—— 而它已經漂了一個 ascii_mode。後果不是「少設一個
    //   選項」,是 TakeSpareSession 的 SameOptions 連長度都不符 →
    //   **每一個預熱好的備用 session 必定被丟掉**。現在兩條路徑
    //   只有一份計畫(common/schema_choice.cc 的 BuildOptionPlan),
    //   而 tests/test_schema_choice.cc 把它的形狀釘住了。
    const rimewin::SchemaChoice choice =
        rimewin::ChooseSchema(langid, ids, pref);
    const std::vector<rimewin::OptionAssign> opts = rimewin::BuildOptionPlan(
        choice, langid, punct, shape, engine ? engine->AsciiMode() : false);

    const ULONGLONG t1 = ::GetTickCount64();
    // ⚠ 建好之後**留著**,不要用完就丟。
    //
    //   舊版是建一個 session、套上方案、然後 EndSession —— 它暖的是
    //   librime 內部的快取(詞典 mmap 之類),而那確實有用。但量到的
    //   rs_session_create 偶爾要 442~753 毫秒(CI run 31316116994),
    //   那是**每一次建立都要付**的,快取暖不掉它。
    //
    //   而 SESSION_NEW 的預算是 300 毫秒。所以把這個已經建好、已經配好的
    //   session 留下來當備用 —— 管道一打開,第一個連上來的宿主直接拿走,
    //   一次上鎖就好,完全不進引擎佇列。
    engine->PrimeSpareSession(langid, choice.schema_id, opts);
    // ⚠ 順手把方案名種給那一橫。引擎到這裡已經知道要用哪一份方案了,
    //   而那一橫要等第一個宿主連上來、而且使用者真的按了一顆鍵才知道
    //   —— 中間那段空窗期它會少畫一格(使用者實機回報過「方案名不見了」)。
    //   種子不是權威:第一份快照一到就覆蓋。只種一次,用第一個 langid 的結果。
    if (bar && warmed == 0 && !choice.schema_id.empty()) {
      // 這一次一定是快取命中(上面剛問過),所以走 Cached 那一支;
      // 真的拿不到就不種 —— 種一個空名字比不種更糟。
      std::vector<std::pair<std::string, std::string>> named;
      if (engine->SchemaListCached(&named) == rimewin::WorkQueue::Status::kDone) {
        for (const auto& kv : named) {
          if (kv.first == choice.schema_id) { bar->SeedSchemaName(kv.second); break; }
        }
      }
    }
    ++warmed;
    Say("[service] 預熱 langid=0x%04X 方案=%s 選項=%d 個 耗時=%llu ms(留作備用)\n",
        static_cast<unsigned>(langid),
        choice.schema_id.empty() ? "(沒有選到)" : choice.schema_id.c_str(),
        static_cast<int>(opts.size()),
        static_cast<unsigned long long>(::GetTickCount64() - t1));
  }
  Say("[service] 預熱完成:%d / %d 組,總耗時 %llu ms\n", warmed,
      rimewin::kWarmUpLangIdCount,
      static_cast<unsigned long long>(::GetTickCount64() - t0));
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
  // ⚠ 第一件事。在這之前印的任何東西,對「瘦 DLL 啟動的那一支」來說
  //   都是掉進黑洞的 —— 而那正是使用者機器上跑的那一支。
  RedirectStdIoIfDetached();

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
  bool open_settings = false;
  long print_choice = -1;
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
    else if (a == L"--settings") open_settings = true;
    else if (a == L"--print-choice" && i + 1 < argc)
      print_choice = ::wcstol(argv[++i], nullptr, 0);
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
        "  安裝目錄(通常是 C:\\Program Files\\LuminaKey)對一般權限的進程是唯讀的,\n"
        "  而 librime 寫不進去時**不會報錯**,只是一個學過的詞都留不住。\n"
        "  使用者資料應該在 %APPDATA%\\LuminaKey。\n",
        user.c_str());
    return 1;
  }

  // --print-dirs 放在這裡:三個目錄都解析完、但還沒動任何狀態(沒有建目錄、
  // 沒有搶單一實例的 mutex、沒有起引擎)。CI 靠它在安裝完成後**立刻**斷言
  // 「shared 指到 Program Files、user 指到 %APPDATA%」,不必等好幾分鐘的詞庫編譯。
  // --print-choice:只做「langid → 方案」那一格的判斷就結束。
  //
  // 這一條是給 CI 用的:它不啟動引擎、不碰管道、不需要詞庫,所以在
  // install-x64 那個 job 裡跑得動,而且斷言的是**裝好的那份二進位**
  // 真的會替簡體使用者選簡體方案 —— 不是單元測試裡的那一份。
  if (print_choice >= 0) {
    rimewin::SettingsStore store(user);
    const rimewin::Settings st = store.Load();
    // 不問引擎(它還沒起來),所以 available 是空的 —— ChooseSchema 對
    // 空清單會回第一順位。這正是我們要斷言的那個「表上寫什麼」。
    // 這裡刻意給一份**寫死的**已啟用清單,而不是空的:規範 §4.4 的
    // 第 3 / 4 層要看清單才算得出來,而引擎這時還沒起來。用的是
    // scripts/collect_data.sh 實際打包的那四個,順序也一樣。
    const std::vector<std::string> shipped = {"luna_pinyin_tw", "bopomofo_tw",
                                              "luna_pinyin", "t9_pinyin"};
    const uint32_t lang = static_cast<uint32_t>(print_choice);
    const rimewin::SchemaChoice c =
        rimewin::ChooseSchema(lang, shipped, st.SchemaPref());
    Say("langid=0x%04X (%s)\n", static_cast<unsigned>(lang),
        rimewin::LangIdName(lang));
    Say("schema=%s\n", c.schema_id.c_str());
    Say("variant=%s\n", c.set_variant ? (c.simplified ? "simplified"
                                                       : "traditional")
                                       : "(不干預)");
    // 實際會送出去的那一組 option,一字不差 —— 斷言的是真的會發生的事,
    // 不是一個中間表示。
    //
    // ⚠ 這裡以前只印簡繁那一組,而真的送出去的計畫還含 ascii_punct 與
    //   ascii_mode。一份「少印兩項」的診斷輸出比沒有診斷更糟:CI 拿它
    //   當判準,而它與現場不是同一份。現在走的是與暖機、SESSION_NEW
    //   同一支 BuildOptionPlan。
    //
    // ⚠ ascii_mode 在這條路徑上只能是預設值(false = 中文):
    //   --print-choice 不啟動引擎,行程層級的那個狀態還不存在。
    for (const rimewin::OptionAssign& a : rimewin::BuildOptionPlan(
             c, lang, st.Punctuation(), st.Shape(), /*ascii_mode=*/false))
      Say("option=%s=%s\n", a.option, a.value ? "true" : "false");
    Say("source=%s\n", c.source);
    return 0;
  }

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
  // ⚠ 名字的定義在 winshared/winutil.cc,不在這裡。瘦 DLL 與
  //   rime_ime_setup.exe doctor 都要問「服務在不在」,三份各抄一次的話,
  //   漂移的症狀是「DLL 每次都以為服務沒在跑」——它會一直嘗試啟動,
  //   而每一支新的都被單一實例擋掉,誰都不會報錯。
  const std::wstring mutex_name = rimewin::RimeServiceMutexName();
  HANDLE single = ::CreateMutexW(nullptr, TRUE, mutex_name.c_str());
  if (!single || ::GetLastError() == ERROR_ALREADY_EXISTS) {
    // 已經有一支在跑,這不是錯誤。
    //
    // ⚠ 但如果是 --settings 進來的,**一定要把訊息傳過去**。
    //   靜靜結束的話,使用者按了語言列上的「設定」什麼都不會發生 ——
    //   而那正是這個專案抓過四次的那種鍵。
    if (open_settings) {
      HANDLE ev = ::OpenEventW(EVENT_MODIFY_STATE, FALSE,
                               rimewin::RimeSettingsEventName().c_str());
      if (ev) {
        ::SetEvent(ev);
        ::CloseHandle(ev);
      } else {
        Err("找不到執行中的服務的設定事件 —— 設定視窗叫不出來。\n");
        if (single) ::CloseHandle(single);
        return 1;
      }
    }
    if (single) ::CloseHandle(single);
    return 0;
  }

  // ── 掃掉升級留下的舊 DLL ────────────────────────────────────────
  //
  // 升級時安裝程式把舊的 rime_tsf.dll 改名挪開(理由見
  // docs/decisions/no-restart.md);當下那顆檔案還被宿主進程握著,刪不掉。
  // 這裡是「下次啟動時再檢查一次」的那一次 —— 而且是最合適的一次:
  // 服務進程在使用者登入後才被拉起來,那時上一輪的宿主大多已經結束了。
  //
  // ⚠ 三件事刻意如此:
  //   · 放在搶到單一實例**之後**,所以同一台機器上只有一支在掃。
  //   · 不排開機清除(那要寫 HKLM,而服務是一般權限)—— 只做刪得掉的。
  //   · **失敗完全不影響啟動流程**,連一行錯誤都不算。刪不掉只代表
  //     還有人握著,那是正常狀態,不是故障。
  {
    const rimewin::StaleDllSweep sw = rimewin::SweepStaleTsfDlls(
        rimewin::ModuleDirectory(nullptr), /*schedule_if_locked=*/false);
    if (sw.deleted > 0 || sw.locked > 0)
      Err("[service] 舊 DLL 清理:刪掉 %d 個,%d 個還被握著(留著,下次再掃)\n",
          sw.deleted, sw.locked);
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

  // ── 設定介面 ────────────────────────────────────────────────
  //
  // 視窗一開始就建好但不顯示(見 settings_window.cc):使用者按下語言列
  // 按鈕時要立刻看到窗,而「按了之後過一秒才出現」與「按了沒反應」
  // 在使用者眼裡是同一件事。系統匣圖示也掛在它的訊息迴圈上。
  rimewin::SettingsStore settings_store(user);
  rimewin::SettingsWindow settings(&engine, &settings_store, shared);
  // ── 懸浮狀態列 ──────────────────────────────────────────────
  //
  // ⚠ 它不是裝飾。在這一輪之前,Windows 端**完全沒有中英切換** ——
  //   `ascii_mode` 從來沒有被設定過,使用者要在句子中間打一個英文字,
  //   唯一的辦法是按 Win+空白鍵把整個輸入法換掉。
  //   這一橫是那個功能唯一的家,所以它預設是開的(見 status_bar.h)。
  rimewin::StatusBar status_bar(&engine, &settings_store);
  HANDLE settings_event = nullptr;
  if (!no_ui) {
    settings.SetCandidateWindow(&window);
    if (!settings.Start()) {
      Err("設定視窗建立失敗 —— 語言列與系統匣的入口這次不會出現。\n");
    } else {
      // 啟動時就把候選字級套上去。少了這一步,使用者要重開一次設定
      // 才會看到自己上次選的字級 —— 那看起來像「設定沒有被記住」。
      const rimewin::Settings st = settings_store.Load();
      const int scale = st.GetEnumInt(rimewin::keys::kAppearanceCandidateScale,
                                      rimewin::kCandScaleValues,
                                      rimewin::kCandScaleCount);
      if (scale > 0) window.SetTextScale(scale / 100.0);
      settings_event = ::CreateEventW(
          nullptr, FALSE, FALSE, rimewin::RimeSettingsEventName().c_str());
      // 兩個表面互相認識:狀態列要開設定視窗,設定視窗改了外觀/語言/
      // 顯示與否要通知狀態列。
      if (status_bar.Start()) {
        status_bar.SetSettingsWindow(&settings);
        settings.SetStatusBar(&status_bar);
      } else {
        Err("懸浮狀態列建立失敗 —— 這一輪沒有中英切換的入口。\n");
      }
    }
  }

  // ── 預熱 ────────────────────────────────────────────────────
  //
  // 部署做完了才有東西可以預熱(見 WarmUpEngine 的說明)。
  //   · --wait-deploy 走過的路徑(CI、以及第一次安裝)這裡一定是 true,
  //     所以預熱是**同步**的:管道還沒開,ready 檔還沒寫。
  //   · 正常啟動沒有 --wait-deploy,部署還在背景跑。那時引擎本來就給不出
  //     候選(ProcessKey 會直接回「沒處理」),所以改用一條背景執行緒
  //     等它做完再預熱 —— 不能為此擋住服務啟動,首次部署要好幾分鐘。
  //
  // ⚠ 一定要兩條都做。只做同步那一條的話,預熱就變成「只有 CI 走得到」
  //   的程式碼 —— 綠燈驗到的是使用者永遠不會走的那條路。
  std::thread warm_thread;
  std::atomic<bool> warm_stop{false};
  // ⚠  時沒有那一橫(CI 的某些 job 這樣跑),傳 nullptr 而不是漏傳 ——
  //   漏傳會變成「有 UI 的時候也沒種」,而那正是要修的缺陷。
  rimewin::StatusBar* const warm_bar = no_ui ? nullptr : &status_bar;
  if (engine.deploy_ok()) {
    WarmUpEngine(&engine, &settings_store, warm_bar);
  } else {
    warm_thread = std::thread([&]() {
      while (!warm_stop.load() && !engine.deploy_done())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (!warm_stop.load() && engine.deploy_ok())
        WarmUpEngine(&engine, &settings_store, warm_bar);
    });
  }

  rimewin::PipeServer server(&engine, ui, &settings_store);
  // 每一份快照也要餵給那一橫(見 pipe_server.h 的說明)。
  server.SetStatusBar(no_ui ? nullptr : &status_bar);
  // 簡繁快捷鍵要走設定視窗那一支寫入(見 pipe_server.h)。
  server.SetSettingsWindow(no_ui ? nullptr : &settings);
  server.SetOpenSettingsHandler([&settings]() { settings.Open(); });
  // 監聽迴圈非預期死掉時,讓這支服務結束。
  //
  // ⚠ 不結束的話它會變成一具擋路的空殼:沒有管道,卻仍然佔著單一實例的
  //   mutex,於是 DLL 想啟動一支新的也啟動不了(新的那支會判定「已經有
  //   一支在跑」然後以 0 結束)。使用者要重開機才會好。
  //   結束掉,DLL 的自動啟動就能接手 —— 那條路本來就在(見 ipc_client.cc
  //   的 TryLaunchService,含節流與「提權的宿主不准啟動」那道保護)。
  server.SetFatalHandler([&quit]() {
    if (quit) ::SetEvent(quit);
  });
  if (!server.Start()) {
    // Start() 失敗的原因已經由 pipe_server.cc 印出 [pipe] 開頭的那幾行,
    // 帶著 GetLastError()。這裡不要用一句籠統的話蓋掉它。
    Err("具名管道沒有備妥,服務不對外開放(原因見上面的 [pipe] 行)\n");
    warm_stop.store(true);
    if (warm_thread.joinable()) warm_thread.join();
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

  // 具名事件那條路。DLL 在管道還沒連上時走它(見 tsf/lang_bar.h)。
  // ⚠ 用**一條自己的執行緒**而不是把它併進下面的 WaitForSingleObject:
  //   結束事件與設定事件的語意完全不同,合在一起等的話,
  //   「開設定」有可能被寫成「結束服務」——那是最糟的一種手滑。
  std::thread settings_thread;
  if (settings_event) {
    settings_thread = std::thread([&]() {
      for (;;) {
        HANDLE waits[2] = {settings_event, quit};
        const DWORD r = ::WaitForMultipleObjects(quit ? 2 : 1, waits, FALSE,
                                                 INFINITE);
        if (r != WAIT_OBJECT_0) return;  // quit 或錯誤
        settings.Open();
      }
    });
  }
  if (open_settings) settings.Open();

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
    // ⚠ 這裡**不可以**立刻 CloseHandle(quit):設定事件那條執行緒也在等它。
    //   關掉之後那條執行緒會在一個已經失效的 handle 上等,而那是未定義行為
    //   —— 症狀會是「結束服務時偶爾當掉」,而且與設定功能看起來毫無關聯。
    //   --quit-after 逾時的路徑上 quit 從來沒被設起來過,所以這裡補一次,
    //   不然下面的 join 會永遠等下去。
    ::SetEvent(quit);
  } else {
    // 建不出事件不該讓服務起不來 —— 退回舊行為,只是停不掉而已。
    if (quit_after > 0)
      ::Sleep(static_cast<DWORD>(quit_after) * 1000);
    else
      for (;;) ::Sleep(60000);
  }
  Say("[service] 結束,收尾中\n");

  // 預熱執行緒可能還在等部署完成(部署要好幾分鐘,而使用者可以在那段時間
  // 裡按下解除安裝)。先叫它停,再 join —— 不然這裡會卡到部署結束。
  warm_stop.store(true);
  if (warm_thread.joinable()) warm_thread.join();

  if (settings_thread.joinable()) {
    // quit 上面一定被設起來了,所以那條執行緒會自己醒。
    settings_thread.join();
  }
  if (quit) ::CloseHandle(quit);
  if (settings_event) ::CloseHandle(settings_event);
  server.Stop();
  settings.Stop();
  window.Stop();
  engine.Stop();
  ::ReleaseMutex(single);
  ::CloseHandle(single);
  return 0;
}
