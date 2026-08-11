// scripts/variant_probe.cc — 簡繁到底有沒有真的送到引擎(而且留得住)
//
// 這一支只回答兩個問題,而兩個都是使用者實機回報的那條缺陷的根:
//
//   1. 套完簡繁之後,rs_status.variant 有沒有回報**引擎實際套用**的字形?
//   2. **再選一次同一個方案**之後,那組簡繁還在不在?
//
// 第 2 條是關鍵。librime 的 ConcreteEngine::InitializeOptions() 在每一次
// 載入方案時都會把 switches 重設回方案宣告的值(有 `reset:` 的那些)。
// 也就是說「換方案」與「套簡繁」必須是一個**不可分割**的動作 ——
// 而產品裡真的會走到:懸浮狀態列第三格的方案選單 → SelectSchemaAll →
// 之後沒有任何人重套簡繁。
//
// ⚠ 這支探針**跑不到 windows/service/engine.cc**(那是 Win32,只有
//   windows-latest 編得起來)。它證的是**引擎這一側的事實**:
//     · bare  模式 = 裸 rs_select_schema  → 簡繁真的會被洗掉
//     · reapply 模式 = 選完之後重套一次   → 簡繁留得住
//   engine.cc 走的是哪一種,由 windows/audit_single_source.sh 在原始碼
//   層面守(rs_select_schema 只能有一個裸呼叫點,在 SelectAndApply 裡)。
//   兩件事合起來才是完整的守門,單獨任何一件都不是。
//
// ⚠ 判準只能用第 3、4 個候選。你好 / 妳好 / 你 在簡繁兩套字集裡長得
//   一模一樣,拿它們斷言等於沒斷言。
//
// 用法: variant_probe <shared_data_dir> <user_data_dir> [solo]
//
//   solo = 只跑情境 0b。它要求 user 目錄的 schema_list **只有**
//   luna_pinyin(驅動腳本會寫一份這樣的 default.custom.yaml),
//   理由見情境 0b 自己的註解。

#include "rime_shell.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<int> g_deploy{-1};
void OnDeploy(rs_deploy_status s, void*) {
  if (s == RS_DEPLOY_SUCCESS || s == RS_DEPLOY_FAILURE)
    g_deploy.store(s == RS_DEPLOY_SUCCESS ? 1 : 0);
}

int g_fail = 0;

