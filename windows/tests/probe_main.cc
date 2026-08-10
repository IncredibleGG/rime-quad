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
//   rime_probe.exe --keys nihao --select 1 --schema luna_pinyin_tw --expect 你好
//   rime_probe.exe --connect-only            只驗「連得上、握得了手、開得了 session」
//   rime_probe.exe --ascii-toggle --schema … 驗 Ctrl+空白鍵真的切得了中英
//   rime_probe.exe --attempts N              最多做 N 次**真正的**連線嘗試
//
// ── --attempts 為什麼要存在,而且為什麼預設是 1 ──────────────────
//
// 舊版寫死「重試 100 次,每次之間睡 100ms」,看起來是「試了 100 次」。
// 實際上不是:每一次都要先過 LinkState::ShouldAttemptConnect() 那一關,
// 而握手不合的退避是 30 秒 —— 於是那 100 次裡**只有第一次**真的開過管道。
// (這個算術釘在 tests/test_link_state.cc 的
//  link_backoff_eats_almost_all_of_a_naive_retry_loop。)
//
// 那個退避對 DLL 是正確的(它跑在宿主的 UI 執行緒上),對一個宣稱
// 「重試 N 次」的驗證工具則是謊報。所以這裡每一次嘗試之前先 ResetLink(),
// 讓「試了 N 次」是真的 N 次,並且把真正的次數印出來。
//
// 預設 1:服務寫 ready 檔的時候,管道**已經**接得起連線
// (service/pipe_server.cc 的 Start() 會等監聽執行緒回報才返回),
// 而且引擎已經預熱過(service/main.cc 的 WarmUpEngine)。
// 所以第一次就該成功。需要重試才連得上,本身就是一個要修的缺陷 ——
// 用「多試幾次總會過」把它蓋掉,就是這個專案一再踩到的那種綠燈。

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../common/hotkey_policy.h"
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

// GetLastError() 的白話。只列這裡真的會遇到的那幾個 ——
// 一張抄來的完整表格會讓人以為每一個都被想過。
const char* OsErrorName(unsigned long e) {
  switch (e) {
    case 0:   return "沒有錯誤碼";
    case 2:   return "ERROR_FILE_NOT_FOUND:這個名字的管道不存在";
    case 5:   return "ERROR_ACCESS_DENIED:管道在,但這個身分開不了它";
    case 231: return "ERROR_PIPE_BUSY:所有實例都忙著";
    case 109: return "ERROR_BROKEN_PIPE:對面已經關掉了";
    default:  return "(沒有列在 probe 的對照表裡)";
  }
}

