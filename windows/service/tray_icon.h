// windows/service/tray_icon.h — 自繪的系統匣圖示(中 / En)
//
// ── 為什麼這一支忽然變得必要 ────────────────────────────────────
//
// 那一橫本來是常駐的,所以托盤圖示只是備援入口 —— 醜一點沒關係。
// 而現在那一橫會**自己消失**(§12.10.6),托盤就從備援升格成
// 「服務活著時唯一必然存在的入口」。一顆認不出來的圖示等於沒有入口。
//
// 原本的寫法是:
//
//     nid.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
//
// 它旁邊的註解自己就寫著「醜,但看得到」。在 Win11 的托盤裡,那是一顆
// 與別的東西一模一樣的通用圖示 —— 使用者找不出哪一顆是輸入法。
//
// ── 為什麼是自繪而不是加一個 .ico ───────────────────────────────
//
// 全 repo 沒有任何 .rc / .ico(查過)。加資源檔要動 windows/build.sh 與
// CI 的建置命令,而那條路只有 windows-latest 驗得到;而且
// docs/theme-format.md §8.12 那條「不要自己發明圖示」的缺口還沒關。
//
// 用 GDI 畫兩個字反而落在規範**裡面**:§12.9.3 第 1 條規定
// 中 / En / 简 / 繁 這四個字面直接寫在繪製碼裡、不進 catalog,因為它們是
// **狀態指示**而不是介面文字。托盤那一格畫的正是同一種東西。
//
// ── 這一支刻意獨立成一個檔案 ────────────────────────────────────
//
// settings_window.cc 同時有另一條線在改(#79 設定視窗卡死)。把繪圖與
// 更新全部收在這裡,那邊只改兩行,衝突面壓到最小。
//
#ifndef RIMEWIN_SERVICE_TRAY_ICON_H_
#define RIMEWIN_SERVICE_TRAY_ICON_H_

#include <windows.h>

namespace rimewin {

// 畫一顆表示目前中英模式的托盤圖示。
//
// ⚠ **字面由呼叫端傳進來**,本檔一個中日韓字元都沒有。
//   那四個規範性字面(§8.12)全 repo 只住在 service/status_bar.cc 一個
//   地方,由 W7 / W10 兩個方向守著。在這裡再寫一份會多出第二份真相,
//   而「改名改一半」正是這個專案吃過虧的形狀 —— 所以走
//   status_bar.h 的 BarModeGlyph(),不替本檔開一個掃描例外。
//
// text 是要畫的字面(BarModeGlyph 給的)。字數影響字級:
// 「En」兩個字元要比一個漢字小,否則會撐出圓底之外。
//
// dpi 決定尺寸:96 → 16、120 → 20、144 → 24、192 → 32。
// ⚠ 回傳的 HICON 由呼叫端負責 DestroyIcon。失敗時回 nullptr,
//   呼叫端要退回 LoadIconW(nullptr, IDI_APPLICATION) —— 一顆醜圖示
//   仍然比沒有圖示好,而沒有圖示等於少一個入口。
HICON MakeModeTrayIcon(const wchar_t* text, UINT dpi);

// 同一支,但尺寸直接給像素。
//
// ⚠ 用途是**視窗類別的圖示**(WNDCLASSEXW.hIcon / hIconSm):
//   那兩格的尺寸來自 GetSystemMetrics(SM_CXICON / SM_CXSMICON),
//   不是 DPI 的那四階。硬把 dpi 湊成 192 去換 32 像素是可以,
//   但下一個人讀到 `MakeModeTrayIcon(g, 192)` 會以為那是高 DPI 的
//   路徑,而不是「我要 32 像素」。
// ⚠ 回傳的 HICON 由呼叫端負責。註冊給視窗類別的那一份
//   **不要 DestroyIcon** —— 類別活多久它就要活多久。
HICON MakeModeIconPx(const wchar_t* text, int px);

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_TRAY_ICON_H_
