// windows/service/ui_confirm.h — 二次確認與提示(§4.9 / §2-C3 / §2-C4)
//
// ── 為什麼不用 MessageBoxW ──────────────────────────────────────
//
// `MessageBoxW` 的按鈕字面由**系統**決定。`MB_YESNO` 在正體中文的
// Windows 上一定是「是／否」,而 §2-C3 明文禁止確認鍵屬於
// {確定, 好, OK, 是, Yes} —— 理由是「是」沒有回答「是什麼」:
// 使用者在對話框上讀完一段話,按下「是」的時候已經不記得那段話了。
// 規範要的是**確認鍵寫出它會做什麼**(「刪掉這 42 個詞」)。
//
// §2-C4 還要求預設焦點在取消。`MB_DEFBUTTON2` 只換位置不換字,
// 兩條都達不到,所以確認對話框只能自己做。
//
// ⚠ 這裡刻意**不**做成 owner-draw 的按鈕:§12.5.1 的判準是
//   「只有在系統控制項無法表達規範要求的某個狀態時才 owner-draw」,
//   而按鈕字面是我們自己給的,系統 BUTTON 完全表達得了。
//   自繪只會賠掉 MSAA 的角色。
//
#ifndef RIMEWIN_SERVICE_UI_CONFIRM_H_
#define RIMEWIN_SERVICE_UI_CONFIRM_H_

#include <windows.h>

#include <string>

#include "ui_font.h"
#include "ui_theme.h"

namespace rimewin {

// 回傳 true = 使用者按了確認。
//
// ⚠ `confirm_text` **必須**寫出它會做什麼。呼叫端傳「確定」的話,
//   W16 的字面檢查會紅 —— 那是刻意的。
// ⚠ 預設焦點永遠在取消,呼叫端沒有選項可以改。
bool ConfirmDialog(HWND parent, Theme* theme, Script script,
                   const std::wstring& title, const std::wstring& body,
                   const std::wstring& confirm_text,
                   const std::wstring& cancel_text);

// 只有一顆「知道了」的訊息窗。**只給失敗路徑用**(§2-D4:
// 成功 → 短暫提示自己消失;失敗 → 停在對話框上不自動關)。
void MessageDialog(HWND parent, Theme* theme, Script script,
                   const std::wstring& title, const std::wstring& body,
                   const std::wstring& dismiss_text);

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_UI_CONFIRM_H_
