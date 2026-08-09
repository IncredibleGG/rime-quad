// windows/service/settings_window.h — 設定介面
//
// ── 技術選型:純 Win32 + 通用控制項 v6 ──────────────────────────
//
// 硬約束是「不要 Electron、不要打包 Chromium」,而它背後的理由不只是體積:
// 這個專案的定位是離線、經得起審計。使用者要能相信「它不連網」,
// 而說服他的方式是 `dumpbin /imports` 看得出來沒有網路 DLL ——
// 塞一個瀏覽器引擎進來,那句話就再也不可能驗證了。
//
// 被排除的與理由:
//
//   · **Electron / WebView2** — 前者打包 Chromium(上百 MB,而且是一整個
//     網路堆疊);後者相依於系統上的 Edge 執行期,那同樣是 Chromium,
//     而且**不在我們的控制之下**(它會自己更新)。兩者都直接違反約束。
//   · **WinUI 3 / XAML Islands** — 需要 Windows App SDK 的可轉散發套件,
//     而且與貫穿全案的 `/MT` 靜態 CRT 打架。
//   · **WPF / WinForms** — 需要 .NET 執行期。
//   · **本機 HTTP + 系統瀏覽器** — 會開 socket。離線定位下這是最糟的選項。
//
// 剩下的是純 Win32。代價是排版要自己算、控制項醜、無障礙要自己顧;
// 換到的是**零額外相依、與 /MT 相容、二進位增加的大小解釋得清楚**。
//
// ── 2026-08-09:改成側欄式,並修掉四類缺陷 ──────────────────────
//
// 版面從分頁(WC_TABCONTROLW)改成側欄(docs/ui-design.md §12.4)。
// **判準吃的是產品的頁數,不是某一端實作了幾頁** —— 桌面端是 7 頁,
// 所以是側欄;照今天實作的 4 頁做分頁,等市集/連網/自己加的詞上線就要整個重做。
//
// 同時修掉的:
//   1. `dpi_scale_` 只算一次而進程是 per-monitor-v2 → 現在處理 WM_DPICHANGED。
//   2. 無條件捨去的縮放 → 一律走 MulDivRound(common/ui_dip.h)。
//   3. 字型走 SPI_GETNONCLIENTMETRICS(違反 §8.6.0)→ 走 ui_font.h。
//   4. 清單每一列印著方案 id(違反 §6.7 第一層)→ 只印名字。
//   5. MB_YESNO 做確認(按鈕字面由系統決定,必然違反 §2-C3)→ ui_confirm.h。
//   6. 硬編的中文字面值 → common/ui_strings.h,英/繁/簡三語。
//
// ── ⚠ 第六個「看得到但摸不到」,以及它為什麼不會再發生 ──────────
//
// 舊版有一顆 `IDC_FOLLOW_MODE` 核取方塊:建立了、有 WM_COMMAND 處理常式、
// 有讀寫設定的完整程式碼 —— 但它**不在任何一頁的 id 陣列裡**,
// 而控制項是用 `WS_CHILD`(沒有 `WS_VISIBLE`)建的,只有 ShowTab 會去
// 顯示它們。於是那顆核取方塊從來沒有被 ShowWindow 過:
// **它在畫面上根本不存在,而程式碼看起來完全正常。**
//
// 這一輪的修法不是「把它加進陣列」,那只修掉這一次。
// 改成:控制項由**一張表**(kControls)產生,表上每一列都帶著它屬於哪一頁。
// 建立與顯示走同一份資料,所以「建了但沒有頁」在結構上不可能發生 ——
// 一顆控制項要嘛不存在,要嘛屬於某一頁而且會被顯示。
//
#ifndef RIMEWIN_SERVICE_SETTINGS_WINDOW_H_
#define RIMEWIN_SERVICE_SETTINGS_WINDOW_H_

#include <windows.h>

// commctrl.h 要在 windows.h 之後。⚠ 它在**標頭**裡是必要的:
// NM_CUSTOMDRAW 的處理常式簽章帶著 NMLVCUSTOMDRAW,而那個型別在這裡。
#include <commctrl.h>

#include <string>
#include <vector>

#include "../common/schema_choice.h"
// ⚠ 頁、控制項 id 與**每一頁的版面**都住在這裡面。本檔不再自己算矩形。
#include "../common/ui_layout.h"
#include "../common/ui_strings.h"
#include "engine.h"
#include "settings_store.h"
#include "ui_font.h"
#include "ui_theme.h"

namespace rimewin {

class CandidateWindow;
class StatusBar;

class SettingsWindow {
 public:
  SettingsWindow(Engine* engine, SettingsStore* store,
                 const std::string& shared_dir);
  ~SettingsWindow();

  bool Start();
  void Stop();

  // 可從任何執行緒呼叫。已經開著就把它帶到最前面。
  void Open();
  // 開在某一頁上(懸浮狀態列的「輸入法沒有在跑」會直接帶到「進階」)。
  void OpenAt(int page);

  void SetCandidateWindow(CandidateWindow* w) { cand_ = w; }
  void SetStatusBar(StatusBar* b) { bar_ = b; }

  // ── 簡繁:設定視窗**以外**的入口 ────────────────────────────
  // 語言設定檔收斂成一份之後,使用者再也不能用 Win+空白鍵切簡繁,
  // 所以那件事必須在我們自己的 UI 裡做得到。
  void SetVariantPref(VariantPref v);
  VariantPref CurrentVariantPref();

  // 懸浮狀態列要用的:目前的介面語言與外觀,好讓兩個表面長得一樣。
  UiLang ui_lang() const { return ui_lang_; }