// ★ 本輪的重點。
//
// 舊版對三種完全不同的失敗一律印同一句「連不上服務或握手失敗」,
// 而它們該修的地方分別是:服務的監聽迴圈、兩邊的建置版本、服務端的速度。
// 一句話蓋掉三件事,等於把下一個看到 CI 紅燈的人往隨機的方向送。
void ReportFailure(const IpcClient& ipc, const std::wstring& pipe_name,
                   int wanted_attempts) {
  const ReadyDiagnosis& d = ipc.diagnosis();
  std::fprintf(stderr, "\n!! 連不上服務 —— 失敗在「%s」這一步\n",
               ReadyStageName(d.stage));
  std::fprintf(stderr, "   管道     : %s\n", WideToUtf8(pipe_name).c_str());
  std::fprintf(stderr, "   真正嘗試 : %u 次(要求 %d 次)\n", d.attempts,
               wanted_attempts);
  std::fprintf(stderr, "   失敗種類 : %s\n", LinkFailureName(d.failure));

  switch (d.stage) {
    case ReadyStage::kPipe:
      std::fprintf(stderr, "   GetLastError = %lu(%s)\n", d.os_error,
                   OsErrorName(d.os_error));
      std::fprintf(stderr,
          "\n   → 這**不是**協議或版本的問題:HELLO 一個位元組都還沒送出去。\n");
      if (d.os_error == 2) {
        std::fprintf(stderr,
            "   → 服務如果已經寫了 ready 檔卻沒有管道,問題在服務的監聽迴圈。\n"
            "     去看 service.log 裡 [pipe] 開頭的那幾行 —— 監聽迴圈死掉時\n"
            "     會在那裡說明原因與 GetLastError。\n"
            "     service.log 裡完全沒有 [pipe] 行,代表監聽迴圈還活著,\n"
            "     那就要查管道**名字**是不是對不上(名字裡帶使用者 SID)。\n");
      } else if (d.os_error == 5) {
        std::fprintf(stderr,
            "   → 管道存在但開不了 = 身分不對。管道的 DACL 只授權建立它的\n"
            "     那個使用者;probe 與服務若跑在不同的權限層級(提權 / 非提權)\n"
            "     或不同的使用者底下,就會是這個錯誤碼。\n");
      }
      break;

    case ReadyStage::kHandshake:
      std::fprintf(stderr, "   我方     : proto=%u  shell_abi=%u\n",
                   d.tried_proto, d.my_shell_abi);
      if (d.peer_replied) {
        std::fprintf(stderr, "   服務回報 : proto=%u  shell_abi=%u  version=\"%s\"\n",
                     d.peer.proto, d.peer.shell_abi,
                     d.peer.service_version.c_str());
        std::fprintf(stderr,
            "\n   → 管道通了、訊息也來回了,是**版本談不攏**。\n"
            "     兩邊不是同一次建置的產物 —— CI 上請確認 probe 與\n"
            "     rime_service.exe 來自同一個 run 的 artifact。\n");
      } else {
        std::fprintf(stderr, "   服務回報 : (沒有回一則解得開的 HELLO_OK)\n");
        std::fprintf(stderr,
            "\n   → 管道連上了,但服務沒有把握手做完。\n"
            "     「%s」是它的下場;若是「對面關掉了連線」,那是舊服務\n"
            "     解不開新版 HELLO 時的標準反應(而降級重試 v1 也失敗了)。\n",
            LinkFailureName(d.failure));
      }
      break;

    case ReadyStage::kSession:
      std::fprintf(stderr, "   協商到的線路版本 = %u\n", d.tried_proto);
      std::fprintf(stderr,
          "\n   → **握手是過的**,協議與 ABI 都對得上。問題在 SESSION_NEW。\n"
          "     服務端處理 SESSION_NEW 時會替使用者的語言選預設方案,\n"
          "     而第一次選方案要載入詞典(冷的時候是幾百毫秒到幾秒),\n"
          "     很容易超過 300ms 的預算。服務應該在開管道之前就預熱\n"
          "     (service/main.cc 的 WarmUpEngine)—— service.log 裡\n"
          "     應該要有一行「預熱完成」。沒有那一行就是預熱沒跑到。\n");
      break;

    case ReadyStage::kNone:
      std::fprintf(stderr,
          "   → 沒有記錄到失敗階段。這代表每一次嘗試都被連線狀態機的退避\n"
          "     擋掉了(真正嘗試 %u 次)—— probe 的重試迴圈有問題,不是服務。\n",
          d.attempts);
      break;
  }
  std::fprintf(stderr, "\n");
}

}  // namespace

