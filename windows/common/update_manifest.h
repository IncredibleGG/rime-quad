// windows/common/update_manifest.h — 線上版本資訊(Windows 自己的那一份)
//
// 對照實作:android/app/src/main/java/org/luminakey/ime/update/
//           VersionManifest.kt(格式與政策照抄)與 PackageIdentity.kt。
//
// ══ 為什麼 Windows 需要**自己一份**清單 ═══════════════════════════
//
// Android 那一份 `rime/version.json` 描述的是一個 APK:檔名、大小、
// sha256、applicationId 全部是 Android 的。桌面端下載的是
// `LuminaKey-Setup-x64-*.exe`,四個欄位沒有一個對得上。共用一份的話,
// 兩邊只要有一邊發版,另一邊就會拿到一個大小與摘要都不對的東西 ——
// 而症狀是「下載完說檔案壞了」,查起來會先怪到網路上。
//
// 所以:`rime/windows/version-windows.json`,由 scripts/publish_desktop.sh 產生。
//
// ══ ⚠ 欄位一律選用,這是硬規則 ═══════════════════════════════════
//
// Android 那條線的教訓(見 VersionManifest.kt 的檔頭):使用者手上跑的
// **舊版**讀的是**新的**清單,而新版也可能讀到一份還沒更新的舊清單。
// 把任何欄位改成必填,等於所有舊版本從此安靜地再也收不到更新,
// 畫面上寫「版本資訊格式錯誤」—— 比原本的問題更糟。
//
// 這裡的分界:**判斷得出「要不要更新」與「要下載什麼」的那五個是必填**
// (version_code / version_name / size / sha256 / url),其餘全部選用。
// 而且選用欄位**格式不對就當成沒有**,不是整份拒收 —— 一個
// `"app_id": 123` 比缺席更危險,它會讓比對得出一個看起來確定、
// 實際上沒有根據的答案。
//
// ══ Windows 的「換了一個產品」是什麼 ═════════════════════════════
//
// Android 是 applicationId;Windows 是安裝程式的 **AppId**(Inno Setup
// 的那一個 GUID,見 windows/installer/luminakey.iss 的檔頭)。它決定
// 「新增或移除程式」裡那一筆的登錄檔鍵名,也決定新版的安裝程式認不認得
// 舊版。AppId 換掉之後,新版**不會**升級舊版 —— 它會在旁邊裝出第二套,
// 兩套同時註冊 TSF,而舊那一套永遠留在「新增或移除程式」裡。
//
// 這件事在按下安裝**之前**就判定得出來,所以 app_id 不同時我們
// 不下載、也不給那顆按鈕(見 update_flow.h 的 MayHandOff)。
// 2026-08-09 產品改名時 AppId 真的換過一次,那時桌面端還沒有更新機制,
// 所以沒有人被這件事咬到 —— 下一次就會。
//
#ifndef RIMEWIN_UPDATE_MANIFEST_H_
#define RIMEWIN_UPDATE_MANIFEST_H_

#include <cstdint>
#include <string>
#include <vector>

namespace rimewin {

// 安裝程式再大也不該超過這個數。防的是「伺服器宣告一個荒謬的 size」——
// 那個值同時是下載的硬上限,所以它必須先被判斷過。
constexpr int64_t kMaxSetupBytes = 300LL * 1024 * 1024;
// 清單本身的上限。它是一份幾百位元組的 JSON。
constexpr int64_t kMaxUpdateManifestBytes = 64 * 1024;

struct WinUpdateManifest {
  // ── 必填 ──────────────────────────────────────────────────
  // 單調遞增的整數。**唯一**用來判斷新舊的欄位 ——
  // version_name 是給人看的字串,拿字串比大小遲早比出 "0.10" < "0.9"。
  int64_t version_code = 0;
  std::string version_name;
  int64_t size = 0;          // 位元組。同時是下載的硬上限
  std::string sha256;        // 小寫十六進位 64 字元
  std::string url;           // 解析完成的絕對網址

