// rime_shell.cc — rime_shell.h 的實作，librime 之上的薄門面。
//
// 設計要點（四端共用，改動請先讀完這段）：
//
// 1. librime 1.17 起不再直接匯出 RimeSetup / RimeInitialize 等 C 函式，
//    只保留 rime_get_api() 回傳的函式指標表。本檔一律走該表。
//
// 2. librime 的 get_context / get_status / get_commit 取出的結構必須配對
//    free_*，且其中字串在 free 之後即失效。本層在 acquire 當下就把所有
//    字串深拷貝進 session 自有的 storage，然後**立刻**把 librime 結構釋放。
//    因此即使前端忘了呼叫 rs_snapshot_release，也不會洩漏 librime 的記憶體，
//    只是 storage 會保留到下一次 acquire。這是刻意的取捨：寧可多一次拷貝，
//    也不要把「跨語言邊界的生命週期管理」丟給 JNI / Swift 那一側。
//
// 3. librime 的 self-versioned struct 靠 data_size 做版本協商，
//    存取新欄位前必須用 RIME_PROVIDED / RIME_STRUCT_HAS_MEMBER 檢查，
//    否則接上舊版 librime 會讀到越界記憶體。

#include "rime_shell.h"

#include <rime_api.h>

#include <atomic>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// librime 私有標頭 rime/key_table.h 的宣告，在此就地重宣告而不 #include 它。
//
// 理由：那份私有標頭會連帶要求一整組 compile definitions（RIME_VERSION、
// GLOG_EXPORT=、YAML_CPP_STATIC_DEFINE、Opencc_BUILT_AS_STATIC…）才能與
// librime.a 的 ABI 對齊，為了兩個查表函式把那些全拉進來並不划算。
//
// 這兩個符號是 **C++ 連結**（key_table.h 沒有包在 extern "C" 裡，RIME_DLL
// 只是可見性屬性），所以在 C++ 中以相同簽章重宣告即可對上同一個 mangled
// 名稱。簽章若在上游變動，結果是連結錯誤而不是靜默失效。
int RimeGetKeycodeByName(const char* name);
const char* RimeGetKeyName(int keycode);

// RimeGetKeycodeByName 查不到時回傳的是 XK_VoidSymbol，不是 0。
#define RS_XK_VOID_SYMBOL 0xffffff

namespace {

RimeApi* g_api = nullptr;
// 由 with_session 等路徑在不持鎖的情況下讀取，故用 atomic 而非裸 bool。
std::atomic<bool> g_initialized{false};
std::mutex g_global_mutex;

rs_deploy_callback g_deploy_cb = nullptr;
void* g_deploy_userdata = nullptr;
bool g_deploy_in_flight = false;

// rs_setup 內的字串一律複製一份自己持有，這樣呼叫端（JNI local ref、Swift
// 暫時字串）不必煩惱要保留到什麼時候。librime 是否複製 RimeTraits 的字串
// 沒有明文保證，不要賭。
std::string g_shared_dir, g_user_dir, g_log_dir, g_app_name;
bool g_has_log_dir = false;

// rs_last_error 的約定是「永不回傳 NULL」，且不同執行緒互不干擾。
thread_local std::string t_last_error;

void set_error(const char* msg) {
  t_last_error = msg ? msg : "";
}

// ── 快照 storage ─────────────────────────────────────────────
// rs_snapshot 內全是裸指標，指向這裡的字串。字串一旦 reserve 之後再 append
// 就可能搬家，所以務必「先全部填好 strings，最後才計算指標」。
struct Storage {
  std::string preedit;
  std::string commit;
  std::string schema_id;
  std::string schema_name;
  std::vector<std::string> cand_text;
  std::vector<std::string> cand_comment;
  std::vector<std::string> cand_label;
  std::vector<rs_candidate> cands;  // 指向上面三個 vector 的內容

