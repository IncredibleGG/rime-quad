// windows/common/ui_strings.h — 使用者看得到的每一個字(§12.9)
//
// ── ⚠ 這是 `windows/` 底下**唯一**允許出現使用者可見字串的地方 ────
//
// W7 在守這件事:除了 ui_strings.cc,任何檔案都不得含有帶中日韓字元的
// 寬字串字面值,命中數必須是 0。而且它同時斷言**掃到的原始檔數 ≥ 20**
// —— 掃描範圍寫錯的話,0 命中不再算通過。這個專案的產品識別碼守門腳本
// 正是在「範圍不含那六項的目錄」的情況下 6/6 全綠的。
//
// ── 為什麼不是 .rc 的 STRINGTABLE ──────────────────────────────────
//
// 1. **型別強制。** 三個語系各一個 `const wchar_t* const [kUiStringCount]`,
//    配上 static_assert —— 少一條字串**編不過**。比任何掃描腳本都早、都硬。
// 2. **介面語言是使用者設定**(`advanced.language`),不是執行緒的 UI 語言。
//    `LoadStringW` 只認執行緒 UI 語言;要指定語系得走 FindResourceExW 再
//    自己剖 16 條一組的 Pascal 字串塊 —— 為了一件本來很簡單的事寫一段
//    很容易錯的碼。
// 3. **建置相依。** .rc 會把資源編譯器拉進建置。它遲早要來(系統匣圖示),
//    但字串不該等它。
//
// ── ⚠ 為什麼在 common/ 而不是 service/(與 §12.9.1 的建議不同)────
//
// 規格建議放 `windows/service/ui_strings.cc`。改放 common/ 有三個理由,
// 而第一個是規格寫的時候沒有考慮到的:
//
//   · **使用者看得到的字不只在服務進程裡。** 語言列的按鈕(tsf/lang_bar.cc,
//     住在**每一個宿主進程**裡)與提權政策的說明(common/elevation_policy.cc)
//     也是使用者讀得到的字。放 service/ 的話,W7 對那兩個檔案就只能列例外,
//     而「允許清單」正是這份規範自己警告過的東西(§2-G3)。
//   · common/ 不 include windows.h,所以**條目數與禁用字掃描可以是真的
//     單元測試**,在 Ubuntu 上跑 —— 而不是只有一支 grep 腳本。
//   · rime_common 已經被 DLL、服務、安裝工具三邊連結,一份就夠。
//     多出來的成本是幾 KB 的唯讀資料,不是一個新的相依。
//
// ── 兩條字面規則(§12.9.3)────────────────────────────────────────
//
// 1. **`中`／`En`／`简`／`繁` 不進本檔。** §8.12 規定這四個字面是規範性、
//    四端一致的。進了 catalog 就會有人把簡體語系的「简」翻成「簡」,
//    而那正是規範要避免的事。它們直接寫在狀態列的繪製碼裡,由 W10 兩頭驗。
// 2. **診斷字串永遠英文,不進本檔**(§4.11)。旁邊那句「這些數字是給我們
//    看的,你不用懂」**要**進來 —— 它是介面文字。
//
#ifndef RIMEWIN_UI_STRINGS_H_
#define RIMEWIN_UI_STRINGS_H_

#include <cstdint>
#include <string>

namespace rimewin {

enum class UiLang { kEnUs = 0, kZhHant, kZhHans, kLangCount };

enum class UiString : int {
  // ── 視窗與側欄 ──────────────────────────────────────────────
  kWindowTitle = 0,
  kNavSchemas,
  kNavAppearance,
  kNavText,
  kNavAdvanced,
  kNavStatusReady,
  // ⚠ 「還在準備」「準備失敗」「沒有在跑」是**三件不同的事**,
  //   所以是三條字串。合併回一條就是 common/service_state.h
  //   檔頭那個缺陷本身。
  kNavStatusPreparing,
  kNavStatusPrepareFailed,
  kNavStatusNotRunning,
  kNavStatusOffline,
  kClose,

  // ── 輸入方案 ────────────────────────────────────────────────
  kSchemasTitle,
  kSchemasSubtitle,
  kSchemasListHeading,
  kSchemasListBlurb,
  kSchemasDefaultBadge,
  kSchemasCurrentDefaultPrefix,
  kSchemasMoveUp,
  kSchemasMoveDown,
  kSchemasApplyOrder,
  kSchemasFollowTitle,
  kSchemasFollowBlurb,
  kSchemasEmptyTitle,
  kSchemasEmptyWhy,
  kSchemasEmptyNext,
  kSchemasOrderChangedHint,

