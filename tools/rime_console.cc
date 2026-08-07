// rime_console.cc — 不經 Android UI 的端到端測試工具
//
// 目的：把「librime + schema 資料 + rime_shell 門面邏輯」這一層，和
// 「Android app / JNI / Compose UI」那一層**分開驗證**。
//
// 這支程式編成 x86_64 Android 執行檔，直接丟進模擬器用 adb shell 跑。
// 它證明的是：資料能部署、session 能建立、按鍵送得進去、候選字出得來、
// 選字之後 commit 文字正確。若這裡是綠的而 APK 打不出字，問題必定在
// JNI 或 UI；若這裡就是紅的，再怎麼改 UI 也沒用。
//
// 用法：
//   rime_console <shared_data_dir> <user_data_dir> <按鍵字母> [選字序號] [方案id]
//   例：rime_console .../shared .../user nihao 1
//       rime_console .../shared .../user su3cl3 1 bopomofo_tw

#include "rime_shell.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

namespace {

std::atomic<int> g_deploy_state{-1};  // -1 = 尚未有結果

void on_deploy(rs_deploy_status status, void* /*ud*/) {
  const char* name = "?";
  switch (status) {
    case RS_DEPLOY_IDLE:    name = "IDLE"; break;
    case RS_DEPLOY_RUNNING: name = "RUNNING"; break;
    case RS_DEPLOY_SUCCESS: name = "SUCCESS"; break;
    case RS_DEPLOY_FAILURE: name = "FAILURE"; break;
  }
  std::printf("[deploy] %s\n", name);
  std::fflush(stdout);
  if (status == RS_DEPLOY_SUCCESS || status == RS_DEPLOY_FAILURE)
    g_deploy_state.store(status == RS_DEPLOY_SUCCESS ? 1 : 0);
}

void dump(const char* tag, rs_session sess) {
  const rs_snapshot* s = rs_snapshot_acquire(sess);
  if (!s) {
    std::printf("  [%s] 取不到快照: %s\n", tag, rs_last_error());
    return;
  }
  std::printf("  [%s] preedit=\"%s\" caret=%d  schema=%s(%s) composing=%d ascii=%d\n",
              tag, s->composition.preedit, s->composition.caret,
              s->status.schema_id, s->status.schema_name,
              (int)s->status.is_composing, (int)s->status.is_ascii_mode);
  std::printf("  [%s] 候選 %d 個 (page %d%s, 高亮 %d):\n", tag, s->menu.count,
              s->menu.page_no, s->menu.is_last_page ? ", 末頁" : "",
              s->menu.highlighted);
  for (int i = 0; i < s->menu.count && i < 9; ++i) {
    const rs_candidate& c = s->menu.items[i];
    std::printf("        %s. %s%s%s\n", c.label, c.text,
                (c.comment && *c.comment) ? "  # " : "",
                (c.comment && *c.comment) ? c.comment : "");
  }
  if (s->commit_text)
    std::printf("  [%s] >>> COMMIT: \"%s\"\n", tag, s->commit_text);
  rs_snapshot_release(sess);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(
        stderr,
        "用法: %s <shared_data_dir> <user_data_dir> <按鍵字母> [選字序號] [方案id]\n",
        argv[0]);
    return 2;
  }
  const char* shared_dir = argv[1];
  const char* user_dir = argv[2];
  const char* keys = argv[3];
  const int select_index = (argc > 4) ? std::atoi(argv[4]) : 1;
  const char* want_schema = (argc > 5) ? argv[5] : nullptr;

  std::printf("=== rime_shell 端到端測試 ===\n");
  std::printf("ABI version : %d\n", rs_abi_version());
  std::printf("shared      : %s\n", shared_dir);
  std::printf("user        : %s\n", user_dir);
  std::printf("按鍵        : %s\n\n", keys);

