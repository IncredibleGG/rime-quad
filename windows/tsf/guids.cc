// windows/tsf/guids.cc — **本專案自己的** GUID 的唯一定義處
//
// TSF 自己那些介面的 IID(IID_ITfThreadMgr、CLSID_TF_CategoryMgr、
// GUID_TFCAT_* …)不在這裡:它們由 Windows SDK 的 uuid.lib 提供。
// uuid.lib 是**靜態**庫,只帶進幾個常數,不會在匯入表裡多一個 DLL ——
// 而這支 DLL 住在每一個宿主進程裡,匯入表就是它對外的全部表面積。
// (刻意不用 initguid.h:msctf.h 是 MIDL 產生的標頭,它的 GUID 是
//  extern 宣告而不是 DEFINE_GUID,initguid.h 對它沒有作用 —— 以為有用
//  的話會得到一整片 LNK2001,而錯誤訊息不會提到原因。)

#include <objbase.h>
#include <windows.h>

#include "guids.h"

// {E94B9FC2-6730-45AD-A462-B7D02995D95B}
extern const CLSID CLSID_RimeTextService = {
    0xe94b9fc2, 0x6730, 0x45ad, {0xa4, 0x62, 0xb7, 0xd0, 0x29, 0x95, 0xd9, 0x5b}};

// {07FB3057-4192-4868-AB6E-E4EE5597C0FE}
extern const GUID GUID_RimeProfile = {
    0x07fb3057, 0x4192, 0x4868, {0xab, 0x6e, 0xe4, 0xee, 0x55, 0x97, 0xc0, 0xfe}};

// {57BE9E4D-3F4E-4B4F-959B-E85E6095F2CA}
extern const GUID GUID_RimeProfileHans = {
    0x57be9e4d, 0x3f4e, 0x4b4f, {0x95, 0x9b, 0xe8, 0x5e, 0x60, 0x95, 0xf2, 0xca}};

// {23BBABB2-5C8A-4751-85F1-B360C70A5637}
extern const GUID GUID_RimeProfileHK = {
    0x23bbabb2, 0x5c8a, 0x4751, {0x85, 0xf1, 0xb3, 0x60, 0xc7, 0x0a, 0x56, 0x37}};

// 註冊在哪些語言底下。說明見 guids.h。
//
// ⚠ 0x0404 那一列必須留在原位、GUID 不可以動:它已經發布出去了,
//   使用者機器上就是那一筆。動了的話,他們清單上的那一項會變成孤兒 ——
//   反註冊找不到它,而新的那一份是另一個輸入法。
//
// 描述字串跟著語言的字形走。簡體使用者的輸入法清單上不該出現一串繁體字。
extern const RimeProfileDef kRimeProfiles[] = {
    {(LANGID)0x0404, &GUID_RimeProfile,     L"RIME 四端輸入法"},  // zh-Hant-TW
    {(LANGID)0x0804, &GUID_RimeProfileHans, L"RIME 四端输入法"},  // zh-Hans-CN
    {(LANGID)0x0C04, &GUID_RimeProfileHK,   L"RIME 四端輸入法"},  // zh-Hant-HK
};
extern const int kRimeProfileCount =
    (int)(sizeof(kRimeProfiles) / sizeof(kRimeProfiles[0]));

// {7A599152-062F-467B-A024-A30EB287BDED}
extern const GUID GUID_RimeDisplayAttributeInput = {
    0x7a599152, 0x062f, 0x467b, {0xa0, 0x24, 0xa3, 0x0e, 0xb2, 0x87, 0xbd, 0xed}};

// {5C43BAAB-FD30-4D57-AEC3-0D8E2404D67A}
extern const GUID GUID_RimePreservedKeyToggle = {
    0x5c43baab, 0xfd30, 0x4d57, {0xae, 0xc3, 0x0d, 0x8e, 0x24, 0x04, 0xd6, 0x7a}};

// {9E2D5B41-0C7A-4E6E-9E52-5F2B8B1C6A44}
const GUID GUID_RimeLangBarButton = {
    0x9e2d5b41, 0x0c7a, 0x4e6e, {0x9e, 0x52, 0x5f, 0x2b, 0x8b, 0x1c, 0x6a, 0x44}};
