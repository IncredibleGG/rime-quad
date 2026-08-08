// windows/tests/probe_main.cc — 走**真的**具名管道去驅動服務進程
//
// 這支程式是本輪 CI 能做到的最強驗證。它蓋掉的範圍是:
//
//   真的具名管道 → 真的線路格式 → 真的服務進程 → 真的 rime_shell → 真的 librime
//
// 也就是說,除了 TSF 本身與候選窗的畫面之外,DLL 會走的每一段都被走過一次。
// 而它用的 IpcClient 就是 DLL 用的那一份原始碼,不是另寫的測試用戶端 ——
// 另寫一份的話,測到的是那一份,不是產品裡的那一份。
//
// **它驗不到什麼**(這一節比上面那一節重要):
//   · TSF 的 COM 註冊、Activate、組字、edit session —— CI 上做不到。
//   · 候選窗長什麼樣、位置對不對。
//   · 按鍵映射:這裡直接送 keysym,沒有經過 VK_* → keysym 那一層。
//     那一層由 windows/tests/test_keymap.cc 與 test_win32_layouts.cc 覆蓋。
//
// 用法:
//   rime_probe.exe --keys nihao --select 1 --expect 你好

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../tsf/ipc_client.h"
#include "../winshared/winutil.h"

using namespace rimewin;

namespace {

void PrintSnapshot(const char* tag, const Snapshot& s) {
  std::printf("  [%s] preedit=\"%s\" 候選=%d 高亮=%d schema=%s\n", tag,
              s.preedit.c_str(), static_cast<int>(s.items.size()), s.highlighted,
              s.schema_id.c_str());
  for (size_t i = 0; i < s.items.size() && i < 9; ++i) {
    std::printf("        %s. %s%s%s\n", s.items[i].label.c_str(),
                s.items[i].text.c_str(), s.items[i].comment.empty() ? "" : "  # ",
                s.items[i].comment.c_str());
  }
  if (s.has_commit) std::printf("  [%s] commit=\"%s\"\n", tag, s.commit_text.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  std::string keys = "nihao";
  std::string expect;
  int select = 1;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
    else if (!std::strcmp(argv[i], "--expect") && i + 1 < argc) expect = argv[++i];
    else if (!std::strcmp(argv[i], "--select") && i + 1 < argc) select = std::atoi(argv[++i]);
    else {
      std::fprintf(stderr, "未知參數: %s\n", argv[i]);
      return 2;
    }
  }

  std::printf("=== rime_probe:經由具名管道驅動服務 ===\n");
  std::printf("管道: %s\n", WideToUtf8(RimePipeName()).c_str());

  IpcClient ipc;
  // 刻意**不**設服務路徑:這支程式不負責把服務叫起來。
  // 叫得起來與否是 DLL 的事,而 CI 是自己先把服務啟動好的 ——
  // 混在一起的話,「服務沒起來」和「協議談不攏」會變成同一個錯誤訊息。
  bool ready = false;
  for (int i = 0; i < 100 && !ready; ++i) {
    ready = ipc.EnsureReady();
    if (!ready) ::Sleep(100);
  }
  if (!ready) {
    std::fprintf(stderr, "!! 連不上服務或握手失敗\n");
    return 1;
  }
  std::printf("握手完成,session 已建立\n\n");

  std::string committed;
  Snapshot last;

  std::printf("--- 送出按鍵 ---\n");
  for (char c : keys) {
    Result r;
    // ASCII 可列印字元的 X11 keysym 就是它的 ASCII 值,與 rime_console 相同。
    if (!ipc.SendKey(static_cast<int32_t>(static_cast<unsigned char>(c)), 0, &r)) {
      std::fprintf(stderr, "!! 送 '%c' 失敗(連線已降級)\n", c);
      return 1;
    }
    std::printf("  '%c' -> %s\n", c, r.handled ? "已被輸入法消費" : "未消費(!)");
    if (r.snap.has_commit) committed += r.snap.commit_text;
    last = r.snap;
  }
  PrintSnapshot("組字後", last);

  // 與 tools/rime_console.cc 完全相同的政策迴圈。
  //   count > 0                  → 還有段落待選
  //   count == 0 && is_composing → 轉換完成待確認
  std::printf("\n--- 政策迴圈 ---\n");
  for (int step = 1; step <= 12; ++step) {
    const int count = static_cast<int>(last.items.size());
    const bool composing = (last.status_flags & kStComposing) != 0;
    std::printf("  [%d] composing=%d 候選=%d preedit=\"%s\"\n", step,
                composing ? 1 : 0, count, last.preedit.c_str());
    Result r;
    if (count > 0) {
      if (!ipc.SendSelect(select - 1, &r)) {  // rs_select_candidate 是 0 起算
        std::fprintf(stderr, "!! 選字失敗\n");
        return 1;
      }
    } else if (composing) {
      if (!ipc.SendCommitComposition(&r)) {
        std::fprintf(stderr, "!! commit 失敗\n");
        return 1;
      }
    } else {
      break;
    }
    if (r.snap.has_commit) committed += r.snap.commit_text;
    last = r.snap;
  }

  std::printf("\n>>> COMMIT: \"%s\"\n", committed.c_str());

  if (!expect.empty()) {
    // 整串精確比對。只用「包含」是不夠的:上屏成「你好嗎」一樣會通過。
    if (committed != expect) {
      std::fprintf(stderr, "!! commit 是「%s」,預期「%s」\n", committed.c_str(),
                   expect.c_str());
      return 1;
    }
    std::printf("斷言通過:commit == 「%s」\n", expect.c_str());
  }
  return 0;
}
