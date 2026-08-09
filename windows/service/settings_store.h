// windows/service/settings_store.h — 設定與 default.custom.yaml 的檔案存取
//
// 判斷邏輯全部在 windows/common/(settings.h、schema_list_patch.h),
// 那邊在 Ubuntu 上有測試。這裡只做 Windows 的檔案 I/O,而且只做三件事:
//
//   1. **原子寫。** 先寫暫存檔再 MoveFileEx(REPLACE_EXISTING)。
//      設定檔寫到一半斷電的話,使用者下次啟動會拿到半份設定 ——
//      而半份設定解析得出來(壞行會被丟掉),所以症狀是
//      「有些設定莫名其妙不見了」,沒有任何錯誤訊息。
//   2. **序列化存取。** 設定視窗在 UI 執行緒上,連線在各自的執行緒上,
//      兩邊都會讀寫。
//   3. **失敗不丟例外。** 這支進程崩了使用者就沒有輸入法。
//
// ⚠ 路徑一律是 `%APPDATA%\LuminaKey` 底下(由 winshared 的
//   RimeUserDataDir() 決定,那是唯一的決定處)。不可以寫進安裝目錄 ——
//   理由見 service/main.cc 檔頭(librime 寫不進去時不會停下來)。
#ifndef RIMEWIN_SERVICE_SETTINGS_STORE_H_
#define RIMEWIN_SERVICE_SETTINGS_STORE_H_

#include <windows.h>

#include <mutex>
#include <string>
#include <vector>

#include "../common/net_policy.h"
#include "../common/settings.h"

namespace rimewin {

class SettingsStore {
 public:
  explicit SettingsStore(std::string user_dir_utf8);

  // 讀不到就回一份空的(= 全部跟隨預設)。**絕不失敗。**
  Settings Load();
  // 寫。回傳 false 代表沒寫成功,呼叫端要告訴使用者 ——
  // 安靜地失敗會變成「設定改了,重開就沒了」。
  bool Save(const Settings& s);

  std::string settings_path() const;
  std::string default_custom_path() const;
  std::string net_log_path() const;
  const std::string& user_dir() const { return dir_; }

  // default.custom.yaml。讀不到回空字串(呼叫端要分辨「空檔案」與
  // 「讀不到」的話,用 exists)。
  std::string ReadDefaultCustom();
  bool WriteDefaultCustom(const std::string& text);
  bool DefaultCustomExists() const;

  // ── 連網紀錄 ──────────────────────────────────────────────
  //
  // ⚠ 讀取**不會建立檔案**。「開關從沒開過 → 紀錄檔根本不存在」
  //   這句話是使用者驗證我們的方式,而一個被讀取動作創造出來的空檔案
  //   會讓那句話變成「紀錄檔存在但是空的」—— 意思不一樣。
  std::vector<NetLogEntry> ReadNetLog();
  void ClearNetLog();

 private:
  std::string PathIn(const char* name) const;
  std::string ReadFileUtf8(const std::string& path) const;
  bool WriteFileAtomic(const std::string& path, const std::string& text);

  mutable std::mutex mu_;
  std::string dir_;
};

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_SETTINGS_STORE_H_
