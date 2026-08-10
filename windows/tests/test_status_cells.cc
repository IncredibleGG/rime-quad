// windows/tests/test_status_cells.cc — 那一橫第一格只顯示一個字面
//
// 使用者實機回報:「中/en 應該是現在是什麼輸入法就顯示什麼什麼輸入法,
// 簡繁 就做得很好」。
//
// ⚠ 這一支**不用**真的字面(中 / En / 简 / 繁)。理由是守門:W7 規定
//   catalog 以外不得有中日韓寬字串字面值,而它掃得到 tests/。
//   換成看得出來的 ASCII 佔位字串之後,測的東西完全一樣 ——
//   「拿哪一個、拿幾個」是這個函式全部的職責,字面長什麼樣不是。
//   真的那四個字仍然住在 service/status_bar.cc,由 W10 兩個方向驗。

#include "status_cells.h"

#include "check.h"

using namespace rimewin;

namespace {

constexpr wchar_t kCn[] = L"<cn>";
constexpr wchar_t kEn[] = L"<en>";
constexpr wchar_t kSimp[] = L"<simplified>";
constexpr wchar_t kTrad[] = L"<traditional>";

StatusGlyphs Glyphs() {
  StatusGlyphs g;
  g.chinese = kCn;
  g.ascii = kEn;
  g.simplified = kSimp;
  g.traditional = kTrad;
  return g;
}

bool Contains(const std::wstring& hay, const wchar_t* needle) {
  return hay.find(needle) != std::wstring::npos;
}

}  // namespace

// ── 這一條就是使用者回報的那件事 ─────────────────────────────────
TEST(status_cells_input_mode_shows_exactly_one_label) {
  const StatusGlyphs g = Glyphs();

  const std::wstring chinese = InputModeCellText(g, /*ascii_mode=*/false);
  CHECK(Contains(chinese, kCn));
  // ⚠ 核心:另一態**不可以**同時出現。舊版是「中 En」並排,
  //   用顏色深淺表示哪一個生效,而使用者看不出來。
  CHECK(!Contains(chinese, kEn));

  const std::wstring ascii = InputModeCellText(g, /*ascii_mode=*/true);
  CHECK(Contains(ascii, kEn));
  CHECK(!Contains(ascii, kCn));

  // 兩態必須真的不一樣(兩邊都回同一個字串也會通過上面四條)。
  CHECK(chinese != ascii);
  // 而且都不是空的 —— 空字串在那一橫上等於「整格略過」。
  CHECK(!chinese.empty());
  CHECK(!ascii.empty());
}

// ── 第一格與第二格必須講同一種話 ─────────────────────────────────
//
// 使用者的原話是「簡繁 就做得很好」。所以判準不是「第一格好不好」,
// 是**兩格一不一致**:兩格都只畫當前那一個。
TEST(status_cells_mode_and_variant_speak_the_same_way) {
  const StatusGlyphs g = Glyphs();
  int checked = 0;
  for (int a = 0; a < 2; ++a) {
    for (int s = 0; s < 2; ++s) {
      StatusBarState st;
      st.ascii_mode = a != 0;
      st.simplified = s != 0;
      st.schema_name = L"luna";
      st.settings_label = L"settings";
      const std::vector<std::wstring> cells = StatusBarCellTexts(g, st);
      CHECK_INT(static_cast<int>(cells.size()), 4);

      // 第一格:一個字面。
      CHECK(Contains(cells[0], st.ascii_mode ? kEn : kCn));
      CHECK(!Contains(cells[0], st.ascii_mode ? kCn : kEn));
      // 第二格:一個字面(它本來就是對的,這裡把它釘住)。
      CHECK(Contains(cells[1], st.simplified ? kSimp : kTrad));
      CHECK(!Contains(cells[1], st.simplified ? kTrad : kSimp));

      // 兩格的「字面數」必須一樣 —— 這就是「講同一種話」。
      const int n0 = (Contains(cells[0], kCn) ? 1 : 0) +
                     (Contains(cells[0], kEn) ? 1 : 0);
      const int n1 = (Contains(cells[1], kSimp) ? 1 : 0) +
                     (Contains(cells[1], kTrad) ? 1 : 0);
      CHECK_INT(n0, 1);
      CHECK_INT(n1, 1);
      CHECK_INT(n0, n1);
      ++checked;
    }
  }
  // 掃描範圍非空(§2-G2)。
  CHECK_INT(checked, 4);
}

// ── 第三格的空狀態:整項略過,不是一塊空白 ────────────────────────
TEST(status_cells_empty_schema_name_is_skipped_not_blank) {
  const StatusGlyphs g = Glyphs();
  StatusBarState st;
  st.settings_label = L"settings";
  const std::vector<std::wstring> cells = StatusBarCellTexts(g, st);
  CHECK_INT(static_cast<int>(cells.size()), 4);
  CHECK(cells[2].empty());  // 呼叫端據此整格略過
  CHECK(!cells[0].empty());
  CHECK(!cells[1].empty());
  CHECK_STR(std::string(cells[3].begin(), cells[3].end()), "settings");

  st.schema_name = L"bopomofo";
  const std::vector<std::wstring> filled = StatusBarCellTexts(g, st);
  CHECK(!filled[2].empty());
}
