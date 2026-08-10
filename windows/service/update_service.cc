// windows/service/update_service.cc — 見標頭。**不碰任何網路 API。**

#include "update_service.h"

#include <windows.h>

#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../common/sha256.h"
#include "../winshared/winutil.h"

namespace rimewin {

// ⚠ 路徑字首 `rime/` 是刻意保留的舊名(見 scripts/lib/product.env 的
//   R2_PREFIX):改了 = 已經發出去的連結全部 404。
const char kWinUpdateManifestUrl[] =
    "https://pub-d6a54d2e5f5947e2b0b23fb8e27ce0a5.r2.dev/rime/windows/"
    "version-windows.json";

namespace {

// 傳給安裝程式的旗標。
//
//   /SILENT           只顯示進度,不走精靈的每一頁。使用者已經在我們的
//                     介面上按過一次了,再問一次「下一步」是重複的。
//   /NORESTART        我們宣稱不重開機。這一行讓那句話不必依賴預設值。
//   /RESTARTIME       **我們自己的**:換完檔之後把服務叫回來(以登入者的
//                     身分)。沒有它的話,使用者按下更新之後設定視窗消失,
//                     然後什麼都不發生 —— 而輸入法其實要等他下一次打字
//                     才會被瘦 DLL 重新叫起來。
constexpr wchar_t kSetupArgs[] = L"/SILENT /NORESTART /RESTARTIME";

std::wstring FileNameOf(const std::wstring& path) {
  const size_t p = path.find_last_of(L"\\/");
  return p == std::wstring::npos ? path : path.substr(p + 1);
}

// 從網址取一個**安全的**檔名。
//
// ⚠ 這個字串來自伺服器。原樣拿去接路徑的話,一個
//   `..\..\Windows\System32\x.exe` 就會讓我們把下載的東西寫到別處。
//   所以:只留 [A-Za-z0-9._-],其餘一律換掉,而且強制副檔名。
std::wstring SafeSetupFileName(const WinUpdateManifest& m) {
  std::string raw = m.file;
  if (raw.empty()) {
    const size_t slash = m.url.find_last_of('/');
    raw = slash == std::string::npos ? std::string() : m.url.substr(slash + 1);
  }
  const size_t q = raw.find_first_of("?#");
  if (q != std::string::npos) raw = raw.substr(0, q);
  std::string clean;
  for (char c : raw) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (ok) clean.push_back(c);
  }
  // 前導的點會做出隱藏檔,而 ".." 之類的東西上面已經被過濾成 ".."。
  while (!clean.empty() && clean[0] == '.') clean.erase(clean.begin());
  if (clean.size() < 5) clean = "update";
  if (clean.size() > 100) clean.resize(100);
  const size_t dot = clean.find_last_of('.');
  const bool is_exe = dot != std::string::npos &&
                      clean.substr(dot) == std::string(".exe");
  if (!is_exe) clean += ".exe";
  return Utf8ToWide(clean);
}

std::string ReadWholeFileUtf8(const std::wstring& path) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::string();
  std::string out;
  char buf[4096];
  DWORD n = 0;
  while (::ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0)
    out.append(buf, n);
  ::CloseHandle(h);
  return out;
}

// 邊讀邊算,不把整個檔案讀進記憶體 —— 安裝程式有幾十 MB。
bool Sha256OfFile(const std::wstring& path, std::string* out) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  Sha256 sha;
  std::vector<char> buf(64 * 1024);
  DWORD n = 0;
  bool ok = true;
  for (;;) {
    if (!::ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr)) {
      ok = false;
      break;
    }
    if (n == 0) break;
    sha.Update(buf.data(), n);
  }
  ::CloseHandle(h);
  if (!ok) return false;
  *out = sha.HexDigest();
  return true;
}

bool WriteWholeFileUtf8(const std::wstring& path, const std::string& text) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD wrote = 0;
  const BOOL ok = ::WriteFile(h, text.data(),
                              static_cast<DWORD>(text.size()), &wrote, nullptr);
  ::CloseHandle(h);
  return ok && wrote == text.size();
}

