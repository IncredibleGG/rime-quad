// windows/tests/test_zip_reader.cc — DEFLATE 解壓器與 zip 中央目錄
//
// ⚠ 這是這條線上唯一直接吃「網路來的位元組」的解析器,所以測試的重點
//   不是「解得對」,是**解不對的時候不會爆掉**:截斷、位元翻轉、亂數、
//   宣告的大小說謊、兩份中繼資料不一致。
//   run_logic_tests.sh --asan 會把同一批案例再跑一次。

#include <cstdint>
#include <string>
#include <vector>

#include "../common/zip_reader.h"
#include "check.h"
#include "deflate_fixtures.h"
#include "zip_build.h"

using namespace rimewin;
using rimewin_test::BuildEntry;
using rimewin_test::BuildZip;
using rimewin_test::StoredEntry;

namespace {

std::string FixtureA() { return std::string(kDeflateA(), kDeflateALen); }
std::string FixtureB() { return std::string(kDeflateB(), kDeflateBLen); }
std::string FixtureC() { return std::string(kDeflateC(), kDeflateCLen); }

std::string PlainA() {
  std::string s;
  for (int i = 0; i < 3; ++i) s += "The quick brown fox jumps over the lazy dog. ";
  return s;
}
std::string PlainB() {
  std::string s;
  for (int i = 0; i < 400; ++i) s += "schema_id: luna_pinyin\n";
  return s;
}

InflateResult Run(const std::string& src, int64_t cap, std::string* out) {
  return Inflate(reinterpret_cast<const uint8_t*>(src.data()), src.size(), cap, out);
}

}  // namespace

/* ═══════════════════════════ CRC32 ══════════════════════════════ */

TEST(Zip_crc32_known_vectors) {
  // 標準向量。自己算的 CRC 只跟自己對得上,所以這兩個值是外部來源。
  CHECK_INT(static_cast<int64_t>(Crc32("")), 0);
  CHECK_INT(static_cast<int64_t>(Crc32("123456789")), 0xCBF43926LL);
  // 三段 fixture 的 CRC 由 python 的 zlib 算出來。
  CHECK_INT(static_cast<int64_t>(Crc32(PlainA())), static_cast<int64_t>(kCrcA));
  CHECK_INT(static_cast<int64_t>(Crc32(PlainB())), static_cast<int64_t>(kCrcB));
}

/* ═══════════════════════════ DEFLATE ════════════════════════════ */

TEST(Inflate_fixed_huffman) {
  std::string out;
  const InflateResult r = Run(FixtureA(), 1 << 20, &out);
  CHECK(r.ok);
  CHECK_STR(out, PlainA());
  CHECK_INT(static_cast<int64_t>(out.size()), static_cast<int64_t>(kPlainALen));
}

TEST(Inflate_dynamic_huffman_with_backrefs) {
  std::string out;
  const InflateResult r = Run(FixtureB(), 1 << 20, &out);
  CHECK(r.ok);
  CHECK_STR(out, PlainB());
}

TEST(Inflate_stored_block) {
  std::string out;
  const InflateResult r = Run(FixtureC(), 1 << 20, &out);
  CHECK(r.ok);
  CHECK_INT(static_cast<int64_t>(out.size()), static_cast<int64_t>(kPlainCLen));
  // 亂數的原文不值得塞進原始碼:用 CRC 釘住它就夠了。
  CHECK_INT(static_cast<int64_t>(Crc32(out)), static_cast<int64_t>(kCrcC));
}

TEST(Inflate_output_cap_is_a_hard_wall) {
  // 宣告的大小是對方給的,所以真正的牆是邊解邊數。
  std::string out;
  const InflateResult r = Run(FixtureB(), 100, &out);
  CHECK(!r.ok);
  CHECK(r.over_limit);
  CHECK(out.empty());   // 失敗時不留半份輸出

  // 剛好等於原文長度要成功;少一個位元組就要失敗。
  std::string ok_out;
  CHECK(Run(FixtureB(), static_cast<int64_t>(kPlainBLen), &ok_out).ok);
  std::string short_out;
  const InflateResult tight = Run(FixtureB(), static_cast<int64_t>(kPlainBLen) - 1,
                                  &short_out);
  CHECK(!tight.ok);
  CHECK(tight.over_limit);

  // 儲存區塊那一條路徑也要有同一道牆(它是另一段程式碼)。
  std::string c_out;
  const InflateResult rc = Run(FixtureC(), 100, &c_out);
  CHECK(!rc.ok);
  CHECK(rc.over_limit);
}

