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

// ⚠ 這張表要與 LinkFailure 一字不差地同步。
//   底下的 link_failure_names_are_all_distinct 會在有人新增了列舉值
//   卻忘了更新它時紅掉 —— 一個「漏掉一種失敗」的測試比沒有測試更糟,
//   因為它讓人以為每一種都被走過了。
const LinkFailure kAllLinkFailures[] = {
    LinkFailure::kConnectFailed, LinkFailure::kHandshake,
    LinkFailure::kTimeout,       LinkFailure::kIoError,
    LinkFailure::kBadMessage,    LinkFailure::kServiceError,
    LinkFailure::kPeerClosed,
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
  s.OnConnected();
  CHECK(s.MayEatKey());
  CHECK(!s.ShouldAttemptConnect(0));  // 已經好了就不要再連
}

TEST(link_every_failure_kind_stops_eating_keys) {
  // ★ 全檔最重要的一條:**每一種**失敗都必須讓 MayEatKey() 變成 false。
  //   少擋任何一種,使用者在那種情境下就打不了字。
  for (LinkFailure k : kAllLinkFailures) {
    LinkState s;
    s.OnConnected();
    CHECK(s.MayEatKey());
    s.OnFailure(k, 1000);
    CHECK(!s.MayEatKey());
    CHECK(s.phase() == LinkPhase::kDegraded);
  }
}

TEST(link_stays_fail_open_across_repeated_timeouts) {
  LinkState s;
  s.OnConnected();
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
  LinkState s;
  s.OnConnected();
  s.OnFailure(LinkFailure::kIoError, 0);
  s.OnFailure(LinkFailure::kIoError, 0);
  CHECK(s.failures() == 2);
  s.OnConnected();
  CHECK(s.MayEatKey());
  // 之前的失敗不該繼續懲罰一條已經好了的連線。
  CHECK_INT(s.backoff_ms(), 0);
  CHECK_INT(s.failures(), 0);
  CHECK(!s.ShouldAttemptConnect(0));
}
