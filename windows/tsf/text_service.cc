#include "text_service.h"

#include "../common/ui_strings.h"

#include <functional>
#include <new>
#include <vector>

#include "../common/ime_policy.h"
#include "../common/hotkey_policy.h"
#include "../common/key_eat_policy.h"
#include "../common/protocol.h"
#include "rime_shell.h"
#include "../winshared/winutil.h"
#include "guids.h"
#include "lang_bar.h"
#include "trace.h"

// DllMain 存下來的模組控制代碼。用來找到與 DLL 同目錄的 rime_service.exe。
extern HMODULE g_rime_module;
extern LONG g_rime_dll_refs;

namespace rimewin {
namespace {

// ⚠ C++ 例外穿過 COM 邊界是未定義行為,而在這裡「未定義」的實際長相
//   就是宿主進程崩潰 —— 而宿主可能是使用者正在編輯的文件。
//   每一個 COM 介面方法都必須擋一次。
#define RIME_GUARD_BEGIN try {
#define RIME_GUARD_END_HR                                                    \
  }                                                                          \
  catch (...) { return E_FAIL; }

// 跑一段同步的 edit session。
//
// final 不是裝飾:COM 介面沒有虛擬解構子,而這裡是 `delete this`。
// 標成 final 才能讓編譯器確定 `this` 就是最終型別 —— 否則某天有人繼承它,
// 就會變成從基底指標刪除衍生物件的未定義行為。
class FnEditSession final : public ITfEditSession {
 public:
  using Fn = std::function<HRESULT(TfEditCookie)>;
  explicit FnEditSession(Fn fn) : fn_(std::move(fn)) {}

  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession))
      *ppv = static_cast<ITfEditSession*>(this);
    if (!*ppv) return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(::InterlockedIncrement(&ref_));
  }
  STDMETHODIMP_(ULONG) Release() override {
    const LONG n = ::InterlockedDecrement(&ref_);
    if (n == 0) delete this;
    return static_cast<ULONG>(n);
  }
  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    try {
      return fn_(ec);
    } catch (...) {
      return E_FAIL;
    }
  }

 private:
  ~FnEditSession() = default;
  LONG ref_ = 1;
  Fn fn_;
};

// 由 TSF 給的 wParam / lParam 加一次 GetKeyboardState 組出 KeyEvent。
// OnTestKeyDown 與 OnKeyDown 必須用**同一份**組法,否則兩者對「這顆鍵映不映得出
// keysym」的判斷會不一致 —— 症狀是某些鍵在特定修飾鍵下悄悄失效。
KeyEvent BuildKeyEvent(WPARAM w, LPARAM l, bool key_up) {
  BYTE state[256] = {0};
  ::GetKeyboardState(state);
  KeyEvent e;
  e.vk = static_cast<uint32_t>(w);
  e.scan_code = static_cast<uint32_t>((l >> 16) & 0xFF);
  e.extended = (l & (1 << 24)) != 0;
  e.key_up = key_up;
  e.shift = (state[VK_SHIFT] & 0x80) != 0;
  e.ctrl = (state[VK_CONTROL] & 0x80) != 0;
  e.alt = (state[VK_MENU] & 0x80) != 0;
  e.win = ((state[VK_LWIN] | state[VK_RWIN]) & 0x80) != 0;
  e.caps_lock = (state[VK_CAPITAL] & 0x01) != 0;
  e.num_lock = (state[VK_NUMLOCK] & 0x01) != 0;
  e.right_alt = (state[VK_RMENU] & 0x80) != 0;
  return e;
}

// ── 背景把服務叫起來 ──────────────────────────────────────────────
//
// ⚠ 這條路徑修的是使用者實際回報的「**沒有任何 UI**」。
//
// 系統匣圖示與設定視窗都住在**服務進程**裡,而在這之前,服務唯一的啟動
// 時機是「第一顆按鍵走到 EnsureReady()」。於是只要按鍵那條路上任何一段
// 斷掉(例如佈局問不出 keysym,見 win32_oracle.h 檔頭),服務就**永遠不會
// 被啟動** —— 使用者看到的是「打不出字」**加上**「沒有任何 UI」,
// 兩個看起來無關的症狀,同一個根因。
//
// 把啟動搬到 ActivateEx(使用者切到這個輸入法的當下)之後:
//   · 系統匣圖示與設定視窗在切過去幾秒內就會出現,不必先打字;
//   · 首次部署(要編譯詞庫,好幾分鐘)提早開始,而不是等到使用者
//     第一次按鍵才開始 —— 那時他會以為輸入法壞了;
//   · 而「打不出字」如果還在,就**只剩**按鍵那條路可以查了。
//     把兩個症狀解耦,比同時修兩件事重要。
//
// ⚠ 一定要在**背景執行緒**上做。ActivateEx 跑在宿主的 UI 執行緒上,
//   在那裡 CreateProcess(甚至只是開一次登錄檔)都會讓切換輸入法卡一下,
//   而那是使用者每天做很多次的動作。
//
// ⚠ 執行緒活著的期間必須抓住 DLL 的參考計數(g_rime_dll_refs)。
//   少了它,宿主可能在 DllCanUnloadNow 回 S_OK 之後把 DLL 卸載掉,
//   而執行緒還在那段已經不存在的程式碼裡 —— 症狀是「切換輸入法時偶爾當掉」。
DWORD WINAPI ServiceStarterThread(LPVOID param) {
  std::wstring* path = static_cast<std::wstring*>(param);
  if (path) {
    if (!ServiceIsRunning()) {
      Trace("ActivateEx:服務沒在跑,背景啟動");
      LaunchService(*path);
    } else {
      Trace("ActivateEx:服務已經在跑");
    }
    delete path;
  }
  ::InterlockedDecrement(&g_rime_dll_refs);
  return 0;
}

// 每個宿主進程只做一次。
//
// 不是節流,是**正確性**:一個宿主裡可以有很多個 TextService 實例
// (每一條有輸入焦點的執行緒一個),而它們會在使用者每次切回這個輸入法時
// 各自 Activate 一遍。沒有這個旗標的話,瀏覽器切幾次分頁就會排出幾十條
// 執行緒,每一條都去問一次「服務在不在」。
LONG g_service_start_once = 0;

// 這個宿主可不可以啟動服務。判準與理由見 common/elevation_policy.h。
//
// ⚠ 進程的一生中不會變,所以問一次就好 —— 但**不要**在 DllMain 裡問
//   (那裡是載入器鎖)。第一次呼叫發生在 ActivateEx 或語言列重畫,
//   兩者都遠在載入器鎖之外。
HostElevation HostElevationOnce() {
  static const HostElevation e = ClassifyHostElevation();
  return e;
}

void StartServiceInBackground(const std::wstring& service_path) {
  if (service_path.empty()) return;
  if (::InterlockedCompareExchange(&g_service_start_once, 1, 0) != 0) return;
  // ⚠ 這裡以前是 `if (IsProcessElevated())` —— 一句「提權就不啟動」。
  //
  //   理由是對的(提權的服務會把使用者詞庫的擁有者換掉),但涵蓋過頭:
  //   內建 Administrator 帳號與關掉 UAC 的機器上,**每一個進程都是提權的**,
  //   於是服務永遠不會被啟動 —— 沒有系統匣圖示、沒有設定視窗、打不出字,
  //   三個症狀一個原因,而且沒有任何錯誤訊息。實測回報就是這一種。
  //
  //   現在問的是正確的問題:「這個使用者在這個工作階段裡,還有沒有另一份
  //   非提權的身分會被我們弄壞?」見 common/elevation_policy.h。
  const HostElevation elev = HostElevationOnce();
  if (!MayStartUserService(elev)) {
    Trace("ActivateEx:不啟動服務(刻意):%s / %s", HostElevationTag(elev),
          HostElevationZh(elev));
    return;
  }
  std::wstring* copy = new (std::nothrow) std::wstring(service_path);
  if (!copy) return;
  ::InterlockedIncrement(&g_rime_dll_refs);
  HANDLE th = ::CreateThread(nullptr, 0, &ServiceStarterThread, copy, 0, nullptr);
  if (!th) {
    ::InterlockedDecrement(&g_rime_dll_refs);
    delete copy;
    return;
  }
  ::CloseHandle(th);
}

HRESULT RunSyncSession(ITfContext* ctx, TfClientId id, FnEditSession::Fn fn) {
  if (!ctx) return E_FAIL;
  FnEditSession* s = new (std::nothrow) FnEditSession(std::move(fn));
  if (!s) return E_OUTOFMEMORY;
  HRESULT session_hr = S_OK;
  // TF_ES_SYNC:按鍵處理必須在這一趟裡做完,不能等非同步的 session ——
  // 非同步回來時那顆按鍵早就被宿主處理過了,順序全亂。
  const HRESULT hr = ctx->RequestEditSession(
      id, s, TF_ES_SYNC | TF_ES_READWRITE, &session_hr);
  s->Release();
  if (FAILED(hr)) return hr;
  return session_hr;
}

}  // namespace

