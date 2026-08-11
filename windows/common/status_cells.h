// windows/common/status_cells.h — 懸浮狀態列那四格顯示什麼(§12.10.4)
//
// ── ⚠ 第一格只顯示**一個**字面 ──────────────────────────────────
//
// 使用者實機回報:「中/en 應該是現在是什麼輸入法就顯示什麼什麼輸入法,
// 簡繁 就做得很好」。
//
// 在這之前第一格是「中 En」兩態並排,用顏色深淺表示哪一個生效 ——
// 而第二格(简/繁)只畫當前那一個。同一條狀態列上兩格用兩種語彙,
// 使用者讀不出第一格現在是哪一邊:深淺是**相對**的訊號,要兩個都看見
// 而且知道規則才解得開;只畫一個字則是**絕對**的,看一眼就是答案。
//
// 規範裡兩種都有,是我們選錯了 source:
//
//   docs/theme-format.md §8.12
//     `input_mode`       →  is_ascii_mode ? "En" : "中"     ← 現在用這個
//     `input_mode_pair`  →  `中/En` 兩態同時顯示            ← 之前用這個
//
// 也就是說這不是「違反規範」,是把 §12.10.4 第一格的 source 從
// `input_mode_pair` 換成 `input_mode` —— 兩者都是規範性的字面,
// 而第二格用的 `variant` 本來就是「只畫當前那一個」那一族。
//
// ⚠ `input_mode_pair` 當初被選上的理由寫在 §12.10.1:「只顯示一個字的話,
//   『中』有兩種讀法(『現在是中文』還是『按下去變中文』),Android 被
//   真機回報過」。那個顧慮是真的,但它**同樣適用於简/繁**,而使用者
//   實機用過之後說第二格「做得很好」。一條狀態列上兩格講同一種話,
//   比兩格各自最佳化重要 —— 讀法的歧義由「整條都是狀態,不是按鈕」
//   這個一致性解決,不是由多畫一個字解決。
//
// ── 為什麼是純函式 ──────────────────────────────────────────────
//
// 在這之前,「那一格畫什麼」寫在 service/status_bar.cc 的 Relayout() 與
// Paint() 裡,而那個檔案在 Ubuntu 上編不起來 —— 也就是說**沒有任何
// 自動化看得到它**,而「畫了兩個字面」正是一個只有人看得出來的缺陷。
// 現在它在這裡,測試直接斷言「第一格裡不會同時出現兩個字面」。
//
// ⚠ 四個規範性字面(中 / En / 简 / 繁)**不在本檔**,由呼叫端傳進來。
//   §12.9.3 第 1 條規定它們直接寫在狀態列的繪製碼裡、不進 catalog,
//   而 W10 兩個方向都驗那件事。本檔只決定「拿哪一個」。
//
//
// ── ⚠ 第二格讀的是引擎**實際套用**的字形轉換,不是我們的偏好 ─────
//
// 使用者實機回報:設定裡選了簡體,那一格畫「简」,而打出來是繁體。
//
// 根因是那一格以前讀 `simplification`,而**本專案打包的方案通通沒有
// 那個開關**:luna_pinyin 家族與 bopomofo 家族用的是一組互斥的 radio
// (zh_hant / zh_hans / zh_hant_hk / zh_hant_tw)。`rs_set_option` 對一個
// 不存在的選項**不會失敗** —— 它原樣記下、原樣回讀。於是那一格顯示的
// 一直是我們自己剛寫進去的偏好,不是引擎的狀態。畫面在替一件沒有發生
// 的事作證。
//
// 判定順序(§12.10.4,四端一致):
//
//     zh_hans 為真                              → 简
//     zh_hant / zh_hant_hk / zh_hant_tw 任一為真 → 繁
//     四個都為假                                 → **整格不顯示**
//
// ⚠ 「四個都為假」是一個真實而且正確的狀態,不是錯誤:純 luna_pinyin 的
//   那組 radio **沒有 `reset:`**,而 librime 的 ConcreteEngine::
//   InitializeOptions() 只在 reset_value >= 0 時才設值 —— 所以剛載入時
//   四個全是 false,而輸出是繁體(詞典本身就是繁體字集,沒有任何
//   simplifier 生效)。此時我們**確實**不知道引擎在做哪一種轉換,
//   整格不顯示是唯一誠實的答案。
//
// ⚠ **不要拿 `simplification` 當 fallback。** 純 luna_pinyin + 我們寫進去的
//   `simplification=1` 會讓這一格畫「简」而輸出是繁體 —— 與使用者截圖
//   一模一樣的缺陷,只是換一個方案觸發。本結構刻意**沒有**
//   simplification 這個欄位,連拿它判定的機會都不留下。
//
// ⚠ 「整格不顯示」沿用 §8.12「空的那一格整項略過」的既有先例
//   (第三格的方案名本來就是這樣),不是畫一塊看不出用途的空白。
//
// ⚠ 這一格**不得樂觀寫入**:點下去之後,在引擎回報新狀態之前不改變
//   字面。呼叫端(service/status_bar.cc)負責這一條。
//
// ── 殘留(明著寫出來,不假裝解決了)────────────────────────────
//
// 一個既沒有那組 radio、也沒有 `simplification` 的第三方方案,
// `rs_set_option(zh_hans, true)` 仍然會被記下並回讀,這一格仍然會畫
// 「简」而輸出沒變。用今天的 `rs_` API 問不出「這個方案有沒有宣告
// 這個選項」—— core/include/rime_shell.h 只有 rs_set_option /
// rs_get_option,沒有任何 config API。真解是新增
// `rs_schema_declares_option(schema_id, option)`,已開工單。
//
#ifndef RIMEWIN_COMMON_STATUS_CELLS_H_
#define RIMEWIN_COMMON_STATUS_CELLS_H_

