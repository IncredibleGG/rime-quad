// windows/tests/test_sha256.cc — SHA-256 的公開向量
//
// ⚠ 這一支存在的理由:更新那條線上,「下載回來的是不是我們發出去的那一份」
//   最後只剩這一個函式在回答。它算錯的話,兩種結局都很糟:算出來永遠不符
//   (誰都更新不了),或永遠相符(等於沒有比對)。向量是外部給的,
//   所以它抓得到「我自己寫的實作與我自己寫的期望值一起錯」。

#include "../common/sha256.h"

#include <string>

#include "check.h"

using namespace rimewin;

TEST(sha256_nist_vectors) {
  // FIPS 180-2 附錄 B 的兩個,加上空字串(RFC 6234 的那一個)。
  CHECK_STR(Sha256::HexOf(""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK_STR(Sha256::HexOf("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK_STR(
      Sha256::HexOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(sha256_one_million_a) {
  // 一百萬個 'a'。跨了很多個區塊,而且長度欄位會用到高位 ——
  // 「只用 32 位元存長度」那種錯誤只有在這一條會現形。
  Sha256 s;
  const std::string chunk(1000, 'a');
  for (int i = 0; i < 1000; ++i) s.Update(chunk.data(), chunk.size());
  CHECK_STR(s.HexDigest(),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(sha256_streaming_matches_one_shot) {
  // 邊收邊算(下載的形狀)與一次算完必須完全一樣,而且**切在任何位置**
  // 都一樣 —— 緩衝區邊界(64 位元組)是最容易寫錯的地方。
  std::string data;
  for (int i = 0; i < 500; ++i) data.push_back(static_cast<char>(i * 7 + 3));
  const std::string want = Sha256::HexOf(data);
  for (size_t cut = 1; cut < data.size(); cut += 7) {
    Sha256 s;
    s.Update(data.data(), cut);
    s.Update(data.data() + cut, data.size() - cut);
    CHECK_STR(s.HexDigest(), want);
  }
}

TEST(sha256_digest_is_stable_and_update_after_final_is_ignored) {
  Sha256 s;
  s.Update("abc", 3);
  const std::string a = s.HexDigest();
  s.Update("more", 4);  // 收工之後再餵 —— 不可以安靜地算出另一個值
  CHECK_STR(s.HexDigest(), a);
  CHECK_STR(a,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(sha256_hex_shape) {
  CHECK(LooksLikeSha256Hex(std::string(64, 'a')));
  CHECK(LooksLikeSha256Hex(std::string(64, 'F')));
  CHECK(!LooksLikeSha256Hex(std::string(63, 'a')));
  CHECK(!LooksLikeSha256Hex(std::string(65, 'a')));
  CHECK(!LooksLikeSha256Hex(std::string(64, 'g')));
  CHECK(!LooksLikeSha256Hex(""));
}

TEST(sha256_compare_ignores_case_and_padding_but_not_content) {
  const std::string a =
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  std::string upper;
  for (char c : a)
    upper.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c);
  CHECK(Sha256HexEqual(a, upper));
  CHECK(Sha256HexEqual("  " + a + "\n", a));
  // 只差一個字元就是不同的檔案。
  std::string off = a;
  off[10] = off[10] == 'a' ? 'b' : 'a';
  CHECK(!Sha256HexEqual(a, off));
  // ⚠ 兩邊都沒有摘要**不算相符**。空字串當成相符的話,一份沒有 sha256
  //   的清單會讓「驗過了」變成永遠成立。
  CHECK(!Sha256HexEqual("", ""));
  CHECK(!Sha256HexEqual(a, ""));
}
