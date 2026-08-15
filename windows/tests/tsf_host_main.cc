// windows/tests/tsf_host_main.cc — rime_tsf_host.exe
//
// ══ 這支程式是什麼 ═════════════════════════════════════════════════
//
// 一個**假的文字編輯器**。它做的事與記事本對 TSF 做的事一樣:
//
//   建 ITfThreadMgr → Activate → 建一份文件(ITextStoreACP)→ 給它焦點
//   → 啟用**我們的**語言設定檔 → 用 ITfKeystrokeMgr 送按鍵進去
//   → 看文件裡最後長出什麼字
//
// 也就是說:**它逼系統走完真正的那條路** —— 讀登錄檔、CoCreateInstance、
// 把 rime_tsf.dll 載入這個進程、呼叫 ActivateEx、掛 key event sink、
// 把按鍵交給我們、我們開 edit session、組字、上屏。
//
// ══ 為什麼要有它 ═══════════════════════════════════════════════════
//
// 上一輪 windows/README.md 把這一整段列在「只有人在真 Windows 上跑才驗得到」:
//
//     切到這個輸入法之後 ActivateEx 有沒有被呼叫、
//     在記事本裡打不打得出字、組字視窗會不會出現
//
// 而使用者實際回報的正是這一段壞掉。CI 驗到的「經由具名管道打出你好」
// **繞過了 TSF**:它證明的是「引擎 + 資料 + IPC」是好的,
// 完全沒有經過 DLL 被系統載入、ActivateEx 被呼叫、按鍵經 TSF 進來這條路。
//
// 這支程式把那條路補上。它跑得起來的話,「輸入法能不能用」就不再是
// 一句只能靠使用者回報的話。
//
// ══ 誠實說明它**不是**什麼 ═════════════════════════════════════════
//
//   · 它不是記事本。真的宿主會有自己的版面、DPI、視窗、UI 執行緒模型,
//     而 GetTextExt 在很多宿主上會失敗 —— 這裡的實作一定成功。
//     所以「候選窗位置對不對」這一格仍然驗不到。
//   · 它證明不了語言列按鈕與系統匣圖示長什麼樣。
//   · runner 是非互動的工作階段,CTF 的行為與使用者桌面上不保證一樣。
//     所以**每一步的 HRESULT 都印出來** —— 走不通的時候要看得出卡在哪,
//     而不是得到一句「失敗」。
//
// ══ 用法 ═══════════════════════════════════════════════════════════
//
//   rime_tsf_host.exe [--langid 0x0404] [--keys nihao1] [--expect 你好]
//                     [--require-activate] [--require-eaten]
//                     [--trace <檔案>] [--wait-ms 3000]
//
//   ── 按鍵矩陣模式(windows/verify_input_matrix.sh 用的)──────────
//
//   --seq "n i h a o {BS} 1"   逐鍵送。`{名字}` 是功能鍵,見 kNamedKeys。
//   --pretext "已經有的字"      先把文件內容設成這樣(游標在最後)。
//                              用來驗「沒有組字時的退格」—— 那需要文件裡
//                              先有東西可以刪。
//   --expect-doc "已經有的"     跑完之後文件必須完全等於這個。
//
//   ⚠ **宿主會做預設處理。** KeyDown 回報沒吃掉的鍵,這支假宿主會像一個
//     真的編輯框那樣自己處理(插入字元、退格刪一個、方向鍵移游標)。
//     少了這一段,「放行」與「掉進黑洞」在文件內容上長得一模一樣 ——
//     而那正是使用者回報的「可以打字,不能刪除」的形狀。
//
//   ── Ctrl+空白鍵(中英切換)────────────────────────────────────
//
//   --check-preserved-key        問 TSF「這顆鍵註冊了沒」(不呼叫產品)
//
//   ── 那一橫的「在場」訊號(工單 #82 的另一半)────────────────
//
//   --watch-presence   在 ActivateProfile **之前**自己把那條具名管道
//                      開起來,然後在 ActivateEx 之後、**送出第一顆
//                      按鍵之前**數服務端有幾條連線。0 條 = 失敗。
//                      見下面 PresenceWatch 的檔頭。
//   --press-preserved-key        **真的按下去**,並斷言 OnPreservedKey 有跑
//   --toggle-mid-compose "nihao" 先打這幾個字讓引擎進入組字,再按那顆鍵
//   --expect-toggle-doc "nihao"  按完之後文件必須完全等於這個
//
//   ⚠ 註冊與按下去是**兩件事**。只問註冊的話,把 OnPreservedKey 改成
//     比對完 GUID 就 `return S_OK` —— CI 三個 job 全綠,而中英切換整條死掉。
//     見下面 PressPreservedKey() 的說明。
//
//   rime_tsf_host.exe --hold-dll <dll路徑> --hold-ms <毫秒>
//                     只 LoadLibrary 那個 DLL 然後睡著,不碰 TSF。
//                     用來**重現使用者解除安裝時撞到的那個狀態**:
//                     還有程式握著 rime_tsf.dll,所以檔案刪不掉。
//                     見 windows/verify_installer.sh §11。
//
//   ── 兩階段模式:升級發生在**這個進程還活著**的時候 ─────────────
//
//   --phase2-ready-file <檔>   第一階段打完字之後寫出這個檔(對外的訊號:
//                              「我已經載入舊版 DLL 並且打得出字了,
//                                可以開始裝新版」)
//   --phase2-go-file <檔>      然後等這個檔出現(對內的訊號:「裝好了」),
//                              再打一次同樣的字並斷言同樣的結果
//   --phase2-timeout-ms <毫秒> 等 go 檔的上限(預設 600000)
//   --phase2-relink-ms <毫秒>  等「重新連上服務」的上限(預設 240000)
//   --phase2-repeat <次數>     第二階段那六顆鍵連跑幾趟(預設 5)——
//                              **任何一趟打錯就紅**。跑一趟的結果不管綠紅
//                              都不構成證據,理由見第二階段那個迴圈上面。
//
//   ⚠ 這個模式驗的是使用者升級之後**真實存在**的狀態:
//     已經開著的程式(檔案總管、瀏覽器)手上是**舊的 DLL 映像**,
//     而服務進程已經換成新的。兩者談不攏的話,症狀是
//     「有些程式能打字、有些不能」——而使用者完全無法理解為什麼。
//     那比全部壞掉更難查,所以它必須有一道關卡。
//     見 windows/verify_installer.sh §13。
//
//   ── ⚠ 兩條量法,以及它們的地位 ──────────────────────────────
//
//   這支程式有兩條完全獨立的量法,而它們的**地位不一樣**:
//
//     線路(判準)  管道 → 握手 → session,一趟走完。不經過 TSF,
//                  所以前景搶不搶得到都答得出來。輸出:
//                    PHASE2_LINK_PIPE / _HANDSHAKE / _SESSION / _STAGE
//                  三格各自獨立會紅,紅字只指向它自己那一件事。
//                  見 ProbeServiceLink 的檔頭(包含它證明不了什麼)。
//     按鍵(加分)  送 nihao1、比對「你好」。它要經過 TSF,而 TSF 只把
//                  按鍵交給**有執行緒焦點**的那一份文字服務 ——
//                  焦點拿不到時它量到的是「沒有答案」,不是「壞掉」。
//                  輸出:KEYPATH_MEASURED / KEYPATH_FAILS / EXCUSED。
//
//   ⚠ 赦免是**一筆一筆**判的,判準是「記下這一筆的當下有沒有焦點」——
//     那是呼叫點才知道的事實,所以 FailKeyPath() 把它當參數收。
//     記帳本身在 common/keypath_ledger.h(那裡的檔頭寫著上一版怎麼漏的),
//     由 tests/test_keypath_ledger.cc 在 Ubuntu 上真的跑。
//   ⚠ 沒有焦點時記下的那些、**而且**線路那三句話全部成立時,才不計入
//     結束碼。有焦點時記下的一律計入 —— 那些是真的量到了。
//     線路那三句話沒走通就一律計入:一個什麼都沒量到的段落綠起來,
//     比它紅起來危險得多。
//   ⚠ KEYPATH_MEASURED 問的是「整趟裡有沒有任何一段量到」,
//     PHASE2_KEYPATH_MEASURED 問的是「第二階段那一段量到沒有」。
//     兩者範圍不同,但不可以打架:有一段量到了,收尾就不准說整趟沒量到。
//
// 結束碼 0 = 要求的每一項都成立。
#include <msctf.h>
#include <windows.h>
// WIN32_LEAN_AND_MEAN 之下 windows.h 不帶 CommandLineToArgvW。
#include <shellapi.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "../tsf/guids.h"
#include "../winshared/winutil.h"
// ⚠ 線路格式與 ABI 常數。這一支**不連結 rime_tsf**(見 CMakeLists 的說明),
//   所以這裡拿到的是與瘦 DLL 同一份原始碼編出來的第二個副本,不是 DLL 手上
//   那一份的程式碼。差別很重要,ProbeServiceLink 的檔頭把它寫清楚了。
#include "keypath_ledger.h"
#include "protocol.h"
#include "rime_shell.h"

using namespace rimewin;

namespace {

int g_fails = 0;

void Say(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stdout, fmt, ap);
  va_end(ap);
  std::fflush(stdout);
}

void Step(const char* what, HRESULT hr) {
  Say("  %-46s hr=0x%08lX %s\n", what, static_cast<unsigned long>(hr),
      SUCCEEDED(hr) ? "OK" : "**失敗**");
}

void Fail(const char* fmt, ...) {
  ++g_fails;
  std::fprintf(stdout, "  !! ");
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stdout, fmt, ap);
  va_end(ap);
  std::fprintf(stdout, "\n");
  std::fflush(stdout);
}

// ── 「按鍵那一段」的失敗記在另一個帳上 ───────────────────────────────
//
// ══ 為什麼要分兩個帳 ═══════════════════════════════════════════════
//
// 送按鍵這條路要經過 TSF,而 TSF 只把按鍵交給**擁有執行緒焦點**的那一份
// 文字服務。焦點的前提是前景,而前景在 CI 的非互動工作階段裡不保證拿得到
// (見 ForceForeground 的檔頭:實測佔著前景的是 runner 自己的 agent 視窗)。
//
// 焦點拿不到的時候,底下每一條按鍵斷言量到的都是同一個東西:
// 「KeyDown 回 S_OK、pfEaten=FALSE」—— 那不是產品的答案,是**沒有答案**。
// 把它算成產品失敗,等於每一次都紅在一個沒有量到的格子上,而紅字指的方向
// (key sink / 佈局 / 連線)全部是好的東西。
//
// ⚠ **這不是放寬斷言。** 這些失敗仍然逐條印出來、仍然預設計入結束碼;
//   只有在同時滿足下面兩件事時才被赦免:
//     1. **記下那一筆的當下**真的拿不到執行緒焦點,而且
//     2. **另一條不需要前景的量法整條走通了**(第二階段的三句話:
//        管道 / 握手 / session —— 見 ProbeServiceLink)。
//   少了第 2 條就沒有任何東西被量到,那時它必須紅 ——
//   一個什麼都沒驗到的段落綠起來,比它紅起來危險得多。
//
// ⚠ **第 1 條問的是那一筆,不是這一趟。** 上一版用一個永不清除的黏滯
//   全域旗標(g_keypath_blind)分帳,而它放過了一條真的假綠:第一階段
//   搶不到前景把旗標設起來,第二階段 ForceForeground() 搶贏了、真的送了
//   按鍵、真的量到「打出來的是 nihao 而不是你好」—— 那一筆仍然被記進
//   「瞎的」那一格,於是被赦免,rc=0。同一份 log 裡宿主自己印著
//   PHASE2_DOC="nihao"。(而新加的 AttachThreadInput 剛好把這個組合從
//   不可能變成可能:升級那幾分鐘裡前景擁有者本來就會變。)
//
//   所以現在 FailKeyPath() 把「這一筆瞎不瞎」當**參數**收 ——
//   由編譯器逼每一個呼叫點回答,而不是從一個跨階段的全域變數推。
//   記帳規則本身在 common/keypath_ledger.h(在 Ubuntu 上編得起來、
//   由 tests/test_keypath_ledger.cc 真的跑);這裡只負責填數字。
rimewin::KeypathLedger g_keypath;
bool g_phase2_ran = false;

// blind = **記下這一筆的當下**執行緒焦點在不在我們身上。
// ⚠ 不要傳一個從全域狀態算出來的值進來 —— 那等於把上面那條假綠放回去。
void FailKeyPath(bool blind, const char* fmt, ...) {
  g_keypath.NoteFail(blind);
  std::fprintf(stdout, "  !! [按鍵路徑%s] ", blind ? "・沒有焦點" : "");
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stdout, fmt, ap);
  va_end(ap);
  std::fprintf(stdout, "\n");
  std::fflush(stdout);
}

void Ok(const char* fmt, ...) {
  std::fprintf(stdout, "  ✓ ");
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stdout, fmt, ap);
  va_end(ap);
  std::fprintf(stdout, "\n");
  std::fflush(stdout);
}

// 訊息幫浦。
//
// ⚠ 不可以省。TSF 在 STA 裡有一部分工作是靠視窗訊息推動的
//   (語言列、profile 通知)。不抽訊息的話,那些事件永遠不會送達,
//   而症狀是「某些 sink 從來沒被呼叫」—— 看起來像產品的錯。
void Pump(DWORD ms) {
  const DWORD end = ::GetTickCount() + ms;
  for (;;) {
    MSG msg;
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
    if (::GetTickCount() >= end) return;
    ::Sleep(10);
  }
}

// ── 前景視窗**是誰** ─────────────────────────────────────────────────
//
// ══ 為什麼非有不可 ═══════════════════════════════════════════════════
//
// 在這之前,整條工具鏈印得出來的只有一個數字:
//
//     前景視窗 = 0000000000010202(我們的是 00000000000402BC) —— 沒搶到
//
// 而那個數字**回答不了任何一個會改變修法的問題**。CI run 31896143629 的
// §13 就卡在這裡:0x10202 這個 handle 從 §12 之後就再也沒放開,而
//   · 如果它是**別人的活視窗**(runner 上的另一支程式),
//     那要修的是測試台的隔離;
//   · 如果它是**已經被我們 taskkill //F 掉的設定視窗留下的殘留 handle**,
//     那要修的是 §12 的收尾 —— 而那是我們自己弄髒的;
//   · 如果它是**我們自己還活著的視窗**,那才是產品的迴歸。
// 三條的修法完全不一樣,而上一輪只能用推論去猜(§13 那兩支服務都是
// --no-ui、連設定視窗都不建,所以至少排得掉第三條)。
//
// IsWindow() 一個布林值就分得開前兩條:殘留的 handle 回 FALSE。
// 加上 pid / 執行檔 / 類別名 / 標題,「前景是誰」就從推論變成事實,
// 一次 CI 就定案。
//
// ⚠ 這個函式**只印,不判**。它拿到的每一格都可能失敗(視窗屬於別的
//   工作階段、我們沒有 PROCESS_QUERY_LIMITED_INFORMATION),
//   而失敗本身也是資訊 —— 所以取不到就印「?」,不要靜靜跳過。
void SayForegroundIdentity(const char* tag) {
  const HWND fg = ::GetForegroundWindow();
  if (!fg) {
    // ⚠ 這一格不是雜訊:「目前沒有前景視窗」正是 SetForegroundWindow
    //   唯一還會放行給我們的那條件。它成立的時候後面必定搶得到。
    Say("  [%s] 前景視窗 = (沒有前景視窗 —— GetForegroundWindow 回 NULL)\n",
        tag);
    return;
  }
  const BOOL alive = ::IsWindow(fg);
  DWORD pid = 0;
  const DWORD tid = ::GetWindowThreadProcessId(fg, &pid);

  wchar_t cls[256];
  if (!::GetClassNameW(fg, cls, 256)) wcscpy_s(cls, 256, L"?");
  wchar_t title[512];
  if (::GetWindowTextW(fg, title, 512) <= 0) wcscpy_s(title, 512, L"");

  // ⚠ 取不到時的說明用**窄**字串:寬字串字面值裡不可以有中日韓字元
  //   (check_ui_spec.sh 的 W7 —— 使用者看得到的字只能住在 ui_strings.cc,
  //   而 L"…" 一旦有中文就分不出「診斷輸出」與「介面文案」)。
  //   Say() 本來就是窄的,所以這裡什麼都沒失去。
  std::wstring exe;
  const char* exe_note = "?(問不到 pid)";
  if (pid) {
    exe_note = "?(打不開這個進程,多半是權限不足)";
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
      wchar_t path[32768];
      DWORD n = 32768;
      if (::QueryFullProcessImageNameW(h, 0, path, &n))
        exe.assign(path, n);
      else
        exe_note = "?(QueryFullProcessImageName 失敗)";
      ::CloseHandle(h);
    }
  }
  const std::string exe_shown =
      exe.empty() ? std::string(exe_note) : WideToUtf8(exe);

  Say("  [%s] 前景視窗 = %p IsWindow=%d pid=%lu tid=%lu\n"
      "        exe=%s\n"
      "        cls=%s 標題=\"%s\"\n",
      tag, static_cast<void*>(fg), alive ? 1 : 0,
      static_cast<unsigned long>(pid), static_cast<unsigned long>(tid),
      exe_shown.c_str(), WideToUtf8(cls).c_str(), WideToUtf8(title).c_str());
  if (!alive)
    Say("        ⚠ IsWindow=0 —— 這是一個**已經不存在的視窗**留下的殘留\n"
        "          前景控制代碼。前景這個欄位不是空的,所以\n"
        "          SetForegroundWindow 的「目前沒有前景視窗」那條放行路\n"
        "          不成立;而它的擁有者已經死了,誰也還不了。\n"
        "          → 要查的是**上一節怎麼收尾的**(有沒有在視窗正是前景時\n"
        "            硬殺掉擁有者),不是輸入法。\n");
}

// 一行版的「誰佔著前景」。給那些**每一次都要印**的句子用 ——
// 一個必印的結論不可以要求讀者往回翻五十行才知道它在講誰。
std::string ForegroundOwnerBrief() {
  const HWND fg = ::GetForegroundWindow();
  if (!fg) return std::string("沒有前景視窗(GetForegroundWindow 回 NULL)");
  DWORD pid = 0;
  ::GetWindowThreadProcessId(fg, &pid);
  wchar_t cls[256];
  if (!::GetClassNameW(fg, cls, 256)) wcscpy_s(cls, 256, L"?");
  std::wstring exe;
  if (pid) {
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
      wchar_t path[32768];
      DWORD n = 32768;
      if (::QueryFullProcessImageNameW(h, 0, path, &n)) {
        exe.assign(path, n);
        const size_t slash = exe.find_last_of(L"\\/");
        if (slash != std::wstring::npos) exe = exe.substr(slash + 1);
      }
      ::CloseHandle(h);
    }
  }
  char buf[768];
  std::snprintf(buf, sizeof(buf), "hwnd=%p IsWindow=%d pid=%lu exe=%s cls=%s",
                static_cast<void*>(fg), ::IsWindow(fg) ? 1 : 0,
                static_cast<unsigned long>(pid),
                exe.empty() ? "?" : WideToUtf8(exe).c_str(),
                WideToUtf8(cls).c_str());
  return std::string(buf);
}

