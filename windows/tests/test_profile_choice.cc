// windows/tests/test_profile_choice.cc — 「清單上只出現一次,而且出現在對的
// 那一欄」的驗證
//
// ⚠ 這一份是本輪修正**唯一**真正被驗到的地方。
//   Windows 上的 CI runner 只有一種語言情境(英文,沒有任何中文輸入語言),
//   所以在那裡跑到的只有最後那一格「使用者沒有任何中文語言」。
//   簡體使用者、繁體使用者、香港使用者、兩種都有的使用者 ——
//   四種情境都只有這裡走得到。
//
// 每一個案例都對應一個**具體的人**,而不是一個等價類別:
// 案例名字裡寫的就是他的語言清單長什麼樣。

#include "../common/profile_choice.h"
#include "check.h"

using namespace rimewin;

namespace {

// 我們實際註冊的三個。與 tsf/guids.cc 的 kRimeProfiles 同一組。
const std::vector<uint16_t> kReg = {kLangHantTW, kLangHansCN, kLangHantHK};

ProfileChoice Choose(const std::vector<std::string>& tags,
                     const std::vector<uint32_t>& installed = {},
                     uint32_t ui = 0x0409, uint32_t explicit_lang = 0) {
  return ChooseUserProfileLangId(tags, installed, ui, explicit_lang, kReg);
}

}  // namespace

// ── 標籤對照 ──────────────────────────────────────────────────────

TEST(ProfileTag_認得的中文標籤) {
  CHECK_INT(ProfileLangIdOfTag("zh-Hans-CN"), kLangHansCN);
  CHECK_INT(ProfileLangIdOfTag("zh-Hant-TW"), kLangHantTW);
  CHECK_INT(ProfileLangIdOfTag("zh-Hant-HK"), kLangHantHK);
  // 大小寫不敏感、底線當連字號 —— 不同來源寫法不一樣,而寫法差異
  // 造成「認不出中文」的話,症狀是我們替他新增一個他不要的語言。
  CHECK_INT(ProfileLangIdOfTag("ZH-HANS-CN"), kLangHansCN);
  CHECK_INT(ProfileLangIdOfTag("zh_hant_tw"), kLangHantTW);
  CHECK_INT(ProfileLangIdOfTag("  zh-Hant-HK "), kLangHantHK);
  // 短標籤
  CHECK_INT(ProfileLangIdOfTag("zh-CN"), kLangHansCN);
  CHECK_INT(ProfileLangIdOfTag("zh-TW"), kLangHantTW);
  CHECK_INT(ProfileLangIdOfTag("zh-HK"), kLangHantHK);
  // 沒註冊的兩個要折到字集最接近的
  CHECK_INT(ProfileLangIdOfTag("zh-SG"), kLangHansCN);
  CHECK_INT(ProfileLangIdOfTag("zh-MO"), kLangHantHK);
  CHECK_INT(ProfileLangIdOfTag("zh-Hant-MO"), kLangHantHK);
  // ⚠ zh-Hant 不帶地區時是**臺灣**那一份,不是香港。
  CHECK_INT(ProfileLangIdOfTag("zh-Hant"), kLangHantTW);
  CHECK_INT(ProfileLangIdOfTag("zh-Hans"), kLangHansCN);
}

TEST(ProfileTag_不是中文的一律零) {
  // 回 0 才會讓上一層繼續往下找。回一個「看起來合理」的值等於
  // 替一個英文使用者選了中文,而他還沒走到那一層。
  CHECK_INT(ProfileLangIdOfTag("en-US"), 0);
  CHECK_INT(ProfileLangIdOfTag("ja-JP"), 0);
  CHECK_INT(ProfileLangIdOfTag("ko-KR"), 0);
  CHECK_INT(ProfileLangIdOfTag(""), 0);
  CHECK_INT(ProfileLangIdOfTag("zhx-CN"), 0);   // 不是 zh
  CHECK_INT(ProfileLangIdOfTag("yue-Hant-HK"), 0);  // 粵語不是 zh
}

TEST(ProfileLangId_數字對照) {
  CHECK_INT(ProfileLangIdOfLangId(0x0404), kLangHantTW);
  CHECK_INT(ProfileLangIdOfLangId(0x0804), kLangHansCN);
  CHECK_INT(ProfileLangIdOfLangId(0x0C04), kLangHantHK);
  CHECK_INT(ProfileLangIdOfLangId(0x1004), kLangHansCN);  // zh-SG
  CHECK_INT(ProfileLangIdOfLangId(0x1404), kLangHantHK);  // zh-MO
  CHECK_INT(ProfileLangIdOfLangId(0x0409), 0);            // en-US
  CHECK_INT(ProfileLangIdOfLangId(0x0411), 0);            // ja-JP
  CHECK_INT(ProfileLangIdOfLangId(0), 0);
}

// ── 四種真實的使用者 ──────────────────────────────────────────────

TEST(選一份_回報這個問題的使用者_簡體) {
  // 截圖上的那個人:語言清單裡有「简体中文(中国大陆)」,
  // 底下掛著微软拼音與小狼毫。他該在**那一欄**底下看到我們,
  // 而且只有一格。
  const ProfileChoice c = Choose({"zh-Hans-CN", "en-US"});
  CHECK_INT(c.langid, kLangHansCN);
  CHECK(std::string(c.reason).find("語言清單") != std::string::npos);
}

TEST(選一份_繁體臺灣使用者) {
  const ProfileChoice c = Choose({"zh-Hant-TW"});
  CHECK_INT(c.langid, kLangHantTW);
}

