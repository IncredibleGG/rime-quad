// windows/tests/test_bar_owner.cc — 13 個宿主怎麼收斂成一個答案
//
// ⚠ **這一支是這一輪唯一能在 Linux 上擋住 S4 復發的東西。** 其餘守門守的
//   都是「那一行還在不在」,擋得住單行還原,擋不住收斂邏輯寫錯 ——
//   而三輪壞掉的都是後者。
//
// 使用者實機證據(Windows 11、13 個進程載入了 rime_tsf.dll):
//   S1 切到我們之後、還沒打字,那一橫不見,要打字才回來。
//   S2 正在記事本打字時,背景宿主陸續 ActivateEx,那一橫時有時無。
//   S4 已經切到微軟拼音了,那一橫還自己冒出來。
//
// 三個症狀在這裡各有一支測試,而且用的是**他機器上的實際數字**:
// 13 個宿主、一個在前景。

#include "bar_owner.h"

#include "check.h"

using namespace rimewin;

namespace {

// 一條握過手、報得出 tid 的在場連線。
BarOwnerClient Host(uint64_t id, uint32_t pid, uint32_t tid) {
  BarOwnerClient c;
  c.client_id = id;
  c.host_pid = pid;
  c.host_tid = tid;
  c.activated = true;
  return c;
}

BarOwnerForeground Fg(uint32_t pid, uint32_t tid) {
  BarOwnerForeground f;
  f.known = true;
  f.pid = pid;
  f.tid = tid;
  return f;
}

}  // namespace

// ── O1:12 條殭屍 + 1 條前景 → 顯示,而且指到前景那一條 ─────────────
TEST(bar_owner_twelve_zombies_plus_one_foreground_shows_the_foreground_one) {
  std::vector<BarOwnerClient> cs;
  // 12 個背景宿主(Snipaste、rustdesk、conhost…)。它們的連線全都活著:
  // 有的凍結了、有的永遠不會送 Deactivate。在舊判準底下,這 12 條
  // 每一條都是一張票。
  for (uint32_t i = 1; i <= 12; ++i)
    cs.push_back(Host(i, 1000 + i, 2000 + i));
  // 使用者真的在用的那一個。
  cs.push_back(Host(99, 4242, 4343));

  const BarOwnerDecision d = DecideBarOwner(cs, Fg(4242, 4343));
  CHECK(!d.undecidable);
  CHECK(d.in_use);
  CHECK_INT(d.focused_client, 99);
  CHECK_INT(cs.size(), 13);  // 掃描範圍非空
}

// ── O2:13 條全在背景 → **隱藏**。這一條就是 S4 ────────────────────
TEST(bar_owner_thirteen_background_hosts_alone_must_hide) {
  std::vector<BarOwnerClient> cs;
  for (uint32_t i = 1; i <= 13; ++i)
    cs.push_back(Host(i, 1000 + i, 2000 + i));
  // 前景是一個完全沒有載入我們的程式(或載入了但用的是微軟拼音)。
  const BarOwnerDecision d = DecideBarOwner(cs, Fg(777, 888));
  CHECK(!d.undecidable);
  // ⚠ 舊判準在這裡是 `active_clients == 13 > 0` → 顯示。
  //   使用者的原話:「你看他隨機出現 我現在用其他的輸入法。但是他突然出現了」。
  CHECK(!d.in_use);
  CHECK_INT(d.focused_client, 0);
}

// ── O3:前景那一條剛被摘掉(使用者切到微軟拼音)→ 隱藏 ──────────────
TEST(bar_owner_foreground_switching_away_hides_even_with_others_alive) {
  std::vector<BarOwnerClient> alive;
  for (uint32_t i = 1; i <= 12; ++i)
    alive.push_back(Host(i, 1000 + i, 2000 + i));
  std::vector<BarOwnerClient> with_fg = alive;
  with_fg.push_back(Host(99, 4242, 4343));

  const BarOwnerForeground fg = Fg(4242, 4343);
  CHECK(DecideBarOwner(with_fg, fg).in_use);
  // profile sink 的「非啟用」那一邊把前景那條連線收掉了 ——
  // 其餘 12 條一個字都沒動,而答案必須翻過來。
  CHECK(!DecideBarOwner(alive, fg).in_use);
}

// ── O4:S1 —— 切過來、還沒打字(在場連線有,session 還是 0)──────────
TEST(bar_owner_presence_without_a_session_is_already_enough) {
  std::vector<BarOwnerClient> cs;
  cs.push_back(Host(7, 4242, 4343));  // session 維持 0
  const BarOwnerDecision d = DecideBarOwner(cs, Fg(4242, 4343));
  CHECK(d.in_use);
  CHECK_INT(d.focused_client, 7);
  // ⚠ 沒有 session 是**正常**的,不是失敗:使用者還沒按過任何一顆鍵。
  //   呼叫端據此決定回讀走哪一條路(見 status_bar.cc 的 RefreshFromEngine)。
  CHECK_INT(d.focused_session, 0);
}

