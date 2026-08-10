// windows/tests/test_net_gate_core.cc — 連網出口的流程
//
// test_net_policy.cc 驗的是「這一跳准不准」的判斷。這一支驗的是**流程**:
// 問開關 → 送出請求 → 跟轉址 → 邊收邊數 → 記一筆,而且順序不能錯。
//
// ── 最重要的兩條 ────────────────────────────────────────────────
//
//   1. 開關關著時,傳輸層**一次都不會被呼叫**。
//   2. 只有真的送出去的請求才會產生紀錄。
//
// 兩條合起來就是使用者驗證我們的方式:「開關從沒開過 → 紀錄是空的」。
//
// 每一個情境都會斷言同一條等式:**記錄到的筆數 == report.hops**
// (hops = 真的送出去的請求數)。這條等式寫在 Harness::Run 裡,所以
// 底下每一個 TEST 都自動帶著它,不會有人新增情境時忘了驗。
//
// 檔尾有「怎麼確認這一支真的會紅」的兩個植入配方。

#include <string>
#include <vector>

#include "../common/net_gate_core.h"
#include "../common/net_policy.h"
#include "check.h"

using namespace rimewin;

namespace {

// 一跳的腳本。預設是「連上了、200、空的 body」。
struct Scripted {
  bool sent = true;          // 連線 API 有沒有真的被呼叫
  bool ok = true;            // 有沒有拿到回應標頭
  int status = 200;
  std::string location;
  int64_t declared = -1;
  std::string body;
  bool body_fails = false;   // 收到一半斷線
};

// 假的傳輸層。**一個 socket 都不開** —— 這個專案的規矩是連測試都不准
// 出現第二個連網出口(與 Android 的 NetworkRedirectTest 同一條規矩)。
class FakeTransport : public NetTransport {
 public:
  std::vector<Scripted> script;
  std::vector<std::string> urls;  // Begin 收到的每一個網址,依序
  int begins = 0;
  int ends = 0;
  size_t idx = 0;

  void Begin(const std::string& url, Head* out) override {
    ++begins;
    urls.push_back(url);
    if (idx >= script.size()) {
      out->sent = false;
      out->error = "腳本用完了";
      return;
    }
    const Scripted& s = script[idx];
    out->sent = s.sent;
    out->ok = s.ok;
    out->status = s.status;
    out->location = s.location;
    out->declared_bytes = s.declared;
    if (!s.sent) out->error = "handle 開不起來";
    else if (!s.ok) out->error = "沒有回應";
  }

  bool ReadBody(NetSink* sink, int64_t max_bytes, int64_t* read, bool* over,
                std::string* error) override {
    *read = 0;
    *over = false;
    if (idx >= script.size()) return false;
    const Scripted& s = script[idx];
    if (s.body_fails) {
      *error = "中斷";
      return false;
    }
    for (char c : s.body) {
      ++*read;
      if (*read > max_bytes) {
        *over = true;
        return false;
      }
      if (!sink->Write(&c, 1)) {
        *error = "寫不下去";
        return false;
      }
    }
    return true;
  }

  void End() override {
    ++ends;
    ++idx;
  }
};

class StringSink : public NetSink {
 public:
  std::string data;
  int discards = 0;
  bool Write(const char* p, size_t n) override {
    data.append(p, n);
    return true;
  }
  void Discard() override {
    ++discards;
    data.clear();
  }
};

struct Harness {
  FakeTransport tr;
  StringSink sink;
  std::vector<NetLogEntry> log;
  bool enabled = true;
  int asks = 0;        // 開關被問了幾次
  int off_after = -1;  // >=0:問到第 off_after 次之後改成「關」
  int64_t clock = 1000;

  bool Enabled() {
    ++asks;
    if (off_after >= 0) return asks <= off_after;
    return enabled;
  }