TEST(選一份_香港使用者) {
  const ProfileChoice c = Choose({"zh-Hant-HK", "en-US"});
  CHECK_INT(c.langid, kLangHantHK);
}

TEST(選一份_兩種中文都有時照他自己的順序) {
  // 使用者的語言清單是**有序的**,第一個就是他主要用的。
  // 這一條抓的是「照我們的偏好選」而不是「照他的順序選」——
  // 那個錯誤的症狀是「我明明把簡體排第一,它卻掛在繁體底下」。
  CHECK_INT(Choose({"zh-Hans-CN", "zh-Hant-TW"}).langid, kLangHansCN);
  CHECK_INT(Choose({"zh-Hant-TW", "zh-Hans-CN"}).langid, kLangHantTW);
  CHECK_INT(Choose({"en-US", "zh-Hant-HK", "zh-Hans-CN"}).langid, kLangHantHK);
}

TEST(選一份_語言清單讀不到時退到已安裝的輸入語言) {
  // 第 2 層拿不到值(讀不到那個登錄檔鍵)的情況。
  const ProfileChoice c = Choose({}, {0x0409, 0x0804});
  CHECK_INT(c.langid, kLangHansCN);
  CHECK(std::string(c.reason).find("已安裝") != std::string::npos);
}

TEST(選一份_都沒有時看系統顯示語言) {
  const ProfileChoice c = Choose({"en-US"}, {0x0409}, 0x0404);
  CHECK_INT(c.langid, kLangHantTW);
  CHECK(std::string(c.reason).find("顯示語言") != std::string::npos);
}

TEST(選一份_完全沒有中文時用退路而且說出來) {
  // 這一格是 CI 的 windows runner 上**唯一**會走到的那一格。
  const ProfileChoice c = Choose({"en-US"}, {0x0409}, 0x0409);
  CHECK_INT(c.langid, kFallbackLangId);
  CHECK_INT(c.langid, kLangHansCN);
  // 理由必須明著說「會替他新增一個語言」—— 這是唯一一個我們主動
  // 改變使用者語言清單的路徑,安裝記錄上必須看得出來。
  CHECK(std::string(c.reason).find("新增") != std::string::npos);
}

TEST(選一份_明確指定優先於一切) {
  const ProfileChoice c =
      Choose({"zh-Hans-CN"}, {0x0804}, 0x0804, 0x0404);
  CHECK_INT(c.langid, kLangHantTW);
  CHECK(std::string(c.reason).find("明確指定") != std::string::npos);
}

TEST(選一份_明確指定了沒註冊的語言時不可以靜靜吃掉) {
  // --lang 0x0411(日文)。我們沒有那一份,所以往下走 ——
  // 但**不可以**就這樣回一個值卻讓人以為 --lang 生效了。
  const ProfileChoice c = Choose({"zh-Hans-CN"}, {}, 0x0409, 0x0411);
  CHECK_INT(c.langid, kLangHansCN);
  CHECK(std::string(c.reason).find("明確指定") == std::string::npos);
}

// ── 回傳值一定在註冊清單裡 ────────────────────────────────────────

TEST(選一份_回傳的一定是有註冊的那幾個之一) {
  // 假設哪天只註冊了兩份。香港使用者要折到臺灣那一份,
  // 而不是回一個沒有註冊的 0x0C04 —— 後者的症狀是
  // EnableLanguageProfile 失敗、清單上什麼都沒有,而每一關都是綠的。
  const std::vector<uint16_t> two = {kLangHantTW, kLangHansCN};
  const ProfileChoice c =
      ChooseUserProfileLangId({"zh-Hant-HK"}, {}, 0x0409, 0, two);
  CHECK_INT(c.langid, kLangHantTW);

  // 只註冊香港那一份時,臺灣使用者折到香港。
  const std::vector<uint16_t> hk = {kLangHantHK};
  CHECK_INT(ChooseUserProfileLangId({"zh-Hant-TW"}, {}, 0x0409, 0, hk).langid,
            kLangHantHK);

  // 簡體折不到繁體(字集不同,寧可走退路也不要掛在錯的字集底下)——
  // 但退路本身也會被折進註冊清單,所以最後仍然是 kLangHantHK。
  // 重點是**回傳值一定在清單裡**,不是它挑了哪一個。
  const ProfileChoice d =
      ChooseUserProfileLangId({"zh-Hans-CN"}, {}, 0x0409, 0, hk);
  CHECK_INT(d.langid, kLangHantHK);
}

TEST(選一份_註冊清單是空的時候回零) {
  // 呼叫端搞錯了。回 0 讓它明確失敗,不要回一個看起來合理的值。
  const ProfileChoice c =
      ChooseUserProfileLangId({"zh-Hans-CN"}, {}, 0x0409, 0, {});
  CHECK_INT(c.langid, 0);
}

// ── 不變式:任何輸入都只會得到**一個**答案,而且它是合法的 ────────

TEST(選一份_任何輸入都只給一個合法答案) {
  // 這一條是整個修正的不變式:清單上只出現一次 ⇔ 這裡只回一個 langid。
  const char* const kTags[] = {"zh-Hans-CN", "zh-Hant-TW", "zh-Hant-HK",
                               "zh-SG",      "zh-MO",      "en-US",
                               "ja-JP",      "",           "zh"};
  for (const char* t : kTags) {
    for (uint32_t ui : {0u, 0x0409u, 0x0404u, 0x0804u}) {
      const ProfileChoice c =
          ChooseUserProfileLangId({t}, {}, ui, 0, kReg);
      CHECK(c.langid == kLangHantTW || c.langid == kLangHansCN ||
            c.langid == kLangHantHK);
      CHECK(c.reason[0] != '\0');
    }
  }
}