  void clear() {
    preedit.clear();
    commit.clear();
    schema_id.clear();
    schema_name.clear();
    cand_text.clear();
    cand_comment.clear();
    cand_label.clear();
    cands.clear();
  }
};

struct Session {
  RimeSessionId id = 0;
  std::mutex mutex;
  Storage storage;
  rs_snapshot snapshot{};
  bool has_commit = false;
};

Session* as_session(rs_session s) {
  return reinterpret_cast<Session*>(s);
}

// ── 通知回呼 ─────────────────────────────────────────────────
void notification_handler(void* /*context*/,
                          RimeSessionId /*session_id*/,
                          const char* message_type,
                          const char* message_value) {
  if (!message_type || !message_value)
    return;
  if (std::strcmp(message_type, "deploy") != 0)
    return;

  rs_deploy_status status;
  if (std::strcmp(message_value, "start") == 0) {
    status = RS_DEPLOY_RUNNING;
  } else if (std::strcmp(message_value, "success") == 0) {
    status = RS_DEPLOY_SUCCESS;
  } else if (std::strcmp(message_value, "failure") == 0) {
    status = RS_DEPLOY_FAILURE;
  } else {
    return;
  }

  // 這裡是 librime 的維護執行緒在呼叫，不要在持鎖狀態下回呼上層，
  // 上層（例如 Android 的 JNI）可能會 attach 執行緒或跳回 UI thread。
  rs_deploy_callback cb;
  void* ud;
  {
    std::lock_guard<std::mutex> lock(g_global_mutex);
    if (status != RS_DEPLOY_RUNNING)
      g_deploy_in_flight = false;
    cb = g_deploy_cb;
    ud = g_deploy_userdata;
  }
  if (cb)
    cb(status, ud);
}

// 把 rs_modifier 轉成 librime（X11）的 modifier 遮罩。
// 注意 kSuperMask 是 1<<26，不是 1<<6 —— 見 librime src/rime/key_table.h。
int to_rime_mask(uint32_t mods) {
  int mask = 0;
  if (mods & RS_MOD_SHIFT)
    mask |= (1 << 0);  // kShiftMask
  if (mods & RS_MOD_CAPS)
    mask |= (1 << 1);  // kLockMask
  if (mods & RS_MOD_CONTROL)
    mask |= (1 << 2);  // kControlMask
  if (mods & RS_MOD_ALT)
    mask |= (1 << 3);  // kAltMask == kMod1Mask
  if (mods & RS_MOD_SUPER)
    mask |= (1 << 26);  // kSuperMask
  if (mods & RS_MOD_RELEASE)
    mask |= (1 << 30);  // kReleaseMask
  return mask;
}

// 候選標籤的優先序：context.select_labels > menu.select_keys > 序號。
// select_labels 是 v0.9.2 之後才有的欄位，必須做版本檢查。
std::string pick_label(RimeContext& ctx, int index) {
  if (RIME_PROVIDED(&ctx, select_labels)) {
    const char* label = ctx.select_labels[index];
    if (label && *label)
      return label;
  }
  if (ctx.menu.select_keys) {
    const char* keys = ctx.menu.select_keys;
    if (index < static_cast<int>(std::strlen(keys)))
      return std::string(1, keys[index]);
  }
  return std::to_string(index + 1);
}

// 重建快照。呼叫端必須持有 session 的鎖。
void rebuild_snapshot(Session* sess) {
  Storage& st = sess->storage;
  st.clear();
  sess->snapshot = rs_snapshot{};
  sess->has_commit = false;

  // ── commit ──
  // librime 的 get_commit 具有「取出即清除」語意，所以 acquire 每次呼叫
  // 都會消費掉待上屏的文字。前端每個輸入事件只可 acquire 一次。
  RIME_STRUCT(RimeCommit, commit);
  if (g_api->get_commit(sess->id, &commit)) {
    if (commit.text) {
      st.commit = commit.text;
      sess->has_commit = true;
    }
    g_api->free_commit(&commit);
  }

  // ── context ──
  RIME_STRUCT(RimeContext, ctx);
  if (g_api->get_context(sess->id, &ctx)) {
    if (ctx.composition.preedit)
      st.preedit = ctx.composition.preedit;

    sess->snapshot.composition.sel_start = ctx.composition.sel_start;
    sess->snapshot.composition.sel_end = ctx.composition.sel_end;
    sess->snapshot.composition.caret = ctx.composition.cursor_pos;

    const int n = ctx.menu.num_candidates;
    st.cand_text.reserve(n);
    st.cand_comment.reserve(n);
    st.cand_label.reserve(n);
    for (int i = 0; i < n; ++i) {
      const RimeCandidate& c = ctx.menu.candidates[i];
      st.cand_text.emplace_back(c.text ? c.text : "");
      st.cand_comment.emplace_back(c.comment ? c.comment : "");
      st.cand_label.emplace_back(pick_label(ctx, i));
    }

    sess->snapshot.menu.count = n;
    sess->snapshot.menu.page_no = ctx.menu.page_no;
    sess->snapshot.menu.is_last_page = ctx.menu.is_last_page != 0;
    sess->snapshot.menu.highlighted =
        n > 0 ? ctx.menu.highlighted_candidate_index : -1;

    g_api->free_context(&ctx);
  } else {
    sess->snapshot.menu.highlighted = -1;
  }

  // ── status ──
  RIME_STRUCT(RimeStatus, status);
  if (g_api->get_status(sess->id, &status)) {
    if (status.schema_id)
      st.schema_id = status.schema_id;
    if (status.schema_name)
      st.schema_name = status.schema_name;
    sess->snapshot.status.is_composing = status.is_composing != 0;
    sess->snapshot.status.is_ascii_mode = status.is_ascii_mode != 0;
    sess->snapshot.status.is_full_shape = status.is_full_shape != 0;
    sess->snapshot.status.is_simplified = status.is_simplified != 0;
    sess->snapshot.status.is_ascii_punct = status.is_ascii_punct != 0;
    sess->snapshot.status.is_disabled = status.is_disabled != 0;
    g_api->free_status(&status);
  }

  // ── variant：引擎**實際套用**的字形轉換 ──
  //
  // ⚠ 刻意不看 is_simplified／`simplification`。理由寫在 rime_shell.h 的
  //   rs_variant 上：那個開關本專案打包的方案通通沒有，而 set_option 對
  //   不存在的選項不會失敗、會原樣記下並回讀 —— 讀它等於把前端自己
  //   寫進去的偏好當成引擎的狀態回報。
  //
  // 讀的是 luna_pinyin 家族那組互斥的 radio。四個都為假是一個真實而且
  // 正確的狀態（radio group 沒有 reset:，剛載入時就是這樣），回
  // RS_VARIANT_UNKNOWN，由前端決定怎麼呈現（Windows 端是整格不顯示）。
  //
  // ⚠ 順序是規範性的：zh_hans 優先。set_option 不維持 radio 的互斥，
  //   而前端套用時是「先關再開」—— 中間真的存在兩個都為真的一瞬。
  {
    const bool zh_hans = g_api->get_option(sess->id, "zh_hans") != False;
    const bool zh_hant = g_api->get_option(sess->id, "zh_hant") != False;
    const bool zh_hant_hk = g_api->get_option(sess->id, "zh_hant_hk") != False;
    const bool zh_hant_tw = g_api->get_option(sess->id, "zh_hant_tw") != False;
    sess->snapshot.status.variant =
        zh_hans ? RS_VARIANT_HANS
                : ((zh_hant || zh_hant_hk || zh_hant_tw) ? RS_VARIANT_HANT
                                                         : RS_VARIANT_UNKNOWN);
  }

  // ── 所有 string 都定案了，現在才計算指標 ──
  st.cands.reserve(st.cand_text.size());
  for (size_t i = 0; i < st.cand_text.size(); ++i) {
    st.cands.push_back(rs_candidate{st.cand_text[i].c_str(),
                                    st.cand_comment[i].c_str(),
                                    st.cand_label[i].c_str()});
  }

  sess->snapshot.composition.preedit = st.preedit.c_str();
  sess->snapshot.menu.items = st.cands.empty() ? nullptr : st.cands.data();
  sess->snapshot.status.schema_id = st.schema_id.c_str();
  sess->snapshot.status.schema_name = st.schema_name.c_str();
  sess->snapshot.commit_text = sess->has_commit ? st.commit.c_str() : nullptr;
}

// rs_schema_list 的回傳字串需在呼叫後仍然有效，存在這裡。
std::mutex g_schema_mutex;
std::vector<std::string> g_schema_ids;
std::vector<std::string> g_schema_names;

}  // namespace

