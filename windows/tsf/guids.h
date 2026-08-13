// windows/tsf/guids.h — 本輸入法自己的 GUID
//
// 這些值一旦發布出去就不能改:改了等於變成另一個輸入法,使用者原本
// 選好的那一個會從語言列上消失,而且舊的那筆註冊沒有東西去清掉它。
//
// ══ 2026-08-09:改名 LuminaKey 時**刻意**換掉了四個 ═══════════════
//
//   CLSID_RimeTextService、GUID_RimeProfile、…Hans、…HK 全部是新值。
//
//   為什麼非換不可:COM 的類別識別碼與語言設定檔 GUID 是這個輸入法在
//   系統上的**身分**。沿用舊值而只換顯示名,使用者機器上會出現一個
//   「名字變了但還是同一筆註冊」的東西 —— 而舊版的解除安裝程式仍然
//   認得它、會去反註冊它,新版卻是從另一個 AppId 裝上去的。兩邊互相
//   看不見對方,卻共用同一個 CLSID,誰先解除安裝誰就把對方也拆了。
//
//   換掉的後果(**必須向使用者說清楚**):
//     · 新版與舊版在系統眼裡是**兩個不同的輸入法**,可以同時存在;
//     · 「新增或移除程式」裡會同時出現兩筆(AppId 也換了);
//     · 舊版留下的 COM 註冊不會被新版清掉 —— 我們**不去刪別人的登錄檔**,
//       那是在猜另一個產品的東西。
//
//   所以正確的升級步驟是:
//     1. 先用**舊版**的解除安裝程式移除舊版(它認得自己的註冊);
//     2. 依提示**重新開機**(瘦 DLL 被宿主進程握著,要開機才刪得掉);
//     3. 再安裝新版。
//   使用者資料(%APPDATA% 底下的詞典與設定)在步驟 1 預設不會被刪,
//   但資料夾名也跟著改名了 —— 見 windows/README.md 的升級章節。
#ifndef RIMEWIN_TSF_GUIDS_H_
#define RIMEWIN_TSF_GUIDS_H_

#include <windows.h>

// 文字服務的 COM class。
// {7D02992E-B213-4E06-B62E-CCC6338DA98A}
extern const CLSID CLSID_RimeTextService;

// 輸入法設定檔(語言列上的那一項)。**每一個語言各一個。**
// {4F78BA11-E997-4BD7-8B97-F4553ABC0B18}  zh-Hant-TW(0x0404)
extern const GUID GUID_RimeProfile;
// {84420A61-0A08-4A68-9D60-292EFD31C7BC}  zh-Hans-CN(0x0804)
extern const GUID GUID_RimeProfileHans;
// {C6B736EB-38E3-4041-B59B-ECF91AD8E28A}  zh-Hant-HK(0x0C04)
extern const GUID GUID_RimeProfileHK;

// 預留:顯示屬性(組字底線)與保留鍵。本輪未實作,但先把值定下來,
// 免得日後補上時得換 GUID —— 換 GUID 就是換一個新的輸入法。
// {7A599152-062F-467B-A024-A30EB287BDED}
extern const GUID GUID_RimeDisplayAttributeInput;
// {5C43BAAB-FD30-4D57-AEC3-0D8E2404D67A}
extern const GUID GUID_RimePreservedKeyToggle;
// 簡繁切換的保留鍵(Ctrl+Shift+F,G76)。
// {3F6A1D28-9C4B-4A7E-B5D3-8E21C0F47A96}
extern const GUID GUID_RimePreservedKeyVariant;

// 語言列(與工作列輸入指示器)上的那一顆「設定」按鈕。
// {9E2D5B41-0C7A-4E6E-9E52-5F2B8B1C6A44}
extern const GUID GUID_RimeLangBarButton;

// COM 類別本身的描述(HKCR\CLSID\{…} 的預設值)。使用者看不到這一個。
#define RIME_TEXT_SERVICE_DESC L"LuminaKey 輸入法"

// ── 註冊在哪些語言底下 ────────────────────────────────────────────
//
// ⚠ **Windows 的語言標籤與實際打出簡體還是繁體是兩件事,不要混在一起。**
//
//   · 語言列上顯示成「繁體中文(台灣)」還是「簡體中文(中國)」,
//     完全由這裡註冊的 langid 決定。
//   · 實際上屏的是簡體還是繁體字,由 RIME 的方案
//     (luna_pinyin vs luna_pinyin_tw)與簡繁開關決定,與 langid 無關。
//
// 第一版只註冊 0x0404(zh-Hant-TW),理由是內建方案是 luna_pinyin_tw /
// bopomofo_tw。結果:**系統語言是簡體中文的使用者,在自己的語言底下找不到
// 這個輸入法** —— 它出現在「繁体中文(中国台湾)」那一欄底下。
// 使用者實際回報過。所以現在每一個中文語言各註冊一份。
//
// 每一份 profile 有自己的 GUID:(clsid, langid, guidProfile) 三元組才是
// TSF 認的鍵,共用一個 GUID 雖然也行得通,但「使用者選的是哪一個」就變得
// 問不清楚,而類別註冊也會分不出是替誰註冊的。
//
// 描述字串跟著語言走:簡體使用者的清單上不該出現一串繁體字,
// 那正是這次被回報的問題的一半。
//
// 刻意**沒有**註冊 zh-SG(0x1004)與 zh-MO(0x1404):每多一份 profile,
// 有該語言的使用者清單上就多一項,而這兩個語言的使用者數量與清單雜訊
// 不成比例。要加的話成本只有一個 GUID 加一列,隨時可以加。
//
// ⚠ GUID 與這張表一旦發布出去就不能改。改了等於變成另一個輸入法,
//   使用者原本選好的那一個會從清單上消失,而且舊的那筆註冊沒有東西去清掉它。
//   (2026-08-09 的改名是**明著**做了這件事一次,見本檔檔頭;
//    從現在起這條規矩重新生效。)
struct RimeProfileDef {
  LANGID langid;
  const GUID* guid;
  const wchar_t* description;
};
extern const RimeProfileDef kRimeProfiles[];
extern const int kRimeProfileCount;

#endif  // RIMEWIN_TSF_GUIDS_H_
