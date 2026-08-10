// windows/service/update_service.h — 線上更新的 Windows 那一半
//
// 判斷全部在 common/update_manifest.h 與 common/update_flow.h(在 Ubuntu 上
// 有單元測試)。這裡只做四件 Windows 才做得到的事:
//
//   1. 讀安裝目錄裡的 version.txt(我是哪一版)
//   2. 經由**唯一的連網出口** NetGate 取清單、下載安裝程式
//   3. 對下載回來的檔案算 sha256
//   4. 把控制權交給安裝程式(ShellExecuteEx),並留下一張交棒單
//
// ══ ⚠ 這裡**沒有**第二個連網出口 ═══════════════════════════════
//
// 一個位元組都不會繞過 service/net_gate.cc:本檔不 include winhttp.h、
// 不出現任何 WinHttp*,windows/audit_offline_win.sh 在 CI 上守著這件事
// (它的允許清單裡恰好一個路徑,而且是路徑錨定的)。
//
// ══ ⚠ 提權:我們不提權,安裝程式自己提 ═══════════════════════════
//
// common/elevation_policy.h 講的是「不可以從提權的進程生出服務」——
// 生出來的服務會繼承提權的權杖,然後用系統管理員的身分去寫使用者的檔案,
// 檔案的擁有者從此換成不對的人。這裡遵守它的方式是:
//
//   · **不呼叫 CreateProcess 提權**,也不帶 `runas` 動詞。
//   · 用 ShellExecuteEx 開那支安裝程式,由**它自己 manifest 裡的**
//     requestedExecutionLevel=requireAdministrator 去要權限 ——
//     與使用者在檔案總管裡雙擊它完全同一條路。
//   · 換完檔之後由**安裝程式**把服務重新叫起來,而且是
//     ExecAsOriginalUser(登入者的身分),不是提權那一份。
//
// ══ ⚠ 交棒之後我們會被殺掉 ═══════════════════════════════════════
//
// 安裝程式的 PrepareToInstall 第一件事就是停掉 rime_service.exe ——
// 也就是這個進程。所以「等它結束、看結束碼」在這裡不成立。
// 交棒前先寫一張交棒單,下一次啟動再和解(見 common/update_flow.h 的
// ReconcileHandoff)。
//
#ifndef RIMEWIN_SERVICE_UPDATE_SERVICE_H_
#define RIMEWIN_SERVICE_UPDATE_SERVICE_H_

#include <string>

#include "../common/update_flow.h"
#include "../common/update_manifest.h"
#include "net_gate.h"
#include "settings_store.h"

namespace rimewin {

// 線上清單的位置。⚠ 桌面端**自己一份** —— Android 那一份描述的是 APK,
// 檔名、大小、sha256 沒有一個對得上。見 common/update_manifest.h 檔頭。
extern const char kWinUpdateManifestUrl[];

class UpdateService {
 public:
  // net / store 必須活得比它久。install_dir 是安裝目錄(version.txt 在那裡)。
  UpdateService(NetGate* net, SettingsStore* store, std::wstring install_dir);

  // ── 本機這一版 ────────────────────────────────────────────
  //
  // ⚠ 讀不到(舊的安裝沒有 version.txt)時 valid() 是 false,而**不是** 0。
  //   當成 0 的話任何一份清單看起來都比較新,於是我們會叫使用者去裝一個
  //   可能比他現在這一版還舊的東西。
  const InstalledVersion& installed() const { return installed_; }
  void ReloadInstalledVersion();

  // ── 查(阻塞;呼叫端丟到背景執行緒)────────────────────────
  bool Check(UpdateFailure* why);
  bool have_manifest() const { return have_manifest_; }
  const WinUpdateManifest& latest() const { return latest_; }
  UpdateVerdict verdict() const;
  AppIdVerdict app_id_verdict() const;

  // ── 下載並核對(阻塞)─────────────────────────────────────
  //
  // 成功之後 verified() 為真、staged_path() 指向那個檔案。
  // ⚠ 失敗一律把檔案刪掉:硬碟上不留半份下載到一半、或摘要不符的東西。
  bool DownloadAndVerify(UpdateFailure* why);
  bool verified() const { return verified_; }
  const std::wstring& staged_path() const { return staged_path_; }

  // ── 交棒 ──────────────────────────────────────────────────
  //
  // @param deploy_running 引擎正在整理字詞 → 現在停不下來,不交棒。
  // 成功之後這個進程隨時會被安裝程式停掉。
  bool HandOff(bool deploy_running, UpdateFailure* why);

  // ── 服務啟動時的和解 ──────────────────────────────────────
  //
  // 有交棒單就把它與「現在裝的是哪一版」對一次,然後**把單子刪掉**
  // (不刪的話下一次啟動會再報一次同樣的結果)。
  UpdateOutcome Reconcile(UpdateFailure* why, std::string* version_name);

  // 診斷用。永遠英文(§4.11)。
  std::string DiagnosticLine() const;

 private:
  std::wstring UpdateDir() const;
  std::wstring HandoffPath() const;
  // 只留目標版本那一個檔案。其餘刪掉 —— 一份安裝程式好幾十 MB。
  void PurgeStaged(const std::wstring& keep);

  NetGate* net_;
  SettingsStore* store_;
  std::wstring install_dir_;

  InstalledVersion installed_;
  bool have_manifest_ = false;
  WinUpdateManifest latest_;
  bool verified_ = false;
  std::wstring staged_path_;
};

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_UPDATE_SERVICE_H_
