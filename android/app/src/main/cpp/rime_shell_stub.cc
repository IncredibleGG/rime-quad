/*
 * rime_shell_stub.cc — rime_shell.h 的假實作
 *
 * ⚠⚠⚠ 這不是真的輸入法。⚠⚠⚠
 *
 * 只在 third_party/prebuilt/<abi>/lib/librime.a 或 core/src/rime_shell.cc
 * 尚未產出時被 CMake 選中，目的是讓 Android 這條線能獨立建置、安裝、
 * 看到鍵盤與候選列，不必空等 native 那條線。
 *
 * 所有候選字都刻意加上「⟦STUB⟧」前綴，schema 名稱也寫死成假的，
 * 任何人看一眼畫面就知道自己在跑假資料。
 */

#include "rime_shell.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct StubSession {
  bool alive = false;
  std::string input;
  int32_t page_no = 0;
  int32_t highlighted = 0;
  std::string pending_commit;

  // 快照的字串後備儲存，生命週期比照 rime_shell.h 的記憶體約定：
  // 只保證到下一次 acquire / release 為止。
  std::vector<std::string> cand_text;
  std::vector<std::string> cand_comment;
  std::vector<std::string> cand_label;
  std::vector<rs_candidate> cand_items;
  std::string preedit;
  std::string commit_buf;
  rs_snapshot snapshot{};
  bool snapshot_held = false;
};

constexpr int kMaxSessions = 8;
StubSession g_sessions[kMaxSessions];

bool g_initialized = false;
bool g_ascii_mode = false;
bool g_full_shape = false;
bool g_simplified = false;
bool g_ascii_punct = false;
std::string g_last_error = "";
std::string g_user_data_dir;
std::string g_shared_data_dir;
rs_deploy_callback g_deploy_cb = nullptr;
void* g_deploy_userdata = nullptr;

StubSession* resolve(rs_session s) {
  if (s == RS_INVALID_SESSION) return nullptr;
  size_t idx = static_cast<size_t>(s) - 1;
  if (idx >= kMaxSessions) return nullptr;
  if (!g_sessions[idx].alive) return nullptr;
  return &g_sessions[idx];
}

const char* kStubSchemaId = "stub_fake_schema";
const char* kStubSchemaName = "⟦STUB⟧ 假 Schema（未接 librime）";

void rebuild_candidates(StubSession* ss) {
  ss->cand_text.clear();
  ss->cand_comment.clear();
  ss->cand_label.clear();

  if (ss->input.empty()) {
    ss->cand_items.clear();
    return;
  }

  // 依輸入長度生出幾個明顯是假的候選，讓 UI 看起來有反應。
  static const char* kFakeWords[] = {
      "假資料", "測試候選", "尚未接上", "librime", "STUB", "佔位符",
  };
  const int kPerPage = 6;
  int total_kinds = static_cast<int>(sizeof(kFakeWords) / sizeof(kFakeWords[0]));

  for (int i = 0; i < kPerPage; ++i) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "⟦STUB⟧%s%d",
                  kFakeWords[(i + ss->page_no) % total_kinds], i + 1);
    ss->cand_text.emplace_back(buf);

    std::snprintf(buf, sizeof(buf), "← 假的 · %s", ss->input.c_str());
    ss->cand_comment.emplace_back(buf);

    std::snprintf(buf, sizeof(buf), "%d", i + 1);
    ss->cand_label.emplace_back(buf);
  }

  ss->cand_items.resize(ss->cand_text.size());
  for (size_t i = 0; i < ss->cand_text.size(); ++i) {
    ss->cand_items[i].text = ss->cand_text[i].c_str();
    ss->cand_items[i].comment = ss->cand_comment[i].c_str();
    ss->cand_items[i].label = ss->cand_label[i].c_str();
  }

  if (ss->highlighted >= static_cast<int32_t>(ss->cand_items.size())) {
    ss->highlighted = 0;
  }
}

}  // namespace

