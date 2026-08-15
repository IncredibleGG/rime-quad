// windows/common/key_eat_policy.h — 「這一顆鍵,我們可不可以宣告吃掉」
//
// ══ 這一格答錯的樣子 ═══════════════════════════════════════════════
//
// 使用者回報:「居然無法刪除字,可以打字,不能刪除。」
//
// 退格鍵按下去什麼都不會發生。不是刪錯字、不是慢 —— 是那一顆鍵不存在了。
// 而畫面完全正常,自動化全綠(CI 的假宿主只送過 `nihao` 六顆字母,
// **從來沒有送過退格**)。
//
// ══ 為什麼會這樣 ═══════════════════════════════════════════════════
//
// TSF 的按鍵有兩趟:
//
//   1. `OnTestKeyDown` —— 「這顆鍵是你的嗎?」**不可以有副作用。**
//   2. `OnKeyDown`     —— 真的處理。
//
// 舊的 `OnTestKeyDown` 只問兩件事:映得出 keysym 嗎?連線通嗎?
// 通了就一律 `*pfEaten = TRUE`。於是**每一顆**映得出 keysym 的鍵都被宣告
// 吃掉了 —— 包含退格、Delete、方向鍵、Home/End、Esc、Enter、Tab。
//
// 接著 `OnKeyDown` 把它送進引擎,librime 在**沒有組字**的時候不處理退格
// (`Editor::ProcessKeyEvent` 開頭就是 `if (ctx->IsComposing())`),
// 回 false,於是我們回 `*pfEaten = FALSE`。
//
// 程式碼裡原本的註解說「回 TRUE 但稍後回 FALSE 是合法的 —— TSF 會把那顆鍵
// 交回宿主」。**那句話是錯的,而且錯得很貴。** 實務上:宿主(以及 Windows
// 給非 TSF 感知程式用的 CUAS 相容層)在 `TestKeyDown` 說「吃」的當下就
// **放棄了自己的預設處理**;`KeyDown` 事後改口說「沒吃」時,那顆鍵已經
// 不會再回到宿主手上了。Microsoft 自己的 TSF 範例寫得很直白:
// **TestKeyDown 說吃了,OnKeyDown 就得負責。**
//
// 所以那顆退格鍵掉進了黑洞:宿主以為我們處理了,我們以為宿主會處理。
//
// ⚠ 這是這個專案抓過四次的「看得到但摸不到」的第五次。前四次都是選單項目
//   或按鈕,這一次是**鍵盤上的鍵**。而「一顆被吃掉卻不做事的鍵,在使用者
//   那裡就是壞掉的鍵盤」—— 比打不出中文嚴重,因為打不出中文他還能切回英數,
//   刪不掉字他只能重開程式。
//
// ══ 規則 ═══════════════════════════════════════════════════════════
//
// 判準只有一句:**宣告吃掉的鍵,一定要有人負責。**
// 據此把鍵分成兩族,兩族的負責方式不一樣:
//
// ── A. 字元鍵(字母、數字、標點、空白)────────────────────────────
//
//   **一律吃。** 不能靠「有沒有在組字」來猜,因為**哪些字元會起頭,
//   是方案決定的,而 DLL 不知道方案**:
//     · 朗月拼音的字母會起頭,數字不會;
//     · **注音的 alphabet 含數字**(`1` 是ㄅ),數字會起頭。
//   不吃數字的話,注音使用者一開始就打不出ㄅㄆㄇ;吃了而拼音使用者
//   又按了數字,引擎不處理 —— 所以吃掉之後必須有後路:
//
//   **引擎不處理時,由我們自己把那個字元寫進文件。**(text_service.cc 的
//   `InsertText`,字元來自鍵盤佈局 oracle,不是猜的。)使用者看到的結果與
//   宿主自己插入完全一樣,而且不會有黑洞。英數模式底下每一顆字母也走這條路。
//
// ── B. 功能鍵(退格、Delete、方向、Home/End、Esc、Enter、Tab、翻頁)──
//
//   **只有在組字中才吃。** 沒有組字時,那是宿主的鍵,原樣放行 ——
//   這正是使用者回報的那一格。
//
//   為什麼這一族可以用「有沒有組字」判斷,而字元鍵不行:功能鍵在**所有**
//   方案裡的意義都一樣(編輯組字串),沒有哪個方案會用退格來起頭。
//   這不是猜測,是「組字串不存在時,沒有東西可以編輯」這件事本身。
//
// ── C. 完全不是我們的 ───────────────────────────────────────────────
//
//   F1–F24、修飾鍵本身、Ctrl / Alt / Win 的組合鍵、PrintScreen…
//   **永遠不吃。** Ctrl+C 被輸入法吃掉是災難,而它以前只是碰巧沒事:
//   Ctrl+C 的 keysym 是 'c',舊規則會吃掉它。
//
// ══ 這個檔案為什麼是純邏輯 ═════════════════════════════════════════
//
// 不 include windows.h,所以 windows/run_logic_tests.sh 在 Ubuntu 上就跑得到
// (見 tests/test_key_eat_policy.cc:一張逐鍵、逐狀態的真值表)。
// 真的按鍵路徑另外由 windows/verify_input_matrix.sh 在 CI 上驗,而那一支
// 斷言的是**文件內容**,不是「有沒有被吃掉」—— 「吃掉了」正是這個 bug 的表現。
#ifndef RIMEWIN_COMMON_KEY_EAT_POLICY_H_
#define RIMEWIN_COMMON_KEY_EAT_POLICY_H_

