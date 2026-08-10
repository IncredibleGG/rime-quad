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

#include <set>
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
  CHECK_INT(b.y - a.bottom(), space::s2);
  CHECK_INT(a.h, metric::kSidebarItemH);
  CHECK_INT(a.w, metric::kSidebarW - 2 * space::s5);
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
    {kPageSchemas, PageState{true, true}},
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
  const PageLayout pl =
      LayoutSettingsPageDip(kPageAppearance, 780, PageState{});
  auto find = [&](int id) {
    for (const PlacedControl& p : pl.items)
      if (p.id == id) return p.rect;
    return RectI{};
  };
  CHECK_INT(find(IDC_THEME_0).y, 574);
  CHECK_INT(find(IDC_THEME_1).y, 604);
  CHECK_INT(find(IDC_THEME_2).y, 634);
  CHECK_INT(find(IDC_BAR_SHOW).y, 754);
  CHECK_INT(find(IDC_APPEAR_NOTE).y, 810);
  CHECK_INT(pl.content_h_dip, 890);

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
  const PageLayout pl = LayoutSettingsPageDip(kPageAdvanced, 780, PageState{});
  int max_bottom = 0;
  for (const PlacedControl& p : pl.items)
    if (!p.rect.empty() && p.rect.bottom() > max_bottom)
      max_bottom = p.rect.bottom();
  CHECK_INT(max_bottom, 810);
  CHECK_INT(pl.content_h_dip, max_bottom + kContentPadBottomDip);
  // 三顆固定寬度的按鈕都要在內容高度以內。
  for (int id : {IDC_REDEPLOY, IDC_DIAG_COPY, IDC_RESET}) {
    for (const PlacedControl& p : pl.items)
      if (p.id == id) CHECK(p.rect.bottom() <= pl.content_h_dip);
  }
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
  int reset_y = -1, diag_copy_bottom = -1;
  for (const PlacedControl& p : pl.items) {
    if (p.id == IDC_RESET) reset_y = p.rect.y;
    if (p.id == IDC_DIAG_COPY) diag_copy_bottom = p.rect.bottom();
  }
  CHECK(reset_y > 0);
  CHECK(diag_copy_bottom > 0);
  CHECK(reset_y >= diag_copy_bottom + 2 * space::s7 + metric::kHairline);
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

  // 捲到一半:只露出可視範圍以內那一截,免得畫在底部固定列上面。
  const ScrolledPlacement half = ScrollPlaceControlDip(r, 120, viewport);
  CHECK(half.visible);
  CHECK_INT(half.y_dip, 480);
  CHECK_INT(half.clip_h_dip, 20);

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
    CHECK(!FindOn(kPageNetwork, state, IDC_NET_UPDATE).empty());
    CHECK(!FindOn(kPageNetwork, state, IDC_NETLOG_PATH).empty());
  }
}

TEST(ui_layout_network_page_clear_button_is_last_and_behind_a_divider) {
  // §4.9 / §2-C2:破壞性動作是該頁最後一個區塊,與上面隔一條 hairline + s7。
  // 清除連網紀錄是破壞性的 —— 清掉之後,使用者用來稽核我們的那份證據
  // 就找不回來了。
  const PageState has_rows{false, false};
  const PageLayout pl = LayoutSettingsPageDip(kPageNetwork, 780, has_rows);
  RectI clear{}, path{};
  int max_bottom = 0;
  for (const PlacedControl& p : pl.items) {
    if (p.id == IDC_NETLOG_CLEAR) clear = p.rect;
    if (p.id == IDC_NETLOG_PATH) path = p.rect;
    if (!p.rect.empty() && p.rect.bottom() > max_bottom)
      max_bottom = p.rect.bottom();
  }
  CHECK(!clear.empty());
  CHECK(!path.empty());
  // 它是最後一顆。
  CHECK_INT(clear.bottom(), max_bottom);
  // 而且與上面隔著一條分隔線的距離(hairline + 兩段 s7)。
  CHECK(clear.y >= path.bottom() + 2 * space::s7 + metric::kHairline);
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