// ─────────────────────────────────────────────────────────────
// 初始化
// ─────────────────────────────────────────────────────────────

int32_t rs_abi_version(void) {
  return RIME_SHELL_ABI_VERSION;
}

const char* rs_last_error(void) {
  return t_last_error.c_str();
}

bool rs_init(const rs_setup* setup) {
  std::lock_guard<std::mutex> lock(g_global_mutex);

  if (g_initialized) {
    set_error("rs_init 已被呼叫過；每個行程只可初始化一次");
    return false;
  }
  if (!setup || !setup->user_data_dir || !setup->shared_data_dir) {
    set_error("rs_setup 缺少 user_data_dir 或 shared_data_dir");
    return false;
  }

  g_api = rime_get_api();
  if (!g_api) {
    set_error("rime_get_api() 回傳 NULL：librime 未正確連結");
    return false;
  }

  g_shared_dir = setup->shared_data_dir;
  g_user_dir = setup->user_data_dir;
  g_has_log_dir = setup->log_dir != nullptr;
  g_log_dir = g_has_log_dir ? setup->log_dir : "";
  g_app_name = setup->app_name ? setup->app_name : "rime.shell";

  RIME_STRUCT(RimeTraits, traits);
  traits.shared_data_dir = g_shared_dir.c_str();
  traits.user_data_dir = g_user_dir.c_str();
  // NULL = 暫存目錄，"" = 只寫 stderr。兩者語意不同，不可混為一談。
  traits.log_dir = g_has_log_dir ? g_log_dir.c_str() : nullptr;
  traits.app_name = g_app_name.c_str();
  traits.distribution_name = "Rime";
  traits.distribution_code_name = "rime-shell";
  traits.distribution_version = "0.1.0";
  traits.min_log_level = 1;  // WARNING 起；行動端不需要 INFO 洪流

  g_deploy_cb = setup->on_deploy;
  g_deploy_userdata = setup->userdata;

  g_api->setup(&traits);
  // 必須在 initialize 之前掛上，否則初次部署的通知會漏接。
  g_api->set_notification_handler(notification_handler, nullptr);
  g_api->initialize(&traits);

  // full_check=True：首次啟動或資料變更時重新編譯 schema。
  // 回傳 True 代表確實進入了維護模式，部署結果會經由通知回呼送達。
  g_api->start_maintenance(True);

  g_initialized = true;
  set_error("");
  return true;
}

