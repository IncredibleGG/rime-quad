// windows/common/store_index.h — 方案市集索引的資料模型、解析與相依展開
//
// 對照:Android 的 store/SchemaIndex.kt、macOS 的 StoreIndex.swift。
// 索引由 `scripts/schema_store/mkindex.py` 產生,目前 34 個套件、97 個方案。
//
// ── 解析政策:單一套件壞掉不能弄垮整份索引 ──────────────────────
//
// 缺欄位、sha256 不合法、category 對不上 —— 這些都只讓**那一個套件**出局
// 並留下一條警告,其餘照常顯示。理由是索引由伺服器側產生,桌面端無法修;
// 若整份拒收,使用者連原本好好的方案都裝不了。
//
// 唯一會整份拒收的是 `format_version` 不相容:那代表欄位語意變了,
// 硬讀下去只會產生沉默的錯誤行為。
//
// ⚠ **沒有 sha256 的套件直接出局**,不是「先裝了再說」。它擋不住惡意的
//   來源(見 service/net_gate.h 的信任錨),但擋得住傳輸損壞與截斷,
//   而一份截斷的詞典會讓之後每一次部署都失敗。
#ifndef RIMEWIN_STORE_INDEX_H_
#define RIMEWIN_STORE_INDEX_H_

#include <cstdint>
#include <string>
#include <vector>

namespace rimewin {

constexpr int kStoreIndexFormatVersion = 1;

struct StoreCategory {
  std::string id;
  std::string name;
  int order = 100;
  bool hidden = false;   // true = 不在市集列表顯示(只作為相依的元件)
};

// 套件提供的一個可切換方案。
//
// ⚠ language 掛在「套件的方案」而不是全域的 id 表,理由與 Android 相同:
//   **方案 id 不是全域唯一的**。`double_pinyin` 同時存在於 double-pinyin
//   (繁體詞庫)與 ice(簡體詞庫),字集相反 —— 只有知道是哪個套件裝的
//   才分得出來(待辦 #38)。
struct StoreSchemaRef {
  std::string id;
  std::string name;
  std::string language;  // BCP 47;空或 "und" = 索引沒說
};

struct StorePackage {
  std::string id;
  std::string name;
  std::string category;
  std::string description;
  std::string upstream;
  std::string upstream_commit;
  std::string license;
  std::string file;      // 相對於 base_url 的檔名,也可以是完整 URL
  int64_t size = 0;      // zip 的位元組數
  std::string sha256;    // 小寫,64 個十六進位字元
  std::vector<StoreSchemaRef> schemas;
  std::vector<std::string> requires_ids;
  bool verified_deployed = false;
  std::string recommended_layout;
  std::string layout_note;
  bool recommended = false;

  // 只作為相依而不出現在切換清單裡的套件。
  bool IsComponentOnly() const { return schemas.empty(); }
};

struct StoreIndex {
  int format_version = 0;
  std::string generated_at;
  std::string base_url;
  std::vector<StoreCategory> categories;
  std::vector<StorePackage> packages;

  const StorePackage* ById(const std::string& id) const;
  // 提供某個方案 id 的套件。⚠ 回第一個 —— id 不是全域唯一的。
  const StorePackage* ProvidingSchema(const std::string& schema_id) const;
  std::vector<const StoreCategory*> VisibleCategories() const;
  // 某類別下要顯示的套件:推薦款在前,其餘依名稱排。
  std::vector<const StorePackage*> PackagesIn(const std::string& category) const;
};

struct IndexParseResult {
  bool ok = false;
  std::string error;                  // ok=false 時的英文說明
  StoreIndex index;
  std::vector<std::string> warnings;  // 出局的套件與原因(英文)
};

IndexParseResult ParseStoreIndex(const std::string& json_text);

// ── 相依展開 ────────────────────────────────────────────────────
//
// ⚠ **循環相依是合法的,不是錯誤。** 真索引裡 `luna-pinyin` 與 `stroke`
//   互相 requires(彼此互為反查詞庫,上游本來就設計成要一起裝)。
//   照字面遞迴會無限下去;把循環當錯誤則會讓這兩個最常用的套件永遠裝不了。
//   所以:用 visited 集合終止遞迴,循環中的每個套件各收一次,全部一起下載。
//
//   順序因此只是「盡量讓相依在前」而不是嚴格拓撲序(有環的圖沒有拓撲序)。
//   順序不影響正確性,因為安裝流程會把**所有**套件都解壓完才部署一次。
struct DependencyPlan {
  std::vector<std::string> to_download;      // 套件 id,盡量讓相依在前
  std::vector<std::string> already_installed;
  std::vector<std::string> cycles;           // 只供顯示,不是錯誤
  int64_t total_bytes = 0;

  // 安裝後實際佔用的**概估**。索引的 size 是 zip 大小,嚴重低估實際佔用:
  // 實測 luna-pinyin zip 0.4MB → 解壓 0.9MB → 部署產物 13MB。
  // ⚠ 這個數字在畫面上**必須**講明是概估,不可以假裝精確。
  int64_t EstimatedInstalledBytes() const { return total_bytes * 30; }
};

struct DependencyResult {
  enum class Status { kOk, kUnknownPackage, kMissingDependency };
  Status status = Status::kOk;
  std::string missing;      // kMissingDependency / kUnknownPackage 的那個 id
  std::string required_by;  // kMissingDependency 時是誰要的
  DependencyPlan plan;
};

DependencyResult ResolveDependencies(const StoreIndex& index,
                                     const std::vector<std::string>& selected,
                                     const std::vector<std::string>& installed);

// 反向查詢:若移除 candidate,哪些**仍在使用中**的套件會失去相依。
// 解除安裝前必須問這個問題,否則使用者刪掉 essay 之後所有方案一起壞掉。
std::vector<std::string> DependentsOf(const StoreIndex& index,
                                      const std::string& candidate,
                                      const std::vector<std::string>& installed);

// 人看的大小字串(ASCII,不在地化 —— 數字與單位四端一致)。
std::string FormatBytes(int64_t bytes);

}  // namespace rimewin

#endif  // RIMEWIN_STORE_INDEX_H_
