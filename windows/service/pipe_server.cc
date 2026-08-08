#include "pipe_server.h"

#include <sddl.h>

#include <cstdio>
#include <string>

#include "../common/schema_choice.h"
#include "../winshared/winutil.h"
#include "rime_shell.h"

namespace rimewin {
namespace {

constexpr DWORD kBufSize = 64 * 1024;

// 一次 overlapped 操作,可以被 stop_event 打斷。
// 回傳 false = 失敗或被要求停止,兩者呼叫端的反應都是收掉這條連線。
bool WaitOverlapped(HANDLE pipe, OVERLAPPED* ov, HANDLE stop_event,
                    DWORD* transferred) {
  HANDLE waits[2] = {ov->hEvent, stop_event};
  const DWORD r = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
  if (r != WAIT_OBJECT_0) {
    ::CancelIoEx(pipe, ov);
    // 一定要等它真的結束:ov 在堆疊上,提早返回等於讓核心寫一塊沒有的記憶體。
    ::GetOverlappedResult(pipe, ov, transferred, TRUE);
    return false;
  }
  return ::GetOverlappedResult(pipe, ov, transferred, FALSE) != FALSE;
}

}  // namespace

PipeServer::PipeServer(Engine* engine, CandidateUi* ui, SettingsStore* settings)
    : engine_(engine), ui_(ui), settings_(settings) {
  stop_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

PipeServer::~PipeServer() {
  Stop();
  if (stop_event_) ::CloseHandle(stop_event_);
}

HANDLE PipeServer::CreateInstance(bool first) {
  const std::wstring name = RimePipeName();

  // 只授權目前的使用者。P = protected(不繼承任何東西進來)。
  // 沒有這一段的話,同一台機器上的其他使用者連得上這條管道 ——
  // 而管道上流的是按鍵。
  std::wstring sid = CurrentUserSidString();
  if (sid.empty()) return INVALID_HANDLE_VALUE;
  const std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")";
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
    return INVALID_HANDLE_VALUE;

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.lpSecurityDescriptor = sd;
  sa.bInheritHandle = FALSE;

  DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
  // FIRST_PIPE_INSTANCE 只給第一個實例:別人已經佔用同名管道時,
  // 我們要**失敗**而不是接手。接手等於兩支服務同時在服務同一批宿主。
  if (first) open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;

  HANDLE pipe = ::CreateNamedPipeW(
      name.c_str(), open_mode,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      PIPE_UNLIMITED_INSTANCES, kBufSize, kBufSize, 0, &sa);
  ::LocalFree(sd);
  return pipe;
}

bool PipeServer::Start() {
  HANDLE probe = CreateInstance(true);
  if (probe == INVALID_HANDLE_VALUE) return false;
  ::CloseHandle(probe);
  listen_thread_ = ::CreateThread(nullptr, 0, &PipeServer::ListenEntry, this, 0,
                                  nullptr);
  return listen_thread_ != nullptr;
}

void PipeServer::Stop() {
  if (stopping_.exchange(true)) return;
  if (stop_event_) ::SetEvent(stop_event_);
  if (listen_thread_) {
    ::WaitForSingleObject(listen_thread_, 3000);
    ::CloseHandle(listen_thread_);
    listen_thread_ = nullptr;
  }
  std::vector<std::thread> clients;
  {
    std::lock_guard<std::mutex> lock(mu_);
    clients.swap(clients_);
  }
  for (std::thread& t : clients)
    if (t.joinable()) t.join();
}

DWORD WINAPI PipeServer::ListenEntry(LPVOID self) {
  static_cast<PipeServer*>(self)->ListenLoop();
  return 0;
}

void PipeServer::ListenLoop() {
  bool first = true;
  while (!stopping_.load()) {
    HANDLE pipe = CreateInstance(first);
    first = false;
    if (pipe == INVALID_HANDLE_VALUE) break;

    OVERLAPPED ov{};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL connected = ::ConnectNamedPipe(pipe, &ov);
    DWORD err = ::GetLastError();
    if (!connected && err == ERROR_PIPE_CONNECTED) {
      connected = TRUE;  // 用戶端在 ConnectNamedPipe 之前就連上了
    } else if (!connected && err == ERROR_IO_PENDING) {
      DWORD n = 0;
      connected = WaitOverlapped(pipe, &ov, stop_event_, &n) ? TRUE : FALSE;
    }
    ::CloseHandle(ov.hEvent);

    if (!connected || stopping_.load()) {
      ::CloseHandle(pipe);
      break;
    }
    std::lock_guard<std::mutex> lock(mu_);
    clients_.emplace_back(&PipeServer::ServeClient, this, pipe);
  }
}

void PipeServer::ServeClient(HANDLE pipe) {
  FrameReader reader;
  bool authed = false;
  uint64_t session = 0;
  // 這條連線的宿主是從哪一個語言設定檔進來的。0 = 不知道
  // (v1 的用戶端,或系統問不出來)。**0 必須等於「沒有意見」。**
  uint32_t langid = 0;
  // 上一次看到的方案 id。使用者按 Ctrl+` 換方案時,快照裡的 schema_id
  // 會變 —— 那是我們唯一能發現「他換了」的地方(切換是 librime 內部
  // 處理的,我們沒有攔那顆鍵)。發現了就記進設定,下一個 app 才不會被
  // 打回預設 —— 否則那顆鍵在使用者眼裡是「換了,但換到別的視窗就沒了」。
  std::string last_schema;
  RECT caret{0, 0, 0, 0};
  char buf[8192];

  auto send = [&](const std::string& payload) -> bool {
    const std::string framed = Frame(payload);
    size_t sent = 0;
    while (sent < framed.size()) {
      OVERLAPPED ov{};
      ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
      DWORD written = 0;
      const BOOL ok = ::WriteFile(pipe, framed.data() + sent,
                                  static_cast<DWORD>(framed.size() - sent),
                                  &written, &ov);
      if (!ok && ::GetLastError() == ERROR_IO_PENDING)
        WaitOverlapped(pipe, &ov, stop_event_, &written);
      ::CloseHandle(ov.hEvent);
      if (written == 0) return false;
      sent += written;
    }
    return true;
  };

  auto push_ui = [&](const Snapshot& snap) {
    if (snap.items.empty())
      ui_->Hide();
    else
      ui_->Update(snap, caret);
  };

  // librime 內建的方案切換器(Ctrl+` / F4)是引擎自己處理的,我們沒有
  // 攔那顆鍵 —— 所以「使用者換了方案」只能從快照上看出來。
  auto note_schema = [&](const Snapshot& snap) {
    if (snap.schema_id.empty() || snap.schema_id == last_schema) return;
    last_schema = snap.schema_id;
    if (!settings_ || langid == 0) return;
    Settings st = settings_->Load();
    // 使用者在設定裡明著指定了「所有語言都用這個」的話,不覆蓋他的選擇 ——
    // 那是他要我們不要猜的意思。
    if (!st.Raw(keys::kSchemaForced).empty()) return;
    st.RememberLastUsed(langid, snap.schema_id);
    settings_->Save(st);
  };

  for (;;) {
    std::string payload;
    while (!reader.Next(&payload)) {
      OVERLAPPED ov{};
      ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
      DWORD got = 0;
      const BOOL ok = ::ReadFile(pipe, buf, sizeof(buf), &got, &ov);
      bool fine = ok != FALSE;
      if (!ok && ::GetLastError() == ERROR_IO_PENDING)
        fine = WaitOverlapped(pipe, &ov, stop_event_, &got);
      ::CloseHandle(ov.hEvent);
      if (!fine || got == 0) goto done;
      if (!reader.Feed(buf, got)) goto done;
    }

    Op op;
    uint32_t seq = 0;
    if (!PeekHeader(payload, &op, &seq)) break;

    // HELLO 之前不接受任何其他訊息。少了這道,一個還沒協商版本的用戶端
    // 就能直接開 session,而版本協商正是為了擋下「舊 DLL 配新服務」。
    if (!authed && op != Op::kHello) {
      ErrorMsg e;
      e.code = 1;
      e.text = "尚未握手";
      send(EncodeError(seq, e));
      break;
    }

    switch (op) {
      case Op::kHello: {
        Hello h;
        uint32_t s = 0;
        if (!DecodeHello(payload, &s, &h)) goto done;
        HelloOk ok;
        ok.proto = kProtocolVersion;
        // 回報的是**實作端**的 ABI(rs_abi_version()),不是編譯期常數:
        // rime_shell.h 檔頭要的就是「實作端 vs 呼叫端」的比對。
        ok.shell_abi = static_cast<uint32_t>(rs_abi_version());
        ok.service_version = "rime-quad-windows/0.2";
        // ⚠ 不是「等於最新版」,是「在支援的區間裡」。
        //   DLL 與服務可能來自不同的建置(瀏覽器可以開好幾天,
        //   它握著的 DLL 就是那麼舊)。舊 DLL 連上新服務時,
        //   我們照它宣告的版本解,少掉的欄位取「沒有意見」。
        if (h.proto < kMinProtocolVersion || h.proto > kProtocolVersion) {
          ErrorMsg e;
          e.code = 2;
          e.text = "協議版本不符";
          send(EncodeError(seq, e));
          goto done;
        }
        // 回報**協商出來的**版本,不是我們支援的最新版 ——
        // 用戶端拿它跟自己送出去的比對。
        ok.proto = h.proto;
        authed = true;
        langid = h.input_langid;
        if (!send(EncodeHelloOk(seq, ok))) goto done;
        break;
      }
      case Op::kSessionNew: {
        SessionOk ok;
        ok.session = engine_->NewSession();
        session = ok.session;
        if (ok.session != 0) {
          // ── 這裡就是那個缺陷的修法 ──────────────────────────
          //
          // 使用者從哪一份語言設定檔進來(langid),決定預設用哪個方案
          // 與哪一種字形。設定介面裡的選擇優先於它 —— 完整的優先順序
          // 寫在 common/schema_choice.h。
          //
          // 套用的時機是 session 剛建立時,**不是**每一顆按鍵:
          // 每顆鍵都套的話,使用者用 Ctrl+` 換過的方案會被一直打回去。
          std::vector<std::string> ids;
          for (const auto& kv : engine_->SchemaList()) ids.push_back(kv.first);
          const Settings st =
              settings_ ? settings_->Load() : Settings();
          const SchemaChoice choice =
              ChooseSchema(langid, ids, st.SchemaPref());
          std::vector<OptionAssign> opts =
              PlanVariant(choice.variant, Variant::kFollow);
          // 標點是獨立的一項(不屬於字形的 radio group)。
          const Tri punct = st.GetTri(keys::kTextAsciiPunct);
          if (punct != Tri::kUnset)
            opts.push_back({"ascii_punct", punct == Tri::kTrue});
          engine_->ApplyChoice(ok.session, choice.schema_id, opts);
          last_schema = engine_->SchemaOfSession(ok.session);
        }
        if (!send(EncodeSessionOk(seq, ok))) goto done;
        break;
      }
      case Op::kSessionEnd: {
        uint64_t sid = 0;
        if (DecodeSimple(payload, &seq, &sid)) engine_->EndSession(sid);
        goto done;  // 單向,而且是連線的終點
      }
      case Op::kKey: {
        KeyReq k;
        if (!DecodeKey(payload, &seq, &k)) goto done;
        Result r = engine_->ProcessKey(k.session, k.keysym, k.mods);
        note_schema(r.snap);
        push_ui(r.snap);
        if (!send(EncodeResult(seq, r))) goto done;
        break;
      }
      case Op::kSelectCandidate:
      case Op::kChangePage:
      case Op::kHighlight:
      case Op::kFocus: {
        ArgReq a;
        if (!DecodeArg(payload, &seq, &a)) goto done;
        if (op == Op::kFocus) {
          if (a.arg == 0) ui_->Hide();
          break;  // 單向,不回覆
        }
        Result r;
        if (op == Op::kSelectCandidate)
          r = engine_->SelectCandidate(a.session, a.arg);
        else if (op == Op::kChangePage)
          r = engine_->ChangePage(a.session, a.arg != 0);
        else
          r.handled = false;
        push_ui(r.snap);
        if (!send(EncodeResult(seq, r))) goto done;
        break;
      }
      case Op::kCommitComposition:
      case Op::kClear: {
        uint64_t sid = 0;
        if (!DecodeSimple(payload, &seq, &sid)) goto done;
        Result r = (op == Op::kClear) ? engine_->Clear(sid)
                                      : engine_->CommitComposition(sid);
        push_ui(r.snap);
        if (!send(EncodeResult(seq, r))) goto done;
        break;
      }
      case Op::kCaretRect: {
        CaretRect c;
        if (!DecodeCaretRect(payload, &seq, &c)) goto done;
        caret.left = c.left;
        caret.top = c.top;
        caret.right = c.right;
        caret.bottom = c.bottom;
        break;  // 單向,不回覆
      }
      case Op::kSelectSchema: {
        SchemaReq sc;
        if (!DecodeSelectSchema(payload, &seq, &sc)) goto done;
        Result r = engine_->SelectSchema(sc.session, sc.schema_id);
        push_ui(r.snap);
        if (!send(EncodeResult(seq, r))) goto done;
        break;
      }
      case Op::kOpenSettings: {
        uint64_t sid = 0;
        if (!DecodeSimple(payload, &seq, &sid)) goto done;
        // 單向,不回覆。⚠ 這裡在連線執行緒上,而且宿主的 UI 執行緒
        // 正在等這一則的**下一顆按鍵** —— 回呼只能 PostMessage,
        // 絕不可以在這裡建視窗或等任何東西。
        if (on_open_settings_) on_open_settings_();
        break;
      }
      case Op::kPing:
        if (!send(EncodePong(seq))) goto done;
        break;
      default: {
        ErrorMsg e;
        e.code = 3;
        e.text = "未知的訊息";
        send(EncodeError(seq, e));
        goto done;
      }
    }
  }

done:
  if (session != 0) engine_->EndSession(session);
  ui_->Hide();
  ::FlushFileBuffers(pipe);
  ::DisconnectNamedPipe(pipe);
  ::CloseHandle(pipe);
}

}  // namespace rimewin
