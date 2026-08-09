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

// 拿來試探「這個 HKL 答不答得出東西」的鍵。
//
// 挑選的條件是「在**每一種**鍵盤佈局上都一定產生字元」:
//   · A / S / K:字母列。連 Dvorak、法文 AZERTY、德文 QWERTZ 上都有字
//     (只是位置不同,而位置正是我們要問的東西)。
//   · 1 / 5:數字列。法文佈局上它們是 & 與 (,一樣是可列印字元。
// 少於三顆的話,某一顆剛好在某個冷門佈局上是死鍵時就會誤判。
constexpr UINT kProbeVks[] = {0x41, 0x53, 0x4B, 0x31, 0x35};

bool ProbeOne(HKL hkl, UINT vk) {
  const UINT scan = ::MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, hkl);
  BYTE state[256] = {0};
  wchar_t buf[8] = {0};
  const int n = ::ToUnicodeEx(vk, scan, state, buf, 8, kNoKeyboardStateChange, hkl);
  if (n < 0) return true;  // 死鍵也算「答得出來」
  if (n == 0) return false;
  const wchar_t c = buf[0];
  return c >= 0x20 && c != 0x7F;
}

// 找一份**真的**鍵盤佈局來問。
//
// 順序:
//   1. 這條執行緒/這個進程目前掛著的佈局清單(GetKeyboardLayoutList)。
//      優先挑語言與原本那一個相同的 —— 使用者用的是中文輸入法時,
//      他底下的那份實體佈局多半就掛在同一個語言下。
//   2. 清單裡任何一份答得出來的。
//   3. 明著載入美式佈局。KLF_NOTELLSHELL:只載入,**不動使用者目前的佈局**。
//      走到這一步代表這台機器的佈局清單很不尋常,但美式佈局是 Windows
//      內建的,一定載得到。此時 Dvorak 使用者會拿到 QWERTY 的映射 ——
//      不理想,但那是「輸入法完全不能用」與「按鍵位置不合他的鍵帽」
//      之間的選擇,而後者他至少看得出來、也修得掉(切一次佈局)。
HKL PickWorkingLayout(HKL requested) {
  const WORD want_lang = static_cast<WORD>(
      reinterpret_cast<UINT_PTR>(requested) & 0xFFFFu);

  HKL list[64] = {0};
  const UINT n = ::GetKeyboardLayoutList(64, list);

  for (int pass = 0; pass < 2; ++pass) {
    for (UINT i = 0; i < n && i < 64; ++i) {
      HKL h = list[i];
      if (!h || h == requested) continue;
      if (Win32KeyboardOracle::LooksLikeTextServiceHkl(h)) continue;
      const WORD lang = static_cast<WORD>(reinterpret_cast<UINT_PTR>(h) & 0xFFFFu);
      if (pass == 0 && lang != want_lang) continue;
      if (Win32KeyboardOracle::LayoutAnswers(h)) return h;
    }
  }

  HKL us = ::LoadKeyboardLayoutW(L"00000409", KLF_NOTELLSHELL);
  if (us && Win32KeyboardOracle::LayoutAnswers(us)) return us;
  return requested;
}

}  // namespace

bool Win32KeyboardOracle::LooksLikeTextServiceHkl(HKL hkl) {
  // 鍵盤佈局的 HKL 高位字是「佈局 id」,實務上落在 0x0000–0xF000 之外的
  // 特殊區段有兩個:
  //   0xExxx  IMM32 那一代的 IME(注音、微軟拼音的舊介面…)
  //   0xFxxx  TSF 的文字服務(TIP)—— **我們自己就是這一種**
  // 這兩種都不是鍵盤佈局,ToUnicodeEx 對它們沒有意義。
  const UINT_PTR v = reinterpret_cast<UINT_PTR>(hkl);
  const WORD hi = static_cast<WORD>((v >> 16) & 0xFFFFu);
  return (hi & 0xF000u) == 0xF000u || (hi & 0xF000u) == 0xE000u;
}

bool Win32KeyboardOracle::LayoutAnswers(HKL hkl) {
  if (!hkl) return false;
  for (UINT vk : kProbeVks)
    if (ProbeOne(hkl, vk)) return true;
  return false;
}

Win32KeyboardOracle::Win32KeyboardOracle(HKL hkl)
    : requested_hkl_(hkl), hkl_(hkl), has_altgr_(false), blind_(false) {
  if (!LayoutAnswers(hkl_)) {
    hkl_ = PickWorkingLayout(hkl);
    blind_ = !LayoutAnswers(hkl_);
  }
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

  // ⚠ 掃描碼也要跟著 hkl_ 走。
  //
  // 呼叫端算掃描碼時用的是**宿主送來的 lParam**,那是實體鍵盤給的,
  // 與佈局無關,所以原樣可用 —— 但只有在 scan_code 真的有值的時候。
  // 有些宿主(以及我們自己的測試工具)送過來的 lParam 沒有掃描碼,
  // 那時得自己從 VK 推,而推的時候要用**實際在查的那一份佈局**。
  UINT scan = static_cast<UINT>(scan_code);
  if (scan == 0) scan = ::MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, hkl_);

  wchar_t buf[8] = {0};
  const int n = ::ToUnicodeEx(vk, scan, state, buf,
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
