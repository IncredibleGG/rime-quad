// windows/tests/test_link_state.cc — DLL 側「絕不吃掉按鍵」的不變式
//
// 這一組測試守的是這個專案吃過最大的虧:
//   > 編譯成功、單元測試全過、發布關卡全綠,而使用者一裝就按鍵永久變灰。
//
// 在 TSF 上,「按鍵變灰」= DLL 回報 pfEaten=TRUE 卻拿不到結果。
// 那顆鍵既沒進文件、也沒變成候選 —— 它消失了,而且在每一個程式裡都消失。

#include "../common/link_state.h"

#include <cstring>

#include "check.h"

using namespace rimewin;

namespace {

// ⚠ 這張表要與 LinkFailure 一字不差地同步。加一個列舉值就要加進這裡一筆。
//
//   漏掉**不會**編譯失敗、不會有紅字 —— 底下每一則測試只是少驗一種,
//   而報表上看起來完全一樣(還是「全過」)。一個「漏掉一種失敗」的測試
//   比沒有測試更糟,因為它讓人以為每一種都被走過了。
//   2026-08-09 加 kNoSession 時就漏了一次。
//
//   `LinkFailureName` 那個 switch 沒有 default,所以編譯器擋得住「漏了名字」;
//   擋不住的是這一份清單 —— 所以清單自己也要有一道檢查,
//   見底下的 link_failure_list_covers_every_enum_value。
const LinkFailure kAllLinkFailures[] = {
    LinkFailure::kConnectFailed, LinkFailure::kHandshake,
    LinkFailure::kTimeout,       LinkFailure::kIoError,
    LinkFailure::kBadMessage,    LinkFailure::kServiceError,
    LinkFailure::kPeerClosed,    LinkFailure::kNoSession,
};

}  // namespace

TEST(link_never_eats_key_before_handshake) {
  LinkState s;
  CHECK(s.phase() == LinkPhase::kDisconnected);
  CHECK(!s.MayEatKey());
  // 剛開機、服務還沒起來:第一次就該嘗試連線。
  CHECK(s.ShouldAttemptConnect(0));
}

TEST(link_eats_key_only_when_ready) {
  LinkState s;
  s.OnConnected(0);
  CHECK(s.MayEatKey());
  CHECK(!s.ShouldAttemptConnect(0));  // 已經好了就不要再連
}

TEST(link_every_failure_kind_stops_eating_keys) {
  // ★ 全檔最重要的一條:**每一種**失敗都必須讓 MayEatKey() 變成 false。
  //   少擋任何一種,使用者在那種情境下就打不了字。
  for (LinkFailure k : kAllLinkFailures) {
    LinkState s;
    s.OnConnected(0);
    CHECK(s.MayEatKey());
    s.OnFailure(k, 1000);
    CHECK(!s.MayEatKey());
    CHECK(s.phase() == LinkPhase::kDegraded);
  }
}

TEST(link_stays_fail_open_across_repeated_timeouts) {
  LinkState s;
  s.OnConnected(0);
  int64_t now = 0;
  for (int i = 0; i < 50; ++i) {
    s.OnFailure(LinkFailure::kTimeout, now);
    CHECK(!s.MayEatKey());
    now += 10;
    // 重連嘗試之間必須有退避,否則每一顆按鍵都會在宿主的 UI 執行緒上開管道。
    if (!s.ShouldAttemptConnect(now)) continue;
    s.OnAttempt(now);
  }
  CHECK(!s.MayEatKey());
}

TEST(link_backoff_grows_and_is_capped) {
  LinkState::Config cfg;
  cfg.initial_backoff_ms = 100;
  cfg.max_backoff_ms = 800;
  LinkState s(cfg);
  s.OnFailure(LinkFailure::kConnectFailed, 0);
  CHECK_INT(s.backoff_ms(), 100);
  s.OnFailure(LinkFailure::kConnectFailed, 0);
  CHECK_INT(s.backoff_ms(), 200);
  s.OnFailure(LinkFailure::kConnectFailed, 0);
  CHECK_INT(s.backoff_ms(), 400);
  s.OnFailure(LinkFailure::kConnectFailed, 0);
  CHECK_INT(s.backoff_ms(), 800);
  s.OnFailure(LinkFailure::kConnectFailed, 0);
  CHECK_INT(s.backoff_ms(), 800);  // 到頂就停,不會變成好幾分鐘
}

