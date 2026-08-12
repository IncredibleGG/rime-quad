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
//
//   <按鍵字母> 傳一個單獨的 "-" 進入 deploy-only 模式：只做部署並列出可用
//   方案，不建立組字流程、不送按鍵。方案市集的打包驗證
//   （scripts/build_schema_store.sh）用它確認每個套件真的被 librime 部署過。
//   該模式會額外印出機器可讀的 "[schema] <id>\t<name>" 與
//   "[deploy-only] OK schemas=<n>"，其餘既有行為完全不變。

#include "rime_shell.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

#include <limits.h>
#include <unistd.h>

namespace {

std::atomic<int> g_deploy_state{-1};  // -1 = 尚未有結果

// ═══════════════════════════════════════════════════════════════════════
//  [diag] 為了回答「macOS 上候選為什麼是 0」而加的一批診斷
//
//  ⚠ 這一批**只印東西**,不改任何行為,定案之後整批拔掉。
//
//  特別注意:底下沒有任何一處呼叫 rs_snapshot_acquire()。
//  rime_shell.h 的契約是「每個輸入事件只可 acquire 一次」,而診斷若自己
//  多 acquire 一次,就會把「被診斷的缺陷」和「診斷造成的缺陷」混在一起。
//  rs_get_option() 讀的是 context,不碰快照,所以印它是安全的。
// ═══════════════════════════════════════════════════════════════════════

// 簡繁互斥組(zh_*)+ 別的方案用的 simplification + 字集守門自己的開關。
// 這幾支正是 core/data/lua/luminakey_charset.lua 的 M.pick() 會問的東西。
const char* const kDiagOptions[] = {
    "zh_hans", "zh_hant", "zh_hant_hk", "zh_hant_tw",
    "simplification", "luminakey_charset_off", "ascii_mode", "ascii_punct",
};

void diag_options(rs_session sess, const char* tag) {
  std::printf("[diag] 選項@%s:", tag);
  for (const char* o : kDiagOptions)
    std::printf(" %s=%d", o, rs_get_option(sess, o) ? 1 : 0);
  std::printf("\n");
  std::fflush(stdout);
}

// ⚠ **這一行是這批診斷裡最重要的一行。**
//
// patches/librime-lua@sandbox.patch 的第二層(modules.cc 的
// kRimeQuadPathSandbox)一開頭就做:
//
//     local ROOT_USER = normalize(user_dir, nil)
//     if not ROOT_USER then error("rimequad: 使用者資料目錄不是絕對路徑: "..) end
//
// 而 normalize(p, nil) 對**相對路徑**回 nil(它只認 "/" 開頭或 "C:/")。
// 也就是說:user_data_dir 是相對路徑 -> 沙盒 error -> lua_pcall 失敗 ->
// rimequad_lua_nuke() 把 require / io / os / package / load 全部設成 nil。
//
// 而 librime-lua 載入 `lua_filter@*luminakey_charset` 的方式是
// src/lua_gears.cc:85 的 `lua_getglobal(L, "require")`。require 是 nil 的話
// 這個 filter **載不起來**,而 librime 對 filter 載入失敗的處置是
// **整段候選變成空的**(core/data/lua/luminakey_charset.lua 檔頭記過這件事)。
//
// 所以「傳進來的資料目錄是不是絕對路徑」可能直接決定候選數是 5 還是 0。
void diag_path(const char* label, const char* p) {
  char buf[PATH_MAX];
  const char* real = (p && *p) ? realpath(p, buf) : nullptr;
  const bool absolute = (p && p[0] == '/');
  std::printf("[diag] %s = \"%s\"  → %s%s\n", label, p ? p : "(null)",
              absolute ? "絕對路徑" : "相對路徑  ← lua 沙盒的 normalize() 會回 nil",
              real ? "" : "  (realpath 解不出來)");
  if (real) std::printf("[diag]   realpath = %s\n", real);
  std::fflush(stdout);
}

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
  // [diag] 在同一個 tag 底下把方案選項一起印出來,免得要靠時間戳對兩段。
  // 放在 release 之後:rs_get_option 不碰快照,順序不重要,但這樣不會有人
  // 以為它在讀 s。
  diag_options(sess, tag);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(
        stderr,
        "用法: %s <shared_data_dir> <user_data_dir> <按鍵字母|-> [選字序號] [方案id]\n",
        argv[0]);
    return 2;
  }
  const char* shared_dir = argv[1];
  const char* user_dir = argv[2];
  const char* keys = argv[3];
  const int select_index = (argc > 4) ? std::atoi(argv[4]) : 1;
  const char* want_schema = (argc > 5) ? argv[5] : nullptr;
  // "-" = 只部署、只列方案，不送按鍵。既有呼叫方一律傳真正的按鍵字串，
  // 因此這個分支對它們永遠不成立，行為不變。
  const bool deploy_only = (std::strcmp(keys, "-") == 0);

  std::printf("=== rime_shell 端到端測試 ===\n");
  std::printf("ABI version : %d\n", rs_abi_version());
  std::printf("shared      : %s\n", shared_dir);
  std::printf("user        : %s\n", user_dir);
  std::printf("按鍵        : %s\n", keys);
  std::printf("方案 id     : %s\n\n", want_schema ? want_schema : "(沒有傳)");

  // [diag] 路徑診斷。理由見 diag_path() 上面那一段。
  {
    char cwd[PATH_MAX];
    std::printf("[diag] cwd = %s\n", getcwd(cwd, sizeof cwd) ? cwd : "(取不到)");
    diag_path("shared_data_dir", shared_dir);
    diag_path("user_data_dir  ", user_dir);
  }
  std::printf("\n");

  // keysym 查表是純查表，不需要 rs_init()，所以在這裡先驗。
  // 這兩個函式是對 librime 私有標頭的符號做就地重宣告，靠 C++ mangling 對上 ——
  // 若 mangling 對不上會是連結錯誤，若查表邏輯錯了就會在這裡看出來。
  {
    static const char* kNames[] = {"BackSpace", "Return", "space",
                                   "a",         "F5",     "no_such_key_name"};
    std::printf("keysym 查表驗證（不需初始化）:\n");
    for (const char* n : kNames) {
      const int32_t k = rs_keysym_by_name(n);
      const char* back = k ? rs_keysym_name(k) : nullptr;
      std::printf("    %-18s -> 0x%06X  反查=%s\n", n, k,
                  back ? back : "(NULL)");
    }
    std::printf("\n");
  }

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
  // [diag] 這是「還沒切方案」的狀態。想知道此刻**目前的方案**是哪一個,
  // 要 acquire 快照 —— 而多一次 acquire 會動到契約,所以預設不做,
  // 由 RIME_DIAG_PRESWITCH=1 另外跑一趟問。不傳方案 id 時本來就不會切,
  // 底下的 dump("初始") 印的就是這個問題的答案。
  diag_options(sess, "session 剛建立(還沒切方案)");
  if (std::getenv("RIME_DIAG_PRESWITCH")) {
    std::printf("[diag] RIME_DIAG_PRESWITCH=1:額外 acquire 一次問「現在是哪個方案」\n");
    dump("切方案前", sess);
  }

  // 列出可用方案，順便驗證 rs_schema_list 的字串生命週期沒問題
  // 容量從 16 放大到 64：市集裡有單一套件提供十餘個方案的情況
  // （例如上古音方案集）。既有兩個測試案例只有 3 個方案，不受影響。
  const char* ids[64];
  const char* names[64];
  int32_t n = rs_schema_list(ids, names, 64);
  std::printf("可用方案 %d 個:\n", n);
  for (int32_t i = 0; i < n && i < 64; ++i) {
    std::printf("    %s  (%s)\n", ids[i], names[i]);
    std::printf("[schema] %s\t%s\n", ids[i], names[i]);  // 機器可讀
  }

  if (deploy_only) {
    // 部署成功 + 方案列得出來，就是品質閘門要的證據。
    std::printf("[deploy-only] OK schemas=%d\n", n);
    rs_session_destroy(sess);
    rs_finalize();
    std::printf("\n=== 結束 ===\n");
    return 0;
  }

  if (want_schema) {
    std::printf("\n切換方案 -> %s\n", want_schema);
    if (!rs_select_schema(sess, want_schema)) {
      std::fprintf(stderr, "切換方案失敗: %s\n", rs_last_error());
      return 1;
    }
    diag_options(sess, "剛切完方案");
  }

  // ── 由環境變數指定要開/關哪些方案選項 ────────────────────────────────
  //
  // ⚠ 為什麼需要它:簡繁、全半形這一類東西是**方案的開關**,而「設定頁寫著
  //   簡體、候選卻全是繁體」這種回報,隔著 app 的 UI 是分不出
  //   「開關沒送到引擎」還是「送到了但方案不吃」的。這裡直接對 librime 送,
  //   一次就能定案。
  //
  //   用法:RIME_SET_OPTIONS="zh_hans=1,ascii_punct=0" rime_console …
  //   每一個都會印出設定前後的實際值 —— 送進去不等於生效,方案沒有那個
  //   開關的話 rs_get_option 事後仍然是 false,而那正是要看的東西。
  if (const char* opts = std::getenv("RIME_SET_OPTIONS")) {
    std::printf("\n--- 套用方案選項 ---\n");
    std::string spec(opts);
    size_t pos = 0;
    while (pos <= spec.size()) {
      size_t comma = spec.find(',', pos);
      std::string one = spec.substr(pos, comma == std::string::npos
                                             ? std::string::npos
                                             : comma - pos);
      pos = (comma == std::string::npos) ? spec.size() + 1 : comma + 1;
      size_t eq = one.find('=');
      if (eq == std::string::npos || eq == 0) continue;
      std::string name = one.substr(0, eq);
      bool want = one.substr(eq + 1) != "0";
      bool before = rs_get_option(sess, name.c_str());
      bool ok = rs_set_option(sess, name.c_str(), want);
      bool after = rs_get_option(sess, name.c_str());
      std::printf("  %s: %d -> 想要 %d,rs_set_option=%s,實際 %d%s\n",
                  name.c_str(), before ? 1 : 0, want ? 1 : 0,
                  ok ? "true" : "false", after ? 1 : 0,
                  (after == want) ? "" : "   ← 沒有生效");
    }
  }

  // 有些方案預設就停在英文（ASCII）模式 —— 鍵道·我流 的 ascii_mode 初值是 1。
  // 這種方案在真實使用上，使用者第一件事就是按中英切換；但自動化測試沒有那顆鍵，
  // 於是每個按鍵都「未被消費」，看起來像方案壞掉。這裡等同於幫它按一下。
  // 既有的拼音／注音案例初始就是 ascii=0，這段對它們不會執行，行為不變。
  if (rs_get_option(sess, "ascii_mode")) {
    std::printf("\n方案預設在英文模式，關掉 ascii_mode（等同使用者按中英切換）\n");
    if (!rs_set_option(sess, "ascii_mode", false))
      std::printf("  rs_set_option 失敗: %s\n", rs_last_error());
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

  // ── 政策迴圈 ──────────────────────────────────────────────
  //
  // 這裡在驗證一條給四端前端共用的政策：
  //
  //     menu.count > 0            → 還有段落待選，選第一個候選
  //     count == 0 && is_composing → 沒有東西可選了但仍在組字，明確 commit
  //     count == 0 && !is_composing → 結束
  //
  // 關鍵問題是「選字後 is_composing 仍為 true」到底代表整句轉換完成待確認，
  // 還是只選了第一段、後面還有。若是後者卻貿然 commit，會把後半段吃掉。
  // 上面那條政策用 menu.count 來區分 —— 本迴圈就是要用多音節輸入把它壓測出來。
  //
  // 每一輪只 acquire 一次：acquire 會消費掉 commit，所以「動作 → 下一輪 acquire
  // 讀結果」必須嚴格一對一，這也正是標頭要求的「每個輸入事件只可 acquire 一次」。
  std::printf("\n--- 政策迴圈 ---\n");
  std::string committed;
  for (int step = 1; step <= 12; ++step) {
    const rs_snapshot* s = rs_snapshot_acquire(sess);
    if (!s) { std::printf("  取不到快照: %s\n", rs_last_error()); break; }

    const bool composing = s->status.is_composing;
    const int count = s->menu.count;
    std::printf("  [%d] composing=%d  候選=%d  preedit=\"%s\"", step,
                (int)composing, count, s->composition.preedit);
    if (count > 0) {
      std::printf("  第一候選=\"%s\"", s->menu.items[0].text);
    }
    if (s->commit_text) {
      std::printf("  >>> COMMIT:\"%s\"", s->commit_text);
      committed += s->commit_text;
    }
    std::printf("\n");
    rs_snapshot_release(sess);

    if (count > 0) {
      std::printf("      -> 還有候選，選第 %d 個\n", select_index);
      if (!rs_select_candidate(sess, select_index - 1)) {
        std::printf("      rs_select_candidate 失敗: %s\n", rs_last_error());
        break;
      }
    } else if (composing) {
      std::printf("      -> 無候選但仍在組字，明確 commit\n");
      if (!rs_commit_composition(sess)) {
        std::printf("      rs_commit_composition 失敗: %s\n", rs_last_error());
        break;
      }
    } else {
      std::printf("      -> 結束\n");
      break;
    }
  }

  std::printf("\n>>> COMMIT: \"%s\"\n", committed.c_str());

  rs_session_destroy(sess);
  rs_finalize();
  std::printf("\n=== 結束 ===\n");
  return 0;
}
