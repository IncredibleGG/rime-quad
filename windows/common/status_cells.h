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
#ifndef RIMEWIN_COMMON_STATUS_CELLS_H_
#define RIMEWIN_COMMON_STATUS_CELLS_H_

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

struct StatusBarState {
  bool ascii_mode = false;
  bool simplified = false;
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
