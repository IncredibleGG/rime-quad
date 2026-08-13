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
      st.variant = s ? VariantCell::kSimplified : VariantCell::kTraditional;
      st.schema_name = L"luna";
      st.settings_label = L"settings";
      const std::vector<std::wstring> cells = StatusBarCellTexts(g, st);
      CHECK_INT(static_cast<int>(cells.size()), 4);

      const bool simp = st.variant == VariantCell::kSimplified;
      // 第一格:一個字面。
      CHECK(Contains(cells[0], st.ascii_mode ? kEn : kCn));
      CHECK(!Contains(cells[0], st.ascii_mode ? kCn : kEn));
      // 第二格:一個字面(它本來就是對的,這裡把它釘住)。
      CHECK(Contains(cells[1], simp ? kSimp : kTrad));
      CHECK(!Contains(cells[1], simp ? kTrad : kSimp));

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
  st.variant = VariantCell::kTraditional;
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

// ─────────────────────────────────────────────────────────────
// §12.10.4 的 variant 判定:引擎**實際套用**的字形轉換,不是我們的偏好
// ─────────────────────────────────────────────────────────────
//
// 使用者實機回報:設定裡選了簡體,那一格畫「简」,打出來卻是繁體。
// 根因是那一格讀的是 `simplification` —— 而 `rs_set_option` 對一個
// **不存在的選項**不會失敗,它會原樣記下、原樣回讀。本專案打包的方案
// (luna_pinyin 家族、bopomofo 家族)通通沒有 `simplification`,所以
// 那一格讀到的一直是**我們自己剛寫進去的那個值**,不是引擎的狀態。
//
// 這一組真值表把新規則釘住,而 W-Variant-1 那一條就是使用者截圖的狀態。

TEST(variant_cell_ignores_simplification_echo) {
  // ⚠ 這一條是根因本身。四個 radio 全 false = 引擎沒有套任何字形轉換,
  //   而 `simplification` 就算被我們設成 true 也只是回音 ——
  //   本結構刻意**沒有** simplification 這個欄位,連拿它判定的機會
  //   都不留下。這一格在這種狀態下不顯示。
  VariantOptions o;
  CHECK(VariantCellFromOptions(o) == VariantCell::kHidden);

  // 「整格不顯示」在那一橫上的表現是空字串(沿用第三格的既有先例)。
  const StatusGlyphs g = Glyphs();
  StatusBarState st;
  st.variant = VariantCellFromOptions(o);
  st.schema_name = L"luna";
  st.settings_label = L"settings";
  const std::vector<std::wstring> cells = StatusBarCellTexts(g, st);
  CHECK_INT(static_cast<int>(cells.size()), 4);
  CHECK(cells[1].empty());
  // ⚠ 不顯示 ≠ 畫一塊空白,也 ≠ 退回去畫繁體。兩個字面都不可以出現。
  CHECK(!Contains(cells[1], kTrad));
  CHECK(!Contains(cells[1], kSimp));
  // 其餘三格照舊在。
  CHECK(!cells[0].empty());
  CHECK(!cells[2].empty());
  CHECK(!cells[3].empty());
}

TEST(variant_cell_truth_table) {
  int seen = 0;
  // 逐一:單獨為真的四種。
  {
    VariantOptions o;
    o.zh_hans = true;
    CHECK(VariantCellFromOptions(o) == VariantCell::kSimplified);
    ++seen;
  }
  {
    VariantOptions o;
    o.zh_hant = true;
    CHECK(VariantCellFromOptions(o) == VariantCell::kTraditional);
    ++seen;
  }
  {
    VariantOptions o;
    o.zh_hant_hk = true;
    CHECK(VariantCellFromOptions(o) == VariantCell::kTraditional);
    ++seen;
  }
  {
    // ⚠ 這就是使用者截圖的狀態:luna_pinyin_tw 的 `__patch` 把
    //   switches/@2/reset 設成 3,載入時 zh_hant_tw = 1,輸出是繁體 ——
    //   而畫面畫的是「简」。修完之後這裡必須是繁。
    VariantOptions o;
    o.zh_hant_tw = true;
    CHECK(VariantCellFromOptions(o) == VariantCell::kTraditional);
    ++seen;
  }
  // zh_hans 優先:radio 理論上互斥,但 rs_set_option 不維持互斥,
  // 而「先關再開」中間真的存在兩個都為真的一瞬間。要有確定的答案。
  for (int hant = 0; hant < 2; ++hant) {
    for (int hk = 0; hk < 2; ++hk) {
      for (int tw = 0; tw < 2; ++tw) {
        VariantOptions o;
        o.zh_hans = true;
        o.zh_hant = hant != 0;
        o.zh_hant_hk = hk != 0;
        o.zh_hant_tw = tw != 0;
        CHECK(VariantCellFromOptions(o) == VariantCell::kSimplified);
        ++seen;
      }
    }
  }
  // 三種繁體任意組合(沒有 zh_hans)都是繁,而且不會變成 kHidden。
  for (int hant = 0; hant < 2; ++hant) {
    for (int hk = 0; hk < 2; ++hk) {
      for (int tw = 0; tw < 2; ++tw) {
        if (!hant && !hk && !tw) continue;  // 那是 kHidden,上面測過
        VariantOptions o;
        o.zh_hant = hant != 0;
        o.zh_hant_hk = hk != 0;
        o.zh_hant_tw = tw != 0;
        CHECK(VariantCellFromOptions(o) == VariantCell::kTraditional);
        ++seen;
      }
    }
  }
  CHECK_INT(seen, 4 + 8 + 7);  // 掃描範圍非空(§2-G2)
}