extern "C" {

int32_t rs_abi_version(void) { return RIME_SHELL_ABI_VERSION; }

bool rs_init(const rs_setup* setup) {
  if (!setup) {
    g_last_error = "rs_init: setup 為 NULL";
    return false;
  }
  g_user_data_dir = setup->user_data_dir ? setup->user_data_dir : "";
  g_shared_data_dir = setup->shared_data_dir ? setup->shared_data_dir : "";
  g_deploy_cb = setup->on_deploy;
  g_deploy_userdata = setup->userdata;
  g_initialized = true;
  g_last_error = "";
  if (g_deploy_cb) {
    g_deploy_cb(RS_DEPLOY_SUCCESS, g_deploy_userdata);
  }
  return true;
}

void rs_finalize(void) {
  for (int i = 0; i < kMaxSessions; ++i) g_sessions[i] = StubSession{};
  g_initialized = false;
}

bool rs_deploy(void) {
  if (!g_initialized) {
    g_last_error = "rs_deploy: 尚未 rs_init";
    return false;
  }
  if (g_deploy_cb) {
    g_deploy_cb(RS_DEPLOY_RUNNING, g_deploy_userdata);
    g_deploy_cb(RS_DEPLOY_SUCCESS, g_deploy_userdata);
  }
  return true;
}

const char* rs_last_error(void) { return g_last_error.c_str(); }

rs_session rs_session_create(void) {
  if (!g_initialized) {
    g_last_error = "rs_session_create: 尚未 rs_init";
    return RS_INVALID_SESSION;
  }
  for (int i = 0; i < kMaxSessions; ++i) {
    if (!g_sessions[i].alive) {
      g_sessions[i] = StubSession{};
      g_sessions[i].alive = true;
      return static_cast<rs_session>(i + 1);
    }
  }
  g_last_error = "rs_session_create: stub session 數量已達上限";
  return RS_INVALID_SESSION;
}

void rs_session_destroy(rs_session s) {
  StubSession* ss = resolve(s);
  if (ss) *ss = StubSession{};
}

bool rs_session_alive(rs_session s) { return resolve(s) != nullptr; }

bool rs_process_key(rs_session s, int32_t keysym, uint32_t modifiers) {
  StubSession* ss = resolve(s);
  if (!ss) return false;
  if (modifiers & RS_MOD_RELEASE) return false;
  if (g_ascii_mode) return false;

  switch (keysym) {
    case 0xFF08: {  // BackSpace
      if (ss->input.empty()) return false;
      ss->input.pop_back();
      ss->page_no = 0;
      rebuild_candidates(ss);
      return true;
    }
    case 0xFF1B: {  // Escape
      if (ss->input.empty()) return false;
      ss->input.clear();
      rebuild_candidates(ss);
      return true;
    }
    case 0xFF0D:    // Return
    case 0x0020: {  // space
      if (ss->input.empty()) return false;
      if (keysym == 0x0020 && !ss->cand_items.empty()) {
        return rs_select_candidate(s, ss->highlighted);
      }
      ss->pending_commit = ss->input;
      ss->input.clear();
      rebuild_candidates(ss);
      return true;
    }
    default:
      break;
  }

  if (keysym >= 'a' && keysym <= 'z') {
    ss->input.push_back(static_cast<char>(keysym));
    ss->page_no = 0;
    rebuild_candidates(ss);
    return true;
  }
  if (keysym >= '0' && keysym <= '9' && !ss->input.empty()) {
    return rs_select_candidate(s, keysym - '1');
  }
  return false;
}

bool rs_select_candidate(rs_session s, int32_t index_on_page) {
  StubSession* ss = resolve(s);
  if (!ss) return false;
  if (index_on_page < 0 ||
      index_on_page >= static_cast<int32_t>(ss->cand_text.size())) {
    return false;
  }
  ss->pending_commit = ss->cand_text[index_on_page];
  ss->input.clear();
  ss->page_no = 0;
  ss->highlighted = 0;
  rebuild_candidates(ss);
  return true;
}

bool rs_delete_candidate(rs_session s, int32_t index_on_page) {
  StubSession* ss = resolve(s);
  if (!ss) return false;
  (void)index_on_page;
  g_last_error = "rs_delete_candidate: stub 不支援刪除使用者詞";
  return false;
}

bool rs_change_page(rs_session s, bool backward) {
  StubSession* ss = resolve(s);
  if (!ss || ss->input.empty()) return false;
  if (backward) {
    if (ss->page_no == 0) return false;
    --ss->page_no;
  } else {
    if (ss->page_no >= 2) return false;
    ++ss->page_no;
  }
  rebuild_candidates(ss);
  return true;
}

bool rs_clear_composition(rs_session s) {
  StubSession* ss = resolve(s);
  if (!ss) return false;
  ss->input.clear();
  ss->page_no = 0;
  ss->highlighted = 0;
  rebuild_candidates(ss);
  return true;
}

const rs_snapshot* rs_snapshot_acquire(rs_session s) {
  StubSession* ss = resolve(s);
  if (!ss) return nullptr;

  ss->preedit = ss->input;
  ss->commit_buf = ss->pending_commit;
  ss->pending_commit.clear();  // 依約定，快照被取走後 commit 即被清除

  rs_snapshot& snap = ss->snapshot;
  snap.composition.preedit = ss->preedit.c_str();
  snap.composition.sel_start = 0;
  snap.composition.sel_end = static_cast<int32_t>(ss->preedit.size());
  snap.composition.caret = static_cast<int32_t>(ss->preedit.size());

  snap.menu.items = ss->cand_items.empty() ? nullptr : ss->cand_items.data();
  snap.menu.count = static_cast<int32_t>(ss->cand_items.size());
  snap.menu.page_no = ss->page_no;
  snap.menu.highlighted = ss->cand_items.empty() ? -1 : ss->highlighted;
  snap.menu.is_last_page = (ss->page_no >= 2);

  snap.status.schema_id = kStubSchemaId;
  snap.status.schema_name = kStubSchemaName;
  snap.status.is_composing = !ss->preedit.empty();
  snap.status.is_ascii_mode = g_ascii_mode;
  snap.status.is_full_shape = g_full_shape;
  snap.status.is_simplified = g_simplified;
  snap.status.is_ascii_punct = g_ascii_punct;
  snap.status.is_disabled = false;

  snap.commit_text = ss->commit_buf.empty() ? nullptr : ss->commit_buf.c_str();

  ss->snapshot_held = true;
  return &snap;
}

void rs_snapshot_release(rs_session s) {
  StubSession* ss = resolve(s);
  if (ss) ss->snapshot_held = false;
}

int32_t rs_schema_list(const char** out_ids, const char** out_names,
                       int32_t capacity) {
  static const char* kIds[] = {"stub_fake_schema", "stub_fake_schema_2"};
  static const char* kNames[] = {"⟦STUB⟧ 假 Schema（未接 librime）",
                                 "⟦STUB⟧ 第二個假 Schema"};
  const int32_t total = 2;
  if (!out_ids && !out_names) return total;
  int32_t n = capacity < total ? capacity : total;
  for (int32_t i = 0; i < n; ++i) {
    if (out_ids) out_ids[i] = kIds[i];
    if (out_names) out_names[i] = kNames[i];
  }
  return total;
}

bool rs_select_schema(rs_session s, const char* schema_id) {
  if (!resolve(s) || !schema_id) return false;
  return true;
}

bool rs_set_option(rs_session s, const char* option, bool value) {
  if (!resolve(s) || !option) return false;
  if (std::strcmp(option, "ascii_mode") == 0) { g_ascii_mode = value; return true; }
  if (std::strcmp(option, "full_shape") == 0) { g_full_shape = value; return true; }
  if (std::strcmp(option, "simplification") == 0) { g_simplified = value; return true; }
  if (std::strcmp(option, "ascii_punct") == 0) { g_ascii_punct = value; return true; }
  return false;
}

bool rs_get_option(rs_session s, const char* option) {
  if (!resolve(s) || !option) return false;
  if (std::strcmp(option, "ascii_mode") == 0) return g_ascii_mode;
  if (std::strcmp(option, "full_shape") == 0) return g_full_shape;
  if (std::strcmp(option, "simplification") == 0) return g_simplified;
  if (std::strcmp(option, "ascii_punct") == 0) return g_ascii_punct;
  return false;
}

}  // extern "C"