// 開機佇列裡有沒有安裝目錄底下的東西。
//
// ⚠ 這是「換檔被拒」與「沒裝成而我們不知道為什麼」的分界線,而它同時是
//   我們對使用者的承諾的一部分:**我們不用重開機來繞過檔案被鎖住。**
//   所以這裡讀的是 Session Manager 的 PendingFileRenameOperations ——
//   windows/verify_installer.sh §13 斷言的也是這一份。
//
// ⚠ REG_MULTI_SZ:一串以 \0 分隔、\0\0 結尾的字串。第二個欄位可能是空的
//   (代表「開機時刪掉」)。讀不到一律回 false —— 「查不出來」不可以
//   變成「就是被鎖住了」,那會給使用者一個沒有根據的指示。
bool PendingRenamesTouch(const std::wstring& dir_lower) {
  if (dir_lower.empty()) return false;
  HKEY k = nullptr;
  const LSTATUS st = ::RegOpenKeyExW(
      HKEY_LOCAL_MACHINE,
      L"SYSTEM\\CurrentControlSet\\Control\\Session Manager", 0,
      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k);
  if (st != ERROR_SUCCESS) return false;
  DWORD type = 0, bytes = 0;
  LSTATUS q = ::RegQueryValueExW(k, L"PendingFileRenameOperations", nullptr,
                                 &type, nullptr, &bytes);
  if (q != ERROR_SUCCESS || type != REG_MULTI_SZ || bytes == 0 ||
      bytes > 4 * 1024 * 1024) {
    ::RegCloseKey(k);
    return false;
  }
  std::vector<wchar_t> data(bytes / sizeof(wchar_t) + 2, L'\0');
  q = ::RegQueryValueExW(k, L"PendingFileRenameOperations", nullptr, &type,
                         reinterpret_cast<LPBYTE>(data.data()), &bytes);
  ::RegCloseKey(k);
  if (q != ERROR_SUCCESS) return false;

  bool hit = false;
  size_t i = 0;
  while (i < data.size() && data[i] != L'\0') {
    std::wstring entry(&data[i]);
    i += entry.size() + 1;
    std::wstring low = entry;
    for (wchar_t& c : low)
      if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    // 我們自己排的舊 DLL 清理不算 —— 它不會讓任何東西要求重開機,
    // 而且它刪的是一顆沒有人會再載入的舊檔。判準與
    // windows/verify_installer.sh 的 pending_filter 同一條。
    if (low.find(L"rime_tsf.dll.old-") != std::wstring::npos) continue;
    if (low.find(dir_lower) != std::wstring::npos) {
      hit = true;
      break;
    }
  }
  return hit;
}

std::wstring LowerW(std::wstring s) {
  for (wchar_t& c : s)
    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
  return s;
}

}  // namespace

UpdateService::UpdateService(NetGate* net, SettingsStore* store,
                             std::wstring install_dir)
    : net_(net), store_(store), install_dir_(std::move(install_dir)) {
  ReloadInstalledVersion();
}

void UpdateService::ReloadInstalledVersion() {
  installed_ = InstalledVersion();
  if (install_dir_.empty()) return;
  installed_ =
      ParseInstalledVersion(ReadWholeFileUtf8(install_dir_ + L"\\version.txt"));
}

std::wstring UpdateService::UpdateDir() const {
  return Utf8ToWide(store_->user_dir()) + L"\\update";
}

std::wstring UpdateService::HandoffPath() const {
  return UpdateDir() + L"\\handoff.txt";
}

UpdateVerdict UpdateService::verdict() const {
  if (!have_manifest_) return UpdateVerdict::kUpToDate;
  return CompareVersion(installed_.version_code, latest_);
}

AppIdVerdict UpdateService::app_id_verdict() const {
  if (!have_manifest_) return AppIdVerdict::kUnknown;
  return CompareAppId(installed_.app_id, latest_.app_id);
}

