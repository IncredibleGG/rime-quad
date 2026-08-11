// windows/tests/test_ui_strings.cc — 字串目錄(W8 / W9 / W10 的一半)
//
// ⚠ **本檔刻意不含任何中日韓字面值。** W7 要求 `windows/` 底下除了
//   ui_strings.cc 之外命中數為 0,而測試檔也在那個範圍裡 ——
//   一個為了測「不准有中文」而自己寫了中文的測試,會讓那條檢查
//   要嘛紅、要嘛得為自己開一個例外,而例外正是這份規範警告過的東西。
//
//   所以禁用字用**碼點**組出來。副作用是它更清楚:讀的人看得到
//   我們比對的到底是哪幾個字,而不是一個看起來像註解的字面值。

#include "../common/ui_strings.h"

#include <string>
#include <vector>

#include "check.h"

using namespace rimewin;

namespace {

std::wstring W(const std::vector<int>& cps) {
  std::wstring s;
  for (int c : cps) s.push_back(static_cast<wchar_t>(c));
  return s;
}

bool Contains(const std::wstring& hay, const std::wstring& needle) {
  return !needle.empty() && hay.find(needle) != std::wstring::npos;
}

// §6.7 第一層的西文禁用字(硬禁,出現就是紅)。
const char* const kBannedAscii[] = {
    "rs_",          "schema_list",      "page_size",   "simplification",
    "ascii_punct",  "full_shape",       "half_shape",  "speller",
    "translator",   "segmentor",        "processor",   "prism",
    "opencc",       "stabledb",         "db_class",    "table_translator",
    "user_dict",    "custom_phrase",    "preedit",     "langid",
    "rime_shell",   "librime",          "applicationId", "namespace",
    "STUB",         "TODO",             "FIXME",
};

std::wstring Wide(const char* ascii) {
  std::wstring s;
  for (const char* p = ascii; *p; ++p) s.push_back(static_cast<wchar_t>(*p));
  return s;
}

}  // namespace

TEST(ui_strings_catalog_is_big_enough) {
  // §12.9.2:條目數 ≥ 80。現況相異中文字面值 85;取 80 是留給重新設計時
  // 的合併,**不是**放寬 —— 低於 80 代表搬漏了。
  CHECK(UiStringCount() >= 80);
  CHECK_INT(UiStringCount(), static_cast<int>(UiString::kUiStringCount));
}

TEST(ui_strings_every_entry_exists_in_every_language) {
  // W8 的執行期對應物。編譯期的 static_assert 擋長度與順序,
  // 這裡擋「有一格是空字串」—— 那在編譯期是合法的,在畫面上是一片空白。
  int checked = 0;
  for (int i = 0; i < UiStringCount(); ++i) {
    const UiString s = static_cast<UiString>(i);
    for (UiLang l : {UiLang::kEnUs, UiLang::kZhHant, UiLang::kZhHans}) {
      const wchar_t* t = UiTextIn(l, s);
      ++checked;
      CHECK(t != nullptr);
      CHECK(t[0] != L'\0');
    }
  }
  CHECK(checked >= 240);  // 80 條 × 三個語系
}

TEST(ui_strings_no_banned_engine_words) {
  // W9:§6.7 第一層禁用字不出現在 catalog 裡。三個語系都掃。
  int scanned = 0;
  for (int i = 0; i < UiStringCount(); ++i) {
    for (UiLang l : {UiLang::kEnUs, UiLang::kZhHant, UiLang::kZhHans}) {
      const std::wstring t = UiTextIn(l, static_cast<UiString>(i));
      ++scanned;
      for (const char* bad : kBannedAscii) {
        if (Contains(t, Wide(bad))) {
          char buf[256];
          std::snprintf(buf, sizeof(buf),
                        "第 %d 條(語系 %d)含禁用字 \"%s\"", i,
                        static_cast<int>(l), bad);
          ::rimewin_test::Fail(__FILE__, __LINE__, buf);
        }
      }
    }
  }
  // ⚠ 範圍斷言:掃到零條而報「沒有違規」正是要堵死的失效方式。
  CHECK(scanned >= 240);
}