// ── 甲:把前景**搶回來**,而且說出用了什麼手段 ───────────────────────
//
// ══ 為什麼一句 SetForegroundWindow 不夠 ═══════════════════════════════
//
// SetForegroundWindow 只在少數幾種情況下成立,而這台 runner 上我們**一種
// 都不符合**:呼叫端不是前景進程、不是被前景進程直接啟動的、收不到任何
// 輸入事件(非互動的工作階段,沒有滑鼠也沒有鍵盤)。
//
// 那為什麼以前搶得到?因為那時「前景視窗」這個欄位是**空的** ——
// 那是最後一條放行條件,而它成立時誰呼叫誰就拿得到。
//
// ⚠ 這一段是查證過的,不是推論:
//   · 綠的那一趟(/tmp/log0.txt,§12b 那時還沒有「設定視窗就是前景視窗」
//     這條斷言)裡,§13a 的宿主印的是「前景視窗 = …09012C(我們的是
//     …09012C)—— 搶到了」、PHASE2_THREAD_FOCUS=1。
//   · 紅的那兩趟(/tmp/log.txt、/tmp/log2.txt,§12b/§12c 已經會斷言
//     「設定視窗就是前景視窗」「前景權轉讓成立」)裡,§13a 之後前景固定是
//     同一個視窗:cls=CASCADIA_HOSTING_WINDOW_CLASS、
//     exe=…\WindowsTerminal.exe、標題是 runner 自己那支代理程式的路徑,
//     IsWindow=1(**活的**,不是殘留 handle)。
//   也就是說:§12 第一次讓一個真的視窗當上前景之後,那個欄位就再也不是
//   空的了,而系統在我們的視窗收掉時把它交給了那支代理程式的視窗。
//
// ══ 那要怎麼辦 ═══════════════════════════════════════════════════════
//
// 標準做法是 AttachThreadInput:把我們這條執行緒接到目前前景那條執行緒的
// 輸入佇列上。接上之後兩條執行緒共用同一份前景狀態,於是我們就「是」前景
// 進程,SetForegroundWindow 那一關就過得去。做完立刻 detach ——
// 接著不放等於把我們的訊息迴圈與別人的綁在一起。
//
// ⚠ **這是測試台的手段,不是產品的手段。** 它只能住在這個檔案裡:
//   一個輸入法對使用者正在用的視窗做 AttachThreadInput 是不可接受的。
//   windows/service/ 與 windows/tsf/ 底下一行都不會有這種東西。
//
// ⚠ 它**仍然可能失敗**(前景屬於更高完整性等級的進程、或系統根本不讓)。
//   所以每一步用了什麼、成功還是失敗都印出來,而且真正的相容性判準
//   不靠它 —— 見 ProbeServiceLink。
//
// 回傳 true = 收工時 hwnd 真的是前景視窗。
bool ForceForeground(HWND hwnd, const char* tag) {
  if (!hwnd) return false;
  ::ShowWindow(hwnd, SW_SHOWNORMAL);
  ::UpdateWindow(hwnd);

  // 手段 0:直接要。前景欄位是空的時候這一條就夠了(以前每一趟都是這樣)。
  ::SetForegroundWindow(hwnd);
  ::SetActiveWindow(hwnd);
  ::SetFocus(hwnd);
  if (::GetForegroundWindow() == hwnd) {
    Say("  [%s] FOREGROUND_GRAB=直接要到(SetForegroundWindow)\n", tag);
    return true;
  }

  // 手段 1:把「前景鎖逾時」歸零。系統會在最後一次輸入事件之後的一段時間
  //         內拒絕換前景;非互動工作階段裡永遠沒有輸入事件,所以這個計時器
  //         可能從開機起就沒被重置過。
  {
    DWORD old_timeout = 0;
    const BOOL got = ::SystemParametersInfoW(SPI_GETFOREGROUNDLOCKTIMEOUT, 0,
                                             &old_timeout, 0);
    // pvParam 就是要寫進去的值,而我們要寫的是 0 —— 空指標剛好就是 0。
    const BOOL set = ::SystemParametersInfoW(SPI_SETFOREGROUNDLOCKTIMEOUT, 0,
                                             nullptr, SPIF_SENDCHANGE);
    Say("  [%s] FOREGROUND_TRY=前景鎖逾時歸零(讀到 %lu ok=%d,寫入 ok=%d)\n",
        tag, static_cast<unsigned long>(old_timeout), got ? 1 : 0,
        set ? 1 : 0);
    ::SetForegroundWindow(hwnd);
    if (::GetForegroundWindow() == hwnd) {
      Say("  [%s] FOREGROUND_GRAB=前景鎖逾時歸零之後要到\n", tag);
      return true;
    }
  }

  // 手段 2:AttachThreadInput —— 這一條才是主力。
  {
    const HWND fg = ::GetForegroundWindow();
    const DWORD me = ::GetCurrentThreadId();
    DWORD fg_pid = 0;
    const DWORD fg_tid = fg ? ::GetWindowThreadProcessId(fg, &fg_pid) : 0;
    if (!fg || fg_tid == 0 || fg_tid == me) {
      Say("  [%s] FOREGROUND_TRY=AttachThreadInput 不適用"
          "(前景那條執行緒 tid=%lu,我們是 %lu)\n",
          tag, static_cast<unsigned long>(fg_tid),
          static_cast<unsigned long>(me));
    } else {
      const BOOL attached = ::AttachThreadInput(me, fg_tid, TRUE);
      Say("  [%s] FOREGROUND_TRY=AttachThreadInput(→ tid=%lu pid=%lu)ok=%d\n",
          tag, static_cast<unsigned long>(fg_tid),
          static_cast<unsigned long>(fg_pid), attached ? 1 : 0);
      // ⚠ 接上之後要**整組**做:只呼叫 SetForegroundWindow 在某些版本上
      //   會只讓工作列按鈕閃一下。TOPMOST 一上一下是為了把 Z 序也帶上來,
      //   否則視窗會是前景卻被別人蓋住,而 PrintWindow 之類的東西會拍到黑框。
      ::AllowSetForegroundWindow(ASFW_ANY);
      ::BringWindowToTop(hwnd);
      ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
      ::SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
      ::SetForegroundWindow(hwnd);
      ::SetActiveWindow(hwnd);
      ::SetFocus(hwnd);
      const bool won = ::GetForegroundWindow() == hwnd;
      if (attached) ::AttachThreadInput(me, fg_tid, FALSE);
      if (won) {
        Say("  [%s] FOREGROUND_GRAB=AttachThreadInput 之後要到\n", tag);
        return true;
      }
    }
  }

  // 手段 3:SwitchToThisWindow。它在 SDK 裡被標成「不要用」,而這裡不是
  //         產品程式碼 —— 用 GetProcAddress 拿,免得 mingw 的標頭有沒有
  //         宣告它變成語法檢查的問題。
  {
    typedef void(WINAPI * SwitchToThisWindowFn)(HWND, BOOL);
    HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
    SwitchToThisWindowFn fn =
        u32 ? reinterpret_cast<SwitchToThisWindowFn>(reinterpret_cast<void*>(
                  ::GetProcAddress(u32, "SwitchToThisWindow")))
            : nullptr;
    Say("  [%s] FOREGROUND_TRY=SwitchToThisWindow(找得到=%d)\n", tag,
        fn ? 1 : 0);
    if (fn) {
      fn(hwnd, TRUE);
      if (::GetForegroundWindow() == hwnd) {
        Say("  [%s] FOREGROUND_GRAB=SwitchToThisWindow 之後要到\n", tag);
        return true;
      }
    }
  }

  Say("  [%s] FOREGROUND_GRAB=**四種手段都沒要到** —— 佔著的是:%s\n", tag,
      ForegroundOwnerBrief().c_str());
  return false;
}

// ── 服務的管道現在**開著沒有** ────────────────────────────────────────
//
// ══ 為什麼要有第二條量法 ═══════════════════════════════════════════
//
// 第二階段(舊 DLL 映像 + 新服務)問的是「舊的 DLL 連不連得回新的服務」。
// 原本的問法是**送一顆 n 進 TSF,看文字服務吃不吃**,而那條路需要
// 執行緒焦點:沒有焦點時 ITfKeystrokeMgr::KeyDown 回 S_OK / pfEaten=FALSE,
// 那顆鍵根本沒有離開這個進程。於是「管道通不通」被一件與管道無關的事
// (前景權)決定 —— run 31896143629 就是這樣:探了 599 次五分鐘,
// 而同一段時間服務端的記錄是
//   [pipe] 連線 #1 離場 存活=300829ms 握手=1 session=0 按鍵=0
// 連線一直在、握手成立、一顆按鍵都沒送到。結論卻寫成「版本協商壞了」,
// 會把人送去改三個完全好的東西。
//
// 這一格改用**不需要前景**的方式問同一件事:直接去開那條管道。
// 開得起來 = 有一支服務在那個名字上聽著(管道名、SID、協議版本後綴
// 全部對得上,因為 RimePipeName() 是同一份決定處)。
// 開不起來 = 管道那一側真的有問題,而那才值得去查版本協商 / ABI。
//
// ⚠ 它證明得了什麼、證明不了什麼,寫清楚免得有人拿它當「打得出字」:
//     證明得了:服務活著、管道名對得上、連得上去。
//     證明不了:握手的版本協商、開得了 session、打得出字。
//               那三件要靠 rime_probe --connect-only(腳本那一側跑)
//               與真的按鍵(需要前景)。
//
// 回傳:開起來的 handle,失敗時是 INVALID_HANDLE_VALUE。
// err 帶出 GetLastError(成功時是 0)。呼叫端負責 CloseHandle。
HANDLE OpenServicePipe(DWORD* err) {
  const std::wstring name = RimePipeName();
  if (err) *err = 0;
  for (int i = 0; i < 2; ++i) {
    HANDLE h = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                             nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                             nullptr);
    if (h != INVALID_HANDLE_VALUE) return h;
    const DWORD e = ::GetLastError();
    if (err) *err = e;
    // ERROR_PIPE_BUSY = 服務在那裡,只是每一份實例都被佔著。
    // 那**也算連得上** —— 名字對得上、有人在聽,再等一下就有位子。
    if (e != ERROR_PIPE_BUSY) return INVALID_HANDLE_VALUE;
    if (!::WaitNamedPipeW(name.c_str(), 2000)) return INVALID_HANDLE_VALUE;
  }
  return INVALID_HANDLE_VALUE;
}

// 只問「開得起來嗎」。這是三句話裡的第一句。
bool ServicePipeReachable(DWORD* err) {
  HANDLE h = OpenServicePipe(err);
  if (h == INVALID_HANDLE_VALUE) return false;
  ::CloseHandle(h);
  return true;
}

// ── 乙:一趟完整的連線 —— 管道 + 握手 + session ────────────────────────
//
// ══ 這一段回答的是哪一個問題 ═══════════════════════════════════════
//
// §13 要問的是:**使用者升級之後,已經開著、手上還是舊 DLL 映像的程式,
// 還能不能用?** 「能不能用」在線路上的意思是三件事,而且是三件:
//
//   1. 管道連得上嗎    —— 舊那一側算出來的管道名跟新服務聽的那個對得上
//   2. 握手成立嗎      —— 版本協商:線路協議版本(proto)與 rime_shell 的
//                        ABI 版本都談得攏
//   3. 建得出 session 嗎 —— 談攏之後引擎真的給得出一個 session
//
// ⚠ 這三件事的修法完全不同(見 tsf/ipc_client.h 的 ReadyStage 檔頭),
//   而上一版把它們壓成一句「舊的 DLL 連不回新的服務」,底下列的三個查證
//   方向在 run 31896143629 上**全部是好的**。一句蓋住三件事的紅字,
//   會把人送去改沒壞的東西 —— 那比沒有這一格更糟。
//
// ══ 為什麼它不經過 TSF ═════════════════════════════════════════════
//
// 因為「連得回去」與「按得出字」是兩件事,而後者需要執行緒焦點:
// 沒有焦點時 ITfKeystrokeMgr::KeyDown 回 S_OK / pfEaten=FALSE,那顆鍵
// 根本沒有離開這個進程。用按鍵去問連線,等於讓一件與連線無關的事
// (前景權)決定連線的結論。這一段直接走線路,所以前景搶不搶得到都答得出來。
//
// ══ ⚠ 它**不是**什麼 —— 這一段一定要誠實 ═══════════════════════════
//
//   · 它跑在**宿主進程裡**(就是手上握著舊 DLL 映像的這一個進程),
//     用的管道名來自 RimePipeName() —— 與瘦 DLL 同一份決定處。
//   · 但它**不是在執行舊 DLL 自己那份 IPC 程式碼**。那份程式碼進不去:
//     rime_tsf.dll 的匯出表只有四個 COM 進入點(見 tsf/rime_tsf.def),
//     沒有任何辦法從外面叫它「去握一次手」。這裡跑的是同一份原始碼編出來
//     的**第二個副本**(rime_tsf_host.exe 自己連進去的 protocol.cc 與
//     RimePipeName)。想要真的跑到 DLL 裡那一份,唯一的辦法是替它加一個
//     匯出的診斷入口 —— 而那會擴大這支 DLL 對外的表面積,產品端不值得。
//   · 而且這一輪的「新版」是**同一次建置**再裝一次(見 verify_installer.sh
//     §13 的檔頭),所以兩側的 proto / ABI 本來就一定相同。也就是說:
//     這三句話證明得了「線路這一整條在升級之後仍然通」,證明不了
//     「兩個不同版本之間談得攏」——後者由 tests/test_proto_compat.cc
//     與 protocol.h 的版本協商守。
//
//   ⚠ 上面這三點不是免責聲明,是**判準的邊界**。把它寫在這裡,是為了
//     下一個看到綠燈的人不會以為這一格證明了它沒有證明的事。
struct LinkProbe {
  bool pipe = false;
  DWORD pipe_err = 0;

  bool handshake = false;
  uint32_t asked_proto = 0;      // 我方最後宣告的線路版本
  uint32_t negotiated_proto = 0; // 服務端回的(協商結果)
  uint32_t my_abi = 0;           // 我方編譯期的 rime_shell ABI
  uint32_t peer_abi = 0;         // 服務端實作端回報的
  std::string service_version;
  uint32_t service_error_code = 0;
  std::string service_error;

  bool session = false;
  uint64_t session_id = 0;

  // 停在哪一步。三個值各自對應一句紅字。
  const char* stage = "開管道";
};

// 一趟往返共用一個絕對 deadline(與 ipc_client.cc 同一個形狀:
// 分成兩份預算的話,一個一次只給一小段的對面可以把一趟拖到兩倍)。
bool PipeWriteAll(HANDLE pipe, HANDLE ev, const std::string& data,
                  DWORD deadline) {
  size_t sent = 0;
  while (sent < data.size()) {
    const LONG left_signed = static_cast<LONG>(deadline - ::GetTickCount());
    const DWORD left = left_signed > 0 ? static_cast<DWORD>(left_signed) : 0;
    if (left == 0) return false;
    OVERLAPPED ov{};
    ov.hEvent = ev;
    ::ResetEvent(ev);
    DWORD written = 0;
    if (!::WriteFile(pipe, data.data() + sent,
                     static_cast<DWORD>(data.size() - sent), &written, &ov)) {
      if (::GetLastError() != ERROR_IO_PENDING) return false;
      if (::WaitForSingleObject(ev, left) != WAIT_OBJECT_0) {
        ::CancelIoEx(pipe, &ov);
        // ⚠ 一定要等它真的結束:ov 與 data 都還活著,提早返回等於讓核心
        //   繼續讀一塊已經沒有的記憶體。
        ::GetOverlappedResult(pipe, &ov, &written, TRUE);
        return false;
      }
      if (!::GetOverlappedResult(pipe, &ov, &written, FALSE)) return false;
    }
    if (written == 0) return false;
    sent += written;
  }
  return true;
}

bool PipeReadFrame(HANDLE pipe, HANDLE ev, FrameReader* reader,
                   std::string* payload, DWORD deadline) {
  if (reader->Next(payload)) return true;
  char buf[4096];
  for (;;) {
    OVERLAPPED ov{};
    ov.hEvent = ev;
    ::ResetEvent(ev);
    DWORD got = 0;
    if (!::ReadFile(pipe, buf, sizeof(buf), &got, &ov)) {
      if (::GetLastError() != ERROR_IO_PENDING) return false;
      const LONG left_signed = static_cast<LONG>(deadline - ::GetTickCount());
      const DWORD left = left_signed > 0 ? static_cast<DWORD>(left_signed) : 0;
      if (::WaitForSingleObject(ev, left) != WAIT_OBJECT_0) {
        ::CancelIoEx(pipe, &ov);
        ::GetOverlappedResult(pipe, &ov, &got, TRUE);
        return false;
      }
      if (!::GetOverlappedResult(pipe, &ov, &got, FALSE)) return false;
    }
    if (got == 0) return false;  // 對面乾淨地關了
    if (!reader->Feed(buf, got)) return false;
    if (reader->Next(payload)) return true;
    if (static_cast<LONG>(deadline - ::GetTickCount()) <= 0) return false;
  }
}

// 預算刻意比產品那一側寬(產品是 300 毫秒,因為它跑在宿主的 UI 執行緒上、
// 使用者按著鍵在等)。這裡沒有人在等,而剛升級完的服務可能正在暖機 ——
// 用 300 毫秒去問會量到「服務慢」而不是「談不攏」,那是兩件事。
constexpr DWORD kProbeTurnMs = 8000;

