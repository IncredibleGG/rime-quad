#include "archive_guard.h"

#include <algorithm>

namespace rimewin {
namespace {

const char* const kAllowedExt[] = {
    "yaml", "yml",   // schema / dict / 配置
    "txt",           // essay.txt、UPSTREAM.txt
    "ocd2",          // opencc 詞典
    "gram",          // octagram 語言模型
    "json",          // opencc 設定
    "md",            // 說明文件
    // lua:Windows 這一輪沒有編 librime-lua,所以這些檔案落在磁碟上
    // 沒有任何東西會載入。擋在「啟用方案」那一關(schema_preflight.h)
    // 比擋在這裡準確 —— 見本檔標頭最後一段。
    "lua",
};

const char* const kAllowedBare[] = {
    "LICENSE", "LICENCE", "COPYING", "NOTICE", "README", "AUTHORS", "CHANGELOG",
};

// Win32 的保留裝置名。**比對的是第一個點之前的部分**,而且不分大小寫。
const char* const kReserved[] = {
    "CON", "PRN", "AUX", "NUL",
    "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
    "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
};

std::string Upper(const std::string& s) {
  std::string o = s;
  for (char& c : o) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return o;
}

std::string Lower(const std::string& s) {
  std::string o = s;
  for (char& c : o) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return o;
}

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    const size_t p = s.find(sep, start);
    if (p == std::string::npos) {
      out.push_back(s.substr(start));
      return out;
    }
    out.push_back(s.substr(start, p - start));
    start = p + 1;
  }
}

}  // namespace

std::string ArchiveRejection::ToString() const {
  const char* k = "?";
  switch (kind) {
    case Kind::kMalformed: k = "MALFORMED"; break;
    case Kind::kPathTraversal: k = "PATH_TRAVERSAL"; break;
    case Kind::kSymlink: k = "SYMLINK"; break;
    case Kind::kZipBomb: k = "ZIP_BOMB"; break;
    case Kind::kExtension: k = "EXTENSION"; break;
    case Kind::kWindowsName: k = "WINDOWS_NAME"; break;
    case Kind::kEmpty: k = "EMPTY"; break;
  }
  std::string s = std::string("[") + k + "] ";
  if (!entry.empty()) s += entry + ": ";
  return s + detail;
}

int64_t ArchiveReport::TotalUncompressed() const {
  int64_t n = 0;
  for (const auto& e : entries) n += e.uncompressed_size;
  return n;
}

bool IsAllowedExtension(const std::string& ext_lower) {
  for (const char* e : kAllowedExt) {
    if (ext_lower == e) return true;
  }
  return false;
}

bool IsAllowedBareName(const std::string& base) {
  for (const char* e : kAllowedBare) {
    if (base == e) return true;
  }
  return false;
}

std::string PathProblemOf(const std::string& name, const ArchiveLimits& limits) {
  if (name.empty()) return "entry name is empty";
  if (name.size() > limits.max_path_length) {
    return "path length " + std::to_string(name.size()) +
           " exceeds the limit of " + std::to_string(limits.max_path_length);
  }
  for (char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) return "path contains control characters";
  }
  // zip 規格說名稱一律用 '/'。出現 '\' 在 Win32 上就是**另一層目錄**,
  // 也就是繞過下面那些以 '/' 為準的檢查最直接的辦法。
  if (name.find('\\') != std::string::npos) return "path contains a backslash";
  if (name[0] == '/') return "path starts with / (absolute)";
  if (name.size() >= 2 && name[1] == ':' &&
      ((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z'))) {
    return "path contains a drive letter (absolute)";
  }

  const std::vector<std::string> segments = Split(name, '/');
  for (size_t i = 0; i < segments.size(); ++i) {
    const std::string& seg = segments[i];
    if (seg == "..") return "path contains a .. segment";
    if (seg == ".") return "path contains a . segment";
    // 尾端的空段是目錄標記("foo/"),合法;其餘位置的空段代表 "//"。
    if (seg.empty() && i + 1 != segments.size()) {
      return "path contains consecutive slashes";
    }
    if (!seg.empty() && (seg.back() == ' ' || seg.back() == '.')) {
      return "path segment '" + seg + "' ends with a space or a dot";
    }
  }
  int depth = 0;
  for (const auto& seg : segments) {
    if (!seg.empty()) ++depth;
  }
  if (depth > limits.max_depth) {
    return "directory depth " + std::to_string(depth) +
           " exceeds the limit of " + std::to_string(limits.max_depth);
  }
  return "";
}

std::string WindowsNameProblemOf(const std::string& name) {
  const std::vector<std::string> segments = Split(name, '/');
  for (const std::string& seg : segments) {
    if (seg.empty()) continue;
    for (char c : seg) {
      // ⚠ '/' 不在這裡:它是分隔符,上面已經拆過了。
      if (c == ':' ) {
        return "segment '" + seg +
               "' contains a colon (NTFS alternate data stream)";
      }
      if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') {
        return std::string("segment '") + seg + "' contains '" + c +
               "', which is not a legal Win32 file name character";
      }
    }
    // 保留裝置名:比對第一個點之前的部分。`con.yaml` 一樣會開到 CON。
    const size_t dot = seg.find('.');
    const std::string stem = Upper(dot == std::string::npos ? seg : seg.substr(0, dot));
    for (const char* r : kReserved) {
      if (stem == r) {
        return "segment '" + seg + "' is the reserved Win32 device name " + r;
      }
    }
  }
  return "";
}

