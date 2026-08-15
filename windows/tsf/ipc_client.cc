#include "ipc_client.h"

#include <vector>

#include "../winshared/winutil.h"
#include "../common/key_deadline.h"
#include "trace.h"

// 版本協商用:DLL 側編譯時看到的門面 ABI。DLL 本身**不連結** rime_shell,
// 只取這一個常數,所以只 include 標頭不會把 librime 拖進宿主進程。
#include "rime_shell.h"

namespace rimewin {
namespace {

// ⚠ 按鍵往返的逾時**不在這裡** —— 它與服務端的上限是一組的,
//   兩個都住在 common/key_deadline.h(kKeyTimeoutMs / kKeyDeadlineMs),
//   由那裡的 static_assert 守住「服務端必須先放棄」這條關係。
//
//   效能紅線仍然是「從按鍵到候選更新一到兩幀」(docs/handoff-windows.md
//   §5),也就是 16–33ms。那個逾時不是目標值,是**放棄的門檻**。

// 連線與握手可以久一點:它不在每一顆按鍵的路徑上。
constexpr DWORD kConnectTimeoutMs = 300;

// 自動啟動服務之後,等管道出現的上限。
constexpr DWORD kWaitPipeMs = 400;

// 兩次嘗試啟動服務之間的最短間隔。沒有這個節流的話,服務只要一直啟動失敗,
// 每一顆按鍵都會 CreateProcess 一次。
constexpr int64_t kLaunchCooldownMs = 10000;

// 連線摘要兩行之間的最短間隔。抖動時 EnsureReady 每 500ms 成功一次,
// 而摘要是**累積**的 —— 十秒寫一行就足以讓使用者貼上來的 tsf.log
// 說得出「重連了幾百次、掉了幾顆鍵」,又不會把行程額度吃光。
constexpr int64_t kLinkSummaryMs = 10000;

int64_t NowMs() { return static_cast<int64_t>(::GetTickCount64()); }

// 這一種失敗值不值得**降級**重試一次舊版的 HELLO。
//
// ⚠ 分辨這件事是有代價的,所以要分辨清楚:每一次多餘的降級重試,都是
//   在宿主的 UI 執行緒上再花一次 kConnectTimeoutMs,而且會把**真正的
//   失敗原因蓋掉** —— 第二輪的 Connect() 會把 last_failure 覆寫成
//   kConnectFailed,於是報表上寫「連不上」,實際發生的卻是別的事。
//
// 值得重試的只有「對面看不懂 / 不接受我們宣告的版本」這一類:
//   kPeerClosed   舊服務解不開 v2 的 HELLO 時做的正是這件事:整則丟掉、
//                 關掉連線(見 service/pipe_server.cc 的 DecodeHello 失敗路徑)。
//   kServiceError 對面明確回了「協議版本不符」。
//   kHandshake    對面回的 HELLO_OK 裡的版本我們不接受。
//   kBadMessage   對面回了一則我們解不開的東西 —— 也是版本形狀的問題。
//
// 不值得的:
//   kTimeout      對面只是慢。再宣告一次舊版一樣會慢。
//   kIoError      連線本身壞了。
//   kConnectFailed 根本沒連上,連 HELLO 都還沒送出去。
bool WorthDowngrading(LinkFailure k) {
  switch (k) {
    case LinkFailure::kPeerClosed:
    case LinkFailure::kServiceError:
    case LinkFailure::kHandshake:
    case LinkFailure::kBadMessage:
      return true;
    case LinkFailure::kTimeout:
    case LinkFailure::kIoError:
    case LinkFailure::kConnectFailed:
    // 服務給不出 session 與版本無關 —— 它多半正在部署。再宣告一次舊版
    // 只會多花一份逾時預算,而且會把「引擎沒空」蓋成「連不上」。
    case LinkFailure::kNoSession:
      return false;
  }
  return false;
}

}  // namespace

bool ServiceIsRunning() {
  // OpenMutexW 成功 = 那個名字底下已經有一個互斥鎖存在 = 服務進程活著。
  // 只要 SYNCHRONIZE 權限,不去取得它 —— 取得了會擋住服務自己的收尾。
  HANDLE m = ::OpenMutexW(SYNCHRONIZE, FALSE, RimeServiceMutexName().c_str());
  if (!m) return false;
  ::CloseHandle(m);
  return true;
}

bool LaunchService(const std::wstring& service_path) {
  if (service_path.empty()) return false;
  // 判準與理由見 windows/common/elevation_policy.h。
  //
  // ⚠ 這裡**一定要留下一行記錄**。舊版是一句 `if (IsProcessElevated())
  //   return false;`,安靜地什麼都不做 —— 於是「刻意不啟動」與「壞掉」
  //   在使用者那裡長得一模一樣。
  const HostElevation elev = ClassifyHostElevation();
  if (!MayStartUserService(elev)) {
    Trace("不啟動服務(刻意):%s / %s", HostElevationTag(elev),
          HostElevationZh(elev));
    return false;
  }

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  std::wstring cmd = L"\"" + service_path + L"\"";
  std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
  mutable_cmd.push_back(L'\0');

  if (!::CreateProcessW(service_path.c_str(), mutable_cmd.data(), nullptr,
                        nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS,
                        nullptr, nullptr, &si, &pi)) {
    // ⚠ 路徑走 WideToUtf8 而不是 printf 的 %ls。MSVC 的窄字元 printf 用
    //   目前的 C locale 去轉寬字元,預設的 "C" locale 碰到非 ASCII 就整段
    //   截斷 —— 而安裝路徑裡有中文正是最需要看到這一行的時候。
    Trace("啟動服務失敗 err=%lu path=%s",
          static_cast<unsigned long>(::GetLastError()),
          WideToUtf8(service_path).c_str());
    return false;
  }
  Trace("已啟動服務 pid=%lu", static_cast<unsigned long>(pi.dwProcessId));
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  return true;
}

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

void IpcClient::ResetLink() {
  // 與 SetProfile 走同一套動作:收掉連線、狀態機歸零。
  // 不呼叫 Fail() —— 這不是失敗,不該進退避。
  Close();
  link_ = LinkState();
  negotiated_proto_ = 0;
}

void IpcClient::MaybeTraceLinkSummary() {
  if (summary_budget_ <= 0) return;
  const int64_t now = NowMs();
  if (last_summary_ms_ != 0 && now - last_summary_ms_ < kLinkSummaryMs) return;
  last_summary_ms_ = now;
  --summary_budget_;
  // ⚠ 這一行要自成一體。舊版是「按鍵那一行說吃不吃,連線失敗那一行說原因」,
  //   而兩邊各有各的額度(5 行 / 6 行)—— 用完之後那個交叉參照就斷了,
  //   使用者貼上來的檔案回答不了任何問題。
  Trace("連線摘要 重連=%u次 上一條活了=%ums 斷線期間放行=%u鍵 "
        "兩趟之間斷線改由我們收尾=%u鍵 "
        "上次原因=%s(階段=%s os_err=%lu 服務說=%s)",
        static_cast<unsigned>(reconnects_),
        static_cast<unsigned>(last_link_ms_),
        static_cast<unsigned>(keys_passed_while_down_),
        static_cast<unsigned>(keys_rescued_between_passes_),
        LinkFailureName(link_.last_failure()), ReadyStageName(diag_.stage),
        static_cast<unsigned long>(diag_.os_error),
        diag_.service_error.empty() ? "(沒說)" : diag_.service_error.c_str());
}

void IpcClient::Fail(LinkFailure kind) {
  // 任何失敗都把連線整條丟掉。
  //
  // 特別是逾時:一次逾時之後,那則回覆仍可能晚一點才到,而管道上的下一個
  // 讀取就會拿到它 —— 從此每一次請求都收到上一次的答案。使用者看到的是
  // 「輸入法慢一拍」,而那種錯位幾乎查不出來。關掉重來是唯一乾淨的做法。
  const int64_t now = NowMs();
  // 這一條連線活了多久。抖動的樣子就是這個數字很小(300ms 上下)而
  // reconnects_ 一直漲 —— 那兩個數字擺在一起才是可讀的訊號。
  //
  // ⚠ **這裡不寫 Trace。** 它跑在宿主的 UI 執行緒上,而且抖動時每 500ms
  //   來一次;寫在這裡就是重蹈本檔標頭那個覆轍。只留資料給摘要。
  if (connected_at_ms_ != 0) {
    const int64_t lived = now - connected_at_ms_;
    last_link_ms_ = static_cast<uint32_t>(lived > 0 ? lived : 0);
    connected_at_ms_ = 0;
  }
  Close();
  // 記進診斷。**階段**(kPipe / kHandshake / kSession)由呼叫端自己設 ——
  // 只有它知道當下在做哪一步,而「哪一步」正是診斷裡最有用的一格。
  diag_.failure = kind;
  link_.OnFailure(kind, now);
}

void IpcClient::SetProfile(uint32_t langid, const std::string& profile_guid) {
  if (langid == langid_ && profile_guid == profile_guid_) return;
  langid_ = langid;
  profile_guid_ = profile_guid;
  // 重新握手。見標頭的說明:服務是在 HELLO 當下決定預設方案的。
  //
  // 不呼叫 Fail():這不是失敗,不該進退避。單純把連線收掉,
  // 下一顆按鍵會照常重連 —— 而重連在退避歸零的狀態下是立刻的。
  Close();
  link_ = LinkState();
  negotiated_proto_ = 0;
}

bool IpcClient::EnsureReady() {
  if (link_.MayEatKey() && pipe_ != INVALID_HANDLE_VALUE && session_ != 0)
    return true;
  const int64_t now = NowMs();
  // 被退避擋掉的那些**不算一次嘗試**,所以不動 diag_.attempts ——
  // 呼叫端(rime_probe)要能說出「我真的試了幾次」,而不是「我迴圈了幾次」。
  if (!link_.ShouldAttemptConnect(now)) return false;
  link_.OnAttempt(now);

  const uint32_t attempts_so_far = diag_.attempts;
  diag_ = ReadyDiagnosis();
  diag_.attempts = attempts_so_far + 1;
  diag_.my_shell_abi = static_cast<uint32_t>(RIME_SHELL_ABI_VERSION);

  // ── 降級重試 ────────────────────────────────────────────────
  //
  // DLL 與服務可能來自不同的建置:使用者更新到一半,或某個長壽的宿主
  // 進程(瀏覽器可以開好幾天)還握著舊的 DLL。舊服務看到 v2 的 HELLO
  // 會整則丟掉並關掉連線,所以新 DLL 要**自己退一步**再試一次 v1。
  //
  // 退到 v1 的代價只有「服務不知道 langid」——預設方案退回 schema_list
  // 第一項,輸入法照常能用。不退的話,使用者看到的是「更新到一半之後
  // 中文輸入在某些程式裡整個消失」,而且他分不出是哪一半。
  //
  // ⚠ 但**只有版本形狀的失敗才降級**(見上面的 WorthDowngrading)。
  //   舊版是「v2 一失敗就退 v1」,那有兩個代價,而第二個是這一輪查出來的:
  //     1. 逾時之後再退一次,等於在宿主的 UI 執行緒上多花一份逾時預算。
  //     2. **它會吃掉真正的錯誤訊息。** 第二輪的 Connect() 會把
  //        link_ 的 last_failure 覆寫掉,於是「握手談不攏」被記成
  //        「連不上」——而這兩件事要修的地方完全不同。
  //
  // 最多退到 kMinProtocolVersion。每一輪都要重新開管道:握手失敗時
  // 連線已經被收掉了。
  bool ok = false;
  for (uint32_t p = kProtocolVersion;;) {
    if (ConnectAndHandshake(p)) {
      ok = true;
      break;
    }
    if (p <= kMinProtocolVersion) break;  // uint32_t 不能減到負的
    if (diag_.stage != ReadyStage::kHandshake) break;
    if (!WorthDowngrading(diag_.failure)) break;
    --p;
  }
  if (!ok || !OpenSession()) {
    if (trace_budget_ > 0) {
      --trace_budget_;
      // 三個欄位缺一不可,而且**不可以併成一句話**:
      //   stage  = 開管道 / 握手 / 建 session,三者要修的地方完全不同
      //   os_err = 2(沒有這條管道)與 5(有,但這個身分開不了)長得一樣
      //   peer   = 版本不合時,對方報的版本是唯一有用的線索
      // ⚠ 服務端說的話要原樣印出來。這一行是 fail-open 之下**唯一**留下的
      //   線索 —— 使用者看到的只有「打出英文」,沒有任何錯誤訊息。
      Trace("連線失敗 第%u次 階段=%s 原因=%s os_err=%lu 宣告proto=%u abi=%u "
            "對方%s(proto=%u abi=%u) 服務說=%s(code=%u)",
            static_cast<unsigned>(diag_.attempts), ReadyStageName(diag_.stage),
            LinkFailureName(diag_.failure),
            static_cast<unsigned long>(diag_.os_error),
            static_cast<unsigned>(diag_.tried_proto),
            static_cast<unsigned>(diag_.my_shell_abi),
            diag_.peer_replied ? "回了" : "沒回",
            static_cast<unsigned>(diag_.peer.proto),
            static_cast<unsigned>(diag_.peer.shell_abi),
            diag_.service_error.empty() ? "(沒說)" : diag_.service_error.c_str(),
            static_cast<unsigned>(diag_.service_error_code));
    }
    return false;
  }
  const int64_t ready_ms = NowMs();
  link_.OnConnected(ready_ms);
  diag_.stage = ReadyStage::kNone;
  if (ever_connected_) ++reconnects_;
  ever_connected_ = true;
  connected_at_ms_ = ready_ms;
  // ⚠ 這一行以前**沒有額度**,而它是抖動時每 500ms 寫一次的那一行 ——
  //   行程額度只有 400 行(trace.cc 的 kMaxLinesPerProcess),所以它會把
  //   ActivateEx 那幾行擠掉,而那正是「記錄裡完全沒有按鍵行」要查的地方。
  //   前三次照寫(第一次連上的 proto / abi / session 是有價值的),
  //   之後交給節流的摘要。
  if (ready_trace_budget_ > 0) {
    --ready_trace_budget_;
    Trace("連線就緒 proto=%u abi=%u session=%llu",
          static_cast<unsigned>(negotiated_proto_),
          static_cast<unsigned>(diag_.my_shell_abi),
          static_cast<unsigned long long>(session_));
  }
  MaybeTraceLinkSummary();
  return true;
}

bool IpcClient::ConnectAndHandshake(uint32_t proto) {
  if (pipe_ != INVALID_HANDLE_VALUE) Close();
  if (!Connect()) return false;
  if (!Handshake(proto)) return false;
  negotiated_proto_ = proto;
  return true;
}

bool IpcClient::Connect() {
  const std::wstring name = RimePipeName();
  DWORD last_err = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    pipe_ = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe_ != INVALID_HANDLE_VALUE) return true;

