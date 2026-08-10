// windows/common/schema_preflight.h — 部署前的相依檢查
//
// 規範 §4 要求:部署失敗時「錯誤訊息必須明確告訴使用者**缺少哪一個詞典**,
// 而不是只說部署失敗」。而 `rs_last_error()` 給不了這個資訊 —— librime 的
// 部署失敗只回一個布林,細節散在 glog 的輸出裡。所以與其事後解讀,
// 不如**事前自己算**:把 schema yaml 讀一遍,把它宣告要用的檔案列出來,
// 逐一確認在 user / shared 目錄裡找得到。
//
// 附帶好處與 Android 相同:檢查在**修改 schema_list 之前**跑,缺東西時
// 根本不會去動 default.custom.yaml,也就不需要回滾。回滾仍然要有,
// 但它只需要處理「檔案都在、librime 仍然編不起來」這種預測不到的情形。
//
// ── ⚠ Severity 的分界線是查 librime 原始碼定出來的,照抄,不要重新發明 ──
//
// Android 端第一版把每一個找不到的檔案都當成「不給啟用」,結果**市集 98 個
// 方案裡有 20 個被自己的預檢擋死** —— 而 librime 根本不在乎那些檔案在不在。
//
// ⚠ **這一輪把整張表對著真的 librime 量過一遍**(host 版 rime_console,
//   /home/lc/dictfix-host)。結論是:**該擋的都該擋,但 Android 那份表
//   對「為什麼」寫錯了一格**,而「為什麼」正是下一個人拿來分類新情況的依據。
//
// | 宣告 | 量到的行為 | 我們 |
// |---|---|---|
// | `translator/dictionary` 缺 | `[deploy] FAILURE`,`dictionary ... failed to compile` | 擋 |
// | `__include` / `import_preset` 的目標缺 | **`[deploy] SUCCESS`**,但整份 schema 配置建不起來 → **候選 0 個** | 擋 |
// | `schema/dependencies` 缺 | 部署成功,`skipped unsatisfied dependency` | 放行 |
// | `reverse_lookup/dictionary` 等次要詞典缺 | 部署成功 | 放行 |
// | `grammar/language` 的 `.gram` 缺 | 部署成功,翻譯器照常運作 | 放行 |
//
// 第二列的對照實驗(同一份 schema,只改 `punctuator/__include` 的目標存不存在):
//   目標在  → `[deploy] SUCCESS`,輸入 nihao 得到 **5 個候選**
//   目標不在 → `[deploy] SUCCESS`,輸入 nihao 得到 **0 個候選**
// 也就是說,它的症狀**不是**「部署失敗」,而是與 lua 同一種:
// **部署成功、畫面上沒有任何錯誤、打不出任何字**。擋的理由是這個,
// 不是 Android 註解寫的「回 false」—— 那一句經不起實測。
//
// ⚠ 這是一個**四端的文件分歧**,不是程式碼分歧(三端的判斷結果一樣)。
//   Android 的 `SchemaPreflight.kt` 與 macOS 的 `SchemaPreflight.swift`
//   都寫著「回 false」。要不要回頭改那兩份註解,由規範所有權方決定。
//
// 放行的那些仍然照樣回報,只是標成 kWarning:使用者該知道「這個方案的
// 筆畫反查用不了」,但那不是不讓他打字的理由。
//
// ── ⚠ Windows 專屬:lua ──────────────────────────────────────────
//
// **Windows 這一輪沒有編 librime-lua**(`windows/build.sh` 的
// `check_lua_sandbox` 明著寫了,README 的「已知限制」也有)。於是一個宣告
// `lua_translator@…` 的方案在 Windows 上會**部署成功但一個候選都沒有** ——
// 那是最難察覺的失敗模式:畫面上沒有任何錯誤,使用者只會覺得「這個輸入法壞了」。
//
// 所以 lua 在這裡是一條 **kBlocking**:寧可在按下「啟用」時說清楚
// 「這個方案要 lua,Windows 版還沒有」,也不要讓他裝完之後對著一個
// 打不出字的方案。
//
// ⚠ 為什麼擋在**方案**這一層而不是整個套件:實測目前索引的 34 個套件,
//   6 個帶 .lua 檔,但只有 4 個套件、97 個方案裡的 17 個真的宣告了 lua_* 元件
//   (`radical-pinyin` 與 `zrm` 帶 lua 檔但方案沒用到,照樣能用)。
//   整包擋掉會連同一包裡不需要 lua 的方案一起殺掉。
//
// ⚠ **已知的漏網之魚**:lua 元件如果是寫在被 `__include` 進來的另一個檔案裡,
//   這裡看不到(本檔不追 include)。那種情形的症狀仍然是「部署成功、沒有候選」。
//   要根治只有一條路:真的把 librime-lua 連同 `patches/librime-lua@sandbox.patch`
//   編進 Windows 版(待辦 #27)。
//
// ⚠ 這**不是**完整的 librime 配置解析器。它刻意只認幾個高頻欄位,
//   而且只在「找不到」時說話 —— 誤報會擋住合法套件,比漏報更糟。
#ifndef RIMEWIN_SCHEMA_PREFLIGHT_H_
#define RIMEWIN_SCHEMA_PREFLIGHT_H_