// ── Ctrl+空白鍵:中英切換 ────────────────────────────────────────
//
// 使用者回報「ctrl+ 空格沒辦法切中英文」。這一支驗的是**服務那一半**:
// 那一顆鍵經由真的具名管道送進來之後,引擎有沒有真的換模式。
//
// ⚠ 判準不是「旗標變了」而已 —— 旗標可以變而行為不變。真正的判準是
//   **同一串字母的下場不一樣**:中文模式下引擎會吃掉它們(開始組字),
//   英數模式下引擎不處理(handled=false,沒有組字)。後者正是瘦 DLL
//   自己把字元寫進文件的那條路(key_eat_policy.h 的 kCharacter)。
//
// ⚠ DLL 那一半(Ctrl+空白鍵有沒有真的被 TSF 攔下來)在這裡驗不到,
//   它在 windows/verify_tsf.sh 的 --check-preserved-key。兩半都要。
int RunAsciiToggle(IpcClient& ipc, const std::string& keys) {
  auto composing_of = [](const Snapshot& s) {
    return (s.status_flags & kStComposing) != 0;
  };
  auto ascii_of = [](const Snapshot& s) {
    return (s.status_flags & kStAsciiMode) != 0;
  };

  // 送一串字母,回報「引擎吃掉了幾顆」與最後的快照。每一輪之前先清乾淨。
  auto type = [&](Snapshot* out, int* eaten) -> bool {
    Result clear;
    ipc.SendClear(&clear);
    *eaten = 0;
    for (char c : keys) {
      Result r;
      if (!ipc.SendKey(static_cast<int32_t>(static_cast<unsigned char>(c)), 0,
                       &r)) {
        std::fprintf(stderr, "!! 送 '%c' 失敗\n", c);
        return false;
      }
      if (r.handled) ++(*eaten);
      *out = r.snap;
    }
    return true;
  };

  std::printf("=== Ctrl+空白鍵:中英切換 ===\n");

  Snapshot before{};
  int eaten_before = 0;
  if (!type(&before, &eaten_before)) return 1;
  std::printf("切換前:引擎吃掉 %d/%d 顆,組字中=%d,英數=%d\n", eaten_before,
              static_cast<int>(keys.size()), composing_of(before) ? 1 : 0,
              ascii_of(before) ? 1 : 0);
  if (ascii_of(before)) {
    std::fprintf(stderr,
                 "!! 一開始就在英數模式 —— 這一輪測不到「切過去」\n");
    return 1;
  }
  if (eaten_before != static_cast<int>(keys.size()) || !composing_of(before)) {
    std::fprintf(stderr,
                 "!! 中文模式下那串字母沒有被引擎吃掉(吃了 %d/%d)——\n"
                 "   對照組不成立,下面的比較沒有意義\n",
                 eaten_before, static_cast<int>(keys.size()));
    return 1;
  }

  {
    Result clear;
    ipc.SendClear(&clear);
  }

  // 這一行就是那顆鍵。keysym / 修飾鍵取自 common/hotkey_policy.cc ——
  // 與瘦 DLL 的 OnPreservedKey 送的是同一組值。
  Result hot;
  if (!ipc.SendKey(AsciiToggleKeysym(), AsciiToggleModifiers(), &hot)) {
    std::fprintf(stderr, "!! Ctrl+空白鍵送不出去\n");
    return 1;
  }
  std::printf("Ctrl+空白鍵:handled=%d,英數=%d\n", hot.handled ? 1 : 0,
              ascii_of(hot.snap) ? 1 : 0);
  if (!hot.handled) {
    std::fprintf(stderr,
                 "!! 服務沒有處理 Ctrl+空白鍵 —— 它被當成一般按鍵交給引擎了\n");
    return 1;
  }
  if (!ascii_of(hot.snap)) {
    std::fprintf(stderr,
                 "!! 切換之後回的快照仍然說「不是英數模式」——\n"
                 "   那一橫的第一格讀的就是這個旗標,它會說謊\n");
    return 1;
  }

  Snapshot after{};
  int eaten_after = 0;
  if (!type(&after, &eaten_after)) return 1;
  std::printf("切換後:引擎吃掉 %d/%d 顆,組字中=%d,英數=%d\n", eaten_after,
              static_cast<int>(keys.size()), composing_of(after) ? 1 : 0,
              ascii_of(after) ? 1 : 0);
  if (eaten_after != 0 || composing_of(after)) {
    std::fprintf(stderr,
                 "!! 英數模式下引擎還在吃字母(吃了 %d/%d,組字中=%d)——\n"
                 "   模式旗標變了而行為沒變,使用者打出來的還是中文\n",
                 eaten_after, static_cast<int>(keys.size()),
                 composing_of(after) ? 1 : 0);
    return 1;
  }

  // 再按一次要切回來。⚠ 少了這一條,「只能單向切」會全綠 ——
  //   那正是這一輪在懸浮狀態列第二格上抓到的同一種缺陷。
  Result back;
  if (!ipc.SendKey(AsciiToggleKeysym(), AsciiToggleModifiers(), &back)) {
    std::fprintf(stderr, "!! 第二次 Ctrl+空白鍵送不出去\n");
    return 1;
  }
  if (ascii_of(back.snap)) {
    std::fprintf(stderr, "!! 再按一次沒有切回中文模式 —— 這顆鍵只能單向\n");
    return 1;
  }
  Snapshot again{};
  int eaten_again = 0;
  if (!type(&again, &eaten_again)) return 1;
  std::printf("再切回來:引擎吃掉 %d/%d 顆,組字中=%d\n", eaten_again,
              static_cast<int>(keys.size()), composing_of(again) ? 1 : 0);
  if (eaten_again != static_cast<int>(keys.size()) || !composing_of(again)) {
    std::fprintf(stderr, "!! 切回中文之後字母又打不進去了\n");
    return 1;
  }

  std::printf("\n>>> ASCII TOGGLE OK\n");
  return 0;
}