std::string ExtensionProblemOf(const std::string& name) {
  const size_t slash = name.find_last_of('/');
  const std::string base =
      slash == std::string::npos ? name : name.substr(slash + 1);
  if (base.empty()) return "file name is empty";
  if (base[0] == '.') {
    return "hidden files (names starting with a dot) are not accepted";
  }
  const size_t dot = base.find_last_of('.');
  if (dot == std::string::npos) {
    if (IsAllowedBareName(base)) return "";
    return "no extension and not in the allowed bare names";
  }
  const std::string ext = Lower(base.substr(dot + 1));
  if (IsAllowedExtension(ext)) return "";
  return "extension ." + ext + " is not on the allowlist";
}

ArchiveReport InspectArchive(const std::string& zip_bytes,
                             const ArchiveLimits& limits) {
  ArchiveReport report;
  std::vector<ZipEntry> entries;
  std::string err;
  if (!ReadZipCentralDirectory(zip_bytes, &entries, &err)) {
    report.rejections.push_back(
        {ArchiveRejection::Kind::kMalformed, "", err});
    return report;
  }

  if (static_cast<int>(entries.size()) > limits.max_entries) {
    report.rejections.push_back(
        {ArchiveRejection::Kind::kZipBomb, "",
         "entry count " + std::to_string(entries.size()) +
             " exceeds the limit of " + std::to_string(limits.max_entries)});
  }

  int64_t total = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    const ZipEntry& e = entries[i];

    // ── §4.1 路徑穿越 ────────────────────────────────────────
    const std::string path_problem = PathProblemOf(e.name, limits);
    if (!path_problem.empty()) {
      report.rejections.push_back(
          {ArchiveRejection::Kind::kPathTraversal, e.name, path_problem});
      continue;
    }

    // ── Win32 專屬(見標頭)──────────────────────────────────
    const std::string win_problem = WindowsNameProblemOf(e.name);
    if (!win_problem.empty()) {
      report.rejections.push_back(
          {ArchiveRejection::Kind::kWindowsName, e.name, win_problem});
      continue;
    }

    // ── §4.2 符號連結 ────────────────────────────────────────
    if (e.IsSymlink()) {
      report.rejections.push_back(
          {ArchiveRejection::Kind::kSymlink, e.name,
           "the unix mode in the external attributes is S_IFLNK"});
      continue;
    }

    if (e.IsDirectory()) continue;

    // ── §4.4 副檔名白名單 ────────────────────────────────────
    const std::string ext_problem = ExtensionProblemOf(e.name);
    if (!ext_problem.empty()) {
      report.rejections.push_back(
          {ArchiveRejection::Kind::kExtension, e.name, ext_problem});
      continue;
    }

    // ── §4.3 解壓炸彈(宣告值這一層)────────────────────────
    if (e.uncompressed_size > limits.max_entry_bytes) {
      report.rejections.push_back(
          {ArchiveRejection::Kind::kZipBomb, e.name,
           "declared uncompressed size " + std::to_string(e.uncompressed_size) +
               " exceeds the per-entry limit of " +
               std::to_string(limits.max_entry_bytes)});
      continue;
    }
    if (e.compressed_size >= limits.ratio_floor_bytes && e.compressed_size > 0) {
      const int64_t ratio = e.uncompressed_size / e.compressed_size;
      if (ratio > limits.max_compression_ratio) {
        report.rejections.push_back(
            {ArchiveRejection::Kind::kZipBomb, e.name,
             "compression ratio " + std::to_string(ratio) +
                 ":1 exceeds the limit of " +
                 std::to_string(limits.max_compression_ratio) + ":1"});
        continue;
      }
    }

    total += e.uncompressed_size;
    if (total > limits.max_total_bytes) {
      report.rejections.push_back(
          {ArchiveRejection::Kind::kZipBomb, "",
           "declared uncompressed total exceeds the limit of " +
               std::to_string(limits.max_total_bytes)});
      break;
    }

    SafeEntry se;
    se.name = e.name;
    se.compressed_size = e.compressed_size;
    se.uncompressed_size = e.uncompressed_size;
    se.index = i;
    report.entries.push_back(std::move(se));
  }

  if (report.rejections.empty() && report.entries.empty()) {
    report.rejections.push_back(
        {ArchiveRejection::Kind::kEmpty, "", "no entry passed the checks"});
  }
  return report;
}

}  // namespace rimewin
