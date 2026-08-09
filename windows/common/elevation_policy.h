// windows/common/elevation_policy.h — 「這個宿主可不可以啟動服務進程」
//
// ══ 這一格為什麼要單獨存在 ═════════════════════════════════════════
//
// 瘦 DLL 住在**每一個**宿主進程裡,而使用者切到本輸入法時,是那個 DLL
// 負責把 rime_service.exe 叫起來 —— 系統匣圖示、設定視窗、候選窗、引擎
// 全部在那支服務裡。也就是說這一格答錯的後果不是「某個功能怪怪的」,
// 而是**整個輸入法完全不存在,而且沒有任何錯誤訊息**。
//
// 舊的判斷只有一行:`if (IsProcessElevated()) return false;`
// 理由是對的 —— 從提權的進程 CreateProcess 出來的服務會繼承提權的權杖,
// 接著用系統管理員的身分去讀寫使用者的詞庫,檔案的擁有者從此換成不對的人,
// 一般權限的那一支服務再也寫不進去。症狀是「用過一次系統管理員的程式之後,
// 輸入法就再也記不住東西」。
//
// 但那一行**涵蓋過頭了**,而且過頭的那一塊不是邊角:
//
//   使用者用內建的 Administrator 帳號登入(SID 尾巴是 -500),
//   或是把 UAC 關掉了。那種工作階段裡**每一個進程都是提權的** ——
//   explorer、瀏覽器、記事本、全部。於是這條規則的效果是
//   「服務永遠不會被啟動」,而使用者看到的是:沒有系統匣圖示、
//   沒有設定視窗、打不出字。三個症狀,一個原因,零個錯誤訊息。
//   (2026-08 的實測回報就是這一種;手動跑一次 rime_service.exe,
//    三個症狀同時消失。)
//
// ══ 正確的判準 ═════════════════════════════════════════════════════
//
// 要拒絕的不是「提權」,是**權限落差**:
//
//   我們即將產生的那支服務,會不會跟這個工作階段裡其他以同一個使用者
//   身分執行的進程**不一樣**?不一樣才有東西會被弄壞。
//
// Windows 對這個問題有一個直接的答案,不必去猜:`TokenElevationType`。
//
//   TokenElevationTypeFull    這是「分裂權杖」提權的那一半 ——
//                             **同一個使用者在這個工作階段裡另有一份受限的
//                             權杖**,而那一份才是他平常用的身分,
//                             他的詞庫檔案屬於那一份。→ 拒絕。
//   TokenElevationTypeLimited 受限的那一半(此時 TokenIsElevated 是 false)。
//                             → 允許,這是絕大多數使用者的日常狀態。
//   TokenElevationTypeDefault **這個身分沒有第二份權杖。**
//                             內建 Administrator、UAC 關掉、或根本不是
//                             系統管理員,都落在這裡。沒有第二份權杖,
//                             就沒有「另一個身分的檔案被我弄壞」這回事:
//                             服務與宿主的使用者 SID 相同、%APPDATA% 相同、
//                             完整性等級也相同。→ 允許。
//
// ⚠ 為什麼不去讀 EnableLUA(UAC 開關)或判斷 SID 尾巴是不是 -500:
//   那兩個是**成因**,而 TokenElevationType 是**結果**,而且結果才是我們
//   真正要問的問題(「有沒有另一份權杖」)。成因不只那兩種,清單會漏;
//   結果只有一個,而且是核心自己算的。那兩個值仍然值得**印出來**
//   (doctor 會印),但不拿來做決定。
//
// ⚠ 另一道必要的閘:**機器帳號**。
//   TokenElevationTypeDefault + 提權,除了「整個工作階段都提權」之外,
//   還有一種:進程根本不是以真人身分在跑(LocalSystem、LOCAL SERVICE、
//   NETWORK SERVICE)。TSF 的 DLL 真的會被載進那種進程 —— 登入畫面的
//   LogonUI.exe 就是以 SYSTEM 執行而且會載入輸入法。從那裡啟動服務的話,
//   那支服務的 %APPDATA% 會是 C:\Windows\system32\config\systemprofile\…,
//   而且它建出來的具名管道名字裡帶的是 S-1-5-18 —— 真正的使用者永遠
//   連不上它。所以機器帳號一律拒絕,而且要**先於**提權判斷。
//
// ══ 為什麼不「從提權的進程啟動一支非提權的服務」═══════════════════
//
// 那的確是最乾淨的解,而且對 kSplitToken 那一種是做得到的
// (TokenLinkedToken 拿到受限的那一份,再 CreateProcessWithTokenW)。
// 但它**對這次真正壞掉的那一種完全無效**:kWholeSession 的定義就是
// 「沒有連結權杖」,所以拿不到可以降權的目標;而 shell 的 ShellExecute
// 那條路要借 explorer 的身分,偏偏在那種工作階段裡 explorer 自己也是提權的。
// 也就是說:**唯一需要降權的場合,正是降權辦不到的場合。**
//
// 而 kSplitToken 那一種不做降權的代價很小:那種工作階段裡本來就有非提權的
// 宿主(這正是「分裂權杖」的意思),服務會由那些宿主之一啟動起來。
// 所以那裡的「拒絕」只是延後,不是永遠。
//
// ══ 拒絕必須被看見 ═════════════════════════════════════════════════
//
// 一個刻意的拒絕不該長得跟壞掉一樣。這一格回傳的判定會出現在三個地方:
//   · 語言列上那顆按鈕的文字與工具提示(tsf/lang_bar.cc)
//   · rime_ime_setup.exe doctor 的第 4 節(setup/doctor.cc)
//   · 瘦 DLL 的除錯記錄(tsf/trace.cc)
// 只寫進第三個是不夠的 —— 使用者不知道那個檔案存在。
//
// ⚠ 這個檔案刻意**不 include windows.h**:判斷邏輯要能在開發用的 Ubuntu 上
//   用 windows/run_logic_tests.sh 直接跑(見 tests/test_elevation_policy.cc)。
//   權杖查詢那一段在 windows/winshared/winutil.cc,它只負責把事實填進來。
#ifndef RIMEWIN_COMMON_ELEVATION_POLICY_H_
#define RIMEWIN_COMMON_ELEVATION_POLICY_H_

