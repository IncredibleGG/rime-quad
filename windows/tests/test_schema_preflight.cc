// windows/tests/test_schema_preflight.cc — 部署前的相依檢查
//
// ⚠ 這個檔案的存在理由是 Android 端的那次事故:第一版把每一個找不到的
//   檔案都當成「不給啟用」,結果**市集 98 個方案裡有 20 個被自己的預檢
//   擋死**,而 librime 根本不在乎那些檔案在不在。
//   所以下面的案例分成兩半:哪些**必須擋**、哪些**必須放行**。
//   只測前半的話,那次事故會原封不動回來而測試全綠。

#include <set>
#include <string>

#include "../common/schema_preflight.h"
#include "check.h"

using namespace rimewin;

namespace {

FileExistsFn Have(const std::set<std::string>& files) {
  return [files](const std::string& name) { return files.count(name) != 0; };
}

int CountKind(const PreflightReport& r, PreflightKind k) {
  int n = 0;
  for (const auto& m : r.missing) {
    if (m.kind == k) ++n;
  }
  return n;
}

bool BlocksOn(const PreflightReport& r, const std::string& name) {
  for (const auto& m : r.Blocking()) {
    if (m.name == name) return true;
  }
  return false;
}

bool WarnsOn(const PreflightReport& r, const std::string& name) {
  for (const auto& m : r.Warnings()) {
    if (m.name == name) return true;
  }
  return false;
}

}  // namespace

/* ═════════════════ 掃描器本身(讀錯 yaml 與規則寫錯要分得開)══════ */

TEST(Preflight_scanner_reads_the_shapes_real_schemas_use) {
  const std::string yaml =
      "# 註解\n"
      "schema:\n"
      "  schema_id: luna_pinyin\n"
      "  name: 朙月拼音\n"
      "  dependencies:\n"
      "    - stroke\n"
      "    - terra_pinyin\n"
      "engine:\n"
      "  translators:\n"
      "    - punct_translator\n"
      "    - table_translator\n"
      "translator:\n"
      "  dictionary: luna_pinyin   # 行尾註解\n"
      "punctuator:\n"
      "  import_preset: default\n";
  const std::vector<YamlPair> pairs = ScanSchemaYaml(yaml);

  bool saw_dict = false, saw_dep = false, saw_preset = false, saw_id = false;
  for (const auto& p : pairs) {
    if (p.key == "dictionary") {
      saw_dict = true;
      CHECK_STR(p.top_key, std::string("translator"));
      CHECK_STR(p.value, std::string("luna_pinyin"));  // 行尾註解要被剝掉
    }
    if (p.key == "dependencies" && p.value == "stroke") saw_dep = true;
    if (p.key == "import_preset") {
      saw_preset = true;
      CHECK_STR(p.top_key, std::string("punctuator"));
    }
    if (p.key == "schema_id") {
      saw_id = true;
      CHECK_STR(p.top_key, std::string("schema"));
    }
  }
  CHECK(saw_dict);
  CHECK(saw_dep);
  CHECK(saw_preset);
  CHECK(saw_id);
}

TEST(Preflight_scanner_handles_flow_sequences) {
  const std::vector<YamlPair> pairs =
      ScanSchemaYaml("schema:\n  dependencies: [stroke, luna_pinyin]\n");
  int n = 0;
  for (const auto& p : pairs) {
    if (p.key == "dependencies") ++n;
  }
  CHECK_INT(n, 2);
}

TEST(Preflight_scanner_does_not_split_include_values_on_the_first_colon) {
  // `__include: symbols.yaml:/punctuator` 的值裡本來就有冒號。
  const std::vector<YamlPair> pairs =
      ScanSchemaYaml("punctuator:\n  __include: symbols.yaml:/punctuator\n");
  bool found = false;
  for (const auto& p : pairs) {
    if (p.key == "__include") {
      found = true;
      CHECK_STR(p.value, std::string("symbols.yaml:/punctuator"));
    }
  }
  CHECK(found);
}

/* ═════════════════ __include 的分界線 ═══════════════════════════ */