void rs_finalize(void) {
  std::lock_guard<std::mutex> lock(g_global_mutex);
  if (!g_initialized)
    return;
  g_api->set_notification_handler(nullptr, nullptr);
  g_api->finalize();
  g_initialized = false;
  g_deploy_cb = nullptr;
  g_deploy_userdata = nullptr;
  g_api = nullptr;
}

bool rs_deploy(void) {
  {
    std::lock_guard<std::mutex> lock(g_global_mutex);
    if (!g_initialized) {
      set_error("尚未初始化");
      return false;
    }
    if (g_deploy_in_flight) {
      set_error("已有部署進行中");
      return false;
    }
    g_deploy_in_flight = true;
  }

  // librime 的 deploy() 是同步的，會跑數秒到數十秒。開一條 detached 執行緒，
  // 進度與結果一律經由 notification_handler 回報，維持本函式「可跨執行緒呼叫」
  // 的契約。
  std::thread([] {
    RimeApi* api = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_global_mutex);
      api = g_api;
    }
    if (!api)
      return;
    bool ok = api->deploy() != False;
    std::lock_guard<std::mutex> lock(g_global_mutex);
    if (g_deploy_in_flight) {
      // librime 沒發通知就自己補一個，避免上層永遠等不到終態。
      g_deploy_in_flight = false;
      if (g_deploy_cb)
        g_deploy_cb(ok ? RS_DEPLOY_SUCCESS : RS_DEPLOY_FAILURE,
                    g_deploy_userdata);
    }
  }).detach();

  return true;
}

