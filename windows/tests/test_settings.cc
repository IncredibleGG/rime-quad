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
  CHECK(s.SchemaPref().variant == VariantPref::kFollowInputMode);
  // 設回預設值必須是**刪掉那個鍵**,不是寫一個哨兵值。
  s.SetVariantPref(VariantPref::kSimplified);
  CHECK(s.Has(keys::kTextVariant));
  s.SetVariantPref(VariantPref::kFollowInputMode);
  CHECK(!s.Has(keys::kTextVariant));
  // 方案同理。
  s.SetPinnedGlobal("luna_pinyin");
  CHECK(s.Has(keys::kSchemasPinnedGlobal));
  s.SetPinnedGlobal("");
  CHECK(!s.Has(keys::kSchemasPinnedGlobal));
}

TEST(Settings_follow_input_mode_defaults_to_true) {
  // ⚠ 預設是 true(規範 §3),所以「沒有這個鍵」必須讀成 true。
  //   寫成 GetTri() == kTrue 的話,全新安裝的機器上自動挑方案是關的,
  //   而那正是這一輪要修的缺陷本身。
  Settings s;
  CHECK(s.SchemaPref().follow_input_mode);
  s.SetFollowInputMode(false);
  CHECK(!s.SchemaPref().follow_input_mode);
  CHECK(s.Has(keys::kSchemasFollowInputMode));
  // 設回 true = 刪掉那個鍵(不要把預設值寫進檔案)。
  s.SetFollowInputMode(true);
  CHECK(!s.Has(keys::kSchemasFollowInputMode));
  CHECK(s.SchemaPref().follow_input_mode);
  // 認不得的字面值也一律當成沒設過 = true。
  CHECK(Settings::Parse("schemas.followInputMode = 亂寫\n")
            .SchemaPref().follow_input_mode);
}

TEST(Settings_pinned_per_charset) {
  Settings s;
  s.SetPinnedForCharSet(CharSet::kHant, "bopomofo_tw");
  s.SetPinnedForCharSet(CharSet::kHans, "luna_pinyin");
  const SchemaPreference p = Settings::Parse(s.Serialize()).SchemaPref();
  CHECK_STR(p.pinned_hant, "bopomofo_tw");
  CHECK_STR(p.pinned_hans, "luna_pinyin");
  // kUnspecified 沒有對應的桶。硬塞進繁體那一桶會讓「不知道是哪一種」
  // 變成「他選了繁體」,而使用者從來沒說過那句話。
  Settings t;
  t.SetPinnedForCharSet(CharSet::kUnspecified, "t9_pinyin");
  CHECK_INT(t.size(), 0);
  // 空字串 = 取消釘,不是釘一個空的。
  s.SetPinnedForCharSet(CharSet::kHant, "");
  CHECK(!s.Has(keys::kSchemasPinnedHant));
}

TEST(Settings_enum_first_value_is_the_follow_slot_and_is_never_written) {
  Settings s;
  const char* k = keys::kAppearanceCandidateScale;
  s.SetEnumInt(k, 120, kCandScaleValues, kCandScaleCount);
  CHECK_STR(s.Raw(k), "120");
  // 第一格 0 是「不干預」。寫它 = 把「沒設過」變成「設過」。
  s.SetEnumInt(k, 0, kCandScaleValues, kCandScaleCount);
  CHECK(!s.Has(k));
  // 不在允許清單裡的值不可以偷偷存起來。
  s.SetEnumInt(k, 111, kCandScaleValues, kCandScaleCount);
  CHECK(!s.Has(k));
}

TEST(Settings_corrupt_value_reads_as_unset_not_as_garbage) {
  Settings s = Settings::Parse(
      "appearance.candidateScale = 這不是數字\ntext.variant = 亂寫\n");
  CHECK_INT(s.GetEnumInt(keys::kAppearanceCandidateScale, kCandScaleValues,
                         kCandScaleCount), 0);
  CHECK(s.SchemaPref().variant == VariantPref::kFollowInputMode);
  // 越界的數字同理。
  Settings t = Settings::Parse("appearance.candidateScale = 999\n");
  CHECK_INT(t.GetEnumInt(keys::kAppearanceCandidateScale, kCandScaleValues,
                         kCandScaleCount), 0);
}

TEST(Settings_one_broken_line_does_not_kill_the_file) {
  Settings s = Settings::Parse(
      "# 註解\n"
      "沒有等號的一行\n"
      "appearance.candidateScale = 120\n"
      "  text.variant   =   simplified  \n"
      "\n"
      "壞 鍵 名 = x\n");
  CHECK_INT(s.GetEnumInt(keys::kAppearanceCandidateScale, kCandScaleValues,
                         kCandScaleCount), 120);
  CHECK(s.SchemaPref().variant == VariantPref::kSimplified);
  CHECK(!s.Has("壞 鍵 名"));
}

TEST(Settings_unknown_keys_survive_a_roundtrip) {
  // 使用者可能同時裝著兩個版本。舊版寫回設定檔時不可以把新版的鍵吃掉 ——
  // 症狀是「升級之後設定莫名其妙回到預設」,而且只在兩個版本交替執行時發生。
  const std::string in =
      "appearance.candidateScale = 120\n"
      "future.thing = 42\n"
      "dictionary.somethingNew = on\n";
  Settings s = Settings::Parse(in);
  const std::string out = s.Serialize();
  Settings again = Settings::Parse(out);
  CHECK_STR(again.Raw("future.thing"), "42");
  CHECK_STR(again.Raw("dictionary.somethingNew"), "on");
  CHECK_INT(again.GetEnumInt(keys::kAppearanceCandidateScale, kCandScaleValues,
                             kCandScaleCount), 120);
  CHECK_INT(again.size(), 3);
}

