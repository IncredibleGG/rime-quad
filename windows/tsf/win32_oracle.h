// windows/tsf/win32_oracle.h — 用 ToUnicodeEx 問「這個佈局上這顆鍵是什麼字」
#ifndef RIMEWIN_TSF_WIN32_ORACLE_H_
#define RIMEWIN_TSF_WIN32_ORACLE_H_

#include <windows.h>

#include "../common/keymap.h"

namespace rimewin {

// 綁定一個 HKL。HKL 換了(使用者切鍵盤佈局)就換一個實例 ——
// AltGr 的有無是整個佈局的性質,快取在裡面。
class Win32KeyboardOracle : public KeyboardOracle {
 public:
  explicit Win32KeyboardOracle(HKL hkl);

  char32_t Translate(uint32_t vk, uint32_t scan_code, bool shift, bool caps,
                     bool altgr, bool* out_is_dead) const override;
  bool HasAltGr() const override { return has_altgr_; }

  HKL hkl() const { return hkl_; }

 private:
  bool DetectAltGr() const;

  HKL hkl_;
  bool has_altgr_;
};

}  // namespace rimewin

#endif  // RIMEWIN_TSF_WIN32_ORACLE_H_