void ProbeServiceLink(LinkProbe* out, uint32_t langid,
                      const std::string& profile_guid) {
  LinkProbe r;
  r.my_abi = static_cast<uint32_t>(RIME_SHELL_ABI_VERSION);

  HANDLE pipe = OpenServicePipe(&r.pipe_err);
  if (pipe == INVALID_HANDLE_VALUE) {
    r.stage = "開管道";
    *out = r;
    return;
  }
  r.pipe = true;
  r.stage = "握手";

  HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ev) {
    ::CloseHandle(pipe);
    *out = r;
    return;
  }
  FrameReader reader;
  uint32_t seq = 0;

  // ── 握手(含降級重試)────────────────────────────────────────────
  //
  // 降級重試照 ipc_client.cc 的 EnsureReady:舊服務解不開新版的 HELLO 時
  // 會乾淨地關掉連線,那時唯一正確的反應是把版本降一階重來。
  // 少了這一段,一個「新 DLL + 舊服務」會被記成「連不上」,
  // 而它真正的樣子是「少一項功能,但輸入法照常能用」。
  for (uint32_t proto = kProtocolVersion; proto >= kMinProtocolVersion;
       --proto) {
    if (proto != kProtocolVersion) {
      // 對面關掉了連線 —— 要重開一條才談得下去。
      ::CloseHandle(pipe);
      DWORD e = 0;
      pipe = OpenServicePipe(&e);
      if (pipe == INVALID_HANDLE_VALUE) break;
      reader = FrameReader();
      seq = 0;
    }
    Hello h;
    h.proto = proto;
    h.shell_abi = r.my_abi;
    h.host_pid = ::GetCurrentProcessId();
    h.input_langid = langid;
    h.profile_guid = profile_guid;
    h.host_tid = static_cast<uint32_t>(::GetCurrentThreadId());
    {
      wchar_t path[MAX_PATH] = {0};
      ::GetModuleFileNameW(nullptr, path, MAX_PATH);
      h.host_exe = WideToUtf8(path);
    }
    r.asked_proto = proto;
    const uint32_t s = ++seq;
    const DWORD deadline = ::GetTickCount() + kProbeTurnMs;
    std::string reply;
    if (!PipeWriteAll(pipe, ev, Frame(EncodeHello(s, h)), deadline)) break;
    if (!PipeReadFrame(pipe, ev, &reader, &reply, deadline)) {
      // 對面關了 = 它解不開這一版。降一階再試。
      if (proto > kMinProtocolVersion) continue;
      break;
    }
    Op op = Op::kError;
    uint32_t got_seq = 0;
    if (!PeekHeader(reply, &op, &got_seq) || got_seq != s) break;
    if (op == Op::kError) {
      ErrorMsg em;
      uint32_t eseq = 0;
      if (DecodeError(reply, &eseq, &em)) {
        r.service_error_code = em.code;
        r.service_error = em.text;
      }
      if (proto > kMinProtocolVersion) continue;
      break;
    }
    HelloOk ok;
    uint32_t rseq = 0;
    if (!DecodeHelloOk(reply, &rseq, &ok)) break;
    r.negotiated_proto = ok.proto;
    r.peer_abi = ok.shell_abi;
    r.service_version = ok.service_version;
    // 判準與產品那一側逐字相同(ipc_client.cc 的 Handshake):
    // 協商出來的版本要正好是我們宣告的那一個,ABI 要一模一樣。
    if (ok.proto != proto || ok.shell_abi != r.my_abi) {
      if (proto > kMinProtocolVersion) continue;
      break;
    }
    r.handshake = true;
    break;
  }

  if (!r.handshake) {
    ::CloseHandle(ev);
    if (pipe != INVALID_HANDLE_VALUE) ::CloseHandle(pipe);
    *out = r;
    return;
  }

  // ── 建一個 session,再好好關掉 ──────────────────────────────────
  r.stage = "建立 session";
  {
    const uint32_t s = ++seq;
    const DWORD deadline = ::GetTickCount() + kProbeTurnMs;
    std::string reply;
    if (PipeWriteAll(pipe, ev, Frame(EncodeSessionNew(s)), deadline) &&
        PipeReadFrame(pipe, ev, &reader, &reply, deadline)) {
      Op op = Op::kError;
      uint32_t got_seq = 0;
      if (PeekHeader(reply, &op, &got_seq) && got_seq == s) {
        if (op == Op::kError) {
          ErrorMsg em;
          uint32_t eseq = 0;
          if (DecodeError(reply, &eseq, &em)) {
            r.service_error_code = em.code;
            r.service_error = em.text;
          }
        } else {
          SessionOk ok;
          uint32_t rseq = 0;
          if (DecodeSessionOk(reply, &rseq, &ok) && ok.session != 0) {
            r.session = true;
            r.session_id = ok.session;
            r.stage = "(沒有失敗)";
          }
        }
      }
    }
  }

  // ⚠ 一定要送 SESSION_END 再關 handle。它是單向的、而且是這條連線的
  //   終點(service/pipe_server.cc 的 kSessionEnd)。直接關掉的話,服務端
  //   要靠 broken pipe 才知道我們走了,而那條路上的 session 會多活一段
  //   時間 —— 一支只是來問問題的探針不該在服務端留下垃圾。
  if (r.session) {
    const uint32_t s = ++seq;
    const DWORD deadline = ::GetTickCount() + kProbeTurnMs;
    PipeWriteAll(pipe, ev, Frame(EncodeSessionEnd(s, r.session_id)), deadline);
  }
  ::CloseHandle(ev);
  ::CloseHandle(pipe);
  *out = r;
}

// 三句話一次判完。**每一句各自獨立會紅**,而且紅字只指向那一句自己那件事
// —— 前面那一句不成立時後面兩句根本沒機會被問到,所以一次只報最前面那一個。
// 一次報三個「失敗」會讓人以為壞了三件事。
void JudgeLinkProbe(const LinkProbe& r) {
  if (!r.pipe) {
    Fail("**管道連不上**(CreateFile 到 %s 失敗,err=%lu)。\n"
         "     這一格與前景無關 —— 它不經過 TSF,所以它是真的。\n"
         "     只查兩件事,**不要**去碰版本協商或 ABI(那兩關還沒被問到):\n"
         "       1. 那一刻有沒有一支服務在跑(err=2 找不到檔案 = 沒有人在\n"
         "          那個名字上聽;err=5 拒絕存取 = 有人在聽,但這個身分開\n"
         "          不了 —— 提權與非提權的宿主各有一份權杖)\n"
         "       2. 管道名兩邊算出來一不一樣(winshared/winutil.cc 的\n"
         "          RimePipeName:它帶使用者 SID,也帶協議版本後綴)",
         WideToUtf8(RimePipeName()).c_str(),
         static_cast<unsigned long>(r.pipe_err));
    return;
  }
  if (!r.handshake) {
    Fail("**管道連上了,但握手不成立**(停在版本協商)。\n"
         "     我方宣告 proto=%u abi=%u;服務回 proto=%u abi=%u 版本=%s。\n"
         "     服務端說的是:%u %s\n"
         "     這一格與前景無關。要查的正好是這兩件事,而且只有這兩件:\n"
         "       1. 線路協議版本(common/protocol.h 的 kProtocolVersion /\n"
         "          kMinProtocolVersion,以及降級重試那一段)\n"
         "       2. rime_shell 的 ABI —— **那一格沒有降級的路**,\n"
         "          不合就是不合(tsf/ipc_client.cc 的 Handshake)\n"
         "     ⚠ 兩邊的版本號都印在上面了。誰是舊的一眼看得出來,不必猜。",
         static_cast<unsigned>(r.asked_proto), static_cast<unsigned>(r.my_abi),
         static_cast<unsigned>(r.negotiated_proto),
         static_cast<unsigned>(r.peer_abi),
         r.service_version.empty() ? "(沒回)" : r.service_version.c_str(),
         static_cast<unsigned>(r.service_error_code),
         r.service_error.empty() ? "(沒說)" : r.service_error.c_str());
    return;
  }
  if (!r.session) {
    Fail("**握手過了,但建不出 session**(proto=%u abi=%u 都談攏了)。\n"
         "     服務端說的是:%u %s\n"
         "     這一格與前景無關,**也與版本協商無關** —— 版本已經談攏了。\n"
         "     要查的是服務端那一側:引擎現在建不建得出 session\n"
         "     (service/pipe_server.cc 的 kSessionNew、service/engine.cc 的\n"
         "      NewSession / TakeSpareSession)。最常見的原因是它還在部署\n"
         "     詞庫 —— 那時前面「等它就緒」那一步沒等夠。",
         static_cast<unsigned>(r.negotiated_proto),
         static_cast<unsigned>(r.peer_abi),
         static_cast<unsigned>(r.service_error_code),
         r.service_error.empty() ? "(沒說)" : r.service_error.c_str());
    return;
  }
  Ok("線路整條走通了:管道連得上、握手成立(proto=%u abi=%u,服務 %s)、"
     "session 建得出來(=%llu)。",
     static_cast<unsigned>(r.negotiated_proto),
     static_cast<unsigned>(r.peer_abi),
     r.service_version.empty() ? "(沒回版本)" : r.service_version.c_str(),
     static_cast<unsigned long long>(r.session_id));
}

// 三句話一次印完。**格式固定**,verify_installer.sh §13c 靠它分派紅字。
void SayLinkProbe(const char* prefix, const LinkProbe& r) {
  Say("  %s_LINK_PIPE=%d(err=%lu 管道名=%s)\n", prefix, r.pipe ? 1 : 0,
      static_cast<unsigned long>(r.pipe_err),
      WideToUtf8(RimePipeName()).c_str());
  Say("  %s_LINK_HANDSHAKE=%d(我方 proto=%u abi=%u;服務回 proto=%u abi=%u "
      "版本=%s)\n",
      prefix, r.handshake ? 1 : 0, static_cast<unsigned>(r.asked_proto),
      static_cast<unsigned>(r.my_abi),
      static_cast<unsigned>(r.negotiated_proto),
      static_cast<unsigned>(r.peer_abi),
      r.service_version.empty() ? "(沒回)" : r.service_version.c_str());
  Say("  %s_LINK_SESSION=%d(session=%llu)\n", prefix, r.session ? 1 : 0,
      static_cast<unsigned long long>(r.session_id));
  Say("  %s_LINK_STAGE=%s\n", prefix, r.stage);
  if (!r.service_error.empty())
    Say("  %s_LINK_SERVICE_ERROR=%u %s\n", prefix,
        static_cast<unsigned>(r.service_error_code), r.service_error.c_str());
}

// ───────────────── 那一橫的「在場」連線:服務端那一格 ─────────────────
//
// ══ 補的是哪一個洞 ═════════════════════════════════════════════════
//
// 那一橫顯不顯示,服務端要答的是「使用者此刻正在用的那一條執行緒上,
// 啟用中的是不是我們」(common/bar_owner.h)。而那句話的前半段只有宿主
// 自己知道,它表達的方式就是**在場連線的生死**:ActivateEx / profile
// sink 的啟用邊開,Deactivate / profile sink 的非啟用邊關。
//
// 判準本身守得很滿(tests/test_bar_owner.cc 十二支 + test_bar_visibility.cc
// 九支)。⚠ **這一格守的是訊號有沒有真的到服務端。**
// verify_tsf.sh 以前走到「ActivateEx 被呼叫了」就停手,
// **沒有問過 activation 會不會開出一條連線**。
//
// 而使用者實機回報的正是那一格:切回輸入法之後那一橫不見了,
// 按下第一顆鍵才回來 —— 因為在那之前,唯一會開連線的
// EnsureReady() 全部在按鍵路徑上。
//
// ══ 這個類別做什麼 ═════════════════════════════════════════════════
//
// 在 ActivateProfile **之前**把那條具名管道自己開起來,然後數有幾條連線
// 連進來。也就是說它站在**服務端**那一格上問問題,而不是問 DLL
// 「你有沒有想連」——後者是「訊號有沒有送出去」,前者才是「訊號有沒有到」。
//
//   · 不回話、**一條連線都不關**。它**不是** rime_service.exe 的替身,
//     只是那條管道的接線員;這個 job 沒有 librime,也不需要。
//   · ⚠ **在場連線現在會送一則 HELLO**(它要報出 host_pid / host_tid,
//     否則服務端只知道「有一條 handle 開著」,而那是一個沒有產品意義的
//     量 —— 見 common/bar_owner.h)。這個接線員不回那則 HELLO,所以
//     DLL 那一側會在握手逾時之後自己重連 —— 也就是說底下數到的
//     `accepted` **不再等於「有幾個宿主」**,只等於「訊號到了幾次」。
//     這一格要驗的仍然是同一件事:第一顆按鍵之前有沒有到過。
//   · ⏳ 讓這個接線員真的回一則 HELLO_OK(於是 Windows CI 會實際跑過
//     proto v3 與 host_tid 上線路那一段),只有 Windows runner 驗得到,
//     已記進 #48。**這一輪沒有做**,不要把 accepted>0 讀成「握手過了」。
//
// ⚠ FIRST_PIPE_INSTANCE 是刻意的:那個名字已經被一支**真的**服務佔著時,
//   我們要失敗並說出來,而不是接手 —— 接手的話數到的連線是誰的說不準,
//   而一個說不準的綠燈比紅燈糟。快速 job 裡沒有服務,所以那是真的紅燈。
class PresenceWatch {
 public:
  ~PresenceWatch() { Stop(); }

  bool Start() {
    stop_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    accept_evt_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_ || !accept_evt_) {
      err_ = ::GetLastError();
      return false;
    }
    pending_ = CreateInstance(true, &err_);
    if (pending_ == INVALID_HANDLE_VALUE) return false;
    thread_ = ::CreateThread(nullptr, 0, &PresenceWatch::ThreadMain, this, 0,
                             nullptr);
    if (!thread_) {
      err_ = ::GetLastError();
      return false;
    }
    return true;
  }

  // 到目前為止**接到過**幾條連線。只增不減 —— 我們要問的是
  // 「第一顆按鍵之前有沒有來過」,不是「此刻有幾條」。
  int accepted() const {
    return static_cast<int>(::InterlockedCompareExchange(
        const_cast<volatile LONG*>(&accepted_), 0, 0));
  }

  DWORD error() const { return err_; }

  void Stop() {
    if (stop_) ::SetEvent(stop_);
    // ⚠ 先把執行緒收乾淨,才碰 held_ —— 那個容器只有它在寫。
    if (thread_) {
      ::WaitForSingleObject(thread_, 3000);
      ::CloseHandle(thread_);
      thread_ = nullptr;
    }
    for (size_t i = 0; i < held_.size(); ++i) ::CloseHandle(held_[i]);
    held_.clear();
    if (pending_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(pending_);
      pending_ = INVALID_HANDLE_VALUE;
    }
    if (accept_evt_) {
      ::CloseHandle(accept_evt_);
      accept_evt_ = nullptr;
    }
    if (stop_) {
      ::CloseHandle(stop_);
      stop_ = nullptr;
    }
  }

 private:
  static HANDLE CreateInstance(bool first, DWORD* err) {
    DWORD mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
    if (first) mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
    // ⚠ 安全描述元用預設的(nullptr)。這裡的用戶端與伺服器是**同一個
    //   進程、同一份權杖**(rime_tsf.dll 被載進 rime_tsf_host.exe),
    //   而產品那一份 DACL 只授權目前使用者這件事是 pipe_server.cc 的事,
    //   不是這支假宿主要驗的。
    HANDLE h = ::CreateNamedPipeW(
        RimePipeName().c_str(), mode,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE && err) *err = ::GetLastError();
    return h;
  }

  static DWORD WINAPI ThreadMain(LPVOID p) {
    static_cast<PresenceWatch*>(p)->Run();
    return 0;
  }

  void Run() {
    HANDLE pipe = pending_;
    pending_ = INVALID_HANDLE_VALUE;
    for (;;) {
      if (pipe == INVALID_HANDLE_VALUE) {
        DWORD e = 0;
        pipe = CreateInstance(false, &e);
        if (pipe == INVALID_HANDLE_VALUE) return;
      }
      OVERLAPPED ov;
      ::ZeroMemory(&ov, sizeof(ov));
      ov.hEvent = accept_evt_;
      ::ResetEvent(accept_evt_);
      BOOL done = ::ConnectNamedPipe(pipe, &ov);
      const DWORD e = done ? 0u : ::GetLastError();
      if (!done && e == ERROR_PIPE_CONNECTED) {
        done = TRUE;  // 用戶端在 ConnectNamedPipe 之前就連上了
      } else if (!done && e == ERROR_IO_PENDING) {
        HANDLE waits[2] = {accept_evt_, stop_};
        if (::WaitForMultipleObjects(2, waits, FALSE, INFINITE) !=
            WAIT_OBJECT_0) {
          ::CancelIoEx(pipe, &ov);
          DWORD n = 0;
          ::GetOverlappedResult(pipe, &ov, &n, TRUE);
          ::CloseHandle(pipe);
          return;
        }
        DWORD n = 0;
        done = ::GetOverlappedResult(pipe, &ov, &n, FALSE);
      }
      if (!done) {
        ::CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
        continue;
      }
      ::InterlockedIncrement(&accepted_);
      // ⚠ 不關。關掉的話 DLL 那一側會重連,而一條連線就會被數成好幾條。
      held_.push_back(pipe);
      pipe = INVALID_HANDLE_VALUE;
      if (::WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) return;
    }
  }

  HANDLE stop_ = nullptr;
  HANDLE accept_evt_ = nullptr;
  HANDLE thread_ = nullptr;
  HANDLE pending_ = INVALID_HANDLE_VALUE;
  // ⚠ 只有接線執行緒寫它,而 Stop() 先 join 才讀。
  std::vector<HANDLE> held_;
  volatile LONG accepted_ = 0;
  DWORD err_ = 0;
};

// ───────────────────────── 假的文件 ─────────────────────────
//
// 最小可用的 ITextStoreACP。夠讓 TSF 的 generic context 在上面做:
// 取選取範圍、查可不可以插入、插入文字、改選取範圍、問插入點的螢幕矩形。
// 其餘的一律 E_NOTIMPL —— 而且**要誠實地 E_NOTIMPL**,不要回 S_OK 假裝做了,
// 那會讓失敗延後到一個與原因無關的地方才出現。
class FakeDoc final : public ITextStoreACP {
 public:
  explicit FakeDoc(HWND hwnd) : hwnd_(hwnd) {}

  std::wstring text;
  LONG sel_start = 0;
  LONG sel_end = 0;

