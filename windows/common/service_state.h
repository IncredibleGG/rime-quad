// windows/common/service_state.h — 「現在到底怎麼了」只有這裡判斷
//
// ── 這個檔案存在的理由是一句謊話 ────────────────────────────────
//
//   使用者第一次安裝完、輸入法正在整理字詞的那 7~12 秒裡,
//   螢幕右下角那一橫寫著紅色的**「輸入法沒有在跑」**。
//
// 那句話是假的。輸入法**正在跑**,它只是還沒準備好。而那 7~12 秒
// 剛好是這個產品最貴的一段 —— 第一次安裝的觀感。看到那句話的人
// 會以為自己裝壞了:去解除安裝重裝(再看一次同一句),或者去按
// 「重新整理字詞」(那只是再等一次,而且把剛剛做完的工作重做)。
//
// 舊的判斷是一個布林:
//
//     service_ready_ = engine_ && engine_->deploy_done() && engine_->deploy_ok();
//
// 三種完全不同的處境被壓成同一個 false,於是畫面上是同一句紅字:
//
//   1. **還在準備**    一切正常,再等一下就好。
//                      ⚠ 這不是錯誤,所以不畫紅字、不說「沒有在跑」。
//   2. **準備失敗**    要看原因(多半是缺檔)。使用者做得到的動作是
//                      到「進階」按「重新整理字詞」。
//   3. **引擎不在**    要重新安裝,或者是提權政策把服務擋下來了。
//
// 三種要說三句不同的話,而且第 1 種那一句不可以說謊。
//
// ── ⚠ 為什麼是純函式,而且住在 common/ ──────────────────────────
//
// `windows/service/` 底下的東西在開發機(Ubuntu)上編不起來 —— 它們
// include windows.h。所以「三種處境 → 三句話」如果留在繪製碼裡,
// 它就只有真人在 Windows 上才驗得到,也就是**實際上沒有人驗**。
// 而這個專案剛剛才被「守門綠著卻抓不到它宣稱抓的東西」咬過一次。
//
// 放在這裡之後,windows/tests/test_service_state.cc 逐一驗得到:
// 三種輸入 → 三個狀態 → 三句**互不相同**的話。把它們合併回同一句
// (不管是讓對照表全回同一條,還是把三條 catalog 條目寫成同樣的字)
// 那個測試都會紅。
//
#ifndef RIMEWIN_SERVICE_STATE_H_
#define RIMEWIN_SERVICE_STATE_H_

#include <cstdint>

#include "ui_strings.h"

namespace rimewin {

// ⚠ 四個值,不是「就緒 / 沒就緒」兩個。把它縮回布林就是這個缺陷本身。
enum class ServiceState {
  // 可以打字。這時那一橫畫的是四格,不是一句話。
  kReady,
  // 正在準備(首次整理字詞,或使用者按了「重新整理字詞」)。
  // ⚠ 輸入法**在跑**。這一種不算失敗。
  kPreparing,
  // 準備有了結果,而結果是失敗。
  kPrepareFailed,
  // 引擎根本不在 —— 服務進程沒起來。
  kNotRunning,
};

constexpr int kServiceStateCount = 4;

// 判斷所需要的全部事實。刻意攤平成四個布林:呼叫端各自去問自己拿得到的
// 來源(服務進程問引擎、狀態列多一份線路上的旗標),判斷只有一份。
struct EngineFacts {
  // 有沒有一個引擎物件。false = 服務進程沒起來。
  bool engine_present = false;
  // 準備有沒有終局結果(還沒有 = 正在準備)。
  bool deploy_done = false;
  // 有結果而且是成功。
  bool deploy_ok = false;
  // ⚠ **引擎自己在線路上說「我還沒準備好」**(protocol.h 的 kStDisabled)。
  //
  //   這一格不是多餘的。deploy_done 只會從 false 變成 true 一次,
  //   之後**永遠**是 true —— 使用者按「重新整理字詞」時它不會退回去。
  //   所以少了這一格,重新整理的那幾十秒裡那一橫仍然畫著四格,
  //   而每一顆按鍵其實都被引擎回「沒處理」:一個在說謊的指示器。
  //
  //   線路上這個旗標本來就有(engine.cc 兩處回填),而在這一輪之前
  //   整個 windows/ 沒有任何一處讀它。
  bool engine_says_not_ready = false;
};

// 三種處境 → 三個狀態。**唯一**的判斷點。
ServiceState ServiceStateOf(const EngineFacts& facts);

// 線路上那份快照有沒有帶「引擎還沒準備好」。
bool SnapshotSaysNotReady(uint32_t status_flags);

// 這份快照的其餘旗標(中英、簡繁)能不能拿來更新指示器。
//
// ⚠ 不能的那一種是真的會發生:引擎在準備期間對每一顆按鍵回的是一份
//   **預設建構**的快照 —— 除了 kStDisabled 以外全是 0。拿它去更新的話,
//   使用者剛切成 En 的指示器會被打回「中」,而他沒有碰過任何開關。
bool SnapshotFlagsAreUsable(uint32_t status_flags);

// 懸浮狀態列上那一句。
// ⚠ kReady 回 kUiStringCount(= 沒有那一句):就緒時畫的是四格。
UiString StatusTextFor(ServiceState state);

// 設定視窗側欄底部那一行。四種各有一句(就緒那一句是「可以打字」)。
UiString SidebarStatusTextFor(ServiceState state);

// 要不要當成錯誤畫(紅色外框、紅字)。
// ⚠ kPreparing **不是**錯誤。把準備中畫成紅的,就是用顏色再說一次那句謊話。
bool StateIsFailure(ServiceState state);

// 那一橫要不要畫四格。只有真的就緒才畫 —— 其餘三種狀態下那四格是假的。
bool StateShowsCells(ServiceState state);

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_STATE_H_
