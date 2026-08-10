// windows/tests/test_store_basics.cc — sha256 與 JSON 讀取器
//
// 這兩支都是「別人的規格,我們照做」的東西,所以測試用的一律是**外部
// 來源的向量**:FIPS 180-4 的例子、NIST 的一百萬個 'a'。自己算出來的
// 期望值只證明程式跟自己一致。

#include <string>

#include "../common/mini_json.h"
#include "../common/sha256.h"
#include "check.h"

using namespace rimewin;

/* ═══════════════════════════ sha256 ═════════════════════════════ */
//
// ⚠ sha256 的主要測試在 tests/test_sha256.cc(FIPS 向量、一百萬個 a、
//   串流與一次算完一致、hex 形狀、大小寫比對)。那一份與 common/sha256.cc
//   都是**線上更新**那一條線先落地的,市集這一條直接用它,不另外寫一份 ——
//   同一個雜湊有兩份實作是最糟的安排。
//
//   下面只補那一份沒有的一格:**補位的邊界**。訊息長度 55/56/63/64/65
//   跨過「長度欄位還塞得下」與「得再多推一個區塊」的分界線,
//   而那一格是這個演算法最容易寫錯的地方。期望值來自 python 的 hashlib。

TEST(sha256_padding_boundaries_55_to_65) {
  CHECK_STR(Sha256::HexOf(std::string(55, 'x')),
            std::string("d5e285683cd4efc02d021a5c62014694"
                        "958901005d6f71e89e0989fac77e4072"));
  CHECK_STR(Sha256::HexOf(std::string(56, 'x')),
            std::string("04c26261370ee7541549d16dee320c72"
                        "3e3fd14671e66a099afe0a377c16888e"));
  CHECK_STR(Sha256::HexOf(std::string(63, 'x')),
            std::string("75220b47218278e656f2013bb8f0c455"
                        "a25eaf01e86c64924e9d48d89776d6f2"));
  CHECK_STR(Sha256::HexOf(std::string(64, 'x')),
            std::string("7ce100971f64e7001e8fe5a51973ecdf"
                        "e1ced42befe7ee8d5fd6219506b5393c"));
  CHECK_STR(Sha256::HexOf(std::string(65, 'x')),
            std::string("9537c5fdf120482f7d58d25e9ed583f5"
                        "2c02b4e304ea814db1633ad565aed7e9"));
}

/* ═══════════════════════════ JSON ═══════════════════════════════ */

TEST(Json_parses_the_shapes_the_index_uses) {
  Json j;
  std::string err;
  const std::string text =
      "{\"format_version\":1,\"base_url\":\"https://x/\",\"packages\":["
      "{\"id\":\"a\",\"size\":1713490,\"recommended\":true,"
      "\"schemas\":[{\"id\":\"s1\"},{\"id\":\"s2\"}],"
      "\"requires\":[\"b\",\"c\"],\"verified\":{\"deployed\":true}}]}";
  CHECK(ParseJson(text, &j, &err));
  CHECK_INT(j.Int("format_version", -1), 1);
  CHECK_STR(j.Str("base_url"), std::string("https://x/"));
  CHECK_INT(static_cast<int>(j.Array("packages").size()), 1);
  const Json& p = j.Array("packages")[0];
  CHECK_STR(p.Str("id"), std::string("a"));
  CHECK_INT(p.Int("size", 0), 1713490);
  CHECK(p.Bool("recommended", false));
  CHECK_INT(static_cast<int>(p.Strings("requires").size()), 2);
  CHECK_STR(p.Strings("requires")[1], std::string("c"));
  const Json* v = p.Find("verified");
  CHECK(v != nullptr);
  CHECK(v->Bool("deployed", false));
}

TEST(Json_rejects_the_things_it_must_reject) {
  Json j;
  std::string err;
  CHECK(!ParseJson("", &j, &err));
  CHECK(!ParseJson("{", &j, &err));
  CHECK(!ParseJson("{\"a\":1,}", &j, &err));      // 尾隨逗號
  CHECK(!ParseJson("[1,2] junk", &j, &err));      // 頂層之後還有東西
  CHECK(!ParseJson("{'a':1}", &j, &err));         // 單引號不是 JSON
  CHECK(!ParseJson("{\"a\":01}", &j, &err));      // 前導零
  CHECK(!ParseJson("nul", &j, &err));
  CHECK(!ParseJson("\"unterminated", &j, &err));
}

TEST(Json_depth_limit_stops_a_stack_overflow) {
  // 巢狀深度是安全控制:一個從網路來的位元組串就能把堆疊耗光。
  std::string deep(200, '[');
  Json j;
  std::string err;
  CHECK(!ParseJson(deep, &j, &err));
  CHECK(err.find("deeper") != std::string::npos);
}

TEST(Json_escapes_and_utf8) {
  Json j;
  std::string err;
  CHECK(ParseJson("{\"a\":\"\\u4e2d\\u6587\",\"b\":\"x\\ty\"}", &j, &err));
  CHECK_STR(j.Str("a"), std::string("\xe4\xb8\xad\xe6\x96\x87"));
  CHECK_STR(j.Str("b"), std::string("x\ty"));
  // 代理對。
  CHECK(ParseJson("{\"a\":\"\\ud83d\\ude00\"}", &j, &err));
  CHECK_INT(static_cast<int>(j.Str("a").size()), 4);
  // 落單的代理不該讓整份索引出局。
  CHECK(ParseJson("{\"a\":\"\\ud83d\"}", &j, &err));
}

TEST(Json_duplicate_key_takes_the_first) {
  // 取最後一個的話,一份索引裡塞兩個 sha256 就能讓「顯示的」與
  // 「驗證用的」不是同一個值。
  Json j;
  std::string err;
  CHECK(ParseJson("{\"sha256\":\"aa\",\"sha256\":\"bb\"}", &j, &err));
  CHECK_STR(j.Str("sha256"), std::string("aa"));
}

TEST(Json_type_mismatch_falls_back_instead_of_throwing) {
  Json j;
  std::string err;
  CHECK(ParseJson("{\"size\":\"not a number\",\"name\":42}", &j, &err));
  CHECK_INT(j.Int("size", -1), -1);
  CHECK_STR(j.Str("name", "fallback"), std::string("fallback"));
  CHECK_INT(static_cast<int>(j.Array("nothing").size()), 0);
}
