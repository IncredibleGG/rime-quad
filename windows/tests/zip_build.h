// windows/tests/zip_build.h — 測試用的 zip 產生器
//
// 刻意自己拼位元組而不是呼叫 zip 工具:要測「中央目錄與 local header
// 不一致」「external attributes 說這是符號連結」「宣告的大小說謊」,
// 就必須做得出這樣的 zip,而任何正常的打包工具都做不出來。
//
// ⚠ 只有一份。三個測試檔共用它 —— 兩份複本會長成兩個不一樣的產生器,
//   而那正是 ui_listview.h 檔頭在講的那個事故的形狀。
#ifndef RIMEWIN_TESTS_ZIP_BUILD_H_
#define RIMEWIN_TESTS_ZIP_BUILD_H_

#include <cstdint>
#include <string>
#include <vector>

#include "../common/zip_reader.h"

namespace rimewin_test {

inline void PutU16(std::string* s, uint32_t v) {
  s->push_back(static_cast<char>(v & 0xFF));
  s->push_back(static_cast<char>((v >> 8) & 0xFF));
}

inline void PutU32(std::string* s, uint32_t v) {
  PutU16(s, v & 0xFFFF);
  PutU16(s, (v >> 16) & 0xFFFF);
}

struct BuildEntry {
  std::string name;
  std::string data;          // 已經壓好的位元組(method=0 時就是原文)
  uint16_t method = 0;
  uint32_t crc = 0;
  uint32_t uncompressed = 0;
  uint32_t unix_mode = 0;    // external attributes 的高 16 位
  uint16_t host_os = 0;      // 3 = unix
  std::string local_name_override;  // 非空 = local header 用另一個名字
};

// 一個 method=0 的 entry,大小與 CRC 都算對。
inline BuildEntry StoredEntry(const std::string& name, const std::string& text) {
  BuildEntry e;
  e.name = name;
  e.data = text;
  e.method = 0;
  e.crc = rimewin::Crc32(text);
  e.uncompressed = static_cast<uint32_t>(text.size());
  return e;
}

inline std::string BuildZip(const std::vector<BuildEntry>& entries) {
  std::string out;
  std::vector<uint32_t> offsets;
  for (const auto& e : entries) {
    offsets.push_back(static_cast<uint32_t>(out.size()));
    const std::string& lname =
        e.local_name_override.empty() ? e.name : e.local_name_override;
    PutU32(&out, 0x04034b50u);
    PutU16(&out, 20);
    PutU16(&out, 0);
    PutU16(&out, e.method);
    PutU16(&out, 0);
    PutU16(&out, 0);
    PutU32(&out, e.crc);
    PutU32(&out, static_cast<uint32_t>(e.data.size()));
    PutU32(&out, e.uncompressed);
    PutU16(&out, static_cast<uint32_t>(lname.size()));
    PutU16(&out, 0);
    out += lname;
    out += e.data;
  }
  const uint32_t cd_offset = static_cast<uint32_t>(out.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    const BuildEntry& e = entries[i];
    PutU32(&out, 0x02014b50u);
    PutU16(&out, (static_cast<uint32_t>(e.host_os) << 8) | 20u);
    PutU16(&out, 20);
    PutU16(&out, 0);
    PutU16(&out, e.method);
    PutU16(&out, 0);
    PutU16(&out, 0);
    PutU32(&out, e.crc);
    PutU32(&out, static_cast<uint32_t>(e.data.size()));
    PutU32(&out, e.uncompressed);
    PutU16(&out, static_cast<uint32_t>(e.name.size()));
    PutU16(&out, 0);
    PutU16(&out, 0);
    PutU16(&out, 0);
    PutU16(&out, 0);
    PutU32(&out, e.unix_mode << 16);
    PutU32(&out, offsets[i]);
    out += e.name;
  }
  const uint32_t cd_size = static_cast<uint32_t>(out.size()) - cd_offset;
  PutU32(&out, 0x06054b50u);
  PutU16(&out, 0);
  PutU16(&out, 0);
  PutU16(&out, static_cast<uint32_t>(entries.size()));
  PutU16(&out, static_cast<uint32_t>(entries.size()));
  PutU32(&out, cd_size);
  PutU32(&out, cd_offset);
  PutU16(&out, 0);
  return out;
}

}  // namespace rimewin_test

#endif  // RIMEWIN_TESTS_ZIP_BUILD_H_
