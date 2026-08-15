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

// 「這條執行緒說過:啟用中的不再是我們」。#111 之後,**只有這個**能讓
// 那一橫收起來 —— 服務端不准再從「查不到」推出「是別人的」。
BarOwnerYield Yield(uint32_t tid, uint64_t at_ms = 1000) {
  BarOwnerYield y;
  y.host_tid = tid;
  y.at_ms = at_ms;
  return y;
}

// 現在的時間。所有不談過期的測試都用同一個值。
const uint64_t kNow = 1000;

const std::vector<BarOwnerYield> kNoYields;

// 沒有任何讓位紀錄的那一格 —— 既有的每一支問的都是這個。
BarOwnerDecision Decide(const std::vector<BarOwnerClient>& cs,
                        const BarOwnerForeground& fg) {
  return DecideBarOwner(cs, kNoYields, fg, kNow);
}

// 前景那條執行緒剛剛說過讓位,而且它此刻收得到文字(有鍵盤焦點視窗)。
BarOwnerDecision DecideAfterYield(const std::vector<BarOwnerClient>& cs,
                                  BarOwnerForeground fg) {
  fg.can_take_text = true;
  const std::vector<BarOwnerYield> ys{Yield(fg.tid)};
  return DecideBarOwner(cs, ys, fg, kNow);
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

  const BarOwnerDecision d = Decide(cs, Fg(4242, 4343));
  CHECK(!d.undecidable);
  CHECK(d.in_use);
  CHECK_INT(d.focused_client, 99);
  CHECK_INT(cs.size(), 13);  // 掃描範圍非空
}

// ── O2:13 條全在背景,而前景**沒說過話** → 維持,不是隱藏 ──────────
//
// ⚠ 這一支**反過來了**,而那正是 #111 的修法本身。它以前斷言的是
//   「一條都對不上 → 隱藏」,也就是「查不到 = 是別人的」—— 而截圖工具、
//   工作列、桌面、以及每一個還抱著舊 DLL 的宿主全部落在同一格。
//   S4(「我在用微軟拼音,它還冒出來」)由後半段那個 kTheirs 守著:
//   收起來還是收得起來,只是需要那條執行緒**自己說出來**。
TEST(bar_owner_thirteen_background_hosts_with_a_silent_foreground_holds) {
  std::vector<BarOwnerClient> cs;
  for (uint32_t i = 1; i <= 13; ++i)
    cs.push_back(Host(i, 1000 + i, 2000 + i));
  // 前景是一個完全沒有載入我們的程式(截圖工具 / 工作列 / 舊 DLL 宿主)。
  const BarOwnerDecision d = Decide(cs, Fg(777, 888));
  CHECK(d.verdict == BarOwnerVerdict::kHold);
  CHECK(d.undecidable);
  CHECK(!d.in_use);
  CHECK_INT(d.focused_client, 0);

  // 同一份輸入,只多一件事:那條執行緒說過「啟用中的不再是我們」,
  // 而且它此刻收得到文字 → 這才是 S4,而它照樣隱藏。
  const BarOwnerDecision t = DecideAfterYield(cs, Fg(777, 888));
  CHECK(t.verdict == BarOwnerVerdict::kTheirs);
  CHECK(!t.undecidable);
  CHECK(!t.in_use);
}

// ── O3:前景那一條被摘掉 —— 連線死掉本身**不是**「使用者切走了」──────
TEST(bar_owner_foreground_switching_away_hides_only_when_it_said_so) {
  std::vector<BarOwnerClient> alive;
  for (uint32_t i = 1; i <= 12; ++i)
    alive.push_back(Host(i, 1000 + i, 2000 + i));
  std::vector<BarOwnerClient> with_fg = alive;
  with_fg.push_back(Host(99, 4242, 4343));

  const BarOwnerForeground fg = Fg(4242, 4343);
  CHECK(Decide(with_fg, fg).verdict == BarOwnerVerdict::kOurs);
  // ⚠ 只把那條連線拿掉 = 「宿主不見了」。宿主被砍掉、宿主凍結、宿主
  //   根本沒載入我們,在服務端長得一模一樣 —— 所以這裡只能維持。
  CHECK(Decide(alive, fg).verdict == BarOwnerVerdict::kHold);
  CHECK(!Decide(alive, fg).in_use);
  // 加上那句話(profile sink 的非啟用邊送出的 kProfileState)之後才隱藏。
  CHECK(DecideAfterYield(alive, fg).verdict == BarOwnerVerdict::kTheirs);
}