// ── 那一橫的「我在用」連線(工單 #82 / S1 / S4)──────────────────────
//
// ══ 這條連線到底在回答什麼 ═════════════════════════════════════════
//
// 那一橫的判準只有一句:**使用者此刻輸入焦點所在的那一條宿主執行緒上,
// 啟用中的 TSF profile 是不是我們。**(收斂規則在 common/bar_owner.h。)
//
// 而那句話的前半段 —— 「這條執行緒上啟用中的是不是我們」—— **只有宿主
// 自己知道**:服務端問不到別的進程的 TSF 狀態(ITfInputProcessorProfileMgr
// 是 per-thread、per-process 的),GetKeyboardLayout 對 TSF TIP 給的是合成
// 的 HKL,跨進程對不回我們的 CLSID。所以宿主必須自己說,而它說的方式
// 就是**這條連線的生死**:啟用時開、不再是我們時關。
//
// 後半段(誰是前景)由服務端自己問 OS —— 不接受宿主宣稱,因為兩個宿主
// 同時宣稱「我是前景」就退化回「有沒有人」那個舊判準。
//
// ══ 這個檔案裡的訊號一直是不對稱的 ═════════════════════════════════
//
//   切走輸入法            Deactivate()  → ipc_.Close()        關掉
//   在兩份 profile 之間切  OnActivated() → ipc_.SetProfile()   也關掉
//   切回來                ActivateEx()  → 什麼都不連          不開
//
// 唯一能開出一條連線的是 IpcClient::EnsureReady(),而它全樹只有三個
// 呼叫點,**三個都在按鍵路徑上**。結果:使用者切回輸入法之後,
// 那一橫要等他按下第一顆鍵才出現 —— 而他回報的是
// 「切換了一下輸入法,狀態欄整個不見了,再也不出現」(S1)。
//
// ⚠ 而**開了不關**是另一個方向,一樣真實:使用者切到微軟拼音之後那一橫
//   自己冒出來(S4)。所以四個邊一個都不能少,見 EnsurePresence /
//   ClosePresence 的兩組呼叫點,以及 audit_single_source.sh 規則 6。
//
// ══ 為什麼不是把 EnsureReady() 搬進 ActivateEx ═════════════════════
//
// 見 ActivateEx 裡那一段:開管道與握手要在**宿主的 UI 執行緒**上等,
// 那會讓切換輸入法卡住。那個理由今天仍然成立,這一輪一個字都沒有動它。
//
// ══ 這裡的做法(四條出路裡的丁)═══════════════════════════════════
//
// 一條**專用的、只為了在場**的連線:自己的背景執行緒、自己的管道 handle,
// 從 ActivateEx 開到 Deactivate 才關。
//
//   · 按鍵那條熱路徑一個字都不用動。ipc_ 仍然只被宿主的 UI 執行緒碰,
//     **不需要鎖**(甲)、**不需要交棒**(乙)。#93 正在講按鍵的等待
//     沒有上限,往那條路上再加一把鎖是往反方向走。
//   · 連線**一直開著**,不是短連一次就關(丙)—— 丙只會讓 clients_
//     走 0→1→0,三秒之後那一橫又不見了。
//
// ══ 動手之前確認過的兩件事 ═════════════════════════════════════════
//
// 1. **同一個宿主可以有多條並行連線。** service/pipe_server.cc 的
//    CreateInstance() 用 PIPE_UNLIMITED_INSTANCES,ListenLoop 每接到一條
//    就 emplace 一條 ServeClient 執行緒、然後立刻再建一個實例;
//    整支伺服器沒有任何一處以宿主進程做識別或去重。
//    打字時同一條執行緒上會有兩條連線(在場一條 + 按鍵一條),而兩條
//    報的 (pid, tid) 一樣 —— common/bar_owner.h 的收斂規則會挑
//    **有 session 的那一條**,因為那一橫要回讀的就是它。
//
// 2. **這條連線要握手,但不建 session。**
//
//    ⚠ 上一版這裡寫的是「不必握手」,而那是這一輪要改的那一句。
//      不握手 = 服務端只知道「有一條管道 handle 開著」,而那是一個
//      沒有產品意義的量:12 個背景宿主每一條都算一票,使用者切到
//      微軟拼音之後那一橫照樣自己冒出來(他實機回報過)。
//      服務端要答的是「**使用者此刻正在用的那一條執行緒**上啟用中的
//      是不是我們」,而那需要這條連線報得出自己是誰 ——
//      host_pid 與**啟用我們的那一條 TSF 執行緒**的 tid(線路 v3)。
//
//    查證過成本:HELLO **不進引擎佇列**(pipe_server.cc 的 Op::kHello
//    分支只呼叫 rs_abi_version(),沒有 Post),真正貴的是 SESSION_NEW
//    的 442~753 毫秒。所以:補 HELLO,不補 SESSION_NEW。而且整段仍然
//    跑在這條背景執行緒上 —— ActivateEx 一個位元組的 I/O 都不等。
//
// ⚠ **這條路不可以啟動服務。** 啟動走 StartServiceInBackground(),
//   提權宿主那道閘(common/elevation_policy.h 的 MayStartUserService)
//   在那裡。這裡從頭到尾只有 CreateFileW:服務不在就連不上,連不上就等。
//
// ⚠ 生命週期:Deactivate() **不等**這條執行緒(它跑在宿主的 UI 執行緒上)。
//   物件自己數參考 —— Stop() 設事件並放掉一份,執行緒退出時放掉另一份,
//   後放的那個負責釋放自己。整段期間抓著 g_rime_dll_refs,
//   理由與上面 ServiceStarterThread 那一段完全相同。
//
// ⚠ 因此 Stop() 回來之後,那條連線還可能活著幾毫秒(執行緒要先醒過來)。
//   那沒有問題:隱藏本來就有 3000 毫秒的遲滯(common/bar_visibility.h),
//   而 CancelIoEx 讓「醒過來」是立刻的事。
class PresenceLink {
 public:
  // 回 nullptr = 起不來。呼叫端不必處理,那只是退回這一版之前的行為。
  //
  // ⚠ host_tid 是**呼叫端那條執行緒**的 id(ActivateEx / OnActivated 都
  //   跑在啟用我們的那條 TSF 執行緒上),不是這條背景執行緒的 id。
  //   拿錯的話服務端會拿一條沒有人在上面打字的執行緒去跟前景比。
  static PresenceLink* Start(uint32_t host_tid) {
    PresenceLink* p = new (std::nothrow) PresenceLink();
    if (!p) return nullptr;
    p->host_tid_ = host_tid;
    p->stop_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!p->stop_) {
      delete p;
      return nullptr;
    }
    p->refs_ = 2;  // 一份給呼叫端,一份給執行緒
    ::InterlockedIncrement(&g_rime_dll_refs);
    HANDLE th = ::CreateThread(nullptr, 0, &PresenceLink::ThreadMain, p, 0,
                               nullptr);
    if (!th) {
      ::InterlockedDecrement(&g_rime_dll_refs);
      p->refs_ = 1;
      p->Release();
      return nullptr;
    }
    ::CloseHandle(th);
    return p;
  }

  // ⚠ **不等執行緒。** 這是在宿主的 UI 執行緒上被呼叫的。
  void Stop() {
    ::SetEvent(stop_);
    Release();
  }

  // ⭐ 「這條執行緒上啟用中的**不再是我們**」——在關掉這條連線之前,
  //    先把這句話說出去(#111)。
  //
  // ⚠ 它**不 Release()**:參考計數仍然由緊接著的 ClosePresence() → Stop()
  //   負責。這裡設 stop_ 只是為了讓背景執行緒立刻從 WaitUntilBroken 那個
  //   INFINITE 的讀裡醒過來 —— 不必為這件事多一個事件、多一條要對齊的
  //   時序。真正的送出在**擁有那個 pipe handle 的那條執行緒**上(Run)。
  //
  // ⚠ 也在宿主的 UI 執行緒上被呼叫,所以一個位元組都不在這裡寫。
  void NoteYield() {
    ::InterlockedExchange(&yielded_, 1);
    ::SetEvent(stop_);
  }

 private:
  // 這一圈沒有成果時的兩段式退避。
  //
  // 前 30 次一秒一次:使用者剛切過來,服務可能正被 StartServiceInBackground
  // 拉起來,早一秒連上就是早一秒看得到那一橫。之後放慢到五秒 ——
  // 首次部署要編詞庫,那是好幾分鐘,這條執行緒在那段期間應該安靜。
  // ⚠ 沒有上限:服務被更新程式重啟過之後也要自己接回來(工單 #73 的形狀)。
  //
  // ⚠ 「這一圈沒有成果」有**三種**,而只有第一種是顯而易見的:
  //     1. CreateFileW 就連不上(服務不在)。
  //     2. 連上了,但**握手沒成功**。服務在、管道在,只是它此刻答不出
  //        HELLO_OK(正在部署、佇列被佔住、或它是舊版而我們還沒降到
  //        它認得的線路版本)。⚠ 這一種以前固定等 1000 毫秒重連,
  //        **永遠進不了五秒的慢車道** —— 每個宿主每秒開一條新連線、
  //        服務端每秒起一條新的 ServeClient 執行緒,而使用者機器上有
  //        13 個宿主。一個以「離線為預設、經得起審計」為定位的產品,
  //        它的背景行為要解釋得出來。
  //     3. 握完手,但連線**立刻**就斷了(服務正在收工 / 正被更新程式
  //        換掉)。那一圈是零等待的 —— 比第 2 種更快。
  //   所以退避的歸零點是「**這一圈真的當了一段時間的在場連線**」,
  //   不是「CreateFileW 回來了」。
  static const DWORD kRetryFastMs = 1000;
  static const DWORD kRetrySlowMs = 5000;
  static const int kFastTries = 30;

  PresenceLink() = default;
  ~PresenceLink() {
    if (stop_) ::CloseHandle(stop_);
  }

  PresenceLink(const PresenceLink&) = delete;
  PresenceLink& operator=(const PresenceLink&) = delete;

  void Release() {
    if (::InterlockedDecrement(&refs_) == 0) delete this;
  }

  static DWORD WINAPI ThreadMain(LPVOID param) {
    PresenceLink* self = static_cast<PresenceLink*>(param);
    self->Run();
    self->Release();
    ::InterlockedDecrement(&g_rime_dll_refs);
    return 0;
  }

  // 這一圈沒有成果 —— 等一下再來。回 false = 被叫停,外層要 break。
  //
  // ⚠ 三條路徑共用這一支就是重點:以前只有「連不上」那一條會慢下來。
  bool Backoff(int* misses) {
    ++*misses;
    const DWORD pause = *misses <= kFastTries ? kRetryFastMs : kRetrySlowMs;
    return ::WaitForSingleObject(stop_, pause) != WAIT_OBJECT_0;
  }

  void Run() {
    const std::wstring name = RimePipeName();
    // overlapped 的事件整條執行緒共用一個:一次只掛一個 I/O。
    HANDLE io = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    int misses = 0;
    int traced = 0;        // 「連不上」寫了幾行(連上就歸零)
    int linked_traced = 0;  // 「已連上」寫了幾行(握手成功才歸零)
    while (::WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
      // ⚠ 只連,不啟動 —— 見上面那一段 ⚠。
      HANDLE pipe = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED, nullptr);
      if (pipe == INVALID_HANDLE_VALUE) {
        // ⚠ 服務不在 = 它可能正被更新程式換掉(工單 #73 的形狀)。
        //   回到最新的線路版本重試 —— 否則一次降版會跟著這個宿主
        //   進程一輩子,而使用者永遠不知道自己少了 per-thread 的精確度。
        proto_ = kProtocolVersion;
        // ⚠ 有預算。連不上是常態(服務還在部署),而每一行都是磁碟寫入。
        if (traced < 2) {
          ++traced;
          Trace("在場連線:連不上(err=%lu)—— 等服務出現",
                static_cast<unsigned long>(::GetLastError()));
        }
        if (!Backoff(&misses)) break;
        continue;
      }
      traced = 0;
      // ⚠ 這一行是 CI 唯一看得到「在場連線真的建立了」的地方之一
      //   (另一個是 tests/tsf_host_main.cc 的 --watch-presence,
      //   它從**服務端**數)。⚠ 它也有預算:握手一直失敗的話,這個
      //   迴圈每一圈都會走到這裡,而沒有預算的話它就是一行一行的磁碟寫入。
      bool traced_link = false;
      if (linked_traced < 2) {
        ++linked_traced;
        traced_link = true;
        Trace("在場連線:已連上 —— 那一橫從現在起看得到這個宿主");
      }
      // ⚠ 握手失敗有兩種,而它們要做的事**不一樣**:
      //     · 對面不認得我們宣告的版本(舊服務 + 新 DLL):它的
      //       DecodeHello 在「剛好用完」那一關整則丟掉,然後關掉連線。
      //       → 降一版重試。降到最後仍然報得出 pid,只是報不出 tid,
      //         而 bar_owner.h 對報不出 tid 的用戶端退回去比 pid。
      //     · 只是慢(服務正在部署,那條執行緒被佔住)→ **不可以降版**。
      //       降了就永久失去 tid,而那是一個沒有人查得出來的降級。
      const DWORD linked_at = ::GetTickCount();
      HandshakeResult hs = SendHello(pipe, io);
      if (hs != HandshakeResult::kOk) {
        if (hs == HandshakeResult::kRejected && proto_ > kMinProtocolVersion)
          --proto_;
        ::CloseHandle(pipe);
        // ⚠ **這裡以前是固定的 kRetryFastMs。** 服務在、而握手一直不成
        //   的時候,那等於每個宿主每秒開一條新連線、服務端每秒起一條新
        //   執行緒,×13,永遠 —— 而且永遠進不了慢車道。
        if (!Backoff(&misses)) break;
        continue;
      }
      const bool stopped = WaitUntilBroken(pipe, io);
      // ⭐ 被 NoteYield 叫醒的話,**先把那句話說出去再走**。
      //
      // ⚠ 順序不可以反:送出必須在關 handle 之前,而且必須在擁有這個
      //   handle 的這條執行緒上。宿主的 UI 執行緒不碰管道 —— 每一次
      //   Win+空白鍵切輸入法都在那裡等一次 I/O 是不能接受的。
      // ⚠ 送不出去(管道剛好斷了、宿主同一瞬間被砍掉)的後果是:服務端
      //   只看到連線消失 → kHold → 維持現狀。那是「那一橫留著」,
      //   不是退回 #111。
      if (stopped && ::InterlockedCompareExchange(&yielded_, 0, 0) != 0)
        SendYield(pipe);
      // ⚠ 這一行就是 #82 的「切走之後那一橫必須消失」:關掉 →
      //   服務端那條 ServeClient 讀到 0 位元組 → ClientTicket 解構
      //   → OnClientDetached → 那筆註冊消失。
      ::CloseHandle(pipe);
      // ⚠ 與上面那一行**成對**:握完手立刻被關掉的迴圈裡,兩行都要一起
      //   閉嘴,否則「已連上」有預算而「已關閉」沒有,磁碟照樣一直寫。
      if (traced_link) Trace("在場連線:已關閉");
      // ⚠ 退避**只在這裡**歸零 —— 判準是「這一圈真的當了一段時間的
      //   在場連線」,不是「CreateFileW 回來了」。握完手立刻被關掉
      //   (服務正在收工 / 正被更新程式換掉)的那一圈是零等待的,
      //   把它算成成功就是一條 100% CPU 的重連迴圈。
      //   ⚠ 無號相減:GetTickCount 是 49.7 天翻轉的 32 位元計數器。
      if (::GetTickCount() - linked_at < kRetryFastMs) {
        if (!Backoff(&misses)) break;
        continue;
      }
      misses = 0;
      linked_traced = 0;
    }
    if (io) ::CloseHandle(io);
  }

  // 一次 overlapped 讀。回傳實際讀到幾個位元組;0 = 連線沒了或被叫停。
  //
  // ⚠ ov 在堆疊上,所以取消之後**一定**要 GetOverlappedResult(..., TRUE)
  //   等它真的結束 —— 提早返回等於讓核心寫一塊已經不存在的記憶體。
  //   與 pipe_server.cc 的 WaitOverlapped 同一條理由。
  // ⚠ timed_out 一定要分出來:「等了 3 秒沒回」與「對面把連線關了」
  //   在回傳值上都是 0,而它們**要做的事完全相反**(前者重試同一版,
  //   後者降版)。少了這一格,一次服務忙碌就會讓這個宿主永久降到 v1。
  DWORD ReadOnce(HANDLE pipe, HANDLE io, char* buf, DWORD cap, DWORD wait_ms,
                 bool* timed_out) {
    if (timed_out) *timed_out = false;
    OVERLAPPED ov;
    ::ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = io;
    ::ResetEvent(io);
    DWORD got = 0;
    if (::ReadFile(pipe, buf, cap, &got, &ov)) return got;
    if (::GetLastError() != ERROR_IO_PENDING) return 0;
    HANDLE waits[2] = {io, stop_};
    const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, wait_ms);
    if (w != WAIT_OBJECT_0) ::CancelIoEx(pipe, &ov);
    if (w == WAIT_TIMEOUT && timed_out) *timed_out = true;
    if (!::GetOverlappedResult(pipe, &ov, &got, TRUE)) return 0;
    return w == WAIT_OBJECT_0 ? got : 0;
  }

  bool WriteAll(HANDLE pipe, HANDLE io, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
      OVERLAPPED ov;
      ::ZeroMemory(&ov, sizeof(ov));
      ov.hEvent = io;
      ::ResetEvent(io);
      DWORD wrote = 0;
      if (!::WriteFile(pipe, data.data() + sent,
                       static_cast<DWORD>(data.size() - sent), &wrote, &ov)) {
        if (::GetLastError() != ERROR_IO_PENDING) return false;
        HANDLE waits[2] = {io, stop_};
        const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, kIoTimeoutMs);
        if (w != WAIT_OBJECT_0) ::CancelIoEx(pipe, &ov);
        if (!::GetOverlappedResult(pipe, &ov, &wrote, TRUE)) return false;
        if (w != WAIT_OBJECT_0) return false;
      }
      if (wrote == 0) return false;
      sent += wrote;
    }
    return true;
  }

  // ⭐ 「這條執行緒上啟用中的不再是我們」——單向,不等回覆。
  //
  // ⚠ **不可以重用 WriteAll。** 它的 WaitForMultipleObjects 帶著 stop_,
  //   而這一刻 stop_ 已經被 NoteYield 設起來了 —— WriteAll 會立刻
  //   CancelIoEx 然後回 false,那句話一個位元組都送不出去。所以這裡用
  //   一個**自己新建的事件**,而且不等 stop_。
  // ⚠ 上限 300 毫秒。這條是背景執行緒,但它拖著的是 g_rime_dll_refs ——
  //   讓它無限等於讓 DLL 卸載不掉。
  // ⚠ ov 在堆疊上:取消之後**一定**要 GetOverlappedResult(..., TRUE),
  //   提早返回等於讓核心寫一塊已經不存在的記憶體。
  // ⚠ proto_ < 4 就不送:PresenceLink 握手被拒時會自己降版(見 Run),
  //   而 v3 以下的服務收到不認得的 op 會回錯並關掉連線。降版過的宿主
  //   行為退回「連線消失 → 服務端維持現狀」—— 留著,不是亂收。
  void SendYield(HANDLE pipe) {
    if (proto_ < 4) return;
    HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) return;
    const std::string data = Frame(EncodeSimple(2, Op::kProfileState, 0));
    OVERLAPPED ov;
    ::ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = ev;
    DWORD wrote = 0;
    if (!::WriteFile(pipe, data.data(), static_cast<DWORD>(data.size()),
                     &wrote, &ov)) {
      if (::GetLastError() == ERROR_IO_PENDING) {
        if (::WaitForSingleObject(ev, kYieldWriteMs) != WAIT_OBJECT_0)
          ::CancelIoEx(pipe, &ov);
        ::GetOverlappedResult(pipe, &ov, &wrote, TRUE);
      }
    }
    ::CloseHandle(ev);
    Trace("在場連線:已告知服務『這條執行緒上啟用中的不再是我們』(送出 %lu 位元組)",
          static_cast<unsigned long>(wrote));
  }

  // ── 這條連線是誰 ────────────────────────────────────────────
  //
  // 服務端要的只有兩格:host_pid 與**啟用我們的那一條執行緒**的 tid。
  // 有了它們,「使用者此刻正在用的那條執行緒上啟用中的是不是我們」
  // 才答得出來(common/bar_owner.h)。不建 session,不碰引擎佇列。
  enum class HandshakeResult { kOk, kRejected, kIoError };

  HandshakeResult SendHello(HANDLE pipe, HANDLE io) {
    // ⚠ 事件建不出來(整個進程的 handle 用光了)。這裡回 kIoError 而不是
    //   kOk:沒握手的連線在服務端 activated=false,一票都不投,所以那一橫
    //   本來就不會替這個宿主顯示 —— 假裝成功只會多一條永遠不會被用到的
    //   連線。回 kIoError 會讓外層以一秒一次的節奏重試,而 io 是整條執行緒
    //   共用的一個 handle,建不出來就是建不出來 —— 節奏由 kRetryFastMs 壓著,
    //   不會變成忙等。
    if (!io) return HandshakeResult::kIoError;
    Hello h;
    h.proto = proto_;
    h.shell_abi = static_cast<uint32_t>(RIME_SHELL_ABI_VERSION);
    h.host_pid = ::GetCurrentProcessId();
    h.host_tid = host_tid_;
    {
      wchar_t path[MAX_PATH] = {0};
      ::GetModuleFileNameW(nullptr, path, MAX_PATH);
      h.host_exe = WideToUtf8(path);
    }
    if (!WriteAll(pipe, io, Frame(EncodeHello(1, h))))
      return HandshakeResult::kIoError;
    // 回覆一定要讀:不讀的話「服務端不認得這個版本」與「一切正常」
    // 在這條執行緒上長得一模一樣,而前者要降版重試。
    FrameReader reader;
    char buf[512];
    for (int spin = 0; spin < 8; ++spin) {
      bool timed_out = false;
      const DWORD got =
          ReadOnce(pipe, io, buf, sizeof(buf), kIoTimeoutMs, &timed_out);
      if (got == 0) {
        // 逾時 = 對面只是慢。對面把連線關了 = 它不認得這個版本
        // (它的 DecodeHello 在「剛好用完」那一關整則丟掉)。
        return timed_out ? HandshakeResult::kIoError
                         : HandshakeResult::kRejected;
      }
      if (!reader.Feed(buf, got)) return HandshakeResult::kIoError;
      std::string payload;
      if (!reader.Next(&payload)) continue;
      uint32_t seq = 0;
      HelloOk ok;
      if (!DecodeHelloOk(payload, &seq, &ok))
        return HandshakeResult::kRejected;
      if (ok.proto != proto_) return HandshakeResult::kRejected;
      Trace("在場連線:握手完成 proto=%u tid=%lu",
            static_cast<unsigned>(ok.proto),
            static_cast<unsigned long>(host_tid_));
      return HandshakeResult::kOk;
    }
    return HandshakeResult::kIoError;
  }

  // 掛一個讀,等到連線斷掉、或有人要求停止。
  //
  // ⚠ 服務端在這條連線上握完手之後**不會再主動送東西**:pipe_server.cc
  //   的 send 是 ServeClient 的區域 lambda,只在回覆請求時用,而我們
  //   握完手之後一則請求都不送。所以這個讀完成 0 位元組 = 連線沒了。
  //   ⚠ 但仍然要用迴圈:萬一將來服務端多送了什麼,一次讀就返回會被
  //     誤判成斷線,然後這條執行緒開始重連迴圈。
  //
  // ⚠ 回傳值:true = 被 stop_ 叫醒(我們要主動收尾,管道還在);
  //           false = 管道斷了(對面已經沒了,一個位元組都寫不出去)。
  //   這兩件事必須分得出來 —— #111 的那句讓位只有在前者才送得出去。
  bool WaitUntilBroken(HANDLE pipe, HANDLE io) {
    if (!io) {
      // 事件建不出來的退路:那就只等停止訊號。連線斷掉會慢一點被發現,
      // 但**絕不可以**在這裡忙等 —— 那是一條 100% CPU 的迴圈。
      ::WaitForSingleObject(stop_, INFINITE);
      return true;
    }
    char scratch[256];
    while (::WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
      if (ReadOnce(pipe, io, scratch, sizeof(scratch), INFINITE, nullptr) == 0)
        // ⚠ 讀到 0 有兩種:對面關了,或 ReadOnce 是被 stop_ 打斷的。
        //   再問一次 stop_ 才分得出來。
        return ::WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0;
    }
    return true;
  }

  static const DWORD kIoTimeoutMs = 3000;
  // 送那句讓位的上限。⚠ 比 kIoTimeoutMs 短:這一刻使用者已經按下
  //   Win+空白鍵了,而這條執行緒拖著 g_rime_dll_refs。
  static const DWORD kYieldWriteMs = 300;

  HANDLE stop_ = nullptr;
  LONG refs_ = 0;
  // ⭐ 「走之前要先說一句話」。由宿主的 UI 執行緒寫(NoteYield)、
  //    背景執行緒讀(Run),所以走 Interlocked。
  LONG yielded_ = 0;
  uint32_t host_tid_ = 0;
  // 這條連線宣告的線路版本。握手被拒就降一版重試(見 Run)。
  uint32_t proto_ = kProtocolVersion;
};

