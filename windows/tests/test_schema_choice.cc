// windows/tests/test_schema_choice.cc — 語言設定檔 → 方案/字形
//
// 這一組守的是一個**使用者實際回報過**的缺陷:簡體使用者在語言列上選了
// 簡體那一份輸入法,打出來全是繁體字。修法是讓 DLL 把 langid 帶進 IPC,
// 而「langid → 方案」這一格是純邏輯,所以在 Ubuntu 上就守得住。

#include <string>
#include <vector>

#include "../common/schema_choice.h"
#include "check.h"

using namespace rimewin;

namespace {

const std::vector<std::string> kShipped = {"luna_pinyin_tw", "bopomofo_tw",
                                           "luna_pinyin", "t9_pinyin"};

std::string OptOf(const std::vector<OptionAssign>& v, const char* name) {
  for (const OptionAssign& a : v)
    if (std::string(a.option) == name) return a.value ? "true" : "false";
  return "(缺)";
}

int CountOn(const std::vector<OptionAssign>& v) {
  int n = 0;
  for (const OptionAssign& a : v) {
    if (!a.value) continue;
    for (int i = 0; i < kVariantOptionCount; ++i)
      if (std::string(a.option) == kVariantOptions[i]) ++n;
  }
  return n;
}

}  // namespace

// ── 核心:三個註冊在案的語言各自拿到什麼 ────────────────────────

TEST(SchemaChoice_TW_gets_traditional) {
  const SchemaChoice c = DefaultForLangId(0x0404, kShipped);
  CHECK_STR(c.schema_id, "luna_pinyin_tw");
  CHECK(c.variant == Variant::kHantTW);
}

TEST(SchemaChoice_Hans_gets_simplified) {
  // 這一條就是那個缺陷本身。修之前它是 luna_pinyin_tw + 繁體。
  const SchemaChoice c = DefaultForLangId(0x0804, kShipped);
  CHECK_STR(c.schema_id, "luna_pinyin");
  CHECK(c.variant == Variant::kHans);
  // 明著寫出「不可以是那一個」,免得日後有人把候選順序改回去。
  CHECK(c.schema_id != "luna_pinyin_tw");
}

TEST(SchemaChoice_HK_gets_hk_glyphs) {
  const SchemaChoice c = DefaultForLangId(0x0C04, kShipped);
  CHECK_STR(c.schema_id, "luna_pinyin");
  CHECK(c.variant == Variant::kHantHK);
}

TEST(SchemaChoice_SG_and_MO_are_mapped_even_though_unregistered) {
  // 目前沒有註冊這兩份 profile,但表上先有 —— 哪天加了 profile,
  // 這一格已經是對的,而不是安靜地落到「沒有意見」。
  CHECK(DefaultForLangId(0x1004, kShipped).variant == Variant::kHans);
  CHECK(DefaultForLangId(0x1404, kShipped).variant == Variant::kHantHK);
}

TEST(SchemaChoice_non_chinese_has_no_opinion) {
  // 0x0409 = en-US。日後若有人把這個輸入法註冊到別的語言底下,
  // 我們不可以替他挑一個中文方案。
  const SchemaChoice c = DefaultForLangId(0x0409, kShipped);
  CHECK(c.schema_id.empty());
  CHECK(c.variant == Variant::kFollow);
  CHECK(!IsChineseLangId(0x0409));
  CHECK(IsChineseLangId(0x0804));
  // 0x0004 是中性中文:是中文,但沒有 sublang 可以推。
  const SchemaChoice n = DefaultForLangId(0x0004, kShipped);
  CHECK(n.schema_id.empty());
  CHECK(n.variant == Variant::kFollow);
}

TEST(SchemaChoice_langid_zero_means_unknown) {
  // v1 的 DLL 連進來時 langid 是 0。0 必須等於「沒有意見」——
  // 若它等於任何一個具體語言,舊 DLL 的使用者會拿到一個他沒選過的字形。
  const SchemaChoice c = DefaultForLangId(0, kShipped);
  CHECK(c.schema_id.empty());
  CHECK(c.variant == Variant::kFollow);
}

