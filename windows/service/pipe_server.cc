#include "pipe_server.h"

#include "status_bar.h"

#include <sddl.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#include "../common/hotkey_policy.h"
#include "../common/redeploy_flow.h"
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
  // ⚠ 部署之後引擎要把 session 建回來,而「該套什麼」只有這裡讀得到
  //   (設定檔 + 語言設定檔 + 方案清單)。把同一支函式交給它,
  //   兩條路就不可能漂移。
  // ⚠ this 的壽命:PipeServer 由 service/main.cc 持有,而它比 Engine
  //   晚建、早收(Stop() 在 Engine::Stop() 之前)。回呼只會在引擎執行緒
  //   上被叫到,而引擎在 Stop() 時會把佇列排乾再 join。
  if (engine_) {
    engine_->SetSessionPlanner(
        [this](uint32_t langid,
               const std::vector<std::pair<std::string, std::string>>& s) {
          return PlanForLang(langid, s);
        });
  }
}

Engine::SessionPlan PipeServer::PlanForLang(
    uint32_t langid,
    const std::vector<std::pair<std::string, std::string>>& schemas) const {
  std::vector<std::string> ids;
  for (const auto& kv : schemas) ids.push_back(kv.first);
  const Settings st = settings_ ? settings_->Load() : Settings();
  const SchemaChoice choice = ChooseSchema(langid, ids, st.SchemaPref());
  Engine::SessionPlan plan;
  plan.schema_id = choice.schema_id;
  if (choice.set_variant) plan.options = PlanVariant(choice.simplified, langid);
  // 標點是獨立的一項(不屬於簡繁那一組)。
  // ⚠ kUnset = followSchema = **完全不呼叫 rs_set_option**。
  //   設成 false 不是同一件事:很多方案根本沒有那個開關,
  //   而有些方案的預設是 true。
  const Tri punct = st.Punctuation();
  if (punct != Tri::kUnset)
    plan.options.push_back({"ascii_punct", punct == Tri::kTrue});
  // ── 中英模式 ────────────────────────────────────────────────
  //
  // ⚠ 使用者用懸浮狀態列切成英文之後開一個新程式,那個程式**也**要是
  //   英文的 —— 少了它,症狀是「這個開關會自己跳回去」,而使用者不會把
  //   「我開了一個新視窗」與那件事聯想在一起。
  //
  // ⚠ 放進 options(而不是建完 session 再設一次)是刻意的:
  //   options 同時是備用 session 的**計畫**,而 TakeSpareSession 會比對
  //   計畫合不合。不放進來的話,備用池裡那個照舊狀態配好的 session
  //   會被判成「計畫相同」而直接交出去。
  //
  // ⚠ 與標點不同,這裡**沒有**「不干預」那一態:中英是一個模式,
  //   不是一個三態偏好,而它的預設(false = 中文)就是 librime 的預設。
  plan.options.push_back({"ascii_mode", engine_ && engine_->AsciiMode()});
  return plan;
}