TextService::TextService() {
  ::InterlockedIncrement(&g_rime_dll_refs);
  const std::wstring dir = ModuleDirectory(g_rime_module);
  if (dir.empty()) {
    // 走到這裡的話,連服務執行檔的位置都算不出來 —— 輸入法必定完全沒作用,
    // 而且沒有任何其他跡象。一定要留一行。
    Trace("!! 算不出模組目錄,服務永遠不會被啟動");
  } else {
    service_path_ = dir + L"\\rime_service.exe";
    ipc_.SetServicePath(service_path_);
  }
}

TextService::~TextService() {
  if (thread_mgr_) Deactivate();
  ::InterlockedDecrement(&g_rime_dll_refs);
}

// ───────────────────────── IUnknown ─────────────────────────

STDMETHODIMP TextService::QueryInterface(REFIID riid, void** ppv) {
  if (!ppv) return E_INVALIDARG;
  *ppv = nullptr;
  if (IsEqualIID(riid, IID_IUnknown) ||
      IsEqualIID(riid, IID_ITfTextInputProcessor))
    *ppv = static_cast<ITfTextInputProcessor*>(this);
  else if (IsEqualIID(riid, IID_ITfTextInputProcessorEx))
    *ppv = static_cast<ITfTextInputProcessorEx*>(this);
  else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink))
    *ppv = static_cast<ITfThreadMgrEventSink*>(this);
  else if (IsEqualIID(riid, IID_ITfKeyEventSink))
    *ppv = static_cast<ITfKeyEventSink*>(this);
  else if (IsEqualIID(riid, IID_ITfCompositionSink))
    *ppv = static_cast<ITfCompositionSink*>(this);
  else if (IsEqualIID(riid, IID_ITfInputProcessorProfileActivationSink))
    *ppv = static_cast<ITfInputProcessorProfileActivationSink*>(this);
  // ⚠ 少了這一格,AdviseSink(IID_ITfTextEditSink, …) 會拿到 E_NOINTERFACE,
  //   而 WatchContext 對那個回傳值是**安靜地放棄**的(有些宿主本來就不給)。
  //   也就是說:漏掉這一行不會有任何錯誤訊息,只會讓 Shift+滑鼠點擊
  //   在**每一個**宿主裡都誤切一次。守門在 audit_single_source.sh 規則 4。
  else if (IsEqualIID(riid, IID_ITfTextEditSink))
    *ppv = static_cast<ITfTextEditSink*>(this);
  if (!*ppv) return E_NOINTERFACE;
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) TextService::AddRef() {
  return static_cast<ULONG>(::InterlockedIncrement(&ref_));
}

STDMETHODIMP_(ULONG) TextService::Release() {
  const LONG n = ::InterlockedDecrement(&ref_);
  if (n == 0) delete this;
  return static_cast<ULONG>(n);
}

// ──────────────────── ITfTextInputProcessor(Ex)────────────────────

STDMETHODIMP TextService::Activate(ITfThreadMgr* mgr, TfClientId id) {
  return ActivateEx(mgr, id, 0);
}

STDMETHODIMP TextService::ActivateEx(ITfThreadMgr* mgr, TfClientId id,
                                     DWORD flags) {
  RIME_GUARD_BEGIN
  // ⚠ 這一行是「使用者切到我們的輸入法之後,系統到底有沒有把我們叫起來」
  //   唯一的答案。在這之前,整條路徑完全是紙上的 ——
  //   CI 驗得到註冊、驗得到引擎,就是驗不到這一格。
  //   (現在 windows/verify_tsf.sh 也驗得到了,靠的就是這一行。)
  Trace("ActivateEx 被呼叫 clientid=%lu flags=0x%lX 執行緒=%lu",
        static_cast<unsigned long>(id), static_cast<unsigned long>(flags),
        static_cast<unsigned long>(::GetCurrentThreadId()));
  if (!mgr) return E_INVALIDARG;
  thread_mgr_ = mgr;
  thread_mgr_->AddRef();
  client_id_ = id;

  ITfSource* source = nullptr;
  if (SUCCEEDED(thread_mgr_->QueryInterface(IID_ITfSource, (void**)&source))) {
    const HRESULT hr = source->AdviseSink(
        IID_ITfThreadMgrEventSink, static_cast<ITfThreadMgrEventSink*>(this),
        &thread_mgr_cookie_);
    if (FAILED(hr)) Trace("!! ThreadMgr sink 掛不上 hr=0x%08lX", (unsigned long)hr);
    source->Release();
  }

  // ══ key event sink ═══════════════════════════════════════════════
  //
  // ⚠⚠ **這裡的回傳值以前沒有人看。** 而它失敗的話,後果是整個輸入法
  //     安靜地不存在:
  //
  //       AdviseKeyEventSink 失敗 → 我們的 OnTestKeyDown / OnKeyDown
  //       **從來不會被呼叫** → 引擎一顆按鍵都收不到 → 連線不建立
  //       → 服務不會被啟動 → 沒有系統匣圖示、沒有設定視窗
  //
  //     也就是與「佈局問不出字」一模一樣的症狀組合,而且一樣沒有錯誤訊息:
  //     `ActivateEx` 照樣回 S_OK,語言列按鈕照樣加得上,使用者看到輸入法
  //     好端端地在清單上、切得過去、然後什麼都不會發生。
  //
  // fForeground = TRUE 要求**呼叫執行緒擁有前景**。文件是這樣寫的,而
  // 微軟自己的 SampleIME 也傳 TRUE —— 問題是那個前提不是永遠成立的
  // (沒有互動式桌面的工作階段、被遠端桌面接管、宿主在背景時被啟用…)。
  // 前景 sink 拿不到就退一步用非前景的:少的是「別的 TIP 也在時的優先權」,
  // 而那遠好過**一顆按鍵都收不到**。
  {
    ITfKeystrokeMgr* keystroke = nullptr;
    const HRESULT qhr =
        thread_mgr_->QueryInterface(IID_ITfKeystrokeMgr, (void**)&keystroke);
    if (SUCCEEDED(qhr) && keystroke) {
      HRESULT ahr = keystroke->AdviseKeyEventSink(
          client_id_, static_cast<ITfKeyEventSink*>(this), TRUE);
      if (FAILED(ahr)) {
        const HRESULT first = ahr;
        ahr = keystroke->AdviseKeyEventSink(
            client_id_, static_cast<ITfKeyEventSink*>(this), FALSE);
        Trace("key sink:前景版失敗 hr=0x%08lX,退成非前景 hr=0x%08lX",
              (unsigned long)first, (unsigned long)ahr);
      } else {
        Trace("key sink:已掛上(前景)");
      }
      key_sink_ok_ = SUCCEEDED(ahr);
      if (!key_sink_ok_)
        Trace("!! key sink 兩種都掛不上 —— 這個宿主裡一顆按鍵都收不到");
      keystroke->Release();
    } else {
      Trace("!! 拿不到 ITfKeystrokeMgr hr=0x%08lX —— 收不到任何按鍵",
            (unsigned long)qhr);
    }
  }

  // ══ 保留鍵:Ctrl+空白鍵切中英 ════════════════════════════════════
  //
  // 使用者原話:「ctrl+ 空格沒辦法切中英文 這個應該是所有輸入法的
  // 基本配置」。做法與**不可以**用的做法,理由都在
  // common/hotkey_policy.h 的檔頭:
  //
  //   · **不掛 WH_KEYBOARD_LL。** 那會看到使用者在每一個程式裡的
  //     每一次按鍵,與「離線、經得起審計」的定位直接衝突。
  //   · **不動 OnTestKeyDown / key_eat_policy。** 那張真值表是
  //     「不要再吃掉別人的鍵」的唯一防線,而 Ctrl+C 曾經真的被吃掉過。
  //
  // PreserveKey 只在**我們自己的文字服務被啟用時**生效,而且只有命中的
  // 那一顆會經由 OnPreservedKey 回來 —— 其餘按鍵我們一個都看不到。
  //
  // ⚠ 失敗不是致命的(別的 TIP 可能已經佔走這個組合)。記下來就好:
  //   使用者仍然點得到那一橫的第一格,而記錄會說明為什麼熱鍵沒反應。
  {
    ITfKeystrokeMgr* keystroke = nullptr;
    if (SUCCEEDED(thread_mgr_->QueryInterface(IID_ITfKeystrokeMgr,
                                              (void**)&keystroke)) &&
        keystroke) {
      TF_PRESERVEDKEY pk{};
      pk.uVKey = VK_SPACE;
      pk.uModifiers = TF_MOD_CONTROL;
      static const WCHAR kDesc[] = L"LuminaKey: Chinese/English";
      const HRESULT hr = keystroke->PreserveKey(
          client_id_, GUID_RimePreservedKeyToggle, &pk, kDesc,
          (ULONG)(sizeof(kDesc) / sizeof(kDesc[0]) - 1));
      preserved_key_ok_ = SUCCEEDED(hr);
      Trace("保留鍵 Ctrl+Space:%s hr=0x%08lX",
            preserved_key_ok_ ? "已註冊" : "註冊失敗(可能被別的輸入法佔走)",
            (unsigned long)hr);

      // 簡繁切換(Ctrl+Shift+F,G76)。微軟拼音的預設,我們兩端都沒有。
      // ⚠ 走的是**同一條**正規做法(PreserveKey),不是低階鍵盤 hook:
      //   TSF 幫我們比對、只把命中的那一顆交回來,其餘按鍵我們一個都
      //   看不到。理由見 common/hotkey_policy.h 的檔頭。
      // ⚠ 也**不動** OnTestKeyDown / key_eat_policy 那張真值表:
      //   PreserveKey 在 key event sink 之前就把那一顆挑走了。
      TF_PRESERVEDKEY vk{};
      vk.uVKey = 'F';
      vk.uModifiers = TF_MOD_CONTROL | TF_MOD_SHIFT;
      static const WCHAR kVarDesc[] = L"LuminaKey: Traditional/Simplified";
      const HRESULT vhr = keystroke->PreserveKey(
          client_id_, GUID_RimePreservedKeyVariant, &vk, kVarDesc,
          (ULONG)(sizeof(kVarDesc) / sizeof(kVarDesc[0]) - 1));
      preserved_variant_ok_ = SUCCEEDED(vhr);
      Trace("保留鍵 Ctrl+Shift+F:%s hr=0x%08lX",
            preserved_variant_ok_ ? "已註冊"
                                  : "註冊失敗(可能被別的輸入法佔走)",
            (unsigned long)vhr);
      keystroke->Release();
    }
  }

  // 語言設定檔變更的通知。見 text_service.h 的說明:三份設定檔共用一個
  // CLSID,所以切換語言時**只有**這一則通知會來。
  {
    ITfSource* psrc = nullptr;
    if (SUCCEEDED(thread_mgr_->QueryInterface(IID_ITfSource, (void**)&psrc))) {
      const HRESULT hr = psrc->AdviseSink(
          IID_ITfInputProcessorProfileActivationSink,
          static_cast<ITfInputProcessorProfileActivationSink*>(this),
          &profile_sink_cookie_);
      if (FAILED(hr))
        Trace("!! profile sink 掛不上 hr=0x%08lX —— "
              "使用者從繁體切到簡體會完全沒有效果", (unsigned long)hr);
      psrc->Release();
    }
  }

  // 現在這一刻是哪一份。sink 只在**之後**的切換時才觸發,
  // 所以第一次一定要自己問一次 —— 少了這一次,使用者開機後
  // 第一個 app 裡的預設方案永遠是「不知道語言」的那一種。
  RefreshProfile();

  // 語言列上的設定按鈕。加不上去不是錯誤(某些宿主沒有語言列),
  // 系統匣那條路是獨立的。
  //
  // ⚠ 第二個參數是「拒絕啟動服務」唯一一個使用者看得到的出口。
  //   條件是**兩個都成立**才示警:我們不會啟動它,而且它現在也沒在跑。
  //   少了後半條的話,分裂權杖的機器上(提權視窗 + 一般視窗並存,
  //   而服務由一般視窗那一邊帶起來了)使用者會在提權視窗裡看到一個
  //   「未啟動」的警告,而輸入法明明好端端地能用 —— 那是在製造假警報,
  //   而假警報會讓真警報失去意義。
  lang_bar_ = CreateLangBarButton(
      [this]() { OpenSettings(); },
      []() -> const wchar_t* {
        const HostElevation e = HostElevationOnce();
        if (MayStartUserService(e)) return nullptr;
        if (ServiceIsRunning()) return nullptr;
        return HostElevationTooltipW(e);
      });
  if (lang_bar_ && !AddLangBarButton(thread_mgr_, lang_bar_)) {
    ReleaseLangBarButton(lang_bar_);
    lang_bar_ = nullptr;
  }
  Trace("語言列按鈕:%s", lang_bar_ ? "已加入" : "加不上(宿主沒有語言列?)");

  // 順手把目前的鍵盤佈局問一遍,結果寫進記錄。
  //
  // ⚠ 這一格是這一輪查到的關鍵。文字服務啟用時 GetKeyboardLayout(0) 拿到的
  //   不保證是一份真的鍵盤佈局(見 win32_oracle.h 檔頭),而它若問不出字,
  //   **每一顆按鍵都會被原樣放行、引擎一顆都收不到**,連線也永遠不會建立。
  //   在這裡問一次,是為了讓那件事在記錄裡是一行明確的話,
  //   而不是「使用者說不能打字」。
  {
    const Win32KeyboardOracle& w = Oracle();
    Trace("鍵盤佈局 hkl=0x%08llX%s%s altgr=%d",
          static_cast<unsigned long long>(
              reinterpret_cast<UINT_PTR>(w.requested_hkl())),
          w.used_fallback() ? " (問不出字,已改問 real hkl)" : "",
          w.blind() ? " **完全問不出字 —— 按鍵一顆都進不了引擎**" : "",
          w.HasAltGr() ? 1 : 0);
    if (w.used_fallback())
      Trace("  改用的佈局 hkl=0x%08llX",
            static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(w.hkl())));
  }

  // 刻意**不**在這裡連線服務:開管道與握手要在宿主的 UI 執行緒上等,
  // 那會讓切換輸入法卡住。連線仍然是第一次真的有按鍵時才做。
  //
  // 但**啟動**服務要在這裡做(而且是在背景執行緒上)——
  // 系統匣圖示與設定視窗住在服務進程裡,等到第一顆按鍵才啟動的話,
  // 按鍵那條路一斷,使用者就同時失去輸入與全部 UI。見上面
  // StartServiceInBackground 的說明。
  StartServiceInBackground(service_path_);

  // ⚠ 上面那一段仍然成立,而且這一行**沒有推翻它**:按鍵那條連線
  //   (ipc_)仍然不在這裡建立。這裡建立的是另一條 ——
  //   「這個文字服務現在是啟用中的」那一條**會維持**的訊號。
  //
  //   少了它,服務端的 clients_ 在使用者切回輸入法之後是 0,而唯一
  //   能推回 1 的是按鍵 —— 那一橫要等他打第一個字才出現(工單 #82)。
  //
  // ⚠ 這一行在 ActivateEx 上的成本只有一次 CreateEventW + 一次
  //   CreateThread,與上面那一行同一個等級:**沒有任何 I/O 等待**。
  //   開管道、握手都發生在那條背景執行緒上。見 PresenceLink 的檔頭。
  EnsurePresence();

  // ⚠ 焦點**可能已經在那裡了**。使用者是在一個已經有輸入框有焦點的視窗裡
  //   切換輸入法的,而 ITfThreadMgrEventSink::OnSetFocus 只在焦點**改變**時
  //   才來 —— 不在這裡主動掛一次的話,他切過來之後的第一個輸入框
  //   直到他點到別的地方為止,都不會有 OnEndEdit,也就是 Shift+滑鼠點擊
  //   在那段期間仍然會誤切。
  WatchFocusedContext();

  // 一行把「這個宿主裡到底能不能用」講完。使用者回報時,這一行就是答案。
  Trace("ActivateEx 完成:key sink=%s 語言列=%s 服務路徑=%s",
        key_sink_ok_ ? "OK" : "**沒掛上,收不到按鍵**",
        lang_bar_ ? "OK" : "沒有",
        service_path_.empty() ? "(算不出來)" : "OK");
  return S_OK;
  RIME_GUARD_END_HR
}