bool rs_sync_user_data(void) {
  // 與 rs_deploy() 共用同一支維護執行緒與同一組通知，所以也共用那個旗標。
  // 兩者同時跑的話，librime 會把後來的那次工作直接丟掉而**不報錯**，
  // 上層永遠等不到終態。
  {
    std::lock_guard<std::mutex> lock(g_global_mutex);
    if (!g_initialized) {
      set_error("尚未初始化");
      return false;
    }
    if (g_deploy_in_flight) {
      set_error("已有部署或同步進行中");
      return false;
    }
    g_deploy_in_flight = true;
  }

  std::thread([] {
    RimeApi* api = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_global_mutex);
      api = g_api;
    }
    if (!api)
      return;
    // sync_user_data() 內部以 cleanup_all_sessions() 開頭，所以所有 session
    // 在這一刻失效。契約寫在 rime_shell.h，前端必須重建。
    bool ok = api->sync_user_data() != False;
    std::lock_guard<std::mutex> lock(g_global_mutex);
    if (g_deploy_in_flight) {
      g_deploy_in_flight = false;
      if (g_deploy_cb)
        g_deploy_cb(ok ? RS_DEPLOY_SUCCESS : RS_DEPLOY_FAILURE,
                    g_deploy_userdata);
    }
  }).detach();

  return true;
}

const char* rs_sync_dir(void) {
  // 用 get_sync_dir_s()（帶長度的那支）而不是 get_sync_dir()：後者在
  // librime 1.17 已標記 deprecated，回傳的是內部靜態緩衝區。
  static thread_local char buf[1024];
  buf[0] = '\0';
  std::lock_guard<std::mutex> lock(g_global_mutex);
  if (!g_initialized || !g_api)
    return buf;
  if (g_api->get_sync_dir_s)
    g_api->get_sync_dir_s(buf, sizeof(buf));
  else if (g_api->get_sync_dir) {
    const char* d = g_api->get_sync_dir();
    if (d) {
      std::snprintf(buf, sizeof(buf), "%s", d);
    }
  }
  return buf;
}

// ─────────────────────────────────────────────────────────────
// Session
// ─────────────────────────────────────────────────────────────

rs_session rs_session_create(void) {
  std::lock_guard<std::mutex> lock(g_global_mutex);
  if (!g_initialized) {
    set_error("尚未初始化");
    return RS_INVALID_SESSION;
  }
  RimeSessionId id = g_api->create_session();
  if (!id) {
    set_error("librime create_session 失敗（通常代表部署尚未完成）");
    return RS_INVALID_SESSION;
  }
  Session* sess = new Session();
  sess->id = id;
  sess->snapshot.menu.highlighted = -1;
  return reinterpret_cast<rs_session>(sess);
}

