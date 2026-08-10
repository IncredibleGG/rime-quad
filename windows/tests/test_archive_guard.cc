// windows/tests/test_archive_guard.cc — 解壓前的安全檢查
//
// ⚠ 這裡的每一條都對應一個**真的攻擊形狀**,不是排版偏好。
//   Win32 的那四條(保留裝置名、ADS、尾端點空白、非法字元)在 Android
//   端不存在,所以它們沒有一份可以抄的測試 —— 它們是這一輪自己長出來的。

#include <cstdint>
#include <string>
#include <vector>

#include "../common/archive_guard.h"
#include "check.h"
#include "zip_build.h"

using namespace rimewin;

namespace {

ArchiveLimits Limits() { return ArchiveLimits(); }

bool PathOk(const std::string& n) { return PathProblemOf(n, Limits()).empty(); }
bool WinOk(const std::string& n) { return WindowsNameProblemOf(n).empty(); }
bool ExtOk(const std::string& n) { return ExtensionProblemOf(n).empty(); }

}  // namespace

TEST(Archive_path_traversal_is_blocked) {
  CHECK(PathOk("luna_pinyin.schema.yaml"));
  CHECK(PathOk("opencc/t2s.json"));
  CHECK(PathOk("lua/moran/utils.lua"));

  CHECK(!PathOk("../evil.yaml"));
  CHECK(!PathOk("a/../../evil.yaml"));
  CHECK(!PathOk("/etc/passwd"));
  CHECK(!PathOk("C:\\Windows\\System32\\x.yaml"));
  CHECK(!PathOk("C:/Windows/x.yaml"));
  CHECK(!PathOk("a//b.yaml"));
  CHECK(!PathOk("./a.yaml"));
  CHECK(!PathOk(""));
  CHECK(!PathOk(std::string(300, 'a') + ".yaml"));
  CHECK(!PathOk("a/b/c/d/e/f.yaml"));  // 深度
  CHECK(!PathOk(std::string("a\x01") + "b.yaml"));
}

TEST(Archive_backslash_is_a_separator_on_windows_so_it_is_blocked) {
  // ⚠ 這一條在 Windows 上比在 Android 上嚴重:`a\..\..\evil.yaml` 的
  //   反斜線在 Win32 上**真的是分隔符**,而以 '/' 為準的分段檢查看不到它。
  CHECK(!PathOk("a\\b.yaml"));
  CHECK(!PathOk("a\\..\\..\\evil.yaml"));
}

TEST(Archive_windows_reserved_device_names_are_blocked) {
  // 在 Win32 上開啟這些名字會開到裝置,而 CreateFile 不會失敗。
  CHECK(!WinOk("CON"));
  CHECK(!WinOk("con.yaml"));       // 含副檔名一樣是 CON
  CHECK(!WinOk("aux.yaml"));
  CHECK(!WinOk("NUL.dict.yaml"));
  CHECK(!WinOk("COM1.yaml"));
  CHECK(!WinOk("lpt9.txt"));
  CHECK(!WinOk("dir/PRN.yaml"));   // 任何一段都算
  // 不是保留名的相似字串要放行,否則會誤殺合法套件。
  CHECK(WinOk("console.yaml"));
  CHECK(WinOk("com10.yaml"));
  CHECK(WinOk("auxiliary.yaml"));
  CHECK(WinOk("luna_pinyin.schema.yaml"));
}

TEST(Archive_alternate_data_stream_and_illegal_chars_are_blocked) {
  // readme.txt:evil 寫出來的是一個檔案總管看不到的資料流。
  CHECK(!WinOk("readme.txt:evil"));
  CHECK(!WinOk("a/b.yaml:stream"));
  CHECK(!WinOk("wild*.yaml"));
  CHECK(!WinOk("what?.yaml"));
  CHECK(!WinOk("a<b.yaml"));
  CHECK(!WinOk("a|b.yaml"));
  CHECK(!WinOk("a\"b.yaml"));
}

TEST(Archive_trailing_dot_or_space_is_blocked) {
  // Win32 建檔時會**安靜地**去掉它們 —— 於是兩個不同的 entry 名字
  // 會落在同一個檔案上,後面那個蓋掉前面那個。
  CHECK(!PathOk("a.yaml "));
  CHECK(!PathOk("a.yaml."));
  CHECK(!PathOk("dir /a.yaml"));
}

TEST(Archive_extension_allowlist) {
  CHECK(ExtOk("a.yaml"));
  CHECK(ExtOk("a.yml"));
  CHECK(ExtOk("essay.txt"));
  CHECK(ExtOk("t2s.json"));
  CHECK(ExtOk("x.ocd2"));
  CHECK(ExtOk("zh.gram"));
  CHECK(ExtOk("README.md"));
  CHECK(ExtOk("LICENSE"));       // 無副檔名的白名單
  CHECK(ExtOk("rime.lua"));      // 見標頭:擋在「啟用」那一關,不是這裡

  CHECK(!ExtOk("a.exe"));
  CHECK(!ExtOk("a.dll"));
  CHECK(!ExtOk("a.bat"));
  CHECK(!ExtOk("a.ps1"));
  // .bin 刻意不在白名單:.table.bin / .prism.bin 是 librime 部署時
  // 自己產生的,而且是最適合藏二進位酬載的位置。
  CHECK(!ExtOk("luna_pinyin.table.bin"));
  CHECK(!ExtOk("noextension"));
  CHECK(!ExtOk(".hidden"));
  // 大小寫不該是繞過的辦法。
  CHECK(!ExtOk("a.EXE"));
  CHECK(ExtOk("A.YAML"));
}

