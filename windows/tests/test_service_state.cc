// windows/tests/test_service_state.cc — 三種處境要說三句不同的話
//
// ⚠ **本檔刻意不含任何中日韓字面值**(與 test_ui_strings.cc 同一個理由):
//   W7 要求 `windows/` 底下除了 ui_strings.cc 之外命中數為 0,而測試檔
//   也在那個範圍裡。要比對字面時,一律拿 UiTextIn() 回來的東西互比,
//   **不寫死任何一句話** —— 那反而更好:改寫辭句不會弄紅這裡,
//   而把三句合併回同一句一定會紅。
//
// 這一組測試守的是 common/service_state.h 檔頭那個缺陷:
// 「還在準備 / 準備失敗 / 引擎不在」被壓成同一個布林,於是畫面上是
// 同一句紅字「輸入法沒有在跑」——而第一種情況那句話是假的。

#include "../common/service_state.h"

#include "../common/first_run_timing.h"

#include <string>
#include <vector>

#include "../common/protocol.h"
#include "../common/ui_strings.h"
#include "check.h"

using namespace rimewin;

namespace {

EngineFacts Facts(bool present, bool done, bool ok, bool says_not_ready) {
  EngineFacts f;
  f.engine_present = present;
  f.deploy_done = done;
  f.deploy_ok = ok;
  f.engine_says_not_ready = says_not_ready;
  return f;
}

const ServiceState kAllStates[] = {
    ServiceState::kReady, ServiceState::kPreparing,
    ServiceState::kPrepareFailed, ServiceState::kNotRunning};

const UiLang kAllLangs[] = {UiLang::kEnUs, UiLang::kZhHant, UiLang::kZhHans};

const char* StateName(ServiceState s) {
  switch (s) {
    case ServiceState::kReady: return "kReady";
    case ServiceState::kPreparing: return "kPreparing";
    case ServiceState::kPrepareFailed: return "kPrepareFailed";
    case ServiceState::kNotRunning: return "kNotRunning";
  }
  return "(?)";
}

}  // namespace

TEST(service_state_three_situations_are_three_different_states) {
  // 這五行就是舊的那一行布林拆開之後的全部內容。
  //   service_ready_ = engine_ && engine_->deploy_done() && engine_->deploy_ok();

  // 1. 引擎物件根本不在 —— 服務進程沒起來。
  CHECK(ServiceStateOf(Facts(false, false, false, false)) ==
        ServiceState::kNotRunning);
  // ⚠ 引擎不在的時候,另外三個旗標說什麼都不重要。
  CHECK(ServiceStateOf(Facts(false, true, true, false)) ==
        ServiceState::kNotRunning);

  // 2. 引擎在,還沒有結果 —— 正在準備(首次安裝的那 7~12 秒)。
  CHECK(ServiceStateOf(Facts(true, false, false, false)) ==
        ServiceState::kPreparing);

  // 3. 有終局結果,而結果是失敗。
  CHECK(ServiceStateOf(Facts(true, true, false, false)) ==
        ServiceState::kPrepareFailed);

  // 4. 一切正常。
  CHECK(ServiceStateOf(Facts(true, true, true, false)) == ServiceState::kReady);

  // 5. 上一次成功了,但引擎在線路上說自己還沒準備好 ——
  //    使用者剛按了「重新整理字詞」。deploy_done 不會退回 false,
  //    所以少了 kStDisabled 這一路,這一格會被說成「可以打字」。
  CHECK(ServiceStateOf(Facts(true, true, true, true)) ==
        ServiceState::kPreparing);

  // ⚠ 失敗要贏過「還沒有結果」:反過來的話,失敗會被說成「還在準備」,
  //   而使用者會一直等一件不會發生的事。
  CHECK(ServiceStateOf(Facts(true, true, false, true)) ==
        ServiceState::kPrepareFailed);
}