TEST(SchemaChoice_never_picks_bopomofo_or_t9) {
  // 注音的鍵位與拼音完全不同,九宮格是行動端的。這兩個可以由使用者自己選,
  // 但不可以是我們替他選的 —— 那等於「裝完之後鍵盤是壞的」。
  for (uint32_t id : {0x0404u, 0x0804u, 0x0C04u, 0x1004u, 0x1404u}) {
    const SchemaChoice c = DefaultForLangId(id, kShipped);
    CHECK(c.schema_id != "bopomofo_tw");
    CHECK(c.schema_id != "t9_pinyin");
  }
}

TEST(SchemaChoice_falls_back_when_preferred_missing) {
  // 使用者從市集裝了東西、又把內建的移出 schema_list 的情形。
  const std::vector<std::string> only_tw = {"luna_pinyin_tw"};
  CHECK_STR(DefaultForLangId(0x0804, only_tw).schema_id, "luna_pinyin_tw");
  const std::vector<std::string> only_plain = {"luna_pinyin"};
  CHECK_STR(DefaultForLangId(0x0404, only_plain).schema_id, "luna_pinyin");
  // 兩個都沒有 → 方案不表示意見(交給 schema_list 第一項),
  // 但**字形照樣套用**:使用者裝的第三方簡體方案也吃同一組 radio 開關。
  const std::vector<std::string> none = {"wubi86", "cangjie5"};
  const SchemaChoice c = DefaultForLangId(0x0804, none);
  CHECK(c.schema_id.empty());
  CHECK(c.variant == Variant::kHans);
}

TEST(SchemaChoice_empty_list_still_gives_first_choice) {
  // 還沒部署完就有人連進來。選不到 librime 會拒絕,而拒絕看得見;
  // 安靜地不選才是查不出來的那一種。
  const SchemaChoice c = DefaultForLangId(0x0804, {});
  CHECK_STR(c.schema_id, "luna_pinyin");
}

// ── 優先順序:設定介面 vs langid ────────────────────────────────

TEST(SchemaChoice_user_forced_schema_beats_langid) {
  SchemaPreference p;
  p.forced_schema = "bopomofo_tw";
  for (uint32_t id : {0x0404u, 0x0804u, 0x0C04u}) {
    const SchemaChoice c = ChooseSchema(id, kShipped, p);
    CHECK_STR(c.schema_id, "bopomofo_tw");
  }
  // 但字形仍然跟著語言 —— 使用者只覆寫了方案。
  CHECK(ChooseSchema(0x0804, kShipped, p).variant == Variant::kHans);
}

TEST(SchemaChoice_user_forced_variant_beats_langid) {
  SchemaPreference p;
  p.forced_variant = Variant::kHans;
  const SchemaChoice c = ChooseSchema(0x0404, kShipped, p);
  CHECK(c.variant == Variant::kHans);
  // 方案沒被覆寫。
  CHECK_STR(c.schema_id, "luna_pinyin_tw");
}

TEST(SchemaChoice_last_used_beats_langid_but_not_forced) {
  SchemaPreference p;
  p.last_used.emplace_back(0x0404u, "bopomofo_tw");
  // 使用者在繁體那一份底下按過 Ctrl+` 換成注音 → 換到別的 app 不該被打回去。
  CHECK_STR(ChooseSchema(0x0404, kShipped, p).schema_id, "bopomofo_tw");
  // 另一個語言不受影響。
  CHECK_STR(ChooseSchema(0x0804, kShipped, p).schema_id, "luna_pinyin");
  // 設定裡明著指定的仍然贏。
  p.forced_schema = "t9_pinyin";
  CHECK_STR(ChooseSchema(0x0404, kShipped, p).schema_id, "t9_pinyin");
}

TEST(SchemaChoice_empty_last_used_entry_is_ignored) {
  SchemaPreference p;
  p.last_used.emplace_back(0x0804u, "");
  CHECK_STR(ChooseSchema(0x0804, kShipped, p).schema_id, "luna_pinyin");
}

// ── 字形:radio group 的互斥 ────────────────────────────────────

