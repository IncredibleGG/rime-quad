// windows/tsf/ipc_client.h — DLL 側的具名管道用戶端
//
// 這一層的每一個決定都服從同一條規則:**寧可輸入法沒作用,不可吃掉按鍵**。
// 所以每一個 I/O 都有逾時,逾時就關掉連線並退回 fail-open,
// 而不是等下去 —— 等下去的地方是宿主的 UI 執行緒。
#ifndef RIMEWIN_TSF_IPC_CLIENT_H_
#define RIMEWIN_TSF_IPC_CLIENT_H_

#include <windows.h>

#include <string>

#include "../common/link_state.h"
#include "../common/protocol.h"

namespace rimewin {

class IpcClient {
 public:
  IpcClient();
  ~IpcClient();

  IpcClient(const IpcClient&) = delete;
  IpcClient& operator=(const IpcClient&) = delete;

  // 服務執行檔的位置(與 DLL 同目錄)。空字串 = 不嘗試自動啟動。
  void SetServicePath(std::wstring path) { service_path_ = std::move(path); }

  // 使用者是從哪一個語言設定檔啟用這個輸入法的(0 = 不知道)。
  //
  // ⚠ 值變了必須**重新握手**:服務進程是在 HELLO 當下決定預設方案的,
  //   而三份設定檔共用同一個 CLSID —— 使用者在 zh-Hant 與 zh-Hans 之間切換時
  //   TSF 不會重新 Activate 這個文字服務,只會通知 profile 換了。
  //   不重連的話,切過去之後仍然是上一個語言的方案,而使用者剛剛做的動作
  //   看起來完全沒有效果。
  void SetProfile(uint32_t langid, const std::string& profile_guid);

  // 協商出來的線路版本。0 = 還沒握手。
  uint32_t negotiated_proto() const { return negotiated_proto_; }

  // 需要時連線 + 握手 + 建立 session。已經好了就直接回 true。
  // **不會阻塞超過設定的逾時**,而且失敗時會照退避節流,不會每顆按鍵都重試。
  bool EnsureReady();

  // 這條連線現在可不可以吃按鍵。TSF 的 pfEaten 只准在這裡回 true 時置位。
  bool MayEatKey() const { return link_.MayEatKey() && session_ != 0; }

  bool SendKey(int32_t keysym, uint32_t mods, Result* out);
  bool SendSelect(int32_t index, Result* out);
  bool SendCommitComposition(Result* out);
  bool SendClear(Result* out);
  bool SendChangePage(bool backward, Result* out);
  bool SendSelectSchema(const std::string& schema_id, Result* out);

  // 單向,沒有回覆:候選窗位置不值得為它多一趟往返。
  void SendCaretRect(int32_t l, int32_t t, int32_t r, int32_t b);
  void SendFocus(bool focused);

  // 叫服務進程把設定視窗開起來(語言列按鈕與系統匣都走這一條)。
  // 回傳 false = 沒送出去(沒連上,或對面是 v1 的服務)。
  // ⚠ 回傳 false 時呼叫端**必須**有別的辦法把設定叫出來,否則那顆按鈕
  //   就是這個專案抓過四次的「看得到但摸不到」。
  bool SendOpenSettings();

  void Close();

  LinkPhase phase() const { return link_.phase(); }

 private:
  bool Connect();
  // proto = 這一次要宣告的線路版本。降級重試就是拿不同的值再來一次。
  bool ConnectAndHandshake(uint32_t proto);
  bool Handshake(uint32_t proto);
  bool OpenSession();
  bool Exchange(const std::string& payload, uint32_t seq, std::string* reply,
                DWORD timeout_ms);
  bool WriteAllTimed(const std::string& data, DWORD timeout_ms);
  bool ReadFrameTimed(std::string* payload, DWORD timeout_ms);
  void SendOneWay(const std::string& payload);
  bool RequestResult(const std::string& payload, uint32_t seq, Result* out);
  void Fail(LinkFailure kind);
  bool TryLaunchService();

  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  HANDLE event_ = nullptr;  // overlapped I/O 用,整個物件共用一個
  LinkState link_;
  FrameReader reader_;
  uint64_t session_ = 0;
  uint32_t seq_ = 0;
  std::wstring service_path_;
  int64_t last_launch_ms_ = -1;
  uint32_t langid_ = 0;
  std::string profile_guid_;
  uint32_t negotiated_proto_ = 0;
};

}  // namespace rimewin

#endif  // RIMEWIN_TSF_IPC_CLIENT_H_
