// windows/tests/test_store_engine.cc — 市集的整條流程
//
// ⚠ 這個檔案要守的是**失敗時使用者看到什麼**(待辦 #62:三種不同的失敗
//   在畫面上是同一句紅字)。所以幾乎每一個案例都在斷言 StoreFailure 的
//   種類,而不只是「有沒有失敗」。
//
// ⚠ 另外兩條是規範:
//   · 連網開關關著時,**一個位元組都不會出去**(fake 會數 Fetch 被叫幾次);
//   · 預檢擋下時,**default.custom.yaml 一個位元組都沒被動過**
//     —— 那是「不需要回滾」這句話的兌現處。

#include <map>
#include <set>
#include <string>
#include <vector>

#include "../common/sha256.h"
#include "../common/store_engine.h"
#include "check.h"
#include "zip_build.h"

using namespace rimewin;

namespace {

const char kSha[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

/* ── 三個假的副作用 ────────────────────────────────────────── */

class FakeNet : public StoreNetwork {
 public:
  bool blocked = false;          // 連網開關關著
  bool fail = false;             // 連得上但失敗
  int calls = 0;                 // **真的被呼叫幾次**
  int block_after = -1;          // >=0 = 第幾次之後開始擋(模擬中途關掉開關)
  std::map<std::string, std::string> bodies;

  Result Fetch(const std::string& url, bool is_package, const std::string& label,
               int64_t max_bytes) override {
    (void)is_package;
    (void)label;
    (void)max_bytes;
    Result r;
    if (blocked || (block_after >= 0 && calls >= block_after)) {
      // ⚠ **不增加 calls**:被開關擋下不是一次連線。
      r.blocked = true;
      r.message = "the network switch is off";
      return r;
    }
    ++calls;
    if (fail) {
      r.message = "could not reach the host";
      return r;
    }
    auto it = bodies.find(url);
    if (it == bodies.end()) {
      r.message = "HTTP 404";
      return r;
    }
    r.ok = true;
    r.body = it->second;
    return r;
  }
};

class FakeFs : public StoreFileSystem {
 public:
  std::map<std::string, std::string> user;    // 使用者資料目錄
  std::set<std::string> shared;               // 隨安裝檔出貨的
  std::map<std::string, std::string> shared_text;
  std::set<std::string> write_fails;          // 這些路徑寫不進去
  int fail_write_from = -1;                   // >=0 = 第幾次寫入開始失敗
  int writes = 0;

  bool DataFileExists(const std::string& name) override {
    return user.count(name) != 0 || shared.count(name) != 0;
  }
  bool ReadDataFile(const std::string& name, std::string* out) override {
    auto it = user.find(name);
    if (it != user.end()) { *out = it->second; return true; }
    auto s = shared_text.find(name);
    if (s != shared_text.end()) { *out = s->second; return true; }
    return false;
  }
  bool WriteUserFile(const std::string& rel, const std::string& bytes) override {
    ++writes;
    if (fail_write_from >= 0 && writes > fail_write_from) return false;
    if (write_fails.count(rel) != 0) return false;
    user[rel] = bytes;
    return true;
  }
  bool RemoveUserFile(const std::string& rel) override {
    return user.erase(rel) > 0;
  }
  bool ReadUserFile(const std::string& rel, std::string* out) override {
    auto it = user.find(rel);
    if (it == user.end()) return false;
    *out = it->second;
    return true;
  }
};

class FakeDeployer : public StoreDeployer {
 public:
  bool started = true;
  bool ok = true;
  std::string last_error;
  int calls = 0;

  Outcome DeployAndWait(const StoreProgressFn& progress) override {
    (void)progress;
    ++calls;
    Outcome o;
    o.started = started;
    o.ok = ok;
    o.elapsed_ms = 42;
    o.last_error = last_error;
    return o;
  }
};

struct Harness {
  FakeNet net;
  FakeFs fs;
  FakeDeployer deployer;

  StoreDeps Deps() {
    StoreDeps d;
    d.net = &net;
    d.fs = &fs;
    d.deployer = &deployer;
    d.now_ms = []() { return static_cast<int64_t>(1770000000000LL); };
    return d;
  }
};

// 一份最小可用的 default.custom.yaml。
const char kCustomYaml[] =
    "# LuminaKey\npatch:\n  schema_list:\n    - schema: luna_pinyin\n";

std::string SamplePackageZip() {
  return rimewin_test::BuildZip({
      rimewin_test::StoredEntry(
          "array30.schema.yaml",
          "schema:\n  schema_id: array30\ntranslator:\n  dictionary: array30\n"),
      rimewin_test::StoredEntry("array30.dict.yaml", "---\nname: array30\n"),
  });
}

std::string IndexJson(const std::string& sha, int64_t size) {
  return std::string("{\"format_version\":1,\"base_url\":\"https://ex.invalid/s/\","
                     "\"packages\":[{\"id\":\"array\",\"name\":\"array 30\","
                     "\"file\":\"array.zip\",\"size\":") +
         std::to_string(size) + ",\"sha256\":\"" + sha +
         "\",\"schemas\":[{\"id\":\"array30\",\"name\":\"array30\"}]}]}";
}

const char kIndexUrl[] = "https://ex.invalid/s/index.json";
const char kPkgUrl[] = "https://ex.invalid/s/array.zip";

}  // namespace

/* ═══════════════════════ 取索引 ═════════════════════════════════ */

TEST(Store_index_fetch_blocked_by_the_switch_is_its_own_failure) {
  Harness h;
  h.net.blocked = true;
  StoreIndex idx;
  const StoreOutcome o = FetchStoreIndex(h.Deps(), kIndexUrl, &idx, nullptr);
  CHECK(!o.ok);
  CHECK(o.failure == StoreFailure::kSwitchOff);
  // ⚠ 開關關著時,連線函式一次都沒有真的送出去。
  CHECK_INT(h.net.calls, 0);
}

TEST(Store_index_network_error_is_not_the_same_as_switch_off) {
  Harness h;
  h.net.fail = true;
  StoreIndex idx;
  const StoreOutcome o = FetchStoreIndex(h.Deps(), kIndexUrl, &idx, nullptr);
  CHECK(!o.ok);
  CHECK(o.failure == StoreFailure::kNetwork);
  CHECK_INT(h.net.calls, 1);   // 這一次是真的連了
}

TEST(Store_index_bad_json_is_its_own_failure) {
  Harness h;
  h.net.bodies[kIndexUrl] = "{not json";
  StoreIndex idx;
  const StoreOutcome o = FetchStoreIndex(h.Deps(), kIndexUrl, &idx, nullptr);
  CHECK(!o.ok);
  CHECK(o.failure == StoreFailure::kIndex);
}

TEST(Store_index_ok) {
  Harness h;
  const std::string zip = SamplePackageZip();
  h.net.bodies[kIndexUrl] = IndexJson(Sha256::HexOf(zip), zip.size());
  StoreIndex idx;
  std::vector<std::string> warnings;
  const StoreOutcome o = FetchStoreIndex(h.Deps(), kIndexUrl, &idx, &warnings);
  CHECK(o.ok);
  CHECK_INT(static_cast<int>(idx.packages.size()), 1);
  CHECK_STR(idx.base_url, std::string("https://ex.invalid/s/"));
}

/* ═══════════════════════ 安裝 ═══════════════════════════════════ */

namespace {

// 把「索引 + 套件」都準備好的 harness。
struct Installed {
  Harness h;
  StoreIndex index;
  DependencyPlan plan;
  std::vector<InstalledPackage> registry;
};

Installed Ready(const std::string& zip) {
  Installed s;
  s.h.net.bodies[kIndexUrl] = IndexJson(Sha256::HexOf(zip), zip.size());
  s.h.net.bodies[kPkgUrl] = zip;
  std::vector<std::string> w;
  FetchStoreIndex(s.h.Deps(), kIndexUrl, &s.index, &w);
  const DependencyResult d = ResolveDependencies(s.index, {"array"}, {});
  s.plan = d.plan;
  return s;
}

}  // namespace

TEST(Store_install_writes_the_files_and_records_them) {
  Installed s = Ready(SamplePackageZip());
  const StoreOutcome o = InstallPackages(s.h.Deps(), kIndexUrl, s.index, s.plan,
                                         &s.registry, nullptr);
  CHECK(o.ok);
  CHECK_INT(static_cast<int>(s.h.fs.user.size()), 2);
  CHECK(s.h.fs.user.count("array30.schema.yaml") == 1);
  CHECK(s.h.fs.user.count("array30.dict.yaml") == 1);
  CHECK_INT(static_cast<int>(s.registry.size()), 1);
  CHECK_STR(s.registry[0].id, std::string("array"));
  CHECK_INT(static_cast<int>(s.registry[0].files.size()), 2);
  CHECK_INT(static_cast<int>(s.registry[0].schemas.size()), 1);
}

TEST(Store_install_rejects_a_sha256_mismatch) {
  Installed s = Ready(SamplePackageZip());
  // 索引說的是別的雜湊。
  s.index.packages[0].sha256 = kSha;
  const StoreOutcome o = InstallPackages(s.h.Deps(), kIndexUrl, s.index, s.plan,
                                         &s.registry, nullptr);
  CHECK(!o.ok);
  CHECK(o.failure == StoreFailure::kIntegrity);
  // 一個檔案都不可以落地。
  CHECK_INT(static_cast<int>(s.h.fs.user.size()), 0);
  CHECK_INT(static_cast<int>(s.registry.size()), 0);
}

TEST(Store_install_rejects_an_unsafe_archive) {
  const std::string zip = rimewin_test::BuildZip({
      rimewin_test::StoredEntry("../../evil.yaml", "x"),
      rimewin_test::StoredEntry("ok.yaml", "y"),
  });
  Installed s = Ready(zip);
  const StoreOutcome o = InstallPackages(s.h.Deps(), kIndexUrl, s.index, s.plan,
                                         &s.registry, nullptr);
  CHECK(!o.ok);
  CHECK(o.failure == StoreFailure::kArchive);
  // ⚠ **整包拒絕**:同一包裡無害的那個 entry 也不可以落地。
  CHECK_INT(static_cast<int>(s.h.fs.user.size()), 0);
}

TEST(Store_install_rejects_an_executable_in_the_package) {
  const std::string zip = rimewin_test::BuildZip({
      rimewin_test::StoredEntry("a.yaml", "x"),
      rimewin_test::StoredEntry("payload.exe", "MZ"),
  });
  Installed s = Ready(zip);
  const StoreOutcome o = InstallPackages(s.h.Deps(), kIndexUrl, s.index, s.plan,
                                         &s.registry, nullptr);
  CHECK(!o.ok);
  CHECK(o.failure == StoreFailure::kArchive);
  CHECK_INT(static_cast<int>(s.h.fs.user.size()), 0);
}

TEST(Store_install_disk_failure_is_its_own_failure) {
  Installed s = Ready(SamplePackageZip());
  s.h.fs.write_fails.insert("array30.dict.yaml");
  const StoreOutcome o = InstallPackages(s.h.Deps(), kIndexUrl, s.index, s.plan,
                                         &s.registry, nullptr);
  CHECK(!o.ok);
  CHECK(o.failure == StoreFailure::kDisk);
}

TEST(Store_install_switch_turned_off_midway_stops_immediately) {
  // 使用者在下載進行到一半時把開關關掉。每一跳都要重問,
  // 而後面的套件連一個位元組都不可以出去。
  Installed s = Ready(SamplePackageZip());
  s.h.net.block_after = 0;   // 索引已經取過了,下一次(套件)開始擋
  const StoreOutcome o = InstallPackages(s.h.Deps(), kIndexUrl, s.index, s.plan,
                                         &s.registry, nullptr);
  CHECK(!o.ok);
  CHECK(o.failure == StoreFailure::kSwitchOff);
  CHECK_INT(static_cast<int>(s.h.fs.user.size()), 0);
}

TEST(Store_install_is_idempotent_on_the_registry) {
  Installed s = Ready(SamplePackageZip());
  InstallPackages(s.h.Deps(), kIndexUrl, s.index, s.plan, &s.registry, nullptr);
  InstallPackages(s.h.Deps(), kIndexUrl, s.index, s.plan, &s.registry, nullptr);
  CHECK_INT(static_cast<int>(s.registry.size()), 1);
}

/* ═══════════════════════ 啟用 ═══════════════════════════════════ */

TEST(Store_enable_blocked_by_preflight_never_touches_the_yaml) {
  // ⚠ 這是「預檢在改檔案之前跑,所以不需要回滾」那句話的兌現處。
  Harness h;
  h.fs.user["default.custom.yaml"] = kCustomYaml;
  h.fs.user["array30.schema.yaml"] =
      "schema:\n  schema_id: array30\ntranslator:\n  dictionary: array30\n";
  // array30.dict.yaml 不在。
  const StoreOutcome o = EnableSchemas(h.Deps(), {"array30"}, nullptr);
  CHECK(!o.ok);
  CHECK(o.failure == StoreFailure::kPreflight);
  CHECK_STR(o.subject, std::string("array30"));
  // 指名缺哪一本詞典 —— 那是規範 §4 的硬性要求。
  CHECK(o.detail.find("array30.dict.yaml") != std::string::npos);
  CHECK_STR(h.fs.user["default.custom.yaml"], std::string(kCustomYaml));
  CHECK(!o.rolled_back);          // 不是「回滾了」,是根本沒動
  CHECK_INT(h.deployer.calls, 0); // 也沒有白跑一次部署
}

TEST(Store_enable_blocked_by_lua_says_so_specifically) {
  Harness h;
  h.fs.user["default.custom.yaml"] = kCustomYaml;
  h.fs.user["moran.schema.yaml"] =
      "schema:\n  schema_id: moran\n"
      "engine:\n  translators:\n    - lua_translator@*moran\n"
      "translator:\n  dictionary: moran\n";
  h.fs.user["moran.dict.yaml"] = "---\n";
  const StoreOutcome o = EnableSchemas(h.Deps(), {"moran"}, nullptr);
  CHECK(!o.ok);
  CHECK(o.failure == StoreFailure::kPreflight);
  CHECK(o.detail.find("librime-lua") != std::string::npos);
  CHECK_STR(h.fs.user["default.custom.yaml"], std::string(kCustomYaml));
}

TEST(Store_enable_reads_the_schema_from_the_shared_dir_too) {
  // 內建方案住在 shared,不在使用者目錄裡。SchemaPreflight 對內建方案
  // 誤報缺檔正是待辦 #59 那個事故。
  Harness h;
  h.fs.user["default.custom.yaml"] = kCustomYaml;
  h.fs.shared_text["bopomofo.schema.yaml"] =
      "schema:\n  schema_id: bopomofo\ntranslator:\n  dictionary: bopomofo\n";
  h.fs.shared.insert("bopomofo.schema.yaml");
  h.fs.shared.insert("bopomofo.dict.yaml");
  const StoreOutcome o = EnableSchemas(h.Deps(), {"bopomofo"}, nullptr);
  CHECK(o.ok);
}

TEST(Store_enable_appends_to_the_schema_list_and_deploys) {
  Harness h;
  h.fs.user["default.custom.yaml"] = kCustomYaml;
  h.fs.user["array30.schema.yaml"] =
      "schema:\n  schema_id: array30\ntranslator:\n  dictionary: array30\n";
  h.fs.user["array30.dict.yaml"] = "---\n";
  const StoreOutcome o = EnableSchemas(h.Deps(), {"array30"}, nullptr);
  CHECK(o.ok);
  CHECK_INT(h.deployer.calls, 1);
  const std::string yaml = h.fs.user["default.custom.yaml"];
  CHECK(yaml.find("array30") != std::string::npos);
  CHECK(yaml.find("luna_pinyin") != std::string::npos);   // 原本的不可以消失
  CHECK(yaml.find("# LuminaKey") != std::string::npos);   // 註解也不可以消失
}

TEST(Store_enable_deploy_failure_rolls_the_yaml_back) {
  Harness h;
  h.fs.user["default.custom.yaml"] = kCustomYaml;
  h.fs.user["array30.schema.yaml"] =
      "schema:\n  schema_id: array30\ntranslator:\n  dictionary: array30\n";
  h.fs.user["array30.dict.yaml"] = "---\n";
  h.deployer.ok = false;
  h.deployer.last_error = "failed to build array30.dict.yaml";
  const StoreOutcome o = EnableSchemas(h.Deps(), {"array30"}, nullptr);
  CHECK(!o.ok);
  CHECK(o.failure == StoreFailure::kDeploy);
  CHECK(o.rolled_back);
  CHECK_STR(h.fs.user["default.custom.yaml"], std::string(kCustomYaml));
  CHECK(o.detail.find("array30.dict.yaml") != std::string::npos);
}

TEST(Store_enable_rollback_failure_is_its_own_failure) {
  // 部署失敗**而且**改回去也失敗 —— 使用者現在有一份會讓部署失敗的設定,
  // 而我們改不回去。那必須是一句不同的話,因為它需要人工介入。
  Harness h;
  h.fs.user["default.custom.yaml"] = kCustomYaml;
  h.fs.user["array30.schema.yaml"] =
      "schema:\n  schema_id: array30\ntranslator:\n  dictionary: array30\n";
  h.fs.user["array30.dict.yaml"] = "---\n";
  h.deployer.ok = false;
  h.fs.fail_write_from = 1;   // 第一次(加進去)成功,第二次(改回來)失敗
  const StoreOutcome o = EnableSchemas(h.Deps(), {"array30"}, nullptr);
  CHECK(!o.ok);
  CHECK(o.failure == StoreFailure::kRollbackFailed);
  CHECK(!o.rolled_back);
  // 檔案還停在「加進去了」的狀態,而我們有說出來。
  CHECK(h.fs.user["default.custom.yaml"].find("array30") != std::string::npos);
}

TEST(Store_enable_with_the_engine_not_running_does_not_roll_back) {
  // 檔案已經寫好了,下次啟動就會生效 —— 所以不回滾,但也不可以說成功。
  Harness h;
  h.fs.user["default.custom.yaml"] = kCustomYaml;
  h.fs.user["array30.schema.yaml"] =
      "schema:\n  schema_id: array30\ntranslator:\n  dictionary: array30\n";
  h.fs.user["array30.dict.yaml"] = "---\n";
  h.deployer.started = false;
  const StoreOutcome o = EnableSchemas(h.Deps(), {"array30"}, nullptr);
  CHECK(!o.ok);
  CHECK(o.failure == StoreFailure::kDeploy);
  CHECK(!o.rolled_back);
  CHECK(h.fs.user["default.custom.yaml"].find("array30") != std::string::npos);
}

TEST(Store_enable_unreadable_custom_yaml_is_a_patch_failure) {
  Harness h;
  h.fs.user["array30.schema.yaml"] =
      "schema:\n  schema_id: array30\ntranslator:\n  dictionary: array30\n";
  h.fs.user["array30.dict.yaml"] = "---\n";
  // default.custom.yaml 根本不存在。
  const StoreOutcome o = EnableSchemas(h.Deps(), {"array30"}, nullptr);
  CHECK(!o.ok);
  CHECK(o.failure == StoreFailure::kPatch);
}

TEST(Store_enable_carries_warnings_without_blocking) {
  Harness h;
  h.fs.user["default.custom.yaml"] = kCustomYaml;
  h.fs.user["cangjie5.schema.yaml"] =
      "schema:\n  schema_id: cangjie5\n"
      "translator:\n  dictionary: cangjie5\n"
      "reverse_lookup:\n  dictionary: luna_quanpin\n";
  h.fs.user["cangjie5.dict.yaml"] = "---\n";
  const StoreOutcome o = EnableSchemas(h.Deps(), {"cangjie5"}, nullptr);
  CHECK(o.ok);
  CHECK_INT(static_cast<int>(o.notes.size()), 1);
  CHECK(o.notes[0].find("luna_quanpin") != std::string::npos);
}

/* ═══════════════════════ 移除 ═══════════════════════════════════ */

TEST(Store_uninstall_removes_files_and_schema_list_entries) {
  Harness h;
  h.fs.user["default.custom.yaml"] =
      "patch:\n  schema_list:\n    - schema: luna_pinyin\n    - schema: array30\n";
  h.fs.user["array30.schema.yaml"] = "x";
  h.fs.user["array30.dict.yaml"] = "y";
  std::vector<InstalledPackage> reg;
  InstalledPackage p;
  p.id = "array";
  p.schemas = {"array30"};
  p.files = {"array30.schema.yaml", "array30.dict.yaml"};
  reg.push_back(p);

  const StoreOutcome o = UninstallPackage(h.Deps(), "array", &reg, nullptr);
  CHECK(o.ok);
  CHECK_INT(static_cast<int>(reg.size()), 0);
  CHECK(h.fs.user.count("array30.schema.yaml") == 0);
  CHECK(h.fs.user["default.custom.yaml"].find("array30") == std::string::npos);
  CHECK(h.fs.user["default.custom.yaml"].find("luna_pinyin") != std::string::npos);
}

TEST(Store_uninstall_refuses_to_empty_the_schema_list) {
  // 空的 schema_list = 一個方案都沒有 = 使用者連設定介面都救不回來。
  Harness h;
  h.fs.user["default.custom.yaml"] =
      "patch:\n  schema_list:\n    - schema: array30\n";
  std::vector<InstalledPackage> reg;
  InstalledPackage p;
  p.id = "array";
  p.schemas = {"array30"};
  reg.push_back(p);
  const StoreOutcome o = UninstallPackage(h.Deps(), "array", &reg, nullptr);
  CHECK(!o.ok);
  CHECK(o.failure == StoreFailure::kPatch);
  CHECK_INT(static_cast<int>(reg.size()), 1);   // 沒有真的移除
}

TEST(Store_uninstall_of_something_we_never_installed) {
  Harness h;
  h.fs.user["default.custom.yaml"] = kCustomYaml;
  std::vector<InstalledPackage> reg;
  const StoreOutcome o = UninstallPackage(h.Deps(), "ghost", &reg, nullptr);
  CHECK(!o.ok);
  CHECK(o.failure == StoreFailure::kUnknownPackage);
}

/* ═══════════════════════ 安裝紀錄 ═══════════════════════════════ */

TEST(Store_installed_record_round_trips) {
  InstalledPackage p;
  p.id = "array";
  p.upstream_commit = "557dbe3";
  p.sha256 = kSha;
  p.installed_at_ms = 1770000000000LL;
  p.schemas = {"array30", "array30_query"};
  p.files = {"array30.schema.yaml", "array30.dict.yaml"};
  const std::string line = EncodeInstalledLine(p);
  InstalledPackage back;
  CHECK(DecodeInstalledLine(line, &back));
  CHECK_STR(back.id, p.id);
  CHECK_STR(back.upstream_commit, p.upstream_commit);
  CHECK_INT(back.installed_at_ms, p.installed_at_ms);
  CHECK_INT(static_cast<int>(back.schemas.size()), 2);
  CHECK_INT(static_cast<int>(back.files.size()), 2);
  CHECK_STR(back.files[1], std::string("array30.dict.yaml"));
}

TEST(Store_installed_record_drops_bad_lines_without_losing_the_rest) {
  const std::string text =
      "good\tc\ts\t1\ta\tf.yaml\n"
      "this line has too few fields\n"
      "\tnoid\ts\t1\ta\tf.yaml\n"
      "when\tc\ts\tnotanumber\ta\tf.yaml\n"
      "also_good\tc\ts\t2\tb\tg.yaml\n";
  const std::vector<InstalledPackage> v = DecodeInstalled(text);
  CHECK_INT(static_cast<int>(v.size()), 2);
  CHECK_STR(v[0].id, std::string("good"));
  CHECK_STR(v[1].id, std::string("also_good"));
}

TEST(Store_failure_tokens_are_all_distinct) {
  // 這個列舉存在的理由就是「不要把不同的失敗壓成同一句話」。
  // 兩個種類共用一個代號等於在診斷輸出上又壓回去了。
  const StoreFailure all[] = {
      StoreFailure::kNone,        StoreFailure::kSwitchOff,
      StoreFailure::kNetwork,     StoreFailure::kIntegrity,
      StoreFailure::kArchive,     StoreFailure::kDisk,
      StoreFailure::kIndex,       StoreFailure::kUnknownPackage,
      StoreFailure::kPreflight,   StoreFailure::kPatch,
      StoreFailure::kDeploy,      StoreFailure::kRollbackFailed,
  };
  std::set<std::string> seen;
  for (StoreFailure f : all) seen.insert(StoreFailureToken(f));
  CHECK_INT(static_cast<int>(seen.size()), 12);
}
