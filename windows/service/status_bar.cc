#include "status_bar.h"

// ── ⚠ 這個檔案會列舉桌面上所有的 top-level 視窗。這裡把理由寫下來 ──
//
// LuminaKey 標榜離線為預設、經得起審計,而「讀取使用者桌面上每一個
// 視窗的矩形、樣式與擁有行程」實質上是一份**視窗清單能力**。它可以被
// 正當化,但不該以實作細節的身分夾帶進去 —— 所以:
//
//   誰在用:CollectFloatingBars()(本檔),只有它呼叫 EnumWindows。
//   為什麼:#120。使用者切到別家輸入法之後,我們那一橫**壓在人家的
//           橫條上** —— 他實際看到過那個畫面。避讓要成立就必須知道
//           「別人的橫條現在在哪裡」,而 Windows 沒有提供「列出所有
//           浮動工具列」這種問法,只有 EnumWindows。
//   讀什麼:每個視窗的 GWL_EXSTYLE、擁有行程的 pid、視窗矩形。
//           **不讀標題、不讀類別名、不讀內容、不比對任何名字**
//           (判準是形狀不是身分 —— 見 CollectFloatingBars 的說明)。
//   去哪裡:哪裡都不去。結果只餵給 common/statusbar_place.h 的
//           AvoidObstacles(純函式),算完一個 y 位移就丟掉;
//           不落地、不上傳、不進任何記錄檔。
//   多常做:需求驅動(ApplyPlacement),而且有 kBarScanMs = 500 的
//           快取。**不是**定時掃描 —— 沒有任何計時器排它。
//
// ⚠ 已知還可以更省(尚未實作,追蹤在 #120):改用 SetWinEventHook 被動
//   收 EVENT_OBJECT_SHOW / HIDE / LOCATIONCHANGE 來維護 bars_cache_,
//   只在真的冒出一個 topmost + no-activate 的窗時才重算。那會把
//   「每次連線抖動都可能掃一輪桌面」降成「真的有浮動橫條出現才付錢」,
//   語意上也更貼近需求。改動不小而且只有 Windows 上跑得出對錯,
//   所以這一輪只做了零風險的那一半(見 CollectFloatingBars 裡的順序)。

// GET_X_LPARAM / GET_Y_LPARAM。⚠ 不要自己用 LOWORD/HIWORD 拆 ——
// 多螢幕時滑鼠座標**會是負的**,而 LOWORD 是無號的,
// 左邊那顆螢幕上的每一次點擊都會落在一個荒謬的座標上。
#include <windowsx.h>

#include <algorithm>
// std::fprintf / std::snprintf —— LogOwnerDecision 那一行走 stderr,
// 而服務的 stderr 就是 service.log(main.cc 的重導向)。
#include <cstdio>
#include <string>

#include "../common/settings.h"
#include "../common/status_cells.h"
#include "../common/statusbar_layout.h"
#include "../common/ui_dip.h"
#include "../common/ui_layout.h"
#include "../common/ui_strings.h"
#include "../winshared/winutil.h"
#include "engine.h"
#include "settings_store.h"
#include "settings_window.h"

