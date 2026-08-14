// windows/tests/test_ui_layout.cc — 設定視窗版面(W18 / W19 / W24)與單位換算(W1)
//
// 這幾條在 docs/ui-design.md §12.12 裡是「真的單元測試」那一組:
// 驗的是邏輯,不是字串。**一條都不需要真的 Windows。**
//
// ⚠ 2026-08-10 之前,這個檔案裡的 W18 是**假綠的**:它量的是
//   ClickableTargetsDip 手工造的一份「輸入方案頁」假骨架
//   (不呼叫任何真正的版面),而且只斷言 w/h >= 28。
//   於是外觀頁的深淺三態排在 y=574/604/634、視窗可視高度只有 506,
//   那三顆單選鈕在畫面上根本不存在 —— 而這一頁全綠。
//   現在每一條都走 LayoutSettingsPageDip(),而且**每一頁**都走。

#include "../common/ui_dip.h"
#include "../common/ui_layout.h"
#include "../common/ui_strings.h"

#include <map>
#include <set>
#include <string>
#include <vector>

#include "check.h"

using namespace rimewin;

// ── W1:單位換算 ─────────────────────────────────────────────────

TEST(dip_muldiv_rounds_instead_of_truncating) {
  // §12.3 的例子:150% 下 11 DIP → 16.5 → **17**,不是 16。
  // 舊寫法 static_cast<int>(11 * 1.5) 給的是 16,而誤差沿著版面累積。
  CHECK_INT(Dip(11, 144), 17);
  CHECK_INT(static_cast<int>(11 * 1.5), 16);  // 記下舊寫法錯在哪

  // 100% 下恆等。
  CHECK_INT(Dip(200, 96), 200);
  CHECK_INT(Dip(0, 96), 0);

  // 175%(168 DPI)那一組,逐項算過:
  CHECK_INT(Dip(20, 168), 35);   // 35.0
  CHECK_INT(Dip(36, 168), 63);   // 63.0
  CHECK_INT(Dip(13, 168), 23);   // 22.75 → 23
  CHECK_INT(Dip(200, 168), 350);

  // 125%(120 DPI)。
  CHECK_INT(Dip(11, 120), 14);   // 13.75 → 14
  CHECK_INT(Dip(6, 120), 8);     // 7.5 → 8(.5 遠離零)
}

TEST(dip_font_height_is_negative) {
  // ⚠ lfHeight 負值 = 字元高度。正值是儲存格高度,字會小一截,
  //   而那個差別在截圖上看起來只是「字有點小」。
  CHECK_INT(FontHeightFromDip(13, 96), -13);
  CHECK_INT(FontHeightFromDip(22, 144), -33);
  CHECK(FontHeightFromDip(11, 168) < 0);
}

TEST(dip_muldiv_denominator_zero_is_not_a_crash) {
  CHECK_INT(MulDivRound(10, 10, 0), -1);
}

// ── W19:§12.4.2 的三組驗算數字 ──────────────────────────────────

TEST(ui_layout_content_column_three_worked_cases) {
  // W=780 → content_w=540, content_x=220
  CHECK_INT(ContentWidthDip(780), 540);
  CHECK_INT(ContentXDip(780), 220);

  // W=660(最小尺寸)→ content_w=440, content_x=210
  // 左右內距被壓到 10 —— 內容欄的 440 下界優先於邊距,這是刻意的。
  CHECK_INT(ContentWidthDip(660), 440);
  CHECK_INT(ContentXDip(660), 210);

  // W=1000 → content_w=640, content_x=280(兩側留白)
  CHECK_INT(ContentWidthDip(1000), 640);
  CHECK_INT(ContentXDip(1000), 280);
}

TEST(ui_layout_content_column_never_leaves_the_window) {
  // 掃一遍合理的寬度區間:內容欄不可以超出視窗,也不可以壓到側欄。
  // ⚠ 從 640 起掃,不是從 kWindowMinW(660):視窗有垂直捲軸,而
  //   AdjustWindowRectEx 不算它 —— client 會比視窗窄 17 DIP 左右。
  //   640 是 cx >= 200 的臨界點(avail = 440 = 內容欄下界),
  //   低於它內容欄就開始往左吃側欄。
  for (int w = 640; w <= 2400; w += 7) {
    const int cx = ContentXDip(w);
    const int cw = ContentWidthDip(w);
    CHECK(cx >= metric::kSidebarW);
    CHECK(cw >= kContentMinW);
    CHECK(cw <= kContentMaxW);
    // ⚠ 這一條是重點:視窗比 840 寬時內容欄置中,不可以因為
    //   (avail - cw) 是奇數而多算一格跑出右邊界。
    CHECK(cx + cw <= w);
  }
}

TEST(ui_layout_sidebar_items_do_not_overlap_the_status_area) {
  // 側欄最多列七頁(§5.3 的桌面端頁數)。第七頁的底不可以蓋到狀態區。
  const RectI last = SidebarItemDip(6);
  const RectI status = SidebarStatusDip(kWindowDefaultH);
  CHECK(last.bottom() <= status.y);
  // 最小高度下也要成立 —— 那是最容易撞到的情形。
  const RectI status_min = SidebarStatusDip(kWindowMinH);
  CHECK(last.bottom() <= status_min.y);
}

TEST(ui_layout_sidebar_items_are_evenly_spaced) {
  const RectI a = SidebarItemDip(0);
  const RectI b = SidebarItemDip(1);
  // ⚠ 列與列之間**沒有間隔**。comctl32 就是這樣排的,而以前這裡釘的是
  //   `space::s2`(4 DIP)—— 一個畫面上不存在、只存在於我們這份算式裡的
  //   間隔。它就是 #75 那 16 DIP 的來源。
  CHECK_INT(b.y - a.bottom(), 0);
  CHECK_INT(a.h, metric::kRowH);
  CHECK_INT(a.w, metric::kSidebarW);
  CHECK_INT(a.x, 0);
}

// ── #75:點得到的地方與畫出來的地方必須是**同一組矩形** ────────────
//
// 使用者回報「點得到的地方跟畫出來的地方差 16 DIP」。那個 16 不是隨便一個
// 數字,它是 4 項 × 一項差 4 DIP 累積出來的 —— 而 4 正是舊版憑空多加的
// 那個列距。這一條把兩邊接回同一個來源:
//
//   · 畫的那一邊:側欄是真的 ListView,第 i 列在
//     SidebarListDip(H).y + i × 列高(列高由 SetRowListRowHeight 釘死)。
//   · 點的那一邊:ClickableTargetsDip() 回的 sidebar_item。
//
// ⚠ 這一條在舊版上是紅的,而且從第 1 項就開始紅。
TEST(ui_layout_sidebar_hit_and_draw_are_the_same_rect) {
  for (int H : {kWindowMinH, kWindowDefaultH, 700, 1000}) {
    const RectI list = SidebarListDip(H);
    const std::vector<HitTarget> targets =
        ClickableTargetsDip(kWindowDefaultW, H, kPageSchemas,
                            PageState{false, true});
    int seen = 0;
    for (int i = 0; i < kPageCount; ++i) {
      // comctl32 排出來的那一列(清單的 y + i × 列高,列與列之間無間隔)。
      const RectI drawn{list.x, list.y + i * metric::kRowH, list.w,
                        metric::kRowH};
      const RectI item = SidebarItemDip(i);
      CHECK_INT(item.x, drawn.x);
      CHECK_INT(item.y, drawn.y);
      CHECK_INT(item.w, drawn.w);
      CHECK_INT(item.h, drawn.h);

      // 命中表上的那一份也必須是同一個矩形 —— 不是「差不多」。
      bool found = false;
      for (const HitTarget& t : targets) {
        if (t.id != IDC_SIDEBAR || t.rect.y != item.y) continue;
        CHECK_INT(t.rect.x, item.x);
        CHECK_INT(t.rect.w, item.w);
        CHECK_INT(t.rect.h, item.h);
        found = true;
      }
      CHECK(found);
      ++seen;

      // 畫出來的那塊底比列窄(§12.4.2 的左右內距 12),但它**一定**
      // 落在列裡面 —— 底左邊那 12 DIP 仍然按得到,不是死區。
      // ⚠ 2026-08-14:底現在**上下也各縮 s1**,好讓 §12.14.6.1 的
      //   「項與項之間 s2(4)」看得見(comctl32 的列是連著的,間隔只能
      //   做在底上)。命中範圍一格都沒有動 —— 上面那三條 CHECK_INT 就是
      //   那件事,而 sidebar_item_fills_leave_a_visible_gap_but_the_hit_area_does_not
      //   從另一邊釘住它。
      const RectI fill = SidebarItemFillDip(i);
      CHECK(fill.x >= item.x);
      CHECK(fill.right() <= item.right());
      CHECK(fill.y >= item.y);
      CHECK(fill.bottom() <= item.bottom());
      CHECK_INT(fill.y - item.y, space::s1);
      CHECK_INT(fill.h, item.h - 2 * space::s1);
      CHECK(fill.w > 0);
    }
    // ⚠ 範圍斷言:掃到零項而報「全部合格」正是 §2-G 講的那個失效方式。
    CHECK_INT(seen, static_cast<int>(kPageCount));
  }
}

// ── #75 的另一半:任何一點只會命中一項 ─────────────────────────
//
// 上一條驗的是「每一項都對」,這一條驗的是「合起來沒有洞、也沒有疊」。
// 舊版兩者都不成立:列之間有 4 DIP 的縫(點下去誰都不是),而累積偏移
// 讓下面的項踩進上一項的畫面範圍。
TEST(ui_layout_no_point_hits_two_sidebar_items) {
  const RectI list = SidebarListDip(kWindowDefaultH);
  int probed = 0;
  for (int y = list.y; y < list.y + kPageCount * metric::kRowH; ++y) {
    int hits = 0;
    for (int i = 0; i < kPageCount; ++i) {
      const RectI r = SidebarItemDip(i);
      if (y >= r.y && y < r.bottom()) ++hits;
    }
    // 恰好一項:沒有縫(0),也沒有重疊(2)。
    CHECK_INT(hits, 1);
    ++probed;
  }
  CHECK(probed >= kPageCount * metric::kRowH);
}

// ── 版面的取材面:每一頁都要走得到 ──────────────────────────────

namespace {

// 每一頁在「輸入方案清單為空 / 不為空」兩種狀態下的版面。
// ⚠ 兩個狀態都要走:空狀態是「輸入方案」頁唯一的執行期分支,
//   而剛安裝完的使用者看到的正是那一個。
struct Variant {
  int page;
  PageState state;
};

// ⚠ 每一個**會換掉整塊版面**的執行期狀態都要有一列。少一列不會紅,
//   會安靜地少測一種畫面 —— 而少測的那一種通常就是新加的那一種。
//   (`ui_layout_every_page_is_covered_by_the_variant_table` 只擋得住
//    「少一頁」,擋不住「一頁少一種狀態」,所以下面兩對要一起讀。)
const Variant kVariants[] = {
    {kPageSchemas, PageState{false, true}},
    // 清單為空的三種:真的一種都沒有、還在問引擎、問不到。
    // ⚠ 三種說的是三件事,長度也不一樣 —— 只走一種等於少測兩種畫面。
    {kPageSchemas, PageState{true, true, kSchemaNoteEmpty}},
    {kPageSchemas, PageState{true, true, kSchemaNoteLoading}},
    {kPageSchemas, PageState{true, true, kSchemaNoteUnavailable}},
    {kPageAppearance, PageState{false, true}},
    {kPageText, PageState{false, true}},
    // 連網頁的兩種:一次都沒有連過(空狀態)、以及有紀錄。
    {kPageNetwork, PageState{false, true}},
    {kPageNetwork, PageState{false, false}},
    {kPageAdvanced, PageState{false, true}},
};
constexpr int kVariantCount =
    static_cast<int>(sizeof(kVariants) / sizeof(kVariants[0]));

}  // namespace

TEST(ui_layout_every_page_is_covered_by_the_variant_table) {
  // ⚠ 這一條看起來多餘,但它是「範圍為空 = 全部合格」的解藥:
  //   新增一頁而忘了把它加進 kVariants,下面每一條都會安靜地少測一頁。
  std::set<int> seen;
  for (int i = 0; i < kVariantCount; ++i) seen.insert(kVariants[i].page);
  CHECK_INT(static_cast<int>(seen.size()), static_cast<int>(kPageCount));
  for (int p = 0; p < kPageCount; ++p) CHECK(seen.count(p) == 1);
}

// ── W18:所有可點矩形 ≥ 28×28 DIP,**每一頁** ────────────────────

TEST(ui_layout_every_clickable_target_meets_the_minimum) {
  const int sizes[][2] = {{660, 460}, {780, 560}, {1000, 700}, {1600, 1000}};
  int measured = 0;
  for (const auto& s : sizes) {
    for (int v = 0; v < kVariantCount; ++v) {
      const std::vector<HitTarget> targets = ClickableTargetsDip(
          s[0], s[1], kVariants[v].page, kVariants[v].state);
      // ⚠ 範圍斷言:一頁上至少要有這麼多可點的東西。掃到零個而報
      //   「全部合格」正是 §2-G 講的那個失效方式。
      //   側欄 4 + 關閉鈕 1 = 5 是下限,再加上該頁自己的。
      CHECK(targets.size() >= 6);
      for (const HitTarget& t : targets) {
        ++measured;
        CHECK(t.rect.w >= metric::kMinTarget);
        CHECK(t.rect.h >= metric::kMinTarget);
      }
    }
  }
  // 4 種尺寸 × 5 個變體 × 至少 6 個。
  CHECK(measured >= 4 * kVariantCount * 6);
}

