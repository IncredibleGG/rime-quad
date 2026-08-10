// windows/tests/test_update_manifest.cc — 線上版本資訊的解析
//
// 這一份 JSON 是**從網路上抓回來的**,而它的後果是在使用者機器上跑一個
// 安裝程式。所以這裡的重點不是「好的輸入解得對」,是**壞的輸入不會讓
// 我們做出有根據的樣子的錯誤決定**。

#include "../common/update_manifest.h"

#include <string>

#include "check.h"

using namespace rimewin;

namespace {

const char kUrl[] = "https://example.invalid/rime/windows/version-windows.json";

std::string Good() {
  return R"({
  "version_code": 24101430,
  "version_name": "0.1.0+20260810-1200.abc1234",
  "commit": "abc1234",
  "file": "LuminaKey-Setup-x64-20260810-1200-abc1234.exe",
  "size": 20971520,
  "sha256": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
  "url": "https://example.invalid/rime/windows/LuminaKey-Setup-x64.exe",
  "app_id": "{4D16C4D6-444A-40A7-953D-57BF873E8689}",
  "page_url": "https://example.invalid/downloads",
  "notes": "修了一個東西"
})";
}

}  // namespace

TEST(update_manifest_parses_a_good_one) {
  const ManifestParseResult r = ParseWinUpdateManifest(Good(), kUrl);
  CHECK(r.ok);
  CHECK_INT(r.manifest.version_code, 24101430);
  CHECK_INT(r.manifest.size, 20971520);
  CHECK_STR(r.manifest.sha256,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK_STR(r.manifest.url,
            "https://example.invalid/rime/windows/LuminaKey-Setup-x64.exe");
  CHECK_STR(r.manifest.app_id, "{4D16C4D6-444A-40A7-953D-57BF873E8689}");
  CHECK_STR(r.manifest.page_url, "https://example.invalid/downloads");
}

TEST(update_manifest_optional_fields_are_really_optional) {
  // ⚠ 這一條釘住的是 Android 那條線的教訓:把任何選用欄位改成必填,
  //   等於所有舊版本從此安靜地再也收不到更新。這裡只有五個必填。
  const std::string minimal = R"({
    "version_code": 2,
    "version_name": "x",
    "size": 10,
    "sha256": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    "url": "https://example.invalid/a.exe"
  })";
  const ManifestParseResult r = ParseWinUpdateManifest(minimal, kUrl);
  CHECK(r.ok);
  CHECK(r.manifest.app_id.empty());
  CHECK(r.manifest.page_url.empty());
  CHECK(r.manifest.notes.empty());
  CHECK(r.manifest.replaces_app_ids.empty());
  // 缺 app_id 一定是「不知道」,不可以是「一樣」。
  CHECK(CompareAppId("{4D16C4D6-444A-40A7-953D-57BF873E8689}",
                     r.manifest.app_id) == AppIdVerdict::kUnknown);
}

TEST(update_manifest_bad_optional_fields_are_treated_as_absent) {
  // 型別不對的選用欄位比缺席更危險:它會讓比對得出一個看起來確定、
  // 實際上沒有根據的答案。所以當成沒有,而不是照收、也不是整份拒收。
  const std::string s = R"({
    "version_code": 2, "version_name": "x", "size": 10,
    "sha256": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    "url": "https://example.invalid/a.exe",
    "app_id": 123,
    "page_url": "ftp://example.invalid/x",
    "replaces_app_id": ["not-a-guid", "{7A033CF7-CB91-408E-A653-EF639F4173DB}"]
  })";
  const ManifestParseResult r = ParseWinUpdateManifest(s, kUrl);
  CHECK(r.ok);
  CHECK(r.manifest.app_id.empty());
  CHECK(r.manifest.page_url.empty());   // ftp 不是 http/https
  CHECK_INT(static_cast<int>(r.manifest.replaces_app_ids.size()), 1);
  CHECK(DeclaresReplacing("{7a033cf7-cb91-408e-a653-ef639f4173db}", r.manifest));

  // ⚠ 上面那一份的 app_id 是**數字**,而數字在取字串時本來就回空字串 ——
  //   也就是說它走的是「缺席」那條路,沒有真的驗到「字串但格式不對」。
  //   反向測試抓到了這個缺口(把選用欄位改成整份拒收,測試竟然還是綠的)。
  //   下面這一份是字串、非空、而且不是 GUID:當成沒有,**不是**整份拒收。
  const std::string s2 = R"({
    "version_code": 2, "version_name": "x", "size": 10,
    "sha256": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    "url": "https://example.invalid/a.exe",
    "app_id": "not-a-guid"
  })";
  const ManifestParseResult r2 = ParseWinUpdateManifest(s2, kUrl);
  CHECK(r2.ok);
  CHECK(r2.manifest.app_id.empty());
}

