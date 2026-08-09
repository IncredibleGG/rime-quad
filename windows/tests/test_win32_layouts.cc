// windows/tests/test_win32_layouts.cc — 拿**真的** Windows 鍵盤佈局測映射
//
// 前面 test_keymap.cc 用的是離線複本,測的是 MapKey 的邏輯。這一支不同:
// 它用 LoadKeyboardLayout 載入系統內建的美式 / 德文 / 法文佈局,
// 再走 ToUnicodeEx —— 也就是使用者實際會走的那條路。
//
// 這是 CI 上唯一能驗到「Win32KeyboardOracle 真的問對了佈局」的辦法。
// 一張寫死的 VK→字元表在這裡會紅,而且是在 GitHub 的美式 runner 上就會紅。
//
// ⚠ 沒有 SKIP。載不到佈局就是失敗。
//   kbdus / kbdgr / kbdfr 是 Windows 內建的,任何一份載不到都代表環境
//   與我們以為的不同 —— 那件事本身就該被看見,不該被一句 "skipped" 蓋掉。
//   (這個專案已經因為「測試安靜地跳過自己」吃過虧。)

#include <windows.h>

#include "../tsf/win32_oracle.h"
#include "check.h"

using namespace rimewin;

namespace {

HKL Load(const wchar_t* id) {
  // 不帶 KLF_ACTIVATE:只要載入,不要動使用者(或 runner)目前的佈局。
  return ::LoadKeyboardLayoutW(id, KLF_NOTELLSHELL);
}

char32_t Ch(const Win32KeyboardOracle& o, UINT vk, bool shift = false,
            bool altgr = false) {
  const UINT scan = ::MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, o.hkl());
  bool dead = false;
  return o.Translate(vk, scan, shift, /*caps=*/false, altgr, &dead);
}

constexpr UINT kVkQ = 0x51, kVk2 = 0x32, kVkOem1 = 0xBA, kVkOem3 = 0xC0,
               kVkOem7 = 0xDE;

}  // namespace

TEST(win32_us_layout_baseline) {
  HKL us = Load(L"00000409");
  CHECK(us != nullptr);
  if (!us) return;
  Win32KeyboardOracle o(us);
  CHECK_INT(Ch(o, kVkOem1), U';');
  CHECK_INT(Ch(o, kVkOem1, /*shift=*/true), U':');
  CHECK_INT(Ch(o, kVk2), U'2');
  CHECK_INT(Ch(o, kVk2, true), U'@');
  // 美式佈局沒有 AltGr 層。判成有的話,使用者真的按 Ctrl+Alt 時修飾鍵會被吞掉。
  CHECK(!o.HasAltGr());
}

TEST(win32_german_layout_differs_from_us) {
  HKL de = Load(L"00000407");
  CHECK(de != nullptr);
  if (!de) return;
  Win32KeyboardOracle o(de);
  // 德文 QWERTZ:這三顆 OEM 鍵上印的是變母音,不是美式的 ; ` '。
  // Rime 的 punctuator 大量使用這幾顆鍵 —— 寫死等於把標點全部對錯。
  CHECK_INT(Ch(o, kVkOem1), 0x00FC);  // ü
  CHECK_INT(Ch(o, kVkOem3), 0x00F6);  // ö
  CHECK_INT(Ch(o, kVkOem7), 0x00E4);  // ä
  CHECK_INT(Ch(o, kVkOem1, /*shift=*/true), 0x00DC);  // Ü

  // AltGr 層存在,而且 AltGr+Q 是 @。
  CHECK(o.HasAltGr());
  CHECK_INT(Ch(o, kVkQ, /*shift=*/false, /*altgr=*/true), U'@');
}

TEST(win32_french_layout_digit_row_is_not_digits) {
  HKL fr = Load(L"0000040C");
  CHECK(fr != nullptr);
  if (!fr) return;
  Win32KeyboardOracle o(fr);
  HKL us = Load(L"00000409");
  CHECK(us != nullptr);
  if (!us) return;
  Win32KeyboardOracle ous(us);

  // AZERTY 的數字列不按 Shift 打出來的是字母。這裡不釘死是哪一個字元
  // (各版本的法文佈局細節有差),只斷言**與美式不同**且不是數字 ——
  // 那正是「寫死 VK_2 → '2'」會踩到的。
  const char32_t fr2 = Ch(o, kVk2);
  const char32_t us2 = Ch(ous, kVk2);
  CHECK_INT(us2, U'2');
  CHECK(fr2 != us2);
  CHECK(fr2 < U'0' || fr2 > U'9');
  // 按了 Shift 才是數字。
  CHECK_INT(Ch(o, kVk2, /*shift=*/true), U'2');
  CHECK(o.HasAltGr());
}

