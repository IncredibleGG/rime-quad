// windows/winshared/winutil.h — DLL 與服務進程都要用的一小撮 Windows 工具
//
// 刻意與 windows/common/ 分開:common/ 底下的東西保證不 include windows.h,
// 因此能在開發用的 Ubuntu 上編譯並跑測試。這裡的東西不行,所以放在別的目錄,
// 免得哪天有人在 common/ 裡順手 #include <windows.h>,那條驗證管道就沒了。
#ifndef RIMEWIN_WINSHARED_WINUTIL_H_
#define RIMEWIN_WINSHARED_WINUTIL_H_

#include <windows.h>

#include <string>

// 判斷邏輯在純邏輯層(不 include windows.h),這裡只提供事實。
#include "../common/elevation_policy.h"

namespace rimewin {

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

// 目前使用者的 SID 字串(例如 "S-1-5-21-…-1001")。取不到時回傳空字串。
std::wstring CurrentUserSidString();

// 具名管道的名字。**帶上使用者 SID**:同一台機器上不同使用者各自一支服務,
// 名字撞在一起的話,先登入的那個人會拿到後登入的人的管道 ——
// 而管道上流的是按鍵。
//
// 名字裡也帶協議版本:新舊版本同時存在時各自用各自的管道,
// 而不是連上去才發現版本不合。
std::wstring RimePipeName();

// 請服務進程結束用的具名事件。
//
// 為什麼需要它:升級與解除安裝之前必須先停掉服務,而服務持有使用者詞庫的
// LevelDB。直接 TerminateProcess 等於在寫入中途拔電源 —— 詞庫壞掉的症狀是
// 「升級之後我學過的詞全沒了」,而使用者不會把它跟解除安裝程序聯想在一起。
//
// 命名空間用 `Local\`(每個登入工作階段各一份)而不是 `Global\`:
// 服務本來就是每個使用者一支,而 Global\ 需要 SeCreateGlobalPrivilege。
// 名字裡帶 SID 的理由同管道名 —— 同一台機器上的兩個人不可以互相關掉對方的服務。
std::wstring RimeServiceQuitEventName();

// 服務的「單一實例」互斥鎖名稱。
//
// 定義在這裡而不是 service/main.cc,是因為現在有**三個**呼叫者:
//   · 服務自己(建立它,藉此擋掉第二支服務)
//   · 瘦 DLL(ActivateEx 時要判斷「服務在不在」,不能靠開管道 ——
//     服務剛啟動、詞庫還在編譯的那幾分鐘裡管道還沒開,而那時它明明在跑,
//     再啟動一支只會被單一實例擋掉,白花一次 CreateProcess)
//   · rime_ime_setup.exe doctor(要回答「服務在不在」這一格)
// 三個地方各抄一份名字的話,漂移的症狀是「DLL 每次都以為服務沒在跑」。
std::wstring RimeServiceMutexName();

// 「把設定視窗叫出來」的具名事件。服務進程建立並等待它;語言列按鈕與
// 系統匣圖示在管道還沒連上時走這一條。
//
// ⚠ 名稱裡帶使用者 SID,與管道同理:同一台機器上的另一個使用者不該
//   有辦法對我們的服務發號施令。
std::wstring RimeSettingsEventName();

// GUID → "{XXXXXXXX-…}"(大寫)。只作診斷與線路上的識別用;
// **比較 GUID 一律用 IsEqualGUID,不要比字串** —— 大小寫與括號的差異
// 會讓「看起來一樣」的兩個值比不相等。
std::string GuidToUtf8(const GUID& g);

// 目前的進程是不是被提權的。
//
// ⚠ **不要拿這一個來決定要不要啟動服務。** 它只回答「高不高」,而那個問題
//   的答案在內建 Administrator 帳號上永遠是「高」—— 於是服務永遠不啟動,
//   使用者永遠打不出字(2026-08 的實測回報)。要決定啟不啟動請用下面的
//   QueryHostElevation() / MayStartUserService()。
//
//   這一支仍然有它的用途:setup_main.cc 要判斷「我現在寫的 HKCU 是不是
//   提權帳號的那一份」,那是另一個問題,而且 IsProcessElevated 對它是對的。
bool IsProcessElevated();

// ── 這個宿主可不可以啟動服務進程 ─────────────────────────────────
//
// 判準與理由全部寫在 windows/common/elevation_policy.h 的檔頭。
// 這裡只負責把權杖裡的**事實**問出來交給那一格判斷 ——
// 判斷本身是純邏輯,在 Ubuntu 上有一張窮舉的真值表在守
// (tests/test_elevation_policy.cc)。
struct HostElevationFacts {
  HostElevation verdict = HostElevation::kUnknown;
  // 下面四個是**原始事實**,不是結論。診斷報告要印它們:使用者回報時
  // 我們才有辦法重算一次判定,而不是只能相信結論。
  bool token_query_ok = false;
  bool is_elevated = true;
  TokenSplit split = TokenSplit::kUnknown;
  bool service_account = false;
  std::wstring sid;
  // 內建 Administrator 是不是成因(SID 尾巴 -500)。**不參與判定** ——
  // 判定只看有沒有連結權杖(見 elevation_policy.h)。印出來是為了讓
  // 回報看得懂「為什麼這台機器整個工作階段都是提權的」。
  bool builtin_administrator = false;
};
HostElevationFacts QueryHostElevation();

// 只要判定的簡便版本。結果在進程的一生中不會變,呼叫端可以快取。
HostElevation ClassifyHostElevation();

// 本模組(DLL 或 exe)所在的目錄,結尾不含反斜線。
std::wstring ModuleDirectory(HMODULE module);

// ── 「改名挪開」留下的舊 DLL ──────────────────────────────────────
//
// 升級時安裝程式**不原地覆蓋** rime_tsf.dll,而是把舊的改名成
// `rime_tsf.dll.old-<時間戳>-<序號>` 再放上新檔(理由與機制見
// docs/decisions/no-restart.md 與 installer/luminakey.iss 的 [Files] 段)。
// 那顆改名後的檔案會被還沒結束的宿主進程繼續握著,所以當下刪不掉 ——
// 這幾支函式負責「以後再回來掃一次」。
//
// ⚠ **刪不掉不是錯誤。** 一次都不是。
//   刪不掉只代表還有進程握著那份映像(瀏覽器可以開好幾天),而那顆檔案
//   佔幾 MB、不影響任何功能。呼叫端一律不因此回報失敗,更不可以因此
//   要求使用者重新開機 —— 那正是這一輪要消滅的東西。
//
// ⚠ 檔名樣式**只寫在這裡一份**。安裝程式那一側(Pascal)另有一份字面值,
//   兩邊必須一致;windows/verify_installer.sh §13 會實際檢查磁碟上長出來的
//   名字對不對得上,所以漂移抓得到。
const wchar_t* RimeTsfDllName();
const wchar_t* RimeStaleTsfDllPattern();

struct StaleDllSweep {
  int deleted = 0;    // 真的刪掉了幾個
  int locked = 0;     // 刪不掉幾個(還有人握著)
  int scheduled = 0;  // 其中幾個排進了開機清除
};

// dir 底下所有 `rime_tsf.dll.old-*` 都試著刪掉。
//
// schedule_if_locked = true 時,刪不掉的那幾個會用
// MoveFileEx(..., MOVEFILE_DELAY_UNTIL_REBOOT) 排進開機清除。
// ⚠ 那個 API 要寫 HKLM,**需要系統管理員權限** —— 所以只有解除安裝程式
//   (提權的)會傳 true;服務進程一律傳 false,它只做刪得掉的那些。
StaleDllSweep SweepStaleTsfDlls(const std::wstring& dir, bool schedule_if_locked);

// ── 使用者資料目錄:**唯一的決定處** ──────────────────────────────
//
// `%APPDATA%\<資料夾名>`。取不到 %APPDATA% 時回傳空字串。
//
// ⚠ 這一格必須只有一份。呼叫者有三個,而且會越來越多:
//     · rime_service.exe    —— 詞典、設定、librime 的編譯產物都在那裡
//     · rime_ime_setup.exe  —— doctor 的引擎層檢查、以及「解除安裝時
//                              連我的資料一起刪」那個選項
//     · 安裝程式(Inno)      —— 它**不自己拼**這個路徑,而是問上面那支
//                              (`rime_ime_setup.exe user-data-path`)
//
//   各自拼一份的下場很具體:資料夾名一改(例如產品改名),漏掉的那一處
//   會去刪 / 去讀一個不存在的資料夾,然後**回報成功** ——
//   使用者以為資料刪乾淨了,其實原封不動;或以為設定存好了,其實沒有。
std::wstring RimeUserDataDir();

// 上面那個路徑的最後一段(資料夾名)。刪除前的安全檢查要用它比對,
// 而比對用的值當然不可以是另外抄的一份。
//
// ⚠ 它不只是「%%APPDATA%% 底下那一段」—— 它是**我們在使用者的側寫檔裡佔的
//   那個資料夾名**,而我們佔了兩處:
//     %%APPDATA%%\<名>       詞典、設定、librime 的編譯產物(RimeUserDataDir)
//     %%LOCALAPPDATA%%\<名>  瘦 DLL 的診斷記錄(tsf/trace.cc)
//   第二處原本自己抄了一份帶著**舊名**的路徑字面值,改名時是**兩個地方**
//   要改 —— 而漏掉的那一處不會編譯失敗、不會有錯誤訊息,
//   只會讓記錄檔靜靜地寫到一個叫舊名字的資料夾裡,然後使用者回報
//   「診斷說找不到記錄檔」。所以 trace.cc 現在問這一個函式。
//
//   回傳的是靜態儲存期的字串:trace.cc 會在 DllMain 的載入器鎖底下呼叫它,
//   那裡不可以配置堆積。
const wchar_t* RimeUserDataFolderName();

}  // namespace rimewin

#endif  // RIMEWIN_WINSHARED_WINUTIL_H_