  // ── 外觀 ────────────────────────────────────────────────────
  kAppearanceTitle,
  kAppearanceSubtitle,
  kCountHeading,
  kCountBlurb,
  kScaleHeading,
  kScaleBlurb,
  kThemeHeading,
  kThemeBlurb,
  kThemeFollowSystem,
  kThemeLight,
  kThemeDark,
  kStatusBarHeading,
  kStatusBarBlurb,
  kStatusBarShow,
  kAppearanceHonestNote,

  // ── 文字 ────────────────────────────────────────────────────
  kTextTitle,
  kTextSubtitle,
  kVariantHeading,
  kVariantBlurb,
  kVariantFollow,
  kVariantTraditional,
  kVariantSimplified,
  kPunctHeading,
  kPunctBlurb,
  kPunctFollow,
  kPunctChinese,
  kPunctEnglish,
  kTextHonestNote,
  kTextHonestAction,

  // ── 進階 ────────────────────────────────────────────────────
  kAdvancedTitle,
  kAdvancedSubtitle,
  kRedeployHeading,
  kRedeployBlurb,
  kRedeployButton,
  kFilesHeading,
  kFilesBlurb,
  kOpenUserDir,
  kOpenSettingsFile,
  kLanguageHeading,
  kLanguageBlurb,
  kLanguageSystem,
  kLanguageEnglish,
  kLanguageHant,
  kLanguageHans,
  kDiagnosticsHeading,
  kDiagnosticsNote,
  kDiagnosticsCopy,
  kDiagnosticsCopied,
  kResetHeading,
  kResetBlurb,
  kResetButton,
  kResetConfirmBody,
  kStatusResetDone,

  // ── 共用的值 ────────────────────────────────────────────────
  kValueFollowSchema,   // 「不干預」
  kCountThree,
  kCountFive,
  kCountSeven,
  kCountNine,
  kScaleSmall,
  kScaleNormal,
  kScaleLarge,
  kScaleHuge,

  // ── 確認對話框(§2-C3:確認鍵要寫出它會做什麼)──────────────
  kCancel,
  kQuitServiceTitle,
  kQuitServiceBody,
  kQuitServiceConfirm,
  kApplyCountTitle,
  kApplyCountBody,
  kApplyCountConfirm,
  kApplyCountLater,

  // ── 狀態列(視窗底部那一行)──────────────────────────────────
  kStatusApplied,
  kStatusSaveFailed,
  kStatusOrderNotApplied,
  kStatusRedeployRunning,   // 帶一個 %u 秒數
  kStatusRedeployDone,      // 帶 %s 原因 + %.1f 秒
  kStatusRedeployFailed,
  kStatusFollowOn,
  kStatusFollowOff,
  kStatusPunctFollow,
  kStatusDefaultCleared,
  kStatusScaleApplied,

  // ── 重新整理字詞的失敗訊息 ──────────────────────────────────
  kRedeployFailTitle,
  kRedeployFailNoReason,
  kRedeployFailRolledBack,
  kRedeployFailRollbackFailed,
  kRedeployRefused,
  kOrderPatchUnreadable,
  kOrderPatchInvalid,
  kOrderPatchWriteFailed,
  kOrderPatchMissing,

  // ── 系統匣 ──────────────────────────────────────────────────
  kTraySettings,
  kTrayRedeploy,
  kTrayQuit,
  kTrayTip,

  // ── 語言列(住在每一個宿主進程裡的那顆按鈕)────────────────
  kLangBarButton,
  kLangBarTooltip,
  kLangBarNotRunning,

  // ── 懸浮狀態列 ──────────────────────────────────────────────
  kBarSettings,
  kBarPreparing,
  kBarPrepareFailed,
  kBarNotRunning,
  kBarPickSchema,

  // ── 提權政策(common/elevation_policy.cc)──────────────────────
  kElevatedBlocked,
  kNotUserBlocked,
  kUnknownTokenBlocked,

  // ── 關於 ────────────────────────────────────────────────────
  kAboutOffline,

