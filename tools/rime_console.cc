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
//
//   rime_console --has-component <shared_data_dir> <user_data_dir> <元件名[,元件名…]>
//
//   問「**這一支二進位檔**裡的 librime 有沒有註冊這些元件」，逐行印
//   `<名字><TAB>yes|no`，有任何一個 no 就以 1 結束（用法錯誤是 2）。
//   ⚠ 只在 argv[1] **逐字**等於 `--has-component` 時成立。既有呼叫方一律
//     把 shared_data_dir 放在 argv[1]，所以 argv[1..5] 的位置語意一字不動。
//
//   它是 scripts/verify_schema_components.sh 的後端：打包進某一端的方案
//   一旦用到該端的引擎註冊不到的元件（症狀 C / 工單 #109 就是這件事：
//   Windows 打包了 `lua_filter@*luminakey_charset`，而 Windows 的 librime
//   沒有編 librime-lua），librime 的處置是只記一行錯就跳過那個元件
//   （engine.cc:314 的 `continue`）—— 每一關都是綠的，而那一層過濾在
//   使用者機器上根本不存在。**手寫一份「這個平台有哪些元件」的清單答不了
//   這件事**（它會在下一次改建置選項時安靜地過期），只有二進位檔自己答得了。

#include "rime_shell.h"

// --has-component 要在**不部署**的情況下把模組載起來（元件是在模組載入時
// 註冊進 Registry 的，與部署無關），所以直接用 librime 的公開 C API，
// 不走 rs_init() —— 後者一定會接著 start_maintenance()，那是編詞庫，
// 在 CI 上是好幾分鐘，而這道閘每一次建置都要跑。
#include <rime_api.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

// ── librime 私有標頭 rime/registry.h 的宣告，在此就地重宣告而不 #include 它
//
// 與 core/src/rime_shell.cc 對 rime/key_table.h 的處置同一個理由，而這裡更硬：
// `rime/registry.h` 會 `#include <rime/common.h>`，那一份把 boost/signals2 與
// boost/unordered 一路拉進來。本目標的 include 路徑只有 core/include 與
// librime 的 prefix/include，而 **boost 不在 prefix 裡**（三支建置腳本都下了
// -DINSTALL_PRIVATE_HEADERS=ON，但「標頭裝了」不等於「它的相依也在」）。
//
// Registry::Find 與 Registry::instance 是 RIME_DLL 匯出的 **C++** 符號
// （librime src/rime/registry.h:20 與 :26）。以相同簽章重宣告即可對上同一個
// mangled 名稱；簽章若在上游變動，結果是**連結錯誤**而不是靜默失效 ——
// 這正是這個做法可以接受的原因。
// ⚠ 這裡只呼叫那兩支成員函式，不建構 Registry、也不碰任何成員，所以少宣告
//   的那個 map_ 成員不影響任何一個 mangled 名稱。
namespace rime {
class ComponentBase;
class Registry {
 public:
  ComponentBase* Find(const std::string& name);
  static Registry& instance();
};
}  // namespace rime

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


// ── 量測用的走頁器 ────────────────────────────────────────────────────────
//
// 為什麼不用底下那個「政策迴圈」來量:那個迴圈會**選字**,而選字會寫進
// userdb —— 量完第一組輸入,第二組量到的就是被前一組訓練過的引擎,而且
// 量測跑過一次之後就再也回不到原狀。量測這條路徑一個候選都不選,只讀快照。
//
// 為什麼要走頁:`menu.count` 永遠 <= `menu/page_size`,光看它分不出
// 「引擎只給得出這麼多」與「一頁只裝得下這麼多」—— 而那正是要量的差別。
struct PageWalk {
  int first_page = 0;    // 第一頁的候選數（= 前端拿得到的那一批）
  int total = 0;         // 走到末頁為止的總數
  int pages = 0;
  bool truncated = false;  // 撞到 kMaxPages 才停,不是真的走到末頁
  std::string first_texts;
};

