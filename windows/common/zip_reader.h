// windows/common/zip_reader.h — 讀 zip 的中央目錄,並把 entry 解出來
//
// 對照:Android 用 `java.util.zip`,macOS 用 Apple 的 Compression framework
// (`COMPRESSION_ZLIB` 就是裸 DEFLATE)。**Windows 兩者都沒有** ——
// 系統只提供 XPRESS / LZMS(`cabinet.dll` 的 Compression API),沒有 DEFLATE;
// 而整個 Windows 建置的相依只有 librime 與它的五個(glog / yaml-cpp /
// leveldb / marisa / opencc),裡面沒有 zlib。
//
// 所以這裡自己解 DEFLATE。三個理由讓這件事划算:
//
//   1. **不必新增一個相依。** 為了解壓 34 個套件把 zlib 拉進建置,
//      要動 build.sh、CMakeLists、THIRD_PARTY_NOTICES,而 CI 一輪十幾分鐘。
//   2. **它是純函式,所以在 Ubuntu 上測得完。** 解壓縮是這條線上唯一
//      直接吃「網路來的位元組」的解析器 —— 它正是最需要被單元測試、
//      被 ASan 跑過、被截斷與亂數輸入餵過的地方。走系統 API 的話,
//      這一段就只有 Windows 上的真人驗得到。
//   3. 走系統的殼層解壓(`IShellDispatch`)會**整個繞過 ArchiveGuard**:
//      檢查的是我們、解壓的是別人,兩邊看的不是同一份中繼資料。
//      那正是 ArchiveGuard 檔頭在警告的事。
//
// ── ⚠ 為什麼自己讀中央目錄(與 Android 的理由逐字相同)────────────
//
// zip 有兩份中繼資料:每個檔案前面的 local file header,以及檔案尾端的
// central directory,**兩者可以不一致**。檢查與解壓必須看同一份,
// 否則攻擊者只要讓兩份不一致就能繞過檢查。
// 本檔一律以 **central directory** 為準:entry 清單、大小、屬性都從那裡讀,
// local header 只用來算「資料從哪個位移開始」,而且會拿名稱與大小回頭核對。
//
// ⚠ 刻意不支援 ZIP64。上限是 256MB,會用到 ZIP64 的套件不是打包出了問題
//   就是有意為之,兩種都該擋。
//
// ⚠ CRC32 **會**驗,但它不是安全控制:CRC 和大小一樣寫在攻擊者控制得了的
//   欄位裡。它擋的是傳輸損壞,與 sha256 擋的是同一類東西(見 sha256.h)。
#ifndef RIMEWIN_ZIP_READER_H_
#define RIMEWIN_ZIP_READER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rimewin {

// ── DEFLATE ─────────────────────────────────────────────────────

struct InflateResult {
  bool ok = false;
  std::string error;   // 英文(§4.11)
  bool over_limit = false;  // 超過 max_out 而中止
};

// 解開一段 raw DEFLATE(zip 裡存的就是這個,沒有 zlib 的兩位元組表頭)。
//
// ⚠ max_out 是**硬牆**:輸出到達上限就立刻中止並回 over_limit。
//   宣告的解壓後大小是攻擊者可控的欄位,所以真正的牆只能是這一條。
InflateResult Inflate(const uint8_t* src, size_t n, int64_t max_out,
                      std::string* out);

// ── zip ─────────────────────────────────────────────────────────

struct ZipEntry {
  std::string name;
  uint16_t method = 0;         // 0 = stored,8 = deflate
  uint32_t crc32 = 0;
  int64_t compressed_size = 0;
  int64_t uncompressed_size = 0;
  int64_t local_offset = 0;
  uint32_t unix_mode = 0;      // external attributes 的高 16 位
  int host_os = 0;             // version made by 的高位元組

  static constexpr int kHostUnix = 3;
  static constexpr uint32_t kSIfmt = 0xF000;
  static constexpr uint32_t kSIflnk = 0xA000;
  static constexpr uint32_t kSIfdir = 0x4000;

  bool IsDirectory() const {
    if (!name.empty() && name.back() == '/') return true;
    return host_os == kHostUnix && (unix_mode & kSIfmt) == kSIfdir;
  }
  // Unix 的 S_IFLNK。非 Unix 打包的 zip 沒有這個概念,一律視為非連結。
  bool IsSymlink() const {
    return host_os == kHostUnix && (unix_mode & kSIfmt) == kSIflnk;
  }
};

// 讀中央目錄。失敗時 err 有一句英文。**絕不拋例外。**
bool ReadZipCentralDirectory(const std::string& bytes,
                             std::vector<ZipEntry>* out, std::string* err);

// 取出一個 entry 的內容。
//
// ⚠ 名稱與大小會回頭跟 local header 核對 —— 兩份中繼資料不一致時
//   直接失敗,不猜哪一份是真的。
bool ExtractZipEntry(const std::string& bytes, const ZipEntry& e,
                     int64_t max_out, std::string* out, std::string* err);

// zip 用的 CRC-32(IEEE 802.3)。測試會拿已知向量釘住它。
uint32_t Crc32(const std::string& data);

}  // namespace rimewin

#endif  // RIMEWIN_ZIP_READER_H_
