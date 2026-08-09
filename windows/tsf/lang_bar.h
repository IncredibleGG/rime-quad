// windows/tsf/lang_bar.h — 語言列上的那一顆按鈕(對外只有四個函式)
//
// ⚠ **這是瘦 DLL 裡刻意增加的唯一一塊 UI 相關程式碼,理由在這裡寫清楚。**
//
// 設定介面本體在服務進程那一側(見 windows/service/settings_window.cc),
// 這個 DLL 一行 librime、一行 YAML、一個字型、一個視窗都沒有。
// 但**入口沒得選**:`ITfLangBarItem` 是 TSF 向文字服務要的介面,
// 而文字服務就是這個 DLL。語言列的按鈕只能在這裡。
//
// 所以這個模組的紀律比別處更嚴:
//
//   · 它不畫任何東西。按鈕的外觀由系統畫,我們只給一個字串。
//   · OnClick 只做一件事:送一則**單向**的訊息。不等回覆、不阻塞、
//     不配置大東西。按下去之後這個 DLL 就沒事了。
//   · 任何例外都不准離開 COM 方法。這裡住在瀏覽器與提權進程裡。
//
// 而「沒有入口的設定介面等於沒做」是這一輪的原話,所以這一塊不能省。
//
// ── 為什麼這個標頭只露出四個函式,不露出類別 ────────────────────
//
// 類別本身要 `ctfutb.h` 的 `ITfLangBarItemButton`,而 **mingw-w64 的
// ctfutb.h 沒有那個介面**(真正的 Windows SDK 有)。
// windows/syntax_check_mingw.sh 是這個專案在推 CI 之前唯一的語法關卡
// (一輪 CI 十幾分鐘),把型別露在標頭上等於讓 text_service.cc ——
// 全案最大、最需要那道關卡的檔案 —— 也一起檢查不了。
//
// 所以型別關在 .cc 裡,對外是不透明指標。**只有 lang_bar.cc 一個檔案
// 需要被跳過**,而那支腳本的跳過是會自己過期的(見它的註解)。
//
// ── 按下去之後真的會發生什麼(三條路,不是一條)────────────────
//
//   1. 管道已經連上且協商到 v2 → 送 Op::kOpenSettings。
//   2. 否則 → 開具名事件 `Local\LuminaKeySettings.<SID>` 並 SetEvent。
//   3. 服務根本沒在跑 → CreateProcess(rime_service.exe --settings)。
//
// 為什麼要三條:UWP／市集 App 的宿主跑在 AppContainer 裡,開不了
// `Local\` 底下別人建立的具名物件 —— 那時只有路 1 走得通。
// 而使用者從「還沒打過任何一個字」的狀態按這顆按鈕時管道還沒連,
// 那時只有路 2 或 3 走得通。少了任何一條,都會有一整類情境下這顆按鈕
// **按下去毫無反應** —— 這個專案抓過四次那種鍵,不要再多一顆。
// (實作在 text_service.cc 的 OpenSettings。)
//
#ifndef RIMEWIN_TSF_LANG_BAR_H_
#define RIMEWIN_TSF_LANG_BAR_H_

#include <msctf.h>
#include <windows.h>

#include <functional>

namespace rimewin {

// 不透明。定義在 lang_bar.cc 裡,見上面的說明。
class LangBarButton;

// 建立。配置失敗回 nullptr —— 呼叫端必須容許沒有這顆按鈕
// (系統匣那條路是獨立的入口)。初始參考計數為 1。
LangBarButton* CreateLangBarButton(std::function<void()> on_click);

// 掛上 / 拿掉。AddLangBarButton 回傳 false 代表這個宿主沒有語言列項目
// 管理員 —— 那不是錯誤(某些宿主就是沒有),但也代表這顆按鈕在那裡
// 不存在,所以系統匣那條路必須獨立成立。
bool AddLangBarButton(ITfThreadMgr* mgr, LangBarButton* item);
void RemoveLangBarButton(ITfThreadMgr* mgr, LangBarButton* item);

// 放掉我們自己那一份參考。語言列可能還握著它,所以不保證立刻銷毀。
void ReleaseLangBarButton(LangBarButton* item);

}  // namespace rimewin

#endif  // RIMEWIN_TSF_LANG_BAR_H_
