#include "ui_layout.h"

#include <algorithm>
#include <initializer_list>

namespace rimewin {

int ContentWidthDip(int window_w_dip) {
  const int avail = window_w_dip - metric::kSidebarW;
  const int want = avail - 2 * space::s7;
  return std::max(kContentMinW, std::min(kContentMaxW, want));
}

int ContentXDip(int window_w_dip) {
  const int avail = window_w_dip - metric::kSidebarW;
  const int cw = ContentWidthDip(window_w_dip);
  // 置中。W > 840 時兩側留白;W = 660 時 (460-440)/2 = 10,
  // 也就是內距被壓到 10 —— 刻意的,見標頭。
  return metric::kSidebarW + (avail - cw) / 2;
}

RectI SidebarItemDip(int index) {
  RectI r;
  // ⚠ 整列,而且列距 = 列高(comctl32 的列之間沒有間隔)。
  //   起點與 SidebarListDip() 的 y 是同一個常數 —— 清單的第一列就從
  //   清單自己的 client 頂端開始。見標頭。
  r.x = 0;
  r.y = space::s5 + index * metric::kRowH;
  r.w = metric::kSidebarW;
  r.h = metric::kRowH;
  return r;
}

RectI SidebarItemFillDip(int index) {
  RectI r = SidebarItemDip(index);
  r.x += space::s5;
  r.w -= 2 * space::s5;
  if (r.w < 0) r.w = 0;
  // ── ⚠ 上下各縮 s1,好讓「項與項之間 s2(4)」真的看得見 ──────────
  //
  // §12.14.6.1 要求項與項之間留 s2(4)。**comctl32 做不到**:report 模式
  // 的列是連著的,列高由 image list 撐出來,列與列之間沒有間隔 ——
  // 而 ui_layout.h 的檔頭記著一次事故:舊版在**命中**那一份算式裡加了
  // 一個 +4 的列距,那個 4 在畫面上不存在,累積到第 5 項差 16 DIP,
  // 也就是 #75 使用者回報的「點得到的地方跟畫出來的地方差了半列」。
  //
  // 所以間隔只能做在**畫出來的那塊底**上:每一項的底上下各縮 2,
  // 相鄰兩塊底之間就有 4。命中範圍仍然是整列 36 —— 使用者按在那 2 DIP
  // 的縫上照樣換頁,不會有一道看不見的死區。
  r.y += space::s1;
  r.h -= 2 * space::s1;
  if (r.h < 0) r.h = 0;
  return r;
}

RectI SidebarIndicatorDip(int index) {
  // ⚠ **從 SidebarItemFillDip() 出**,不是從 SidebarItemDip()。
  //   底、圓角、指示條三者必須共用同一個矩形 —— 分開算的樣子是
  //   「指示條與底色的圓角對不齊」,而那看起來只是「做得不精緻」。
  const RectI f = SidebarItemFillDip(index);
  RectI r;
  r.x = f.x;  // x = 項目的左緣,**不縮排**
  r.w = metric::kIndicatorW;
  r.h = metric::kIndicatorH;
  r.y = f.y + (f.h - r.h) / 2;  // 垂直置中
  return r;
}

BadgePlacement BadgePlacementDip(int list_w_dip, int row_top_dip,
                                 int row_h_dip, int badge_text_w_dip,
                                 int border_dip, int scrollbar_w_dip,
                                 bool has_vscroll) {
  BadgePlacement p;
  // 高 = t5 的行盒 16 + 上下各 1 = 18(§12.14.6.5)。
  const int badge_h = TextLineBoxDip(text_size::t5) + 2;
  const int sb = has_vscroll ? (scrollbar_w_dip > 0 ? scrollbar_w_dip : 0) : 0;
  // ⚠ **從控制項的寬度算,不是從列矩形算。** 見標頭。
  const int text_col_right = list_w_dip - border_dip - sb - space::s5;
  const int badge_w = badge_text_w_dip + 2 * space::s3;
  p.badge.w = badge_w;
  p.badge.x = text_col_right - badge_w;
  p.badge.h = badge_h;
  p.badge.y = row_top_dip + (row_h_dip - badge_h) / 2;  // 列內置中
  // 膠囊:圓角 = 高 ÷ 2。**不是自由參數**。
  p.radius_dip = badge_h / 2;
  // 名稱截尾的右界與徽章左緣**由同一個算式產生**,所以長方案名不會
  // 壓到徽章上。
  p.name_right = p.badge.x - space::s3;
  return p;
}

RectI SidebarStatusDip(int window_h_dip) {
  RectI r;
  r.x = space::s5;
  r.y = window_h_dip - metric::kSidebarStatusH;
  r.w = metric::kSidebarW - 2 * space::s5;
  r.h = metric::kSidebarStatusH;
  return r;
}

RectI SidebarListDip(int window_h_dip) {
  RectI r;
  r.x = 0;
  r.y = space::s5;
  r.w = metric::kSidebarW;
  // ⚠ 高度要**同時**扣掉上面那段留白與底部狀態區。少扣上面那一段
  //   就是實機回報的那個缺陷:清單壓進狀態區 12 DIP。
  r.h = window_h_dip - metric::kSidebarStatusH - space::s5;
  if (r.h < 0) r.h = 0;
  return r;
}

int TextLineBoxDip(int size_dip) { return size_dip * 4 / 3 + 2; }

namespace {

// 這個碼點畫出來大約是一個字級寬(全形)嗎。
//
// ⚠ 只求「夠不夠寬」,不求精確:分錯一個字的代價是那一行多算/少算
//   0.4 個字級。分錯的方向要往**寬**的一邊 —— 多算一行是多一點空白,
//   少算一行是文字被無聲切掉。
bool IsWideCodePoint(unsigned int c) {
  return (c >= 0x1100u && c <= 0x115Fu) ||   // 韓文字母
         // ⚠ U+2010–U+2027:連字號、破折號、引號、刪節號、間隔號。
         //   舊表從 U+2E80 才開始,所以「——」與「…」被當成 0.6 個字級
         //   —— 而中文文案裡這兩個很常見,一段最多低估約 17.6 DIP,
         //   也就是整整一行。它們在中文字體裡是全形。
         (c >= 0x2010u && c <= 0x2027u) ||
         (c >= 0x2E80u && c <= 0x303Eu) ||   // 部首、注音、CJK 標點
         (c >= 0x3041u && c <= 0x33FFu) ||   // 假名、諺文、相容字
         (c >= 0x3400u && c <= 0x4DBFu) ||   // 擴充 A
         (c >= 0x4E00u && c <= 0x9FFFu) ||   // 基本區
         (c >= 0xA000u && c <= 0xA4CFu) ||
         (c >= 0xAC00u && c <= 0xD7A3u) ||   // 諺文音節
         (c >= 0xF900u && c <= 0xFAFFu) ||   // 相容表意
         (c >= 0xFE30u && c <= 0xFE6Fu) ||
         (c >= 0xFF00u && c <= 0xFF60u) ||   // 全形 ASCII
         (c >= 0xFFE0u && c <= 0xFFE6u);
}

// 這個字**之後**可以斷行嗎(它本身仍然屬於前面那一段)。
//
// DrawTextW 的 DT_WORDBREAK:拉丁文字斷在空白與連字號,中日韓字每一個
// 字之後都可以斷。中間那些黏在一起的字母是一個**不可斷開的詞**,而
// 「一個詞放不下就整個推到下一行」正是舊估算漏掉的那件事。
bool BreaksAfterCodePoint(unsigned int c) {
  return c == 0x20u ||   // 空白
         c == 0x09u ||   // tab
         c == 0x2Du ||   // ASCII 連字號
         IsWideCodePoint(c);
}

// 字寬表本身的誤差餘裕。
//
// ⚠ **這個分數不再負責「一個詞被推到下一行留下的空白」** ——
//   那件事現在是算出來的(見下面)。舊版用 6/7 當它的替代品,而
//   1/7 的欄寬在 440 DIP 的內容欄上只有 62 DIP = t5 底下 9.5 個拉丁
//   字母:比它長的詞一出現,留的空白就不夠,估算低於實際,而低估的
//   方向是文字被切掉。使用者看得到的英文文案裡就有 `administrator`
//   (13)與 `mid-sentence`(12)。
//
//   剩下要留的只有一件事:0.6 個字級是拉丁字母的**平均**寬度,不是
//   上界(大寫 W、M 接近 0.9)。這 1/16 是給那個誤差的。
constexpr int kWidthSlackNum = 15;
constexpr int kWidthSlackDen = 16;

}  // namespace

int EstimateTextLinesDip(const wchar_t* text, int size_dip, int w_dip) {
  if (!text || !*text || size_dip <= 0 || w_dip <= 0) return 1;
  // 單位是 1/100 DIP,免得整數除法把每一個字都抹掉一點。
  const long usable100 =
      static_cast<long>(w_dip) * 100 * kWidthSlackNum / kWidthSlackDen;
  if (usable100 <= 0) return 1;

  long x = 0;     // 這一行已經放了多寬
  long atom = 0;  // 還沒放上去的那一段不可斷開的字
  int lines = 1;

  // 把手上那個詞放到某一行上。
  //
  // ⚠ **不可以有「放不下就回 1 行」這種捷徑。** 舊版有一條
  //   `if (usable <= size_dip) return 1;`,於是 size=11 時欄寬 <= 13 的
  //   每一格都回 1 行,而 14 回 200 行 —— 欄越窄行數反而變少,
  //   而變少的方向是文字被無聲切掉。這裡的規則只有一條:
  //   **一行至少放一個東西**(x > 0 才換行),所以行數對欄寬一定單調。
  auto flush = [&] {
    if (atom <= 0) return;
    if (x > 0 && x + atom > usable100) {
      ++lines;
      x = 0;
    }
    x += atom;
    atom = 0;
    // 一個比整行還寬的詞(窄欄、或者根本沒有空白的長字串):
    // GDI 會把它硬切開,行數要跟著算。
    while (x > usable100) {
      ++lines;
      x -= usable100;
    }
  };

  for (const wchar_t* p = text; *p; ++p) {
    const unsigned int c = static_cast<unsigned int>(*p);
    if (c == 0x0Du) continue;
    if (c == 0x0Au) {  // 明寫的換行
      flush();
      ++lines;
      x = 0;
      continue;
    }
    atom += static_cast<long>(size_dip) * (IsWideCodePoint(c) ? 100 : 60);
    if (BreaksAfterCodePoint(c)) flush();
  }
  flush();
  return lines;
}

int EstimateTextBoxHeightDip(const wchar_t* text, int size_dip, int w_dip) {
  // ⚠ 內層擋了 size_dip <= 0(回 1 行),而這裡以前**沒擋** —— 它把那
  //   1 行直接乘上 TextLineBoxDip(size_dip):
  //     size = -11 → -12 DIP:高度是負的,Stack 往回縮,下一段疊上來;
  //     size = 0   →   2 DIP:那一段從畫面上消失,而且沒有任何錯誤。
  //   兩種都是「畫面壞掉,程式不吭聲」,而這一支在 WM_PAINT 路徑上。
  //
  // ⚠ 回 0 也不行 —— 那就是上面第二種。不合法的字級一律當成 t5:
  //   壞掉的那一段仍然佔一個看得見的位置,而不是把整頁的版面帶壞。
  const int size = size_dip > 0 ? size_dip : text_size::t5;
  return EstimateTextLinesDip(text, size, w_dip) * TextLineBoxDip(size);
}

int EstimateTextWidthDip(const wchar_t* text, int size_dip) {
  if (!text || !*text || size_dip <= 0) return 0;
  long w100 = 0;
  for (const wchar_t* p = text; *p; ++p) {
    const unsigned int c = static_cast<unsigned int>(*p);
    // 換行字元在這裡不換行:按鈕上的字是一行(見標頭)。
    if (c == 0x0Du || c == 0x0Au) continue;
    w100 += static_cast<long>(size_dip) * (IsWideCodePoint(c) ? 100 : 60);
  }
  // 餘裕:0.6 是拉丁字母的**平均**寬,不是上界(大寫 W、M 接近 0.9)。
  // 與 EstimateTextLinesDip 的 kWidthSlack 是同一個 1/16,方向相反 ——
  // 那一支縮欄寬,這一支撐字寬,兩邊都往「字比較寬」的一側錯。
  w100 = w100 * kWidthSlackDen / kWidthSlackNum;
  return static_cast<int>((w100 + 99) / 100);  // 無條件進位到整 DIP
}

int ButtonWidthDip(UiString label) {
  const int text_w = EstimateTextWidthDip(UiText(label), text_size::t4);
  // §12.14.6.2:左右內距 s5(12),最小寬 80。
  return std::max(metric::kButtonMinW, text_w + 2 * space::s5);
}

UiString SettingsButtonLabel(int id) {
  switch (id) {
    case IDC_UP:                return UiString::kSchemasMoveUp;
    case IDC_DOWN:              return UiString::kSchemasMoveDown;
    case IDC_APPLY_ORDER:       return UiString::kSchemasApplyOrder;
    case IDC_UPDATE_CHECK:      return UiString::kUpdateCheckButton;
    // ⚠ 這一顆的字是執行期換的(下載 / 安裝 / 重試),而版面要一個
    //   固定的寬。取**最長的那一句**:短的那幾句擺進去只是左右多一點
    //   內距,而反過來是字被截尾。
    case IDC_UPDATE_ACTION:     return UiString::kUpdateInstallButton;
    case IDC_UPDATE_PAGE:       return UiString::kUpdateOpenPageButton;
    case IDC_NETLOG_CLEAR:      return UiString::kNetLogClearButton;
    case IDC_REDEPLOY:          return UiString::kRedeployButton;
    case IDC_OPEN_USER_DIR:     return UiString::kOpenUserDir;
    case IDC_OPEN_SETTINGS_FILE: return UiString::kOpenSettingsFile;
    case IDC_DIAG_COPY:         return UiString::kDiagnosticsCopy;
    case IDC_RESET:             return UiString::kResetButton;
    default:                    return UiString::kUiStringCount;
  }
}

SchemaNoteText SchemaNoteLines(int note) {
  switch (note) {
    case kSchemaNoteLoading:
      return SchemaNoteText{UiString::kSchemasLoadingTitle,
                            UiString::kSchemasLoadingWhy,
                            UiString::kSchemasLoadingNext};
    case kSchemaNoteUnavailable:
      return SchemaNoteText{UiString::kSchemasUnavailableTitle,
                            UiString::kSchemasUnavailableWhy,
                            UiString::kSchemasUnavailableNext};
    case kSchemaNoteEmpty:
    default:
      return SchemaNoteText{UiString::kSchemasEmptyTitle,
                            UiString::kSchemasEmptyWhy,
                            UiString::kSchemasEmptyNext};
  }
}

UiString SettingsPageName(int page) {
  switch (page) {
    case kPageAppearance:
      return UiString::kNavAppearance;
    case kPageText:
      return UiString::kNavText;
    case kPageNetwork:
      return UiString::kNavNetwork;
    case kPageAdvanced:
      return UiString::kNavAdvanced;
    case kPageSchemas:
    default:
      return UiString::kNavSchemas;
  }
}

RectI SidebarStatusLineDip(int window_h_dip, int line) {
  const RectI strip = SidebarStatusDip(window_h_dip);
  RectI r;
  r.x = strip.x;
  r.w = strip.w;
  r.h = kSidebarStatusLineH;
  r.y = strip.y + space::s4 + line * (kSidebarStatusLineH + space::s1);
  return r;
}

RectI Stack::Push(int h_dip, int gap_dip) {
  RectI r;
  r.x = x_;
  r.y = y_;
  r.w = w_;
  r.h = h_dip;
  y_ += h_dip + gap_dip;
  return r;
}

RectI Stack::PushDivider() {
  y_ += space::s7;
  RectI r;
  r.x = x_;
  r.y = y_;
  r.w = w_;
  r.h = metric::kHairline;
  y_ += metric::kHairline + space::s7;
  return r;
}

// ────────────────────────────────────────────────────────────────
// 每一頁的版面
//
// ⚠ 這裡的每一行都曾經住在 service/settings_window.cc::LayoutUi() 裡,
//   而那個位置測不到。搬過來的時候**一格都沒有動** —— 唯一的差別是
//   下面那三顆固定寬度的按鈕(重新整理字詞 / 複製診斷 / 重設全部設定)
//   現在會推進堆疊。舊版寫的是
//
//       place(IDC_RESET, RectI{cx, st.y(), 220, btn_h});   // 之後不管了
//
//   高度不進堆疊 = 不進 content_h_dip = 不進捲動範圍。捲到底,
//   最後那顆危險鍵仍然差 32 DIP 碰不到,而且**沒有任何東西會發現**。
// ────────────────────────────────────────────────────────────────

PageLayout LayoutSettingsPageDip(int page, int window_w_dip,
                                 PageState state) {
  PageLayout out;
  const int cx = ContentXDip(window_w_dip);
  const int cw = ContentWidthDip(window_w_dip);
  // 卡片內寬:卡片左右各 s6 的內距(§12.14.5)。
  // ⚠ 卡片**不往外撐**。往外撐的算式在最小視窗(660)下會把卡片的左緣
  //   推到 x = 194 —— 也就是**側欄裡面**。那是「在我這台機器上看起來
  //   很好」的典型形狀:預設 780 寬時它是對的。
  const int inner_w = cw - 2 * space::s6;
  // ── ⚠ 一行說明文字的**盒高** ──────────────────────────────
  //
  // 行盒高度只有一個來源:TextLineBoxDip()。
  const int t5h = TextLineBoxDip(text_size::t5);
  // §12.14.6.2:按鈕高 32。以前是 kMinTarget + s2,值一樣但那是巧合。
  //
  // ⚠ **按鈕一律走 card_block,不走 card_row。**
  //   card_row 帶 36 的下限（卡片裡「一列」的最小高）,而按鈕不是「一列」
  //   —— 用它推的話 32 會被墊成 36,而底部固定列的「關閉」仍然是 32:
  //   同一個視窗上兩種按鈕高度。這正是 1a711a2 → 54e4b4d 之間跑掉的那一格
  //   （基底寫的是 st.Push(btn_h, gap),直接推 32）。
  //   ⚠ W3 抓不到它:32 與 36 都在允許的字面值集合裡。守門在
  //   tests/test_ui_layout.cc 的
  //   ui_layout_one_window_has_exactly_one_button_height。
  const int btn_h = metric::kButtonH;

  Stack st(cx, kContentTopDip, cw);

  auto emit = [&](int id, const RectI& r, bool clickable, const char* what,
                  int size_dip, int lines, bool in_card) {
    out.items.push_back(
        PlacedControl{id, r, clickable, what, size_dip, lines, in_card});
  };
  // 這一頁上不出現的控制項:空矩形。呼叫端據此隱藏 ——
  // 「建了但不屬於任何一頁」在結構上不可能發生,靠的就是這一份清單
  //  是**唯一**的來源。
  auto hide = [&](int id) {
    emit(id, RectI{}, false, "not_on_this_page", 0, 0, false);
  };

  // ── ⚠ 高度從**字**算出來,不是各自寫死 ──────────────────────
  //
  // #76 的根因就是「每一段各自寫死行數」。單看每一段都很寬鬆合理,
  // 六段加起來多了 195 DIP,而使用者付的代價是一顆碰不到的按鈕。
  auto lines_of = [&](UiString s, int w) {
    return EstimateTextLinesDip(UiText(s), text_size::t5, w);
  };
  auto text_h = [&](UiString s) {
    return EstimateTextBoxHeightDip(UiText(s), text_size::t5, cw);
  };
  // 文字要到執行期才知道的那幾格,取候選裡最高的那一個。
  auto lines_max = [&](UiString a, UiString b, int w) {
    return std::max(lines_of(a, w), lines_of(b, w));
  };

  // ── 卡片(§12.14.5)────────────────────────────────────────────
  //
  // ⚠ 卡片是一個**輸出**,不是一個新的座標系。卡內控制項仍然由同一個
  //   Stack 推出來,只是左右各縮 s6 —— 所以 W18/W31' 量的還是同一批矩形。
  int card_top = -1;
  std::vector<int> card_divs;
  auto card_begin = [&]() {
    card_top = st.y();
    card_divs.clear();
    st.Skip(space::s4);  // 上內距
  };
  // 卡內一列。⚠ 最小高 36(= 一個點擊目標 + 餘裕),規格明文。
  // ⚠ **按鈕不要用這一支** —— 見上面 btn_h 那一段。這一支是給開關、
  //   單選鈕、清單列那種「一列」用的;按鈕的高度由 §12.14.6.2 釘死,
  //   被墊高的話同一個視窗上會出現兩種按鈕高度。
  auto card_row = [&](int h_dip, int gap_dip) {
    const RectI row = st.Push(h_dip < metric::kRowH ? metric::kRowH : h_dip,
                              gap_dip);
    return RectI{row.x + space::s6, row.y, inner_w, row.h};
  };
  // 卡內一列,但**不**吃 36 的下限(整段說明文字、清單這種本來就高的)。
  auto card_block = [&](int h_dip, int gap_dip) {
    const RectI row = st.Push(h_dip, gap_dip);
    return RectI{row.x + space::s6, row.y, inner_w, row.h};
  };
  auto card_divider = [&]() { card_divs.push_back(st.y()); };
  auto card_end = [&](int gap_after) {
    st.Skip(space::s4);  // 下內距
    CardRect c;
    c.rect = RectI{cx, card_top, cw, st.y() - card_top};
    c.divider_ys = card_divs;
    out.cards.push_back(c);
    st.Skip(gap_after);
    card_top = -1;
  };

  // ── 頁首與區段標題**不在卡片裡** ──────────────────────────────
  //
  // Win11 的設定介面就是這個形狀:區段標題坐在卡片群組的上面,不在裡面。
  // 它也讓說明文字拿到整個內容欄的寬度 —— 塞進卡片的話,每一段說明都會
  // 少 32 DIP,而少的那一格會變成多一行,再變成一顆碰不到的按鈕(#76)。
  auto title_block = [&](int title_id, int sub_id, UiString sub) {
    // ⚠ 從 `t1 + s3`(28)改成 TextLineBoxDip(t1)(31)。§12.14.0 第 4 條:
    //   STATIC 是 SS_LEFT(頂端對齊)並且會裁切,短的那 3 DIP 是
    //   **每一個頁標題的下緣被削掉一條**。
    emit(title_id, st.Push(TextLineBoxDip(text_size::t1), space::s1), false,
         "page_title", text_size::t1, 1, false);
    emit(sub_id, st.Push(text_h(sub), space::s7), false, "page_subtitle",
         text_size::t5, lines_of(sub, cw), false);
  };
  auto heading = [&](int head_id, int blurb_id, UiString blurb) {
    // ⚠ 同上:`t2 + s2`(19)→ TextLineBoxDip(t2)(22)。
    emit(head_id, st.Push(TextLineBoxDip(text_size::t2), space::s1), false,
         "section_heading", text_size::t2, 1, false);
    if (blurb_id)
      emit(blurb_id, st.Push(text_h(blurb), space::s3), false, "section_blurb",
           text_size::t5, lines_of(blurb, cw), false);
  };
  // 單選群組:一列一個(桌面欄的密度)。
  // ⚠ 每一顆的 id 都**寫出來**,不用 `first + i`。理由是守門:
  //   check_ui_spec.sh 的 W24 逐字比對這裡與 settings_window.cc 的
  //   kControls,而 `first + i` 讓一半的 id 在文字上不存在。
  // ⚠ 整列高從 kMinTarget(28)改成 kRowH(36)——§12.14.6.6 的表。
  auto radios = [&](std::initializer_list<int> ids, const char* what) {
    card_begin();
    int i = 0;
    const int n = static_cast<int>(ids.size());
    for (int id : ids) {
      ++i;
      emit(id, card_row(metric::kRowH, i == n ? 0 : space::s1), true, what,
           text_size::t3, 1, true);
      if (i != n) card_divider();
    }
    card_end(space::s7);
  };
  // ── 一列按鈕:**依內容寬**,由左往右,彼此 s3 ────────────────
  //
  // ⚠ 舊版是 `bw = (inner_w - 2*s3) / 3`,也就是把整個內容欄平均切開。
  //   那不是工具列,是對話框的按鈕列 —— 而且視窗一拉寬,「上移」這兩個
  //   字底下的按鈕就跟著長到 300 DIP。§12.14.6.2/.3 的「左右內距 s5、
  //   最小寬 80」在那個算式底下從來沒有機會生效。
  struct Btn {
    int id;
    const char* what;
  };
  // 這一列按鈕排在 (x0, y),回傳整列的總寬。
  auto place_buttons = [&](int x0, int y, std::initializer_list<Btn> bs) {
    int x = x0;
    for (const Btn& b : bs) {
      const int bw = ButtonWidthDip(SettingsButtonLabel(b.id));
      emit(b.id, RectI{x, y, bw, btn_h}, true, b.what, 0, 0, true);
      x += bw + space::s3;
    }
    return bs.size() ? x - space::s3 - x0 : 0;
  };
  // 這一列按鈕要多寬(還沒擺之前就要知道,靠右對齊時要用)。
  auto buttons_width = [&](std::initializer_list<Btn> bs) {
    int w = 0;
    for (const Btn& b : bs) w += ButtonWidthDip(SettingsButtonLabel(b.id));
    if (bs.size() > 1) w += (static_cast<int>(bs.size()) - 1) * space::s3;
    return w;
  };
  // 卡片裡靠左的一列按鈕。⚠ **一定要推進堆疊**。
  // ⚠ card_block 而不是 card_row:按鈕不吃 36 的下限（見 btn_h 那一段）。
  auto card_buttons = [&](std::initializer_list<Btn> bs, int gap_after) {
    const RectI row = card_block(btn_h, gap_after);
    place_buttons(row.x, row.y, bs);
  };

  // ── §12.14.6.9:「標籤在左、按鈕在右」的一張卡 ────────────────
  //
  // ⚠ **這一支取代了「heading() 在卡片外面 + 卡片裡只放一顆按鈕」。**
  //   舊版那張卡是一個整行寬的空盒:進階頁的「重新整理字詞」是
  //   540×52,而按鈕只佔左邊 180 —— 右邊 360 DIP 全是空白,
  //   而「這個畫面看起來沒做完」最大的單一來源就是它。
  //   對照組(Win11 設定)在同一塊空間裡放的是**標籤 + 一排按鈕**,
  //   而我們的標籤本來就有:它是原本浮在卡片上面的那個區段標題。
  //   所以修法不是「發明一個新標籤」,是**把區段標題與說明搬進卡片的左欄**
  //   —— 順便消掉「一個標題浮在一個空盒上面」那個形狀。
  //
  // ⚠ 兩種排法,由**寬度**決定,不是由頁決定:
  //     · 標籤欄 >= 按鈕欄  → 左右並排(對照組的樣子);
  //     · 否則              → 上下堆疊(標籤整行寬,按鈕自己一列靠左)。
  //   判準寫成「標籤欄不得比按鈕欄窄」而不是一個新的常數:窄過頭的時候
  //   那一段說明會被擠成七八行,卡片會比原本那個空盒還高 ——
  //   也就是修完比修之前難看。660 的最小視窗、以及英文介面(按鈕字比較長)
  //   都會走到堆疊那一支,兩支都有測試走過。
  auto action_card = [&](int head_id, int blurb_id, UiString blurb,
                         std::initializer_list<Btn> bs, int gap_after) {
    const int head_h = TextLineBoxDip(text_size::t2);
    const int btn_w = buttons_width(bs);
    const int label_w = inner_w - btn_w - space::s6;
    card_begin();
    if (label_w >= btn_w && label_w > 0) {
      const int nl = lines_of(blurb, label_w);
      const int label_h = head_h + space::s1 + nl * t5h;
      const int row_h = std::max(label_h, btn_h);
      const RectI row = card_block(row_h, 0);
      emit(head_id, RectI{row.x, row.y, label_w, head_h}, false,
           "section_heading", text_size::t2, 1, true);
      emit(blurb_id,
           RectI{row.x, row.y + head_h + space::s1, label_w, nl * t5h}, false,
           "section_blurb", text_size::t5, nl, true);
      // 按鈕靠右、垂直置中。⚠ 靠右對齊的右緣是**卡片內寬的右緣**,
      //   不是內容欄的右緣 —— 兩者差 s6,而差那 16 DIP 的樣子是
      //   「按鈕比卡片還往外凸一點」。
      place_buttons(row.x + inner_w - btn_w, row.y + (row_h - btn_h) / 2, bs);
    } else {
      emit(head_id, card_block(head_h, space::s1), false, "section_heading",
           text_size::t2, 1, true);
      const int nl = lines_of(blurb, inner_w);
      emit(blurb_id, card_block(nl * t5h, space::s3), false, "section_blurb",
           text_size::t5, nl, true);
      card_buttons(bs, 0);
    }
    card_end(gap_after);
  };

  switch (page) {
    case kPageSchemas: {
      title_block(IDC_SCHEMAS_TITLE, IDC_SCHEMAS_SUB,
                  UiString::kSchemasSubtitle);
      heading(IDC_SCHEMAS_LIST_HEAD, IDC_SCHEMAS_LIST_BLURB,
              UiString::kSchemasListBlurb);
      const int list_h = 4 * metric::kRowH + space::s3;
      card_begin();
      if (state.schema_list_empty) {
        hide(IDC_SCHEMA_LIST);
        hide(IDC_UP);
        hide(IDC_DOWN);
        hide(IDC_APPLY_ORDER);
        hide(IDC_SCHEMAS_DEFAULT_LINE);
        // ── ⚠ 高度從**這一格自己的字**算,不是寫死 4 行 ────────
        const SchemaNoteText note = SchemaNoteLines(state.schema_note);
        const int nl =
            EstimateTextLinesDip(UiText(note.title), text_size::t5, inner_w) +
            EstimateTextLinesDip(UiText(note.why), text_size::t5, inner_w) +
            EstimateTextLinesDip(UiText(note.next), text_size::t5, inner_w);
        emit(IDC_SCHEMAS_EMPTY, card_block(nl * t5h, 0), false, "empty_state",
             text_size::t5, nl, true);
      } else {
        hide(IDC_SCHEMAS_EMPTY);
        emit(IDC_SCHEMA_LIST, card_block(list_h, space::s3), true,
             "schema_list", 0, 0, true);
        // ⚠ 清單底下的一排工具列按鈕:**靠左、依內容寬**。
        //   舊版把它們平均切成三份各 165 DIP,而那正是「像對話框」
        //   的樣子 —— 工具列的按鈕跟著字走,不跟著容器走。
        card_buttons({{IDC_UP, "move_up"},
                      {IDC_DOWN, "move_down"},
                      {IDC_APPLY_ORDER, "apply_order"}},
                     space::s3);
        emit(IDC_SCHEMAS_DEFAULT_LINE, card_block(t5h, 0), false,
             "default_line", text_size::t5, 1, true);
      }
      card_end(space::s7);

      card_begin();
      emit(IDC_FOLLOW_MODE, card_row(metric::kRowH, space::s1), true,
           "follow_input_mode_switch", text_size::t3, 1, true);
      emit(IDC_FOLLOW_BLURB,
           card_block(lines_of(UiString::kSchemasFollowBlurb, inner_w) * t5h,
                      0),
           false, "follow_blurb", text_size::t5,
           lines_of(UiString::kSchemasFollowBlurb, inner_w), true);
      card_end(space::s7);
      break;
    }
    case kPageAppearance: {
      title_block(IDC_APPEAR_TITLE, IDC_APPEAR_SUB,
                  UiString::kAppearanceSubtitle);
      heading(IDC_COUNT_HEAD, IDC_COUNT_BLURB, UiString::kCountBlurb);
      radios({IDC_COUNT_0, IDC_COUNT_1, IDC_COUNT_2, IDC_COUNT_3, IDC_COUNT_4},
             "cand_count_radio");
      heading(IDC_SCALE_HEAD, IDC_SCALE_BLURB, UiString::kScaleBlurb);
      radios({IDC_SCALE_0, IDC_SCALE_1, IDC_SCALE_2, IDC_SCALE_3, IDC_SCALE_4},
             "cand_scale_radio");
      heading(IDC_THEME_HEAD, IDC_THEME_BLURB, UiString::kThemeBlurb);
      radios({IDC_THEME_0, IDC_THEME_1, IDC_THEME_2}, "appearance_radio");
      heading(IDC_BAR_HEAD, IDC_BAR_BLURB, UiString::kStatusBarBlurb);
      card_begin();
      emit(IDC_BAR_SHOW, card_row(metric::kRowH, 0), true, "status_bar_switch",
           text_size::t3, 1, true);
      card_end(space::s7);
      emit(IDC_APPEAR_NOTE, st.Push(text_h(UiString::kAppearanceHonestNote), 0),
           false, "honest_note", text_size::t5,
           lines_of(UiString::kAppearanceHonestNote, cw), false);
      break;
    }
    case kPageText: {
      title_block(IDC_TEXT_TITLE, IDC_TEXT_SUB, UiString::kTextSubtitle);
      heading(IDC_VARIANT_HEAD, IDC_VARIANT_BLURB, UiString::kVariantBlurb);
      radios({IDC_VARIANT_0, IDC_VARIANT_1, IDC_VARIANT_2}, "variant_radio");
      heading(IDC_PUNCT_HEAD, IDC_PUNCT_BLURB, UiString::kPunctBlurb);
      radios({IDC_PUNCT_0, IDC_PUNCT_1, IDC_PUNCT_2}, "punct_radio");
      // 全／半形(G70)。⚠ 擺在標點**後面**、輕點 Shift 前面。
      heading(IDC_SHAPE_HEAD, IDC_SHAPE_BLURB, UiString::kShapeBlurb);
      radios({IDC_SHAPE_0, IDC_SHAPE_1, IDC_SHAPE_2}, "shape_radio");
      // 輕點 Shift 切中英(#89)。
      heading(IDC_SHIFTTAP_HEAD, IDC_SHIFTTAP_BLURB, UiString::kShiftTapBlurb);
      card_begin();
      emit(IDC_SHIFTTAP_SWITCH, card_row(metric::kRowH, 0), true,
           "shift_tap_switch", text_size::t3, 1, true);
      card_end(space::s7);
      break;
    }
    case kPageNetwork: {
      title_block(IDC_NET_TITLE, IDC_NET_SUB, UiString::kNetworkSubtitle);
      // ⚠ 開關**沒有**自己的區段標題。頁標題已經是「連網」。
      card_begin();
      emit(IDC_NET_SWITCH, card_row(metric::kRowH, space::s1), true,
           "network_switch", text_size::t3, 1, true);
      card_divider();
      // 開著一句、關著一句,哪一句由 NetSwitchSummary() 決定 —— 兩句
      // 都可能出現在同一個框裡,所以取**比較高的那一句**。
      {
        const int nl = lines_max(UiString::kNetworkOnSummary,
                                 UiString::kNetworkOffSummary, inner_w);
        emit(IDC_NET_STATE, card_block(nl * t5h, space::s3), false,
             "network_state", text_size::t5, nl, true);
      }
      // 誠實的代價(DNS/SNI 是明文)。⚠ 這一段**開著關著都在**。
      {
        const int nl = lines_of(UiString::kNetworkOnDetail, inner_w);
        emit(IDC_NET_DETAIL, card_block(nl * t5h, 0), false,
             "network_cost_note", text_size::t5, nl, true);
      }
      card_end(space::s7);

      // ── 更新 ──────────────────────────────────────────────
      //
      // ⚠ IDC_UPDATE_TRUST(沒有數位簽章那一句)排在**按鈕之前**。
      heading(IDC_UPDATE_HEAD, IDC_UPDATE_BLURB, UiString::kUpdateBlurb);
      card_begin();
      {
        const int nl = lines_of(UiString::kUpdateTrustAnchor, inner_w);
        emit(IDC_UPDATE_TRUST, card_block(nl * t5h, space::s3), false,
             "update_trust_anchor", text_size::t5, nl, true);
      }
      {
        const int nl = lines_of(UiString::kUpdateWhatHappens, inner_w);
        emit(IDC_UPDATE_WHAT, card_block(nl * t5h, space::s3), false,
             "update_what_happens", text_size::t5, nl, true);
      }
      // ⚠ 這一格的那句話是執行期依結果組出來的(五種狀態 + 幾種失敗),
      //   所以仍然是固定行數,而且抓最長的那一種還要再寬一點。
      emit(IDC_UPDATE_STATUS, card_block(t5h * 3, space::s3), false,
           "update_status", text_size::t5, 3, true);
      card_buttons({{IDC_UPDATE_CHECK, "update_check"},
                    {IDC_UPDATE_ACTION, "update_action"},
                    {IDC_UPDATE_PAGE, "update_page"}},
                   0);
      card_end(space::s7);

      heading(IDC_NETLOG_HEAD, IDC_NETLOG_BLURB, UiString::kNetLogBlurb);
      if (state.net_log_empty) {
        hide(IDC_NETLOG_SUMMARY);
        hide(IDC_NETLOG_COLS);
        hide(IDC_NETLOG_LIST);
        card_begin();
        // §4.7 的空狀態:為什麼是空的、這是不是正常。
        emit(IDC_NETLOG_EMPTY, card_block(t5h * 4, space::s3), false,
             "empty_state", text_size::t5, 4, true);
        emit(IDC_NETLOG_PATH, card_block(t5h * 2, 0), false, "log_file_path",
             text_size::t6, 2, true);
        // ⚠ 這是這一頁的最後一張卡(空紀錄時沒有危險區塊),所以**不留
        //   尾巴的 s7** —— 留了的話捲動範圍會多出一段永遠是空白的高度,
        //   而那看起來像「底下還有東西沒載入」。
        card_end(0);
        // ⚠ 沒有東西可以清的時候**不給**清除鍵。
        hide(IDC_NETLOG_CLEAR_HEAD);
        hide(IDC_NETLOG_CLEAR_BLURB);
        hide(IDC_NETLOG_CLEAR);
      } else {
        hide(IDC_NETLOG_EMPTY);
        card_begin();
        emit(IDC_NETLOG_SUMMARY, card_block(t5h, space::s3), false, "log_count",
             text_size::t5, 1, true);
        emit(IDC_NETLOG_COLS, card_block(t5h, space::s2), false, "log_columns",
             text_size::t5, 1, true);
        emit(IDC_NETLOG_LIST, card_block(6 * metric::kRowH + space::s3,
                                         space::s3),
             true, "net_log_list", 0, 0, true);
        emit(IDC_NETLOG_PATH, card_block(t5h * 2, 0), false, "log_file_path",
             text_size::t6, 2, true);
        // ⚠ 尾巴不留 s7:下一行的 PushDivider() 自己就會先留 s7。
        //   兩邊都留的話那條分隔線會離上面那張卡 40 DIP、離下面 20,
        //   看起來像它屬於下面那一段 —— 而它分的是「上面結束了」。
        card_end(0);
        // ⚠ 危險操作一律是該頁最後一個區塊,上面隔一條 hairline + s7
        //   (§4.9 / §2-C2)。
        st.PushDivider();
        action_card(IDC_NETLOG_CLEAR_HEAD, IDC_NETLOG_CLEAR_BLURB,
                    UiString::kNetLogClearBlurb,
                    {{IDC_NETLOG_CLEAR, "clear_log_button"}}, 0);
      }
      break;
    }
    case kPageAdvanced: {
      title_block(IDC_ADV_TITLE, IDC_ADV_SUB, UiString::kAdvancedSubtitle);
      action_card(IDC_REDEPLOY_HEAD, IDC_REDEPLOY_BLURB,
                  UiString::kRedeployBlurb,
                  {{IDC_REDEPLOY, "redeploy_button"}}, space::s7);
      action_card(IDC_FILES_HEAD, IDC_FILES_BLURB, UiString::kFilesBlurb,
                  {{IDC_OPEN_USER_DIR, "open_user_dir"},
                   {IDC_OPEN_SETTINGS_FILE, "open_settings_file"}},
                  space::s7);
      heading(IDC_LANG_HEAD, IDC_LANG_BLURB, UiString::kLanguageBlurb);
      radios({IDC_LANG_0, IDC_LANG_1, IDC_LANG_2, IDC_LANG_3},
             "ui_language_radio");
      heading(IDC_DIAG_HEAD, IDC_DIAG_NOTE, UiString::kDiagnosticsNote);
      card_begin();
      emit(IDC_DIAG, card_block(t5h * 6, space::s3), true, "diagnostics_edit",
           0, 0, true);
      card_buttons({{IDC_DIAG_COPY, "diagnostics_copy"}}, 0);
      card_end(0);
      // ⚠ 危險操作一律是該頁最後一個區塊,上面隔一條 hairline + s7。
      st.PushDivider();
      action_card(IDC_RESET_HEAD, IDC_RESET_BLURB, UiString::kResetBlurb,
                  {{IDC_RESET, "reset_button"}}, 0);
      break;
    }
    default:
      break;
  }

  out.content_h_dip = st.y() + kContentPadBottomDip;
  return out;
}

int ContentViewportHeightDip(int window_h_dip) {
  return std::max(0, window_h_dip - kBottomStripH);
}

int ContentClipLineDip(int window_h_dip, int scroll_dip, int scroll_max_dip) {
  const int vp = ContentViewportHeightDip(window_h_dip);
  // 已經捲到底、或者根本不用捲 → 沒有淡出區,裁在可視高度上。
  // ⚠ `>=` 不是 `==`:呼叫端的 scroll_ 可能因為換頁/換 DPI 暫時超出
  //   上限,而那一格不該讓淡出區憑空出現一次又消失。
  if (scroll_max_dip <= 0 || scroll_dip >= scroll_max_dip) return vp;
  // 視窗矮到淡出區比可視高度還高的時候不能回負數 —— 那會讓每一顆
  // 控制項都被判成「跨過裁切線」而整頁空白。
  return std::max(0, vp - kScrollFadeH);
}

int ScrollMaxDip(int page, int window_w_dip, int window_h_dip,
                 PageState state) {
  const PageLayout pl = LayoutSettingsPageDip(page, window_w_dip, state);
  return std::max(0, pl.content_h_dip - ContentViewportHeightDip(window_h_dip));
}

ScrolledPlacement ScrollPlaceControlDip(const RectI& content_rect,
                                        int scroll_dip, int clip_line_dip) {
  ScrolledPlacement p;
  p.y_dip = content_rect.y - scroll_dip;
  // ⚠ 只裁不藏。這一行就是整個捲動能不能給鍵盤使用者用的分界。
  p.visible = true;
  p.clip_h_dip = -1;
  // 子視窗本來就會被父視窗的 client 矩形裁掉,所以**上方**不必處理。
  // 要處理的只有下方那 54 DIP:底部固定列與它上面那條 hairline ——
  // 那一塊仍然在 client 裡面,捲到一半的控制項會畫在「關閉」鈕上面。
  //
  // ⚠ **全有或全無。** 舊版回的是 `max(0, clip_line - y)`,也就是
  //   半顆控制項:預設尺寸下文字頁的「全形/半形」區段標題在 y=497、
  //   高 22,裁切線 506 —— 它被畫成 9 DIP 高,從字的中間橫著切過去。
  //   那是使用者說的「摺線那裡看起來像壞掉」。半個字沒有任何辦法
  //   被讀成「還有東西可以捲」,所以現在跨線的控制項整顆不畫,
  //   而「還有更多」由淡出區(kScrollFadeH)與捲軸去說。
  if (!content_rect.empty() && p.y_dip + content_rect.h > clip_line_dip)
    p.clip_h_dip = 0;
  return p;
}

std::vector<HitTarget> ClickableTargetsDip(int window_w_dip, int window_h_dip,
                                           int page, PageState state) {
  std::vector<HitTarget> out;
  // 側欄的每一頁(不捲動)。
  for (int i = 0; i < kPageCount; ++i)
    out.push_back(HitTarget{"sidebar_item", SidebarItemDip(i), IDC_SIDEBAR,
                            false});
  // 底部固定列的關閉鈕(不捲動)。它與 settings_window.cc 用的是同一組
  // 常數 —— 那一列的位置是唯一還由呼叫端算的東西,所以它必須在這裡驗。
  // ⚠ 高 32,不是 kMinTarget(28)。§12.14.6.8:「關閉」是**次要按鈕**,
  //   而按鈕高在 §12.14.6.2/.3 是 32 —— 28 那一版比同一頁上其他按鈕矮
  //   4 DIP,而那正是「看起來像對話框」的來源之一。
  out.push_back(HitTarget{"close_button",
                          RectI{window_w_dip - space::s7 - 100,
                                window_h_dip - kBottomBarH, 100,
                                metric::kButtonH},
                          IDC_CLOSE, false});

  const PageLayout pl = LayoutSettingsPageDip(page, window_w_dip, state);
  for (const PlacedControl& p : pl.items) {
    if (!p.clickable || p.rect.empty()) continue;
    out.push_back(HitTarget{p.what, p.rect, p.id, true});
  }
  return out;
}

}  // namespace rimewin