#include <cstdint>
#include <string>
#include <vector>

namespace rimewin {

// 呼叫端提供的四個規範性字面(§8.12,四端一致、不得在地化)。
struct StatusGlyphs {
  const wchar_t* chinese = nullptr;      // 中
  const wchar_t* ascii = nullptr;        // En
  const wchar_t* simplified = nullptr;   // 简
  const wchar_t* traditional = nullptr;  // 繁
};

// ── 那一格的三態(§12.10.4)──────────────────────────────────────
//
// kHidden 不是「不知道所以先畫繁體」,是**整格不顯示**。見檔頭。
enum class VariantCell : uint8_t { kHidden = 0, kTraditional, kSimplified };

// 引擎目前實際套用的字形轉換開關,由 rs_get_option 逐一讀回來
// (radio group 的四個成員,見 schema_choice.h 的 kVariantOptions)。
//
// ⚠ 這裡**沒有** `simplification`。那不是遺漏,是本檔存在的理由:
//   它是我們寫進去的回音,不是引擎的狀態。見檔頭。
struct VariantOptions {
  bool zh_hans = false;
  bool zh_hant = false;
  bool zh_hant_hk = false;
  bool zh_hant_tw = false;
};

VariantCell VariantCellFromOptions(const VariantOptions& o);

struct StatusBarState {
  bool ascii_mode = false;
  // ⚠ 三態。kHidden = **那一格整項略過**,見檔頭與 VariantCellFromOptions。
  VariantCell variant = VariantCell::kHidden;
  // 方案名。**空字串 = 那一格整項略過**(§8.12 規範性),
  // 不是畫成一塊看不出用途的空白。
  std::wstring schema_name;
  // 第四格的「設定」。它進 catalog、要在地化(§12.10.4)。
  std::wstring settings_label;
};

// §12.10.4 的四格,依序:
//   0 input_mode  1 variant  2 schema_name  3 設定
//
// ⚠ 回傳的每一格都是**一個**字面。第 0 格不得同時含中與 En ——
//   那正是使用者回報的那件事,而 tests/test_status_cells.cc 直接斷言它。
std::vector<std::wstring> StatusBarCellTexts(const StatusGlyphs& glyphs,
                                             const StatusBarState& state);

// 第一格單獨拿出來的版本(那一橫以外的入口日後也會用到,而且它是
// 「只有一個字面」這條規則最小的可測單位)。
std::wstring InputModeCellText(const StatusGlyphs& glyphs, bool ascii_mode);

}  // namespace rimewin

#endif  // RIMEWIN_COMMON_STATUS_CELLS_H_