// ── O4:S1 —— 切過來、還沒打字(在場連線有,session 還是 0)──────────
TEST(bar_owner_presence_without_a_session_is_already_enough) {
  std::vector<BarOwnerClient> cs;
  cs.push_back(Host(7, 4242, 4343));  // session 維持 0
  const BarOwnerDecision d = Decide(cs, Fg(4242, 4343));
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
    const BarOwnerDecision d = Decide(cs, fg);
    CHECK(d.in_use);
    CHECK_INT(d.focused_client, 1);
    CHECK_INT(d.focused_session, 55);
    ++seen;
  }
  while (cs.size() > 1) {
    cs.pop_back();
    const BarOwnerDecision d = Decide(cs, fg);
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
  CHECK(!Decide(cs, Fg(4242, 4343)).in_use);

  // 握完手就投。
  cs[0].activated = true;
  CHECK(Decide(cs, Fg(4242, 4343)).in_use);
}

// ── O7:OS 答不出前景 → 維持現狀,不是「沒有人在用」───────────────
TEST(bar_owner_unknown_foreground_keeps_the_caller_from_changing_anything) {
  std::vector<BarOwnerClient> cs{Host(1, 4242, 4343)};
  BarOwnerForeground none;  // known = false(UAC 提示 / 安全桌面)
  const BarOwnerDecision d = Decide(cs, none);
  CHECK(d.undecidable);
  // ⚠ 呼叫端**不可以**把 in_use 讀成答案。這裡順手把它釘成 false,
  //   讓「忘了看 undecidable」在真機上是「那一橫不見了」而不是隨機。
  CHECK(!d.in_use);

  // tid 問不出來(GetWindowThreadProcessId 失敗)也算問不出來。
  BarOwnerForeground half;
  half.known = true;
  half.pid = 4242;
  half.tid = 0;
  CHECK(Decide(cs, half).undecidable);
}

// ── O8:UWP —— 前景是 ApplicationFrameHost,宿主在 CoreWindow 那一層 ──
TEST(bar_owner_uwp_matches_through_the_core_window) {
  std::vector<BarOwnerClient> cs{Host(1, 4242, 4343)};
  BarOwnerForeground fg = Fg(900, 901);  // ApplicationFrameHost.exe
  // 直接比 → 對不上。使用者正在 UWP app 裡打字,而那一橫會消失。
  CHECK(!Decide(cs, fg).in_use);
  // 把框底下那個 Windows.UI.Core.CoreWindow 的執行緒問出來之後就對上了。
  fg.inner_known = true;
  fg.inner_pid = 4242;
  fg.inner_tid = 4343;
  const BarOwnerDecision d = Decide(cs, fg);
  CHECK(d.in_use);
  CHECK_INT(d.focused_client, 1);
}

// ── O9:報得出 tid 就不准退回去比 pid ────────────────────────────
TEST(bar_owner_a_tid_capable_client_never_falls_back_to_pid) {
  // 同一支程式的兩條 UI 執行緒。我們只在 4343 那一條上是啟用中的,
  // 而使用者此刻的焦點在 4344 那一條(他在那個視窗裡用微軟拼音)。
  std::vector<BarOwnerClient> cs{Host(1, 4242, 4343)};
  // ⚠ 「對不上」現在是 kHold(維持),不是 kTheirs —— 我們對 4344 那條
  //   執行緒**一無所知**,它可能只是還沒連上來。
  CHECK(Decide(cs, Fg(4242, 4344)).verdict == BarOwnerVerdict::kHold);
  CHECK(!Decide(cs, Fg(4242, 4344)).in_use);
  // 4344 自己說過讓位之後才是「別人的」。⚠ 注意 4343 那條連線**還活著**,
  //   而它一票都不該投給 4344 —— per-thread 的作用域在這裡也要成立。
  CHECK(DecideAfterYield(cs, Fg(4242, 4344)).verdict == BarOwnerVerdict::kTheirs);
  // 同一筆,但這個用戶端報不出 tid(舊 DLL)—— 只剩 pid 可比,
  // 精確度差一階,但不能因此看不見它。
  cs[0].host_tid = 0;
  CHECK(Decide(cs, Fg(4242, 4344)).in_use);
}

