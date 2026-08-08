// windows/tsf/registration_check.h — 「這個輸入法真的註冊好了嗎」的檢查
//
// ── 這個檔案存在的理由 ───────────────────────────────────────────
//
// 上一輪的 windows/README.md 把「regsvr32 是否真的註冊成功」「輸入法是否
// 出現在系統的清單上」列在**驗不了**的那一欄。那個判斷有一半是錯的:
// GitHub 的 windows-latest runner 上我們有系統管理員權限,所以
//
//     靜默安裝 → 斷言登錄檔 → 用 TSF 的 API 列舉出自己 → 靜默解除安裝
//     → 斷言清乾淨、而使用者詞典還在
//
// 整條是跑得動的。「輸入法有沒有被系統接受」從「驗不了」變成「驗得了」,
// 這件事的價值比安裝程式本身還高 —— 這個專案最貴的失敗一直都是
// 「編得出來、測試全綠、使用者一裝就不能用」。
//
// ⚠ 這一份**不連進 rime_tsf.dll**,只連進 rime_ime_setup.exe。
//   DLL 住在每一個宿主進程裡,它的相依有一份很短的允許清單
//   (windows/check_binaries.sh),沒有理由為了「檢查」多背一份程式碼。
//
// 仍然驗不到的(這一節請不要縮水):
//   · 切到這個輸入法之後 ActivateEx 有沒有被呼叫、sink 有沒有掛上
//   · 在記事本 / 瀏覽器 / Office / 市集 App 裡真的打不打得出字
//   · 候選窗長什麼樣、位置對不對、高 DPI 與多螢幕
//   · 使用者的語言列上到底看不看得到它(那還取決於使用者的語言清單裡
//     有沒有 zh-Hant-TW,而那是 CI 上沒有辦法製造的情境)
#ifndef RIMEWIN_TSF_REGISTRATION_CHECK_H_
#define RIMEWIN_TSF_REGISTRATION_CHECK_H_

#include <windows.h>

#include <string>

namespace rimewin {

struct CheckOptions {
  // 非空時,額外斷言 InprocServer32 指到的就是這個路徑(不分大小寫)。
  // 這一條抓的是一種很真實的失敗:機器上先前用建置樹裡的 DLL 註冊過,
  // 安裝程式裝進 Program Files 之後,登錄檔卻還指著那份早就被刪掉的檔案。
  std::wstring expect_dll_path;
  // 同時檢查 HKCU 的啟用狀態(安裝程式以使用者身分跑 enable-user 之後)。
  bool check_user = false;

  // 要不要問 TSF 的列舉 API。
  //
  // ⚠ 預設要問 —— 那是這整個檢查最有價值的一項(「系統接受了嗎」)。
  //   但**安裝程式剛註冊完的那一瞬間不可以問**:實測在 register 回傳成功之後
  //   0.12 秒,EnumLanguageProfiles 還看不到我們;22 秒後同一支程式跑同一段
  //   就全部看得到。CTF 那一側的可見性不是同步的,而登錄檔是。
  //   安裝程式因此改用 --no-enum(只驗登錄檔,那部分是確定的),
  //   「系統接受了嗎」交給 CI 的 verify_installer.sh 事後問。
  bool check_enum = true;
};

// 全部通過才回傳 true。過程與失敗原因一律印到 stdout(窄字元 UTF-8)。
// 失敗時會順便把 CTF 底下的實際子樹印出來 —— 沒有它的話,「路徑不對」與
// 「根本沒註冊」在報表上長得一模一樣,而每問一次都要等一輪 CI。
bool CheckRegistration(const CheckOptions& opt);

// 診斷用:把 HKLM/HKCU 的 CTF\TIP\{clsid} 子樹原樣印出來。
void DumpRegistration();

}  // namespace rimewin

#endif  // RIMEWIN_TSF_REGISTRATION_CHECK_H_