// ── W24:每一個可點控制項都碰得到(這一條是新的,見檔頭)─────────
//
// 「碰得到」的定義:
//   · 底部固定列與側欄不捲動 → 必須整個落在視窗裡。
//   · 內容區會捲動 → 必須整個落在 [0, 可視高度 + 捲動上限] 之內。
//     內容放得下的時候那個上界就是**可視高度**本身,也就是
//     「bottom() <= 可用高度」的字面意思;放不下的時候上界是內容總高,
//     而內容總高是捲動範圍唯一的來源 —— 所以任何一顆沒有推進堆疊的
//     控制項(舊版的「重設全部設定」正是如此)會立刻變紅。

TEST(ui_layout_every_clickable_target_is_reachable) {
  const int sizes[][2] = {{kWindowMinW, kWindowMinH},
                          {kWindowDefaultW, kWindowDefaultH},
                          {1000, 700},
                          {1600, 1000}};
  int checked = 0;
  for (const auto& s : sizes) {
    const int W = s[0], H = s[1];
    const int viewport = ContentViewportHeightDip(H);
    CHECK(viewport > 0);
    for (int v = 0; v < kVariantCount; ++v) {
      const int page = kVariants[v].page;
      const PageState state = kVariants[v].state;
      const int reach = viewport + ScrollMaxDip(page, W, H, state);
      const std::vector<HitTarget> targets =
          ClickableTargetsDip(W, H, page, state);
      CHECK(targets.size() >= 6);
      for (const HitTarget& t : targets) {
        ++checked;
        CHECK(t.rect.x >= 0);
        CHECK(t.rect.right() <= W);
        CHECK(t.rect.y >= 0);
        if (t.scrolls) {
          CHECK(t.rect.bottom() <= reach);
        } else {
          CHECK(t.rect.bottom() <= H);
        }
      }
    }
  }
  CHECK(checked >= 4 * kVariantCount * 6);
}

TEST(ui_layout_scroll_range_actually_uses_the_window_height) {
  // ⚠ 舊版的 ClickableTargetsDip 最後一行是 `(void)window_h_dip;` ——
  //   視窗高度被整個丟掉,於是「排到視窗底部以外」對測試而言不存在。
  //   這一條把「高度真的有參與」釘住:視窗長高,捲動上限必須跟著縮,
  //   而且高到某個程度必須歸零。
  const int W = kWindowDefaultW;
  const int prev_min =
      ScrollMaxDip(kPageAppearance, W, kWindowMinH, PageState{});
  const int prev_def =
      ScrollMaxDip(kPageAppearance, W, kWindowDefaultH, PageState{});
  CHECK(prev_min > prev_def);
  CHECK(prev_def > 0);
  // 高度每多 1 DIP,捲動上限就少 1 DIP,直到 0。
  CHECK_INT(prev_min - prev_def, kWindowDefaultH - kWindowMinH);
  const PageLayout pl =
      LayoutSettingsPageDip(kPageAppearance, W, PageState{});
  CHECK_INT(ScrollMaxDip(kPageAppearance, W, pl.content_h_dip + kBottomStripH,
                         PageState{}),
            0);
  CHECK_INT(ScrollMaxDip(kPageAppearance, W, 4000, PageState{}), 0);
}

TEST(ui_layout_appearance_page_still_needs_scrolling_at_the_default_size) {
  // 這一頁是缺陷回報的原始現場,數字全部釘住 —— 有人「順手重排」
  // 而讓它剛好又掉出可視範圍時,這一條會指著同一個地方。
  //
  // ⚠ 2026-08-11 起版面**跟著介面語言變**:說明段的高度是從字數算出來的
  //   (#76,見 EstimateTextLinesDip),而同一句話的繁中與英文長度不一樣。
  //   所以釘數字之前要先把語言釘住,不然這一條會變成「看它前面跑了誰」。
  SetUiLang(UiLang::kZhHant);
  const PageLayout pl =
      LayoutSettingsPageDip(kPageAppearance, 780, PageState{});
  auto find = [&](int id) {
    for (const PlacedControl& p : pl.items)
      if (p.id == id) return p.rect;
    return RectI{};
  };
  // ⚠ 2026-08-11:說明段的高度改成從字數算(#76),所以這一組整個往上
  //   移了。舊值 574 / 604 / 634 / 754 / 810,content_h 890。
  //   ⚠ **重點沒有變**:深淺三態仍然在可視高度(506)以下,這一頁仍然
  //     非捲不可。修的是「多出來的空白」,不是「這頁其實放得下」。
  //
  // ⚠ 2026-08-12(winbar 併進來):那一橫的說明從
  //   「…要在句子中間切中英文,目前只有它做得到。」改成
  //   「…你在用這個輸入法的時候它才出現。在句子中間切中英文:
  //     Ctrl + 空白鍵,或點這一橫的第一格。」
  //   —— 多一行,所以它**底下**的三個數字各 +16(714→730、770→786、
  //   822→838)。深淺三態在那一段**上面**,一格都沒動。
  //
  //   這正是這兩條線碰在一起的地方:winbar 把那句話改長(舊的那句在
  //   它改完狀態列生命週期之後是**假的**),而 win-next 讓版面真的去
  //   量那句話。兩件事都要在,所以數字必須跟著走 —— 釘住舊數字等於
  //   宣稱那句話沒改。
  // ⚠ 2026-08-14(§12.14 的風格那一輪):整組又往下移了,舊值
  //   563 / 593 / 623 / 730 / 786,content_h 838。三個來源:
  //     · 單選/開關列 28 → 36(§12.14.6.6 的表,一列高 36);
  //     · 每一組控制項外面多了一張**卡片**(上下內距各 s4);
  //     · 頁標題 28 → 31、區段標題 19 → 22(§12.14.0 第 4 條,
  //       行盒本來就該是 TextLineBoxDip)。
  //   ⚠ **重點沒有變**:深淺三態仍然在可視高度(506)以下,這一頁仍然
  //     非捲不可。
  CHECK_INT(find(IDC_THEME_0).y, 705);
  CHECK_INT(find(IDC_THEME_1).y, 743);
  CHECK_INT(find(IDC_THEME_2).y, 781);
  CHECK_INT(find(IDC_BAR_SHOW).y, 919);
  CHECK_INT(find(IDC_APPEAR_NOTE).y, 985);
  CHECK_INT(pl.content_h_dip, 1037);

  // 預設尺寸下的可視高度是 506 —— 深淺三態在 574 以下,**看不到**。
  CHECK_INT(ContentViewportHeightDip(kWindowDefaultH), 506);
  CHECK(find(IDC_THEME_0).y > ContentViewportHeightDip(kWindowDefaultH));
  // 所以捲動不是選配。捲動上限必須真的蓋過那一段。
  const int reach =
      ContentViewportHeightDip(kWindowDefaultH) +
      ScrollMaxDip(kPageAppearance, 780, kWindowDefaultH, PageState{});
  CHECK(reach >= find(IDC_APPEAR_NOTE).bottom());
}

TEST(ui_layout_advanced_page_counts_the_last_button_in_its_height) {
  // ⚠ 舊版:`place(IDC_RESET, RectI{cx, st.y(), 220, btn_h})` 之後
  //   **不推進堆疊**。那一顆的 32 DIP 不進內容高度 = 不進捲動範圍,
  //   於是捲到最底仍然差 32 DIP 碰不到那顆危險鍵。
  SetUiLang(UiLang::kZhHant);
  const PageLayout pl = LayoutSettingsPageDip(kPageAdvanced, 780, PageState{});
  int max_bottom = 0;
  for (const PlacedControl& p : pl.items)
    if (!p.rect.empty() && p.rect.bottom() > max_bottom)
      max_bottom = p.rect.bottom();
  // 2026-08-11:「連上網路」與「更新」兩區**搬去「連網」那一頁**了,
  // 所以這個數字回到 810。⚠ 它是**釘住**的:改版面時要一起改,
  // 而那正是重點 —— 有人「順手重排」而讓最後那顆危險鍵又掉出捲動範圍時,
  // 這一條會先攔下來。
  //
  // ⚠ 為什麼搬:兩條線各自做了一次同一件事,合併之後同一個連網開關
  //   在畫面上有**兩個入口**(進階頁一個、連網頁一個),而它們讀的是
  //   同一個值。使用者在其中一邊改完,另一邊要等下一次重畫才會跟上,
  //   而中間那一段畫面上會有兩個互相矛盾的開關。
  //
  // ⚠ 2026-08-11:說明段的高度改成從字數算(#76),舊值 810。
  // ⚠ 2026-08-14:卡片進來之後,最後一顆控制項的下面還有卡片的下內距
  //   (s4)。所以「整頁的底」是**卡片的底**,不是控制項的底 ——
  //   內容高度必須把它算進去,否則捲到最底時最後那顆危險鍵下緣
  //   會貼著摺線(#76 的同一族)。舊值 762。
  // ⚠ 2026-08-15:按鈕列改走 card_block(不吃 card_row 的 36 下限),
  //   所以進階頁上那四列按鈕各矮 4 DIP:918 − 4×4 = 902。
  //   這一行是釘住的,改版面時要一起改。
  // ⚠ 2026-08-15（A）：「重新整理字詞」「你的檔案」「重設全部設定」三張卡
  //   從「區段標題浮在一個只放按鈕的空盒上面」
  //   改成「標籤在左、按鈕在右」（§12.14.6.9）。
  //   標題與說明搬進卡片之後，每一張卡省下一整塊高度：
  //   902 → 836（矮 66 DIP）。舊值 902 裡面有 66 DIP 是空白。
  CHECK_INT(max_bottom, 836);
  for (const CardRect& c : pl.cards)
    if (c.rect.bottom() > max_bottom) max_bottom = c.rect.bottom();
  CHECK_INT(pl.content_h_dip, max_bottom + kContentPadBottomDip);
  // 三顆固定寬度的按鈕都要在內容高度以內。
  for (int id : {IDC_REDEPLOY, IDC_DIAG_COPY, IDC_RESET}) {
    for (const PlacedControl& p : pl.items)
      if (p.id == id) CHECK(p.rect.bottom() <= pl.content_h_dip);
  }
}

TEST(ui_layout_network_page_counts_the_update_card_in_its_height) {
  // ⚠ 更新卡片是 2026-08-11 從進階頁搬過來的,而它的最後一顆(三顆按鈕
  //   那一列)在紀錄不是空的時候**還不是**整頁的最後一顆 —— 底下還有
  //   清除紀錄那個危險區塊。所以這一條要問的是整頁的底,不是卡片的底:
  //   卡片被塞進來之後把危險鍵擠出捲動範圍,是這一頁最可能發生的迴歸。
  for (const PageState& state : {PageState{false, true}, PageState{false, false}}) {
    const PageLayout pl = LayoutSettingsPageDip(kPageNetwork, 780, state);
    int max_bottom = 0;
    for (const PlacedControl& p : pl.items)
      if (!p.rect.empty() && p.rect.bottom() > max_bottom)
        max_bottom = p.rect.bottom();
    // ⚠ 卡片的下內距也算(見進階頁那一條)。
    for (const CardRect& c : pl.cards)
      if (c.rect.bottom() > max_bottom) max_bottom = c.rect.bottom();
    CHECK_INT(pl.content_h_dip, max_bottom + kContentPadBottomDip);
    // 三顆更新鍵與清除鍵都要在內容高度以內 —— 捲到最底碰得到。
    for (int id : {IDC_UPDATE_CHECK, IDC_UPDATE_ACTION, IDC_UPDATE_PAGE,
                   IDC_NETLOG_CLEAR}) {
      for (const PlacedControl& p : pl.items)
        if (p.id == id && !p.rect.empty())
          CHECK(p.rect.bottom() <= pl.content_h_dip);
    }
  }
}

