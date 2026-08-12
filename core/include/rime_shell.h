/*
 * rime_shell.h — 四端共用的 C ABI 門面
 *
 * 這是 librime `rime_api.h` 之上的一層薄門面，不重造輪子，只做三件事：
 *
 *   1. 把 librime 分散的 RimeContext / RimeCommit / RimeStatus 收斂成
 *      一次呼叫就能取得的「UI 狀態快照」(rs_snapshot)，讓前端不必理解
 *      librime 的取值時序。
 *   2. 收束部署（deployment）與維護執行緒的生命週期，前端只看到回呼。
 *   3. 提供一個四端完全一致、且 ABI 穩定的邊界 —— Android 走 JNI，
 *      Apple 走 Swift C interop，Windows 走服務進程內直接連結。
 *
 * ── 執行緒約定 ─────────────────────────────────────────────────
 *   除 rs_deploy() 外，同一 session 的所有呼叫必須在同一執行緒上序列化。
 *   在行動端，這代表 IME 的主執行緒；不要從 UI 動畫執行緒呼叫。
 *
 *   rs_deploy_callback **不會**在呼叫端的執行緒上被呼叫。它來自 librime 的
 *   維護執行緒，而且可能在 rs_deploy() 早已返回之後才觸發。因此：
 *     - Android：JNI 端必須 AttachCurrentThread，再切回主執行緒更新 UI。
 *     - Apple / Windows：不可在回呼裡直接碰 UI，必須 dispatch 回主執行緒。
 *   回呼期間本層不持有任何鎖，可以安全地在回呼裡再呼叫本層的其他函式。
 *
 * ── 記憶體約定 ─────────────────────────────────────────────────
 *   rs_snapshot_acquire() 回傳的指標（含其中所有字串）由本層擁有，
 *   在同一 session 的下一次 acquire 或 release 之前有效。
 *   前端必須在該窗口內複製出自己需要的資料，不得長期持有。
 *
 *   連續呼叫 acquire 而不 release 是**合法**的：後一次的內容覆寫前一次，
 *   前一次取得的指標即失效。
 *
 *   ⚠ commit 在 **acquire 當下**就被消費掉，不是在 release。
 *   librime 的 get_commit 具有「取出即清除」語意，本層照實反映。
 *   因此「先 acquire 看一眼狀態、做點事、再 acquire 讀結果」這種寫法會
 *   **遺失第一次的 commit_text**。正確的紀律是：
 *
 *       每個輸入事件（按鍵／點選候選／確認）之後，只 acquire 一次，
 *       在那一次就把 commit_text 處理掉。
 *
 * ── 版本協商 ───────────────────────────────────────────────────
 *   rs_abi_version() 回傳的是**實作端（.so/.dylib/.dll）編譯時**的版本；
 *   RIME_SHELL_ABI_VERSION 是**呼叫端編譯時**看到的版本。
 *   兩者比對即可分辨是實作端過舊還是呼叫端過舊：
 *
 *       if (rs_abi_version() != RIME_SHELL_ABI_VERSION) 拒絕載入;
 */

#ifndef RIME_SHELL_H_
#define RIME_SHELL_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 任何破壞相容性的變更都必須遞增此值。前端啟動時應以 rs_abi_version()
 * 比對自身編譯期的常數，不符即拒絕載入 —— 行動端熱更新資料檔時，
 * 這是唯一能擋下 so/dylib 與上層不同步的關卡。 */
#define RIME_SHELL_ABI_VERSION 3

typedef uintptr_t rs_session;
#define RS_INVALID_SESSION ((rs_session)0)

/* ───────────────────────── 初始化 ───────────────────────── */

typedef enum {
  RS_DEPLOY_IDLE = 0,
  RS_DEPLOY_RUNNING,
  RS_DEPLOY_SUCCESS,
  RS_DEPLOY_FAILURE,
} rs_deploy_status;

typedef void (*rs_deploy_callback)(rs_deploy_status status, void* userdata);

