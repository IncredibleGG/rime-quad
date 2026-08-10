// windows/tests/test_net_ui.cc — 「連網」那一頁的呈現邏輯
//
// 這一頁上有三件事,寫壞了畫面看起來**完全正常**:
//
//   1. 開關關著時按「檢查更新」照樣連出去。畫面上會多一句「正在檢查
//      更新…」,而那正是使用者以為不會發生的事。
//   2. 開與關說同一句話。使用者看不出現在是哪一種狀態,而這一頁存在的
//      理由就是讓他看得出來。
//   3. 紀錄是空的時候什麼都不說。空白讓人分不出「沒連過」與「壞掉了」,
//      而「開關從沒開過所以紀錄是空的」正是使用者驗證我們的方式。
//
// 三件都在這裡,而且每一條都對應一種**植入違規**(見檔尾)。
//
// ⚠ 本檔刻意不含任何中日韓字面值(W7)。要斷言「兩句話不一樣」就比
//   指標與內容,不是比一段寫死的中文 —— 寫死的那一份會在翻譯改動時
//   變成雜訊,而雜訊會讓人把檢查關掉。

#include "../common/net_ui.h"

#include <cstring>
#include <string>
#include <vector>

#include "check.h"

using namespace rimewin;

namespace {

const UiLang kLangs[] = {UiLang::kEnUs, UiLang::kZhHant, UiLang::kZhHans};
constexpr int kLangCount = 3;

// 只給錯誤訊息用:紀錄裡的時間、主機、位元組數都是 ASCII。
std::string Narrow(const std::wstring& w) {
  std::string s;
  for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
  return s;
}

bool Has(const std::wstring& hay, const std::wstring& needle) {
  return !needle.empty() && hay.find(needle) != std::wstring::npos;
}

NetLogEntry Entry(int64_t at_ms, const char* host, NetPurpose p,
                  const char* label, NetOutcome o, int64_t bytes,
                  const char* detail) {
  NetLogEntry e;
  e.at_ms = at_ms;
  e.host = host;
  e.purpose = p;
  e.label = label;
  e.outcome = o;
  e.bytes = bytes;
  e.detail = detail;
  return e;
}

}  // namespace

// ── 開關 ────────────────────────────────────────────────────────

TEST(net_ui_switch_says_two_different_things) {
  // ⚠ 這一條擋的是「開與關壓成同一句」。壓成同一句在畫面上看起來
  //   完全正常 —— 只是使用者再也看不出開關現在是哪一邊。
  CHECK(NetSwitchSummary(true) != NetSwitchSummary(false));
  CHECK(NetSwitchStatus(true) != NetSwitchStatus(false));

  int checked = 0;
  for (UiLang l : kLangs) {
    const std::wstring on = UiTextIn(l, NetSwitchSummary(true));
    const std::wstring off = UiTextIn(l, NetSwitchSummary(false));
    CHECK(!on.empty());
    CHECK(!off.empty());
    CHECK(on != off);
    const std::wstring son = UiTextIn(l, NetSwitchStatus(true));
    const std::wstring soff = UiTextIn(l, NetSwitchStatus(false));
    CHECK(!son.empty());
    CHECK(!soff.empty());
    CHECK(son != soff);
    ++checked;
  }
  CHECK_INT(checked, kLangCount);
}

// ── 檢查更新 ────────────────────────────────────────────────────