TEST(Variant_follow_sends_nothing) {
  // 「沒有意見」與「選繁體」不是同一件事。新裝的機器上把每個人
  // 強制設成傳統漢字,等於替使用者做了他沒做過的決定。
  CHECK_INT(PlanVariant(Variant::kFollow, Variant::kHantTW).size(), 0);
}

TEST(Variant_hans_turns_exactly_one_on_and_the_rest_off) {
  const std::vector<OptionAssign> v = PlanVariant(Variant::kHans, Variant::kFollow);
  // radio group 的互斥不是 rs_set_option 會替我們做的事:
  // 兩個同時為真的話 t2s 之後再串一次 t2tw,輸出變成沒有人要的東西。
  CHECK_INT(CountOn(v), 1);
  CHECK_STR(OptOf(v, "zh_hans"), "true");
  CHECK_STR(OptOf(v, "zh_hant"), "false");
  CHECK_STR(OptOf(v, "zh_hant_hk"), "false");
  CHECK_STR(OptOf(v, "zh_hant_tw"), "false");
  // 而且要一併送 simplification:本專案打包的方案沒有這個開關,
  // 但第三方方案(五筆·簡入繁出之類)有,而**只送它是沒有作用的**
  // —— Android 端在模擬器上實測過,打 guojia 出來還是「國家」。
  CHECK_STR(OptOf(v, "simplification"), "true");
}

TEST(Variant_tw_is_specific_and_not_overridden_by_saved) {
  // kHantTW 是明確的要求(來自 langid 或設定),拿 saved 蓋掉它
  // 等於忽略使用者剛剛按的那一下。
  const std::vector<OptionAssign> v = PlanVariant(Variant::kHantTW, Variant::kHant);
  CHECK_STR(OptOf(v, "zh_hant_tw"), "true");
  CHECK_STR(OptOf(v, "zh_hant"), "false");
  CHECK_STR(OptOf(v, "simplification"), "false");
  CHECK_INT(CountOn(v), 1);
}

TEST(Variant_generic_traditional_restores_saved) {
  // 簡繁切換那顆鍵切回來時,只說「不要簡體」,沒說要哪一種繁體。
  // 本來停在臺灣字形的人就該回到臺灣字形 —— 硬設 zh_hant 的話他會
  // 安靜地落到傳統漢字,差別小到當下不會發現,只覺得「有幾個字變了」。
  const std::vector<OptionAssign> v = PlanVariant(Variant::kHant, Variant::kHantTW);
  CHECK_STR(OptOf(v, "zh_hant_tw"), "true");
  CHECK_STR(OptOf(v, "zh_hant"), "false");
  CHECK_INT(CountOn(v), 1);
  // 沒有東西可以還原時才落到 zh_hant 本身。
  const std::vector<OptionAssign> w = PlanVariant(Variant::kHant, Variant::kFollow);
  CHECK_STR(OptOf(w, "zh_hant"), "true");
  // saved 是簡體也不算「可以還原的繁體」。
  const std::vector<OptionAssign> x = PlanVariant(Variant::kHant, Variant::kHans);
  CHECK_STR(OptOf(x, "zh_hant"), "true");
}

TEST(Variant_token_roundtrip) {
  const Variant all[] = {Variant::kFollow, Variant::kHant, Variant::kHans,
                         Variant::kHantHK, Variant::kHantTW};
  for (Variant v : all) CHECK(VariantFromToken(VariantToken(v)) == v);
  // 認不得的字面值 = 跟隨,不是崩潰也不是預設繁體。
  CHECK(VariantFromToken("zh_hant_xx") == Variant::kFollow);
  CHECK(VariantFromToken("") == Variant::kFollow);
  // 設定檔的字面值刻意就是 librime 的 option 名稱。
  CHECK_STR(VariantToken(Variant::kHans), "zh_hans");
}

TEST(LangIdName_is_only_for_logs) {
  CHECK_STR(LangIdName(0x0804), "zh-Hans-CN");
  CHECK_STR(LangIdName(0x0404), "zh-Hant-TW");
  CHECK_STR(LangIdName(0x0409), "?");
}
