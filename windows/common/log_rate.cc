#include "log_rate.h"

namespace rimewin {

LogTokenBucket::LogTokenBucket(int capacity, int refill_ms)
    : capacity_(capacity > 0 ? capacity : 1),
      refill_ms_(refill_ms > 0 ? refill_ms : 1),
      tokens_(capacity > 0 ? capacity : 1) {}

void LogTokenBucket::Refill(int64_t now_ms) {
  // 第一次呼叫時把時間軸釘在「現在」。⚠ 不可以拿 0 當起點:
  // GetTickCount() 開機就已經是好幾百萬,那會讓第一次呼叫直接補滿
  // ——桶本來就是滿的,不影響結果,但之後的 last_refill_ms_ 會落在
  // 一個與現在無關的地方。
  if (!started_) {
    started_ = true;
    last_refill_ms_ = now_ms;
    return;
  }
  // ⚠ 時間倒退(GetTickCount 49.7 天繞回、或呼叫端傳錯)一律當成
  //   「沒有經過時間」並重新對時。不處理的話那個負數會讓 tokens_
  //   往下掉,而症狀是「記錄忽然整段不見」——查不出來的那一種。
  if (now_ms < last_refill_ms_) {
    last_refill_ms_ = now_ms;
    return;
  }
  const int64_t elapsed = now_ms - last_refill_ms_;
  const int64_t gained = elapsed / refill_ms_;
  if (gained <= 0) return;
  // ⚠ 餘數要留著。每次都把 last_refill_ms_ 設成 now_ms 的話,
  //   一個「每 59 秒問一次」的呼叫端永遠補不到令牌 —— 那是本檔頭
  //   說的「額度用完就再也不寫」換一個形狀回來。
  last_refill_ms_ += gained * static_cast<int64_t>(refill_ms_);
  if (gained >= static_cast<int64_t>(capacity_ - tokens_)) {
    tokens_ = capacity_;
  } else {
    tokens_ += static_cast<int>(gained);
  }
}

bool LogTokenBucket::Allow(int64_t now_ms, int* suppressed_before) {
  Refill(now_ms);
  if (tokens_ <= 0) {
    ++suppressed_;
    if (suppressed_before) *suppressed_before = 0;
    return false;
  }
  --tokens_;
  if (suppressed_before) *suppressed_before = suppressed_;
  suppressed_ = 0;
  return true;
}

}  // namespace rimewin
