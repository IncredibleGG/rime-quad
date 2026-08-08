// windows/tsf/guids.h — 本輸入法自己的 GUID
//
// 這些值一旦發布出去就不能改:改了等於變成另一個輸入法,使用者原本
// 選好的那一個會從語言列上消失,而且舊的那筆註冊沒有東西去清掉它。
#ifndef RIMEWIN_TSF_GUIDS_H_
#define RIMEWIN_TSF_GUIDS_H_

#include <windows.h>

// 文字服務的 COM class。
// {E94B9FC2-6730-45AD-A462-B7D02995D95B}
extern const CLSID CLSID_RimeTextService;

// 輸入法設定檔(語言列上的那一項)。**每一個語言各一個。**
// {07FB3057-4192-4868-AB6E-E4EE5597C0FE}  zh-Hant-TW(0x0404)
extern const GUID GUID_RimeProfile;
// {57BE9E4D-3F4E-4B4F-959B-E85E6095F2CA}  zh-Hans-CN(0x0804)
extern const GUID GUID_RimeProfileHans;
// {23BBABB2-5C8A-4751-85F1-B360C70A5637}  zh-Hant-HK(0x0C04)
extern const GUID GUID_RimeProfileHK;

// 預留:顯示屬性(組字底線)與保留鍵。本輪未實作,但先把值定下來,
// 免得日後補上時得換 GUID —— 換 GUID 就是換一個新的輸入法。
// {7A599152-062F-467B-A024-A30EB287BDED}
extern const GUID GUID_RimeDisplayAttributeInput;
// {5C43BAAB-FD30-4D57-AEC3-0D8E2404D67A}
extern const GUID GUID_RimePreservedKeyToggle;

// COM 類別本身的描述(HKCR\CLSID\{…} 的預設值)。使用者看不到這一個。
#define RIME_TEXT_SERVICE_DESC L"RIME 四端輸入法"

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
struct RimeProfileDef {
  LANGID langid;
  const GUID* guid;
  const wchar_t* description;
};
extern const RimeProfileDef kRimeProfiles[];
extern const int kRimeProfileCount;

#endif  // RIMEWIN_TSF_GUIDS_H_