/* ── 整包檢查 ───────────────────────────────────────────────── */

namespace {

// 把共用的產生器包成「這個測試需要的形狀」:預設大小算對,
// 呼叫端只在要說謊的時候才覆寫。
rimewin_test::BuildEntry Ent(const std::string& name, const std::string& data,
                             uint32_t unix_mode = 0, uint16_t host_os = 0,
                             int64_t declared = -1) {
  rimewin_test::BuildEntry e = rimewin_test::StoredEntry(name, data);
  e.unix_mode = unix_mode;
  e.host_os = host_os;
  if (declared >= 0) e.uncompressed = static_cast<uint32_t>(declared);
  return e;
}

std::string Zip(const std::vector<rimewin_test::BuildEntry>& es) {
  return rimewin_test::BuildZip(es);
}

bool HasKind(const ArchiveReport& r, ArchiveRejection::Kind k) {
  for (const auto& x : r.rejections) {
    if (x.kind == k) return true;
  }
  return false;
}

}  // namespace

TEST(Archive_inspect_accepts_a_plain_package) {
  const ArchiveReport r = InspectArchive(Zip({
      Ent("luna_pinyin.schema.yaml", "schema:\n  schema_id: luna_pinyin\n"),
      Ent("luna_pinyin.dict.yaml", "---\nname: luna_pinyin\n"),
      Ent("README.md", "hi"),
  }));
  CHECK(r.IsSafe());
  CHECK_INT(static_cast<int>(r.entries.size()), 3);
}

TEST(Archive_inspect_rejects_symlinks) {
  // S_IFLNK = 0xA000。Android 端必須自己剖 external attributes 才看得到
  // 這個位元,我們也一樣。
  const ArchiveReport r = InspectArchive(Zip({
      Ent("ok.yaml", "x"),
      Ent("link.yaml", "/etc/passwd", 0xA1FFu, ZipEntry::kHostUnix),
  }));
  CHECK(!r.IsSafe());
  CHECK(HasKind(r, ArchiveRejection::Kind::kSymlink));
}

TEST(Archive_inspect_reports_every_problem_not_just_the_first) {
  // 使用者自帶的檔案通常不只錯一處,一次只報一個會讓人來回好幾趟。
  const ArchiveReport r = InspectArchive(Zip({
      Ent("../evil.yaml", "x"),
      Ent("payload.exe", "MZ"),
      Ent("CON.yaml", "x"),
  }));
  CHECK(!r.IsSafe());
  CHECK_INT(static_cast<int>(r.rejections.size()), 3);
  CHECK(HasKind(r, ArchiveRejection::Kind::kPathTraversal));
  CHECK(HasKind(r, ArchiveRejection::Kind::kExtension));
  CHECK(HasKind(r, ArchiveRejection::Kind::kWindowsName));
}

TEST(Archive_inspect_catches_a_lying_declared_size) {
  // 宣告 500MB。這一層只擋明目張膽的;真正的牆是解壓時邊數邊擋
  // (見 test_zip_reader.cc 的 Inflate_output_cap_is_a_hard_wall)。
  ArchiveLimits lim;
  const ArchiveReport r = InspectArchive(
      Zip({Ent("big.yaml", "x", 0, 0, 500LL * 1024 * 1024)}), lim);
  CHECK(!r.IsSafe());
  CHECK(HasKind(r, ArchiveRejection::Kind::kZipBomb));
}

TEST(Archive_inspect_catches_an_absurd_compression_ratio) {
  ArchiveLimits lim;
  lim.ratio_floor_bytes = 4;
  // 壓縮前 8 位元組、宣告解壓後 100000 → 12500:1
  const ArchiveReport r =
      InspectArchive(Zip({Ent("bomb.yaml", "12345678", 0, 0, 100000)}), lim);
  CHECK(!r.IsSafe());
  CHECK(HasKind(r, ArchiveRejection::Kind::kZipBomb));
}

TEST(Archive_inspect_rejects_an_empty_archive) {
  const ArchiveReport r = InspectArchive(Zip({}));
  CHECK(!r.IsSafe());
  CHECK(HasKind(r, ArchiveRejection::Kind::kEmpty));
}

TEST(Archive_inspect_rejects_garbage) {
  const ArchiveReport r = InspectArchive("not a zip at all");
  CHECK(!r.IsSafe());
  CHECK(HasKind(r, ArchiveRejection::Kind::kMalformed));
}

TEST(Archive_directory_entries_are_skipped_not_rejected) {
  const ArchiveReport r = InspectArchive(Zip({
      Ent("lua/", ""),
      Ent("lua/x.lua", "return 1"),
  }));
  CHECK(r.IsSafe());
  CHECK_INT(static_cast<int>(r.entries.size()), 1);
}
