// windows/common/profile_choice.h — 「這台機器上要**啟用**哪一份語言設定檔」
//
// ══ 這個檔案在修什麼 ═══════════════════════════════════════════════
//
// 使用者的 Win + 空白鍵清單長這樣(他的截圖):
//
//     简体中文(中国大陆)            LuminaKey 輸入法
//     简体中文(中国大陆)            微软拼音
//     简体中文(中国大陆)            小狼毫
//     繁体中文(中国台湾)            LuminaKey 輸入法
//     繁体中文(中国香港特别行政区)   LuminaKey 輸入法
//
// 微软拼音與小狼毫各佔一格,我們佔三格。他的話:「輸入法不應該顯示那麼多。」
//
// ── 為什麼會變成三格(這一段是整個修正的根據)────────────────────
//
// **註冊(HKLM)與啟用(HKCU)是兩件事,而清單上出現幾格只由後者決定。**
//
//   · `RegisterProfile` 寫的是 HKLM\…\CTF\TIP\{clsid}\LanguageProfile\<langid>。
//     它讓這個輸入法在那個語言底下**可以被加入**,但它自己不會出現在
//     任何使用者的清單上。三份註冊在清單上是零格。
//   · `EnableLanguageProfile`(HKCU)才是「把它加進**這個使用者的**清單」。
//     而對一個**使用者清單裡沒有的語言**做這件事,Windows 會順手把那個語言
//     加進他的語言清單 —— 於是清單上多出一整個語言,底下只掛著我們一個。
//
// 上面那張截圖正是這個形狀的指紋:「繁体中文(中国台湾)」與
// 「繁体中文(中国香港特别行政区)」底下**只有 LuminaKey**。使用者若真的
// 自己裝過這兩個語言,微软注音之類的東西會跟著出現。是我們加上去的。
//
// 原本的 `enable-user` 對三份 profile 各呼叫一次 EnableLanguageProfile,
// 而且刻意「不因為其中一個失敗就放棄其餘的」—— 那個設計假設的是
// 「哪幾份會成功取決於使用者的系統」,但實際上**三份都會成功**,
// 因為失敗的條件根本不是「使用者沒有那個語言」。
//
// ── 所以修正是 ────────────────────────────────────────────────────
//
//   註冊三份(HKLM,不動)+ 只啟用一份(HKCU,本檔決定是哪一份)
//
// 這樣同時滿足兩件本來看起來衝突的事:
//
//   · 清單上只有一格 —— 使用者要的。
//   · **簡體使用者仍然在自己的語言底下找得到它** —— 那是最早的 bug,
//     不可以退回去。他的語言清單裡有「简体中文(中国大陆)」,
//     本檔就選 0x0804,他在自己那一欄底下看到 LuminaKey。
//     繁體使用者同理。而 Windows 自己的「新增鍵盤」清單裡,三個語言
//     底下都仍然找得到我們(那是 HKLM 那一半的作用)。
//
// ⚠ **簡繁不再由 langid 決定。** 只啟用一份之後,使用者沒辦法再用
//   Win + 空白鍵在簡繁之間切 —— 所以那件事必須在我們自己的 UI 裡做得到
//   (系統匣選單、語言列按鈕的選單、設定的「文字」分頁)。
//   schema_choice.h 的 langid 仍然是**初始值**的來源,使用者一旦在
//   `text.variant` 表示過意見就以他的為準。
//
// ── 為什麼是純邏輯(不 include windows.h)─────────────────────────
//
// 「選哪一個 langid」是這一輪唯一一個會直接決定使用者看到什麼的判斷,
// 而它取決於**使用者機器上的語言清單** —— CI 的 runner 上只有一種情境
// (英文,沒有任何中文),所以在 Windows 上頂多驗得到那一格。
// 判斷本身抽成純函式之後,windows/run_logic_tests.sh 可以在 Ubuntu 上
// 用一張表把「简体使用者」「繁體使用者」「香港使用者」「兩種都有」
// 「一種都沒有」全部走過一遍,而那是這個決定真正的驗證。
#ifndef RIMEWIN_PROFILE_CHOICE_H_
#define RIMEWIN_PROFILE_CHOICE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace rimewin {