TEST(net_ui_update_never_starts_while_the_switch_is_off) {
  // ⚠ **這是整個檔案裡最重要的一條。** 開關關著時按下「檢查更新」,
  //   絕不可以走到 kStart —— 那一條路後面接的是真的連線。
  //
  //   出口本身(net_gate.cc)也會再擋一次(fail-closed,每一跳重問),
  //   兩層是刻意的:這一層讓使用者**看到**「開關是關的」而不是
  //   「連不上」,而那兩句話要說的事完全不同。
  int checked = 0;
  for (bool running : {false, true}) {
    const UpdateAction a = DecideUpdateAction(false, running);
    CHECK(a != UpdateAction::kStart);
    CHECK(a == UpdateAction::kSwitchIsOff);
    ++checked;
  }
  CHECK_INT(checked, 2);

  // 開關開著才輪得到「已經在跑了」。
  CHECK(DecideUpdateAction(true, false) == UpdateAction::kStart);
  CHECK(DecideUpdateAction(true, true) == UpdateAction::kAlreadyRunning);

  // ⚠ 順序:開關**排在**「已經在跑了」前面。反過來寫的話,一次卡住的
  //   檢查會讓「開關是關的」永遠說不出口,而使用者讀到的是「請稍候」——
  //   一句在開關關著時完全是假的話。
  CHECK(DecideUpdateAction(false, true) != UpdateAction::kAlreadyRunning);

  // 三種動作各說各的話,而且「開關是關的」與另外兩種不同。
  CHECK(UpdateActionText(UpdateAction::kSwitchIsOff) !=
        UpdateActionText(UpdateAction::kStart));
  CHECK(UpdateActionText(UpdateAction::kSwitchIsOff) !=
        UpdateActionText(UpdateAction::kAlreadyRunning));
}

TEST(net_ui_update_states_are_five_different_sentences) {
  // ⚠ 「開關是關的」「連不上」「已經最新」「有新版本」「還沒接上來源」
  //   是五件不同的事。把任兩件壓成同一句紅字,正是這個專案在 Windows
  //   端犯過的錯(見任務清單裡「三種不同的失敗在畫面上是同一句紅字」)。
  const UpdateCheckState all[] = {
      UpdateCheckState::kNotWired, UpdateCheckState::kUpToDate,
      UpdateCheckState::kAvailable, UpdateCheckState::kBlocked,
      UpdateCheckState::kFailed};
  constexpr int n = 5;
  int compared = 0;
  for (int i = 0; i < n; ++i) {
    CHECK(!std::wstring(UiText(UpdateStateText(all[i]))).empty());
    for (int j = i + 1; j < n; ++j) {
      CHECK(UpdateStateText(all[i]) != UpdateStateText(all[j]));
      for (UiLang l : kLangs) {
        CHECK(std::wstring(UiTextIn(l, UpdateStateText(all[i]))) !=
              std::wstring(UiTextIn(l, UpdateStateText(all[j]))));
      }
      ++compared;
    }
  }
  CHECK_INT(compared, n * (n - 1) / 2);

  // 「還沒接上更新來源」**不可以**被講成「已經是最新的」。
  // 那是「宣稱做不到的事」的反面:宣稱做了一件其實沒做的事。
  CHECK(UpdateStateText(UpdateCheckState::kNotWired) !=
        UpdateStateText(UpdateCheckState::kUpToDate));
  // 「開關是關的」與按鈕按下去當場擋掉時說的是同一句 —— 兩條路一句話。
  CHECK(UpdateStateText(UpdateCheckState::kBlocked) ==
        UpdateActionText(UpdateAction::kSwitchIsOff));
}

// ── 時間 ────────────────────────────────────────────────────────