PipeServer::~PipeServer() {
  Stop();
  // ⚠ 引擎手上那個回呼捕捉的是 this。它比我們晚死(main.cc 的區域變數
  //   宣告順序),所以走之前要把它收回來 —— 不然引擎關閉時排乾佇列的
  //   那一下,重建工作會呼叫一個已經不在的物件。
  if (engine_) engine_->SetSessionPlanner(nullptr);
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
  // 這個 session 的第一份快照還沒看過。
  //
  // ⚠ 它取代的是 SESSION_NEW 裡那一趟 engine_->SchemaOfSession():
  //   我們需要知道「一開始是哪個方案」,好讓 note_schema 不要把
  //   **我們自己剛選的**方案誤判成「使用者按了 Ctrl+` 換方案」而寫進設定。
  //   但那個答案要再跑一趟引擎佇列才問得到,而那一趟就排在 ApplyChoice
  //   後面 —— 白白多花一次往返,而 SESSION_NEW 只有 300 毫秒的預算。
  //   看到的第一份快照就是答案,不必問。
  bool schema_seeded = false;
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
    // 同一份快照也餵給懸浮狀態列。⚠ **同一份**是重點:兩個表面顯示
    //   不一致的值是規範明文不允許的(§12.10.1)。
    if (bar_) bar_->OnSnapshot(snap);
  };

  // librime 內建的方案切換器(Ctrl+` / F4)是引擎自己處理的,我們沒有
  // 攔那顆鍵 —— 所以「使用者換了方案」只能從快照上看出來。
  auto note_schema = [&](const Snapshot& snap) {
    if (snap.schema_id.empty() || snap.schema_id == last_schema) return;
    last_schema = snap.schema_id;
    // ⚠ 第一份快照只記下來,**不當成使用者換了方案**。
    //   那一份是 SESSION_NEW 時我們自己套上去的(ApplyChoice),
    //   把它寫進設定等於幫使用者「釘」了一個他沒有選過的方案。
    if (!schema_seeded) {
      schema_seeded = true;
      return;
    }
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
        ok.service_version = "luminakey-windows/0.2";
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
        // ⚠ 這一趟有 300 毫秒的預算(ipc_client.cc 的 kConnectTimeoutMs),
        //   而它跑在**宿主的 UI 執行緒**上。超過就 fail-open:那個宿主
        //   整個工作階段都打不出中文,而使用者只看到英文、沒有錯誤訊息。
        //   2026-08-09 的按鍵矩陣 18 個宿主裡有 8 個卡在這裡。
        //   所以下面每一步都要問一句:它非得擋在這條路徑上嗎?
        const DWORD t_begin = ::GetTickCount();
        // ── 先把「這個語言該用什麼」算出來 ──────────────────────
        //
        // 兩件事都要用到它:比對備用 session 的計畫合不合,以及(沒有
        // 備用時)當場套上去。方案清單走快取,所以這一段不進引擎佇列。
        // ⚠ 拿不到清單(引擎在停)與「一個方案都沒有」不可以混成同一件事:
        //   兩者都會讓 ChooseSchema 挑不到東西,而前者是暫時的。
        //   這裡不能擋著不放行(宿主那一側只有 50 毫秒的預算),但要
        //   在記錄裡分得出來 —— 出事時那一行是唯一的線索。
        std::vector<std::pair<std::string, std::string>> schemas;
        const WorkQueue::Status schema_st = engine_->SchemaListCached(&schemas);
        if (schema_st != WorkQueue::Status::kDone)
          Log("[pipe] 方案清單拿不到(引擎沒有回應)—— 這一個 session 不套方案\n");
        // ⚠ **與部署後重建走同一支**(PlanForLang / engine.h 的 SessionPlanner)。
        //   算法留在這裡而重建那條路自己算一次的話,兩份就會漂移。
        const Engine::SessionPlan plan = PlanForLang(langid, schemas);
        const std::string& chosen_schema = plan.schema_id;
        const std::vector<OptionAssign>& opts = plan.options;

        // ── 有沒有預先建好的可以直接拿 ────────────────────────
        //
        // ⚠ 這是整段最重要的一行。量到的 rs_session_create 偶爾要 442~753
        //   毫秒(CI run 31316116994),而這一趟的預算是 300 毫秒 ——
        //   那不是排程解得掉的,只能**不要在使用者等著的時候建**。
        SessionOk ok;
        bool from_spare = false;
        ok.session = engine_->TakeSpareSession(langid, chosen_schema, opts);
        if (ok.session != 0) {
          from_spare = true;
        } else {
          ok.session = engine_->NewSession();
        }
        session = ok.session;
        const DWORD t_created = ::GetTickCount();
        if (ok.session == 0) {
          // ⚠ **不可以送一則 session=0 的 SESSION_OK。**
          //
          //   舊版就是那樣做的,而用戶端把「session 是 0」判成
          //   kBadMessage —— 於是診斷寫著「訊息解不開或序號錯位」,
          //   而線路格式一個位元都沒錯。看到那句話的人會去查編解碼與分幀,
          //   那兩段都是好的;真正的原因幾乎一定是 librime 正在部署
          //   (那段期間 rs_session_create() 就是給不出東西)。
          //
          //   明著回一則 ERROR,把原因寫進去。用戶端會把這段字原樣記進
          //   除錯記錄 —— fail-open 之下那是唯一留得下來的線索。
          ErrorMsg e;
          e.code = 4;
          // ⚠ 三種原因要分得開(common/redeploy_flow.h)。舊版把
          //   「使用者剛按了重新整理字詞」說成「引擎建不出 session」,
          //   而看記錄的人會去查管道與編解碼 —— 那兩段都是好的。
          e.text = SessionRefusedReason(engine_->redeploy_phase(),
                                        engine_->deploy_ok());
          Log("[pipe] SESSION_NEW 失敗:%s(等了 %lu ms)\n", e.text.c_str(),
              static_cast<unsigned long>(t_created - t_begin));
          send(EncodeError(seq, e));
          break;
        }
        {
          // ── 這裡就是那個缺陷的修法(docs/settings-model.md §4)──
          //
          // 使用者從哪一份語言設定檔進來(langid = 輸入模式),決定預設
          // 用哪個方案與要不要簡體。設定介面裡釘的方案優先於它 ——
          // 完整的四層優先順序在 common/schema_choice.h。
          //
          // 套用的時機是 session 剛建立時,**不是**每一顆按鍵:
          // 每顆鍵都套的話,使用者用 Ctrl+` 換過的方案會被一直打回去。
          if (!from_spare) {
            engine_->SetSessionLangId(ok.session, langid);
            // ⚠⚠ **這一步必須是同步的。試過非同步,而且量到它更糟。**
            //
            //   rs_select_schema 要載入詞典與 prism。2026-08-09 我把它改成
            //   「丟進佇列就走」,想法是:佇列是 FIFO 的,第一顆按鍵一定排在
            //   它後面,所以順序仍然正確。順序**確實**正確,但成本沒有消失 ——
            //   它只是從 300 毫秒的預算搬進了 **50 毫秒**的預算
            //   (kKeyTimeoutMs,按鍵往返):
            //       11:51:08.048 按鍵 vk=0x4E … 吃掉=1   ← TestKeyDown 說吃
            //       11:51:08.165 按鍵 vk=0x49 … 吃掉=0   ← KeyDown 逾時
            //   TestKeyDown 說吃、KeyDown 說不吃 —— 那顆鍵在真的宿主裡會
            //   **直接消失**,比原本的「打不出中文」更糟。
            //
            //   正確的做法是**根本不要在使用者等著的時候做** ——
            //   也就是上面那個備用 session。這裡是它拿不到時的退路,
            //   而退路本來就該是「和以前一樣」。
            engine_->ApplyChoice(ok.session, chosen_schema, opts);
          }
          // ⚠ 原本這裡還有一趟 engine_->SchemaOfSession(),目的只是讓
          //   note_schema 不要把「我們剛選的方案」誤判成「使用者按了
          //   Ctrl+` 換方案」。那一趟現在被 schema_seeded 取代 ——
          //   看到的第一份快照就是答案,不必再問引擎一次。
          schema_seeded = false;
          // 把剛拿走的那一個補回去(低優先,要等引擎真的閒下來)。
          engine_->RequestSpareSession(langid, chosen_schema, opts);
        }
        // ⚠⚠ **記錄一定要在 send 之前。**
        //
        //   慢到讓用戶端放棄的那幾次,用戶端已經把管道關掉了,於是這裡的
        //   send 會失敗並 goto done —— 記錄若排在 send 後面,
        //   **正好就是失敗的那幾次沒有留下數字**,而 §6e 只會看到成功的那些。
        //   一道只量得到成功案例的關卡等於沒在量(實測:CI run 31313134953
        //   有一個宿主逾時,而 §6e 報「最久 203ms、0 次超過」)。
        //
        //   這一行是使用者回報「有時候不能打中文」時唯一量得到的數字。
        //   300 是用戶端的預算(ipc_client.cc 的 kConnectTimeoutMs)——
        //   超過它就代表那個宿主會 fail-open 成打英文。
        {
          const DWORD t_done = ::GetTickCount();
          // ⚠ 一定要說出這一次是**拿現成的**還是**當場建的**。
          //   備用 session 那條路的價值全在數字上,而「今天剛好很快」與
          //   「真的拿到現成的」在總時間上長得一模一樣 ——
          //   分不出來的話,池子壞掉(永遠拿不到)也不會有人發現。
          Log("[pipe] SESSION_NEW_MS=%lu(建 session %lu ms,%s)%s\n",
              static_cast<unsigned long>(t_done - t_begin),
              static_cast<unsigned long>(t_created - t_begin),
              from_spare ? "備用" : "當場建",
              (t_done - t_begin) >= 300 ? "  ** 超過用戶端 300ms 的預算 **" : "");
        }
        if (!send(EncodeSessionOk(seq, ok))) goto done;
        break;
      }
      case Op::kSessionEnd: {
        uint64_t sid = 0;
        // 同 done: 那一段的理由 —— 不讓離開的宿主佔著連線執行緒等詞典寫完。
        if (DecodeSimple(payload, &seq, &sid)) engine_->EndSessionAsync(sid);
        goto done;  // 單向,而且是連線的終點
      }
      case Op::kKey: {
        KeyReq k;
        if (!DecodeKey(payload, &seq, &k)) goto done;
        // ── Ctrl+空白鍵:中英切換 ────────────────────────────────
        //
        // ⚠ 攔在 librime **之前**,而且判斷來自 common/hotkey_policy.cc
        //   那一份 —— 瘦 DLL 的 OnPreservedKey 用的是同一支。各寫一份就是
        //   兩份真相,而漂移的樣子是「按下去沒有反應」。
        //
        // ⚠ 這裡不需要新的協議操作:那一顆鍵就走既有的 kKey,所以線路
        //   格式一個位元都沒有變 —— 舊 DLL 配新服務、新 DLL 配舊服務
        //   兩個方向都仍然連得起來。
        Result r = IsAsciiToggleHotkey(k.keysym, k.mods)
                       ? engine_->ToggleAsciiMode(k.session)
                       : engine_->ProcessKey(k.session, k.keysym, k.mods);
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
  // ⚠ 不等它做完。rs_session_destroy 要把使用者詞典寫回去,而**下一個**
  //   宿主的 SESSION_NEW 就排在它後面 —— 那正是矩陣裡 8 個宿主逾時的
  //   成因之一。這裡改成非同步之後,這條連線的執行緒不再陪著等;
  //   工作本身仍然在引擎執行緒上、順序不變,詞典一樣寫得回去。
  if (session != 0) engine_->EndSessionAsync(session);
  ui_->Hide();
  ::FlushFileBuffers(pipe);
  ::DisconnectNamedPipe(pipe);
  ::CloseHandle(pipe);
}

}  // namespace rimewin