// ── #76:說明段的高度必須跟著字走 ─────────────────────────────
//
// 缺陷本身:連網頁(紀錄不空)content_h_dip = 1138,而可視高度只有 506
// —— 破壞性的「清除紀錄」落在摺線下 580 DIP。根因不是「視窗太小」,
// 是六段說明**各自寫死行數**,每一段都寬鬆一點,合起來多了 195 DIP。
//
// ⚠ 這一條驗的是估算本身的性質,不是某一個數字:
//   估算不準是必然的(真正的斷行是 GDI 做的),但它必須**單調**、
//   必須**至少一行**、而且必須往**高**的一邊錯。
TEST(ui_layout_text_height_follows_the_text) {
  // 一行的盒高只有一個來源。舊版說明段用的是 t5 + s2 = 15,而
  // TextLineBoxDip(11) = 16 —— 每一行少 1 DIP,中文字的下緣被切掉一點。
  CHECK_INT(TextLineBoxDip(text_size::t5), 16);
  CHECK_INT(text_size::t5 + space::s2, 15);  // 記下舊值錯在哪

  // 空字串仍然佔一行(那一格還是要有位置)。
  CHECK_INT(EstimateTextLinesDip(L"", 11, 540), 1);
  CHECK_INT(EstimateTextLinesDip(nullptr, 11, 540), 1);

  // ⚠ 底下的全形字用**碼點**組,不寫中文字面值:check_ui_spec.sh 的 W7
  //   要求使用者可見的中日韓字串只能住在 common/ui_strings.cc,而那條
  //   規則對測試檔一視同仁 —— 一個為了測規則而自己違規的測試,會讓那
  //   條檢查要嘛紅、要嘛得為自己開一個例外,而例外正是規則死掉的方式。
  const wchar_t han = static_cast<wchar_t>(0x5B57);  // 「字」

  // 一個全形字放得下 → 一行。
  CHECK_INT(EstimateTextLinesDip(std::wstring(1, han).c_str(), 11, 540), 1);

  // 單調:同一段字,欄越窄行數只會多不會少。
  // 刻意混兩種字寬 —— 全形一個字級、拉丁 0.6 個字級。
  const std::wstring body = std::wstring(24, han) +
                            L" latin words and digits 0123456789 " +
                            std::wstring(24, han);
  int prev = 0;
  for (int w : {640, 540, 440, 340, 240}) {
    const int n = EstimateTextLinesDip(body.c_str(), 11, w);
    CHECK(n >= 1);
    CHECK(n >= prev);
    prev = n;
  }
  // 而且窄欄真的比寬欄多。
  CHECK(EstimateTextLinesDip(body.c_str(), 11, 240) >
        EstimateTextLinesDip(body.c_str(), 11, 640));

  // 明寫的換行要算進去。
  CHECK_INT(EstimateTextLinesDip(L"a\nb\nc", 11, 540), 3);

  // 往高的一邊錯:估出來的高度 >= 逐字硬算的下界。
  //   (全形字每個 1 個字級,540 寬只算 6/7 = 462,一行最多 42 個
  //    → 100 個字至少 3 行)
  const std::wstring cjk(100, han);
  CHECK(EstimateTextLinesDip(cjk.c_str(), 11, 540) >= 3);
}

// ── #76:一頁不可以長到捲不完 ──────────────────────────────────
//
// 上面那一條守的是「每一段配到的高度對不對」,這一條守的是**合起來**。
// 少了它,「每一段各自看起來都很合理,加起來是一頁 1138 DIP」還會再發生
// 一次 —— 而那正是這個缺陷的形狀。
//
// N = 2.5 屏。這個數字的根據,以及**它守不住什麼**:
//
//   · 現況最高的一頁是英文介面下的「連網」頁(紀錄不空),2.19 屏。
//     2.5 留了約一成餘裕,擋得住「又多加一段說明」那種漸進的長胖。
//   · 上界不該再放寬:使用者要找的那顆鍵在第三屏以下時,捲動已經不是
//     解法了 —— 那一頁該拆頁(§4.9 的危險區塊本來就要排在最後)。
//     這一條紅掉的時候,對的動作是拆頁,不是把 N 調大。
//
// ⚠ **講清楚:這一條擋不住 #76 本身。** 出事時的 1138 DIP 是 2.25 屏,
//   就在 2.5 底下 —— 也就是說,如果這條上界早就存在,它是綠的。
//   要把它訂在 2.25 以下才攔得到,而英文介面現在是 2.19,那樣的餘裕
//   (2.7%)只會讓下一次正常的文案修改變成假紅,然後它被調大或關掉。
//
//   真正擋住 #76 那一類的是**高度不再是一個可以隨手填的自由參數**
//   (上面那條 text_height_follows_the_text,以及每一段的高度都從
//   EstimateTextLinesDip 來)。這一條是天花板,不是那個守門。
//
// ⚠ 英文的「連網」頁 2.19 屏已經貼著天花板了。那不是排版問題 ——
//   那一頁上有開關、代價說明、整張更新卡片、紀錄清單、清除紀錄五件事。
//   它該拆,而這一行是那件事的紀錄。
//
// ⚠ 三種介面語言都要走。同一句話的長度不一樣,而**英文一律比較長**
//   —— 只測繁中的話,英文使用者那一頁沒有人看得到。
TEST(ui_layout_a_page_never_grows_past_two_and_a_half_screens) {
  const int viewport = ContentViewportHeightDip(kWindowDefaultH);
  CHECK_INT(viewport, 506);
  int measured = 0;
  for (UiLang lang : {UiLang::kZhHant, UiLang::kZhHans, UiLang::kEnUs}) {
    SetUiLang(lang);
    for (int v = 0; v < kVariantCount; ++v) {
      const PageLayout pl = LayoutSettingsPageDip(
          kVariants[v].page, kWindowDefaultW, kVariants[v].state);
      // 2 × content <= 5 × viewport,也就是 content <= 2.5 屏。
      CHECK(2 * pl.content_h_dip <= 5 * viewport);
      // 下界:一頁不可能是空的。掃到零而報「全部合格」是這張表自己
      // 最可能的失效方式。
      CHECK(pl.content_h_dip > viewport / 4);
      ++measured;
    }
  }
  CHECK_INT(measured, 3 * kVariantCount);
  SetUiLang(UiLang::kZhHant);  // ⚠ 一定要還原:別的測試釘著數字。
}

// ── #76 的使用者症狀本身:破壞性的那幾顆碰得到 ───────────────────
//
// 「捲到最底」= 可視高度 + 捲動上限。那顆鍵的底必須在裡面 ——
// 它在摺線下多少 DIP 不重要,重要的是捲得到。
TEST(ui_layout_destructive_buttons_are_reachable_at_the_default_size) {
  int checked = 0;
  for (UiLang lang : {UiLang::kZhHant, UiLang::kZhHans, UiLang::kEnUs}) {
    SetUiLang(lang);
    struct Want { int page; PageState state; int id; };
    const Want wants[] = {
        // 紀錄不空的時候才有「清除紀錄」(空的時候刻意不給那顆鍵)。
        {kPageNetwork, PageState{false, false}, IDC_NETLOG_CLEAR},
        {kPageAdvanced, PageState{false, true}, IDC_RESET},
    };
    for (const Want& w : wants) {
      const int reach =
          ContentViewportHeightDip(kWindowDefaultH) +
          ScrollMaxDip(w.page, kWindowDefaultW, kWindowDefaultH, w.state);
      const PageLayout pl =
          LayoutSettingsPageDip(w.page, kWindowDefaultW, w.state);
      bool found = false;
      for (const PlacedControl& p : pl.items) {
        if (p.id != w.id || p.rect.empty()) continue;
        CHECK(p.rect.bottom() <= reach);
        found = true;
      }
      CHECK(found);  // 那一頁上真的有那顆鍵,不是掃了個空
      ++checked;
    }
  }
  CHECK_INT(checked, 6);
  SetUiLang(UiLang::kZhHant);
}

TEST(ui_layout_every_control_belongs_to_exactly_one_page) {
  // 「建了但不屬於任何一頁」曾經真的發生過(IDC_FOLLOW_MODE 從來沒有被
  // ShowWindow 過)。現在版面是唯一的來源,所以反過來要守的是:
  // 同一顆控制項不可以同時屬於兩頁 —— 那樣切頁時會有一顆殘留在畫面上。
  std::set<int> seen;
  int total = 0;
  for (int page = 0; page < kPageCount; ++page) {
    std::set<int> on_page;
    // ⚠ **每一種**執行期狀態都要走一次。只走預設那一種的話,
    //   「空狀態才出現的那幾顆同時也被別頁認領了」看不出來。
    for (const PageState& state :
         {PageState{false, false}, PageState{false, true},
          PageState{true, false}, PageState{true, true}}) {
      const PageLayout pl = LayoutSettingsPageDip(page, 780, state);
      for (const PlacedControl& p : pl.items) on_page.insert(p.id);
    }
    CHECK(on_page.size() >= 5);
    for (int id : on_page) {
      CHECK(seen.count(id) == 0);  // 已經被別頁認領過 = 兩頁共用一顆
      seen.insert(id);
      ++total;
    }
  }
  CHECK(total >= 50);
  // 底部固定列與側欄**不屬於**任何一頁 —— 它們不捲動,由呼叫端擺。
  CHECK(seen.count(IDC_STATUS) == 0);
  CHECK(seen.count(IDC_CLOSE) == 0);
  CHECK(seen.count(IDC_SIDEBAR) == 0);
}

TEST(ui_layout_stack_puts_danger_last_behind_a_divider) {
  // §2-C2:破壞性動作在該頁最後一個區塊,與上面隔一條 hairline + s7。
  Stack st(0, 0, 400);
  const RectI a = st.Push(30, space::s3);
  const RectI line = st.PushDivider();
  const RectI danger = st.Push(30, 0);
  CHECK_INT(line.h, metric::kHairline);
  CHECK(line.y >= a.bottom() + space::s7);
  CHECK(danger.y >= line.bottom() + space::s7);

  // 進階頁上真的是這個形狀:重設鈕是最後一顆,而且在那條線下面。
  const PageLayout pl = LayoutSettingsPageDip(kPageAdvanced, 780, PageState{});
  int reset_y = -1, reset_head_y = -1, diag_copy_bottom = -1;
  for (const PlacedControl& p : pl.items) {
    if (p.id == IDC_RESET) reset_y = p.rect.y;
    if (p.id == IDC_RESET_HEAD) reset_head_y = p.rect.y;
    if (p.id == IDC_DIAG_COPY) diag_copy_bottom = p.rect.bottom();
  }
  CHECK(reset_y > 0);
  CHECK(reset_head_y > 0);
  CHECK(diag_copy_bottom > 0);
  // ⚠ 量的是**區段標題**,不是按鈕。按鈕與上一塊之間本來就隔著區段標題
  //   與說明(21 + 36 = 57 DIP),已經大於 s7 + hairline + s7 = 41 ——
  //   拿按鈕去量的話,把 st.PushDivider() 整行刪掉這一條仍然是綠的。
  //   (2026-08-10 的反向測試就是這樣抓到的,在連網頁那一條上。)
  // ⚠ 2026-08-14:診斷那顆「複製」現在坐在卡片裡,所以按鈕下緣與那條
  //   分隔線之間還隔著卡片的下內距(s4)。分隔線本身沒有變。
  // ⚠ 2026-08-15（A）：「重設全部設定」那一區的區段標題
  //   現在坐在卡片裡面（§12.14.6.9 的「標籤在左」），
  //   所以它與那條分隔線之間多了一個卡片的上內距 s4。
  //   ⚠ 分隔線本身沒有變：上下各 s7 + 1 DIP。
  CHECK_INT(reset_head_y, diag_copy_bottom + space::s4 + 2 * space::s7 +
                              metric::kHairline + space::s4);
  CHECK(reset_y > reset_head_y);
}


// ── W25:捲動量真的有套到控制項上,而且捲出去的不准藏 ──────────────
//
// ⚠ 這兩條是**上一輪守門失效的直接補丁**。上一輪 W25 驗的是
//   「settings_window.cc 裡有沒有 WS_VSCROLL / EnsureFocusVisible 這些字」,
//   而覆核者把 `const int y = p->rect.y - scroll_;` 改成
//   `const int y = p->rect.y;`(捲軸拖得動、內容一動也不動),
//   206 個測試與守門腳本全綠。現在那三件事住在 ScrollPlaceControlDip()。

TEST(ui_layout_scrolled_placement_actually_subtracts_the_scroll) {
  const RectI r{40, 600, 300, 30};
  const int viewport = 500;

  // 捲動量 0:原地。
  CHECK_INT(ScrollPlaceControlDip(r, 0, viewport).y_dip, 600);
  // 捲動量 250:往上移 250。**不是 600。**
  CHECK_INT(ScrollPlaceControlDip(r, 250, viewport).y_dip, 350);
  // 捲過頭:y 可以是負的(控制項的上半截被父視窗裁掉,那是對的)。
  CHECK_INT(ScrollPlaceControlDip(r, 800, viewport).y_dip, -200);

  // 捲動量每多 1 DIP,y 就少 1 DIP —— 一格都不能停。
  // (`y = rect.y` 那種寫法在這裡會立刻紅:差值會是 0。)
  for (int scroll = 0; scroll <= 400; scroll += 37) {
    CHECK_INT(ScrollPlaceControlDip(r, scroll, viewport).y_dip, r.y - scroll);
    CHECK_INT(ScrollPlaceControlDip(r, scroll, viewport).y_dip -
                  ScrollPlaceControlDip(r, scroll + 1, viewport).y_dip,
              1);
  }

  // 同一頁上的兩顆控制項,捲動之後相對位置不變(整頁一起動)。
  const RectI a{40, 100, 300, 30};
  const RectI b{40, 700, 300, 30};
  const int gap = b.y - a.y;
  CHECK_INT(ScrollPlaceControlDip(b, 260, viewport).y_dip -
                ScrollPlaceControlDip(a, 260, viewport).y_dip,
            gap);
}

