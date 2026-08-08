// windows/tests/test_net_policy.cc — 離線守門的判斷與連網紀錄
//
// 最重要的一條在最上面:**被開關擋下的嘗試不算一次連線,不進紀錄。**
// 使用者驗證我們的方式是「開關從沒開過 → 紀錄是空的」,
// 記進去就等於毀掉那句話。

#include <string>
#include <vector>

#include "../common/net_policy.h"
#include "check.h"

using namespace rimewin;

TEST(NetGate_switch_off_blocks_before_anything_else) {
  // 開關要在**任何**解析之前問。開關是關的時候,連網址長什麼樣都不必知道。
  CHECK(CheckHop(false, "https://example.com/index.json") ==
        HopVerdict::kBlockedBySwitch);
  CHECK(CheckHop(false, "這根本不是網址") == HopVerdict::kBlockedBySwitch);
  CHECK(CheckHop(false, "file:///C:/Windows/System32/config/SAM") ==
        HopVerdict::kBlockedBySwitch);
}

TEST(NetGate_only_proceed_may_be_logged) {
  // 判決與「要不要記錄」是一對一的:只有 kProceed 之後才會有連線發生。
  // 這一條寫成測試是為了讓「加一種新判決」的人立刻撞到這個約定。
  CHECK(CheckHop(true, "https://example.com/a") == HopVerdict::kProceed);
  CHECK(CheckHop(true, "http://example.com/a") == HopVerdict::kProceed);
  CHECK(CheckHop(true, "file:///etc/passwd") == HopVerdict::kBadScheme);
  CHECK(CheckHop(true, "ftp://example.com/a") == HopVerdict::kBadScheme);
  CHECK(CheckHop(true, "javascript:alert(1)") == HopVerdict::kBadUrl);
  CHECK(CheckHop(true, "") == HopVerdict::kBadUrl);
  CHECK(CheckHop(true, "://nohost/") == HopVerdict::kBadUrl);
  CHECK(CheckHop(true, "https://") == HopVerdict::kBadUrl);
}

TEST(NetGate_scheme_check_is_case_insensitive) {
  CHECK(CheckHop(true, "HTTPS://example.com/a") == HopVerdict::kProceed);
  CHECK(SchemeAllowed("HtTp://example.com/"));
  CHECK(!SchemeAllowed("FILE://x/"));
}

TEST(NetGate_no_host_allowlist) {
  // 刻意沒有白名單:索引來源是使用者可以改的,轉址可以落在任何主機。
  // 設計上讓目的地看得見,而不是限制它。這一條寫成測試,是因為
  // 「順手加一個白名單」看起來像在加強安全,實際上是換掉了設計。
  CHECK(CheckHop(true, "https://127.0.0.1:8099/index.json") == HopVerdict::kProceed);
  CHECK(CheckHop(true, "https://any-random-host.example/x") == HopVerdict::kProceed);
  CHECK(CheckHop(true, "http://[2001:db8::1]:8080/x") == HopVerdict::kProceed);
}

TEST(NetLog_host_only_never_path) {
  CHECK_STR(HostOf("https://cdn.example/rime/index.json?a=b"), "cdn.example");
  CHECK_STR(HostOf("http://127.0.0.1:8099/index.json"), "127.0.0.1");
  CHECK_STR(HostOf("https://user:pw@secret.example/x"), "secret.example");
  CHECK_STR(HostOf("http://[2001:db8::1]:8080/x"), "[2001:db8::1]");
  // **永不回傳空字串**:讀不懂的值比空欄位好,空欄位看起來像被藏起來了。
  CHECK_STR(HostOf("不是網址"), "不是網址");
  CHECK(!HostOf("https://").empty());
}

TEST(NetLog_remote_string_cannot_forge_a_line) {
  // host 來自轉址目標 = 對方控制得了。不清理的話,一個換行就能在紀錄裡
  // 偽造出一筆看起來無害的連線。
  NetLogEntry e;
  e.at_ms = 1;
  e.host = "evil\n1970-01-01\tgood.example";
  e.purpose = NetPurpose::kStoreIndex;
  e.outcome = NetOutcome::kFailed;
  const std::string line = EncodeLogLine(e);
  CHECK(line.find('\n') == std::string::npos);
  int tabs = 0;
  for (char c : line)
    if (c == '\t') ++tabs;
  CHECK_INT(tabs, 6);  // 七欄 = 六個 tab,一個都不能多
  NetLogEntry back;
  CHECK(DecodeLogLine(line, &back));
  CHECK(back.host.find('\n') == std::string::npos);
  CHECK(back.host.find('\t') == std::string::npos);
}

TEST(NetLog_fields_are_truncated) {
  NetLogEntry e;
  e.at_ms = 1;
  e.host = std::string(500, 'h');
  e.label = std::string(500, 'l');
  e.detail = std::string(500, 'd');
  NetLogEntry back;
  CHECK(DecodeLogLine(EncodeLogLine(e), &back));
  CHECK_INT(back.host.size(), kMaxLogHost);
  CHECK_INT(back.label.size(), kMaxLogLabel);
  CHECK_INT(back.detail.size(), kMaxLogDetail);
}

