// windows/common/redeploy_flow.h — 重新部署期間誰可以活著
//
// ── 這個檔案存在的理由是一個會靜靜壞掉的檔案 ──────────────────────
//
//   使用者按下「重新整理字詞」的時候,他打開的每一個程式底下都掛著一個
//   活著的 librime session,而每一個 session 都對 `*.table.bin` /
//   `*.prism.bin` 開著 **memory mapping**。
//
//   librime 的 dict_compiler 在重編時做的第一件事是 `table->Remove()`
//   —— 也就是**刪掉那個檔案**。而 Windows 不允許刪除(也不允許 resize)
//   一個還有 section mapping 掛在上面的檔案,所以那一發會失敗。
//
//   接下來是這個缺陷真正的形狀:**那個回傳值沒有人看**
//   (dict_compiler.cc:265 / :358)。刪不掉之後 `MappedFile::Create()`
//   走進 overwriting 分支,Resize 一樣失敗、一樣沒有人看,最後它用讀寫
//   模式重新映射**舊檔**,然後把新編出來的表寫進一段**還有 session 正在
//   讀**的記憶體。
//
//   使用者那一側看到的不是錯誤訊息,是打字打到一半候選變成亂碼、或者
//   服務直接消失。而 librime 從頭到尾回報成功。
//
//   weasel 與 squirrel 用 `RimeStartMaintenance` 而不是
//   `RimeDeployWorkspace`,理由就是這個。
//
// ── 所以規則只有一條,而它必須是可以被測到的 ──────────────────────
//
//   **詞庫檔正在被改寫的那一刻,一個 session 都不可以在。**
//
//   把這條規則寫成一句註解是不夠的 —— 它是一個時序性質,而時序性質
//   在 `windows/service/` 底下(那裡 include windows.h,開發機上編不起來)
//   等於沒有人驗得到。所以階段機在這裡,是純函式,
//   `windows/tests/test_redeploy_flow.cc` 逐條驗得到:
//
//     · 四個階段裡只有 kDeploying 允許改寫詞庫檔,而那一個階段
//       **不允許有 session**(兩者是互補的,測試直接驗這件事)。
//     · 從 kIdle 到 kDeploying 之間一定要經過 kClosingSessions ——
//       沒有任何一條事件邊可以跳過它。
//     · 部署的終局(成功**或**失敗)、以及 rs_deploy() 拒絕啟動,
//       三條路都要回到 kRebuilding。少了任何一條,使用者就永久
//       停在一個沒有 session 的引擎上 —— 從此打不出中文,重開機也沒用。
//
// ── ⚠ 部署期間打不出中文,那是刻意的 ────────────────────────────
//
//   這與**首次部署**的既有行為一致(engine.cc 的 fail-open:引擎回
//   handled=false,宿主自己收下那顆鍵)。它比替代方案好:替代方案是
//   「靜靜地讀一份被換到一半的詞庫」。
//
//   但畫面上**必須說出來**。那條路是 GateStatusFlags() 回填的
//   `kStDisabled` → 懸浮狀態列的 SnapshotSaysNotReady() → ServiceState::
//   kPreparing → 「正在準備,馬上就好」。少了那一格,使用者看到的是
//   一個沒反應的輸入法,而他會以為它壞了。
//
// ── ⚠ 而「fail-open」這個名字騙過我們一次(#116)──────────────────
//
//   上一句寫著「宿主自己收下那顆鍵」,而那**不是**實際發生的事。
//   DLL 在 OnTestKeyDown 已經宣告吃掉了,所以它不會退回宿主 ——
//   tsf/text_service.cc 走的是 SelfInsertChar:**由我們把那個字母寫進
//   使用者的文件**。兩者在英數模式下看起來一樣,在**組字中**完全不同。
//
//   於是使用者升級之後在訊息框裡打「你好」,拿到的是「ni好」:前兩顆鍵
//   落在這道門後面(服務剛被安裝程式換掉重啟、引擎在部署),我們替他
//   打了 'n' 'i';第三顆起引擎回來,從「h」開始組字、上屏「好」。
//   沒有紅字、沒有提示,那一橫還顯示「中」,他要到送出之後才看得到。
//
//   所以 GateStatusFlags() 現在回**兩個**位元:kStDisabled 給畫面,
//   kStKeyNotAnswered 給 DLL(「引擎對這顆鍵一個字都沒說」,見
//   protocol.h 與 key_eat_policy.h 的 DecideKeyOutlet)。
//
#ifndef RIMEWIN_REDEPLOY_FLOW_H_
#define RIMEWIN_REDEPLOY_FLOW_H_

#include <cstdint>