    const DWORD err = ::GetLastError();
    last_err = err;
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
  // 錯誤碼一定要留下來。ERROR_FILE_NOT_FOUND(2)= 這條管道不存在
  // (服務沒在監聽);ERROR_ACCESS_DENIED(5)= 管道在,但這個身分開不了
  // (提權與非提權的宿主各有一份權杖,而 DACL 只授權目前使用者)。
  // 兩者的症狀一模一樣,修的地方完全不同。
  diag_.stage = ReadyStage::kPipe;
  diag_.failure = LinkFailure::kConnectFailed;
  diag_.os_error = static_cast<unsigned long>(last_err);
  link_.OnFailure(LinkFailure::kConnectFailed, NowMs());
  return false;
}

bool IpcClient::TryLaunchService() {
  const int64_t now = NowMs();
  if (last_launch_ms_ >= 0 && now - last_launch_ms_ < kLaunchCooldownMs)
    return false;
  last_launch_ms_ = now;

  // 服務已經在跑,只是管道還沒開(首次部署要編譯詞庫,好幾分鐘)。
  // 再啟動一支只會被單一實例的互斥鎖擋掉,而且它會安靜地以 0 結束 ——
  // 白花一次 CreateProcess,還會讓「服務沒起來」與「服務起來了但還沒好」
  // 在診斷上混成同一件事。
  if (ServiceIsRunning()) return false;

  return LaunchService(service_path_);
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

bool IpcClient::ReadFrameTimed(std::string* payload, DWORD timeout_ms,
                               bool* eof) {
  if (eof) *eof = false;
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
      if (!::GetOverlappedResult(pipe_, &ov, &got, FALSE)) {
        // ERROR_BROKEN_PIPE = 對面把它那一端關掉了。這不是逾時。
        if (eof && ::GetLastError() == ERROR_BROKEN_PIPE) *eof = true;
        return false;
      }
    }
    if (got == 0) {
      // 對面乾淨地關了。**這不是逾時** —— 舊服務解不開新版的 HELLO 時
      // 做的正是這件事,而那是唯一值得降級重試 v1 的情形。
      if (eof) *eof = true;
      return false;
    }
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
  bool eof = false;
  if (!ReadFrameTimed(reply, timeout_ms, &eof)) {
    Fail(eof ? LinkFailure::kPeerClosed : LinkFailure::kTimeout);
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
    // ⚠ 一定要把那則訊息讀出來再放棄。服務端**已經把原因寫在裡面了**
    //   (「尚未握手」/「協議版本不符」/「引擎現在建不出 session」),
    //   而丟掉它之後診斷就只剩一句「服務回了 ERROR」——
    //   五種完全不同的原因長成同一句話,而 fail-open 讓使用者什麼都看不到。
    uint32_t eseq = 0;
    ErrorMsg em;
    if (DecodeError(*reply, &eseq, &em)) {
      diag_.service_error_code = em.code;
      diag_.service_error = em.text;
    }
    Fail(LinkFailure::kServiceError);
    return false;
  }
  link_.OnExchangeOk(NowMs());
  return true;
}

