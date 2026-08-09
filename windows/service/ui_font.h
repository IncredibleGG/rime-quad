// windows/service/ui_font.h — 字型解析(§12.8)
//
// ── ⚠ 這是 `windows/` 底下唯一允許呼叫 CreateFontIndirectW 的地方 ──
//
// W6 在守這件事,理由是下面這個陷阱:
//
//   **`CreateFontIndirectW` 永遠不會失敗。**
//   `lfFaceName` 給一個不存在的字體,它回一個**有效的 HFONT**,
//   然後 GDI 安靜地替你挑一個「相近」的字體。
//   也就是說**回傳值不能拿來判斷字體在不在**。
//
// 散在各處呼叫的結果是:每一處都以為自己拿到了想要的字體,而實際上
// 有幾處拿到的是 GDI 隨手挑的。症狀是「有些地方的字長得不一樣」,
// 而沒有任何一行程式碼看起來是錯的。唯一可靠的存在性檢查是
// `EnumFontFamiliesExW`,而它貴(每次列舉整台機器的字體),所以要快取 ——
// 兩件事都只該做一次,所以只該有一個地方做。
//
// ── 為什麼不用 SPI_GETNONCLIENTMETRICS(現況違規)──────────────
//
// §8.6.0 明文禁止「桌面端改用系統 UI 字型當預設」。理由不是美觀:
// `$system` 代號已經是「用系統字體」的意思,而**走代號才能讓
// 「主題指定了字體」與「主題沒指定」是同一條程式路徑** ——
// 兩條路的話,主題一旦指定字體,那條沒被走過的路就開始腐爛。
//
// ⚠ 順帶修掉一個計算錯誤:舊碼把 `lfMessageFont.lfHeight` 再乘一次
//   `dpi_scale_`。在 per-monitor-v2 進程裡,`SPI_GETNONCLIENTMETRICS`
//   回的是**系統 DPI** 的度量,已經含一次縮放 —— 系統 DPI > 96 時
//   那是二次縮放,字會過大。(規格說這一條是從 API 契約推的、沒在真機量過;
//   Ubuntu 上的 mingw 標頭裡有 `SystemParametersInfoForDpi` 這支
//   **明確帶 dpi 參數**的變體,那正好佐證不帶參數的那一支回的是系統 DPI 的值。
//   仍然沒有在高 DPI 機器上量過字高 —— 見報告的「只有人看得到」那一節。)
//   走下面的解析順序之後這個問題自然消失,因為字級直接來自規範的 DIP 值。
//
#ifndef RIMEWIN_SERVICE_UI_FONT_H_
#define RIMEWIN_SERVICE_UI_FONT_H_

#include <windows.h>

#include <map>
#include <string>

namespace rimewin {

// §8.4.2 的 script_of(run)。設定視窗用介面語言決定,候選窗用
// `rs_status.is_simplified` 決定(§8.4.2 第 5 條,四端必須一致)。
enum class Script { kLatin, kHant, kHans };

// §8.6.0 的字體綁定。名字對應 typography.fonts.* 的四個堆疊 + $mono。
enum class FontRole { kUi, kCandidate, kLabel, kComment, kMono };

// 字體在不在。**唯一可靠的存在性檢查**,結果會快取 ——
// EnumFontFamiliesExW 每一次都會列舉整台機器的字體,放在 WM_PAINT 裡
// 等於每一幀掃一次字體清單。
bool FontExists(const wchar_t* family);

// 依 §12.8.2 的順序解析出一個真的存在的家族名。
// 全部不存在時回**空字串** —— 不是硬塞 "Segoe UI"。空字串下 GDI 依
// lfPitchAndFamily 挑,在中文版 Windows 上挑出來的東西比我們猜的好。
std::wstring ResolveFamily(FontRole role, Script script);

// 一個 DPI 底下的一組字型。⚠ 字型是**像素**單位的,所以 WM_DPICHANGED
// 時整組要重建 —— 不重建的結果是模糊或錯大小,而那看起來只像「有點糊」。
class FontSet {
 public:
  ~FontSet() { Clear(); }

  void Reset(UINT dpi, Script script);
  void Clear();

  // size_dip 取 §3.2 桌面欄的字級(t1 22 / t2 15 / t3 13 / t4 12 / t5 11)。
  HFONT Get(int size_dip, bool semibold = false, FontRole role = FontRole::kUi);

  UINT dpi() const { return dpi_; }
  Script script() const { return script_; }

 private:
  struct Key {
    int size_dip;
    int weight;
    int role;
    bool operator<(const Key& o) const {
      if (size_dip != o.size_dip) return size_dip < o.size_dip;
      if (weight != o.weight) return weight < o.weight;
      return role < o.role;
    }
  };
  UINT dpi_ = 96;
  Script script_ = Script::kHant;
  std::map<Key, HFONT> cache_;
};

}  // namespace rimewin

#endif  // RIMEWIN_SERVICE_UI_FONT_H_
