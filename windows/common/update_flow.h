// windows/common/update_flow.h — 線上更新的**流程與失敗分類**(純邏輯)
//
// 對照實作:android/.../update/UpdateController.kt 與 InstallFailure.kt。
//
// ══ Windows 的更新到底做了什麼 ═══════════════════════════════════
//
// **我們不自己換檔案。** 下載回來的是與下載頁上完全同一支
// `LuminaKey-Setup-x64-*.exe`,驗過摘要之後交給它,它自己會跳系統管理員
// 的確認視窗(那是它 manifest 裡的 requireAdministrator,不是我們去提權
// 的 —— 見 common/elevation_policy.h 檔頭:從提權的進程生出來的服務會
// 繼承提權的權杖,然後把使用者的檔案擁有者換成不對的人)。
//
// 這條路徑不是省事,是**它已經被驗過一輪**:
//
//   · 「不重開機、不登出」的機制(把正被載入著的 rime_tsf.dll 改名挪開,
//     而不是原地覆蓋)寫在 installer/luminakey.iss 的 PrepareToInstall,
//     由 windows/verify_installer.sh §13 在真的 Windows 上斷言 ——
//     包括「升級沒有把**任何東西**排進開機佇列」。
//   · 停服務、註冊、使用者那一側的啟用,全部在同一支安裝程式裡。
//
// 自己寫一套換檔邏輯 = 這些關卡一條都不涵蓋它。
//
// ══ 交棒之後我們就死了,所以結果要「事後和解」════════════════════
//
// ⚠ 安裝程式做的第一件事就是停掉 rime_service.exe —— 也就是**跑這段
//   程式的這個進程**。所以「等安裝程式結束、看它的結束碼」在這裡不成立:
//   我們等不到。
//
// 因此:交棒之前先寫一張**交棒單**(哪一版、什麼時候、檔案在哪),
// 服務下一次啟動時把它讀回來,拿「現在裝的是哪一版」跟它對:
//
//   對上了            → 更新成功,說一句,把檔案清掉。
//   沒對上 + 開機佇列裡有安裝目錄底下的東西
//                     → **換檔被拒**:有程式握著檔案。說出來,不排重開機。
//   沒對上 + 佇列乾淨 → 沒裝成,而我們不知道為什麼。**照實說**。
//
// 見 ReconcileHandoff。第二種正是使用者最需要一句人話的那一格:
// 症狀是「按了更新,什麼都沒變」。
//
// ══ 每一種失敗都要有自己的一句話 ═════════════════════════════════
//
// 這一條是這個檔案存在的主要理由(對應 Windows 端已知的缺陷:
// 三種不同的失敗在畫面上是同一句紅字)。UpdateFailureText 對每一格回
// 一條**不同**的 UiString,而 test_update_flow.cc 逐對比對 —— 有人把兩格
// 併成同一句,測試會指名是哪兩格。
//
#ifndef RIMEWIN_UPDATE_FLOW_H_
#define RIMEWIN_UPDATE_FLOW_H_

#include <cstdint>
#include <string>

#include "net_gate_core.h"
#include "ui_strings.h"
#include "update_manifest.h"