// ── O10:同一條執行緒兩條連線 → 回讀要問**有 session 的**那一條 ───────
TEST(bar_owner_prefers_the_connection_that_actually_has_a_session) {
  std::vector<BarOwnerClient> cs;
  cs.push_back(Host(1, 4242, 4343));  // 在場連線,session = 0
  cs.push_back(Host(2, 4242, 4343));  // 按鍵連線
  cs.back().session = 77;
  const BarOwnerDecision d = Decide(cs, Fg(4242, 4343));
  CHECK(d.in_use);
  CHECK_INT(d.focused_client, 2);
  // ⚠ 這就是 S3 的一半:回讀中英狀態時問錯 session,那一格畫的是
  //   **別的宿主**的狀態。舊版是 sessions_.begin(),等於擲骰子。
  CHECK_INT(d.focused_session, 77);

  // 順序反過來也一樣 —— 答案不可以取決於連線建立的先後。
  std::vector<BarOwnerClient> flipped{cs[1], cs[0]};
  CHECK_INT(Decide(flipped, Fg(4242, 4343)).focused_session, 77);
}

// ── O11:一條連線都沒有 → **維持**(服務剛起來的常態)──────────────
//
// ⚠ 也反過來了。服務剛起來時那一橫本來就是藏著的(shown_ 預設 false),
//   所以 kHold 的結果照樣是「看不到」—— 差別在它不會把一個**已經顯示
//   著**的那一橫關掉。那正是使用者遇到的:所有宿主都抱著舊 DLL 時,
//   regs_ 是空的,而他每切一次視窗那一橫就被判成「別人的」。
TEST(bar_owner_no_clients_at_all_holds_instead_of_claiming_theirs) {
  std::vector<BarOwnerClient> none;
  const BarOwnerDecision d = Decide(none, Fg(4242, 4343));
  CHECK(d.verdict == BarOwnerVerdict::kHold);
  CHECK(d.undecidable);
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
  const BarOwnerDecision d = Decide(cs, Fg(4242, 4343));
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
  const BarOwnerDecision d = Decide(cs, gui);
  CHECK(d.undecidable);
  // ⚠ 而且釘成 false —— 「忘了看 undecidable」在真機上要是「那一橫
  //   不見了」這種一眼看得出來的樣子,不是隨機。
  CHECK(!d.in_use);
  CHECK_INT(d.focused_session, 0);

  // ⚠ 只有**服務自己**這樣。別人的前景照常裁決,否則這一條會變成
  //   「誰都不算」—— 那是把 S4 用另一個方向放回去。
  BarOwnerForeground host = Fg(4242, 4343);
  host.service_pid = kServicePid;
  CHECK(!Decide(cs, host).undecidable);
  CHECK(Decide(cs, host).in_use);
  CHECK_INT(Decide(cs, host).focused_session, 55);

  // ⚠ 沒填 service_pid(0)不可以變成「前景 pid 是 0 就對上了」。
  BarOwnerForeground unset = Fg(0, 4343);
  CHECK(!Decide(cs, unset).undecidable);

  // UWP 那一層也要擋:框是 ApplicationFrameHost、CoreWindow 是我們自己。
  // 這一格在今天的 Win32 服務上到不了,但收斂規則不該只擋得住一個方向。
  BarOwnerForeground uwp = Fg(900, 901);
  uwp.service_pid = kServicePid;
  uwp.inner_known = true;
  uwp.inner_pid = kServicePid;
  uwp.inner_tid = 902;
  CHECK(Decide(cs, uwp).undecidable);
}

// ══ #111:「查不到」不等於「是別人的」════════════════════════════
//
// 使用者的 tsf.log 這一次證實了一件比截圖工具嚴重得多的事:**他在升級前
// 就開著的每一個宿主行程,到現在都還抱著舊的 rime_tsf.dll**(「在場連線」
// 這種紀錄是這一版才有的,08-09 / 08-14 載入 DLL 的那幾個進程一行都沒有,
// 08-15 重裝後才開的全都有 —— 分界乾淨到零例外)。Explorer.exe(工作列、
// 檔案總管)幾乎永遠不會重開。也就是說「前景那條執行緒上查不到在場連線」
// 不是罕見情況,是他桌面上一半的程式。