PageWalk walk_pages(rs_session sess) {
  const int kMaxPages = 60;
  PageWalk w;
  for (int p = 0; p < kMaxPages; ++p) {
    const rs_snapshot* s = rs_snapshot_acquire(sess);
    if (!s)
      break;
    const int n = s->menu.count;
    const bool last = s->menu.is_last_page;
    if (p == 0) {
      w.first_page = n;
      for (int i = 0; i < n; ++i) {
        if (i)
          w.first_texts += "|";
        w.first_texts += s->menu.items[i].text;
      }
    }
    w.total += n;
    w.pages = p + 1;
    rs_snapshot_release(sess);
    if (n == 0 || last)
      return w;
    if (!rs_change_page(sess, false))
      return w;
  }
  w.truncated = true;
  return w;
}

void send_keys(rs_session sess, const std::string& keys) {
  for (char c : keys)
    rs_process_key(sess, (int32_t)(unsigned char)c, 0);
}

// 逗號分隔的清單拆成一段一段。
std::vector<std::string> split_commas(const char* spec) {
  std::vector<std::string> out;
  std::string s(spec ? spec : "");
  size_t pos = 0;
  while (pos <= s.size()) {
    size_t comma = s.find(',', pos);
    std::string one =
        s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    pos = (comma == std::string::npos) ? s.size() + 1 : comma + 1;
    if (!one.empty())
      out.push_back(one);
  }
  return out;
}

// ── --has-component ────────────────────────────────────────────────────
//
// 回傳值刻意分三級，呼叫端（scripts/verify_schema_components.sh）靠它分辨
// 「答案是沒有」與「根本問不出答案」：
//   0 = 全部都在
//   1 = 至少一個不在（stdout 上那幾行是答案）
//   2 = 問不出來（參數錯、librime 沒連上）—— 這一種**不可以**被當成 no，
//       否則一支壞掉的二進位檔會讓閘去查豁免清單，而不是當場死掉。
int RunHasComponent(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr,
                 "用法: %s --has-component <shared_data_dir> <user_data_dir> "
                 "<元件名[,元件名…]>\n",
                 argv[0]);
    return 2;
  }
  const std::vector<std::string> names = split_commas(argv[4]);
  if (names.empty()) {
    std::fprintf(stderr, "--has-component: 元件名清單是空的\n");
    return 2;
  }

  RimeApi* api = rime_get_api();
  if (!api) {
    std::fprintf(stderr, "rime_get_api() 回傳 NULL：librime 未正確連結\n");
    return 2;
  }

  RIME_STRUCT(RimeTraits, traits);
  traits.shared_data_dir = argv[2];
  traits.user_data_dir = argv[3];
  traits.log_dir = "";   // "" = 只寫 stderr（NULL 才是暫存目錄，語意不同）
  traits.app_name = "rime.console.has-component";
  traits.distribution_name = "Rime";
  traits.distribution_code_name = "rime-shell";
  traits.distribution_version = "0.1.0";
  traits.min_log_level = 3;  // 只留 FATAL：這支的 stdout 是要被腳本讀的

  api->setup(&traits);
  api->initialize(&traits);
  // ⚠ 刻意**不**呼叫 start_maintenance()。元件是在模組載入時註冊進
  //   Registry 的（librime src/rime_api_impl.h 的 RimeInitialize → LoadModules，
  //   而 gears_module.cc 的 r.Register("simplifier", …) 那一段就是註冊本身）。
  //   部署與這件事無關，而部署要編詞庫。

  int missing = 0;
  for (const std::string& n : names) {
    const bool has = rime::Registry::instance().Find(n) != nullptr;
    std::printf("%s\t%s\n", n.c_str(), has ? "yes" : "no");
    if (!has)
      missing = 1;
  }
  std::fflush(stdout);
  api->finalize();
  return missing;
}

}  // namespace