STDMETHODIMP TextService::Deactivate() {
  Trace("Deactivate 被呼叫");
  // ⚠ 輕點 Shift 的狀態機要歸零。使用者按著 Shift 切走(或關掉輸入法)
  //   時,那顆 Shift 的放開我們**永遠不會看到** —— 不歸零的話,下一次
  //   啟用之後的第一顆 Shift 放開會被算成那一次的結尾而誤切一次。
  shift_tap_.Reset();
  if (lang_bar_) {
    RemoveLangBarButton(thread_mgr_, lang_bar_);
    ReleaseLangBarButton(lang_bar_);
    lang_bar_ = nullptr;
  }
  if (profile_sink_cookie_ != TF_INVALID_COOKIE && thread_mgr_) {
    ITfSource* psrc = nullptr;
    if (SUCCEEDED(thread_mgr_->QueryInterface(IID_ITfSource, (void**)&psrc))) {
      psrc->UnadviseSink(profile_sink_cookie_);
      psrc->Release();
    }
    profile_sink_cookie_ = TF_INVALID_COOKIE;
  }
  RIME_GUARD_BEGIN
  // ⚠ 文件編輯 sink 一定要在這裡拆掉。我們對那個 context 持有一份參考,
  //   不放的話宿主的文件會被一個已經停用的文字服務吊著。
  WatchContext(nullptr);
  if (composition_ && composition_ctx_) {
    ITfContext* ctx = composition_ctx_;
    RunSyncSession(ctx, client_id_, [this](TfEditCookie ec) -> HRESULT {
      EndComposition(ec);
      return S_OK;
    });
  }
  // session 沒收乾淨的話,服務端會留著一個永遠不會再被用到的 librime session。
  ipc_.Close();
  // 在場連線也要收,而且這一行就是 #82 的「切走之後那一橫必須消失」:
  // 關掉之後服務端那筆註冊消失 → 前景那條執行緒對不上任何一筆 → 隱藏。
  // ⚠ **不等那條執行緒**(這裡是宿主的 UI 執行緒)。
  // ⚠ Deactivate 不是唯一的收尾邊,而且它**不保證會來**:背景宿主延遲
  //   或永不、宿主被終止時來不及。真正在「使用者切到微軟拼音」那一刻
  //   一定會來的是 OnActivated 的非啟用邊,見 ClosePresence 的另一個
  //   呼叫點。
  ClosePresence();

  if (thread_mgr_) {
    ITfKeystrokeMgr* keystroke = nullptr;
    if (SUCCEEDED(thread_mgr_->QueryInterface(IID_ITfKeystrokeMgr,
                                              (void**)&keystroke))) {
      keystroke->UnadviseKeyEventSink(client_id_);
      // ⚠ 保留鍵一定要還回去。不還的話,這個宿主接下來的 Ctrl+空白鍵
      //   會被一個已經不存在的文字服務攔著 —— 使用者看到的是
      //   「某個程式裡空白鍵偶爾失靈」,而那查不到我們頭上。
      if (preserved_key_ok_) {
        TF_PRESERVEDKEY pk{};
        pk.uVKey = VK_SPACE;
        pk.uModifiers = TF_MOD_CONTROL;
        keystroke->UnpreserveKey(GUID_RimePreservedKeyToggle, &pk);
        preserved_key_ok_ = false;
      }
      // 同上,簡繁那一顆也要還回去。⚠ 只還真的註冊成功的那一顆。
      if (preserved_variant_ok_) {
        TF_PRESERVEDKEY vk{};
        vk.uVKey = 'F';
        vk.uModifiers = TF_MOD_CONTROL | TF_MOD_SHIFT;
        keystroke->UnpreserveKey(GUID_RimePreservedKeyVariant, &vk);
        preserved_variant_ok_ = false;
      }
      keystroke->Release();
    }
    if (thread_mgr_cookie_ != TF_INVALID_COOKIE) {
      ITfSource* source = nullptr;
      if (SUCCEEDED(thread_mgr_->QueryInterface(IID_ITfSource, (void**)&source))) {
        source->UnadviseSink(thread_mgr_cookie_);
        source->Release();
      }
      thread_mgr_cookie_ = TF_INVALID_COOKIE;
    }
    thread_mgr_->Release();
    thread_mgr_ = nullptr;
  }
  client_id_ = TF_CLIENTID_NULL;
  return S_OK;
  RIME_GUARD_END_HR
}

// ──────────────────── ITfThreadMgrEventSink ────────────────────

STDMETHODIMP TextService::OnInitDocumentMgr(ITfDocumentMgr*) { return S_OK; }
STDMETHODIMP TextService::OnUninitDocumentMgr(ITfDocumentMgr*) { return S_OK; }

// ⚠ 這兩個以前是空的。現在它們要做一件事:context 堆疊動了之後,
//   「最上層的那一個」可能換了人,而輕點 Shift 的眼睛(OnEndEdit)
//   一次只掛得住一個。兩個都重問一次「現在有焦點的是誰」——
//   push / pop 有可能發生在**別的** document manager 上,所以不能直接
//   拿參數那一個來掛。
STDMETHODIMP TextService::OnPushContext(ITfContext*) {
  RIME_GUARD_BEGIN
  WatchFocusedContext();
  return S_OK;
  RIME_GUARD_END_HR
}
STDMETHODIMP TextService::OnPopContext(ITfContext*) {
  RIME_GUARD_BEGIN
  WatchFocusedContext();
  return S_OK;
  RIME_GUARD_END_HR
}

STDMETHODIMP TextService::OnSetFocus(ITfDocumentMgr* focus,
                                     ITfDocumentMgr* /*prev*/) {
  RIME_GUARD_BEGIN
  // ⚠ 焦點換了 = 那一段輕點作廢。使用者按著 Shift 用滑鼠點到別的輸入框
  //   再放開,不算一次輕點 —— 這是產品判準明列的一條。
  //
  // ⚠ 而**在同一個輸入框裡點一下不會走到這裡** —— document manager
  //   沒有換,這個函式與下面 ITfKeyEventSink 的 OnSetFocus 一個都不會來。
  //   那一格由 OnEndEdit 補,見那裡。
  shift_tap_.Reset();
  // 掛 / 換 ITfTextEditSink。⚠ 用參數的 focus 而不是 thread_mgr_->GetFocus():
  //   我們正**在**焦點切換的中途,GetFocus 這一刻回什麼沒有保證。
  WatchContextOf(focus);
  // 換到別的輸入框:一定要把上一段組字收掉,否則它會留在原本的文件裡,
  // 而使用者接下來打的字會接在一段看起來已經沒有主人的 preedit 後面。
  if (composition_ && composition_ctx_) {
    ITfContext* ctx = composition_ctx_;
    RunSyncSession(ctx, client_id_, [this](TfEditCookie ec) -> HRESULT {
      SetCompositionText(ec, composition_ctx_, L"");
      EndComposition(ec);
      return S_OK;
    });
    Result r;
    ipc_.SendClear(&r);
    // 引擎手上的輸入被清掉了 —— Composing() 必須跟著回到 false,
    // 否則退格會繼續被當成「組字中的鍵」而吃掉,使用者就刪不掉字。
    engine_composing_ = false;
  }
  ipc_.SendFocus(focus != nullptr);
  return S_OK;
  RIME_GUARD_END_HR
}

// ──────────────────── ITfKeyEventSink ────────────────────

STDMETHODIMP TextService::OnSetFocus(BOOL /*foreground*/) {
  // ITfKeyEventSink 的那一個 —— 整個**視窗**的焦點進出。
  //
  // ⚠ 這一格與上面 ITfThreadMgrEventSink 的同名函式**不是**同一件事,
  //   兩個都要接:Alt+Tab 走掉時只有這一個會被呼叫(文件管理員沒有換),
  //   而那正是「按著 Shift 切走、在別的程式裡放開」的實際樣子。
  shift_tap_.Reset();
  return S_OK;
}

