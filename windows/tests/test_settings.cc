// windows/tests/test_settings.cc — 設定的讀寫
//
// 這一組守的是三件「壞了以後畫面完全正常」的事:
//   1. 「沒設過」與「設成預設值」必須分得出來(否則預設值再也不能動)。
//   2. 未知的鍵不可以被吃掉(否則兩個版本交替執行會互相清設定)。
//   3. 下拉選單的索引↔數值換算(錯了以後使用者選「中」拿到「小」)。

#include <string>

#include "../common/settings.h"
#include "check.h"

using namespace rimewin;

TEST(Settings_absent_key_means_unset_not_default) {
  Settings s;
  CHECK(!s.Has(keys::kTextVariant));
  CHECK(s.SchemaPref().forced_variant == Variant::kFollow);
  // 設成「跟隨」必須是**刪掉那個鍵**,不是寫一個哨兵值。
  s.SetForcedVariant(Variant::kHans);
  CHECK(s.Has(keys::kTextVariant));
  s.SetForcedVariant(Variant::kFollow);
  CHECK(!s.Has(keys::kTextVariant));
  // 方案同理。
  s.SetForcedSchema("luna_pinyin");
  CHECK(s.Has(keys::kSchemaForced));
  s.SetForcedSchema("");
  CHECK(!s.Has(keys::kSchemaForced));
}

TEST(Settings_enum_first_value_is_the_follow_slot_and_is_never_written) {
  Settings s;
  s.SetEnumInt(keys::kCandCount, 7, kCandCountValues, kCandCountCount);
  CHECK_STR(s.Raw(keys::kCandCount), "7");
  // kCandCountValues[0] == 0 是「跟隨主題」。寫它 = 把「沒設過」變成「設過」。
  s.SetEnumInt(keys::kCandCount, 0, kCandCountValues, kCandCountCount);
  CHECK(!s.Has(keys::kCandCount));
  // 不在允許清單裡的值不可以偷偷存起來。
  s.SetEnumInt(keys::kCandCount, 6, kCandCountValues, kCandCountCount);
  CHECK(!s.Has(keys::kCandCount));
}

TEST(Settings_corrupt_value_reads_as_unset_not_as_garbage) {
  Settings s = Settings::Parse("cand.count = 這不是數字\ntext.variant = 亂寫\n");
  CHECK_INT(s.GetEnumInt(keys::kCandCount, kCandCountValues, kCandCountCount), 0);
  CHECK(s.SchemaPref().forced_variant == Variant::kFollow);
  // 越界的數字同理。
  Settings t = Settings::Parse("cand.count = 999\n");
  CHECK_INT(t.GetEnumInt(keys::kCandCount, kCandCountValues, kCandCountCount), 0);
}

TEST(Settings_one_broken_line_does_not_kill_the_file) {
  Settings s = Settings::Parse(
      "# 註解\n"
      "沒有等號的一行\n"
      "cand.count = 5\n"
      "  text.variant   =   zh_hans  \n"
      "\n"
      "壞 鍵 名 = x\n");
  CHECK_INT(s.GetEnumInt(keys::kCandCount, kCandCountValues, kCandCountCount), 5);
  CHECK(s.SchemaPref().forced_variant == Variant::kHans);
  CHECK(!s.Has("壞 鍵 名"));
}

TEST(Settings_unknown_keys_survive_a_roundtrip) {
  // 使用者可能同時裝著兩個版本。舊版寫回設定檔時不可以把新版的鍵吃掉 ——
  // 症狀是「升級之後設定莫名其妙回到預設」,而且只在兩個版本交替執行時發生。
  const std::string in =
      "cand.count = 5\n"
      "future.thing = 42\n"
      "schema.last.0804 = luna_pinyin\n";
  Settings s = Settings::Parse(in);
  const std::string out = s.Serialize();
  Settings again = Settings::Parse(out);
  CHECK_STR(again.Raw("future.thing"), "42");
  CHECK_STR(again.Raw("schema.last.0804"), "luna_pinyin");
  CHECK_INT(again.GetEnumInt(keys::kCandCount, kCandCountValues, kCandCountCount), 5);
  CHECK_INT(again.size(), 3);
}

TEST(Settings_serialize_is_stable) {
  // 每次寫出來都一樣,不然設定檔會在版本控制或備份裡無謂地變動。
  Settings s = Settings::Parse("b.z = 1\na.y = 2\ncand.count = 9\n");
  CHECK_STR(s.Serialize(), s.Serialize());
  CHECK_STR(Settings::Parse(s.Serialize()).Serialize(), s.Serialize());
}