  NetReport Run(const std::string& url, int64_t max_bytes = kMaxIndexBytes) {
    NetRequest req;
    req.url = url;
    req.purpose = NetPurpose::kStorePackage;
    req.label = "萬象";
    req.max_bytes = max_bytes;
    const NetReport r = RunFetch(
        req, [this] { return Enabled(); }, &tr, &sink,
        [this](const NetLogEntry& e) { log.push_back(e); },
        [this] { return clock++; });
    // ⚠ 這一條在**每一個**情境都成立:紀錄筆數 == 真的送出去的請求數。
    //   「被擋下也記一筆」會讓左邊多一,「真的連線卻漏記」會讓右邊多一。
    CHECK_INT(log.size(), r.hops);
    return r;
  }
};

Scripted Redirect(int status, const std::string& to) {
  Scripted s;
  s.status = status;
  s.location = to;
  return s;
}

Scripted Ok200(const std::string& body) {
  Scripted s;
  s.body = body;
  return s;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════
//  1. 開關關著 = 什麼都沒發生
// ═══════════════════════════════════════════════════════════════

TEST(NetGateCore_switch_off_never_touches_the_wire) {
  Harness h;
  h.enabled = false;
  h.tr.script.push_back(Ok200("這一份永遠不該被取回來"));
  const NetReport r = h.Run("https://cdn.example/index.json");

  CHECK(r.result == NetResult::kBlocked);
  // 傳輸層**一次都沒被呼叫**。不是「呼叫了但失敗」。
  CHECK_INT(h.tr.begins, 0);
  CHECK_INT(h.tr.ends, 0);
  CHECK_INT(r.hops, 0);
  CHECK(h.sink.data.empty());
}

TEST(NetGateCore_switch_off_leaves_the_log_empty) {
  // 使用者驗證我們的方式:開關從沒開過 → 紀錄是空的。
  // 這一條若壞掉,那句話就變成謊話,而它是整個離線定位的兌現處。
  Harness h;
  h.enabled = false;
  h.tr.script.push_back(Ok200("x"));
  const NetReport r = h.Run("https://cdn.example/index.json");
  CHECK_INT(h.log.size(), 0);
  CHECK(r.result == NetResult::kBlocked);
}

TEST(NetGateCore_switch_off_is_asked_before_the_url_is_even_parsed) {
  // 開關要在**任何**解析之前問。網址是垃圾也一樣回 kBlocked ——
  // 開關關著的時候,連「這個網址長什麼樣」都不必知道。
  Harness h;
  h.enabled = false;
  const NetReport r = h.Run("這根本不是網址");
  CHECK(r.result == NetResult::kBlocked);
  CHECK_INT(h.log.size(), 0);
  CHECK_INT(h.tr.begins, 0);
}

TEST(NetGateCore_blocked_is_distinguishable_from_a_network_failure) {
  // 「連不上伺服器」要給重試,「開關是關的」要給一顆開啟開關的按鈕。
  // 兩者壓成同一句紅字正是 Windows 端犯過的錯。
  Harness off;
  off.enabled = false;
  CHECK(NetResultIsBlocked(off.Run("https://a.example/x").result));

  Harness fail;
  Scripted s;
  s.ok = false;
  fail.tr.script.push_back(s);
  const NetReport r = fail.Run("https://a.example/x");
  CHECK(!NetResultIsBlocked(r.result));
  CHECK(r.result == NetResult::kTransportError);
}

TEST(NetGateCore_switch_flipped_off_mid_redirect_stops_immediately) {
  // 使用者在下載途中把開關關掉。下一跳不可以再連,而且**不可以**
  // 因為「被擋下」多記一筆 —— 已經發生的那一跳照記,新的那一跳不記。
  Harness h;
  h.off_after = 1;  // 第一次問是開的,第二次之後是關的
  h.tr.script.push_back(Redirect(302, "https://b.example/y"));
  h.tr.script.push_back(Ok200("不該拿到"));
  const NetReport r = h.Run("https://a.example/x");

  CHECK(r.result == NetResult::kBlocked);
  CHECK_INT(h.tr.begins, 1);      // 只連了第一跳
  CHECK_INT(r.hops, 1);
  CHECK_INT(h.log.size(), 1);     // 而且只有那一跳被記下來
  CHECK(h.log[0].outcome == NetOutcome::kRedirected);
  CHECK(h.sink.discards > 0);     // 收到一半的東西被丟掉了
}

// ═══════════════════════════════════════════════════════════════
//  2. 還沒連線的失敗一律不記錄
// ═══════════════════════════════════════════════════════════════

TEST(NetGateCore_bad_url_and_bad_scheme_are_not_connections) {
  {
    Harness h;
    const NetReport r = h.Run("file:///C:/Windows/System32/config/SAM");
    CHECK(r.result == NetResult::kBadScheme);
    CHECK_INT(h.tr.begins, 0);
    CHECK_INT(h.log.size(), 0);
  }
  {
    Harness h;
    const NetReport r = h.Run("這不是網址");
    CHECK(r.result == NetResult::kBadUrl);
    CHECK_INT(h.tr.begins, 0);
    CHECK_INT(h.log.size(), 0);
  }
}

TEST(NetGateCore_a_transport_that_never_sent_is_not_a_connection) {
  // handle 都開不起來 = 一個封包都沒出去 = 不是一次連線。
  Harness h;
  Scripted s;
  s.sent = false;
  h.tr.script.push_back(s);
  const NetReport r = h.Run("https://a.example/x");
  CHECK(r.result == NetResult::kTransportError);
  CHECK_INT(h.tr.begins, 1);   // 試過了
  CHECK_INT(r.hops, 0);        // 但沒送出去
  CHECK_INT(h.log.size(), 0);  // 所以不記
}

TEST(NetGateCore_a_request_that_was_sent_is_recorded_even_when_it_fails) {
  // 反過來:請求送出去了(DNS 已經查了),就算連不上也要記 ——
  // 這一筆正是使用者想看到的「它連了哪裡」。
  Harness h;
  Scripted s;
  s.sent = true;
  s.ok = false;
  h.tr.script.push_back(s);
  const NetReport r = h.Run("https://a.example/x");
  CHECK_INT(r.hops, 1);
  CHECK_INT(h.log.size(), 1);
  CHECK(h.log[0].outcome == NetOutcome::kFailed);
  CHECK_STR(h.log[0].host, "a.example");
}

TEST(NetGateCore_redirect_to_a_bad_scheme_records_only_the_hop_that_happened) {
  // 302 → file://。那一跳(302)是真的連線,要記一筆;
  // file:// 那一跳一個封包都沒送,不記。
  Harness h;
  h.tr.script.push_back(Redirect(302, "file:///etc/passwd"));
  const NetReport r = h.Run("https://a.example/x");
  CHECK(r.result == NetResult::kBadScheme);
  CHECK_INT(h.tr.begins, 1);
  CHECK_INT(r.hops, 1);
  CHECK_INT(h.log.size(), 1);
  CHECK(h.log[0].outcome == NetOutcome::kFailed);
}

TEST(NetGateCore_a_non_http_location_cannot_sneak_past_as_a_relative_path) {
  // ⚠ 這一條是實測抓到的。net_policy.h 的 ResolveUrl 只認得 http(s)://
  //   開頭的絕對網址,其餘當成**相對路徑**:
  //       ResolveUrl("https://a.example/x", "file:///etc/passwd")
  //         → "https://a.example/file:///etc/passwd"
  //   那是一個合法的 https 網址,CheckHop 會說 kProceed ——
  //   也就是「每一跳的 scheme 都驗過」這句話會變成假的。
  //   先驗 Location 自己的 scheme 才擋得住。
  CHECK_STR(ResolveUrl("https://a.example/x", "file:///etc/passwd"),
            "https://a.example/file:///etc/passwd");
  CHECK(CheckHop(true, "https://a.example/file:///etc/passwd") ==
        HopVerdict::kProceed);
  CHECK(!LocationSchemeAllowed("file:///etc/passwd"));
  CHECK(!LocationSchemeAllowed("ftp://a.example/x"));
  CHECK(!LocationSchemeAllowed("javascript://x/"));
  // 相對網址沒有 scheme,一律放行(由 ResolveUrl 接上 base 的)。
  CHECK(LocationSchemeAllowed("/moved/here.json"));
  CHECK(LocationSchemeAllowed("here.json"));
  CHECK(LocationSchemeAllowed("//cdn.example/x"));
  CHECK(LocationSchemeAllowed(""));
  // 路徑裡剛好有 "://" 的相對網址不算絕對網址。
  CHECK(LocationSchemeAllowed("/redirect?to=https://x.example/"));
  CHECK(LocationSchemeAllowed("HTTPS://a.example/x"));
}

// ═══════════════════════════════════════════════════════════════
//  3. 轉址
// ═══════════════════════════════════════════════════════════════

TEST(NetGateCore_every_redirect_hop_gets_its_own_record) {
  // 「它到底連了哪些主機」不可以被一次轉址藏起來。
  Harness h;
  h.tr.script.push_back(Redirect(301, "https://b.example/y"));
  h.tr.script.push_back(Redirect(302, "https://c.example/z"));
  h.tr.script.push_back(Ok200("payload"));
  const NetReport r = h.Run("https://a.example/x");

  CHECK(r.result == NetResult::kOk);
  CHECK_INT(r.hops, 3);
  CHECK_INT(h.log.size(), 3);
  CHECK_STR(h.log[0].host, "a.example");
  CHECK(h.log[0].outcome == NetOutcome::kRedirected);
  CHECK_STR(h.log[1].host, "b.example");
  CHECK(h.log[1].outcome == NetOutcome::kRedirected);
  CHECK_STR(h.log[2].host, "c.example");
  CHECK(h.log[2].outcome == NetOutcome::kOk);
  CHECK_STR(h.sink.data, "payload");
}

TEST(NetGateCore_relative_location_is_resolved_against_the_current_hop) {
  Harness h;
  h.tr.script.push_back(Redirect(302, "/moved/here.json"));
  h.tr.script.push_back(Ok200("ok"));
  const NetReport r = h.Run("https://a.example/deep/path/index.json");
  CHECK(r.result == NetResult::kOk);
  CHECK_INT(h.tr.urls.size(), 2);
  CHECK_STR(h.tr.urls[1], "https://a.example/moved/here.json");
}

TEST(NetGateCore_redirect_may_leave_the_original_host) {
  // net_policy.h 明著說**不做主機白名單**:轉址可以落在任何主機,
  // 設計上讓目的地看得見(每一跳都記),而不是限制它。
  // ⚠ 這一條與 Android 的 NetworkGate.redirectAllowed **不一致** ——
  //   那邊要求轉址不得離開原主機。差異已在交付報告裡標出來,
  //   等規範所有權方裁決。在裁決之前照 net_policy.h 走。
  Harness h;
  h.tr.script.push_back(Redirect(302, "https://somewhere-else.example/z"));
  h.tr.script.push_back(Ok200("ok"));
  const NetReport r = h.Run("https://a.example/x");
  CHECK(r.result == NetResult::kOk);
  CHECK_STR(h.log[0].detail, "HTTP 302 → somewhere-else.example");
}

TEST(NetGateCore_https_must_not_be_downgraded_to_http) {
  // 沒有程式碼簽章的平台上,TLS 憑證是唯一的信任錨。一次降級轉址
  // 就把它拿掉了,而使用者在畫面上看不出差別。
  Harness h;
  h.tr.script.push_back(Redirect(302, "http://a.example/x"));
  h.tr.script.push_back(Ok200("不該拿到"));
  const NetReport r = h.Run("https://a.example/x");
  CHECK(r.result == NetResult::kDowngraded);
  CHECK_INT(h.tr.begins, 1);
  CHECK_INT(h.log.size(), 1);
  CHECK(h.log[0].outcome == NetOutcome::kFailed);
}

TEST(NetGateCore_downgrade_and_bad_scheme_are_two_different_answers) {
  // https → http 是**降級**(唯一的信任錨被拿掉)。
  // https → ftp 是 **scheme 不准**。兩者都走不通,但要說的話不一樣,
  // 而說錯話比不說更糟。
  CHECK(!NoDowngrade("https://a.example/x", "http://a.example/x"));
  CHECK(NoDowngrade("https://a.example/x", "https://b.example/x"));
  CHECK(NoDowngrade("http://a.example/x", "http://a.example/x"));
  // ftp 不是「降級」,是「scheme 不准」—— 由 LocationSchemeAllowed 回答。
  CHECK(NoDowngrade("https://a.example/x", "ftp://a.example/x"));
  CHECK(!LocationSchemeAllowed("ftp://a.example/x"));
  Harness h;
  h.tr.script.push_back(Redirect(302, "ftp://a.example/x"));
  const NetReport r = h.Run("https://a.example/x");
  CHECK(r.result == NetResult::kBadScheme);
  CHECK(h.log[0].detail.find("scheme") != std::string::npos);
}

TEST(NetGateCore_http_to_https_is_an_upgrade_and_is_allowed) {
  Harness h;
  h.tr.script.push_back(Redirect(302, "https://a.example/x"));
  h.tr.script.push_back(Ok200("ok"));
  CHECK(h.Run("http://a.example/x").result == NetResult::kOk);
}

TEST(NetGateCore_redirect_limit_follows_five_and_connects_six_times) {
  // net_policy.h:++redirects > kMaxRedirects,所以跟得動 5 次轉址、
  // 最多 6 次連線。
  CHECK_INT(kMaxRedirects, 5);
  Harness h;
  for (int i = 0; i < 8; ++i)
    h.tr.script.push_back(Redirect(302, "https://a.example/hop"));
  const NetReport r = h.Run("https://a.example/x");
  CHECK(r.result == NetResult::kTooManyRedirects);
  CHECK_INT(h.tr.begins, 6);
  CHECK_INT(h.log.size(), 6);
  CHECK_STR(h.log[5].detail, "轉址過多");
}

TEST(NetGateCore_redirect_without_location_is_a_failure_not_a_loop) {
  Harness h;
  h.tr.script.push_back(Redirect(302, ""));
  const NetReport r = h.Run("https://a.example/x");
  CHECK(r.result == NetResult::kHttpError);
  CHECK_INT(h.log.size(), 1);
  CHECK_STR(h.log[0].detail, "HTTP 302 無 Location");
}

// ═══════════════════════════════════════════════════════════════
//  4. 大小上限
// ═══════════════════════════════════════════════════════════════

TEST(NetGateCore_declared_size_over_limit_stops_before_reading_the_body) {
  Harness h;
  Scripted s = Ok200(std::string(100, 'x'));
  s.declared = 1000;
  h.tr.script.push_back(s);
  const NetReport r = h.Run("https://a.example/x", 10);
  CHECK(r.result == NetResult::kTooLarge);
  CHECK_INT(h.log.size(), 1);
  CHECK_STR(h.log[0].detail, "宣告大小超過上限");
  CHECK(h.sink.data.empty());
}

TEST(NetGateCore_streaming_is_the_real_wall_when_the_declaration_lies) {
  // 宣告值是對方給的。它說「很小」而實際上送無窮長的東西時,
  // 擋得住的只有邊收邊數。
  Harness h;
  Scripted s = Ok200(std::string(5000, 'x'));
  s.declared = 4;  // 撒謊
  h.tr.script.push_back(s);
  const NetReport r = h.Run("https://a.example/x", 64);
  CHECK(r.result == NetResult::kTooLarge);
  CHECK_INT(h.log.size(), 1);
  CHECK_STR(h.log[0].detail, "超過大小上限,已中止");
  CHECK(h.sink.discards > 0);  // 收到一半的東西不留在硬碟上
  CHECK(h.sink.data.empty());
}

TEST(NetGateCore_missing_content_length_is_fine) {
  // 沒宣告(-1)不代表超過上限 —— 這是很常見的分支寫反。
  Harness h;
  Scripted s = Ok200("hello");
  s.declared = -1;
  h.tr.script.push_back(s);
  const NetReport r = h.Run("https://a.example/x", 64);
  CHECK(r.result == NetResult::kOk);
  CHECK_INT(r.bytes, 5);
  CHECK_STR(h.sink.data, "hello");
}

// ═══════════════════════════════════════════════════════════════
//  5. 紀錄的內容
// ═══════════════════════════════════════════════════════════════

TEST(NetGateCore_log_carries_host_only_never_the_path) {
  // 一個以隱私為賣點的程式把使用者的行為細節寫進本機檔案「以求透明」,
  // 是自相矛盾的。紀錄裡只有主機名。
  Harness h;
  h.tr.script.push_back(Ok200("x"));
  h.Run("https://cdn.example/rime/packages/secret-name.zip?token=abc");
  CHECK_INT(h.log.size(), 1);
  CHECK_STR(h.log[0].host, "cdn.example");
  CHECK(h.log[0].host.find('/') == std::string::npos);
  CHECK(h.log[0].detail.find("token") == std::string::npos);
  CHECK(h.log[0].detail.find("secret-name") == std::string::npos);
}

TEST(NetGateCore_log_carries_purpose_label_bytes_and_time) {
  Harness h;
  h.clock = 4242;
  h.tr.script.push_back(Ok200("12345"));
  h.Run("https://a.example/x");
  CHECK_INT(h.log.size(), 1);
  CHECK(h.log[0].purpose == NetPurpose::kStorePackage);
  CHECK_STR(h.log[0].label, "萬象");
  CHECK(h.log[0].outcome == NetOutcome::kOk);
  CHECK_INT(h.log[0].bytes, 5);
  CHECK_INT(h.log[0].at_ms, 4242);
}

TEST(NetGateCore_http_error_is_recorded_with_the_status) {
  Harness h;
  Scripted s;
  s.status = 404;
  h.tr.script.push_back(s);
  const NetReport r = h.Run("https://a.example/x");
  CHECK(r.result == NetResult::kHttpError);
  CHECK_INT(h.log.size(), 1);
  CHECK_STR(h.log[0].detail, "HTTP 404");
  CHECK(h.log[0].outcome == NetOutcome::kFailed);
}

TEST(NetGateCore_broken_body_is_recorded_and_discarded) {
  Harness h;
  Scripted s;
  s.body_fails = true;
  h.tr.script.push_back(s);
  const NetReport r = h.Run("https://a.example/x");
  CHECK(r.result == NetResult::kTransportError);
  CHECK_INT(h.log.size(), 1);
  CHECK(h.log[0].outcome == NetOutcome::kFailed);
  CHECK(h.sink.discards > 0);
}

TEST(NetGateCore_every_begin_is_matched_by_an_end) {
  // handle 漏掉的症狀是「用久了就連不上」,而那要幾小時才看得到。
  Harness h;
  h.tr.script.push_back(Redirect(302, "https://b.example/y"));
  h.tr.script.push_back(Ok200("ok"));
  h.Run("https://a.example/x");
  CHECK_INT(h.tr.begins, h.tr.ends);
}

// ═══════════════════════════════════════════════════════════════
//  ⚠ 怎麼確認這一支真的會紅(改動這一區的程式碼之前請先跑一次)
// ═══════════════════════════════════════════════════════════════
//
// 這個專案有過「測試是綠的,因為它沒在測」。所以下面兩個植入配方是
// 交付的一部分,不是建議:
//
//   植入 A —— 「開關關著也放行」
//     windows/common/net_gate_core.cc,RunFetch 裡:
//         const bool enabled = is_enabled ? is_enabled() : false;
//     改成
//         const bool enabled = true;
//     預期變紅:switch_off_never_touches_the_wire、
//               switch_off_leaves_the_log_empty、
//               switch_off_is_asked_before_the_url_is_even_parsed、
//               blocked_is_distinguishable_from_a_network_failure、
//               switch_flipped_off_mid_redirect_stops_immediately
//
//   植入 B —— 「被擋下也記一筆」
//     同一個檔案,`if (v != HopVerdict::kProceed)` 那一段裡,
//     sink->Discard() 之前補一行
//         record(HostOf(url), NetOutcome::kFailed, 0, "被開關擋下");
//     預期變紅:Harness::Run 裡那條等式(log.size() == hops)
//               在所有被擋下的情境都會失敗。
//
// 兩個都驗過了,結果寫在交付報告裡。