void Check(bool ok, const char* what) {
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

const char* VariantName(rs_variant v) {
  switch (v) {
    case RS_VARIANT_HANS: return "HANS";
    case RS_VARIANT_HANT: return "HANT";
    default: return "UNKNOWN";
  }
}

// windows/common/schema_choice.cc 的 PlanVariant(simplified=true,
// langid=0x0404) 產出的那一串,**順序一模一樣**。
//
// ⚠ 這裡是手抄的一份。它不能 include windows/common/(那一份的測試在
//   Ubuntu 上跑,這一支在 Android 模擬器上跑,兩邊的建置管道不同),
//   所以 tests/test_schema_choice.cc 的 PlanVariant_* 那幾支測試是它的
//   對照 —— 那邊改了這裡沒改的話,這支探針就不再代表產品。
void ApplyPlanSimplified(rs_session s) {
  rs_set_option(s, "simplification", true);
  rs_set_option(s, "zh_hant", false);
  rs_set_option(s, "zh_hant_hk", false);
  rs_set_option(s, "zh_hant_tw", false);
  rs_set_option(s, "zh_hans", true);
}

void DumpOptions(rs_session s, const char* tag) {
  static const char* kOpts[5] = {"simplification", "zh_hant", "zh_hans",
                                 "zh_hant_hk", "zh_hant_tw"};
  std::printf("       [%s]", tag);
  for (int i = 0; i < 5; ++i)
    std::printf(" %s=%d", kOpts[i], rs_get_option(s, kOpts[i]) ? 1 : 0);
  std::printf("\n");
}

// 打 nihao,回傳前 n 個候選。
std::vector<std::string> TypeNihao(rs_session s, int n) {
  std::vector<std::string> out;
  for (const char* p = "nihao"; *p; ++p)
    rs_process_key(s, static_cast<int32_t>(static_cast<unsigned char>(*p)), 0);
  const rs_snapshot* snap = rs_snapshot_acquire(s);
  if (snap) {
    for (int i = 0; i < snap->menu.count && i < n; ++i)
      out.push_back(snap->menu.items[i].text ? snap->menu.items[i].text : "");
    rs_snapshot_release(s);
  }
  rs_clear_composition(s);
  return out;
}

rs_variant VariantOf(rs_session s) {
  rs_variant v = RS_VARIANT_UNKNOWN;
  const rs_snapshot* snap = rs_snapshot_acquire(s);
  if (snap) {
    v = snap->status.variant;
    rs_snapshot_release(s);
  }
  return v;
}

void ShowCandidates(const std::vector<std::string>& c) {
  std::printf("       候選:");
  for (size_t i = 0; i < c.size(); ++i)
    std::printf(" %zu=%s", i + 1, c[i].c_str());
  std::printf("\n");
}

// ⚠ 只看第 3、4 個。你好 / 妳好 / 你 兩套字集相同。
bool LooksSimplified(const std::vector<std::string>& c) {
  return c.size() >= 4 && c[2] == "逆号" && c[3] == "拟好";
}
bool LooksTraditional(const std::vector<std::string>& c) {
  return c.size() >= 4 && c[2] == "逆號" && c[3] == "擬好";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "用法: variant_probe <shared> <user> [solo]\n");
    return 2;
  }
  const bool solo = (argc > 3 && std::strcmp(argv[3], "solo") == 0);
  rs_setup su{};
  su.shared_data_dir = argv[1];
  su.user_data_dir = argv[2];
  su.log_dir = "";
  su.app_name = "variant.probe";
  su.on_deploy = OnDeploy;
  if (!rs_init(&su)) {
    std::fprintf(stderr, "rs_init 失敗: %s\n", rs_last_error());
    return 1;
  }
  for (int i = 0; i < 600 && g_deploy.load() < 0; ++i)
    std::this_thread::sleep_for(std::chrono::seconds(1));
  if (g_deploy.load() != 1) {
    std::fprintf(stderr, "部署沒有成功結束\n");
    return 1;
  }

  // ── 情境 0b(solo):**只有** luna_pinyin 的設定檔 ────────────────
  //
  // 這一段推翻「沒有那組 radio 時 fallback 到 `simplification`」那個修法。
  //
  // 純 luna_pinyin 的 radio group(zh_hant / zh_hans / zh_hant_hk /
  // zh_hant_tw)**沒有 `reset:`**,而 ConcreteEngine::InitializeOptions()
  // 只在 reset_value >= 0 時才設值。所以四個全是 false,而輸出是繁體
  // (詞典本身就是繁體字集,沒有任何 simplifier 生效)。此時
  // `simplification` 若被我們寫成 1,fallback 就會畫「简」而輸出是繁體
  // —— 與使用者截圖一模一樣的缺陷,只是換一個方案觸發。
  //
  // ⚠ 為什麼要一份**只有 luna_pinyin** 的設定檔:見情境 0 的註解。
  //   本專案出貨的 schema_list 第一項是 luna_pinyin_tw,而
  //   `switcher/save_options` 會讓 zh_hant_tw 跟著跑到 luna_pinyin 上,
  //   所以在出貨設定下看不到「四個全假」的樣子。這一段要的是
  //   **方案自己**的事實,不是安裝現場的事實。
  if (solo) {
    std::printf("\n=== 0b. solo:只有 luna_pinyin 的設定檔 ===\n");
    rs_session s = rs_session_create();
    DumpOptions(s, "剛建 session");
    const rs_variant v = VariantOf(s);
    std::printf("       rs_status.variant = %s\n", VariantName(v));
    Check(v == RS_VARIANT_UNKNOWN,
          "0b: 四個 radio 全假 → variant 是 UNKNOWN(那一格整格不顯示)");
    const std::vector<std::string> c = TypeNihao(s, 6);
    ShowCandidates(c);
    Check(LooksTraditional(c),
          "0b: 而輸出是**繁體** —— 此時 fallback 到 simplification 會畫「简」");
    rs_session_destroy(s);
    std::printf("\n失敗 %d 項\n", g_fail);
    return g_fail == 0 ? 0 : 1;
  }

  // ── 情境 0(基準):出貨設定下,剛載入時那一格說什麼 ──────────────
  //
  // ⚠ **這一段必須跑在最前面**,而理由不是排版:
  //
  //   shared/default.yaml 的 `switcher/save_options` 列著
  //   full_shape / ascii_punct / simplification / extended_charset /
  //   zh_hant / zh_hans / zh_hant_tw。librime 會記住這些選項的狀態,
  //   而**同一個行程裡後來建的 session 會把它們吃回去**。也就是說:
  //   情境 A 一旦設過 zh_hans,這一段就再也看不到「剛載入」的樣子。
  //
  // ⚠ 這裡也是一個**與原本的分析不同**的實測結果,值得寫下來:
  //
  //   出貨的 schema_list 第一項是 luna_pinyin_tw(它的 __patch 把
  //   switches/@2/reset 設成 3,載入時 zh_hant_tw=1)。而 zh_hant_tw 在
  //   save_options 裡,所以**接著選 luna_pinyin 也會帶著 zh_hant_tw=1**
  //   —— 四個 radio 全假的狀態在出貨設定下幾乎看不到。
  //
  //   這不改變修法,只改變預期:RS_VARIANT_UNKNOWN 在真實安裝上是
  //   少見狀態,不是常態。「整格不顯示」仍然必須存在而且必須正確,
  //   但使用者多數時候會看到那一格。方案自己的事實由情境 0b 釘住。
  //
  // 這一段真正的判準只有一條,而它就是使用者回報的那件事:
  // **沒有套過任何簡繁的時候,那一格不得宣稱「简」。**
  std::printf("\n=== 0. 基準:出貨設定,還沒套過任何簡繁 ===\n");
  {
    rs_session s = rs_session_create();
    DumpOptions(s, "剛建 session");
    const rs_variant v0 = VariantOf(s);
    std::printf("       rs_status.variant = %s\n", VariantName(v0));
    const std::vector<std::string> c0 = TypeNihao(s, 6);
    ShowCandidates(c0);
    Check(v0 != RS_VARIANT_HANS, "0: 沒套過簡繁時,variant 不得宣稱 HANS");
    Check(LooksTraditional(c0), "0: 而輸出是繁體");
    // ⚠ 核心不變式:**variant 說简的時候,輸出就必須是簡體**。
    //   這一條在每一個情境都要成立,而它就是這條缺陷的定義。
    Check((v0 == RS_VARIANT_HANS) == LooksSimplified(c0),
          "0: variant 與實際輸出一致");
    rs_session_destroy(s);
  }

  // ── 情境 A:選方案 → 套簡繁(SESSION_NEW 走的順序)────────────
  std::printf("\n=== A. 選方案之後套簡繁 ===\n");
  {
    rs_session s = rs_session_create();
    rs_select_schema(s, "luna_pinyin_tw");
    DumpOptions(s, "選完方案");
    ApplyPlanSimplified(s);
    DumpOptions(s, "套完 PlanVariant");
    const rs_variant v = VariantOf(s);
    std::printf("       rs_status.variant = %s\n", VariantName(v));
    Check(v == RS_VARIANT_HANS, "A: variant 回報 HANS");
    const std::vector<std::string> c = TypeNihao(s, 6);
    ShowCandidates(c);
    Check(LooksSimplified(c), "A: 候選第 3、4 個是簡體(逆号 拟好)");
    Check((v == RS_VARIANT_HANS) == LooksSimplified(c),
          "A: variant 與實際輸出一致");
    rs_session_destroy(s);
  }

  // ── 情境 B:裸 rs_select_schema 再選一次同一個方案 ─────────────
  //
  // 這是**缺陷本身**。斷言的是「它真的會把簡繁洗掉」—— 也就是
  // audit_single_source.sh 規則 2 存在的理由。這一條若哪天變綠了,
  // 表示 librime 換了行為,那條規則要重新評估,不是默默放掉。
  std::printf("\n=== B. 裸 rs_select_schema 再選一次(缺陷的形狀)===\n");
  {
    rs_session s = rs_session_create();
    rs_select_schema(s, "luna_pinyin_tw");
    ApplyPlanSimplified(s);
    rs_select_schema(s, "luna_pinyin_tw");  // ← 沒有重套
    DumpOptions(s, "再選一次之後");
    const rs_variant v = VariantOf(s);
    std::printf("       rs_status.variant = %s\n", VariantName(v));
    Check(v != RS_VARIANT_HANS, "B: 簡繁真的被換方案洗掉了(variant 不再是 HANS)");
    const std::vector<std::string> c = TypeNihao(s, 6);
    ShowCandidates(c);
    Check(LooksTraditional(c), "B: 候選變回繁體(逆號 擬好)");
    Check((v == RS_VARIANT_HANS) == LooksSimplified(c),
          "B: variant 與實際輸出一致");
    rs_session_destroy(s);
  }

  // ── 情境 C:SelectAndApply 的順序(修法的形狀)──────────────────
  //
  // 選方案與套簡繁是一個不可分割的動作。engine.cc 的四個呼叫點
  // 全部要走這個順序,而「只有一個裸呼叫點」由 audit_single_source.sh
  // 在原始碼層面守。
  std::printf("\n=== C. 選完方案立刻重套(SelectAndApply 的順序)===\n");
  {
    rs_session s = rs_session_create();
    rs_select_schema(s, "luna_pinyin_tw");
    ApplyPlanSimplified(s);
    rs_select_schema(s, "luna_pinyin_tw");
    ApplyPlanSimplified(s);  // ← 這一行就是 SelectAndApply 做的事
    DumpOptions(s, "重套之後");
    const rs_variant v = VariantOf(s);
    std::printf("       rs_status.variant = %s\n", VariantName(v));
    Check(v == RS_VARIANT_HANS, "C: variant 仍然是 HANS");
    const std::vector<std::string> c = TypeNihao(s, 6);
    ShowCandidates(c);
    Check(LooksSimplified(c), "C: 候選仍然是簡體(逆号 拟好)");
    Check((v == RS_VARIANT_HANS) == LooksSimplified(c),
          "C: variant 與實際輸出一致");
    rs_session_destroy(s);
  }

  std::printf("\n失敗 %d 項\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