TEST(service_state_every_state_says_a_different_sentence) {
  // ⚠ **這一條就是那個缺陷的回歸測試。**
  //   把三種合併回同一句 —— 不管是讓對照表全回同一條 UiString,
  //   還是把三條 catalog 條目寫成同樣的字 —— 這裡都會紅。
  int compared = 0;
  for (size_t i = 0; i < sizeof(kAllStates) / sizeof(kAllStates[0]); ++i) {
    for (size_t j = i + 1; j < sizeof(kAllStates) / sizeof(kAllStates[0]);
         ++j) {
      const ServiceState a = kAllStates[i];
      const ServiceState b = kAllStates[j];

      // 側欄:四種狀態各有一句,兩兩不同。
      // ⚠ 每一次比對都要算一個斷言:check.h 的「斷言數為 0 就算失敗」
      //   那條規則,要靠報表上的數字反映真的比了幾次才有意義。
      ++::rimewin_test::Assertions();
      if (SidebarStatusTextFor(a) == SidebarStatusTextFor(b)) {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "%s 與 %s 在側欄上指到同一條字串",
                      StateName(a), StateName(b));
        ::rimewin_test::Fail(__FILE__, __LINE__, buf);
      }
      ++compared;

      // 那一橫:就緒時畫的是四格(沒有句子),其餘三種兩兩不同。
      if (a != ServiceState::kReady && b != ServiceState::kReady) {
        ++::rimewin_test::Assertions();
        if (StatusTextFor(a) == StatusTextFor(b)) {
          char buf[200];
          std::snprintf(buf, sizeof(buf),
                        "%s 與 %s 在那一橫上指到同一條字串",
                        StateName(a), StateName(b));
          ::rimewin_test::Fail(__FILE__, __LINE__, buf);
        }
        ++compared;
      }

      // ⚠ 只比 enum 值擋不住「兩條 catalog 條目寫成同樣的字」——
      //   那在畫面上與合併成一條完全一樣。三個語系逐一比字面。
      for (UiLang lang : kAllLangs) {
        const std::wstring sa = UiTextIn(lang, SidebarStatusTextFor(a));
        const std::wstring sb = UiTextIn(lang, SidebarStatusTextFor(b));
        ++::rimewin_test::Assertions();
        if (sa == sb) {
          char buf[220];
          std::snprintf(buf, sizeof(buf),
                        "側欄:%s 與 %s 在語系 %d 上是同一句話",
                        StateName(a), StateName(b), static_cast<int>(lang));
          ::rimewin_test::Fail(__FILE__, __LINE__, buf);
        }
        ++compared;
        if (a != ServiceState::kReady && b != ServiceState::kReady) {
          const std::wstring ba = UiTextIn(lang, StatusTextFor(a));
          const std::wstring bb = UiTextIn(lang, StatusTextFor(b));
          ++::rimewin_test::Assertions();
          if (ba == bb) {
            char buf[220];
            std::snprintf(buf, sizeof(buf),
                          "那一橫:%s 與 %s 在語系 %d 上是同一句話",
                          StateName(a), StateName(b), static_cast<int>(lang));
            ::rimewin_test::Fail(__FILE__, __LINE__, buf);
          }
          ++compared;
        }
      }
    }
  }
  // ⚠ 範圍斷言:一組都沒比而報「沒有違規」正是要堵死的失效方式。
  //   4 個狀態兩兩 = 6 組,每組側欄 1 + 三語 3,非就緒的 3 組再加 4。
  CHECK_INT(compared, 6 * 4 + 3 * 4);
}

TEST(service_state_preparing_is_not_a_failure) {
  // ⚠ 這是整組測試裡最重要的一條。
  //   「正在準備」時輸入法**在跑**。把它畫成紅字、或者讓它與「沒有在跑」
  //   共用同一句,就是用兩種方式再說一次同一句謊話。
  CHECK(!StateIsFailure(ServiceState::kPreparing));
  CHECK(!StateIsFailure(ServiceState::kReady));
  CHECK(StateIsFailure(ServiceState::kPrepareFailed));
  CHECK(StateIsFailure(ServiceState::kNotRunning));

  // 而且那兩句話在三個語系裡都不可以一樣。
  for (UiLang lang : kAllLangs) {
    const std::wstring preparing =
        UiTextIn(lang, StatusTextFor(ServiceState::kPreparing));
    const std::wstring dead =
        UiTextIn(lang, StatusTextFor(ServiceState::kNotRunning));
    CHECK(!preparing.empty());
    CHECK(preparing != dead);
  }
}

TEST(service_state_only_ready_draws_the_four_cells) {
  // 四格(中/En、简/繁、方案名、設定)在其餘三種狀態下顯示的都是假的:
  // 引擎還沒準備好的時候,那些值不是「現在的狀態」,是預設值。
  CHECK(StateShowsCells(ServiceState::kReady));
  CHECK(!StateShowsCells(ServiceState::kPreparing));
  CHECK(!StateShowsCells(ServiceState::kPrepareFailed));
  CHECK(!StateShowsCells(ServiceState::kNotRunning));

  // 就緒時那一橫沒有句子 —— 它畫的是四格。
  CHECK(StatusTextFor(ServiceState::kReady) == UiString::kUiStringCount);
  CHECK(UiText(StatusTextFor(ServiceState::kReady))[0] == L'\0');
}