TEST(Settings_value_cannot_forge_an_extra_line) {
  // 值有一部分來自下載回來的市集索引,那是不可信輸入。
  Settings s;
  s.SetRaw(keys::kNetIndexUrl, "https://a/\nnet.enabled = true");
  CHECK(s.GetTri(keys::kNetEnabled) == Tri::kUnset);
  const std::string text = s.Serialize();
  CHECK(Settings::Parse(text).GetTri(keys::kNetEnabled) == Tri::kUnset);
}

TEST(Settings_network_switch_defaults_to_off) {
  // ⚠ 這一條是離線定位的地基。未設 == 關,而且只有一個地方知道答案。
  Settings s;
  CHECK(!s.NetworkEnabled());
  CHECK(s.GetTri(keys::kNetEnabled) == Tri::kUnset);
  s.SetTri(keys::kNetEnabled, Tri::kFalse);
  CHECK(!s.NetworkEnabled());
  s.SetTri(keys::kNetEnabled, Tri::kTrue);
  CHECK(s.NetworkEnabled());
  // 「關」與「沒設過」在行為上一樣,但仍然分得出來 ——
  // 分不出來的話,哪天要問「有多少人主動關掉」就問不出來了。
  s.SetTri(keys::kNetEnabled, Tri::kUnset);
  CHECK(!s.Has(keys::kNetEnabled));
  // 認不得的字面值也一律當成沒設過(= 關)。
  CHECK(!Settings::Parse("net.enabled = yes\n").NetworkEnabled());
  CHECK(!Settings::Parse("net.enabled = 1\n").NetworkEnabled());
  CHECK(Settings::Parse("net.enabled = true\n").NetworkEnabled());
}

TEST(Settings_dropdown_index_maps_both_ways) {
  // Android 端踩過:字串陣列與數值陣列的順序對不上,畫面完全正常,
  // 只是使用者選了「中」拿到「小」。
  for (int i = 0; i < kCandCountCount; ++i)
    CHECK_INT(IndexOfCandCount(CandCountAtIndex(i)), i);
  for (int i = 0; i < kCandScaleCount; ++i)
    CHECK_INT(IndexOfCandScale(CandScaleAtIndex(i)), i);
  // 第一格永遠是「跟隨」。
  CHECK_INT(CandCountAtIndex(0), 0);
  CHECK_INT(CandScaleAtIndex(0), 0);
  // 越界的索引不可以讀到別的東西,一律回第一格。
  CHECK_INT(CandCountAtIndex(-1), 0);
  CHECK_INT(CandCountAtIndex(999), 0);
  CHECK_INT(IndexOfCandCount(4), 0);  // 不是清單上的值
  // 候選數對應的是 librime 的 menu/page_size,所以每一格都必須是
  // librime 收得下的正整數。負數或 0 送進去的話部署不會失敗,
  // 但候選窗會變成空的 —— 又一個「看起來正常」的失效。
  for (int i = 1; i < kCandCountCount; ++i) CHECK(CandCountAtIndex(i) > 0);
}

TEST(Settings_last_used_is_per_language) {
  Settings s;
  s.RememberLastUsed(0x0404, "bopomofo_tw");
  s.RememberLastUsed(0x0804, "luna_pinyin");
  CHECK_STR(s.Raw("schema.last.0404"), "bopomofo_tw");
  CHECK_STR(s.Raw("schema.last.0804"), "luna_pinyin");
  const SchemaPreference p = Settings::Parse(s.Serialize()).SchemaPref();
  bool tw = false, cn = false;
  for (const auto& kv : p.last_used) {
    if (kv.first == 0x0404 && kv.second == "bopomofo_tw") tw = true;
    if (kv.first == 0x0804 && kv.second == "luna_pinyin") cn = true;
  }
  CHECK(tw);
  CHECK(cn);
  CHECK_INT(p.last_used.size(), 2);
  // 空的 schema id 不記 —— 記了會在下次啟動時把使用者的方案清成「無」。
  s.RememberLastUsed(0x0C04, "");
  CHECK(!s.Has("schema.last.0C04"));
}

TEST(Settings_schema_order_roundtrip_and_comma_defence) {
  Settings s;
  s.SetSchemaOrder({"a", "b", "c"});
  const std::vector<std::string> got = s.SchemaOrder();
  CHECK_INT(got.size(), 3);
  CHECK_STR(got[0], "a");
  CHECK_STR(got[2], "c");
  // 逗號是分隔符。含逗號的 id(來自市集索引 = 不可信)會把一項變兩項。
  s.SetSchemaOrder({"a", "b,c", "d"});
  const std::vector<std::string> got2 = s.SchemaOrder();
  CHECK_INT(got2.size(), 2);
  CHECK_STR(got2[0], "a");
  CHECK_STR(got2[1], "d");
  s.SetSchemaOrder({});
  CHECK(!s.Has(keys::kSchemaOrder));
}