namespace rimewin {

enum class UpdateStage {
  kIdle = 0,
  kChecking,
  kDownloading,
  kVerifying,
  kReady,       // 下載完、摘要對得上,等使用者按下更新
  kHandedOff,   // 安裝程式已經叫起來了;這個進程隨時會被它停掉
};

// ⚠ 加一格的時候:UpdateFailureText 與 UpdateFailureTag 各要補一行,
//   否則編不過(switch 沒有 default,並且有 static 斷言涵蓋)。
//   「忘了處理」在這裡的代價是使用者拿到別人的錯誤訊息。
enum class UpdateFailure {
  kNone = 0,
  kSwitchOff,             // 連網開關是關的。**沒有連出去,也沒有記錄**
  kUnreachable,           // 取不到版本資訊
  kUnreadable,            // 取到了,但讀不懂
  kOwnVersionUnknown,     // 查不出本機是哪一版(舊安裝沒有 version.txt)
  kDownloadInterrupted,   // 下載中斷(含中途被開關擋下)
  kDownloadTooLarge,      // 收到的比宣告的大
  kSha256Mismatch,        // 摘要不符。**唯一可以說「檔案壞了」的那一格**
  kStagingWriteFailed,    // 存不下來(磁碟滿、被握住)
  kProductChanged,        // 線上那一版換了 AppId,裝了會多一套
  kBusyCannotStop,        // 正在整理字詞,現在停不下來
  kElevationDeclined,     // 系統管理員確認視窗按了否
  kHandoffFailed,         // 叫不起安裝程式
  kFileLocked,            // 沒裝成,而且開機佇列裡有安裝目錄底下的東西
  kNotInstalled,          // 沒裝成,原因不明。**照實說,不要猜**
  kFailureCount,
};

// 穩定的 ASCII 標籤。給記錄檔與驗證腳本用 —— 腳本不該去比對中文句子,
// 那種比對會在措辭改一個字的時候安靜地失效。
const char* UpdateFailureTag(UpdateFailure f);
// 給使用者的那一句。⚠ 每一格都不同,見檔頭。
UiString UpdateFailureText(UpdateFailure f);
// 再按一次有機會成功嗎(決定要不要給重試)。
bool UpdateFailureCanRetry(UpdateFailure f);
// 使用者要做的事是「打開連網開關」嗎。**只有 kSwitchOff 是 true** ——
// 把「連不上伺服器」也導向開關,會讓一個網路問題看起來像我們擋的。
bool UpdateFailureNeedsSwitch(UpdateFailure f);

// ── 網路結果 → 失敗分類 ──────────────────────────────────────
//
// ⚠ kBlocked 一定要與其他失敗分得開:「連不上」給重試,「開關是關的」
//   給一顆開啟開關的按鈕。壓成同一句紅字正是這一輪在修的東西。
UpdateFailure ClassifyManifestFetch(NetResult r);
UpdateFailure ClassifyDownload(NetResult r);

// ShellExecuteEx 的結果。使用者在系統管理員確認視窗按「否」時,Windows
// 回的是 ERROR_CANCELLED —— 那**不是**失敗,是他改變主意,訊息要不一樣。
constexpr unsigned long kWin32ErrorCancelled = 1223;
UpdateFailure ClassifyHandoff(bool shell_ok, unsigned long win32_error);

// ── 交棒前的最後一道 ─────────────────────────────────────────
struct HandoffPreflight {
  bool installed_version_known = false;
  bool have_manifest = false;
  UpdateVerdict verdict = UpdateVerdict::kUpToDate;
  AppIdVerdict app_id = AppIdVerdict::kUnknown;
  bool file_present = false;
  // ⚠ **這一格是硬條件。** 沒有它為 true,底下一律回 false ——
  //   不管其他每一格長什麼樣。test_update_flow.cc 把整個布林立方體
  //   跑過一遍來釘住這句話。
  bool sha256_verified = false;
  // 引擎正在整理字詞。中途被安裝程式停掉會留下半份資料,
  // 而症狀是「更新之後我自己加的詞不見了」。
  bool deploy_running = false;
};

// 現在可不可以把控制權交給安裝程式。
//
// @param why 回 false 時填入原因;「沒有東西要裝」不是錯誤,填 kNone。
bool MayHandOff(const HandoffPreflight& pre, UpdateFailure* why);

// ── 交棒之後的和解(服務下一次啟動時)───────────────────────
enum class UpdateOutcome {
  kNoHandoff = 0,  // 沒有交棒單。正常啟動
  kInstalled,      // 版本對上了
  kNotInstalled,   // 沒對上。why 說明是哪一種
};

// @param have_record          交棒單存在嗎
// @param handed_code          交棒時要裝的那一版
// @param installed_code       現在裝的是哪一版(0 = 查不出來)
// @param queued_in_install_dir 開機佇列裡有沒有安裝目錄底下的東西
UpdateOutcome ReconcileHandoff(bool have_record, int64_t handed_code,
                               int64_t installed_code,
                               bool queued_in_install_dir, UpdateFailure* why);

// ── 畫面上那張卡片 ───────────────────────────────────────────
struct UpdateUiState {
  UpdateStage stage = UpdateStage::kIdle;
  UpdateFailure failure = UpdateFailure::kNone;
  bool network_enabled = false;
  bool have_manifest = false;
  UpdateVerdict verdict = UpdateVerdict::kUpToDate;
  AppIdVerdict app_id = AppIdVerdict::kUnknown;
  bool file_verified = false;
  bool installed_version_known = true;
};

// kUiStringCount = 這一格不顯示。
struct UpdateCard {
  UiString status = UiString::kUiStringCount;
  // ⚠ **永遠是 kUpdateTrustAnchor,一格都不准是別的。** 沒有程式碼簽章
  //   這件事不是「錯誤狀態才要說的話」;使用者在按下更新**之前**就要看到。
  //   DescribeUpdateCard 的每一條路徑都經過測試斷言這一點。
  UiString trust = UiString::kUpdateTrustAnchor;
  // 主要動作按鈕的字面。kUiStringCount = 這一格沒有主要動作。
  //
  // ⚠ 刻意分成**兩下**:第一下下載並核對,第二下才交給安裝程式。
  //   交棒等於這個視窗與輸入法會消失一下(安裝程式做的第一件事就是停掉
  //   我們),那不該是一顆「下載」按鈕的副作用 —— 使用者要在看過
  //   trust 那一句、而且知道接下來會發生什麼之後,自己按下第二下。
  UiString action = UiString::kUiStringCount;
  bool show_check_button = true;
  bool show_page_button = false;
  bool busy = false;

  bool has_action() const { return action != UiString::kUiStringCount; }
};
UpdateCard DescribeUpdateCard(const UpdateUiState& s);

}  // namespace rimewin

#endif  // RIMEWIN_UPDATE_FLOW_H_
