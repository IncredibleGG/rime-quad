// windows/tests/test_update_flow.cc — 更新流程的**行為**
//
// 這一支守四件事,而每一件都對應一個真的會發生、而且已經有人被咬過的形狀:
//
//   1. **沒驗過摘要就不准交棒。** 把整個布林立方體跑一遍,不是挑幾組。
//   2. **每一種失敗都有自己的一句話。** 逐對比對 —— 這正是「三種不同的
//      失敗在畫面上是同一句紅字」那個缺陷。
//   3. **信任錨那一句在每一條路徑上都在。** 它是「誠實面對沒有簽章」
//      的兌現處;少在任何一格,那句話就變成看情況才說。
//   4. **交棒之後的和解分得出「被鎖住」與「不知道」。** 前者使用者做得到
//      一件事(關掉那些程式),後者只能回報 —— 兩者說同一句話等於騙人。

#include "../common/update_flow.h"

#include <set>
#include <string>
#include <vector>

#include "check.h"

using namespace rimewin;

namespace {

const UpdateFailure kAllFailures[] = {
    UpdateFailure::kNone,
    UpdateFailure::kSwitchOff,
    UpdateFailure::kUnreachable,
    UpdateFailure::kUnreadable,
    UpdateFailure::kOwnVersionUnknown,
    UpdateFailure::kDownloadInterrupted,
    UpdateFailure::kDownloadTooLarge,
    UpdateFailure::kSha256Mismatch,
    UpdateFailure::kStagingWriteFailed,
    UpdateFailure::kProductChanged,
    UpdateFailure::kBusyCannotStop,
    UpdateFailure::kElevationDeclined,
    UpdateFailure::kHandoffFailed,
    UpdateFailure::kFileLocked,
    UpdateFailure::kNotInstalled,
};
constexpr int kAllFailureCount =
    static_cast<int>(sizeof(kAllFailures) / sizeof(kAllFailures[0]));

const UpdateStage kAllStages[] = {
    UpdateStage::kIdle,      UpdateStage::kChecking, UpdateStage::kDownloading,
    UpdateStage::kVerifying, UpdateStage::kReady,    UpdateStage::kHandedOff,
};

}  // namespace

TEST(update_flow_failure_list_covers_the_enum) {
  // ⚠ 範圍斷言。加了一格失敗而忘了加進上面那張表的話,底下每一條
  //   「逐一比對」都會安靜地少測一格 —— 那正是這個專案最常見的失效方式。
  CHECK_INT(kAllFailureCount, static_cast<int>(UpdateFailure::kFailureCount));
}

TEST(update_flow_every_failure_says_something_different) {
  // 這一條就是那個缺陷本身:三種不同的失敗不可以是同一句紅字。
  std::set<int> seen;
  int checked = 0;
  for (UpdateFailure f : kAllFailures) {
    if (f == UpdateFailure::kNone) continue;  // kNone 沒有訊息
    const UiString s = UpdateFailureText(f);
    ++checked;
    // 有訊息,而且是真的字串。
    CHECK(s != UiString::kUiStringCount);
    CHECK(UiTextIn(UiLang::kZhHant, s)[0] != L'\0');
    if (seen.count(static_cast<int>(s))) {
      ::rimewin_test::Fail(__FILE__, __LINE__,
                           std::string("兩種失敗指到同一句:") +
                               UpdateFailureTag(f));
    }
    ++::rimewin_test::Assertions();
    seen.insert(static_cast<int>(s));
  }
  CHECK_INT(checked, kAllFailureCount - 1);
  CHECK_INT(static_cast<int>(seen.size()), kAllFailureCount - 1);
}