bool IpcClient::Handshake(uint32_t proto) {
  // 從這裡開始的失敗都算在「握手」這一步上。管道已經開起來了,
  // 所以任何失敗都不該被報成「連不上服務」。
  diag_.stage = ReadyStage::kHandshake;
  diag_.tried_proto = proto;
  diag_.peer_replied = false;
  diag_.peer = HelloOk();
  Hello h;
  h.proto = proto;
  h.shell_abi = static_cast<uint32_t>(RIME_SHELL_ABI_VERSION);
  h.host_pid = ::GetCurrentProcessId();
  // EncodeHello 依 h.proto 決定要不要寫尾巴。proto < 2 時這兩個欄位
  // 一個位元組都不會上線路,舊服務因此解得開。
  h.input_langid = langid_;
  h.profile_guid = profile_guid_;
  // ⚠ **啟用我們的那一條 TSF 執行緒**。這一支只在宿主的 UI 執行緒上被
  //   呼叫(EnsureReady 的三個呼叫點都在按鍵路徑上),所以
  //   GetCurrentThreadId() 就是答案。服務端拿它跟前景那條執行緒比,
  //   決定那一橫該不該顯示(common/bar_owner.h)。
  h.host_tid = static_cast<uint32_t>(::GetCurrentThreadId());
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
  // 對方說了什麼一定要留著,**包含版本不合的時候** ——
  // 那正是唯一有用的線索:「它說它是 proto=1 / abi=1」直接指出誰是舊的。
  diag_.peer_replied = true;
  diag_.peer = ok;
  // 版本協商。rime_shell.h 檔頭要求「不符即拒絕載入」,只是這裡跨了進程。
  // 拒絕的方式是永遠不吃按鍵 —— 使用者打得出英文,只是中文輸入沒作用。
  if (ok.proto != proto ||
      ok.shell_abi != static_cast<uint32_t>(RIME_SHELL_ABI_VERSION)) {
    Fail(LinkFailure::kHandshake);
    return false;
  }
  return true;
}