// ── O14:前景是截圖工具 —— 13 條宿主一條都對不上,而它沒說過話 ────────
//
// 這一支就是使用者這次回報的那一格:他在記事本打字(那一橫顯示著),
// 按下截圖工具,框選疊層搶到前景超過 3000 毫秒,那一橫消失。
TEST(bar_owner_a_screenshot_overlay_must_not_take_the_bar_away) {
  std::vector<BarOwnerClient> cs;
  for (uint32_t i = 1; i <= 12; ++i) cs.push_back(Host(i, 1000 + i, 2000 + i));
  cs.push_back(Host(99, 4242, 4343));  // 他剛剛在打字的記事本
  cs.back().session = 55;

  // 先確認那一橫本來是顯示著的。
  CHECK(Decide(cs, Fg(4242, 4343)).verdict == BarOwnerVerdict::kOurs);

  // Snipaste 的框選疊層。它從來不載入我們的 TIP,所以 13 條註冊一條都
  // 對不上;它也不會送 kProfileState(它根本不知道我們存在)。
  BarOwnerForeground snip = Fg(7788, 7789);
  snip.can_take_text = true;  // 疊層甚至可能有焦點視窗 —— 照樣不算證據
  const BarOwnerDecision d = Decide(cs, snip);
  CHECK(d.verdict == BarOwnerVerdict::kHold);
  CHECK(d.undecidable);
  CHECK(d.hold_reason ==
        BarOwnerDecision::HoldReason::kNoPresenceOnThread);
  // ⚠ undecidable 為真 = 呼叫端一個字都不改,in_use_ 維持上一次的 true。
  CHECK(!d.in_use);
}

// ── O15:他真的切走了 —— 那條執行緒說過讓位,而且收得到文字 → 隱藏 ────
TEST(bar_owner_a_thread_that_said_it_yielded_really_hides_the_bar) {
  std::vector<BarOwnerClient> cs;
  for (uint32_t i = 1; i <= 12; ++i) cs.push_back(Host(i, 1000 + i, 2000 + i));

  BarOwnerForeground fg = Fg(4242, 4343);
  fg.can_take_text = true;  // 記事本的編輯區有焦點
  const std::vector<BarOwnerYield> ys{Yield(4343, 900)};
  const BarOwnerDecision d = DecideBarOwner(cs, ys, fg, 1000);
  CHECK(d.verdict == BarOwnerVerdict::kTheirs);
  // ⚠ **不是** undecidable:這是一個確定的答案,呼叫端要照它把 in_use_
  //   寫成 false,3000 毫秒之後那一橫收起來。
  CHECK(!d.undecidable);
  CHECK(!d.in_use);
  CHECK_INT(d.focused_client, 0);

  // UWP 也要成立:框是 ApplicationFrameHost,讓位的是 CoreWindow 那條。
  BarOwnerForeground uwp = Fg(900, 901);
  uwp.can_take_text = true;
  uwp.inner_known = true;
  uwp.inner_pid = 4242;
  uwp.inner_tid = 4343;
  CHECK(DecideBarOwner(cs, ys, uwp, 1000).verdict == BarOwnerVerdict::kTheirs);
}

// ── O16:說過讓位,但那條執行緒此刻收不到文字 → 維持 ──────────────────
//
// ⚠ 讓位紀錄是**黏著的**(它必須活得比連線久),所以它不可以在使用者
//   點一下桌面之後就把那一橫永久壓住。桌面、工作列沒有鍵盤焦點視窗 ——
//   那不是「他在用別的輸入法打字」,只是「他此刻沒在打字」。
//   ⚠ 這一格也是 GetGUIThreadInfo 對提權宿主回 FALSE 時的樣子:方向
//     一律是「留著」,不是「亂收」。
TEST(bar_owner_a_yield_on_a_thread_that_cannot_take_text_only_holds) {
  std::vector<BarOwnerClient> cs;
  BarOwnerForeground desktop = Fg(4242, 4343);
  desktop.can_take_text = false;  // Progman / Shell_TrayWnd:沒有 hwndFocus
  const std::vector<BarOwnerYield> ys{Yield(4343, 900)};
  const BarOwnerDecision d = DecideBarOwner(cs, ys, desktop, 1000);
  CHECK(d.verdict == BarOwnerVerdict::kHold);
  CHECK(d.undecidable);
  CHECK(d.hold_reason == BarOwnerDecision::HoldReason::kYieldedButNoFocus);
}

