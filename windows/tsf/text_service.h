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
#include "../common/shift_tap.h"
#include "ipc_client.h"
#include "win32_oracle.h"

namespace rimewin {

class LangBarButton;
// 那一橫的「在場」連線。實作整個住在 text_service.cc(它要 g_rime_dll_refs,
// 而那個符號只存在於 rime_tsf.dll —— ipc_client.cc 還被 rime_ime_setup 與
// rime_probe 連結,把它放進去會讓那兩支連不起來)。
class PresenceLink;

class TextService : public ITfTextInputProcessorEx,
                    public ITfThreadMgrEventSink,
                    public ITfKeyEventSink,
                    public ITfCompositionSink,
                    public ITfInputProcessorProfileActivationSink,
                    // ⚠ 這一個**只為了一件事**存在:輕點 Shift 的狀態機
                    //   看不到滑鼠。宿主說「這份文件的選取被動過了」是
                    //   我們唯一收得到的滑鼠證據。見 OnEndEdit 與
                    //   common/shift_tap.h 的「看不見的那一種輸入」。
                    public ITfTextEditSink {
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

  // ITfTextEditSink —— 宿主的文件被改動之後(含**只有選取變了**)。
  //
  // ⚠ 這裡唯一在意的是「選取變了,而它不是我們按鍵引起的」——
  //   那就是使用者用滑鼠點了一下。輕點 Shift 的狀態機看不到滑鼠,
  //   這一格是它唯一的眼睛。理由與涵蓋不到的那一格見 .cc。
  STDMETHODIMP OnEndEdit(ITfContext* ctx, TfEditCookie ec,
                         ITfEditRecord* record) override;

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
  // 事件已經組好時走這一個。⚠ BuildKeyEvent 裡有一次 GetKeyboardState,
  //   而 OnTestKeyDown 要把同一顆事件同時餵給輕點 Shift 的狀態機 ——
  //   組兩次是在宿主的 UI 執行緒上白付一次系統呼叫。
  KeyPlan PlanKey(const KeyEvent& e);

  // 目前有沒有進行中的組字。
  //
  // 兩個來源取聯集是刻意的:composition_ 是**我們在宿主文件上**開的那一段,
  // engine_composing_ 是**引擎**說它手上還有輸入。正常情況兩者一致,
  // 而不一致的那一刻(StartComposition 失敗、或宿主自己把組字收掉了)
  // 用聯集才不會把退格鍵誤判成「宿主的鍵」而讓引擎裡的輸入卡住。
  bool Composing() const { return composition_ != nullptr || engine_composing_; }

  // 一顆按鍵的完整處理。回傳「宿主要不要吃掉它」。
  bool HandleKey(ITfContext* ctx, WPARAM w, LPARAM l, bool key_up);

  // 送出一次「中英切換」並把回來的快照套進文件。回傳:服務有沒有處理它。
  //
  // ⚠ 兩個入口共用:Ctrl+空白鍵(OnPreservedKey)與輕點 Shift(OnTestKeyUp)。
  //   切中英**會**產生上屏文字(切到英數時 librime 把組字上屏並清掉),
  //   而那份快照是那段文字唯一的一次現身 —— 兩邊各寫一份收尾邏輯,
  //   漂移的樣子就是「使用者打到一半的字不見了」。細節見 .cc 裡那一整段 ⚠。
  bool SendAsciiToggle(ITfContext* ctx, int32_t keysym, uint32_t mods,
                       const char* label);
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

  // ── 文件編輯 sink 的掛接(輕點 Shift 唯一看得到滑鼠的地方)────────
  //
  // ⚠ 一次只掛**一個** context:目前有焦點的那份文件的最上層 context。
  //   使用者一次只在一個輸入框裡點,而每個 context 各掛一份就要一張表、
  //   一組生命週期,還要在瀏覽器與提權進程裡維護它。
  //   掛不上時**什麼都不做**(不是錯誤):有些宿主不給,而那一格的
  //   後果只是「在那個宿主裡 Shift+點擊仍然會誤切一次」,見 OnEndEdit。
  void WatchContext(ITfContext* ctx);
  // 從一份 document manager 取最上層 context 來掛。dim 為 null = 取消掛接。
  void WatchContextOf(ITfDocumentMgr* dim);
  // 問系統「現在有焦點的是哪一份文件」,然後掛它。
  void WatchFocusedContext();

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
  // 「這個文字服務現在是啟用中的」那一條會維持的訊號(工單 #82 的另一半)。
  //
  // ⚠ 它**不是** ipc_ 的第二個用途:它有自己的執行緒、自己的管道 handle,
  //   一個位元都不碰 ipc_。ipc_ 仍然只由宿主的 UI 執行緒碰,沒有鎖 ——
  //   那是 ipc_client.h 檔頭那條規矩,這一輪一個字都沒有動它。
  //
  // nullptr = 沒起來(建不出事件或執行緒)。那時行為退回這一版之前:
  //   那一橫要等使用者按下第一顆鍵才出現。不是致命的,所以不吵。
  PresenceLink* presence_ = nullptr;
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
  // 簡繁快捷鍵(Ctrl+Shift+F)註冊成功了沒有。⚠ 與上面分開記:
  //   兩顆鍵可能一顆被別的輸入法佔走、另一顆沒有,而 Deactivate 只能
  //   還回真的註冊成功的那些(還一顆沒註冊的會是一次無聲的失敗)。
  bool preserved_variant_ok_ = false;

  // 輕點 Shift 切中英的狀態機(工單 #89)。判斷本體是純函式,
  // 住在 common/shift_tap.h —— 在 Ubuntu 上有一張逐事件的真值表。
  //
  // ⚠ 每一個 TextService 實例一份,也就是**每個宿主進程各自一份**。
  //   那是對的:按鍵是逐進程送進來的,而使用者一次只在一個程式裡打字。
  //
  // ⚠ 只由 OnTestKeyDown / OnTestKeyUp 餵(它們是修飾鍵唯一會走到的兩趟),
  //   並由 Deactivate 與兩個 OnSetFocus 歸零。
  //
  // ⚠ 還有第三個入口:OnOtherInput()。它是這個狀態機**看不到滑鼠**的補丁,
  //   由 OnEndEdit(選取變了)與 OnCompositionTerminated 呼叫。
  //   「在同一個輸入框裡點一下」不會換 document manager,所以兩個
  //   OnSetFocus 一個都不會來 —— 少了那兩個呼叫點,延伸選取的標準手勢
  //   「按住 Shift → 點一下 → 放開 Shift」會切一次中英。
  //   守門在 windows/audit_single_source.sh 規則 4(純函式驗不到有沒有人呼叫它)。
  ShiftTapState shift_tap_;

  // 目前掛著 ITfTextEditSink 的 context 與它的 cookie。
  // ⚠ 這裡持有一份參考(AddRef)。與 thread_mgr_ 一樣,Deactivate 必須放掉,
  //   否則宿主的文件會被我們吊著不放。
  ITfContext* edit_sink_ctx_ = nullptr;
  DWORD edit_sink_cookie_ = TF_INVALID_COOKIE;

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
