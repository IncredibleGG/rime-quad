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
// ⚠ **鍵名照 `docs/settings-model.md` §3 的 id,不要自己取。**
//   那份規範說「鍵名共用,值可同步,但檔案格式各端自訂」——
//   所以檔案長什麼樣是我們的事,鍵叫什麼不是。
//
//   B 層(應用偏好,只有 UI 與渲染層讀)。
//   A 層(librime 自己會讀的:schema_list、menu/page_size)**不在這裡**,
//   它在 `<user>/default.custom.yaml`,見 schema_list_patch.h。
//   C 層(session 選項)不落地,由 B 推導。

// 輸入方案
constexpr const char* kSchemasFollowInputMode = "schemas.followInputMode";
constexpr const char* kSchemasPinnedGlobal = "schemas.pinnedGlobal";
constexpr const char* kSchemasPinnedHant = "schemas.pinnedHant";
constexpr const char* kSchemasPinnedHans = "schemas.pinnedHans";
// 外觀
// ⚠ appearance.candidateCount 是 **A 層**(librime 的 menu/page_size),
//   規範 §3 明著標的。主題不得改變一頁有幾個候選,否則序號標籤會與
//   使用者按的數字鍵對不上。所以這裡**沒有**那個鍵。
constexpr const char* kAppearanceCandidateScale = "appearance.candidateScale";
// 深淺色。三態(followSystem / light / dark),id 取自 docs/settings-model.md §3。
constexpr const char* kAppearanceAppearance = "appearance.appearance";
// ⚠ 以下兩個是 **Windows 端自己加的**,docs/settings-model.md §3 還沒有對應的 id。
//   懸浮狀態列是這一輪新增的表面(docs/ui-design.md §12.10),而規範裡的
//   `appearance.showStatusBar` 指的是**候選窗裡面**那一條(§8.12),不是同一個東西。
//   已寫進 docs/coordination.md §5 請規範所有權方(macOS 端)裁決 id。
//   在裁決之前用這兩個名字,而且**不與規範的鍵重名** —— 重名的話,
//   哪天規範真的定義了 showStatusBar,兩邊會安靜地互相覆蓋。
constexpr const char* kAppearanceFloatingBar = "appearance.floatingBar";
constexpr const char* kAppearanceFloatingBarPos = "appearance.floatingBarPos";
// 文字
constexpr const char* kTextVariant = "text.variant";
constexpr const char* kTextPunctuation = "text.punctuation";
// 全形／半形。⚠ 鍵名照 docs/settings-model.md §3 的 id(`text.shape`),
//   不要自己取 —— 那份規範說「鍵名共用,值可同步,但檔案格式各端自訂」。
//   ⚠ 規範把它寫成 enum `followSchema`|`halfShape`|`fullShape`,而這裡存的
//     值是 `true`/`false`(鍵不存在 = followSchema)。**與 text.punctuation
//     完全一樣的落差**,那一個已經是這樣了(規範的 enum 是
//     `followSchema`|`full`|`half`)。鍵名一致、值的字面不一致,已寫進
//     docs/coordination.md §5 請規範所有權方(macOS 端)裁決要不要統一。
constexpr const char* kTextShape = "text.shape";
// ⚠ 這一個也是 **Windows 端自己加的**(同 appearance.floatingBar 那兩個):
//   docs/settings-model.md §3 還沒有「切中英要按哪一顆鍵」的 id。
//   已寫進 docs/coordination.md §5 請規範所有權方(macOS 端)裁決 ——
//   macOS 的 InputModeSwitch 有同一顆開關,兩端遲早要共用一個名字。
//   在裁決之前用這個名字,而且**不與規範的鍵重名**。
constexpr const char* kTextShiftTapToggle = "text.shiftTapToggle";
// 連網
// 進階
// 介面語言。system / en / zh-Hant / zh-Hans。
constexpr const char* kAdvancedLanguage = "advanced.language";
// 連網
constexpr const char* kNetworkEnabled = "network.enabled";
constexpr const char* kStoreIndexUrl = "store.indexUrl";
}  // namespace keys

// ── 候選窗的可選值 ──────────────────────────────────────────────
//
// ⚠ 這兩張表的**索引**就是設定介面下拉選單的索引。Android 端在這裡踩過:
//   字串陣列與數值陣列的順序一旦對不上,畫面完全正常,只是使用者選了「中」
//   拿到「小」。所以索引↔數值的換算是純函式,而且有測試。
//
//   0 一律代表「跟隨主題預設」,固定排在第一格。
// ⚠ 候選個數是 **A 層**:它就是 librime 的 `menu/page_size`(規範 §3)。
//   只截掉畫面上的候選而不動它的話,數字鍵仍然選得到看不見的那幾個 ——
//   那正是「看得到但摸不到」的鏡像版本。所以這張表的值直接寫進
//   default.custom.yaml,B 層沒有對應的鍵。
//   第一格 0 = 「跟著方案」。
extern const int kCandCountValues[];   // {0, 3, 5, 7, 9}
extern const int kCandCountCount;
extern const int kCandScaleValues[];   // {0, 85, 100, 120, 145}
extern const int kCandScaleCount;

