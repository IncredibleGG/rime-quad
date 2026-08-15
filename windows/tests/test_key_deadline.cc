// windows/tests/test_key_deadline.cc — 一顆按鍵的兩個上限,以及逾時那一份的去處
//
// 這一組守的是覆核者在 winfix-usable 上抓到的那條:按鍵逾時借用了
// kStDisabled,而那一份空快照會走進 pipe_server 的 push_ui ——
// 候選窗當場收掉、那一橫改寫成「正在準備字詞」而且卡在那裡。
//
// ⚠ 那條路的本體(engine.cc / pipe_server.cc)在開發機上編不起來
//   (要 MSVC)。所以判準搬到 common/key_deadline.h:
//
//   · 「逾時的那一份不可以碰 UI」 → DecideKeyUiAction(),這裡逐格驗。
//   · 「服務端必須先放棄,而且留得下管道的時間」 → 那個標頭裡的
//     static_assert。它們在**編譯**這個檔案時就會爆,所以這裡再用
//     執行期斷言把同一條寫一次 —— 兩者的分工是:static_assert 讓
//     破壞關係的人編不過,而下面這些讓 run_logic_tests.sh 的報表上
//     看得到「這一條真的被檢查了」(這個框架沒有 SKIP,斷言數為 0
//     就算失敗)。
//   · 「呼叫端真的有把它接上去」 → 那一格編譯器守不到,由
//     run_logic_tests.sh 的文字守門負責(見那支腳本裡的 #93 那一段)。

#include "../common/key_deadline.h"

#include "check.h"

using namespace rimewin;

TEST(KeyDeadline_TimedOutMustNotTouchUi) {
  // 逾時的那一份不是快照,是佔位 —— UI 一個像素都不准動。
  CHECK(DecideKeyUiAction(/*timed_out=*/true) == KeyUiAction::kLeaveUiAlone);
  // 沒有逾時的那一份才是引擎的現況。
  CHECK(DecideKeyUiAction(/*timed_out=*/false) == KeyUiAction::kUpdateUi);
  // ⚠ 兩格都要驗。只驗一格的話,把這支函式改成「永遠回 kLeaveUiAlone」
  //   仍然是綠的 —— 而那會讓候選窗從此不再更新,也就是完全打不出字。
  CHECK(DecideKeyUiAction(true) != DecideKeyUiAction(false));
}

TEST(KeyDeadline_ServiceGivesUpFirst) {
  // 服務端必須先放棄。反過來 = DLL 先 Fail() → Close(),一顆慢鍵的代價
  // 從「這顆鍵沒打出來」變成「整條連線被丟掉」。
  CHECK_MSG(kKeyDeadlineMs < kKeyTimeoutMs,
            "服務端的上限必須嚴格小於 DLL 端的逾時");
  // 光是「小於」不夠:放棄之後還要把回覆寫進管道並序列化。
  CHECK_MSG(kKeyDeadlineMs + kKeyPipeMarginMs <= kKeyTimeoutMs,
            "服務端放棄之後送回覆的時間沒有留夠 —— 等於沒有先放棄");
  // 餘裕不可以是 0:那樣上面那一條會退化成「小於等於」。
  CHECK(kKeyPipeMarginMs > 0);
}

TEST(KeyDeadline_SlowKeyLogIsBelowTheCap) {
  // KEY_MS 那一行要看得到「快要逾時」的那一段,不能只記已經逾時的。
  CHECK_MSG(kKeySlowLogMs < kKeyDeadlineMs,
            "慢鍵的記錄門檻必須低於上限,否則只有逾時的那些會被記下來");
  CHECK(kKeySlowLogMs > 0);
}

TEST(KeyDeadline_NoLongerTheOldRegressionBand) {
  // ⚠ 這一條是**針對那段迴歸帶**的。
  //
  //   舊的組合是服務端 35 / DLL 50:引擎工作落在 35–50ms 的按鍵,
  //   在加上限之前打得出中文,加上之後變成 fail-open 的英文字母。
  //   放寬之後那一段回來了,而這裡把它釘住 —— 哪天有人想「保守一點」
  //   把服務端調回 50 以下,這一條會紅並且指著理由。
  CHECK_MSG(kKeyDeadlineMs >= 50,
            "服務端的上限低於 50ms 會重新造出一段『本來打得出中文、"
            "現在插英文字母』的迴歸帶");
  // 而 DLL 端不可以無限放寬:它是宿主 UI 執行緒最壞停住的時間,
  // 超過人「以為程式沒收到、於是再按一次」的門檻就不划算了。
  CHECK_MSG(kKeyTimeoutMs <= 200,
            "DLL 端的逾時是宿主 UI 執行緒最壞停住的時間,不可以超過 200ms");
}
