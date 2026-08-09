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

// {7D02992E-B213-4E06-B62E-CCC6338DA98A}(改名前是 {E94B9FC2-6730-45AD-A462-B7D02995D95B})
extern const CLSID CLSID_RimeTextService = {
    0x7d02992e, 0xb213, 0x4e06, {0xb6, 0x2e, 0xcc, 0xc6, 0x33, 0x8d, 0xa9, 0x8a}};

// {4F78BA11-E997-4BD7-8B97-F4553ABC0B18}(改名前是 {07FB3057-4192-4868-AB6E-E4EE5597C0FE})
extern const GUID GUID_RimeProfile = {
    0x4f78ba11, 0xe997, 0x4bd7, {0x8b, 0x97, 0xf4, 0x55, 0x3a, 0xbc, 0x0b, 0x18}};

// {84420A61-0A08-4A68-9D60-292EFD31C7BC}(改名前是 {57BE9E4D-3F4E-4B4F-959B-E85E6095F2CA})
extern const GUID GUID_RimeProfileHans = {
    0x84420a61, 0x0a08, 0x4a68, {0x9d, 0x60, 0x29, 0x2e, 0xfd, 0x31, 0xc7, 0xbc}};

// {C6B736EB-38E3-4041-B59B-ECF91AD8E28A}(改名前是 {23BBABB2-5C8A-4751-85F1-B360C70A5637})
extern const GUID GUID_RimeProfileHK = {
    0xc6b736eb, 0x38e3, 0x4041, {0xb5, 0x9b, 0xec, 0xf9, 0x1a, 0xd8, 0xe2, 0x8a}};

// 註冊在哪些語言底下。說明見 guids.h。
//
// ⚠ 這三個 GUID 在 2026-08-09 產品定名 LuminaKey 時**一起換掉了**,
//   理由與後果見 guids.h 檔頭。換完之後這條規矩重新生效:
//   從現在起這三列的 GUID 不可以再動,動了就是再變成另一個輸入法一次。
//
// 描述字串跟著語言的字形走。簡體使用者的輸入法清單上不該出現一串繁體字。
extern const RimeProfileDef kRimeProfiles[] = {
    {(LANGID)0x0404, &GUID_RimeProfile,     L"LuminaKey 輸入法"},  // zh-Hant-TW
    {(LANGID)0x0804, &GUID_RimeProfileHans, L"LuminaKey 输入法"},  // zh-Hans-CN
    {(LANGID)0x0C04, &GUID_RimeProfileHK,   L"LuminaKey 輸入法"},  // zh-Hant-HK
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