#include <cstdint>

namespace rimewin {

enum class KeyKind {
  // 映不出 keysym。永遠不吃(它進不了引擎)。
  kUnmappable,
  // 會產生字元的鍵。一律吃,引擎不處理時由我們自己補進文件。
  kCharacter,
  // 編輯組字串:退格、Delete。只有組字中才吃。
  kEditing,
  // 移動 / 翻頁:方向鍵、Home、End、PageUp、PageDown。只有組字中才吃。
  kNavigation,
  // 結束組字:Enter(上屏)、Esc(取消)、Tab。只有組字中才吃。
  kFinish,
  // 宿主的鍵:F1–F24、修飾鍵、Ctrl/Alt/Win 組合、PrintScreen…永遠不吃。
  kHostOnly,
};

// keysym + 修飾鍵 → 這是哪一族。
//
// ⚠ modifiers 用的是 keymap.h 的 Mod 位元(kModControl / kModAlt / kModSuper)。
//   帶著 Ctrl / Alt / Win 的鍵一律是 kHostOnly —— 那些是宿主的快捷鍵,
//   而輸入法把 Ctrl+C 吃掉,比它打不出字嚴重得多。
//   (Shift 與 CapsLock **不算** —— 那兩個是字元的一部分。)
KeyKind ClassifyKeyKind(int32_t keysym, uint32_t modifiers);

// `OnTestKeyDown` 該不該宣告吃掉。composing = 目前有沒有進行中的組字。
bool ShouldEatKey(KeyKind kind, bool composing);

// 引擎回報「我不處理這顆鍵」時,我們該不該自己把字元補進文件。
//
// 只有 kCharacter 是 true:我們宣告吃了它,就得負責(見檔頭)。
// 功能鍵不補 —— 它們只在組字中才會被吃,而那時「引擎不處理」的正確反應
// 是什麼都不做,不是去動宿主的文件。
bool ShouldSelfInsert(KeyKind kind);

// 這個 keysym 代表的字元(Unicode 碼點)。0 = 它不代表一個字元。
//
// 引擎回報「我不處理」時,`kCharacter` 那一族要由我們自己把字寫進文件,
// 而寫哪一個字就是這裡回答的。**不要用「使用者按了哪顆鍵」去猜** ——
// 同一顆實體按鍵在 Dvorak / 德文 / AZERTY 上是不同的字元,而 keysym 已經
// 是問過鍵盤佈局之後的結果(見 keymap.h 檔頭)。
//
// 數字鍵台是特例:XK_KP_5 不是 '5' 的 keysym,但使用者按它就是要一個 5。
char32_t CharForSelfInsert(int32_t keysym);

// 記錄與診斷用的穩定標籤(ASCII)。驗證腳本比對的是這個,不是中文句子。
const char* KeyKindTag(KeyKind kind);

// ══ 宣告吃掉了、而引擎沒有把這顆鍵處理掉 —— 那這顆鍵的出口是哪一個 ══
//
// ══ 這一格答錯的樣子 ═══════════════════════════════════════════════
//
// 使用者升級之後,在 LINE 的訊息框裡打「你好」,拿到的是 **「ni好」**。
//
// 前面那兩個字母是**我們**替他打的:服務剛被安裝程式換掉重啟,引擎還在
// 部署,前兩顆鍵回的是 handled=false;DLL 分不出「引擎不要這顆鍵」與
// 「引擎沒有回答」,於是照既有規則把原始字母補進文件。第三顆鍵引擎回來
// 了,從「h」開始組字,最後上屏「好」。
//
// ⚠ 這比整串英文糟。`nihao1` 使用者一眼看得出輸入法沒作用;
//   「ni好」看起來像他自己打錯字 —— 沒有紅字、沒有提示,那一橫還顯示
//   「中」,而他按下送出之後才會發現。**靜默的資料污染**,發生在他真實的
//   訊息裡。Android 的 #105 是同一個缺陷,已經照同一條規矩修掉。
//
// ══ 規則 ═══════════════════════════════════════════════════════════
//
// 判準是一句話:**引擎沒有表態的時候,不准替它動使用者的文件。**
//
//   · 引擎**回答了、它不要這顆鍵** → 照舊由我們補進文件。這條路每天都
//     會走到(英數模式下的每一顆字母),而且它是對的:引擎表過態了。
//   · 引擎**沒有回答**(逾時 / 部署中 fail-open / 不認得 session /
//     連線斷在兩趟之間)→ **吃掉,而且文件一個位元都不動。**
//
// ⚠ 「吃掉不動」的代價是那顆鍵按下去沒反應,而 text_service.cc 的
//   `SendKey` 失敗那一格曾經寫過「按了完全沒有反應…比打出英文更糟」。
//   那句話對**功能鍵**成立,對**組字中的字元鍵**是反的,理由是損害停在
//   哪裡:
//
//     · 吃掉不動 → 引擎的組字裡少一個音,而那看得見(preedit 是
//       「hao」不是「nihao」)、按 Esc / 退格救得回來、而且**上屏之前
//       什麼都沒有進到文件**。
//     · 補進文件 → 那兩個字母**已經在文件裡了**,Esc 清不掉、引擎不知道
//       它們存在,而使用者要到送出之後才看得到。
//
//   兩邊都是「打出來的字不對」,差別在可不可逆、看不看得見。
//   所以這一格選可逆的那一邊,而**代價要說出來**:引擎在準備字詞的那
//   十幾秒裡,字元鍵按下去是沒有反應的。那一橫在那段時間必須說出自己
//   是什麼(service/status_bar.cc 讀 kStDisabled,已經在做),
//   而「乾脆不要吃它、讓宿主自己打那個英文字母」是更好的長期解 ——
//   那要動 OnTestKeyDown,不在這一輪(#108 追蹤)。
enum class KeyOutlet {
  // 由我們自己把那個字元寫進文件(text_service.cc 的 SelfInsertChar)。
  kSelfInsert,
  // 吃掉,而且**文件一個位元都不動**。
  kEatSilently,
  // 交回宿主。只發生在「沒有組字、而且不是字元鍵」——
  // 依 ShouldEatKey,那一族在沒有組字時根本不會被宣告吃掉,所以
  // 正常情況到不了這裡;真的到了,放行最壞是「這顆鍵沒作用」。
  kPassToHost,
};

// engine_answered = 引擎**表過態**了(它跑完了 rs_process_key,答案是
//   「我不要這顆鍵」)。false = 逾時 / 部署中 / 不認得 session /
//   連線斷在兩趟之間 —— 線路上的表示是 protocol.h 的 kStKeyNotAnswered,
//   而**沒有那個位元不等於回答了**(舊服務永遠不送它,見那裡的極性說明)。
// have_composition = **我們這一側**真的開著一段組字(TSF 的 composition)。
//   自己插字元會插進組字的 range 裡,所以這一格為真時不准補字元。
// composing = 兩側任何一邊在組字(我們的 composition 或引擎說它在組字)。
//   ⚠ 與上一格刻意分開:引擎說它在組字、而我們這一側 StartComposition
//     失敗時,那顆鍵在 OnTestKeyDown 已經被宣告吃掉了,這裡必須用同一個
//     條件回答,不然那一格就是黑洞。
// have_char = CharForSelfInsert() 給得出一個字元。
KeyOutlet DecideKeyOutlet(KeyKind kind, bool engine_answered,
                          bool have_composition, bool composing,
                          bool have_char);

// 記錄與診斷用的穩定標籤(ASCII)。同 KeyKindTag。
const char* KeyOutletTag(KeyOutlet outlet);

}  // namespace rimewin

#endif  // RIMEWIN_COMMON_KEY_EAT_POLICY_H_
