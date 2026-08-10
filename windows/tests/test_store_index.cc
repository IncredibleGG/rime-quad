// windows/tests/test_store_index.cc — 索引解析與相依展開
//
// ⚠ 兩件事在這裡是**規範**,不是實作偏好,兩端都要有案例守著:
//   1. 單一套件壞掉不可以弄垮整份索引(伺服器產生的東西,桌面端修不了);
//   2. 循環相依是**合法的**。真索引裡 luna-pinyin 與 stroke 互相 requires,
//      把它當錯誤會讓這兩個最常用的套件永遠裝不了。

#include <string>
#include <vector>

#include "../common/store_index.h"
#include "check.h"

using namespace rimewin;

namespace {

const char kSha[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

std::string Pkg(const std::string& id, const std::string& extra = "") {
  return "{\"id\":\"" + id + "\",\"name\":\"" + id + "\",\"file\":\"" + id +
         ".zip\",\"size\":100,\"sha256\":\"" + kSha + "\"" +
         (extra.empty() ? "" : "," + extra) + "}";
}

std::string Index(const std::string& packages, int version = 1) {
  return "{\"format_version\":" + std::to_string(version) +
         ",\"base_url\":\"https://example.invalid/s/\",\"packages\":[" + packages +
         "]}";
}

bool Has(const std::vector<std::string>& v, const std::string& x) {
  for (const auto& s : v) {
    if (s == x) return true;
  }
  return false;
}

}  // namespace

TEST(StoreIndex_parses_a_normal_index) {
  const IndexParseResult r = ParseStoreIndex(Index(
      Pkg("luna-pinyin",
          "\"schemas\":[{\"id\":\"luna_pinyin\",\"name\":\"朙月拼音\","
          "\"language\":\"zh-Hant\"}],\"requires\":[\"prelude\"],"
          "\"recommended\":true,\"verified\":{\"deployed\":true}") +
      "," + Pkg("prelude")));
  CHECK(r.ok);
  CHECK_INT(static_cast<int>(r.index.packages.size()), 2);
  const StorePackage* p = r.index.ById("luna-pinyin");
  CHECK(p != nullptr);
  CHECK_INT(static_cast<int>(p->schemas.size()), 1);
  CHECK_STR(p->schemas[0].language, std::string("zh-Hant"));
  CHECK(p->recommended);
  CHECK(p->verified_deployed);
  CHECK(r.index.ProvidingSchema("luna_pinyin") == p);
  CHECK(r.index.ProvidingSchema("nope") == nullptr);
}

TEST(StoreIndex_rejects_a_format_version_it_does_not_understand) {
  // 唯一整份拒收的情形:欄位語意變了,硬讀下去只會產生沉默的錯誤行為。
  const IndexParseResult r = ParseStoreIndex(Index(Pkg("a"), 2));
  CHECK(!r.ok);
  CHECK(r.error.find("format_version") != std::string::npos);
}

TEST(StoreIndex_one_bad_package_does_not_kill_the_index) {
  const IndexParseResult r = ParseStoreIndex(Index(
      "{\"id\":\"no-sha\",\"file\":\"a.zip\"}," +
      Pkg("good") + ",{\"file\":\"b.zip\",\"sha256\":\"" + kSha + "\"}"));
  CHECK(r.ok);
  CHECK_INT(static_cast<int>(r.index.packages.size()), 1);
  CHECK_STR(r.index.packages[0].id, std::string("good"));
  CHECK_INT(static_cast<int>(r.warnings.size()), 2);
}

TEST(StoreIndex_drops_a_package_with_no_or_bad_sha256) {
  // 規範:必須驗 sha256。沒有它就沒有辦法驗,不是「先裝了再說」。
  const IndexParseResult a =
      ParseStoreIndex(Index("{\"id\":\"x\",\"file\":\"x.zip\"}"));
  CHECK(!a.ok);   // 一個能用的套件都不剩
  const IndexParseResult b = ParseStoreIndex(
      Index("{\"id\":\"x\",\"file\":\"x.zip\",\"sha256\":\"tooshort\"}"));
  CHECK(!b.ok);
}

TEST(StoreIndex_rejects_an_id_we_would_not_write_to_a_path) {
  // id 從網路來,而它會被寫進 default.custom.yaml 與檔案路徑。
  const IndexParseResult r = ParseStoreIndex(Index(
      "{\"id\":\"../evil\",\"file\":\"a.zip\",\"sha256\":\"" + std::string(kSha) +
      "\"}," + Pkg("ok")));
  CHECK(r.ok);
  CHECK_INT(static_cast<int>(r.index.packages.size()), 1);
  CHECK_STR(r.index.packages[0].id, std::string("ok"));
}

TEST(StoreIndex_duplicate_ids_keep_the_first) {
  const IndexParseResult r = ParseStoreIndex(
      Index(Pkg("dup", "\"description\":\"first\"") + "," +
            Pkg("dup", "\"description\":\"second\"")));
  CHECK(r.ok);
  CHECK_INT(static_cast<int>(r.index.packages.size()), 1);
  CHECK_STR(r.index.packages[0].description, std::string("first"));
}

TEST(StoreIndex_unknown_category_falls_back_to_other) {
  const std::string text =
      "{\"format_version\":1,\"categories\":[{\"id\":\"mandarin\",\"order\":1},"
      "{\"id\":\"other\",\"order\":9},{\"id\":\"essential\",\"hidden\":true}],"
      "\"packages\":[" + Pkg("a", "\"category\":\"nope\"") + "]}";
  const IndexParseResult r = ParseStoreIndex(text);
  CHECK(r.ok);
  CHECK_STR(r.index.packages[0].category, std::string("other"));
  CHECK_INT(static_cast<int>(r.index.VisibleCategories().size()), 2);
}

TEST(StoreIndex_garbage_is_an_error_not_a_crash) {
  CHECK(!ParseStoreIndex("").ok);
  CHECK(!ParseStoreIndex("not json").ok);
  CHECK(!ParseStoreIndex("[]").ok);
  CHECK(!ParseStoreIndex("{}").ok);
  CHECK(!ParseStoreIndex("{\"format_version\":1,\"packages\":[]}").ok);
}

/* ═══════════════════════ 相依展開 ═══════════════════════════════ */

TEST(StoreDeps_expands_transitively_and_puts_dependencies_first) {
  const IndexParseResult r = ParseStoreIndex(Index(
      Pkg("array", "\"requires\":[\"luna-pinyin\",\"prelude\"]") + "," +
      Pkg("luna-pinyin", "\"requires\":[\"prelude\"]") + "," + Pkg("prelude")));
  CHECK(r.ok);
  const DependencyResult d = ResolveDependencies(r.index, {"array"}, {});
  CHECK(d.status == DependencyResult::Status::kOk);
  CHECK_INT(static_cast<int>(d.plan.to_download.size()), 3);
  CHECK_STR(d.plan.to_download.back(), std::string("array"));
  CHECK_STR(d.plan.to_download.front(), std::string("prelude"));
  CHECK_INT(d.plan.total_bytes, 300);
}

TEST(StoreDeps_a_cycle_is_legal_and_both_get_downloaded) {
  // ⚠ 真索引就是這個形狀:luna-pinyin 與 stroke 互為反查詞庫。
  //   把循環當錯誤 = 這兩個套件永遠裝不了。
  const IndexParseResult r = ParseStoreIndex(
      Index(Pkg("luna-pinyin", "\"requires\":[\"stroke\"]") + "," +
            Pkg("stroke", "\"requires\":[\"luna-pinyin\"]")));
  CHECK(r.ok);
  const DependencyResult d = ResolveDependencies(r.index, {"luna-pinyin"}, {});
  CHECK(d.status == DependencyResult::Status::kOk);
  CHECK_INT(static_cast<int>(d.plan.to_download.size()), 2);
  CHECK(Has(d.plan.to_download, "luna-pinyin"));
  CHECK(Has(d.plan.to_download, "stroke"));
  CHECK_INT(static_cast<int>(d.plan.cycles.size()), 1);
}

TEST(StoreDeps_already_installed_is_subtracted) {
  const IndexParseResult r = ParseStoreIndex(
      Index(Pkg("array", "\"requires\":[\"prelude\"]") + "," + Pkg("prelude")));
  const DependencyResult d = ResolveDependencies(r.index, {"array"}, {"prelude"});
  CHECK(d.status == DependencyResult::Status::kOk);
  CHECK_INT(static_cast<int>(d.plan.to_download.size()), 1);
  CHECK_STR(d.plan.to_download[0], std::string("array"));
  CHECK_INT(static_cast<int>(d.plan.already_installed.size()), 1);
}

TEST(StoreDeps_a_dependency_that_is_not_in_the_index_names_who_wanted_it) {
  const IndexParseResult r =
      ParseStoreIndex(Index(Pkg("array", "\"requires\":[\"ghost\"]")));
  const DependencyResult d = ResolveDependencies(r.index, {"array"}, {});
  CHECK(d.status == DependencyResult::Status::kMissingDependency);
  CHECK_STR(d.missing, std::string("ghost"));
  CHECK_STR(d.required_by, std::string("array"));
  // 失敗時不留半個計畫。
  CHECK_INT(static_cast<int>(d.plan.to_download.size()), 0);
}

TEST(StoreDeps_selecting_an_unknown_package_is_a_different_failure) {
  const IndexParseResult r = ParseStoreIndex(Index(Pkg("a")));
  const DependencyResult d = ResolveDependencies(r.index, {"nope"}, {});
  CHECK(d.status == DependencyResult::Status::kUnknownPackage);
  CHECK_STR(d.missing, std::string("nope"));
}

TEST(StoreDeps_dependents_of_answers_the_uninstall_question) {
  const IndexParseResult r = ParseStoreIndex(
      Index(Pkg("array", "\"requires\":[\"prelude\"]") + "," +
            Pkg("cangjie", "\"requires\":[\"prelude\"]") + "," + Pkg("prelude")));
  const std::vector<std::string> deps =
      DependentsOf(r.index, "prelude", {"array", "cangjie", "prelude"});
  CHECK_INT(static_cast<int>(deps.size()), 2);
}

TEST(StoreDeps_estimated_size_is_not_the_zip_size) {
  // 索引的 size 是 zip 大小,嚴重低估實際佔用(luna-pinyin:0.4MB → 13MB)。
  DependencyPlan plan;
  plan.total_bytes = 1024 * 1024;
  CHECK(plan.EstimatedInstalledBytes() > plan.total_bytes * 10);
}

TEST(StoreIndex_format_bytes) {
  CHECK_STR(FormatBytes(0), std::string("0 B"));
  CHECK_STR(FormatBytes(2048), std::string("2 KB"));
  CHECK_STR(FormatBytes(1713490), std::string("1.6 MB"));
  CHECK_STR(FormatBytes(2LL * 1024 * 1024 * 1024), std::string("2.00 GB"));
}
