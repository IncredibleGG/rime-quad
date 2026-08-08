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

// 目前的進程是不是被提權的。TSF 的 DLL 會被載入到提權的進程裡,
// 而從那裡 CreateProcess 起來的服務也會是提權的 —— 那支服務接著會用
// 提權的身分去讀寫使用者的設定檔與詞庫。不可以。
bool IsProcessElevated();

// 本模組(DLL 或 exe)所在的目錄,結尾不含反斜線。
std::wstring ModuleDirectory(HMODULE module);

}  // namespace rimewin

#endif  // RIMEWIN_WINSHARED_WINUTIL_H_