typedef struct {
  /* 使用者資料目錄：schema、使用者詞典、個人配置。行動端須為可寫路徑。
   * iOS 上這必須位於 App Group 容器內，否則主 App 與鍵盤擴展看不到同一份。 */
  const char* user_data_dir;
  /* 唯讀的隨附資料目錄：內建 schema 與詞庫。 */
  const char* shared_data_dir;
  /* NULL = 交給 librime 決定（暫存目錄）；"" = 只寫 stderr，不落地。
   * 兩者語意不同，不要混用。 */
  const char* log_dir;
  /* 寫進 librime 的 distribution 資訊(以及使用者目錄下的 installation.yaml)。
   * 慣例是 `rime.<前端>`,同 rime.weasel / rime.squirrel;各端再自己接平台,
   * 例:"rime.luminakey.android"。字根定義在 scripts/lib/product.env 的
   * LIBRIME_APP_NAME_PREFIX —— 四端請從那裡取,不要各寫一個。 */
  const char* app_name;

  rs_deploy_callback on_deploy; /* 可為 NULL */
  void* userdata;
} rs_setup;

int32_t rs_abi_version(void);

/* 全域初始化，行程內只可呼叫一次。失敗時以 rs_last_error() 取得原因。
 *
 * rs_setup 內的所有字串都會被**複製**一份，本函式返回後呼叫端即可釋放。
 * （JNI 的 GetStringUTFChars、Swift 的暫時字串都不必特地保留。）
 *
 * ⚠ 兩個資料目錄若是相對路徑，本層會**以呼叫當下的 cwd 轉成絕對路徑**再交給
 *   librime。呼叫端因此不必自己算，但也不要指望「傳相對路徑之後再 chdir」
 *   還會跟著走 —— 綁定在 rs_init 的那一刻。
 *
 *   為什麼門面要管這件事：librime-lua 的路徑沙盒對相對路徑是 fail-closed 的，
 *   而它 fail 之後的症狀是**所有候選消失**（lua filter 載不起來 → librime
 *   讓整段 translation 變空 → 使用者打 nihao 上屏 "nihao"）。症狀離原因五層遠，
 *   四端各有一個踩得到的入口，所以在唯一的共用邊界上一次解決。
 *   細節見 core/src/rime_shell.cc 的 make_absolute()。 */
bool rs_init(const rs_setup* setup);
void rs_finalize(void);

/* 觸發部署（重新編譯 schema）。非同步，結果經由 rs_setup.on_deploy 回報。
 * 這是唯一允許跨執行緒呼叫的函式。 */
bool rs_deploy(void);

/* 回傳最近一次失敗的原因，永不為 NULL。指標在下一次 API 呼叫前有效。 */
const char* rs_last_error(void);

/* 把使用者詞典的未落地變更寫進磁碟，並輸出可攜的文字快照。
 *
 * ⚠ **為什麼需要這支，而不是直接複製 *.userdb/ 目錄：**
 *   librime 在使用者上屏之後，是把剛學到的詞放進一個記憶體裡的 leveldb
 *   WriteBatch，要等 FinishSession() 或 ~UserDictionary 才落地。也就是說
 *   **「使用者剛剛打的那些字」通常只存在於記憶體**。這時候複製目錄，拿到的是
 *   上一輪的詞庫 —— 能開、能用、大小差不多，只是少了最近的學習成果，
 *   **而且沒有任何錯誤訊息**。備份功能最不該有的就是這種失敗。
 *
 * ⚠ **本函式會銷毀所有 session。** librime 的 sync 以 cleanup_all_sessions()
 *   開頭。呼叫後既有的 rs_session 一律失效，前端必須重建 —— 與部署之後一樣。
 *   rs_session_alive() 會回報 false。
 *
 * ⚠ **非同步。** 與 rs_deploy() 共用 librime 的維護執行緒與同一組通知，
 *   結果經由 rs_setup.on_deploy 回報（RS_DEPLOY_SUCCESS / RS_DEPLOY_FAILURE）。
 *   同一時間只能有一個維護工作：部署進行中時本函式回傳 false。
 *   回傳 true 只代表「排程成功」，不代表已經寫完。
 *
 * 完成後，rs_sync_dir() 底下會有各詞典的 *.userdb.txt —— 那是 RIME 的正式
 * 可攜格式：純文字、可合併、跨版本，是四端交換詞庫時唯一該用的東西。 */
bool rs_sync_user_data(void);

/* 同步目錄的絕對路徑，永不為 NULL（未初始化時為空字串）。
 * 指標在下一次 API 呼叫前有效，需要保留請自行複製。
 *
 * 沒有這支的話 rs_sync_user_data() 是不可用的：同步得動，但找不到產物。 */