// 送出一次「中英切換」並把回來的快照套進文件。回傳:服務有沒有處理它。
//
// ⚠ **兩個入口共用這一份**:Ctrl+空白鍵(OnPreservedKey)與輕點 Shift
//   (OnTestKeyUp,工單 #89)。各寫一份的話,下面那一段「快照不可以丟掉」
//   的收尾邏輯會漂移,而漂移的樣子是「使用者打到一半的字不見了」——
//   這個專案已經在同一段程式碼上踩過一次(見下面那一整段 ⚠)。
//
// label 只進記錄檔,讓「按了沒反應」查得出是哪一顆鍵那一條路斷的。
bool TextService::SendAsciiToggle(ITfContext* ctx, int32_t keysym,
                                  uint32_t mods, const char* label) {
  // ⚠ 連不上服務就**不要**宣稱處理了這顆鍵。宿主的 Ctrl+空白鍵可能有
  //   它自己的用途(有些編輯器是自動完成),吃掉又不做事就是一顆
  //   壞掉的鍵 —— 這正是 key_eat_policy.h 檔頭那一課。
  if (!ipc_.EnsureReady()) {
    Trace("%s:連不上服務,放行給宿主", label);
    return false;
  }

  // 送的是**一顆按鍵**,不是一個新的協議操作:服務端用同一份
  // common/hotkey_policy.cc 認出它,所以線路格式一個位元都沒有變
  // (舊的服務配新的 DLL 仍然連得起來,只是那顆鍵會被交給 librime,
  //  而 librime 不處理 Control+space —— 也就是「沒有作用」,不是壞掉)。
  Result r;
  if (!ipc_.SendKey(keysym, mods, &r)) {
    Trace("%s:送不出去,放行給宿主", label);
    return false;
  }
  if (!r.handled) {
    // 服務沒有處理它:舊版的服務(不認得這個熱鍵,交給了 librime)、
    // 首次部署還沒做完(engine.cc 的 ToggleAsciiMode 會這樣回),
    // 或者使用者把輕點 Shift 關掉了(pipe_server.cc 的 KeyAction::kIgnore)。
    // 呼叫端會據此宣告**沒有**吃掉這顆鍵,所以這裡也不可以動文件 ——
    // 動了就是「宿主與我們同時處理同一顆鍵」。
    Trace("%s:服務沒有處理(部署中?舊服務?開關關著?),放行給宿主", label);
    return false;
  }

  // ══ ⚠ 這一份快照**不可以**丟掉 ══════════════════════════════════
  //
  // 舊版這裡寫著「中英切換不會產生上屏文字,也不會動組字」。**那句話是
  // 錯的**,而且錯在最要命的地方:common/hotkey_policy.h 的檔頭寫著這顆鍵
  // 存在的理由就是「中英切換發生在句子中間」——
  // 也就是說按下去的當下,引擎手上幾乎一定握著一段組字。
  //
  // librime 切到英數模式時會把那一段**上屏並清掉**(真機實測:
  // preedit="ni hao" → handled=1 has_commit=1 commit="nihao" 組字中=0)。
  // 而共用層的契約是:**commit 在 rs_snapshot_acquire 的當下就被消費**,
  // 不是在 release(service/engine.cc 的 TakeSnapshotLocked 檔頭)。
  // 也就是說這一份快照是那段文字唯一的一次現身。
  //
  // 把它丟掉的後果有三個,而且三個都是靜默的:
  //   (a) 使用者打到一半的字**永久消失**;
  //   (b) 螢幕上那段底線組字留著不收尾;
  //   (c) engine_composing_ 停在 true —— 見 Composing() 的說明,
  //       那之後每一顆退格都會被宣告吃掉又沒有人處理,那一格就是黑洞。
  //
  // 走的是與 HandleKey **完全同一段** ApplyPlan:兩邊各寫一份收尾邏輯
  // 就是兩份真相,而漂移的樣子正是這一類「畫面看起來正常、東西不見了」。
  //
  // ⚠ 目標 context 取 composition_ctx_ 優先:組字是開在**那一份**文件上的,
  //   而 TSF 交進來的 ctx 不保證是同一個。拿錯的話 SetCompositionText 會
  //   在別人的文件上動選取範圍。
  ITfContext* target = composition_ ? composition_ctx_ : ctx;
  if (target) {
    pending_rect_ = false;
    const Snapshot snap = r.snap;
    const HRESULT shr =
        RunSyncSession(target, client_id_,
                       [this, target, &snap](TfEditCookie ec) -> HRESULT {
                         return ApplyPlan(ec, target, snap);
                       });
    if (FAILED(shr)) {
      // edit session 沒跑成(宿主拒絕同步的讀寫鎖)。ApplyPlan 一步都沒做,
      // 而引擎那一側的組字**已經**被切中英清掉了 —— 這裡至少要把
      // engine_composing_ 拉回來,否則退格從此掉進黑洞(見 Composing())。
      engine_composing_ = (r.snap.status_flags & kStComposing) != 0;
      Trace("%s:edit session 失敗 hr=0x%08lX,上屏文字沒寫進文件", label,
            static_cast<unsigned long>(shr));
    }
    // IPC 放在 edit session **之外**:session 期間持有文件鎖(同 HandleKey)。
    if (pending_rect_) {
      ipc_.SendCaretRect(pending_rect_value_.left, pending_rect_value_.top,
                         pending_rect_value_.right, pending_rect_value_.bottom);
      pending_rect_ = false;
    }
  } else {
    // 沒有文件可以動(TSF 在焦點不在任何輸入框時也會把保留鍵送進來)。
    // 至少讓 engine_composing_ 跟上引擎 —— 少了這一行,上面 (c) 那個
    // 黑洞照樣會發生,而且完全沒有跡象。
    engine_composing_ = (r.snap.status_flags & kStComposing) != 0;
    Trace("%s:沒有 context 可以套用快照(has_commit=%d)", label,
          r.snap.has_commit ? 1 : 0);
  }

  Trace("%s:已切換 handled=1 英數=%d 組字中=%d has_commit=%d", label,
        (r.snap.status_flags & kStAsciiMode) ? 1 : 0,
        (r.snap.status_flags & kStComposing) ? 1 : 0, r.snap.has_commit ? 1 : 0);
  return true;
}

STDMETHODIMP TextService::OnPreservedKey(ITfContext* ctx, REFGUID guid,
                                        BOOL* eaten) {
  RIME_GUARD_BEGIN
  if (eaten) *eaten = FALSE;
  // ⚠ 兩顆保留鍵走**同一支** SendAsciiToggle。那支的名字是在只有一顆
  //   熱鍵的時候取的,而它真正做的事是「送一顆鍵給服務,再把回來的快照
  //   套進文件」—— 那一整段收尾邏輯(尤其「這一份快照不可以丟掉」)
  //   兩顆鍵一字不差地需要。
  //   名字沒有改,是因為 windows/audit_single_source.sh:447 把它釘住了
  //   (它要求 OnTestKeyUp 裡出現 `SendAsciiToggle(`)。
  bool handled = false;
  if (IsEqualGUID(guid, GUID_RimePreservedKeyToggle)) {
    handled = SendAsciiToggle(ctx, AsciiToggleKeysym(), AsciiToggleModifiers(),
                              "保留鍵 Ctrl+Space");
  } else if (IsEqualGUID(guid, GUID_RimePreservedKeyVariant)) {
    handled = SendAsciiToggle(ctx, VariantToggleKeysym(),
                              VariantToggleModifiers(),
                              "保留鍵 Ctrl+Shift+F");
  } else {
    return S_OK;
  }
  if (eaten) *eaten = handled ? TRUE : FALSE;
  return S_OK;
  RIME_GUARD_END_HR
}

STDMETHODIMP TextService::OnTestKeyDown(ITfContext* ctx, WPARAM w, LPARAM l,
                                        BOOL* eaten) {
  RIME_GUARD_BEGIN
  if (!eaten) return E_INVALIDARG;
  *eaten = FALSE;
  // OnTestKeyDown 不可以有副作用,所以這裡只做「這顆鍵是不是我們的」判斷。
  //
  // ⚠⚠ **這一趟說「吃」,就等於承諾了 OnKeyDown 會處理它。**
  //
  //   舊版的註解寫「回 TRUE 但稍後 OnKeyDown 回 FALSE 是合法的 —— TSF 會把
  //   那顆鍵交回宿主」。**那句話是錯的。** 宿主(以及 Windows 給非 TSF
  //   感知程式用的 CUAS 相容層)在這一趟聽到「吃」的當下就放棄了自己的
  //   預設處理;事後改口說沒吃時,那顆鍵已經回不去了。
  //
  //   使用者回報的「可以打字,不能刪除」就是這樣來的:退格映得出 keysym、
  //   連線也是通的,於是這裡說吃;引擎在沒有組字時不處理退格,於是
  //   OnKeyDown 說不吃 —— 那顆鍵掉進兩邊中間的黑洞。
  //
  //   所以現在多問一個問題:**這顆鍵在目前的狀態下,真的是我們的嗎?**
  //   規則(以及為什麼字元鍵與功能鍵的規則不一樣)在
  //   common/key_eat_policy.h 的檔頭。
  //
  // ── 輕點 Shift 的狀態機:**按下**這一半(工單 #89)────────────
  //
  // ⚠ 為什麼餵在 OnTestKeyDown 而不是 OnKeyDown:TSF 的協議是
  //   「Test 說吃了才會叫 Key」,而修飾鍵我們**永遠不吃**(kHostOnly)——
  //   也就是 OnKeyDown 對 Shift 根本不會被呼叫。Test 那一趟是我們唯一
  //   看得到它的地方。
  //
  // ⚠ TSF 說 OnTestKeyDown「不可以有副作用」。那句話講的是**文件** ——
  //   宿主還沒有答應讓我們動它。這裡只更新自己的私有狀態,一個位元都
  //   不碰文件、不碰引擎、不送 IPC;而真的要做事的那一步在放開那一趟。
  //
  // ⚠ 事件只組**一次**再分給兩邊用:BuildKeyEvent 裡有一次
  //   GetKeyboardState,而這裡是宿主的 UI 執行緒,每顆按鍵多問一次
  //   鍵盤狀態是白付的。
  const KeyEvent ev = BuildKeyEvent(w, l, /*key_up=*/false);
  shift_tap_.OnKey(ev, ::GetTickCount());

  // 走到這裡就代表 key event sink 是好的 —— 它沒掛上的話這個函式
  // 根本不會被呼叫。所以「記錄裡完全沒有按鍵行」與「按鍵行的 keysym 是 0」
  // 是兩件完全不同的事,前者要查的是 ActivateEx 那一段。
  const KeyPlan plan = PlanKey(ev);
  const bool ready = plan.eat && ipc_.EnsureReady();
  // ⚠ 額度用完之後不要完全靜音。這一格(「我們本來要吃它,但連線不在」)
  //   是使用者說「間歇打不出中文」時最直接的計數,而它以前只活在那 5 行
  //   額度裡 —— 用完之後整段時間完全沒有痕跡。只加計數,不寫記錄。
  if (plan.eat && !ready) ipc_.NoteKeyPassedThrough();
  if (key_trace_budget_ > 0) {
    --key_trace_budget_;
    // 五個欄位就足以指出斷在哪一段:
    //   keysym == 0         → 佈局那一段(按鍵根本沒進引擎,見 win32_oracle.h)
    //   族別 + 組字中 = 不吃 → 刻意放行給宿主(退格在沒有組字時就走這裡)
    //   吃了但 !ready       → IPC 那一段(上面 EnsureReady 已經記了原因)
    // ⚠ 多帶「階段 / 原因」兩欄是刻意的:上面那句「吃了但 !ready → IPC
    //   那一段(上面 EnsureReady 已經記了原因)」有一個前提 ——
    //   EnsureReady 的原因行只有 6 行額度(ipc_client.h 的 trace_budget_),
    //   用完之後那個交叉參照就斷了。寫在這一行上,一行就自足。
    const ReadyDiagnosis& d = ipc_.diagnosis();
    Trace("按鍵 vk=0x%02X scan=0x%02X keysym=0x%X mods=0x%X 族=%s 組字中=%d "
          "吃掉=%d 階段=%s 原因=%s",
          static_cast<unsigned>(w),
          static_cast<unsigned>((l >> 16) & 0xFF),
          static_cast<unsigned>(plan.mapped.keysym),
          static_cast<unsigned>(plan.mapped.modifiers), KeyKindTag(plan.kind),
          Composing() ? 1 : 0, ready ? 1 : 0, ReadyStageName(d.stage),
          LinkFailureName(d.failure));
    if (key_trace_budget_ == 0)
      Trace("(按鍵記錄額度用完,之後不再記 —— 它在宿主的 UI 執行緒上)");
  }
  if (!ready) return S_OK;
  *eaten = TRUE;
  (void)ctx;
  return S_OK;
  RIME_GUARD_END_HR
}

STDMETHODIMP TextService::OnKeyDown(ITfContext* ctx, WPARAM w, LPARAM l,
                                    BOOL* eaten) {
  RIME_GUARD_BEGIN
  if (!eaten) return E_INVALIDARG;
  *eaten = HandleKey(ctx, w, l, /*key_up=*/false) ? TRUE : FALSE;
  return S_OK;
  RIME_GUARD_END_HR
}

STDMETHODIMP TextService::OnTestKeyUp(ITfContext* ctx, WPARAM w, LPARAM l,
                                      BOOL* eaten) {
  // key-up 一律不吃。
  //
  // ⚠ 這裡原本寫著「TSF 本來就不會把純修飾鍵(Shift / Ctrl)的事件交給
  //   key event sink,所以那套『按一下 Shift 切中英』在這條路徑上做不到,
  //   要另外掛低階鍵盤 hook」。**那句話是假的**,而且它替一個結論背了很久
  //   的書(見 docs/coordination.md 與 docs/product-gaps.md)。
  //
  // ── 實測 ────────────────────────────────────────────────────────
  //
  //   出處:CI run 31511075812(sha ca97498),logic-x64 的
  //         「真的經過 TSF」那一步 = windows/verify_tsf.sh --press-shift。
  //   量法:windows/tests/tsf_host_main.cc 的 MeasureShiftDelivery。在真的
  //         ActivateEx 過的文字服務上,經 ITfKeystrokeMgr 送一次左 Shift 的
  //         TestKeyDown / KeyDown / KeyUp,然後**數 trace 檔多了幾行** ——
  //         沒有任何回傳值看得出 sink 有沒有被呼叫(TSF 收下、回 S_OK、
  //         pfEaten=FALSE,與「根本沒交給我們」長得一模一樣)。
  //   結果:
  //         SHIFT_SCAN_SENT=0x2A   SHIFT_TESTKEYDOWN_EATEN=0
  //         SHIFT_KEYDOWN_EATEN=0  SHIFT_KEYUP_EATEN=0   SHIFT_TRACE_LINES=1
  //         按鍵 vk=0x10 scan=0x2A keysym=0xFFE1 mods=0x0 族=host-only
  //         組字中=0 吃掉=0
  //
  //   → **key event sink 收得到純修飾鍵。** 多出來的那一行是 OnTestKeyDown
  //     自己寫的,所以純修飾鍵確實走到我們身上;我們只是照
  //     common/key_eat_policy.cc 的分類把它放行給宿主(kHostOnly)。
  //     **那一份(「不太會…但交過來時」)才是對的那一份。**
  //
  // 所以「要掛低階鍵盤 hook」這個前提不成立:輕點 Shift 切中英完全在
  // 這幾支 sink 裡用純函式的狀態機做完,WH_KEYBOARD_LL 那條紅線不必碰。
  // **這一輪把它做出來了**(工單 #89),就在下面。
  //
  // ⚠ 仍然沒有量到的:那支 harness 走的是 ITfKeystrokeMgr::KeyDown ——
  //   **那是宿主呼叫的入口**。它證明「sink 收得到」,證不到「真實宿主
  //   (記事本 / Chrome / Word)的訊息迴圈會不會把 VK_SHIFT 送進 TSF」。
  //   後者只有人在真機上試得出來,已列進 #48。**這顆鍵的驗收必須包含
  //   真機那一格** —— 這裡的一切在 CI 上都只驗得到「判斷對不對」。
  //
  // ⚠ 左右分不出來:送過來的 wParam 是**泛用的** VK_SHIFT(0x10),而
  //   keymap.cc:157 又把它一律折成 XK_Shift_L(0xFFE1)。要分左右只能看
  //   scan code(左 0x2A、右 0x36)—— 上面量到 scan code 確實有正確帶進來,
  //   而 common/shift_tap.cc 就是靠它認「放開的是不是同一顆」。
  //
  // ══ 輕點 Shift:**放開**這一半 ═════════════════════════════════
  //
  // ⚠ **`*eaten` 永遠是 FALSE,這一格不商量。**
  //
  //   宣告吃掉一顆修飾鍵的 key-up,代價是宿主收不到那則訊息 ——
  //   而自己追蹤 Shift 狀態的程式會從此以為 Shift 按著不放。那是
  //   「壞掉的鍵盤」,而這個專案的規矩是壞掉的鍵比缺功能嚴重
  //   (common/key_eat_policy.h 檔頭)。所以偵測與動作都在不吃的前提下做。
  //
  // ⚠ 做事的地方是 Test 這一趟,而不是 OnKeyUp。理由同上面按下那一半:
  //   不吃就不會有 OnKeyUp。這是一個**知情的取捨** —— TSF 說 Test 那一趟
  //   不該動文件,而我們在這裡確實可能動它(切中英會把組字上屏)。
  //   兩害相權:動文件失敗的話 SendAsciiToggle 裡那一段已經有處理
  //   (edit session 被拒時把 engine_composing_ 拉回來並記一行);
  //   吃掉修飾鍵失敗的話,使用者的 Shift 卡住而且完全查不到原因。
  RIME_GUARD_BEGIN
  if (eaten) *eaten = FALSE;
  const ShiftTap tap =
      shift_tap_.OnKey(BuildKeyEvent(w, l, /*key_up=*/true), ::GetTickCount());
  if (tap != ShiftTap::kToggleAsciiMode) return S_OK;

  // ⚠ 送的是**輕點 Shift 自己的正規形式**(裸的 XK_Shift_L),不是
  //   Ctrl+空白鍵那一組。服務端要分得出來才有辦法只關掉這一顆,
  //   而使用者關掉它時 Ctrl+空白鍵必須照樣能用(common/hotkey_policy.h)。
  //
  // 開關關著時服務端會回 handled=false,於是這裡什麼都不做 ——
  // 那正是「關掉之後一點痕跡都沒有」。
  Trace("輕點 Shift:偵測到(scan=0x%02X)",
        static_cast<unsigned>((l >> 16) & 0xFF));
  SendAsciiToggle(ctx, ShiftTapKeysym(), ShiftTapModifiers(), "輕點 Shift");
  return S_OK;
  RIME_GUARD_END_HR
}