namespace rimewin {

// ⚠ 四個階段,不是「有沒有在部署」兩個。把它縮回布林之後,
//   「已經收乾淨了」與「還在收」就分不出來,而 rs_deploy() 正好要在
//   那兩者之間才能開始。
enum class RedeployPhase {
  // 平常。可以打字,可以建 session。
  kIdle,
  // 正在把所有 session 收掉(rs_session_destroy 同時是使用者剛學到的詞
  // 落地的唯一時機)。**還沒開始部署** —— 這時詞庫檔仍然不可以被動。
  kClosingSessions,
  // librime 正在就地改寫詞庫檔。此刻一個 session 都不可以在。
  kDeploying,
  // 部署有了終局。正在把 session 建回來,並重套方案 / 簡繁 / 標點 / 中英。
  kRebuilding,
};

constexpr int kRedeployPhaseCount = 4;

enum class RedeployEvent {
  // 使用者按了「重新整理字詞」(或設定改動觸發了一次重新部署)。
  kRequested,
  // 最後一個 session 收完了,詞庫檔現在沒有人映射著。
  kSessionsClosed,
  // 部署有了終局。**成功與失敗走同一條** —— 失敗時 session 一樣得建回來。
  kDeployFinished,
  // rs_deploy() 拒絕啟動。session 已經收掉了,一樣得建回來。
  kStartRefused,
  // 收 session 那一步**根本沒有跑**(引擎在停,工作沒有入列)。
  // ⚠ 與 kStartRefused 不同:session 一個都沒被收掉,所以**不必**重建,
  //   直接回 kIdle。兩者混成同一條的話,不是多建一批沒有人要的 session,
  //   就是把還活著的那批漏掉。
  kTeardownFailed,
  // session 都建回來、設定也重套完了。
  kRebuilt,
};

// 走一步。合法就改寫 *phase 並回 true;不合法**原地不動**並回 false。
//
// ⚠ 回 false 不是可以忽略的:它代表呼叫端對「現在是哪一個階段」的認知
//   與事實不一致(例如兩個地方同時要求重新部署)。呼叫端必須據此拒絕,
//   而不是硬把階段推過去。
bool AdvanceRedeploy(RedeployPhase* phase, RedeployEvent ev);

// 這個階段裡,librime 可不可以就地改寫詞庫檔。
bool WorkspaceMayBeRewritten(RedeployPhase p);

// 這個階段裡,可不可以有活著的 session。
// ⚠ 與上面那一支是**互補**的,而那正是這整個缺陷:兩者同時為真的那一刻
//   就是「在活著的 session 腳下抽換 mmap」。測試直接驗互補性。
bool SessionsMayExist(RedeployPhase p);

// 這個階段裡,可不可以**建**一個新的 session(宿主的 SESSION_NEW、
// 備用池的補充)。
//
// ⚠ kClosingSessions 是 false,而這一格不是多餘的:收乾淨與 rs_deploy()
//   之間有一段空檔,那段時間引擎執行緒是閒的,一個剛開的程式正好會在
//   那時候要 session —— 建起來的話它會活著撐過整場部署,而我們剛剛
//   才把所有 session 收掉就是為了不要有它。
bool SessionCreationAllowed(RedeployPhase p);

// 這一顆按鍵能不能交給引擎。first_deploy_ok = 首次部署成功過。
//
// ⚠ 舊版的判準是 `deploy_state_ == 1`,而那個值**首次部署成功後永遠是 1**
//   —— 於是重新部署期間這道門是開的,按鍵照樣打進一個正在被抽換的
//   詞庫裡。first_deploy_ok 只回答「有沒有能用的詞庫」,階段回答
//   「現在能不能碰它」,兩件事都要問。
bool TypingAllowed(RedeployPhase p, bool first_deploy_ok);

// 不能打字時要回填在快照上的旗標。能打字時回 0。
//
// ⚠ 不能打字時是**兩個**位元,而它們的讀者不同:
//   · kStDisabled       → 畫面(SnapshotSaysNotReady → 「正在準備」)
//   · kStKeyNotAnswered → DLL(「引擎對這顆鍵一個字都沒說」,於是
//     DecideKeyOutlet 走「吃掉、不動文件」而不是把字母補進去)
//   這道門是「這顆鍵不交給引擎」,所以第二個位元照定義成立。
//   漏掉它的下場是 #116 的「ni好」—— 詳見上面檔頭那一段。
uint32_t GateStatusFlags(RedeployPhase p, bool first_deploy_ok);

// 「這一顆鍵不要交給引擎」的單一問法,順便把要回填的旗標寫進 *out_flags。
//
// ⚠ 存在的理由是**呼叫順序**。engine.cc 舊版是先 Find(id) 再看部署狀態,
//   而重新部署期間 session 是真的不見了 —— Find() 回 0 之後那條路徑
//   直接 return,回給宿主的是 handled=false 配一份 status_flags **全 0**
//   的快照。狀態列讀到「旗標可用」,於是它把使用者剛切好的中英打回
//   預設,而且不說任何一句「還沒好」。
//   一律先問這一支,再去找 session。
bool ShouldFailOpen(RedeployPhase p, bool first_deploy_ok, uint32_t* out_flags);

// 這個階段要不要讓畫面說「正在準備」(service_state.h 的
// EngineFacts::engine_says_not_ready)。
//
// ⚠ deploy_done 只會從 false 變成 true 一次,之後**永遠**是 true ——
//   重新部署時它不會退回去。所以側欄與懸浮狀態列那兩處要靠這一格。
bool PhaseSaysPreparing(RedeployPhase p);

// 有沒有一場重新部署正在進行(= 不是 kIdle)。
bool RedeployInFlight(RedeployPhase p);

// 建不出 session 時寫進線路上那則 ERROR 的原因。
//
// ⚠ 這是**診斷字串,不是 UI 文案**:用戶端會把它原樣記進除錯記錄,
//   fail-open 之下那是唯一留得下來的線索。所以它是窄字串、不進
//   ui_strings 的 catalog(W7 掃的是寬字串)。
//
// ⚠ 三種原因要分得開。舊版對兩種都說「引擎建不出 session」,而
//   「第一次還在整理」與「使用者剛按了重新整理」對看記錄的人是
//   完全不同的兩件事。
const char* SessionRefusedReason(RedeployPhase p, bool first_deploy_ok);

// 給日誌用的短名。
const char* RedeployPhaseName(RedeployPhase p);

}  // namespace rimewin

#endif  // RIMEWIN_REDEPLOY_FLOW_H_
