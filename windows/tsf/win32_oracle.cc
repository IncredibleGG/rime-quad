#include "win32_oracle.h"

namespace rimewin {
namespace {

// ToUnicodeEx 的 wFlags 第 2 位元(值 4)= 「不要改動鍵盤狀態」。
//
// ⚠ 這個旗標不是可有可無的。ToUnicodeEx 預設會**消費掉核心裡的死鍵狀態**:
//   使用者按了法文的 ^ 之後,我們為了查表而呼叫一次 ToUnicodeEx,
//   那個待組合的 ^ 就被吃掉了,接著按 e 打出來的是 e 而不是 ê。
//   症狀是「裝了這個輸入法之後,別的語言的重音字打不出來」,
//   而且與輸入法本身的功能毫無關聯,幾乎不可能聯想到這裡。
//
//   這個旗標從 Windows 10 1607 起支援。更早的版本上它會被忽略,
//   死鍵狀態仍會被動到 —— 那是已知且已接受的下限。
constexpr UINT kNoKeyboardStateChange = 0x4;

char32_t FromBuffer(const wchar_t* buf, int n) {
  if (n <= 0) return 0;
  if (n >= 2 && buf[0] >= 0xD800 && buf[0] <= 0xDBFF && buf[1] >= 0xDC00 &&
      buf[1] <= 0xDFFF) {
    return 0x10000 + ((static_cast<char32_t>(buf[0]) - 0xD800) << 10) +
           (static_cast<char32_t>(buf[1]) - 0xDC00);
  }
  return static_cast<char32_t>(buf[0]);
}

}  // namespace

Win32KeyboardOracle::Win32KeyboardOracle(HKL hkl) : hkl_(hkl), has_altgr_(false) {
  has_altgr_ = DetectAltGr();
}

char32_t Win32KeyboardOracle::Translate(uint32_t vk, uint32_t scan_code,
                                        bool shift, bool caps, bool altgr,
                                        bool* out_is_dead) const {
  *out_is_dead = false;

  // 自己組一張乾淨的鍵盤狀態表,而不是拿 GetKeyboardState 的結果來改。
  //
  // 理由:真實狀態裡可能有 Ctrl 被按著,而 Ctrl+A 產生的是控制碼 0x01,
  // 不是 'a'。引擎要的是「哪一顆鍵」加「按著什麼修飾鍵」兩件分開的事實。
  BYTE state[256] = {0};
  if (shift) {
    state[VK_SHIFT] = 0x80;
    state[VK_LSHIFT] = 0x80;
  }
  if (caps) state[VK_CAPITAL] = 0x01;  // toggle 在低位元,不是 0x80
  if (altgr) {
    // Windows 的 AltGr = 左 Ctrl + 右 Alt。兩者都要,少一個就拿不到第三層。
    state[VK_CONTROL] = 0x80;
    state[VK_LCONTROL] = 0x80;
    state[VK_MENU] = 0x80;
    state[VK_RMENU] = 0x80;
  }

  wchar_t buf[8] = {0};
  const int n = ::ToUnicodeEx(vk, scan_code, state, buf,
                              static_cast<int>(sizeof(buf) / sizeof(buf[0])),
                              kNoKeyboardStateChange, hkl_);
  if (n < 0) {
    *out_is_dead = true;
    return FromBuffer(buf, 1);
  }
  return FromBuffer(buf, n);
}

bool Win32KeyboardOracle::DetectAltGr() const {
  // Windows 沒有「這個佈局有沒有 AltGr」的 API。做法是實地問一遍:
  // 掃過所有可能產生字元的 VK,只要有任何一顆在 Ctrl+Alt 之下給出
  // 可列印字元,這個佈局就有 AltGr 層。
  //
  // 猜錯的代價是不對稱的,所以寧可**寧缺勿濫**:
  //   · 該有卻判成沒有 → 德文使用者的 AltGr+Q 被當成 Ctrl+Alt+q,打不出 @。
  //   · 沒有卻判成有   → 使用者真的按 Ctrl+Alt 時,修飾鍵被吞掉,快捷鍵失效。
  // 兩者都是實在的傷害,所以用「真的問出一個字元」當條件,不用啟發式。
  BYTE state[256] = {0};
  state[VK_CONTROL] = 0x80;
  state[VK_LCONTROL] = 0x80;
  state[VK_MENU] = 0x80;
  state[VK_RMENU] = 0x80;

  for (UINT vk = 0x30; vk <= 0xFF; ++vk) {
    const UINT scan = ::MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, hkl_);
    if (scan == 0) continue;
    wchar_t buf[8] = {0};
    const int n = ::ToUnicodeEx(vk, scan, state, buf, 8, kNoKeyboardStateChange, hkl_);
    if (n == 0) continue;
    const wchar_t c = buf[0];
    // 控制碼不算:沒有 AltGr 的佈局在 Ctrl 之下就是給控制碼。
    if (n < 0 || (c >= 0x20 && c != 0x7F)) return true;
  }
  return false;
}

}  // namespace rimewin