TEST(ui_layout_scrolled_out_controls_stay_in_the_tab_order) {
  const int viewport = 500;
  const RectI r{40, 600, 300, 30};

  // 完全在可視範圍以外(還沒捲到)。**不准藏** ——
  // 藏起來就退出 Tab 順序,鍵盤使用者再也走不到它,
  // 而捲動的存在正是為了讓那些控制項碰得到。
  const ScrolledPlacement below = ScrollPlaceControlDip(r, 0, viewport);
  CHECK(below.visible);
  CHECK_INT(below.clip_h_dip, 0);  // 一個像素都不露,但它在

  // 已經捲上去、整顆在可視範圍裡:不裁。
  const ScrolledPlacement inside = ScrollPlaceControlDip(r, 300, viewport);
  CHECK(inside.visible);
  CHECK_INT(inside.clip_h_dip, -1);

  // 捲到一半：⚠ **全有或全無**。舊版回的是 20（露出
  //   可視範圍以內那一截），也就是**半顆控制項**——
  //   預設尺寸下文字頁的「全形/半形」區段標題在 y=497、高 22，
  //   裁切線 506，於是它被畫成 9 DIP：從字的中間橫著切過去。
  //   那就是使用者說的「摺線那裡看起來像壞掉」。
  const ScrolledPlacement half = ScrollPlaceControlDip(r, 120, viewport);
  CHECK(half.visible);
  CHECK_INT(half.y_dip, 480);
  CHECK_INT(half.clip_h_dip, 0);
  // ⚠ 而且不只這一組：clip_h_dip 只允許有兩個值。
  //   沒有這一圈的話，「只裁一點點」會悠悠地回來。
  for (int scroll = 0; scroll <= 900; ++scroll) {
    const int c = ScrollPlaceControlDip(r, scroll, viewport).clip_h_dip;
    if (c != -1 && c != 0) CHECK_INT(c, -1);
  }

  // 捲過頭(整顆在上方):上方由父視窗的 client 矩形負責,這裡不裁。
  const ScrolledPlacement above = ScrollPlaceControlDip(r, 900, viewport);
  CHECK(above.visible);
  CHECK_INT(above.clip_h_dip, -1);

  // 走一次真的版面:外觀頁在預設視窗尺寸下捲不完,而**每一顆**
  // (含捲到看不見的那幾顆)都必須 visible。
  const int W = kWindowDefaultW;
  const int vh = ContentViewportHeightDip(kWindowDefaultH);
  const PageLayout pl =
      LayoutSettingsPageDip(kPageAppearance, W, PageState{});
  const int smax =
      ScrollMaxDip(kPageAppearance, W, kWindowDefaultH, PageState{});
  CHECK(smax > 0);  // 這一頁真的捲得動,否則下面測不到東西
  int off_screen = 0, hidden = 0;
  for (const PlacedControl& c : pl.items) {
    if (c.rect.empty()) continue;
    const ScrolledPlacement sp = ScrollPlaceControlDip(c.rect, 0, vh);
    if (sp.clip_h_dip == 0) ++off_screen;
    if (!sp.visible) ++hidden;
  }
  CHECK(off_screen > 0);   // 掃描範圍非空:真的有捲到看不見的控制項
  CHECK_INT(hidden, 0);    // 而且沒有一顆是藏起來的
}

// ── 底部固定列那一帶:畫面上不得有任何一塊 ──────────────────────
//
// ⚠ **這一條是拿實機截圖上量到的東西寫回來的。**
//
//   十張截圖(796×599,client 763×560)逐像素量出來:
//     · settings-p1-light,y=559(client 528):x287..734 一整條
//       (255,255,255)= 淺色的 kSurface,把「關閉」鈕蓋掉只剩右緣十幾點;
//     · settings-p2-light,同一條掃描線:多段 (69,79,81)
//       = 淺色的 kOnSurfaceVariant,也就是區段說明的字,壓在同一列上。
//   對回版面:外觀頁的 IDC_SCALE_2 在 y=507..543、文字頁的
//   IDC_SHAPE_BLURB 在 y=521..553,而底部固定列從 H−kBottomBarH=512
//   開始。**兩顆都在固定列上面。**
//
// ⚠ 這一列不是裝飾:SetStatus() / SetTransientStatus() 寫的
//   「已套用」「正在套用…」「重新整理字詞跑了幾秒」「複製好了」
//   全在那一行。被蓋住 = 使用者改完設定得不到任何回饋。
//
// ⚠ 為什麼舊的守門沒有響:「不畫」以前是靠 SetWindowRgn() 給控制項
//   套一個空區域達成的,而**區域不在版面模型裡**。純函式只看得到
//   「這顆在 y=507」,看不到那一個 Win32 呼叫有沒有生效 ——
//   所以這一條改成量 DrawnRectsDip():畫得出來的是哪些矩形。
//   下一次有人把裁切拆掉,矩形就會回到固定列上,這一條就紅。
TEST(nothing_is_ever_drawn_on_the_bottom_fixed_bar) {
  // 三種視窗:預設(捲軸吃掉 17 DIP 的那一份)、最小、很高的那一個。
  const int sizes[][2] = {{kWindowDefaultW - 17, kWindowDefaultH},
                          {kWindowDefaultW, kWindowDefaultH},
                          {kWindowMinW, kWindowMinH},
                          {kWindowDefaultW, 900}};
  int scanned = 0, scrolled_pages = 0;
  for (const auto& wh : sizes) {
    const int W = wh[0], H = wh[1];
    const int bar_top = H - kBottomBarH;
    for (int page = 0; page < kPageCount; ++page) {
      const int smax = ScrollMaxDip(page, W, H, PageState{});
      if (smax > 0) ++scrolled_pages;
      for (int scroll = 0; scroll <= smax; ++scroll) {
        const int clip = ContentClipLineDip(H, scroll, smax);
        for (const DrawnRect& d :
             DrawnRectsDip(page, W, H, scroll, PageState{})) {
          ++scanned;
          const std::string where =
              std::string(d.what) + " id=" + std::to_string(d.id) +
              " page=" + std::to_string(page) + " scroll=" +
              std::to_string(scroll) + " y=" + std::to_string(d.rect.y) +
              ".." + std::to_string(d.rect.bottom());
          // 1. 固定列那一帶不得有任何一塊(控制項或卡片)。
          // ⚠ CHECK_MSG 的第二個引數只在**失敗時**才求值,所以上面那一句
          //   字串是白花的 —— 但它讓紅字說得出是哪一頁、哪一顆、
          //   在哪一個捲動位置,而查不出來的紅字會被當成雜訊然後被關掉。
          CHECK_MSG(d.rect.bottom() <= bar_top,
                    "畫在底部固定列上:" + where + "(固定列從 " +
                        std::to_string(bar_top) + " 開始)");
          // 2. 摺線上不得有被攔腰切開的**控制項**。
          //    (卡片是刻意跨過去的 —— 淡出區把它接起來,見 kScrollFadeH。)
          CHECK_MSG(d.is_card || d.rect.y >= clip || d.rect.bottom() <= clip,
                    "被裁切線攔腰切開:" + where + "(裁切線 " +
                        std::to_string(clip) + ")");
        }
      }
    }
  }
  // ⚠ 掃描範圍不得是空的:上面那一圈一顆都沒走過的話,這一條會安靜地
  //   全綠 —— 而那正是這一輪要消滅的形狀。
  CHECK(scanned > 1000);
  CHECK(scrolled_pages > 0);
}

// ── 那一條真的抓得到嗎:同一個版面,把裁切拿掉就必須紅 ──────────
//
// ⚠ 上面那一條斷言的是「沒有東西在固定列上」。一條**永遠不會紅**的
//   斷言與一條守得住的斷言在 CI 上長得一模一樣,所以這裡把缺陷本身
//   造出來:不裁,直接把每一顆控制項擺在 y − scroll 上(那正是
//   2026-08-15 之前畫面上發生的事),然後要求它真的落在固定列上。
TEST(the_bottom_bar_check_would_actually_catch_the_regression) {
  const int W = kWindowDefaultW - 17, H = kWindowDefaultH;
  const int bar_top = H - kBottomBarH;
  int would_hit = 0;
  for (int page = 0; page < kPageCount; ++page) {
    const PageLayout pl = LayoutSettingsPageDip(page, W, PageState{});
    for (const PlacedControl& p : pl.items) {
      if (p.rect.empty()) continue;
      // 不裁的那一版:rect 直接就是 {x, y - scroll, w, h}。
      const RectI raw{p.rect.x, p.rect.y, p.rect.w, p.rect.h};
      if (raw.y < H && raw.bottom() > bar_top) ++would_hit;
    }
  }
  // 外觀頁的 IDC_SCALE_2、文字頁的 IDC_SHAPE_BLURB…… 至少兩顆。
  CHECK(would_hit >= 2);
}

// ── W26:側欄清單與底部狀態區不可以重疊,兩行文字要放得下 ──────────
//
// 使用者實機回報:側欄底部那兩行「可以打字」「離線」是斷的,第一行被
// 讀成「⼝以打字」—— 那是「可」的上緣被裁掉之後剩下的形狀。
//
// 根因是算式,不是繪製:清單擺在 y = 12,高度卻給了 H - 64,
// 於是它的下緣落在 H - 52,而狀態區從 H - 64 開始。重疊 12 DIP。
// 側欄清單是**子視窗**,而設定視窗開著 WS_CLIPCHILDREN ——
// 父視窗畫在那一塊的東西不是被蓋住,是根本不會被畫。
//
// ⚠ 這件事以前沒有任何自動化看得到:位置在 settings_window.cc 裡算,
//   而那個檔案在 Ubuntu 上編不起來。
TEST(ui_layout_sidebar_list_never_covers_the_status_lines) {
  // 三種高度:預設、最小、以及一個很高的視窗。
  const int heights[] = {kWindowDefaultH, kWindowMinH, 1200};
  int checked = 0;
  for (int H : heights) {
    const RectI list = SidebarListDip(H);
    const RectI strip = SidebarStatusDip(H);

    // 清單本身要有高度(否則這一條在測一個不存在的東西)。
    CHECK(list.h > 0);
    // ⚠ 核心那一條:清單的下緣**不得**越過狀態區的上緣。
    CHECK(list.y + list.h <= strip.y);
    // 清單也不可以跑出視窗。
    CHECK(list.y >= 0);
    CHECK(list.y + list.h <= H);

    for (int i = 0; i < 2; ++i) {
      const RectI line = SidebarStatusLineDip(H, i);
      // 兩行都要完整落在狀態區裡。
      CHECK(line.y >= strip.y);
      CHECK(line.y + line.h <= strip.y + strip.h);
      CHECK(line.y + line.h <= H);
      // 而且都在清單下面 —— 不然又會被子視窗裁掉。
      CHECK(line.y >= list.y + list.h);
      // 行高要放得下一行漢字。⚠ 「字級 + 4」不夠:漢字的行高大約是
      //   字級的 4/3,那樣寫是零餘裕,換一套字體就削掉一條。
      CHECK(line.h >= TextLineBoxDip(text_size::t5));
      // 左右也要在側欄裡面。
      CHECK(line.x >= 0);
      CHECK(line.x + line.w <= metric::kSidebarW);
      ++checked;
    }
  }
  // 掃描範圍非空(§2-G2):上面真的跑過六行,不是零行都沒跑。
  CHECK_INT(checked, 6);

  // 兩行不可以互相疊在一起。
  const RectI a = SidebarStatusLineDip(kWindowDefaultH, 0);
  const RectI b = SidebarStatusLineDip(kWindowDefaultH, 1);
  CHECK(a.y + a.h <= b.y);

  // 反向的證明:舊的那個算式(高度只扣狀態區、不扣上面那段留白)
  // **必須**踩到上面那一條。這一行是讓「W26 其實沒在測」現形的地方。
  const int old_bottom = space::s5 + (kWindowDefaultH - metric::kSidebarStatusH);
  CHECK(old_bottom > SidebarStatusDip(kWindowDefaultH).y);
}

// 行高的規則本身:它必須真的比字級大,而且比「字級 + 4」那個舊寫法大。
TEST(ui_layout_text_line_box_leaves_room_for_han_characters) {
  const int sizes[] = {text_size::t5, text_size::t4, text_size::t3,
                       text_size::t2, text_size::t1};
  int n = 0;
  for (int s : sizes) {
    CHECK(TextLineBoxDip(s) > s);
    // 漢字的 ascent+descent 大約 4/3 個字級。四捨五入之後至少要有這麼高。
    CHECK(TextLineBoxDip(s) >= s * 4 / 3);
    ++n;
  }
  CHECK_INT(n, 5);
  // 舊寫法(字級 + 4)在 t5 上不夠 —— 這一行說明為什麼要換掉它。
  CHECK(TextLineBoxDip(text_size::t5) > text_size::t5 + space::s2);
}


// ── 連網頁:空狀態不是「一片空白」,而且危險鍵跟著消失 ─────────────
//
// ⚠ 使用者的原話:「空的時候要說『一次都沒有連過』,不要只是一片空白 ——
//   空白讓人分不出『沒連過』與『壞掉了』。」
//
//   那句話要成立,版面上必須**真的**換掉一塊:清單不出現、換上一段說明。
//   只是「清單裡沒有列」不算 —— 那正是一片空白的定義。
//   這一條就是那個差別的斷言。

namespace {

RectI FindOn(int page, PageState state, int id) {
  const PageLayout pl = LayoutSettingsPageDip(page, 780, state);
  for (const PlacedControl& p : pl.items)
    if (p.id == id) return p.rect;
  return RectI{};
}

}  // namespace

