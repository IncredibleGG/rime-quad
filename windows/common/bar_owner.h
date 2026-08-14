// windows/common/bar_owner.h — 那一橫此刻是「誰的」(§12.10.6 的收斂層)
//
// ── 這一支解的是哪一句話寫錯了作用域 ────────────────────────────
//
// 那一橫的判準只有一句:
//
//   **它只在「使用者此刻輸入焦點所在的那一條宿主執行緒上,啟用中的
//   TSF profile 是我們」的時候顯示。**
//
// 不是「有沒有人連著我們」,不是「有幾個宿主載入了我們」,也不是
// 「系統當前的輸入法是不是我們」。上一輪的判準問的是**有沒有人**
// (`active_clients > 0`),而該問的是**這一個人是不是他**。
//
// 使用者實機回報的四個症狀裡有兩個是同一句話寫錯作用域的兩個方向:
//
//   S1 切到我們之後、還沒打字,那一橫不在。
//      → 條件其實成立,而我們**看不到**它(ActivateEx 不開任何連線)。
//   S4 已經切到微軟拼音了,那一橫還自己冒出來。
//      → 焦點那條執行緒上條件不成立,而我們把**別人的**條件當成自己的
//        (「有幾條管道 handle 開著」是一個沒有產品意義的量)。
//
// 而 S2(打字時時有時無)是第三個方向:焦點以前是**單一全域布林**,
// 由「最後說話的那個宿主」寫 —— 背景宿主一進場就把前景那個蓋掉。
//
// ── 三個層次,不是三個選項 ──────────────────────────────────────
//
//   1. **宿主提案。** 只有宿主自己知道「這條執行緒上啟用中的 profile
//      是不是我們」—— 服務端問不到別的進程的 TSF 狀態
//      (ITfInputProcessorProfileMgr 是 per-thread、per-process 的)。
//      宿主表達的方式是**在場連線的生死**:ActivateEx 開、
//      Deactivate 或 profile 被換掉時關。連線在 = 「我這條執行緒上是我們」。
//   2. **服務裁決。** 13 個宿主同時提案,收斂成一個答案的規則就是本檔。
//      ⚠ 逐連線 OR(上一輪的做法)是**單調沾黏**的:任何一條洩漏的連線
//        把那一橫永久釘住,而那正是 S4。
//   3. **系統打平手。** 誰是前景由 OS 回答,不由 13 個宿主各自宣稱 ——
//      讓宿主自己說「我是前景」的話,兩個宿主同時說 true 就退化回 OR。
//      OS 也是唯一**不依賴宿主活著、不依賴宿主誠實**的一方:被凍結的
//      UWP 進程、來不及送 Deactivate 就被砍掉的宿主,都靠這一層擋。
//
// ── 為什麼是純函式 ──────────────────────────────────────────────
//
// 三輪壞掉的都不是「判準寫錯」,是「送到判準面前的那份答案錯了」。
// grep 守得住「這一行還在嗎」,守不住「13 個宿主怎麼收斂成一個答案」。
// 這一支收在 common/ 就是為了讓 S4 那個方向在 Ubuntu 上擋得住 ——
// tests/test_bar_owner.cc 直接把 12 條殭屍 + 1 條前景餵進來。
//
#ifndef RIMEWIN_COMMON_BAR_OWNER_H_
#define RIMEWIN_COMMON_BAR_OWNER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rimewin {

// 服務端手上的**一條連線**。一個宿主可以有兩條(在場一條、按鍵一條)。
struct BarOwnerClient {
  // 服務端自己發的流水號。只用來讓答案是確定的(不會在兩條同分的
  // 連線之間跳來跳去),沒有別的意義。
  uint64_t client_id = 0;
  uint32_t host_pid = 0;
  // ⚠ **啟用我們的那一條 TSF 執行緒**,不是宿主的主執行緒。
  //   輸入法在 Windows 上是 per-thread 的(系統設定裡「讓我為每個
  //   應用程式視窗使用不同的輸入法」把這件事做成使用者看得見的事實),
  //   所以作用域必須是執行緒。0 = 這個用戶端報不出來(舊 DLL)。
  uint32_t host_tid = 0;
  // 這條連線的 librime session。0 = 還沒建(在場連線一直是 0)。
  // 那一橫回讀中英狀態時要問**這一個**,不是隨便挑一個活著的。
  uint64_t session = 0;
  // 握過手了嗎。⚠ **沒握手的連線一票都不投**:服務端在讀到第一個
  //   位元組之前就已經有一條連線了(ClientTicket 建構在 ServeClient
  //   最頂端),而那時我們連它是不是我們自己的 DLL 都不知道。
  bool activated = false;
};

// OS 這一刻說前景是誰。由服務端自己去問,不接受宿主宣稱。
struct BarOwnerForeground {
  // ⚠ 假 = **問不出來**(安全桌面 / UAC 提示時 GetForegroundWindow()
  //   回 NULL)。那時候**不要改狀態** —— 見 BarOwnerDecision::os_unknown。
  bool known = false;
  uint32_t pid = 0;
  uint32_t tid = 0;
  // ── UWP:前景視窗不是宿主的 ──────────────────────────────────
  //
  // 使用者在一個 UWP app 裡打字時,GetForegroundWindow() 回的是
  // ApplicationFrameHost.exe 的框視窗 —— pid 與 tid 都**不是**真正的
  // 宿主。真正的宿主握著那個框底下的 Windows.UI.Core.CoreWindow。
  // 少了這一層,「使用者正在 UWP app 裡打字」會被判成沒有人在用。
  bool inner_known = false;
  uint32_t inner_pid = 0;
  uint32_t inner_tid = 0;
};

struct BarOwnerDecision {
  // ⚠ 真 = OS 答不出前景。呼叫端**維持現狀**,不要把它當成「沒有人在用」
  //   —— UAC 提示跳出來的那一秒不該讓那一橫消失,而 3000 毫秒的遲滯
  //   (common/bar_visibility.h)本來就壓得住這種空窗。
  bool os_unknown = false;
  // 使用者此刻正在用的那條執行緒上,啟用中的是不是我們。
  bool in_use = false;
  // 那一條連線是誰。0 = 沒有。
  uint64_t focused_client = 0;
  // 它的 session。0 = 它還沒建 session(切過來但還沒打字)。
  uint64_t focused_session = 0;
};

// ── 收斂規則(逐條,而且每一條都有一支測試)────────────────────
//
//   · OS 答不出前景          → os_unknown,呼叫端維持現狀
//   · 沒握手的連線            → 一票都不投
//   · 報得出 tid 的連線       → **只**比 tid。報得出來卻退回去比 pid,
//                              等於在 per-app-window 模式下再壞一次
//   · 報不出 tid 的連線(舊 DLL)→ 才退回去比 pid
//   · UWP:框視窗比不上時,再比 CoreWindow 那一層
//   · 同分時:先挑**有 session 的**那一條(在場連線與按鍵連線並存時,
//     要回讀的是有 session 的那一條),再挑 client_id 最小的 ——
//     答案必須是確定的,不可以在兩條之間跳
BarOwnerDecision DecideBarOwner(const std::vector<BarOwnerClient>& clients,
                                const BarOwnerForeground& fg);

}  // namespace rimewin

#endif  // RIMEWIN_COMMON_BAR_OWNER_H_
