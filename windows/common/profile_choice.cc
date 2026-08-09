// windows/common/profile_choice.cc — 說明見 profile_choice.h

#include "profile_choice.h"

namespace rimewin {
namespace {

// 小寫化、把 '_' 當 '-'、丟掉前後空白。
// 自己寫是因為 <cctype> 的 tolower 受 locale 影響,而標籤只會是 ASCII。
std::string Normalize(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
    if (c == '_') c = '-';
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    out.push_back(c);
  }
  return out;
}

// 「a-b-c」的第 n 段(從 0 起)。沒有那一段就回空字串。
std::string Part(const std::string& s, int n) {
  size_t begin = 0;
  for (int i = 0; i < n; ++i) {
    const size_t dash = s.find('-', begin);
    if (dash == std::string::npos) return std::string();
    begin = dash + 1;
  }
  const size_t dash = s.find('-', begin);
  return dash == std::string::npos ? s.substr(begin)
                                   : s.substr(begin, dash - begin);
}

bool Contains(const std::vector<uint16_t>& v, uint16_t x) {
  for (uint16_t e : v)
    if (e == x) return true;
  return false;
}

// 把任意一個「我們認得的中文」折進**實際註冊了**的那幾個裡。
// 折不進去回 0。
uint16_t FoldToRegistered(uint16_t want, const std::vector<uint16_t>& reg) {
  if (want == 0) return 0;
  if (Contains(reg, want)) return want;
  // 註冊清單裡沒有它。退而求其次:同字集的另一個。
  //   繁體(TW / HK)互相替代;簡體只有 CN 一個。
  if (want == kLangHantHK && Contains(reg, kLangHantTW)) return kLangHantTW;
  if (want == kLangHantTW && Contains(reg, kLangHantHK)) return kLangHantHK;
  return 0;
}

}  // namespace

uint16_t ProfileLangIdOfTag(const std::string& tag) {
  const std::string t = Normalize(tag);
  if (t.empty()) return 0;
  if (Part(t, 0) != "zh") return 0;

  // 完整比對優先,免得「zh-hant-hk」被「zh-hant」那一條先吃掉。
  // ⚠ 順序有意義:最長、最具體的排前面。
  struct Row {
    const char* tag;
    uint16_t langid;
  };
  static const Row kExact[] = {
      {"zh-hans-cn", kLangHansCN}, {"zh-hant-tw", kLangHantTW},
      {"zh-hant-hk", kLangHantHK}, {"zh-hant-mo", kLangHantHK},
      {"zh-hans-sg", kLangHansCN}, {"zh-cn", kLangHansCN},
      {"zh-tw", kLangHantTW},      {"zh-hk", kLangHantHK},
      {"zh-mo", kLangHantHK},      {"zh-sg", kLangHansCN},
      {"zh-chs", kLangHansCN},     {"zh-cht", kLangHantTW},
      {"zh-hans", kLangHansCN},    {"zh-hant", kLangHantTW},
  };
  for (const Row& r : kExact)
    if (t == r.tag) return r.langid;

  // 「zh-hans-cn-x-something」那種帶擴充的。看前兩段就夠。
  const std::string script = Part(t, 1);
  if (script == "hans") return kLangHansCN;
  if (script == "hant") {
    const std::string region = Part(t, 2);
    if (region == "hk" || region == "mo") return kLangHantHK;
    return kLangHantTW;
  }
  if (script == "cn" || script == "sg") return kLangHansCN;
  if (script == "tw") return kLangHantTW;
  if (script == "hk" || script == "mo") return kLangHantHK;

  // 光禿禿的 "zh"。Windows 的清單上實務上不會出現,但別人的資料可能有。
  // ⚠ 與 kFallbackLangId 同一個理由:不知道就往人數多的那一邊倒。
  if (t == "zh") return kFallbackLangId;
  return 0;
}

uint16_t ProfileLangIdOfLangId(uint32_t langid) {
  switch (langid & 0xFFFF) {
    case 0x0404: return kLangHantTW;   // zh-TW
    case 0x0804: return kLangHansCN;   // zh-CN
    case 0x0C04: return kLangHantHK;   // zh-HK
    case 0x1004: return kLangHansCN;   // zh-SG(沒註冊,折到簡體)
    case 0x1404: return kLangHantHK;   // zh-MO(沒註冊,折到香港)
    default: return 0;
  }
}

ProfileChoice ChooseUserProfileLangId(
    const std::vector<std::string>& user_language_tags,
    const std::vector<uint32_t>& installed_langids, uint32_t ui_langid,
    uint32_t explicit_langid, const std::vector<uint16_t>& registered) {
  ProfileChoice out;

  // registered 是空的 = 呼叫端搞錯了。回 0,讓呼叫端明確地失敗,
  // 不要回一個「看起來合理」的值 —— 那會變成「啟用了一個沒註冊的語言」,
  // 而那個症狀是清單上什麼都沒有。
  if (registered.empty()) {
    out.reason = "沒有任何已註冊的語言設定檔(呼叫端傳了空清單)";
    return out;
  }

  // 1. 使用者明確指定。
  if (explicit_langid != 0) {
    const uint16_t v =
        FoldToRegistered(ProfileLangIdOfLangId(explicit_langid), registered);
    if (v != 0) {
      out.langid = v;
      out.reason = "使用者明確指定(--lang)";
      return out;
    }
    // 指定了一個我們沒註冊的語言。**不要靜靜地忽略它**往下走 ——
    // 那樣使用者會以為 --lang 生效了。往下走,但理由要說出來。
  }

  // 2. 使用者的語言清單,照他自己的順序。
  for (const std::string& tag : user_language_tags) {
    const uint16_t v = FoldToRegistered(ProfileLangIdOfTag(tag), registered);
    if (v != 0) {
      out.langid = v;
      out.reason = "使用者的語言清單裡已經有這個語言";
      return out;
    }
  }

  // 3. 系統回報「已經有輸入法」的語言。
  for (uint32_t lang : installed_langids) {
    const uint16_t v =
        FoldToRegistered(ProfileLangIdOfLangId(lang), registered);
    if (v != 0) {
      out.langid = v;
      out.reason = "系統已安裝的輸入語言裡有這個語言";
      return out;
    }
  }

  // 4. 系統顯示語言。
  {
    const uint16_t v = FoldToRegistered(ProfileLangIdOfLangId(ui_langid),
                                        registered);
    if (v != 0) {
      out.langid = v;
      out.reason = "系統顯示語言";
      return out;
    }
  }

  // 5. 退路。這一步會讓 Windows 替使用者新增一個語言,理由見標頭。
  out.langid = FoldToRegistered(kFallbackLangId, registered);
  if (out.langid == 0) out.langid = registered[0];
  out.reason = "使用者沒有任何中文語言,用預設(會替他新增一個語言)";
  return out;
}

}  // namespace rimewin