TEST(Inflate_truncated_input_never_succeeds_and_never_crashes) {
  const std::string full = FixtureB();
  int failures = 0;
  for (size_t n = 0; n < full.size(); ++n) {
    std::string out;
    const InflateResult r =
        Inflate(reinterpret_cast<const uint8_t*>(full.data()), n, 1 << 20, &out);
    if (!r.ok) ++failures;
  }
  // 每一個真前綴都必須失敗。有任何一個「成功」就代表我們讀到了
  // 輸入以外的東西,或是把不完整的資料當成完整的。
  CHECK_INT(failures, static_cast<int>(full.size()));
}

TEST(Inflate_bit_flips_never_crash) {
  const std::string full = FixtureB();
  int ok_count = 0;
  for (size_t i = 0; i < full.size(); ++i) {
    for (int bit = 0; bit < 8; bit += 3) {
      std::string mutated = full;
      mutated[i] = static_cast<char>(mutated[i] ^ (1 << bit));
      std::string out;
      const InflateResult r = Run(mutated, 4 << 20, &out);
      if (r.ok) ++ok_count;
    }
  }
  // 有些位元翻轉會產生另一個合法的串流,那是正常的 —— 這裡要的是
  // 「跑完了、沒有崩、沒有無限迴圈」。--asan 那一輪會抓越界。
  CHECK(ok_count >= 0);
}

TEST(Inflate_garbage_input_never_crashes) {
  uint32_t seed = 20260810u;
  for (int iter = 0; iter < 400; ++iter) {
    std::string junk;
    const int n = 1 + (iter % 97);
    for (int k = 0; k < n; ++k) {
      seed = seed * 1103515245u + 12345u;
      junk.push_back(static_cast<char>((seed >> 16) & 0xFF));
    }
    std::string out;
    Run(junk, 1 << 16, &out);
  }
  CHECK(true);  // 跑完就是通過
}

TEST(Inflate_invalid_block_type_is_rejected) {
  // BFINAL=1, BTYPE=11 → 0b111 = 0x07
  std::string bad("\x07", 1);
  std::string out;
  const InflateResult r = Run(bad, 1024, &out);
  CHECK(!r.ok);
  CHECK(r.error.find("block type 3") != std::string::npos);
}

TEST(Inflate_back_reference_before_the_start_is_rejected) {
  // ⚠ 這一條是反向驗證(windows/verify_store_mutations.sh)逼出來的:
  //   把「回溯距離不得超過已輸出長度」那道檢查拿掉之後,原本的
  //   截斷/位元翻轉/亂數三組測試**全部還是綠的** —— 因為隨機壞掉的
  //   串流很少剛好走到那一格,而越界讀在沒有 ASan 的建置下不會叫。
  //
  //   所以這裡直接手刻一段「一開頭就回頭參照」的固定 Huffman 串流:
  //     BFINAL=1, BTYPE=01(固定),長度符號 257(len=3),距離符號 0(dist=1)
  //   輸出還是空的就要往回讀 1 個位元組。python 的 zlib 對同一段位元組
  //   說的是 "invalid distance too far back"。
  const std::string crafted("\x03\x02", 2);
  std::string out;
  const InflateResult r = Run(crafted, 1024, &out);
  CHECK(!r.ok);
  CHECK(r.error.find("back-reference") != std::string::npos);
  CHECK(out.empty());
}

TEST(Inflate_stored_block_with_bad_complement_is_rejected) {
  // BFINAL=1 BTYPE=00,然後 LEN=1 NLEN=1(應該是 0xFFFE)
  std::string bad;
  bad.push_back('\x01');
  bad.push_back('\x01'); bad.push_back('\x00');
  bad.push_back('\x01'); bad.push_back('\x00');
  bad.push_back('Z');
  std::string out;
  const InflateResult r = Run(bad, 1024, &out);
  CHECK(!r.ok);
}

/* ═══════════════════════════ zip ════════════════════════════════ */

TEST(Zip_reads_stored_and_deflated_entries) {
  BuildEntry deflated;
  deflated.name = "luna_pinyin.schema.yaml";
  deflated.data = FixtureA();
  deflated.method = 8;
  deflated.crc = kCrcA;
  deflated.uncompressed = static_cast<uint32_t>(kPlainALen);

  const std::string zip = BuildZip({
      StoredEntry("README.md", "hello"),
      deflated,
  });

  std::vector<ZipEntry> entries;
  std::string err;
  CHECK(ReadZipCentralDirectory(zip, &entries, &err));
  CHECK_INT(static_cast<int>(entries.size()), 2);
  CHECK_STR(entries[0].name, std::string("README.md"));
  CHECK_STR(entries[1].name, std::string("luna_pinyin.schema.yaml"));

  std::string body;
  CHECK(ExtractZipEntry(zip, entries[0], 1 << 20, &body, &err));
  CHECK_STR(body, std::string("hello"));
  CHECK(ExtractZipEntry(zip, entries[1], 1 << 20, &body, &err));
  CHECK_STR(body, PlainA());
}

