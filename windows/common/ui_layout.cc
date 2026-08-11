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
  r.y = space::s5 + index * metric::kSidebarItemH;
  r.w = metric::kSidebarW;
  r.h = metric::kSidebarItemH;
  return r;
}

RectI SidebarItemFillDip(int index) {
  RectI r = SidebarItemDip(index);
  r.x += space::s5;
  r.w -= 2 * space::s5;
  if (r.w < 0) r.w = 0;
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

namespace {

// 這個碼點畫出來大約是一個字級寬(全形)嗎。
//
// ⚠ 只求「夠不夠寬」,不求精確:分錯一個字的代價是那一行多算/少算
//   0.4 個字級,而可用寬度已經先扣掉 1/7 了。
bool IsWideCodePoint(unsigned int c) {
  return (c >= 0x1100u && c <= 0x115Fu) ||   // 韓文字母
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

}  // namespace

int EstimateTextLinesDip(const wchar_t* text, int size_dip, int w_dip) {
  if (!text || !*text || size_dip <= 0 || w_dip <= 0) return 1;
  // ⚠ 只用欄寬的 6/7。見標頭:那 1/7 是留給拉丁逐詞斷行的鋸齒邊。
  const int usable = w_dip * 6 / 7;
  if (usable <= size_dip) return 1;
  // 單位是 1/100 DIP,免得整數除法把每一個字都抹掉一點。
  const long usable100 = static_cast<long>(usable) * 100;
  long x = 0;
  int lines = 1;
  for (const wchar_t* p = text; *p; ++p) {
    const unsigned int c = static_cast<unsigned int>(*p);
    if (c == 0x0Au) {  // 明寫的換行
      ++lines;
      x = 0;
      continue;
    }
    if (c == 0x0Du) continue;
    const long w = static_cast<long>(size_dip) * (IsWideCodePoint(c) ? 100 : 60);
    if (x + w > usable100) {
      ++lines;
      x = 0;
    }
    x += w;
  }
  return lines;
}

int EstimateTextBoxHeightDip(const wchar_t* text, int size_dip, int w_dip) {
  return EstimateTextLinesDip(text, size_dip, w_dip) * TextLineBoxDip(size_dip);
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
  // ── ⚠ 一行說明文字的**盒高** ──────────────────────────────
  //
  // 舊版是 `text_size::t5 + space::s2` = 11 + 4 = **15**,而
  // TextLineBoxDip(11) = 11×4/3 + 2 = **16**。也就是每一行少 1 DIP,
  // 而 DrawTextW 沒有 DT_NOCLIP —— 中文字的下緣(那一橫、豎鉤的鉤)
  // 會被切掉一點點。單看一行看不出來,整段看起來像字型沒調好。
  //
  // 行盒高度只有一個來源:TextLineBoxDip()。
  const int t5h = TextLineBoxDip(text_size::t5);
  const int btn_h = metric::kMinTarget + space::s2;

  Stack st(cx, kContentTopDip, cw);

  auto emit = [&](int id, const RectI& r, bool clickable, const char* what) {
    out.items.push_back(PlacedControl{id, r, clickable, what});
  };
  // 這一頁上不出現的控制項:空矩形。呼叫端據此隱藏 ——
  // 「建了但不屬於任何一頁」在結構上不可能發生,靠的就是這一份清單
  //  是**唯一**的來源。
  auto hide = [&](int id) { emit(id, RectI{}, false, "not_on_this_page"); };

  // ── ⚠ 高度從**字**算出來,不是各自寫死 ──────────────────────
  //
  // #76 的根因就是「每一段各自寫死行數」。單看每一段都很寬鬆合理,
  // 六段加起來多了 195 DIP,而使用者付的代價是一顆碰不到的按鈕。
  //
  // ⚠ 這是估算,真正的斷行是 GDI 做的(見 EstimateTextLinesDip 的檔頭)。
  //   它刻意抓得偏高:少算一行 = 文字被切掉,多算一行 = 多一點空白。
  auto text_h = [&](UiString s) {
    return EstimateTextBoxHeightDip(UiText(s), text_size::t5, cw);
  };
  // 文字要到執行期才知道的那幾格,取候選裡最高的那一個。
  auto text_h_max = [&](UiString a, UiString b) {
    return std::max(text_h(a), text_h(b));
  };

  auto title_block = [&](int title_id, int sub_id, UiString sub) {
    emit(title_id, st.Push(text_size::t1 + space::s3, space::s1), false,
         "page_title");
    // ⚠ 副標以前固定一行,而每一頁的副標都是完整的一句話 ——
    //   660 DIP 寬的視窗上它一定不只一行,於是後半句被切掉。
    emit(sub_id, st.Push(text_h(sub), space::s7), false, "page_subtitle");
  };
  auto heading = [&](int head_id, int blurb_id, UiString blurb) {
    emit(head_id, st.Push(text_size::t2 + space::s2, space::s1), false,
         "section_heading");
    if (blurb_id)
      emit(blurb_id, st.Push(text_h(blurb), space::s3), false, "section_blurb");
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
      title_block(IDC_SCHEMAS_TITLE, IDC_SCHEMAS_SUB,
                  UiString::kSchemasSubtitle);
      heading(IDC_SCHEMAS_LIST_HEAD, IDC_SCHEMAS_LIST_BLURB,
              UiString::kSchemasListBlurb);
      const int list_h = 4 * metric::kSidebarItemH + space::s3;
      if (state.schema_list_empty) {
        hide(IDC_SCHEMA_LIST);
        hide(IDC_UP);
        hide(IDC_DOWN);
        hide(IDC_APPLY_ORDER);
        hide(IDC_SCHEMAS_DEFAULT_LINE);
        // ⚠ 文字執行期才組(§4.7 的空狀態要說「為什麼是空的」),
        //   所以這一格仍然是固定行數。抓 4 行是刻意寬鬆的一邊。
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
      emit(IDC_FOLLOW_BLURB,
           st.Push(text_h(UiString::kSchemasFollowBlurb), space::s7), false,
           "follow_blurb");
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
      emit(IDC_BAR_SHOW, st.Push(metric::kSidebarItemH, space::s7), true,
           "status_bar_switch");
      emit(IDC_APPEAR_NOTE,
           st.Push(text_h(UiString::kAppearanceHonestNote), 0), false,
           "honest_note");
      break;
    }
    case kPageText: {
      title_block(IDC_TEXT_TITLE, IDC_TEXT_SUB, UiString::kTextSubtitle);
      heading(IDC_VARIANT_HEAD, IDC_VARIANT_BLURB, UiString::kVariantBlurb);
      radios({IDC_VARIANT_0, IDC_VARIANT_1, IDC_VARIANT_2}, "variant_radio");
      heading(IDC_PUNCT_HEAD, IDC_PUNCT_BLURB, UiString::kPunctBlurb);
      radios({IDC_PUNCT_0, IDC_PUNCT_1, IDC_PUNCT_2}, "punct_radio");
      break;
    }
    case kPageNetwork: {
      title_block(IDC_NET_TITLE, IDC_NET_SUB, UiString::kNetworkSubtitle);
      // ⚠ 開關**沒有**自己的區段標題。頁標題已經是「連網」,再寫一次
      //   只是把同一個詞說三遍 —— 而 §4.1 要的是「開關在右、標題說明
      //   在左」,那個標題就是開關自己的文字。
      emit(IDC_NET_SWITCH, st.Push(metric::kSidebarItemH, space::s1), true,
           "network_switch");
      // 開著/關著各一句,由 common/net_ui.h 的 NetSwitchSummary() 決定
      // 哪一句 —— 那是純函式,而「兩種狀態說同一句話」有單元測試擋著。
      // 開著一句、關著一句,哪一句由 NetSwitchSummary() 決定 —— 兩句
      // 都可能出現在同一個框裡,所以取**比較高的那一句**。
      emit(IDC_NET_STATE,
           st.Push(text_h_max(UiString::kNetworkOnSummary,
                              UiString::kNetworkOffSummary),
                   space::s3),
           false, "network_state");
      // 誠實的代價(DNS/SNI 是明文)。⚠ 這一段**開著關著都在**:
      // 只在開著時才出現的話,使用者是在按下開關**之後**才讀到代價,
      // 而那等於沒說。
      emit(IDC_NET_DETAIL,
           st.Push(text_h(UiString::kNetworkOnDetail), space::s7), false,
           "network_cost_note");

      // ── 更新 ──────────────────────────────────────────────
      //
      // ⚠ IDC_UPDATE_TRUST(沒有數位簽章那一句)排在**按鈕之前**。
      //   排在後面的話,使用者按下去的時候還沒讀到它 —— 而那句話的
      //   全部價值就在於他按之前就知道。
      heading(IDC_UPDATE_HEAD, IDC_UPDATE_BLURB, UiString::kUpdateBlurb);
      emit(IDC_UPDATE_TRUST,
           st.Push(text_h(UiString::kUpdateTrustAnchor), space::s3), false,
           "update_trust_anchor");
      emit(IDC_UPDATE_WHAT,
           st.Push(text_h(UiString::kUpdateWhatHappens), space::s3), false,
           "update_what_happens");
      // ⚠ 這一格的那句話是執行期依結果組出來的(五種狀態 + 幾種失敗),
      //   所以仍然是固定行數,而且抓最長的那一種還要再寬一點。
      emit(IDC_UPDATE_STATUS, st.Push(t5h * 3, space::s3), false,
           "update_status");
      {
        const RectI row = st.Push(btn_h, space::s7);
        const int bw = (cw - 2 * space::s3) / 3;
        emit(IDC_UPDATE_CHECK, RectI{row.x, row.y, bw, row.h}, true,
             "update_check");
        emit(IDC_UPDATE_ACTION,
             RectI{row.x + bw + space::s3, row.y, bw, row.h}, true,
             "update_action");
        emit(IDC_UPDATE_PAGE,
             RectI{row.x + 2 * (bw + space::s3), row.y, bw, row.h}, true,
             "update_page");
      }

      heading(IDC_NETLOG_HEAD, IDC_NETLOG_BLURB, UiString::kNetLogBlurb);
      if (state.net_log_empty) {
        hide(IDC_NETLOG_SUMMARY);
        hide(IDC_NETLOG_COLS);
        hide(IDC_NETLOG_LIST);
        // §4.7 的空狀態:為什麼是空的、這是不是正常。
        emit(IDC_NETLOG_EMPTY, st.Push(t5h * 4, space::s3), false,
             "empty_state");
        emit(IDC_NETLOG_PATH, st.Push(t5h * 2, 0), false, "log_file_path");
        // ⚠ 沒有東西可以清的時候**不給**清除鍵。一顆按下去什麼都不會
        //   發生的危險鍵,比沒有那顆鍵更難理解。
        hide(IDC_NETLOG_CLEAR_HEAD);
        hide(IDC_NETLOG_CLEAR_BLURB);
        hide(IDC_NETLOG_CLEAR);
      } else {
        hide(IDC_NETLOG_EMPTY);
        emit(IDC_NETLOG_SUMMARY, st.Push(t5h, space::s3), false, "log_count");
        emit(IDC_NETLOG_COLS, st.Push(t5h, space::s2), false, "log_columns");
        emit(IDC_NETLOG_LIST,
             st.Push(6 * metric::kSidebarItemH + space::s3, space::s3), true,
             "net_log_list");
        emit(IDC_NETLOG_PATH, st.Push(t5h * 2, 0), false, "log_file_path");
        // ⚠ 危險操作一律是該頁最後一個區塊,上面隔一條 hairline + s7
        //   (§4.9 / §2-C2)。清除紀錄是破壞性的:清掉之後,使用者拿來
        //   稽核我們的那份證據就找不回來了。
        st.PushDivider();
        heading(IDC_NETLOG_CLEAR_HEAD, IDC_NETLOG_CLEAR_BLURB,
                UiString::kNetLogClearBlurb);
        button(IDC_NETLOG_CLEAR, 220, 0, "clear_log_button");
      }
      break;
    }
    case kPageAdvanced: {
      title_block(IDC_ADV_TITLE, IDC_ADV_SUB, UiString::kAdvancedSubtitle);
      heading(IDC_REDEPLOY_HEAD, IDC_REDEPLOY_BLURB,
              UiString::kRedeployBlurb);
      button(IDC_REDEPLOY, 180, space::s7, "redeploy_button");
      heading(IDC_FILES_HEAD, IDC_FILES_BLURB, UiString::kFilesBlurb);
      {
        const RectI row = st.Push(btn_h, space::s7);
        const int bw = (cw - space::s3) / 2;
        emit(IDC_OPEN_USER_DIR, RectI{row.x, row.y, bw, row.h}, true,
             "open_user_dir");
        emit(IDC_OPEN_SETTINGS_FILE,
             RectI{row.x + bw + space::s3, row.y, bw, row.h}, true,
             "open_settings_file");
      }
      heading(IDC_LANG_HEAD, IDC_LANG_BLURB, UiString::kLanguageBlurb);
      radios({IDC_LANG_0, IDC_LANG_1, IDC_LANG_2, IDC_LANG_3},
             "ui_language_radio");
      heading(IDC_DIAG_HEAD, IDC_DIAG_NOTE, UiString::kDiagnosticsNote);
      emit(IDC_DIAG, st.Push(t5h * 6, space::s3), true, "diagnostics_edit");
      button(IDC_DIAG_COPY, 120, 0, "diagnostics_copy");
      // ⚠ 危險操作一律是該頁最後一個區塊,上面隔一條 hairline + s7
      //   (§4.9 / §2-C2)。PushDivider 就是那條線。
      st.PushDivider();
      heading(IDC_RESET_HEAD, IDC_RESET_BLURB, UiString::kResetBlurb);
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
                 PageState state) {
  const PageLayout pl = LayoutSettingsPageDip(page, window_w_dip, state);
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
                                           int page, PageState state) {
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

  const PageLayout pl = LayoutSettingsPageDip(page, window_w_dip, state);
  for (const PlacedControl& p : pl.items) {
    if (!p.clickable || p.rect.empty()) continue;
    out.push_back(HitTarget{p.what, p.rect, p.id, true});
  }
  return out;
}

}  // namespace rimewin
