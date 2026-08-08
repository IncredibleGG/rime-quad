// windows/tsf/lang_bar.h — 語言列上的那一顆按鈕
//
// ⚠ **這是瘦 DLL 裡刻意增加的唯一一塊 UI 相關程式碼,理由在這裡寫清楚。**
//
// 設定介面本體在服務進程那一側(見 windows/service/settings_window.cc),
// 這個 DLL 一行 librime、一行 YAML、一個字型、一個視窗都沒有。
// 但**入口沒得選**:`ITfLangBarItem` 是 TSF 向文字服務要的介面,
// 而文字服務就是這個 DLL。語言列的按鈕只能在這裡。
//
// 所以這個檔案的紀律比別處更嚴:
//
//   · 它不畫任何東西。按鈕的外觀由系統畫,我們只給一個字串。
//   · OnClick 只做一件事:送一則**單向**的 IPC 訊息(或設一個具名事件)。
//     不等回覆、不阻塞、不配置大東西。按下去之後這個 DLL 就沒事了。
//   · 任何例外都不准離開 COM 方法。這裡住在瀏覽器與提權進程裡。
//
// 而「沒有入口的設定介面等於沒做」是這一輪的原話,所以這一塊不能省。
//
// ── 按下去之後真的會發生什麼(兩條路,不是一條)────────────────
//
//   1. 管道已經連上且協商到 v2 → 送 Op::kOpenSettings。
//   2. 否則 → 開具名事件 `Local\RimeQuadSettings.<SID>` 並 SetEvent。
//      服務進程有一條執行緒在等它。
//
// 為什麼要兩條:UWP／市集 App 的宿主跑在 AppContainer 裡,開不了
// `Local\` 底下別人建立的具名物件 —— 那時只有路 1 走得通。
// 而使用者從「還沒打過任何一個字」的狀態按這顆按鈕時管道還沒連,
// 那時只有路 2 走得通。少了任何一條,都會有一整類情境下這顆按鈕
// **按下去毫無反應** —— 這個專案抓過四次那種鍵,不要再多一顆。
//
#ifndef RIMEWIN_TSF_LANG_BAR_H_
#define RIMEWIN_TSF_LANG_BAR_H_

#include <ctfutb.h>
#include <msctf.h>
#include <windows.h>

#include <functional>

namespace rimewin {

class LangBarButton final : public ITfLangBarItemButton, public ITfSource {
 public:
  // on_click 由 TextService 提供,做的事見檔頭。
  explicit LangBarButton(std::function<void()> on_click);

  // 掛上 / 拿掉。回傳 false 代表這個宿主沒有語言列項目管理員 ——
  // 那不是錯誤(某些宿主就是沒有),但也代表這顆按鈕在那裡不存在,
  // 所以系統匣那條路必須獨立成立。
  static bool AddTo(ITfThreadMgr* mgr, LangBarButton* item);
  static void RemoveFrom(ITfThreadMgr* mgr, LangBarButton* item);

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  // ITfLangBarItem
  STDMETHODIMP GetInfo(TF_LANGBARITEMINFO* info) override;
  STDMETHODIMP GetStatus(DWORD* flags) override;
  STDMETHODIMP Show(BOOL show) override;
  STDMETHODIMP GetTooltipString(BSTR* tooltip) override;

  // ITfLangBarItemButton
  STDMETHODIMP OnClick(TfLBIClick click, POINT pt, const RECT* area) override;
  STDMETHODIMP InitMenu(ITfMenu* menu) override;
  STDMETHODIMP OnMenuSelect(UINT id) override;
  STDMETHODIMP GetIcon(HICON* icon) override;
  STDMETHODIMP GetText(BSTR* text) override;

  // ITfSource —— 語言列要能訂閱我們的變更。我們沒有變更可通知,
  // 但**不可以**回 E_NOTIMPL:部分宿主會因此整個不顯示這一項。
  STDMETHODIMP AdviseSink(REFIID riid, IUnknown* punk, DWORD* cookie) override;
  STDMETHODIMP UnadviseSink(DWORD cookie) override;

 private:
  ~LangBarButton();

  LONG ref_ = 1;
  std::function<void()> on_click_;
  ITfLangBarItemSink* sink_ = nullptr;
  DWORD sink_cookie_ = 0;
};

}  // namespace rimewin

#endif  // RIMEWIN_TSF_LANG_BAR_H_