namespace rimewin {
namespace {

constexpr wchar_t kClass[] = L"LuminaKeyStatusBar";
constexpr wchar_t kPopupClass[] = L"LuminaKeyStatusBarPopup";
constexpr UINT WM_RIME_REFRESH = WM_APP + 1;
constexpr UINT WM_RIME_SHOW = WM_APP + 2;
constexpr UINT WM_RIME_QUIT = WM_APP + 3;
constexpr UINT WM_RIME_THEME = WM_APP + 4;
// 引擎那一頭把方案清單查回來了(快取已經填好)。
// ⚠ 由**引擎執行緒** PostThreadMessage 過來,所以它不帶任何指標;
//   要用的東西這一頭自己去快取拿。見 OpenSchemaPopup。
constexpr UINT WM_RIME_SCHEMAS_READY = WM_APP + 5;

// ── ⚠ 為什麼要一顆計時器 ────────────────────────────────────────
//
// Refresh() 只在 push_ui 被呼叫時發生(按鍵、選字、上屏、選方案)。
// 也就是說:首次安裝那十幾秒(實測值見 common/first_run_timing.h)
// 過完之後,那一橫**會一直停在準備中**,
// 直到使用者跑去某個輸入框打一個字。而他不會 —— 他看到的是一句
// 「還沒好」,所以他在等。兩邊互相等著對方先動。
//
// 引擎那兩個旗標是 atomic,問一次的成本是一次 load。半秒問一次,
// 狀態一變就重排重畫;沒變就什麼都不做(不會有多餘的重繪)。
constexpr UINT_PTR kStateTimer = 1;
constexpr UINT kStatePollMs = 500;

// §12.10.3 的尺寸(DIP)。
// ── ⚠ 幾何**不在這裡**了(§12.15 的 W34)────────────────────────
//
// 每一格的矩形以前是 Relayout() 自己算的,而這個檔案在 Ubuntu 上編不
// 起來 —— 所以「每一格只有 26 DIP 高」(§12.14.0 第 5 條,低於 §3.6 的
// 28)**沒有任何自動化看得到**,而它在畫面上看起來只是「那一橫有點扁」。
//
// 現在算式住在 common/statusbar_layout.cc,這裡只剩「量字寬」與「畫」。
constexpr int kBarH = barmetric::kBarH;          // 32(從 28 改)
constexpr int kBarRadius = barmetric::kBarRadius;  // 8(從 7 改)
constexpr int kCellRadius = barmetric::kCellRadius;  // 4
constexpr int kBarBorder = barmetric::kBorder;   // 1

// ── ⚠ §8.12 的規範性字面,四端一致,**不得在地化** ────────────────
//
// 它們刻意**不**進 ui_strings.cc(§12.9.3 第 1 條):進了 catalog 就會有人
// 把簡體語系的「简」翻成別的寫法,而那正是規範要避免的事 ——
// 這四個字是**狀態指示**,不是介面文字。
//
// W10 兩個方向都驗:它們必須出現在這裡,而且不得出現在 catalog 裡。
//
// ⚠ 第一格**只畫其中一個**(§8.12 的 `input_mode`,不是 `input_mode_pair`)。
//   哪一個由 common/status_cells.cc 決定 —— 那是純函式,在 Ubuntu 上
//   測得到,而「畫了兩個字面」正是使用者實機回報的缺陷,以前這裡沒有
//   任何自動化看得到。理由與取捨寫在 common/status_cells.h 的檔頭。
constexpr wchar_t kGlyphChinese[] = L"中";
constexpr wchar_t kGlyphAscii[] = L"En";
constexpr wchar_t kGlyphSimplified[] = L"简";
constexpr wchar_t kGlyphTraditional[] = L"繁";

enum CellIndex { kCellMode = 0, kCellVariant, kCellSchema, kCellSettings };

}  // namespace

StatusBar::StatusBar(Engine* engine, SettingsStore* store)
    : engine_(engine), store_(store) {}

StatusBar::~StatusBar() { Stop(); }

DWORD WINAPI StatusBar::ThreadEntry(LPVOID self) {
  static_cast<StatusBar*>(self)->ThreadMain();
  return 0;
}

bool StatusBar::Start() {
  if (thread_) return true;
  ready_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  thread_ = ::CreateThread(nullptr, 0, &StatusBar::ThreadEntry, this, 0,
                           &thread_id_);
  if (!thread_) return false;
  if (ready_) ::WaitForSingleObject(ready_, 5000);
  return hwnd_ != nullptr;
}

void StatusBar::Stop() {
  if (thread_id_) ::PostThreadMessageW(thread_id_, WM_RIME_QUIT, 0, 0);
  if (thread_) {
    ::WaitForSingleObject(thread_, 3000);
    ::CloseHandle(thread_);
    thread_ = nullptr;
  }
  if (ready_) {
    ::CloseHandle(ready_);
    ready_ = nullptr;
  }
}

// 見標頭:那四個字面的唯一出口。
const wchar_t* BarModeGlyph(bool ascii_mode) {
  return ascii_mode ? kGlyphAscii : kGlyphChinese;
}

// ── 前景是誰:唯一不依賴宿主誠實的那一層 ────────────────────────
//
// ⚠ 這裡只**取值**,一個判斷都不做 —— 判斷全在 common/bar_owner.cc
//   (純函式,tests/test_bar_owner.cc 直接餵 13 個宿主)。這一段在
//   Ubuntu 上驗不到,所以它必須薄到「看一眼就知道對不對」。
namespace {

// UWP:前景視窗屬於 ApplicationFrameHost.exe 的框,真正的宿主握著框
// 底下那個 Windows.UI.Core.CoreWindow。少了這一層,「使用者正在 UWP
// app 裡打字」會被判成沒有人在用,那一橫直接消失。
BOOL CALLBACK FindCoreWindow(HWND child, LPARAM lp) {
  wchar_t cls[64] = {0};
  if (::GetClassNameW(child, cls, 63) <= 0) return TRUE;
  if (::lstrcmpW(cls, L"Windows.UI.Core.CoreWindow") != 0) return TRUE;
  *reinterpret_cast<HWND*>(lp) = child;
  return FALSE;  // 找到就停
}

BarOwnerForeground ReadForegroundOwner() {
  BarOwnerForeground fg;
  // ⚠ 這一行不是判斷,是**取值**:「前景是服務自己 → 維持現狀」那個
  //   判斷在 common/bar_owner.cc,那裡在 Ubuntu 上測得到(O13)。
  //   少了這一行:使用者從那一橫點「設定」→ 設定視窗成為前景 →
  //   它是服務自己的進程、自己的執行緒,13 個宿主一筆都對不上 →
  //   in_use 變 false → 3000 毫秒後那一橫在他眼前消失。
  fg.service_pid = static_cast<uint32_t>(::GetCurrentProcessId());
  const HWND top = ::GetForegroundWindow();
  // ⚠ NULL 是真的會發生的:UAC 提示、鎖定畫面、切換桌面的那一瞬間。
  //   known 維持 false → 裁決器回 undecidable → 呼叫端維持現狀。
  if (!top) return fg;
  DWORD pid = 0;
  const DWORD tid = ::GetWindowThreadProcessId(top, &pid);
  if (!tid) return fg;
  fg.known = true;
  fg.pid = static_cast<uint32_t>(pid);
  fg.tid = static_cast<uint32_t>(tid);
  // ⚠ 同樣是**取值**,不是判斷:「這條執行緒收不到文字就不准隱藏」那個
  //   判斷在 common/bar_owner.cc(測得到,O16)。
  //   桌面、工作列、截圖疊層沒有鍵盤焦點視窗 —— 它們代表的是「他此刻
  //   沒在打字」,不是「他換了輸入法」。GetGUIThreadInfo 是跨進程、
  //   同桌面、不需權限的一次呼叫;對更高完整性層級的前景可能回 FALSE,
  //   那時 can_take_text 維持 false → 隱藏那條路走不通 → 維持現狀。
  GUITHREADINFO gti;
  ::ZeroMemory(&gti, sizeof(gti));
  gti.cbSize = sizeof(gti);
  fg.can_take_text = ::GetGUIThreadInfo(tid, &gti) && gti.hwndFocus != nullptr;

  // ⚠ 只有前景真的是 UWP 的框視窗時才去列舉子視窗。每半秒對每一個
  //   前景視窗列舉一次子視窗是白花的。
  wchar_t cls[64] = {0};
  if (::GetClassNameW(top, cls, 63) > 0 &&
      ::lstrcmpW(cls, L"ApplicationFrameWindow") == 0) {
    HWND core = nullptr;
    ::EnumChildWindows(top, &FindCoreWindow, reinterpret_cast<LPARAM>(&core));
    if (core) {
      DWORD cpid = 0;
      const DWORD ctid = ::GetWindowThreadProcessId(core, &cpid);
      if (ctid && cpid != pid) {
        fg.inner_known = true;
        fg.inner_pid = static_cast<uint32_t>(cpid);
        fg.inner_tid = static_cast<uint32_t>(ctid);
      }
    }
  }
  return fg;
}

}  // namespace

void StatusBar::SetVisible(bool on) {
  if (thread_id_)
    ::PostThreadMessageW(thread_id_, WM_RIME_SHOW, on ? 1 : 0, 0);
}

// ⚠ 這四支從**連線執行緒**上被呼叫,所以只碰 regs_(自己的鎖),
//   一個視窗 API 都不碰。真正的顯示/隱藏由 UI 執行緒上的 kStateTimer
//   撿走(500 毫秒一次)—— 而「立刻」那一條靠 PostThreadMessage 把
//   計時器提前叫醒一次,不必等下一個 tick。
void StatusBar::OnClientAttached(uint64_t client_id) {
  {
    std::lock_guard<std::mutex> lock(reg_mu_);
    BarOwnerClient c;
    c.client_id = client_id;
    // ⚠ activated 維持 false:這一刻我們連它是不是我們自己的 DLL 都
    //   還不知道(ServeClient 在讀第一個位元組之前就建構了 ClientTicket)。
    //   一條沒握手的連線**一票都不投** —— 把「有一條 handle 開著」
    //   當成「有一個宿主在用我們」,就是上一版那個 active_clients。
    regs_.push_back(c);
  }
  // 不必叫醒:還沒握手的註冊改變不了任何答案。
}

void StatusBar::OnClientIdentified(uint64_t client_id, uint32_t host_pid,
                                   uint32_t host_tid) {
  {
    std::lock_guard<std::mutex> lock(reg_mu_);
    for (BarOwnerClient& c : regs_) {
      if (c.client_id != client_id) continue;
      c.host_pid = host_pid;
      c.host_tid = host_tid;
      c.activated = true;
      break;
    }
    // ⚠ 這條執行緒上我們的 TIP 又活了 —— 舊的讓位紀錄必須**立刻**作廢。
    //   少了這一行:使用者切走再切回來,那一橫會因為一筆還沒過期的
    //   舊證據而繼續藏著(最長 10 分鐘),而他什麼都沒做錯。
    for (size_t i = 0; i < yields_.size();) {
      if (host_tid != 0 && yields_[i].host_tid == host_tid)
        yields_.erase(yields_.begin() + static_cast<long>(i));
      else
        ++i;
    }
  }
  // 立刻顯示是規範的一部分(§12.10.6):使用者切過來、還沒打第一個字
  // 之前那一橫就該在。等 500 毫秒的話他會先看到一個空位。
  if (thread_id_) ::PostThreadMessageW(thread_id_, WM_RIME_REFRESH, 0, 0);
}

void StatusBar::OnClientSession(uint64_t client_id, uint64_t session) {
  std::lock_guard<std::mutex> lock(reg_mu_);
  for (BarOwnerClient& c : regs_) {
    if (c.client_id != client_id) continue;
    c.session = session;
    break;
  }
}

// ⭐ #111 的新事實來源:宿主**說出來**「這條執行緒上啟用中的不再是我們」。
//
// ⚠ 這一支跑在連線執行緒上,所以只碰 regs_ / yields_,一個視窗 API 都不碰。
void StatusBar::OnClientProfileState(uint64_t client_id, bool ours_active) {
  {
    std::lock_guard<std::mutex> lock(reg_mu_);
    uint32_t host_tid = 0;
    for (size_t i = 0; i < regs_.size(); ++i) {
      if (regs_[i].client_id != client_id) continue;
      host_tid = regs_[i].host_tid;
      break;
    }
    // ⚠ 報不出 tid(舊 DLL)就沒有可以下鍵的地方。舊 DLL 本來就不會送
    //   這則訊息,走到這裡代表對面壞了 —— 什麼都不做才是對的:
    //   一筆 tid=0 的紀錄會對上任何一個「查不到 tid」的前景。
    //
    // ── ⚠ 出口 F:但「什麼都不做」不包含「什麼都不說」(#120)────────
    //
    //   DLL 那一側費了三個出口的力氣把那句話送出來,服務端在這裡默默
    //   丟掉 —— 兩邊的記錄合起來仍然是一個斷點:DLL 說「已告知服務」,
    //   服務端這邊沒有任何一行,而那一橫照樣留著。
    //
    //   ⚠ 額度:每條連線最多幾則,而且最後一則會說自己是最後一則。
    //     這一支跑在連線執行緒上,13 個宿主 × 每次切輸入法一則。
    if (host_tid == 0) {
      static std::atomic<long> seen{0};
      const long n = ++seen;
      if (n <= 8) {
        std::fprintf(stderr,
                     "[bar] 收到讓位但這條連線報不出 host_tid(client=%llu)"
                     " —— 沒有可以下鍵的地方,整則丟掉%s\n",
                     static_cast<unsigned long long>(client_id),
                     n == 8 ? "(額度用完,這個出口不再記)" : "");
        std::fflush(stderr);
      }
      return;
    }
    for (size_t i = 0; i < yields_.size();) {
      if (yields_[i].host_tid == host_tid)
        yields_.erase(yields_.begin() + static_cast<long>(i));
      else
        ++i;
    }
    if (!ours_active) {
      // 上限 64,滿了丟最舊的。
      if (yields_.size() >= 64) yields_.erase(yields_.begin());
      BarOwnerYield y;
      y.host_tid = host_tid;
      y.at_ms = ::GetTickCount64();  // ⚠ 單調時鐘,不是牆上時鐘
      yields_.push_back(y);
    }
  }
  // 立刻重算一次:使用者按 Win+空白鍵切走的那一刻就該開始倒數,
  // 不必等下一個 500 毫秒的 tick。
  if (thread_id_) ::PostThreadMessageW(thread_id_, WM_RIME_REFRESH, 0, 0);
}

// ⚠ **這一支不准碰 yields_。** 連線死掉不是「使用者換了輸入法」的證據,
//   只是「這個宿主不見了」—— 而宿主被砍掉、宿主凍結、宿主根本沒載入
//   我們,在服務端是同一個事件。上一版把這兩件事混成一件,那就是 #111。
//   audit_single_source.sh 有一條負面守門盯著這一格。
void StatusBar::OnClientDetached(uint64_t client_id) {
  {
    std::lock_guard<std::mutex> lock(reg_mu_);
    for (size_t i = 0; i < regs_.size(); ++i) {
      if (regs_[i].client_id != client_id) continue;
      regs_.erase(regs_.begin() + static_cast<long>(i));
      break;
    }
  }
  if (thread_id_) ::PostThreadMessageW(thread_id_, WM_RIME_REFRESH, 0, 0);
}

// ⚠ 只在那一橫自己的 UI 執行緒上呼叫(它會阻塞在 Post 上等引擎回話,
//   與既有的 SetAsciiModeAll 同一個風險類別,不引入新的)。
void StatusBar::RefreshFromEngine() {
  if (!engine_) return;
  // ⚠ 問的是**使用者此刻正在打字的那一個 session**,不是隨便挑一個。
  //   舊版是引擎裡的 sessions_.begin() —— 13 個宿主各有自己的
  //   ascii_mode(方案自己的按鍵、ascii_composer 都會單獨翻某一個),
  //   挑第一個等於擲骰子,而那正是使用者回報的
  //   「點那一格中英不切換,而那一格始終顯示『中』」。
  const Engine::StatusReadback rb = engine_->ReadBackStatus(focused_session_);

  // ── ⛔ 中英那一格**不准**停在一個已知過期的值上 ──────────────
  //
  //   舊版在 rb.ok 為假時整支返回,於是那一格會一直畫著上一次讀到的
  //   東西 —— 使用者實機回報的正是「引擎已經切成英數(完全打不出中文)
  //   而那一格還說『中』」。
  //
  //   而修法**不是**在這裡塞一份預設值(那是另一次沒有證據的宣稱),
  //   也不是替它發明一個「不知道」的第三態 —— 中英這一格有一個
  //   **行程層級、而且永遠答得出來**的來源:Engine::AsciiMode()。
  //   那正是 ClickCell 決定往哪一邊切時讀的同一格。
  //
  // ⚠ **畫面與方向必須同一個來源。** 兩邊分岔的樣子就是「點了沒反應」:
  //   畫面說中(某個 session)、方向從行程層級算(已經是 En)→ 再點
  //   一次送的還是同一個值。所以讀到 session 的答案時,順手把它記回
  //   行程層級 —— 它是使用者正在用的那一個,本來就該是行程層級的現況。
  bool ascii;
  if (rb.ok) {
    ascii = (rb.status_flags & kStAsciiMode) != 0;
    engine_->NoteAsciiModeFromSession(ascii);
  } else {
    ascii = engine_->AsciiMode();
  }

  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    const bool a = ascii;
    // 簡繁沒有行程層級的等價物(見 common/status_cells.h:那組 radio
    // 有沒有被宣告,只有 session 答得出來)。所以回讀失敗時這一格
    // **維持不動** —— 它已經有一個誠實的第三態 kHidden,不需要猜。
    const VariantCell v =
        rb.ok ? VariantCellFrom((rb.status_flags & kStVariantKnown) != 0,
                                (rb.status_flags & kStSimplified) != 0)
              : variant_;
    if (a != ascii_mode_ || v != variant_) {
      ascii_mode_ = a;
      variant_ = v;
      changed = true;
    }
  }
  if (changed) {
    Relayout();
    ::InvalidateRect(hwnd_, nullptr, TRUE);
    // 托盤那一格畫的也是中英模式 —— 兩個指示器講的是同一件事,
    // 不能只更新一個。⚠ 只是 Post,真正的重畫在設定視窗那條執行緒上。
    if (settings_) settings_->NotifyModeChanged();
  }
}

