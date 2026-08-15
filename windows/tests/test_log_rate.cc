// windows/tests/test_log_rate.cc — 連線記錄的節流:速率,不是一輩子的總量
//
// 這一組守的是覆核者在 winfix-usable 上抓到的第三條:連線進出那兩行的
// 額度是 `conn_log_budget_{64}`,一輩子只扣不補,而 service.log **不是**
// 環形檔(main.cc 的大小檢查只在啟動時做一次)。於是那道額度擋掉的不是
// 雜訊,是**最近的訊號** —— 使用者下午撞到 #108 去撈記錄時,唯一分得開
// 「重連迴圈」與「引擎佇列塞住」的那組行早就不寫了。
//
// ⚠ 最後那一條(KeepsWritingAfterEightHours)是這一輪對這條線的**硬要求**
//   本身,寫成可執行的形式:使用者開機八小時之後撞到問題,那組行還在寫。
//   舊的 64 額度在這一條下面是紅的。

#include "../common/log_rate.h"

#include "check.h"

using namespace rimewin;

TEST(LogRate_BurstThenThrottle) {
  LogTokenBucket b(3, 1000);
  int sup = -1;
  // 桶一開始是滿的:連著三則都寫得出去,而且中間沒有壓掉任何一則。
  for (int i = 0; i < 3; ++i) {
    CHECK(b.Allow(10000, &sup));
    CHECK(sup == 0);
  }
  // 第四則沒有令牌了。
  CHECK(!b.Allow(10000, &sup));
  CHECK(b.pending_suppressed() == 1);
}

TEST(LogRate_SuppressedCountRidesOnTheNextLine) {
  LogTokenBucket b(1, 1000);
  int sup = -1;
  CHECK(b.Allow(0, &sup));
  CHECK(sup == 0);
  // 同一毫秒裡再來五則,全部被壓掉。
  for (int i = 0; i < 5; ++i) CHECK(!b.Allow(0, &sup));
  CHECK(b.pending_suppressed() == 5);
  // ⚠ 被壓掉的則數不可以消失:下一則寫得出去的行要把它帶出去。
  //   那個數字本身就是重連迴圈最強的訊號(一分鐘 137 條連線,
  //   比 10 行「存活=300ms」還說得清楚)。
  CHECK(b.Allow(1000, &sup));
  CHECK(sup == 5);
  // 帶出去之後歸零,不會被下一行重複計。
  CHECK(b.pending_suppressed() == 0);
}

TEST(LogRate_RefillRemainderIsKept) {
  // ⚠ 這一條擋的是「每次補完都把時鐘設成 now」那種寫法:
  //   一個每 999ms 問一次的呼叫端在那種寫法下**永遠**補不到令牌,
  //   而那正是「額度用完就再也不寫」換一個形狀回來。
  LogTokenBucket b(1, 1000);
  int sup = 0;
  CHECK(b.Allow(0, &sup));
  int64_t t = 0;
  int allowed = 0;
  for (int i = 0; i < 20; ++i) {
    t += 999;
    if (b.Allow(t, &sup)) ++allowed;
  }
  // 20 × 999ms = 19980ms → 至少補得到 19 顆令牌。
  CHECK_MSG(allowed >= 19, "餘數沒有留著,補不回令牌");
}

TEST(LogRate_TimeGoingBackwardsDoesNotStarve) {
  // GetTickCount() 49.7 天會繞回。倒退時不可以讓 tokens_ 往下掉 ——
  // 那個症狀是「記錄忽然整段不見」,而它查不出來。
  LogTokenBucket b(2, 1000);
  int sup = 0;
  CHECK(b.Allow(1000000, &sup));
  CHECK(b.Allow(1000000, &sup));
  CHECK(!b.Allow(1000000, &sup));
  // 時間倒退之後,照樣要能靠回補寫得出來。
  CHECK(!b.Allow(5, &sup));
  CHECK(b.Allow(5 + 1000, &sup));
}

TEST(LogRate_KeepsWritingAfterEightHours) {
  // ── 這一條就是這一輪的硬要求 ────────────────────────────────
  //
  // 情境:使用者早上登入,13 個宿主 × 每個兩條連線 = 26 條連線一次進場;
  // 中間整天正常用(每分鐘幾條);下午撞到 #108,被請去撈 service.log。
  // 那時「連線進場 / 離場」還必須在寫。
  LogTokenBucket b(kConnLogBurst, kConnLogRefillMs);
  int sup = 0;
  int written_at_login = 0;
  for (int i = 0; i < 26; ++i)
    if (b.Allow(0, &sup)) ++written_at_login;
  CHECK_MSG(written_at_login == kConnLogBurst,
            "登入那一波要寫得出滿桶那幾條 —— 『連續好幾行』才是樣式");
  CHECK(b.pending_suppressed() == 26 - kConnLogBurst);

  // 八小時之後(整天每分鐘都有連線進出,桶一直是空的)。
  const int64_t eight_hours = 8LL * 60 * 60 * 1000;
  int written_in_the_incident = 0;
  int64_t t = eight_hours;
  // 事故持續十分鐘,每 5 秒一條連線(重連迴圈的樣子)。
  for (int i = 0; i < 120; ++i) {
    t += 5000;
    if (b.Allow(t, &sup)) ++written_in_the_incident;
  }
  CHECK_MSG(written_in_the_incident > 0,
            "開機八小時之後撞到問題,連線那組行必須還在寫 —— "
            "一輩子只扣不補的額度在這裡是 0");
  // 而且不是只有一行:要看得出樣式。
  CHECK_MSG(written_in_the_incident >= 10,
            "十分鐘的事故裡只寫得出幾行的話,『連續好幾行存活=300ms』"
            "這個樣式讀不出來");
}

TEST(LogRate_LongRunStaysWithinTheFile) {
  // 另一半:節流本身不可以讓檔案無限長得太快。八小時的上限要留得住
  // 1 MiB 的檔案(連線那兩行 ≈ 280 位元組)。
  LogTokenBucket b(kConnLogBurst, kConnLogRefillMs);
  int sup = 0;
  int written = 0;
  for (int64_t t = 0; t < 8LL * 60 * 60 * 1000; t += 1000)
    if (b.Allow(t, &sup)) ++written;
  const int bytes = written * 280;
  CHECK_MSG(bytes < (1 << 20),
            "八小時的連線記錄不可以自己就把 1 MiB 吃完");
}
