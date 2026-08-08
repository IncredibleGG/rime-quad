// windows/tests/mingw_syntax_shim.h — 只給 windows/syntax_check_mingw.sh 用
//
// ⚠ **這個檔案不會被任何產品目標編進去。** 它存在的唯一理由是讓
//   windows/tsf 與 windows/service 底下的 Windows 專屬程式碼,能在開發用的
//   Ubuntu 上用 mingw-w64 的交叉編譯器做一次 `-fsyntax-only`。
//
// 為什麼值得:Windows 端唯一的建置管道是 GitHub Actions。一個打錯的成員名
// 要等一輪 CI 才知道,而 TSF 那一層有兩千行沒有別的辦法先驗。
// 一次語法檢查十秒,一輪 CI 五分鐘 —— 差距大到值得這個檔案存在。
//
// mingw-w64 13.0 的 msctf.h 少了下面這幾樣(Windows SDK 有):
//   · ITfTextInputProcessorEx
//   · TF_CLIENTID_NULL
//   · GUID_TFCAT_TIPCAP_* 那一組能力類別
//
// ⚠ 這裡的宣告是**照 SDK 抄的形狀**,不是自己發明的介面。
//   它只保證「呼叫的形狀對不對」,不保證與真正的 SDK 二進位相容 ——
//   真正的驗證仍然是 MSVC 在 CI 上編一次。這支腳本綠了**不代表** MSVC 會綠。

#pragma once

#include <windows.h>

#include <msctf.h>  // mingw-w64 沒有 ctffunc.h,而我們也沒用到它裡面的東西

#ifndef TF_CLIENTID_NULL
#define TF_CLIENTID_NULL ((TfClientId)0)
#endif

#ifndef __ITfTextInputProcessorEx_INTERFACE_DEFINED__
#define __ITfTextInputProcessorEx_INTERFACE_DEFINED__
extern "C" const IID IID_ITfTextInputProcessorEx;
struct ITfTextInputProcessorEx : public ITfTextInputProcessor {
  virtual HRESULT STDMETHODCALLTYPE ActivateEx(ITfThreadMgr* ptim, TfClientId tid,
                                               DWORD dwFlags) = 0;
};
#endif

extern "C" {
extern const GUID GUID_TFCAT_TIP_KEYBOARD;
extern const GUID GUID_TFCAT_TIPCAP_SECUREMODE;
extern const GUID GUID_TFCAT_TIPCAP_UIELEMENTENABLED;
extern const GUID GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT;
extern const GUID GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT;
extern const GUID GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT;
}
