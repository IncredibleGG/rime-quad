#include "ipc_client.h"

#include <vector>

#include "../winshared/winutil.h"

// 版本協商用:DLL 側編譯時看到的門面 ABI。DLL 本身**不連結** rime_shell,
// 只取這一個常數,所以只 include 標頭不會把 librime 拖進宿主進程。
#include "rime_shell.h"

namespace rimewin {
namespace {

// 按鍵往返的逾時。
//
// 效能紅線是「從按鍵到候選更新一到兩幀」(docs/handoff-windows.md §5),
// 也就是 16–33ms。這裡的 50ms 不是目標值,是**放棄的門檻**:超過它就
// 認定服務不對勁,關掉連線、放行按鍵。在宿主的 UI 執行緒上等更久,
// 使用者感覺到的是整個應用程式卡住。
constexpr DWORD kKeyTimeoutMs = 50;

// 連線與握手可以久一點:它不在每一顆按鍵的路徑上。
constexpr DWORD kConnectTimeoutMs = 300;

// 自動啟動服務之後,等管道出現的上限。
constexpr DWORD kWaitPipeMs = 400;

// 兩次嘗試啟動服務之間的最短間隔。沒有這個節流的話,服務只要一直啟動失敗,
// 每一顆按鍵都會 CreateProcess 一次。
constexpr int64_t kLaunchCooldownMs = 10000;

int64_t NowMs() { return static_cast<int64_t>(::GetTickCount64()); }

}  // namespace

IpcClient::IpcClient() {
  event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

IpcClient::~IpcClient() {
  Close();
  if (event_) ::CloseHandle(event_);
}

void IpcClient::Close() {
  if (pipe_ != INVALID_HANDLE_VALUE) {
    ::CancelIoEx(pipe_, nullptr);
    ::CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }
  session_ = 0;
  reader_ = FrameReader();
}

void IpcClient::Fail(LinkFailure kind) {
  // 任何失敗都把連線整條丟掉。
  //
  // 特別是逾時:一次逾時之後,那則回覆仍可能晚一點才到,而管道上的下一個
  // 讀取就會拿到它 —— 從此每一次請求都收到上一次的答案。使用者看到的是
  // 「輸入法慢一拍」,而那種錯位幾乎查不出來。關掉重來是唯一乾淨的做法。
  Close();
  link_.OnFailure(kind, NowMs());
}

bool IpcClient::EnsureReady() {
  if (link_.MayEatKey() && pipe_ != INVALID_HANDLE_VALUE && session_ != 0)
    return true;
  const int64_t now = NowMs();
  if (!link_.ShouldAttemptConnect(now)) return false;
  link_.OnAttempt(now);

  if (!Connect()) return false;
  if (!Handshake()) return false;
  if (!OpenSession()) return false;
  link_.OnConnected();
  return true;
}

bool IpcClient::Connect() {
  const std::wstring name = RimePipeName();
  for (int attempt = 0; attempt < 2; ++attempt) {
    pipe_ = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe_ != INVALID_HANDLE_VALUE) return true;

    const DWORD err = ::GetLastError();
    if (err == ERROR_PIPE_BUSY) {
      // 服務在,只是所有實例都忙著。等一下再試。
      if (!::WaitNamedPipeW(name.c_str(), kWaitPipeMs)) break;
      continue;
    }
    if (attempt == 0 && err == ERROR_FILE_NOT_FOUND) {
      if (!TryLaunchService()) break;
      ::WaitNamedPipeW(name.c_str(), kWaitPipeMs);
      continue;
    }
    break;
  }
  link_.OnFailure(LinkFailure::kConnectFailed, NowMs());
  return false;
}

bool IpcClient::TryLaunchService() {
  if (service_path_.empty()) return false;

  // ⚠ 提權的宿主進程不可以啟動服務。
  //
  // TSF 的 DLL 會被載入到提權的進程裡(以系統管理員身分執行的編輯器、
  // UAC 的對話框…)。從那裡 CreateProcess 起來的服務會繼承提權的權杖,
  // 而那支服務接下來會用**系統管理員**的身分去讀寫使用者的設定與詞庫 ——
  // 檔案的擁有者從此變成不對的人,一般權限的那份服務再也寫不進去。
  // 症狀是「用過一次系統管理員的程式之後,輸入法就再也記不住東西」。
  if (IsProcessElevated()) return false;

  const int64_t now = NowMs();
  if (last_launch_ms_ >= 0 && now - last_launch_ms_ < kLaunchCooldownMs)
    return false;
  last_launch_ms_ = now;

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  std::wstring cmd = L"\"" + service_path_ + L"\"";
  std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
  mutable_cmd.push_back(L'\0');

  if (!::CreateProcessW(service_path_.c_str(), mutable_cmd.data(), nullptr,
                        nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS,
                        nullptr, nullptr, &si, &pi))
    return false;
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  return true;
}

bool IpcClient::WriteAllTimed(const std::string& data, DWORD timeout_ms) {
  size_t sent = 0;
  while (sent < data.size()) {
    OVERLAPPED ov{};
    ov.hEvent = event_;
    ::ResetEvent(event_);
    DWORD written = 0;
    const BOOL ok = ::WriteFile(pipe_, data.data() + sent,
                                static_cast<DWORD>(data.size() - sent), &written,
                                &ov);
    if (!ok) {
      if (::GetLastError() != ERROR_IO_PENDING) return false;
      if (::WaitForSingleObject(event_, timeout_ms) != WAIT_OBJECT_0) {
        ::CancelIoEx(pipe_, &ov);
        // 一定要等它真的結束才可以離開 —— ov 與 data 都在堆疊上,
        // 提早返回等於讓核心繼續寫一塊已經沒有的記憶體。
        ::GetOverlappedResult(pipe_, &ov, &written, TRUE);
        return false;
      }
      if (!::GetOverlappedResult(pipe_, &ov, &written, FALSE)) return false;
    }
    if (written == 0) return false;
    sent += written;
  }
  return true;
}

bool IpcClient::ReadFrameTimed(std::string* payload, DWORD timeout_ms) {
  // 緩衝區裡可能已經有一則完整訊息(上一次讀多了)。
  if (reader_.Next(payload)) return true;

  const DWORD deadline = ::GetTickCount() + timeout_ms;
  char buf[4096];
  for (;;) {
    OVERLAPPED ov{};
    ov.hEvent = event_;
    ::ResetEvent(event_);
    DWORD got = 0;
    const BOOL ok = ::ReadFile(pipe_, buf, sizeof(buf), &got, &ov);
    if (!ok) {
      if (::GetLastError() != ERROR_IO_PENDING) return false;
      const DWORD now = ::GetTickCount();
      const DWORD left = (now >= deadline) ? 0 : (deadline - now);
      if (::WaitForSingleObject(event_, left) != WAIT_OBJECT_0) {
        ::CancelIoEx(pipe_, &ov);
        ::GetOverlappedResult(pipe_, &ov, &got, TRUE);
        return false;
      }
      if (!::GetOverlappedResult(pipe_, &ov, &got, FALSE)) return false;
    }
    if (got == 0) return false;  // 對面關了
    if (!reader_.Feed(buf, got)) return false;
    if (reader_.Next(payload)) return true;
    if (::GetTickCount() >= deadline) return false;
  }
}

bool IpcClient::Exchange(const std::string& payload, uint32_t seq,
                         std::string* reply, DWORD timeout_ms) {
  if (pipe_ == INVALID_HANDLE_VALUE) return false;
  if (!WriteAllTimed(Frame(payload), timeout_ms)) {
    Fail(LinkFailure::kIoError);
    return false;
  }
  if (!ReadFrameTimed(reply, timeout_ms)) {
    Fail(LinkFailure::kTimeout);
    return false;
  }
  Op op = Op::kError;
  uint32_t got_seq = 0;
  if (!PeekHeader(*reply, &op, &got_seq)) {
    Fail(LinkFailure::kBadMessage);
    return false;
  }
  if (got_seq != seq) {
    // 序號對不上 = 串流錯位。**不可以**跳過它繼續讀:那只是把錯位往後推。
    Fail(LinkFailure::kBadMessage);
    return false;
  }
  if (op == Op::kError) {
    Fail(LinkFailure::kServiceError);
    return false;
  }
  link_.OnExchangeOk();
  return true;
}

bool IpcClient::Handshake() {
  Hello h;
  h.proto = kProtocolVersion;
  h.shell_abi = static_cast<uint32_t>(RIME_SHELL_ABI_VERSION);
  h.host_pid = ::GetCurrentProcessId();
  {
    wchar_t path[MAX_PATH] = {0};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    h.host_exe = WideToUtf8(path);
  }
  const uint32_t seq = ++seq_;
  std::string reply;
  if (!Exchange(EncodeHello(seq, h), seq, &reply, kConnectTimeoutMs)) return false;

  uint32_t rseq = 0;
  HelloOk ok;
  if (!DecodeHelloOk(reply, &rseq, &ok)) {
    Fail(LinkFailure::kBadMessage);
    return false;
  }
  // 版本協商。rime_shell.h 檔頭要求「不符即拒絕載入」,只是這裡跨了進程。
  // 拒絕的方式是永遠不吃按鍵 —— 使用者打得出英文,只是中文輸入沒作用。
  if (ok.proto != kProtocolVersion ||
      ok.shell_abi != static_cast<uint32_t>(RIME_SHELL_ABI_VERSION)) {
    Fail(LinkFailure::kHandshake);
    return false;
  }
  return true;
}

bool IpcClient::OpenSession() {
  const uint32_t seq = ++seq_;
  std::string reply;
  if (!Exchange(EncodeSessionNew(seq), seq, &reply, kConnectTimeoutMs))
    return false;
  uint32_t rseq = 0;
  SessionOk ok;
  if (!DecodeSessionOk(reply, &rseq, &ok) || ok.session == 0) {
    Fail(LinkFailure::kBadMessage);
    return false;
  }
  session_ = ok.session;
  return true;
}

bool IpcClient::RequestResult(const std::string& payload, uint32_t seq,
                              Result* out) {
  std::string reply;
  if (!Exchange(payload, seq, &reply, kKeyTimeoutMs)) return false;
  uint32_t rseq = 0;
  if (!DecodeResult(reply, &rseq, out)) {
    Fail(LinkFailure::kBadMessage);
    return false;
  }
  return true;
}

bool IpcClient::SendKey(int32_t keysym, uint32_t mods, Result* out) {
  if (!MayEatKey()) return false;
  KeyReq k;
  k.session = session_;
  k.keysym = keysym;
  k.mods = mods;
  const uint32_t seq = ++seq_;
  return RequestResult(EncodeKey(seq, k), seq, out);
}

bool IpcClient::SendSelect(int32_t index, Result* out) {
  if (!MayEatKey()) return false;
  ArgReq a;
  a.session = session_;
  a.arg = index;
  const uint32_t seq = ++seq_;
  return RequestResult(EncodeArg(seq, Op::kSelectCandidate, a), seq, out);
}

bool IpcClient::SendChangePage(bool backward, Result* out) {
  if (!MayEatKey()) return false;
  ArgReq a;
  a.session = session_;
  a.arg = backward ? 1 : 0;
  const uint32_t seq = ++seq_;
  return RequestResult(EncodeArg(seq, Op::kChangePage, a), seq, out);
}

bool IpcClient::SendCommitComposition(Result* out) {
  if (!MayEatKey()) return false;
  const uint32_t seq = ++seq_;
  return RequestResult(EncodeSimple(seq, Op::kCommitComposition, session_), seq,
                       out);
}

bool IpcClient::SendClear(Result* out) {
  if (!MayEatKey()) return false;
  const uint32_t seq = ++seq_;
  return RequestResult(EncodeSimple(seq, Op::kClear, session_), seq, out);
}

bool IpcClient::SendSelectSchema(const std::string& schema_id, Result* out) {
  if (!MayEatKey()) return false;
  SchemaReq m;
  m.session = session_;
  m.schema_id = schema_id;
  const uint32_t seq = ++seq_;
  return RequestResult(EncodeSelectSchema(seq, m), seq, out);
}

void IpcClient::SendOneWay(const std::string& payload) {
  if (pipe_ == INVALID_HANDLE_VALUE) return;
  if (!WriteAllTimed(Frame(payload), kKeyTimeoutMs)) Fail(LinkFailure::kIoError);
}

void IpcClient::SendCaretRect(int32_t l, int32_t t, int32_t r, int32_t b) {
  if (!MayEatKey()) return;
  CaretRect c;
  c.session = session_;
  c.left = l;
  c.top = t;
  c.right = r;
  c.bottom = b;
  SendOneWay(EncodeCaretRect(++seq_, c));
}

void IpcClient::SendFocus(bool focused) {
  if (!MayEatKey()) return;
  ArgReq a;
  a.session = session_;
  a.arg = focused ? 1 : 0;
  SendOneWay(EncodeArg(++seq_, Op::kFocus, a));
}

}  // namespace rimewin
