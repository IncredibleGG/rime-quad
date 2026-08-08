// windows/tsf/text_service.h — TSF 的文字服務(瘦 DLL 的全部)
//
// ⚠ 這個類別的實例活在**每一個**接受文字輸入的進程裡 —— 瀏覽器、Office、
//   以及提權的系統進程。所以這裡的紀律不是風格問題:
//
//     · 不做重量級初始化,不載入 librime、不讀 YAML、不開字型。
//     · 不在宿主的 UI 執行緒上長時間阻塞(所有 IPC 都有逾時)。
//     · 不讓任何例外離開 COM 介面方法。C++ 例外穿過 COM 邊界是未定義行為,
//       而在這裡「未定義」的實際長相就是宿主崩潰。
//     · 拿不到服務進程時**一律放行按鍵**,不吃。
//
// 候選窗不在這裡。它是服務進程開的獨立 top-level window。這個 DLL 只把
// 插入點的螢幕座標送過去。
#ifndef RIMEWIN_TSF_TEXT_SERVICE_H_
#define RIMEWIN_TSF_TEXT_SERVICE_H_

#include <msctf.h>
#include <windows.h>

#include <memory>
#include <string>

#include "ipc_client.h"
#include "win32_oracle.h"

namespace rimewin {

class TextService : public ITfTextInputProcessorEx,
                    public ITfThreadMgrEventSink,
                    public ITfKeyEventSink,
                    public ITfCompositionSink {
 public:
  TextService();
  virtual ~TextService();

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  // ITfTextInputProcessor / Ex
  STDMETHODIMP Activate(ITfThreadMgr* mgr, TfClientId id) override;
  STDMETHODIMP Deactivate() override;
  STDMETHODIMP ActivateEx(ITfThreadMgr* mgr, TfClientId id, DWORD flags) override;

  // ITfThreadMgrEventSink
  STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr* dim) override;
  STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr* dim) override;
  STDMETHODIMP OnSetFocus(ITfDocumentMgr* focus, ITfDocumentMgr* prev) override;
  STDMETHODIMP OnPushContext(ITfContext* ctx) override;
  STDMETHODIMP OnPopContext(ITfContext* ctx) override;

  // ITfKeyEventSink
  STDMETHODIMP OnSetFocus(BOOL foreground) override;
  STDMETHODIMP OnTestKeyDown(ITfContext* ctx, WPARAM w, LPARAM l,
                             BOOL* eaten) override;
  STDMETHODIMP OnKeyDown(ITfContext* ctx, WPARAM w, LPARAM l,
                         BOOL* eaten) override;
  STDMETHODIMP OnTestKeyUp(ITfContext* ctx, WPARAM w, LPARAM l,
                           BOOL* eaten) override;
  STDMETHODIMP OnKeyUp(ITfContext* ctx, WPARAM w, LPARAM l, BOOL* eaten) override;
  STDMETHODIMP OnPreservedKey(ITfContext* ctx, REFGUID guid, BOOL* eaten) override;

  // ITfCompositionSink —— 宿主(而不是我們)結束組字時會走這裡。
  STDMETHODIMP OnCompositionTerminated(TfEditCookie ec,
                                       ITfComposition* composition) override;

 private:
  // 一顆按鍵的完整處理。回傳「宿主要不要吃掉它」。
  bool HandleKey(ITfContext* ctx, WPARAM w, LPARAM l, bool key_up);
  // 依快照對文件做事。必須在 edit session 內呼叫。
  HRESULT ApplyPlan(TfEditCookie ec, ITfContext* ctx, const Snapshot& snap);
  HRESULT StartCompositionIfNeeded(TfEditCookie ec, ITfContext* ctx);
  HRESULT SetCompositionText(TfEditCookie ec, ITfContext* ctx,
                             const std::wstring& text);
  HRESULT InsertText(TfEditCookie ec, ITfContext* ctx, const std::wstring& text);
  void EndComposition(TfEditCookie ec);
  void ReportCaretRect(TfEditCookie ec, ITfContext* ctx);
  const KeyboardOracle& Oracle();

  LONG ref_ = 1;
  ITfThreadMgr* thread_mgr_ = nullptr;
  TfClientId client_id_ = TF_CLIENTID_NULL;
  DWORD thread_mgr_cookie_ = TF_INVALID_COOKIE;
  ITfComposition* composition_ = nullptr;
  ITfContext* composition_ctx_ = nullptr;

  IpcClient ipc_;
  // 每個 HKL 一個 oracle。使用者切換鍵盤佈局時重建。
  std::unique_ptr<Win32KeyboardOracle> oracle_;
  HKL oracle_hkl_ = nullptr;

  // 最近一次待送出的候選窗位置。在 edit session 內算好,出來之後才送 ——
  // edit session 裡做 IPC 等於在持有文件鎖的時候等別的進程。
  bool pending_rect_ = false;
  RECT pending_rect_value_{};
};

}  // namespace rimewin

#endif  // RIMEWIN_TSF_TEXT_SERVICE_H_
