// windows/tests/test_link_state.cc — DLL 側「絕不吃掉按鍵」的不變式
//
// 這一組測試守的是這個專案吃過最大的虧:
//   > 編譯成功、單元測試全過、發布關卡全綠,而使用者一裝就按鍵永久變灰。
//
// 在 TSF 上,「按鍵變灰」= DLL 回報 pfEaten=TRUE 卻拿不到結果。
// 那顆鍵既沒進文件、也沒變成候選 —— 它消失了,而且在每一個程式裡都消失。

#include "../common/link_state.h"

#include "check.h"

using namespace rimewin;

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
  const LinkFailure kinds[] = {
      LinkFailure::kConnectFailed, LinkFailure::kHandshake,
      LinkFailure::kTimeout,       LinkFailure::kIoError,
      LinkFailure::kBadMessage,    LinkFailure::kServiceError,
  };
  for (LinkFailure k : kinds) {
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