  // ── IUnknown ──────────────────────────────────────────────
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITextStoreACP))
      *ppv = static_cast<ITextStoreACP*>(this);
    if (!*ppv) return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(::InterlockedIncrement(&ref_));
  }
  STDMETHODIMP_(ULONG) Release() override {
    const LONG n = ::InterlockedDecrement(&ref_);
    if (n == 0) delete this;
    return static_cast<ULONG>(n);
  }

  // ── sink ──────────────────────────────────────────────────
  STDMETHODIMP AdviseSink(REFIID riid, IUnknown* punk, DWORD mask) override {
    if (!IsEqualIID(riid, IID_ITextStoreACPSink)) return E_INVALIDARG;
    if (sink_) sink_->Release();
    sink_ = nullptr;
    mask_ = mask;
    return punk->QueryInterface(IID_ITextStoreACPSink, (void**)&sink_);
  }
  STDMETHODIMP UnadviseSink(IUnknown*) override {
    if (sink_) {
      sink_->Release();
      sink_ = nullptr;
    }
    return S_OK;
  }

  // ── 鎖 ────────────────────────────────────────────────────
  //
  // ⚠ 一定要**同步**授予。我們的文字服務用的是
  //   RequestEditSession(TF_ES_SYNC | TF_ES_READWRITE)(見 text_service.cc:
  //   非同步的 session 回來時那顆按鍵早就被宿主處理過了,順序全亂)。
  //   這裡若排隊改成非同步,那條路就完全走不到 —— 而 CI 會綠,
  //   因為驗到的是另一條路。
  STDMETHODIMP RequestLock(DWORD flags, HRESULT* result) override {
    if (!sink_ || !result) return E_UNEXPECTED;
    if (lock_ != 0) {
      *result = TS_E_SYNCHRONOUS;
      return S_OK;
    }
    lock_ = flags & (TS_LF_READ | TS_LF_READWRITE);
    *result = sink_->OnLockGranted(lock_);
    lock_ = 0;
    return S_OK;
  }

  STDMETHODIMP GetStatus(TS_STATUS* status) override {
    if (!status) return E_INVALIDARG;
    status->dwDynamicFlags = 0;
    status->dwStaticFlags = TS_SS_NOHIDDENTEXT;
    return S_OK;
  }

  STDMETHODIMP QueryInsert(LONG start, LONG end, ULONG /*cch*/,
                           LONG* out_start, LONG* out_end) override {
    if (!out_start || !out_end) return E_INVALIDARG;
    const LONG len = static_cast<LONG>(text.size());
    *out_start = start < 0 ? 0 : (start > len ? len : start);
    *out_end = end < 0 ? 0 : (end > len ? len : end);
    return S_OK;
  }

  STDMETHODIMP GetSelection(ULONG index, ULONG count, TS_SELECTION_ACP* sel,
                            ULONG* fetched) override {
    if (!fetched) return E_INVALIDARG;
    *fetched = 0;
    if (count == 0) return S_OK;
    if (index != TF_DEFAULT_SELECTION && index != 0) return TS_E_NOSELECTION;
    if (!sel) return E_INVALIDARG;
    sel[0].acpStart = sel_start;
    sel[0].acpEnd = sel_end;
    sel[0].style.ase = TS_AE_END;
    sel[0].style.fInterimChar = FALSE;
    *fetched = 1;
    return S_OK;
  }

  STDMETHODIMP SetSelection(ULONG count, const TS_SELECTION_ACP* sel) override {
    if (count == 0 || !sel) return E_INVALIDARG;
    sel_start = sel[0].acpStart;
    sel_end = sel[0].acpEnd;
    return S_OK;
  }

  STDMETHODIMP GetText(LONG start, LONG end, WCHAR* plain, ULONG plain_req,
                       ULONG* plain_out, TS_RUNINFO* run, ULONG run_req,
                       ULONG* run_out, LONG* next) override {
    const LONG len = static_cast<LONG>(text.size());
    if (start < 0) start = 0;
    if (start > len) start = len;
    LONG stop = (end < 0) ? len : end;
    if (stop > len) stop = len;
    if (stop < start) stop = start;
    ULONG n = static_cast<ULONG>(stop - start);
    if (plain && plain_req < n) n = plain_req;
    if (plain && n > 0) ::memcpy(plain, text.data() + start, n * sizeof(WCHAR));
    if (plain_out) *plain_out = n;
    if (run && run_req > 0) {
      run[0].uCount = n;
      run[0].type = TS_RT_PLAIN;
      if (run_out) *run_out = 1;
    } else if (run_out) {
      *run_out = 0;
    }
    if (next) *next = start + static_cast<LONG>(n);
    return S_OK;
  }

  STDMETHODIMP SetText(DWORD /*flags*/, LONG start, LONG end,
                       const WCHAR* s, ULONG cch, TS_TEXTCHANGE* change) override {
    const LONG len = static_cast<LONG>(text.size());
    if (start < 0) start = 0;
    if (start > len) start = len;
    if (end < 0 || end > len) end = len;
    if (end < start) end = start;
    text.replace(static_cast<size_t>(start), static_cast<size_t>(end - start),
                 s ? std::wstring(s, cch) : std::wstring());
    if (change) {
      change->acpStart = start;
      change->acpOldEnd = end;
      change->acpNewEnd = start + static_cast<LONG>(cch);
    }
    sel_start = sel_end = start + static_cast<LONG>(cch);
    return S_OK;
  }

  STDMETHODIMP InsertTextAtSelection(DWORD flags, const WCHAR* s, ULONG cch,
                                     LONG* out_start, LONG* out_end,
                                     TS_TEXTCHANGE* change) override {
    // TS_IAS_QUERYONLY:只回報「會插在哪裡」,一個字都不可以動。
    // 我們的 StartCompositionIfNeeded 走的正是這一條(它要的是一個
    // 折疊在插入點的 range,不是真的插入)。
    if (flags & TS_IAS_QUERYONLY) {
      if (out_start) *out_start = sel_start;
      if (out_end) *out_end = sel_end;
      return S_OK;
    }
    const LONG start = sel_start;
    const LONG end = sel_end;
    TS_TEXTCHANGE tc{};
    const HRESULT hr = SetText(0, start, end, s, cch, &tc);
    if (FAILED(hr)) return hr;
    if (out_start) *out_start = tc.acpStart;
    if (out_end) *out_end = tc.acpNewEnd;
    if (change) *change = tc;
    return S_OK;
  }

  STDMETHODIMP GetEndACP(LONG* acp) override {
    if (!acp) return E_INVALIDARG;
    *acp = static_cast<LONG>(text.size());
    return S_OK;
  }

  STDMETHODIMP GetActiveView(TsViewCookie* cookie) override {
    if (!cookie) return E_INVALIDARG;
    *cookie = 1;
    return S_OK;
  }

  STDMETHODIMP GetTextExt(TsViewCookie, LONG, LONG, RECT* rc,
                          BOOL* clipped) override {
    if (!rc || !clipped) return E_INVALIDARG;
    // 固定一塊矩形。**這裡與真實宿主最不像**,所以要說清楚:
    // 真的宿主上 GetTextExt 常常失敗或回空矩形(見 README 的「沒被驗證」
    // 第 4 項),而候選窗定位的退路正是為那件事寫的。這支程式驗不到它。
    ::SetRect(rc, 100, 200, 101, 220);
    *clipped = FALSE;
    return S_OK;
  }

  STDMETHODIMP GetScreenExt(TsViewCookie, RECT* rc) override {
    if (!rc) return E_INVALIDARG;
    ::SetRect(rc, 0, 0, 800, 600);
    return S_OK;
  }

  STDMETHODIMP GetWnd(TsViewCookie, HWND* hwnd) override {
    if (!hwnd) return E_INVALIDARG;
    *hwnd = hwnd_;
    return S_OK;
  }

  // ── 用不到的 ──────────────────────────────────────────────
  STDMETHODIMP GetFormattedText(LONG, LONG, IDataObject**) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP GetEmbedded(LONG, REFGUID, REFIID, IUnknown**) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP QueryInsertEmbedded(const GUID*, const FORMATETC*,
                                   BOOL* ok) override {
    if (ok) *ok = FALSE;
    return S_OK;
  }
  STDMETHODIMP InsertEmbedded(DWORD, LONG, LONG, IDataObject*,
                              TS_TEXTCHANGE*) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP InsertEmbeddedAtSelection(DWORD, IDataObject*, LONG*, LONG*,
                                         TS_TEXTCHANGE*) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP RequestSupportedAttrs(DWORD, ULONG, const TS_ATTRID*) override {
    return S_OK;
  }
  STDMETHODIMP RequestAttrsAtPosition(LONG, ULONG, const TS_ATTRID*,
                                      DWORD) override {
    return S_OK;
  }
  STDMETHODIMP RequestAttrsTransitioningAtPosition(LONG, ULONG,
                                                   const TS_ATTRID*,
                                                   DWORD) override {
    return S_OK;
  }
  STDMETHODIMP FindNextAttrTransition(LONG, LONG, ULONG, const TS_ATTRID*, DWORD,
                                      LONG*, BOOL*, LONG*) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP RetrieveRequestedAttrs(ULONG, TS_ATTRVAL*,
                                      ULONG* fetched) override {
    if (fetched) *fetched = 0;
    return S_OK;
  }
  STDMETHODIMP GetACPFromPoint(TsViewCookie, const POINT*, DWORD,
                               LONG*) override {
    return E_NOTIMPL;
  }

 private:
  ~FakeDoc() {
    if (sink_) sink_->Release();
  }
  LONG ref_ = 1;
  HWND hwnd_;
  ITextStoreACPSink* sink_ = nullptr;
  DWORD mask_ = 0;
  DWORD lock_ = 0;
};

// ── 把一個 ASCII 字元變成 (VK, lParam) ────────────────────────────
bool AsciiToKey(wchar_t ch, WPARAM* vk, LPARAM* lparam) {
  SHORT vs = ::VkKeyScanW(ch);
  if (vs == -1) return false;
  const WPARAM v = static_cast<WPARAM>(vs & 0xFF);
  const UINT scan = ::MapVirtualKeyW(static_cast<UINT>(v), MAPVK_VK_TO_VSC);
  *vk = v;
  // 重複次數 1、掃描碼在 16–23 位。這正是宿主收到 WM_KEYDOWN 時的形狀 ——
  // 我們的 BuildKeyEvent 從這裡取掃描碼。
  *lparam = static_cast<LPARAM>(1 | (static_cast<LPARAM>(scan) << 16));
  return true;
}

std::string Narrow(const std::wstring& s) { return WideToUtf8(s); }

// ── 具名功能鍵 ────────────────────────────────────────────────────
//
// ⚠ 這張表存在的理由:舊的 --keys 只吃 ASCII 字元(VkKeyScanW),
//   所以 CI **從來沒有送過一顆功能鍵**。使用者回報的退格鍵缺陷因此
//   完全在自動化的射程之外 —— 六顆字母全綠,而鍵盤是壞的。
struct NamedKey {
  const wchar_t* name;
  WPARAM vk;
  bool extended;  // lParam bit 24。方向鍵那一組在主鍵盤上是 extended。
};
const NamedKey kNamedKeys[] = {
    {L"BS", VK_BACK, false},      {L"DEL", VK_DELETE, true},
    {L"LEFT", VK_LEFT, true},     {L"RIGHT", VK_RIGHT, true},
    {L"UP", VK_UP, true},         {L"DOWN", VK_DOWN, true},
    {L"HOME", VK_HOME, true},     {L"END", VK_END, true},
    {L"PGUP", VK_PRIOR, true},    {L"PGDN", VK_NEXT, true},
    {L"ESC", VK_ESCAPE, false},   {L"ENTER", VK_RETURN, false},
    {L"TAB", VK_TAB, false},      {L"SPACE", VK_SPACE, false},
    {L"F1", VK_F1, false},        {L"INS", VK_INSERT, true},
};

// 一顆要送出去的鍵。
struct SeqKey {
  std::wstring label;  // 印出來用
  WPARAM vk = 0;
  LPARAM lparam = 0;
  wchar_t ch = 0;  // 字元鍵才有;宿主的預設處理要拿它插進文件
};

bool NamedToKey(const std::wstring& name, SeqKey* out) {
  for (const NamedKey& n : kNamedKeys) {
    if (name != n.name) continue;
    const UINT scan =
        ::MapVirtualKeyW(static_cast<UINT>(n.vk), MAPVK_VK_TO_VSC);
    out->label = L"{" + name + L"}";
    out->vk = n.vk;
    out->lparam = static_cast<LPARAM>(1 | (static_cast<LPARAM>(scan) << 16) |
                                      (n.extended ? (1L << 24) : 0));
    out->ch = (n.vk == VK_SPACE) ? L' ' : 0;
    return true;
  }
  return false;
}

// "n i h a o {BS} 1" → 一串 SeqKey。空白與逗號都是分隔符;
// 不在 {} 裡的每一個非空白字元各自是一顆鍵。
// 宿主的預設處理:KeyDown 說「沒吃掉」的鍵,一個真的編輯框會做的事。
//
// ⚠⚠ **這一段是這支程式這一輪最重要的改動。**
//
//   沒有它的話,「輸入法放行了,宿主收到了」與「輸入法吃掉了但什麼都沒做」
//   在文件內容上完全一樣(兩者都是文件沒變)。而後者正是使用者回報的
//   「可以打字,不能刪除」。也就是說:沒有這一段,那個缺陷在 CI 上
//   **不可能**被抓到,不管送多少顆退格。
//
//   所以矩陣的斷言看的是**文件內容**,不是「有沒有被吃掉」。
void HostDefaultAction(FakeDoc* doc, const SeqKey& k) {
  const LONG len = static_cast<LONG>(doc->text.size());
  LONG& s = doc->sel_start;
  LONG& e = doc->sel_end;
  if (s < 0) s = 0;
  if (s > len) s = len;
  switch (k.vk) {
    case VK_BACK:
      if (s > 0) {
        doc->text.erase(static_cast<size_t>(s - 1), 1);
        s = e = s - 1;
      }
      return;
    case VK_DELETE:
      if (s < static_cast<LONG>(doc->text.size()))
        doc->text.erase(static_cast<size_t>(s), 1);
      e = s;
      return;
    case VK_LEFT:  if (s > 0) s = e = s - 1; return;
    case VK_RIGHT: if (s < len) s = e = s + 1; return;
    case VK_HOME:  s = e = 0; return;
    case VK_END:   s = e = len; return;
    // 單行編輯框對這幾顆鍵不動文件。Enter 也一樣 —— 插入換行只會讓
    // 矩陣的預期字串變得難讀,而我們要驗的是「有沒有被放行」。
    case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
    case VK_ESCAPE: case VK_TAB: case VK_RETURN: case VK_F1: case VK_INSERT:
      return;
    default: break;
  }
  if (k.ch != 0) {
    doc->text.insert(static_cast<size_t>(s), 1, k.ch);
    s = e = s + 1;
  }
}

// 送一顆鍵並讓假宿主做它的預設處理。
//
// ⚠ 抽成函式**不是**為了少打字。第二階段(升級之後)要走的必須是
//   與第一階段**完全同一段**程式碼 —— 兩段各寫一份的話,「升級之後
//   還打得出字」驗到的會是另一條路,而那正是這個專案抓過很多次的形狀。
struct SendOutcome {
  BOOL test_eaten = FALSE;
  BOOL eaten = FALSE;
  bool host_did = false;
};

// ── Ctrl+空白鍵那顆保留鍵,TSF 到底收下了沒有 ────────────────────
//
// 使用者回報「ctrl+ 空格沒辦法切中英文」。做法是 ITfKeystrokeMgr::PreserveKey
// (**不是**低階鍵盤 hook,理由見 common/hotkey_policy.h),而那一次呼叫
// 發生在 ActivateEx 裡面 —— 也就是說「有沒有註冊成功」只有在**真的被
// 系統啟用過的那一份文字服務**上問得到。
//
// ⚠ 這一支問的是註冊,不是按下去會怎樣。用 IsPreservedKey 向 TSF 反問:
//   它認不認得 {我們的 GUID, VK_SPACE + TF_MOD_CONTROL}。
//
// ⚠ 第二問同樣重要:{VK_C + TF_MOD_CONTROL} **必須不認得**。這個專案吃過
//   「宣稱每一個 keysym 都要,結果吃掉 Ctrl+C」的虧,而保留鍵是同一種
//   權力 —— 註冊得太寬的話,使用者在每一個程式裡的 Ctrl+C 都會消失,
//   而他不會知道是輸入法幹的。
void CheckPreservedKey(ITfKeystrokeMgr* ks) {
  Say("\n--- 保留鍵(Ctrl+空白鍵)---\n");
  if (!ks) {
    Fail("拿不到 ITfKeystrokeMgr —— 問不了保留鍵");
    return;
  }
  TF_PRESERVEDKEY want{};
  want.uVKey = VK_SPACE;
  want.uModifiers = TF_MOD_CONTROL;
  BOOL registered = FALSE;
  HRESULT hr = ks->IsPreservedKey(GUID_RimePreservedKeyToggle, &want,
                                  &registered);
  Say("  IsPreservedKey(Ctrl+Space) hr=0x%08lX registered=%d\n",
      static_cast<unsigned long>(hr), registered ? 1 : 0);
  if (FAILED(hr) || !registered)
    Fail("Ctrl+空白鍵**沒有**被註冊成保留鍵 —— 使用者按下去不會有任何事發生。\n"
         "     註冊在 TextService::ActivateEx 裡(見 common/hotkey_policy.h);\n"
         "     那裡失敗時記錄會有一行『保留鍵 Ctrl+Space:註冊失敗』。");

  TF_PRESERVEDKEY nope{};
  nope.uVKey = 'C';
  nope.uModifiers = TF_MOD_CONTROL;
  BOOL stolen = FALSE;
  hr = ks->IsPreservedKey(GUID_RimePreservedKeyToggle, &nope, &stolen);
  Say("  IsPreservedKey(Ctrl+C)     hr=0x%08lX registered=%d\n",
      static_cast<unsigned long>(hr), stolen ? 1 : 0);
  if (SUCCEEDED(hr) && stolen)
    Fail("Ctrl+C 也被註冊成保留鍵了 —— 使用者在每一個程式裡的複製都會消失,\n"
         "     而他不會知道是輸入法幹的。保留鍵只准註冊真的要處理的那一顆。");
}

// ── ⚠ 註冊之外的另一半:那顆鍵**按下去**會怎樣 ──────────────────
//
// 上面 CheckPreservedKey() 問的是「TSF 收下註冊了沒」。它不會呼叫產品
// 一行程式碼 —— 實測過:把 tsf/text_service.cc 的 OnPreservedKey 在比對完
// GUID 之後直接 `return S_OK`(註冊留著、hotkey_policy 留著、服務端的攔截
// 也留著),**CI 三個 job 全綠**。也就是說中英切換可以整條死掉而沒有人知道。
//
// 原因是中間那一段沒有任何自動化走過:
//     OnPreservedKey → IPC → 服務 → 引擎切模式 → 快照 → edit session → 文件
// verify_tsf.sh 只問註冊,verify_ime.sh 只走管道問服務端。這一節補的就是它。
//
// ── 「到底有沒有被呼叫」怎麼知道 ────────────────────────────────
//
// 沒有任何回傳值看得出來:TSF 收下按鍵、回 S_OK、pfEaten=FALSE ——
// 與「文字服務把它直接 return S_OK 掉」長得一模一樣。唯一的證據是瘦 DLL
// 自己在 OnPreservedKey 裡寫下的那一行記錄(寫完才返回)。所以這裡數
// 記錄檔裡那句話多了幾筆,而不是猜。
std::string ReadAll(const std::wstring& path);  // 定義在下面

int CountIn(const std::string& hay, const char* needle) {
  const std::string pat(needle);
  if (pat.empty()) return 0;
  int n = 0;
  size_t at = 0;
  while ((at = hay.find(pat, at)) != std::string::npos) {
    ++n;
    at += pat.size();
  }
  return n;
}

struct PreservedSend {
  const char* route = "none";
  bool delivered = false;
  BOOL eaten = FALSE;
};

// 送出 Ctrl+空白鍵。
//
// ① 先送**真的按鍵**:把 Ctrl 壓進這條執行緒的鍵盤輸入狀態(瘦 DLL 的
//    BuildKeyEvent 讀的就是 GetKeyboardState),再走與其他每一顆鍵完全
//    同一條 TestKeyDown / KeyDown。TSF 認得那顆保留鍵的話,它會轉成
//    OnPreservedKey 交給文字服務 —— 那是使用者機器上發生的事。
//
// ② 真的按鍵沒有到達時,退一步用 ITfKeystrokeMgr::SimulatePreservedKey。
//    那是 TSF 為這件事準備的 API:一樣由 TSF 去比對註冊、一樣呼叫我們的
//    OnPreservedKey。走哪一條都會**逼產品的那一整段跑一次**,而那正是
//    這一輪之前沒有任何自動化走過的部分。
//
// ⚠ 走了哪一條要印出來。退到 ②「不算失敗」(這個 runner 是非互動的工作
//   階段,而 TSF 的按鍵路由本來就與桌面上不保證一樣),但兩條都沒到達
//   **就是失敗** —— 那代表 OnPreservedKey 根本沒有被呼叫。
//
// ⚠ 這一支刻意不呼叫 HostDefaultAction。真的宿主在 Ctrl 壓著的時候也不會
//   把空白鍵插進文件 —— 讓它插進去的話,下面每一條文件斷言都會多一個空格,
//   而那是測試自己製造的假象。
PreservedSend PressPreservedKey(ITfKeystrokeMgr* ks, ITfContext* ctx,
                                const std::wstring& trace_path) {
  PreservedSend out;
  static const char kMark[] = "保留鍵 Ctrl+Space:";
  const int before = CountIn(ReadAll(trace_path), kMark);

  BYTE saved[256] = {0};
  BYTE state[256] = {0};
  const bool got_state = ::GetKeyboardState(saved) != FALSE;
  if (got_state) {
    std::memcpy(state, saved, sizeof(state));
    state[VK_CONTROL] |= 0x80;
    state[VK_LCONTROL] |= 0x80;
    ::SetKeyboardState(state);
  }
  const UINT scan = ::MapVirtualKeyW(VK_SPACE, MAPVK_VK_TO_VSC);
  const LPARAM lp = static_cast<LPARAM>(1 | (static_cast<LPARAM>(scan) << 16));
  BOOL test_eaten = FALSE;
  BOOL eaten = FALSE;
  BOOL up_eaten = FALSE;
  ks->TestKeyDown(VK_SPACE, lp, &test_eaten);
  ks->KeyDown(VK_SPACE, lp, &eaten);
  ks->KeyUp(VK_SPACE, lp | 0xC0000000, &up_eaten);
  if (got_state) ::SetKeyboardState(saved);
  Pump(80);

  const int after = CountIn(ReadAll(trace_path), kMark);
  Say("  真的按鍵 Ctrl+Space:test=%d down=%d(記錄多了 %d 行)\n",
      test_eaten ? 1 : 0, eaten ? 1 : 0, after - before);
  if (after > before) {
    out.route = "real";
    out.delivered = true;
    out.eaten = eaten;
    return out;
  }

  BOOL sim_eaten = FALSE;
  const HRESULT hr =
      ks->SimulatePreservedKey(ctx, GUID_RimePreservedKeyToggle, &sim_eaten);
  Pump(80);
  const int after2 = CountIn(ReadAll(trace_path), kMark);
  Say("  SimulatePreservedKey hr=0x%08lX eaten=%d(記錄多了 %d 行)\n",
      static_cast<unsigned long>(hr), sim_eaten ? 1 : 0, after2 - after);
  if (after2 > after) {
    out.route = "simulated";
    out.delivered = true;
    out.eaten = sim_eaten;
  }
  return out;
}

