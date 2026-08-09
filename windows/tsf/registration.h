// windows/tsf/registration.h — 輸入法的註冊、反註冊,以及「註冊到哪些鍵」的唯一定義
//
// ── 為什麼要把這一段從 dll.cc 抽出來 ─────────────────────────────
//
// 這一輪有了安裝程式,於是同一段註冊邏輯有**兩個**呼叫者:
//
//   1. rime_tsf.dll 的 DllRegisterServer / DllUnregisterServer(regsvr32 走這條)
//   2. rime_ime_setup.exe(安裝程式走這條,而且它還要能**檢查**與**回報**)
//
// 兩邊各寫一份的話會漂移,而漂移的症狀是「用安裝程式裝的能用,自己 regsvr32
// 的不能用」——兩種註冊狀態長得幾乎一樣,只差一個子鍵。所以只有一份實作。
//
// 更重要的是第二個理由:**CI 現在驗得到註冊了。** windows-latest 的 runner 上
// 我們有系統管理員權限,所以「靜默安裝 → 斷言登錄檔真的長出東西 → 靜默解除安裝
// → 斷言清乾淨」整條是跑得動的。而要斷言,就得有一個地方明確說出
// 「註冊成功的定義是哪幾個鍵」——那就是這個檔案。
//
// ⚠ 這裡不放任何需要 oleaut32(BSTR)的東西。rime_tsf.dll 的相依有一份
//   很短的允許清單(見 windows/check_binaries.sh),而它住在**每一個**宿主
//   進程裡,多一個相依就多一種「在某台機器上載入失敗」的可能。
//   要列舉、要讀描述字串的那些東西在 registration_check.cc,那一份只連進
//   rime_ime_setup.exe,不連進 DLL。
#ifndef RIMEWIN_TSF_REGISTRATION_H_
#define RIMEWIN_TSF_REGISTRATION_H_

#include <windows.h>

#include <string>

namespace rimewin {

// ── 註冊會碰到的登錄檔位置(唯一定義處)────────────────────────
//
// 刻意**不用 HKEY_CLASSES_ROOT**。HKCR 是 HKLM\SOFTWARE\Classes 與
// HKCU\SOFTWARE\Classes 的合併檢視,寫進去時落在哪一邊取決於權限與
// 呼叫者的身分。安裝程式是全機安裝,就該明著寫 HKLM ——
// 而且 CI 要斷言的那個路徑也必須是確定的,不能「看情況」。
std::wstring ClsidString();          // "{7D02992E-…}"
std::wstring ClsidRegPath();         // HKLM 底下:SOFTWARE\Classes\CLSID\{…}
std::wstring InprocRegPath();        // …\InprocServer32
std::wstring CtfTipRegPath();        // HKLM 底下:SOFTWARE\Microsoft\CTF\TIP\{…}
std::wstring CtfCategoryRegPath();   // …\Category\Category

// ── 語言設定檔(每一個中文語言各一份)────────────────────────────
//
// 為什麼是多份:第一版只註冊 zh-Hant-TW(0x0404),結果系統語言是簡體中文的
// 使用者在自己的語言底下找不到這個輸入法 —— 它掛在「繁体中文(中国台湾)」
// 那一欄。使用者實際回報過。清單見 tsf/guids.cc 的 kRimeProfiles。
int ProfileCount();
LANGID ProfileLangId(int i);
std::wstring ProfileGuidString(int i);
bool IsOurProfile(REFGUID guid);

// 我們註冊了幾個能力類別(Category\Category 底下的子鍵數)。
// CI 靠這個數字斷言「類別註冊真的發生過」,不必在腳本裡寫死一串
// GUID_TFCAT_* 的值 —— 寫死等於憑印象抄一份 SDK 常數,抄錯的話這道檢查
// 會在「其實註冊正確」時失敗,而那看起來像產品壞了。
int RegisteredCategoryCount();

// 再深一層的總數(每個類別底下掛了幾筆)。多註冊一個語言,它自己會跟著變 ——
// 這一條抓的是「語言加了,但能力類別只替其中一份註冊」那種半套狀態。
int ExpectedCategoryItemCount();

// 全機註冊。需要系統管理員權限(寫 HKLM)。
// dll_path 必須是**絕對路徑**:登錄檔裡存的就是它,而 COM 之後會照著它去載入。
HRESULT RegisterTextService(const std::wstring& dll_path);

// 全機反註冊。盡量做完,不因為中途某一步失敗就停 ——
// 停在一半留下的殘骸,使用者只能自己動登錄檔。
HRESULT UnregisterTextService();

// 目前使用者的啟用 / 停用(寫 HKCU)。
//
// ⚠ 這兩支**必須以使用者自己的身分**執行,不可以在提權的權杖底下跑:
//   使用者用系統管理員帳號提權安裝時,提權後的 HKCU 是**那個管理員的**,
//   把輸入法加到他的清單裡,而真正在用電腦的人什麼都沒拿到。
//   安裝程式那邊靠 Inno 的 runasoriginaluser / ExecAsOriginalUser 處理。
//
// 一次做一份(index 是 kRimeProfiles 的索引)。刻意**不**包成「全做完」的
// 一支:使用者的語言清單裡通常只有其中一種中文,哪幾份會成功取決於他的系統,
// 不是我們決定得了的。呼叫端要能逐一看到每一份的 HRESULT ——
// 包成一支只回傳第一個錯誤的話,「三份都失敗」與「兩份成功一份失敗」
// 在報表上長得一模一樣,而要分辨得再等一輪 CI。
HRESULT SetProfileEnabledForCurrentUser(int index, bool enable);

}  // namespace rimewin

#endif  // RIMEWIN_TSF_REGISTRATION_H_
