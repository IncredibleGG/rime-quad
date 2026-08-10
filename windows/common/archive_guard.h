// windows/common/archive_guard.h — 解壓前的安全檢查(docs/schema-store.md §4)
//
// 這是 Android `store/ArchiveGuard.kt` 的移植。**規則照抄** ——
// 四端對「這一包能不能收」的答案必須一樣,否則使用者拿同一個 zip 在兩台
// 裝置上會得到兩種結果,而我們沒有辦法解釋是哪一邊錯了。
//
// 每一行都在假設「zip 是攻擊者給的」。市集下載的套件有 sha256,但
// **使用者自帶的 zip 沒有任何憑據**,而兩者走同一條解壓路徑,
// 所以檢查一律以最壞情況為準。
//
// ── 宣告的大小會說謊 ────────────────────────────────────────────
// 中央目錄裡的 uncompressed size 是攻擊者可控的欄位。所以解壓炸彈的防線
// 有兩層:先用宣告值擋掉明目張膽的,再在**實際解壓時**硬性計數
// (zip_reader.h 的 Inflate 有 max_out)。只做前者等於沒做。
//
// ── ⚠ Windows 多出來的四條 ──────────────────────────────────────
//
// 下面這幾條在 Android 上不存在,不是因為誰漏了,是因為 NTFS 與 Win32
// 的路徑語意本來就不一樣。它們**不能**只寫在註解裡:
//
//   1. **保留裝置名。** `CON` / `PRN` / `AUX` / `NUL` / `COM1`…`COM9` /
//      `LPT1`…`LPT9`,**含副檔名也算**(`con.yaml` 一樣是 CON)。
//      在 Win32 上開啟這種名字會開到裝置而不是檔案 —— 寫進去的位元組
//      跑到主控台或印表機,而 `CreateFile` 不會失敗。
//   2. **交替資料流(ADS)。** 檔名裡的 `:` 會把後面的東西當成資料流名,
//      `readme.txt:evil.exe` 寫出來的是一個藏在 readme.txt 裡、
//      任何檔案總管都看不到的檔案。
//   3. **尾端的點與空白。** Win32 建檔時會**安靜地去掉**它們,於是
//      `foo.yaml ` 與 `foo.yaml` 落在同一個檔案 —— 一個 zip 就能覆蓋
//      另一個 entry 剛寫好的內容,而路徑檢查看到的是兩個不同的名字。
//      (Android 那一份也擋這一條,理由相同。)
//   4. **萬用字元與其他 Win32 不合法字元** `< > : " | ? *`。
//      擋掉的理由是「一個叫 `*.yaml` 的檔案」對之後每一個處理它的工具
//      都是意外,而它對合法套件毫無用處。
//
// ⚠ **`.lua` 為什麼還在白名單裡**,見 schema_preflight.h 的 lua 那一段。
//   簡單說:Windows 這一輪沒有編 librime-lua,所以 .lua 落在磁碟上是
//   **死資料**,沒有東西會載入它;而**擋在啟用那一關**(預檢)比擋在
//   解壓那一關準確得多 —— 34 個套件裡 6 個帶 lua 檔,但只有 4 個套件、
//   17 個方案真的宣告了 lua_* 元件,其餘照樣能用。
#ifndef RIMEWIN_ARCHIVE_GUARD_H_
#define RIMEWIN_ARCHIVE_GUARD_H_

#include <cstdint>
#include <string>
#include <vector>

#include "zip_reader.h"

namespace rimewin {

struct ArchiveLimits {
  // entry 數量上限。RIME 方案最多幾十個檔案(實測最多 57),2000 很寬鬆。
  int max_entries = 2000;
  // 單一檔案解壓後的位元組上限。最大的合法檔案是語言模型,約 6MB;
  // 實測最大的單一 entry 在 moran 裡,約 40MB。
  int64_t max_entry_bytes = 64LL * 1024 * 1024;
  // 整包解壓後的位元組上限。實測最大的一包(moran)解壓後 81.7MB。
  int64_t max_total_bytes = 256LL * 1024 * 1024;
  // 壓縮比上限。zip 炸彈的本質就是極高的壓縮比;文字類詞典約 3–5 倍。
  int64_t max_compression_ratio = 200;
  int64_t ratio_floor_bytes = 4 * 1024;
  size_t max_path_length = 255;
  int max_depth = 4;
};

struct ArchiveRejection {
  enum class Kind {
    kMalformed,       // 根本不是合法 zip
    kPathTraversal,   // §4.1
    kSymlink,         // §4.2
    kZipBomb,         // §4.3
    kExtension,       // §4.4
    kWindowsName,     // Win32 專屬:保留裝置名 / ADS / 不合法字元
    kEmpty,
  };
  Kind kind = Kind::kMalformed;
  std::string entry;    // 空 = 問題出在整包而不是某個 entry
  std::string detail;   // 英文(§4.11)

  std::string ToString() const;
};

struct SafeEntry {
  std::string name;
  int64_t compressed_size = 0;
  int64_t uncompressed_size = 0;
  size_t index = 0;   // 在中央目錄裡的位置,解壓時用
};

struct ArchiveReport {
  std::vector<SafeEntry> entries;
  std::vector<ArchiveRejection> rejections;
  bool IsSafe() const { return rejections.empty(); }
  int64_t TotalUncompressed() const;
};

// 只檢查、不解壓。
//
// ⚠ 一旦有任何一項不合格就**整包拒絕**,但仍會把找到的所有問題列出來 ——
//   使用者自帶的檔案通常不只錯一處,一次只報一個會讓人來回好幾趟。
ArchiveReport InspectArchive(const std::string& zip_bytes,
                             const ArchiveLimits& limits = ArchiveLimits());

// 個別檢查(公開是為了讓測試直接打它們,而不是繞一整個 zip)。
// 回傳空字串 = 沒問題。
std::string PathProblemOf(const std::string& raw_name, const ArchiveLimits& limits);
std::string ExtensionProblemOf(const std::string& name);
// Win32 專屬的那四條。見檔頭。
std::string WindowsNameProblemOf(const std::string& raw_name);

// 副檔名白名單(小寫,不含點)。
bool IsAllowedExtension(const std::string& ext_lower);
// 無副檔名但允許的檔名(區分大小寫,這些是慣例全大寫)。
bool IsAllowedBareName(const std::string& base);

}  // namespace rimewin

#endif  // RIMEWIN_ARCHIVE_GUARD_H_
