// windows/common/ime_policy.h — 「引擎狀態 → 宿主該做什麼」的政策
//
// 把兩件很容易寫錯、而且錯了以後畫面看起來完全正常的事變成純函式:
//
//   1. **「選字」不等於「上屏」。** 拼音方案選字當下就 commit,注音方案
//      選字後仍停留在組字狀態。判別條件是 menu.count,不是 is_composing。
//      這條政策是 rime_shell.h 檔頭與 docs/coordination.md §4 共同要求的,
//      Android 端已經用多音節輸入壓測過。四端必須是同一條。
//
//   2. **TSF 的組字視窗要不要開、要不要關。** 開了沒關,使用者換到別的
//      輸入框時上一段組字會留在畫面上;該開沒開,preedit 完全看不見。
//      兩者都是「畫面看起來正常、自動化全過」的那一類 bug。
//
// 純函式的用意是這些判斷可以在 Ubuntu 上跑測試 —— TSF 本身 CI 驗不了,
// 但這兩條決定佔了「打不出字」的絕大多數成因。
//
#ifndef RIMEWIN_IME_POLICY_H_
#define RIMEWIN_IME_POLICY_H_

#include <string>

#include "protocol.h"

namespace rimewin {

// ── 1. 服務進程側:要不要替使用者把組字結果送出去 ──────────────
//
//   menu.count > 0                   → 還有段落待選,**不可** commit
//   count == 0 && is_composing       → 轉換完成待確認 → rs_commit_composition()
//   count == 0 && !is_composing      → 已經結束,什麼都不用做
//
// ⚠ 這條規則本身沒有爭議(檔頭寫死的),有爭議的是**什麼時候套用**。
//   目前套用在「每一次輸入事件之後」。理由:librime 只要還在組字就一定
//   會給出候選,所以 count == 0 && is_composing 是個明確的狀態,不是
//   打字打到一半的中途態。這一點在 Android 上成立,在 Windows 上
//   **只有真人在真的輸入框裡打過才算驗過** —— 見 windows/README.md 的
//   「只有人驗得到」那一節。
inline bool ShouldCommitComposition(bool is_composing, int32_t menu_count) {
  return is_composing && menu_count == 0;
}

// ── 2. DLL 側:拿到一份快照之後,對宿主文件該做什麼 ─────────────

enum class DocAction {
  kNothing,          // 什麼都不做
  kUpdate,           // 開啟(或更新)組字,內容是 preedit
  kCommitAndEnd,     // 把 commit_text 送進文件並結束組字
  kCommitAndUpdate,  // 送出 commit_text,但 preedit 還有東西,組字繼續
  kEnd,              // 結束組字,不送任何文字
};

struct HostPlan {
  DocAction action = DocAction::kNothing;
  std::string commit_text;
  std::string preedit;
  int32_t caret = 0;
  // 候選窗要不要顯示。空的候選頁必須關窗 —— 留著一個空窗在畫面上,
  // 是這個專案抓過的「看得到但摸不到」同一族的問題。
  bool show_candidates = false;
};

// had_composition = 這個 context 目前是否已經有一段進行中的組字。
HostPlan PlanFromSnapshot(bool had_composition, const Snapshot& s);

// ── 3. 候選標籤格式(規範 §8.6.1)────────────────────────────────
//
// 支援 {label} / {index} / {index0}。
// **未知的佔位符必須原樣保留** —— 規範明著寫了「不得丟棄,也不得報錯」,
// 而「原樣保留」是四端唯一能一致的行為。這條很容易被實作成「丟掉」,
// 而丟掉之後主題作者看到的是標籤莫名其妙變短,查不出原因。
std::string FormatLabel(const std::string& format, const std::string& label,
                        int32_t index0);

}  // namespace rimewin

#endif  // RIMEWIN_IME_POLICY_H_
