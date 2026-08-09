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

// EnsureReady() 失敗在**哪一步**。
//
// ⚠ 這三步的修法完全不同,所以絕對不可以併成一句話講:
//
//   kPipe       管道開不起來。服務沒在監聽,或 SID / 權限對不上。
//               → 要查的是服務進程的監聽迴圈(service.log 的 [pipe] 行),
//                 不是協議。
//   kHandshake  連上了,但 HELLO 談不攏(線路版本或 rime_shell ABI)。
//               → 兩邊不是同一次建置的產物。要查的是「誰是舊的」。
//   kSession    握手過了,SESSION_NEW 失敗或逾時。
//               → 協議沒問題,是服務端太慢(多半是第一次載入方案詞典)。
//                 要查的是服務端那一段做了什麼、為什麼超過預算。
//
// 這個列舉存在的直接理由:上一版的 rime_probe 對這三種情形一律印
// 「連不上服務或握手失敗」,於是 CI 紅了也不知道該往哪裡查。
enum class ReadyStage {
  kNone,       // 還沒試過,或成功了
  kPipe,
  kHandshake,
  kSession,
};

inline const char* ReadyStageName(ReadyStage s) {
  switch (s) {
    case ReadyStage::kNone:      return "(沒有失敗)";
    case ReadyStage::kPipe:      return "開管道";
    case ReadyStage::kHandshake: return "握手";
    case ReadyStage::kSession:   return "建立 session";
  }
  return "(未知的階段)";
}

// 上一次嘗試的完整結果。診斷訊息要能說出「對方回了什麼」,
// 不能只說「不合」—— 版本不合時,對方報的版本就是唯一有用的線索。
struct ReadyDiagnosis {
  ReadyStage stage = ReadyStage::kNone;
  LinkFailure failure = LinkFailure::kConnectFailed;
  // 開管道失敗時的 GetLastError()。ERROR_FILE_NOT_FOUND(2)= 沒有這條管道;
  // ERROR_ACCESS_DENIED(5)= 管道在,但這個身分開不了(提權 / 非提權)。
  // 這兩者長得一樣但完全不同,所以錯誤碼一定要留下來。
  unsigned long os_error = 0;
  uint32_t tried_proto = 0;   // 我方最後宣告的線路版本
  uint32_t my_shell_abi = 0;  // 我方編譯時看到的 RIME_SHELL_ABI_VERSION
  bool peer_replied = false;  // 服務端有沒有回一則解得開的 HELLO_OK
  HelloOk peer;               // 它回了什麼
  // 真正開過管道的次數。**不含**被連線狀態機的退避擋掉的那些 ——
  // 「重試 100 次」不等於「嘗試了 100 次」,見 common/link_state.h
  // 與 tests/test_link_state.cc 的 link_backoff_eats_almost_all_of_a_naive_retry_loop。
  uint32_t attempts = 0;
};

// ── 與連線狀態無關的兩支 ──────────────────────────────────────────
//
// 放在類別外面是刻意的:ActivateEx 那條路要在**背景執行緒**上用它們,
// 而 IpcClient 的實例活在宿主的 UI 執行緒上、沒有任何鎖。
// 從兩條執行緒碰同一個 IpcClient 是一個等著發生的損壞。

// 服務進程在不在。
//
// ⚠ 判斷依據是**單一實例的互斥鎖**,不是管道。
//   服務啟動之後要先跑完 rs_init 才開管道,而首次部署那段時間是**好幾分鐘**。
//   拿管道當「在不在」的依據,會在那幾分鐘裡一直說「不在」,
//   於是每次都再啟動一支 —— 新的那支被互斥鎖擋掉、以 0 結束、什麼都不說。
bool ServiceIsRunning();

// 啟動服務進程(卸離、無視窗)。
// 回傳 false = 沒有啟動:路徑是空的、這個宿主不准啟動、或 CreateProcess 失敗。
//
// ⚠ 「這個宿主准不准啟動」的判準在 windows/common/elevation_policy.h,
//   那裡也寫了為什麼判準**不是**「有沒有提權」:
//
//     要防的是「產生一支與使用者日常身分不同的服務」—— 那會把詞庫檔案的
//     擁有者換成系統管理員,一般權限的那份服務再也寫不進去(症狀是
//     「用過一次系統管理員的程式之後,輸入法就再也記不住東西」)。
//
//     但「提權」不等於「不同」。內建 Administrator 帳號與關掉 UAC 的機器上,
//     **整個工作階段的每一個進程都是提權的** —— 那裡沒有另一個身分會被
//     弄壞,而舊的判準會讓那些使用者永遠啟動不了服務,也就是永遠打不出字。
//
// ⚠ 拒絕時**一定會寫一行除錯記錄**,而且語言列上那顆按鈕會變成「未啟動」。
//   一個刻意的拒絕不該長得跟壞掉一樣。
bool LaunchService(const std::wstring& service_path);

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

  // 上一次 EnsureReady() 的診斷。DLL 不需要它(它只需要「能不能吃按鍵」),
  // 存在是為了讓 rime_probe 與日後的診斷工具講得出人話。
  const ReadyDiagnosis& diagnosis() const { return diag_; }

  // 把連線狀態機歸零(退避、失敗計數、階段),連線本身也收掉。
  //
  // ⚠ **DLL 不可以呼叫這個。** 退避是它保護宿主 UI 執行緒的手段:
  //   服務真的死掉時,沒有退避就等於每一顆按鍵都在 UI 執行緒上開一次管道。
  //
  //   給誰用:驗證用的 rime_probe。它的等待迴圈要「真的試 N 次」,
  //   而退避會把那 N 次吃掉大半(握手不合的退避是 30 秒,於是十秒的
  //   等待迴圈裡只有第一次是真的)。那對 DLL 是正確行為,對一個
  //   宣稱「重試 N 次」的測試工具則是謊報。
  void ResetLink();

 private:
  bool Connect();
  // proto = 這一次要宣告的線路版本。降級重試就是拿不同的值再來一次。
  bool ConnectAndHandshake(uint32_t proto);
  bool Handshake(uint32_t proto);
  bool OpenSession();
  bool Exchange(const std::string& payload, uint32_t seq, std::string* reply,
                DWORD timeout_ms);
  bool WriteAllTimed(const std::string& data, DWORD timeout_ms);
  // eof 收「對面乾淨地關掉了連線」(讀到 0 位元組)。與逾時分開,
  // 理由見 common/link_state.h 的 LinkFailure::kPeerClosed。
  bool ReadFrameTimed(std::string* payload, DWORD timeout_ms, bool* eof);
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
  ReadyDiagnosis diag_;
  // 還可以往除錯記錄寫幾行。
  //
  // ⚠ 有預算是必要的,不是保守。連不上時每一顆按鍵都會來一次 EnsureReady
  //   (被退避擋掉的除外),而寫記錄是磁碟 I/O、發生在宿主的 UI 執行緒上。
  //   而且真正有用的是**前幾次**:第七次的失敗訊息與第一次一模一樣,
  //   只會把記錄檔前面那些有價值的行擠掉。
  int trace_budget_ = 6;
};

}  // namespace rimewin

#endif  // RIMEWIN_TSF_IPC_CLIENT_H_
