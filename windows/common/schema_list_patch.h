// windows/common/schema_list_patch.h — 改寫 default.custom.yaml 的 schema_list
//
// 「方案排序」在 RIME 裡就是 `default.custom.yaml` 的 `patch: schema_list:`。
// 這一格是**使用者資料**:安裝時只補不覆蓋(見 service/main.cc),
// 而且使用者可能自己動過。所以改寫的規則是:
//
//   **只換掉 schema_list 那一段,其餘每一行原樣保留。**
//
// 為什麼不用 yaml-cpp(服務進程裡明明有):
//
//   1. 這一段要能在 Ubuntu 上跑測試。common/ 底下不准有相依,
//      而「排序寫壞了會怎樣」正是最需要測試的地方 ——
//      寫壞的症狀是使用者一啟動就部署失敗,而且沒有自救途徑
//      (設定介面也打不開,因為方案清單是空的)。
//   2. **yaml-cpp 會把註解全部吃掉。** 那個檔案是我們自己產生的,
//      裡面有整段解釋「為什麼要限縮 schema_list」。使用者按一次
//      「上移」就把它清空,是很難解釋的行為。
//
// 代價是這裡不是完整的 YAML 解析器。所以規則寫得很保守:認得出來就改,
// 認不出來就**明著失敗**,由呼叫端告訴使用者「你的 default.custom.yaml
// 我看不懂,請手動編輯」。安靜地重寫整個檔案是最糟的選項。
//
#ifndef RIMEWIN_SCHEMA_LIST_PATCH_H_
#define RIMEWIN_SCHEMA_LIST_PATCH_H_

#include <string>
#include <vector>

namespace rimewin {

// 從 default.custom.yaml 的內容裡讀出目前的 schema_list 順序。
// 讀不到(沒有那一段)回傳空 vector —— 那與「有一段但它是空的」
// 在呼叫端是不同的意思,所以另有 found 旗標。
std::vector<std::string> ReadSchemaList(const std::string& yaml, bool* found);

enum class PatchResult {
  kOk = 0,
  kNoPatchSection,   // 檔案裡沒有 `patch:` —— 我們不敢自己加,結構未知
  kEmptyOrder,       // 呼叫端給了空清單。schema_list 空掉 = 一個方案都沒有
  kBadSchemaId,      // 清單裡有不像方案 id 的東西
};

// 產生新的檔案內容。
//
// · 有 schema_list 那一段 → **原地**換掉它底下的項目,縮排沿用原本的。
// · 有 `patch:` 但沒有 schema_list → 在 patch: 底下加一段。
// · 連 `patch:` 都沒有 → 失敗(kNoPatchSection)。
//
// out 只在回傳 kOk 時有意義。
PatchResult WriteSchemaList(const std::string& yaml,
                            const std::vector<std::string>& order,
                            std::string* out);

// 在 `patch:` 底下設定(或改寫)一個純量鍵,例如 `menu/page_size: 9`。
// value 為空字串 = **刪掉那個鍵**(回到「沒設過」,見 settings.h 的檔頭)。
//
// 與 WriteSchemaList 同樣保守:找不到 `patch:` 就明著失敗,不猜。
PatchResult UpsertPatchScalar(const std::string& yaml, const std::string& key,
                              const std::string& value, std::string* out);

// 讀回來。沒有那個鍵回傳空字串。
std::string ReadPatchScalar(const std::string& yaml, const std::string& key);

// 方案 id 的字元規則。id 有一部分來自下載回來的市集索引(不可信輸入),
// 而它會被寫進一個 librime 會照著去找檔案的 YAML 檔。
bool IsPlausibleSchemaId(const std::string& id);

}  // namespace rimewin

#endif  // RIMEWIN_SCHEMA_LIST_PATCH_H_