bool UpdateService::Check(UpdateFailure* why) {
  UpdateFailure dummy = UpdateFailure::kNone;
  UpdateFailure& out = why ? *why : dummy;
  out = UpdateFailure::kNone;
  have_manifest_ = false;

  std::string text;
  const NetReport rep =
      net_->FetchText(kWinUpdateManifestUrl, NetPurpose::kUpdateManifest,
                      std::string(), &text, kMaxUpdateManifestBytes);
  if (rep.result != NetResult::kOk) {
    out = ClassifyManifestFetch(rep.result);
    return false;
  }
  const ManifestParseResult parsed =
      ParseWinUpdateManifest(text, kWinUpdateManifestUrl);
  if (!parsed.ok) {
    out = UpdateFailure::kUnreadable;
    return false;
  }
  latest_ = parsed.manifest;
  have_manifest_ = true;

  // 目標版本換了 → 上一次留下來的那一份沒有意義了。
  if (verified_ && FileNameOf(staged_path_) != SafeSetupFileName(latest_)) {
    verified_ = false;
    staged_path_.clear();
  }
  return true;
}

void UpdateService::PurgeStaged(const std::wstring& keep) {
  const std::wstring dir = UpdateDir();
  WIN32_FIND_DATAW fd{};
  HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const std::wstring name = fd.cFileName;
    if (name == keep) continue;
    if (name == L"handoff.txt") continue;
    ::DeleteFileW((dir + L"\\" + name).c_str());
  } while (::FindNextFileW(h, &fd));
  ::FindClose(h);
}

bool UpdateService::DownloadAndVerify(UpdateFailure* why) {
  UpdateFailure dummy = UpdateFailure::kNone;
  UpdateFailure& out = why ? *why : dummy;
  out = UpdateFailure::kNone;
  verified_ = false;
  staged_path_.clear();

  if (!have_manifest_) {
    out = UpdateFailure::kUnreachable;
    return false;
  }
  if (!installed_.valid()) {
    out = UpdateFailure::kOwnVersionUnknown;
    return false;
  }
  if (app_id_verdict() == AppIdVerdict::kChanged) {
    // ⚠ **排在下載之前**。這一行就是「不要下載幾十 MB 再讓使用者發現
    //   它裝下去會多一套」。
    out = UpdateFailure::kProductChanged;
    return false;
  }
  if (verdict() != UpdateVerdict::kUpdateAvailable) {
    out = UpdateFailure::kNone;
    return false;
  }

  const std::wstring dir = UpdateDir();
  ::CreateDirectoryW(dir.c_str(), nullptr);
  const std::wstring name = SafeSetupFileName(latest_);
  const std::wstring dest = dir + L"\\" + name;
  PurgeStaged(name);

  // 上限就是宣告的 size:多一個位元組都代表這不是我們要的那個檔案,
  // 沒有理由把它讀完再說。
  const NetReport rep = net_->DownloadFile(
      latest_.url, WideToUtf8(dest), NetPurpose::kUpdateSetup,
      latest_.version_name, latest_.size);
  if (rep.result != NetResult::kOk) {
    ::DeleteFileW(dest.c_str());
    out = ClassifyDownload(rep.result);
    return false;
  }

  std::string got;
  if (!Sha256OfFile(dest, &got)) {
    ::DeleteFileW(dest.c_str());
    out = UpdateFailure::kStagingWriteFailed;
    return false;
  }
  if (!Sha256HexEqual(latest_.sha256, got)) {
    // ⚠ **整條線上唯一一句可以說「檔案壞了」的話**,而且是因為這裡
    //   真的比對過。不符即整包丟棄:不留著、不「先裝再說」。
    ::DeleteFileW(dest.c_str());
    out = UpdateFailure::kSha256Mismatch;
    return false;
  }

  verified_ = true;
  staged_path_ = dest;
  return true;
}

