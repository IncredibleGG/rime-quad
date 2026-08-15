#include "bar_owner.h"

namespace rimewin {
namespace {

// 這一筆註冊在不在「使用者此刻的那一條執行緒」上。
//
// 回 0 = 不在。否則回一個**名次**,數字越小越精確:
//   1 前景執行緒的 tid 對上了              ← 唯一真正想要的那一格
//   2 UWP 內層(CoreWindow)的 tid 對上了
//   3 前景進程的 pid 對上了(這個用戶端報不出 tid)
//   4 UWP 內層的 pid 對上了(同上)
//
// ⚠ 名次存在的理由不是排版:同一個宿主可能同時有兩條連線,而「哪一條
//   是使用者正在打字的那一條」決定了回讀中英狀態時要問哪一個 session。
int MatchRank(const BarOwnerClient& c, const BarOwnerForeground& fg) {
  // ⚠ 沒握手 = 我們連它是不是我們自己的 DLL 都還不知道。ClientTicket
  //   在讀第一個位元組之前就已經建構了(pipe_server.cc 的 ServeClient
  //   最頂端),所以「有一條連線」與「有一個宿主在用我們」是兩件事 ——
  //   把它們混成一件,就是上一輪 active_clients 那個量。
  if (!c.activated) return 0;

  if (c.host_tid != 0) {
    if (c.host_tid == fg.tid) return 1;
    if (fg.inner_known && c.host_tid == fg.inner_tid) return 2;
    // ⚠ **報得出 tid 就不准退回去比 pid。** 一個進程可以有好幾條 UI
    //   執行緒,而輸入法是 per-thread 的:同一支程式的另一個視窗上
    //   使用者可能正在用微軟拼音。退回去比 pid 等於把作用域放大一階,
    //   而那正是這一輪在修的錯。
    return 0;
  }

  // 報不出 tid(舊 DLL,線路版本 < 3)。只剩 pid 可比 —— 精確度差一階,
  // 但總比「完全看不見這個宿主」好:那會讓舊 DLL 的使用者失去那一橫。
  if (c.host_pid == 0) return 0;
  if (c.host_pid == fg.pid) return 3;
  if (fg.inner_known && c.host_pid == fg.inner_pid) return 4;
  return 0;
}

// 同分時誰贏。⚠ 這一段唯一的要求是**確定性**:答案在兩次呼叫之間
//   不可以自己跳,否則那一橫會在兩條連線之間閃。
bool Beats(const BarOwnerClient& a, int rank_a, const BarOwnerClient& b,
           int rank_b) {
  if (rank_a != rank_b) return rank_a < rank_b;
  // 有 session 的優先:在場連線(session=0)與按鍵連線(session≠0)
  // 會同時存在同一條執行緒上,而要回讀的是後者。
  const bool a_has = a.session != 0;
  const bool b_has = b.session != 0;
  if (a_has != b_has) return a_has;
  return a.client_id < b.client_id;
}

// 這條執行緒明確說過「啟用中的不再是我們」,而且那句話還沒過期嗎。
//
// ⚠ UWP 那一層也要看:前景是 ApplicationFrameHost 的框視窗時,真正
//   讓位的是 CoreWindow 底下那條執行緒。
bool SaidItYielded(const std::vector<BarOwnerYield>& yields,
                   const BarOwnerForeground& fg, uint64_t now_ms) {
  for (size_t i = 0; i < yields.size(); ++i) {
    const BarOwnerYield& y = yields[i];
    if (y.host_tid == 0) continue;
    if (y.host_tid != fg.tid && !(fg.inner_known && y.host_tid == fg.inner_tid))
      continue;
    // ⚠ 先比大小再相減,不要直接 `now_ms - y.at_ms`:now_ms 可能比
    //   at_ms 小(呼叫端餵了一個較早的時間),而無號相減會繞成一個
    //   巨大的正數 —— 那會讓一筆**剛剛才寫下**的證據看起來已經過期。
    //   時間倒退時當成「還沒過期」:證據本身是可信的,存疑的只有它
    //   的年紀,讓 TTL 自然走完才是對的。
    if (now_ms < y.at_ms) return true;
    if (now_ms - y.at_ms < BarOwnerYieldTtlMs()) return true;
  }
  return false;
}

}  // namespace

// 10 分鐘。夠長到「切走輸入法之後去做別的事、再回來」還算數,
// 夠短到一個被回收的 tid 不會一直冒充別人。
uint32_t BarOwnerYieldTtlMs() { return 600000; }

BarOwnerDecision DecideBarOwner(const std::vector<BarOwnerClient>& clients,
                                const std::vector<BarOwnerYield>& yields,
                                const BarOwnerForeground& fg, uint64_t now_ms) {
  BarOwnerDecision out;
  // ⚠ 前景問不出來就**什麼都不要說**。這裡回 in_use=false 的話,UAC 提示
  //   跳出來的那一秒那一橫會開始倒數隱藏,而使用者什麼都沒做。
  //   tid 為 0 也算問不出來:那是 GetWindowThreadProcessId 失敗的樣子。
  if (!fg.known || fg.tid == 0) {
    out.undecidable = true;
    out.verdict = BarOwnerVerdict::kHold;
    out.hold_reason = BarOwnerDecision::HoldReason::kForegroundUnknown;
    return out;
  }
  // ── 前景是**服務自己**:設定視窗 / 托盤選單 / 那一橫的彈出選單 ──
  //
  // ⚠ 這一格是「使用者從那一橫點了設定」。設定視窗是服務自己的進程、
  //   自己的執行緒,13 個宿主的註冊**一筆都對不上** —— 底下那個迴圈
  //   會誠實地算出 in_use=false,而那不是事實:他沒有切走輸入法,
  //   他只是打開了設定。少了這一格的樣子是「視窗開起來,3000 毫秒後
  //   那一橫自己不見了」。
  //
  // ⚠ 為什麼是「維持現狀」而不是「顯示」:他也可能是從**系統匣圖示**
  //   打開設定的(切走輸入法之後唯一的入口,見 ui-design §12.10.6)。
  //   那時候那一橫本來就該是藏著的,強制顯示等於把 S4 換個地方放回去。
  //
  // ⚠ service_pid 為 0 = 呼叫端沒填。那時候這一格不比 —— 否則一份
  //   預設建構的 BarOwnerForeground 會讓「前景 pid 是 0」變成我們自己。
  //   status_bar.cc 那一行由 audit_single_source.sh 規則 6 守著。
  if (fg.service_pid != 0 &&
      (fg.pid == fg.service_pid ||
       (fg.inner_known && fg.inner_pid == fg.service_pid))) {
    out.undecidable = true;
    out.verdict = BarOwnerVerdict::kHold;
    out.hold_reason = BarOwnerDecision::HoldReason::kForegroundIsService;
    return out;
  }

  int best_rank = 0;
  const BarOwnerClient* best = nullptr;
  for (size_t i = 0; i < clients.size(); ++i) {
    const int rank = MatchRank(clients[i], fg);
    if (rank == 0) continue;
    if (!best || Beats(clients[i], rank, *best, best_rank)) {
      best = &clients[i];
      best_rank = rank;
    }
  }
  if (best) {
    out.verdict = BarOwnerVerdict::kOurs;
    out.in_use = true;
    out.focused_client = best->client_id;
    out.focused_session = best->session;
    return out;
  }

  // ── 一條都對不上。**這一行就是 #111 的全部。** ────────────────
  //
  // 以前這裡直接 `return out;`(undecidable=false、in_use=false)——
  // 也就是「查不到 = 是別人的」。截圖工具、工作列、桌面、以及每一個
  // 在升級前就開著、還抱著舊 DLL 的宿主(它們沒有在場連線這回事)
  // 全部落在這裡,於是那一橫在使用者眼前消失。
  //
  // 現在**隱藏需要正面證據**:這條執行緒要自己說過「啟用中的不再是
  // 我們」(kProfileState),而且它此刻真的收得到文字。兩者缺一,
  // 答案就是「我不知道」→ 呼叫端維持上一次的判斷。
  if (!SaidItYielded(yields, fg, now_ms)) {
    out.undecidable = true;
    out.verdict = BarOwnerVerdict::kHold;
    out.hold_reason = BarOwnerDecision::HoldReason::kNoPresenceOnThread;
    return out;
  }
  // ⚠ 它說過讓位,但這條執行緒此刻**沒有鍵盤焦點視窗**(桌面、工作列、
  //   截圖疊層,或 GetGUIThreadInfo 對提權宿主回 FALSE)。那不是
  //   「使用者正在用別的輸入法打字」,只是「他此刻沒有在打字」——
  //   而讓位紀錄是黏著的,它會一直在,不能讓它把那一橫永久壓住。
  if (!fg.can_take_text) {
    out.undecidable = true;
    out.verdict = BarOwnerVerdict::kHold;
    out.hold_reason = BarOwnerDecision::HoldReason::kYieldedButNoFocus;
    return out;
  }
  out.verdict = BarOwnerVerdict::kTheirs;  // in_use 維持 false → 隱藏
  return out;
}

}  // namespace rimewin
