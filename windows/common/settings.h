// windows/common/settings.h — 桌面端的使用者設定(純邏輯,不含檔案 I/O)
//
// ── 為什麼是 key = value 而不是 YAML ────────────────────────────
//
// 服務進程裡其實有 yaml-cpp(librime 的相依),但 `windows/common/` 底下
// 的東西必須在 Ubuntu 上用一支 g++ 編起來、不帶任何相依 ——
// 那是這個專案唯一能在推 CI 之前驗證邏輯的管道(見 run_logic_tests.sh)。
// 設定檔本身也是使用者會打開來看的東西,`key = value` 一眼就讀得懂,
// 而解析器只有四十行。
//
// 檔案位置由呼叫端決定,本檔只負責字串進、字串出。
//
// ── ⚠ 「沒設過」與「設成預設值」是兩件事 ────────────────────────
//
// 這是 Android 端整套設定的地基,桌面端照抄:**鍵不存在 = 使用者從來
// 沒表示過意見**,而不是「他選了預設值」。差別在哪天預設值變了 ——
// 沒表示過意見的人跟著走,明確選過的人不動。
// 如果把當下的預設值寫進檔案,這兩種人就再也分不出來了,
// 而分不出來之後,任何預設值的調整都會變成「不能動的東西」。
//
// 所以:
//   · 設定成「跟隨」/「預設」= **刪掉那個鍵**,不是寫一個哨兵值。
//   · 讀不到、型別不對、超出範圍 = 一律當成沒設過(絕不讓整份設定壞掉)。
//
// ── 未知的鍵一律原樣保留 ────────────────────────────────────────
//
// 使用者可能同時裝著兩個版本(升級到一半、或是攜帶版),舊版寫回設定檔
// 時不可以把新版的鍵吃掉 —— 那個症狀是「升級之後設定莫名其妙回到預設」,
// 而且只有在兩個版本交替執行時才會發生,幾乎不可能被回報清楚。
//
#ifndef RIMEWIN_SETTINGS_H_
#define RIMEWIN_SETTINGS_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "schema_choice.h"

namespace rimewin {

// 三態。順序刻意讓 kUnset == 0,預設建構就是「沒設過」。
enum class Tri { kUnset = 0, kFalse, kTrue };

// ── 鍵名 ────────────────────────────────────────────────────────
//
// 命名慣例:`區塊.項目`。Android 那邊是扁平的 snake_case,桌面端多了
// 一層前綴是因為設定檔是使用者看得到的檔案,分區之後好讀。
// 對應關係寫在 windows/README.md。
namespace keys {
// 方案
constexpr const char* kSchemaForced = "schema.forced";      // "" = 跟隨語言
constexpr const char* kSchemaOrder = "schema.order";        // 逗號分隔;空 = 不改
constexpr const char* kSchemaLastPrefix = "schema.last.";   // + 4 位大寫十六進位 langid
// 文字
constexpr const char* kTextVariant = "text.variant";        // "" | zh_hant | zh_hans | …
constexpr const char* kTextAsciiPunct = "text.ascii_punct";  // 三態
// 候選窗
constexpr const char* kCandCount = "cand.count";            // 0=跟隨;3/5/7/9;-1=不限
constexpr const char* kCandScale = "cand.scale";            // 0=跟隨;85/100/120/145
// 連網
constexpr const char* kNetEnabled = "net.enabled";          // 三態,未設 == 關
constexpr const char* kNetIndexUrl = "net.index_url";       // "" = 用內建預設
}  // namespace keys

// ── 候選窗的可選值 ──────────────────────────────────────────────
//
// ⚠ 這兩張表的**索引**就是設定介面下拉選單的索引。Android 端在這裡踩過:
//   字串陣列與數值陣列的順序一旦對不上,畫面完全正常,只是使用者選了「中」
//   拿到「小」。所以索引↔數值的換算是純函式,而且有測試。
//
//   0 一律代表「跟隨主題預設」,固定排在第一格。
extern const int kCandCountValues[];   // {0, 3, 5, 7, 9, -1}  -1 = 不限
extern const int kCandCountCount;
extern const int kCandScaleValues[];   // {0, 85, 100, 120, 145}
extern const int kCandScaleCount;

// 找不到就回 0(第一格,「跟隨」)。永遠回傳合法索引。
int IndexOfCandCount(int value);
int IndexOfCandScale(int value);
// 索引超出範圍一律回 0 那一格的值。
int CandCountAtIndex(int index);
int CandScaleAtIndex(int index);

class Settings {
 public:
  // 解析。**永不失敗** —— 壞掉的行整行丟掉,其餘照常。
  // 一行壞掉就讓整份設定回到預設,是使用者最不需要的那種嚴謹。
  static Settings Parse(const std::string& text);

  // 序列化。已知的鍵照 kKnownKeys 的順序輸出並帶上分區註解;
  // 未知的鍵原樣附在最後(見檔頭)。
  std::string Serialize() const;

  bool Has(const std::string& key) const;
  void Unset(const std::string& key);
  std::string Raw(const std::string& key) const;  // 沒有就回空字串
  void SetRaw(const std::string& key, const std::string& value);

  Tri GetTri(const std::string& key) const;
  void SetTri(const std::string& key, Tri v);  // kUnset → 刪掉

  // 讀不到 / 不是數字 / 不在 allowed 裡 → 回傳 allowed[0]。
  int GetEnumInt(const std::string& key, const int* allowed, int n) const;
  void SetEnumInt(const std::string& key, int value, const int* allowed, int n);

  // ── 語意層(設定介面與服務都只該用這一層)────────────────────
  SchemaPreference SchemaPref() const;
  void SetForcedSchema(const std::string& schema_id);  // 空 = 跟隨語言
  void SetForcedVariant(Variant v);                    // kFollow = 跟隨語言
  void RememberLastUsed(uint32_t langid, const std::string& schema_id);

  std::vector<std::string> SchemaOrder() const;
  void SetSchemaOrder(const std::vector<std::string>& ids);

  // 連網總開關。⚠ 未設 == **關**。這一條不可以寫成「未設 == 開」,
  // 也不可以在別處各寫一次判斷 —— 只有這一個函式知道答案。
  bool NetworkEnabled() const { return GetTri(keys::kNetEnabled) == Tri::kTrue; }

  const std::map<std::string, std::string>& all() const { return kv_; }
  size_t size() const { return kv_.size(); }

 private:
  std::map<std::string, std::string> kv_;
};

// langid → "schema.last.0804"。大寫十六進位、固定四位。
std::string SchemaLastKey(uint32_t langid);

}  // namespace rimewin

#endif  // RIMEWIN_SETTINGS_H_