TEST(win32_mapkey_end_to_end_with_real_german_layout) {
  // 從 MapKey 這一層看:同一顆 VK,美式與德文必須給出不同的 keysym。
  HKL de = Load(L"00000407");
  HKL us = Load(L"00000409");
  CHECK(de != nullptr && us != nullptr);
  if (!de || !us) return;
  Win32KeyboardOracle ode(de), ous(us);

  KeyEvent e;
  e.vk = kVkOem1;
  e.scan_code = ::MapVirtualKeyExW(kVkOem1, MAPVK_VK_TO_VSC, us);
  CHECK_INT(MapKey(e, ous).keysym, U';');
  e.scan_code = ::MapVirtualKeyExW(kVkOem1, MAPVK_VK_TO_VSC, de);
  CHECK_INT(MapKey(e, ode).keysym, 0x00FC);  // ü 的 keysym 就是碼點(Latin-1)

  // AltGr:keysym 要是 '@',而且回報的 modifier 不可以帶 Control/Alt。
  KeyEvent g;
  g.vk = kVkQ;
  g.scan_code = ::MapVirtualKeyExW(kVkQ, MAPVK_VK_TO_VSC, de);
  g.ctrl = true;
  g.alt = true;
  g.right_alt = true;
  const MappedKey m = MapKey(g, ode);
  CHECK_INT(m.keysym, U'@');
  CHECK_INT(m.modifiers & kModControl, 0u);
  CHECK_INT(m.modifiers & kModAlt, 0u);
}

// ══════════════════════════════════════════════════════════════════════
//  問不出字的 HKL
// ══════════════════════════════════════════════════════════════════════
//
// ⚠ 這一組測試對應的是使用者實際回報的「一個字都打不出來,而且沒有任何 UI」。
//
// 我們的文字服務被啟用時,`GetKeyboardLayout(0)` 拿到的**不保證是一份真的
// 鍵盤佈局**:TSF 的文字服務(TIP)在系統裡也有自己的 HKL,形狀是
// 0xFxxx<langid>;IMM32 那一代的 IME 是 0xExxx<langid>。兩者都不是鍵盤佈局,
// ToUnicodeEx 對它們一個字都不會給。
//
// 而那件事的後果是靜默且災難性的:
//   ToUnicodeEx 回 0 → MapKey 回 keysym 0 → OnTestKeyDown 不吃這顆鍵
//   → OnKeyDown 根本不會被呼叫 → 引擎收不到任何按鍵 → 連線永遠不建立
//   → **服務進程不會被啟動** → 系統匣圖示與設定視窗也不會出現。
//
// 也就是「打不出中文」與「沒有任何 UI」是**同一個根因**。
//
// 這裡不能用真的 TIP HKL 來測(runner 上不保證有任何 TIP),所以用一個
// 一定不存在的控制代碼 —— 對 ToUnicodeEx 而言,結果是一樣的:什麼都問不到。

TEST(win32_fake_tip_hkl_is_recognised) {
  // 高位字 0xF0xx = TSF 的文字服務;0xE0xx = IMM32 的 IME。
  CHECK(Win32KeyboardOracle::LooksLikeTextServiceHkl(
      reinterpret_cast<HKL>(static_cast<UINT_PTR>(0xF0DE0404u))));
  CHECK(Win32KeyboardOracle::LooksLikeTextServiceHkl(
      reinterpret_cast<HKL>(static_cast<UINT_PTR>(0xE0200804u))));
  // 真的鍵盤佈局不可以被誤判 —— 誤判的話我們會放著好好的佈局不問。
  HKL us = Load(L"00000409");
  CHECK(us != nullptr);
  if (!us) return;
  CHECK(!Win32KeyboardOracle::LooksLikeTextServiceHkl(us));
  CHECK(Win32KeyboardOracle::LayoutAnswers(us));
}

TEST(win32_blind_hkl_falls_back_to_a_real_layout) {
  // 一個不存在的控制代碼。ToUnicodeEx 對它什麼都問不到。
  HKL bogus = reinterpret_cast<HKL>(static_cast<UINT_PTR>(0xF0DE0404u));
  CHECK(!Win32KeyboardOracle::LayoutAnswers(bogus));

  Win32KeyboardOracle o(bogus);
  // 換掉了,而且換到的那一份問得出字。
  CHECK(o.used_fallback());
  CHECK(!o.blind());
  CHECK(o.hkl() != bogus);

  // 而且真的翻得出東西 —— 這一條才是使用者感受得到的那一件事。
  bool dead = false;
  const char32_t a = o.Translate(0x41, 0, /*shift=*/false, /*caps=*/false,
                                 /*altgr=*/false, &dead);
  CHECK_INT(a, U'a');

  // 從 MapKey 那一層再看一次:keysym 不可以是 0。
  // keysym == 0 就是「這顆鍵原樣放行、永遠進不了引擎」。
  KeyEvent e;
  e.vk = 0x41;
  e.scan_code = 0;  // 有些宿主的 lParam 不帶掃描碼,那時要自己推
  CHECK_INT(MapKey(e, o).keysym, U'a');
}

TEST(win32_good_hkl_does_not_fall_back) {
  // 反向:佈局本來就好好的時候**不可以**換掉它。
  // 換掉的話,Dvorak / 德文 / 法文使用者會拿到美式的映射,
  // 而那是這整個檔案存在的理由所要避免的事。
  HKL de = Load(L"00000407");
  CHECK(de != nullptr);
  if (!de) return;
  Win32KeyboardOracle o(de);
  CHECK(!o.used_fallback());
  CHECK(!o.blind());
  CHECK(o.hkl() == de);
  CHECK_INT(Ch(o, kVkOem1), 0x00FC);  // 仍然是 ü,沒有被換成美式的 ;
}
