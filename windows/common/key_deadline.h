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
// ── ⚠ 上面那段結論在這一格仍然成立,但它證明的範圍比它看起來小(#116)──
//
// 留著上面那一整段,因為**對簡繁這顆鍵它是對的**:換順序確實比多帶一格好,
// 而且現在仍然是這個檔案在做的事。要更新的是它最後那一句的**適用範圍**。
//
// 那句話說的是:新欄位舊 DLL 收不到 ⟹ 不要動線路。
// 而它真正證明的只有前半句 —— **舊 DLL 救不回來**。它一個字都沒有說到
// 「新 DLL 也不要救」。照它的寫法推下去,任何純加法的協議改良都會被
// 永久否決(舊的那一端永遠收不到),而那顯然不是我們要的規矩。
//
// 為什麼現在的判斷不同,有兩件事變了:
//
//   一、**代價變了,而且換順序治不到它。** 上面設想的損害是「組字當場
//       消失而且沒有上屏」。#116 量到的是同一個混淆的另一半,而它更糟:
//       handled=false 同時蓋著「引擎跑完了、它不要這顆鍵」與「引擎一個字
//       都沒說」,DLL 分不出來,於是它把使用者剛按的字母**補進他的文件**
//       (tsf/text_service.cc 的 SelfInsertChar,不是交回宿主 —— 那顆鍵
//       在 OnTestKeyDown 已經宣告吃掉了)。使用者升級之後在 LINE 的訊息框
//       裡打「你好」拿到「ni好」:靜默、不可逆、送出之後才看得到。
//       ⚠ 而「換順序」對這一格**沒有東西可換** —— 逾時的那顆鍵不存在
//         一個「把它變成有回答」的順序。上面那條路在這裡走不通。
//
//   二、**做法不是上面被否決的那一個。** 上面否決的是「在 Result 尾巴
//       **加欄位**」,理由是 DecodeResult 要求「剛好用完」,舊 DLL 會把
//       整則丟掉 —— 那個理由完全正確,而且它只對加欄位成立。實際採用的是
//       protocol.h 的 kStKeyNotAnswered:一個住在**既有那個 u32 裡**的位元,
//       位元組佈局一個位元都沒有變,舊 DLL 照樣解得完、只是看不見它。
//       與 kStVariantKnown 同一種加法,tests/test_proto_compat.cc 兩個方向
//       都驗。也就是說上面那個反對意見在這裡**根本沒有被觸發**。
//
// ⚠ 極性只准一個方向:**有那個位元 = 確定沒回答**;沒有它不等於回答了
//   (舊服務永遠不送它)。反過來讀會讓「舊服務 + 新 DLL」變成每顆字母都
//   沒反應 —— 那才是真的把舊組合弄壞。
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
// ══════════════════════════════════════════════════════════════════
// ── `case Op::kKey` 上**每一個**出口都要走的同一道門(#119)────────
// ══════════════════════════════════════════════════════════════════
//
// ── 為什麼上面那一套不夠 ────────────────────────────────────────
//
// PlanVariantKey 是對的,但它只守**一顆鍵**。而同一個錯在 `Ctrl+空格`
// 上原封不動地又發生了一次,而且沒有人回頭看:
//
//   service/engine.cc 的 ToggleAsciiMode() 先 SetAsciiModeAll(!now)
//   —— 那一句是 store + PostAsync,**不等**,所以中英在那一瞬間就真的
//   切了 —— 然後才去做那趟有上限的「切中英後取快照」。逾時的時候它回
//   handled=false,於是:
//
//     · TSF 的 OnPreservedKey 回 FALSE → **宿主也吃到那顆 Ctrl+空格**
//       (Everything 的搜尋框、某些編輯器的自動完成),使用者按一下
//       同時得到「中英切了」與「別人的功能被叫起來了」;
//     · 而 pipe_server 那側 `key_result_is_current = !kw.timed_out`
//       → DecideKeyUiAction 回 kLeaveUiAlone → **那一橫一個像素都不動**,
//       畫面說他還在中文,引擎已經在英數。
//
// 對照組 `3fa0dab`:第二趟是 Engine::Post() = 永遠等,handled 永遠是
// true —— 這個狀態在 main 上**不存在**,是「替第二趟加上限」造出來的。
// 與 Ctrl+Shift+F 那一格一字不差的成因。
//
// ── 所以判準不再是「這顆鍵是哪一顆」,是「有沒有做過那件不可逆的事」──
//
// 兩個輸入,而且**兩個都是事實,不是意圖**:
//
//   side_effect_done   —— 這顆鍵已經對引擎或設定做過不可逆的事。
//                         · Ctrl+空格:SetAsciiModeAll() 已經呼叫過;
//                         · Ctrl+Shift+F:ToggleVariantPref() 回了 true;
//                         · 一般按鍵:rs_process_key() 回了 true
//                           (它吃了那顆鍵 = 組字狀態動過了)。
//                         逾時而**作廢成功**時它是 false —— 本體一步都
//                         沒跑,所以沒有東西可以「已經做過」。
//   snapshot_is_current —— 手上這一份 Result 是引擎的現況,不是佔位。
//
// 三個輸出,而且它們**互相沒有自由度**(見下面四條 static_assert):
//
//   eat_key          → 回給 DLL 的 handled。true = 宿主不准再動作一次。
//   result_is_current→ 服務端可不可以拿這一份碰候選窗與那一橫;
//                      同時決定線路上要不要帶 kStKeyNotAnswered。
//   doc              → DLL 可不可以把這一份套進使用者的文件。
//
// ── 「吃掉、但文件一個位元都不動」不是新東西 ─────────────────────
//
// 它就是 common/key_eat_policy.h 的 KeyOutlet::kEatSilently,而線路上的
// 表示是 protocol.h 的 kStKeyNotAnswered —— 一個住在既有 u32 裡的位元,
// 舊 DLL 照樣解得完。也就是說 `Ctrl+空格` 逾時那一格**早就有**正確的
// 出口,只是沒有人把它接上去。
//
// ⚠ 這道門是 `case Op::kKey` 上唯一算得出 `Result::handled` 的地方。
//   守它的不是這段註解:windows/run_logic_tests.sh 的 key_path_gates()
//   第 (7) 段掃 service/engine.cc 與 service/pipe_server.cc,要求
//     (1) 凡是會做那件不可逆的事的 Engine 入口(body 裡有
//         SetAsciiModeAll / rs_process_key / rs_select_candidate /
//         rs_commit_composition / rs_clear_composition / ApplyVariantAll)
//         都要把它寫進 wait->side_effect_done ——「哪些算副作用」是
//         **結構上的**判準,新增第四種按鍵動作時它自己會抓到;
//     (2) `case Op::kKey` 區塊裡 `PlanKeyExit(` 恰好一次;
//     (3) 那一次的結果真的被寫回 `r.handled = key_exit.eat_key;`,
//         而且那道門**之後**再也沒有別的地方寫 `r.handled =`;
//     (4) 每一個會進佇列的分支都交得出 key_side_effect_done,
//         而且不是常數;
//     (5) 不是現況的那一份要走 KeyExitNeedsNotAnsweredBit() OR 上
//         kStKeyNotAnswered。
//   六種拆法都有反向測試,而且都會紅(run_logic_tests.sh 那一段的
//   ascii_gate_removed / ascii_gate_overridden /
//   ascii_side_effect_unreported / processkey_side_effect_unreported /
//   branch_side_effect_const / not_answered_bit_dropped)。

