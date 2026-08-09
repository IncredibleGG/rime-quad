// windows/setup/doctor.h — 「它到底哪裡壞了」的一頁式自我診斷
//
// ══ 為什麼需要它 ═══════════════════════════════════════════════════
//
// 使用者能給的資訊是「不能用」。而「不能用」在這條路徑上至少有九種:
//
//   1. 檔案沒裝齊(尤其是 data\shared —— 少了它,每一步都成功而一個字打不出來)
//   2. 全機註冊(HKLM)沒做成 / 指到已經不存在的 DLL
//   3. 使用者側(HKCU)沒啟用 —— 清單裡根本看不到這個輸入法
//   4. 系統沒有載入 rime_tsf.dll(註冊看起來對,但 COM 起不來)
//   5. 載入了,但 ActivateEx 沒被呼叫
//   6. ActivateEx 過了,但鍵盤佈局問不出字 → 按鍵一顆都進不了引擎
//   7. 按鍵進得去,但服務進程沒在跑
//   8. 服務在跑,但管道連不上 / 握手不合(兩邊不是同一次建置)
//   9. 全部都好,但引擎層本身壞了(詞庫沒部署完、資料缺)
//
// **這九種在使用者眼裡長得一模一樣。** 而我們要來回好幾輪才問得出是哪一種,
// 每一輪都是一次「你能不能幫我看一下…」。
//
// 所以 `rime_ime_setup.exe doctor` 一次把九格全部印出來,每一格都是
// PASS / FAIL / WARN / INFO 開頭的一行,後面接一句人話說明「這一格紅了代表
// 什麼、接下來該做什麼」。使用者只要把輸出貼過來就好。
//
// ══ 這支工具本身也要被驗 ═══════════════════════════════════════════
//
// 一支只會印綠字的診斷工具比沒有更糟 —— 它讓人以為有人在看。
// 所以 windows/verify_installer.sh 對它有正反兩面的斷言:
//   · 安裝**之前**跑,必須非零結束,而且註冊那一格必須是 FAIL
//   · 安裝**之後**、服務跑著的時候跑,必須以 0 結束,而且該綠的都綠
//   · 把服務停掉再跑一次,服務那一格必須變 FAIL(證明它真的在看)
#ifndef RIMEWIN_SETUP_DOCTOR_H_
#define RIMEWIN_SETUP_DOCTOR_H_

#include <string>

namespace rimewin {

struct DoctorOptions {
  // 跑一次引擎層的端到端檢查(呼叫同目錄的 rime_console.exe)。
  // 預設開著 —— 「引擎層通不通」正是分層診斷的第一刀。
  // 首次部署還沒跑完時它會逾時,那時報 WARN 而不是 FAIL(見 doctor.cc)。
  bool check_engine = true;
  // 把報告另存一份並用記事本打開。給使用者用:他不必會開命令列。
  bool open_report = false;
  // 掃描其他進程、看誰載入了 rime_tsf.dll。
  // 這一格會逐一開啟系統上的每一個進程,在很忙的機器上要一兩秒。
  bool scan_processes = true;
};

// 回傳 FAIL 的格數(0 = 全部通過)。報告一律印到 stdout。
int RunDoctor(const DoctorOptions& opt);

}  // namespace rimewin

#endif  // RIMEWIN_SETUP_DOCTOR_H_