 private:
  // ⚠ 頁的順序(kPageSchemas … kPageCount)與控制項 id 都在
  //   common/ui_layout.h。它們**不可以**住在這裡:住在這裡就表示
  //   版面測不到,而「外觀頁的深淺三態排到視窗外而 W18 全綠」
  //   就是那樣發生的。

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
  static DWORD WINAPI ThreadEntry(LPVOID self);
  void ThreadMain();

  void CreateUi(HWND hwnd);
  void LayoutUi();
  void ShowPage(int page);
  void ReloadFromSettings();
  void ReloadSchemaList();
  void OnCommand(int id, int code);
  void OnNotify(NMHDR* nm, LRESULT* result);
  void OnDeployTick();
  void OnPaint(HDC hdc);
  void OnDpiChanged(UINT dpi, const RECT* suggested);
  void RefreshTheme();
  void ApplyFonts();

  // ── 內容區的捲動(§12.4 A2)────────────────────────────────
  //
  // ⚠ 為什麼一定要有:外觀頁的內容高 890 DIP,而 150% 的 1080p 筆電
  //   client 高約 667 DIP —— **視窗拉到最大也碰不到深淺色三態**,
  //   而那三顆是整個 UI 上唯一的入口。加大最小尺寸解決不了,
  //   因為使用者的螢幕就那麼高。
  void SetScroll(int dip);
  void OnVScroll(int code, int track_pos);
  void OnMouseWheel(int delta);
  // Win32 **不會**自動把鍵盤焦點捲進可見範圍。少了這一支,Tab 走到
  // 視窗外的控制項時畫面完全不動,使用者在盲按。
  void EnsureFocusVisible();
  // 把控制項裁掉伸進底部固定列的那一截(子視窗只會被父視窗的 client
  // 矩形裁掉,而底部那 54 DIP 仍在 client 裡面)。
  // 裁掉底部固定列蓋到的那一截。**clip_h_dip < 0 = 不裁**;
  // 它由 ScrollPlaceControlDip() 算出來,這裡不再自己判斷 ——
  // 自己判斷的那一版單元測試看不到。
  void ClipToViewport(int index, HWND c, int w_dip, int clip_h_dip);

  // 自繪(§12.5.3 的六類裡的四類在這個檔案)。
  LRESULT DrawSidebar(NMLVCUSTOMDRAW* cd);
  LRESULT DrawSchemaList(NMLVCUSTOMDRAW* cd);
  void DrawDangerButton(DRAWITEMSTRUCT* di);

  void AddTray();
  void RemoveTray();
  void OnTray(WPARAM w, LPARAM l);

  void ApplyVariantNow();
  void CommitVariantPref(VariantPref v);
  void ApplyPunctNow();
  void ApplyAppearancePref();
  void ApplyUiLanguage();
  void ApplyScaleNow();
  void ApplyStatusBarVisibility();
  void DoResetSettings();
  bool ApplyOrderAndPageSize(std::string* error);
  void StartRedeploy(UiString why);
  void SetStatus(const std::wstring& text);
  void SetStatus(UiString s) { SetStatus(UiText(s)); }
  // 成功訊息 4 秒後自己清掉(§12.5.3:不做浮層,成功不值得一個新表面)。
  void SetTransientStatus(UiString s);

  int SelectedSchemaRow() const;
  std::wstring SchemaDisplayName(size_t index) const;
  Script script() const;

  Engine* engine_;
  SettingsStore* store_;
  std::string shared_dir_;
  CandidateWindow* cand_ = nullptr;
  StatusBar* bar_ = nullptr;

  HANDLE thread_ = nullptr;
  DWORD thread_id_ = 0;
  HANDLE ready_ = nullptr;
  HWND hwnd_ = nullptr;
  HWND sidebar_ = nullptr;
  HWND schema_list_ = nullptr;

  // ⚠ **沒有 dpi_scale_。** 那個 double 正是舊版每一項各少 0~1 px、
  //   而誤差沿著版面累積的來源。一律 MulDivRound(dip, dpi_, 96)。
  UINT dpi_ = 96;
  FontSet fonts_;
  Theme theme_;
  UiLang ui_lang_ = UiLang::kZhHant;
  int page_ = kPageSchemas;
  // 鍵盤使用時才畫焦點環(§12.6.4 第 1 條)。滑鼠使用者身上到處是框,
  // 是 Win32 自繪最常見的破綻。WM_UPDATEUISTATE 維護它。
  bool show_focus_ = false;

  // 內容區的捲動量與上限,單位 DIP(不是像素 —— 換螢幕時 DPI 會變)。
  int scroll_ = 0;
  int scroll_max_ = 0;
  // ⚠ SetScrollInfo 讓捲軸出現/消失時會送 WM_SIZE,而 WM_SIZE 又叫
  //   LayoutUi。沒有這個旗標就是無限遞迴。
  bool in_layout_ = false;
  // 每一顆控制項目前被裁掉之後剩下的高度(DIP)。-1 = 沒有裁。
  // ⚠ 存著是為了**只在變動時**才呼叫 SetWindowRgn:那一支會重畫,
  //   每次 LayoutUi 都無條件呼叫的話,捲動時整頁會閃。
  std::vector<int> clip_h_;

  bool deploying_ = false;
  uint32_t deploy_seq_ = 0;
  DWORD deploy_start_ = 0;
  UiString deploy_why_ = UiString::kRedeployButton;
  bool has_rollback_ = false;
  std::string rollback_yaml_;

  bool tray_added_ = false;
  UINT taskbar_created_ = 0;

  Settings settings_;
  std::vector<std::pair<std::string, std::string>> schemas_;  // id, name
  std::vector<std::string> order_;   // 目前清單上的順序(id)
};

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_SETTINGS_WINDOW_H_
