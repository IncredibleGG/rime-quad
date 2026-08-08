#include "pipe_server.h"

#include <sddl.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#include "../common/schema_choice.h"
#include "../winshared/winutil.h"
#include "rime_shell.h"

namespace rimewin {
namespace {

constexpr DWORD kBufSize = 64 * 1024;

// 連續幾次建不出實例 / 接不到連線就放棄。
//
// 不是 1(舊版等於 1:第一次失敗就整條收掉)。單一一次失敗多半是暫時的:
// 系統資源一時不足、某個宿主進程在連上的瞬間死掉。為了那一次就把**所有**
// 宿主的輸入法一起停掉,是把一個小問題放大成一個大問題。
// 也不是無限:真的壞掉時要停下來並說出原因,而不是無聲地空轉。
constexpr int kMaxConsecutiveErrors = 8;

// 兩次重試之間的間隔。這是錯誤路徑上的節流,不是拿睡覺蓋住競態 ——
// 沒有它的話,一個持續失敗的 CreateNamedPipeW 會變成一條 100% CPU 的迴圈。
// 用 stop_event 等而不是 Sleep:停止服務時要立刻醒得過來。
constexpr DWORD kRetryPauseMs = 200;

// 監聽迴圈的診斷。走 stderr(服務進程的 stderr 會被導進 service.log),
// 而且每一行都以 [pipe] 開頭 —— 驗證腳本會濾掉 glog 的 W/I/E 行,
// 這個前綴讓這幾行活得下來。
void Log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fflush(stderr);
}

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

HANDLE PipeServer::CreateInstance(bool first, DWORD* err) {
  if (err) *err = 0;
  const std::wstring name = RimePipeName();

  // 只授權目前的使用者。P = protected(不繼承任何東西進來)。
  // 沒有這一段的話,同一台機器上的其他使用者連得上這條管道 ——
  // 而管道上流的是按鍵。
  std::wstring sid = CurrentUserSidString();
  if (sid.empty()) {
    if (err) *err = ::GetLastError();
    Log("[pipe] 取不到目前使用者的 SID,管道名與 DACL 都算不出來\n");
    return INVALID_HANDLE_VALUE;
  }
  const std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")";
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
    if (err) *err = ::GetLastError();
    return INVALID_HANDLE_VALUE;
  }

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
  const DWORD create_err = ::GetLastError();
  ::LocalFree(sd);
  if (pipe == INVALID_HANDLE_VALUE && err) *err = create_err;
  return pipe;
}

bool PipeServer::Start() {
  // ⚠ 第一個實例建好之後**不關掉**,直接交給監聽執行緒。
  //
  //   舊的寫法是「建一個 → 立刻關掉 → 讓監聽執行緒再建一次」。那開了兩個窗:
  //
  //   1. 關掉到重建之間,這個名字的管道在系統上根本不存在。用戶端這時
  //      CreateFileW 拿到的是 ERROR_FILE_NOT_FOUND —— 與「服務沒在跑」
  //      完全同一個錯誤碼,分不出來。
  //
  //   2. 重建時又帶了 FILE_FLAG_FIRST_PIPE_INSTANCE。剛關掉的那個實例
  //      只要還沒被系統回收乾淨,這一次就會以 ERROR_ACCESS_DENIED 失敗,
  //      而舊的監聽迴圈遇到失敗是直接 break ——**一個字都不印**。
  //      Start() 早就回 true 了,ready 檔照樣寫、`[service] ready` 照樣印,
  //      然後這支服務一輩子沒有管道,而 service.log 乾乾淨淨。
  //
  //   兩個窗都是「間歇」的長相,而且症狀一模一樣:服務說它好了,
  //   用戶端連不上,沒有人說得出為什麼。所以不留窗,也不留第二次宣告。
  DWORD err = 0;
  first_instance_ = CreateInstance(true, &err);
  if (first_instance_ == INVALID_HANDLE_VALUE) {
    Log("[pipe] 第一個管道實例建立失敗(GetLastError=%lu)%s\n",
        static_cast<unsigned long>(err),
        err == ERROR_ACCESS_DENIED
            ? " —— 同名管道已經有主人,多半是另一支服務還在跑"
            : "");
    return false;
  }

  listening_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!listening_event_) {
    Log("[pipe] 就緒事件建立失敗(GetLastError=%lu)\n",
        static_cast<unsigned long>(::GetLastError()));
    ::CloseHandle(first_instance_);
    first_instance_ = INVALID_HANDLE_VALUE;
    return false;
  }

  listen_thread_ = ::CreateThread(nullptr, 0, &PipeServer::ListenEntry, this, 0,
                                  nullptr);
  if (!listen_thread_) {
    Log("[pipe] 監聽執行緒建立失敗(GetLastError=%lu)\n",
        static_cast<unsigned long>(::GetLastError()));
    ::CloseHandle(first_instance_);
    first_instance_ = INVALID_HANDLE_VALUE;
    return false;
  }

  // 等監聽執行緒親自回報「ConnectNamedPipe 已經掛上」。
  //
  // 也一起等執行緒本身:它若是先結束了(建實例失敗之類),
  // WaitForMultipleObjects 會回第二個物件,我們就知道它死了 ——
  // 而不是傻等到逾時再猜。
  HANDLE waits[2] = {listening_event_, listen_thread_};
  const DWORD r = ::WaitForMultipleObjects(2, waits, FALSE, 10000);
  if (r != WAIT_OBJECT_0) {
    Log("[pipe] 管道沒有備妥就要對外服務了(WaitForMultipleObjects=%lu)%s\n",
        static_cast<unsigned long>(r),
        r == WAIT_OBJECT_0 + 1 ? " —— 監聽執行緒已經結束" : "");
    return false;
  }
  return true;
}

