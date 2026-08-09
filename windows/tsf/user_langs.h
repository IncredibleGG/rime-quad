// windows/tsf/user_langs.h — 「這個使用者有哪些語言」與「只啟用一份」
//
// ── 為什麼是獨立的一個檔案 ──────────────────────────────────────
//
// 這裡的東西**只有 rime_ime_setup.exe 用得到**(安裝、升級、以及 CI 的斷言)。
// 放進 registration.cc 的話它會跟著進 rime_tsf.dll,而那支 DLL 住在
// 每一個接受文字輸入的進程裡 —— 瀏覽器、Office、提權視窗。
// 它不需要知道使用者的語言清單長什麼樣,所以它不該帶著這段程式碼。
// (相依上沒有新東西:advapi32 + ole32 兩邊本來都有。分開純粹是為了
//  「DLL 裡的每一行都說得出為什麼非在那裡不可」這條紀律。)
//
// ── 這裡在解的問題 ──────────────────────────────────────────────
//
// 見 common/profile_choice.h 的檔頭:清單上出現幾格由 **HKCU 的啟用**決定,
// 不是由 HKLM 的註冊決定。本檔負責兩件事:
//
//   1. 蒐集判斷所需的事實(使用者的語言清單、已安裝的輸入語言、顯示語言);
//   2. 把結論落地成「**正好一份**被啟用」,而且是冪等的 ——
//      舊版裝過三份的機器,升級時會被收斂回一份。
#ifndef RIMEWIN_TSF_USER_LANGS_H_
#define RIMEWIN_TSF_USER_LANGS_H_

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rimewin {

// 使用者的語言清單,**照他自己的順序**,BCP-47 標籤(如 "zh-Hans-CN")。
//
// 來源:HKCU\Control Panel\International\User Profile
//   · 先讀 REG_MULTI_SZ 的 `Languages` —— 它是有序的,而順序就是使用者
//     在「語言」設定頁上排的順序,也就是「他主要用哪一種中文」的答案。
//   · 讀不到就退回列舉子鍵(每個子鍵名就是一個標籤)。子鍵是**無序的**,
//     所以這是次等來源,但有總比沒有好。
//
// ⚠ 讀不到(兩條都失敗)時回空 vector,**不要編一個預設值**。
//   空的意思是「不知道」,而 ChooseUserProfileLangId 對「不知道」有
//   自己的下一層;編一個假的清單會讓那幾層永遠走不到。
std::vector<std::wstring> CurrentUserLanguageTags();

// 系統回報「已經有輸入法」的語言(ITfInputProcessorProfiles::GetLanguageList)。
// 失敗回空 vector。
std::vector<uint32_t> InstalledInputLangIds();

// 目前**這個使用者**啟用了我們的哪幾份 profile。回傳的是 kRimeProfiles 的索引。
//
// ⚠ 答案向 TSF 自己要(IsEnabledLanguageProfile),不是數登錄檔的子鍵。
//   停用之後 HKCU 底下那個鍵**可能還在**(只是 Enable 變 0),
//   數子鍵的話「停用了」與「還啟用著」會長得一模一樣 ——
//   而這一輪要斷言的正好就是「只剩一份」。
std::vector<int> EnabledProfileIndexesForCurrentUser();

// 讓**正好一份**被啟用,其餘全部停用。冪等。
//
// ⚠ 順序是「先開新的、再關舊的」,不可以反過來。
//   先全關的話,中間有一瞬間這個使用者一份都沒有 —— 而他很可能正用著
//   我們打字(升級時服務會被停掉,但 TSF 那一側的選擇是系統記著的)。
//   那一瞬間過後系統可能已經把輸入法切到別的地方去了。
//
// keep_index 是 kRimeProfiles 的索引。回傳最後一個失敗的 HRESULT
// (全部成功則 S_OK);*enabled / *disabled 回填實際成功的數量,可為 nullptr。
HRESULT KeepOnlyProfileEnabled(int keep_index, int* enabled, int* disabled);

}  // namespace rimewin

#endif  // RIMEWIN_TSF_USER_LANGS_H_