const char* rs_sync_dir(void);

/* ───────────────────────── Session ───────────────────────── */

rs_session rs_session_create(void);
void rs_session_destroy(rs_session s);
/* 部署後舊 session 會失效，前端須據此重建。 */
bool rs_session_alive(rs_session s);

/* ───────────────────────── 按鍵輸入 ─────────────────────────
 *
 * ⚠ 四端接入的最大工作量在此。
 *
 * librime 沿用 X11 keysym 與 modifier 定義（見 librime 的 rime_keytable.h），
 * 而四端的原生鍵碼各不相同：
 *   Android  KeyEvent.KEYCODE_*  ／ iOS  鍵盤擴展自繪，由佈局直接產生 keysym
 *   macOS    NSEvent keyCode + charactersIgnoringModifiers
 *   Windows  TSF 收到的 VK_* + 鍵盤狀態表
 *
 * 每一端都必須自行實作「原生鍵碼 → X11 keysym」的映射表，
 * 且此映射受使用者實體鍵盤佈局影響（QWERTY / Dvorak / 各國佈局），
 * 不可寫死。行動端因為鍵盤是自繪的，可直接由 layout yaml 指定 keysym，
 * 是四端中唯一能繞開這個問題的形態 —— 這也是先做 Android 的理由之一。
 */

typedef enum {
  RS_MOD_SHIFT   = 1 << 0,
  RS_MOD_CONTROL = 1 << 1,
  RS_MOD_ALT     = 1 << 2,
  RS_MOD_SUPER   = 1 << 3,  /* Win 鍵 / Command */
  RS_MOD_CAPS    = 1 << 4,
  RS_MOD_RELEASE = 1 << 5,  /* 置位表示這是 key-up 事件 */
} rs_modifier;

/* 回傳 true 表示此按鍵已被輸入法消費，宿主不應再處理。 */
bool rs_process_key(rs_session s, int32_t keysym, uint32_t modifiers);

/* 直接選字／翻頁／清除，供 UI 點擊使用（不必偽造按鍵）。 */
bool rs_select_candidate(rs_session s, int32_t index_on_page);
bool rs_delete_candidate(rs_session s, int32_t index_on_page); /* 刪除使用者詞 */
/* 移動高亮但不選中。供佈局的 candidate:next / candidate:prev 之類的動作使用：
 * 前端從快照讀 menu.highlighted，加減一之後呼叫本函式。 */
bool rs_highlight_candidate(rs_session s, int32_t index_on_page);

bool rs_change_page(rs_session s, bool backward);
bool rs_clear_composition(rs_session s);

/* 把目前的組字內容直接上屏。
 *
 * ⚠ 方案之間行為不同，前端不可假設「選字＝上屏」：
 *   - 拼音類方案（luna_pinyin）選字當下就會產生 commit。
 *   - 注音類方案（bopomofo）選字之後仍停留在組字狀態，
 *     此時 status.is_composing 依然為 true，preedit 已是選中的文字，
 *     必須由前端明確呼叫本函式（或送出 Enter）才會真正上屏。
 *
 * ⚠ 光看 is_composing 是不夠的。它無法區分兩種狀態：
 *     (a) 整句轉換完成，只差確認  → 該 commit
 *     (b) 只選了第一段，後面還有  → **不該** commit，否則會吃掉後半段
 *
 * 判別條件是 menu.count。以下政策已在模擬器上用多音節輸入實測驗證：
 *
 *     menu.count > 0                  → 還有段落待選，繼續選字，不要 commit
 *     count == 0 && is_composing      → 轉換完成待確認，呼叫本函式
 *     count == 0 && !is_composing     → 已經結束，什麼都不用做
 *
 * 實測依據（見 tools/rime_console.cc）：部分選字之後，librime 一定會為
 * 剩餘段落給出新的候選，所以 menu.count 必然大於 0。例如注音輸入
 * ㄋㄧˇㄏㄠˇ 選了只覆蓋第一音節的「你」之後，preedit 變成「你ㄏㄠˇ」
 * 且 count=4；要等到 count 歸零，整串才算轉換完畢。
 *
 * 回傳 true 代表有待讀取的 commit 文字，下一次 rs_snapshot_acquire()
 * 會在 commit_text 拿到它。
 */
