// windows/service/cand_window.h — 候選窗
//
// ⚠ 候選窗是**服務進程**開的獨立 top-level window,不是 DLL 畫的。
//   這是架構上不可妥協的一條:DLL 住在每一個宿主進程裡,在那裡建視窗、
//   載字型、跑排版,等於把所有這些東西的風險複製到瀏覽器與提權進程裡。
//
// ⚠ 視覺部分刻意做到最小。
//   候選窗的規範由 macOS 端(第一個桌面端)擴充、Windows 端繼承
//   (docs/coordination.md §2)。規範 §11 自承多欄／表格排版與狀態列外觀
//   尚未定義,而 §8.6.7 的 max_width 溢出行為寫的是「由實作決定」。
//   在那些落地之前把視覺做滿,等於自己發明一套,「一套配置四端共用」就沒了。
//   所以這裡只用 §8.6.1–8.6.5 與 §8.6.7 **已經寫下來**的欄位,取規範預設值,
//   不讀主題檔、不加欄位。缺口已寫進 coordination.md §5。
#ifndef RIMEWIN_SERVICE_CAND_WINDOW_H_
#define RIMEWIN_SERVICE_CAND_WINDOW_H_

#include <windows.h>

#include <mutex>

#include "../common/cand_layout.h"
#include "../common/protocol.h"

namespace rimewin {

// 服務裡的其他部分只看到這個介面 —— --no-ui 模式(CI 用)換一個空實作,
// 而不是在每個呼叫點加 if。
class CandidateUi {
 public:
  virtual ~CandidateUi() = default;
  virtual void Update(const Snapshot& snap, const RECT& caret) = 0;
  virtual void Hide() = 0;
};

// 什麼都不做的實作。CI 的 runner 上沒有人在看螢幕,而建視窗會把
// 「服務啟動失敗」和「視窗建不起來」混成同一個症狀。
class NullCandidateUi : public CandidateUi {
 public:
  void Update(const Snapshot&, const RECT&) override {}
  void Hide() override {}
};

// 真的視窗。內部自己起一條有訊息迴圈的執行緒 —— Win32 的視窗屬於建立它的
// 執行緒,而連線執行緒是阻塞在管道讀取上的,沒有訊息迴圈。
class CandidateWindow : public CandidateUi {
 public:
  CandidateWindow();
  ~CandidateWindow() override;

  bool Start();
  void Stop();

  // 可從任何執行緒呼叫:複製一份狀態,再用 PostMessage 叫醒 UI 執行緒。
  void Update(const Snapshot& snap, const RECT& caret) override;
  void Hide() override;

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
  // 不用 captureless lambda 轉函式指標:LPTHREAD_START_ROUTINE 是 WINAPI
  // (__stdcall),lambda 轉出來的是 __cdecl。x64 上兩者相同所以看不出問題,
  // 但 x86 與 arm64 上就不是了 —— 而 arm64 正是下一個要做的架構。
  static DWORD WINAPI UiThreadEntry(LPVOID self);
  void UiThread();
  void Relayout();
  void Paint(HDC hdc);
  Extent MeasureWithFont(const std::string& utf8, double size);

  HANDLE thread_ = nullptr;
  DWORD thread_id_ = 0;
  HANDLE ready_ = nullptr;
  HWND hwnd_ = nullptr;

  std::mutex mu_;
  Snapshot pending_;
  RECT pending_caret_{};

  // 以下只在 UI 執行緒上碰。
  Snapshot shown_;
  RECT caret_{};
  CandidateStyle style_;
  WindowLayout layout_;
  double dpi_scale_ = 1.0;
};

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_CAND_WINDOW_H_