void StatusBar::EvaluateVisibility() {
  if (!hwnd_) return;
  // ── 誰是這一刻的擁有者 ────────────────────────────────────
  //
  // 宿主提案(每一條註冊)、服務裁決(DecideBarOwner)、系統打平手
  // (ReadForegroundOwner)。⚠ 前景**只能**由 OS 回答:讓 13 個宿主
  //   各自宣稱「我是前景」的話,兩個同時說 true 就退化回舊判準的 OR。
  std::vector<BarOwnerClient> snapshot;
  std::vector<BarOwnerYield> yields;
  {
    std::lock_guard<std::mutex> lock(reg_mu_);
    snapshot = regs_;
    yields = yields_;
  }
  // ⚠ fg 必須具名:記錄那一行要印它的 tid 與行程名,而那是使用者下一次
  //   回報時我們唯一指得出「是哪一個程式把那一橫拿走了」的東西。
  const BarOwnerForeground fg = ReadForegroundOwner();
  const BarOwnerDecision owner =
      DecideBarOwner(snapshot, yields, fg, ::GetTickCount64());
  // ⚠ 前景回答不了這個問題時 → **維持現狀**。兩個原因:OS 答不出來
  //   (UAC 提示 / 安全桌面),或前景是**服務自己的視窗**(設定視窗 /
  //   托盤選單 / 那一橫自己的彈出選單)。在那裡把 in_use_ 打成 false,
  //   使用者只是被問了一次系統權限、或只是從那一橫點開了設定,
  //   那一橫就開始倒數消失。
  if (!owner.undecidable) {
    in_use_ = owner.in_use;
    focused_session_ = owner.focused_session;
  }

  BarVisibilityInputs in;
  in.user_enabled = user_enabled_;
  in.in_use = in_use_;
  in.now_ms = ::GetTickCount64();  // ⚠ 單調時鐘,不是牆上時鐘
  const BarAction act = visibility_.Feed(in);
  // ⚠ 記錄要寫在 kPending 那個早退**之前**:倒數中的那一段正是使用者
  //   要貼給我們看的東西,寫在後面的話它永遠不會被寫下來。
  const bool final_shown =
      (act == BarAction::kPending) ? shown_ : (act == BarAction::kShow);
  LogOwnerDecision(fg, owner, snapshot.size(), final_shown);
  if (act == BarAction::kPending) return;  // 遲滯還沒到期,維持現狀
  const bool want = act == BarAction::kShow;
  if (want == shown_) return;
  shown_ = want;
  // ⚠ 重新出現時**不重新定位**:回到使用者拖過的同一個位置。
  //   ApplyPlacement 已經在 Relayout 裡做過了,這裡只切可見性。
  ::ShowWindow(hwnd_, want ? SW_SHOWNOACTIVATE : SW_HIDE);
}

// ── 那一橫為什麼在 / 為什麼不在 ──────────────────────────────────
//
// ⚠ 這是使用者下一次回報時我們**唯一**查得到的東西。#111 的整個定位
//   過程之所以要靠推理,就是因為服務端從來沒有寫下「我這一刻判成什麼、
//   前景是誰」—— tsf.log 只看得到宿主那一側。
//
// ⚠ 只在**結果改變**時寫一行(節流鍵 = 三態 + 最後的可見性)。每 500
//   毫秒一行的話,一天下來是十七萬行,而使用者要貼給我們看。
//
// ⚠ 這一行走 stderr。從瘦 DLL 以 DETACHED_PROCESS 拉起來的服務,stderr
//   被 main.cc 重導向到 service.log;從開始功能表捷徑啟動的那一支有
//   console,這一行會印在那個一閃就沒的黑框裡(工單 #110 / 症狀 D)。
void StatusBar::LogOwnerDecision(const BarOwnerForeground& fg,
                                 const BarOwnerDecision& owner,
                                 size_t n_clients, bool shown) {
  if (ever_logged_ && owner.verdict == last_logged_verdict_ &&
      shown == last_logged_shown_)
    return;
  const bool was_shown = last_logged_shown_;
  const bool first = !ever_logged_;
  ever_logged_ = true;
  last_logged_verdict_ = owner.verdict;
  last_logged_shown_ = shown;

  const char* why = "hold(說不出來)";
  switch (owner.verdict) {
    case BarOwnerVerdict::kOurs:
      why = "ours(前景那條執行緒上啟用中的是我們)";
      break;
    case BarOwnerVerdict::kTheirs:
      why = "theirs(那條執行緒自己說過:啟用中的不再是我們)";
      break;
    case BarOwnerVerdict::kHold:
      switch (owner.hold_reason) {
        case BarOwnerDecision::HoldReason::kForegroundUnknown:
          why = "hold(OS 答不出前景)";
          break;
        case BarOwnerDecision::HoldReason::kForegroundIsService:
          why = "hold(前景是服務自己的視窗)";
          break;
        case BarOwnerDecision::HoldReason::kNoPresenceOnThread:
          why = "hold(那條執行緒上查不到在場連線,而它也沒說過讓位)";
          break;
        case BarOwnerDecision::HoldReason::kYieldedButNoFocus:
          why = "hold(說過讓位,但那條執行緒此刻收不到文字)";
          break;
        case BarOwnerDecision::HoldReason::kNone:
          break;
      }
      break;
  }

  // ⚠ exe / cls 只在真的要寫的時候才去查 —— 平常每 500 毫秒那一圈
  //   一個系統呼叫都不多花。
  std::string exe = "(取不到)";
  if (fg.pid != 0) {
    HANDLE ph = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                              static_cast<DWORD>(fg.pid));
    if (ph) {
      wchar_t path[MAX_PATH] = {0};
      DWORD n = MAX_PATH;
      if (::QueryFullProcessImageNameW(ph, 0, path, &n)) {
        const std::wstring full(path, n);
        const size_t slash = full.find_last_of(L'\\');
        exe = WideToUtf8(slash == std::wstring::npos ? full
                                                    : full.substr(slash + 1));
      } else {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "(取不到,err=%lu)",
                      static_cast<unsigned long>(::GetLastError()));
        exe = buf;
      }
      ::CloseHandle(ph);
    } else {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "(開不了,err=%lu)",
                    static_cast<unsigned long>(::GetLastError()));
      exe = buf;
    }
  }
  wchar_t clsw[64] = {0};
  std::string cls = "(取不到)";
  if (::GetClassNameW(::GetForegroundWindow(), clsw, 63) > 0)
    cls = WideToUtf8(clsw);

  const char* decision = shown ? "顯示" : "隱藏";
  const char* moved = (!first && shown == was_shown) ? "(維持)" : "";
  std::fprintf(stderr,
               "[bar] 判斷變了:前景 tid=%u pid=%u exe=%s cls=%s "
               "在場連線=%zu 收文字=%s 三態=%s 決定=%s%s\n",
               static_cast<unsigned>(fg.tid), static_cast<unsigned>(fg.pid),
               exe.c_str(), cls.c_str(), n_clients,
               fg.can_take_text ? "是" : "否", why, decision, moved);
  std::fflush(stderr);
}

void StatusBar::Refresh() {
  if (thread_id_) ::PostThreadMessageW(thread_id_, WM_RIME_REFRESH, 0, 0);
}

void StatusBar::RefreshTheme() {
  if (thread_id_) ::PostThreadMessageW(thread_id_, WM_RIME_THEME, 0, 0);
}

void StatusBar::OnSnapshot(const Snapshot& snap) {
  // ⚠ 線路上早就有這件事,而在這一輪之前整個 windows/ 沒有一處讀它。
  //   引擎還沒準備好時(首次整理字詞、或使用者按了「重新整理字詞」),
  //   protocol.h 的 kStDisabled 會被回填在每一份快照上。
  const bool not_ready = SnapshotSaysNotReady(snap.status_flags);
  bool changed = engine_not_ready_.exchange(not_ready) != not_ready;
  {
    std::lock_guard<std::mutex> lock(mu_);
    // ⚠ **帶著那個旗標的快照不可以拿來更新指示器。**
    //   引擎在準備期間對每一顆按鍵回的是一份**預設建構**的快照:
    //   除了 kStDisabled 以外全是 0。照單全收的話,使用者剛切成 En
    //   的那一格會自己跳回「中」,而他沒有碰過任何開關 ——
    //   那正是這個檔頭說的「說謊的指示器」。
    if (SnapshotFlagsAreUsable(snap.status_flags)) {
      const bool a = (snap.status_flags & kStAsciiMode) != 0;
      // ⚠ 三態,而且**known 為假時不看 simplified**。
      //   舊版的服務不會送 kStVariantKnown → 這裡是 kHidden → 那一格
      //   整格不顯示,而不是退回去畫繁體。判斷本身在 common/,測得到。
      const VariantCell v =
          VariantCellFrom((snap.status_flags & kStVariantKnown) != 0,
                          (snap.status_flags & kStSimplified) != 0);
      if (a != ascii_mode_ || v != variant_ ||
          snap.schema_name != schema_name_ || !have_snapshot_) {
        ascii_mode_ = a;
        variant_ = v;
        if (!snap.schema_name.empty()) schema_name_ = snap.schema_name;
        have_snapshot_ = true;
        changed = true;
      }
    }
  }
  if (changed) Refresh();
}

// ⭐ #119:只推中/英那一格。見 status_bar.h 上的說明。
//
// ⚠ **不碰 variant_ / schema_name_ / have_snapshot_。** 那三格的事實
//   來源是引擎的快照,而走到這裡正是因為那一份快照拿不到 ——
//   順手把它們寫成預設值就是「畫面說謊」,而那是這個檔案的檔頭
//   從第一行就在講的事。
void StatusBar::OnAsciiMode(bool ascii) {
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (ascii != ascii_mode_) {
      ascii_mode_ = ascii;
      changed = true;
    }
  }
  if (changed) Refresh();
}

ServiceState StatusBar::CurrentServiceState() const {
  EngineFacts facts;
  facts.engine_present = engine_ != nullptr;
  facts.deploy_done = engine_ && engine_->deploy_done();
  facts.deploy_ok = engine_ && engine_->deploy_ok();
  // ⚠ 兩個來源要 or 起來,而第二個是這一輪(#90)補的。
  //
  //   線路上那個旗標只有在**使用者按了鍵**時才會更新。而重新部署是從
  //   設定視窗按下去的 —— 他按完之後多半就回去看那一橫,一顆鍵都沒按。
  //   少了第二個來源,那一橫會一直畫著四格(說「可以打字」),
  //   而每一顆鍵其實都會被退回來。
  //
  //   這一格由 kStatePollMs 的計時器每半秒問一次,所以部署開始與結束
  //   那一橫都會自己動,不必等使用者去戳它。
  facts.engine_says_not_ready =
      engine_not_ready_.load() ||
      (engine_ && PhaseSaysPreparing(engine_->redeploy_phase()));
  return ServiceStateOf(facts);
}