TEST(Settings_serialize_is_stable) {
  // 每次寫出來都一樣,不然設定檔會在版本控制或備份裡無謂地變動。
  Settings s = Settings::Parse("b.z = 1\na.y = 2\ntext.variant = simplified\n");
  CHECK_STR(s.Serialize(), s.Serialize());
  CHECK_STR(Settings::Parse(s.Serialize()).Serialize(), s.Serialize());
}

TEST(Settings_value_cannot_forge_an_extra_line) {
  // 值有一部分來自下載回來的市集索引,那是不可信輸入。
  Settings s;
  s.SetRaw(keys::kStoreIndexUrl, "https://a/\nnetwork.enabled = true");
  CHECK(s.GetTri(keys::kNetworkEnabled) == Tri::kUnset);
  const std::string text = s.Serialize();
  CHECK(Settings::Parse(text).GetTri(keys::kNetworkEnabled) == Tri::kUnset);
}

TEST(Settings_network_switch_defaults_to_off) {
  // ⚠ 這一條是離線定位的地基。未設 == 關,而且只有一個地方知道答案。
  Settings s;
  CHECK(!s.NetworkEnabled());
  CHECK(s.GetTri(keys::kNetworkEnabled) == Tri::kUnset);
  s.SetTri(keys::kNetworkEnabled, Tri::kFalse);
  CHECK(!s.NetworkEnabled());
  s.SetTri(keys::kNetworkEnabled, Tri::kTrue);
  CHECK(s.NetworkEnabled());
  // 「關」與「沒設過」在行為上一樣,但仍然分得出來 ——
  // 分不出來的話,哪天要問「有多少人主動關掉」就問不出來了。
  s.SetTri(keys::kNetworkEnabled, Tri::kUnset);
  CHECK(!s.Has(keys::kNetworkEnabled));
  // 認不得的字面值也一律當成沒設過(= 關)。
  CHECK(!Settings::Parse("network.enabled = yes\n").NetworkEnabled());
  CHECK(!Settings::Parse("network.enabled = 1\n").NetworkEnabled());
  CHECK(Settings::Parse("network.enabled = true\n").NetworkEnabled());
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
  // 候選數是 A 層,對應 librime 的一頁候選數,所以每一格都必須是
  // 它收得下的正整數。負數或 0 送進去的話部署不會失敗,
  // 但選字視窗會變成空的 —— 又一個「看起來正常」的失效。
  for (int i = 1; i < kCandCountCount; ++i) CHECK(CandCountAtIndex(i) > 0);
}


// ── 全／半形(G70)─────────────────────────────────────────────
//
// ⚠ 這一組守的與標點是同一件事:**「跟著方案」與「設成半形」不是同一件事。**
//   很多方案根本沒有 full_shape 這個開關,而有些方案的預設是全形。
//   無條件送 false 會讓「不干預」變成「一律半形」,而使用者選的是不干預。
TEST(Settings_shape_unset_means_follow_schema) {
  Settings s;
  CHECK(!s.Has(keys::kTextShape));
  CHECK(s.Shape() == Tri::kUnset);
  s.SetShape(Tri::kTrue);
  CHECK(s.Has(keys::kTextShape));
  CHECK(s.Shape() == Tri::kTrue);
  s.SetShape(Tri::kFalse);
  CHECK(s.Shape() == Tri::kFalse);
  // 回到「跟著方案」= **刪掉那個鍵**,不是寫一個哨兵值(見 settings.h 檔頭)。
  s.SetShape(Tri::kUnset);
  CHECK(!s.Has(keys::kTextShape));
}

TEST(Settings_shape_survives_a_round_trip) {
  // 序列化沒有把它列進 kKnownKeys 的話,它會掉到「未知的鍵」那一段 ——
  // 讀得回來,但分區註解與順序都不對,而且**沒有任何測試看得出來**。
  // 所以這裡直接斷言它出現在「文字」那一區、而且緊接在標點後面。
  Settings s;
  s.SetPunctuation(Tri::kTrue);
  s.SetShape(Tri::kTrue);
  const std::string text = s.Serialize();
  // ⚠ 「排在標點後面」擋不住這件事:**未知的鍵是附在整份設定的最後**,
  //   所以少列一條 kKnownKeys 時 p < q 照樣成立(實跑確認過:那個植入
  //   完全沒有變紅)。判準要用「它排在**它後面那個已知鍵**的前面」。
  s.SetTri(keys::kTextShiftTapToggle, Tri::kFalse);
  const std::string text2 = s.Serialize();
  const size_t p = text2.find(keys::kTextPunctuation);
  const size_t q = text2.find(keys::kTextShape);
  const size_t r = text2.find(keys::kTextShiftTapToggle);
  CHECK(p != std::string::npos);
  CHECK(q != std::string::npos);
  CHECK(r != std::string::npos);
  CHECK(p < q);
  CHECK(q < r);
  Settings back = Settings::Parse(text);
  CHECK(back.Shape() == Tri::kTrue);
}
