// windows/common/elevation_policy.cc — 見 elevation_policy.h 的檔頭

#include "elevation_policy.h"

namespace rimewin {

HostElevation ClassifyHostElevation(bool token_query_ok, bool is_elevated,
                                    TokenSplit split, bool is_service_account) {
  // ⚠ 順序有意義,不要重排。
  //
  // 1. 問不到就拒絕。這一格答錯的後果是「用錯的身分改掉使用者詞庫的擁有者」,
  //    而那是不可逆的;答成「不啟動」則是看得見、查得出來的(見檔頭三個
  //    使用者可見的出口)。
  if (!token_query_ok) return HostElevation::kUnknown;

  // 2. 機器帳號**優先於**提權判斷。
  //    LocalSystem 的權杖 TokenIsElevated 是 true 而 TokenElevationType 是
  //    Default —— 與「內建 Administrator」長得一模一樣。差別在於它根本不是
  //    一個人:從那裡啟動的服務,%APPDATA% 會落在 systemprofile 底下,
  //    具名管道的名字裡帶的是 S-1-5-18,真正的使用者永遠連不上。
  //    登入畫面的 LogonUI.exe 就是這一種,而它真的會載入輸入法的 DLL。
  if (is_service_account) return HostElevation::kServiceAccount;

  // 3. 沒提權 —— 絕大多數使用者的日常。包含分裂權杖裡「受限」的那一半。
  if (!is_elevated) return HostElevation::kNormal;

  // 4. 提權了。唯一要問的是:**這個使用者在這個工作階段裡還有沒有另一份
  //    非提權的權杖?**有 → 那一份才是他的日常身分,他的詞庫屬於那一份,
  //    我們不可以用提權的身分去碰。沒有 → 這個工作階段就長這樣,
  //    服務與宿主的 SID、%APPDATA%、完整性等級全部相同,沒有東西會被弄壞。
  switch (split) {
    case TokenSplit::kNoLinkedToken:
      return HostElevation::kWholeSession;
    case TokenSplit::kFullWithLinked:
      return HostElevation::kSplitToken;
    case TokenSplit::kLimitedWithLinked:
      // 自相矛盾:核心說這是「受限的那一半」,卻又說它是提權的。
      // 不去猜哪一個才對,保守拒絕。
      return HostElevation::kSplitToken;
    case TokenSplit::kUnknown:
      return HostElevation::kUnknown;
  }
  return HostElevation::kUnknown;
}

bool MayStartUserService(HostElevation e) {
  // ⚠ 白名單,不是黑名單。日後多一個列舉值時,預設會是「不啟動」——
  //   那是看得見的失敗;而「用錯的身分啟動」不會有人發現。
  return e == HostElevation::kNormal || e == HostElevation::kWholeSession;
}

const char* HostElevationTag(HostElevation e) {
  switch (e) {
    case HostElevation::kUnknown:        return "unknown";
    case HostElevation::kNormal:         return "normal";
    case HostElevation::kSplitToken:     return "split-token-elevated";
    case HostElevation::kWholeSession:   return "whole-session-elevated";
    case HostElevation::kServiceAccount: return "service-account";
  }
  return "?";
}

const char* HostElevationZh(HostElevation e) {
  switch (e) {
    case HostElevation::kUnknown:
      return "問不出權杖的狀態(保守起見不啟動服務)";
    case HostElevation::kNormal:
      return "一般(沒有提權)";
    case HostElevation::kSplitToken:
      return "這個視窗是提權的,而你在這個工作階段裡另有一份一般身分";
    case HostElevation::kWholeSession:
      return "整個工作階段都是提權的(內建 Administrator 帳號,或 UAC 關閉)";
    case HostElevation::kServiceAccount:
      return "這個進程是系統帳號(不是一個使用者)";
  }
  return "?";
}

const wchar_t* HostElevationTooltipW(HostElevation e) {
  switch (e) {
    case HostElevation::kNormal:
    case HostElevation::kWholeSession:
      return nullptr;  // 可以啟動 —— 按鈕顯示正常的樣子
    case HostElevation::kSplitToken:
      return L"LuminaKey:這個視窗是以系統管理員身分執行的,"
             L"輸入法服務不會從這裡啟動(這是刻意的:那會把你的詞庫檔案"
             L"換成系統管理員所有)。請在一般的視窗裡先打一個字,"
             L"服務起來之後這個視窗也能用。";
    case HostElevation::kServiceAccount:
      return L"LuminaKey:這個進程不是以使用者身分執行的,輸入法服務不會"
             L"從這裡啟動。";
    case HostElevation::kUnknown:
      return L"LuminaKey:問不出這個進程的權限狀態,保守起見沒有啟動輸入法"
             L"服務。請執行 rime_ime_setup.exe doctor 取得診斷。";
  }
  return nullptr;
}

}  // namespace rimewin