void StatusBar::ThreadMain() {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &StatusBar::WndProc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;  // 全自繪
  wc.lpszClassName = kClass;
  // §12.14.4:懸浮狀態列**有**陰影,而且是系統畫的。
  // ⚠ 不得自己畫陰影 —— GDI 沒有模糊,自畫的陰影只能是幾條漸深的邊線,
  //   那在 2026 年看起來比沒有陰影更舊。CS_DROPSHADOW 還會**尊重使用者
  //   關掉陰影的設定**(SPI_GETDROPSHADOW),那正是我們要的行為。
  wc.style |= CS_DROPSHADOW;
  ::RegisterClassExW(&wc);

  WNDCLASSEXW pc{};
  pc.cbSize = sizeof(pc);
  pc.lpfnWndProc = &StatusBar::PopupProc;
  pc.hInstance = ::GetModuleHandleW(nullptr);
  pc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  pc.hbrBackground = nullptr;
  pc.lpszClassName = kPopupClass;
  // 方案彈出清單也是 top-level,同樣有陰影(§12.14.4 的表)。
  pc.style |= CS_DROPSHADOW;
  ::RegisterClassExW(&pc);

  // §12.10.3:TOOLWINDOW 讓它不出現在 Alt+Tab 與工作列;
  // NOACTIVATE 讓點它不搶焦點 —— 而「在句子中間切中英」正是它存在的理由,
  // 搶了焦點就會讓使用者正在打字的輸入框失去插入點。
  hwnd_ = ::CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClass, L"",
      WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, ::GetModuleHandleW(nullptr),
      this);
  if (ready_) ::SetEvent(ready_);
  if (!hwnd_) return;

  {
    UINT dpi = 96;
    using GetDpiFn = UINT(WINAPI*)(HWND);
    HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
    GetDpiFn fn = u32 ? reinterpret_cast<GetDpiFn>(reinterpret_cast<void*>(
                            ::GetProcAddress(u32, "GetDpiForWindow")))
                      : nullptr;
    if (fn) dpi = fn(hwnd_);
    dpi_ = dpi ? dpi : 96;
  }
  const Settings st = store_ ? store_->Load() : Settings();
  theme_.Refresh(AppearancePrefFromValue(
      st.Raw(keys::kAppearanceAppearance).c_str()));
  fonts_.Reset(dpi_, Script::kHant);
  user_enabled_ = st.GetTri(keys::kAppearanceFloatingBar) != Tri::kFalse;

  Relayout();  // Relayout 自己會走 ApplyPlacement(寬度是它算出來的)
  // ⚠ **不在這裡 ShowWindow。** 服務剛起來時還沒有任何宿主連上來,
  //   那一橫不該先出現三秒再消失 —— 那是一次沒有人要求過的閃爍。
  //   顯示由狀態機決定,而第一個宿主連上來時 OnClientAttached 會把
  //   計時器提前叫醒。
  EvaluateVisibility();
  // ⚠ 少了這一行,首次安裝那一橫會一直停在「正在準備」,
  //   直到使用者跑去某個輸入框打一個字(見上面 kStateTimer 的說明)。
  ::SetTimer(hwnd_, kStateTimer, kStatePollMs, nullptr);

  MSG msg;
  while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.hwnd == nullptr) {
      switch (msg.message) {
        case WM_RIME_REFRESH:
          Relayout();
          // 連線生死與焦點訊息也走這一則(它們從連線執行緒上 Post 過來),
          // 所以「立刻顯示」不必等下一個 500 毫秒的 tick。
          EvaluateVisibility();
          ::InvalidateRect(hwnd_, nullptr, TRUE);
          continue;
        case WM_RIME_SHOW:
          // ⚠ 這是**使用者的總開關**,不是「現在顯不顯示」。
          //   自動隱藏不走這條路 —— 它不改變這個值,所以條件恢復時
          //   那一橫自己會回來。§12.10.6。
          user_enabled_ = msg.wParam != 0;
          EvaluateVisibility();
          continue;
        case WM_RIME_THEME: {
          const Settings s2 = store_ ? store_->Load() : Settings();
          theme_.Refresh(AppearancePrefFromValue(
              s2.Raw(keys::kAppearanceAppearance).c_str()));
          ::InvalidateRect(hwnd_, nullptr, TRUE);
          continue;
        }
        case WM_RIME_SCHEMAS_READY:
          schema_query_inflight_ = false;
          // 選單還開著而且還在等 —— 換成真的清單。
          if (popup_ && popup_loading_) {
            popup_items_.clear();
            if (engine_ && engine_->SchemaListFromCache(&popup_items_) &&
                !popup_items_.empty()) {
              popup_loading_ = false;
              popup_hot_ = -1;
              PlacePopup();  // 列數變了,視窗要跟著長
              ::InvalidateRect(popup_, nullptr, TRUE);
            }
            // ⚠ 還是拿不到的話**不要把選單關掉**:使用者會以為自己
            //   點錯了。那句「正在讀方案…」留著,下一次按會再問一次。
          }
          continue;
        case WM_RIME_QUIT:
          ClosePopup();
          ::KillTimer(hwnd_, kStateTimer);
          ::DestroyWindow(hwnd_);
          hwnd_ = nullptr;
          return;
        default:
          break;
      }
    }
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }
  fonts_.Clear();
  theme_.Clear();
}

// ─────────────────────────── 版面 ───────────────────────────

void StatusBar::SeedSchemaName(const std::string& name) {
  // 只在還沒有任何快照進來過的時候種。有快照之後那一份才是真的 ——
  // 使用者可能已經自己換過方案了,種子會把畫面倒退回啟動時的樣子。
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (have_snapshot_ || name.empty() || name == schema_name_) return;
    schema_name_ = name;
  }
  if (hwnd_) ::PostThreadMessageW(thread_id_, WM_RIME_REFRESH, 0, 0);
}

void StatusBar::Relayout() {
  bool ascii;
  VariantCell variant;
  std::string name;
  {
    std::lock_guard<std::mutex> lock(mu_);
    ascii = ascii_mode_;
    variant = variant_;
    name = schema_name_;
  }
  // ⚠ 這裡以前是一個布林:
  //     service_ready_ = engine_ && deploy_done() && deploy_ok();
  //   於是「還在準備 / 準備失敗 / 引擎不在」三種完全不同的處境
  //   全部畫同一句紅字「輸入法沒有在跑」—— 而第一種那句話是假的,
  //   輸入法正在跑,只是還沒準備好。使用者第一次安裝時看到的
  //   就是那一句,而那是這個產品最貴的一段。
  service_state_ = CurrentServiceState();

  cells_.clear();
  if (!StateShowsCells(service_state_)) {
    // 四格在這三種狀態下**全部不畫**(它們此刻都是假的),
    // 整條改成一句話,而且整條可點。
    // ⚠ 哪一句由 common/service_state.cc 的對照表決定 —— 三種三句,
    //   而那張表有單元測試盯著「三種不可以回同一句」。
    Cell c;
    c.text = UiText(StatusTextFor(service_state_));
    cells_.push_back(c);
  } else {
    // ⚠ 四格畫什麼由 common/status_cells.cc 決定(純函式)。
    //   這裡只負責把四個規範性字面交出去、把結果貼進 cells_ ——
    //   一格畫幾個字面不是繪製碼可以自己決定的事。
    StatusGlyphs glyphs;
    glyphs.chinese = kGlyphChinese;
    glyphs.ascii = kGlyphAscii;
    glyphs.simplified = kGlyphSimplified;
    glyphs.traditional = kGlyphTraditional;
    StatusBarState st;
    st.ascii_mode = ascii;
    // 三態直接交給純函式;kHidden 那一格會拿到空字串 = 整項略過。
    st.variant = variant;
    // 空狀態**整項略過**(§8.12 規範性):方案名還沒載入完成時,
    // 那一格完全不佔位置,不得畫成一塊看不出用途的空白。
    st.schema_name = name.empty() ? std::wstring() : Utf8ToWide(name);
    st.settings_label = UiText(UiString::kBarSettings);
    for (const std::wstring& text : StatusBarCellTexts(glyphs, st)) {
      Cell c;
      c.text = text;
      cells_.push_back(c);
    }
  }

  // ── 量字寬(只有這一段需要 HDC)────────────────────────────
  //
  // ⚠ 純函式量不了字,所以量好了交給它。**不要**在版面那一側估字寬:
  //   估出來的那一份與畫出來的那一份會分岔,而分岔的樣子是
  //   「點擊區與看到的格子錯開」。
  HDC hdc = ::GetDC(hwnd_);
  HGDIOBJ oldf = hdc ? ::SelectObject(hdc, fonts_.Get(text_size::t4)) : nullptr;
  std::vector<BarCellIn> measured(cells_.size());
  for (size_t i = 0; i < cells_.size(); ++i) {
    if (cells_[i].text.empty()) continue;
    SIZE sz{};
    if (hdc)
      ::GetTextExtentPoint32W(hdc, cells_[i].text.c_str(),
                              static_cast<int>(cells_[i].text.size()), &sz);
    measured[i].text_w_dip = MulDivRound(sz.cx, 96, static_cast<int>(dpi_));
  }
  if (hdc) {
    if (oldf) ::SelectObject(hdc, oldf);
    ::ReleaseDC(hwnd_, hdc);
  }

  const bool sentence = !StateShowsCells(service_state_);
  const BarLayout bl =
      sentence ? LayoutStatusBarSentenceDip(
                     measured.empty() ? 0 : measured[0].text_w_dip)
               : LayoutStatusBarCellsDip(measured);
  bar_separator_x_ = bl.separator_x_dip < 0
                         ? -1
                         : Dip(bl.separator_x_dip, dpi_);
  schema_truncated_ = bl.schema_truncated;
  for (size_t i = 0; i < cells_.size(); ++i) {
    Cell& c = cells_[i];
    // ⚠ 空字串的那一格**整格略過,不佔位置**(§8.12 規範性)。
    //   簡/繁 那一格在 kHidden(引擎沒有回報任何字形)時拿到的就是空字串,
    //   而它若還佔著位置,使用者會按到一個**看不見的開關** ——
    //   按下去改變的是他看不見的東西,方向還是猜的。
    //   零寬 → HitCell 跳過(`if (r.right <= r.left) continue;`),
    //   那兩行合起來才是「點不到」。W26 兩個方向都在守。
    if (c.text.empty()) {
      c.rc = RECT{0, 0, 0, 0};  // 略過:不佔位置
      continue;
    }
    if (i >= bl.cells.size() || bl.cells[i].skipped) {
      c.rc = RECT{0, 0, 0, 0};
      continue;
    }
    const RectI& r = bl.cells[i].rect;
    c.rc = RECT{Dip(r.x, dpi_), Dip(r.y, dpi_), Dip(r.x + r.w, dpi_),
                Dip(r.y + r.h, dpi_)};
  }
  const int total = Dip(bl.total_w_dip, dpi_);
  const int minw = Dip(barmetric::kCellMinW, dpi_);

  // ── ⚠ 這裡以前是 SWP_NOMOVE ────────────────────────────────────
  //
  // 左上角釘死、只往右長。而這一橫是**右錨定**的
  // (statusbar_place.cc:「mon->right - dx - w」),預設離右邊 12 DIP。
  // 最痛的那條路是「未就緒(1 格,約 114 DIP)→ 就緒(4 格,約 195~220)」:
  // 寬度多 80~110 DIP,扣掉 12 的邊距之後有 70~100 DIP 在螢幕外面 ——
  // 「設定」整格點不到,而使用者剛裝好、正需要那一格。
  //
  // 修法不是「往左退一點」,是**寬度一變就重走 §12.10.5 的第 3 條**:
  // 一律把視窗矩形夾進工作區。那是純函式,單元測試碰得到。
  // ⚠ 只呼叫一次 SetWindowPos。先改尺寸再重擺的話,中間會有一格畫面
  //   是「已經長寬、還沒往左移」—— 也就是缺陷本身的樣子,閃一下。
  const int total_w = std::max(total, minw);
  ApplyPlacement(MulDivRound(total_w, 96, static_cast<int>(dpi_)));
}

