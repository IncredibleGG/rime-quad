#include "status_cells.h"

namespace rimewin {
namespace {

std::wstring Str(const wchar_t* s) { return s ? std::wstring(s) : std::wstring(); }

}  // namespace

VariantCell VariantCellFromOptions(const VariantOptions& o) {
  // 順序是規範性的(§12.10.4)。zh_hans 優先:rs_set_option
  // 不維持 radio 的互斥,而 PlanVariant 是「先關再開」——
  // 中間真的存在兩個都為真的一瞬,要有一個確定的答案。
  if (o.zh_hans) return VariantCell::kSimplified;
  if (o.zh_hant || o.zh_hant_hk || o.zh_hant_tw)
    return VariantCell::kTraditional;
  // 四個都為假。這是一個真實而且正確的狀態,見檔頭。
  return VariantCell::kHidden;
}

std::wstring InputModeCellText(const StatusGlyphs& glyphs, bool ascii_mode) {
  // ⚠ 一個字面,不是兩個。見標頭。
  return ascii_mode ? Str(glyphs.ascii) : Str(glyphs.chinese);
}

std::vector<std::wstring> StatusBarCellTexts(const StatusGlyphs& glyphs,
                                             const StatusBarState& state) {
  std::vector<std::wstring> out;
  out.push_back(InputModeCellText(glyphs, state.ascii_mode));
  // 第二格本來就是「只畫當前那一個」—— 使用者說它「做得很好」,
  // 第一格現在跟它一樣。
  //
  // kHidden -> 空字串：**整格略過**（與第三格同一個先例）。
  // 不是退回去畫繁體 —— 那就是拿一個猜的值去冒充引擎的狀態。
  switch (state.variant) {
    case VariantCell::kSimplified:
      out.push_back(Str(glyphs.simplified));
      break;
    case VariantCell::kTraditional:
      out.push_back(Str(glyphs.traditional));
      break;
    case VariantCell::kHidden:
      out.push_back(std::wstring());
      break;
  }
  // 空狀態整項略過:方案名還沒載入完成時,那一格完全不佔位置。
  out.push_back(state.schema_name);
  out.push_back(state.settings_label);
  return out;
}

}  // namespace rimewin
