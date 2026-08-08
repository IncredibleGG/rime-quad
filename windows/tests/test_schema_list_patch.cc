// windows/tests/test_schema_list_patch.cc — 方案排序寫回 default.custom.yaml
//
// 這個檔案是**使用者資料**,而且寫壞的症狀特別惡劣:librime 一啟動就
// 部署失敗,方案清單變成空的,於是設定介面也救不回來。所以這一組測試
// 的重點不是「排序有沒有生效」,而是「認不出來的時候有沒有明著失敗」。

#include <string>

#include "../common/schema_list_patch.h"
#include "check.h"

using namespace rimewin;

namespace {

// 這是 scripts/collect_data.sh 真的產生出來的那一份(節錄但格式一致)。
const char* kShipped =
    "# 由 scripts/collect_data.sh 產生。\n"
    "#\n"
    "# 上游 rime-prelude 的 default.yaml 列出的方案多於本專案實際打包的,\n"
    "# 未打包的方案會在部署時報錯。\n"
    "patch:\n"
    "  schema_list:\n"
    "    - schema: luna_pinyin_tw    # 拼音(臺灣字形)\n"
    "    - schema: bopomofo_tw       # 注音(臺灣字形)\n"
    "    - schema: luna_pinyin       # 拼音(原版)\n"
    "    - schema: t9_pinyin         # 九宮格拼音\n";

}  // namespace

TEST(SchemaListPatch_reads_the_shipped_file) {
  bool found = false;
  const std::vector<std::string> got = ReadSchemaList(kShipped, &found);
  CHECK(found);
  CHECK_INT(got.size(), 4);
  CHECK_STR(got[0], "luna_pinyin_tw");
  CHECK_STR(got[3], "t9_pinyin");
}

TEST(SchemaListPatch_reorders_in_place) {
  std::string out;
  CHECK(WriteSchemaList(kShipped, {"luna_pinyin", "luna_pinyin_tw"}, &out) ==
        PatchResult::kOk);
  bool found = false;
  const std::vector<std::string> got = ReadSchemaList(out, &found);
  CHECK(found);
  CHECK_INT(got.size(), 2);
  CHECK_STR(got[0], "luna_pinyin");
  CHECK_STR(got[1], "luna_pinyin_tw");
}

TEST(SchemaListPatch_keeps_every_other_line_including_comments) {
  // yaml-cpp 會把註解全部吃掉,而那個檔案裡有整段解釋它為什麼存在。
  // 使用者按一次「上移」就把它清空,是很難解釋的行為。
  std::string out;
  CHECK(WriteSchemaList(kShipped, {"luna_pinyin"}, &out) == PatchResult::kOk);
  CHECK(out.find("由 scripts/collect_data.sh 產生") != std::string::npos);
  CHECK(out.find("未打包的方案會在部署時報錯") != std::string::npos);
  CHECK(out.find("patch:") != std::string::npos);
}

TEST(SchemaListPatch_keeps_sibling_keys_after_the_list) {
  const std::string in =
      "patch:\n"
      "  schema_list:\n"
      "    - schema: a\n"
      "    - schema: b\n"
      "  menu/page_size: 9\n"
      "  \"speller/auto_select\": true\n";
  std::string out;
  CHECK(WriteSchemaList(in, {"b", "a"}, &out) == PatchResult::kOk);
  CHECK(out.find("menu/page_size: 9") != std::string::npos);
  CHECK(out.find("speller/auto_select") != std::string::npos);
  bool found = false;
  const std::vector<std::string> got = ReadSchemaList(out, &found);
  CHECK_INT(got.size(), 2);
  CHECK_STR(got[0], "b");
}

TEST(SchemaListPatch_roundtrip_is_idempotent) {
  std::string a, b;
  CHECK(WriteSchemaList(kShipped, {"luna_pinyin", "bopomofo_tw"}, &a) ==
        PatchResult::kOk);
  CHECK(WriteSchemaList(a, {"luna_pinyin", "bopomofo_tw"}, &b) ==
        PatchResult::kOk);
  CHECK_STR(a, b);
}

TEST(SchemaListPatch_adds_the_section_when_patch_exists_without_it) {
  const std::string in = "patch:\n  menu/page_size: 9\n";
  std::string out;
  CHECK(WriteSchemaList(in, {"luna_pinyin"}, &out) == PatchResult::kOk);
  bool found = false;
  CHECK_INT(ReadSchemaList(out, &found).size(), 1);
  CHECK(found);
  CHECK(out.find("menu/page_size: 9") != std::string::npos);
}

TEST(SchemaListPatch_refuses_when_it_cannot_understand_the_file) {
  // ⚠ 這一組才是重點。認不出來就**明著失敗**,由呼叫端告訴使用者
  //   「你的 default.custom.yaml 我看不懂」。安靜地重寫整個檔案
  //   是最糟的選項:使用者的其他設定會無聲消失。
  std::string out;
  CHECK(WriteSchemaList("", {"a"}, &out) == PatchResult::kNoPatchSection);
  CHECK(WriteSchemaList("# 只有註解\n", {"a"}, &out) ==
        PatchResult::kNoPatchSection);
  CHECK(WriteSchemaList("something_else:\n  x: 1\n", {"a"}, &out) ==
        PatchResult::kNoPatchSection);
  // tab 縮排:YAML 規範根本不允許,而我們的縮排判斷會把它算錯 ——
  // 算錯的結果是把別人的設定吃掉。
  CHECK(WriteSchemaList("patch:\n\tschema_list:\n\t  - schema: a\n", {"b"},
                        &out) == PatchResult::kNoPatchSection);
}

TEST(SchemaListPatch_refuses_an_empty_order) {
  // schema_list 空掉 = 一個方案都沒有 = 輸入法完全不能用,
  // 而且沒有自救途徑(設定介面的清單也是空的)。
  std::string out;
  CHECK(WriteSchemaList(kShipped, {}, &out) == PatchResult::kEmptyOrder);
}

TEST(SchemaListPatch_refuses_a_hostile_schema_id) {
  // id 有一部分來自下載回來的市集索引,而它會被寫進一個 librime 會
  // 照著去找檔案的 YAML。
  std::string out;
  CHECK(WriteSchemaList(kShipped, {"../../evil"}, &out) ==
        PatchResult::kBadSchemaId);
  CHECK(WriteSchemaList(kShipped, {"a\nb: c"}, &out) == PatchResult::kBadSchemaId);
  CHECK(WriteSchemaList(kShipped, {""}, &out) == PatchResult::kBadSchemaId);
  CHECK(WriteSchemaList(kShipped, {"ok", "not ok"}, &out) ==
        PatchResult::kBadSchemaId);
  CHECK(!IsPlausibleSchemaId(".hidden"));
  CHECK(!IsPlausibleSchemaId("a/b"));
  CHECK(IsPlausibleSchemaId("luna_pinyin_tw"));
  CHECK(IsPlausibleSchemaId("double-pinyin.flypy"));
}

TEST(SchemaListPatch_read_distinguishes_missing_from_empty) {
  bool found = true;
  const std::vector<std::string> none = ReadSchemaList("patch:\n  x: 1\n", &found);
  CHECK(!found);
  CHECK_INT(none.size(), 0);
  found = false;
  const std::vector<std::string> empty =
      ReadSchemaList("patch:\n  schema_list:\n  x: 1\n", &found);
  CHECK(found);
  CHECK_INT(empty.size(), 0);
}