void StatusBar::ApplyPlacement(int w_dip) {
  // §12.10.5 的三段回落。**全部是純函式**(common/statusbar_place.h),
  // 所以在 Ubuntu 上測得到 —— W20 靠這條。
  //
  // ⚠ 寬度由呼叫端給,**不是**從 GetWindowRect 讀回來的。Relayout 一改
  //   寬度就要重走這裡,而那時視窗上的寬度可能還是舊的。
  if (w_dip <= 0) {
    RECT cur{};
    ::GetWindowRect(hwnd_, &cur);
    w_dip = MulDivRound(cur.right - cur.left, 96, static_cast<int>(dpi_));
  }
  // ⚠ 錨點快取著,不要每次重排都去讀一次設定檔:Relayout 會跟著
  //   引擎狀態變動被叫到,而那是使用者打字的路徑上。
  if (!anchor_loaded_) {
    if (store_) {
      const Settings st = store_->Load();
      anchor_ = ParseAnchor(st.Raw(keys::kAppearanceFloatingBarPos));
    }
    anchor_loaded_ = true;
  }
  const BarAnchor anchor = anchor_;

  std::vector<WorkArea> monitors;
  struct Ctx {
    std::vector<WorkArea>* out;
  } ctx{&monitors};
  ::EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR mon, HDC, LPRECT, LPARAM p) -> BOOL {
        Ctx* c = reinterpret_cast<Ctx*>(p);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!::GetMonitorInfoW(mon, &mi)) return TRUE;
        WorkArea w;
        // ⚠ 用**工作區**(rcWork)不是整個螢幕矩形:整個矩形會讓這一橫
        //   被工作列蓋住,而 §8.6.7.3 註明那是實測會發生的事。
        w.left = mi.rcWork.left;
        w.top = mi.rcWork.top;
        w.right = mi.rcWork.right;
        w.bottom = mi.rcWork.bottom;
        w.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        w.dpi = 96;
        using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        static HMODULE shcore = ::LoadLibraryW(L"shcore.dll");
        static GetDpiForMonitorFn fn =
            shcore ? reinterpret_cast<GetDpiForMonitorFn>(
                         reinterpret_cast<void*>(
                             ::GetProcAddress(shcore, "GetDpiForMonitor")))
                   : nullptr;
        if (fn) {
          UINT dx = 96, dy = 96;
          if (SUCCEEDED(fn(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy)))
            w.dpi = static_cast<int>(dy);
        }
        c->out->push_back(w);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));

  PlacedBar p = PlaceStatusBar(anchor, monitors, w_dip, kBarH);
  // ── §12.10.5 的避讓:不要壓在別人的浮動橫條上(#120)──────────────
  //
  // ⚠ 判準是**形狀**,不是身分。認得出特定輸入法的白名單會過期,而且
  //   過期的白名單長得跟綠燈一模一樣:它不報錯,只是安靜地不避讓。
  //   幾何本身是純函式(common/statusbar_place.h 的 AvoidObstacles),
  //   在 Ubuntu 上測得到;這裡只負責「哪些窗算障礙」。
  if (p.monitor >= 0 && static_cast<size_t>(p.monitor) < monitors.size())
    p = AvoidObstacles(p, monitors,
                       CollectFloatingBars(monitors[
                           static_cast<size_t>(p.monitor)]));
  nudge_dy_ = p.nudge_dy;
  ::SetWindowPos(hwnd_, HWND_TOPMOST, p.x, p.y, p.w, p.h,
                 SWP_NOACTIVATE);
  ApplyWindowCorners(hwnd_, p.w, p.h, Dip(kBarRadius, dpi_));
}