  // ── 選用 ──────────────────────────────────────────────────
  std::string commit;
  std::string file;          // 安裝程式的檔名(給人看)
  std::string notes;         // 更新說明。可能是空字串
  // 線上那一版的 Inno AppId。**選用** —— 舊清單沒有它。
  // 空字串代表「發布端沒說」,不代表「一樣」。
  std::string app_id;
  // 這一版**取代**的舊 AppId(換 AppId 時才會有;字串或字串陣列都收)。
  // 有它才分得出「這是我們自己換的」與「這份清單根本不是給這個產品的」。
  std::vector<std::string> replaces_app_ids;
  // 給人看的下載頁。沒有就退回 url。
  std::string page_url;
};

struct ManifestParseResult {
  bool ok = false;
  WinUpdateManifest manifest;
  std::string error;  // ok = false 時,一句給使用者看的話(繁中)
};

// 解析 version-windows.json。
//
// 政策:**必填欄位任何一項不對就整份拒收**。這份清單只描述一件事,
// 其中任何一項不可信,整件事就不可信 —— 而這件事的後果是在使用者的
// 機器上跑一個安裝程式。
//
// @param manifest_url 清單自己的位置,用來解析相對網址。
ManifestParseResult ParseWinUpdateManifest(const std::string& text,
                                           const std::string& manifest_url);

// ── 版本比較 ──────────────────────────────────────────────────
enum class UpdateVerdict {
  kUpdateAvailable,
  kUpToDate,
  // 刻意不併進 kUpToDate:遠端比本機舊代表發布端出了事(回退了、
  // 或清單指到舊檔)。硬給一顆「更新」按鈕只會讓他按下去,
  // 然後 Inno 用一個更舊的版本蓋掉現在這一版。
  kDowngrade,
};
UpdateVerdict CompareVersion(int64_t installed_code,
                             const WinUpdateManifest& remote);

// ── 產品身分(Windows = Inno 的 AppId)────────────────────────
enum class AppIdVerdict {
  kSame,
  // ⚠ 第一級公民,不是「大概一樣吧」的同義詞。舊清單沒有 app_id,
  //   而使用者手上跑的可能是任何一版。把「不知道」當成「一樣」會讓
  //   這條線退化成「裝完才發現多了一套」。
  kUnknown,
  kChanged,
};
AppIdVerdict CompareAppId(const std::string& installed,
                          const std::string& remote);
bool DeclaresReplacing(const std::string& installed,
                       const WinUpdateManifest& remote);

// `{8-4-4-4-12}` 的 GUID 字面(大小寫不拘)。刻意寬鬆:這裡的用途是
// **把垃圾擋在比對之外**,不是當一個 GUID 驗證器。
bool LooksLikeAppId(const std::string& s);
// 比對前的正規化:去空白、轉大寫。⚠ 只給比對用,不要拿去寫檔案。
std::string NormalizeAppId(const std::string& s);

// ── 本機這一版是誰(安裝目錄裡的 version.txt)────────────────
//
// 由 windows/make_installer.sh 產生、由安裝程式放進安裝目錄。
// 為什麼是一個檔案而不是編進 exe 的版本資源:版本號是**打包時**才算得
// 出來的(它由 commit 時間推導),而執行檔在那之前就編好了。要把它編
// 進去就得讓每一次打包都重編一次 —— 而重編出來的那一份沒有被任何一關
// 測過。一個五行的文字檔沒有這個問題,而且使用者自己打得開。
//
// 格式(逐字,不做展開;認不得的行安靜忽略,以便日後加欄位):
//   version_code=26081200
//   version_name=0.1.0+20260810-1200.abc1234
//   app_id={4D16C4D6-...}
//   commit=abc1234
struct InstalledVersion {
  int64_t version_code = 0;
  std::string version_name;
  std::string app_id;
  std::string commit;
  bool valid() const { return version_code > 0; }
};
// 讀不出 version_code → valid() 為 false。
// ⚠ 呼叫端必須把「讀不到」當成「不知道自己是哪一版」,而**不是**當成 0:
//   當成 0 的話,任何一份清單看起來都比較新,於是我們會叫使用者去裝一個
//   可能比他現在這一版還舊的東西。見 update_flow.h 的 MayHandOff。
InstalledVersion ParseInstalledVersion(const std::string& text);

}  // namespace rimewin

#endif  // RIMEWIN_UPDATE_MANIFEST_H_
