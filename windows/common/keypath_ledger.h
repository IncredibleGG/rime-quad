// windows/common/keypath_ledger.h — 「按鍵路徑」那幾筆失敗算不算數的記帳
//
// ══ 這個檔案存在的理由 ════════════════════════════════════════════
//
// tsf_host 有兩條完全獨立的量法,而它們的**地位不一樣**:
//
//   線路(判準)  管道 → 握手 → session。不經過 TSF,所以前景搶不搶得到
//                都答得出來。
//   按鍵(加分)  送 nihao1、比對「你好」。它要經過 TSF,而 TSF 只把按鍵
//                交給**有執行緒焦點**的那一份文字服務 —— 拿不到焦點時
//                量到的每一個數字都是「沒有答案」,不是「壞掉」。
//
// 所以「按鍵那一段的失敗」有兩種,而且只有一種赦免得起來:
//
//   · 記下它的當下**沒有**焦點  → 那一筆什麼都沒量到,線路那三句話成立
//                                 時它赦免得起來。
//   · 記下它的當下**有**焦點    → 那一筆是真的量到了,一律計入。
//
// ⚠ **這一分不可以從一個跨階段的全域旗標推。** 上一版就是那樣寫的,
//   而它放過了一條真的假綠:
//
//     `g_keypath_blind` 是一個永不清除的黏滯全域旗標:第一階段搶不到
//     前景時把它設成 true,之後再也沒有人把它設回去。第二階段
//     `ForceForeground()` 搶贏了、真的送了按鍵、真的量到「打出來的是
//     nihao 而不是你好」—— 那一筆失敗仍然被記進「瞎的」那一格,
//     於是被赦免,宿主以 rc=0 結束,而腳本把它讀成
//     「舊的 DLL 映像在升級之後照樣打得出你好」。
//     同一份 log 裡宿主自己印著 PHASE2_DOC="nihao"。
//
//     ⚠ 而新加的 AttachThreadInput 剛好把這個組合從不可能變成可能 ——
//       升級那幾分鐘裡前景擁有者本來就會變。
//
//   「這一筆失敗當時到底有沒有焦點」是**呼叫點才知道的事實**。
//   所以 NoteFail() 把它當參數收,由編譯器逼每一個呼叫點回答;
//   本類別不從自己的成員狀態推導它,一次都不。
//
// ══ 兩個數字不可以打架 ════════════════════════════════════════════
//
// 上一版收尾印的 KEYPATH_MEASURED 用的也是那個黏滯旗標
// (`g_keypath_blind ? 0 : 1`),於是同一份 log 裡會同時出現
// `PHASE2_KEYPATH_MEASURED=1` 與 `KEYPATH_MEASURED=0`,外加一句
// 「⚠ 按鍵路徑這一次沒有量到」—— 明明量到了。
//
// 這裡把兩個欄位的定義分乾淨:
//
//   PHASE2_KEYPATH_MEASURED  **那一段**有沒有量到(第二階段自己印)。
//   KEYPATH_MEASURED         **整趟**裡有沒有任何一段量到 = measured()。
//
// 兩者範圍不同,所以「第一階段有焦點、第二階段瞎」時 KEYPATH_MEASURED=1
// 而 PHASE2_KEYPATH_MEASURED=0 不是矛盾。真正的矛盾 —— 有一段量到了、
// 收尾卻說整趟沒量到 —— 在新定義下不可能發生:measured() 是由「量到的
// 段落數」直接數出來的。
//
// ⚠ 而那句「按鍵路徑這一次沒有量到」只有 say_nothing_measured() 為真時
//   才准印:一段都沒量到,而且確實有段落是瞎的。
//
// 這裡沒有 windows.h、沒有時鐘、沒有全域狀態 —— tsf_host_main.cc 在
// Ubuntu 上編不起來(要 MSVC + msctf.h),而這一段判準必須有人真的跑。
// 它由 tests/test_keypath_ledger.cc 驗,而 tsf_host_main.cc 用的就是這一份
// (不是複製一份過去 —— 那樣的話這裡綠著也不代表宿主是對的)。
//
#ifndef RIMEWIN_KEYPATH_LEDGER_H_
#define RIMEWIN_KEYPATH_LEDGER_H_

namespace rimewin {

class KeypathLedger {
 public:
  // 走完一段按鍵路徑。
  // had_focus = **那一段送鍵的當下**執行緒焦點在不在我們身上。
  // 一趟可以有好幾段(第一階段一段、升級之後的第二階段一段)。
  void NoteSegment(bool had_focus) {
    if (had_focus)
      ++segments_focused_;
    else
      ++segments_blind_;
  }

  // 記一筆按鍵路徑的失敗。
  //
  // ⚠ blind 是**呼叫點的事實**,不是本物件推出來的。改成從成員狀態推
  //   (例如 `segments_blind_ > 0`)就是把上面檔頭講的那條假綠放回來:
  //   前一段瞎過,後一段真的量到的失敗就會被記進瞎的那一格。
  void NoteFail(bool blind) {
    if (blind)
      ++fails_blind_;
    else
      ++fails_focused_;
  }

  // 乙:不需要前景的那條路(管道 / 握手 / session 三句話)整條走通了。
  void set_link_verified(bool v) { link_verified_ = v; }
  bool link_verified() const { return link_verified_; }

  int segments_focused() const { return segments_focused_; }
  int segments_blind() const { return segments_blind_; }
  int fails_focused() const { return fails_focused_; }
  int fails_blind() const { return fails_blind_; }

  // 整趟裡**至少有一段**按鍵路徑是在有焦點的狀態下走完的。
  // 這就是收尾那行 KEYPATH_MEASURED 的定義。
  bool measured() const { return segments_focused_ > 0; }

  // 整趟裡至少有一段是瞎的。
  bool any_blind_segment() const { return segments_blind_ > 0; }

  // 被赦免的筆數。赦免的條件有三個,而且**三個都要成立**:
  //   1. 這一筆記下來的當下沒有焦點(它在 fails_blind_ 裡);
  //   2. 這一趟確實有一段是瞎的 —— 沒有瞎過卻有瞎的失敗,那是記帳自己
  //      壞了,壞掉的記帳不可以拿來赦免任何東西;
  //   3. 乙走通了(link_verified)—— 也就是這一格**確實有東西被量到**。
  //      少了這一條就一律計入:一個什麼都沒驗到的段落綠起來,
  //      比它紅起來危險得多。
  int excused_fails() const {
    return (link_verified_ && any_blind_segment()) ? fails_blind_ : 0;
  }

  // 真的計入結束碼的筆數。
  // ⚠ 有焦點時記下的那些**永遠**在裡面 —— 它們是真的量到了。
  int counted_fails() const {
    return fails_focused_ + fails_blind_ - excused_fails();
  }

  // 「⚠ 按鍵路徑這一次沒有量到」那句話准不准印。
  // 一段都沒量到,而且確實有段落是瞎的 —— 兩個條件缺一不可。
  bool say_nothing_measured() const {
    return !measured() && any_blind_segment();
  }

  // 量到了,但不是每一段都量到。這一句與上面那句互斥。
  bool say_partly_measured() const { return measured() && any_blind_segment(); }

 private:
  int segments_focused_ = 0;
  int segments_blind_ = 0;
  int fails_focused_ = 0;
  int fails_blind_ = 0;
  bool link_verified_ = false;
};

}  // namespace rimewin

#endif  // RIMEWIN_KEYPATH_LEDGER_H_