TEST(link_backoff_gate_respects_the_clock) {
  LinkState::Config cfg;
  cfg.initial_backoff_ms = 500;
  LinkState s(cfg);
  s.OnFailure(LinkFailure::kConnectFailed, 1000);
  CHECK(!s.ShouldAttemptConnect(1000));
  CHECK(!s.ShouldAttemptConnect(1499));
  CHECK(s.ShouldAttemptConnect(1500));
}

TEST(link_handshake_mismatch_backs_off_much_longer) {
  // 版本對不上是不會自己好的(除非使用者換了檔案),不該每半秒敲一次。
  LinkState::Config cfg;
  cfg.initial_backoff_ms = 500;
  cfg.handshake_backoff_ms = 30000;
  LinkState s(cfg);
  s.OnFailure(LinkFailure::kHandshake, 0);
  CHECK_INT(s.backoff_ms(), 30000);
  CHECK(!s.MayEatKey());
  CHECK(!s.ShouldAttemptConnect(29999));
  CHECK(s.ShouldAttemptConnect(30000));
}

TEST(link_peer_closing_the_connection_is_not_a_handshake_mismatch) {
  // 舊服務收到解不開的新版 HELLO 時,做的是「整則丟掉、關掉連線」。
  // 那要走一般的指數退避(對面可能只是正在重啟),**不是**版本不合的
  // 30 秒退避 —— 記錯的話,使用者升級到一半那段時間裡,每一個宿主進程
  // 都要等半分鐘才會再試一次。
  LinkState::Config cfg;
  cfg.initial_backoff_ms = 500;
  cfg.handshake_backoff_ms = 30000;
  LinkState s(cfg);
  s.OnFailure(LinkFailure::kPeerClosed, 0);
  CHECK_INT(s.backoff_ms(), 500);
  CHECK(!s.MayEatKey());
}

TEST(link_failure_names_are_all_distinct_and_nonempty) {
  // 每一種失敗都要說得出自己的名字。名字重複或空白 = 診斷訊息把兩件
  // 不同的事講成同一件,而這正是本輪要修掉的那個症狀
  // (「連不上服務或握手失敗」把三種完全不同的失敗併成一句)。
  const int n = static_cast<int>(sizeof(kAllLinkFailures) /
                                 sizeof(kAllLinkFailures[0]));
  for (int i = 0; i < n; ++i) {
    const char* a = LinkFailureName(kAllLinkFailures[i]);
    CHECK(a != nullptr && a[0] != '\0');
    for (int j = i + 1; j < n; ++j)
      CHECK(std::strcmp(a, LinkFailureName(kAllLinkFailures[j])) != 0);
  }
}

TEST(link_backoff_eats_almost_all_of_a_naive_retry_loop) {
  // ★ 這一條記的是一個真的踩到的陷阱。
  //
  //   rime_probe 的等待迴圈長這樣:「重試 100 次,每次之間睡 100ms」。
  //   看起來是「試了 100 次、等了 10 秒」。實際上不是 —— 每一次都要先過
  //   ShouldAttemptConnect() 那一關,而握手不合的退避是 30 秒。
  //   於是那 100 次裡**只有第一次**真的開過管道,其餘 99 次連試都沒試,
  //   然後報出一句「連不上服務或握手失敗」。
  //
  //   把這個算術釘在測試裡,是為了讓下一個寫等待迴圈的人看得到:
  //   「重試 N 次」不等於「嘗試了 N 次」。
  LinkState::Config cfg;
  cfg.initial_backoff_ms = 500;
  cfg.max_backoff_ms = 5000;
  cfg.handshake_backoff_ms = 30000;

  auto count_attempts = [&cfg](LinkFailure kind) {
    LinkState s(cfg);
    int attempts = 0;
    for (int64_t now = 0; now < 10000; now += 100) {
      if (!s.ShouldAttemptConnect(now)) continue;
      s.OnAttempt(now);
      ++attempts;
      s.OnFailure(kind, now);
    }
    return attempts;
  };

  // 握手不合:10 秒裡只試得到一次。
  CHECK_INT(count_attempts(LinkFailure::kHandshake), 1);
  // 連不上:500 → 1000 → 2000 → 4000 → 5000,10 秒裡五次。
  CHECK_INT(count_attempts(LinkFailure::kConnectFailed), 5);
}