TEST(ui_strings_no_banned_cjk_words) {
  // §6.2 / §6.7 第二層裡「使用者看得到的地方不留」的那幾個:
  //   部署 U+90E8 U+7F72     → 重新整理字詞
  //   上屏 U+4E0A U+5C4F     → 打出來
  //   配置 U+914D U+7F6E     → 設定
  //   候選 U+5019 U+9078     → 選字
  //   詞庫 U+8A5E U+5EAB     → 你自己加的詞
  //   碼表 U+78BC U+8868     → 同上
  const std::vector<std::vector<int>> banned = {
      {0x90E8, 0x7F72}, {0x4E0A, 0x5C4F}, {0x914D, 0x7F6E},
      {0x5019, 0x9078}, {0x8A5E, 0x5EAB}, {0x78BC, 0x8868},
      // 簡體寫法也要擋,不然簡體語系會漏掉:
      // 词库 U+8BCD U+5E93、码表 U+7801 U+8868、候选 U+5019 U+9009
      {0x8BCD, 0x5E93}, {0x7801, 0x8868}, {0x5019, 0x9009},
  };
  int scanned = 0;
  for (int i = 0; i < UiStringCount(); ++i) {
    for (UiLang l : {UiLang::kEnUs, UiLang::kZhHant, UiLang::kZhHans}) {
      const std::wstring t = UiTextIn(l, static_cast<UiString>(i));
      ++scanned;
      for (const auto& b : banned) {
        if (Contains(t, W(b))) {
          char buf[256];
          std::snprintf(buf, sizeof(buf),
                        "第 %d 條(語系 %d)含第二層禁用詞(碼點 U+%04X…)",
                        i, static_cast<int>(l), b[0]);
          ::rimewin_test::Fail(__FILE__, __LINE__, buf);
        }
      }
    }
  }
  CHECK(scanned >= 240);
}

TEST(ui_strings_mode_glyphs_are_not_in_the_catalog) {
  // W10 的前半(後半「必須出現在狀態列繪製碼裡」由 check_ui_spec.sh 驗)。
  //
  // §12.9.3 第 1 條:`中` U+4E2D、`简` U+7B80、`繁` U+7E41 是 §8.12
  // **規範性、四端一致**的字面。進了 catalog 就會有人把簡體語系的
  // 「简」翻成「繁」的對應寫法,而那正是規範要避免的事。
  const std::vector<int> glyphs = {0x4E2D, 0x7B80, 0x7E41};
  int scanned = 0;
  for (int i = 0; i < UiStringCount(); ++i) {
    for (UiLang l : {UiLang::kEnUs, UiLang::kZhHant, UiLang::kZhHans}) {
      const std::wstring t = UiTextIn(l, static_cast<UiString>(i));
      ++scanned;
      for (int g : glyphs) {
        // ⚠ 只比對**整條就是那一個字**的情形。「中文標點」裡的「中」
        //   是一般用字,不是狀態指示 —— 一律禁掉會逼出一堆繞路的譯法。
        if (t == W({g})) {
          char buf[160];
          std::snprintf(buf, sizeof(buf),
                        "第 %d 條整條就是狀態字面 U+%04X —— 它不該進 catalog",
                        i, g);
          ::rimewin_test::Fail(__FILE__, __LINE__, buf);
        }
      }
    }
  }
  CHECK(scanned >= 240);
}

TEST(ui_strings_no_units_on_screen) {
  // §2-B3:畫面上不出現百分比、毫秒、倍數、像素。
  // 例外只有兩條帶格式符的狀態訊息,而它們的 `%` 是佔位符不是百分號。
  const char* const units[] = {"px", " ms", "dpi", "DPI"};
  for (int i = 0; i < UiStringCount(); ++i) {
    const UiString s = static_cast<UiString>(i);
    for (UiLang l : {UiLang::kEnUs, UiLang::kZhHant, UiLang::kZhHans}) {
      const std::wstring t = UiTextIn(l, s);
      for (const char* u : units) CHECK(!Contains(t, Wide(u)));
      // 倍 U+500D
      CHECK(!Contains(t, W({0x500D})));
    }
  }
}