bool rs_commit_composition(rs_session s);

/* 直接改寫「目前正在打的那一串輸入」,並讓引擎重新計算候選。
 *
 * ⚠ **為什麼需要這一支:九宮格的音節消歧只能這樣做。**
 *   一顆鍵三四個字母,「MG」可能是 ni 也可能是 mi。使用者點了 ni 之後,
 *   引擎必須知道「第一個音節確定是 ni,後面仍然模糊」。
 *
 *   librime **沒有**「選擇某個拼寫」的 API —— 上游作者明講過(rime/librime#123):
 *   這是前端的工作,做法是**把輸入串改寫掉**(把那一段數字碼換成精確的拼音),
 *   而這要求方案是**雙編碼**的:精確拼音與模糊碼共存於同一個 prism。
 *
 *   rs_select_candidate() 做不到這件事:它確定的是**字**,不是**音節**。
 *
 * ⚠ 這是「重打一次」而不是「附加」:引擎會把整串重新切分。呼叫端要自己保證
 *   新的字串在方案的 alphabet 裡,否則會被靜靜地丟掉一部分。
 *
 * 回傳 false 代表引擎拒絕(session 無效,或字串裡有 alphabet 不認得的字元)。 */
bool rs_set_input(rs_session s, const char* input);

/* 取得引擎目前持有的輸入串(不是 preedit —— preedit 是顯示用的,
 * 可能已經被 speller 加了分隔符或轉寫過)。永不為 NULL;
 * 指標在下一次 API 呼叫前有效。
 *
 * 沒有這一支的話 rs_set_input() 很難用對:改寫之前要先知道現在是什麼。 */
const char* rs_get_input(rs_session s);

/* ───────────────────────── 狀態快照 ───────────────────────── */

typedef struct {
  const char* preedit;   /* 組字串，UTF-8；無組字時為空字串而非 NULL */
  int32_t sel_start;     /* 以 UTF-8 位元組為單位的選取區間 */
  int32_t sel_end;
  int32_t caret;
} rs_composition;

typedef struct {
  const char* text;      /* 候選文字 */
  const char* comment;   /* 註解，例如拼音或編碼提示；可為空字串 */
  const char* label;     /* 序號標籤，例如 "1"、"①"；可為空字串 */
} rs_candidate;

typedef struct {
  const rs_candidate* items;
  int32_t count;         /* 本頁候選數 */
  int32_t page_no;       /* 由 0 起算 */
  int32_t highlighted;   /* 本頁中高亮的索引；無候選時為 -1 */
  bool is_last_page;
} rs_menu;

/* 引擎目前**實際套用**的字形轉換。
 *
 * ⚠ 為什麼不用 is_simplified：那個欄位是 librime 的 RimeStatus 原樣轉過來
 *   的，而它只反映 `simplification` 這一個開關。**本專案打包的方案通通
 *   沒有那個開關** —— luna_pinyin 家族與 bopomofo 家族用的是一組互斥的
 *   radio（zh_hant / zh_hans / zh_hant_hk / zh_hant_tw）。
 *
 *   而 rs_set_option 對一個不存在的選項**不會失敗**：librime 只是記下一個
 *   沒有人讀的選項，然後原樣回讀。於是前端從 is_simplified 讀到的一直是
 *   它自己剛寫進去的偏好，不是引擎的狀態 —— 畫面在替一件沒有發生的事
 *   作證。Windows 端實機回報過：設定裡選簡體，狀態列畫「简」，打出來是
 *   繁體。
 *
 * RS_VARIANT_UNKNOWN 是一個真實而且正確的狀態，不是錯誤：純 luna_pinyin
 * 的那組 radio **沒有 `reset:`**，而 ConcreteEngine::InitializeOptions()
 * 只在 reset_value >= 0 時才設值 —— 所以剛載入時四個全是 false，而輸出是
 * 繁體（詞典本身就是繁體字集，沒有任何 simplifier 生效）。此時我們確實
 * 不知道引擎在做哪一種轉換，前端該做的是**整格不顯示**，不是猜一個。
 *
 * ⚠ 殘留：一個既沒有那組 radio、也沒有 simplification 的第三方方案，
 *   rs_set_option(zh_hans, true) 仍然會被記下並回讀，這裡仍然會回
 *   RS_VARIANT_HANS 而輸出沒變。用今天的 rs_ API 問不出「這個方案有沒有
 *   宣告這個選項」（本標頭只有 rs_set_option / rs_get_option，沒有任何
 *   config API）。真解是新增 rs_schema_declares_option()，已開工單。 */