STDMETHODIMP TextService::OnKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* eaten) {
  // ⚠ 這裡**故意不餵** shift_tap_。同一顆鍵餵兩次的話,一次乾淨的
  //   按放會變成「按下、按下、放開、放開」—— 而第二個按下會被狀態機
  //   當成自動重複而整段作廢,也就是這顆鍵永遠不會有反應。
  //   餵的地方只有 OnTestKeyDown / OnTestKeyUp,理由見那兩處。
  if (eaten) *eaten = FALSE;
  return S_OK;
}

STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie /*ec*/,
                                                  ITfComposition* composition) {
  RIME_GUARD_BEGIN
  // ⚠ 宿主自己把組字收掉,在使用者那一端最常見的原因就是**他用滑鼠點了
  //   一下**。輕點 Shift 的狀態機看不到滑鼠,所以這裡要告訴它。
  //   這一格與下面 OnEndEdit 是兩條獨立的路,兩條都要:
  //     · 這一條只有「當時正在組字」才會來,但它不需要宿主回報選取;
  //     · OnEndEdit 不需要組字,但需要宿主回報選取(不是每個宿主都報)。
  //   兩條都接,是為了讓任何一邊不成立時另一邊還在。
  shift_tap_.OnOtherInput();
  // 引擎那邊也要跟著清掉,否則下一次按鍵會接在一段已經不存在的組字後面。
  if (composition_ == composition) {
    composition_->Release();
    composition_ = nullptr;
    if (composition_ctx_) {
      composition_ctx_->Release();
      composition_ctx_ = nullptr;
    }
    Result r;
    ipc_.SendClear(&r);
    // 引擎手上的輸入被清掉了 —— Composing() 必須跟著回到 false,
    // 否則退格會繼續被當成「組字中的鍵」而吃掉,使用者就刪不掉字。
    engine_composing_ = false;
  }
  return S_OK;
  RIME_GUARD_END_HR
}

// ──────────────────── ITfTextEditSink ────────────────────
//
// ══ 為什麼這個 sink 存在 ═══════════════════════════════════════════
//
// 只為了一件事:**輕點 Shift 的狀態機看不到滑鼠。**
//
// 它的判準是「按下 Shift → 放開 Shift,而且中間什麼都沒發生」,而滑鼠
// 一顆按鍵事件都不會產生。於是
//
//     按住 Shift → 滑鼠點一下 → 放開 Shift
//
// 在四支 key event sink 眼裡與一次乾淨的輕點**逐位元相同**,會切一次中英。
// 而那一串正是**延伸選取的標準手勢** —— 任何人在文件裡選一段字都會做,
// 而這顆鍵預設是開的。
//
// ⚠ 為什麼不能靠現有的那三個 Reset() 補:**在同一個輸入框裡點一下不會換
//   document manager**,所以 ITfThreadMgrEventSink::OnSetFocus 不會來;
//   視窗焦點也沒有動,所以 ITfKeyEventSink::OnSetFocus 也不會來;
//   Deactivate 更不會。三個歸零點一個都碰不到這一格。
//
// ══ 為什麼是這條路,不是別的三條 ═══════════════════════════════════
//
// 1. **ITfMouseSink —— 評估過,不用。** 它不是「文件上的滑鼠事件」,是
//    「**某一段 range 上**的滑鼠事件」:`ITfMouseTracker::AdviseMouseSink`
//    收的是 `ITfRangeAnchor`,點在那段之外的一律不通知。要涵蓋整份文件就得
//    先拿文件鎖造一個涵蓋全文的 range、再隨著每次編輯維護它,而
//    `ITfMouseTracker` 本身還是宿主**可以不實作**的介面。
//    代價是一整套 COM 加一份跨事件的 range 生命週期,住在瀏覽器與提權
//    進程裡的瘦 DLL 裡,換到的仍然是部分涵蓋。
//
// 2. **GetAsyncKeyState / 低階滑鼠鉤子 —— 不可以。** 那條紅線是產品定位
//    (見 common/hotkey_policy.h、windows/audit_offline_win.sh)。而且就算
//    肯用也答不對:在 Shift 放開的那一刻,那一下點擊早就結束了,
//    問「現在有沒有按著」永遠是「沒有」。
//
// 3. **選取變了 —— 用這個。** 它比「看到滑鼠」更貼近判準:一次沒有挪動
//    游標的點擊,對「中間什麼都沒發生」來說本來就等於沒發生;而觸控與
//    手寫筆挪動游標時走的是同一則通知,不必各認一次。
//
// ⚠ **涵蓋不到的那一格,說清楚**:宿主得自己呼叫
//   `ITfContextOwnerServices::OnSelectionChange`(或在 edit session 裡改),
//   這一則通知才會出現。**那是宿主的義務,不是我們保證得了的事**,而這棵樹
//   上一輪才剛因為「讀碼推測平台行為」被自己的 CI 打臉一次。所以這裡不寫
//   「已經修好了」:記事本 / Chrome / Word 各自會不會報,只有真機驗得到,
//   已列進 #48 的清單。上面第 1 條(OnCompositionTerminated)是另一條腿 ——
//   使用者正在打字打到一半時點下去,那一條不需要宿主回報選取。
STDMETHODIMP TextService::OnEndEdit(ITfContext* /*ctx*/, TfEditCookie /*ec*/,
                                    ITfEditRecord* record) {
  RIME_GUARD_BEGIN
  if (!record) return S_OK;
  BOOL selection_changed = FALSE;
  if (FAILED(record->GetSelectionStatus(&selection_changed))) return S_OK;
  if (!selection_changed) return S_OK;
  // ⚠ 這裡**刻意不分辨**「這一次是不是我們自己動的」。我們動文件永遠是
  //   某顆非 Shift 的鍵引起的,而那顆鍵在它自己的 OnTestKeyDown 就已經把
  //   這一段毒掉了 —— 也就是說我們自己造成的 OnEndEdit 進來時,狀態機
  //   一定不在 kArmed,而 OnOtherInput() 在 kIdle / kPoisoned 是空操作。
  //   多存一個「現在是我在編輯」的位元,只會多一個會漂掉的東西。
  shift_tap_.OnOtherInput();
  return S_OK;
  RIME_GUARD_END_HR
}

void TextService::WatchContext(ITfContext* ctx) {
  if (ctx == edit_sink_ctx_) return;  // 已經掛在同一個上面
  if (edit_sink_ctx_) {
    ITfSource* src = nullptr;
    if (SUCCEEDED(edit_sink_ctx_->QueryInterface(IID_ITfSource, (void**)&src))) {
      if (edit_sink_cookie_ != TF_INVALID_COOKIE)
        src->UnadviseSink(edit_sink_cookie_);
      src->Release();
    }
    edit_sink_ctx_->Release();
    edit_sink_ctx_ = nullptr;
    edit_sink_cookie_ = TF_INVALID_COOKIE;
  }
  if (!ctx) return;
  ITfSource* src = nullptr;
  if (FAILED(ctx->QueryInterface(IID_ITfSource, (void**)&src))) return;
  DWORD cookie = TF_INVALID_COOKIE;
  const HRESULT hr = src->AdviseSink(IID_ITfTextEditSink,
                                     static_cast<ITfTextEditSink*>(this),
                                     &cookie);
  src->Release();
  // ⚠ 掛不上就**安靜地放棄**,不是錯誤。有些宿主不給,而後果是有邊界的:
  //   在那個宿主裡「按住 Shift 用滑鼠點一下」會多切一次中英,使用者
  //   再按一下就回來。反過來若在這裡當成失敗往上丟,壞掉的會是整個
  //   焦點切換那條路 —— 那是「這個程式裡完全不能打字」。
  if (FAILED(hr)) return;
  ctx->AddRef();
  edit_sink_ctx_ = ctx;
  edit_sink_cookie_ = cookie;
}

void TextService::WatchContextOf(ITfDocumentMgr* dim) {
  ITfContext* top = nullptr;
  // GetTop 失敗時 top 保持 nullptr = 取消掛接,那是對的:問不出最上層
  // 的 context 就等於我們現在什麼都看不到。
  if (dim) dim->GetTop(&top);
  WatchContext(top);
  if (top) top->Release();
}

void TextService::WatchFocusedContext() {
  ITfDocumentMgr* dim = nullptr;
  if (thread_mgr_) thread_mgr_->GetFocus(&dim);
  WatchContextOf(dim);
  if (dim) dim->Release();
}

// ──────────────────── 內部 ────────────────────

const Win32KeyboardOracle& TextService::Oracle() {
  // ⚠ 每次都問一次目前執行緒的 HKL。使用者可以在任何時候切換鍵盤佈局
  //   (Win+空白鍵),而快取住第一次看到的那個,等於從此用錯的佈局解讀按鍵。
  const HKL hkl = ::GetKeyboardLayout(0);
  if (!oracle_ || oracle_hkl_ != hkl) {
    oracle_.reset(new Win32KeyboardOracle(hkl));
    oracle_hkl_ = hkl;
  }
  return *oracle_;
}

TextService::KeyPlan TextService::PlanKey(WPARAM w, LPARAM l, bool key_up) {
  return PlanKey(BuildKeyEvent(w, l, key_up));
}

TextService::KeyPlan TextService::PlanKey(const KeyEvent& e) {
  KeyPlan p;
  p.mapped = MapKey(e, Oracle());
  p.kind = ClassifyKeyKind(p.mapped.keysym, p.mapped.modifiers);
  p.eat = ShouldEatKey(p.kind, Composing());
  return p;
}

bool TextService::SelfInsertChar(ITfContext* ctx, char32_t ch) {
  if (!ctx || ch == 0) return false;
  // char32_t → UTF-16。BMP 以外要拆成代理對,不然使用者按出來的字會變成
  // 一個問號 —— 而那種字元真的存在於某些佈局上。
  std::wstring text;
  if (ch < 0x10000) {
    text.push_back(static_cast<wchar_t>(ch));
  } else {
    const char32_t v = ch - 0x10000;
    text.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
    text.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
  }
  RunSyncSession(ctx, client_id_,
                 [this, ctx, &text](TfEditCookie ec) -> HRESULT {
                   return InsertText(ec, ctx, text);
                 });
  return true;
}

