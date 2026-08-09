// windows/tests/test_elevation_policy.cc — 「宿主可不可以啟動服務」的真值表
//
// 這一格答錯的後果是「輸入法完全不存在,而且沒有任何錯誤訊息」
// (2026-08 的實測回報:內建 Administrator 帳號,三個症狀一個原因)。
// 所以這裡不挑幾個代表性的組合,而是**把整張表窮舉**。

#include "../common/elevation_policy.h"
#include "check.h"

using namespace rimewin;

namespace {

// 每一種 TokenSplit 都要出現在測試裡,漏一種就是漏一整類使用者。
const TokenSplit kAllSplits[] = {
    TokenSplit::kUnknown, TokenSplit::kNoLinkedToken,
    TokenSplit::kFullWithLinked, TokenSplit::kLimitedWithLinked};

const HostElevation kAllVerdicts[] = {
    HostElevation::kUnknown, HostElevation::kNormal, HostElevation::kSplitToken,
    HostElevation::kWholeSession, HostElevation::kServiceAccount};

}  // namespace

// ── 這一輪真正壞掉的那一格 ────────────────────────────────────────
//
// 內建 Administrator(SID 尾巴 -500)或 UAC 關掉:提權,但沒有連結權杖。
// 舊規則對它說「不啟動」,於是那位使用者**永遠**用不了這個輸入法。
TEST(whole_session_elevated_may_start_service) {
  const HostElevation e = ClassifyHostElevation(
      /*token_query_ok=*/true, /*is_elevated=*/true,
      TokenSplit::kNoLinkedToken, /*is_service_account=*/false);
  CHECK(e == HostElevation::kWholeSession);
  CHECK(MayStartUserService(e));
}

// ── 而這一格**不可以**被上面那個修法弄壞 ───────────────────────────
//
// 一般帳號是多數使用者。它以前是對的,現在也必須是對的。
TEST(normal_user_may_start_service) {
  // 完全不是系統管理員的帳號:沒提權、沒有連結權杖。
  const HostElevation plain = ClassifyHostElevation(
      true, /*is_elevated=*/false, TokenSplit::kNoLinkedToken, false);
  CHECK(plain == HostElevation::kNormal);
  CHECK(MayStartUserService(plain));

  // UAC 底下的系統管理員,平常用的那一半權杖(受限)。
  // ⚠ 這是「一般使用者」裡人數最多的一種,而它的 TokenElevationType
  //   是 Limited **不是** Default —— 用「有沒有連結權杖」當判準時很容易
  //   把這一種一起擋掉,那會讓大部分 Windows 使用者都用不了。
  const HostElevation limited = ClassifyHostElevation(
      true, /*is_elevated=*/false, TokenSplit::kLimitedWithLinked, false);
  CHECK(limited == HostElevation::kNormal);
  CHECK(MayStartUserService(limited));
}

// ── 原本那條規則保護的東西,必須還在 ──────────────────────────────
TEST(split_token_elevated_must_not_start_service) {
  const HostElevation e = ClassifyHostElevation(
      true, /*is_elevated=*/true, TokenSplit::kFullWithLinked, false);
  CHECK(e == HostElevation::kSplitToken);
  CHECK(!MayStartUserService(e));
}

// LocalSystem / LOCAL SERVICE / NETWORK SERVICE。
// 登入畫面的 LogonUI.exe 是以 SYSTEM 執行而且會載入輸入法的 DLL,
// 而它的權杖形狀與內建 Administrator **一模一樣**(提權 + 沒有連結權杖)。
// 少了這一道閘,上面那個修法會讓我們在登入畫面上啟動一支
// %APPDATA% 指到 systemprofile 的服務。
TEST(service_account_must_not_start_service) {
  for (TokenSplit s : kAllSplits) {
    for (bool elevated : {false, true}) {
      const HostElevation e =
          ClassifyHostElevation(true, elevated, s, /*is_service_account=*/true);
      CHECK(e == HostElevation::kServiceAccount);
      CHECK(!MayStartUserService(e));
    }
  }
}

TEST(unknown_token_state_must_not_start_service) {
  // 權杖查詢失敗 —— 不管其他欄位長什麼樣。
  for (TokenSplit s : kAllSplits) {
    for (bool elevated : {false, true}) {
      for (bool svc : {false, true}) {
        const HostElevation e =
            ClassifyHostElevation(/*token_query_ok=*/false, elevated, s, svc);
        CHECK(e == HostElevation::kUnknown);
        CHECK(!MayStartUserService(e));
      }
    }
  }
  // 提權了,但問不出 TokenElevationType —— 也就是不知道有沒有另一半。
  const HostElevation e =
      ClassifyHostElevation(true, true, TokenSplit::kUnknown, false);
  CHECK(e == HostElevation::kUnknown);
  CHECK(!MayStartUserService(e));
}

// ── 窮舉:整張表只有兩格是「可以啟動」──────────────────────────────
//
// 這一條是**反向守門**。日後有人想放寬,他會先撞到這裡,
// 而不是讓一個放寬的判斷安靜地上線。
TEST(only_two_verdicts_may_start_service) {
  int allowed = 0;
  for (bool ok : {false, true})
    for (bool elevated : {false, true})
      for (TokenSplit s : kAllSplits)
        for (bool svc : {false, true}) {
          const HostElevation e = ClassifyHostElevation(ok, elevated, s, svc);
          if (!MayStartUserService(e)) continue;
          ++allowed;
          // 允許的話,只可能是這兩種之一。
          CHECK(e == HostElevation::kNormal ||
                e == HostElevation::kWholeSession);
        }
  // 真的有東西被允許 —— 不然這條斷言等於什麼都沒測。
  CHECK(allowed > 0);
}

// 標籤是驗證腳本比對的東西(verify_installer.sh 抓 doctor 印出來的那一行)。
// 少一個 case 的話,腳本會比對到 "?" 而永遠不成立 —— 那是安靜的失效。
TEST(every_verdict_has_a_tag_and_a_sentence) {
  for (HostElevation e : kAllVerdicts) {
    const char* tag = HostElevationTag(e);
    const char* zh = HostElevationZh(e);
    CHECK(tag != nullptr && tag[0] != '\0');
    CHECK(std::string(tag) != "?");
    CHECK(zh != nullptr && zh[0] != '\0');
    CHECK(std::string(zh) != "?");
    // 拒絕的三種一定要有給使用者看的一句話 —— 「拒絕必須被看見」。
    if (!MayStartUserService(e))
      CHECK(HostElevationTooltipW(e) != nullptr);
    else
      CHECK(HostElevationTooltipW(e) == nullptr);
  }
}
