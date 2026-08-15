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
  // 對面**乾淨地**把連線關掉了(讀到 0 位元組),而不是我們等到逾時。
  //
  // ⚠ 這一格與 kTimeout 分開,不是為了好看。舊服務收到它解不開的 v2 HELLO
  //   時做的正是這件事:整則丟掉、關掉連線。若把它記成 kTimeout,
  //   「對面拒絕了我們的版本」與「對面只是慢」就變成同一個值 ——
  //   而前者該降級重試 v1,後者不該(再試一次只是多花 300ms 的宿主 UI
  //   執行緒時間,而且會把真正的原因蓋掉)。
  kPeerClosed,
  // 握手過了、訊息也解得開,但服務**給不出一個 session**。
  //
  // ⚠ 這一格是 2026-08-09 從 kBadMessage 裡分出來的,而分出來的理由是
  //   一次真的誤導:服務在 `NewSession()` 回 0 時仍然送一則
  //   session=0 的 SESSION_OK,而用戶端把它判成 kBadMessage ——
  //   於是診斷寫著「訊息解不開或序號錯位」,而線路格式**完全正常**。
  //   看到那句話的人會去查編解碼與分幀,那兩段都是好的;
  //   真正的原因是 librime 正在部署,那段期間 rs_session_create() 給不出東西。
  //
  //   「線路壞了」與「引擎現在沒空」要修的地方完全不同,所以它們不可以
  //   共用一個名字。服務端現在會明著回一則 ERROR(見 pipe_server.cc),
  //   這一格則負責接住舊服務送來的 session=0。
  kNoSession,
};

// 給診斷訊息用的名字。**不是**給程式判斷用的:比對請直接比 enum。
//
// 存在的理由:「連不上服務或握手失敗」這種把好幾種原因併成一句的訊息,
// 會讓下一個看到它的人往錯的方向查。每一種失敗都要說得出自己的名字。
inline const char* LinkFailureName(LinkFailure k) {
  switch (k) {
    case LinkFailure::kConnectFailed: return "開不了管道";
    case LinkFailure::kHandshake:     return "握手版本不合";
    case LinkFailure::kTimeout:       return "逾時";
    case LinkFailure::kIoError:       return "I/O 失敗";
    case LinkFailure::kBadMessage:    return "訊息解不開或序號錯位";
    case LinkFailure::kServiceError:  return "服務回了 ERROR";
    case LinkFailure::kPeerClosed:    return "對面關掉了連線";
    case LinkFailure::kNoSession:     return "服務給不出 session";
  }
  // 沒有 default:新增一個列舉值而忘了給名字時,編譯器會用 -Wswitch 提醒。
  // 走到這裡代表值不在列舉範圍內(記憶體被踩了),說出來比回空字串好。
  return "(未知的失敗種類)";
}

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
    // 一條連線要活多久才算「真的好了」。見 OnConnected / OnExchangeOk。
    int64_t stable_ms = 2000;
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

  // 握手成功。
  //
  // ⚠ **這裡不歸零退避**,而那是這個檔案裡最反直覺的一行。
  //
  //   舊版是 `backoff_ms_ = 0; failures_ = 0;`,讀起來很對:
  //   「之前的失敗不該讓一條已經好了的連線繼續被懲罰」。問題在
  //   「已經好了」這四個字 —— 握手成功只證明**這一刻**管道通,
  //   不證明這條連線活得下去。
  //
  //   於是「連上 → 第一顆鍵在服務端等到逾時 → ipc_client 的 Fail() 把
  //   整條連線 Close() → 500ms 後再連上」這個迴圈,退避**永遠停在
  //   initial_backoff_ms**,一次都不會升級:每一輪都先歸零再乘 2,
  //   而 0 乘 2 還是走 initial。節流因此只涵蓋「連不上」,
  //   完全不涵蓋「連上又立刻斷」—— 而後者才是使用者機器上正在發生的。
  //
  //   代價是真的:每一輪重連在服務端都是一次 SESSION_NEW(實測
  //   rs_session_create 442~753ms)加一次 EndSessionAsync(把使用者詞典
  //   寫回去),而那些正是把下一顆鍵擠爆的慢工作。十三個宿主一起以
  //   500ms 轉的時候,引擎那條 FIFO 前面永遠有人 —— 這是一個正回饋迴圈。
  //
  //   所以歸零延到 OnExchangeOk(),而且要求這條連線**活過 stable_ms**。
  void OnConnected(int64_t now_ms) {
    phase_ = LinkPhase::kReady;
    connected_at_ms_ = now_ms;
    connected_ = true;
    // next_attempt_ms_ 不動:kReady 期間 ShouldAttemptConnect 一律回 false,
    // 而下一次 OnFailure 會重算它。留著上一輪的值,診斷才看得出來歷。
  }

  void OnFailure(LinkFailure kind, int64_t now_ms) {
    ++failures_;
    last_failure_ = kind;
    phase_ = LinkPhase::kDegraded;
    // ⚠ 這裡**不重置 backoff_ms_**,而那正是修法的全部:
    //   OnConnected 不再歸零之後,一條活不過 stable_ms 的連線斷掉時,
    //   下面那個乘 2 是接著**上一輪**繼續倍增的,一路到 max_backoff_ms。
    connected_ = false;
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
  //
  // 退避的歸零在**這裡**,不在 OnConnected —— 而且要求這條連線已經活過
  // cfg_.stable_ms。判準是「撐得住」,不是「連得上」。
  //
  // ⚠ 這樣不會把「服務剛重開、第一顆鍵慢了一下、之後就正常」罰死:
  //   OnExchangeOk 在**每一次**成功往返時都會被呼叫,所以連線只要真的
  //   穩下來,過了 stable_ms 的第一次往返就把退避清乾淨了。
  //   真正被罰滿的只有「每一次連上都撐不過 stable_ms」那一種。
  //
  // ⚠ stable_ms 的預設值 2000 沒有量測依據,它是推測。行為由
  //   tests/test_link_state.cc 的 link_flap_escalates_backoff 與
  //   link_stable_connection_still_resets_backoff 釘住,值本身可以再調。
  void OnExchangeOk(int64_t now_ms) {
    if (phase_ != LinkPhase::kReady) return;
    if (!connected_ || now_ms - connected_at_ms_ < cfg_.stable_ms) return;
    backoff_ms_ = 0;
    failures_ = 0;
  }

  // 這一條連線(若還活著)已經活了多久。沒有連線 = -1。
  // 給診斷用:「連線摘要」那一行要說得出「上一條活了 300ms」。
  int64_t ConnectedForMs(int64_t now_ms) const {
    if (!connected_) return -1;
    const int64_t d = now_ms - connected_at_ms_;
    return d > 0 ? d : 0;
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
  int64_t connected_at_ms_ = 0;
  bool connected_ = false;
};

}  // namespace rimewin

#endif  // RIMEWIN_LINK_STATE_H_
