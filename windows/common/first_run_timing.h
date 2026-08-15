// windows/common/first_run_timing.h — 「第一次要等多久」只有這裡有數字
//
// ══ 為什麼要有這個檔案 ═════════════════════════════════════════════
//
// 同一件事,四個地方各講了一個數字,而其中**兩個是使用者讀得到的**:
//
//   installer/luminakey.iss 的 FinishedLabel   「一到數分鐘」  ← 使用者
//   common/service_state.h / service/status_bar.cc  「7~12 秒」
//   service/engine.h                            「可能好幾分鐘」
//   setup/doctor.cc 的診斷報告                  「一到數分鐘」  ← 使用者
//
// 實測是 **7~12 秒**(出貨預設方案;README 的 U16 就是在量這一格)。
// 也就是說使用者讀得到的那兩處差了一個數量級,而且是往**壞的**那一邊差:
// 一個等了十幾秒的人會以為自己還要再等好幾分鐘,然後跑去做別的事,
// 或者乾脆判定裝壞了去重裝 —— 而重裝會讓計時器從頭來過。
//
// 對照:安卓端的同一句話是「Takes about 13 seconds.」。有數字,而且是對的。
//
// ── ⚠ 「好幾分鐘」不是錯的,它回答的是另一個問題 ──────────────────
//
// 使用者自己灌了一份很大的第三方詞典(moran 那一級)時,部署確實可能
// 跑到分鐘級。所以**逾時要留多寬**的答案是分鐘級,而**要跟剛裝完的人
// 說什麼**的答案是秒級。這兩件事在這裡分開命名,以後就不會再有人拿
// 逾時預算去嚇一個剛按下「完成」的人。
//
// ══ 誰讀這裡 ═══════════════════════════════════════════════════════
//
//   C++            直接 include。setup/doctor.cc 把數字印進報告;
//                  tests/test_service_state.cc 驗它的性質。
//   .iss(Pascal)  不能 include C++,所以 windows/make_installer.sh 從
//                  **本檔**把數字讀出來,用 ISCC 的 /D 傳進去
//                  (FirstDeployLowSec / FirstDeployHighSec /
//                   ProfileVisibleSec)。
//
//   ⚠ 那條 derivation 斷掉的時候會**大聲**斷:.iss 裡寫的是
//     `{#FirstDeployLowSec}`,而 ISPP 遇到未定義的識別字是當場失敗,
//     不是安靜地展開成空字串。所以「傳參壞了而安裝程式照樣編得出來、
//     只是句子裡少一個數字」這種形狀在這裡不成立。
//
// ⚠ 要改數字的話改這裡,而且只改這裡。改之前先問一句:**這是量出來的嗎。**
//
#ifndef RIMEWIN_FIRST_RUN_TIMING_H_
#define RIMEWIN_FIRST_RUN_TIMING_H_

namespace rimewin {

// 出貨預設方案的首次部署,實測 7~12 秒。
// ⚠ **使用者讀得到的句子一律用這一組**,不准用下面那個逾時預算。
inline constexpr int kFirstDeployTypicalLowSec = 7;
inline constexpr int kFirstDeployTypicalHighSec = 12;

// 等待預算(不是期待值)。使用者自己灌了很大的詞典時部署可能跑到分鐘級,
// 逾時要留得夠寬 —— setup/doctor.cc 等 rime_console 就是等這麼久,
// 而且逾時在那裡刻意報 WARN 不報 FAIL。
inline constexpr int kDeployWaitBudgetSec = 180;

// 註冊成功之後,Win + 空白鍵那份清單多久才看得到我們。
//
// 實測(windows/verify_tsf.sh §13、README 的同一段):`register` 回傳成功
// 之後 **0.12 秒**時 EnumLanguageProfiles 看不到我們,**22 秒**後同一支
// 程式跑同一段就全部看得到。登錄檔是同步的,CTF 的可見性不是。
//
// ⚠ 這個數字之所以要出現在使用者讀得到的地方:麻瓜的動作序列正好是
//   「按下完成 → 立刻 Win + 空白鍵」→ 清單上沒有它 → 結論是「裝失敗了」
//   → 重裝。畫面上要有一句話叫他等一下。
inline constexpr int kProfileVisibleObservedSec = 22;

}  // namespace rimewin

#endif  // RIMEWIN_FIRST_RUN_TIMING_H_