TEST(Zip_crc_mismatch_is_caught) {
  BuildEntry e = StoredEntry("a.yaml", "content");
  e.crc ^= 1u;
  const std::string zip = BuildZip({e});
  std::vector<ZipEntry> entries;
  std::string err;
  CHECK(ReadZipCentralDirectory(zip, &entries, &err));
  std::string body;
  CHECK(!ExtractZipEntry(zip, entries[0], 1 << 20, &body, &err));
  CHECK(err.find("CRC32") != std::string::npos);
  CHECK(body.empty());
}

TEST(Zip_size_mismatch_is_caught) {
  BuildEntry e = StoredEntry("a.yaml", "content");
  e.uncompressed = 999;  // 中央目錄說謊
  const std::string zip = BuildZip({e});
  std::vector<ZipEntry> entries;
  std::string err;
  CHECK(ReadZipCentralDirectory(zip, &entries, &err));
  std::string body;
  CHECK(!ExtractZipEntry(zip, entries[0], 1 << 20, &body, &err));
}

TEST(Zip_local_header_naming_a_different_entry_is_rejected) {
  // ⚠ 這是整份檔案存在的理由之一:檢查看中央目錄、解壓看 local header
  //   時,攻擊者只要讓兩份不一致就能繞過檢查。這裡兩份必須一致。
  BuildEntry e = StoredEntry("safe.yaml", "content");
  e.local_name_override = "../../evil.yaml";
  const std::string zip = BuildZip({e});
  std::vector<ZipEntry> entries;
  std::string err;
  CHECK(ReadZipCentralDirectory(zip, &entries, &err));
  CHECK_STR(entries[0].name, std::string("safe.yaml"));  // 中央目錄看到的是無害的名字
  std::string body;
  CHECK(!ExtractZipEntry(zip, entries[0], 1 << 20, &body, &err));
  CHECK(err.find("local header") != std::string::npos);
}

TEST(Zip_entry_count_mismatch_is_rejected) {
  const std::string zip = BuildZip({StoredEntry("a.yaml", "x")});
  std::string broken = zip;
  // EOCD 的 total entries 欄位(倒數第 12、11 個位元組)改成 5。
  broken[broken.size() - 12] = 5;
  std::vector<ZipEntry> entries;
  std::string err;
  CHECK(!ReadZipCentralDirectory(broken, &entries, &err));
}

TEST(Zip_not_a_zip_is_rejected) {
  std::vector<ZipEntry> entries;
  std::string err;
  CHECK(!ReadZipCentralDirectory("", &entries, &err));
  CHECK(!ReadZipCentralDirectory(std::string(4096, 'x'), &entries, &err));
  CHECK(!ReadZipCentralDirectory("PK\x05\x06", &entries, &err));
}

TEST(Zip_truncation_never_crashes) {
  BuildEntry deflated;
  deflated.name = "d.yaml";
  deflated.data = FixtureA();
  deflated.method = 8;
  deflated.crc = kCrcA;
  deflated.uncompressed = static_cast<uint32_t>(kPlainALen);
  const std::string zip = BuildZip({StoredEntry("a.yaml", "x"), deflated});
  for (size_t n = 0; n <= zip.size(); ++n) {
    std::vector<ZipEntry> entries;
    std::string err;
    if (ReadZipCentralDirectory(zip.substr(0, n), &entries, &err)) {
      for (const auto& e : entries) {
        std::string body;
        ExtractZipEntry(zip.substr(0, n), e, 1 << 20, &body, &err);
      }
    }
  }
  CHECK(true);
}

TEST(Zip_encrypted_entry_is_refused) {
  // 加密旗標在 local header 的 flags 位元 0。BuildZip 一律寫 0,
  // 所以這裡直接改那個位元組。
  const std::string zip = BuildZip({StoredEntry("a.yaml", "content")});
  std::string tampered = zip;
  tampered[6] = 1;  // local header + 6 = flags
  std::vector<ZipEntry> entries;
  std::string err;
  CHECK(ReadZipCentralDirectory(tampered, &entries, &err));
  std::string body;
  CHECK(!ExtractZipEntry(tampered, entries[0], 1 << 20, &body, &err));
  CHECK(err.find("encrypted") != std::string::npos);
}