TEST(update_manifest_required_fields_reject_the_whole_thing) {
  struct Case {
    const char* what;
    const char* json;
  };
  const Case cases[] = {
      {"沒有 version_code",
       R"({"version_name":"x","size":10,"sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","url":"https://a.invalid/x.exe"})"},
      {"version_code 是零",
       R"({"version_code":0,"version_name":"x","size":10,"sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","url":"https://a.invalid/x.exe"})"},
      {"version_code 是字串",
       R"({"version_code":"9","version_name":"x","size":10,"sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","url":"https://a.invalid/x.exe"})"},
      {"沒有 version_name",
       R"({"version_code":2,"size":10,"sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","url":"https://a.invalid/x.exe"})"},
      {"size 是負的",
       R"({"version_code":2,"version_name":"x","size":-1,"sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","url":"https://a.invalid/x.exe"})"},
      {"size 荒謬地大",
       R"({"version_code":2,"version_name":"x","size":999999999999,"sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","url":"https://a.invalid/x.exe"})"},
      {"sha256 長度不對",
       R"({"version_code":2,"version_name":"x","size":10,"sha256":"abc","url":"https://a.invalid/x.exe"})"},
      {"沒有 url 也沒有 file",
       R"({"version_code":2,"version_name":"x","size":10,"sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"})"},
      {"url 是 file://",
       R"({"version_code":2,"version_name":"x","size":10,"sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","url":"file:///C:/x.exe"})"},
      {"頂層不是物件", R"(["version_code"])"},
      {"根本不是 JSON", "<html>404</html>"},
      {"尾巴還有第二份", R"({"version_code":2,"version_name":"x","size":10,"sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","url":"https://a.invalid/x.exe"}{"version_code":9})"},
  };
  for (const Case& c : cases) {
    const ManifestParseResult r = ParseWinUpdateManifest(c.json, kUrl);
    if (r.ok) {
      ::rimewin_test::Fail(__FILE__, __LINE__,
                           std::string("這一份應該被拒收:") + c.what);
    } else {
      ++::rimewin_test::Assertions();
      // 錯誤訊息不可以是空的 —— 使用者按了「查更新」得到一片空白,
      // 與「壞掉了」是同一個畫面。
      CHECK(!r.error.empty());
    }
  }
}

TEST(update_manifest_setup_url_must_be_https) {
  // ⚠ 這一條守的是**信任錨本身**,不是一個格式規則。
  //
  //   這一端沒有程式碼簽章(見 service/net_gate.h 開頭那一段)。
  //   使用者按下「現在更新」之後會有一支安裝程式以系統管理員身分跑起來,
  //   而我們對「那真的是我們發的那一支」的全部根據,就是那條 TLS 連線。
  //   sha256 擋不住這一段:它是從同一份清單裡拿的,而清單走明文的話,
  //   換掉安裝程式與換掉它的 sha256 是同一個動作。
  {
    std::string bad = Good();
    const size_t i = bad.find("\"url\": \"https://");
    CHECK(i != std::string::npos);
    bad = bad.substr(0, i) + "\"url\": \"http://" +
          bad.substr(i + std::string("\"url\": \"https://").size());
    const ManifestParseResult r = ParseWinUpdateManifest(bad, kUrl);
    CHECK(!r.ok);
    CHECK(!r.error.empty());
  }
  // 相對網址仍然可以 —— 它會被接到清單自己的 https 主機上。
  {
    std::string rel = Good();
    const size_t i = rel.find("\"url\": \"https://example.invalid/rime/windows/"
                              "LuminaKey-Setup-x64.exe\"");
    CHECK(i != std::string::npos);
    rel.replace(i, std::string("\"url\": \"https://example.invalid/rime/windows/"
                               "LuminaKey-Setup-x64.exe\"").size(),
                "\"url\": \"LuminaKey-Setup-x64.exe\"");
    const ManifestParseResult r = ParseWinUpdateManifest(rel, kUrl);
    CHECK(r.ok);
    CHECK_STR(r.manifest.url,
              "https://example.invalid/rime/windows/LuminaKey-Setup-x64.exe");
  }
  // 下載頁是選用欄位:明文的話**當成沒有**,不整份拒收 ——
  // 它只是一個連結,而一份少了連結的清單仍然是可用的。
  {
    std::string p = Good();
    const size_t i = p.find("\"page_url\": \"https://");
    CHECK(i != std::string::npos);
    p = p.substr(0, i) + "\"page_url\": \"http://" +
        p.substr(i + std::string("\"page_url\": \"https://").size());
    const ManifestParseResult r = ParseWinUpdateManifest(p, kUrl);
    CHECK(r.ok);
    CHECK(r.manifest.page_url.empty());
  }
}

TEST(update_manifest_is_not_fooled_by_nested_objects_or_strings) {
  // ⚠ 發布端那一側踩過同一個坑(publish_apk.sh 的 --self-check B2):
  //   用貪婪的字串比對抓 version_code,會被巢狀物件裡更大的那個騙走。
  //   這裡的剖析器只認**頂層**。
  const std::string s = R"({
    "version_code": 5,
    "version_name": "x",
    "size": 10,
    "sha256": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    "url": "https://a.invalid/x.exe",
    "notes": "修好 \"version_code\": 99999999 的顯示",
    "meta": { "version_code": 88888888, "size": 999999999999 }
  })";
  const ManifestParseResult r = ParseWinUpdateManifest(s, kUrl);
  CHECK(r.ok);
  CHECK_INT(r.manifest.version_code, 5);
  CHECK_INT(r.manifest.size, 10);
  CHECK(r.manifest.notes.find("99999999") != std::string::npos);
}

TEST(update_manifest_rejects_absurd_nesting_without_blowing_up) {
  // 深度上限。沒有它的話,一份 [[[[[… 就能把遞迴下降的剖析器打爆,
  // 而那只需要對方能回應一次 HTTP。
  std::string deep(200, '[');
  const ManifestParseResult r = ParseWinUpdateManifest(deep, kUrl);
  CHECK(!r.ok);
}

TEST(update_manifest_relative_url_resolves_against_the_manifest) {
  const std::string s = R"({
    "version_code": 2, "version_name": "x", "size": 10,
    "sha256": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    "file": "LuminaKey-Setup-x64-latest.exe"
  })";
  const ManifestParseResult r = ParseWinUpdateManifest(s, kUrl);
  CHECK(r.ok);
  CHECK_STR(r.manifest.url,
            "https://example.invalid/rime/windows/"
            "LuminaKey-Setup-x64-latest.exe");
}

TEST(update_manifest_version_comparison_uses_the_integer) {
  ManifestParseResult r = ParseWinUpdateManifest(Good(), kUrl);
  CHECK(r.ok);
  CHECK(CompareVersion(24101429, r.manifest) == UpdateVerdict::kUpdateAvailable);
  CHECK(CompareVersion(24101430, r.manifest) == UpdateVerdict::kUpToDate);
  // ⚠ 遠端比較舊要分得出來。併進「已經最新」的話,發布端回退時
  //   使用者會看到一顆按下去就用舊版蓋掉自己的按鈕。
  CHECK(CompareVersion(24101431, r.manifest) == UpdateVerdict::kDowngrade);
}

TEST(update_manifest_app_id_shape_and_comparison) {
  CHECK(LooksLikeAppId("{4D16C4D6-444A-40A7-953D-57BF873E8689}"));
  CHECK(LooksLikeAppId("{4d16c4d6-444a-40a7-953d-57bf873e8689}"));
  CHECK(!LooksLikeAppId("4D16C4D6-444A-40A7-953D-57BF873E8689"));   // 少了大括號
  CHECK(!LooksLikeAppId("{4D16C4D6-444A-40A7-953D-57BF873E868}"));  // 少一位
  CHECK(!LooksLikeAppId("{ZZ16C4D6-444A-40A7-953D-57BF873E8689}"));
  CHECK(!LooksLikeAppId(""));

  const std::string mine = "{4D16C4D6-444A-40A7-953D-57BF873E8689}";
  const std::string other = "{7A033CF7-CB91-408E-A653-EF639F4173DB}";
  CHECK(CompareAppId(mine, "{4d16c4d6-444a-40a7-953d-57bf873e8689}") ==
        AppIdVerdict::kSame);
  CHECK(CompareAppId(mine, other) == AppIdVerdict::kChanged);
  CHECK(CompareAppId(mine, "") == AppIdVerdict::kUnknown);
  // ⚠ **本機不知道自己是誰也是 kUnknown。** 當成 kChanged 的話,
  //   一個讀不到 version.txt 的安裝從此再也更新不了。
  CHECK(CompareAppId("", other) == AppIdVerdict::kUnknown);
}

TEST(update_manifest_installed_version_file) {
  const InstalledVersion v = ParseInstalledVersion(
      "# LuminaKey\n"
      "version_code=24101430\n"
      "version_name=0.1.0+20260810-1200.abc1234\n"
      "app_id={4D16C4D6-444A-40A7-953D-57BF873E8689}\n"
      "commit=abc1234\n"
      "future_field=whatever\n");
  CHECK(v.valid());
  CHECK_INT(v.version_code, 24101430);
  CHECK_STR(v.app_id, "{4D16C4D6-444A-40A7-953D-57BF873E8689}");
  CHECK_STR(v.commit, "abc1234");

  // ⚠ 讀不到 = 不知道,**不是** 0。當成 0 的話任何一份清單看起來都比較新。
  CHECK(!ParseInstalledVersion("").valid());
  CHECK(!ParseInstalledVersion("version_code=abc\n").valid());
  CHECK(!ParseInstalledVersion("version_code=0\n").valid());
  // 舊的安裝目錄裡根本沒有這個檔案 —— 那條路也要走得下去。
  CHECK_INT(ParseInstalledVersion("隨便什麼東西").version_code, 0);
}