// 找不到就回 0(第一格,「跟隨」)。永遠回傳合法索引。
int IndexOfCandCount(int value);
int IndexOfCandScale(int value);
// 索引超出範圍一律回 0 那一格的值。
int CandCountAtIndex(int index);
int CandScaleAtIndex(int index);

// 設定檔在使用者資料目錄底下叫什麼。
//
// ⚠ 這個名字以前寫死在 service/settings_store.cc 裡,而 tests/probe_main.cc
//   也要用它(它要在連線之前把偏好寫進去,模擬「使用者在設定裡選了簡體
//   然後開一個新程式」)。兩個地方各寫一份的話,改名的那一天探針會安靜地
//   寫到一個沒有人讀的檔案 —— 而症狀是「CI 說簡繁沒生效」,查半天才發現
//   是檔名。
constexpr const char* kSettingsFileName = "luminakey.settings";

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
  void SetFollowInputMode(bool on);
  void SetPinnedGlobal(const std::string& schema_id);     // 空 = 不釘
  void SetPinnedForCharSet(CharSet cs, const std::string& schema_id);
  void SetVariantPref(VariantPref v);

  // 標點。三態:kUnset = followSchema(**完全不呼叫 rs_set_option**)。
  // ⚠ 「跟著方案」與「設成 false」不是同一件事:很多方案根本沒有那個
  //    開關,而有些方案的預設是 true。無條件設 false 會讓「跟著方案」
  //    變成「一律西文標點」,而使用者選的是不干預。
  Tri Punctuation() const { return GetTri(keys::kTextPunctuation); }
  void SetPunctuation(Tri t);

  // 全形／半形。三態:kUnset = followSchema(**完全不呼叫 rs_set_option**)。
  //
  // ⚠ 與標點同一條理由,而且更容易踩到:很多方案根本沒有 `full_shape`
  //    這個開關,而有些方案(注音一族的全形變體、日文一族)預設就是全形。
  //    無條件送 false 會讓「跟著方案」變成「一律半形」,而使用者選的是
  //    不干預 —— 那是一個他沒有做過的決定,而且他找不到是誰做的。
  //
  // ⚠ **未設 == 跟著方案**,不是「半形」。半形雖然是絕大多數中文方案的
  //    預設,但把它寫成我們的預設,等於在每一個 session 上都送一次
  //    `full_shape=false`,連「這個方案沒有這個開關」都一起蓋掉。
  Tri Shape() const { return GetTri(keys::kTextShape); }
  void SetShape(Tri t);

  // 輕點 Shift 切中英。
  //
  // ⚠ **未設 == 開。** 根據是業界慣例:微軟拼音、搜狗、macOS 內建的注音
  //   與拼音,預設全部是這顆鍵 —— 從別的輸入法換過來的人會直接按它,
  //   而「按了沒反應」他不會去設定裡找一個他不知道存在的開關。
  //
  // ⚠ 而它必須關得掉:有些人用 Shift 打大寫打得很兇,對他們來說
  //   誤觸的成本是每隔幾個字就要切回來一次。
  //
  // ⚠ 判斷只有這一份。服務端(pipe_server)與設定介面各寫一次
  //   `GetTri(...) != kFalse` 就是兩份真相,而漂移的樣子是
  //   「設定介面顯示關著,實際上還是會切」。
  bool ShiftTapToggle() const {
    return GetTri(keys::kTextShiftTapToggle) != Tri::kFalse;
  }

  // 連網總開關。⚠ 未設 == **關**。這一條不可以寫成「未設 == 開」,
  // 也不可以在別處各寫一次判斷 —— 只有這一個函式知道答案。
  bool NetworkEnabled() const {
    return GetTri(keys::kNetworkEnabled) == Tri::kTrue;
  }

  const std::map<std::string, std::string>& all() const { return kv_; }
  size_t size() const { return kv_.size(); }

 private:
  std::map<std::string, std::string> kv_;
};

// langid → "schema.last.0804"。大寫十六進位、固定四位。
}  // namespace rimewin

#endif  // RIMEWIN_SETTINGS_H_
