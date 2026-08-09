// windows/tsf/trace.h — 瘦 DLL 的落地除錯記錄
//
// ══ 為什麼需要這個檔案 ═════════════════════════════════════════════
//
// 使用者回報「裝好了,但什麼都打不出來,也沒有任何 UI」。而我們手上能問到的
// 全部資訊就是那一句話 —— 因為:
//
//   · TSF 的 DLL 住在**別人的進程**裡(記事本、瀏覽器、Office)。它印到
//     stdout 的東西沒有人看得到,而且那些宿主根本沒有主控台。
//   · OutputDebugString 需要一個偵錯器接著。**使用者手上沒有偵錯器。**
//   · 「DLL 沒有被載入」「載入了但 ActivateEx 沒被呼叫」「ActivateEx 過了但
//     按鍵映不出 keysym」「keysym 有了但連不上服務」——這四種在使用者眼裡
//     長得一模一樣:「不能打字」。而它們要修的地方完全不同。
//
// 所以這裡把最關鍵的幾件事寫進一個**檔案**。它是 rime_ime_setup.exe doctor
// 的資料來源之一,也是 CI 的 windows/verify_tsf.sh 斷言「ActivateEx 真的被
// 呼叫過」的依據 —— 也就是說這個機制本身有人在驗,不會悄悄壞掉。
//
// ══ 規矩 ═══════════════════════════════════════════════════════════
//
// 1. **不可以在按鍵路徑上做 I/O。** 每一顆按鍵寫一行 = 每一顆按鍵一次
//    磁碟寫入,而那發生在宿主的 UI 執行緒上。所以按鍵只記**前幾顆**
//    (見 text_service.cc 的 key_trace_budget_),之後靜音。
// 2. **只用 kernel32。** 這支 DLL 的相依有一份很短的允許清單
//    (windows/check_binaries.sh)。CreateFileW / WriteFile / GetLocalTime /
//    GetEnvironmentVariableW / SRW lock 全在 kernel32 裡,一個新相依都不加。
// 3. **不配置堆積、不丟例外。** 格式化走堆疊上的固定緩衝區。
//    這段程式碼會在 DllMain 的載入器鎖底下跑一次(見下),那裡能做的事極少。
// 4. **失敗一律安靜。** 寫不進去(唯讀的使用者、AppContainer 的宿主)不可以
//    影響輸入法本身 —— 診斷壞掉不該讓產品跟著壞掉。
//
// ══ 為什麼寫在 %LOCALAPPDATA% 而不是 %APPDATA% ═════════════════════
//
// %APPDATA%(Roaming)是**使用者的資料** —— 詞典、設定,跟著人走。
// 記錄檔是這台機器上的診斷資料,漫遊它只是讓網域使用者的設定檔變重。
// 而且它每次登入都可能被寫,放進漫遊區等於每次登出都同步一份垃圾。
//
// ══ 隱私 ═══════════════════════════════════════════════════════════
//
// 記錄裡有宿主程式的**檔名**(notepad.exe)。那是診斷「在哪些程式裡不能用」
// 的核心資訊,但也是使用者機器上的個人資料 —— 所以:
//   · 只記檔名,不記完整路徑、不記視窗標題、**絕對不記按鍵內容或候選字**;
//   · 檔案只在本機,沒有任何東西會把它送出去(windows/audit_offline_win.sh
//     在原始碼層面守著「沒有任何檔案碰網路 API」);
//   · 使用者可以用環境變數 RIME_TSF_TRACE=0 整個關掉。
#ifndef RIMEWIN_TSF_TRACE_H_
#define RIMEWIN_TSF_TRACE_H_

#include <windows.h>

namespace rimewin {

// 記錄檔的完整路徑。緩衝區至少要 MAX_PATH。回傳 false = 記錄是關掉的。
//
// 順序:
//   1. 環境變數 RIME_TSF_TRACE
//        "0" / "off" / "OFF"  → 關掉
//        其餘                 → 當成檔案的完整路徑(CI 用這條)
//   2. %LOCALAPPDATA%\RimeQuad\diagnostics\tsf.log
//   3. 兩者都拿不到 → 關掉
bool TraceFilePath(wchar_t* out, size_t out_len);

// 記錄一行。fmt 是**窄字元 UTF-8**(與服務進程的輸出同一個約定,
// 見 service/main.cc 檔頭:同一個檔案裡混用寬窄字元 I/O 是未定義行為)。
//
// 呼叫者不必先做任何初始化;第一次呼叫時會自己備妥。
void Trace(const char* fmt, ...);

// 這支 DLL 是在哪一個宿主裡。只回檔名(notepad.exe),不回路徑。
// 記錄的每一行都會帶著它 —— 「在 Edge 裡不行、在記事本裡可以」是
// 這一類問題裡最有價值的一句話。
const char* TraceHostName();

}  // namespace rimewin

#endif  // RIMEWIN_TSF_TRACE_H_
