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
 * ── 記憶體約定 ─────────────────────────────────────────────────
 *   rs_snapshot_acquire() 回傳的指標（含其中所有字串）由本層擁有，
 *   在同一 session 的下一次 acquire 或 release 之前有效。
 *   前端必須在該窗口內複製出自己需要的資料，不得長期持有。
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
  const char* log_dir;      /* 可為 NULL，代表不落地日誌 */
  const char* app_name;     /* 例："rime.android"，會寫進 librime 的 distribution 資訊 */

  rs_deploy_callback on_deploy; /* 可為 NULL */
  void* userdata;
} rs_setup;

int32_t rs_abi_version(void);

/* 全域初始化，行程內只可呼叫一次。失敗時以 rs_last_error() 取得原因。 */
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
 * 正確的前端邏輯是：選字後檢查 is_composing，仍為 true 時才呼叫本函式。
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

/* 列舉已啟用的 schema。回傳個數；out 可為 NULL 以先查詢個數。
 * 字串生命週期同快照，前端須立即複製。 */
int32_t rs_schema_list(const char** out_ids, const char** out_names, int32_t capacity);

bool rs_select_schema(rs_session s, const char* schema_id);

/* librime 的具名開關，例如 "ascii_mode"、"simplification"、"full_shape"。 */
bool rs_set_option(rs_session s, const char* option, bool value);
bool rs_get_option(rs_session s, const char* option);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* RIME_SHELL_H_ */
