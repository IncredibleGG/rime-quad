// windows/service/settings_window.h — 設定介面
//
// ── 技術選型:純 Win32 + 通用控制項 v6 ──────────────────────────
//
// 硬約束是「不要 Electron、不要打包 Chromium」,而它背後的理由不只是體積:
// 這個專案的定位是離線、經得起審計。使用者要能相信「它不連網」,
// 而說服他的方式是 `dumpbin /imports` 看得出來沒有網路 DLL ——
// 塞一個瀏覽器引擎進來,那句話就再也不可能驗證了。
//
// 被排除的與理由:
//
//   · **Electron / WebView2** — 前者打包 Chromium(上百 MB,而且是一整個
//     網路堆疊);後者相依於系統上的 Edge 執行期,那同樣是 Chromium,
//     而且**不在我們的控制之下**(它會自己更新)。兩者都直接違反約束。
//   · **WinUI 3 / XAML Islands** — 需要 Windows App SDK 的可轉散發套件。
//     那是幾十 MB 的另一組 DLL,而且與我們貫穿全案的 `/MT` 靜態 CRT
//     打架(見 README:相依到 VCRUNTIME140.dll 的話,沒裝 VC++ 執行期的
//     機器上輸入法會在載入階段失敗,而症狀是「在某些程式裡整個不存在」)。
//   · **WPF / WinForms** — 需要 .NET 執行期。同上,而且多一個更新來源。
//   · **本機 HTTP + 系統瀏覽器** — 會開 socket。離線定位下這是最糟的選項:
//     為了設定介面而讓服務進程監聽一個連接埠,等於親手推翻自己的主張。
//
// 剩下的是純 Win32。代價是排版要自己算、控制項醜、無障礙要自己顧;
// 換到的是**零額外相依、與 /MT 相容、二進位增加的大小解釋得清楚**。
//
// ── 為什麼在服務進程裡,而不是另一支 exe ────────────────────────
//
// 設定介面要做的事幾乎每一件都需要引擎:列出方案(rs_schema_list)、
// 立刻套用簡繁(rs_set_option)、重新部署並回報進度(rs_deploy)。
// 另開一支 exe 的話,這些全部要再走一次 IPC,而且「設定改了但引擎沒收到」
// 會變成一整類新的失效。服務進程本來就是那個持有引擎的地方。
//
// ⚠ **瘦 DLL 那一側一行都沒有碰。** 這個檔案不會被載入到宿主進程裡。
//   DLL 那邊只多了一顆語言列按鈕(tsf/lang_bar.cc),它送一則單向訊息。
//
// ── 每一顆控制項都必須真的做它宣稱的事 ──────────────────────────
//
// 這個專案抓過四個「畫面完全正常、自動化全過」的鍵。所以這裡的規矩是:
// 做不到就**不要放上去**。目前刻意沒有的:候選窗主題(規範還沒落地)、
// 方案市集(見 README「沒有被驗證的部分」)、詞庫匯出匯入。
//
#ifndef RIMEWIN_SERVICE_SETTINGS_WINDOW_H_
#define RIMEWIN_SERVICE_SETTINGS_WINDOW_H_

#include <windows.h>

#include <string>

#include "engine.h"
#include "settings_store.h"

namespace rimewin {

class CandidateWindow;

class SettingsWindow {
 public:
  SettingsWindow(Engine* engine, SettingsStore* store,
                 const std::string& shared_dir);
  ~SettingsWindow();

  // 起一條有訊息迴圈的 UI 執行緒。視窗要到 Open() 才會出現。
  bool Start();
  void Stop();

  // 可從任何執行緒呼叫(具名事件那條路是另一條執行緒,IPC 是連線執行緒)。
  // 已經開著就把它帶到最前面 —— 再開一個視窗是「按了沒反應」的另一種長相。
  void Open();

  // 設定變更後要通知候選窗重新讀字級。可為 nullptr。
  void SetCandidateWindow(CandidateWindow* w) { cand_ = w; }

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
  static DWORD WINAPI ThreadEntry(LPVOID self);
  void ThreadMain();

  void CreateUi(HWND hwnd);
  void LayoutUi();
  void ShowTab(int index);
  void ReloadFromSettings();
  void ReloadSchemaList();
  void OnCommand(int id, int code);
  void OnDeployTick();

  // 系統匣圖示。刻意掛在**設定視窗的**訊息迴圈上,不另外開一條執行緒:
  // 系統匣的回呼是視窗訊息,而這裡已經有一條有訊息迴圈的執行緒了。
  void AddTray();
  void RemoveTray();
  void OnTray(WPARAM w, LPARAM l);

  void ApplyVariantNow();
  void ApplyPunctNow();
  void ApplyDefaultSchemaNow();
  bool ApplyOrderAndPageSize(std::string* error);
  void StartRedeploy(const wchar_t* why);
  void SetStatus(const std::wstring& text);

  Engine* engine_;
  SettingsStore* store_;
  std::string shared_dir_;
  CandidateWindow* cand_ = nullptr;

  HANDLE thread_ = nullptr;
  DWORD thread_id_ = 0;
  HANDLE ready_ = nullptr;
  HWND hwnd_ = nullptr;
  HFONT font_ = nullptr;
  double dpi_scale_ = 1.0;
  int tab_ = 0;

  // 部署進度
  bool deploying_ = false;
  uint32_t deploy_seq_ = 0;
  DWORD deploy_start_ = 0;
  std::wstring deploy_why_;
  // 改 A 層之前的整份內容。⚠ 規範 §2:失敗時**整份還原**,
  // 不是「套用反向的編輯」—— 反向編輯的前提是外科手術本身沒有 bug,
  // 那正是出事當下最不該假設的事。
  bool has_rollback_ = false;
  std::string rollback_yaml_;

  bool tray_added_ = false;
  // explorer.exe 重啟之後系統匣會清空,而它會廣播這則訊息。
  // 沒有處理它的話,使用者的圖示在 explorer 崩過一次之後就永遠消失了 ——
  // 而他不會把兩件事聯想在一起。
  UINT taskbar_created_ = 0;

  Settings settings_;
  std::vector<std::pair<std::string, std::string>> schemas_;  // id, name
  std::vector<std::string> order_;  // 目前清單上的順序(id)
};

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_SETTINGS_WINDOW_H_
