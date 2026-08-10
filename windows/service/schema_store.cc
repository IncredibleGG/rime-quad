#include "schema_store.h"

#include <chrono>
#include <thread>

#include "../common/archive_guard.h"
#include "../winshared/winutil.h"

namespace rimewin {

// ⚠ 這個位址與 Android / macOS 用的是同一份索引。它是**預設值**,
//   不是白名單:使用者改得了(見標頭)。
const char kDefaultStoreIndexUrl[] =
    "https://pub-d6a54d2e5f5947e2b0b23fb8e27ce0a5.r2.dev/rime/schemas/index.json";

namespace {

const char kInstalledRecord[] = "installed-packages.tsv";

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::wstring JoinPath(const std::string& dir_utf8, const std::string& rel_utf8) {
  std::wstring w = Utf8ToWide(dir_utf8);
  if (!w.empty() && w.back() != L'\\' && w.back() != L'/') w.push_back(L'\\');
  std::wstring r = Utf8ToWide(rel_utf8);
  for (wchar_t& c : r) {
    if (c == L'/') c = L'\\';
  }
  return w + r;
}

bool ReadWholeFile(const std::wstring& path, std::string* out) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size;
  if (!::GetFileSizeEx(h, &size) || size.QuadPart > 256LL * 1024 * 1024) {
    ::CloseHandle(h);
    return false;
  }
  out->resize(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  bool ok = true;
  if (size.QuadPart > 0) {
    ok = ::ReadFile(h, &(*out)[0], static_cast<DWORD>(size.QuadPart), &read,
                    nullptr) != 0 &&
         read == size.QuadPart;
  }
  ::CloseHandle(h);
  if (!ok) out->clear();
  return ok;
}

bool FileExistsW(const std::wstring& path) {
  const DWORD a = ::GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// 建出 path 底下缺的每一層目錄(path 本身是檔案)。
void EnsureParentDirs(const std::wstring& file_path) {
  const size_t slash = file_path.find_last_of(L'\\');
  if (slash == std::wstring::npos) return;
  const std::wstring dir = file_path.substr(0, slash);
  // 逐層建。SHCreateDirectoryEx 要 shell32,這裡不值得多一個相依。
  size_t pos = dir.find(L'\\');
  while (pos != std::wstring::npos) {
    ::CreateDirectoryW(dir.substr(0, pos).c_str(), nullptr);
    pos = dir.find(L'\\', pos + 1);
  }
  ::CreateDirectoryW(dir.c_str(), nullptr);
}

// 原子寫。理由與 settings_store.cc 相同:寫到一半斷電的話,
// 使用者下次啟動會拿到半份檔案,而半份 dict.yaml 會讓之後
// **每一次**部署都失敗,症狀是「這個方案突然壞了」而且沒有錯誤訊息。
bool WriteFileAtomicW(const std::wstring& path, const std::string& bytes) {
  EnsureParentDirs(path);
  const std::wstring tmp = path + L".part";
  HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  bool ok = true;
  size_t written_total = 0;
  while (written_total < bytes.size()) {
    const size_t chunk =
        (bytes.size() - written_total) > (1u << 20) ? (1u << 20)
                                                    : (bytes.size() - written_total);
    DWORD written = 0;
    if (!::WriteFile(h, bytes.data() + written_total,
                     static_cast<DWORD>(chunk), &written, nullptr) ||
        written != chunk) {
      ok = false;
      break;
    }
    written_total += written;
  }
  if (ok) ok = ::FlushFileBuffers(h) != 0;
  ::CloseHandle(h);
  if (!ok) {
    ::DeleteFileW(tmp.c_str());
    return false;
  }
  if (!::MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    ::DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

}  // namespace

/* ═══════════════════════ 連線 ═══════════════════════════════════ */

StoreNetwork::Result NetGateStoreNetwork::Fetch(const std::string& url,
                                                bool is_package,
                                                const std::string& label,
                                                int64_t max_bytes) {
  Result out;
  if (gate_ == nullptr) {
    // 沒有出口 = 不知道 = 關。「不知道」必須等於「關」。
    out.blocked = true;
    out.message = "the network gate is not available";
    return out;
  }
  std::string body;
  const NetReport r = gate_->FetchText(
      url, is_package ? NetPurpose::kStorePackage : NetPurpose::kStoreIndex,
      label, &body, max_bytes);
  if (r.result == NetResult::kOk) {
    out.ok = true;
    out.body = std::move(body);
    return out;
  }
  // ⚠ kBlocked 一定要與其他失敗分得開:開關關著要給一顆開啟開關的按鈕,
  //   連不上要給重試。把兩者壓成同一句紅字正是待辦 #62。
  out.blocked = NetResultIsBlocked(r.result);
  out.message = r.message.empty() ? NetResultText(r.result) : r.message;
  return out;
}

/* ═══════════════════════ 檔案 ═══════════════════════════════════ */

bool WinStoreFileSystem::JoinUser(const std::string& rel,
                                  std::wstring* out) const {
  // 第二道路徑檢查。第一道在 ArchiveGuard(字串規則),這一道在寫入前,
  // 兩道互相獨立 —— 任一道被繞過另一道還在。
  ArchiveLimits limits;
  if (!PathProblemOf(rel, limits).empty()) return false;
  if (!WindowsNameProblemOf(rel).empty()) return false;
  *out = JoinPath(user_, rel);
  return true;
}

bool WinStoreFileSystem::DataFileExists(const std::string& name) {
  std::wstring p;
  if (JoinUser(name, &p) && FileExistsW(p)) return true;
  if (shared_.empty()) return false;
  return FileExistsW(JoinPath(shared_, name));
}

bool WinStoreFileSystem::ReadDataFile(const std::string& name, std::string* out) {
  // librime 的搜尋順序:user 在前、shared 在後。
  std::wstring p;
  if (JoinUser(name, &p) && ReadWholeFile(p, out)) return true;
  if (shared_.empty()) return false;
  return ReadWholeFile(JoinPath(shared_, name), out);
}

bool WinStoreFileSystem::WriteUserFile(const std::string& rel,
                                       const std::string& bytes) {
  std::wstring p;
  if (!JoinUser(rel, &p)) return false;
  return WriteFileAtomicW(p, bytes);
}

bool WinStoreFileSystem::RemoveUserFile(const std::string& rel) {
  std::wstring p;
  if (!JoinUser(rel, &p)) return false;
  if (::DeleteFileW(p.c_str())) return true;
  // 本來就不在,對呼叫端而言與刪掉了是同一件事。
  return ::GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES;
}

bool WinStoreFileSystem::ReadUserFile(const std::string& rel, std::string* out) {
  std::wstring p;
  if (!JoinUser(rel, &p)) return false;
  return ReadWholeFile(p, out);
}

/* ═══════════════════════ 部署 ═══════════════════════════════════ */

StoreDeployer::Outcome EngineStoreDeployer::DeployAndWait(
    const StoreProgressFn& progress) {
  Outcome out;
  if (engine_ == nullptr) {
    // ⚠ **這不是部署失敗。** 檔案已經寫好了,服務下次啟動就會建。
    //   說成失敗會讓使用者去做一件不需要做的事。
    out.started = false;
    return out;
  }
  uint32_t seq = 0;
  if (!engine_->BeginDeploy(&seq)) {
    out.started = false;
    return out;
  }
  out.started = true;
  const auto t0 = std::chrono::steady_clock::now();
  while (true) {
    int status = 0;
    if (engine_->PollDeploy(seq, &status)) {
      out.ok = status == 1;
      out.elapsed_ms = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0)
              .count());
      if (!out.ok) out.last_error = "librime reported a failed deployment";
      return out;
    }
    const int elapsed = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0)
            .count());
    if (elapsed > timeout_s_ * 1000) {
      // 逾時。⚠ 不假裝失敗也不假裝成功 —— 部署**還在跑**,
      //   librime 沒有辦法取消它。呼叫端會把這句話原樣說給使用者。
      out.ok = false;
      out.elapsed_ms = elapsed;
      out.last_error = "the rebuild is still running after the time we waited";
      return out;
    }
    if (progress) {
      StoreProgress pr;
      pr.phase = StoreProgress::Phase::kDeploying;
      pr.elapsed_ms = elapsed;
      progress(pr);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

/* ═══════════════════════ 門面 ═══════════════════════════════════ */

SchemaStoreService::SchemaStoreService(NetGate* gate, Engine* engine,
                                       std::string user_dir_utf8,
                                       std::string shared_dir_utf8)
    : net_(gate),
      fs_(std::move(user_dir_utf8), std::move(shared_dir_utf8)),
      // 首次建詞庫可能要好幾分鐘(moran 的詞典很大)。
      deployer_(engine, 600) {}

StoreDeps SchemaStoreService::Deps() {
  StoreDeps d;
  d.net = &net_;
  d.fs = &fs_;
  d.deployer = &deployer_;
  d.now_ms = &NowMs;
  return d;
}

std::vector<InstalledPackage> SchemaStoreService::Installed() {
  std::string text;
  // ⚠ 讀取**不會建立檔案**:「從來沒裝過任何東西」與「裝過又全部移除」
  //   對使用者來說不是同一句話,而一個被讀取動作創造出來的空檔案
  //   會把前者變成後者。
  if (!fs_.ReadUserFile(kInstalledRecord, &text)) return {};
  return DecodeInstalled(text);
}

bool SchemaStoreService::SaveRegistry(const std::vector<InstalledPackage>& v) {
  return fs_.WriteUserFile(kInstalledRecord, EncodeInstalled(v));
}

std::string SchemaStoreService::installed_record_path() const {
  return fs_.user_dir() + "\\" + kInstalledRecord;
}

StoreOutcome SchemaStoreService::LoadIndex(const std::string& index_url,
                                           StoreIndex* out,
                                           std::vector<std::string>* warnings) {
  return FetchStoreIndex(Deps(), index_url, out, warnings);
}

StoreOutcome SchemaStoreService::Install(const std::string& index_url,
                                         const StoreIndex& index,
                                         const std::vector<std::string>& package_ids,
                                         const StoreProgressFn& progress) {
  std::vector<InstalledPackage> registry = Installed();
  std::vector<std::string> have;
  have.reserve(registry.size());
  for (const auto& p : registry) have.push_back(p.id);

  const DependencyResult dep = ResolveDependencies(index, package_ids, have);
  if (dep.status == DependencyResult::Status::kMissingDependency) {
    return StoreOutcome::Fail(StoreFailure::kUnknownPackage,
                              "required by " + dep.required_by, dep.missing);
  }
  if (dep.status == DependencyResult::Status::kUnknownPackage) {
    return StoreOutcome::Fail(StoreFailure::kUnknownPackage,
                              "not in the index", dep.missing);
  }

  const StoreOutcome o = InstallPackages(Deps(), index_url, index, dep.plan,
                                         &registry, progress);
  // ⚠ 成功的部分照樣要記帳,失敗才不會讓使用者重新下載一次
  //   (moran 是 31MB)。
  if (!SaveRegistry(registry) && o.ok) {
    return StoreOutcome::Fail(StoreFailure::kDisk,
                              "the packages are installed but the record of "
                              "them could not be written");
  }
  return o;
}

StoreOutcome SchemaStoreService::Enable(const std::vector<std::string>& schema_ids,
                                        const StoreProgressFn& progress) {
  return EnableSchemas(Deps(), schema_ids, progress);
}

StoreOutcome SchemaStoreService::Uninstall(const std::string& package_id,
                                           const StoreProgressFn& progress) {
  std::vector<InstalledPackage> registry = Installed();
  const StoreOutcome o =
      UninstallPackage(Deps(), package_id, &registry, progress);
  if (o.ok || o.failure == StoreFailure::kDeploy) {
    // 檔案真的刪了的兩種情形都要更新紀錄。
    SaveRegistry(registry);
  }
  return o;
}

}  // namespace rimewin
