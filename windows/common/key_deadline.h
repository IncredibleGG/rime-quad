// windows/common/key_deadline.h — 一顆按鍵的兩個上限,以及逾時那一份的去處
//
// ── 為什麼這兩個數字必須住在同一個檔案 ──────────────────────────
//
// 一顆按鍵有兩個上限,而它們在兩個不同的二進位檔裡:
//
//   · 服務端:引擎那條佇列等多久(kKeyDeadlineMs)
//   · DLL 端:IpcClient 等服務多久(kKeyTimeoutMs)
//
// 它們之間有一條硬關係:**服務端必須先放棄**,而且要留得下管道往返與
// 序列化的時間。反過來的話,DLL 先逾時 —— 而 ipc_client.cc 的 Fail()
// 第一句就是 Close():整條連線被丟掉、session_ 歸零,那個宿主接下來要
// 重連、重建 session(實測 442~753ms)、重套方案。一顆慢鍵於是變成
// 一整段打不出中文。
//
// 上一輪這兩個數字各寫在各自的檔案裡(engine.h 的 35 / ipc_client.cc 的
// 50),而它們的關係只寫在註解裡。**註解守不住東西**:兩邊各有一個人
// 各調一次,關係就沒了,而兩邊都還是綠的。所以搬到這裡,用 static_assert
// 守 —— 服務端與 DLL 端都 include 這一份,任何一邊破壞關係都編不過,
// 而 windows/tests/test_key_deadline.cc 讓同一條在 Ubuntu 上也跑得到
// (windows/service/ 與 windows/tsf/ 在開發機上編不起來)。
//
// ── ⚠ 為什麼不是 35 / 50 了 ─────────────────────────────────────
//
// 35 與 50 之間有一段**迴歸帶**。引擎工作落在 35–50ms 的那些按鍵:
//
//   · 在服務端還沒有上限的時候 —— DLL 等到 50ms,而工作 40ms 就回來了,
//     使用者打出**正確的中文**。
//   · 加上 35ms 的上限之後 —— 服務端先放棄、回 handled=false,DLL 把一個
//     英文字母插進使用者的文件。
//
// 也就是說,為了保護連線而新增的那道門,自己造出了一段「本來會對、
// 現在會錯」。而那一段不是邊角:使用者有 13 個宿主,Alt+Tab 過去打第一個
// 字就會排一次 SESSION_NEW,而 SESSION_NEW 的 ApplyChoice 實測 442~753ms。
//
// 上限的目的是**不讓一件慢工作丟掉整條連線**,不是把等待壓到最短。
// 所以兩個數字一起放寬:
//
//   · DLL 端 150ms —— 這是**一趟管道往返**最壞會停住宿主 UI 執行緒的
//     時間(不是一顆鍵的全部,見下面 kKeyHostStallWorstMs)。它換到的是
//     那一顆鍵打出正確的中文。相對地,50ms 換到的是一個錯的英文字母
//     **加上**一次斷線,而斷線之後那個宿主要重跑一次 SESSION_NEW
//     (它自己的預算就是 300ms,而且實測會到 442~753ms)—— 代價比多等
//     的那 100ms 大得多。
//   · 服務端 100ms —— 150 扣掉餘裕。
//
// ⚠ 這兩個值都是**推算**出來的,不是量出來的最佳值 —— 與它們取代的
//   35/50 完全一樣。真正量得到的東西是 pipe_server.cc 那一行 KEY_MS
//   (每條連線最多 20 行),要再調先看它。
//
// ⚠ 放寬**不能**取代作廢權。150ms 之後仍然會有逾時(ApplyChoice 那一種
//   慢工作是它的好幾倍),而那時 engine.cc 的 CallAbandonable 仍然是
//   「引擎組了字、宿主也打了字」唯一的擋板。
#ifndef RIMEWIN_KEY_DEADLINE_H_
#define RIMEWIN_KEY_DEADLINE_H_