TEST(NetLog_decode_rejects_garbage_without_throwing) {
  NetLogEntry e;
  CHECK(!DecodeLogLine("", &e));
  CHECK(!DecodeLogLine("1\t2\t3", &e));
  CHECK(!DecodeLogLine("不是數字\th\tSTORE_INDEX\t\tOK\t0\t", &e));
  CHECK(!DecodeLogLine("1\th\tNO_SUCH_PURPOSE\t\tOK\t0\t", &e));
  CHECK(!DecodeLogLine("1\th\tSTORE_INDEX\t\tNO_SUCH\t0\t", &e));
  CHECK(!DecodeLogLine("1\th\tSTORE_INDEX\t\tOK\t不是數字\t", &e));
  // 欄位多一個也不行:多出來的那一欄代表雙方對格式的理解不同。
  CHECK(!DecodeLogLine("1\th\tSTORE_INDEX\t\tOK\t0\t\t", &e));
}

TEST(NetLog_one_bad_line_does_not_destroy_the_rest) {
  const std::string text =
      "1\ta.example\tSTORE_INDEX\t\tOK\t100\t\n"
      "這一行是壞的\n"
      "2\tb.example\tSTORE_PACKAGE\t萬象\tFAILED\t0\tHTTP 404\n";
  const std::vector<NetLogEntry> v = DecodeLog(text);
  CHECK_INT(v.size(), 2);
  CHECK_STR(v[0].host, "a.example");
  CHECK_STR(v[1].detail, "HTTP 404");
  CHECK(v[1].purpose == NetPurpose::kStorePackage);
}

TEST(NetLog_roundtrip_and_order_is_oldest_first) {
  std::vector<NetLogEntry> v;
  for (int i = 0; i < 3; ++i) {
    NetLogEntry e;
    e.at_ms = 100 + i;
    e.host = "h" + std::to_string(i);
    e.outcome = NetOutcome::kRedirected;
    v.push_back(e);
  }
  const std::vector<NetLogEntry> back = DecodeLog(EncodeLog(v));
  CHECK_INT(back.size(), 3);
  CHECK_STR(back[0].host, "h0");
  CHECK_STR(back[2].host, "h2");
  CHECK(back[1].outcome == NetOutcome::kRedirected);
}

TEST(NetLog_cap_drops_the_oldest) {
  std::vector<NetLogEntry> v;
  for (int i = 1; i <= 5; ++i) {
    NetLogEntry e;
    e.at_ms = i;
    e.host = "h" + std::to_string(i);
    v = AppendCapped(v, e, 3);
  }
  CHECK_INT(v.size(), 3);
  CHECK_STR(v[0].host, "h3");
  CHECK_STR(v[2].host, "h5");
}

TEST(NetLog_purpose_is_a_closed_set) {
  // 自由文字遲早會有人把 URL 或使用者輸入塞進去,而那是紀錄檔最不該有的。
  NetPurpose p;
  CHECK(NetPurposeFromToken("STORE_INDEX", &p));
  CHECK(p == NetPurpose::kStoreIndex);
  CHECK(NetPurposeFromToken("STORE_PACKAGE", &p));
  CHECK(!NetPurposeFromToken("ANYTHING_ELSE", &p));
  NetOutcome o;
  CHECK(NetOutcomeFromToken("REDIRECTED", &o));
  CHECK(o == NetOutcome::kRedirected);
  CHECK(!NetOutcomeFromToken("redirected", &o));  // 大小寫敏感,不做寬鬆比對
}

TEST(NetUrl_resolve_relative) {
  CHECK_STR(ResolveUrl("https://a.example/rime/index.json", "b.zip"),
            "https://a.example/rime/b.zip");
  CHECK_STR(ResolveUrl("https://a.example/rime/index.json", "/x/b.zip"),
            "https://a.example/x/b.zip");
  CHECK_STR(ResolveUrl("https://a.example/rime/index.json", "https://c/z"),
            "https://c/z");
  CHECK_STR(ResolveUrl("https://a.example/rime/index.json", "//c/z"),
            "https://c/z");
  CHECK_STR(ResolveUrl("https://a.example/index.json?v=2", "b.zip"),
            "https://a.example/b.zip");
}

TEST(NetUrl_package_url_three_step_fallback) {
  const std::string idx = "https://a.example/rime/index.json";
  // 1) file 本身是絕對網址
  CHECK_STR(ResolvePackageUrl(idx, "https://b/", "https://c/p.zip"),
            "https://c/p.zip");
  // 2) base_url 存在
  CHECK_STR(ResolvePackageUrl(idx, "https://cdn.example/pkgs", "p.zip"),
            "https://cdn.example/pkgs/p.zip");
  // 3) 沒有 base_url → 以索引網址為基底
  CHECK_STR(ResolvePackageUrl(idx, "", "p.zip"), "https://a.example/rime/p.zip");
  // base_url 是相對的話,先對索引網址解析。
  CHECK_STR(ResolvePackageUrl(idx, "pkgs/", "p.zip"),
            "https://a.example/rime/pkgs/p.zip");
}

TEST(NetGate_redirect_and_size_limits_are_documented_constants) {
  // 這些數字散落在實作裡的話會漂移。釘在這裡,改的人會看到測試紅。
  CHECK_INT(kMaxRedirects, 5);
  CHECK_INT(kMaxIndexBytes, 4 * 1024 * 1024);
  CHECK_INT(kMaxPackageBytes, 256 * 1024 * 1024);
  CHECK_INT(kDefaultMaxLogEntries, 500);
}
