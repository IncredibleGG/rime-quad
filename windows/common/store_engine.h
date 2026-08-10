// windows/common/store_engine.h — 方案市集的整條流程(純邏輯)
//
// 下載 → 驗 sha256 → zip 安全檢查 → 解壓 → 記帳 → 預檢 → 改 schema_list →
// 部署 → 失敗回滾。
//
// 對照:Android 的 store/SchemaStore.kt、macOS 的 StoreEngine.swift。
//
// ── 為什麼整條流程住在 common/ 而不是 service/ ────────────────────
//
// 因為**這條流程的每一個分岔都是「失敗時使用者看到什麼」**,而那正是
// 這個專案反覆吃虧的地方(待辦 #62:三種不同的失敗在畫面上是同一句紅字)。
// 網路、檔案、librime 三種副作用全部走介面注入,於是
// 「下載到一半使用者把連網開關關掉」「sha256 對不上」「缺一本詞典」
// 「default.custom.yaml 看不懂」「部署失敗而且回滾也失敗」
// 這些情境在 Ubuntu 上就跑得出來,不必真的建一次詞庫。
//
// ── ⚠ 連網一律走地基那條線的出口 ────────────────────────────────
//
// 本檔**不知道** WinHTTP 存在,它只看得到 StoreNetwork 這個介面,
// 而唯一的實作在 service/schema_store.cc 裡包 NetGate。單一出口是硬約束:
// windows/audit_offline_win.sh 的允許清單裡只有 service/net_gate.cc。
//
// ── ⚠ 回滾的範圍是刻意的 ────────────────────────────────────────
//
// 安裝到一半失敗時,已經解壓成功的套件**留在磁碟上不刪** —— 它們沒有被
// 加進 schema_list,librime 不會碰它們,留著的成本是零,而刪掉的成本是
// 使用者要重新下載一次(moran 是 31MB)。
//
// 真正需要回滾的只有 EnableSchemas:它動了 default.custom.yaml。
// 而因為預檢在動檔案**之前**跑,絕大多數失敗根本走不到需要回滾的地方。
#ifndef RIMEWIN_STORE_ENGINE_H_
#define RIMEWIN_STORE_ENGINE_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "archive_guard.h"
#include "schema_preflight.h"
#include "store_index.h"

namespace rimewin {

// ── 失敗的種類 ──────────────────────────────────────────────────
//
// ⚠ **這個列舉存在的理由就是「不要把不同的失敗壓成同一句紅字」。**
//   每一種對使用者的下一步都不同:開關關著要給一顆開啟開關的按鈕,
//   連不上要給重試,缺詞典要指名缺哪一本,而回滾失敗要叫人來。
enum class StoreFailure {
  kNone = 0,
  kSwitchOff,        // 連網開關是關的。**不曾連線,不曾記錄**
  kNetwork,          // 連不上、逾時、HTTP 非 200
  kIntegrity,        // sha256 或 CRC 對不上
  kArchive,          // zip 安全檢查不過
  kDisk,             // 寫不進去
  kIndex,            // 索引解析不出來 / 版本不相容
  kUnknownPackage,   // 選的套件或它的相依不在索引裡
  kPreflight,        // 缺詞典、缺配置、需要 lua
  kPatch,            // default.custom.yaml 看不懂或寫不進去
  kDeploy,           // librime 部署失敗
  kRollbackFailed,   // 部署失敗**而且**改回去也失敗 —— 需要人工介入
};

const char* StoreFailureToken(StoreFailure f);   // 診斷用,英文

struct StoreOutcome {
  bool ok = false;
  StoreFailure failure = StoreFailure::kNone;
  // 英文的故障載荷(§4.11)。UI 把它當參數帶進在地化字串,
  // **不是**直接畫上去的那一句話。
  std::string detail;
  // 出問題的那個套件 / 方案 / 檔名,給 UI 指名用。
  std::string subject;
  // 不擋、但使用者該知道的事(例如反查詞典不在)。英文。
  std::vector<std::string> notes;
  // 動過 schema_list 又放回去了。
  bool rolled_back = false;