TEST(variant_cell_hidden_is_the_only_empty_state) {
  // 三態各自在那一橫上的字面,一次釘死。
  const StatusGlyphs g = Glyphs();
  int seen = 0;
  const VariantCell all[3] = {VariantCell::kHidden, VariantCell::kTraditional,
                              VariantCell::kSimplified};
  for (const VariantCell v : all) {
    StatusBarState st;
    st.variant = v;
    st.schema_name = L"luna";
    st.settings_label = L"settings";
    const std::vector<std::wstring> cells = StatusBarCellTexts(g, st);
    if (v == VariantCell::kHidden) {
      CHECK(cells[1].empty());
    } else {
      CHECK(!cells[1].empty());
      CHECK(Contains(cells[1], v == VariantCell::kSimplified ? kSimp : kTrad));
      CHECK(!Contains(cells[1], v == VariantCell::kSimplified ? kTrad : kSimp));
    }
    ++seen;
  }
  CHECK_INT(seen, 3);
}

// ── 線路旗標 → 三態 ──────────────────────────────────────────────
TEST(variant_cell_from_wire_flags) {
  // known 為假 → kHidden,而且**不看** simplified。
  // 舊版的服務不會送 kStVariantKnown,它送來的每一份快照都落在這裡。
  CHECK(VariantCellFrom(false, false) == VariantCell::kHidden);
  CHECK(VariantCellFrom(false, true) == VariantCell::kHidden);
  // known 為真才看 simplified。
  CHECK(VariantCellFrom(true, false) == VariantCell::kTraditional);
  CHECK(VariantCellFrom(true, true) == VariantCell::kSimplified);

  // 四種組合都掃過,而且三態都出現過(掃描範圍非空,§2-G2)。
  int hidden = 0, trad = 0, simp = 0;
  for (int k = 0; k < 2; ++k) {
    for (int m = 0; m < 2; ++m) {
      switch (VariantCellFrom(k != 0, m != 0)) {
        case VariantCell::kHidden: ++hidden; break;
        case VariantCell::kTraditional: ++trad; break;
        case VariantCell::kSimplified: ++simp; break;
      }
    }
  }
  CHECK_INT(hidden, 2);
  CHECK_INT(trad, 1);
  CHECK_INT(simp, 1);
}

// ── 簡繁快捷鍵按下去要送哪一邊(G76)──────────────────────────────
//
// ⚠ 方向的判斷只有這一份。狀態列第二格、Ctrl+Shift+F、設定視窗
//   三條路各寫一次的話會漂移,而漂移的樣子是「從這裡切有效、
//   從那裡切無效」,使用者猜不到差別在哪。
TEST(toggle_variant_target_flips_and_refuses_to_guess) {
  bool to_simplified = false;
  CHECK(ToggleVariantTarget(VariantCell::kSimplified, &to_simplified));
  CHECK(!to_simplified);
  CHECK(ToggleVariantTarget(VariantCell::kTraditional, &to_simplified));
  CHECK(to_simplified);

  // ⚠ kHidden = 引擎**沒有回報**任何字形(第三方方案多半沒有那一組
  //   開關)。此時方向是猜的,而按下去改變的是使用者看不見的東西 ——
  //   所以什麼都不做,連輸出參數都不動。
  //   這與 §12.10.4 第二格「那一格點不到」是同一條規矩。
  to_simplified = true;
  CHECK(!ToggleVariantTarget(VariantCell::kHidden, &to_simplified));
  CHECK(to_simplified);

  CHECK(!ToggleVariantTarget(VariantCell::kSimplified, nullptr));
}