bool IpcClient::OpenSession() {
  // 握手已經過了 —— 從這裡開始的失敗與「版本不合」無關,
  // 報成「握手失敗」會把人往完全錯的方向送。
  diag_.stage = ReadyStage::kSession;
  const uint32_t seq = ++seq_;
  std::string reply;
  // ⚠ 這一趟往返的預算與握手同一個(300ms),而服務端在 SESSION_NEW 裡
  //   會替使用者的語言選預設方案 —— 第一次選方案要載入詞典,冷的時候
  //   是幾百毫秒到幾秒。所以服務端**必須在開管道之前先預熱**
  //   (見 service/main.cc 的 WarmUpEngine)。少了那一步,第一個連上來的
  //   宿主必定在這裡逾時,而使用者看到的是「剛開機時第一個程式打不出中文」。
  //
  //   為什麼不乾脆把預算調大:EnsureReady() 跑在宿主的 UI 執行緒上
  //   (text_service.cc 的 OnTestKeyDown / HandleKey)。調大等於讓使用者
  //   按下第一顆鍵時整個程式卡住那麼久 —— 那不是修好,是換一種壞法。
  if (!Exchange(EncodeSessionNew(seq), seq, &reply, kConnectTimeoutMs))
    return false;
  uint32_t rseq = 0;
  SessionOk ok;
  // ⚠ 這兩件事**不可以共用一個失敗種類**,而它們曾經共用過。
  //
  //   · 解不開      = 線路格式或序號真的錯了 → 去查 protocol.cc 與分幀。
  //   · session == 0 = 線路完全正常,是**引擎給不出 session**
  //                    (librime 部署期間 rs_session_create 就是回這個)。
  //
  //   併成 kBadMessage 的代價是實際發生過的:診斷寫著「訊息解不開或
  //   序號錯位」,而編解碼一個位元都沒錯,於是查的人被送去看一段好的程式碼。
  //   新版服務會明著回一則 ERROR;這一格接住的是**舊版服務**送來的
  //   session=0(升級之後舊 DLL 配新服務、或新 DLL 配舊服務都可能發生)。
  if (!DecodeSessionOk(reply, &rseq, &ok)) {
    Fail(LinkFailure::kBadMessage);
    return false;
  }
  if (ok.session == 0) {
    Fail(LinkFailure::kNoSession);
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

bool IpcClient::SendOpenSettings() {
  // ⚠ 只有協商到 v2 之後才准送。v1 的服務收到不認得的 op 會回錯誤
  //   並關掉連線 —— 那會讓「按了設定按鈕之後輸入法忽然沒反應」變成
  //   一個沒有人猜得到成因的症狀。
  if (!MayEatKey() || negotiated_proto_ < 2) return false;
  SendOneWay(EncodeSimple(++seq_, Op::kOpenSettings, session_));
  return true;
}

void IpcClient::SendFocus(bool focused) {
  if (!MayEatKey()) return;
  ArgReq a;
  a.session = session_;
  a.arg = focused ? 1 : 0;
  SendOneWay(EncodeArg(++seq_, Op::kFocus, a));
}

}  // namespace rimewin