bool TextService::HandleKey(ITfContext* ctx, WPARAM w, LPARAM l, bool key_up) {
  // ⚠ 與 OnTestKeyDown 用**同一份**判斷。兩邊分岔 = 黑洞(見那裡的說明)。
  const KeyPlan plan = PlanKey(w, l, key_up);
  if (!plan.eat) return false;

  // ⚠ 連線斷掉時一定要把 engine_composing_ 歸零。
  //
  //   它是「退格算不算我們的」的依據之一(見 Composing())。連線斷在
  //   組字進行到一半的時候,這個旗標會**永遠停在 true** —— 於是使用者
  //   從此每一顆退格都被吃掉、而引擎又收不到,那顆鍵就再也沒有作用了。
  //   一個「暫時的連線失敗」不該留下一顆永久壞掉的鍵。
  if (!ipc_.EnsureReady()) {
    engine_composing_ = false;
    return false;
  }

  Result result;
  if (!ipc_.SendKey(plan.mapped.keysym, plan.mapped.modifiers, &result)) {
    // ── ⚠ 這一格以前是 `return false`,而那是一個黑洞 ──────────────
    //
    //   舊註解說「拿不到結果就**放行**,不可以吃掉」。那句話對的是
    //   **上面那一格**(EnsureReady 失敗):那一條路上 OnTestKeyDown
    //   同一趟也會失敗,所以我們從頭到尾沒有宣告吃過,宿主的預設處理
    //   還在。這一條不是 —— 走到這裡代表 EnsureReady() 在 Test 那一趟
    //   是好的、我們已經 `*eaten = TRUE`,而連線斷在**兩趟之間**。
    //
    //   宿主(以及 CUAS 相容層)在聽到「吃」的那一刻就放棄了自己的預設
    //   處理;事後在這裡改口說沒吃,那顆鍵回不去 —— 它就這樣消失。
    //   使用者看到的不是「打出英文」,是「按了完全沒有反應」,
    //   而那比打出英文更糟。這正是本檔 OnTestKeyDown 那一段 ⚠⚠
    //   寫的「掉進兩邊中間的黑洞」。
    //
    //   ⚠⚠ **上面那句取捨,對「組字中的字元鍵」是反的,而這一輪把它改掉。**
    //
    //   走到這裡代表連線斷在兩趟之間 —— 引擎對這顆鍵**一個字都沒說**,
    //   比逾時更徹底。以前這裡照樣 SelfInsertChar,而那正是使用者升級
    //   之後在訊息框裡拿到「ni好」的兩條路之一(另一條在下面
    //   !result.handled 那一段)。差別在損害停在哪裡:
    //
    //     · 按了沒反應 → 少一個音,看得見(preedit 對不上他打的字)、
    //       Esc / 退格救得回來、**上屏之前什麼都沒進到文件**。
    //     · 補進文件   → 字母已經在文件裡了,Esc 清不掉、引擎不知道它
    //       存在,而使用者要到送出之後才看得到。
    //
    //   兩邊都是「打出來的字不對」,差別在可不可逆。判斷本身是純函式,
    //   在 Ubuntu 上有一張真值表:common/key_eat_policy.h 的 DecideKeyOutlet。
    //
    //   所以這裡要負責到底,規則與下面 !result.handled 那一段完全相同。
    engine_composing_ = false;
    // 只加計數,不寫記錄:這裡是宿主的 UI 執行緒。數字由 ipc_client 的
    // 節流摘要帶出去。
    // ⚠ **兩種下場分開數**,理由整段寫在 ipc_client.h 的那兩支 Note* 上面:
    //   一句「改由我們收尾=N 鍵」會讓讀 tsf.log 的人跳過這裡,而那 N 顆
    //   裡有一部分正是他要查的「按了沒反應」。
    // ⚠ engine_answered=**false**:連線斷在兩趟之間,引擎連「我不要這顆鍵」
    //   都沒說得出口。DecideKeyOutlet 在這一格永遠不會回 kSelfInsert
    //   (見它的 ②),所以下面那一段補字元**這條路現在走不到**。
    //   照樣呼叫那支純函式而不是把 false 寫死在這裡,理由是單一真相:
    //   規則只准有一份,下一個人改的是 common/ 那一支,兩個呼叫點一起跟著走。
    const char32_t self_ch = CharForSelfInsert(plan.mapped.keysym);
    if (DecideKeyOutlet(plan.kind, /*engine_answered=*/false,
                        /*have_composition=*/composition_ != nullptr,
                        Composing(),
                        /*have_char=*/self_ch != 0) == KeyOutlet::kSelfInsert) {
      if (SelfInsertChar(ctx, self_ch)) {
        ipc_.NoteKeyRescuedBySelfInsert();
        return true;
      }
    }
    // 走到這裡 = 這顆鍵被吃掉了,而且文件上什麼都沒發生。
    ipc_.NoteKeyEatenWithNoOutlet();
    // 組字進行中的功能鍵(或補不出字元的那些):吃掉並且什麼都不做。
    // 理由與下面那一段一字不差 —— 自己插字元會插進組字的 range 裡,
    // 放行給宿主會讓它拿方向鍵去動壓在組字上的游標。
    return true;
  }
  if (!result.handled) {
    // 引擎不處理這顆鍵 —— 但我們在 OnTestKeyDown 已經宣告吃掉它了,
    // 宿主不會再處理它。所以這裡必須有人負責。
    //
    // ── 沒有組字時:字元鍵由我們自己把字寫進文件 ─────────────────
    //
    //   這條路每天都會走到:英數模式底下每一顆字母、朗月拼音底下的數字,
    //   引擎都不處理。(為什麼不乾脆不吃它們:哪些字元會起頭是**方案**
    //   決定的,注音的 alphabet 含數字。見 key_eat_policy.h。)
    //
    // ── 組字進行中:吃掉並且什麼都不做 ───────────────────────────
    //
    //   ⚠ 這一段是刻意的,不是偷懶。組字進行中,輸入法擁有這個輸入脈絡:
    //
    //     · 自己插字元 → 會插進組字的 range 裡,把 preedit 弄壞。
    //     · 放行給宿主 → 宿主會拿 Tab / 方向鍵去動**它自己的**游標,
    //       而那個游標正壓在一段進行中的組字上。使用者看到的是組字
    //       突然跳到別的地方、或文件裡多出半截 preedit。
    //
    //   兩條都比「什麼都不做」糟。而且這樣一來
    //   「OnTestKeyDown 說吃 ⟹ OnKeyDown 也說吃」是**結構上**成立的,
    //   不是碰運氣 —— verify_input_matrix.sh 的 MISMATCH 那一格量的就是它。
    //   ⚠ 這裡以前寫著「唯一的例外是連線在兩趟之間斷掉,而那條路上面
    //     已經放行了」。**那句話是錯的**:上面那條路(SendKey 失敗)
    //     以前是 `return false`,而它發生在 *eaten 已經是 TRUE 之後 ——
    //     那不是放行,那是黑洞。現在它與這裡走同一套規則,所以
    //     「Test 說吃 ⟹ Key 也說吃」**沒有例外**。
    // ── ⚠ handled=false 有**兩種**,而以前這裡把它們當成同一種 ──────
    //
    //   · 引擎跑完了、它不要這顆鍵(英數模式的字母)→ 補進文件,對的。
    //   · 引擎**一個字都沒說** —— service/engine.cc 的三個出口:部署中的
    //     fail-open、CallKeyBounded 逾時、工作跑完但不認得那個 session。
    //     以前這一種也走補字元,於是使用者升級之後拿到的是「ni好」:
    //     前兩顆鍵引擎還沒回來,我們替他打了 'n' 'i';第三顆起引擎回來,
    //     從「h」開始組字,最後上屏「好」。**一個詞被切成兩截,而且是
    //     靜默的** —— 那比整串英文糟,因為整串他一眼看得出來。
    //
    //   分辨兩者的是線路上那個位元(protocol.h 的 kStKeyNotAnswered),
    //   判斷本身是純函式(common/key_eat_policy.h 的 DecideKeyOutlet),
    //   在 Ubuntu 上有一張逐格的真值表。
    //
    // ⚠ 極性只准這一個方向:**有那個位元 = 確定沒回答**。沒有它不等於
    //   回答了 —— 舊服務不會送這個位元,而那時的行為與這一輪之前完全一樣
    //   (照舊補字元)。也就是說這一格不會讓任何既有組合變得更差。
    const bool engine_answered =
        (result.snap.status_flags & kStKeyNotAnswered) == 0;
    const char32_t self_ch = CharForSelfInsert(plan.mapped.keysym);
    const KeyOutlet outlet =
        DecideKeyOutlet(plan.kind, engine_answered,
                        /*have_composition=*/composition_ != nullptr,
                        Composing(), /*have_char=*/self_ch != 0);
    if (outlet == KeyOutlet::kSelfInsert) {
      if (SelfInsertChar(ctx, self_ch)) {
        // 這條路每天都會走到(英數模式的每一顆字母),所以額度要與按鍵那一行
        // 共用 —— 不然它會變成一條每顆按鍵一次磁碟寫入的路徑,而這裡是
        // 宿主的 UI 執行緒。
        if (key_trace_budget_ > 0) {
          --key_trace_budget_;
          Trace("引擎不吃這顆字元鍵,由我們補進文件 keysym=0x%X",
                static_cast<unsigned>(plan.mapped.keysym));
        }
        return true;
      }
    } else if (!engine_answered && outlet == KeyOutlet::kEatSilently) {
      // ⚠ 條件裡的 `outlet == kEatSilently` 不可以省。引擎沒回答**而且**
      //   這顆鍵落在 kPassToHost(沒有組字的功能鍵)時,舊行為是放行給
      //   宿主,而那是對的:退格交回宿主最壞是「這顆鍵沒作用」,吃掉
      //   最壞是「這顆鍵永遠壞了」(key_eat_policy.h 的 B 族)。
      //   這一輪只改字元鍵那一格,別的一個位元都不動。
      // ── ⚠ 這一行有**自己的**額度,而那是這一次查不出病因的直接原因 ──
      //
      //   上面那一行(「引擎不吃這顆字元鍵」)每天都會走到,英數模式下
      //   打幾個字母就把 key_trace_budget_ 用光了;於是 §13c 第二階段
      //   真的踩到這個缺陷的時候,瘦 DLL 的除錯記錄裡**一行都沒有**
      //   (log4:1170「按鍵記錄額度用完」)。一條只在額度還沒用完時才
      //   留得下痕跡的路徑,等於沒有痕跡。
      //
      //   所以罕見的這一格自己拿一份小額度 —— 它不是每天走的路,
      //   寫得起。
      if (not_answered_trace_budget_ > 0) {
        --not_answered_trace_budget_;
        Trace("引擎沒有回答這顆鍵(kStKeyNotAnswered),吃掉但**不動文件**"
              " keysym=0x%X 出口=%s 旗標=0x%X",
              static_cast<unsigned>(plan.mapped.keysym), KeyOutletTag(outlet),
              static_cast<unsigned>(result.snap.status_flags));
      }
      ipc_.NoteKeyEatenWithNoOutlet();
      // 吃掉、什麼都不做。文件一個位元都不動 —— 損害留在組字裡,而組字
      // 是可逆的、看得見的。理由整段在 common/key_eat_policy.h 的檔頭。
      return true;
    }
    // 組字進行中 → 吃掉並且什麼都不做(理由見上面那段)。
    //
    // ⚠ 用 Composing() 而不是 composition_:引擎說它還在組字、而我們這一側
    //   的組字沒開起來(StartComposition 失敗)時,這顆鍵在 OnTestKeyDown
    //   已經被宣告吃掉了 —— 這裡要用**同一個**條件回答,不然那一格就是黑洞。
    if (Composing()) return true;

    // 走到這裡 = 沒有組字、而且這顆鍵不是字元鍵(或字元補不出來)。
    // 依 key_eat_policy 的規則,功能鍵在沒有組字時根本不會被宣告吃掉,
    // 所以正常情況到不了這裡。真的到了就放行 ——
    // 放行最壞是「這顆鍵沒作用」,吃掉最壞是「這顆鍵永遠壞了」。
    return false;
  }

  pending_rect_ = false;
  const Snapshot snap = result.snap;
  // ⚠ 目標 context 與 SendAsciiToggle 用**同一份**判斷:組字是開在
  //   composition_ctx_ 那一份文件上的,而 TSF 交進來的 ctx 不保證是同一個。
  //   拿錯的話 SetCompositionText 會用 composition_->GetRange() 取範圍、
  //   卻對**別人的** ctx 呼叫 SetSelection。兩條路各一份判斷本身就是缺陷。
  //   ⚠ 這一格只有真機驗得到(#48),CI 上驗不到 —— 不宣稱它已經修好。
  ITfContext* target = composition_ ? composition_ctx_ : ctx;
  if (target) {
    // ⚠ 回傳值一定要接住。舊版把它整個丟掉,於是「宿主拒絕了同步的讀寫鎖」
    //   這件事完全沒有痕跡:ApplyPlan 一步都沒做(包括它第一行的
    //   engine_composing_ 更新),而畫面上引擎好、管道通、那一橫顯示中/繁,
    //   文件裡什麼都沒有出現。孿生路徑(SendAsciiToggle)做了這三件事,
    //   這一條沒做 —— 兩份真相已經漂移。
    const HRESULT shr =
        RunSyncSession(target, client_id_,
                       [this, target, &snap](TfEditCookie ec) -> HRESULT {
                         return ApplyPlan(ec, target, snap);
                       });
    if (FAILED(shr)) {
      // ApplyPlan 一步都沒做,所以 engine_composing_ 要在這裡自己跟上引擎
      // —— 少了它,退格從此掉進黑洞(見 Composing())。
      engine_composing_ = (snap.status_flags & kStComposing) != 0;
      if (edit_fail_trace_budget_ > 0) {
        --edit_fail_trace_budget_;
        Trace("按鍵:edit session 失敗 hr=0x%08lX,快照沒寫進文件 keysym=0x%X",
              static_cast<unsigned long>(shr),
              static_cast<unsigned>(plan.mapped.keysym));
      }
    }
  } else {
    // 沒有文件可以動。至少讓 engine_composing_ 跟上引擎(同上)。
    engine_composing_ = (snap.status_flags & kStComposing) != 0;
    if (edit_fail_trace_budget_ > 0) {
      --edit_fail_trace_budget_;
      Trace("按鍵:沒有 context 可以套用快照 keysym=0x%X has_commit=%d",
            static_cast<unsigned>(plan.mapped.keysym),
            snap.has_commit ? 1 : 0);
    }
  }

  // IPC 放在 edit session **之外**:session 期間持有文件鎖,在那裡等別的
  // 進程回話,等於把宿主的文件鎖交給另一個進程的排程。
  if (pending_rect_) {
    ipc_.SendCaretRect(pending_rect_value_.left, pending_rect_value_.top,
                       pending_rect_value_.right, pending_rect_value_.bottom);
    pending_rect_ = false;
  }
  return true;
}

HRESULT TextService::ApplyPlan(TfEditCookie ec, ITfContext* ctx,
                               const Snapshot& snap) {
  // 引擎自己說的「我還在組字」。下一顆按鍵要靠它決定退格是誰的
  // (見 Composing())。**一定要在這裡更新**:少了它,StartComposition
  // 失敗的那個宿主上引擎會一直卡著上一段輸入,而使用者按退格會被放行給宿主,
  // 於是他刪的是自己文件裡的字,而組字串原封不動。
  engine_composing_ = (snap.status_flags & kStComposing) != 0;
  const HostPlan plan = PlanFromSnapshot(composition_ != nullptr, snap);
  const std::wstring commit = Utf8ToWide(plan.commit_text);
  const std::wstring preedit = Utf8ToWide(plan.preedit);

  switch (plan.action) {
    case DocAction::kNothing:
      break;
    case DocAction::kUpdate:
      if (FAILED(StartCompositionIfNeeded(ec, ctx))) return E_FAIL;
      SetCompositionText(ec, ctx, preedit);
      break;
    case DocAction::kCommitAndEnd:
    case DocAction::kCommitAndUpdate:
      if (composition_) {
        SetCompositionText(ec, ctx, commit);
        EndComposition(ec);
      } else {
        InsertText(ec, ctx, commit);
      }
      if (plan.action == DocAction::kCommitAndUpdate) {
        if (FAILED(StartCompositionIfNeeded(ec, ctx))) return E_FAIL;
        SetCompositionText(ec, ctx, preedit);
      }
      break;
    case DocAction::kEnd:
      if (composition_) {
        // 先把組字文字清空再結束。少了這一步,最後那一段 preedit 會
        // **留在文件裡**變成使用者沒打過的字。
        SetCompositionText(ec, ctx, L"");
        EndComposition(ec);
      }
      break;
  }

  if (plan.show_candidates) ReportCaretRect(ec, ctx);
  return S_OK;
}

HRESULT TextService::StartCompositionIfNeeded(TfEditCookie ec, ITfContext* ctx) {
  if (composition_) return S_OK;
  if (!ctx) return E_FAIL;

  ITfInsertAtSelection* ias = nullptr;
  if (FAILED(ctx->QueryInterface(IID_ITfInsertAtSelection, (void**)&ias)))
    return E_FAIL;
  ITfRange* range = nullptr;
  HRESULT hr = ias->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, &range);
  ias->Release();
  if (FAILED(hr) || !range) return E_FAIL;

  ITfContextComposition* cc = nullptr;
  hr = ctx->QueryInterface(IID_ITfContextComposition, (void**)&cc);
  if (SUCCEEDED(hr)) {
    hr = cc->StartComposition(ec, range, static_cast<ITfCompositionSink*>(this),
                              &composition_);
    cc->Release();
  }
  range->Release();
  if (FAILED(hr) || !composition_) return E_FAIL;

  composition_ctx_ = ctx;
  composition_ctx_->AddRef();
  return S_OK;
}