namespace rimewin {

// DLL 端**一趟**管道往返的預算(tsf/ipc_client.cc)。
// 超過它 = 放棄 + 關掉連線,不是「這顆鍵慢了」。
constexpr int kKeyTimeoutMs = 150;

// 服務端等引擎執行緒的上限(service/engine.cc 的 CallKeyBounded)。
constexpr int kKeyDeadlineMs = 100;

// 留給管道往返 + 序列化 + 兩邊排程抖動的餘裕。
// ⚠ 這是**下限**,不是估計值:服務端放棄之後還要把回覆寫進管道,
//   而那一段若吃掉的時間超過餘裕,DLL 仍然會先逾時 —— 那正是這整個
//   關係要防的事。
constexpr int kKeyPipeMarginMs = 40;

// 慢鍵記錄的門檻(pipe_server.cc 的 KEY_MS)。
// ⚠ 低於它的按鍵不值得寫一行,而理由**不是**「會被捲掉」:service.log
//   沒有任何一行會在跑的期間被捲掉(main.cc 的大小檢查是重新開檔,
//   不是環形緩衝)。理由是它與同一份檔案裡別的行**搶額度也搶注意力**
//   —— 每一顆鍵都寫一行的話,那一行自己就是雜訊。
constexpr int kKeySlowLogMs = 30;

// ── 一顆按鍵在宿主 UI 執行緒上最壞停住多久 ──────────────────────
//
// ⚠ 它**不是** kKeyTimeoutMs,而上一輪這裡就是這樣寫的、還有一條
//   CHECK_MSG 在守那個假保證(「≤200ms」)。真正的路是**兩趟**:
//
//   1. IpcClient::Exchange —— 送出這顆鍵並等回覆。
//      ⚠ 它以前是 WriteAllTimed(timeout) **加上** ReadFrameTimed(timeout),
//        兩段各拿一份完整預算;而 WriteAllTimed 在 while 迴圈裡**每一塊**
//        都重新給一次 timeout,連絕對上限都沒有。現在兩段共用一個
//        **絕對 deadline**(見 ipc_client.cc),所以一趟 ≤ kKeyTimeoutMs。
//   2. TextService::HandleKey 收尾時的 SendCaretRect —— 走 SendOneWay,
//      那是**另一趟**寫入,再拿一份完整預算。
//
// 所以最壞是 kKeyTimeoutMs × kKeyPipeTripsPerKey。把它寫成一個具名常數
// 而不是註解裡的一句話,是因為下一個人會去調 kKeyTimeoutMs ——
// 調到 200 的時候真正的最壞是 400ms,而只有這個算式看得到那件事。
//
// ⚠ 這個數字只涵蓋**連線已經建立**的那條路。連線要重建時的成本另計,
//   由 common/link_state.h 的退避管,而那條路上的按鍵是 fail-open 的。
constexpr int kKeyPipeTripsPerKey = 2;
constexpr int kKeyHostStallWorstMs = kKeyTimeoutMs * kKeyPipeTripsPerKey;

// ── 這幾條就是「註解守不住」的那個關係 ──────────────────────────
static_assert(kKeyDeadlineMs < kKeyTimeoutMs,
              "服務端必須先放棄。反過來的話 DLL 會先 Fail() → Close(),"
              "一顆慢鍵的代價從『這顆鍵沒打出來』變成『整條連線被丟掉』");
static_assert(kKeyDeadlineMs + kKeyPipeMarginMs <= kKeyTimeoutMs,
              "服務端放棄之後還要把回覆送回去。餘裕不夠等於沒有先放棄");
static_assert(kKeySlowLogMs < kKeyDeadlineMs,
              "慢鍵的門檻要低於上限,否則只有逾時的那些會被記下來,"
              "而『快要逾時』正是最需要看到的那一段");
static_assert(kKeyHostStallWorstMs <= 300,
              "一顆鍵在宿主 UI 執行緒上最壞停住的時間 = 一趟的預算 × 趟數。"
              "超過 300ms 之後,使用者會以為程式沒收到而再按一次");

// ── 一顆按鍵在服務端只有**一份**預算 ────────────────────────────
//
// ⚠ 這一支存在的理由:`case Op::kKey` 那條路不是每一種都只走一趟引擎
//   佇列。簡繁快捷鍵(Ctrl+Shift+F)要走兩趟 —— 先回讀狀態,再取快照。
//   兩趟各給 kKeyDeadlineMs 的話,最壞是 200ms,而 DLL 那側只有 150ms:
//   服務端不再「先放棄」,上面那條 static_assert 守的關係在**執行期**
//   被破壞掉,而它一個字都看不到。
//
// 所以呼叫端記下這顆鍵是什麼時候開始的,每一趟都問一次「還剩多少」。
// 剩 0 = 不必再排進佇列了,直接當成逾時 —— 排進去只會讓那件工作
// 遲到之後撞上已經放棄的呼叫端。
constexpr int RemainingKeyBudgetMs(int spent_ms) {
  return spent_ms >= kKeyDeadlineMs ? 0 : kKeyDeadlineMs - spent_ms;
}

// ── 逾時的那一份**不是快照** ────────────────────────────────────
//
// Engine::ProcessKey 逾時時,工作本體多半一步都沒跑(作廢成功)——
// 引擎那邊的組字狀態原封不動。它回的那個 Result 只是一個佔位,用來讓
// DLL 知道「這顆鍵我們沒處理」,**不是**引擎的現況。
//
// 把它餵進 UI 會發生兩件事,而且兩件使用者都當場看得到:
//
//   · 它的 items 是空的 → pipe_server 的 push_ui 會 ui_->Hide():
//     使用者組字組到一半,候選窗當場收掉,而引擎那邊組字還在。
//     畫面與引擎從此分岔,而分岔之後每一顆鍵都更難解釋。
//   · 它的 status_flags 也會被那一橫讀走 → 一個健康的引擎在狀態列上
//     自稱「正在準備字詞」,而那一格正好會誘導使用者去按「重新整理
//     字詞」—— 一件真的要花幾十秒、而且會再排一批慢工作進同一條 FIFO
//     的事。(⚠ 上一輪這裡寫成「那一格沒有任何一條路清得回來」,
//     **那是錯的**:status_bar.cc 是 engine_not_ready_.exchange(算出來的值),
//     下一份不帶 kStDisabled 的快照就會把它清成 false。真正的代價是
//     「它會一直說謊到下一顆成功的按鍵為止」,而一顆鍵按下去沒反應
//     之後,人的下一個動作正好是停手。)
//
// ── 而「不碰 UI」不只逾時那一種 ──────────────────────────────────
//
// 同一支 pipe_server.cc 在「使用者把輕點 Shift 關掉了」那一格立過同一條
// 規矩(原話:什麼都不做包含**不碰 UI**),而簡繁快捷鍵「什麼都沒做」
// 那一格(ToggleVariantPref 回 false:引擎回讀不到,或方案根本沒宣告
// 字形開關)在它下面 25 行 —— 那一格照樣 push_ui 一份**預設建構**的
// 快照:items 空的 → 候選窗被收掉;旗標全 0 → SnapshotFlagsAreUsable()
// 是 true(它只看 kStDisabled),於是那一橫把中/英寫成「中」、簡繁那格
// 整個消失,而引擎可能正在英數模式、什麼都沒變。
//
// 所以判準要問的不是「有沒有逾時」,是**這一份 Result 是不是引擎的現況**。
enum class KeyUiAction {
  // 這一份是引擎的現況:照常餵候選窗與那一橫。
  kUpdateUi,
  // 這一份不是現況:UI 一個像素都不准動。
  kLeaveUiAlone,
};

// timed_out        = 等不到引擎(佔位)。
// result_is_current = 引擎那條佇列真的把工作跑完了,而 snap 是跑完之後
//                     取的那一份。⚠ 它**不是** handled:引擎不吃這顆鍵
//                     (英數模式下的字母)時 handled=false,但快照仍然是
//                     現況,候選窗與那一橫該照著更新。
constexpr KeyUiAction DecideKeyUiAction(bool timed_out, bool result_is_current) {
  return (!timed_out && result_is_current) ? KeyUiAction::kUpdateUi
                                           : KeyUiAction::kLeaveUiAlone;
}

// ── 簡繁快捷鍵那條路:那件不可逆的事排在**最後一趟** ────────────────
//
// `case Op::kKey` 上的簡繁快捷鍵(Ctrl+Shift+F)在服務端要走**兩趟**引擎
// 佇列,而其中一趟是有副作用的:PipeServer::ToggleVariantPref() 回 true 的
// 當下,簡繁**已經真的換掉了** —— settings_window_->SetVariantPref() →
// PostMessage(WM_RIME_SET_VARIANT) → CommitVariantPref() → store_->Save() +
// engine_->ApplyVariantAll()(settings_window.cc:2446 / :770 / :2425;hwnd_ 在
// ThreadMain 開機就建好,不是開過設定才有,所以這條路一直是通的)。
//
// ── 上一輪的順序造出來的東西 ────────────────────────────────────
//
// 順序是「先切、再取快照」,而取快照那一趟在這一輪被加上了上限。逾時的
// 那一份是預設建構的 Result,handled 留 false → 送回 DLL →
// tsf/text_service.cc 的 `if (!r.handled) { … return false; }` →
// `*eaten = FALSE` → OnPreservedKey 回 FALSE → **TSF 把那顆鍵交給宿主**。
//
// 使用者按一下的結果是:簡繁在背後換了,**同時**宿主的 Ctrl+Shift+F 也開了
// (VS Code 的跨檔搜尋、Word 的字型對話框、檔案總管的新資料夾)。
//
// ⚠ main 上不存在這個狀態 —— 當時第二趟是 Engine::Post() = 永遠等,handled
//   永遠是 true。**是「替第二趟加上限」造出來的**,而觸發條件正好是這一輪
//   在對付的情境:引擎忙。兩趟共用一份 kKeyDeadlineMs,第一趟吃掉 70ms
//   之後第二趟只剩 30ms,而兩趟之間還插得進新工作。
//
// ── 為什麼是「換順序」,不是「多一個協議欄位」 ──────────────────
//
// 另一條路是逾時時仍然宣告 handled=true,再多帶一格告訴 DLL「吃掉了,但
// 不要動文件」。那要動 Result 的線路格式,而 Result 是**服務 → DLL**:
// 舊 DLL 的 DecodeResult 要求「剛好用完」,所以新欄位只能照協商出來的版本
// 寫 —— 也就是 proto ≤ 4 的 DLL(舊的、以及新 DLL 對舊服務降版之後的那些)
// 收不到那一格,而它們踩到的正是「組字當場消失而且沒有上屏」。升版本身是
// 純加法沒錯,但這裡的「少一項功能」等於「那個破壞性的缺陷還在」。
//
// 換順序不必動線路,而且它把問題**消掉**而不是描述它:唯一那件不可逆的事
// 排在最後,前面每一趟失敗都退回「一步都沒做」—— 而那條路早就存在、
// 也早就是對的(handled=false → 那顆鍵交回宿主,兩邊一致)。
//
// ⚠ 這裡**一次都不預測**「預算夠不夠走兩趟」。第一趟要花多久事前不知道,
//   所以「最前面查一次」不算數;這裡改成每一趟開始時各問一次
//   RemainingKeyBudgetMs(),而第一趟花掉多少都行:剩 0 的話第二趟那道
//   `deadline_ms <= 0` 的門會直接回「什麼都沒做」。
struct VariantKeyPlan {
  // 可不可以去做那件不可逆的事。
  // ⚠ 呼叫端只有在它為真時才准呼叫 ToggleVariantPref() —— 這一格**就是**
  //   「副作用排在最後一趟」那道門。
  bool may_toggle;
  // 回給 DLL 的 handled。true = 這顆鍵算我們吃掉了,宿主不准再動作一次。
  bool eat_key;
  // 這一份快照是不是引擎的現況 —— 服務端可不可以拿它碰 UI,以及 DLL 可不
  // 可以把它套進文件。
  bool ui_is_current;
};

// snapshot_is_current = 第一趟(唯讀的 Engine::CurrentResult)真的跑完了,
//   **而且**引擎認得那個 session。
//   ⚠ 後半段不可以省。CurrentResult 的工作本體在 Find(id) 失敗時直接
//     return,盒子維持預設(handled=false、快照全 0),而 CallKeyBounded
//     仍然回 true —— 工作**確實跑完了**,只是答案是「不認得」。只看
//     timed_out 的話,一份全 0 的快照會被當成現況:候選窗被收掉、那一橫把
//     中/英寫成「中」、簡繁那格消失,而 DLL 會把它套進文件,使用者打到
//     一半的組字當場消失而且沒有上屏。
//   ⚠ 而且那不是競態,是一個**會留著的狀態**:重新部署後某個宿主重建失敗
//     (Engine::RebuildSessionsOnEngineThread 的 `++failed; continue;`),
//     它的 id 就永久不在 sessions_ 裡,而 ReadBackStatus(0) 的
//     sessions_.begin() 退路照樣成功 —— 那個宿主之後每按一次就被清一次。
// toggled = 第二趟真的做了(ToggleVariantPref 回 true)。
//   ⚠ snapshot_is_current 為假時呼叫端根本不會去呼叫它,那一格傳 false。
constexpr VariantKeyPlan PlanVariantKey(bool snapshot_is_current,
                                        bool toggled) {
  return VariantKeyPlan{snapshot_is_current, snapshot_is_current && toggled,
                        snapshot_is_current && toggled};
}

// ── 兩條硬性要求。四格全列出來,而且是**編譯期**的 ──────────────────
//
// ⚠ 寫成 static_assert 而不是註解:這個標頭服務端與 DLL 端都 include,
//   破壞任何一條的人編不過。tests/test_key_deadline.cc 再用執行期斷言把
//   同一組寫一次,讓報表上看得到「這一條真的被檢查了」。
//
//   ① 副作用發生了 ⇒ 那顆鍵一定算被我們吃掉。
//   ② 那顆鍵算被吃掉 ⇒ 手上那一份一定是引擎的現況。
//   ③ 沒有握著現況快照 ⇒ 不准去做那件不可逆的事(①② 靠它才成立)。
constexpr bool VariantKeyPlanIsSound(bool snapshot_is_current, bool toggled) {
  return
      // ① 副作用只在 may_toggle 為真時才可能發生,而那時一定 eat_key。
      (!(PlanVariantKey(snapshot_is_current, toggled).may_toggle && toggled) ||
       PlanVariantKey(snapshot_is_current, toggled).eat_key) &&
      // ② 吃掉了就一定拿得出現況(DLL 套進文件的不會是空的或舊的)。
      (!PlanVariantKey(snapshot_is_current, toggled).eat_key ||
       PlanVariantKey(snapshot_is_current, toggled).ui_is_current) &&
      (!PlanVariantKey(snapshot_is_current, toggled).eat_key ||
       snapshot_is_current) &&
      // ③ 沒有現況快照就不准動那件不可逆的事。
      (PlanVariantKey(snapshot_is_current, toggled).may_toggle ==
       snapshot_is_current);
}

static_assert(VariantKeyPlanIsSound(false, false) &&
                  VariantKeyPlanIsSound(false, true) &&
                  VariantKeyPlanIsSound(true, false) &&
                  VariantKeyPlanIsSound(true, true),
              "簡繁快捷鍵那條路的兩條硬性要求:副作用發生了那顆鍵就一定要"
              "算被吃掉(否則宿主的 Ctrl+Shift+F 會同時動作一次),而算被"
              "吃掉就一定要拿得出引擎的現況(否則 DLL 會把一份空快照套進"
              "文件,使用者打到一半的組字當場消失而且沒有上屏)");

// 反向:把 PlanVariantKey 改成「永遠吃掉」或「永遠不吃」都要編不過。
static_assert(PlanVariantKey(true, true).eat_key,
              "握著現況快照、而且真的切了 —— 這一格必須吃掉那顆鍵,"
              "不然簡繁換了而宿主的 Ctrl+Shift+F 也開了");
static_assert(!PlanVariantKey(true, false).eat_key &&
                  !PlanVariantKey(false, true).eat_key &&
                  !PlanVariantKey(false, false).eat_key,
              "什麼都沒做就不可以吃掉那顆鍵(key_eat_policy.h:做不到的事"
              "不要吃掉那顆鍵)");

}  // namespace rimewin

#endif  // RIMEWIN_KEY_DEADLINE_H_
