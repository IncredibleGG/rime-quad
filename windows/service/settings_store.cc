#include "settings_store.h"

#include <vector>

#include "../winshared/winutil.h"

namespace rimewin {
namespace {

constexpr const char* kSettingsFile = "rimequad.settings";
constexpr const char* kDefaultCustom = "default.custom.yaml";
constexpr const char* kNetLogFile = "connections.tsv";

// 設定檔不該大。上限的目的是讓「檔案壞了變成幾百 MB」在讀取的第一步
// 就停下來,而不是變成一次巨大的配置。
constexpr DWORD kMaxReadBytes = 8u * 1024u * 1024u;

}  // namespace

SettingsStore::SettingsStore(std::string user_dir_utf8)
    : dir_(std::move(user_dir_utf8)) {}

std::string SettingsStore::PathIn(const char* name) const {
  return dir_ + "\\" + name;
}

std::string SettingsStore::settings_path() const { return PathIn(kSettingsFile); }
std::string SettingsStore::default_custom_path() const {
  return PathIn(kDefaultCustom);
}
std::string SettingsStore::net_log_path() const { return PathIn(kNetLogFile); }

std::string SettingsStore::ReadFileUtf8(const std::string& path) const {
  HANDLE h = ::CreateFileW(Utf8ToWide(path).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::string();
  LARGE_INTEGER size{};
  if (!::GetFileSizeEx(h, &size) || size.QuadPart <= 0 ||
      size.QuadPart > static_cast<LONGLONG>(kMaxReadBytes)) {
    ::CloseHandle(h);
    return std::string();
  }
  std::string out(static_cast<size_t>(size.QuadPart), '\0');
  DWORD got = 0;
  const BOOL ok = ::ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &got,
                             nullptr);
  ::CloseHandle(h);
  if (!ok) return std::string();
  out.resize(got);
  // UTF-8 BOM。使用者用記事本存過的話會有,而 BOM 會讓第一個鍵名
  // 變成「\xEF\xBB\xBFschema.forced」—— 那個鍵永遠不會被認出來。
  if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
      static_cast<unsigned char>(out[1]) == 0xBB &&
      static_cast<unsigned char>(out[2]) == 0xBF)
    out.erase(0, 3);
  return out;
}

bool SettingsStore::WriteFileAtomic(const std::string& path,
                                    const std::string& text) {
  const std::wstring wpath = Utf8ToWide(path);
  const std::wstring tmp = wpath + L".tmp";
  HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL ok = text.empty() ||
                  ::WriteFile(h, text.data(), static_cast<DWORD>(text.size()),
                              &written, nullptr);
  // Flush 之後才 rename。少了它,「檔案已經改名」與「內容真的落地」
  // 之間有一段時間差,而那段時間裡斷電會得到一個長度正確、內容全是零的檔案。
  if (ok) ::FlushFileBuffers(h);
  ::CloseHandle(h);
  if (!ok || written != text.size()) {
    ::DeleteFileW(tmp.c_str());
    return false;
  }
  if (!::MoveFileExW(tmp.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    ::DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

Settings SettingsStore::Load() {
  std::lock_guard<std::mutex> lock(mu_);
  return Settings::Parse(ReadFileUtf8(settings_path()));
}

bool SettingsStore::Save(const Settings& s) {
  std::lock_guard<std::mutex> lock(mu_);
  return WriteFileAtomic(settings_path(), s.Serialize());
}

std::string SettingsStore::ReadDefaultCustom() {
  std::lock_guard<std::mutex> lock(mu_);
  return ReadFileUtf8(default_custom_path());
}

bool SettingsStore::WriteDefaultCustom(const std::string& text) {
  std::lock_guard<std::mutex> lock(mu_);
  return WriteFileAtomic(default_custom_path(), text);
}

bool SettingsStore::DefaultCustomExists() const {
  std::lock_guard<std::mutex> lock(mu_);
  const DWORD attr =
      ::GetFileAttributesW(Utf8ToWide(default_custom_path()).c_str());
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::vector<NetLogEntry> SettingsStore::ReadNetLog() {
  std::lock_guard<std::mutex> lock(mu_);
  // ⚠ 讀不到就是空的。**不建立檔案。** 見標頭。
  return DecodeLog(ReadFileUtf8(net_log_path()));
}

void SettingsStore::ClearNetLog() {
  std::lock_guard<std::mutex> lock(mu_);
  ::DeleteFileW(Utf8ToWide(net_log_path()).c_str());
}

}  // namespace rimewin