TEST(update_flow_failure_tags_are_distinct_ascii) {
  std::set<std::string> tags;
  for (UpdateFailure f : kAllFailures) {
    const std::string t = UpdateFailureTag(f);
    CHECK(!t.empty());
    CHECK(t != "UNKNOWN");
    // SHA256_MISMATCH 有數字。要的是「穩定、腳本比得動」,不是「只有字母」。
    for (char c : t)
      CHECK((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_');
    tags.insert(t);
  }
  CHECK_INT(static_cast<int>(tags.size()), kAllFailureCount);
}

TEST(update_flow_only_the_switch_case_points_at_the_switch) {
  // 「連不上伺服器」導向開關的話,一個網路問題看起來會像我們擋的。
  for (UpdateFailure f : kAllFailures) {
    const bool want = (f == UpdateFailure::kSwitchOff);
    CHECK(UpdateFailureNeedsSwitch(f) == want);
  }
  // 而且開關那一格不可以給重試 —— 再按一次一樣被擋。
  CHECK(!UpdateFailureCanRetry(UpdateFailure::kSwitchOff));
  CHECK(!UpdateFailureCanRetry(UpdateFailure::kProductChanged));
  CHECK(UpdateFailureCanRetry(UpdateFailure::kDownloadInterrupted));
  CHECK(UpdateFailureCanRetry(UpdateFailure::kFileLocked));
}

TEST(update_flow_net_results_are_not_all_the_same_red_line) {
  // 開關擋下 vs 連不上 vs 太大 —— 三種不同的下一步。
  CHECK(ClassifyManifestFetch(NetResult::kOk) == UpdateFailure::kNone);
  CHECK(ClassifyManifestFetch(NetResult::kBlocked) == UpdateFailure::kSwitchOff);
  CHECK(ClassifyManifestFetch(NetResult::kTransportError) ==
        UpdateFailure::kUnreachable);
  CHECK(ClassifyManifestFetch(NetResult::kHttpError) ==
        UpdateFailure::kUnreachable);
  CHECK(ClassifyManifestFetch(NetResult::kBadScheme) ==
        UpdateFailure::kUnreadable);
  CHECK(ClassifyManifestFetch(NetResult::kDowngraded) ==
        UpdateFailure::kUnreadable);

  CHECK(ClassifyDownload(NetResult::kOk) == UpdateFailure::kNone);
  // 下載到一半使用者把開關關掉:走的是 kBlocked,而它要說的是
  // 「開關」那一句,不是「下載失敗」。
  CHECK(ClassifyDownload(NetResult::kBlocked) == UpdateFailure::kSwitchOff);
  // 下載到一半斷線。
  CHECK(ClassifyDownload(NetResult::kTransportError) ==
        UpdateFailure::kDownloadInterrupted);
  CHECK(ClassifyDownload(NetResult::kTooLarge) ==
        UpdateFailure::kDownloadTooLarge);

  // 兩個分類器對同一個 NetResult 可以有不同答案,而且**應該**有 ——
  // 取清單失敗與下載失敗是兩件事。
  CHECK(ClassifyManifestFetch(NetResult::kTransportError) !=
        ClassifyDownload(NetResult::kTransportError));
}

TEST(update_flow_uac_declined_is_not_a_failure_message) {
  CHECK(ClassifyHandoff(true, 0) == UpdateFailure::kNone);
  CHECK(ClassifyHandoff(false, kWin32ErrorCancelled) ==
        UpdateFailure::kElevationDeclined);
  CHECK(ClassifyHandoff(false, 2) == UpdateFailure::kHandoffFailed);
  // 兩者不可以是同一句:一個是「你按了否」,一個是「我們叫不起它」。
  CHECK(UpdateFailureText(UpdateFailure::kElevationDeclined) !=
        UpdateFailureText(UpdateFailure::kHandoffFailed));
}

TEST(update_flow_never_hands_off_an_unverified_file) {
  // ⚠ **整個布林立方體。** 挑幾組會漏掉「某個組合下這條規則失效」,
  //   而那正是攻擊面最大的地方 —— 交出去的是一個要用系統管理員權限
  //   執行的檔案。
  int cases = 0;
  for (int known = 0; known < 2; ++known)
    for (int have = 0; have < 2; ++have)
      for (int present = 0; present < 2; ++present)
        for (int busy = 0; busy < 2; ++busy)
          for (int v = 0; v < 3; ++v)
            for (int a = 0; a < 3; ++a) {
              HandoffPreflight pre;
              pre.installed_version_known = known != 0;
              pre.have_manifest = have != 0;
              pre.file_present = present != 0;
              pre.deploy_running = busy != 0;
              pre.verdict = static_cast<UpdateVerdict>(v);
              pre.app_id = static_cast<AppIdVerdict>(a);
              pre.sha256_verified = false;
              UpdateFailure why = UpdateFailure::kNone;
              ++cases;
              CHECK(!MayHandOff(pre, &why));
              CHECK(why == UpdateFailure::kSha256Mismatch);
            }
  CHECK_INT(cases, 2 * 2 * 2 * 2 * 3 * 3);
}

TEST(update_flow_handoff_preflight_reasons) {
  auto base = []() {
    HandoffPreflight p;
    p.installed_version_known = true;
    p.have_manifest = true;
    p.file_present = true;
    p.sha256_verified = true;
    p.verdict = UpdateVerdict::kUpdateAvailable;
    p.app_id = AppIdVerdict::kSame;
    p.deploy_running = false;
    return p;
  };
  UpdateFailure why = UpdateFailure::kNone;

  CHECK(MayHandOff(base(), &why));
  CHECK(why == UpdateFailure::kNone);

  // 不知道自己是哪一版 → 不是「版本資訊讀不懂」,是**我們**這一側不知道。
  HandoffPreflight p = base();
  p.installed_version_known = false;
  CHECK(!MayHandOff(p, &why));
  CHECK(why == UpdateFailure::kOwnVersionUnknown);

  // 換了身分 → 裝下去會多一套。不給。
  p = base();
  p.app_id = AppIdVerdict::kChanged;
  CHECK(!MayHandOff(p, &why));
  CHECK(why == UpdateFailure::kProductChanged);
  // ⚠ 不知道對方是誰**不擋**:舊的清單沒有 app_id,擋了等於所有舊安裝
  //   從此更新不了。
  p.app_id = AppIdVerdict::kUnknown;
  CHECK(MayHandOff(p, &why));

  // 正在整理字詞 → 現在停不下來。這就是「服務停不下來」那一格:
  // 交棒之後安裝程式會殺掉我們,而中途被殺會留下做到一半的東西。
  p = base();
  p.deploy_running = true;
  CHECK(!MayHandOff(p, &why));
  CHECK(why == UpdateFailure::kBusyCannotStop);

  // 線上比較舊 / 一樣新 → 沒有東西要裝,而那**不是錯誤**。
  for (UpdateVerdict v : {UpdateVerdict::kUpToDate, UpdateVerdict::kDowngrade}) {
    p = base();
    p.verdict = v;
    CHECK(!MayHandOff(p, &why));
    CHECK(why == UpdateFailure::kNone);
  }

  // 檔案不見了(被防毒軟體收走、使用者自己刪了)。
  p = base();
  p.file_present = false;
  CHECK(!MayHandOff(p, &why));
  CHECK(why == UpdateFailure::kStagingWriteFailed);
}

TEST(update_flow_reconcile_tells_locked_apart_from_unknown) {
  UpdateFailure why = UpdateFailure::kNone;

  // 沒有交棒單 = 這是一次普通的啟動。
  CHECK(ReconcileHandoff(false, 0, 100, false, &why) ==
        UpdateOutcome::kNoHandoff);
  CHECK(why == UpdateFailure::kNone);

  // 裝上去了。
  CHECK(ReconcileHandoff(true, 200, 200, false, &why) ==
        UpdateOutcome::kInstalled);
  CHECK(why == UpdateFailure::kNone);

  // ⚠ 使用者可以在交棒之後、我們回來之前**自己**去裝一個更新的。
  //   用 == 比的話會判成「沒裝成」,然後對一個剛手動更新完的人
  //   說「更新沒有裝上去」。
  CHECK(ReconcileHandoff(true, 200, 201, false, &why) ==
        UpdateOutcome::kInstalled);

  // 沒裝成 + 開機佇列裡有我們的檔案 → **換檔被拒**。使用者做得到一件事。
  CHECK(ReconcileHandoff(true, 200, 100, true, &why) ==
        UpdateOutcome::kNotInstalled);
  CHECK(why == UpdateFailure::kFileLocked);

  // 沒裝成 + 佇列乾淨 → 我們不知道為什麼,而且要照實說。
  CHECK(ReconcileHandoff(true, 200, 100, false, &why) ==
        UpdateOutcome::kNotInstalled);
  CHECK(why == UpdateFailure::kNotInstalled);

  // 兩者不可以是同一句話。
  CHECK(UpdateFailureText(UpdateFailure::kFileLocked) !=
        UpdateFailureText(UpdateFailure::kNotInstalled));

  // 查不出現在裝的是哪一版 → 不可以樂觀地判成成功。
  CHECK(ReconcileHandoff(true, 200, 0, false, &why) ==
        UpdateOutcome::kNotInstalled);
}

TEST(update_card_always_carries_the_trust_anchor) {
  // ⚠ 這一條是「誠實面對信任錨」在程式碼裡的樣子:沒有簽章這件事
  //   **不是錯誤狀態才要說的話**,而是每一格都要在。
  int cases = 0;
  for (UpdateStage st : kAllStages)
    for (UpdateFailure f : kAllFailures)
      for (int net = 0; net < 2; ++net)
        for (int have = 0; have < 2; ++have)
          for (int ver = 0; ver < 3; ++ver)
            for (int app = 0; app < 3; ++app)
              for (int fv = 0; fv < 2; ++fv)
                for (int known = 0; known < 2; ++known) {
                  UpdateUiState s;
                  s.stage = st;
                  s.failure = f;
                  s.network_enabled = net != 0;
                  s.have_manifest = have != 0;
                  s.verdict = static_cast<UpdateVerdict>(ver);
                  s.app_id = static_cast<AppIdVerdict>(app);
                  s.file_verified = fv != 0;
                  s.installed_version_known = known != 0;
                  const UpdateCard c = DescribeUpdateCard(s);
                  ++cases;
                  CHECK(c.trust == UiString::kUpdateTrustAnchor);
                  // 有主要動作 = 使用者馬上要按下去的那一下,那時
                  // 信任錨一定得在畫面上。
                  if (c.has_action())
                    CHECK(c.trust == UiString::kUpdateTrustAnchor);
                  // 忙的時候不給任何動作 —— 按下去只會是第二次下載。
                  if (c.busy) {
                    CHECK(!c.has_action());
                    CHECK(!c.show_check_button);
                  }
                  // 沒驗過的檔案不可以出現「現在更新」。
                  if (c.action == UiString::kUpdateInstallNowButton)
                    CHECK(s.file_verified);
                }
  CHECK(cases >= 3000);
}

TEST(update_card_states_say_the_right_thing) {
  UpdateUiState s;
  // 一開始:還沒查過。
  CHECK(DescribeUpdateCard(s).status == UiString::kUpdateStatusIdle);
  CHECK(DescribeUpdateCard(s).show_check_button);

  // 查到了、有新版本 → 給「下載並更新」。
  s.have_manifest = true;
  s.app_id = AppIdVerdict::kSame;
  s.verdict = UpdateVerdict::kUpdateAvailable;
  UpdateCard c = DescribeUpdateCard(s);
  CHECK(c.status == UiString::kUpdateStatusAvailable);
  CHECK(c.action == UiString::kUpdateInstallButton);
  CHECK(c.show_page_button);

  // 下載完、驗過了 → 換成「現在更新」。
  s.stage = UpdateStage::kReady;
  s.file_verified = true;
  c = DescribeUpdateCard(s);
  CHECK(c.status == UiString::kUpdateStatusReady);
  CHECK(c.action == UiString::kUpdateInstallNowButton);

  // 已經最新 → 沒有動作。
  s = UpdateUiState();
  s.have_manifest = true;
  s.app_id = AppIdVerdict::kSame;
  s.verdict = UpdateVerdict::kUpToDate;
  c = DescribeUpdateCard(s);
  CHECK(c.status == UiString::kUpdateStatusUpToDate);
  CHECK(!c.has_action());

  // 線上比較舊 → 說出來,而且**不給**按鈕。
  s.verdict = UpdateVerdict::kDowngrade;
  c = DescribeUpdateCard(s);
  CHECK(c.status == UiString::kUpdateStatusDowngrade);
  CHECK(!c.has_action());

  // 換了身分 → 不下載,改請他去下載頁。
  s.verdict = UpdateVerdict::kUpdateAvailable;
  s.app_id = AppIdVerdict::kChanged;
  c = DescribeUpdateCard(s);
  CHECK(c.status == UiString::kUpdateErrProductChanged);
  CHECK(!c.has_action());
  CHECK(c.show_page_button);

  // 開關關著 → 一顆重試都不給,狀態是那一句開關的話。
  s = UpdateUiState();
  s.failure = UpdateFailure::kSwitchOff;
  c = DescribeUpdateCard(s);
  CHECK(c.status == UiString::kUpdateErrSwitchOff);
  CHECK(!c.show_check_button);
  CHECK(!c.has_action());

  // 下載中斷 → 給重試。
  s.failure = UpdateFailure::kDownloadInterrupted;
  c = DescribeUpdateCard(s);
  CHECK(c.status == UiString::kUpdateErrDownloadInterrupted);
  CHECK(c.show_check_button);
}
