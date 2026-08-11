// windows/common/status_line.h — 「視窗底部那一行現在是誰寫的」
//
// ── 為什麼這需要一個型別 ────────────────────────────────────────
//
// 設定視窗底部只有**一行**狀態訊息,而寫它的人有五個:
//
//   · 套用的結果(「已套用」/「套用失敗」)
//   · 存檔失敗的紅字
//   · 引擎心跳(「還在處理剛才那一下」)
//   · 重新整理字詞的進度(每 200 毫秒一則)
//   · 4 秒後的自動清空
//
// 前四個都是在**說**一件事,而最後一個是在**收回**一件事。這兩種動作
// 差別很大:說話寫下去就對了;收回只有在「收的是自己寫的那一則」時
// 才是對的。
//
// 舊版兩處都是無條件 `SetStatus(std::wstring())`:
//
//   · 心跳:引擎卡住 → 寫一句;卡住解除 → **無條件清空**。
//     使用者在那之間按了一顆會失敗的按鈕、拿到紅字,那行紅字會在
//     一個他完全無法預期的時間點被抹掉。
//   · 4 秒的計時器:它是為了收回「已套用」而設的,但它收的是
//     **那一刻畫面上的任何東西**。
//
// 兩個都不是「偶爾」——它們是「只要那兩件事同時發生就一定會」,而且
// 症狀是**訊息消失**,也就是沒有人會回報的那一種。
//
// ⚠ 這個類別**刻意不存文字**。它只回答「我寫的那一則還在畫面上嗎」。
//   存文字的話會出現第二份真相(畫面上一份、這裡一份),而這個檔案
//   存在的理由正好是「同一件事不要有兩份」。
//
// ⚠ 放在 common/ 是為了在 Ubuntu 上測得到 —— service/settings_window.cc
//   在這裡編不起來,而上面那兩個缺陷都是純粹的順序邏輯。
//
#ifndef RIMEWIN_COMMON_STATUS_LINE_H_
#define RIMEWIN_COMMON_STATUS_LINE_H_

namespace rimewin {

class StatusLine {
 public:
  // 一則訊息的識別。0 = 「我沒有寫過」,而它永遠不 StillShowing。
  using Ticket = unsigned;
  static constexpr Ticket kNone = 0;

  // 寫了一則訊息。拿走的那張票代表「畫面上現在是我那一則」。
  Ticket Write() { return ++serial_; }

  // 我拿到票之後,有沒有別人蓋過去?
  //
  // ⚠ 這一支是**收回**動作唯一的守門。清空之前沒有問它,就是
  //   「在隨機的時間點抹掉使用者剛拿到的紅字」。
  bool StillShowing(Ticket t) const { return t != kNone && t == serial_; }

  // 目前畫面上是第幾則。給「寫完之後才知道自己要記哪一張票」的
  // 呼叫端用(例如心跳:先 SetStatus,再記下這一則是我的)。
  Ticket current() const { return serial_; }

 private:
  // ⚠ 只增不減。回收序號會讓一張很舊的票意外變成「還在畫面上」,
  //   而那個錯誤只在跑很久的進程上出現。unsigned 溢位要 43 億則訊息,
  //   而這一行每 4 秒最多換一次。
  Ticket serial_ = kNone;
};

}  // namespace rimewin

#endif  // RIMEWIN_COMMON_STATUS_LINE_H_