void rs_session_destroy(rs_session s) {
  Session* sess = as_session(s);
  if (!sess)
    return;
  {
    std::lock_guard<std::mutex> lock(g_global_mutex);
    if (g_initialized && sess->id)
      g_api->destroy_session(sess->id);
  }
  delete sess;
}

bool rs_session_alive(rs_session s) {
  Session* sess = as_session(s);
  if (!sess)
    return false;
  std::lock_guard<std::mutex> lock(g_global_mutex);
  return g_initialized && g_api->find_session(sess->id) != False;
}

// ─────────────────────────────────────────────────────────────
// 輸入
// ─────────────────────────────────────────────────────────────

namespace {
// 所有會改變 session 狀態的操作走同一條路徑：檢查、上鎖、呼叫。
template <typename Fn>
bool with_session(rs_session s, Fn&& fn) {
  Session* sess = as_session(s);
  if (!sess) {
    set_error("session 為 NULL");
    return false;
  }
  if (!g_initialized) {
    set_error("尚未初始化");
    return false;
  }
  std::lock_guard<std::mutex> lock(sess->mutex);
  return fn(sess);
}
}  // namespace

bool rs_process_key(rs_session s, int32_t keysym, uint32_t modifiers) {
  return with_session(s, [&](Session* sess) {
    return g_api->process_key(sess->id, keysym, to_rime_mask(modifiers)) !=
           False;
  });
}

bool rs_select_candidate(rs_session s, int32_t index_on_page) {
  return with_session(s, [&](Session* sess) {
    return g_api->select_candidate_on_current_page(
               sess->id, static_cast<size_t>(index_on_page)) != False;
  });
}

bool rs_delete_candidate(rs_session s, int32_t index_on_page) {
  return with_session(s, [&](Session* sess) {
    return g_api->delete_candidate_on_current_page(
               sess->id, static_cast<size_t>(index_on_page)) != False;
  });
}

bool rs_highlight_candidate(rs_session s, int32_t index_on_page) {
  return with_session(s, [&](Session* sess) {
    return g_api->highlight_candidate_on_current_page(
               sess->id, static_cast<size_t>(index_on_page)) != False;
  });
}

bool rs_change_page(rs_session s, bool backward) {
  return with_session(s, [&](Session* sess) {
    return g_api->change_page(sess->id, backward ? True : False) != False;
  });
}

bool rs_clear_composition(rs_session s) {
  return with_session(s, [&](Session* sess) {
    g_api->clear_composition(sess->id);
    return true;
  });
}

bool rs_set_input(rs_session s, const char* input) {
  const char* text = input ? input : "";
  return with_session(s, [&](Session* sess) {
    // librime 的 set_input 會把整串重新切分並重算候選,等同「重打一次」。
    return g_api->set_input(sess->id, text) != False;
  });
}

const char* rs_get_input(rs_session s) {
  // ⚠ 不能用 with_session:它回傳 bool。這裡自己走同一套檢查,
  //    並把結果複製進 thread_local 緩衝 —— 直接把 librime 的指標交出去,
  //    生命週期就綁在它的內部狀態上,而契約承諾的是「下一次 API 呼叫前有效」。
  static thread_local std::string buf;
  buf.clear();
  Session* sess = as_session(s);
  if (!sess || !g_initialized)
    return buf.c_str();
  std::lock_guard<std::mutex> lock(sess->mutex);
  const char* p = g_api->get_input(sess->id);
  if (p)
    buf.assign(p);
  return buf.c_str();
}

bool rs_commit_composition(rs_session s) {
  return with_session(s, [&](Session* sess) {
    // librime 的回傳值是「是否有待讀取的 commit 文字」，
    // 正好就是前端需要知道的：true 代表下一次 acquire 會拿到 commit_text。
    return g_api->commit_composition(sess->id) != False;
  });
}

// ─────────────────────────────────────────────────────────────
// 快照
// ─────────────────────────────────────────────────────────────