int main(int argc, char** argv) {
  // ⚠ 這一格必須在下面那個 `argc < 4` 的用法檢查**之前**，而且只在 argv[1]
  //   逐字等於 --has-component 時成立。既有呼叫方（scripts/run_console_test.sh、
  //   apple/scripts/verify_console.sh、scripts/verify_syllables.sh、
  //   windows/setup/doctor.cc 第 7 節…）一律把 shared_data_dir 放在 argv[1]，
  //   所以它們一個都走不到這裡。
  if (argc >= 2 && std::strcmp(argv[1], "--has-component") == 0)
    return RunHasComponent(argc, argv);

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
  std::printf("按鍵        : %s\n\n", keys);

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


  // ── 量測模式(RIME_MEASURE="nihao,ni,hao,…")────────────────────────────
  //
  // ⚠ 只讀不選。走完就 return —— 底下的政策迴圈會選字、會寫 userdb,
  //   而量測必須可以重複跑出同一組數字。
  //
  // 印出來的是機器可讀的一行:
  //   [cand] keys=<輸入> page1=<第一頁幾個> total=<全部幾個> pages=<幾頁> texts=…
  // page1 就是前端(候選列)實際拿得到的那一批;total 是引擎**還有**多少 ——
  // 兩者的差就是「調大 page_size 之後畫得出來的空間」。
  if (const char* spec = std::getenv("RIME_MEASURE")) {
    std::printf("\n--- 量測(只讀,不選字)---\n");
    for (const std::string& one : split_commas(spec)) {
      rs_clear_composition(sess);
      send_keys(sess, one);
      PageWalk w = walk_pages(sess);
      std::printf("[cand] keys=%s page1=%d total=%d pages=%d%s texts=%s\n",
                  one.c_str(), w.first_page, w.total, w.pages,
                  w.truncated ? " 截斷" : "", w.first_texts.c_str());
      std::fflush(stdout);
      rs_clear_composition(sess);
    }
    rs_session_destroy(sess);
    rs_finalize();
    std::printf("\n=== 量測結束 ===\n");
    return 0;
  }

  // ── 頁內索引探針(RIME_PAGEINDEX="nihao")──────────────────────────────
  //
  // 要回答的問題只有一個:`rs_select_candidate(i)` 的 i 是**頁內**索引,
  // 還是攤平之後的整體索引?兩者在第一頁上完全一樣,所以只能到第二頁去問。
  //
  // 做法:翻到第 2 頁,記下第 2 頁的第 0 個是什麼,然後 select(0),
  // 看上屏的是第 2 頁的第 0 個,還是第 1 頁的第 0 個。
  // 這一格弄錯的症狀是「點了第 7 個卻上屏第 2 個」,而畫面完全正常。
  if (const char* pk = std::getenv("RIME_PAGEINDEX")) {
    std::printf("\n--- 頁內索引探針 ---\n");
    rs_clear_composition(sess);
    send_keys(sess, pk);

    std::string p0_first, p0_second;
    int p0_no = -1, p0_count = 0;
    if (const rs_snapshot* s = rs_snapshot_acquire(sess)) {
      p0_no = s->menu.page_no;
      p0_count = s->menu.count;
      if (s->menu.count > 0) p0_first = s->menu.items[0].text;
      if (s->menu.count > 1) p0_second = s->menu.items[1].text;
      rs_snapshot_release(sess);
    }
    std::printf("[pageidx] 第1頁 page_no=%d count=%d [0]=%s [1]=%s\n", p0_no,
                p0_count, p0_first.c_str(), p0_second.c_str());

    if (!rs_change_page(sess, false)) {
      std::printf("[pageidx] 只有一頁,這個輸入問不出來\n");
    } else {
      std::string p1_first, p1_second;
      int p1_no = -1, p1_count = 0;
      if (const rs_snapshot* s = rs_snapshot_acquire(sess)) {
        p1_no = s->menu.page_no;
        p1_count = s->menu.count;
        if (s->menu.count > 0) p1_first = s->menu.items[0].text;
        if (s->menu.count > 1) p1_second = s->menu.items[1].text;
        rs_snapshot_release(sess);
      }
      std::printf("[pageidx] 第2頁 page_no=%d count=%d [0]=%s [1]=%s\n", p1_no,
                  p1_count, p1_first.c_str(), p1_second.c_str());

      rs_select_candidate(sess, 0);
      std::string got;
      if (const rs_snapshot* s = rs_snapshot_acquire(sess)) {
        if (s->commit_text) got = s->commit_text;
        rs_snapshot_release(sess);
      }
      std::printf("[pageidx] 在第2頁 select(0) -> commit=\"%s\"\n", got.c_str());
      std::printf("[pageidx] 判定=%s\n",
                  got == p1_first ? "頁內索引（index_on_page）"
                                  : (got == p0_first ? "整體索引（攤平）"
                                                     : "兩者皆非（要人看）"));
    }
    rs_clear_composition(sess);
    rs_session_destroy(sess);
    rs_finalize();
    std::printf("\n=== 探針結束 ===\n");
    return 0;
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
