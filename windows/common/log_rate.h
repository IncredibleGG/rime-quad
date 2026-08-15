// windows/common/log_rate.h — 記錄的節流:速率,不是一輩子的總量
//
// ── 為什麼上一版是錯的 ──────────────────────────────────────────
//
// 連線進出那兩行原本用 `std::atomic<int> conn_log_budget_{64}` 擋,而
// pipe_server.h 給的理由是「service.log 是 1 MiB 的環形檔,先被捲掉的
// 正好是稀有的那一種」。**那個理由是錯的**:
//
//   · service/main.cc 的大小檢查**一輩子只在行程啟動時做一次**
//     (too_big → freopen "w",否則 "a")。跑的期間那個檔案只會長,
//     沒有任何一行會被捲掉。
//   · 超過 1 MiB 時被丟掉的是**整個檔案**,而且發生在**下一次啟動**。
//
// 於是那道額度擋掉的不是雜訊,**是最近的訊號**。服務沒有閒置離開的路徑,
// 所以 64 用完之後,從登入到關機再也不會有第二行連線記錄 —— 而 64 很快
// 就沒:13 個宿主 × 每個兩條連線,光登入就 26 條。
//
// 後果很具體:使用者下午再撞到 #108、照慣例被請去撈 service.log 時,
// 唯一能在服務端分辨「重連迴圈」與「引擎佇列塞住」的那組行
// (存活=300ms、按鍵=1、逾時=1 連續好幾行),已經在登入後幾十分鐘就
// 用完額度不再寫了。
//
// ── 換成什麼 ────────────────────────────────────────────────────
//
// 令牌桶(token bucket),兩個參數:
//
//   · 容量 = 一開始、或安靜一陣子之後,可以**連著**寫幾則。
//     登入那一波 26 條會寫掉滿桶的那幾條 —— 那正是「連續好幾行」
//     這個樣式需要的密度。
//   · 回補間隔 = 之後每隔多久多一則。它決定了長時間的上限,
//     也就是這個檔案八小時之後還會不會長。
//
// 而被壓掉的則數**不會消失**:下一則寫得出來的行會把它帶出去
// (「這中間另有 N 條沒記」)。那個數字本身就是重連迴圈最強的訊號 ——
// 一分鐘內 137 條連線,比 10 行「存活=300ms」還說得清楚。
//
// ⚠ 這一份是**純邏輯**:時間由呼叫端傳進來,所以 windows/tests/ 在
//   Ubuntu 上驗得到「八小時之後還在寫」,不必等 CI、更不必等使用者。
#ifndef RIMEWIN_LOG_RATE_H_
#define RIMEWIN_LOG_RATE_H_

#include <cstdint>

namespace rimewin {

// 連線進出那兩行的參數。
//
// ⚠ 容量 10:登入那一波(13 個宿主 × 2)寫得出 10 條,剩下的變成一個
//   數字跟在第 11 條後面。抖動時 10 條「存活=300ms」已經是樣式。
// ⚠ 回補一分鐘一則:八小時 ≈ 480 條連線 × 兩行 ≈ 135 KB,
//   在 1 MiB 裡面站得住,而且**八小時後那組行還在寫** —— 這是這一輪
//   對這條線唯一的硬要求。
//   (真的被開著好幾天的機器由 service/main.cc 的執行期大小檢查兜底,
//    那是「檔案無限長」這個風險真正的擋板。)
constexpr int kConnLogBurst = 10;
constexpr int kConnLogRefillMs = 60000;

class LogTokenBucket {
 public:
  // capacity <= 0 或 refill_ms <= 0 會被夾成 1:一個「永遠不准寫」的
  // 節流器與「這一行不存在」沒有分別,而那是這整支檔案要修的缺陷。
  LogTokenBucket(int capacity, int refill_ms);

  // 現在這一則准不准寫。
  // *suppressed_before 回報**從上一則寫得出去的到這一則之間**被壓掉幾則
  // (只有回 true 時才有意義,而且回 true 之後就歸零)。
  bool Allow(int64_t now_ms, int* suppressed_before);

  // 已經被壓掉、還沒有被任何一行帶出去的則數。
  int pending_suppressed() const { return suppressed_; }

 private:
  void Refill(int64_t now_ms);

  int capacity_;
  int refill_ms_;
  int tokens_;
  int suppressed_ = 0;
  int64_t last_refill_ms_ = 0;
  bool started_ = false;
};

}  // namespace rimewin

#endif  // RIMEWIN_LOG_RATE_H_