// ── ⚠ 這是一支**量測**,不是斷言 ──────────────────────────────
//
// 使用者實機回報:「我用 shift 切換中英文,但是他不變」。要不要做
// 「輕點 Shift 切中英」取決於一個至今沒有人量過的事實:
//
//   **TSF 到底會不會把純修飾鍵交給 key event sink?**
//
// 這棵樹裡有兩份互相矛盾的答案,而兩份都沒有任何測試或實測在背書:
//
//   tsf/text_service.cc 的 OnTestKeyUp:
//     「TSF 本來就不會把純修飾鍵(Shift / Ctrl)的事件交給 key event
//       sink,所以 librime 那套『按一下 Shift 切中英』在這條路徑上
//       做不到」
//
//   common/key_eat_policy.cc:
//     「不太會…但交過來時」(它替 Shift_L…Super_R 準備了 kHostOnly)
//
// 擋著 Shift 這條路的不是技術,是這一句沒有人量過的話。所以這一支
// **不 Fail**:它把量到的東西印出來,由那個結果決定下一輪走哪一條。
//
// ⚠ **2026-08-12:這一支已經答過一次了 —— 收得到。** CI run 31511075812
//   (sha ca97498)logic-x64:SHIFT_TRACE_LINES=1,vk=0x10 scan=0x2A
//   keysym=0xFFE1 族=host-only 吃掉=0。上面那兩份互相矛盾的答案裡,
//   **key_eat_policy.cc 是對的那一份**;text_service.cc 那一份已照實測改寫。
//   這一支**留著**:它是那個結論唯一的出處,而且事實會隨 OS 版本改變。
//
// ── 判準與 PressPreservedKey 一樣:數 trace 檔多了幾行 ────────────
//
// 沒有任何回傳值看得出來 OnTestKeyDown 有沒有被呼叫:TSF 收下按鍵、
// 回 S_OK、pfEaten=FALSE —— 與「根本沒交給我們」長得一模一樣。唯一的
// 證據是瘦 DLL 自己在 OnTestKeyDown 裡寫下的那一行「按鍵 vk=…」。
//
// ⚠ 順便量兩件下一輪一定會用到的事實,而且它們都寫在那一行記錄裡:
//     · wParam 是泛用的 VK_SHIFT(0x10)還是 VK_LSHIFT(0xA0)?
//       keymap.cc:157 現在把泛用的 VK_SHIFT 一律折成 XK_Shift_L,
//       左右分不出來 —— 如果 TSF 送的是泛用碼,那就得靠 scan code 分。
//     · scan code 有沒有正確帶進來(左 Shift = 0x2A,右 = 0x36)。
//
// ⚠ 這支 harness 走的是 ITfKeystrokeMgr::KeyDown —— **那是宿主呼叫的
//   入口**。它證明的是「sink 收得到」,證**不到**「真實宿主的訊息迴圈
//   會不會把 VK_SHIFT 送進 TSF」。後者只有人在記事本 / Chrome / Word
//   上試得出來,已列進 #48。
void MeasureShiftDelivery(ITfKeystrokeMgr* ks, const std::wstring& trace_path) {
  static const char kMark[] = "按鍵 vk=";
  const std::string before_all = ReadAll(trace_path);
  const int before = CountIn(before_all, kMark);

  // 左 Shift。scan code 從系統查,不要寫死 —— 不同的鍵盤配置上不一樣,
  // 而寫死一個值會讓量到的結果變成「我們送了什麼」而不是「系統怎麼看」。
  const UINT scan = ::MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC);
  const LPARAM lp = static_cast<LPARAM>(1 | (static_cast<LPARAM>(scan) << 16));
  BOOL test_eaten = FALSE;
  BOOL down_eaten = FALSE;
  BOOL up_eaten = FALSE;
  ks->TestKeyDown(VK_SHIFT, lp, &test_eaten);
  ks->KeyDown(VK_SHIFT, lp, &down_eaten);
  ks->KeyUp(VK_SHIFT, lp | 0xC0000000, &up_eaten);
  Pump(80);

  const std::string after_all = ReadAll(trace_path);
  const int after = CountIn(after_all, kMark);

  Say("  SHIFT_SCAN_SENT=0x%02X\n", static_cast<unsigned>(scan));
  Say("  SHIFT_TESTKEYDOWN_EATEN=%d\n", test_eaten ? 1 : 0);
  Say("  SHIFT_KEYDOWN_EATEN=%d\n", down_eaten ? 1 : 0);
  Say("  SHIFT_KEYUP_EATEN=%d\n", up_eaten ? 1 : 0);
  // ⚠ **這一行就是整支的產出。** 0 = TSF 沒把純修飾鍵交給 sink
  //   (text_service.cc 那句話成立);> 0 = 它交了,而那條路走得通。
  Say("  SHIFT_TRACE_LINES=%d\n", after - before);

  if (after > before) {
    // 把新增的那幾行原樣印出來 —— vk 與 scan 的**實際值**在裡面,
    // 而下一輪的 ResolveShiftSide() 就是照那個值寫的。
    Say("  ── 新增的按鍵記錄 ──\n");
    size_t at = before_all.size() <= after_all.size() ? before_all.size() : 0;
    size_t line = after_all.find('\n', at);
    int shown = 0;
    while (at < after_all.size() && shown < 8) {
      const size_t end = (line == std::string::npos) ? after_all.size() : line;
      const std::string one = after_all.substr(at, end - at);
      if (one.find(kMark) != std::string::npos) {
        Say("  %s\n", one.c_str());
        ++shown;
      }
      if (line == std::string::npos) break;
      at = line + 1;
      line = after_all.find('\n', at);
    }
  } else {
    Say("  (一行都沒有多 —— key event sink 沒有收到這顆鍵)\n");
  }
  // ⚠ **刻意不 Fail。** 這一支問的是「事實是什麼」,不是「合不合格」。
  //   兩種結果都會發生,而兩種都有對應的下一步:
  //     > 0 → P1-乙 走 ShiftTapDetector 那條路
  //     = 0 → 走降級版(TF_MOD_ON_KEYUP)或乾脆不做 Shift,靠文案把
  //           Ctrl + 空白鍵教出來。那是誠實的退路,不是失敗。
}

SendOutcome SendKeyThrough(ITfKeystrokeMgr* ks, FakeDoc* doc, const SeqKey& sk) {
  SendOutcome o;
  ks->TestKeyDown(sk.vk, sk.lparam, &o.test_eaten);
  ks->KeyDown(sk.vk, sk.lparam, &o.eaten);
  BOOL up_eaten = FALSE;
  ks->KeyUp(sk.vk, sk.lparam | 0xC0000000, &up_eaten);
  // ⚠ 輸入法沒吃 → 宿主自己處理。真的編輯框就是這樣。
  if (!o.eaten) {
    HostDefaultAction(doc, sk);
    o.host_did = true;
  }
  return o;
}

bool FileThere(const std::wstring& path) {
  return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool TouchFile(const std::wstring& path) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  ::WriteFile(h, "ready\r\n", 7, &written, nullptr);
  ::CloseHandle(h);
  return true;
}

bool ParseSeq(const std::wstring& seq, std::vector<SeqKey>* out) {
  size_t i = 0;
  while (i < seq.size()) {
    const wchar_t c = seq[i];
    if (c == L' ' || c == L',' || c == L'\t') {
      ++i;
      continue;
    }
    if (c == L'{') {
      const size_t close = seq.find(L'}', i);
      if (close == std::wstring::npos) {
        Say("!! --seq 裡有沒收尾的 '{'\n");
        return false;
      }
      const std::wstring name = seq.substr(i + 1, close - i - 1);
      SeqKey k;
      if (!NamedToKey(name, &k)) {
        Say("!! --seq 不認得 {%s} —— 請加進 kNamedKeys\n",
            Narrow(name).c_str());
        return false;
      }
      out->push_back(k);
      i = close + 1;
      continue;
    }
    SeqKey k;
    if (!AsciiToKey(c, &k.vk, &k.lparam)) {
      Say("!! '%s' 在目前的佈局上打不出來\n", Narrow(std::wstring(1, c)).c_str());
      return false;
    }
    k.label = std::wstring(1, c);
    k.ch = c;
    out->push_back(k);
    ++i;
  }
  return true;
}

void DumpTrace(const std::wstring& path) {
  Say("\n--- 瘦 DLL 的除錯記錄(%s)---\n", Narrow(path).c_str());
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    Say("  (檔案不存在 —— DLL 從來沒有被載入這個進程)\n");
    return;
  }
  char buf[4096];
  DWORD got = 0;
  std::string all;
  while (::ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0)
    all.append(buf, got);
  ::CloseHandle(h);
  std::fwrite(all.data(), 1, all.size(), stdout);
  std::fflush(stdout);
}

std::string ReadAll(const std::wstring& path) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::string();
  char buf[4096];
  DWORD got = 0;
  std::string all;
  while (::ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0)
    all.append(buf, got);
  ::CloseHandle(h);
  return all;
}

}  // namespace

// ── 把 DLL 抓在手上不放 ────────────────────────────────────────────
//
// 這不是測試 TSF,是**重現使用者的處境**:瀏覽器、檔案總管、Office 這些
// 程式在解除安裝的當下仍然握著 rime_tsf.dll,於是那個檔案刪不掉,
// Inno 只好把它排進「開機時刪除」的佇列,然後問使用者要不要重新啟動。
//
// 刻意用最笨的方式(LoadLibrary + 睡)——不註冊、不啟用 profile、不建 TSF 情境。
// 要重現的就只是「有一個進程握著這個檔案」這件事本身,多做任何一步都是
// 在測別的東西。
int HoldDll(const std::wstring& path, DWORD ms) {
  Say("LoadLibrary %s\n", Narrow(path).c_str());
  HMODULE h = ::LoadLibraryW(path.c_str());
  if (!h) {
    Say("!! 載入不了 err=%lu\n", static_cast<unsigned long>(::GetLastError()));
    return 1;
  }
  Say("已載入,握住 %lu 毫秒\n", static_cast<unsigned long>(ms));
  std::fflush(stdout);
  ::Sleep(ms);
  ::FreeLibrary(h);
  Say("已放開\n");
  return 0;
}

static int Run(int argc, wchar_t** argv);

int main(int, char**) {
  int argc = 0;
  LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (!argv) return 2;
  const int rc = Run(argc, argv);
  ::LocalFree(argv);
  return rc;
}