TEST(Preflight_include_target_colon_is_the_boundary) {
  // ⚠ 這一段在 Android 端曾經整段寫錯,代價是十個方案按「啟用」被擋死。
  //   librime 的規則:**有沒有冒號才是分界線,不是開頭的斜線。**
  IncludeRef ref;
  CHECK(!ParseIncludeTarget("array30_format", &ref));   // 同檔案內的節點
  CHECK(!ParseIncludeTarget("reverse_format", &ref));
  CHECK(!ParseIncludeTarget("/patch/foo", &ref));       // 沒有冒號 → 同檔案
  CHECK(!ParseIncludeTarget(":/only/node", &ref));      // 冒號在開頭 → 同檔案

  CHECK(ParseIncludeTarget("symbols.yaml:/punctuator", &ref));
  CHECK_STR(ref.file_name, std::string("symbols.yaml"));
  CHECK(!ref.optional);

  CHECK(ParseIncludeTarget("pinyin:/translator", &ref));
  CHECK_STR(ref.file_name, std::string("pinyin.yaml"));  // 自動補 .yaml

  CHECK(ParseIncludeTarget("maybe.yaml:/node?", &ref));
  CHECK(ref.optional);   // 結尾的 ? = 可有可無,缺了不算失敗
}

/* ═════════════════ 必須擋的 ═══════════════════════════════════ */

TEST(Preflight_blocks_a_missing_translator_dictionary) {
  // librime 的 SchemaUpdate::Run 只編這一本,缺了部署真的回 false。
  const std::string yaml =
      "schema:\n  schema_id: wubi86\ntranslator:\n  dictionary: wubi86\n";
  const PreflightReport r = PreflightSchemaText("wubi86", yaml, Have({}));
  CHECK(!r.Ok());
  CHECK(BlocksOn(r, "wubi86.dict.yaml"));
  CHECK_STR(r.schema_id, std::string("wubi86"));

  // 檔案在的時候不可以有任何話。
  const PreflightReport ok =
      PreflightSchemaText("wubi86", yaml, Have({"wubi86.dict.yaml"}));
  CHECK(ok.Ok());
  CHECK_INT(static_cast<int>(ok.missing.size()), 0);
}

TEST(Preflight_blocks_a_missing_include_or_preset) {
  const std::string yaml =
      "schema:\n  schema_id: x\n"
      "punctuator:\n  import_preset: default\n"
      "key_binder:\n  __include: symbols.yaml:/key_binder\n";
  const PreflightReport r = PreflightSchemaText("x", yaml, Have({}));
  CHECK(!r.Ok());
  CHECK(BlocksOn(r, "default.yaml"));
  CHECK(BlocksOn(r, "symbols.yaml"));
}

TEST(Preflight_optional_include_only_warns) {
  const std::string yaml =
      "schema:\n  schema_id: x\nfoo:\n  __include: extra.yaml:/node?\n";
  const PreflightReport r = PreflightSchemaText("x", yaml, Have({}));
  CHECK(r.Ok());
  CHECK(WarnsOn(r, "extra.yaml"));
}

/* ═════════════════ 必須放行的(那次事故的形狀)═════════════════ */

TEST(Preflight_lets_a_missing_dependency_through) {
  // librime 只印 `skipped unsatisfied dependency`,主方案照樣部署成功。
  const std::string yaml =
      "schema:\n  schema_id: array30\n  dependencies:\n    - luna_pinyin\n"
      "translator:\n  dictionary: array30\n";
  const PreflightReport r =
      PreflightSchemaText("array30", yaml, Have({"array30.dict.yaml"}));
  CHECK(r.Ok());   // ← 這一行就是那次事故
  CHECK(WarnsOn(r, "luna_pinyin.schema.yaml"));
}