  // ── 連網(側欄第四頁)──────────────────────────────────────
  //
  // ⚠ 用詞與 Android 對齊:那一端叫「連網」(network_switch_title),
  //   所以這一端也叫「連網」—— 不可以一邊叫「離線模式」一邊叫
  //   「允許連網」,那會讓兩台裝置上的同一件事看起來像兩件事。
  kNavNetwork,
  kNetworkTitle,
  kNetworkSubtitle,
  kNetworkSwitch,
  // ⚠ 開與關**必須是兩條不同的字串**。合併成一條的話,使用者看不出
  //   現在到底是哪一種狀態,而這一頁存在的理由就是讓他看得出來。
  kNetworkOnSummary,
  kNetworkOffSummary,
  kNetworkOnDetail,
  kNetworkTurnedOn,
  kNetworkTurnedOff,

  // 連網紀錄
  kNetLogHeading,
  kNetLogBlurb,
  kNetLogColumns,
  kNetLogCount,
  // ⚠ 空狀態要**說話**,不可以是一片空白 —— 空白讓人分不出
  //   「沒連過」與「壞掉了」。
  kNetLogEmptyTitle,
  kNetLogEmptyWhy,
  kNetLogFileLine,
  kNetLogClearHeading,
  kNetLogClearBlurb,
  kNetLogClearButton,
  kNetLogCleared,
  // ⚠ 紀錄的用途與結果在畫面上一律走這幾條,**不走**
  //   net_policy.cc 的 NetPurposeText()/NetOutcomeText() —— 那兩支
  //   回的是窄字串的中文,只有一種語言,是給紀錄檔與診斷用的。
  kNetPurposeIndex,
  kNetPurposePackage,
  kNetOutcomeOk,
  kNetOutcomeFailed,
  kNetOutcomeRedirected,

  // ── 更新(住在「連網」那一頁上)──────────────────────────────────────────
  //
  // ⚠ kUpdateTrustAnchor 是**誠實那一句**:沒有程式碼簽章,TLS 是唯一的
  //   信任錨,sha256 擋的是傳輸損壞而不是惡意替換。它必須出現在使用者
  //   看得到的地方,不是只寫在註解裡 —— common/update_flow.cc 的
  //   DescribeUpdateCard 每一條路徑都帶著它,並由測試釘住。
  //
  // ⚠ kUpdateErr* 每一條都是**不同的一句話**。這一整組存在的理由就是
  //   「三種不同的失敗在畫面上是同一句紅字」那個缺陷 ——
  //   test_update_flow.cc 逐對比對,兩格指到同一條就會紅。
  kUpdateHeading,
  kUpdateBlurb,
  kUpdateTrustAnchor,
  kUpdateWhatHappens,
  kUpdateCheckButton,
  kUpdateInstallButton,
  kUpdateInstallNowButton,
  kUpdateOpenPageButton,
  kUpdateStatusIdle,
  kUpdateStatusChecking,
  kUpdateStatusUpToDate,
  kUpdateStatusAvailable,
  kUpdateStatusDowngrade,
  kUpdateStatusDownloading,
  kUpdateStatusVerifying,
  kUpdateStatusReady,
  kUpdateStatusHandedOff,
  kUpdateStatusInstalled,
  kUpdateErrSwitchOff,
  kUpdateErrUnreachable,
  kUpdateErrUnreadable,
  kUpdateErrOwnVersionUnknown,
  kUpdateErrDownloadInterrupted,
  kUpdateErrTooLarge,
  kUpdateErrSha256,
  kUpdateErrStagingWrite,
  kUpdateErrProductChanged,
  kUpdateErrBusy,
  kUpdateErrElevationDeclined,
  kUpdateErrHandoffFailed,
  kUpdateErrFileLocked,
  kUpdateErrNotInstalled,

  kUiStringCount,
};

// 目前的介面語言。預設 kZhHant(與現況一致)。
UiLang CurrentUiLang();
void SetUiLang(UiLang lang);

// `advanced.language` 的值 → 語系。`system` 或不認得的值 → 依 langid 推。
// ⚠ langid 是 TSF 語言設定檔的 id,不是系統地區設定 —— 使用者可能在
//   英文系統上用繁體輸入法。0 = 不知道 → 回 kZhHant(隨附方案的主語言)。
UiLang ResolveUiLang(const std::string& pref, uint32_t langid);
// 反向:語系 → 設定檔要存的值。
const char* UiLangPrefValue(UiLang lang);

// 取一條字串。**索引一定合法** —— 越界回空字串而不是崩潰,
// 因為這支會在繪製路徑上被呼叫,而繪製路徑不可以是崩潰來源。
const wchar_t* UiText(UiString s);
const wchar_t* UiTextIn(UiLang lang, UiString s);

// 條目數。掃描腳本與測試用。
int UiStringCount();

}  // namespace rimewin

#endif  // RIMEWIN_UI_STRINGS_H_