HRESULT TextService::SetCompositionText(TfEditCookie ec, ITfContext* ctx,
                                        const std::wstring& text) {
  if (!composition_ || !ctx) return E_FAIL;
  ITfRange* range = nullptr;
  if (FAILED(composition_->GetRange(&range)) || !range) return E_FAIL;

  HRESULT hr = range->SetText(ec, 0, text.c_str(), static_cast<LONG>(text.size()));
  if (SUCCEEDED(hr)) {
    ITfRange* caret = nullptr;
    if (SUCCEEDED(range->Clone(&caret)) && caret) {
      caret->Collapse(ec, TF_ANCHOR_END);
      TF_SELECTION sel{};
      sel.range = caret;
      sel.style.ase = TF_AE_NONE;
      sel.style.fInterimChar = FALSE;
      ctx->SetSelection(ec, 1, &sel);
      caret->Release();
    }
  }
  range->Release();
  return hr;
}

HRESULT TextService::InsertText(TfEditCookie ec, ITfContext* ctx,
                                const std::wstring& text) {
  if (!ctx || text.empty()) return S_OK;
  ITfInsertAtSelection* ias = nullptr;
  if (FAILED(ctx->QueryInterface(IID_ITfInsertAtSelection, (void**)&ias)))
    return E_FAIL;
  ITfRange* range = nullptr;
  const HRESULT hr = ias->InsertTextAtSelection(
      ec, 0, text.c_str(), static_cast<LONG>(text.size()), &range);
  ias->Release();
  if (SUCCEEDED(hr) && range) {
    range->Collapse(ec, TF_ANCHOR_END);
    TF_SELECTION sel{};
    sel.range = range;
    sel.style.ase = TF_AE_NONE;
    sel.style.fInterimChar = FALSE;
    ctx->SetSelection(ec, 1, &sel);
  }
  if (range) range->Release();
  return hr;
}

void TextService::EndComposition(TfEditCookie ec) {
  if (composition_) {
    composition_->EndComposition(ec);
    composition_->Release();
    composition_ = nullptr;
  }
  if (composition_ctx_) {
    composition_ctx_->Release();
    composition_ctx_ = nullptr;
  }
}

void TextService::ReportCaretRect(TfEditCookie ec, ITfContext* ctx) {
  if (!ctx) return;
  ITfContextView* view = nullptr;
  if (FAILED(ctx->GetActiveView(&view)) || !view) return;

  ITfRange* range = nullptr;
  if (composition_) {
    composition_->GetRange(&range);
  } else {
    TF_SELECTION sel{};
    ULONG fetched = 0;
    if (SUCCEEDED(ctx->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched)) &&
        fetched == 1) {
      range = sel.range;  // 所有權轉移過來
    }
  }

  RECT rc{};
  BOOL clipped = FALSE;
  bool got = false;
  if (range && SUCCEEDED(view->GetTextExt(ec, range, &rc, &clipped)) &&
      (rc.right != rc.left || rc.bottom != rc.top)) {
    got = true;
  }
  if (!got) {
    // GetTextExt 在不少宿主上會失敗或給出空矩形(尤其是還沒有任何文字時)。
    // 退回宿主視窗的左上角:候選窗位置不理想,總比跑到螢幕原點好。
    HWND hwnd = nullptr;
    if (SUCCEEDED(view->GetWnd(&hwnd)) && hwnd) {
      RECT wr{};
      if (::GetWindowRect(hwnd, &wr)) {
        rc.left = wr.left + 8;
        rc.top = wr.top + 8;
        rc.right = rc.left + 1;
        rc.bottom = rc.top + 20;
        got = true;
      }
    }
  }
  if (got) {
    pending_rect_ = true;
    pending_rect_value_ = rc;
  }
  if (range) range->Release();
  view->Release();
}

// ── 語言設定檔 ────────────────────────────────────────────────────

STDMETHODIMP TextService::OnActivated(DWORD /*profile_type*/, LANGID langid,
                                      REFCLSID clsid, REFGUID /*catid*/,
                                      REFGUID guid_profile, HKL /*hkl*/,
                                      DWORD flags) {
  RIME_GUARD_BEGIN
  // ⚠ **旗標一定要先讀。** 這裡以前是兩個早退:
  //
  //     if (!IsEqualCLSID(clsid, CLSID_RimeTextService)) return S_OK;
  //     if (!(flags & TF_IPSINK_FLAG_ACTIVE)) return S_OK;
  //
  //   那兩行丟掉的正是**使用者切到別的輸入法時唯一會來的那兩個邊**:
  //   我們的 clsid 帶著 ACTIVE 被清除,或別人的 clsid 帶著 ACTIVE 被設立。
  //   丟掉的後果是使用者實機回報的 S4:「我現在用其他的輸入法,
  //   但是他突然出現了」—— 在場連線還開著,而服務端沒有任何辦法知道
  //   這條執行緒上啟用中的已經不是我們了。
  const bool ours = IsEqualCLSID(clsid, CLSID_RimeTextService);
  const bool active = (flags & TF_IPSINK_FLAG_ACTIVE) != 0;

  if (ours && active) {
    // 這條通知是「使用者從繁體切到簡體」唯一的管道(三份設定檔共用一個
    // CLSID,所以 TSF 不會重新 Activate)。在這之前它整條是紙上的。
    Trace("profile sink:啟用 langid=0x%04X", static_cast<unsigned>(langid));
    ipc_.SetProfile(static_cast<uint32_t>(langid), GuidToUtf8(guid_profile));
    // 語言列按鈕上的字跟著這一份語言設定檔走(見 lang_bar.cc 的說明)。
    SetUiLang(ResolveUiLang("system", static_cast<uint32_t>(langid)));
    // ⚠ 在自家 profile 之間切的時候**沒有** ActivateEx,所以在場連線
    //   要在這裡接回來 —— 上面那個分支剛剛才把它收掉。
    EnsurePresence();
    return S_OK;
  }

  if ((ours && !active) || (!ours && active)) {
    // 這條執行緒上啟用中的不再是我們。⚠ 這兩個邊是這一輪的核心:
    //   · ours && !active  = 我們這一份被停用
    //   · !ours && active  = 別人(微軟拼音…)在這條執行緒上被啟用
    // 兩者都必須讓服務端**立刻**看不到這個宿主,否則那一橫會在使用者
    // 已經換了輸入法之後繼續冒出來。
    Trace("profile sink:停用(我們的=%d 帶ACTIVE=%d)—— 收在場連線與 session",
          ours ? 1 : 0, active ? 1 : 0);
    ipc_.Close();
    // ⭐ **整個 #111 的修法只靠這一行。** 它必須在 ClosePresence() 之前:
    //   收連線與「說出那句話」是兩件事,而後者要趁那條管道還在。
    //
    //   少了它,服務端就再也收不到任何「別人的」正面證據 —— 判準只剩
    //   kOurs 與 kHold,那一橫變成真正的常駐(#82 復活)。
    //   audit_single_source.sh 有一條守門盯著這一行的存在與位置。
    NotePresenceYielded();
    ClosePresence();
    return S_OK;
  }

  // 剩下的是「別人被停用」。與我們無關 —— 接下來多半就輪到我們被啟用,
  // 而那一則會自己來。這裡什麼都不做,尤其**不可以**把在場連線收掉。
  return S_OK;
  RIME_GUARD_END_HR
}

// ── 在場連線:唯一的開關與唯一的收尾 ──────────────────────────
//
// ⚠ 這兩支存在的理由是「各有兩個呼叫點」:ActivateEx 與 profile sink 的
//   啟用邊都要開,Deactivate 與 profile sink 的非啟用邊都要收。把
//   PresenceLink::Start() / presence_->Stop() 各自收在**一處**,守門才
//   守得住「那兩個邊都還在」而不是只數呼叫次數
//   (audit_single_source.sh 規則 6)。
void TextService::EnsurePresence() {
  if (presence_) return;  // 已經有一條了 —— 不重複開
  // ⚠ 傳的是**現在這條執行緒**的 id。ActivateEx 與 OnActivated 都跑在
  //   啟用我們的那條 TSF 執行緒上,而那正是服務端要拿去跟前景比的東西。
  presence_ = PresenceLink::Start(
      static_cast<uint32_t>(::GetCurrentThreadId()));
}

void TextService::ClosePresence() {
  if (!presence_) return;
  presence_->Stop();
  presence_ = nullptr;
}

// ⭐ 「這條執行緒上啟用中的不再是我們」——服務端唯一的正面證據(#111)。
//
// ⚠ **只有 profile sink 的非啟用邊走這裡,Deactivate 不走。**
//   Deactivate 的意思是「我們的 TIP 正從這條執行緒上被卸下」,它答不了
//   「使用者選了誰」—— 宿主結束、視窗關掉、TSF 收工都會走它,而那些
//   都不是「他換了輸入法」。而且它本來就不保證會來(見 Deactivate 那一段)。
//   拿它當證據,就等於把 #111 換一個方向放回去:每關一個視窗那一橫閃一下。
void TextService::NotePresenceYielded() {
  if (presence_) presence_->NoteYield();
}

void TextService::RefreshProfile() {
  // 首選:ITfInputProcessorProfileMgr::GetActiveProfile —— 它同時給
  // langid 與 profile GUID,而且問的是「現在真的啟用的是哪一份」。
  ITfInputProcessorProfiles* profiles = nullptr;
  if (FAILED(::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_ITfInputProcessorProfiles,
                                (void**)&profiles)) ||
      !profiles) {
    // ⚠ 退路:HKL 的低 16 位就是目前輸入語言的 langid。
    //   拿不到 profile GUID,但 langid 已經足夠決定方案 ——
    //   而「拿不到就什麼都不做」等於讓簡體使用者繼續看到繁體字。
    const HKL hkl = ::GetKeyboardLayout(0);
    const uint32_t lang =
        static_cast<uint32_t>(reinterpret_cast<UINT_PTR>(hkl) & 0xFFFFu);
    Trace("語言設定檔:第3層(HKL 低位字)langid=0x%04X —— "
          "連 CLSID_TF_InputProcessorProfiles 都建不出來",
          static_cast<unsigned>(lang));
    ipc_.SetProfile(lang, std::string());
    return;
  }

  uint32_t langid = 0;
  std::string guid;
  // 走到哪一層。README 記著「三層退路一層都沒被執行過一次」——
  // 少了這一行,那句話下一輪還是一樣。
  const char* layer = "?";
  ITfInputProcessorProfileMgr* mgr = nullptr;
  if (SUCCEEDED(profiles->QueryInterface(IID_ITfInputProcessorProfileMgr,
                                         (void**)&mgr)) &&
      mgr) {
    TF_INPUTPROCESSORPROFILE prof{};
    if (SUCCEEDED(mgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &prof)) &&
        IsEqualCLSID(prof.clsid, CLSID_RimeTextService)) {
      langid = static_cast<uint32_t>(prof.langid);
      guid = GuidToUtf8(prof.guidProfile);
      layer = "第1層 ProfileMgr::GetActiveProfile";
    }
    mgr->Release();
  }
  if (langid == 0) {
    // 沒有 ProfileMgr(較舊的系統),或啟用的不是我們 —— 後者在
    // Activate 的當下是可能的,系統的快取還沒更新(見 README:
    // 註冊完 0.12 秒時列舉不到自己,22 秒後才看得到)。
    LANGID cur = 0;
    if (SUCCEEDED(profiles->GetCurrentLanguage(&cur))) {
      langid = static_cast<uint32_t>(cur);
      layer = "第2層 GetCurrentLanguage(啟用的還不是我們 / CTF 快取沒跟上)";
    }
  }
  profiles->Release();
  if (langid == 0) {
    const HKL hkl = ::GetKeyboardLayout(0);
    langid = static_cast<uint32_t>(reinterpret_cast<UINT_PTR>(hkl) & 0xFFFFu);
    layer = "第3層 HKL 低位字";
  }
  Trace("語言設定檔:%s langid=0x%04X guid=%s", layer,
        static_cast<unsigned>(langid), guid.empty() ? "(沒有)" : guid.c_str());
  ipc_.SetProfile(langid, guid);
  SetUiLang(ResolveUiLang("system", langid));
}

void TextService::OpenSettings() {
  // ⚠ 前景權要在**送出請求之前**讓出去,而且路 1 與路 2 都要。
  //
  //   路 1(IPC)與路 2(具名事件)都只是「請已經在跑的那一支服務開窗」,
  //   真正呼叫 SetForegroundWindow 的是那一支 —— 而它不符合任何一條放行
  //   條件(不是前景進程、不是被前景進程啟動的、沒收到最後一個輸入事件),
  //   系統只會讓工作列按鈕閃一下。使用者看到的是「視窗開了,可是在別的
  //   視窗後面」,而那與「按了沒反應」在體感上是同一件事。
  //
  //   宿主進程在使用者按下語言列按鈕的當下**就是**前景進程,所以它有權轉讓。
  //   ⚠ 精確 pid,不用 ASFW_ANY。
  //   ⚠ 路 3(下面的 CreateProcess)不需要這一步:那一支自己建視窗,而且
  //     它是被有前景權的宿主啟動的 —— 放行條件第二條本來就成立。
  //   ⚠ 類別名問 winshared,**不要在這裡抄一份字面值**(見 winutil.h)。
  const HWND settings_hwnd =
      ::FindWindowW(RimeSettingsWindowClassName(), nullptr);
  if (settings_hwnd) {
    DWORD settings_pid = 0;
    ::GetWindowThreadProcessId(settings_hwnd, &settings_pid);
    if (settings_pid) ::AllowSetForegroundWindow(settings_pid);
  }

  // 路 1:管道已經連上 → 走 IPC。UWP / 市集 App 的宿主跑在 AppContainer 裡,
  // 開不了 Local\ 底下別人建立的具名物件,那時只有這一條走得通。
  if (ipc_.SendOpenSettings()) {
    Trace("設定按鈕:路1(IPC)");
    return;
  }

  // 路 2:具名事件。使用者還沒打過任何一個字時管道沒連,只有這一條走得通。
  HANDLE ev = ::OpenEventW(EVENT_MODIFY_STATE, FALSE,
                           RimeSettingsEventName().c_str());
  if (ev) {
    ::SetEvent(ev);
    ::CloseHandle(ev);
    Trace("設定按鈕:路2(具名事件)");
    return;
  }
  Trace("設定按鈕:路3(CreateProcess)");

  // 路 3:服務根本沒在跑 → 把它叫起來,直接開設定視窗。
  // ⚠ 判準與 ActivateEx 那一條**必須是同一個**,不然會出現「按鈕按得動
  //   但服務起不來」或反過來。見 common/elevation_policy.h。
  if (!MayStartUserService(HostElevationOnce())) {
    Trace("設定按鈕:不啟動服務(刻意):%s",
          HostElevationTag(HostElevationOnce()));
    return;
  }
  std::wstring exe = ModuleDirectory(g_rime_module);
  if (exe.empty()) return;
  exe += L"\\rime_service.exe";
  std::wstring cmd = L"\"" + exe + L"\" --settings";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');
  if (::CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
  }
}

}  // namespace rimewin