TEST(ui_layout_network_page_empty_log_says_so_instead_of_showing_a_blank_list) {
  const PageState empty{false, true};
  const PageState has_rows{false, false};

  // 一次都沒有連過:清單、欄名、計數**都不在版面上**,換成一段說明。
  CHECK(FindOn(kPageNetwork, empty, IDC_NETLOG_LIST).empty());
  CHECK(FindOn(kPageNetwork, empty, IDC_NETLOG_COLS).empty());
  CHECK(FindOn(kPageNetwork, empty, IDC_NETLOG_SUMMARY).empty());
  CHECK(!FindOn(kPageNetwork, empty, IDC_NETLOG_EMPTY).empty());
  // 說明要放得下不只一行 —— 「為什麼是空的」講不完一行。
  CHECK(FindOn(kPageNetwork, empty, IDC_NETLOG_EMPTY).h >=
        2 * TextLineBoxDip(text_size::t5));

  // ⚠ 沒有東西可以清的時候,**清除紀錄那一整塊也不在**。
  //   留著的話就是一顆按下去什麼都不會發生的危險鍵。
  CHECK(FindOn(kPageNetwork, empty, IDC_NETLOG_CLEAR).empty());
  CHECK(FindOn(kPageNetwork, empty, IDC_NETLOG_CLEAR_HEAD).empty());
  CHECK(FindOn(kPageNetwork, empty, IDC_NETLOG_CLEAR_BLURB).empty());

  // 有紀錄:反過來。
  CHECK(!FindOn(kPageNetwork, has_rows, IDC_NETLOG_LIST).empty());
  CHECK(!FindOn(kPageNetwork, has_rows, IDC_NETLOG_COLS).empty());
  CHECK(!FindOn(kPageNetwork, has_rows, IDC_NETLOG_SUMMARY).empty());
  CHECK(!FindOn(kPageNetwork, has_rows, IDC_NETLOG_CLEAR).empty());
  CHECK(FindOn(kPageNetwork, has_rows, IDC_NETLOG_EMPTY).empty());

  // 兩種狀態下**都在**的東西:開關、它的兩句話、檢查更新、紀錄檔位置。
  // (開關在空狀態下消失的話,使用者就再也打不開連網了。)
  for (const PageState& state : {empty, has_rows}) {
    CHECK(!FindOn(kPageNetwork, state, IDC_NET_SWITCH).empty());
    CHECK(!FindOn(kPageNetwork, state, IDC_NET_STATE).empty());
    CHECK(!FindOn(kPageNetwork, state, IDC_NET_DETAIL).empty());
    // 更新卡片:信任錨那一句與三顆鍵。⚠ 信任錨要**在按鈕之前** ——
    //   排在後面的話,使用者按下去的時候還沒讀到它。
    const RectI trust = FindOn(kPageNetwork, state, IDC_UPDATE_TRUST);
    const RectI check = FindOn(kPageNetwork, state, IDC_UPDATE_CHECK);
    CHECK(!trust.empty());
    CHECK(!check.empty());
    CHECK(trust.y + trust.h <= check.y);
    CHECK(!FindOn(kPageNetwork, state, IDC_UPDATE_ACTION).empty());
    CHECK(!FindOn(kPageNetwork, state, IDC_NETLOG_PATH).empty());
  }
}

TEST(ui_layout_network_page_clear_button_is_last_and_behind_a_divider) {
  // §4.9 / §2-C2:破壞性動作是該頁最後一個區塊,與上面隔一條 hairline + s7。
  // 清除連網紀錄是破壞性的 —— 清掉之後,使用者用來稽核我們的那份證據
  // 就找不回來了。
  const PageState has_rows{false, false};
  const PageLayout pl = LayoutSettingsPageDip(kPageNetwork, 780, has_rows);
  RectI clear{}, path{}, head{};
  int max_bottom = 0;
  for (const PlacedControl& p : pl.items) {
    if (p.id == IDC_NETLOG_CLEAR) clear = p.rect;
    if (p.id == IDC_NETLOG_CLEAR_HEAD) head = p.rect;
    if (p.id == IDC_NETLOG_PATH) path = p.rect;
    if (!p.rect.empty() && p.rect.bottom() > max_bottom)
      max_bottom = p.rect.bottom();
  }
  CHECK(!clear.empty());
  CHECK(!path.empty());
  CHECK(!head.empty());
  // ⚠ 它在**最後一個區塊**裡。
  //   2026-08-15（A）：這一條以前寫的是「它是最後一顆控制項」
  //   （clear.bottom() == max_bottom）。改成「標籤在左、按鈕在右」
  //   之後，那張卡的左欄是標題 + 兩行說明（22 + 2 + 32 = 56），
  //   比按鈕（32）高，而按鈕是**垂直置中**的——所以說明的下緣
  //   （1020）比按鈕的下緣（1008）低 12 DIP。
  //   ⚠ 那不是缺陷：§4.9 要的是「危險動作在最後一個區塊」，
  //     不是「危險按鈕的下緣是全頁最低的一個像素」。所以判準改成
  //     「它在最後一張卡裡，而且那張卡是全頁最後一張」——
  //     這才是規範真正說的那件事。
  CHECK(!pl.cards.empty());
  {
    const CardRect& last = pl.cards.back();
    CHECK(clear.y >= last.rect.y);
    CHECK(clear.bottom() <= last.rect.bottom());
    for (const CardRect& c : pl.cards)
      CHECK(c.rect.bottom() <= last.rect.bottom());
    // 而且危險區塊的下面**沒有別的控制項**。
    for (const PlacedControl& q : pl.items)
      if (!q.rect.empty()) CHECK(q.rect.y <= last.rect.bottom());
  }
  for (const CardRect& c : pl.cards)
    if (c.rect.bottom() > max_bottom) max_bottom = c.rect.bottom();
  // ⚠ 與上面隔著一條分隔線(s7 + hairline + s7 = 41 DIP),而且量的是
  //   **區段標題**——它才是分隔線正下方的第一個東西。拿按鈕去量的話,
  //   標題(21)加說明(36)本身就有 57 DIP,把 st.PushDivider() 刪掉
  //   斷言照樣成立。這是反向測試實際抓到的一條假綠。
  // ⚠ 2026-08-14:紀錄檔路徑那一行在卡片裡,所以中間多了卡片的下內距。
  // ⚠ 2026-08-15（A）：那個區段標題現在坐在卡片裡面，
  //   所以它與分隔線之間又多了一個卡片的上內距 s4。
  CHECK_INT(head.y, path.bottom() + space::s4 + 2 * space::s7 +
                        metric::kHairline + space::s4);
  // ⚠ 那條分隔線與上面那張卡之間**只有一份** s7(卡片不留尾巴,
  //   PushDivider 自己留)。兩份的樣子是線離上面遠、離下面近,
  //   看起來像它屬於下面那一段。
  CHECK(clear.y > head.y);
  // ⚠ 最後一顆的高度要真的進內容高度。舊版的「重設全部設定」就是
  //   沒有推進堆疊,於是捲到底仍然差 32 DIP 碰不到。
  CHECK_INT(pl.content_h_dip, max_bottom + kContentPadBottomDip);
}

TEST(ui_layout_network_page_scrolls_at_the_default_size) {
  // 這一頁比外觀頁還長(開關 + 誠實說明 + 更新 + 紀錄)。捲不動的話,
  // 「清除紀錄」與紀錄檔位置在預設視窗尺寸下碰不到。
  const PageState has_rows{false, false};
  const int W = kWindowDefaultW;
  const int smax = ScrollMaxDip(kPageNetwork, W, kWindowDefaultH, has_rows);
  CHECK(smax > 0);
  const int reach = ContentViewportHeightDip(kWindowDefaultH) + smax;
  const PageLayout pl = LayoutSettingsPageDip(kPageNetwork, W, has_rows);
  CHECK(reach >= pl.content_h_dip - kContentPadBottomDip);
  // 視窗長高,捲動上限跟著縮,高到某個程度歸零 —— 高度真的有參與。
  CHECK_INT(ScrollMaxDip(kPageNetwork, W, 4000, has_rows), 0);
}


// ── 側欄的名字與頁是同一份 ──────────────────────────────────────
//
// ⚠ 這一條是加第五頁(連網)時冒出來的洞。名字原本住在
//   service/settings_window.cc 的一個平行陣列裡,順序必須與 SettingsPage
//   一模一樣而沒有人守 —— 錯開一格的樣子是「側欄寫著『連網』,點下去
//   出現的是進階頁」,而每一頁都有名字、每一頁都有內容,
//   畫面上看起來完全正常。

TEST(ui_layout_every_page_has_its_own_name) {
  std::set<int> seen;
  for (int p = 0; p < kPageCount; ++p) {
    const UiString s = SettingsPageName(p);
    // 兩頁不可以共用一個名字。
    CHECK(seen.count(static_cast<int>(s)) == 0);
    seen.insert(static_cast<int>(s));
    // 三個語系都要有字 —— 空字串在側欄上是一列看不見的項目。
    for (UiLang l : {UiLang::kEnUs, UiLang::kZhHant, UiLang::kZhHans})
      CHECK(UiTextIn(l, s)[0] != L'\0');
  }
  CHECK_INT(static_cast<int>(seen.size()), static_cast<int>(kPageCount));

  // 對應關係本身釘死:側欄第 n 列 = 第 n 頁的版面。
  CHECK(SettingsPageName(kPageSchemas) == UiString::kNavSchemas);
  CHECK(SettingsPageName(kPageAppearance) == UiString::kNavAppearance);
  CHECK(SettingsPageName(kPageText) == UiString::kNavText);
  CHECK(SettingsPageName(kPageNetwork) == UiString::kNavNetwork);
  CHECK(SettingsPageName(kPageAdvanced) == UiString::kNavAdvanced);

  // 越界不崩潰,而且回一個真的存在的名字(它在 WM_PAINT 路徑上)。
  CHECK(UiTextIn(UiLang::kZhHant, SettingsPageName(-1))[0] != L'\0');
  CHECK(UiTextIn(UiLang::kZhHant, SettingsPageName(999))[0] != L'\0');
}

// ── #76 的三個沒被守住的角:估算本身 ─────────────────────────────
//
// 上面那條 ui_layout_text_height_follows_the_text 只探
// {640,540,440,340,240} 五個寬度,而它要驗的「單調」在那五格之間是真的、
// 在格與格之間是假的。下面三條把它補起來。
//
// ⚠ 這裡的字串一律用**碼點**組(W7:使用者可見的中日韓字只能住在
//   common/ui_strings.cc,對測試檔一視同仁)。

namespace {

// 一段參考用的字:全形 + 拉丁 + 數字 + 一個誰都斷不開的長詞。
std::wstring MixedBody() {
  const wchar_t han = static_cast<wchar_t>(0x5B57);  // 「字」
  return std::wstring(18, han) + L" the administrator account is " +
         std::wstring(9, han) + L" mid-sentence 0123456789 " +
         std::wstring(18, han);
}

// ── 參考實作:GDI 的 DT_WORDBREAK 在做的事 ─────────────────────
//
// **刻意寫得笨**:空白/連字號之後可以斷,全形字之後可以斷,其餘的字
// 黏成一個不可斷開的詞;寬度用與產品端同一組字寬(全形 1 個字級、
// 其餘 0.6),而且**一分餘裕都不留**(整個欄寬都能用)。
//
// 它算出來的是「連字寬表都完全正確時,這段字最少要幾行」。產品端的
// 估算只要低於它,畫面上就是**文字被無聲切掉** —— 那正是要擋的事。
int RefWordWrapLines(const std::wstring& s, int size_dip, int w_dip) {
  if (s.empty() || size_dip <= 0 || w_dip <= 0) return 1;
  const long full = static_cast<long>(w_dip) * 100;
  long x = 0, atom = 0;
  int lines = 1;
  auto flush = [&] {
    if (atom <= 0) return;
    if (x > 0 && x + atom > full) {
      ++lines;
      x = 0;
    }
    x += atom;
    atom = 0;
    while (x > full) {  // 比整行還寬的詞,GDI 會硬切
      ++lines;
      x -= full;
    }
  };
  for (wchar_t ch : s) {
    const unsigned int c = static_cast<unsigned int>(ch);
    if (c == 0x0Du) continue;
    if (c == 0x0Au) {
      flush();
      ++lines;
      x = 0;
      continue;
    }
    const bool wide = (c >= 0x2E80u && c <= 0x9FFFu) ||
                      (c >= 0x2010u && c <= 0x2027u) ||
                      (c >= 0xFF00u && c <= 0xFF60u);
    atom += static_cast<long>(size_dip) * (wide ? 100 : 60);
    if (wide || c == 0x20u || c == 0x09u || c == 0x2Du) flush();
  }
  flush();
  return lines;
}

}  // namespace

// ── 戊-2:「欄越窄行數只會多」必須在**每一格**都成立 ──────────────
//
// 出事時的形狀:size=11 時 `usable = w*6/7`,而 w<=13 → usable<=11
// → `if (usable <= size_dip) return 1` → **不管那段字有多長一律 1 行**。
// w=14 時同一段 200 字回 200 行。也就是欄從 14 縮到 13,行數從 200
// 掉到 1,而少算行數的方向是**文字被切掉**。
//
// 舊的那條測試探 {640,540,440,340,240},一格都沒踩到 8..13 —— 它被
// 校準成看不到這件事。所以這裡改成掃過一個連續區間,每一格都比。
TEST(ui_layout_text_lines_is_monotone_at_every_column_width) {
  const std::wstring body = MixedBody();
  for (int size : {text_size::t5, text_size::t4, text_size::t2, text_size::t1}) {
    int prev = -1;
    for (int w = 800; w >= 8; --w) {
      const int n = EstimateTextLinesDip(body.c_str(), size, w);
      CHECK(n >= 1);
      // 欄變窄,行數只能持平或變多。
      CHECK(n >= prev);
      prev = n;
    }
  }

  // 使用者真的會看到的每一句話都要成立,三個語系都要。
  for (UiLang lang : {UiLang::kEnUs, UiLang::kZhHant, UiLang::kZhHans}) {
    for (int i = 0; i < UiStringCount(); ++i) {
      const wchar_t* t = UiTextIn(lang, static_cast<UiString>(i));
      int prev = -1;
      for (int w = 800; w >= 8; --w) {
        const int n = EstimateTextLinesDip(t, text_size::t5, w);
        CHECK(n >= prev);
        prev = n;
      }
    }
  }
}

