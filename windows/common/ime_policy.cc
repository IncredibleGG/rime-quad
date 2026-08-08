#include "ime_policy.h"

namespace rimewin {

HostPlan PlanFromSnapshot(bool had_composition, const Snapshot& s) {
  HostPlan p;
  p.preedit = s.preedit;
  p.caret = s.caret;
  p.show_candidates = !s.items.empty();

  const bool has_preedit = !s.preedit.empty();

  if (s.has_commit && !s.commit_text.empty()) {
    p.commit_text = s.commit_text;
    // commit 之後還有 preedit,是真實會發生的:例如選了句子的前半段,
    // librime 立刻把前半段 commit 掉,後半段仍在組字。
    // 若這裡一律 kCommitAndEnd,後半段就會從畫面上消失,而使用者接著打的
    // 每一個字都會接在錯的地方 —— 典型的「畫面正常但東西不見了」。
    p.action = has_preedit ? DocAction::kCommitAndUpdate : DocAction::kCommitAndEnd;
    return p;
  }

  if (has_preedit) {
    p.action = DocAction::kUpdate;
    return p;
  }

  // 沒有 commit 也沒有 preedit:如果先前有組字,現在要收掉。
  p.action = had_composition ? DocAction::kEnd : DocAction::kNothing;
  p.show_candidates = false;
  return p;
}

std::string FormatLabel(const std::string& format, const std::string& label,
                        int32_t index0) {
  std::string out;
  out.reserve(format.size() + label.size());

  const std::string idx = std::to_string(static_cast<long long>(index0) + 1);
  const std::string idx0 = std::to_string(static_cast<long long>(index0));

  size_t i = 0;
  while (i < format.size()) {
    if (format[i] != '{') {
      out.push_back(format[i++]);
      continue;
    }
    const size_t close = format.find('}', i + 1);
    if (close == std::string::npos) {
      // 沒有配對的 '}':剩下的原樣輸出。不報錯 —— 規範要求容錯。
      out.append(format, i, std::string::npos);
      break;
    }
    const std::string name = format.substr(i + 1, close - i - 1);
    if (name == "label") {
      out.append(label);
    } else if (name == "index") {
      out.append(idx);
    } else if (name == "index0") {
      out.append(idx0);
    } else {
      // 未知佔位符:**連大括號一起原樣保留**(規範 §8.6.1)。
      out.append(format, i, close - i + 1);
    }
    i = close + 1;
  }
  return out;
}

}  // namespace rimewin
