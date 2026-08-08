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

  void Close();

  LinkPhase phase() const { return link_.phase(); }

 private:
  bool Connect();
  bool Handshake();
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
};

}  // namespace rimewin

#endif  // RIMEWIN_TSF_IPC_CLIENT_H_