// ── 戊-4:估算不可以低於「逐詞斷行最少要幾行」 ────────────────────
//
// 舊版靠一個 6/7 的折扣當作「拉丁逐詞斷行的鋸齒邊」的替代品,而那個
// 折扣是 1/7 的欄寬:640 DIP 的欄 = 91 DIP,440 DIP 的欄 = 62 DIP
// —— t5 底下分別是 13.8 與 9.5 個拉丁字母。一個比它長的詞被推到下一行,
// 那一行右邊空掉的比預留的多,估算就低於實際,而低估的方向是切字。
// (`administrator` 13 個、`mid-sentence` 12 個,兩個都在使用者看得到的
//  英文文案裡。)
//
// 所以不再賭那個折扣夠不夠:把逐詞斷行**算出來**,並要求估算不低於它。
TEST(ui_layout_text_lines_never_undercounts_word_wrapping) {
  // 先用一個誰都能手算的例子:10 個 20 字母的詞,欄寬 200 DIP。
  // t5 底下一個詞 = 20 × 0.6 × 11 = 132 DIP,兩個詞放不進 200 →
  // 逐詞斷行**至少** 10 行。舊版逐字塞 6/7 × 200 = 171 DIP ≈ 25 個字母,
  // 210 個字母 → 9 行 —— 少一行,而那一行的字在畫面上不存在。
  std::wstring words;
  for (int i = 0; i < 10; ++i) {
    if (i) words += L" ";
    words += std::wstring(20, L'a');
  }
  CHECK(EstimateTextLinesDip(words.c_str(), text_size::t5, 200) >=
        RefWordWrapLines(words, text_size::t5, 200));
  CHECK(EstimateTextLinesDip(words.c_str(), text_size::t5, 200) >= 10);

  // 然後是真的文案:每一句 × 每一個內容欄寬。
  for (UiLang lang : {UiLang::kEnUs, UiLang::kZhHant, UiLang::kZhHans}) {
    for (int i = 0; i < UiStringCount(); ++i) {
      const std::wstring t = UiTextIn(lang, static_cast<UiString>(i));
      for (int w = kContentMinW; w <= kContentMaxW; w += 4) {
        CHECK(EstimateTextLinesDip(t.c_str(), text_size::t5, w) >=
              RefWordWrapLines(t, text_size::t5, w));
      }
    }
  }

  // 混合字串也一樣,而且窄欄尤其要成立(窄欄的 1/7 最小)。
  const std::wstring body = MixedBody();
  for (int w = 120; w <= 800; w += 1) {
    CHECK(EstimateTextLinesDip(body.c_str(), text_size::t5, w) >=
          RefWordWrapLines(body, text_size::t5, w));
  }
}

// ── 戊-1:包裝也要擋 size_dip <= 0 ───────────────────────────────
//
// 內層 EstimateTextLinesDip 擋了(size_dip <= 0 → 1 行),而包裝
// **直接把那 1 行乘上 TextLineBoxDip(size_dip)**:
//   size = -11 → TextLineBoxDip = -12 → 那一段的高度是**負的**,
//     Stack 往回縮,下一段疊在它上面;
//   size = 0  → 2 DIP → 那一段從畫面上消失,而且沒有任何錯誤。
// 兩種都是「畫面壞掉,程式不吭聲」,而這一支在 WM_PAINT 路徑上。
TEST(ui_layout_text_box_height_never_goes_negative_or_vanishes) {
  const wchar_t han = static_cast<wchar_t>(0x5B57);
  const std::wstring body = std::wstring(60, han);
  for (int bad : {0, -1, -11, -1000}) {
    const int h = EstimateTextBoxHeightDip(body.c_str(), bad, 540);
    // 至少要放得下一行 t5 —— 不可以是負的,也不可以是「看不見」。
    CHECK(h >= TextLineBoxDip(text_size::t5));
  }
  // 正常的字級一個位元都不能變。
  CHECK_INT(EstimateTextBoxHeightDip(L"", text_size::t5, 540),
            TextLineBoxDip(text_size::t5));
  // 寬度壞掉也一樣:回一行的高度,不是 0、不是負的。
  for (int bad_w : {0, -1, -540})
    CHECK(EstimateTextBoxHeightDip(body.c_str(), text_size::t5, bad_w) >=
          TextLineBoxDip(text_size::t5));
}

// ── 戊-3:U+2010–U+2027 在中文字體裡是全形 ───────────────────────
//
// 破折號(U+2014)與刪節號(U+2026)在中文文案裡很常見,而舊表從
// U+2E80 才開始 —— 它們被當成 0.6 個字級,一段中文最多低估約 17.6 DIP,
// 也就是一行。低估的方向一樣是切字。
TEST(ui_layout_wide_punctuation_is_measured_as_wide) {
  const wchar_t emdash = static_cast<wchar_t>(0x2014);
  const wchar_t ellip = static_cast<wchar_t>(0x2026);
  const wchar_t han = static_cast<wchar_t>(0x5B57);
  // 60 個破折號要佔的寬度與 60 個漢字一樣 → 行數一樣。
  for (int w : {200, 320, 440, 540, 640}) {
    CHECK_INT(EstimateTextLinesDip(std::wstring(60, emdash).c_str(),
                                   text_size::t5, w),
              EstimateTextLinesDip(std::wstring(60, han).c_str(),
                                   text_size::t5, w));
    CHECK_INT(EstimateTextLinesDip(std::wstring(60, ellip).c_str(),
                                   text_size::t5, w),
              EstimateTextLinesDip(std::wstring(60, han).c_str(),
                                   text_size::t5, w));
  }
  // 區間的兩端都要進去(U+2010 連字號、U+2027 間隔號)。
  for (unsigned int c : {0x2010u, 0x2027u}) {
    CHECK_INT(EstimateTextLinesDip(
                  std::wstring(60, static_cast<wchar_t>(c)).c_str(),
                  text_size::t5, 440),
              EstimateTextLinesDip(std::wstring(60, han).c_str(),
                                   text_size::t5, 440));
  }
}

// ── #62:三種不同的情況,畫面上不可以是同一句話 ────────────────────
//
// 「輸入方案」頁上那一格說明以前只有一種內容:「目前一種都沒有」。
// 而走到那一格的路有三條,下一步完全不同:
//
//   · 真的一種都沒有            → 去「進階」按「重新整理字詞」
//   · 還在問引擎(快取是冷的)  → 什麼都不用做,等一下就出來
//   · 問不到(引擎在停/沒有工作者,那件工作**根本沒有入列**)
//                               → 沒有人會回來,那句話會永遠停在那裡
//
// 第三條是這一輪 `Engine::Post` 把 `Status` 丟掉的直接後果:呼叫端拿到
// 一個空 vector,而「引擎沒有回應」與「一個方案都沒有」長得一模一樣。
TEST(ui_layout_schema_note_says_three_different_things) {
  CHECK_INT(static_cast<int>(kSchemaNoteCount), 3);

  // 每一種、每一個語系都要有字,而且三種之間**一個字串都不可以共用**。
  for (UiLang lang : {UiLang::kEnUs, UiLang::kZhHant, UiLang::kZhHans}) {
    SetUiLang(lang);
    std::set<std::wstring> seen;
    for (int n = 0; n < kSchemaNoteCount; ++n) {
      const SchemaNoteText t = SchemaNoteLines(n);
      const std::wstring joined = std::wstring(UiTextIn(lang, t.title)) + L"\n" +
                                  UiTextIn(lang, t.why) + L"\n" +
                                  UiTextIn(lang, t.next);
      CHECK(UiTextIn(lang, t.title)[0] != L'\0');
      CHECK(UiTextIn(lang, t.why)[0] != L'\0');
      CHECK(UiTextIn(lang, t.next)[0] != L'\0');
      // 三種說的必須是三件事。共用一句 = 使用者分不出他在哪一種情況。
      CHECK(seen.count(joined) == 0);
      seen.insert(joined);
    }
    CHECK_INT(static_cast<int>(seen.size()), kSchemaNoteCount);
  }
  // 越界不崩潰(它在 WM_PAINT 路徑上),而且回一個真的存在的說法。
  for (int bad : {-1, 3, 999}) {
    const SchemaNoteText t = SchemaNoteLines(bad);
    CHECK(UiTextIn(UiLang::kZhHant, t.title)[0] != L'\0');
  }
  SetUiLang(UiLang::kZhHant);
}

// ── 那一格的高度要跟著它自己的字走,不是寫死 4 行 ──────────────────
//
// 舊版是 `st.Push(t5h * 4, ...)` —— 一個寫死的行數,而 #76 的根因就是
// 寫死行數。三種說法長度不一樣,而**英文一律比較長**:寫死的那一個
// 一旦不夠,使用者看到的是半句話,而且沒有任何錯誤。
TEST(ui_layout_schema_note_box_follows_its_own_text) {
  bool differs_somewhere = false;
  for (UiLang lang : {UiLang::kEnUs, UiLang::kZhHant, UiLang::kZhHans}) {
    SetUiLang(lang);
    for (int w : {kWindowMinW, kWindowDefaultW, 1000, 1600}) {
      const int cw = ContentWidthDip(w);
      std::set<int> heights;
      for (int n = 0; n < kSchemaNoteCount; ++n) {
        PageState st;
        st.schema_list_empty = true;
        st.schema_note = n;
        const PageLayout pl = LayoutSettingsPageDip(kPageSchemas, w, st);
        int h = 0;
        for (const PlacedControl& p : pl.items)
          if (p.id == IDC_SCHEMAS_EMPTY) h = p.rect.h;
        // 這一格一定要在(空狀態的頁上),而且高度**剛好**是它自己那
        // 三行字要的高度 —— 不是一個寫死的行數,也不是「寫死 + 餘裕」。
        const SchemaNoteText t = SchemaNoteLines(n);
        const int want =
            EstimateTextBoxHeightDip(UiText(t.title), text_size::t5, cw) +
            EstimateTextBoxHeightDip(UiText(t.why), text_size::t5, cw) +
            EstimateTextBoxHeightDip(UiText(t.next), text_size::t5, cw);
        CHECK_INT(h, want);
        heights.insert(h);
      }
      if (heights.size() >= 2) differs_somewhere = true;
    }
  }
  // ⚠ 「高度跟著字走」不可以只是巧合地與寫死的那個數字相等:至少要有
  //   一個語系 × 一個視窗寬度上,三種說法給出**不同**的高度。
  //   (窄欄的英文就是那一格 —— 英文一律比較長,而窄欄正是寫死行數
  //    會把字切掉的地方。不逐一指定哪一格,是因為那會隨文案改動而假紅。)
  CHECK(differs_somewhere);
  SetUiLang(UiLang::kZhHant);
}

// ────────────────────────────────────────────────────────────────
// §12.14 帶來的新檢核項(W31' / 卡片 / 指示條 / 徽章)
// ────────────────────────────────────────────────────────────────

// ── W31':內容區每一個放文字的矩形,高度 ≥ 行盒 × 行數 ───────────
//
// ⚠ 反向:把 title_block 的頁標題改回 `text_size::t1 + space::s3`(28),
//   這一條會紅 —— 而畫面上只是「每一個頁標題的下緣被削掉一條」,
//   看起來像字型沒調好,不像 bug。§12.14.0 第 4 條就是這麼被漏掉的。
TEST(every_text_rect_is_at_least_its_line_box_times_lines) {
  int measured = 0;
  for (UiLang lang : {UiLang::kEnUs, UiLang::kZhHant, UiLang::kZhHans}) {
    SetUiLang(lang);
    for (int w : {kWindowMinW, kWindowDefaultW, 1200}) {
      for (int page = 0; page < kPageCount; ++page) {
        for (int variant = 0; variant < 2; ++variant) {
          PageState st;
          st.schema_list_empty = variant == 1;
          st.net_log_empty = variant == 1;
          const PageLayout pl = LayoutSettingsPageDip(page, w, st);
          for (const PlacedControl& p : pl.items) {
            if (p.rect.empty() || p.text_size_dip <= 0) continue;
            const int want = TextLineBoxDip(p.text_size_dip) * p.text_lines;
            if (p.rect.h < want) {
              char buf[256];
              std::snprintf(buf, sizeof(buf),
                            "id=%d what=%s h=%d < %d (t%d x %d lines)", p.id,
                            p.what, p.rect.h, want, p.text_size_dip,
                            p.text_lines);
              ::rimewin_test::Fail(__FILE__, __LINE__, buf);
            }
            ++measured;
          }
        }
      }
    }
  }
  SetUiLang(UiLang::kZhHant);
  // 範圍斷言(§2-G2):W31 要求量到的文字矩形數 ≥ 30。
  CHECK(measured >= 30);
}