namespace rimewin {

// Windows 的 TOKEN_ELEVATION_TYPE 直譯。名字刻意講「有沒有連結權杖」而不是
// 「Full / Limited」—— 我們要的判斷是前者,而 Full/Limited 這兩個字讓人
// 以為問題是「權限高不高」。
enum class TokenSplit {
  kUnknown = 0,        // 問不到(GetTokenInformation 失敗)
  kNoLinkedToken,      // TokenElevationTypeDefault:這個身分只有這一份權杖
  kFullWithLinked,     // TokenElevationTypeFull:提權的那一份,另有一份受限的
  kLimitedWithLinked,  // TokenElevationTypeLimited:受限的那一份,另有一份提權的
};

// 這個宿主進程處在哪一種局面。
enum class HostElevation {
  kUnknown = 0,     // 問不出來 → 保守拒絕
  kNormal,          // 沒有提權的一般進程 → 可以啟動
  kSplitToken,      // 提權,而且同一個使用者另有一份受限權杖 → 拒絕
  kWholeSession,    // 提權,但這個身分沒有第二份權杖 → 可以啟動
  kServiceAccount,  // 機器帳號(SYSTEM / LOCAL SERVICE / NETWORK SERVICE) → 拒絕
};

// 由**事實**算出判定。這裡沒有任何 Windows API,所以測得到。
//
//   token_query_ok      權杖查詢有沒有全部成功。false → kUnknown(保守拒絕)。
//   is_elevated         TOKEN_ELEVATION.TokenIsElevated
//   split               TokenElevationType
//   is_service_account  使用者 SID 是不是 S-1-5-18 / 19 / 20
HostElevation ClassifyHostElevation(bool token_query_ok, bool is_elevated,
                                    TokenSplit split, bool is_service_account);

// 這個局面底下,可不可以從這個進程 CreateProcess 出一支服務。
//
// ⚠ 只有 kNormal 與 kWholeSession 是 true。加新的列舉值時預設要是 false ——
//   「忘了處理」的後果應該是不啟動(使用者看得到、查得出來),
//   而不是用錯的身分啟動(檔案擁有者被換掉,而且沒有人會發現)。
bool MayStartUserService(HostElevation e);

// 穩定的 ASCII 標籤。給記錄檔與驗證腳本用 —— 腳本不該去比對中文句子,
// 那種比對會在措辭改一個字的時候安靜地失效。
const char* HostElevationTag(HostElevation e);

// 給人看的一句話(UTF-8)。doctor 與除錯記錄用。
const char* HostElevationZh(HostElevation e);

// 拒絕啟動時,語言列的工具提示上要寫什麼(UTF-16)。
// 可以啟動的兩種回傳 nullptr —— 那時按鈕顯示正常的樣子。
const wchar_t* HostElevationTooltipW(HostElevation e);

}  // namespace rimewin

#endif  // RIMEWIN_COMMON_ELEVATION_POLICY_H_