#include <functional>
#include <string>
#include <vector>

namespace rimewin {

// Windows 目前有沒有 lua。**唯一的決定處** ——
// 哪一天真的編了 librime-lua,改這一行,而 tests/test_schema_preflight.cc
// 兩個分支都有案例釘住(改了這一行不會有任何測試變綠又變紅,
// 但把「不支援時擋下」拿掉會紅)。
constexpr bool kLuaSupported = false;

enum class PreflightKind {
  kDictionary,  // translator / reverse_lookup 的 dictionary
  kSchema,      // schema/dependencies 指名的另一個方案
  kConfig,      // __include / import_preset 指向的配置檔
  kGrammar,     // grammar/language 指向的語言模型
  kLua,         // 方案宣告了 lua_* 元件,而這一版沒有 lua
};

enum class PreflightSeverity {
  kBlocking,  // 部署一定失敗(或一定沒有候選)—— 不要動 schema_list
  kWarning,   // 部署會成功,但某個功能不會有作用。告知,不阻擋
};

struct PreflightMissing {
  PreflightKind kind = PreflightKind::kDictionary;
  // 缺的那個檔案(kLua 時是那個元件名,例如 "lua_translator@*moran")。
  std::string name;
  std::string referenced_by;  // 方案 id
  PreflightSeverity severity = PreflightSeverity::kBlocking;
};

struct PreflightReport {
  std::string schema_id;
  std::vector<PreflightMissing> missing;

  std::vector<PreflightMissing> Blocking() const;
  std::vector<PreflightMissing> Warnings() const;
  // 「可以啟用嗎」。⚠ 這**不是**「沒有任何東西缺」。
  bool Ok() const;
};

// 「這個檔名在 user / shared 裡找得到嗎」。抽成介面是為了讓這一整套
// 在 Ubuntu 上測得完 —— 檔案系統不該出現在判斷邏輯裡。
using FileExistsFn = std::function<bool(const std::string&)>;

// 檢查一份 `*.schema.yaml` 的內容。
PreflightReport PreflightSchemaText(const std::string& schema_id,
                                    const std::string& text,
                                    const FileExistsFn& exists,
                                    bool lua_supported = kLuaSupported);

// ── 底下三支公開是為了讓測試直接打,不必繞一整份 yaml ──────────

struct IncludeRef {
  std::string file_name;
  bool optional = false;
};

// `__include` 指到哪個檔案,或者根本不指到檔案(回 false)。
//
// ⚠ 這一段在 Android 端曾經整段寫錯,代價是市集裡十個方案按下「啟用」
//   被擋死。librime 自己的註解(`config_compiler.cc` 的 `ParseInclude`)
//   就是答案:
//       __include: path/to/local/node
//       __include: filename[.yaml]:/path/to/external/node
//   **有沒有冒號才是分界線,不是開頭的斜線。** 沒有冒號時
//   `CreateReference()` 直接取 `current_resource_id()`,也就是同一個檔案。
//   結尾的 `?` 代表可有可無(`Reference::optional`),缺了不算失敗。
bool ParseIncludeTarget(const std::string& raw, IncludeRef* out);

// 這份 yaml 有沒有宣告 lua 元件。first 會被填成第一個命中的元件名。
bool ReferencesLuaComponent(const std::string& text, std::string* first);

// 掃出來的一組 (最上層鍵, 鍵, 值)。公開只為了測試掃描器本身 ——
// 「yaml 讀錯了」與「規則寫錯了」是兩種故障,要分得開。
struct YamlPair {
  std::string top_key;
  std::string key;
  std::string value;
};
std::vector<YamlPair> ScanSchemaYaml(const std::string& text);

}  // namespace rimewin

#endif  // RIMEWIN_SCHEMA_PREFLIGHT_H_
