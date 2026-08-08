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

// 輸入法設定檔(語言列上的那一項)。
// {07FB3057-4192-4868-AB6E-E4EE5597C0FE}
extern const GUID GUID_RimeProfile;

// 預留:顯示屬性(組字底線)與保留鍵。本輪未實作,但先把值定下來,
// 免得日後補上時得換 GUID —— 換 GUID 就是換一個新的輸入法。
// {7A599152-062F-467B-A024-A30EB287BDED}
extern const GUID GUID_RimeDisplayAttributeInput;
// {5C43BAAB-FD30-4D57-AEC3-0D8E2404D67A}
extern const GUID GUID_RimePreservedKeyToggle;

// 語言列上的顯示名稱與註冊語言。
//
// ⚠ 0x0404 = zh-Hant-TW。專案內建的方案是 luna_pinyin_tw / bopomofo_tw,
//   都是繁體,所以先註冊在繁中底下。要不要另外註冊一份 zh-Hans(0x0804)
//   是產品決定,不是技術限制 —— 多一份 profile 就多一個 GUID。
#define RIME_TEXT_SERVICE_DESC L"RIME 四端輸入法"
#define RIME_PROFILE_LANGID ((LANGID)0x0404)

#endif  // RIMEWIN_TSF_GUIDS_H_