TEST(link_recovers_cleanly_after_service_comes_back) {
  // ⚠ 這一支的預期在 #93/#108 那一輪**刻意改過**,改的是「什麼時候算
  //   恢復」:舊版是「握手成功的那一刻」,新版是「這條連線撐過
  //   stable_ms 並且真的成功往返過一次」。
  //
  //   舊判準把「連上又立刻斷」讀成恢復,於是那個迴圈的退避永遠停在
  //   500ms(見 link_flap_escalates_backoff)。而使用者要的「服務重開
  //   之後立刻又能打字」這件事**沒有變**:下面後半段就是它。
  LinkState::Config cfg;
  cfg.stable_ms = 2000;
  LinkState s(cfg);
  s.OnConnected(0);
  s.OnFailure(LinkFailure::kIoError, 0);
  s.OnFailure(LinkFailure::kIoError, 0);
  CHECK(s.failures() == 2);
  s.OnConnected(0);
  // 吃不吃按鍵**只看 phase**,與退避無關 —— 連上就立刻能打字,
  // 使用者感覺得到的那一半一個位元都沒有變。
  CHECK(s.MayEatKey());
  CHECK(!s.ShouldAttemptConnect(0));
  // 但退避要等這條連線證明自己撐得住。
  CHECK(s.backoff_ms() > 0);
  s.OnExchangeOk(2000);
  CHECK_INT(s.backoff_ms(), 0);
  CHECK_INT(s.failures(), 0);
}

// ⚠ 這一則守的是**上面那份清單漏了一筆**,而它剛剛真的發生過。
//
// 2026-08-09 加 kNoSession 時漏了加進 kAllLinkFailures:名字的重複檢查
// 照樣是綠的(它只是少驗一種),而報表上看不出任何差別。
//
// 「(未知的失敗種類)」是 LinkFailureName 在**值不在列舉範圍內**時的退路。
// 拿它當探針:從第一個值往後掃,凡是叫得出名字的都必須在清單裡。
// 這樣下一個人加一格而忘了加進清單時,這一則會紅,而不是靜靜地少驗一種。
TEST(link_failure_list_covers_every_enum_value) {
  const int listed = static_cast<int>(sizeof(kAllLinkFailures) /
                                      sizeof(kAllLinkFailures[0]));
  int named = 0;
  // 32 是一個綽綽有餘的上界;真正的終止條件是「這個值沒有名字」。
  for (int i = 0; i < 32; ++i) {
    const char* n = LinkFailureName(static_cast<LinkFailure>(i));
    if (n == nullptr || std::strcmp(n, "(未知的失敗種類)") == 0) break;
    ++named;
  }
  // 叫得出名字的有幾種,清單裡就該有幾筆。
  CHECK_INT(named, listed);
}

// 「引擎現在沒空」與「線路壞了」必須是兩件事。
//
// 併成同一格的代價是實際付過的:診斷寫著「訊息解不開或序號錯位」,
// 而編解碼與分幀一個位元都沒錯 —— 查的人被送去看一段好的程式碼,
// 而真正的原因(librime 正在部署,rs_session_create 給不出東西)沒有人看見。
// fail-open 之下使用者只是打出英文,所以記錄裡那個名字就是全部的線索。
TEST(no_session_is_not_the_same_as_a_broken_wire) {
  CHECK(std::strcmp(LinkFailureName(LinkFailure::kNoSession),
                    LinkFailureName(LinkFailure::kBadMessage)) != 0);
}