TEST(Preflight_lets_a_missing_reverse_lookup_dictionary_through) {
  // SchemaUpdate 只看 translator/dictionary,別處的 dictionary 碰都沒碰。
  const std::string yaml =
      "schema:\n  schema_id: cangjie5\n"
      "translator:\n  dictionary: cangjie5\n"
      "reverse_lookup:\n  dictionary: luna_quanpin\n";
  const PreflightReport r =
      PreflightSchemaText("cangjie5", yaml, Have({"cangjie5.dict.yaml"}));
  CHECK(r.Ok());
  CHECK(WarnsOn(r, "luna_quanpin.dict.yaml"));
  CHECK_INT(static_cast<int>(r.Blocking().size()), 0);
}

TEST(Preflight_lets_a_missing_grammar_through) {
  const std::string yaml =
      "schema:\n  schema_id: ice\n"
      "translator:\n  dictionary: ice\n"
      "grammar:\n  language: zh-hans-t-essay-bgw\n";
  const PreflightReport r = PreflightSchemaText("ice", yaml, Have({"ice.dict.yaml"}));
  CHECK(r.Ok());
  CHECK(WarnsOn(r, "zh-hans-t-essay-bgw.gram"));
}

TEST(Preflight_include_of_a_local_node_needs_no_file) {
  // 倉頡的 `__include: array30_format` 指的是同一份 yaml 裡的節點。
  const std::string yaml =
      "schema:\n  schema_id: array30\n"
      "translator:\n  dictionary: array30\n"
      "comment_format:\n  __include: array30_format\n";
  const PreflightReport r =
      PreflightSchemaText("array30", yaml, Have({"array30.dict.yaml"}));
  CHECK(r.Ok());
  CHECK_INT(static_cast<int>(r.missing.size()), 0);
}

TEST(Preflight_stricter_reference_wins_when_a_file_is_named_twice) {
  const std::string yaml =
      "schema:\n  schema_id: x\n"
      "reverse_lookup:\n  dictionary: shared\n"
      "translator:\n  dictionary: shared\n";
  const PreflightReport r = PreflightSchemaText("x", yaml, Have({}));
  CHECK(!r.Ok());
  CHECK(BlocksOn(r, "shared.dict.yaml"));
  CHECK_INT(CountKind(r, PreflightKind::kDictionary), 1);  // 只講一次
}

TEST(Preflight_uses_the_declared_schema_id_in_the_message) {
  const std::string yaml =
      "schema:\n  schema_id: double_pinyin_flypy\ntranslator:\n  dictionary: x\n";
  const PreflightReport r = PreflightSchemaText("some_file_name", yaml, Have({}));
  CHECK_STR(r.schema_id, std::string("double_pinyin_flypy"));
  CHECK_STR(r.Blocking().front().referenced_by,
            std::string("double_pinyin_flypy"));
}

TEST(Preflight_unparsable_yaml_does_not_invent_problems) {
  // 認不出來就別亂猜,讓 librime 自己去報錯 —— 誤報會擋住合法套件。
  const PreflightReport r =
      PreflightSchemaText("x", "\x01\x02\x03 not yaml at all", Have({}));
  CHECK(r.Ok());
}

/* ═════════════════ lua(Windows 專屬)═══════════════════════════ */

TEST(Preflight_blocks_lua_schemas_while_this_build_has_no_lua) {
  // 沒有 librime-lua 時,這種方案會**部署成功但一個候選都沒有** ——
  // 畫面上沒有任何錯誤,使用者只會覺得輸入法壞了。
  const std::string yaml =
      "schema:\n  schema_id: moran\n"
      "engine:\n"
      "  translators:\n"
      "    - lua_translator@*moran_charset_filter\n"
      "translator:\n  dictionary: moran\n";
  const PreflightReport blocked =
      PreflightSchemaText("moran", yaml, Have({"moran.dict.yaml"}),
                          /*lua_supported=*/false);
  CHECK(!blocked.Ok());
  CHECK_INT(CountKind(blocked, PreflightKind::kLua), 1);
  CHECK_STR(blocked.Blocking().front().name,
            std::string("lua_translator@*moran_charset_filter"));

  // 哪一天真的編了 librime-lua,同一份 yaml 必須通過。
  const PreflightReport allowed =
      PreflightSchemaText("moran", yaml, Have({"moran.dict.yaml"}),
                          /*lua_supported=*/true);
  CHECK(allowed.Ok());
  CHECK_INT(CountKind(allowed, PreflightKind::kLua), 0);
}