int main(int /*narrow_argc*/, char** /*narrow_argv*/) {
  // ⚠ 參數必須從 CommandLineToArgvW 取,不可以用窄字元的 argv。
  //   窄字元 argv 走的是系統 ANSI 代碼頁,而我們要比對的預期值是「你好」——
  //   在英文 runner 上那兩個字會變成 "??",於是斷言永遠不會通過,
  //   而錯誤訊息長得像「commit 是「你好」,預期「??」」。實測過(CI run #20)。
  //
  //   進入點仍然是 main 而不是 wmain:理由見 service/main.cc 檔頭
  //   (glog 的 __argv)。這裡雖然沒有 glog,但兩支保持同一種寫法。
  int argc = 0;
  LPWSTR* wargv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (!wargv) {
    std::fprintf(stderr, "CommandLineToArgvW 失敗\n");
    return 2;
  }
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) args.push_back(WideToUtf8(wargv[i]));
  ::LocalFree(wargv);

  std::string keys = "nihao";
  std::string expect;
  std::string schema;
  int select = 1;
  int attempts = 1;
  bool connect_only = false;
  bool ascii_toggle = false;
  for (int i = 1; i < argc; ++i) {
    const std::string& a = args[static_cast<size_t>(i)];
    if (a == "--keys" && i + 1 < argc) keys = args[static_cast<size_t>(++i)];
    else if (a == "--expect" && i + 1 < argc) expect = args[static_cast<size_t>(++i)];
    else if (a == "--schema" && i + 1 < argc) schema = args[static_cast<size_t>(++i)];
    else if (a == "--select" && i + 1 < argc)
      select = std::atoi(args[static_cast<size_t>(++i)].c_str());
    else if (a == "--attempts" && i + 1 < argc)
      attempts = std::atoi(args[static_cast<size_t>(++i)].c_str());
    else if (a == "--connect-only") connect_only = true;
    else if (a == "--ascii-toggle") ascii_toggle = true;
    else {
      std::fprintf(stderr, "未知參數: %s\n", a.c_str());
      return 2;
    }
  }
  if (attempts < 1) {
    std::fprintf(stderr, "--attempts 至少要 1(給了 %d)\n", attempts);
    return 2;
  }

  const std::wstring pipe_name = RimePipeName();
  std::printf("=== rime_probe:經由具名管道驅動服務 ===\n");
  std::printf("管道: %s\n", WideToUtf8(pipe_name).c_str());
  std::fflush(stdout);

  IpcClient ipc;
  // 刻意**不**設服務路徑:這支程式不負責把服務叫起來。
  // 叫得起來與否是 DLL 的事,而 CI 是自己先把服務啟動好的 ——
  // 混在一起的話,「服務沒起來」和「協議談不攏」會變成同一個錯誤訊息。
  bool ready = false;
  for (int i = 0; i < attempts && !ready; ++i) {
    // 每一次之前把狀態機歸零,讓「試了 N 次」是真的 N 次。
    // 見檔頭 --attempts 那一段:退避會把一個看起來很有耐心的迴圈
    // 變成只試一次,而那個迴圈還會回報「重試了 100 次」。
    if (i > 0) {
      ipc.ResetLink();
      ::Sleep(100);
    }
    ready = ipc.EnsureReady();
  }
  if (!ready) {
    ReportFailure(ipc, pipe_name, attempts);
    return 1;
  }
  // 成功時也把協商結果印出來。這是「兩邊真的是同一次建置」在線路上的
  // 唯一證據 —— 只印「握手完成」的話,一個版本錯配但湊巧相容的組合
  // 看起來與正常完全一樣。
  {
    const ReadyDiagnosis& d = ipc.diagnosis();
    std::printf("握手完成,session 已建立(第 %u 次嘗試)\n", d.attempts);
    std::printf("  線路版本 = %u,shell ABI = %u,服務 = \"%s\"\n",
                ipc.negotiated_proto(), d.peer.shell_abi,
                d.peer.service_version.c_str());
  }
  if (connect_only) {
    std::printf("\n>>> CONNECT OK\n");
    return 0;
  }

  // ⚠ 一定要明確指定方案。librime 把「上次選的方案」記在使用者目錄的
  //   user.yaml 裡,所以「不指定」的結果取決於那個目錄的歷史 ——
  //   CI 上這個目錄是從 verify_console.sh 沿用來的,而它最後一個案例是注音,
  //   於是 nihao 被當成注音打出了「所噢草莓」。實測過(CI run #20)。
  if (!schema.empty()) {
    Result r;
    if (!ipc.SendSelectSchema(schema, &r)) {
      std::fprintf(stderr, "!! 切換方案 %s 失敗\n", schema.c_str());
      return 1;
    }
    std::printf("方案 -> %s(引擎回報 %s)\n", schema.c_str(),
                r.snap.schema_id.c_str());
    if (r.snap.schema_id != schema) {
      std::fprintf(stderr, "!! 方案沒有切過去:要的是 %s,實際是 %s\n",
                   schema.c_str(), r.snap.schema_id.c_str());
      return 1;
    }
  }
  std::printf("\n");

  if (ascii_toggle) return RunAsciiToggle(ipc, keys);

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
