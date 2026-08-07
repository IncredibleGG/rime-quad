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
#define RIME_SHELL_ABI_VERSION 1

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
  const char* app_name;     /* 例："rime.android"，會寫進 librime 的 distribution 資訊 */

  rs_deploy_callback on_deploy; /* 可為 NULL */
  void* userdata;
} rs_setup;

int32_t rs_abi_version(void);

/* 全域初始化，行程內只可呼叫一次。失敗時以 rs_last_error() 取得原因。
 *
 * rs_setup 內的所有字串都會被**複製**一份，本函式返回後呼叫端即可釋放。
 * （JNI 的 GetStringUTFChars、Swift 的暫時字串都不必特地保留。） */
bool rs_init(const rs_setup* setup);
void rs_finalize(void);

/* 觸發部署（重新編譯 schema）。非同步，結果經由 rs_setup.on_deploy 回報。
 * 這是唯一允許跨執行緒呼叫的函式。 */
bool rs_deploy(void);

/* 回傳最近一次失敗的原因，永不為 NULL。指標在下一次 API 呼叫前有效。 */
const char* rs_last_error(void);

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

typedef struct {
  const char* schema_id;
  const char* schema_name;
  bool is_composing;
  bool is_ascii_mode;    /* 中／英 */
  bool is_full_shape;    /* 全／半形 */
  bool is_simplified;    /* 簡／繁 */
  bool is_ascii_punct;
  bool is_disabled;      /* 部署中等不可用狀態 */
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