static int Run(int argc, wchar_t** argv) {
  LANGID langid = 0x0404;
  std::wstring keys;
  std::wstring seq;
  std::wstring pretext;
  // ⚠ Ctrl+空白鍵那顆保留鍵有沒有真的被 TSF 收下。見下面 CheckPreservedKey()。
  bool check_preserved = false;
  // ⚠ 註冊之外的另一半:那顆鍵**按下去**會不會走完整條路。見 PressPreservedKey()。
  bool press_preserved = false;
  // ⚠ 量測,不是斷言。見 MeasureShiftDelivery()。
  bool press_shift = false;
  // ⚠ 那一橫的「在場」訊號。見 PresenceWatch 的檔頭。
  bool watch_presence = false;
  std::wstring toggle_keys;
  std::wstring expect_toggle_doc;
  bool have_expect_toggle = false;
  std::wstring expect_doc;
  bool have_expect_doc = false;
  std::wstring expect;
  std::wstring trace_path;
  bool require_activate = false;
  bool require_eaten = false;
  DWORD wait_ms = 3000;
  std::wstring hold_dll;
  DWORD hold_ms = 0;
  std::wstring phase2_ready;
  std::wstring phase2_go;
  DWORD phase2_timeout_ms = 600000;
  DWORD phase2_relink_ms = 240000;
  // 第二階段那六顆鍵連跑幾趟。1 = 舊行為。
  int phase2_repeat = 5;

  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    if (a == L"--langid" && i + 1 < argc)
      langid = static_cast<LANGID>(::wcstol(argv[++i], nullptr, 0));
    else if (a == L"--keys" && i + 1 < argc) keys = argv[++i];
    else if (a == L"--seq" && i + 1 < argc) seq = argv[++i];
    else if (a == L"--pretext" && i + 1 < argc) pretext = argv[++i];
    else if (a == L"--expect-doc" && i + 1 < argc) {
      expect_doc = argv[++i];
      have_expect_doc = true;
    }
    else if (a == L"--expect" && i + 1 < argc) expect = argv[++i];
    else if (a == L"--trace" && i + 1 < argc) trace_path = argv[++i];
    else if (a == L"--check-preserved-key") check_preserved = true;
    else if (a == L"--press-shift") press_shift = true;
    else if (a == L"--watch-presence") watch_presence = true;
    else if (a == L"--press-preserved-key") press_preserved = true;
    else if (a == L"--toggle-mid-compose" && i + 1 < argc) {
      toggle_keys = argv[++i];
      press_preserved = true;
    }
    else if (a == L"--expect-toggle-doc" && i + 1 < argc) {
      expect_toggle_doc = argv[++i];
      have_expect_toggle = true;
    }
    else if (a == L"--require-activate") require_activate = true;
    else if (a == L"--require-eaten") require_eaten = true;
    else if (a == L"--wait-ms" && i + 1 < argc)
      wait_ms = static_cast<DWORD>(::wcstol(argv[++i], nullptr, 0));
    else if (a == L"--hold-dll" && i + 1 < argc) hold_dll = argv[++i];
    else if (a == L"--hold-ms" && i + 1 < argc)
      hold_ms = static_cast<DWORD>(::wcstol(argv[++i], nullptr, 0));
    else if (a == L"--phase2-ready-file" && i + 1 < argc) phase2_ready = argv[++i];
    else if (a == L"--phase2-go-file" && i + 1 < argc) phase2_go = argv[++i];
    else if (a == L"--phase2-timeout-ms" && i + 1 < argc)
      phase2_timeout_ms = static_cast<DWORD>(::wcstol(argv[++i], nullptr, 0));
    else if (a == L"--phase2-relink-ms" && i + 1 < argc)
      phase2_relink_ms = static_cast<DWORD>(::wcstol(argv[++i], nullptr, 0));
    else if (a == L"--phase2-repeat" && i + 1 < argc) {
      phase2_repeat = static_cast<int>(::wcstol(argv[++i], nullptr, 0));
      // 0 或負數會讓那個迴圈一趟都不跑,而「一趟都沒跑」在報表上與
      // 「每一趟都對」長得一模一樣(rounds_ok == phase2_repeat)。
      // 一個驗不到東西的段落綠起來,比它紅起來危險得多。
      if (phase2_repeat < 1) phase2_repeat = 1;
    }
    else {
      Say("未知參數: %s\n", Narrow(a).c_str());
      return 2;
    }
  }

  // --hold-dll 是一條完全獨立的路徑:不碰 COM、不碰 TSF、不寫除錯記錄。
  if (!hold_dll.empty()) return HoldDll(hold_dll, hold_ms ? hold_ms : 30000);

  if (trace_path.empty()) {
    wchar_t tmp[MAX_PATH] = {0};
    ::GetTempPathW(MAX_PATH, tmp);
    trace_path = std::wstring(tmp) + L"rime-tsf-host.log";
  }
  ::DeleteFileW(trace_path.c_str());
  // DLL 在**這個進程裡**讀這個環境變數,所以在載入它之前設好就行。
  ::SetEnvironmentVariableW(L"RIME_TSF_TRACE", trace_path.c_str());

  Say("=== rime_tsf_host:用真的 TSF 走一遍 ===\n");
  Say("  langid = 0x%04X\n", static_cast<unsigned>(langid));
  Say("  按鍵   = %s\n", keys.empty() ? "(不送)" : Narrow(keys).c_str());
  Say("  記錄   = %s\n\n", Narrow(trace_path).c_str());

  const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Step("CoInitializeEx(STA)", com);
  if (FAILED(com)) return 2;

  // 一個隱藏視窗。GetWnd 要回傳一個真的 HWND —— 有些 TSF 的路徑會拿它
  // 去問 DPI 與螢幕。
  WNDCLASSW wc{};
  wc.lpfnWndProc = ::DefWindowProcW;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.lpszClassName = L"RimeTsfHostWnd";
  ::RegisterClassW(&wc);
  HWND hwnd = ::CreateWindowExW(0, L"RimeTsfHostWnd", L"rime tsf host",
                                WS_OVERLAPPEDWINDOW, 0, 0, 400, 200, nullptr,
                                nullptr, wc.hInstance, nullptr);
  if (!hwnd) {
    Fail("建不出視窗 err=%lu", ::GetLastError());
  } else {
    // ⚠ 一定要真的顯示並搶到前景。
    //
    // TSF 的 key event sink 分「前景」與「非前景」兩種,而
    // AdviseKeyEventSink(..., fForeground = TRUE) 要求**呼叫執行緒擁有前景**。
    // 視窗藏著不顯示的話,文字服務掛前景 sink 會失敗,於是
    // ITfKeystrokeMgr::KeyDown 送進去的按鍵**不會被交給任何人** ——
    // 而回傳值仍然是 S_OK、pfEaten 仍然是 FALSE,看起來就像「輸入法不吃這顆鍵」。
    // 實測(CI run #58 的 install job)就是這樣:ActivateEx 過了、
    // 語言列按鈕也加上了,而六顆按鍵一顆都沒有到達 OnTestKeyDown。
    //
    // ⚠ **先把「動手之前前景是誰」印出來。** 這一行以前沒有,而它正是
    //   上一輪唯一缺的那一格證據:搶到了的時候我們只印自己的 hwnd,
    //   於是「本來就沒人佔著」與「從別人手上搶過來」在記錄上長得一模一樣。
    //   前者成立時 SetForegroundWindow 必定放行(那是它最後一條放行條件),
    //   後者成立時它必定被拒 —— 兩者要修的東西完全不同。
    SayForegroundIdentity("搶之前");
    ::ShowWindow(hwnd, SW_SHOWNORMAL);
    ::UpdateWindow(hwnd);
    ForceForeground(hwnd, "啟動");
    Pump(200);
    HWND fg = ::GetForegroundWindow();
    Say("  前景視窗 = %p(我們的是 %p)%s\n", static_cast<void*>(fg),
        static_cast<void*>(hwnd),
        fg == hwnd ? " —— 搶到了" : " —— **沒搶到**(非互動的工作階段?)");
    // 沒搶到的時候,「它是誰」是唯一會改變修法的那一格 —— 見
    // SayForegroundIdentity 的說明。搶到了就不必再印一次自己。
    if (fg != hwnd) SayForegroundIdentity("啟動");
  }

  ITfThreadMgr* thread_mgr = nullptr;
  HRESULT hr = ::CoCreateInstance(CLSID_TF_ThreadMgr, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
                                  (void**)&thread_mgr);
  Step("CoCreateInstance(CLSID_TF_ThreadMgr)", hr);
  if (FAILED(hr) || !thread_mgr) {
    Fail("這台機器上建不出 ITfThreadMgr —— TSF 在這個工作階段裡不可用。\n"
         "     (GitHub runner 是非互動的工作階段,這一步走不通是可能的。\n"
         "      走不通的話,這支程式驗不到任何東西,但**它必須明說**,\n"
         "      不可以靜靜地以 0 結束。)");
    return 3;
  }

  TfClientId client_id = TF_CLIENTID_NULL;
  hr = thread_mgr->Activate(&client_id);
  Step("ITfThreadMgr::Activate", hr);
  if (FAILED(hr)) {
    Fail("Activate 失敗,後面每一步都沒有意義");
    return 3;
  }

  ITfDocumentMgr* docmgr = nullptr;
  hr = thread_mgr->CreateDocumentMgr(&docmgr);
  Step("CreateDocumentMgr", hr);

  FakeDoc* doc = new FakeDoc(hwnd);
  // 「沒有組字時按退格」需要文件裡先有東西可以刪 —— 不然刪成功與
  // 刪失敗都是空字串,斷言等於沒有斷言。
  if (!pretext.empty()) {
    doc->text = pretext;
    doc->sel_start = doc->sel_end = static_cast<LONG>(pretext.size());
  }
  ITfContext* ctx = nullptr;
  TfEditCookie ec = 0;
  if (docmgr) {
    hr = docmgr->CreateContext(client_id, 0, static_cast<ITextStoreACP*>(doc),
                               &ctx, &ec);
    Step("CreateContext(帶 ITextStoreACP)", hr);
    if (SUCCEEDED(hr)) {
      hr = docmgr->Push(ctx);
      Step("ITfDocumentMgr::Push", hr);
    }
  }
  hr = thread_mgr->SetFocus(docmgr);
  Step("ITfThreadMgr::SetFocus", hr);
  Pump(200);

  // ── 那一橫的「在場」連線:先把服務端那一格架起來 ──────────────
  //
  // ⚠ **一定要在 ActivateProfile 之前。** DLL 是在那一步才被載入並
  //   ActivateEx 的,而在場連線就是 ActivateEx 開出去的 ——
  //   管道晚一步存在,那條連線就會連不上,而我們會把它讀成「沒有訊號」。
  PresenceWatch presence;
  bool presence_ready = false;
  if (watch_presence) {
    presence_ready = presence.Start();
    if (presence_ready)
      Say("  PresenceWatch:那條具名管道已由這支假宿主接管\n");
    else
      Say("  PresenceWatch:**架不起來**(err=%lu)\n",
          static_cast<unsigned long>(presence.error()));
  }

  // ── 啟用我們的語言設定檔 ────────────────────────────────────
  //
  // ⚠ 這一步是整支程式的重點,而且**旗標的組合會決定它成不成功**。
  //   所以這裡不是「試一種然後宣布結論」,是把幾種都試過去,
  //   並且把每一種的 HRESULT 都印出來 —— 走不通的時候,
  //   下一輪才知道是哪一種組合的問題,而不是「ActivateProfile 失敗」。
  ITfInputProcessorProfiles* profiles = nullptr;
  hr = ::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                          CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfiles,
                          (void**)&profiles);
  Step("CoCreateInstance(InputProcessorProfiles)", hr);

  const GUID* profile_guid = nullptr;
  for (int i = 0; i < kRimeProfileCount; ++i)
    if (kRimeProfiles[i].langid == langid) profile_guid = kRimeProfiles[i].guid;
  if (!profile_guid) {
    Fail("langid 0x%04X 不在我們註冊的三個語言裡", static_cast<unsigned>(langid));
    return 2;
  }

  bool activated = false;
  if (profiles) {
    ITfInputProcessorProfileMgr* mgr = nullptr;
    if (SUCCEEDED(profiles->QueryInterface(IID_ITfInputProcessorProfileMgr,
                                           (void**)&mgr)) &&
        mgr) {
      struct Attempt {
        const char* name;
        DWORD flags;
      };
      static const Attempt kAttempts[] = {
          {"FORPROCESS|DONTCARECURRENTINPUTLANGUAGE",
           TF_IPPMF_FORPROCESS | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE},
          {"FORSESSION|DONTCARECURRENTINPUTLANGUAGE",
           TF_IPPMF_FORSESSION | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE},
          {"DONTCARECURRENTINPUTLANGUAGE", TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE},
          {"0", 0},
      };
      for (const Attempt& a : kAttempts) {
        const HRESULT ar = mgr->ActivateProfile(
            TF_PROFILETYPE_INPUTPROCESSOR, langid, CLSID_RimeTextService,
            *profile_guid, nullptr, a.flags);
        Say("  ActivateProfile(%-42s) hr=0x%08lX\n", a.name,
            static_cast<unsigned long>(ar));
        if (SUCCEEDED(ar)) {
          activated = true;
          break;
        }
      }
      mgr->Release();
    } else {
      Fail("拿不到 ITfInputProcessorProfileMgr");
    }
    // 舊介面再試一次。ActivateLanguageProfile 在某些系統組態下是唯一走得通的。
    if (!activated) {
      const HRESULT ar = profiles->ActivateLanguageProfile(
          CLSID_RimeTextService, langid, *profile_guid);
      Step("ITfInputProcessorProfiles::ActivateLanguageProfile", ar);
      activated = SUCCEEDED(ar);
    }
    profiles->Release();
  }
  if (!activated) Fail("三種旗標與舊介面都啟用不了我們的語言設定檔");

  // 啟用之後再給一次焦點:有些系統要焦點事件才會真的把 TIP 叫起來。
  if (docmgr) thread_mgr->SetFocus(docmgr);
  Pump(wait_ms);

  // ── ActivateEx 到底有沒有被呼叫 ────────────────────────────
  const std::string trace = ReadAll(trace_path);
  const bool saw_load = trace.find("DLL 載入") != std::string::npos;
  const bool saw_activate = trace.find("ActivateEx 被呼叫") != std::string::npos;
  const bool key_sink_bad = trace.find("**沒掛上,收不到按鍵**") != std::string::npos;
  Say("\n--- 分層結論 ---\n");
  if (saw_load)
    Ok("系統把 rime_tsf.dll 載入了這個進程");
  else
    Fail("rime_tsf.dll **沒有被載入** —— 註冊那一段有問題(登錄檔 / CLSID / 路徑)");
  if (saw_activate)
    Ok("ActivateEx 被呼叫了 —— 文字服務真的被啟用");
  else if (require_activate)
    Fail("ActivateEx **沒有被呼叫** —— DLL 載入了,但系統沒有把它當成輸入法叫起來");
  else
    Say("  (ActivateEx 沒有被呼叫)\n");

  // ── 那一橫的訊號:ActivateEx 之後、**任何按鍵之前** ────────────
  //
  // ⚠ 位置就是斷言本身。放到送按鍵之後的話,第一顆按鍵的 EnsureReady()
  //   也會開一條連線 —— 那一條**本來就會有**,而它正好蓋住我們要問的
  //   那件事(「按鍵之前有沒有」)。這裡不是「順手也數一下」。
  if (watch_presence) {
    if (!presence_ready) {
      Say("PRESENCE_STUB=unavailable err=%lu\n",
          static_cast<unsigned long>(presence.error()));
      Fail("那條具名管道架不起來 —— 這一格什麼都沒驗到。"
           "err=5(ACCESS_DENIED)多半是這台機器上有一支真的服務佔著它");
    } else {
      // 在場連線是 DLL 在**背景執行緒**上開的,所以要給它一點時間。
      // ⚠ 上限 3 秒,而且只等到「看見為止」—— 沒有連線時**不可以**
      //   因為等得夠久就宣布成功。
      int seen = presence.accepted();
      for (int i = 0; i < 60 && seen == 0; ++i) {
        Pump(50);
        seen = presence.accepted();
      }
      Say("PRESENCE_STUB=ok\n");
      Say("PRESENCE_CLIENTS_BEFORE_KEYS=%d\n", seen);
      if (seen > 0)
        Ok("ActivateEx 之後、第一顆按鍵之前,服務端已經有 %d 條連線 —— "
           "那一橫的訊號是**啟用**,不是**按鍵**", seen);
      else
        Fail("ActivateEx 過了,服務端**一條連線都沒有**。"
             "那一橫要等使用者按下第一顆鍵才會出現(工單 #82 的另一半):"
             "clients_ 是 0,而唯一推得回去的 EnsureReady() 全部在按鍵路徑上");
    }
  }

  // ── 送按鍵 ─────────────────────────────────────────────────
  //
  // --seq 是逐鍵模式(功能鍵寫成 {BS});--keys 是舊的 ASCII 字串模式。
  // 兩者最後都變成同一串 SeqKey,走同一條路 —— 不然「矩陣測到的」與
  // 「§6c 測到的」會是兩段不同的程式碼。
  std::vector<SeqKey> plan;
  if (!seq.empty()) {
    if (!ParseSeq(seq, &plan)) return 2;
  } else {
    for (wchar_t ch : keys) {
      SeqKey k;
      if (!AsciiToKey(ch, &k.vk, &k.lparam)) {
        Fail("'%c' 在目前的佈局上打不出來", static_cast<char>(ch));
        continue;
      }
      k.label = std::wstring(1, ch);
      k.ch = ch;
      plan.push_back(k);
    }
  }

  int eaten_count = 0;
  int mismatch_count = 0;
  int host_handled = 0;
  // 第一階段這一段按鍵路徑:走了沒有,以及走的當下有沒有執行緒焦點。
  // ⚠ phase1_ran 是分開的一格。沒走過的話「瞎不瞎」根本沒有答案,
  //   而「沒有答案」不可以被當成「瞎的」拿去赦免 —— 那時一律計入。
  bool phase1_ran = false;
  bool phase1_focus = false;
  // ⚠ ks 提到迴圈外面:第二階段(升級之後)要用**同一個**介面再送一次鍵。
  //   在第一階段就 Release 掉的話,第二階段得自己再 QueryInterface 一次 ——
  //   那是另一條路,而「升級之後還能不能打字」要驗的正是原本那一條。
  ITfKeystrokeMgr* ks = nullptr;
  if (!plan.empty()) {
    hr = thread_mgr->QueryInterface(IID_ITfKeystrokeMgr, (void**)&ks);
    Step("QueryInterface(ITfKeystrokeMgr)", hr);
    if (SUCCEEDED(hr) && ks) {
      // ⚠ 送按鍵之前先把「TSF 覺得誰有焦點」印出來。
      //
      // ITfKeystrokeMgr 只會把按鍵交給**有執行緒焦點**的那一份文字服務。
      // IsThreadFocus 是 FALSE 的話,KeyDown 會乖乖回 S_OK / pfEaten=FALSE
      // 而完全不呼叫任何人 —— 看起來就像「輸入法不吃這顆鍵」,
      // 但其實它根本沒被問過。這兩件事要查的地方完全不同。
      BOOL thread_focus = FALSE;
      const HRESULT fhr = thread_mgr->IsThreadFocus(&thread_focus);
      Say("  IsThreadFocus = %d (hr=0x%08lX)\n", thread_focus ? 1 : 0,
          static_cast<unsigned long>(fhr));
      phase1_focus = thread_focus != FALSE;
      ITfDocumentMgr* focused = nullptr;
      const HRESULT ghr = thread_mgr->GetFocus(&focused);
      Say("  ThreadMgr::GetFocus = %p (hr=0x%08lX)%s\n",
          static_cast<void*>(focused), static_cast<unsigned long>(ghr),
          focused == docmgr ? " —— 就是我們那一份" : " —— **不是我們那一份**");
      if (focused) focused->Release();
      // 焦點不在我們身上就再要一次。有些系統要在 profile 啟用**之後**
      // 才吃得下 SetFocus。
      if (focused != docmgr && docmgr) {
        const HRESULT shr = thread_mgr->SetFocus(docmgr);
        Say("  重新 SetFocus hr=0x%08lX\n", static_cast<unsigned long>(shr));
        Pump(200);
      }
      // ── ⚠ 送按鍵之前把視窗前景搶回來 ────────────────────────────
      //
      // ActivateEx 到這裡之間會發生別的事:最重要的是**服務進程可能剛被
      // 我們自己啟動起來**,而它會建自己的視窗(設定視窗、系統匣)。
      // 那些視窗一出現就可能把前景拿走,而 TSF 只把按鍵交給擁有前景的
      // 那條執行緒 —— 沒有前景時 KeyDown 回 S_OK、pfEaten=FALSE,
      // 一顆鍵都不會到達文字服務,**而且不報錯**。
      //
      // 實測(CI run 31314468397 的 §5d):`前景視窗 = …030136
      // (我們的是 …05018E)`、`IsThreadFocus = 0`,六顆鍵一顆都沒到,
      // 而報表寫的是「第一次打字失敗」—— 一個看起來像產品壞掉的測試台問題。
      if (!thread_focus && hwnd) {
        // 甲:四種手段輪著上,而且每一種用了沒用、成了沒成都印出來。
        ForceForeground(hwnd, "送按鍵前");
        if (docmgr) thread_mgr->SetFocus(docmgr);
        Pump(300);
        BOOL again = FALSE;
        thread_mgr->IsThreadFocus(&again);
        Say("  重新搶前景之後 IsThreadFocus = %d(前景視窗 %p,我們的是 %p)\n",
            again ? 1 : 0, static_cast<void*>(::GetForegroundWindow()),
            static_cast<void*>(hwnd));
        // ⚠ 搶完之後**重新問**的這一個才算數。上一版只把一個全域旗標
        //   設起來就不管了,而底下真正送鍵的是這一刻的焦點狀態。
        phase1_focus = again != FALSE;
        if (!again) {
          SayForegroundIdentity("送按鍵前");
          FailKeyPath(
              /*blind=*/true,
              "送按鍵之前仍然拿不到執行緒焦點 —— TSF 不會把任何按鍵交給\n"
              "     文字服務,所以底下量到的東西**不能拿來判斷產品好壞**。\n"
              "     這是測試台/工作階段的問題,不是輸入法的問題。\n"
              "     上面那一段「前景視窗 = …」寫的就是**誰佔著前景**。");
        }
      }
      // 這一段按鍵路徑要走了。它「有沒有量到」由**這一刻**的焦點決定,
      // 而不由這一趟稍後(例如第二階段)發生的事決定。
      phase1_ran = true;
      g_keypath.NoteSegment(phase1_focus);
      if (check_preserved) CheckPreservedKey(ks);
      // ⚠ 擺在這裡(其他按鍵送出去**之前**)是刻意的:trace 的按鍵額度
      //   有限(text_service.cc 的 key_trace_budget_),被別的鍵用完的話
      //   這一支量到的 0 會是假的 0 —— 而假的 0 會讓我們做出「Shift
      //   這條路不通」的錯誤結論。
      if (press_shift) MeasureShiftDelivery(ks, trace_path);
      Say("\n--- 送按鍵 ---\n");
      for (const SeqKey& sk : plan) {
        const SendOutcome o = SendKeyThrough(ks, doc, sk);
        if (o.host_did) ++host_handled;
        // TestKeyDown 說吃、KeyDown 說不吃 = **這顆鍵在真實宿主上會消失**。
        // 這支假宿主因為有 HostDefaultAction 所以看不出來,但真的程式不會
        // 補救它 —— 所以在這裡明著數出來。
        // 這正是「可以打字,不能刪除」的形狀。
        if (o.test_eaten && !o.eaten) ++mismatch_count;

        // 這一行是給 verify_input_matrix.sh 解析的。欄位順序不要動。
        Say("  KEY %-8s vk=0x%02X test=%d down=%d host=%d doc=\"%s\"\n",
            Narrow(sk.label).c_str(), static_cast<unsigned>(sk.vk),
            o.test_eaten ? 1 : 0, o.eaten ? 1 : 0, o.host_did ? 1 : 0,
            Narrow(doc->text).c_str());
        if (o.eaten) ++eaten_count;
        Pump(60);
      }
    }
    Say("\n  文件內容 = \"%s\"\n", Narrow(doc->text).c_str());
    Say("  被吃掉的按鍵 = %d / %d(宿主自己處理了 %d 顆)\n", eaten_count,
        static_cast<int>(plan.size()), host_handled);
    Say("  DOC=\"%s\"\n", Narrow(doc->text).c_str());
    Say("  MISMATCH=%d\n", mismatch_count);
    if (mismatch_count > 0)
      Fail("有 %d 顆鍵是 TestKeyDown 說「吃」、KeyDown 說「不吃」。\n"
           "     在真的宿主裡那幾顆鍵會**直接消失**:宿主在測試那一趟就\n"
           "     放棄了自己的預設處理,而我們事後改口它收不到。\n"
           "     使用者回報的「可以打字,不能刪除」就是這個形狀。\n"
           "     修的地方是 windows/common/key_eat_policy.cc —— \n"
           "     沒有實作的鍵就不要在 OnTestKeyDown 宣告吃掉。",
           mismatch_count);
  }

  // 「一顆按鍵都沒有到達 OnTestKeyDown」與「到達了但沒吃」是兩件完全不同的事。
  // 前者要查 ActivateEx 那一段(key event sink 掛上了沒有),
  // 後者要查佈局或連線。把它們併成一句話,就是把人往錯的方向送。
  //
  // ⚠ 一定要**重新讀一次**記錄檔。上面那個 trace 是在送按鍵**之前**讀的
  //   (它要回答「ActivateEx 有沒有被呼叫」),按鍵行還沒寫進去。
  //   拿它來判斷「按鍵有沒有到達」會永遠說沒有 —— 實測 CI run #59 就是這樣:
  //   記錄檔裡明明有兩行 keysym=0x6E / 0x69,而這支程式報「一顆都沒到達」。
  //   一個讀舊資料的斷言比沒有斷言更糟:它會把人送去查一段其實是好的程式碼。
  // ⚠ 底下那幾條斷言量到的都是**第一階段那一段**的結果,所以它們的
  //   「瞎不瞎」也由第一階段決定 —— 不是由這一趟後來發生的事決定。
  //   沒走過按鍵那一段(phase1_ran=0)時一律當成「有量到」計入:
  //   那時失敗的原因與前景無關(例如 --expect 給了卻沒給 --keys)。
  const bool p1_blind = phase1_ran && !phase1_focus;
  const std::string trace_after_keys = ReadAll(trace_path);
  const bool saw_any_key =
      trace_after_keys.find("按鍵 vk=") != std::string::npos;
  if (!keys.empty() && !saw_any_key) {
    FailKeyPath(
        /*blind=*/p1_blind,
        "按鍵**一顆都沒有到達 OnTestKeyDown** —— 不是「不吃」,是根本沒收到。\n"
        "     要查的是 ActivateEx 裡的 AdviseKeyEventSink,不是佈局也不是連線。\n"
        "     記錄裡的 key sink 那一行會說掛上了沒有。");
    if (key_sink_bad || trace_after_keys.find("**沒掛上,收不到按鍵**") !=
                            std::string::npos)
      // ⚠ 這一條也記在按鍵那個帳上,而理由不是寬容:
      //   AdviseKeyEventSink(..., fForeground = TRUE) 本來就**要求呼叫執行緒
      //   擁有前景**。拿不到前景時它掛不上是必然的,不是缺陷。
      FailKeyPath(/*blind=*/p1_blind, "記錄明說 key event sink 兩種都掛不上");
  }
  if (require_eaten && eaten_count == 0 && saw_any_key)
    FailKeyPath(/*blind=*/p1_blind,
                "按鍵到達了 OnTestKeyDown,但一顆都沒有被吃掉。\n"
                "     照除錯記錄裡的 keysym 判斷是哪一段:\n"
                "       keysym=0x0  → 鍵盤佈局問不出字(win32_oracle.h)\n"
                "       keysym!=0   → 連不上服務(上面會有一行「連線失敗」)");

  if (have_expect_doc) {
    if (doc->text == expect_doc)
      Ok("文件內容 = \"%s\"(與預期相同)", Narrow(expect_doc).c_str());
    else
      FailKeyPath(/*blind=*/p1_blind,
                  "文件內容 = \"%s\",預期 \"%s\"\n"
                  "     ⚠ 斷言看的是**文件內容**,不是「有沒有被吃掉」——\n"
                  "     「吃掉了但什麼都沒做」正是這一輪要抓的缺陷。",
                  Narrow(doc->text).c_str(), Narrow(expect_doc).c_str());
  }

  if (!expect.empty()) {
    if (doc->text == expect)
      Ok("文件裡真的是「%s」—— 整條 TSF 路徑走通了", Narrow(expect).c_str());
    else
      FailKeyPath(/*blind=*/p1_blind, "文件裡是「%s」,預期「%s」",
                  Narrow(doc->text).c_str(), Narrow(expect).c_str());
  }

  // ══════════════════════════════════════════════════════════════
  //  組字**進行中**按下 Ctrl+空白鍵
  // ══════════════════════════════════════════════════════════════
  //
  // ⚠ common/hotkey_policy.h 的檔頭寫著這顆鍵存在的理由:
  //   「中英切換發生在句子中間」。也就是說**唯一該驗的情境就是這一個**。
  //
  // librime 在切到英數的當下會把手上那一段組字**上屏並清掉**
  // (實測:preedit="ni hao" → has_commit=1 commit="nihao" 組字中=0)。
  // 那份 commit 在 rs_snapshot_acquire 的當下就被消費,只現身一次 ——
  // 瘦 DLL 的 OnPreservedKey 沒有把它寫進文件,使用者打到一半的字就
  // **永久消失**,而畫面上不會有任何錯誤。
  //
  // 所以這一節的斷言全部看**文件內容**與**按鍵的下場**,不看任何內部狀態:
  //   1. 熱鍵之後,文件裡要有那段被上屏的字;
  //   2. 接著按退格要**刪得掉**(組字收乾淨了、engine_composing_ 沒卡住);
  //   3. 再按一次熱鍵要回得到中文(中英模式是行程層級的,留在英數的話
  //      後面每一節都會以「打不出中文」的樣子紅,而原因在這裡)。
  if (press_preserved) {
    Say("\n══ 組字進行中按 Ctrl+空白鍵 ══\n");
    if (!ks) {
      const HRESULT khr = thread_mgr->QueryInterface(IID_ITfKeystrokeMgr,
                                                     (void**)&ks);
      Step("QueryInterface(ITfKeystrokeMgr)", khr);
    }
    if (!ks || !ctx) {
      Fail("拿不到 ITfKeystrokeMgr(%p)或 ITfContext(%p)—— 這一節什麼都驗不到",
           static_cast<void*>(ks), static_cast<void*>(ctx));
    } else {
      std::vector<SeqKey> pre;
      if (!toggle_keys.empty() && !ParseSeq(toggle_keys, &pre)) return 2;
      if (!pre.empty()) {
        doc->text.clear();
        doc->sel_start = doc->sel_end = 0;
        int pre_eaten = 0;
        for (const SeqKey& sk : pre) {
          const SendOutcome o = SendKeyThrough(ks, doc, sk);
          if (o.eaten) ++pre_eaten;
          Pump(40);
        }
        Say("  TOGGLE_PRE_DOC=\"%s\"(引擎吃掉 %d/%d 顆)\n",
            Narrow(doc->text).c_str(), pre_eaten,
            static_cast<int>(pre.size()));
        if (pre_eaten != static_cast<int>(pre.size()) || doc->text.empty())
          Fail("那幾顆字母沒有進到組字裡(吃掉 %d/%d,文件=\"%s\")——\n"
               "     對照組不成立:這一節測的是「**組字進行中**切中英」,\n"
               "     沒有組字的話下面量到的東西一點意義都沒有。",
               pre_eaten, static_cast<int>(pre.size()),
               Narrow(doc->text).c_str());
      }

      const PreservedSend ps = PressPreservedKey(ks, ctx, trace_path);
      Say("  PRESERVEDKEY_ROUTE=%s\n", ps.route);
      Say("  PRESERVEDKEY_DELIVERED=%d\n", ps.delivered ? 1 : 0);
      Say("  TOGGLE_DOC=\"%s\"\n", Narrow(doc->text).c_str());
      if (!ps.delivered)
        Fail("Ctrl+空白鍵**沒有到達 OnPreservedKey**:真的按鍵與\n"
             "     SimulatePreservedKey 兩條路都沒有讓瘦 DLL 寫下任何一行記錄。\n"
             "     註冊是好的(IsPreservedKey 說得到),壞的是後面那一段 ——\n"
             "     這條功能可以整個死掉而沒有人發現,而這就是那個樣子。");

      if (have_expect_toggle) {
        if (doc->text == expect_toggle_doc)
          Ok("組字中切中英:打到一半的「%s」被寫進文件了",
             Narrow(expect_toggle_doc).c_str());
        else
          Fail("組字中按 Ctrl+空白鍵之後文件是「%s」,預期「%s」。\n"
               "     librime 在切到英數的當下把手上那一段**上屏並清掉**,而那份\n"
               "     commit 在 rs_snapshot_acquire 的當下就被消費 —— 只現身一次。\n"
               "     瘦 DLL 的 OnPreservedKey 把快照丟掉的話,使用者打到一半的字\n"
               "     就永久消失,而螢幕上那段底線組字還留著不收尾。\n"
               "     修的地方是 tsf/text_service.cc 的 OnPreservedKey:\n"
               "     那份快照要走與 HandleKey 同一段 ApplyPlan。",
               Narrow(doc->text).c_str(), Narrow(expect_toggle_doc).c_str());

        // ── 組字收乾淨了沒有:用**退格**問 ────────────────────────
        //
        // 不去讀任何內部狀態,問使用者感覺得到的那件事。engine_composing_
        // 停在 true 的話,key_eat_policy 會把退格判成「組字中的鍵」而由我們
        // 吃掉,可是引擎手上根本沒有組字 —— 那一格就是黑洞:按下去什麼都
        // 不會發生。使用者的說法會是「可以打字,不能刪除」。
        SeqKey bs;
        if (NamedToKey(L"BS", &bs) && !doc->text.empty()) {
          const std::wstring want_bs = doc->text.substr(0, doc->text.size() - 1);
          const std::wstring had = doc->text;
          SendKeyThrough(ks, doc, bs);
          Pump(60);
          Say("  TOGGLE_BS_DOC=\"%s\"\n", Narrow(doc->text).c_str());
          if (doc->text == want_bs)
            Ok("切完之後退格刪得掉字(組字收乾淨了,engine_composing_ 沒卡在 true)");
          else
            Fail("切完中英之後按退格,文件從「%s」變成「%s」,預期「%s」。\n"
                 "     這是那個黑洞的形狀:引擎那一側的組字被切中英清掉了,而\n"
                 "     瘦 DLL 的 engine_composing_ 還停在 true,於是退格被宣告\n"
                 "     吃掉、又沒有人處理它。",
                 Narrow(had).c_str(), Narrow(doc->text).c_str(),
                 Narrow(want_bs).c_str());
        }

        // ── 再按一次要回得到中文 ──────────────────────────────────
        //
        // ⚠ 這不是收尾禮貌。中英是**行程層級**的模式(service/engine.h 的
        //   SetAsciiModeAll),留在英數的話,這支宿主結束之後每一個新開的
        //   session 都是英數 —— 後面每一節都會以「打不出中文」的樣子紅,
        //   而真正的原因在這裡。
        //
        // 判準不是旗標而是**同一顆鍵的下場不一樣**:中文模式下 'n' 進組字、
        // Esc 收得掉(文件回到原樣);英數模式下那個 'n' 是我們自己寫進文件
        // 的字,Esc 收不掉它。
        const PreservedSend back = PressPreservedKey(ks, ctx, trace_path);
        Say("  PRESERVEDKEY_BACK_ROUTE=%s\n", back.route);
        const std::wstring base = doc->text;
        SeqKey probe;
        SeqKey esc;
        if (AsciiToKey(L'n', &probe.vk, &probe.lparam) &&
            NamedToKey(L"ESC", &esc)) {
          probe.label = L"n";
          probe.ch = L'n';
          SendKeyThrough(ks, doc, probe);
          Pump(60);
          SendKeyThrough(ks, doc, esc);
          Pump(60);
          Say("  TOGGLE_BACK_DOC=\"%s\"\n", Narrow(doc->text).c_str());
          if (doc->text == base)
            Ok("再按一次切回中文了('n' 進了組字,Esc 收得掉)");
          else
            Fail("再按一次**沒有**切回中文:打 'n' 再按 Esc 之後文件是「%s」,\n"
                 "     預期「%s」。英數模式下那個 'n' 是我們自己寫進文件的字,\n"
                 "     Esc 收不掉它。\n"
                 "     ⚠ 中英是行程層級的模式:留在英數的話,後面每一節都會\n"
                 "     以「打不出中文」的樣子紅,而真正的原因在這裡。",
                 Narrow(doc->text).c_str(), Narrow(base).c_str());
        }
      }
    }
  }

  // ── 這個進程到底載入了哪一份 DLL ──────────────────────────────
  //
  // 升級之後同一台機器上會同時有兩份:新的在 rime_tsf.dll,舊的被改名成
  // rime_tsf.dll.old-<時間戳>(見 docs/decisions/no-restart.md)。
  // 「新開的進程有沒有用到新版」這個問題,只有在進程**裡面**答得出來。
  {
    HMODULE m = ::GetModuleHandleW(L"rime_tsf.dll");
    wchar_t p[MAX_PATH] = {0};
    if (m && ::GetModuleFileNameW(m, p, MAX_PATH))
      Say("  LOADED_TSF_DLL=%s\n", Narrow(p).c_str());
    else
      Say("  LOADED_TSF_DLL=(沒有載入)\n");
  }

  // ══════════════════════════════════════════════════════════════
  //  第二階段:升級發生在**這個進程還活著**的時候
  // ══════════════════════════════════════════════════════════════
  //
  // 使用者實測回報「我沒重啟也能用」。他機器上的真實狀態是:
  // 已經開著的程式握著**舊的** DLL 映像,而服務進程已經是**新的**。
  // 這一段就是把那個狀態做出來,然後問一句話:那樣還打得出字嗎?
  //
  // 走不通的症狀不是「壞掉」,是「有些程式能打字、有些不能」——
  // 而使用者完全無法理解為什麼。那比全部壞掉更難查。
  if (!phase2_go.empty()) {
    Say("\n══ 第二階段:舊的 DLL 映像 + 新的服務 ══\n");
    if (!ks || plan.empty()) {
      Fail("第二階段需要 --keys 或 --seq(要有東西可以打)");
    } else if (expect.empty()) {
      Fail("第二階段需要 --expect —— 沒有它就沒有斷言,那一格等於沒在測");
    } else {
      bool go = false;
      if (!phase2_ready.empty()) {
        if (TouchFile(phase2_ready))
          Say("  已寫出就緒訊號 %s(外面可以開始裝新版了)\n",
              Narrow(phase2_ready).c_str());
        else
          Fail("寫不出就緒訊號 %s —— 外面的腳本會一直等下去",
               Narrow(phase2_ready).c_str());
      }
      Say("  等 %s 出現(上限 %lu 毫秒)…\n", Narrow(phase2_go).c_str(),
          static_cast<unsigned long>(phase2_timeout_ms));
      const DWORD go_deadline = ::GetTickCount() + phase2_timeout_ms;
      for (;;) {
        if (FileThere(phase2_go)) {
          go = true;
          break;
        }
        if (::GetTickCount() >= go_deadline) break;
        // ⚠ 這裡要抽訊息,不能單純 Sleep。這個進程仍然是一個 TSF 宿主,
        //   而 TSF 有一部分工作靠視窗訊息推動 —— 幾分鐘不抽訊息的話,
        //   等升級結束之後它已經不是一個正常的宿主了,
        //   而後面量到的「連不回去」就會是我們自己造成的。
        Pump(200);
      }
      if (!go) {
        Fail("等不到 %s —— 外面那一步沒有完成,第二階段什麼都沒驗到",
             Narrow(phase2_go).c_str());
      } else {
        Say("  收到訊號:新版已經裝好,而這個進程手上的 DLL 映像還是舊的。\n");

        // ── ⚠ 先把前景搶回來 ──────────────────────────────────
        //
        // 等升級的那幾分鐘裡,別的東西可能拿走了前景(安裝程式、服務、
        // 系統的通知)。而 TSF 只把按鍵交給**擁有前景**的那條執行緒:
        // 沒有前景時 KeyDown 回 S_OK、pfEaten=FALSE,一顆鍵都不會到達
        // 文字服務,**而且不報錯**。
        //
        // 少了這一段,一次前景被搶走會被記成「舊的 DLL 連不回新的服務」——
        // 一個把人送去查版本協商、而其實那一段完全是好的的結論。
        // (實測過:第一版把這支宿主丟到背景跑,量到的正是那個假結論。)
        ForceForeground(hwnd, "第二階段");
        if (docmgr) thread_mgr->SetFocus(docmgr);
        Pump(300);
        BOOL phase2_focus = FALSE;
        {
          thread_mgr->IsThreadFocus(&phase2_focus);
          const HWND fg = ::GetForegroundWindow();
          Say("  PHASE2_THREAD_FOCUS=%d(前景視窗 %p,我們的是 %p)\n",
              phase2_focus ? 1 : 0, static_cast<void*>(fg),
              static_cast<void*>(hwnd));
          if (!phase2_focus) SayForegroundIdentity("第二階段");
        }
        // ⚠ 這裡**不再**去設一個全域旗標。第二階段瞎不瞎是第二階段
        //   自己的事,而底下每一筆失敗都自己帶著這個答案走
        //   (見 common/keypath_ledger.h 的檔頭)。

        // ── 這個進程手上那一份 DLL,現在躺在磁碟的哪裡 ──────────────
        //
        // 升級把舊檔**改名挪開**了(見 docs/decisions/no-restart.md)。
        // 改名不影響已經映射進來的映像,所以這個進程手上仍然是舊的那一份
        // —— 而「仍然是舊的」這句話,只有在進程裡面才證明得了。
        //
        // ⚠ GetModuleFileNameW 回的是**載入當下記下來的路徑**,它不會跟著
        //   改名走。真正跟著走的是映射區段背後那個檔案物件,問它的是
        //   GetMappedFileName。兩個都印,而且印出來的名字不一樣才是對的：
        //   一個是舊路徑、一個是 .old- 那個新名字。
        {
          HMODULE m = ::GetModuleHandleW(L"rime_tsf.dll");
          wchar_t p[MAX_PATH] = {0};
          if (m && ::GetModuleFileNameW(m, p, MAX_PATH))
            Say("  PHASE2_DLL_MODULE_PATH=%s\n", Narrow(p).c_str());
          else
            Say("  PHASE2_DLL_MODULE_PATH=(問不到)\n");
          // K32GetMappedFileNameW 住在 kernel32,用 GetProcAddress 拿 ——
          // 這樣就不必管 mingw 的 psapi.h 有沒有宣告它。
          typedef DWORD(WINAPI * MappedNameFn)(HANDLE, LPVOID, LPWSTR, DWORD);
          HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
          MappedNameFn mapped =
              k32 ? reinterpret_cast<MappedNameFn>(reinterpret_cast<void*>(
                        ::GetProcAddress(k32, "K32GetMappedFileNameW")))
                  : nullptr;
          wchar_t mp[MAX_PATH] = {0};
          if (m && mapped && mapped(::GetCurrentProcess(),
                                    reinterpret_cast<LPVOID>(m), mp, MAX_PATH))
            Say("  PHASE2_DLL_MAPPED_PATH=%s\n", Narrow(mp).c_str());
          else
            Say("  PHASE2_DLL_MAPPED_PATH=(問不到 —— 這一格只是佐證,"
                "不影響判定)\n");
        }

        // ── 重新連上服務 ────────────────────────────────────────
        //
        // 升級把舊的服務進程停掉了。舊的 DLL 必須自己:
        //   開管道失敗 → 啟動新的 rime_service.exe → 重新握手
        //   (版本協商,必要時降級到 v1)→ 重新開 session。
        //
        // ══ ⚠ 這一段被改過,而理由要寫在這裡 ═══════════════════════
        //
        // 原本只有一條量法:「送一顆 n 看它吃不吃」。它的優點是量的是
        // 使用者感覺得到的那一件事;它的**致命缺點**是那條路要經過 TSF,
        // 而 TSF 只把按鍵交給有執行緒焦點的那一份文字服務。
        //
        // run 31896143629 的 §13c 就死在這個缺口上:PHASE2_THREAD_FOCUS=0,
        // 於是 599 次探測(300235 ms ÷ 599 = 501 ms,正好是底下那個
        // 固定的 Pump(500),不是任何退避)一顆鍵都沒有離開這個進程 ——
        // 而服務端同一段時間的記錄是
        //     [pipe] 連線 #1 離場 存活=300829ms 握手=1 session=0 按鍵=0
        // 連線一直在、握手成立、零顆按鍵。也就是說**管道那一側全程是好的**,
        // 而報表印出來的結論是「舊的 DLL 連不回新的服務」,底下還列了
        // 三條要去查的方向(管道名 / 版本協商 / rime_shell ABI)——
        // 那三條全部是好的。一個把人送去改沒壞的東西的紅字,
        // 比沒有這一格更糟,而且每次紅都多燒五分鐘 CI。
        //
        // ══ 這一輪把「事實」那一半補成完整的一趟 ═══════════════════
        //
        // 上一輪只問到「管道開得起來嗎」(ServicePipeReachable)。那只是
        // 三分之一:開得起來只證明有人在那個名字上聽著,證明不了兩邊談得攏,
        // 更證明不了引擎給得出 session。於是「舊的 DLL 連不回新的服務」
        // 這一句仍然蓋著三件修法完全不同的事。
        //
        // 現在改成走完整條:**管道 → 握手 → session → 收工**。
        // 三句話各自獨立,任何一句不成立都紅,而且紅字直接指到那一件事。
        // 這一整段不經過 TSF,所以前景搶不搶得到都答得出來 ——
        // 那正是它存在的理由。它的邊界寫在 ProbeServiceLink 的檔頭。
        LinkProbe link;
        // profile_guid 留空:protocol.h 說它只作診斷,服務端不得據此改行為。
        //
        // ⚠ 這支探針自己會在引擎那條 FIFO 上排一件**慢**工作:它建了一個
        //   session 然後把管道關掉,而 service/pipe_server.cc 在連線離場時
        //   `EndSessionAsync(session)` —— 那是「把使用者詞典寫回去」,
        //   engine.cc 說它是這條路上最慢的一步之一。也就是說這一段在真正
        //   開始量按鍵之前,主動在引擎前面塞了一件慢工作。
        //
        //   它現在不必搬走,而理由是**下面那個判準已經把它等掉了**:
        //   新的 relink 判準要求引擎真的組出字(送 'n' → 文件變非空 →
        //   送 ESC → 文件變空),而那要兩趟成功的引擎往返 —— 那件慢工作
        //   在那之前必然已經跑完。⚠ 若哪天判準又改回「吃掉就算數」,
        //   這一段就必須搬到打字**後面**,或明著送 SESSION_END 並等它收完。
        ProbeServiceLink(&link, langid, std::string());
        SayLinkProbe("PHASE2", link);
        // 舊欄位保留:verify_installer.sh 與人的眼睛都還在找它。
        const bool pipe_ok = link.pipe;
        const DWORD pipe_err = link.pipe_err;
        Say("  PHASE2_PIPE_REACHABLE=%d(err=%lu)\n", pipe_ok ? 1 : 0,
            static_cast<unsigned long>(pipe_err));

        SeqKey probe_key;
        probe_key.label = L"n";
        probe_key.ch = L'n';
        SeqKey esc_key;
        const bool have_probe =
            AsciiToKey(L'n', &probe_key.vk, &probe_key.lparam);
        const bool have_esc = NamedToKey(L"ESC", &esc_key);
        if (!have_probe) Fail("這個佈局上打不出 'n',第二階段沒辦法探測");
        bool relinked = false;
        int probes = 0;
        const DWORD t0 = ::GetTickCount();
        const DWORD relink_deadline = t0 + phase2_relink_ms;
        // ══ ⚠ 判準換掉了,而理由要寫在這裡 ═══════════════════════════
        //
        // 舊的判準是「這顆 'n' 被吃掉了嗎」,註解寫著
        //     吃掉了 = 連上、握手過、session 開好、**正在組字**
        // 而最後那一句**從來就不成立**,只是以前碰巧看不出來:
        // tsf/text_service.cc 的 `!handled → SelfInsertChar` 這條路上,
        // 一顆鍵在引擎完全沒回話的情況下照樣會被吃掉。這一輪把那條路改成
        // 「引擎沒回答就吃掉、什麼都不做」之後,它更是**必然**被吃掉。
        //
        // 也就是說舊判準會在「DLL 連上了、但引擎還在部署」的那一刻宣告
        // relinked,然後立刻開始打那六顆鍵 —— 前兩顆落在引擎回來之前,
        // 打出來就是 log4 那個「ni好」。`連線就緒 … session=5` 在
        // 21:34:22.210,而文件內容 dump 在 22.899:六顆鍵全落在那 600ms 裡。
        //
        // ── 新判準:送 'n' → 文件真的多了東西 → 送 ESC → 文件真的空了 ──
        //
        //   · 引擎**真的在組字** → 'n' 之後文件是 preedit「n」(非空),
        //     ESC 取消組字 → 文件回到空。**兩件都成立才算數。**
        //   · 引擎沒回答(吃掉不動)→ 'n' 之後文件仍然是空的 → 第一關就不過。
        //   · 引擎回答了但不要這顆鍵(英數模式,我們補字元)→ 'n' 之後
        //     文件是字面的「n」,但 ESC **清不掉它** → 第二關擋下來。
        //
        // ⚠ 這不是把斷言放寬,是**把等待改成等對的東西**:這一格量的仍然
        //   是「升級之後打得出你好嗎」,只是不再在引擎還沒回話時就開跑。
        //
        // ⚠ 舊版在迴圈**外面**無條件 `doc->text.clear()`,而那正好把唯一
        //   能分辨這三種情形的證據抹掉。現在判斷做在清掉之前。
        while (phase2_focus && have_probe &&
               ::GetTickCount() < relink_deadline) {
          doc->text.clear();
          doc->sel_start = doc->sel_end = 0;
          ++probes;
          const SendOutcome o = SendKeyThrough(ks, doc, probe_key);
          const bool doc_grew = !doc->text.empty();
          bool esc_cleared = false;
          if (o.eaten && doc_grew && have_esc) {
            SendKeyThrough(ks, doc, esc_key);
            esc_cleared = doc->text.empty();
          }
          if (o.eaten && doc_grew && esc_cleared) {
            relinked = true;
            break;
          }
          // ⚠ 沒過關的那幾趟也要留下痕跡,而且要說得出是**哪一關**沒過。
          //   少了它,「探了 480 次」在報表上是一個沒有內容的數字,而這
          //   三種情形要修的地方完全不同。額度 6 行,免得 300 秒刷成洪水。
          if (probes <= 6) {
            Say("  PHASE2_PROBE#%d eaten=%d doc_grew=%d esc_cleared=%d "
                "doc=\"%s\"\n",
                probes, o.eaten ? 1 : 0, doc_grew ? 1 : 0,
                esc_cleared ? 1 : 0, Narrow(doc->text).c_str());
          }
          Pump(500);
        }
        doc->text.clear();
        doc->sel_start = doc->sel_end = 0;
        const DWORD relink_ms = ::GetTickCount() - t0;
        Say("  PHASE2_RELINK_MS=%lu\n", static_cast<unsigned long>(relink_ms));
        Say("  PHASE2_PROBES=%d\n", probes);
        // ⚠ 這一行是給人看的,也是給 §13c 的報表抓的:它把「跑了但失敗」
        //   與「前提不成立所以根本沒跑」分開。少了它,PHASE2_PROBES=0
        //   會被誤讀成「一次都沒試就放棄」。
        Say("  PHASE2_PROBE_SKIPPED=%d\n", phase2_focus ? 0 : 1);

        // ── 三句話,不是一句 ──────────────────────────────────────
        //
        // ⚠ 順序就是線路上的順序:管道 → 握手 → session。前面那一句不成立
        //   時後面兩句根本沒機會被問到,所以只報最前面那一個 ——
        //   一次報三個「失敗」會讓人以為壞了三件事。
        //
        // ⚠ 這三句**全部不需要前景**,也就是說焦點搶不搶得到都答得出來。
        //   它們才是「去查管道名 / 版本協商 / ABI」那三條的門票,
        //   而底下按鍵那一段**不是**。
        Say("  ↑ 上面這三句話問的是:**這個握著舊 DLL 映像的進程**,\n"
            "    跟已經換成新版的那支服務,線路上還講不講得通。\n"
            "    它們就是使用者升級之後那些沒關掉的程式(檔案總管、瀏覽器)\n"
            "    的處境,而且一句都不經過 TSF。\n");
        JudgeLinkProbe(link);

        // ── 按鍵那一段:量得到就判,量不到就明說 ────────────────────
        //
        // ⚠ 它的地位在這裡講清楚:**它是加分題,不是判準**。
        //   判準是上面那三句話(它們不需要前景)。這一段需要執行緒焦點,
        //   而焦點在非互動的工作階段裡拿不拿得到不由產品決定 ——
        //   拿不到時它量到的每一個數字都是「沒有答案」,不是「壞掉」。
        //
        // ⚠ 不管量到沒量到,底下這一行**每一次都印**。這是刻意的取捨,
        //   而一個看不見的取捨等於沒有取捨。
        // 第二階段這一段按鍵路徑的結論就在下面這個 if 鏈裡,
        // 而它有沒有量到只由 phase2_focus 決定 —— 與第一階段無關。
        g_keypath.NoteSegment(phase2_focus != FALSE);
        if (!phase2_focus) {
          Say("  PHASE2_KEYPATH_MEASURED=0\n");
          Say("  ⚠ 第二階段的按鍵路徑這一次沒有量到(拿不到前景:%s)\n",
              ForegroundOwnerBrief().c_str());
          Say("     這一格的判定由上面那三句話(管道 / 握手 / session)決定,\n"
              "     它們不經過 TSF。要讓按鍵那一段也量得到,要修的是**誰佔著\n"
              "     前景**(上一節的收尾、或這台 runner 上別的東西),\n"
              "     不是管道名、不是版本協商、也不是 rime_shell 的 ABI。\n");
        } else if (!relinked) {
          Say("  PHASE2_KEYPATH_MEASURED=1\n");
          // ⚠ 這一條在 phase2_focus==TRUE 的分支裡:確實量到了。
          FailKeyPath(
              /*blind=*/!phase2_focus,
              "按鍵探測在 %lu 毫秒 / %d 次之內沒有成功,而**線路整條是通的**\n"
              "     (管道 / 握手 / session 三句話都成立)。也就是說連線那一段\n"
              "     是好的,壞在更後面:session 開了但按鍵沒有回應。\n"
              "     要看的是服務端 [pipe] 那幾行的 按鍵= / 逾時= 兩個數字,\n"
              "     以及瘦 DLL 的除錯記錄。",
              static_cast<unsigned long>(relink_ms), probes);
        } else {
          Say("  PHASE2_KEYPATH_MEASURED=1\n");
          Ok("舊的 DLL 重新連上了新的服務(%lu 毫秒、%d 次嘗試)",
             static_cast<unsigned long>(relink_ms), probes);
          // ── ⚠ 跑 N 趟,不是一趟 ──────────────────────────────────
          //
          //   f1fe449 綠、00e7273 紅,而 `git show --stat 00e7273` 只有
          //   windows/verify_installer.sh 十行 —— **兩者的產品程式碼完全
          //   相同**。同一份二進位一趟綠一趟紅 = 競態,而跑一次的結果
          //   不管綠紅都不構成證據:綠的那一次只證明「這一次沒踩到」。
          //
          //   所以這一格跑 N 趟,**任何一趟不對就紅**。它不會讓一個真的
          //   壞掉的產品變綠(那樣每一趟都會錯),只會讓一個間歇的缺陷
          //   從「三次裡紅一次」變成「幾乎每次都紅」——
          //   而那正是一個守門該有的樣子。
          int rounds_ok = 0;
          std::wstring bad_doc;
          int bad_round = 0;
          for (int round = 1; round <= phase2_repeat; ++round) {
            doc->text.clear();
            doc->sel_start = doc->sel_end = 0;
            Say("\n--- 第二階段送按鍵(第 %d/%d 趟,同一段程式碼、同一個 ks)"
                "---\n",
                round, phase2_repeat);
            for (const SeqKey& sk : plan) {
              const SendOutcome o = SendKeyThrough(ks, doc, sk);
              Say("  KEY %-8s vk=0x%02X test=%d down=%d host=%d doc=\"%s\"\n",
                  Narrow(sk.label).c_str(), static_cast<unsigned>(sk.vk),
                  o.test_eaten ? 1 : 0, o.eaten ? 1 : 0, o.host_did ? 1 : 0,
                  Narrow(doc->text).c_str());
              Pump(60);
            }
            Say("  PHASE2_DOC=\"%s\"(第 %d/%d 趟)\n",
                Narrow(doc->text).c_str(), round, phase2_repeat);
            if (doc->text == expect) {
              ++rounds_ok;
            } else if (bad_round == 0) {
              bad_round = round;
              bad_doc = doc->text;
            }
          }
          Say("  PHASE2_ROUNDS_OK=%d/%d\n", rounds_ok, phase2_repeat);
          if (rounds_ok == phase2_repeat)
            Ok("**升級之後,同一個進程用舊的 DLL 照樣打得出「%s」**"
               "(連續 %d 趟都對)",
               Narrow(expect).c_str(), phase2_repeat);
          else
            // ⚠ 這一條也在 phase2_focus==TRUE 的分支裡,而且它是**送完鍵、
            //   比對過文件內容**才記的 —— 那是這條路上最硬的一筆量測。
            //   上一版把它記進「瞎的」那一格,於是 rc=0。
            FailKeyPath(/*blind=*/!phase2_focus,
                        "第二階段第 %d 趟打出來的是「%s」,預期「%s」"
                        "(%d 趟裡對了 %d 趟)——\n"
                        "     舊的 DLL 連上了新的服務,但打出來的東西不對。\n"
                        "     那比連不上更糟:使用者不會發現。\n"
                        "     ⚠ 半串拼音半串中文(例如「ni好」)= 引擎沒有\n"
                        "       回答那幾顆鍵,而 DLL 把原始字母補進了文件。\n"
                        "       要看的是服務端 service.log 的 [pipe] 那幾行\n"
                        "       (按鍵= / 逾時= / KEY_MS),以及瘦 DLL 記錄裡\n"
                        "       「引擎沒有回答這顆鍵」那幾行。",
                        bad_round, Narrow(bad_doc).c_str(),
                        Narrow(expect).c_str(), phase2_repeat, rounds_ok);
        }
        // 上面那三句話全部成立,才有資格赦免按鍵那一段量不到的失敗。
        g_phase2_ran = true;
        g_keypath.set_link_verified(link.pipe && link.handshake &&
                                    link.session);
      }
    }
  }

  if (ks) ks->Release();

  DumpTrace(trace_path);

  // 收尾。Deactivate 走了才會看到 DLL 的 Deactivate 記錄。
  if (ctx) ctx->Release();
  if (docmgr) {
    docmgr->Pop(TF_POPF_ALL);
    docmgr->Release();
  }
  thread_mgr->Deactivate();
  thread_mgr->Release();
  doc->Release();
  if (hwnd) ::DestroyWindow(hwnd);
  ::CoUninitialize();

  // ── 焦點拿不到、而且沒有第二階段:那就在這裡問一次那三句話 ──────
  //
  // ⚠ 為什麼不是只有 §13c 需要它。§13d(升級之後新開一個宿主)、§5d、
  //   §6c 走的是同一支程式、同一條 TSF 路徑,前景搶不到的時候它們一樣
  //   量不到東西 —— 上一輪 §13d 就跟著 §13c 一起紅,而紅字講的是
  //   「新開的宿主打不出你好」。那句話同樣把人送去查一段好的程式碼。
  //
  //   所以判準統一成一句:**焦點拿不到的時候,改用不需要前景的那條路
  //   問同一個問題**;那條路走不通就紅在那條路上,走得通才赦免按鍵。
  //   沒有第二階段的跑法也拿得到這一格,而且它一樣會紅在該紅的地方。
  if (g_keypath.any_blind_segment() && !g_phase2_ran) {
    Say("\n══ 拿不到前景 —— 改用不經過 TSF 的那條路問一次 ══\n");
    LinkProbe link;
    // profile_guid 留空:protocol.h 說它只作診斷,服務端不得據此改行為。
    ProbeServiceLink(&link, langid, std::string());
    SayLinkProbe("LINK", link);
    JudgeLinkProbe(link);
    g_keypath.set_link_verified(link.pipe && link.handshake && link.session);
  }

  // ── 按鍵那一段的帳,現在結 ────────────────────────────────────────
  //
  // 赦免是**一筆一筆**判的,而且三個條件都要成立(見
  // common/keypath_ledger.h::excused_fails):
  //   1. **記下那一筆的當下**拿不到執行緒焦點 —— 也就是說它量到的是
  //      「沒有答案」,不是「壞掉」;
  //   2. 這一趟確實有一段是瞎的 —— 記帳自己壞了的時候不赦免任何東西;
  //   3. 另一條不需要前景的量法(管道 / 握手 / session 三句話)整條走通了
  //      —— 也就是說這一格**確實有東西被量到**。
  //
  // ⚠ 第 1 條問的是**那一筆**,不是這一趟。上一版問的是一個跨階段的黏滯
  //   全域旗標,於是「第一階段瞎、第二階段搶回前景並量到打錯字」那一筆
  //   也被赦免掉,rc=0。那條假綠現在由 tests/test_keypath_ledger.cc 的
  //   four_combinations_blind_then_seen_failure_is_not_excused 守著。
  //
  // ⚠ 少了第 3 條就一律計入。這一點是整段最重要的一行:一個什麼都沒驗到
  //   的段落綠起來,比它紅起來危險得多。§5c / §5d / §6c 那些沒有第二階段
  //   的跑法拿不到焦點時,乙走不通就仍然會紅。
  //
  // ⚠ 不管赦不赦免,那些失敗**上面已經逐條印出來了**,而且下面這一行
  //   每一次都印。看不見的取捨等於沒有取捨。
  const int excused = g_keypath.excused_fails();
  // **無條件印。** 這一行是整個取捨的帳單,而一張只在某些情況下才出現的
  // 帳單,等於沒有帳單。
  //
  // ⚠ 欄位的定義,一個一個講清楚(上一版 KEYPATH_MEASURED 與
  //   PHASE2_KEYPATH_MEASURED 會在同一份 log 裡打架,那本身就是缺陷):
  //     KEYPATH_MEASURED    整趟裡**有沒有任何一段**是在有焦點的狀態下
  //                         走完的。第二階段那一行問的是那一段,範圍不同,
  //                         但兩者不會矛盾:這一格是把量到的段落數出來的。
  //     KEYPATH_BLIND       整趟裡**有沒有任何一段**是瞎的。
  //                         ⚠ 它與 KEYPATH_MEASURED **不是互補**:
  //                         兩段一瞎一明時兩個都是 1,兩個都對。
  //     KEYPATH_SEGMENTS    量到的段落數 / 瞎的段落數。
  //     KEYPATH_FAILS_SEEN  記下的當下**有**焦點的失敗 —— 永遠計入。
  //     KEYPATH_FAILS_BLIND 記下的當下**沒有**焦點的失敗 —— 有條件赦免。
  //     EXCUSED             真的被赦免掉的**筆數**(不是 0/1)。
  Say("\nKEYPATH_MEASURED=%d KEYPATH_FAILS_BLIND=%d KEYPATH_FAILS_SEEN=%d "
      "KEYPATH_BLIND=%d LINK_VERIFIED=%d EXCUSED=%d "
      "KEYPATH_SEGMENTS=%d/%d\n",
      g_keypath.measured() ? 1 : 0, g_keypath.fails_blind(),
      g_keypath.fails_focused(), g_keypath.any_blind_segment() ? 1 : 0,
      g_keypath.link_verified() ? 1 : 0, excused, g_keypath.segments_focused(),
      g_keypath.segments_blind());
  // ⚠ 「這一次沒有量到」只有真的一段都沒量到時才准印。上一版憑黏滯旗標
  //   印它,於是第二階段明明量到了、log 裡還是有這一句。
  if (g_keypath.say_nothing_measured()) {
    Say("⚠ 整趟下來按鍵路徑這一次沒有量到(拿不到前景:%s)\n",
        ForegroundOwnerBrief().c_str());
  } else if (g_keypath.say_partly_measured()) {
    // 量到了,但不是每一段都量到。這一句與上面那句互斥,而且它**不是**
    // 「沒有量到」—— 說錯話會把人送去查前景,而該查的是量到的那一段。
    Say("⚠ 按鍵路徑有 %d 段量到了、%d 段沒有(沒量到的那幾段在上面各自\n"
        "  印了自己那一行)。這一行的判定看的是量到的那幾段。\n",
        g_keypath.segments_focused(), g_keypath.segments_blind());
  }
  if (excused > 0) {
    Say("  上面 %d 項按鍵相關的失敗**不計入結束碼**,因為它們是在拿不到\n"
        "  焦點的當下記的 —— 量到的是「沒有答案」而不是「壞掉」。\n"
        "  它們的判定由不需要前景的那三句話(管道 / 握手 / session)決定,\n"
        "  而它們全部成立。\n"
        "  ⚠ 這是刻意的取捨,而且它只赦免瞎的那幾筆:有焦點時記下的\n"
        "    %d 項照樣計入。要讓瞎的那幾段也量得到,要修的是**誰佔著\n"
        "    前景**,不是輸入法。\n",
        excused, g_keypath.fails_focused());
  } else if (g_keypath.fails_blind() > 0) {
    // ⚠ 這裡**不可以**說「這一趟什麼都沒有被量到」——「第一階段量到了、
    //   第二階段掉了前景」那一種組合底下那句話是假的,而假話會把人送去
    //   查前景,而該查的是量到的那一段。只講那幾筆自己的處境。
    Say("  拿不到焦點時記下的那 %d 項**照樣計入**:它們自己什麼都沒量到,\n"
        "  而不需要前景的那條路也沒有走通(LINK_VERIFIED=%d)——\n"
        "  沒有任何一條路對它們給出過答案。\n"
        "  一個驗不到東西的段落綠起來,比它紅起來危險得多。\n",
        g_keypath.fails_blind(), g_keypath.link_verified() ? 1 : 0);
  }
  g_fails += g_keypath.counted_fails();

  Say("\n=== %s(%d 項失敗)===\n", g_fails == 0 ? "通過" : "失敗", g_fails);
  return g_fails == 0 ? 0 : 1;
}