typedef enum {
  RS_VARIANT_UNKNOWN = 0,  /* 四個 radio 都為假 —— 前端應整格不顯示 */
  RS_VARIANT_HANT = 1,     /* zh_hant / zh_hant_hk / zh_hant_tw 任一為真 */
  RS_VARIANT_HANS = 2      /* zh_hans 為真（優先於上面那一條） */
} rs_variant;

typedef struct {
  const char* schema_id;
  const char* schema_name;
  bool is_composing;
  bool is_ascii_mode;    /* 中／英 */
  bool is_full_shape;    /* 全／半形 */
  /* ⚠ **只反映 `simplification` 這一個開關。**
   *   本專案打包的方案都沒有它，讀它等於讀自己寫進去的回音。
   *   要判斷簡繁請用下面的 variant。保留這個欄位是因為它是 librime 的
   *   原欄位，第三方「簡入繁出」之類的方案真的靠它。 */
  bool is_simplified;
  bool is_ascii_punct;
  bool is_disabled;      /* 部署中等不可用狀態 */
  /* 引擎實際套用的字形轉換。見上面 rs_variant 的說明。
   * ⚠ 這是新增欄位（ABI 純加法）。macOS / Android 兩端可以各自挑時間
   *   接上，接上之前行為不變。 */
  rs_variant variant;
} rs_status;

typedef struct {
  rs_composition composition;
  rs_menu menu;
  rs_status status;
  /* 本次應上屏的文字；無則為 NULL。前端消費後該次 commit 即被清除。 */
  const char* commit_text;
} rs_snapshot;

/* 取得快照。回傳的指標在下一次 acquire / release 前有效，見檔頭記憶體約定。
 * session 失效時回傳 NULL。 */
const rs_snapshot* rs_snapshot_acquire(rs_session s);
void rs_snapshot_release(rs_session s);

/* ───────────────────────── Schema 與選項 ───────────────────── */

/* 列舉已啟用的 schema。回傳總數（可能大於 capacity）；
 * out_ids / out_names 可為 NULL，用來先查詢總數再配置空間。
 *
 * 字串的生命週期**與快照無關**，兩者各自使用獨立的緩衝：
 * 這些指標在下一次 rs_schema_list() 之前有效，不受 rs_snapshot_acquire()
 * 或 rs_snapshot_release() 影響，交錯呼叫也不會互相踩。
 * 即便如此，仍建議取得後立刻複製。
 *
 * 本函式不吃 rs_session —— schema 清單是全域的，不屬於任何 session。 */
int32_t rs_schema_list(const char** out_ids, const char** out_names, int32_t capacity);

bool rs_select_schema(rs_session s, const char* schema_id);

/* librime 的具名開關，例如 "ascii_mode"、"simplification"、"full_shape"。 */
bool rs_set_option(rs_session s, const char* option, bool value);
bool rs_get_option(rs_session s, const char* option);

/* ───────────────────── keysym 查表 ─────────────────────
 *
 * 這兩個函式是純查表，**不需要 rs_init()**，也不涉及任何 session。
 * 存在的理由是佈局檔（core/layouts/*.yaml）以名稱指定 keysym，
 * 而各端不應該各自維護一份會腐爛的名稱表 —— librime 自己就有權威的那一份。
 */

/* 由 X11 keysym 名稱取得 keysym 值，例如 "BackSpace" → 0xFF08。
 * **查不到回傳 0**。（注意：librime 內部查不到時回傳的是 XK_VoidSymbol
 * 0xffffff，那是個看起來很像有效 keysym 的值；本層一律正規化成 0，
 * 避免前端把未知的鍵當成有效鍵送進引擎。） */
int32_t rs_keysym_by_name(const char* name);

/* 反向查詢，主要供除錯與日誌使用。查不到回傳 NULL。
 * 回傳的是 librime 內部的靜態字串，永久有效，不需要複製或釋放。 */
const char* rs_keysym_name(int32_t keysym);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* RIME_SHELL_H_ */
