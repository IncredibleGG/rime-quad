// windows/tests/test_schema_choice.cc — 輸入模式 ↔ 方案 ↔ 簡繁
//
// 守的是 `docs/settings-model.md` §4,而 §4 開頭記著**四端已經各錯了一次**:
// macOS 註冊了兩個輸入模式但兩個都載入繁體方案;Windows 只註冊了一個 langid。
// 共同點是「畫面完全正常、自動化全過,錯的只有打出來是哪一種字」。
//
// Windows 端的版本是使用者實際回報過的:選了簡體輸入法,打出來全是繁體字。

#include <string>
#include <vector>

#include "../common/schema_choice.h"
#include "check.h"

using namespace rimewin;

namespace {

// scripts/collect_data.sh 實際打包的四個,順序就是 schema_list 的順序。
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

// ── §4.2 輸入模式的字集 ────────────────────────────────────────

TEST(CharSet_of_langid) {
  CHECK(CharSetOfLangId(0x0404) == CharSet::kHant);  // zh-Hant-TW
  CHECK(CharSetOfLangId(0x0C04) == CharSet::kHant);  // zh-Hant-HK
  CHECK(CharSetOfLangId(0x1404) == CharSet::kHant);  // zh-Hant-MO
  CHECK(CharSetOfLangId(0x0804) == CharSet::kHans);  // zh-Hans-CN
  CHECK(CharSetOfLangId(0x1004) == CharSet::kHans);  // zh-Hans-SG
}

TEST(CharSet_unknown_is_never_guessed_as_traditional) {
  // ⚠ 規範 §4.2 明著寫的:認不出來回 unspecified,**不要預設繁體**。
  //    猜錯的代價是使用者打出他不要的字,而且完全不知道為什麼。
  CHECK(CharSetOfLangId(0) == CharSet::kUnspecified);       // v1 的 DLL
  CHECK(CharSetOfLangId(0x0409) == CharSet::kUnspecified);  // en-US
  CHECK(CharSetOfLangId(0x0004) == CharSet::kUnspecified);  // 中性中文
  CHECK(CharSetOfLangId(0x0411) == CharSet::kUnspecified);  // ja-JP
}

// ── §4.3 方案的字集 ────────────────────────────────────────────

TEST(CharSet_of_schema_id_by_naming_convention) {
  CHECK(CharSetOfSchemaId("luna_pinyin_tw") == CharSet::kHant);
  CHECK(CharSetOfSchemaId("bopomofo_tw") == CharSet::kHant);
  CHECK(CharSetOfSchemaId("bopomofo") == CharSet::kHant);
  CHECK(CharSetOfSchemaId("jyutping_hk") == CharSet::kHant);
  CHECK(CharSetOfSchemaId("wubi_trad") == CharSet::kHant);
  CHECK(CharSetOfSchemaId("pinyin_simp") == CharSet::kHans);
  CHECK(CharSetOfSchemaId("wubi_cn") == CharSet::kHans);
  CHECK(CharSetOfSchemaId("x_sc") == CharSet::kHans);
}

TEST(CharSet_never_assumes_pinyin_means_simplified) {
  // 規範明著禁止的一條:**不要加「含 pinyin 就是簡體」**。
  // luna_pinyin 的預設輸出是繁體 —— 猜錯會讓 simplification 被設成
  // 相反的值,比不猜更糟。
  CHECK(CharSetOfSchemaId("luna_pinyin") == CharSet::kUnspecified);
  CHECK(CharSetOfSchemaId("t9_pinyin") == CharSet::kUnspecified);
  CHECK(CharSetOfSchemaId("double_pinyin") == CharSet::kUnspecified);
  CHECK(CharSetOfSchemaId("") == CharSet::kUnspecified);
}

// ── §4.5 簡繁:那個被回報的缺陷 ────────────────────────────────

TEST(Choice_hans_user_gets_simplified_output) {
  // **這一條就是缺陷本身。** 修之前:簡體使用者拿到繁體輸出。
  const SchemaChoice c = ChooseSchema(0x0804, kShipped, SchemaPreference());
  CHECK(c.set_variant);
  CHECK(c.simplified);
}

TEST(Choice_hant_user_gets_traditional_output) {
  const SchemaChoice c = ChooseSchema(0x0404, kShipped, SchemaPreference());
  CHECK(c.set_variant);
  CHECK(!c.simplified);
  // 已啟用清單第一個字集相符的就是 luna_pinyin_tw。
  CHECK_STR(c.schema_id, "luna_pinyin_tw");
  CHECK_STR(c.source, "charset-match");
}

TEST(Choice_hk_user_gets_traditional_output) {
  const SchemaChoice c = ChooseSchema(0x0C04, kShipped, SchemaPreference());
  CHECK(c.set_variant);
  CHECK(!c.simplified);
}

TEST(Choice_unknown_mode_touches_nothing) {
  // ⚠ 「不碰」與「設成 false」不是同一件事。v1 的 DLL 連進來時 langid 是 0,
  //    那時我們對簡繁**沒有意見** —— 強制設成繁體等於替使用者做了
  //    他沒有做過的決定。
  for (uint32_t id : {0u, 0x0409u, 0x0004u}) {
    const SchemaChoice c = ChooseSchema(id, kShipped, SchemaPreference());
    CHECK(!c.set_variant);
  }
}

TEST(Choice_explicit_variant_beats_input_mode) {
  // 規範 §4.5:「文字」頁的明確選擇優先於輸入模式 ——
  // 使用者在 app 裡明講過的話,比作業系統替他宣告的更重要。
  SchemaPreference p;
  p.variant = VariantPref::kSimplified;
  const SchemaChoice c = ChooseSchema(0x0404, kShipped, p);  // 繁體模式
  CHECK(c.set_variant);
  CHECK(c.simplified);

  p.variant = VariantPref::kTraditional;
  const SchemaChoice d = ChooseSchema(0x0804, kShipped, p);  // 簡體模式
  CHECK(d.set_variant);
  CHECK(!d.simplified);
}

TEST(Choice_follow_off_touches_nothing_at_all) {
  // 規範 §4.5 最後一條:關掉 followInputMode 時**連 simplification 都不碰**。
  // 半套(方案不跟、簡繁還是跟)更難理解。
  SchemaPreference p;
  p.follow_input_mode = false;
  const SchemaChoice c = ChooseSchema(0x0804, kShipped, p);
  CHECK(!c.set_variant);
  // 而且方案也不看輸入模式,只看有沒有釘全域的。
  CHECK_STR(c.source, "first-enabled");
  // 但明確選過簡繁的話仍然照做 —— 那是他自己說的話。
  p.variant = VariantPref::kSimplified;
  const SchemaChoice d = ChooseSchema(0x0804, kShipped, p);
  CHECK(d.set_variant);
  CHECK(d.simplified);
}

// ── §4.4 挑方案的四層 ──────────────────────────────────────────

TEST(Choice_pinned_for_mode_wins) {
  SchemaPreference p;
  p.pinned_hans = "t9_pinyin";
  p.pinned_hant = "bopomofo_tw";
  CHECK_STR(ChooseSchema(0x0804, kShipped, p).schema_id, "t9_pinyin");
  CHECK_STR(ChooseSchema(0x0404, kShipped, p).schema_id, "bopomofo_tw");
  // 釘的字集不符也照做 —— 那是使用者說的話,靠簡繁開關把字集補齊,
  // **不要偷偷換掉他選的方案**(規範 §4.4)。
  CHECK(ChooseSchema(0x0804, kShipped, p).simplified);
}

TEST(Choice_pinned_global_is_below_pinned_for_mode) {
  SchemaPreference p;
  p.pinned_global = "luna_pinyin";
  p.pinned_hans = "t9_pinyin";
  CHECK_STR(ChooseSchema(0x0804, kShipped, p).schema_id, "t9_pinyin");
  // 沒有為這個模式釘的話才輪到全域那一個。
  CHECK_STR(ChooseSchema(0x0404, kShipped, p).schema_id, "luna_pinyin");
  CHECK_STR(ChooseSchema(0x0404, kShipped, p).source, "pinnedGlobal");
}

TEST(Choice_pinned_but_disabled_falls_through) {
  // 釘的方案已經被停用了 → 當作沒釘,往下一條走。
  // 選一個清單上沒有的東西的話,librime 會拒絕,而使用者看到的是
  // 「輸入法忽然沒有候選」。
  SchemaPreference p;
  p.pinned_hant = "cangjie5";   // 沒裝
  p.pinned_global = "wubi86";   // 也沒裝
  const SchemaChoice c = ChooseSchema(0x0404, kShipped, p);
  CHECK_STR(c.schema_id, "luna_pinyin_tw");
  CHECK_STR(c.source, "charset-match");
}

TEST(Choice_charset_match_then_first) {
  // 第 3 層:清單中第一個字集相符的。
  const std::vector<std::string> list = {"luna_pinyin", "wubi_cn", "bopomofo_tw"};
  CHECK_STR(ChooseSchema(0x0804, list, SchemaPreference()).schema_id, "wubi_cn");
  CHECK_STR(ChooseSchema(0x0404, list, SchemaPreference()).schema_id,
            "bopomofo_tw");
  // 第 4 層:一個都不相符 → 清單的第一個,並靠簡繁開關補齊字集
  // (規範 §4.6 明著承認這個情形)。
  const std::vector<std::string> none = {"luna_pinyin", "t9_pinyin"};
  const SchemaChoice c = ChooseSchema(0x0804, none, SchemaPreference());
  CHECK_STR(c.schema_id, "luna_pinyin");
  CHECK_STR(c.source, "first-enabled");
  CHECK(c.simplified);
}

TEST(Choice_empty_list_does_nothing) {
  // 清單是空的 → 什麼都不做。呼叫端要在畫面上說「還沒有任何方案」。
  const SchemaChoice c = ChooseSchema(0x0804, {}, SchemaPreference());
  CHECK(c.schema_id.empty());
  CHECK_STR(c.source, "no-schemas");
}

// ── 套用:radio group 的互斥 ────────────────────────────────────

TEST(PlanVariant_turns_exactly_one_on) {
  // ⚠ rs_set_option 不會替我們維持 radio 的互斥(那是 librime 的 switcher
  //    在使用者從選單裡選時才做的)。兩個同時為真的話,t2s 之後會再串
  //    一次 t2tw,輸出變成沒有人要的東西。
  const std::vector<OptionAssign> v = PlanVariant(true, 0x0804);
  CHECK_INT(CountOn(v), 1);
  CHECK_STR(OptOf(v, "zh_hans"), "true");
  CHECK_STR(OptOf(v, "zh_hant"), "false");
  CHECK_STR(OptOf(v, "zh_hant_hk"), "false");
  CHECK_STR(OptOf(v, "zh_hant_tw"), "false");
}

TEST(PlanVariant_always_sends_simplification_too) {
  // 本專案打包的方案**沒有** simplification 這個開關,它們用 radio;
  // 而第三方方案(五筆·簡入繁出之類)只有 simplification。兩個都要送。
  // Android 端實測過:只送 simplification,打 guojia 出來還是「國家」。
  CHECK_STR(OptOf(PlanVariant(true, 0x0804), "simplification"), "true");
  CHECK_STR(OptOf(PlanVariant(false, 0x0404), "simplification"), "false");
}

TEST(PlanVariant_picks_the_right_kind_of_traditional) {
  // 使用者選的是「繁體」,是哪一種繁體由他的語言設定檔決定 ——
  // 那不是一個設定項。香港的使用者不該拿到臺灣字形。
  CHECK_STR(OptOf(PlanVariant(false, 0x0404), "zh_hant_tw"), "true");
  CHECK_STR(OptOf(PlanVariant(false, 0x0C04), "zh_hant_hk"), "true");
  CHECK_STR(OptOf(PlanVariant(false, 0x1404), "zh_hant_hk"), "true");
  // 不知道是哪一種繁體 → 傳統漢字(不轉換),而不是隨便挑一個地區。
  CHECK_STR(OptOf(PlanVariant(false, 0), "zh_hant"), "true");
  CHECK_STR(OptOf(PlanVariant(false, 0x0409), "zh_hant"), "true");
  for (uint32_t id : {0u, 0x0404u, 0x0C04u, 0x1404u, 0x0409u})
    CHECK_INT(CountOn(PlanVariant(false, id)), 1);
}

TEST(VariantPref_token_roundtrip) {
  const VariantPref all[] = {VariantPref::kFollowInputMode,
                             VariantPref::kTraditional,
                             VariantPref::kSimplified};
  for (VariantPref v : all)
    CHECK(VariantPrefFromToken(VariantPrefToken(v)) == v);
  // 字面值照規範 §3 的 enum。
  CHECK_STR(VariantPrefToken(VariantPref::kFollowInputMode), "followInputMode");
  CHECK_STR(VariantPrefToken(VariantPref::kSimplified), "simplified");
  // 認不得 = 預設值,不是崩潰也不是繁體。
  CHECK(VariantPrefFromToken("zh_hans") == VariantPref::kFollowInputMode);
  CHECK(VariantPrefFromToken("") == VariantPref::kFollowInputMode);
}

TEST(LangIdName_is_only_for_logs) {
  CHECK_STR(LangIdName(0x0804), "zh-Hans-CN");
  CHECK_STR(LangIdName(0x0404), "zh-Hant-TW");
  CHECK_STR(LangIdName(0x0409), "?");
}

// ══════════════════════════════════════════════════════════════════
//  預熱要暖的是「方案 + 選項」那一整組
// ══════════════════════════════════════════════════════════════════
//
// ⚠ 這兩則守的是一個**已經發生過、而且打到每一個新使用者**的缺陷。
//
// 服務的預熱原本只用 langid 0 暖一次,註解寫著「三份語言設定檔選到的
// 都是同一個方案,所以涵蓋得到全部三種使用者」。那句話**是對的,
// 但它不相干** —— 貴的東西不在方案那一格,在選項:
// simplification / zh_hans 這一組要載入 OpenCC 的 .ocd2 字典,
// 而那是第一次用到才發生的檔案載入。
//
// 於是每一個新使用者的第一次打字,都在 SESSION_NEW 的 300 毫秒預算裡
// 付那筆錢 —— 逾時、fail-open、打出一串英文,而且沒有任何錯誤訊息。
// (2026-08-09,CI §5c 冷啟動。)

// 本專案實際打包的 schema_list(core/data/user/default.custom.yaml)。
static std::vector<std::string> ShippedSchemas() {
  return {"luna_pinyin_tw", "bopomofo_tw", "luna_pinyin", "t9_pinyin"};
}

TEST(warming_up_with_langid_zero_alone_does_not_cover_a_real_user) {
  const std::vector<std::string> shipped = ShippedSchemas();
  SchemaPreference pref;  // 全新安裝:follow_input_mode = true,什麼都沒釘

  const SchemaChoice warm = ChooseSchema(0, shipped, pref);
  const SchemaChoice real = ChooseSchema(0x0804, shipped, pref);

  // 方案 id **一樣** —— 舊版的註解就是看到這一點才說「涵蓋得到」。
  CHECK_STR(warm.schema_id.c_str(), real.schema_id.c_str());

  // 而選項完全不同,那才是要載檔的那一格。
  CHECK(!warm.set_variant);  // langid 0 → kUnspecified → 一個選項都不套
  CHECK(real.set_variant);   // 0x0804  → kHans        → 要套 simplification
  CHECK(real.simplified);
}

// 暖的那一組,必須涵蓋真的使用者會帶進來的每一種選項計畫。
//
// ⚠ 這一則是上面那一則的「正面」版本:上面說明為什麼一個不夠,
//   這一則要求**現在這份清單真的夠**。有人把 kWarmUpLangIds 砍短、
//   或新增一種語言設定檔而忘了加進去時,它會紅。
TEST(warm_up_langids_cover_every_real_users_option_plan) {
  const std::vector<std::string> shipped = ShippedSchemas();
  SchemaPreference pref;

  // 使用者真的可能帶進來的 langid:三份註冊的語言設定檔,加上「問不出來」。
  const uint32_t kReal[] = {0u, 0x0404u, 0x0804u, 0x0C04u};
  for (uint32_t langid : kReal) {
    const SchemaChoice want = ChooseSchema(langid, shipped, pref);
    std::vector<OptionAssign> want_opts;
    if (want.set_variant) want_opts = PlanVariant(want.simplified, langid);

    bool covered = false;
    for (int i = 0; i < kWarmUpLangIdCount; ++i) {
      const uint32_t w = kWarmUpLangIds[i];
      const SchemaChoice got = ChooseSchema(w, shipped, pref);
      if (got.schema_id != want.schema_id) continue;
      std::vector<OptionAssign> got_opts;
      if (got.set_variant) got_opts = PlanVariant(got.simplified, w);
      if (got_opts.size() != want_opts.size()) continue;
      bool same = true;
      for (size_t k = 0; k < got_opts.size(); ++k)
        if (std::string(got_opts[k].option) != std::string(want_opts[k].option) ||
            got_opts[k].value != want_opts[k].value)
          same = false;
      if (same) {
        covered = true;
        break;
      }
    }
    CHECK(covered);
  }
}
