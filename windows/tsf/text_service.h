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

#include "../common/key_eat_policy.h"
#include "ipc_client.h"
#include "win32_oracle.h"

namespace rimewin {

class LangBarButton;

class TextService : public ITfTextInputProcessorEx,
                    public ITfThreadMgrEventSink,
                    public ITfKeyEventSink,
                    public ITfCompositionSink,
                    public ITfInputProcessorProfileActivationSink {
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

  // ITfInputProcessorProfileActivationSink
  //
  // ⚠ 這個 sink 不是可有可無的。三份語言設定檔(zh-Hant-TW / zh-Hans-CN /
  //   zh-Hant-HK)**共用同一個 CLSID**,所以使用者在它們之間切換時,
  //   TSF 不會 Deactivate 再 Activate 這個文字服務 —— 它只發這一則通知。
  //   少了它,使用者從繁體切到簡體之後打出來仍然是繁體字,
  //   而那正是這一輪要修的缺陷本身。
  STDMETHODIMP OnActivated(DWORD profile_type, LANGID langid, REFCLSID clsid,
                           REFGUID catid, REFGUID guid_profile, HKL hkl,
                           DWORD flags) override;

 private:
  // 一顆按鍵算出來的東西:keysym、族別、以及「該不該宣告吃掉」。
  //
  // ⚠ OnTestKeyDown 與 OnKeyDown **必須用同一份**。兩邊各算各的,
  //   就會出現「測試說吃、真的處理時說不吃」—— 而那正是使用者回報的
  //   「可以打字,不能刪除」:宿主在測試那一趟就放棄了自己的預設處理,
  //   事後改口它收不到。見 common/key_eat_policy.h 的檔頭。
  struct KeyPlan {
    MappedKey mapped;
    KeyKind kind = KeyKind::kUnmappable;
    bool eat = false;
  };
  KeyPlan PlanKey(WPARAM w, LPARAM l, bool key_up);

  // 目前有沒有進行中的組字。
  //
  // 兩個來源取聯集是刻意的:composition_ 是**我們在宿主文件上**開的那一段,
  // engine_composing_ 是**引擎**說它手上還有輸入。正常情況兩者一致,
  // 而不一致的那一刻(StartComposition 失敗、或宿主自己把組字收掉了)
  // 用聯集才不會把退格鍵誤判成「宿主的鍵」而讓引擎裡的輸入卡住。
  bool Composing() const { return composition_ != nullptr || engine_composing_; }

  // 一顆按鍵的完整處理。回傳「宿主要不要吃掉它」。
  bool HandleKey(ITfContext* ctx, WPARAM w, LPARAM l, bool key_up);
  // 引擎說它不處理這顆字元鍵時,由我們把那個字寫進文件。
  // 見 common/key_eat_policy.h:宣告吃掉的鍵一定要有人負責。
  bool SelfInsertChar(ITfContext* ctx, char32_t ch);
  // 依快照對文件做事。必須在 edit session 內呼叫。
  HRESULT ApplyPlan(TfEditCookie ec, ITfContext* ctx, const Snapshot& snap);
  HRESULT StartCompositionIfNeeded(TfEditCookie ec, ITfContext* ctx);
  HRESULT SetCompositionText(TfEditCookie ec, ITfContext* ctx,
                             const std::wstring& text);
  HRESULT InsertText(TfEditCookie ec, ITfContext* ctx, const std::wstring& text);
  void EndComposition(TfEditCookie ec);
  void ReportCaretRect(TfEditCookie ec, ITfContext* ctx);
  // 回具體型別而不是介面:診斷要問得到 used_fallback() / blind(),
  // 而那兩件事正是這一輪查出來的關鍵(見 win32_oracle.h 檔頭)。
  // MapKey 收的仍然是介面,所以這不影響「純邏輯層不碰 windows.h」那條界線。
  const Win32KeyboardOracle& Oracle();
  // 問系統「使用者現在用的是我們的哪一份語言設定檔」,結果交給 ipc_。
  void RefreshProfile();
  // 語言列按鈕按下去時做的事。單向,不等回覆。
  void OpenSettings();

  LONG ref_ = 1;
  ITfThreadMgr* thread_mgr_ = nullptr;
  TfClientId client_id_ = TF_CLIENTID_NULL;
  DWORD thread_mgr_cookie_ = TF_INVALID_COOKIE;
  DWORD profile_sink_cookie_ = TF_INVALID_COOKIE;
  LangBarButton* lang_bar_ = nullptr;
  // key event sink 掛上了沒有。
  //
  // ⚠ 它是 false 的話,這個宿主裡**一顆按鍵都收不到**,而且沒有任何
  //   其他跡象:ActivateEx 照樣回 S_OK,語言列按鈕照樣在。
  //   以前這個回傳值完全沒有人看。
  bool key_sink_ok_ = false;
  ITfComposition* composition_ = nullptr;
  ITfContext* composition_ctx_ = nullptr;
  // 引擎最後一次回報的「我還在組字」。見 Composing()。
  bool engine_composing_ = false;

  IpcClient ipc_;
  // 服務執行檔的完整路徑(與 DLL 同目錄)。空字串 = 算不出來。
  std::wstring service_path_;
  // 每個 HKL 一個 oracle。使用者切換鍵盤佈局時重建。
  std::unique_ptr<Win32KeyboardOracle> oracle_;
  HKL oracle_hkl_ = nullptr;

  // 還可以把幾顆按鍵寫進除錯記錄。
  //
  // Ctrl+空白鍵那顆保留鍵註冊成功了沒有。⚠ 只有註冊成功才可以
  // UnpreserveKey —— 對一顆沒註冊過的鍵反註冊會拿到錯誤碼,而那則錯誤
  // 會蓋掉真正的問題。理由與整個做法見 common/hotkey_policy.h。
  bool preserved_key_ok_ = false;

  // ⚠ **不可以每一顆都寫。** OnTestKeyDown 跑在宿主的 UI 執行緒上,
  //   每一顆按鍵一次磁碟寫入是不能接受的。而診斷需要的資訊在前幾顆就齊了:
  //   「vk 進來了、映出的 keysym 是什麼、有沒有被吃掉」——
  //   第二十顆按鍵的那一行與第一顆一模一樣。
  int key_trace_budget_ = 5;

  // 最近一次待送出的候選窗位置。在 edit session 內算好,出來之後才送 ——
  // edit session 裡做 IPC 等於在持有文件鎖的時候等別的進程。
  bool pending_rect_ = false;
  RECT pending_rect_value_{};
};

}  // namespace rimewin

#endif  // RIMEWIN_TSF_TEXT_SERVICE_H_
