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
  r.x = space::s5;
  r.y = space::s5 + index * (metric::kSidebarItemH + space::s2);
  r.w = metric::kSidebarW - 2 * space::s5;
  r.h = metric::kSidebarItemH;
  return r;
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
                                 bool schema_list_empty) {
  PageLayout out;
  const int cx = ContentXDip(window_w_dip);
  const int cw = ContentWidthDip(window_w_dip);
  const int t5h = text_size::t5 + space::s2;
  const int btn_h = metric::kMinTarget + space::s2;

  Stack st(cx, kContentTopDip, cw);

  auto emit = [&](int id, const RectI& r, bool clickable, const char* what) {
    out.items.push_back(PlacedControl{id, r, clickable, what});
  };
  // 這一頁上不出現的控制項:空矩形。呼叫端據此隱藏 ——
  // 「建了但不屬於任何一頁」在結構上不可能發生,靠的就是這一份清單
  //  是**唯一**的來源。
  auto hide = [&](int id) { emit(id, RectI{}, false, "not_on_this_page"); };

  auto title_block = [&](int title_id, int sub_id) {
    emit(title_id, st.Push(text_size::t1 + space::s3, space::s1), false,
         "page_title");
    emit(sub_id, st.Push(t5h, space::s7), false, "page_subtitle");
  };
  auto heading = [&](int head_id, int blurb_id, int blurb_lines) {
    emit(head_id, st.Push(text_size::t2 + space::s2, space::s1), false,
         "section_heading");
    if (blurb_id)
      emit(blurb_id, st.Push(t5h * blurb_lines, space::s3), false,
           "section_blurb");
  };
  // 單選群組:一列一個(桌面欄的密度)。
  // ⚠ 每一顆的 id 都**寫出來**,不用 `first + i`。理由是守門:
  //   check_ui_spec.sh 的 W24 逐字比對這裡與 settings_window.cc 的
  //   kControls,而 `first + i` 讓一半的 id 在文字上不存在 ——
  //   於是「表上多了三顆、版面上沒有」就檢查不到了。
  auto radios = [&](std::initializer_list<int> ids, const char* what) {
    for (int id : ids)
      emit(id, st.Push(metric::kMinTarget, space::s1), true, what);
    st.Skip(space::s7 - space::s1);
  };
  // 固定寬度的按鈕。⚠ **一定要推進堆疊**,見上面的說明。
  auto button = [&](int id, int w, int gap, const char* what) {
    const RectI row = st.Push(btn_h, gap);
    emit(id, RectI{row.x, row.y, w, row.h}, true, what);
  };

  switch (page) {
    case kPageSchemas: {
      title_block(IDC_SCHEMAS_TITLE, IDC_SCHEMAS_SUB);
      heading(IDC_SCHEMAS_LIST_HEAD, IDC_SCHEMAS_LIST_BLURB, 2);
      const int list_h = 4 * metric::kSidebarItemH + space::s3;
      if (schema_list_empty) {
        hide(IDC_SCHEMA_LIST);
        hide(IDC_UP);
        hide(IDC_DOWN);
        hide(IDC_APPLY_ORDER);
        hide(IDC_SCHEMAS_DEFAULT_LINE);
        emit(IDC_SCHEMAS_EMPTY, st.Push(t5h * 4, space::s7), false,
             "empty_state");
      } else {
        hide(IDC_SCHEMAS_EMPTY);
        emit(IDC_SCHEMA_LIST, st.Push(list_h, space::s3), true, "schema_list");
        const RectI row = st.Push(btn_h, space::s3);
        const int bw = (cw - 2 * space::s3) / 3;
        emit(IDC_UP, RectI{row.x, row.y, bw, row.h}, true, "move_up");
        emit(IDC_DOWN, RectI{row.x + bw + space::s3, row.y, bw, row.h}, true,
             "move_down");
        emit(IDC_APPLY_ORDER,
             RectI{row.x + 2 * (bw + space::s3), row.y, bw, row.h}, true,
             "apply_order");
        emit(IDC_SCHEMAS_DEFAULT_LINE, st.Push(t5h, space::s7), false,
             "default_line");
      }
      emit(IDC_FOLLOW_MODE, st.Push(metric::kSidebarItemH, space::s1), true,
           "follow_input_mode_switch");
      emit(IDC_FOLLOW_BLURB, st.Push(t5h * 3, space::s7), false,
           "follow_blurb");
      break;
    }
    case kPageAppearance: {
      title_block(IDC_APPEAR_TITLE, IDC_APPEAR_SUB);
      heading(IDC_COUNT_HEAD, IDC_COUNT_BLURB, 2);
      radios({IDC_COUNT_0, IDC_COUNT_1, IDC_COUNT_2, IDC_COUNT_3, IDC_COUNT_4},
             "cand_count_radio");
      heading(IDC_SCALE_HEAD, IDC_SCALE_BLURB, 1);
      radios({IDC_SCALE_0, IDC_SCALE_1, IDC_SCALE_2, IDC_SCALE_3, IDC_SCALE_4},
             "cand_scale_radio");
      heading(IDC_THEME_HEAD, IDC_THEME_BLURB, 1);
      radios({IDC_THEME_0, IDC_THEME_1, IDC_THEME_2}, "appearance_radio");
      heading(IDC_BAR_HEAD, IDC_BAR_BLURB, 3);
      emit(IDC_BAR_SHOW, st.Push(metric::kSidebarItemH, space::s7), true,
           "status_bar_switch");
      emit(IDC_APPEAR_NOTE, st.Push(t5h * 4, 0), false, "honest_note");
      break;
    }
    case kPageText: {
      title_block(IDC_TEXT_TITLE, IDC_TEXT_SUB);
      heading(IDC_VARIANT_HEAD, IDC_VARIANT_BLURB, 1);
      radios({IDC_VARIANT_0, IDC_VARIANT_1, IDC_VARIANT_2}, "variant_radio");
      heading(IDC_PUNCT_HEAD, IDC_PUNCT_BLURB, 2);
      radios({IDC_PUNCT_0, IDC_PUNCT_1, IDC_PUNCT_2}, "punct_radio");
      break;
    }
    case kPageAdvanced: {
      title_block(IDC_ADV_TITLE, IDC_ADV_SUB);
      heading(IDC_REDEPLOY_HEAD, IDC_REDEPLOY_BLURB, 2);
      button(IDC_REDEPLOY, 180, space::s7, "redeploy_button");
      heading(IDC_FILES_HEAD, IDC_FILES_BLURB, 2);
      {
        const RectI row = st.Push(btn_h, space::s7);
        const int bw = (cw - space::s3) / 2;
        emit(IDC_OPEN_USER_DIR, RectI{row.x, row.y, bw, row.h}, true,
             "open_user_dir");
        emit(IDC_OPEN_SETTINGS_FILE,
             RectI{row.x + bw + space::s3, row.y, bw, row.h}, true,
             "open_settings_file");
      }
      heading(IDC_LANG_HEAD, IDC_LANG_BLURB, 1);
      radios({IDC_LANG_0, IDC_LANG_1, IDC_LANG_2, IDC_LANG_3},
             "ui_language_radio");
      heading(IDC_DIAG_HEAD, IDC_DIAG_NOTE, 2);
      emit(IDC_DIAG, st.Push(t5h * 6, space::s3), true, "diagnostics_edit");
      button(IDC_DIAG_COPY, 120, 0, "diagnostics_copy");
      // ⚠ 危險操作一律是該頁最後一個區塊,上面隔一條 hairline + s7
      //   (§4.9 / §2-C2)。PushDivider 就是那條線。
      st.PushDivider();
      heading(IDC_RESET_HEAD, IDC_RESET_BLURB, 2);
      button(IDC_RESET, 220, 0, "reset_button");
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

int ScrollMaxDip(int page, int window_w_dip, int window_h_dip,
                 bool schema_list_empty) {
  const PageLayout pl =
      LayoutSettingsPageDip(page, window_w_dip, schema_list_empty);
  return std::max(0, pl.content_h_dip - ContentViewportHeightDip(window_h_dip));
}

ScrolledPlacement ScrollPlaceControlDip(const RectI& content_rect,
                                        int scroll_dip, int viewport_h_dip) {
  ScrolledPlacement p;
  p.y_dip = content_rect.y - scroll_dip;
  // ⚠ 只裁不藏。這一行就是整個捲動能不能給鍵盤使用者用的分界。
  p.visible = true;
  p.clip_h_dip = -1;
  // 子視窗本來就會被父視窗的 client 矩形裁掉,所以**上方**不必處理。
  // 要處理的只有下方那 54 DIP:底部固定列與它上面那條 hairline ——
  // 那一塊仍然在 client 裡面,捲到一半的控制項會畫在「關閉」鈕上面。
  if (!content_rect.empty() && p.y_dip + content_rect.h > viewport_h_dip)
    p.clip_h_dip = std::max(0, viewport_h_dip - p.y_dip);
  return p;
}

std::vector<HitTarget> ClickableTargetsDip(int window_w_dip, int window_h_dip,
                                           int page, bool schema_list_empty) {
  std::vector<HitTarget> out;
  // 側欄的每一頁(不捲動)。
  for (int i = 0; i < kPageCount; ++i)
    out.push_back(HitTarget{"sidebar_item", SidebarItemDip(i), IDC_SIDEBAR,
                            false});
  // 底部固定列的關閉鈕(不捲動)。它與 settings_window.cc 用的是同一組
  // 常數 —— 那一列的位置是唯一還由呼叫端算的東西,所以它必須在這裡驗。
  out.push_back(HitTarget{"close_button",
                          RectI{window_w_dip - space::s7 - 100,
                                window_h_dip - kBottomBarH, 100,
                                metric::kMinTarget},
                          IDC_CLOSE, false});

  const PageLayout pl =
      LayoutSettingsPageDip(page, window_w_dip, schema_list_empty);
  for (const PlacedControl& p : pl.items) {
    if (!p.clickable || p.rect.empty()) continue;
    out.push_back(HitTarget{p.what, p.rect, p.id, true});
  }
  return out;
}

}  // namespace rimewin