  static StoreOutcome Ok() {
    StoreOutcome o;
    o.ok = true;
    return o;
  }
  static StoreOutcome Fail(StoreFailure f, const std::string& detail,
                           const std::string& subject = "") {
    StoreOutcome o;
    o.failure = f;
    o.detail = detail;
    o.subject = subject;
    return o;
  }
};

// ── 進度 ────────────────────────────────────────────────────────

struct StoreProgress {
  enum class Phase {
    kDownloading,
    kVerifying,
    kExtracting,
    kPreflight,
    kDeploying,
    kRollingBack,
  };
  Phase phase = Phase::kDownloading;
  std::string package;   // 套件的顯示名
  int ordinal = 0;       // 第幾個(從 1 起)
  int total = 0;
  int64_t read = 0;
  int64_t bytes = 0;
  int elapsed_ms = 0;    // 只有 kDeploying 有意義
};
using StoreProgressFn = std::function<void(const StoreProgress&)>;

// ── 注入的三個副作用 ────────────────────────────────────────────

class StoreNetwork {
 public:
  virtual ~StoreNetwork() = default;
  struct Result {
    bool ok = false;
    // ⚠ **blocked 一定要與其他失敗分得開。** 見 net_gate_core.h 的 NetResult。
    bool blocked = false;
    std::string message;  // 英文
    std::string body;
  };
  // is_package 決定紀錄裡的用途欄(STORE_INDEX / STORE_PACKAGE)。
  virtual Result Fetch(const std::string& url, bool is_package,
                       const std::string& label, int64_t max_bytes) = 0;
};

class StoreFileSystem {
 public:
  virtual ~StoreFileSystem() = default;
  // 這個檔名在 librime 的搜尋路徑(user 在前、shared 在後)裡找得到嗎。
  virtual bool DataFileExists(const std::string& name) = 0;
  // 依同樣的順序讀一份資料檔。⚠ 預檢要讀的 *.schema.yaml 可能來自
  // shared(隨安裝檔出貨的內建方案),不一定在使用者目錄裡。
  virtual bool ReadDataFile(const std::string& name, std::string* out) = 0;
  // 寫進使用者資料目錄。rel 是已經通過 ArchiveGuard 的相對路徑。
  virtual bool WriteUserFile(const std::string& rel, const std::string& bytes) = 0;
  virtual bool RemoveUserFile(const std::string& rel) = 0;
  virtual bool ReadUserFile(const std::string& rel, std::string* out) = 0;
};

class StoreDeployer {
 public:
  virtual ~StoreDeployer() = default;
  struct Outcome {
    bool started = false;   // false = 引擎根本沒在跑
    bool ok = false;
    int elapsed_ms = 0;
    std::string last_error;  // 英文
  };
  // **同步阻塞**直到真的完成。
  virtual Outcome DeployAndWait(const StoreProgressFn& progress) = 0;
};

// ── 安裝紀錄 ────────────────────────────────────────────────────
//
// 「已安裝」與「已啟用」是兩件事:裝了不代表在 schema_list 裡,
// 而移除必須知道當初寫了哪些檔案(否則只能整個目錄砍掉,那會連同
// 使用者自己放的東西一起消失)。
//
// TSV,一行一個套件。理由與連網紀錄相同:使用者直接 `type` 出來就看得懂。
struct InstalledPackage {
  std::string id;
  std::string upstream_commit;
  std::string sha256;
  int64_t installed_at_ms = 0;
  std::vector<std::string> schemas;  // 這個套件提供的方案 id
  std::vector<std::string> files;    // 相對於使用者資料目錄
};

std::string EncodeInstalledLine(const InstalledPackage& p);
bool DecodeInstalledLine(const std::string& line, InstalledPackage* out);
std::vector<InstalledPackage> DecodeInstalled(const std::string& text);
std::string EncodeInstalled(const std::vector<InstalledPackage>& v);

// ── 流程 ────────────────────────────────────────────────────────

struct StoreDeps {
  StoreNetwork* net = nullptr;
  StoreFileSystem* fs = nullptr;
  StoreDeployer* deployer = nullptr;
  // 現在的毫秒時間(給安裝紀錄)。
  std::function<int64_t()> now_ms;
};

// 取索引並解析。
StoreOutcome FetchStoreIndex(const StoreDeps& deps, const std::string& index_url,
                             StoreIndex* out, std::vector<std::string>* warnings);

// 下載並解壓 plan 裡的每一個套件。**不碰 schema_list、不部署。**
//
// registry 進來是目前的安裝紀錄,成功安裝的套件會被加進去(呼叫端負責存檔)。
StoreOutcome InstallPackages(const StoreDeps& deps, const std::string& index_url,
                             const StoreIndex& index,
                             const DependencyPlan& plan,
                             std::vector<InstalledPackage>* registry,
                             const StoreProgressFn& progress);

// 把 schema_ids 加進 schema_list 並部署。
//
// 順序**刻意**是:預檢 → 改檔案 → 部署 → 失敗改回去。
// 預檢在改檔案之前,所以「缺一本詞典」這種最常見的失敗根本不會動到檔案。
StoreOutcome EnableSchemas(const StoreDeps& deps,
                           const std::vector<std::string>& schema_ids,
                           const StoreProgressFn& progress);

// 移除一個套件:刪掉它寫下的檔案、從 schema_list 拿掉它的方案、重新部署。
//
// ⚠ 呼叫端必須先問 DependentsOf() —— 這裡不替使用者決定「還有別的方案
//   在用這一包」該怎麼辦。
StoreOutcome UninstallPackage(const StoreDeps& deps, const std::string& package_id,
                              std::vector<InstalledPackage>* registry,
                              const StoreProgressFn& progress);

// 索引的 base_url + 檔名 → 絕對網址(轉呼叫 net_policy 的同一支,
// 四端對這件事必須算出同一個結果)。
std::string StorePackageUrl(const std::string& index_url,
                            const std::string& base_url, const std::string& file);

}  // namespace rimewin

#endif  // RIMEWIN_STORE_ENGINE_H_