TEST(page_title_and_section_heading_use_the_full_line_box) {
  // §12.14.0 第 4 條列的那兩處:t1 給 28(要 31)、t2 給 19(要 22)。
  // 這一條把修正釘在具體的數字上,而不是只釘在 ≥ 的關係上。
  CHECK_INT(TextLineBoxDip(text_size::t1), 31);
  CHECK_INT(TextLineBoxDip(text_size::t2), 22);
  const PageLayout pl =
      LayoutSettingsPageDip(kPageSchemas, kWindowDefaultW, PageState{});
  int title_h = 0, head_h = 0;
  for (const PlacedControl& p : pl.items) {
    if (p.id == IDC_SCHEMAS_TITLE) title_h = p.rect.h;
    if (p.id == IDC_SCHEMAS_LIST_HEAD) head_h = p.rect.h;
  }
  CHECK_INT(title_h, 31);
  CHECK_INT(head_h, 22);
}

// ── 卡片(§12.14.5)────────────────────────────────────────────────

TEST(every_card_stays_inside_the_content_column) {
  // ⚠ 第一版的卡片往**外**撐 s6,而那在最小視窗(660)下把卡片的左緣
  //   推到 x = 194 —— 也就是側欄裡面。預設 780 寬時它看起來完全正確,
  //   這正是「在我這台機器上很好」的形狀。
  int seen = 0;
  for (int w : {kWindowMinW, kWindowDefaultW, 1200}) {
    const int cx = ContentXDip(w);
    const int cw = ContentWidthDip(w);
    for (int page = 0; page < kPageCount; ++page) {
      const PageLayout pl = LayoutSettingsPageDip(page, w, PageState{});
      for (const CardRect& c : pl.cards) {
        CHECK_INT(c.rect.x, cx);
        CHECK_INT(c.rect.w, cw);
        CHECK(c.rect.x >= metric::kSidebarW);
        CHECK(c.rect.h > 0);
        ++seen;
      }
    }
  }
  CHECK(seen >= 15);
}

TEST(every_in_card_control_sits_inside_its_card_with_padding) {
  // 卡片是「控制項矩形的聯集 + padding」。所以每一顆標了 in_card 的
  // 控制項都要真的落在某一張卡片裡面,而且左右各留 s6、上下各留 s4。
  int matched = 0;
  for (int page = 0; page < kPageCount; ++page) {
    for (int variant = 0; variant < 2; ++variant) {
      PageState st;
      st.schema_list_empty = variant == 1;
      st.net_log_empty = variant == 1;
      const PageLayout pl =
          LayoutSettingsPageDip(page, kWindowDefaultW, st);
      for (const PlacedControl& p : pl.items) {
        if (!p.in_card || p.rect.empty()) continue;
        bool inside = false;
        for (const CardRect& c : pl.cards) {
          if (p.rect.x >= c.rect.x + space::s6 &&
              p.rect.right() <= c.rect.right() - space::s6 &&
              p.rect.y >= c.rect.y + space::s4 &&
              p.rect.bottom() <= c.rect.bottom() - space::s4) {
            inside = true;
            break;
          }
        }
        if (!inside) {
          char buf[200];
          std::snprintf(buf, sizeof(buf),
                        "in_card control id=%d (%s) is not inside any card",
                        p.id, p.what);
          ::rimewin_test::Fail(__FILE__, __LINE__, buf);
        }
        ++matched;
      }
    }
  }
  CHECK(matched >= 40);
}

TEST(card_inner_dividers_are_indented_and_inside_the_card) {
  // §12.14.6.7:卡片內的分隔線**要縮排** s6 —— 不縮排的話它會碰到
  // 卡片的圓角,看起來像裂縫。
  int lines = 0;
  const PageLayout pl =
      LayoutSettingsPageDip(kPageAppearance, kWindowDefaultW, PageState{});
  for (const CardRect& c : pl.cards) {
    for (int y : c.divider_ys) {
      CHECK(y > c.rect.y);
      CHECK(y < c.rect.bottom());
      ++lines;
    }
  }
  CHECK(lines >= 3);  // 候選數那一張卡有五顆單選鈕 → 四條線
  CHECK(space::s6 == 16);
}

TEST(radio_rows_and_switch_rows_are_36_tall) {
  // §12.14.6.6 的表:整列高 36。現況單選是 kMinTarget(28)。
  const PageLayout pl =
      LayoutSettingsPageDip(kPageAppearance, kWindowDefaultW, PageState{});
  int rows = 0;
  for (const PlacedControl& p : pl.items) {
    if (p.rect.empty()) continue;
    const std::string what = p.what ? p.what : "";
    if (what.find("radio") == std::string::npos &&
        what.find("switch") == std::string::npos)
      continue;
    CHECK_INT(p.rect.h, metric::kRowH);
    CHECK_INT(metric::kRowH, 36);
    ++rows;
  }
  CHECK(rows >= 13);
}

// ── 側欄左緣指示條(§12.14.6.1)───────────────────────────────────

TEST(sidebar_indicator_is_3x16_centred_on_the_item_fill) {
  for (int i = 0; i < kPageCount; ++i) {
    const RectI fill = SidebarItemFillDip(i);
    const RectI ind = SidebarIndicatorDip(i);
    CHECK_INT(ind.w, 3);
    CHECK_INT(ind.h, 16);
    CHECK_INT(ind.w, metric::kIndicatorW);
    CHECK_INT(ind.h, metric::kIndicatorH);
    // x = 項目的左緣,**不縮排**。
    CHECK_INT(ind.x, fill.x);
    // 垂直置中。
    CHECK_INT(ind.y - fill.y, fill.bottom() - ind.bottom());
    // ⚠ 一定落在那塊底裡面 —— 底、圓角、指示條共用同一個矩形。
    CHECK(ind.y >= fill.y);
    CHECK(ind.bottom() <= fill.bottom());
  }
}

TEST(sidebar_item_fills_leave_a_visible_gap_but_the_hit_area_does_not) {
  // §12.14.6.1:項與項之間 s2(4)。comctl32 的列是連著的,所以間隔只能
  // 做在**畫出來的那塊底**上。
  // ⚠ 而命中範圍必須仍然是整列 —— 舊版把間隔加進命中那一份算式,
  //   累積到第 5 項差 16 DIP(#75)。
  for (int i = 1; i < kPageCount; ++i) {
    const RectI prev = SidebarItemFillDip(i - 1);
    const RectI cur = SidebarItemFillDip(i);
    CHECK_INT(cur.y - prev.bottom(), space::s2);
  }
  for (int i = 0; i < kPageCount; ++i) {
    const RectI row = SidebarItemDip(i);
    const RectI fill = SidebarItemFillDip(i);
    // 底在列裡面,而且列與列之間**沒有**縫(命中是連續的)。
    CHECK(fill.y >= row.y);
    CHECK(fill.bottom() <= row.bottom());
    CHECK_INT(row.h, metric::kRowH);
    if (i > 0) CHECK_INT(SidebarItemDip(i - 1).bottom(), row.y);
  }
}

TEST(sidebar_text_left_clears_the_indicator_with_room_to_spare) {
  // §12.14.6.1:文字左緣 = 項目左緣 + s6(16),**選中與未選中一樣**。
  // 16 剛好讓文字清開 3 DIP 的指示條還有 13 DIP 的餘裕。
  CHECK_INT(space::s6, 16);
  CHECK(space::s6 - metric::kIndicatorW >= 12);
}

// ── 「預設」徽章(§12.14.6.5)────────────────────────────────────

TEST(badge_is_a_capsule_18_tall_centred_in_the_row) {
  const BadgePlacement p = BadgePlacementDip(/*list_w=*/400, /*row_top=*/72,
                                             /*row_h=*/metric::kRowH,
                                             /*badge_text_w=*/24,
                                             /*border=*/1, /*scrollbar_w=*/17,
                                             /*has_vscroll=*/false);
  CHECK_INT(p.badge.h, 18);
  CHECK_INT(p.radius_dip, 9);            // 膠囊 = 高 ÷ 2
  CHECK_INT(p.badge.w, 24 + 2 * space::s3);  // 左右內距 s3
  // 列內置中:(36 − 18) / 2 = 9。
  CHECK_INT(p.badge.y - 72, 9);
}

TEST(badge_right_edge_moves_left_when_the_scrollbar_appears) {
  // ⚠ **這是 §12.14.0 第 3 條的修法。** 舊算式從列矩形往回推,而列矩形
  //   會被撐到 client 寬 —— 捲軸出現之後徽章被排到可視區外面,被裁掉。
  const BadgePlacement no_sb =
      BadgePlacementDip(400, 0, metric::kRowH, 24, 1, 17, false);
  const BadgePlacement with_sb =
      BadgePlacementDip(400, 0, metric::kRowH, 24, 1, 17, true);
  CHECK_INT(no_sb.badge.right(), 400 - 1 - 0 - space::s5);
  CHECK_INT(with_sb.badge.right(), 400 - 1 - 17 - space::s5);
  CHECK(with_sb.badge.right() < no_sb.badge.right());
  // 兩種情況下徽章都在控制項的可視範圍內。
  CHECK(no_sb.badge.right() <= 400);
  CHECK(with_sb.badge.right() <= 400 - 17);
}

TEST(badge_left_and_name_ellipsis_come_from_the_same_expression) {
  // 三件事同時被修好的第 3 件:名稱的截尾點與徽章的左緣由同一個算式
  // 產生,所以長方案名不會壓到徽章上。
  for (int text_w : {8, 24, 60, 200}) {
    const BadgePlacement p =
        BadgePlacementDip(400, 0, metric::kRowH, text_w, 1, 17, true);
    CHECK_INT(p.name_right, p.badge.x - space::s3);
    CHECK(p.name_right < p.badge.x);
  }
}

// ── 按鈕高度:一個視窗上只准有一種(§12.14.6.2/.3)──────────────
//
// ⚠ **這一條是覆核抓到的迴歸的守門,不是裝飾。**
//
//   基底 1a711a2 寫的是 `btn_h = kMinTarget + s2 = 32; st.Push(btn_h, gap)`
//   —— 直接推 32。54e4b4d 把它換成 `card_row(btn_h, gap)`,而 card_row
//   帶 36 的下限(卡片裡「一列」的最小高)。於是每一顆按鈕都被墊成 36,
//   而底部固定列的「關閉」仍然是 32:**同一個視窗上兩種按鈕高度**。
//
//   而 ui_layout.cc 那一輪自己加的註解描述的正是這個缺陷(「28 那一版比
//   同一頁上其他按鈕矮 4 DIP,而那正是『看起來像對話框』的來源之一」)
//   —— 缺陷原封不動地還在,只是從 28-vs-32 變成 32-vs-36。
//
// ⚠ **W3 抓不到這一件事。** 它驗的是 ui_layout.h 裡的字面值集合,而 32
//   與 36 都在允許集合 {1,2,3,16,28,32,36,64,200} 裡面。每一個字面值都
//   合法,組出來的畫面不合法 —— 這正是「值的白名單」擋不住的那一類。
namespace {

// 按鈕的 what 名字。⚠ 新增一顆按鈕要加進來 ——
//   `no_button_escapes_the_height_check` 會在漏掉的時候紅。
bool IsButtonWhat(const std::string& what) {
  static const std::set<std::string> kButtons = {
      "move_up",          "move_down",          "apply_order",
      "update_check",     "update_action",      "update_page",
      "clear_log_button", "redeploy_button",    "open_user_dir",
      "open_settings_file", "diagnostics_copy", "reset_button",
  };
  return kButtons.count(what) == 1;
}

// 可點、而且**不放文字**(text_size_dip == 0 && text_lines == 0)的控制項
// 只有兩類:按鈕,以及「框由控制項自己畫」的清單 / EDIT。
// 第二類逐一列出 —— 兩份名單都對不上的那一顆,就是新加的按鈕沒有表態。
bool IsNotAButton(const std::string& what) {
  static const std::set<std::string> kNotButtons = {
      "schema_list",
      "net_log_list",
      "diagnostics_edit",
  };
  return kNotButtons.count(what) == 1;
}

}  // namespace

TEST(ui_layout_every_button_is_exactly_kButtonH_tall) {
  int measured = 0;
  for (int w : {660, 780, 1000, 1600}) {
    for (int v = 0; v < kVariantCount; ++v) {
      const PageLayout pl =
          LayoutSettingsPageDip(kVariants[v].page, w, kVariants[v].state);
      for (const PlacedControl& p : pl.items) {
        if (p.rect.empty() || !IsButtonWhat(p.what)) continue;
        // ⚠ 用 CHECK_INT,它印得出兩邊的值 —— 36 vs 32 這種差 4 的迴歸,
        //   訊息裡沒有數字的話下一個人得重跑一次才知道差在哪。
        CHECK_INT(p.rect.h, metric::kButtonH);
        ++measured;
      }
    }
  }
  // 「範圍為空 = 全部合格」的解藥:下限也要驗。
  CHECK(measured >= 30);
}