// DLL 收到這一份之後,可不可以拿它動使用者的文件。
enum class KeyDocAction {
  // 照舊:套進文件(上屏、組字、收尾)。
  kApplySnapshot,
  // **文件一個位元都不動。** 那顆鍵仍然算我們吃掉了(見 eat_key),
  // 只是引擎沒有交出現況,沒有東西可以套。
  kLeaveDocAlone,
};

struct KeyExitPlan {
  bool eat_key;
  bool result_is_current;
  KeyDocAction doc;
};

constexpr KeyExitPlan PlanKeyExit(bool side_effect_done,
                                  bool snapshot_is_current) {
  return KeyExitPlan{
      // ① 做過那件不可逆的事 ⇒ 這顆鍵一定算我們吃掉。
      //    ⚠ 反過來不成立:引擎表態說「我不要這顆鍵」(英數模式下的
      //      字母)時什麼都沒做,那一顆本來就該讓宿主收尾。
      side_effect_done,
      snapshot_is_current,
      // ② 不是現況 ⇒ 文件一個位元都不動。
      snapshot_is_current ? KeyDocAction::kApplySnapshot
                          : KeyDocAction::kLeaveDocAlone};
}

// 這一份要不要在線路上帶 kStKeyNotAnswered。
// ⚠ 極性只有一個方向(protocol.h):**有那個位元 = 確定沒回答**。
constexpr bool KeyExitNeedsNotAnsweredBit(bool side_effect_done,
                                          bool snapshot_is_current) {
  return !PlanKeyExit(side_effect_done, snapshot_is_current).result_is_current;
}