TEST(Preflight_lua_detection_covers_all_four_component_kinds) {
  const char* kinds[] = {"lua_translator@*x", "lua_filter@*y",
                         "lua_processor@*z", "lua_segmentor@*w"};
  for (const char* k : kinds) {
    const std::string yaml = std::string("engine:\n  translators:\n    - ") + k + "\n";
    std::string first;
    CHECK(ReferencesLuaComponent(yaml, &first));
    CHECK_STR(first, std::string(k));
  }
  // 不含 lua 的方案不可以被誤判 —— 誤判會擋住 34 個套件裡的大多數。
  CHECK(!ReferencesLuaComponent(
      "engine:\n  translators:\n    - table_translator\n"
      "    - script_translator\n  filters:\n    - simplifier\n",
      nullptr));
  // 只是提到 lua 這個字(例如註解或檔名)不算。
  CHECK(!ReferencesLuaComponent("# 這個方案不用 lua\nname: lua_ish\n", nullptr));
}

TEST(Preflight_lua_package_that_ships_lua_files_but_declares_none_is_fine) {
  // 實測:radical-pinyin 與 zrm 帶 .lua 檔,但方案沒有宣告 lua_* 元件。
  // 整包擋掉會連同這些能用的方案一起殺掉,所以判斷在**方案**這一層。
  const std::string yaml =
      "schema:\n  schema_id: radical_pinyin\ntranslator:\n  dictionary: radical_pinyin\n";
  const PreflightReport r = PreflightSchemaText(
      "radical_pinyin", yaml, Have({"radical_pinyin.dict.yaml"}), false);
  CHECK(r.Ok());
}

/* ═════════════════ 實測釘住的那兩條(見標頭的量測表)═══════════════ */

TEST(Preflight_missing_include_is_blocking_because_it_is_a_silent_dud) {
  // ⚠ 這一條的理由**不是** Android 註解寫的「部署回 false」。
  //   實測(host rime_console,對照實驗只改 include 目標存不存在):
  //     目標在  → [deploy] SUCCESS,nihao 得到 5 個候選
  //     目標不在 → [deploy] SUCCESS,nihao 得到 0 個候選
  //   也就是與 lua 同一種症狀:部署成功、沒有錯誤、打不出字。
  //   所以它必須是 kBlocking,而且**測試要釘住的是這個結論**,
  //   免得有人照著那句寫錯的理由把它降級成警告。
  const std::string yaml =
      "schema:\n  schema_id: t\n"
      "translator:\n  dictionary: t\n"
      "punctuator:\n  __include: missing_file:/punctuator\n";
  const PreflightReport r = PreflightSchemaText("t", yaml, Have({"t.dict.yaml"}));
  CHECK(!r.Ok());
  CHECK(BlocksOn(r, "missing_file.yaml"));
}

TEST(Preflight_real_index_shape_sautungva_beta_is_correctly_blocked) {
  // 真索引裡唯一被非 lua 理由擋下的方案。實測它確實建不起來
  // (`unresolved dependency: Include(hangul:/key_to_hangul)`),
  // 所以擋它**不是**誤報 —— 這個案例存在是為了避免有人把它當成誤報而放行。
  const std::string yaml =
      "schema:\n  schema_id: sautungva_beta\r\n"
      "translator:\r\n  dictionary: sautungva_beta\r\n"
      "hangeul:\r\n    __include: hangul:/key_to_hangul\r\n";
  const PreflightReport r =
      PreflightSchemaText("sautungva_beta", yaml, Have({"sautungva_beta.dict.yaml"}));
  CHECK(!r.Ok());
  CHECK(BlocksOn(r, "hangul.yaml"));
  // 順帶釘住 CRLF:真的套件裡有一半是 CRLF 換行的。
  CHECK_STR(r.schema_id, std::string("sautungva_beta"));
}
