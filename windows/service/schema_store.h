// windows/service/schema_store.h — 方案市集的 Windows 側接線
//
// 判斷與流程全部在 `windows/common/store_engine.h`(那邊在 Ubuntu 上有
// 單元測試,含「開關關著時一個位元組都沒出去」與「預檢擋下時
// default.custom.yaml 一個位元組都沒被動過」)。這裡只做三件 Windows 才有的事:
//
//   1. **連線**:轉呼叫 NetGate。⚠ 這裡**沒有**、也不可以有任何 WinHTTP ——
//      單一出口是硬約束,windows/audit_offline_win.sh 的允許清單裡
//      只有 service/net_gate.cc 一個路徑。
//   2. **檔案**:原子寫(暫存檔 + MoveFileEx),以及 user → shared 的搜尋順序。
//   3. **部署**:轉呼叫 Engine 的 BeginDeploy / PollDeploy 並等它結束。
//
// ⚠ 執行緒:整組 API 都是**同步阻塞**的(下載一包 31MB 的方案要好幾秒,
//   首次部署要好幾分鐘)。一律由呼叫端放到背景執行緒跑,絕不可以在
//   UI 執行緒上呼叫 —— 服務進程的 UI 執行緒同時在跑候選窗。
#ifndef RIMEWIN_SERVICE_SCHEMA_STORE_H_
#define RIMEWIN_SERVICE_SCHEMA_STORE_H_

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

#include "../common/store_engine.h"
#include "engine.h"
#include "net_gate.h"

namespace rimewin {

// 索引的位址。⚠ 使用者改得了(設定檔的 `store.index_url`)——
// 這是刻意的:離線為預設的產品不該把「只能連我們的伺服器」寫死,
// 而且 net_policy.h 明著說不做主機白名單。
extern const char kDefaultStoreIndexUrl[];

// ── 連線:唯一的實作,轉呼叫 NetGate ──────────────────────────
class NetGateStoreNetwork : public StoreNetwork {
 public:
  explicit NetGateStoreNetwork(NetGate* gate) : gate_(gate) {}
  Result Fetch(const std::string& url, bool is_package, const std::string& label,
               int64_t max_bytes) override;

 private:
  NetGate* gate_;
};

// ── 檔案:使用者資料目錄 + 隨安裝檔出貨的 shared ────────────────
class WinStoreFileSystem : public StoreFileSystem {
 public:
  WinStoreFileSystem(std::string user_dir_utf8, std::string shared_dir_utf8)
      : user_(std::move(user_dir_utf8)), shared_(std::move(shared_dir_utf8)) {}

  bool DataFileExists(const std::string& name) override;
  bool ReadDataFile(const std::string& name, std::string* out) override;
  bool WriteUserFile(const std::string& rel, const std::string& bytes) override;
  bool RemoveUserFile(const std::string& rel) override;
  bool ReadUserFile(const std::string& rel, std::string* out) override;

  const std::string& user_dir() const { return user_; }

 private:
  // ⚠ rel 已經通過 ArchiveGuard(沒有 ..、沒有絕對路徑、沒有反斜線、
  //   沒有保留裝置名、沒有 ADS)。這裡**再擋一次**:第二道與第一道
  //   互相獨立,任一道被繞過另一道還在。
  bool JoinUser(const std::string& rel, std::wstring* out) const;

  std::string user_;
  std::string shared_;
};

// ── 部署:轉呼叫引擎那一條執行緒 ────────────────────────────────
class EngineStoreDeployer : public StoreDeployer {
 public:
  // engine 可以是 nullptr(設定介面在服務沒起來時也開得起來)——
  // 那時 started 會是 false,而**那是一句不同的話**,不是部署失敗。
  EngineStoreDeployer(Engine* engine, int timeout_seconds)
      : engine_(engine), timeout_s_(timeout_seconds) {}
  Outcome DeployAndWait(const StoreProgressFn& progress) override;

 private:
  Engine* engine_;
  int timeout_s_;
};

// ── 把三者兜起來的門面 ──────────────────────────────────────────
//
// 安裝紀錄存在使用者資料目錄的 `installed-packages.tsv`。
// TSV 的理由與連網紀錄相同:使用者直接 `type` 出來就看得懂。
class SchemaStoreService {
 public:
  SchemaStoreService(NetGate* gate, Engine* engine, std::string user_dir_utf8,
                     std::string shared_dir_utf8);

  // 取索引。**會連網**(所以開關關著時回 kSwitchOff)。
  StoreOutcome LoadIndex(const std::string& index_url, StoreIndex* out,
                         std::vector<std::string>* warnings);

  // 下載並安裝 package_ids(含相依)。不改 schema_list、不部署。
  StoreOutcome Install(const std::string& index_url, const StoreIndex& index,
                       const std::vector<std::string>& package_ids,
                       const StoreProgressFn& progress);

  // 啟用方案(預檢 → 改 schema_list → 部署 → 失敗回滾)。**不連網。**
  StoreOutcome Enable(const std::vector<std::string>& schema_ids,
                      const StoreProgressFn& progress);

  StoreOutcome Uninstall(const std::string& package_id,
                         const StoreProgressFn& progress);

  // 已安裝的套件。讀不到回空的(**不會建立檔案**)。
  std::vector<InstalledPackage> Installed();
  std::string installed_record_path() const;

 private:
  bool SaveRegistry(const std::vector<InstalledPackage>& v);
  StoreDeps Deps();

  NetGateStoreNetwork net_;
  WinStoreFileSystem fs_;
  EngineStoreDeployer deployer_;
};

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_SCHEMA_STORE_H_