TEST(net_ui_log_time_is_computed_not_guessed) {
  // 固定的幾個時刻,逐一算過。⚠ 不走 localtime/gmtime —— 那兩支吃
  //   行程的時區設定,而那樣的測試在別人的機器上會是紅的。
  CHECK_STR(Narrow(FormatNetLogTime(0, 0)), "1970-01-01 00:00:00");
  CHECK_STR(Narrow(FormatNetLogTime(86399LL * 1000, 0)),
            "1970-01-01 23:59:59");
  CHECK_STR(Narrow(FormatNetLogTime(86400LL * 1000, 0)),
            "1970-01-02 00:00:00");
  // 有名的那一秒。
  CHECK_STR(Narrow(FormatNetLogTime(1000000000LL * 1000, 0)),
            "2001-09-09 01:46:40");
  // 時區真的有加進去(UTC+8)。
  CHECK_STR(Narrow(FormatNetLogTime(1000000000LL * 1000, 480)),
            "2001-09-09 09:46:40");
  // 負偏移要往前跨一天,而且 C++ 的 / 對負數是向零取整 ——
  // 沒有向下取整的除法,這一行會變成 1970-01-01。
  CHECK_STR(Narrow(FormatNetLogTime(0, -300)), "1969-12-31 19:00:00");
  // 閏日。
  CHECK_STR(Narrow(FormatNetLogTime(1582977600LL * 1000, 0)),
            "2020-02-29 12:00:00");
  // 毫秒不進位、也不四捨五入成下一秒。
  CHECK_STR(Narrow(FormatNetLogTime(999, 0)), "1970-01-01 00:00:00");
  CHECK_STR(Narrow(FormatNetLogTime(1999, 0)), "1970-01-01 00:00:01");
}

TEST(net_ui_bytes_are_readable) {
  CHECK_STR(Narrow(FormatNetBytes(0)), "0 B");
  CHECK_STR(Narrow(FormatNetBytes(1023)), "1023 B");
  CHECK_STR(Narrow(FormatNetBytes(1024)), "1 KB");
  CHECK_STR(Narrow(FormatNetBytes(1024 * 1024 - 1)), "1023 KB");
  CHECK_STR(Narrow(FormatNetBytes(1024 * 1024)), "1.0 MB");
  CHECK_STR(Narrow(FormatNetBytes(1024 * 1024 * 3 / 2)), "1.5 MB");
  // 負數不會變成一個看不懂的大數字。
  CHECK_STR(Narrow(FormatNetBytes(-5)), "0 B");
}

// ── 紀錄的檢視 ──────────────────────────────────────────────────

TEST(net_ui_empty_log_still_says_something) {
  // ⚠ 空的時候**也要有一句話**。這一條就是「不要只是一片空白」。
  int checked = 0;
  for (UiLang l : kLangs) {
    const NetLogView v = BuildNetLogView({}, l, 0);
    CHECK(v.empty);
    CHECK_INT(v.count, 0);
    CHECK(v.rows.empty());
    CHECK(!v.summary.empty());
    // 而且那句話與「共 N 次」不是同一句 —— 不然「一次都沒有連過」
    // 會變成「共 0 次連線」,那是一句技術上正確、讀起來像壞掉的話。
    const NetLogView one = BuildNetLogView(
        {Entry(0, "a.example", NetPurpose::kStoreIndex, "", NetOutcome::kOk, 1,
               "")},
        l, 0);
    CHECK(!one.empty);
    CHECK(one.summary != v.summary);
    ++checked;
  }
  CHECK_INT(checked, kLangCount);
}