const rs_snapshot* rs_snapshot_acquire(rs_session s) {
  Session* sess = as_session(s);
  if (!sess) {
    set_error("session 為 NULL");
    return nullptr;
  }
  if (!g_initialized) {
    set_error("尚未初始化");
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(sess->mutex);
  if (g_api->find_session(sess->id) == False) {
    set_error("session 已失效（通常是部署後舊 session 被回收，請重建）");
    return nullptr;
  }
  rebuild_snapshot(sess);
  return &sess->snapshot;
}

void rs_snapshot_release(rs_session s) {
  Session* sess = as_session(s);
  if (!sess)
    return;
  std::lock_guard<std::mutex> lock(sess->mutex);
  // librime 的記憶體在 acquire 當下就已釋放，這裡只是讓快照失效，
  // 好讓「用完之後還拿著舊指標」這種錯誤盡早暴露。
  sess->storage.clear();
  sess->snapshot = rs_snapshot{};
  sess->snapshot.menu.highlighted = -1;
  sess->has_commit = false;
}

// ─────────────────────────────────────────────────────────────
// Schema 與選項
// ─────────────────────────────────────────────────────────────

int32_t rs_schema_list(const char** out_ids,
                       const char** out_names,
                       int32_t capacity) {
  std::lock_guard<std::mutex> global(g_global_mutex);
  if (!g_initialized) {
    set_error("尚未初始化");
    return 0;
  }

  RimeSchemaList list{};
  if (!g_api->get_schema_list(&list)) {
    set_error("get_schema_list 失敗");
    return 0;
  }

  std::lock_guard<std::mutex> lock(g_schema_mutex);
  g_schema_ids.clear();
  g_schema_names.clear();
  for (size_t i = 0; i < list.size; ++i) {
    g_schema_ids.emplace_back(list.list[i].schema_id ? list.list[i].schema_id
                                                     : "");
    g_schema_names.emplace_back(list.list[i].name ? list.list[i].name : "");
  }
  g_api->free_schema_list(&list);

  const int32_t total = static_cast<int32_t>(g_schema_ids.size());
  if (out_ids || out_names) {
    const int32_t n = total < capacity ? total : capacity;
    for (int32_t i = 0; i < n; ++i) {
      if (out_ids)
        out_ids[i] = g_schema_ids[i].c_str();
      if (out_names)
        out_names[i] = g_schema_names[i].c_str();
    }
  }
  return total;
}

bool rs_select_schema(rs_session s, const char* schema_id) {
  if (!schema_id) {
    set_error("schema_id 為 NULL");
    return false;
  }
  return with_session(s, [&](Session* sess) {
    return g_api->select_schema(sess->id, schema_id) != False;
  });
}

bool rs_set_option(rs_session s, const char* option, bool value) {
  if (!option) {
    set_error("option 為 NULL");
    return false;
  }
  return with_session(s, [&](Session* sess) {
    g_api->set_option(sess->id, option, value ? True : False);
    return true;
  });
}

/* ───────────────────── keysym 查表（不需初始化） ───────────────── */

int32_t rs_keysym_by_name(const char* name) {
  if (!name || !*name)
    return 0;
  const int code = RimeGetKeycodeByName(name);
  // 對外一律用 0 表示「未知」，不要把 librime 的 XK_VoidSymbol 洩漏出去 ——
  // 0xffffff 是個看起來很像有效 keysym 的值，前端若沒特別處理會把未知的鍵
  // 當成有效鍵送進 librime。
  return code == RS_XK_VOID_SYMBOL ? 0 : code;
}

const char* rs_keysym_name(int32_t keysym) {
  return RimeGetKeyName(keysym);
}

bool rs_get_option(rs_session s, const char* option) {
  if (!option)
    return false;
  bool result = false;
  with_session(s, [&](Session* sess) {
    result = g_api->get_option(sess->id, option) != False;
    return true;
  });
  return result;
}