TEST(ui_strings_language_resolution) {
  // 明確指定優先於一切。
  CHECK(ResolveUiLang("en", 0x0404) == UiLang::kEnUs);
  CHECK(ResolveUiLang("zh-Hant", 0x0804) == UiLang::kZhHant);
  CHECK(ResolveUiLang("zh-Hans", 0x0404) == UiLang::kZhHans);

  // system:依這個輸入法註冊在哪個語言底下推。
  CHECK(ResolveUiLang("system", 0x0404) == UiLang::kZhHant);
  CHECK(ResolveUiLang("system", 0x0C04) == UiLang::kZhHant);
  CHECK(ResolveUiLang("system", 0x0804) == UiLang::kZhHans);
  CHECK(ResolveUiLang("system", 0x1004) == UiLang::kZhHans);
  CHECK(ResolveUiLang("system", 0x0409) == UiLang::kEnUs);

  // ⚠ 0 = 不知道。**必須**回一個具體語系,而且是現況的那一個 ——
  //   回英文的話,今天的使用者升級之後會突然看到英文介面。
  CHECK(ResolveUiLang("system", 0) == UiLang::kZhHant);
  CHECK(ResolveUiLang("", 0) == UiLang::kZhHant);
  CHECK(ResolveUiLang("nonsense", 0x0804) == UiLang::kZhHans);

  // 往返。
  for (UiLang l : {UiLang::kEnUs, UiLang::kZhHant, UiLang::kZhHans})
    CHECK(ResolveUiLang(UiLangPrefValue(l), 0) == l);
}

TEST(ui_strings_out_of_range_is_empty_not_a_crash) {
  CHECK(UiText(static_cast<UiString>(-1))[0] == L'\0');
  CHECK(UiText(static_cast<UiString>(999999))[0] == L'\0');
}


// ─────────────────────────────────────────────────────────────
// 那一橫的說明必須說得出「怎麼切中英文」
// ─────────────────────────────────────────────────────────────
//
// 使用者實機回報:「我用 shift 切換中英文,但是他不變」。
//
// 他以為有一顆鍵,而**確實有**(fix4-winkey 那一輪註冊了
// PreserveKey{VK_SPACE, TF_MOD_CONTROL},tsf/text_service.cc)——
// 只是我們從來沒有告訴過他。而那一橫的說明當時還寫著「在句子中間切
// 中英文,目前只有它做得到」,那句話在那一輪之後就已經是假的。
//
// 這一條把「說明裡要提到那顆鍵」變成一個測得到的事實。它同時是
// 那一橫敢自動隱藏的前提:那一橫會消失,而鍵盤上那顆鍵不會。
TEST(ui_strings_bar_blurb_names_the_hotkey) {
  int checked = 0;
  for (int lang = 0; lang < static_cast<int>(UiLang::kLangCount); ++lang) {
    const std::wstring blurb =
        UiTextIn(static_cast<UiLang>(lang), UiString::kStatusBarBlurb);
    CHECK(!blurb.empty());
    // Ctrl 與空白鍵。⚠ 三個語系都要有 —— 缺一個的使用者就回到
    //   「我以為只有那一橫做得到」。
    CHECK(Contains(blurb, L"Ctrl"));
    // 「空白」的中文寫法各語系不同,所以只斷言西文那一半 +
    // 「這一句有提到一個組合鍵」的形狀(有 + 號或 Space)。
    CHECK(Contains(blurb, L"+") || Contains(blurb, L"Space"));

    // ⚠ 而且不得再宣稱「只有它做得到」。這句話現在是假的。
    //   判準用碼點寫(本檔不含中日韓字面值,見檔頭)。
    //   「只有」= U+53EA U+6709
    CHECK(!Contains(blurb, W({0x53EA, 0x6709})));

    // ⚠ Windows 的文案不走 Markdown,`**` 會被原樣畫成星號(工單 #77)。
    CHECK(!Contains(blurb, L"**"));
    ++checked;
  }
  CHECK_INT(checked, static_cast<int>(UiLang::kLangCount));  // 掃描範圍非空(§2-G2)
}
