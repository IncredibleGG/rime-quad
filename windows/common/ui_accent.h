// windows/common/ui_accent.h — 系統 accent color:取得、階梯、對比守門(§12.14.1)
//
// ── 為什麼整支是純函式 ──────────────────────────────────────────
//
// §12.15 的 W30 要求「accent 解析與衍生是**純函式**,而且每一個衍生角色
// 都過門檻」。理由不是潔癖:accent 是**使用者選的**,我們對它沒有任何
// 保證。Windows 的 accent 挑色盤裡有 #FFB900(黃)與 #00CC6A(亮綠),
// 它們當實心按鈕的底、上面配白字是 3.4:1 與 2.4:1,兩個都不到 4.5。
//
// 而「顏色不夠對比」在畫面上**看起來沒有任何東西是錯的** ——
// 只有算得出來。所以它必須是一支在 Ubuntu 上跑得動的函式,
// 不是一段只有真 Windows 才走得到的碼。
//
// 本檔**不 include windows.h**。登錄檔那兩把讀取住在 service/ui_theme.cc,
// 而它讀回來的位元組交給這裡的 AccentFromPalette()/AccentFromDword() 解析。
// 那條分界就是「測得到」與「測不到」的分界。
//
// ── ⚠ 這一節的產品代價,寫在這裡讓它被看見 ──────────────────────
//
// 跟著系統 accent 之後,LuminaKey 的青瓷綠 #1F6F63 **不出現在 Windows
// 的設定介面裡**(它仍然是安裝程式、圖示與另外三端的識別色)。
// 願意付這個代價的理由是 §12.14.1:核取方塊與單選鈕的**方塊本身**由
// uxtheme 畫、我們改不掉,所以畫面上一定會出現系統 accent 的那一格。
// 其餘地方堅持青瓷綠的結果是同一頁上兩個不相干的彩色。
//
#ifndef RIMEWIN_UI_ACCENT_H_
#define RIMEWIN_UI_ACCENT_H_

#include <cstddef>
#include <cstdint>

#include "ui_palette.h"

namespace rimewin {

// ── 混色(§12.14.1 的階梯公式)──────────────────────────────────
//
//   mix(a, b, t) = round(a + (b − a) * t)     // 每通道
//
// ⚠ GDI 沒有 alpha,所有顏色最後都要是不透明值 —— 這一支就是
//   「疊 14% 的 accent」在 GDI 上唯一畫得出來的形式。
//
// ⚠ **t 是一個整數比例(num/den),不是 double。** 第一版用 double 寫,
//   而 g++ -O1 把 `a + (b−a)*t` 收縮成一次 FMA:中間值不捨入,
//   `120 + 135*0.7` 因此變成 214.5 而不是 214.49999999999997,
//   整個色票有三格差 1/255,而且**同一份原始碼在不同 -O 之下不一樣**。
//   顏色差 1/255 沒有人看得出來,但「同一支程式在兩台機器上算出兩份
//   色票」是一種你永遠查不到的缺陷。整數比例沒有這個問題。
//   捨入是**四捨六入五成雙**(與規格那張表用的 Python round 一致)。
Rgb Mix(Rgb a, Rgb b, int num, int den);

// 七階。只有種子色時,其餘六階由 Mix 產生。
struct AccentLadder {
  Rgb light3, light2, light1, base, dark1, dark2, dark3;
};
AccentLadder LadderOf(Rgb base);

// ── 取得(§12.14.1 三段回落的第 1、2 段的**解析**部分)──────────
//
// 第 1 段:HKCU\...\Explorer\Accent → AccentPalette,REG_BINARY,
//          32 位元組 = 8 組 RGBA。淺色取 index 4(AccentDark1)、
//          深色取 index 1(AccentLight2)。
//
// ⚠ **位元組數不是 32 就整段跳過**(回 false,呼叫端走第 2 段)。
//   那 32 個位元組的佈局是公開流傳的知識,不是文件保證的
//   —— §12.16 第 3 條自承沒有 dump 過。所以退化路徑本身是規格的一部分。
bool AccentFromPalette(const uint8_t* bytes, size_t n, bool dark, Rgb* out);

// 第 2 段:HKCU\Software\Microsoft\Windows\DWM → AccentColor,
//          REG_DWORD,**0x00BBGGRR**(不是 RGB,位元組序相反)。
Rgb AccentFromDword(uint32_t v);

// 第 3 段:都讀不到 → §3.4 的青瓷綠當**種子**,走同一條階梯公式。
// ⚠ 所以淺色的 primary 是 #19594F 而不是 #1F6F63 本身:
//   一條路徑比兩條路徑重要(§12.14.2 的原話)。
Rgb AccentFallbackSeed();

// ── 對比守門(§12.14.1「這一段是本節最重要的一條」)──────────────
//
// 沿著「與 fg 相反」的方向推 base,用 8-bit 量化的二分搜尋找**第一個**
// 過門檻的值。t = 1 是純黑/純白,一定過,所以搜尋不會失敗。
Rgb PushForContrast(Rgb base, Rgb fg, double want);

// 對**兩個底**都要過門檻的版本(指示條、accent 文字)。
// ⚠ §3.4.1 記著一次「只驗一半就宣告過關」的事故(只算了對卡片、
//   忘了對畫面底)。這一支兩個底都算,就是為了不再犯。
Rgb PushForContrastTwo(Rgb base, Rgb bg_a, Rgb bg_b, double want,
                       bool toward_dark);

// ── 衍生(§12.14.2 B 那九格)────────────────────────────────────
struct AccentRoles {
  Rgb primary;                    // 實心底
  Rgb primary_hover;              // 往「與字相反」推 10%
  Rgb primary_pressed;            // 同,18%
  Rgb on_primary;                 // #FFFFFF / #101010 取對比高的
  Rgb accent_text;                // accent 當文字
  Rgb accent_indicator;           // 側欄選中項左緣那條 3×16
  Rgb container;                  // 選中底
  Rgb container_hover;
  Rgb container_pressed;
};

// seed = 系統 accent(或退路種子)。surface/background/on_surface 是
// 中性色票裡的三個角色 —— 守門要對它們算,所以它們是輸入。
AccentRoles DeriveAccentRoles(Rgb seed, bool dark, Rgb surface, Rgb background,
                              Rgb on_surface);

}  // namespace rimewin

#endif  // RIMEWIN_UI_ACCENT_H_
