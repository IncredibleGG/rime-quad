// windows/winshared/winutil.h — DLL 與服務進程都要用的一小撮 Windows 工具
//
// 刻意與 windows/common/ 分開:common/ 底下的東西保證不 include windows.h,
// 因此能在開發用的 Ubuntu 上編譯並跑測試。這裡的東西不行,所以放在別的目錄,
// 免得哪天有人在 common/ 裡順手 #include <windows.h>,那條驗證管道就沒了。
#ifndef RIMEWIN_WINSHARED_WINUTIL_H_
#define RIMEWIN_WINSHARED_WINUTIL_H_

#include <windows.h>

#include <string>

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

// 目前的進程是不是被提權的。TSF 的 DLL 會被載入到提權的進程裡,
// 而從那裡 CreateProcess 起來的服務也會是提權的 —— 那支服務接著會用
// 提權的身分去讀寫使用者的設定檔與詞庫。不可以。
bool IsProcessElevated();

// 本模組(DLL 或 exe)所在的目錄,結尾不含反斜線。
std::wstring ModuleDirectory(HMODULE module);

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
const wchar_t* RimeUserDataFolderName();

}  // namespace rimewin

#endif  // RIMEWIN_WINSHARED_WINUTIL_H_