TEST(ui_layout_one_window_has_exactly_one_button_height) {
  // 底部固定列的「關閉」**不在** LayoutSettingsPageDip 裡(它的位置是
  // 唯一還由呼叫端算的東西),所以「按鈕多高」在樹上是兩份 ——
  // 而兩份會漂開。這一條就是那兩份的對帳,而且它是使用者真的看得到的
  // 那個判準:一個視窗上只有一種按鈕高度。
  for (int w : {660, 780, 1000, 1600}) {
    for (int v = 0; v < kVariantCount; ++v) {
      std::set<int> heights;
      const std::vector<HitTarget> targets =
          ClickableTargetsDip(w, 560, kVariants[v].page, kVariants[v].state);
      for (const HitTarget& t : targets) {
        if (t.rect.empty()) continue;
        if (std::string(t.what) == "close_button" || IsButtonWhat(t.what))
          heights.insert(t.rect.h);
      }
      CHECK(!heights.empty());
      CHECK_INT(static_cast<int>(heights.size()), 1);
      CHECK_INT(*heights.begin(), metric::kButtonH);
    }
  }
}

TEST(no_button_escapes_the_height_check) {
  // ⚠ 這一條看起來多餘,但它是上面兩條的「範圍為空」解藥:新增一顆按鈕
  //   而忘了把名字加進 kButtons,上面兩條會安靜地少驗一顆 ——
  //   而少驗的那一顆通常就是新加的那一顆。
  int checked = 0;
  for (int v = 0; v < kVariantCount; ++v) {
    const PageLayout pl =
        LayoutSettingsPageDip(kVariants[v].page, 780, kVariants[v].state);
    for (const PlacedControl& p : pl.items) {
      if (p.rect.empty() || !p.clickable) continue;
      if (p.text_size_dip != 0 || p.text_lines != 0) continue;
      const std::string what = p.what;
      if (!IsButtonWhat(what) && !IsNotAButton(what)) {
        // 用 CHECK_STR 讓名字進到訊息裡 —— 不然下一個人得自己去猜
        // 是哪一顆沒有表態。
        CHECK_STR(what, "(這一顆沒有表態:是按鈕就加進 kButtons,"
                        "不是就加進 kNotButtons)");
      }
      ++checked;
    }
  }
  CHECK(checked >= 12);
}

// ── A:按鈕依內容寬,卡片裡不准只有按鈕(§12.14.6.2/.3/.9)──────────
//
// ⚠ 這三條是這一輪覆核的直接判準。覆核者打開十張真的截圖之後說的是:
//   「卡片又大又空」與「按鈕被拉成等寬長方形」。兩件事是同一個成因:
//   **版面在用「填滿寬度」思考,而不是用「內容需要多寬」思考。**
//   舊版一列 n 顆按鈕的寬是 `(inner_w - (n-1)*s3) / n` —— 視窗一拉寬,
//   「上移」這兩個字底下的按鈕就跟著長。

TEST(buttons_are_sized_to_their_label_never_to_the_column) {
  SetUiLang(UiLang::kZhHant);
  int measured = 0;
  for (int v = 0; v < kVariantCount; ++v) {
    for (int w : {660, 780, 1000, 1600}) {
      const PageLayout pl =
          LayoutSettingsPageDip(kVariants[v].page, w, kVariants[v].state);
      for (const PlacedControl& p : pl.items) {
        if (p.rect.empty() || !IsButtonWhat(p.what)) continue;
        const UiString label = SettingsButtonLabel(p.id);
        // ⚠ 每一顆按鈕都要在那張表上。不在表上的話 ButtonWidthDip
        //   會拿到 kUiStringCount,而寬度會安靜地掉到下界 80。
        CHECK(label != UiString::kUiStringCount);
        CHECK_INT(p.rect.w, ButtonWidthDip(label));
        CHECK(p.rect.w >= metric::kButtonMinW);
        ++measured;
      }
    }
  }
  CHECK(measured >= 30);
}

TEST(widening_the_window_never_widens_a_button) {
  // ⚠ **這一條就是那張截圖。** 三顆 165 DIP 的等寬按鈕橫跨整個內容欄,
  //   讀起來是對話框的按鈕列。判準寫成「視窗變寬,按鈕一格都不准變」
  //   —— 平均分配的算式在這一條底下不可能存活。
  SetUiLang(UiLang::kZhHant);
  int compared = 0;
  for (int v = 0; v < kVariantCount; ++v) {
    std::map<int, int> narrow;
    const PageLayout a =
        LayoutSettingsPageDip(kVariants[v].page, 660, kVariants[v].state);
    for (const PlacedControl& p : a.items)
      if (!p.rect.empty() && IsButtonWhat(p.what)) narrow[p.id] = p.rect.w;
    for (int w : {780, 1000, 1600}) {
      const PageLayout b =
          LayoutSettingsPageDip(kVariants[v].page, w, kVariants[v].state);
      for (const PlacedControl& p : b.items) {
        if (p.rect.empty() || !IsButtonWhat(p.what)) continue;
        auto it = narrow.find(p.id);
        if (it == narrow.end()) continue;
        CHECK_INT(p.rect.w, it->second);
        ++compared;
      }
    }
  }
  // 9 個變體 × 3 個寬度,只有輸入方案/連網/進階三頁有按鈕 —— 45 次比對。
  CHECK(compared >= 40);
}

TEST(no_card_holds_only_buttons) {
  // ⚠ 覆核者的原話:「一張卡裡只有按鈕時,卡不該是整行寬的空盒」。
  //   進階頁那張「重新整理字詞」是 540×52,而按鈕只佔左邊 180 ——
  //   右邊 360 DIP 全是空白,而那是「看起來沒做完」最大的單一來源。
  //
  //   走的是(乙):卡片留著,**左邊放標籤、右邊放按鈕**(對照組的做法)。
  //   所以判準是:每一張卡裡都必須有至少一顆帶字的控制項。
  //   走(甲)(按鈕不套卡片)的話這一條也會綠 —— 它擋的是那個空盒,
  //   不是某一種做法。
  SetUiLang(UiLang::kZhHant);
  int cards_seen = 0;
  for (int v = 0; v < kVariantCount; ++v) {
    for (int w : {660, 780, 1600}) {
      const PageLayout pl =
          LayoutSettingsPageDip(kVariants[v].page, w, kVariants[v].state);
      for (const CardRect& c : pl.cards) {
        bool has_text = false;
        bool has_anything = false;
        for (const PlacedControl& p : pl.items) {
          if (!p.in_card || p.rect.empty()) continue;
          if (p.rect.y < c.rect.y || p.rect.bottom() > c.rect.bottom())
            continue;
          has_anything = true;
          // 帶字的:STATIC / 開關 / 單選鈕 / 清單 / 唯讀 EDIT。
          // 按鈕的 text_size_dip 是 0(字由控制項自己畫)。
          if (p.text_size_dip > 0 || IsNotAButton(p.what)) has_text = true;
        }
        CHECK(has_anything);
        if (!has_text)
          CHECK_STR("這張卡裡只有按鈕", "卡片要嘛帶標籤(§12.14.6.9),"
                                        "要嘛那組按鈕不要套卡片");
        ++cards_seen;
      }
    }
  }
  CHECK(cards_seen >= 30);
}

TEST(action_card_puts_the_label_left_and_the_buttons_right) {
  // §12.14.6.9 的幾何:標籤欄靠左、按鈕靠右,右緣對齊**卡片內寬**的右緣
  // (不是內容欄的右緣 —— 差 s6,而差那 16 DIP 的樣子是
  //  「按鈕比卡片還往外凸一點」)。
  SetUiLang(UiLang::kZhHant);
  const PageLayout pl = LayoutSettingsPageDip(kPageAdvanced, 780, PageState{});
  const int cx = ContentXDip(780);
  const int cw = ContentWidthDip(780);
  auto find = [&](int id) {
    for (const PlacedControl& p : pl.items)
      if (p.id == id) return p.rect;
    return RectI{};
  };
  const RectI head = find(IDC_REDEPLOY_HEAD);
  const RectI blurb = find(IDC_REDEPLOY_BLURB);
  const RectI btn = find(IDC_REDEPLOY);
  CHECK(!head.empty());
  CHECK(!blurb.empty());
  CHECK(!btn.empty());
  // 標題與說明搬進卡片了 —— 它們的底要跟著換成 surface(in_card)。
  for (const PlacedControl& p : pl.items)
    if (p.id == IDC_REDEPLOY_HEAD || p.id == IDC_REDEPLOY_BLURB)
      CHECK(p.in_card);
  // 左欄靠左,右緣不碰到按鈕。
  CHECK_INT(head.x, cx + space::s6);
  CHECK_INT(blurb.x, head.x);
  CHECK(head.right() + space::s6 <= btn.x);
  // 按鈕靠右:右緣 = 卡片內寬的右緣。
  CHECK_INT(btn.right(), cx + cw - space::s6);
  // 兩顆的那一張也一樣,而且兩顆之間 s3。
  const RectI b1 = find(IDC_OPEN_USER_DIR);
  const RectI b2 = find(IDC_OPEN_SETTINGS_FILE);
  CHECK(!b1.empty());
  CHECK(!b2.empty());
  CHECK_INT(b2.right(), cx + cw - space::s6);
  CHECK_INT(b2.x - b1.right(), space::s3);
}

TEST(button_width_follows_the_label_and_has_a_floor) {
  SetUiLang(UiLang::kZhHant);
  // 短標籤吃下界。
  CHECK_INT(ButtonWidthDip(UiString::kSchemasMoveUp), metric::kButtonMinW);
  // 長標籤比短標籤寬 —— 而且是**因為字**,不是因為欄寬。
  CHECK(ButtonWidthDip(UiString::kResetButton) >
        ButtonWidthDip(UiString::kSchemasMoveUp));
  // 全形字大約一個字級寬,拉丁字母 0.6 個 —— 同樣五個字,全形比較寬。
  // ⚠ 用碼點寫,不寫字面:W7 規定 catalog 以外不得出現中日韓寬字串,
  //   而那條規矩沒有「測試除外」—— 有了例外,下一個真的違規的就進得來。
  CHECK(EstimateTextWidthDip(L"12345", text_size::t4) <
        EstimateTextWidthDip(L"\u4E00\u4E8C\u4E09\u56DB\u4E94",
                             text_size::t4));
  // 字越多越寬 —— 一格都不准反過來。
  int prev = -1;
  std::wstring grow;
  for (int i = 0; i < 40; ++i) {
    const int w = EstimateTextWidthDip(grow.c_str(), text_size::t4);
    CHECK(w >= prev);
    prev = w;
    grow += L'W';
  }
  // 邊界:空字串 / 空指標 / 不合法字級都不可以回負數。
  CHECK_INT(EstimateTextWidthDip(nullptr, text_size::t4), 0);
  CHECK_INT(EstimateTextWidthDip(L"", text_size::t4), 0);
  CHECK_INT(EstimateTextWidthDip(L"abc", 0), 0);
  CHECK_INT(EstimateTextWidthDip(L"abc", -5), 0);
}

// ── B:摺線 —— 沒有一顆控制項會被攔腰切開 ────────────────────────
//
// ⚠ 缺陷的原始現場:預設尺寸(client 763×560、摺線 506)下,文字頁的
//   「全形/半形」區段標題在 y=497、高 22 —— 舊版把它裁成 9 DIP,
//   也就是從字的中間橫著切過去。使用者回報的是「摺線那裡看起來像壞掉」。

TEST(no_control_is_ever_cut_through_its_middle) {
  SetUiLang(UiLang::kZhHant);
  int cut = 0, whole = 0;
  for (int v = 0; v < kVariantCount; ++v) {
    for (int wh : {460, 560, 700}) {
      const PageLayout pl =
          LayoutSettingsPageDip(kVariants[v].page, 780, kVariants[v].state);
      const int smax =
          ScrollMaxDip(kVariants[v].page, 780, wh, kVariants[v].state);
      for (int scroll = 0; scroll <= smax; scroll += 13) {
        const int line = ContentClipLineDip(wh, scroll, smax);
        for (const PlacedControl& p : pl.items) {
          if (p.rect.empty()) continue;
          const ScrolledPlacement sp =
              ScrollPlaceControlDip(p.rect, scroll, line);
          // 只有兩種答案:整顆畫,或者一個像素都不畫。
          CHECK(sp.clip_h_dip == -1 || sp.clip_h_dip == 0);
          CHECK(sp.visible);
          if (sp.clip_h_dip == 0) ++cut; else ++whole;
        }
      }
    }
  }
  // 掃描範圍非空的兩面:真的有畫的,也真的有不畫的。
  CHECK(cut > 0);
  CHECK(whole > 0);
}

TEST(the_fade_strip_exists_only_when_there_is_more_below) {
  const int H = kWindowDefaultH;
  const int vp = ContentViewportHeightDip(H);
  // 內容放得下 → 沒有淡出區。
  CHECK_INT(ContentClipLineDip(H, 0, 0), vp);
  // 還有更多 → 讓出 kScrollFadeH。
  CHECK_INT(ContentClipLineDip(H, 0, 300), vp - kScrollFadeH);
  CHECK_INT(ContentClipLineDip(H, 299, 300), vp - kScrollFadeH);
  // 已經捲到底 → 沒有淡出區(下面沒有東西了,淡出等於騙人)。
  CHECK_INT(ContentClipLineDip(H, 300, 300), vp);
  // 呼叫端把 scroll 帶過頭(換頁/換 DPI 的那一格)也不可以讓它閃一下。
  CHECK_INT(ContentClipLineDip(H, 999, 300), vp);
  // 視窗矮到淡出區比可視高度還高:回 0,不回負數 ——
  // 負數會讓每一顆控制項都被判成跨線,整頁空白。
  CHECK(ContentClipLineDip(kBottomStripH + 4, 0, 300) >= 0);
  CHECK(ContentClipLineDip(10, 0, 300) >= 0);
}