// ── 「連上又立刻斷」完全不受節流保護 ──────────────────────────────
//
// ★ 這一支是症狀 B 那個正回饋迴圈的可執行證明。
//
//   退避原本只涵蓋**連不上**:OnConnected() 一被呼叫就把 backoff_ms_
//   歸零。於是「連上 → 第一顆鍵逾時 → 整條連線被丟掉 → 500ms 後再連上」
//   這個迴圈**永遠停在 initial_backoff_ms**,一次都不會升級。
//
//   而每一輪重連在服務端都是一次 SESSION_NEW(實測 442~753ms 的
//   rs_session_create)+ 一次 EndSessionAsync(把使用者詞典寫回去)——
//   也就是說,節流失效本身就在生產「把下一顆鍵擠爆」的慢工作。
//   十三個宿主一起這樣轉的時候,引擎佇列前面永遠有人。
//
// 判準:一條**活不過 stable_ms** 的連線不算數,退避照樣倍增到上限。
TEST(link_flap_escalates_backoff) {
  LinkState::Config cfg;
  cfg.initial_backoff_ms = 500;
  cfg.max_backoff_ms = 5000;
  cfg.stable_ms = 2000;
  LinkState s(cfg);

  const int64_t kExpected[] = {500, 1000, 2000, 4000, 5000};
  int64_t now = 0;
  for (int i = 0; i < 5; ++i) {
    s.OnConnected(now);          // 連上了
    s.OnFailure(LinkFailure::kTimeout, now + 50);  // 50ms 之後第一顆鍵就逾時
    CHECK_INT(s.backoff_ms(), kExpected[i]);
    CHECK_INT(s.next_attempt_ms(), now + 50 + kExpected[i]);
    // 500ms 的固定週期在第二輪之後就不夠了 —— 那正是「有節流」的意思。
    if (i >= 1) CHECK(!s.ShouldAttemptConnect(now + 50 + 500));
    now = now + 50 + kExpected[i];
  }
  // 十輪之後仍然頂在上限,不會回頭。
  for (int i = 0; i < 5; ++i) {
    s.OnConnected(now);
    s.OnFailure(LinkFailure::kTimeout, now + 50);
    CHECK_INT(s.backoff_ms(), 5000);
    now = now + 50 + 5000;
  }
}

// 反過來的那一半:連線**真的穩下來**之後,退避一定要歸零。
//
// 少了這一條,上面那支可以靠「永遠不歸零」全綠 —— 而那個實作的意思是
// 「服務重開之後使用者要等五秒才打得出中文」,那不是修好,是換一個病。
TEST(link_stable_connection_still_resets_backoff) {
  LinkState::Config cfg;
  cfg.initial_backoff_ms = 500;
  cfg.max_backoff_ms = 5000;
  cfg.stable_ms = 2000;
  LinkState s(cfg);

  // 先抖三輪,把退避推上去。
  int64_t now = 0;
  for (int i = 0; i < 3; ++i) {
    s.OnConnected(now);
    s.OnFailure(LinkFailure::kTimeout, now + 50);
    now += 5000;
  }
  CHECK(s.backoff_ms() >= 2000);

  // 這一條連線活得久。
  s.OnConnected(now);
  s.OnExchangeOk(now + 10);      // 還不夠久 —— 不可以歸零
  CHECK(s.backoff_ms() >= 2000);
  s.OnExchangeOk(now + 1999);    // 差一毫秒也還是不夠
  CHECK(s.backoff_ms() >= 2000);
  s.OnExchangeOk(now + 2000);    // 撐過 stable_ms 了
  CHECK_INT(s.backoff_ms(), 0);
  CHECK_INT(s.failures(), 0);

  // 歸零之後再壞一次,要從 initial 重新開始,不是接著上一輪。
  s.OnFailure(LinkFailure::kTimeout, now + 3000);
  CHECK_INT(s.backoff_ms(), 500);
}
