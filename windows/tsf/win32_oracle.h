// windows/tsf/win32_oracle.h — 用 ToUnicodeEx 問「這個佈局上這顆鍵是什麼字」
#ifndef RIMEWIN_TSF_WIN32_ORACLE_H_
#define RIMEWIN_TSF_WIN32_ORACLE_H_

#include <windows.h>

#include "../common/keymap.h"

namespace rimewin {

// 綁定一個 HKL。HKL 換了(使用者切鍵盤佈局)就換一個實例 ——
// AltGr 的有無是整個佈局的性質,快取在裡面。
//
// ══ 為什麼建構子裡有一段「這個 HKL 答不答得出東西」的檢查 ═══════════
//
// 呼叫端給的 HKL 來自 `GetKeyboardLayout(0)`,而**我們的文字服務啟用時,
// 那個值不保證是一份真的鍵盤佈局**。TSF 的文字服務(TIP)在系統裡也有
// 自己的 HKL,形狀是 0xFxxx<langid>(高位字的最高四個位元是 1);
// IMM32 那一代的 IME 則是 0xExxx<langid>。這兩種都**不是**鍵盤佈局的
// 控制代碼 —— 拿去餵 ToUnicodeEx,它一個字都不會給。
//
// 而「一個字都不會給」在這一整條路上的後果是災難性的,因為它是靜默的:
//
//     ToUnicodeEx 回 0 → MapKey 回 keysym == 0
//       → OnTestKeyDown 直接回 S_OK 且 *eaten = FALSE
//         → OnKeyDown 根本不會被呼叫
//           → **引擎一顆鍵都收不到,連線也永遠不會被建立**
//             → 服務進程不會被啟動 → 系統匣圖示與設定視窗也不會出現
//
// 也就是說使用者看到的是「一個字都打不出來」**而且**「沒有任何 UI」——
// 兩個看起來無關的症狀,同一個根因。而且沒有任何錯誤訊息:每一層都
// 「正常地」回報成功,只是誰都沒事做。
//
// 所以這裡的做法是:建構時實地問幾顆一定有字的鍵。問不出來就換一份
// **真的**佈局來問(見 cc 檔的 PickWorkingLayout)。
//
// ⚠ 換的仍然是「問一份真的佈局」,不是退回「VK_A 就當 'a'」。
//   後者在 QWERTY 上看起來完全正確,而 Dvorak 使用者打出來的每一個字
//   都是錯的 —— 那正是 common/keymap.cc 整個檔案要避免的東西。
class Win32KeyboardOracle : public KeyboardOracle {
 public:
  explicit Win32KeyboardOracle(HKL hkl);

  char32_t Translate(uint32_t vk, uint32_t scan_code, bool shift, bool caps,
                     bool altgr, bool* out_is_dead) const override;
  bool HasAltGr() const override { return has_altgr_; }

  // 實際拿去問 ToUnicodeEx 的那一個(可能已經被換過)。
  HKL hkl() const { return hkl_; }
  // 呼叫端原本給的那一個。
  HKL requested_hkl() const { return requested_hkl_; }
  // 原本那個問不出字,已經換成別的。診斷用 —— 這件事一旦發生,
  // 使用者機器上的狀況就與我們在 CI 上測的不一樣,值得記一行。
  bool used_fallback() const { return hkl_ != requested_hkl_; }
  // 連換過之後都問不出字。到這一步輸入法必定不能用,而**知道它不能用**
  // 比安靜地不能用好太多:doctor 會把這一格印成 FAIL。
  bool blind() const { return blind_; }

  // 給診斷工具用:這個 HKL 問得出字嗎(不建物件、沒有副作用)。
  static bool LayoutAnswers(HKL hkl);
  // 這個 HKL 看起來像不像文字服務/IME 的假控制代碼而不是鍵盤佈局。
  static bool LooksLikeTextServiceHkl(HKL hkl);

 private:
  bool DetectAltGr() const;

  HKL requested_hkl_;
  HKL hkl_;
  bool has_altgr_;
  bool blind_;
};

}  // namespace rimewin

#endif  // RIMEWIN_TSF_WIN32_ORACLE_H_