// ── 「哪些窗算障礙」──────────────────────────────────────────────
//
// 四個條件,而且四個都是**結構上的**性質,不是名字:
//
//   1. 看得見,而且不是被 DWM 藏起來的(UWP 切走的那些殼還在,
//      但 DWMWA_CLOAKED 是真的);
//   2. WS_EX_TOPMOST —— 永遠在最上層;
//   3. WS_EX_NOACTIVATE —— 點它不搶焦點。
//      ⚠ 2 + 3 合起來就是「一條浮動工具列**必須**有的樣子」:它要一直
//        看得到,又不能讓使用者正在打字的插入點跳掉。我們自己那一橫
//        (status_bar.cc 的 WM_MOUSEACTIVATE 回 MA_NOACTIVATE)、
//        任何一家輸入法的狀態列,都會落在這兩格裡。一般的應用程式視窗
//        不會 —— 它們要搶焦點。
//   4. 別的行程的。自己的候選窗、設定視窗、那一橫本身都不算。
//
// 另外擋掉「大到不像橫條」的東西:全螢幕的覆蓋層(投影片、遊戲、
// 螢幕錄影的框)也符合 2+3,躲它沒有意義,而且會把這一橫趕到角落。
//
// ⚠ 這一支會走一次 EnumWindows,而 ApplyPlacement 在連線進出、狀態改變、
//   工作區變動時都會被叫到。所以節流:kBarScanMs 之內重用上一次的結果。
//   位置晚半秒才讓開是看不出來的;每一次連線抖動都掃一輪整個桌面不是。
std::vector<ObstacleRect> StatusBar::CollectFloatingBars(
    const WorkArea& on) const {
  const DWORD now = ::GetTickCount();
  if (bars_scanned_ms_ != 0 && now - bars_scanned_ms_ < kBarScanMs)
    return bars_cache_;
  struct Ctx {
    const StatusBar* self;
    std::vector<ObstacleRect>* out;
    int work_w;
    int work_h;
  };
  std::vector<ObstacleRect> out;
  // 「大到不像橫條」的門檻用**那一橫落腳的那顆螢幕**算 ——
  // 而那一顆是三段回落挑出來的,不是 MonitorFromWindow(此刻視窗還在
  // 舊位置上,而我們正要把它移走)。
  Ctx ctx{this, &out, on.width(), on.height()};
  ::EnumWindows(
      [](HWND h, LPARAM lp) -> BOOL {
        Ctx* c = reinterpret_cast<Ctx*>(lp);
        if (h == c->self->hwnd_ || h == c->self->popup_) return TRUE;
        if (!::IsWindowVisible(h)) return TRUE;
        const LONG ex = ::GetWindowLongW(h, GWL_EXSTYLE);
        if ((ex & WS_EX_TOPMOST) == 0) return TRUE;
        if ((ex & WS_EX_NOACTIVATE) == 0) return TRUE;
        DWORD pid = 0;
        ::GetWindowThreadProcessId(h, &pid);
        if (pid == ::GetCurrentProcessId()) return TRUE;
        // ⚠ 順序是**刻意的**:先做完全部本地的判斷(視窗樣式、擁有者、
        //   矩形、形狀),最後才問 DWM。
        //   DwmGetWindowAttribute 是這個回呼裡**唯一的跨行程往返**,
        //   而它以前排在矩形與尺寸過濾**前面** —— 於是每一個
        //   topmost + no-activate 的全螢幕覆蓋層(投影片、遊戲、
        //   螢幕錄影的框)都要先跨一次行程,才被下面那條「大到不像
        //   橫條」丟掉。放到最後,絕大多數往返直接不必發生,
        //   而通過的集合一模一樣(這四條全部是 AND)。
        RECT r{};
        if (!::GetWindowRect(h, &r)) return TRUE;
        const int w = r.right - r.left;
        const int hgt = r.bottom - r.top;
        if (w <= 0 || hgt <= 0) return TRUE;
        // 全螢幕的覆蓋層不是一條橫條。
        if (c->work_w > 0 && c->work_h > 0 && w * 4 >= c->work_w * 3 &&
            hgt * 4 >= c->work_h * 3)
          return TRUE;
        // ⚠ UWP 切到背景之後視窗還在、還「看得見」,只是被 DWM 藏起來。
        //   不擋掉的話那一橫會躲一個根本畫不出來的東西。
        //   14 = DWMWA_CLOAKED;mingw 的 dwmapi.h 沒有這個列舉。
        using DwmGetFn = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
        static HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll");
        static DwmGetFn fn =
            dwm ? reinterpret_cast<DwmGetFn>(reinterpret_cast<void*>(
                      ::GetProcAddress(dwm, "DwmGetWindowAttribute")))
                : nullptr;
        if (fn) {
          DWORD cloaked = 0;
          if (SUCCEEDED(fn(h, 14, &cloaked, sizeof(cloaked))) && cloaked != 0)
            return TRUE;
        }
        ObstacleRect o;
        o.left = r.left;
        o.top = r.top;
        o.right = r.right;
        o.bottom = r.bottom;
        c->out->push_back(o);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  bars_cache_ = out;
  bars_scanned_ms_ = now;
  return out;
}

void StatusBar::ApplyWindowCorners(HWND hwnd, int w_px, int h_px,
                                   int radius_px) {
  // ── §12.14.4「兩條路,**不得同時用**」──────────────────────────
  //
  // 1. DwmSetWindowAttribute(hwnd, 33, DWMWCP_ROUND) —— DWM 會做去鋸齒的
  //    圓角,而且陰影跟著對。成功就**完成**。
  // 2. 失敗(Win10 沒有這個屬性)→ SetWindowRgn + CreateRoundRectRgn。
  //
  // ⚠ 兩條同時走的結果是**雙重圓角**(DWM 圓一次、region 再切一次),
  //   邊緣會出現鋸齒的月牙。所以第 1 條成功時**不准**再呼叫 SetWindowRgn。
  if (!hwnd || w_px <= 0 || h_px <= 0) return;
  using DwmSetFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
  static HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll");
  static DwmSetFn fn =
      dwm ? reinterpret_cast<DwmSetFn>(reinterpret_cast<void*>(
                ::GetProcAddress(dwm, "DwmSetWindowAttribute")))
          : nullptr;
  if (fn) {
    // 33 = DWMWA_WINDOW_CORNER_PREFERENCE、2 = DWMWCP_ROUND。
    // mingw 的 dwmapi.h 沒有這兩個列舉,所以寫成常數並註明來源。
    const DWORD pref = 2;
    if (SUCCEEDED(fn(hwnd, 33, &pref, sizeof(pref)))) {
      // ⚠ 成功了就把可能殘留的 region 拿掉 —— 上一次可能走的是第 2 條
      //   (換螢幕、換 Windows 版本都做得到),留著就是雙重圓角。
      ::SetWindowRgn(hwnd, nullptr, FALSE);
      return;
    }
  }
  // ⚠ CreateRoundRectRgn 的右下角是**排他的**,所以是 w + 1, h + 1。
  //   少了那個 +1,右邊與下面各少一像素,症狀是「外框在右下角斷掉」。
  ::SetWindowRgn(hwnd,
                 ::CreateRoundRectRgn(0, 0, w_px + 1, h_px + 1, radius_px * 2,
                                      radius_px * 2),
                 TRUE);
}

void StatusBar::SavePlacement() {
  if (!store_) return;
  RECT rc{};
  ::GetWindowRect(hwnd_, &rc);
  HMONITOR mon = ::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!::GetMonitorInfoW(mon, &mi)) return;
  WorkArea on;
  on.left = mi.rcWork.left;
  on.top = mi.rcWork.top;
  on.right = mi.rcWork.right;
  on.bottom = mi.rcWork.bottom;
  on.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
  on.dpi = static_cast<int>(dpi_);
  // ⚠ 把避讓挪過的那一段**扣回去**。存進設定檔的必須是使用者選的那個
  //   位置 —— 存避讓後的位置,下一次重排會從那裡再讓一次,而那個偏移
  //   會一路累積,最後那一橫自己爬到螢幕另一頭。
  //   (拖動那條路上 nudge_dy_ 在 WM_LBUTTONDOWN 就歸零了,所以那一次
  //    這裡扣的是 0,存下來的正是他放手的位置。)
  const BarAnchor a = MakeAnchor(on, rc.left, rc.top - nudge_dy_,
                                 rc.right - rc.left, rc.bottom - rc.top);
  Settings st = store_->Load();
  st.SetRaw(keys::kAppearanceFloatingBarPos, SerializeAnchor(a));
  store_->Save(st);
  anchor_ = a;
  anchor_loaded_ = true;
}

// ─────────────────────────── 繪製 ───────────────────────────

void StatusBar::Paint(HDC hdc) {
  RECT client{};
  ::GetClientRect(hwnd_, &client);

  // 雙緩衝(§12.6.4 第 3 條)。少了它,狀態一變就閃。
  HDC mem = ::CreateCompatibleDC(hdc);
  HBITMAP bmp = ::CreateCompatibleBitmap(hdc, client.right, client.bottom);
  HGDIOBJ old_bmp = ::SelectObject(mem, bmp);

  // ── 底:**圓角**,不是先填滿再畫一條弧 ────────────────────────
  //
  // ⚠ §12.14.0 第 2 條記的就是舊寫法:`FillRect(client, kSurface)` 之後
  //   `RoundRect(NULL_BRUSH)` —— 第二步只畫線,沒有把四個角外面那塊底色
  //   挖掉,所以圓角是一條畫在方塊裡面的弧,視窗本身仍然是方的。
  //   真正的圓角在視窗那一層(ApplyWindowCorners),這裡負責的是
  //   「角外面那塊不要有底色」。
  {
    const int r = Dip(kBarRadius, dpi_);
    HGDIOBJ oldb = ::SelectObject(mem, theme_.Brush(kSurface));
    HGDIOBJ oldp = ::SelectObject(mem, theme_.Pen(kSurface, 1));
    ::RoundRect(mem, client.left, client.top, client.right, client.bottom,
                r * 2, r * 2);
    ::SelectObject(mem, oldp);
    ::SelectObject(mem, oldb);
  }

  // 第 3 格與第 4 格之間那條 1 DIP 分隔線(左右各 s3)。
  // ⚠ 理由不是裝飾:1–3 格**改狀態**,第 4 格**開一個視窗**。兩種不同的
  //   後果之間要有一個看得見的界線,否則使用者會以為第四格也是一個開關。
  //   1–2–3 之間**沒有**分隔線(它們是同一種東西)。
  if (bar_separator_x_ > 0) {
    RECT sep{bar_separator_x_, Dip(space::s1 + space::s2, dpi_),
             bar_separator_x_ + Dip(kBarBorder, dpi_),
             client.bottom - Dip(space::s1 + space::s2, dpi_)};
    ::FillRect(mem, &sep, theme_.Brush(kOutline));
  }

  ::SetBkMode(mem, TRANSPARENT);
  HGDIOBJ oldf = ::SelectObject(mem, fonts_.Get(text_size::t4));

  for (size_t i = 0; i < cells_.size(); ++i) {
    const Cell& c = cells_[i];
    if (c.rc.right <= c.rc.left) continue;  // 略過的那一格
    RECT r = c.rc;
    const bool hot = static_cast<int>(i) == hot_;
    const bool down = static_cast<int>(i) == pressed_;
    // 格的圓角 4(控制項級);底只在滑過／按下時才畫。
    // ⚠ 「一句話」那一種外觀畫的是**整條**,所以圓角跟著視窗走(8)。
    const int cell_r =
        Dip(StateShowsCells(service_state_) ? kCellRadius : kBarRadius, dpi_);
    if (down || hot) {
      const Role bg = down ? kRowPressed : kRowHover;
      HGDIOBJ oldb = ::SelectObject(mem, theme_.Brush(bg));
      HGDIOBJ oldp = ::SelectObject(mem, theme_.Pen(bg, 1));
      ::RoundRect(mem, r.left, r.top, r.right, r.bottom, cell_r * 2,
                  cell_r * 2);
      ::SelectObject(mem, oldp);
      ::SelectObject(mem, oldb);
    }

    if (!StateShowsCells(service_state_)) {
      ::SetTextColor(mem, theme_.Color(StateIsFailure(service_state_)
                                           ? kError
                                           : kOnSurfaceVariant));
      ::DrawTextW(mem, c.text.c_str(), -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      continue;
    }

    // ⚠ 每一格都是**一個**字面,四格畫法一致。以前第一格是兩段並排、
    //   用顏色深淺表示哪一段生效,而使用者看不出來(見 status_cells.h)。
    ::SetTextColor(mem, theme_.Color(hot || down ? kOnSurface
                                                 : kOnSurfaceVariant));
    // ⚠ 第 3 格(方案名)壓到 120 DIP 之後要在**字元邊界**截,
    //   不是畫到一半被裁掉。DT_END_ELLIPSIS 是唯一做得到這件事的旗標。
    //   截尾**不是死路**:第 3 格點下去開的自繪清單裡是完整的名字。
    UINT flags = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
    if (i == 2 && schema_truncated_) flags |= DT_END_ELLIPSIS;
    ::DrawTextW(mem, c.text.c_str(), -1, &r, flags);
  }

  ::SelectObject(mem, oldf);

  // ── 外框畫在**最後** ──────────────────────────────────────────
  //
  // 一般是 `controlBorder`(它畫在 surface 上,不是分隔兩塊),
  // **出事的時候**才是 `error` 色。
  // ⚠ 「正在準備」不是出事:輸入法在跑,只是還沒好。把它畫成紅的,
  //   等於用顏色再說一次那句謊話。
  //
  // ⚠ **順序不是隨便的。** 服務沒起來時整條是一句話、而且**整條**可點,
  //   所以滑過/按下畫的是整個 client 矩形 —— 先畫外框的話,那塊底會把
  //   紅色外框整圈蓋掉,而使用者滑過去的那一刻「出事了」這個訊號就消失。
  //   偏偏那正是他最需要它的時候(他正要按下去)。
  {
    const Role edge =
        StateIsFailure(service_state_) ? kError : kControlBorder;
    HPEN pen = theme_.Pen(edge, Dip(kBarBorder, dpi_));
    HGDIOBJ oldp = ::SelectObject(mem, pen);
    HGDIOBJ oldb = ::SelectObject(mem, ::GetStockObject(NULL_BRUSH));
    const int r = Dip(kBarRadius, dpi_);
    ::RoundRect(mem, client.left, client.top, client.right, client.bottom,
                r * 2, r * 2);
    ::SelectObject(mem, oldb);
    ::SelectObject(mem, oldp);
  }

  ::BitBlt(hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
  ::SelectObject(mem, old_bmp);
  ::DeleteObject(bmp);
  ::DeleteDC(mem);
}

int StatusBar::HitCell(POINT pt) const {
  for (size_t i = 0; i < cells_.size(); ++i) {
    const RECT& r = cells_[i].rc;
    if (r.right <= r.left) continue;
    if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom)
      return static_cast<int>(i);
  }
  // 只有一句話的時候**整條**可點(§12.10.4 末段)。
  if (!StateShowsCells(service_state_) && !cells_.empty()) return 0;
  return -1;
}

void StatusBar::ClickCell(int cell) {
  if (!StateShowsCells(service_state_)) {
    // §4.10 公式的第三段:一個**使用者做得到的動作**,
    // 不是「請聯絡開發者」。而**三種處境該做的事不一樣**:
    //   · 出事了 → 帶他到「進階」,那裡有「重新整理字詞」。
    //   · 還在準備 → 那裡沒有他該做的事;按「重新整理字詞」只是
    //     把剛做到一半的工作重做一次,更慢。帶他到第一頁,
    //     那裡的空狀態會說「字詞還沒整理完」。
    // ⚠ **不可以寫字面數字。** 這裡本來是 `? 3 : 0`,而註解說 3 是
    //   「進階」。這一輪 ui_layout.h 把 kPageNetwork 插在 kPageAdvanced
    //   **前面**(離線為預設的產品,那顆開關不該藏在最後一頁),於是 3
    //   變成了「連網」—— 出事時會把使用者帶到一頁沒有「重新整理字詞」
    //   的地方,而畫面上沒有任何東西看起來不對。
    //   check_ui_spec.sh 的 W30 現在擋著這件事。
    if (settings_)
      settings_->OpenAt(StateIsFailure(service_state_) ? kPageAdvanced
                                                       : kPageSchemas);
    return;
  }
  // ── ⚠ 在上一次「切一下」落地**之前**產生的點擊一律丟掉 ──────
  //
  //   回讀是阻塞的(它排在引擎佇列上,而佇列可以被部署或一顆慢按鍵
  //   佔住好幾秒 —— #93 / #103)。那段期間這條 UI 執行緒停著,使用者
  //   會再點、再點,而那些訊息在解除阻塞之後**一次全部**送進來。
  //   一下一次翻轉,N 下就是 N 次翻轉 —— 奇數次結束時引擎在 ASCII,
  //   而使用者只按了他以為的「一下」。使用者實機回報:
  //   「連點幾次之後變成完全打不出中文」。
  //
  //   ⚠ 冪等的對象是**意圖**,不是「點擊」:比的是訊息**產生**的時刻
  //     (GetMessageTime,與 GetTickCount 同一個時間基準),不是處理的
  //     時刻。使用者看到那一格變了之後再按,時間戳一定在後面,照常生效。
  //   ⚠ 只擋這兩格。方案選單與設定那兩格不碰引擎,擋它們只會讓那一橫
  //     在部署期間看起來壞掉。
  if (cell == kCellMode || cell == kCellVariant) {
    // ⚠ **無號相減,再轉有號。** 兩個都是 49.7 天翻轉的 32 位元計數器
    //   (GetMessageTime 與 GetTickCount 同一個時間基準)。先各自轉成
    //   有號再相減的話,跨過翻轉點的那一次是**有號溢位**(UB);
    //   在無號那一側相減是明文定義的環繞,轉回有號才得到帶正負號的差。
    //   實務上碰得到的機率趨近於零,而這個 codebase 的標準是把它寫下來。
    const DWORD produced = static_cast<DWORD>(::GetMessageTime());
    if (static_cast<LONG>(produced - toggle_settled_ms_) < 0) return;
  }
  switch (cell) {
    case kCellMode: {
      // ⚠ 這一格是這一輪最重要的一顆鍵。在它之前,Windows 使用者
      //   **完全沒有**中英切換 —— ascii_mode 從來沒有被設定過。
      //
      // ⚠ 方向從**引擎**的行程層級狀態算,不是從畫面上那個字算。
      //   拿畫面反推的話,只要畫面曾經與引擎不一致(而那正是這一輪在修
      //   的東西),再按一次送的就是同一個值 —— 使用者會覺得這一格
      //   只能往一個方向切。
      if (!engine_) return;
      engine_->SetAsciiModeAll(!engine_->AsciiMode());
      // ⚠ **不樂觀寫入。** 立刻回讀,由引擎說現在是什麼。
      RefreshFromEngine();
      toggle_settled_ms_ = ::GetTickCount();
      return;
    }
    case kCellVariant: {
      // ⚠ 這一格以前是**樂觀寫入**:點下去就自己翻,不等任何引擎的
      //   證據。當時的理由很實際 —— variant_ 唯一的更新路徑是
      //   OnSnapshot,而 OnSnapshot 要等使用者真的打一個字。但代價是
      //   那一橫可以顯示一個從來沒有發生過的狀態,而使用者回報的
      //   「畫面說简、打出來是繁」就是那個形狀的一種。
      //
      //   現在改成「送出去 → 立刻向引擎回讀」。回讀是證據,而且一樣
      //   不需要使用者先打一個字。
      bool now_simplified;
      {
        std::lock_guard<std::mutex> lock(mu_);
        // ⚠ **這裡讀到的 variant_ 不可能是 kHidden,而那是刻意的。**
        //
        //   舊註解寫的是「kHidden 當成現在不是簡體,所以點下去會切到
        //   簡體」。那段行為**永遠不會發生** —— 又一句替不會發生的事
        //   作證的註解。不可達是三行程式碼合起來的結果:
        //     1. kHidden → StatusBarCellTexts 給這一格**空字串**
        //        (common/status_cells.cc)
        //     2. 空字串 → Relayout 把它的矩形設成 {0,0,0,0}
        //        (本檔的 `if (c.text.empty())`)
        //     3. 零寬 → HitCell 跳過(`if (r.right <= r.left) continue;`)
        //   後兩行是 Win32、Ubuntu 上編不動,所以由 check_ui_spec.sh 的
        //   W26 在原始碼層面守著(它有反向測試 W26f / W26g)。
        //
        //   **而「點不到」是要的行為,不是缺陷。** kHidden 的意思是引擎
        //   沒有回報任何字形 —— 這個方案根本沒有那一組開關(第三方方案
        //   多半沒有),或使用者說了「簡繁我自己管」。那一格此刻不代表
        //   任何事實,所以整格不畫;一個畫不出來卻按得到的開關,按下去
        //   改變的是使用者看不見的東西,而且方向還是猜的 —— 那正是這一
        //   輪在拆的樂觀寫入,只是換了個位置。
        //
        //   使用者要改簡繁的路沒有斷:設定視窗的「文字」那一項。設過
        //   之後 DecideVariant 就會回真,選項真的被送到引擎,這一格也就
        //   有證據可以畫、可以點了。
        now_simplified = variant_ == VariantCell::kSimplified;
      }
      // 走設定視窗那一支,三條路(狀態列、系統匣、設定)共用同一份寫入 ——
      // 各寫一份會漂移,而漂移的症狀是「從這裡切有效、從那裡切無效」。
      if (settings_)
        settings_->SetVariantPref(now_simplified ? VariantPref::kTraditional
                                                 : VariantPref::kSimplified);
      RefreshFromEngine();
      toggle_settled_ms_ = ::GetTickCount();
      return;
    }
    case kCellSchema:
      OpenSchemaPopup();
      return;
    case kCellSettings:
      if (settings_) settings_->Open();
      return;
    default:
      return;
  }
}

// ── 方案清單:自繪的 top-level 小視窗 ──────────────────────────
//
// ⚠ **不是 TrackPopupMenu。** 它要先 SetForegroundWindow 才會在點外面時
//   正常關閉,而那就是搶焦點 —— 使用者正在打字的輸入框會失去插入點,
//   而「在句子中間改東西」正是這一橫存在的理由。用自己的小窗 + 滑鼠捕捉。

int StatusBar::PopupRowCount() const {
  // 還在讀的時候是一列 —— 那一列是**一句話**,不是一個方案。
  return popup_loading_ ? 1 : static_cast<int>(popup_items_.size());
}

void StatusBar::PlacePopup() {
  if (!popup_ || !hwnd_) return;
  const int row_h = Dip(metric::kRowH, dpi_);
  const int w = Dip(200, dpi_);
  const int h = row_h * PopupRowCount() + 2 * Dip(space::s2, dpi_);
  RECT bar{};
  ::GetWindowRect(hwnd_, &bar);
  const int x = bar.left + cells_[kCellSchema].rc.left;
  // 往上開;上面放不下就往下。⚠ 讀完之後列數會變,所以這一段要**重算**,
  //   不能只改高度 —— 往上開的選單長高之後,y 也要跟著往上移。
  int y = bar.top - h - Dip(space::s2, dpi_);
  if (y < 0) y = bar.bottom + Dip(space::s2, dpi_);
  ::SetWindowPos(popup_, HWND_TOPMOST, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
  // 方案彈出清單也是 top-level → 視窗級圓角 8(§12.14.4)。
  // ⚠ 同一支 helper,所以「DWM 或 region,不得同時」那一條在這裡也成立。
  ApplyWindowCorners(popup_, w, h, Dip(kBarRadius, dpi_));
}

void StatusBar::OpenSchemaPopup() {
  ClosePopup();
  if (!engine_) return;

  // ── ⚠ 這一支跑在懸浮那一橫自己的 UI 執行緒上 ────────────────────
  //
  //   舊版是 `SchemaListForUi(1500, ...)`:引擎慢的時候整條狀態列凍結
  //   1.5 秒(點不動也拖不動),逾時就**什麼都不做** —— 使用者按了
  //   一下沒事發生。而 BeginDeploy 會清快取,所以整個部署期間每一次按
  //   都走冷快取,而且每按一次就多排一件沒有人讀的工作。
  //
  //   現在這一支**完全不等**:快取有就直接開;沒有就立刻開一個
  //   說得出話的選單,背景查完由 WM_RIME_SCHEMAS_READY 換掉。
  popup_items_.clear();
  popup_loading_ = !engine_->SchemaListFromCache(&popup_items_);

  if (popup_loading_ && !schema_query_inflight_) {
    // ⚠ **已經有一件在飛就不要再排。** 這一格就是「無限累積」那一半:
    //   部署期間快取一直是冷的,而使用者會一直按。
    const DWORD tid = thread_id_;
    // ⚠ 這個 lambda 跑在**引擎執行緒**上。它只捕捉一個 DWORD(傳值),
    //   而且只做一件事 —— 送一則訊息回這一條執行緒。不碰任何成員。
    if (engine_->RefreshSchemaListAsync([tid] {
          if (tid) ::PostThreadMessageW(tid, WM_RIME_SCHEMAS_READY, 0, 0);
        }))
      schema_query_inflight_ = true;
    // 排不進去(引擎在停)= 沒有人會回來。旗標不設,所以下一次按會再試
    // 一次;選單上那句「正在讀方案…」仍然是實話。
  }

  popup_ = ::CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kPopupClass, L"",
      WS_POPUP, 0, 0, 10, 10, hwnd_, nullptr, ::GetModuleHandleW(nullptr),
      this);
  if (!popup_) return;
  popup_hot_ = -1;
  PlacePopup();
  // 滑鼠捕捉:點到外面就關。SetForegroundWindow 那條路會搶焦點,不能用。
  ::SetCapture(popup_);
}

void StatusBar::ClosePopup() {
  if (!popup_) return;
  if (::GetCapture() == popup_) ::ReleaseCapture();
  ::DestroyWindow(popup_);
  popup_ = nullptr;
  popup_hot_ = -1;
  popup_loading_ = false;
  // ⚠ schema_query_inflight_ **不在這裡清**:那件工作還在飛,而清掉
  //   等於下一次按又排一件。它只由 WM_RIME_SCHEMAS_READY 清。
}

void StatusBar::PaintPopup(HDC hdc) {
  RECT client{};
  ::GetClientRect(popup_, &client);
  HDC mem = ::CreateCompatibleDC(hdc);
  HBITMAP bmp = ::CreateCompatibleBitmap(hdc, client.right, client.bottom);
  HGDIOBJ old_bmp = ::SelectObject(mem, bmp);

  ::FillRect(mem, &client, theme_.Brush(kSurface));
  {
    HPEN pen = theme_.Pen(kOutline, Dip(kBarBorder, dpi_));
    HGDIOBJ oldp = ::SelectObject(mem, pen);
    HGDIOBJ oldb = ::SelectObject(mem, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(mem, client.left, client.top, client.right, client.bottom);
    ::SelectObject(mem, oldb);
    ::SelectObject(mem, oldp);
  }
  ::SetBkMode(mem, TRANSPARENT);
  HGDIOBJ oldf = ::SelectObject(mem, fonts_.Get(text_size::t3));

  const int row_h = Dip(metric::kRowH, dpi_);
  const int top = Dip(space::s2, dpi_);
  if (popup_loading_) {
    // ⚠ 覆核指出「狀態列現在沒有地方放字」,所以那句話放在**選單裡**。
    //   它是一句話不是一個方案:不畫 hover 底色,點下去也不會選到東西。
    RECT r{Dip(space::s2, dpi_), top, client.right - Dip(space::s2, dpi_),
           top + row_h};
    ::SetTextColor(mem, theme_.Color(kOnSurfaceVariant));
    RECT tr = r;
    tr.left += Dip(space::s4, dpi_);
    const wchar_t* msg = UiText(UiString::kStatusBarSchemaLoading);
    ::DrawTextW(mem, msg, -1, &tr,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
    ::SelectObject(mem, oldf);
    ::BitBlt(hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
    ::SelectObject(mem, old_bmp);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    return;
  }
  for (size_t i = 0; i < popup_items_.size(); ++i) {
    RECT r{Dip(space::s2, dpi_), top + static_cast<int>(i) * row_h,
           client.right - Dip(space::s2, dpi_),
           top + static_cast<int>(i + 1) * row_h};
    if (static_cast<int>(i) == popup_hot_)
      ::FillRect(mem, &r, theme_.Brush(kRowHover));
    ::SetTextColor(mem, theme_.Color(kOnSurface));
    RECT tr = r;
    tr.left += Dip(space::s4, dpi_);
    // ⚠ 只印名字,不印 id(§6.7 第一層)。名字為空才退回 id。
    const std::wstring name =
        Utf8ToWide(popup_items_[i].second.empty() ? popup_items_[i].first
                                                  : popup_items_[i].second);
    ::DrawTextW(mem, name.c_str(), -1, &tr,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
  }
  ::SelectObject(mem, oldf);
  ::BitBlt(hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
  ::SelectObject(mem, old_bmp);
  ::DeleteObject(bmp);
  ::DeleteDC(mem);
}

LRESULT CALLBACK StatusBar::PopupProc(HWND hwnd, UINT msg, WPARAM w,
                                      LPARAM l) {
  StatusBar* self =
      reinterpret_cast<StatusBar*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      break;
    }
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = ::BeginPaint(hwnd, &ps);
      if (self) self->PaintPopup(hdc);
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (!self) break;
      RECT c{};
      ::GetClientRect(hwnd, &c);
      const int y = GET_Y_LPARAM(l);
      const int row_h = Dip(metric::kRowH, self->dpi_);
      const int top = Dip(space::s2, self->dpi_);
      int hot = -1;
      if (GET_X_LPARAM(l) >= 0 && GET_X_LPARAM(l) < c.right && y >= top) {
        const int i = (y - top) / (row_h > 0 ? row_h : 1);
        if (i >= 0 && i < static_cast<int>(self->popup_items_.size())) hot = i;
      }
      if (hot != self->popup_hot_) {
        self->popup_hot_ = hot;
        ::InvalidateRect(hwnd, nullptr, TRUE);
      }
      return 0;
    }
    case WM_LBUTTONUP: {
      if (!self) break;
      POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      RECT c{};
      ::GetClientRect(hwnd, &c);
      const bool inside = pt.x >= 0 && pt.x < c.right && pt.y >= 0 &&
                          pt.y < c.bottom;
      // ⚠ 還在讀的時候,選單裡那一列是一句話 —— 點它**不關選單**。
      //   關掉的話使用者會以為自己點錯了,然後再按一次(而那一次
      //   仍然是冷快取)。讓他等在原地,清單一回來就換上去。
      if (inside && self->popup_loading_) return 0;
      const int pick = self->popup_hot_;
      const std::string id =
          (inside && pick >= 0 &&
           pick < static_cast<int>(self->popup_items_.size()))
              ? self->popup_items_[pick].first
              : std::string();
      // ⚠ 這裡以前也是樂觀寫入(直接把 schema_name_ 寫成選單上的名字)。
      //   當時的理由是「不寫的話,選完方案那一格上還印著舊名字,使用者
      //   會以為沒選到、再選一次」。
      //
      //   但方案名與簡繁不一樣:它是**選單上那一項自己的字面**,不是
      //   一個我們推測出來的引擎狀態 —— 使用者點的就是這一項。真正
      //   不誠實的是「選了但其實沒選成功」,而那件事現在由
      //   SelectSchemaAll 之後的回讀來收:方案名照舊先寫上去(它是使用者
      //   剛剛點的東西),中英與簡繁則等引擎回話 ——
      //   換方案會把 switches 重設,那一格必須跟著動。
      const std::string picked_name =
          (inside && pick >= 0 &&
           pick < static_cast<int>(self->popup_items_.size()))
              ? self->popup_items_[pick].second
              : std::string();
      self->ClosePopup();
      if (!id.empty() && self->engine_) {
        if (!picked_name.empty()) {
          std::lock_guard<std::mutex> lock(self->mu_);
          self->schema_name_ = picked_name;
        }
        self->engine_->SelectSchemaAll(id);
        // ⚠ 換方案之後那一格**一定會變**(SelectAndApply 會重套簡繁,
        //   而新方案宣告的字形也可能不同)。不回讀的話,那一格會停在
        //   換方案之前的字面,直到使用者打一個字。
        self->RefreshFromEngine();
        self->Relayout();
        ::InvalidateRect(self->hwnd_, nullptr, TRUE);
      }
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (self && self->popup_) self->ClosePopup();
      return 0;
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, w, l);
}

// ─────────────────────────── 訊息 ───────────────────────────

LRESULT CALLBACK StatusBar::WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  StatusBar* self =
      reinterpret_cast<StatusBar*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      break;
    }
    // ⚠ 點它不可以啟動它:搶焦點會讓使用者正在打字的輸入框失去插入點 ——
    //   而「在句子中間切中英」正是這一橫存在的理由。
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = ::BeginPaint(hwnd, &ps);
      if (self) self->Paint(hdc);
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (!self) break;
      POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      if (self->dragging_) {
        // §12.10.3 的拖動:位移超過系統的拖動門檻才算拖,
        // 否則當作點擊那一格。⚠ SM_CXDRAG 是使用者可調的,不要寫死 4 px。
        const int dx = pt.x - self->drag_start_.x;
        const int dy = pt.y - self->drag_start_.y;
        if (!self->drag_moved_ &&
            (std::abs(dx) > ::GetSystemMetrics(SM_CXDRAG) ||
             std::abs(dy) > ::GetSystemMetrics(SM_CYDRAG)))
          self->drag_moved_ = true;
        if (self->drag_moved_) {
          POINT screen = pt;
          ::ClientToScreen(hwnd, &screen);
          RECT rc{};
          ::GetWindowRect(hwnd, &rc);
          ::SetWindowPos(hwnd, HWND_TOPMOST, screen.x - self->drag_start_.x,
                         screen.y - self->drag_start_.y, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;
      }
      const int hot = self->HitCell(pt);
      if (hot != self->hot_) {
        self->hot_ = hot;
        ::InvalidateRect(hwnd, nullptr, TRUE);
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        ::TrackMouseEvent(&tme);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      if (self && self->hot_ != -1) {
        self->hot_ = -1;
        ::InvalidateRect(hwnd, nullptr, TRUE);
      }
      return 0;
    case WM_LBUTTONDOWN: {
      if (!self) break;
      self->drag_start_ = POINT{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      // ⚠ 從這一刻起視窗的位置是**使用者的手**在決定的(WM_MOUSEMOVE
      //   直接 SetWindowPos,不走 ApplyPlacement),所以避讓的那一段
      //   不再成立。不歸零的話 SavePlacement 會把它多扣一次。
      self->nudge_dy_ = 0;
      self->dragging_ = true;
      self->drag_moved_ = false;
      self->pressed_ = self->HitCell(self->drag_start_);
      ::SetCapture(hwnd);
      ::InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }
    case WM_LBUTTONUP: {
      if (!self) break;
      const bool moved = self->drag_moved_;
      const int cell = self->pressed_;
      self->dragging_ = false;
      self->drag_moved_ = false;
      self->pressed_ = -1;
      if (::GetCapture() == hwnd) ::ReleaseCapture();
      ::InvalidateRect(hwnd, nullptr, TRUE);
      if (moved) {
        // 拖過就存位置,**不觸發那一格** —— 拖完順便切了中英,
        // 使用者會以為輸入法自己亂跳。
        self->SavePlacement();
      } else if (cell >= 0) {
        self->ClickCell(cell);
      }
      return 0;
    }
    case WM_TIMER: {
      // ⚠ 部署做完之後那一橫要**自己**從一句話變回四格。
      //   以前要等使用者去某個輸入框打一個字才會變 —— 而他看到的是
      //   「還沒好」,所以他在等。兩邊互相等著對方先動。
      //   ⚠ 只在狀態**真的變了**時重排重畫:每半秒無條件重繪一次
      //     會讓一個永遠在螢幕上的視窗一直閃。
      if (!self || w != kStateTimer) break;
      const ServiceState now = self->CurrentServiceState();
      if (now != self->service_state_) {
        self->Relayout();
        ::InvalidateRect(hwnd, nullptr, TRUE);
      }
      // ⚠ 待隱藏的到期就在這裡收 —— **不新增計時器**。這一顆本來就是
      //   半秒一次,而 3000 毫秒的遲滯用半秒的解析度綽綽有餘。
      //   多一顆計時器就多一條要對齊的時序。
      self->EvaluateVisibility();
      // ⚠ 順便保鮮:引擎那一側可能被別的路徑改了(設定視窗、系統匣、
      //   方案自己的按鍵),而那些路徑不會通知這一橫。半秒問一次,
      //   沒變就什麼都不做(RefreshFromEngine 自己會比對)。
      self->RefreshFromEngine();
      return 0;
    }
    case WM_DPICHANGED: {
      if (!self) break;
      self->dpi_ = HIWORD(w);
      self->fonts_.Reset(self->dpi_, Script::kHant);
      // 取建議矩形的**位置**,尺寸自己依新 DPI 重算 —— 它是固定 DIP
      // 尺寸的小窗,建議尺寸對它沒有意義。
      RECT* sug = reinterpret_cast<RECT*>(l);
      if (sug)
        ::SetWindowPos(hwnd, HWND_TOPMOST, sug->left, sug->top, 0, 0,
                       SWP_NOSIZE | SWP_NOACTIVATE);
      self->Relayout();
      ::InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }
    case WM_SETTINGCHANGE:
      if (self && (Theme::IsColorSetChange(l) || w == SPI_SETHIGHCONTRAST)) {
        self->RefreshTheme();
        // 工作區可能變了(工作列改大小)—— 重新夾一次位置。
        self->ApplyPlacement(0);
      }
      break;
    case WM_DISPLAYCHANGE:
      // ⚠ 螢幕拔掉了。不重新定位的話,那一橫會留在一個不存在的座標上,
      //   而症狀是「它不見了」。
      if (self) self->ApplyPlacement(0);
      return 0;
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, w, l);
}

}  // namespace rimewin