TEST(net_ui_log_row_has_all_four_columns) {
  const std::vector<NetLogEntry> entries = {
      Entry(1000000000LL * 1000, "one.example", NetPurpose::kStoreIndex, "",
            NetOutcome::kOk, 2048, ""),
      Entry(1000000060LL * 1000, "two.example", NetPurpose::kStorePackage,
            "bopomofo", NetOutcome::kFailed, 0, "HTTP 404"),
  };
  const NetLogView v = BuildNetLogView(entries, UiLang::kZhHant, 0);
  CHECK(!v.empty);
  CHECK_INT(v.count, 2);
  CHECK_INT(static_cast<int>(v.rows.size()), 2);

  // ⚠ 由**新到舊**:紀錄檔是追加寫的(舊在前),畫面上最有用的
  //   那一筆是最後一次連線。
  CHECK_STR(Narrow(v.rows[0].host), "two.example");
  CHECK_STR(Narrow(v.rows[1].host), "one.example");

  int checked = 0;
  for (const NetLogRow& r : v.rows) {
    // 四欄一個都不能空 —— 空欄位在畫面上看起來像被藏起來了。
    CHECK(!r.when.empty());
    CHECK(!r.host.empty());
    CHECK(!r.reason.empty());
    CHECK(!r.outcome.empty());
    // 而且四欄真的都出現在那一列上。⚠ 只驗 line 的話,「主機欄不見了」
    // 與「分隔符變了」在斷言上長得一樣。
    CHECK(Has(r.line, r.when));
    CHECK(Has(r.line, r.host));
    CHECK(Has(r.line, r.reason));
    CHECK(Has(r.line, r.outcome));
    ++checked;
  }
  CHECK_INT(checked, 2);

  // 成功那一筆帶位元組數;失敗那一筆帶原因,而且不帶位元組數。
  CHECK(Has(v.rows[1].outcome, FormatNetBytes(2048)));
  CHECK(Has(v.rows[0].outcome, std::wstring(L"HTTP 404")));
  CHECK(!Has(v.rows[0].outcome, FormatNetBytes(0)));
  // 套件名進了「原因」欄。
  CHECK(Has(v.rows[0].reason, std::wstring(L"bopomofo")));
  // 兩種用途說的不是同一句話。
  CHECK(v.rows[0].reason != v.rows[1].reason);
}

TEST(net_ui_log_rows_speak_every_language) {
  // 三種結果 × 三個語系:每一格都要有字,而且三種結果彼此不同。
  const NetOutcome outcomes[] = {NetOutcome::kOk, NetOutcome::kFailed,
                                 NetOutcome::kRedirected};
  int checked = 0;
  for (UiLang l : kLangs) {
    for (int i = 0; i < 3; ++i) {
      const std::wstring a = NetOutcomeUiText(outcomes[i], l);
      CHECK(!a.empty());
      for (int j = i + 1; j < 3; ++j)
        CHECK(a != std::wstring(NetOutcomeUiText(outcomes[j], l)));
      ++checked;
    }
    CHECK(std::wstring(NetPurposeUiText(NetPurpose::kStoreIndex, l)) !=
          std::wstring(NetPurposeUiText(NetPurpose::kStorePackage, l)));
  }
  CHECK_INT(checked, kLangCount * 3);
}

TEST(net_ui_log_row_cannot_forge_an_extra_line) {
  // ⚠ 這是**安全控制**,不是排版。host 與 detail 來自轉址目標與伺服器
  //   的回應,是對方控制得了的字串。一個換行就能在畫面上偽造出多一筆
  //   看起來無害的連線 —— 而這份紀錄的用途正是讓使用者相信它。
  const std::vector<NetLogEntry> entries = {
      Entry(0, "evil.example\nfake.example", NetPurpose::kStoreIndex,
            "a\tb\rc", NetOutcome::kFailed, 0, "x\ny"),
  };
  const NetLogView v = BuildNetLogView(entries, UiLang::kEnUs, 0);
  CHECK_INT(static_cast<int>(v.rows.size()), 1);
  const std::wstring& line = v.rows[0].line;
  CHECK(line.find(L'\n') == std::wstring::npos);
  CHECK(line.find(L'\r') == std::wstring::npos);
  CHECK(line.find(L'\t') == std::wstring::npos);
  // 內容還在(只是換行被換成空白),不是整段被丟掉。
  CHECK(Has(line, std::wstring(L"evil.example")));
  CHECK(Has(line, std::wstring(L"fake.example")));

  // 長度上限:對方給一個超長的主機名,不可以把整列撐爆。
  std::string huge(4000, 'z');
  const NetLogView big = BuildNetLogView(
      {Entry(0, huge.c_str(), NetPurpose::kStoreIndex, "", NetOutcome::kFailed,
             0, "")},
      UiLang::kEnUs, 0);
  CHECK(big.rows[0].host.size() <= kMaxLogHost);
}