// ── O5:S2 —— 背景宿主進進出出,前景那一格一個字都不准變 ─────────────
TEST(bar_owner_background_hosts_coming_and_going_never_move_the_answer) {
  const BarOwnerForeground fg = Fg(4242, 4343);
  std::vector<BarOwnerClient> cs;
  cs.push_back(Host(1, 4242, 4343));  // Notepad3,使用者正在這裡打字
  cs.back().session = 55;

  int seen = 0;
  // 19:31:47 Snipaste、19:31:52 rustdesk、19:32:17 conhost… 逐一進場,
  // 再逐一退場。每一步都問一次。
  for (uint32_t i = 0; i < 12; ++i) {
    cs.push_back(Host(100 + i, 5000 + i, 6000 + i));
    const BarOwnerDecision d = DecideBarOwner(cs, fg);
    CHECK(d.in_use);
    CHECK_INT(d.focused_client, 1);
    CHECK_INT(d.focused_session, 55);
    ++seen;
  }
  while (cs.size() > 1) {
    cs.pop_back();
    const BarOwnerDecision d = DecideBarOwner(cs, fg);
    CHECK(d.in_use);
    CHECK_INT(d.focused_client, 1);
    ++seen;
  }
  CHECK_INT(seen, 24);
}

// ── O6:沒握手的連線一票都不投 ───────────────────────────────────
TEST(bar_owner_a_connection_that_never_said_hello_does_not_vote) {
  BarOwnerClient silent;
  silent.client_id = 1;
  silent.host_pid = 4242;
  silent.host_tid = 4343;
  silent.activated = false;  // ServeClient 頂端就有連線,但還沒讀到一個位元組
  std::vector<BarOwnerClient> cs{silent};
  CHECK(!DecideBarOwner(cs, Fg(4242, 4343)).in_use);

  // 握完手就投。
  cs[0].activated = true;
  CHECK(DecideBarOwner(cs, Fg(4242, 4343)).in_use);
}

// ── O7:OS 答不出前景 → 維持現狀,不是「沒有人在用」───────────────
TEST(bar_owner_unknown_foreground_keeps_the_caller_from_changing_anything) {
  std::vector<BarOwnerClient> cs{Host(1, 4242, 4343)};
  BarOwnerForeground none;  // known = false(UAC 提示 / 安全桌面)
  const BarOwnerDecision d = DecideBarOwner(cs, none);
  CHECK(d.undecidable);
  // ⚠ 呼叫端**不可以**把 in_use 讀成答案。這裡順手把它釘成 false,
  //   讓「忘了看 undecidable」在真機上是「那一橫不見了」而不是隨機。
  CHECK(!d.in_use);

  // tid 問不出來(GetWindowThreadProcessId 失敗)也算問不出來。
  BarOwnerForeground half;
  half.known = true;
  half.pid = 4242;
  half.tid = 0;
  CHECK(DecideBarOwner(cs, half).undecidable);
}

// ── O8:UWP —— 前景是 ApplicationFrameHost,宿主在 CoreWindow 那一層 ──
TEST(bar_owner_uwp_matches_through_the_core_window) {
  std::vector<BarOwnerClient> cs{Host(1, 4242, 4343)};
  BarOwnerForeground fg = Fg(900, 901);  // ApplicationFrameHost.exe
  // 直接比 → 對不上。使用者正在 UWP app 裡打字,而那一橫會消失。
  CHECK(!DecideBarOwner(cs, fg).in_use);
  // 把框底下那個 Windows.UI.Core.CoreWindow 的執行緒問出來之後就對上了。
  fg.inner_known = true;
  fg.inner_pid = 4242;
  fg.inner_tid = 4343;
  const BarOwnerDecision d = DecideBarOwner(cs, fg);
  CHECK(d.in_use);
  CHECK_INT(d.focused_client, 1);
}

// ── O9:報得出 tid 就不准退回去比 pid ────────────────────────────
TEST(bar_owner_a_tid_capable_client_never_falls_back_to_pid) {
  // 同一支程式的兩條 UI 執行緒。我們只在 4343 那一條上是啟用中的,
  // 而使用者此刻的焦點在 4344 那一條(他在那個視窗裡用微軟拼音)。
  std::vector<BarOwnerClient> cs{Host(1, 4242, 4343)};
  CHECK(!DecideBarOwner(cs, Fg(4242, 4344)).in_use);
  // 同一筆,但這個用戶端報不出 tid(舊 DLL)—— 只剩 pid 可比,
  // 精確度差一階,但不能因此看不見它。
  cs[0].host_tid = 0;
  CHECK(DecideBarOwner(cs, Fg(4242, 4344)).in_use);
}

