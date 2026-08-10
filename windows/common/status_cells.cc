#include "status_cells.h"

namespace rimewin {
namespace {

std::wstring Str(const wchar_t* s) { return s ? std::wstring(s) : std::wstring(); }

}  // namespace

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
  out.push_back(state.simplified ? Str(glyphs.simplified)
                                 : Str(glyphs.traditional));
  // 空狀態整項略過:方案名還沒載入完成時,那一格完全不佔位置。
  out.push_back(state.schema_name);
  out.push_back(state.settings_label);
  return out;
}

}  // namespace rimewin