bool UpdateService::HandOff(bool deploy_running, UpdateFailure* why) {
  UpdateFailure dummy = UpdateFailure::kNone;
  UpdateFailure& out = why ? *why : dummy;

  HandoffPreflight pre;
  pre.installed_version_known = installed_.valid();
  pre.have_manifest = have_manifest_;
  pre.verdict = verdict();
  pre.app_id = app_id_verdict();
  pre.file_present =
      !staged_path_.empty() &&
      ::GetFileAttributesW(staged_path_.c_str()) != INVALID_FILE_ATTRIBUTES;
  pre.sha256_verified = verified_;
  pre.deploy_running = deploy_running;
  if (!MayHandOff(pre, &out)) return false;

  // 交棒單要在**叫起安裝程式之前**寫。反過來的話,安裝程式停掉我們的
  // 那一瞬間可能剛好卡在寫檔前面 —— 於是更新真的裝上去了,而下一次啟動
  // 什麼都不知道,使用者得到的是沉默。
  char line[512];
  std::snprintf(line, sizeof(line),
                "version_code=%lld\nversion_name=%s\nfile=%s\n",
                static_cast<long long>(latest_.version_code),
                latest_.version_name.c_str(),
                WideToUtf8(staged_path_).c_str());
  ::CreateDirectoryW(UpdateDir().c_str(), nullptr);
  WriteWholeFileUtf8(HandoffPath(), line);

  // ⚠ ShellExecuteEx,不是 CreateProcess:安裝程式的 manifest 裡寫著
  //   requireAdministrator,而 CreateProcess 對那種執行檔會直接回
  //   ERROR_ELEVATION_REQUIRED(740)。這裡走的是與使用者雙擊它
  //   **完全同一條**路 —— 提權由它自己要,不是我們替它要。
  SHELLEXECUTEINFOW si{};
  si.cbSize = sizeof(si);
  si.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
  si.lpVerb = nullptr;  // 預設動作。**不寫 runas** —— 見上。
  si.lpFile = staged_path_.c_str();
  si.lpParameters = kSetupArgs;
  si.lpDirectory = UpdateDir().c_str();
  si.nShow = SW_SHOWNORMAL;
  const BOOL ok = ::ShellExecuteExW(&si);
  const DWORD err = ::GetLastError();
  if (si.hProcess) ::CloseHandle(si.hProcess);
  if (!ok) {
    // 沒叫起來 = 沒有交棒。單子要撤掉,否則下一次啟動會報一個
    // 從來沒有發生過的「更新沒有裝上去」。
    ::DeleteFileW(HandoffPath().c_str());
    out = ClassifyHandoff(false, err);
    return false;
  }
  out = UpdateFailure::kNone;
  return true;
}

UpdateOutcome UpdateService::Reconcile(UpdateFailure* why,
                                       std::string* version_name) {
  UpdateFailure dummy = UpdateFailure::kNone;
  UpdateFailure& out = why ? *why : dummy;
  out = UpdateFailure::kNone;

  const std::string text = ReadWholeFileUtf8(HandoffPath());
  const bool have = !text.empty();
  const InstalledVersion handed = ParseInstalledVersion(text);
  if (version_name) *version_name = handed.version_name;

  ReloadInstalledVersion();
  const UpdateOutcome outcome =
      ReconcileHandoff(have, handed.version_code, installed_.version_code,
                       PendingRenamesTouch(LowerW(install_dir_)), &out);

  if (have) {
    // 讀完就刪。留著的話下一次啟動會再報一次同樣的結果。
    ::DeleteFileW(HandoffPath().c_str());
    if (outcome == UpdateOutcome::kInstalled) {
      // 裝完了,那份幾十 MB 的安裝程式沒有留著的理由。
      PurgeStaged(L"");
      verified_ = false;
      staged_path_.clear();
    }
  }
  return outcome;
}

std::string UpdateService::DiagnosticLine() const {
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "update: installed=%lld app_id=%s manifest=%s verified=%s",
                static_cast<long long>(installed_.version_code),
                installed_.app_id.empty() ? "?" : installed_.app_id.c_str(),
                have_manifest_ ? "yes" : "no", verified_ ? "yes" : "no");
  return buf;
}

}  // namespace rimewin
