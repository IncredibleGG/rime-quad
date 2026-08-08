// windows/common/link_state.h — DLL 側連線狀態機(fail-open)
//
// 這個檔案存在的唯一理由,是這條教訓:
//
//   > 有過編譯成功、單元測試全過、發布關卡全綠,而使用者一裝就按鍵永久變灰。
//
// 「按鍵永久變灰」在 TSF 上的具體長相是:DLL 對宿主回報 `pfEaten = TRUE`,
// 然後因為服務進程沒起來 / 崩了 / 版本不合而拿不到任何結果 ——
// 於是那顆鍵既沒有進到文件裡,也沒有變成候選字。**它就消失了。**
// 使用者看到的是「鍵盤壞了」,而且在每一個程式裡都壞。
//
// 所以本狀態機只保護一條不變式:
//
//   ⚠ **只有在確定拿得到服務進程的回覆時,才可以吃掉按鍵。**
//      其餘每一種狀態 —— 沒連上、握手版本不合、逾時、對面崩了、
//      解碼失敗、序號對不上 —— 一律放行,讓宿主自己處理那顆鍵。
//
// 輸入法沒作用,使用者會抱怨;輸入法吃掉按鍵,使用者的電腦不能打字。
// 這兩者不是同一個等級的失敗。
//
// 狀態機是純邏輯,沒有 windows.h、沒有時鐘 —— 時間由呼叫端傳進來,
// 這樣「逾時三次之後仍然不吃按鍵」這種事才測得出來,不必真的等三秒。
//
#ifndef RIMEWIN_LINK_STATE_H_
#define RIMEWIN_LINK_STATE_H_

#include <cstdint>

namespace rimewin {

enum class LinkPhase {
  kDisconnected,  // 還沒連上,或連線已被放棄
  kReady,         // 握手完成,可以吃按鍵
  kDegraded,      // 出過事,退避中。**不吃按鍵**
};

enum class LinkFailure {
  kConnectFailed,   // 管道不存在或開不起來(服務沒跑)
  kHandshake,       // 版本對不上(協議版本或 rime_shell ABI)
  kTimeout,         // 送出去了,但在預算內沒等到回覆
  kIoError,         // 寫入或讀取失敗,對面多半已經不在了
  kBadMessage,      // 解碼失敗或序號對不上 —— 串流錯位,不可重新同步
  kServiceError,    // 對面明確回了 ERROR
};

class LinkState {
 public:
  struct Config {
    // 第一次退避多久才重試。太短會在服務真的死掉時每次按鍵都嘗試連線
    // (那是在宿主的 UI 執行緒上開管道,使用者會感覺到卡)。
    int64_t initial_backoff_ms = 500;
    // 退避上限。到頂之後就一直是這個間隔 —— 使用者手動把服務叫起來之後,
    // 不該還要等好幾分鐘才恢復。
    int64_t max_backoff_ms = 5000;
    // 握手版本不合是**不會自己好**的:除非使用者換了檔案。
    // 這種情形退避到上限,不要每半秒去敲一次。
    int64_t handshake_backoff_ms = 30000;
  };

  // 不寫成 `LinkState(Config cfg = {})`:類別內用還沒完成的巢狀型別當
  // 預設引數,GCC 會拒絕。分成兩個建構子是可攜的寫法。
  LinkState() = default;
  explicit LinkState(const Config& cfg) : cfg_(cfg) {}

  LinkPhase phase() const { return phase_; }

  // 這是全檔最重要的一行。
  bool MayEatKey() const { return phase_ == LinkPhase::kReady; }

  // 現在該不該嘗試(重新)連線。
  bool ShouldAttemptConnect(int64_t now_ms) const {
    if (phase_ == LinkPhase::kReady) return false;
    return now_ms >= next_attempt_ms_;
  }

  void OnAttempt(int64_t now_ms) { last_attempt_ms_ = now_ms; }

  // 握手成功。退避歸零 —— 之前的失敗不該讓一條已經好了的連線繼續被懲罰。
  void OnConnected() {
    phase_ = LinkPhase::kReady;
    backoff_ms_ = 0;
    failures_ = 0;
    next_attempt_ms_ = 0;
  }

  void OnFailure(LinkFailure kind, int64_t now_ms) {
    ++failures_;
    last_failure_ = kind;
    phase_ = LinkPhase::kDegraded;
    if (kind == LinkFailure::kHandshake) {
      backoff_ms_ = cfg_.handshake_backoff_ms;
    } else {
      backoff_ms_ = backoff_ms_ == 0
                        ? cfg_.initial_backoff_ms
                        : (backoff_ms_ * 2 > cfg_.max_backoff_ms
                               ? cfg_.max_backoff_ms
                               : backoff_ms_ * 2);
    }
    next_attempt_ms_ = now_ms + backoff_ms_;
  }

  // 一次成功的請求／回覆。用來確認連線還活著。
  void OnExchangeOk() {
    if (phase_ == LinkPhase::kReady) failures_ = 0;
  }

  int failures() const { return failures_; }
  LinkFailure last_failure() const { return last_failure_; }
  int64_t next_attempt_ms() const { return next_attempt_ms_; }
  int64_t backoff_ms() const { return backoff_ms_; }

 private:
  Config cfg_;
  LinkPhase phase_ = LinkPhase::kDisconnected;
  int failures_ = 0;
  LinkFailure last_failure_ = LinkFailure::kConnectFailed;
  int64_t backoff_ms_ = 0;
  int64_t next_attempt_ms_ = 0;
  int64_t last_attempt_ms_ = 0;
};

}  // namespace rimewin

#endif  // RIMEWIN_LINK_STATE_H_