// ── O10:同一條執行緒兩條連線 → 回讀要問**有 session 的**那一條 ───────
TEST(bar_owner_prefers_the_connection_that_actually_has_a_session) {
  std::vector<BarOwnerClient> cs;
  cs.push_back(Host(1, 4242, 4343));  // 在場連線,session = 0
  cs.push_back(Host(2, 4242, 4343));  // 按鍵連線
  cs.back().session = 77;
  const BarOwnerDecision d = DecideBarOwner(cs, Fg(4242, 4343));
  CHECK(d.in_use);
  CHECK_INT(d.focused_client, 2);
  // ⚠ 這就是 S3 的一半:回讀中英狀態時問錯 session,那一格畫的是
  //   **別的宿主**的狀態。舊版是 sessions_.begin(),等於擲骰子。
  CHECK_INT(d.focused_session, 77);

  // 順序反過來也一樣 —— 答案不可以取決於連線建立的先後。
  std::vector<BarOwnerClient> flipped{cs[1], cs[0]};
  CHECK_INT(DecideBarOwner(flipped, Fg(4242, 4343)).focused_session, 77);
}

// ── O11:一條連線都沒有 → 隱藏(服務剛起來的常態)────────────────
TEST(bar_owner_no_clients_at_all_is_simply_hidden) {
  std::vector<BarOwnerClient> none;
  const BarOwnerDecision d = DecideBarOwner(none, Fg(4242, 4343));
  CHECK(!d.undecidable);
  CHECK(!d.in_use);
  CHECK_INT(d.focused_session, 0);
}

// ── O12:精確的那一條贏 —— tid 對上時不可以被 pid 對上的那條蓋過 ─────
TEST(bar_owner_exact_thread_match_wins_over_a_mere_process_match) {
  std::vector<BarOwnerClient> cs;
  BarOwnerClient legacy = Host(1, 4242, 0);  // 舊 DLL,只有 pid
  legacy.session = 11;
  cs.push_back(legacy);
  BarOwnerClient exact = Host(2, 4242, 4343);
  exact.session = 22;
  cs.push_back(exact);
  const BarOwnerDecision d = DecideBarOwner(cs, Fg(4242, 4343));
  CHECK(d.in_use);
  // ⚠ 名次比「有沒有 session」優先。兩條都有 session 時,精確的那一條
  //   才是使用者正在打字的那一條。
  CHECK_INT(d.focused_client, 2);
  CHECK_INT(d.focused_session, 22);
}

// ── O13:前景是**服務自己的視窗**(設定 / 托盤選單)→ 維持現狀 ────────
//
// ⚠ 這一支是這一輪新造的那個缺陷。使用者從那一橫點「設定」,設定視窗
//   是**服務自己的進程、自己的執行緒** —— 13 個宿主的註冊一筆都對不上。
//   在這裡回 in_use=false 的話,3000 毫秒之後那一橫在他眼前消失,而他
//   做的事只是打開設定。他今晚已經被「它又不見了」耽誤過三次。
TEST(bar_owner_our_own_settings_window_is_not_an_answer) {
  const uint32_t kServicePid = 31337;
  std::vector<BarOwnerClient> cs;
  for (uint32_t i = 1; i <= 12; ++i) cs.push_back(Host(i, 1000 + i, 2000 + i));
  cs.push_back(Host(99, 4242, 4343));  // 他剛剛在打字的那一個
  cs.back().session = 55;

  // 服務自己的設定視窗成為前景:pid 是服務的,tid 是設定視窗那條執行緒。
  BarOwnerForeground gui = Fg(kServicePid, 555);
  gui.service_pid = kServicePid;
  const BarOwnerDecision d = DecideBarOwner(cs, gui);
  CHECK(d.undecidable);
  // ⚠ 而且釘成 false —— 「忘了看 undecidable」在真機上要是「那一橫
  //   不見了」這種一眼看得出來的樣子,不是隨機。
  CHECK(!d.in_use);
  CHECK_INT(d.focused_session, 0);

  // ⚠ 只有**服務自己**這樣。別人的前景照常裁決,否則這一條會變成
  //   「誰都不算」—— 那是把 S4 用另一個方向放回去。
  BarOwnerForeground host = Fg(4242, 4343);
  host.service_pid = kServicePid;
  CHECK(!DecideBarOwner(cs, host).undecidable);
  CHECK(DecideBarOwner(cs, host).in_use);
  CHECK_INT(DecideBarOwner(cs, host).focused_session, 55);

  // ⚠ 沒填 service_pid(0)不可以變成「前景 pid 是 0 就對上了」。
  BarOwnerForeground unset = Fg(0, 4343);
  CHECK(!DecideBarOwner(cs, unset).undecidable);

  // UWP 那一層也要擋:框是 ApplicationFrameHost、CoreWindow 是我們自己。
  // 這一格在今天的 Win32 服務上到不了,但收斂規則不該只擋得住一個方向。
  BarOwnerForeground uwp = Fg(900, 901);
  uwp.service_pid = kServicePid;
  uwp.inner_known = true;
  uwp.inner_pid = kServicePid;
  uwp.inner_tid = 902;
  CHECK(DecideBarOwner(cs, uwp).undecidable);
}