// ── O17:讓位紀錄過期 → 回到維持(tid 被系統回收的那道防護)──────────
TEST(bar_owner_a_stale_yield_expires_back_into_hold) {
  std::vector<BarOwnerClient> cs;
  BarOwnerForeground fg = Fg(4242, 4343);
  fg.can_take_text = true;
  const std::vector<BarOwnerYield> ys{Yield(4343, 1000)};

  // 剛好還沒到期。
  const uint64_t ttl = BarOwnerYieldTtlMs();
  CHECK(DecideBarOwner(cs, ys, fg, 1000 + ttl - 1).verdict ==
        BarOwnerVerdict::kTheirs);
  // 剛好到期。⚠ 過期的後果是 kHold = 維持當下,而當下本來就是隱藏 ——
  //   所以過期本身製造不出任何使用者看得到的錯。
  CHECK(DecideBarOwner(cs, ys, fg, 1000 + ttl).verdict ==
        BarOwnerVerdict::kHold);
  CHECK(DecideBarOwner(cs, ys, fg, 1000 + ttl + 600000).verdict ==
        BarOwnerVerdict::kHold);
  // 別條執行緒的讓位紀錄一律不算。
  const std::vector<BarOwnerYield> other{Yield(9999, 1000)};
  CHECK(DecideBarOwner(cs, other, fg, 1000).verdict == BarOwnerVerdict::kHold);
  // host_tid == 0 的垃圾紀錄不可以對上「前景 tid 也是 0」以外的任何東西,
  // 而 tid==0 的前景在前面就已經被判成問不出來了。
  const std::vector<BarOwnerYield> junk{Yield(0, 1000)};
  CHECK(DecideBarOwner(cs, junk, fg, 1000).verdict == BarOwnerVerdict::kHold);
}

// ── O18:連線優先於讓位紀錄 —— 他切回來了 ────────────────────────────
//
// 呼叫端在 OnClientIdentified 就會把那條 tid 的讓位紀錄抹掉
// (status_bar.cc),但判準本身也不該依賴呼叫端記得做這件事。
TEST(bar_owner_a_live_presence_outranks_an_old_yield_on_the_same_thread) {
  std::vector<BarOwnerClient> cs{Host(7, 4242, 4343)};
  cs[0].session = 55;
  BarOwnerForeground fg = Fg(4242, 4343);
  fg.can_take_text = true;
  const std::vector<BarOwnerYield> ys{Yield(4343, 900)};
  const BarOwnerDecision d = DecideBarOwner(cs, ys, fg, 1000);
  CHECK(d.verdict == BarOwnerVerdict::kOurs);
  CHECK(d.in_use);
  CHECK_INT(d.focused_client, 7);
  CHECK_INT(d.focused_session, 55);
}

// ── O19:服務自己的視窗仍然優先於讓位紀錄 ────────────────────────────
//
// ⚠ 使用者切走輸入法 → 那條 tid 留下一筆讓位紀錄 → 他從系統匣打開設定。
//   設定視窗是服務自己的執行緒,不該去撞那筆紀錄(它的 tid 也不會撞),
//   而更重要的是:**那一刻的答案必須是「維持」**,不是「別人的」——
//   否則設定視窗一開,那一橫的狀態就被一件與它無關的事改寫了。
TEST(bar_owner_our_own_window_still_beats_a_yield_record) {
  const uint32_t kServicePid = 31337;
  std::vector<BarOwnerClient> cs;
  BarOwnerForeground gui = Fg(kServicePid, 555);
  gui.service_pid = kServicePid;
  gui.can_take_text = true;
  const std::vector<BarOwnerYield> ys{Yield(555, 900)};
  const BarOwnerDecision d = DecideBarOwner(cs, ys, gui, 1000);
  CHECK(d.verdict == BarOwnerVerdict::kHold);
  CHECK(d.hold_reason == BarOwnerDecision::HoldReason::kForegroundIsService);
}