// 我們註冊的三個 langid。與 tsf/guids.cc 的 kRimeProfiles 是同一組值,
// 但這裡刻意是純數字 —— 那一份要 windows.h(LANGID),這一份不要。
// ⚠ 兩邊對不上的話會**安靜地**選不到東西,所以 setup_main.cc 啟動時
//   會把 kRimeProfiles 的 langid 傳進來當 `registered`,而不是用這幾個常數。
enum : uint16_t {
  kLangHantTW = 0x0404,  // zh-Hant-TW
  kLangHansCN = 0x0804,  // zh-Hans-CN
  kLangHantHK = 0x0C04,  // zh-Hant-HK
};

// BCP-47 標籤 → 我們註冊的某一個 langid。認不得回 0。
//
// 大小寫不敏感;底線當連字號;尾巴多餘的子標籤忽略。
// zh-SG / zh-MO 我們**沒有**註冊(見 guids.h),折到字集最接近的那一個:
//   zh-SG(新加坡,簡體)→ 0x0804      zh-MO(澳門,繁體)→ 0x0C04
uint16_t ProfileLangIdOfTag(const std::string& tag);

// 任意 Windows langid → 我們註冊的某一個。認不得回 0。
// 0x1004(zh-SG)、0x1404(zh-MO)同上折疊;非中文一律 0。
uint16_t ProfileLangIdOfLangId(uint32_t langid);

// 「一種中文都沒有」時要用哪一個。
//
// ⚠ 這個值只在使用者的語言清單、已安裝的輸入語言、系統顯示語言**三者
//   都沒有中文**的時候才用得到 —— 也就是說啟用它會讓 Windows 替使用者
//   **新增一個語言**。那是不得不做的:不加就等於這個輸入法裝完之後
//   在他的清單上不存在。
//
// 選 0x0804 而不是 0x0404 的理由,寫下來免得日後被當成隨手挑的:
//   1. 這一端最早的 bug 就是「只註冊繁中,簡體使用者找不到」。落到這一格
//      的人是「我們對他一無所知」的人,往人數多的那一邊倒。
//   2. 它只決定**清單上那一格的標籤與初始字集**,不決定打出來是簡是繁 ——
//      後者由 text.variant 決定,而那在系統匣、語言列、設定三個地方都改得到。
//   3. 猜錯的代價因此是「標籤不合他的意」,不是「打不出他要的字」。
//      反過來選 0x0404 的話,代價一樣,但踩到的是我們已經修過一次的那個坑。
inline constexpr uint16_t kFallbackLangId = kLangHansCN;

struct ProfileChoice {
  // 要啟用的 langid。永遠是 `registered` 裡的其中一個(不會是 0)。
  uint16_t langid = 0;
  // 為什麼選它。**靜態儲存期的字面值**,直接印進安裝記錄與 CI 的斷言 ——
  // 「選了 0x0804」與「為什麼選 0x0804」是兩個不同的問題,而只印前者的話,
  // 選錯時要再等一輪才知道是哪一層做的決定。
  const char* reason = "";
};

// 依序試四層,第一個命中的就是答案:
//
//   1. `explicit_langid`   使用者明確指定(`enable-user --lang 0x0404`)。
//   2. `user_language_tags` 使用者的語言清單,**照他自己的順序**。
//      這是最重要的一層:它回答「他在哪一欄底下找得到我們」。
//   3. `installed_langids`  系統回報「已經有輸入法」的語言(TSF 的
//      GetLanguageList)。第 2 層讀不到時的第二意見 —— 兩者來源不同,
//      一個是語言清單,一個是輸入法清單,任一個有中文都算數。
//   4. `ui_langid`          系統顯示語言。他沒有中文輸入語言,但介面是中文。
//
// 全部落空 → kFallbackLangId。
//
// ⚠ 回傳的 langid **保證在 `registered` 裡**:上面每一層算出來的值都會再
//   折一次。少了這一步的話,`registered` 哪天改了(例如真的補上 zh-SG),
//   這裡會回一個沒有註冊過的 langid,而 EnableLanguageProfile 會失敗 ——
//   症狀是「裝完了,清單上什麼都沒有」,而每一關都是綠的。
ProfileChoice ChooseUserProfileLangId(
    const std::vector<std::string>& user_language_tags,
    const std::vector<uint32_t>& installed_langids, uint32_t ui_langid,
    uint32_t explicit_langid, const std::vector<uint16_t>& registered);

}  // namespace rimewin

#endif  // RIMEWIN_PROFILE_CHOICE_H_
