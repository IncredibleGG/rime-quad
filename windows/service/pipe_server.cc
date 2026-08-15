#include "pipe_server.h"

#include "settings_window.h"
#include "status_bar.h"

#include <sddl.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>

#include "../common/hotkey_policy.h"
#include "../common/key_deadline.h"
#include "../common/redeploy_flow.h"
#include "../common/schema_choice.h"
#include "../common/status_cells.h"
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
  // 候選窗上的滾輪。⚠ 候選窗比我們晚死(main.cc 的宣告順序),
  //   所以解構子要把這個回呼收回來。
  if (ui_) ui_->SetPageHandler([this](int32_t steps) {
    OnCandidateWheel(steps);
  });
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
  // ⚠ **整份計畫只有一個產生點:common/schema_choice.cc 的 BuildOptionPlan。**
  //
  //   這裡以前是「PlanVariant + ascii_punct + ascii_mode」三段手拉的碼,
  //   而暖機那一條路(main.cc)是另一份 —— 兩份會漂移,而漂移的形狀
  //   最惡劣:順序不同時 SameOptions 逐項比對就不相等,備用池整池報廢,
  //   而**行為完全正確**,只是每一個新視窗都慢半秒。
  //   windows/audit_single_source.sh 規則 1 在原始碼層面守這一條:
  //   main.cc / pipe_server.cc 裡不得出現 PlanVariant 的呼叫。
  //
  //   那份計畫裡的三件事:
  //     1. 簡繁(set_variant 為假 = 使用者說「我自己管」,一個都不碰)
  //     2. 標點(kUnset = followSchema = 整項不出現;設成 false 不是同一件事)
  //     3. 全／半形(同標點:kUnset = 跟著方案 = 整項不出現)
  //     4. 中英(**永遠在**)—— 使用者在那一橫上切成英文之後開的新程式
  //        也要是英文的;而它同時是備用 session 的**計畫**,不放進來的話
  //        備用池裡那個照舊狀態配好的 session 會被判成「計畫相同」
  //        而直接交出去。
  plan.options = BuildOptionPlan(choice, langid, st.Punctuation(), st.Shape(),
                                 engine_ && engine_->AsciiMode());
  return plan;
}

PipeServer::~PipeServer() {
  Stop();
  // ⚠ 引擎手上那個回呼捕捉的是 this。它比我們晚死(main.cc 的區域變數
  //   宣告順序),所以走之前要把它收回來 —— 不然引擎關閉時排乾佇列的
  //   那一下,重建工作會呼叫一個已經不在的物件。
  if (engine_) engine_->SetSessionPlanner(nullptr);
  // 同上:候選窗還活著,而它手上那個 lambda 捕捉的是 this。
  if (ui_) ui_->SetPageHandler(nullptr);
  if (stop_event_) ::CloseHandle(stop_event_);
}

bool PipeServer::ToggleVariantPref() {
  if (!settings_window_ || !engine_) return false;
  // ⚠ 方向從**引擎回讀**算,不是從設定檔算,也不是從畫面反推 ——
  //   與狀態列第一格那一段是同一條理由:拿畫面反推的話,只要畫面曾經
  //   與引擎不一致,再按一次送的就是同一個值,使用者會覺得這顆鍵
  //   只能往一個方向切。
  const Engine::StatusReadback rb = engine_->ReadBackStatus();
  if (!rb.ok) return false;
  const VariantCell now =
      VariantCellFrom((rb.status_flags & kStVariantKnown) != 0,
                      (rb.status_flags & kStSimplified) != 0);
  bool to_simplified = false;
  // ⚠ 判斷只有一份(common/status_cells.cc)。kHidden 時它回 false ——
  //   引擎沒有回報字形,方向是猜的,所以這顆鍵什麼都不做。
  if (!ToggleVariantTarget(now, &to_simplified)) return false;
  settings_window_->SetVariantPref(to_simplified ? VariantPref::kSimplified
                                                 : VariantPref::kTraditional);
  return true;
}