void PipeServer::Stop() {
  if (stopping_.exchange(true)) return;
  if (stop_event_) ::SetEvent(stop_event_);
  if (listen_thread_) {
    ::WaitForSingleObject(listen_thread_, 3000);
    ::CloseHandle(listen_thread_);
    listen_thread_ = nullptr;
  }
  // Start() 失敗時第一個實例可能還在我們手上(監聽執行緒沒接手)。
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (first_instance_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(first_instance_);
      first_instance_ = INVALID_HANDLE_VALUE;
    }
  }
  if (listening_event_) {
    ::CloseHandle(listening_event_);
    listening_event_ = nullptr;
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
  // 第一個實例是 Start() 建的,所有權在這裡轉移過來。
  //
  // 走 mu_ 而不是直接讀寫:Start() 逾時放棄時(監聽執行緒還活著)
  // Stop() 也會來收這個 handle,兩邊同時關同一個 handle 是很難查的
  // 那一類錯誤 —— 而且關掉之後那個 handle 值可能已經被別的物件重用。
  HANDLE pipe = INVALID_HANDLE_VALUE;
  {
    std::lock_guard<std::mutex> lock(mu_);
    pipe = first_instance_;
    first_instance_ = INVALID_HANDLE_VALUE;
  }

  int consecutive_errors = 0;
  const char* why = "收到停止訊號";

  while (!stopping_.load()) {
    if (pipe == INVALID_HANDLE_VALUE) {
      DWORD err = 0;
      // 第二個之後的實例不帶 FIRST_PIPE_INSTANCE —— 那個旗標是「宣告這個
      // 名字歸我」的意思,只該宣告一次,而第一次是 Start() 做的。
      pipe = CreateInstance(false, &err);
      if (pipe == INVALID_HANDLE_VALUE) {
        ++consecutive_errors;
        Log("[pipe] 建立管道實例失敗(GetLastError=%lu),第 %d 次\n",
            static_cast<unsigned long>(err), consecutive_errors);
        if (consecutive_errors >= kMaxConsecutiveErrors) {
          why = "連續建立管道實例失敗";
          break;
        }
        if (::WaitForSingleObject(stop_event_, kRetryPauseMs) == WAIT_OBJECT_0)
          break;
        continue;
      }
      consecutive_errors = 0;
    }

    OVERLAPPED ov{};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
      Log("[pipe] accept 用的事件建立失敗(GetLastError=%lu)\n",
          static_cast<unsigned long>(::GetLastError()));
      ::CloseHandle(pipe);
      pipe = INVALID_HANDLE_VALUE;
      why = "accept 用的事件建立失敗";
      break;
    }
    BOOL connected = ::ConnectNamedPipe(pipe, &ov);
    DWORD err = connected ? 0u : ::GetLastError();
    bool pending = false;
    if (!connected && err == ERROR_PIPE_CONNECTED) {
      connected = TRUE;  // 用戶端在 ConnectNamedPipe 之前就連上了
    } else if (!connected && err == ERROR_IO_PENDING) {
      pending = true;
    }

    // ★ 到這一行為止,管道**確定接得起連線**:要嘛已經有人連上,
    //   要嘛 accept 已經掛在核心裡等。Start() 等的就是這個訊號,
    //   而 service/main.cc 的 ready 檔是在 Start() 回來之後才寫的 ——
    //   所以「ready 檔存在」= 「連得上」,不是「大概快好了」。
    //   手動重設的事件,設過就一直是設著,之後每一圈再設一次也無妨。
    if (listening_event_) ::SetEvent(listening_event_);

    if (pending) {
      DWORD n = 0;
      connected = WaitOverlapped(pipe, &ov, stop_event_, &n) ? TRUE : FALSE;
      err = connected ? 0u : ::GetLastError();
    }
    ::CloseHandle(ov.hEvent);

    if (stopping_.load()) {
      ::CloseHandle(pipe);
      // 一定要清掉,不然迴圈外面那道收尾會**再關一次**同一個 handle。
      // 重複關閉不只是「多做一次」:那個 handle 值這時可能已經被別的
      // 物件重用,關掉的就是別人的東西。
      pipe = INVALID_HANDLE_VALUE;
      break;
    }
    if (!connected) {
      // ⚠ 舊版在這裡 break —— 一條連線沒接成,整台伺服器就收攤。
      //   後果是:任何一個宿主進程在連上的瞬間死掉(瀏覽器分頁被關、
      //   程式崩了),就會讓**其他所有程式**的中文輸入一起停掉,
      //   而且不重啟服務不會好。那正是「有時候打不出字」的長相。
      //   一條連線的失敗是那一條的事,換一個實例重來就好。
      ++consecutive_errors;
      Log("[pipe] accept 失敗(GetLastError=%lu),第 %d 次,換一個實例重來\n",
          static_cast<unsigned long>(err), consecutive_errors);
      ::CloseHandle(pipe);
      pipe = INVALID_HANDLE_VALUE;
      if (consecutive_errors >= kMaxConsecutiveErrors) {
        why = "連續 accept 失敗";
        break;
      }
      if (::WaitForSingleObject(stop_event_, kRetryPauseMs) == WAIT_OBJECT_0)
        break;
      continue;
    }
    consecutive_errors = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      clients_.emplace_back(&PipeServer::ServeClient, this, pipe);
    }
    pipe = INVALID_HANDLE_VALUE;  // 所有權轉給那條連線執行緒
  }

  if (pipe != INVALID_HANDLE_VALUE) ::CloseHandle(pipe);

  if (!stopping_.load()) {
    // ⚠ 監聽迴圈在沒有人要求停止的情況下結束了。
    //
    //   這是本輪最重要的一行輸出。舊版在同樣的情況下**什麼都不印**:
    //   服務照樣印 ready、照樣寫 ready 檔、照樣活著佔住單一實例的 mutex,
    //   只是永遠沒有管道。從外面看,那與「協議談不攏」長得一模一樣,
    //   而兩者該修的地方完全不同。
    Log("[pipe] **監聽迴圈非預期結束**:%s。\n"
        "[pipe]   從現在起這支服務沒有管道:每一個宿主都會連不上並 fail-open\n"
        "[pipe]   (中文輸入沒作用,但按鍵不會被吃掉)。服務將自行結束,\n"
        "[pipe]   把單一實例的位置讓出來,好讓 DLL 重新啟動一支乾淨的。\n",
        why);
    if (on_fatal_) on_fatal_();
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
    if (!settings_) return;
    const CharSet mode = CharSetOfLangId(langid);
    if (mode == CharSet::kUnspecified) return;
    Settings st = settings_->Load();
    // 使用者在設定裡明著釘了一個全域方案的話,不覆蓋他的選擇 ——
    // 那是他要我們不要猜的意思。
    if (!st.Raw(keys::kSchemasPinnedGlobal).empty()) return;
    // 規範 §4.4 第 1 層:「使用者為這個輸入模式釘的方案」。
    // 他按 Ctrl+` 換過就是釘過 —— 不記的話,換到別的程式就被打回去,
    // 而那顆鍵在他眼裡等於沒有作用。
    st.SetPinnedForCharSet(mode, snap.schema_id);
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
          // ── 這裡就是那個缺陷的修法(docs/settings-model.md §4)──
          //
          // 使用者從哪一份語言設定檔進來(langid = 輸入模式),決定預設
          // 用哪個方案與要不要簡體。設定介面裡釘的方案優先於它 ——
          // 完整的四層優先順序在 common/schema_choice.h。
          //
          // 套用的時機是 session 剛建立時,**不是**每一顆按鍵:
          // 每顆鍵都套的話,使用者用 Ctrl+` 換過的方案會被一直打回去。
          engine_->SetSessionLangId(ok.session, langid);
          std::vector<std::string> ids;
          for (const auto& kv : engine_->SchemaList()) ids.push_back(kv.first);
          const Settings st = settings_ ? settings_->Load() : Settings();
          const SchemaPreference pref = st.SchemaPref();
          const SchemaChoice choice = ChooseSchema(langid, ids, pref);
          std::vector<OptionAssign> opts;
          if (choice.set_variant) opts = PlanVariant(choice.simplified, langid);
          // 標點是獨立的一項(不屬於簡繁那一組)。
          // ⚠ kUnset = followSchema = **完全不呼叫 rs_set_option**。
          //   設成 false 不是同一件事:很多方案根本沒有那個開關,
          //   而有些方案的預設是 true。
          const Tri punct = st.Punctuation();
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