  rs_setup setup{};
  setup.shared_data_dir = shared_dir;
  setup.user_data_dir = user_dir;
  setup.log_dir = "";  // "" = 只寫 stderr，模擬器上比較好看
  setup.app_name = "rime.console";
  setup.on_deploy = on_deploy;

  if (!rs_init(&setup)) {
    std::fprintf(stderr, "rs_init 失敗: %s\n", rs_last_error());
    return 1;
  }
  std::printf("rs_init OK，等待首次部署（編譯 schema，可能要數分鐘）...\n");
  std::fflush(stdout);

  // 首次部署要編譯 luna_pinyin / terra_pinyin / stroke 三本詞庫加 essay 語言模型，
  // 在模擬器上很慢，給足時間。
  for (int i = 0; i < 600 && g_deploy_state.load() < 0; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (i % 30 == 29) { std::printf("  ...已等 %ds\n", i + 1); std::fflush(stdout); }
  }
  const int deploy = g_deploy_state.load();
  if (deploy == 0) {
    std::fprintf(stderr, "部署失敗。檢查 shared_data_dir 的內容與 stderr 上的 glog 訊息。\n");
    return 1;
  }
  if (deploy < 0)
    std::printf("警告：等待逾時仍未收到部署結果，繼續嘗試建立 session。\n");

  rs_session sess = rs_session_create();
  if (sess == RS_INVALID_SESSION) {
    std::fprintf(stderr, "建立 session 失敗: %s\n", rs_last_error());
    return 1;
  }
  std::printf("\nsession 已建立\n");

  // 列出可用方案，順便驗證 rs_schema_list 的字串生命週期沒問題
  const char* ids[16];
  const char* names[16];
  int32_t n = rs_schema_list(ids, names, 16);
  std::printf("可用方案 %d 個:\n", n);
  for (int32_t i = 0; i < n && i < 16; ++i)
    std::printf("    %s  (%s)\n", ids[i], names[i]);

  if (want_schema) {
    std::printf("\n切換方案 -> %s\n", want_schema);
    if (!rs_select_schema(sess, want_schema)) {
      std::fprintf(stderr, "切換方案失敗: %s\n", rs_last_error());
      return 1;
    }
  }

  dump("初始", sess);

  // 逐字送出按鍵。X11 keysym 對 ASCII 可列印字元就是其 ASCII 值。
  std::printf("\n--- 送出按鍵 ---\n");
  for (const char* p = keys; *p; ++p) {
    bool consumed = rs_process_key(sess, (int32_t)(unsigned char)*p, 0);
    std::printf("  '%c' -> %s\n", *p, consumed ? "已被輸入法消費" : "未消費(!)");
    if (!consumed) {
      std::printf("  !! 按鍵未被消費，代表 librime 沒有在處理輸入。\n");
    }
  }

  dump("組字後", sess);

  // 選字：用 rs_select_candidate（UI 點擊路徑），索引由 0 起算
  std::printf("\n--- 選第 %d 個候選 ---\n", select_index);
  if (!rs_select_candidate(sess, select_index - 1))
    std::printf("  rs_select_candidate 回傳 false: %s\n", rs_last_error());

  dump("選字後", sess);

  // 方案差異：拼音在選字當下就上屏；注音選字後仍停留在組字狀態，
  // 需要明確的確認動作。前端必須處理這個差異，不能假設選字＝上屏。
  {
    const rs_snapshot* s = rs_snapshot_acquire(sess);
    const bool still_composing = s && s->status.is_composing;
    rs_snapshot_release(sess);
    if (still_composing) {
      std::printf("\n--- 選字後仍在組字，明確送出 commit ---\n");
      if (!rs_commit_composition(sess))
        std::printf("  rs_commit_composition 回傳 false: %s\n", rs_last_error());
      dump("commit 後", sess);
    }
  }

  rs_session_destroy(sess);
  rs_finalize();
  std::printf("\n=== 結束 ===\n");
  return 0;
}
