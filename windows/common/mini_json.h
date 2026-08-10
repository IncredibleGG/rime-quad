// windows/common/mini_json.h — 只夠讀方案市集索引的 JSON 讀取器
//
// 對照:Android 的 store/MiniJson.kt、macOS 的 MiniJson.swift。
// 三端各有一份是刻意的 —— 索引是**不可信輸入**(從網路來),
// 而三端不可以因為某一端多接受了一種寫法就對同一份索引產生不同結果。
// 所以這一份與那兩份一樣嚴:多一個逗號、少一個引號都直接失敗。
//
// ⚠ 為什麼不拉一個現成的 JSON 函式庫進來:
//   common/ 底下不准有相依(見 schema_list_patch.h 檔頭的同一段理由)——
//   這裡的東西必須能在開發用的 Ubuntu 上直接 g++ 起來跑測試。
//   而且 nlohmann/json 這種等級的東西為了讀 34 個套件的中繼資料
//   進到一個以「離線、可稽核」為賣點的專案裡,增加的閱讀成本大於收益。
//
// 刻意不支援的:
//   · 註解(JSON 本來就沒有)
//   · NaN / Infinity
//   · 尾隨逗號
//   · 頂層以外的殘餘字元
//
// ⚠ 兩個上限是**安全控制**,不是排版:巢狀深度與整份大小。
//   遞迴下降的解析器碰上 100000 個 `[` 會把堆疊耗光,而那是一個
//   從網路來的位元組就做得到的事。
#ifndef RIMEWIN_MINI_JSON_H_
#define RIMEWIN_MINI_JSON_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rimewin {

// 巢狀深度上限。索引最深是 packages[].verified.probe.schema = 4 層。
constexpr int kJsonMaxDepth = 32;

class Json {
 public:
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

  Type type = Type::kNull;
  bool boolean = false;
  double number = 0.0;
  int64_t integer = 0;      // 只有 is_integer 時有意義
  bool is_integer = false;
  std::string text;
  std::vector<Json> items;                                   // kArray
  std::vector<std::pair<std::string, Json>> fields;           // kObject

  bool IsNull() const { return type == Type::kNull; }
  bool IsObject() const { return type == Type::kObject; }
  bool IsArray() const { return type == Type::kArray; }

  // 找一個欄位。找不到回 nullptr。⚠ 重複鍵取**第一個** ——
  // 取最後一個的話,一份索引裡塞兩個 sha256 就能讓「顯示的」與
  // 「驗證用的」不是同一個值。
  const Json* Find(const std::string& key) const;

  // 取值。型別對不上一律當成沒有(回 fallback)——
  // 索引是伺服器產生的,一個欄位寫錯型別不該讓整份索引出局。
  std::string Str(const std::string& key, const std::string& dflt = "") const;
  int64_t Int(const std::string& key, int64_t dflt = 0) const;
  bool Bool(const std::string& key, bool dflt = false) const;
  // 陣列。不是陣列或不存在都回一個空的靜態陣列,呼叫端不必判 null。
  const std::vector<Json>& Array(const std::string& key) const;
  // 字串陣列;非字串的項目跳過。
  std::vector<std::string> Strings(const std::string& key) const;
};

// 解析。失敗時 err 會有一句英文說明(§4.11:診斷永遠英文)。
// ⚠ **絕不拋例外**:呼叫端是服務進程,而它崩了使用者就沒有輸入法。
bool ParseJson(const std::string& text, Json* out, std::string* err);

}  // namespace rimewin

#endif  // RIMEWIN_MINI_JSON_H_
