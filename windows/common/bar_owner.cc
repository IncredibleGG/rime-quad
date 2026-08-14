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

}  // namespace

BarOwnerDecision DecideBarOwner(const std::vector<BarOwnerClient>& clients,
                                const BarOwnerForeground& fg) {
  BarOwnerDecision out;
  // ⚠ 前景問不出來就**什麼都不要說**。這裡回 in_use=false 的話,UAC 提示
  //   跳出來的那一秒那一橫會開始倒數隱藏,而使用者什麼都沒做。
  //   tid 為 0 也算問不出來:那是 GetWindowThreadProcessId 失敗的樣子。
  if (!fg.known || fg.tid == 0) {
    out.os_unknown = true;
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
  if (!best) return out;  // in_use 維持 false —— 前景那條執行緒不是我們的
  out.in_use = true;
  out.focused_client = best->client_id;
  out.focused_session = best->session;
  return out;
}

}  // namespace rimewin