void PipeServer::OnCandidateWheel(int32_t steps) {
  // ⚠ 這一支跑在**候選窗的 UI 執行緒**上。
  uint64_t sid = 0;
  RECT caret{};
  {
    std::lock_guard<std::mutex> lock(ui_mu_);
    sid = ui_session_;
    caret = ui_caret_;
  }
  if (sid == 0 || engine_ == nullptr || steps == 0) return;
  const bool backward = steps < 0;
  int32_t n = steps < 0 ? -steps : steps;
  // ⚠ 一次很用力的撥可能算出幾十頁,而每一頁都要走一趟引擎佇列 ——
  //   而這條執行緒正是畫候選窗的那一條,排隊時它畫不了東西。
  //   撥過頭的人要的是「往回一點」,不是「跑到最後一頁」。
  if (n > 8) n = 8;
  Result r;
  bool moved = false;
  for (int32_t i = 0; i < n; ++i) {
    r = engine_->ChangePage(sid, backward);
    // 已經在第一頁 / 最後一頁 —— librime 說沒動,就不要再問下去,
    // 也不要拿一份「沒動」的快照去重畫(那只是白閃一下)。
    if (!r.handled) break;
    moved = true;
  }
  if (!moved) return;
  // ⚠ 兩個表面必須讀**同一份**快照(ui-design §12.10.1 規範性)。
  //   只更新候選窗的話,那一橫上的簡繁/中英會停在翻頁前那一份 ——
  //   翻頁不改變它們,所以現在看不出來,而那正是它以後會漂的原因。
  if (ui_) ui_->Update(r.snap, caret);
  if (bar_) bar_->OnSnapshot(r.snap);
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
  // ── 那一橫的主要訊號:有沒有宿主在用這個輸入法(§12.10.6)──────
  //
  // ⚠ 為什麼是連線生死而不是焦點:ipc_client.cc 的
  //   `if (!MayEatKey()) return;` —— 焦點訊號在使用者打第一個字之前
  //   根本不送。連線生死沒有這道閘,而且與「哪一個輸入框」無關 ——
  //   同一支程式裡跳輸入框、在都用 LuminaKey 的程式之間 Alt+Tab,
  //   連線數都不會變,那一橫因此不會閃。
  //
  // ⚠ 一定要配對。這一支有很多條 goto done,所以用 RAII 保證離開時
  //   那筆註冊一定消失 —— 漏掉一次,那一橫就會替一個已經走掉的宿主
  //   繼續顯示,而那種缺陷只有人肉試得出來。
  //
  // ⚠ **註冊不是一張票。** 建構的這一刻我們連對面是不是我們自己的 DLL
  //   都還不知道(還沒讀到第一個位元組),所以它 activated=false,
  //   一票都不投;要等 HELLO 才算一筆有效的提案。收斂規則在
  //   common/bar_owner.h,13 個宿主怎麼變成一個答案由純函式測試守著。
  static std::atomic<uint64_t> next_client_id{1};
  const uint64_t client_id = next_client_id.fetch_add(1);
  struct ClientTicket {
    StatusBar* bar;
    uint64_t id;
    ClientTicket(StatusBar* b, uint64_t i) : bar(b), id(i) {
      if (bar) bar->OnClientAttached(id);
    }
    ~ClientTicket() {
      if (bar) bar->OnClientDetached(id);
    }
  } ticket(bar_, client_id);

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

  // ── 這一條連線的量測(#108)──────────────────────────────────
  //
  // ServeClient 全程只有四處 Log,而且全在 SESSION_NEW 與換方案上;
  // 連線建立與關閉一個字都不印 —— 所以服務端這一側**完全看不到抖動**。
  // 使用者回報「間歇打不出中文」時,這幾個數字是唯一能把它變成樣式的東西:
  //
  //   [pipe] 連線 #12 離場 存活=312ms 握手=1 session=7 按鍵=1 逾時=1 …
  //
  // 連續好幾行長這樣 = 重連迴圈;存活很久而逾時一直漲 = 引擎佇列塞住。
  // 兩者要修的地方完全不同。
  const DWORD conn_t0 = ::GetTickCount();
  uint32_t keys_seen = 0;
  uint32_t keys_timed_out = 0;
  uint32_t keys_abandoned = 0;
  // 每條連線最多 20 行 KEY_MS。⚠ 一定要有額度:這是**每鍵**路徑,
  // 沒有額度的話一個卡住的引擎會把 1 MiB 的 service.log 寫滿並從頭來過,
  // 把真正有價值的前幾行捲掉。
  int key_ms_budget = 20;
  // 這條連線的進場 / 離場要不要寫。⚠ **一次決定,兩行共用** ——
  //   額度在中途用完的話會留下一條只有進場沒有離場的連線,而
  //   「存活多久 / 最後一步是什麼」正是這兩行唯一的產出。
  bool log_conn = false;
  {
    int left = conn_log_budget_.load(std::memory_order_relaxed);
    while (left > 0 && !conn_log_budget_.compare_exchange_weak(
                           left, left - 1, std::memory_order_relaxed)) {
    }
    log_conn = left > 0;
  }
  // 這條連線是怎麼結束的。⚠ 一律指向字面值,不必管生命週期。
  const char* last_step = "(還在讀)";
  unsigned last_op = 0;
  if (log_conn)
    Log("[pipe] 連線 #%llu 進場\n",
        static_cast<unsigned long long>(client_id));

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
      if (written == 0) {
        last_step = "送不出去(對面已經不在了)";
        return false;
      }
      sent += written;
    }
    return true;
  };

  auto push_ui = [&](const Snapshot& snap) {
    {
      // 滾輪翻頁要知道翻誰的。⚠ 沒有候選時清成 0 —— 留著的話,候選窗
      //   收起來之後滾輪仍然會去翻一個看不見的 session:使用者在別的
      //   地方捲網頁,我們在背後動他的組字狀態,而畫面上什麼都看不出來。
      std::lock_guard<std::mutex> lock(ui_mu_);
      ui_session_ = snap.items.empty() ? 0 : session;
      ui_caret_ = caret;
    }
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
      if (!fine || got == 0) {
        last_step = got == 0 ? "對面關掉了連線(讀到 0 位元組)" : "讀取失敗";
        goto done;
      }
      if (!reader.Feed(buf, got)) {
        last_step = "分幀失敗";
        goto done;
      }
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

    last_op = static_cast<unsigned>(op);
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
        // ── 那一橫的訊號:這條連線是「哪一條執行緒上的我們」──────
        //
        // ⚠ 這一行是 S1 與 S4 的共同出口。少了它,服務端只知道
        //   「有一條管道 handle 開著」—— 一個沒有產品意義的量:
        //   12 個背景宿主(有的凍結、有的永遠不送 Deactivate)每一條
        //   都算一票,使用者切到微軟拼音之後那一橫照樣自己冒出來。
        //   有了 (pid, tid),bar_owner 才比得出「使用者此刻正在用的
        //   那一條執行緒上啟用中的是不是我們」。
        // ⚠ host_tid 只有 proto >= 3 才在線路上;0 = 報不出來(舊 DLL),
        //   那時 bar_owner 退回去比 pid,精確度差一階但看得見它。
        if (bar_) bar_->OnClientIdentified(client_id, h.host_pid, h.host_tid);
        // ── ⭐ 這個宿主抱著的是**升級前的那一份 DLL** ──────────────
        //
        // 使用者這一次的 tsf.log 證實了一件事:重裝之前就開著的每一個
        // 宿主行程,直到它自己重開之前都還載著舊的 rime_tsf.dll ——
        // 分界乾淨到零例外。Explorer.exe(工作列、檔案總管)幾乎永遠
        // 不會重開,所以這不是罕見情況,是他桌面上一半的程式。
        //
        // ⚠ 這一行是為了讓**下一次**回報查得到,不是給使用者看的 ——
        //   刻意不做成彈窗:一個離線為預設的輸入法不該在使用者打字時
        //   跳出一個他沒有辦法立刻處理的東西。
        // ⚠ 有節流:同一個 pid 只寫一次(連線會斷會重連,而 13 個宿主
        //   ×每次重連一行就是一面牆)。上限 64,滿了就不再記 ——
        //   超過這個量級時那一行的價值已經被稀釋掉了。
        if (h.proto < kProtocolVersion) {
          bool fresh = false;
          {
            std::lock_guard<std::mutex> lk(old_proto_mu_);
            if (old_proto_seen_.size() < 64 &&
                old_proto_seen_.insert(h.host_pid).second)
              fresh = true;
          }
          if (fresh)
            // ⚠ 這一句是**直接對使用者說的**(他會被請去看 service.log),
            //   所以裡面不可以有黑話。舊版寫的是「線路版本」「還載著舊的
            //   rime_tsf.dll」「那一橫」「重啟 Explorer」—— 四個都是我們
            //   自己的講法,而「那一橫」連在文件裡都只是內部叫法。
            //   一個看到這一行的人只需要知道兩件事:哪一個程式、要做什麼。
            //   兩個版本號留在句尾的括號裡:它們是給下一次回報看的,
            //   不必讓使用者讀懂。
            Log("[pipe] 這個程式還在用舊版的輸入法:%s(pid=%lu)。"
                "它在你更新輸入法之前就已經開著了,所以新版對它還沒生效。"
                "把這個程式關掉、再重新打開就好;"
                "如果它是桌面、工作列或檔案總管,那就把電腦重新開機一次。"
                "(它報的版本 %u,現在是 %u)\n",
                h.host_exe.empty() ? "(不明)" : h.host_exe.c_str(),
                static_cast<unsigned long>(h.host_pid),
                static_cast<unsigned>(h.proto),
                static_cast<unsigned>(kProtocolVersion));
        }
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
        //   這裡不能擋著不放行(宿主那一側只有 150 毫秒的預算),但要
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
        // 那一橫回讀中英狀態時要問**這一個**(見 status_bar.cc 的
        // RefreshFromEngine):使用者正在打字的那個宿主的 session。
        if (bar_) bar_->OnClientSession(client_id, ok.session);
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
        last_step = "SESSION_END(宿主自己收工)";
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
        //
        // ── 輕點 Shift 也走這裡(工單 #89)────────────────────────
        //
        // 瘦 DLL 偵測到一次輕點之後送的是「一顆裸的 XK_Shift_L」——
        // 同一個 kKey,線路格式一樣一個位元都沒有變。兩顆鍵分成兩格是
        // 因為**只有輕點 Shift 有開關**(見 hotkey_policy.h)。
        //
        // ⚠ 設定是在**這一刻**讀的,不是開機時讀一次快取起來:
        //   使用者在設定裡按下那顆開關之後,下一次輕點就該照新的走 ——
        //   而一顆「要重開才生效」的開關,他會以為它壞了。
        //
        // ⚠ 但**只有真的可能是輕點 Shift 的那一顆才讀**。上面那句
        //   「只發生在真的輕點了 Shift 的時候…不在每一顆按鍵的路上」
        //   以前與程式碼不符:`settings_->Load()` 是無條件執行的,排在
        //   DecideKeyAction 之前,於是**每一顆按鍵**都在連線執行緒上做一次
        //   CreateFileW + ReadFile + Parse(settings_store.cc 的 Load /
        //   ReadFileUtf8),而且握著一把與 Save / AppendNetLog 共用的
        //   行程層級 mutex —— AppendNetLog 是「整份讀 + 寫暫存檔 +
        //   FlushFileBuffers + MoveFileExW」,它跑的時候每一個宿主的每一顆
        //   按鍵都卡在那把鎖上。防毒軟體對 %APPDATA% 的即時掃描讓這一步的
        //   延遲既真實又間歇。
        //
        // ⚠ 行為一個位元都沒有變:DecideKeyAction 只有
        //   Hotkey::kToggleAsciiModeShiftTap 那一格會用到 shift_tap_enabled
        //   (見 common/hotkey_policy.cc),而那一格正是這裡問到的那一顆。
        //   分類本身是純函式,不碰磁碟。
        DWORD dt_set = 0;
        bool shift_tap_on = true;
        if (ClassifyHotkey(k.keysym, k.mods) ==
            Hotkey::kToggleAsciiModeShiftTap) {
          const DWORD t_set = ::GetTickCount();
          shift_tap_on = settings_ ? settings_->Load().ShiftTapToggle() : true;
          dt_set = ::GetTickCount() - t_set;
        }
        const KeyAction action =
            DecideKeyAction(k.keysym, k.mods, shift_tap_on);
        if (action == KeyAction::kIgnore) {
          // 使用者把輕點 Shift 關掉了。**什麼都不做** ——
          //
          // ⚠ 而「什麼都不做」包含**不碰 UI**。這裡不可以走 push_ui:
          //   那一份空快照會把候選窗與那一橫當成「沒有候選了」而收掉,
          //   於是使用者組字到一半按了一下 Shift,畫面上的候選就消失了。
          //   一個被關掉的功能不可以有任何看得見的痕跡。
          //
          // ⚠ 也不可以順手交給 librime(它自己也認得 Shift_L)——
          //   理由見 common/hotkey_policy.h 的 KeyAction::kIgnore。
          Result r;
          r.handled = false;
          if (!send(EncodeResult(seq, r))) goto done;
          break;
        }
        // ⚠ 這一格要留在 if/else 外面:下面那一段 UI 的去留由它決定,
        //   而算出它的地方在 else 分支裡面。
        bool key_timed_out = false;
        Result r;
        if (action == KeyAction::kToggleVariant) {
          // 簡繁快捷鍵(Ctrl+Shift+F,G76)。
          //
          // ⚠ 回的是**當下這一份**快照,不是空的。DLL 收到 handled=1
          //   之後會把它套進文件(tsf/text_service.cc 的 SendAsciiToggle
          //   那一整段 ⚠),而空快照的意思是「沒有組字」——
          //   使用者打到一半的那一段會當場消失。
          //   簡繁本身是非同步套上去的(設定視窗那條執行緒),所以此刻
          //   文件本來就不該有任何變化,拿當下這一份正是對的。
          if (ToggleVariantPref()) {
            r = engine_->CurrentResult(k.session);
            r.handled = true;
          }
          // ToggleVariantPref 回 false = 什麼都沒做 → r.handled 維持 false
          // → DLL 把那顆鍵交回宿主。**不可以**假裝切了。
        } else if (action == KeyAction::kToggleAsciiMode) {
          r = engine_->ToggleAsciiMode(k.session);
        } else {
          // ── 把「間歇打不出中文」變成一個數字(#108)────────────
          //
          // ⚠ 四個欄位缺一不可,而且它們**分得開第一名與第二名**:
          //   · 引擎佇列塞住 → 佇列前面是「收 session」/「套用方案與選項」,
          //     已跑 / 最舊等待很大,而 dt_set 接近 0。
          //   · 設定檔 I/O   → dt_set 很大而佇列那三個接近 0。
          //   沒有分項的話,兩者在總時間上長得一模一樣 —— 這個專案已經
          //   因為「一道只量得到成功案例的關卡」吃過虧,不要再造一個。
          //
          // 三個佇列欄位早就存在(work_queue.h),只是到現在為止唯一的
          // 消費點是設定視窗(settings_window.cc),連線路徑上沒有人讀。
          Engine::KeyWait kw;
          const DWORD t0 = ::GetTickCount();
          r = engine_->ProcessKey(k.session, k.keysym, k.mods, &kw);
          const DWORD dt = ::GetTickCount() - t0;
          ++keys_seen;
          if (kw.timed_out) ++keys_timed_out;
          if (kw.abandoned) ++keys_abandoned;
          key_timed_out = kw.timed_out;
          // 門檻與兩個上限是一組的,所以同樣住在 common/key_deadline.h
          // (kKeySlowLogMs)。它刻意低於服務端的上限:只記已經逾時的那些
          // 會看不到「快要逾時」那一段,而那一段才是樣式開始的地方。
          if ((dt >= static_cast<DWORD>(kKeySlowLogMs) || kw.timed_out) &&
              key_ms_budget > 0) {
            --key_ms_budget;
            Log("[pipe] KEY_MS=%lu(設定讀取=%lums,佇列前面是「%s」,"
                "已跑 %lldms,最舊等待 %lldms,逾時=%d,本體作廢=%d)%s\n",
                static_cast<unsigned long>(dt),
                static_cast<unsigned long>(dt_set),
                engine_->CurrentJobLabel().c_str(),
                static_cast<long long>(engine_->StalledMs()),
                static_cast<long long>(engine_->OldestWaitingMs()),
                kw.timed_out ? 1 : 0, kw.abandoned ? 1 : 0,
                key_ms_budget == 0 ? "  (KEY_MS 額度用完,這條連線不再記)"
                                   : "");
          }
        }
        // ── ⚠ 逾時的那一份不可以碰 UI(#93/#108 的覆核抓到的)──────
        //
        //   逾時代表本體多半一步都沒跑(作廢成功),引擎那邊的組字狀態
        //   原封不動。那一份 Result 是佔位,不是現況 —— 餵進去的話:
        //   候選是空的 → push_ui 會把候選窗收掉,而使用者正組字到一半;
        //   旗標也會被那一橫讀走 → 一個健康的引擎自稱「正在準備字詞」。
        //   兩件事使用者都當場看得到,而且畫面從此與引擎分岔。
        //
        //   這與上面「使用者把輕點 Shift 關掉了」那一格是同一條規矩:
        //   什麼都不做包含**不碰 UI**。判準在 common/key_deadline.h,
        //   在 Ubuntu 上驗得到(tests/test_key_deadline.cc)。
        //
        // ⚠ 回給宿主的 r 照送不誤 —— DLL 要靠 handled=false 才知道
        //   這顆鍵要自己收尾(tsf/text_service.cc)。不送等於那顆鍵消失。
        if (DecideKeyUiAction(key_timed_out) == KeyUiAction::kUpdateUi) {
          note_schema(r.snap);
          push_ui(r.snap);
        }
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
          // ⚠ **這一則不再餵那一橫,而那是刻意的。**
          //
          //   它以前寫進一個**單一全域** any_focused_,由「最後說話的
          //   那個宿主」蓋掉前一個 —— 而每一個宿主都會送這一則。
          //   使用者在記事本打字時,19:31:47 的 Snipaste、19:31:52 的
          //   rustdesk、19:32:17 的 conhost 一進場就把前景那一份蓋掉,
          //   那一橫因此時有時無(他實機回報的 S2)。
          //
          //   焦點現在是**一個位置**,不是一個布林:誰是前景由服務端
          //   自己問 OS(status_bar.cc 的 ReadForegroundOwner),再跟
          //   每一條連線報上來的 (pid, tid) 比。這一則仍然要收 ——
          //   候選窗要靠它收起來 —— 但它不再是可見性的輸入。
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
      // ⭐ #111 的新事實來源。單向,不回覆。
      //
      // ⚠ 那個 u64 是布林(0 = 這條執行緒上啟用中的不再是我們),不是
      //   session —— 它只是沿用 EncodeSimple 既有的形狀,不必動編碼器。
      // ⚠ :582 的 `if (!authed && op != Op::kHello)` 已經保證這則訊息一定
      //   在 HELLO 之後才被接受,所以 client_id 那一筆一定已經有 host_tid。
      case Op::kProfileState: {
        uint64_t v = 0;
        if (!DecodeSimple(payload, &seq, &v)) goto done;
        if (bar_) bar_->OnClientProfileState(client_id, v != 0);
        break;  // 單向,不回覆
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
        // ⚠ 換方案之前先把設定檔裡的簡繁偏好重讀一次。
        //
        //   Engine::SelectAndApply 換完方案要重套簡繁(librime 每次載入
        //   方案都會把 switches 重設回方案宣告的值),而它用的是
        //   Engine::variant_pref_ —— 那是**設定的複本**,只有服務啟動時
        //   與設定視窗改過時才更新(engine.h 自己寫著「真相在設定檔」)。
        //
        //   設定檔在服務跑著的時候被別人改掉,這一趟就會拿舊偏好去洗掉
        //   使用者剛選的簡繁。真的走得到:設定視窗有一顆「用記事本開啟
        //   設定檔」,而 verify_installer.sh §6g 案例二也正是這條路 ——
        //   先寫檔、再連上已經在跑的服務、再換方案,上屏變回繁體。
        //
        //   SESSION_NEW 那一條路每一次都重讀設定(見上面的 st/choice/opts),
        //   這裡照做。SetVariantPref 與 SelectSchema 都走同步的 Post,
        //   所以順序是保證的。這條 op 本來就要載入詞典與 prism,
        //   多一次讀設定檔不會改變它的量級。
        //
        // ⚠ **判斷本身不在這裡**,在 common/schema_choice.cc 的
        //   PickVariantPrefForSchemaSwitch —— windows/service/ 在 Ubuntu 上
        //   編不起來,寫在這裡的判斷等於只有 Windows CI 驗得到,而這一格
        //   唯一的守門(verify_installer.sh §6g 案例二)正是那種東西。
        //   抽出去之後 tests/test_schema_choice.cc 驗得到它。
        //   而「這裡真的呼叫了它」由 audit_single_source.sh 規則 3 守。
        //
        // ⚠ 兩個參數的型別**刻意不同**(OnDiskPref / EngineCopyPref)。
        //   同型別的舊簽章對調之後就是 648c02c 的原缺陷,而編譯器、純函式
        //   測試、規則 3 全都看不見 —— 理由整段在 schema_choice.h 的
        //   OnDiskPref 檔頭。這裡**不要**把它們拆成區域變數再傳:
        //   規則 5 驗的是這一行實際的引數(設定檔那一格必須真的走
        //   settings_->Load(),引擎那一格必須真的是 VariantPrefCopy()),
        //   而它只看得到寫在呼叫裡的東西。
        const VariantPrefPick pick = PickVariantPrefForSchemaSwitch(
            settings_ ? OnDiskPref::FromSettingsFile(
                            settings_->Load().SchemaPref())
                      : OnDiskPref::Unreadable(),
            EngineCopyPref(engine_->VariantPrefCopy()));
        engine_->SetVariantPref(pick.use);
        if (pick.engine_copy_was_stale) {
          // 只在真的過期時說話。這一行是「設定檔在服務跑著的時候被改掉」
          // 唯一留得下痕跡的地方 —— 沒有它,查這個缺陷時看到的只有
          // 「換方案之後簡繁不對」,而看不到偏好是什麼時候漂走的。
          Log("[pipe] 換方案:引擎手上的簡繁偏好已經過期,改用設定檔那一份\n");
        }
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
  // ── 這一行是抖動在 service.log 裡的樣式(#108)──
  //
  // 存活=300ms、按鍵=1、逾時=1 連續好幾行 = 重連迴圈;
  // 存活很久而逾時一直漲 = 引擎佇列塞住。兩者要修的地方完全不同,
  // 而在這一行之前,服務端這一側兩種都看不到。
  if (log_conn)
    Log("[pipe] 連線 #%llu 離場 存活=%lums 握手=%d session=%llu "
        "按鍵=%u 逾時=%u 本體作廢=%u 最後一步=%s(op=%u)%s\n",
        static_cast<unsigned long long>(client_id),
        static_cast<unsigned long>(::GetTickCount() - conn_t0),
        authed ? 1 : 0, static_cast<unsigned long long>(session),
        static_cast<unsigned>(keys_seen), static_cast<unsigned>(keys_timed_out),
        static_cast<unsigned>(keys_abandoned), last_step, last_op,
        conn_log_budget_.load(std::memory_order_relaxed) == 0
            ? "  (連線進出的額度用完,之後的連線不再記)"
            : "");
  // ⚠ 不等它做完。rs_session_destroy 要把使用者詞典寫回去,而**下一個**
  //   宿主的 SESSION_NEW 就排在它後面 —— 那正是矩陣裡 8 個宿主逾時的
  //   成因之一。這裡改成非同步之後,這條連線的執行緒不再陪著等;
  //   工作本身仍然在引擎執行緒上、順序不變,詞典一樣寫得回去。
  if (session != 0) engine_->EndSessionAsync(session);
  {
    // ⚠ 這條連線走了,滾輪就不可以再指著它的 session。少了這一段,
    //   宿主關掉之後在候選窗**最後出現的那個位置**上滾輪,會對一個
    //   已經被 EndSession 的 id 呼叫 rs_change_page。
    std::lock_guard<std::mutex> lock(ui_mu_);
    if (ui_session_ == session) ui_session_ = 0;
  }
  ui_->Hide();
  ::FlushFileBuffers(pipe);
  ::DisconnectNamedPipe(pipe);
  ::CloseHandle(pipe);
}

}  // namespace rimewin