TEST(service_state_every_sentence_exists_in_every_language) {
  // 對照表要是**全的**:少一種狀態就回空字串的話,畫面上是一片空白,
  // 而空白在截圖上與「一切正常」長得一樣。
  int checked = 0;
  for (ServiceState s : kAllStates) {
    for (UiLang lang : kAllLangs) {
      const wchar_t* side = UiTextIn(lang, SidebarStatusTextFor(s));
      CHECK(side != nullptr);
      CHECK(side[0] != L'\0');
      ++checked;
      if (s != ServiceState::kReady) {
        const wchar_t* bar = UiTextIn(lang, StatusTextFor(s));
        CHECK(bar != nullptr);
        CHECK(bar[0] != L'\0');
        ++checked;
      }
    }
  }
  CHECK_INT(checked, 4 * 3 + 3 * 3);
}

TEST(service_state_reads_the_not_ready_flag_off_the_wire) {
  // ⚠ protocol.h:167 定義並回填了這個旗標,而在這一輪之前
  //   整個 windows/ **沒有任何一處讀它**。這一條把它釘住。
  CHECK(SnapshotSaysNotReady(kStDisabled));
  CHECK(SnapshotSaysNotReady(kStDisabled | kStAsciiMode | kStSimplified));
  CHECK(!SnapshotSaysNotReady(0));
  CHECK(!SnapshotSaysNotReady(kStComposing | kStAsciiMode | kStSimplified |
                              kStFullShape | kStAsciiPunct));

  // 帶著這個旗標的那一份快照,其餘欄位不可信:引擎在準備期間對每一顆
  // 按鍵回的是一份**預設建構**的快照,除了這個旗標以外全是 0。
  // 拿它去更新指示器的話,使用者剛切成 En 的那一格會自己跳回「中」。
  CHECK(!SnapshotFlagsAreUsable(kStDisabled));
  CHECK(!SnapshotFlagsAreUsable(kStDisabled | kStSimplified));
  CHECK(SnapshotFlagsAreUsable(kStSimplified));
  CHECK(SnapshotFlagsAreUsable(0));
}

TEST(service_state_flag_survives_a_real_round_trip_on_the_wire) {
  // ⚠ 上面那一條只驗了位元運算。這一條走**真的**編解碼:
  //   旗標如果在線路格式上掉了,狀態列就永遠看不到「引擎還沒準備好」,
  //   而畫面上的症狀與「有人把三種合併回同一句」一模一樣。
  Result r;
  r.handled = false;
  r.snap.status_flags = kStDisabled;
  const std::string wire = EncodeResult(7, r);
  Result back;
  uint32_t seq = 0;
  CHECK(DecodeResult(wire, &seq, &back));
  CHECK_INT(seq, 7);
  CHECK(SnapshotSaysNotReady(back.snap.status_flags));
  CHECK(ServiceStateOf(Facts(true, true, true,
                             SnapshotSaysNotReady(back.snap.status_flags))) ==
        ServiceState::kPreparing);
}

// ── W3:「首次要等多久」四份碼講四個數字,其中兩份使用者讀得到 ────
//
//   installer/luminakey.iss 的 FinishedLabel  一到數分鐘   ← 使用者讀得到
//   common/service_state.h / status_bar.cc     7~12 秒
//   service/engine.h                           可能好幾分鐘
//   setup/doctor.cc 的診斷報告                 一到數分鐘   ← 使用者讀得到
//
// 實測是 7~12 秒。也就是說使用者讀得到的那兩處差了一個數量級,而且是
// 往**壞的**那一邊差 —— 一個等了十幾秒的人會以為還要再等好幾分鐘。
// (對照:安卓端同一句話是「Takes about 13 seconds.」,有數字而且是對的。)
//
// ⚠ 這一條驗的不是「等於 7 或 12」——那樣的測試只是把同一個數字再抄
//   一份,而抄一份正是這件事本身。驗的是**性質**:使用者讀得到的那句
//   話必須是秒級的,而且下界要比上界小。
TEST(first_run_timing_is_measured_seconds_not_worst_case_minutes) {
  CHECK(kFirstDeployTypicalLowSec > 0);
  CHECK(kFirstDeployTypicalLowSec < kFirstDeployTypicalHighSec);
  // 一分鐘以上就不再是「馬上就好」,而一個被嚇跑的人不會回來確認
  // 它其實只花了十秒。
  CHECK(kFirstDeployTypicalHighSec < 60);
  // 逾時預算是**另一個問題**的答案(使用者自己灌了很大的詞典時,
  // 部署真的可能跑到分鐘級),所以它必須明顯比上面那一組寬 ——
  // 兩件事分開命名之後,就不會再有人拿逾時預算去嚇一個剛裝完的人。
  CHECK(kDeployWaitBudgetSec > kFirstDeployTypicalHighSec * 4);
  // Win + 空白鍵的清單多久才看得到我們(登錄檔是同步的,CTF 不是)。
  CHECK(kProfileVisibleObservedSec > 0);
}