// ── 四條硬性要求,四格全列出來,而且是**編譯期**的 ────────────────
constexpr bool KeyExitPlanIsSound(bool side_effect_done,
                                  bool snapshot_is_current) {
  return
      // ① 副作用發生了 ⇒ 那顆鍵一定算被我們吃掉。
      //    這一條就是 #119:`Ctrl+空格` 切了中英卻回 handled=false,
      //    於是宿主同時吃到同一顆鍵。
      (!side_effect_done ||
       PlanKeyExit(side_effect_done, snapshot_is_current).eat_key) &&
      // ② 不是現況 ⇒ DLL 不准把它套進文件。
      //    (少了這一條,①「逾時也要吃掉」就會把一份空快照送進
      //     SendAsciiToggle 的 ApplyPlan,使用者打到一半的組字當場消失。)
      (PlanKeyExit(side_effect_done, snapshot_is_current).result_is_current ||
       PlanKeyExit(side_effect_done, snapshot_is_current).doc ==
           KeyDocAction::kLeaveDocAlone) &&
      // ③ 不是現況 ⇒ 服務端的 UI 也不准動,而且與 DecideKeyUiAction
      //    必須是**同一個答案**(兩份真相就是兩個會漂移的畫面)。
      (DecideKeyUiAction(
           /*timed_out=*/!PlanKeyExit(side_effect_done, snapshot_is_current)
               .result_is_current,
           PlanKeyExit(side_effect_done, snapshot_is_current)
               .result_is_current) ==
       (PlanKeyExit(side_effect_done, snapshot_is_current).result_is_current
            ? KeyUiAction::kUpdateUi
            : KeyUiAction::kLeaveUiAlone)) &&
      // ④ 吃掉了、而且不是現況 ⇒ 線路上一定要帶 kStKeyNotAnswered,
      //    否則 DLL 的 DecideKeyOutlet 會走 kSelfInsert,把使用者剛按的
      //    字母補進他的文件(§13c 的「ni好」)。
      (!(PlanKeyExit(side_effect_done, snapshot_is_current).eat_key &&
         !PlanKeyExit(side_effect_done, snapshot_is_current)
              .result_is_current) ||
       KeyExitNeedsNotAnsweredBit(side_effect_done, snapshot_is_current));
}

static_assert(KeyExitPlanIsSound(false, false) &&
                  KeyExitPlanIsSound(false, true) &&
                  KeyExitPlanIsSound(true, false) &&
                  KeyExitPlanIsSound(true, true),
              "case Op::kKey 上每一個出口的四條硬性要求:做過不可逆的事就"
              "一定要吃掉那顆鍵(否則宿主同時也吃到它)、不是現況就不准"
              "碰文件也不准碰 UI、而且線路上一定要說出『引擎沒有回答』");

// ── 反向:把 PlanKeyExit 寫成任何一種「省事」的樣子都要編不過 ─────
static_assert(PlanKeyExit(true, false).eat_key,
              "#119 本身:中英已經切了而快照逾時 —— 這一格**必須**吃掉"
              "那顆鍵,不然宿主的 Ctrl+空格 也會動作一次");
static_assert(PlanKeyExit(true, false).doc == KeyDocAction::kLeaveDocAlone,
              "吃掉不等於可以動文件:那一份是佔位,套進去等於把使用者"
              "打到一半的組字清掉");
static_assert(!PlanKeyExit(false, false).eat_key &&
                  !PlanKeyExit(false, true).eat_key,
              "什麼都沒做就不可以吃掉那顆鍵(key_eat_policy.h:做不到的事"
              "不要吃掉那顆鍵)");
static_assert(!KeyExitNeedsNotAnsweredBit(true, true) &&
                  !KeyExitNeedsNotAnsweredBit(false, true),
              "現況就是現況 —— 亂送 kStKeyNotAnswered 會讓 DLL 對每一顆"
              "正常的字母都『吃掉但不動文件』,那是整條輸入法失效");

// ── 上面那顆專用的門必須是這顆通用門的一個特例 ────────────────────
//
// ⚠ 兩份真相就是兩個會漂移的答案。PlanVariantKey 留著(它多守一件事:
//   **副作用排在最後一趟**,也就是 may_toggle),但它交出去的
//   eat_key / ui_is_current 必須與 PlanKeyExit 一字不差。
constexpr bool VariantPlanAgreesWithKeyExit(bool snapshot_is_current,
                                            bool toggled) {
  return PlanVariantKey(snapshot_is_current, toggled).eat_key ==
             PlanKeyExit(snapshot_is_current && toggled,
                         snapshot_is_current && toggled)
                 .eat_key &&
         PlanVariantKey(snapshot_is_current, toggled).ui_is_current ==
             PlanKeyExit(snapshot_is_current && toggled,
                         snapshot_is_current && toggled)
                 .result_is_current;
}

static_assert(VariantPlanAgreesWithKeyExit(false, false) &&
                  VariantPlanAgreesWithKeyExit(false, true) &&
                  VariantPlanAgreesWithKeyExit(true, false) &&
                  VariantPlanAgreesWithKeyExit(true, true),
              "簡繁那顆專用的門必須是通用那道門的特例 —— 兩份真相會漂移,"
              "而漂移的樣子是『某一顆鍵又開始被宿主吃第二次』");

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
