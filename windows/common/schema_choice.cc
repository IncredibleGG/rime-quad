// windows/common/schema_choice.cc — 純邏輯,不含任何平台 API。
// 實作的是 docs/settings-model.md §4。

#include "schema_choice.h"

#include "settings.h"  // Tri 的列舉值(標頭裡只有不透明宣告)

#include <algorithm>

namespace rimewin {
namespace {

constexpr const char* kZhHant = "zh_hant";
constexpr const char* kZhHans = "zh_hans";
constexpr const char* kZhHantHK = "zh_hant_hk";
constexpr const char* kZhHantTW = "zh_hant_tw";

// librime 內建 simplifier 的預設 option 名稱。本專案打包的方案都沒有用它,
// 但第三方方案(五筆·簡入繁出之類)有。
//
// ⚠ 規範 §4.5:「無條件設,不要先判斷方案有沒有這個開關」——
//   對一個不存在的 switch 呼叫 rs_set_option 是安全的(librime 只是記下
//   一個沒有人讀的選項),而判斷「這個方案有沒有 simplification」要解析
//   第三方方案的 YAML,猜錯的機會更大。
constexpr const char* kSimplification = "simplification";

bool EndsWith(const std::string& s, const char* suffix) {
  const std::string t(suffix);
  return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

bool StartsWith(const std::string& s, const char* prefix) {
  const std::string t(prefix);
  return s.size() >= t.size() && s.compare(0, t.size(), t) == 0;
}

bool Contains(const std::vector<std::string>& v, const std::string& s) {
  return !s.empty() && std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

const char* const kVariantOptions[4] = {kZhHant, kZhHans, kZhHantHK, kZhHantTW};

// 說明見標頭。這四個要涵蓋 CharSetOfLangId 的每一種結果,
// **以及** PlanVariant 會分岔的每一種字形(臺灣 / 香港 / 簡體)。
const uint32_t kWarmUpLangIds[4] = {
    0u,       // 還沒表明語言的宿主(v1 的 DLL,或系統問不出來)
    0x0404u,  // 繁體(臺灣字形)
    0x0804u,  // 簡體
    0x0C04u,  // 繁體(香港字形)
};

CharSet CharSetOfLangId(uint32_t langid) {
  // LANGID = (SUBLANG << 10) | PRIMARYLANG,LANG_CHINESE = 0x04。
  if ((langid & 0x3FFu) != 0x04u) return CharSet::kUnspecified;
  switch (langid) {
    case 0x0404u:  // zh-Hant-TW
    case 0x0C04u:  // zh-Hant-HK
    case 0x1404u:  // zh-Hant-MO
      return CharSet::kHant;
    case 0x0804u:  // zh-Hans-CN
    case 0x1004u:  // zh-Hans-SG
      return CharSet::kHans;
    default:
      // 中文但不是我們列過的 sublang(例如 0x0004 中性)。
      // ⚠ 規範 §4.2:認不出來回 unspecified,**不要預設繁體**。
      return CharSet::kUnspecified;
  }
}

CharSet CharSetOfSchemaId(const std::string& id) {
  if (id.empty()) return CharSet::kUnspecified;
  if (EndsWith(id, "_tw") || EndsWith(id, "_hk") || EndsWith(id, "_trad") ||
      StartsWith(id, "bopomofo"))
    return CharSet::kHant;
  if (EndsWith(id, "_cn") || EndsWith(id, "_sc") || EndsWith(id, "_simp"))
    return CharSet::kHans;
  // ⚠ 規範明著禁止的那一條:**不要加「含 pinyin 就是簡體」**。
  //   luna_pinyin 的預設輸出是繁體。
  return CharSet::kUnspecified;
}

const char* VariantPrefToken(VariantPref v) {
  switch (v) {
    case VariantPref::kTraditional: return "traditional";
    case VariantPref::kSimplified: return "simplified";
    case VariantPref::kFollowInputMode: break;
  }
  return "followInputMode";
}

VariantPref VariantPrefFromToken(const std::string& s) {
  if (s == "traditional") return VariantPref::kTraditional;
  if (s == "simplified") return VariantPref::kSimplified;
  // 認不得的字面值 = 預設值,不是崩潰也不是繁體。
  return VariantPref::kFollowInputMode;
}

const char* LangIdName(uint32_t langid) {
  switch (langid) {
    case 0x0404u: return "zh-Hant-TW";
    case 0x0804u: return "zh-Hans-CN";
    case 0x0C04u: return "zh-Hant-HK";
    case 0x1004u: return "zh-Hans-SG";
    case 0x1404u: return "zh-Hant-MO";
    default: return "?";
  }
}

bool DecideVariant(uint32_t langid, const SchemaPreference& pref,
                   bool* simplified) {
  *simplified = false;
  switch (pref.variant) {
    case VariantPref::kTraditional:
      return true;
    case VariantPref::kSimplified:
      *simplified = true;
      return true;
    case VariantPref::kFollowInputMode:
      break;
  }
  // ⚠ follow_input_mode 關掉時**連 simplification 都不碰**(§4.5 最後一條)。
  //   使用者要的是「我自己管」,半套(方案不跟、簡繁還是跟)更難理解。
  if (!pref.follow_input_mode) return false;
  const CharSet mode = CharSetOfLangId(langid);
  if (mode == CharSet::kUnspecified) return false;
  *simplified = (mode == CharSet::kHans);
  return true;
}

SchemaChoice ChooseSchema(uint32_t langid,
                          const std::vector<std::string>& available,
                          const SchemaPreference& pref) {
  SchemaChoice out;
  const CharSet mode = CharSetOfLangId(langid);

  // ── 方案(§4.4 的四層)────────────────────────────────────────
  //
  // 每一層都要求「那個方案真的在已啟用清單上」。釘的方案被停用了就
  // 當作沒釘 —— 選一個清單上沒有的東西,librime 會拒絕,而使用者看到的
  // 是「輸入法忽然沒有候選」。
  if (available.empty()) {
    // 清單是空的 → 什麼都不做。呼叫端要在畫面上說「還沒有任何方案」。
    out.source = "no-schemas";
  } else if (pref.follow_input_mode && mode == CharSet::kHant &&
             Contains(available, pref.pinned_hant)) {
    out.schema_id = pref.pinned_hant;
    out.source = "pinnedHant";
  } else if (pref.follow_input_mode && mode == CharSet::kHans &&
             Contains(available, pref.pinned_hans)) {
    out.schema_id = pref.pinned_hans;
    out.source = "pinnedHans";
  } else if (Contains(available, pref.pinned_global)) {
    out.schema_id = pref.pinned_global;
    out.source = "pinnedGlobal";
  } else if (pref.follow_input_mode && mode != CharSet::kUnspecified) {
    // 第 3 層:清單中第一個字集相符的。
    for (const std::string& id : available) {
      if (CharSetOfSchemaId(id) == mode) {
        out.schema_id = id;
        out.source = "charset-match";
        break;
      }
    }
    if (out.schema_id.empty()) {
      out.schema_id = available.front();
      out.source = "first-enabled";
    }
  } else {
    out.schema_id = available.front();
    out.source = "first-enabled";
  }

  // ── 簡繁(§4.5)──────────────────────────────────────────────
  out.set_variant = DecideVariant(langid, pref, &out.simplified);
  return out;
}

std::vector<OptionAssign> PlanVariant(bool simplified, uint32_t langid_hint) {
  std::vector<OptionAssign> out;

  // 1. simplification。真的有這個開關的方案靠它(第三方的五筆·簡入繁出
  //    之類),沒有的方案無視它。
  out.push_back({kSimplification, simplified});

  // 2. 本專案打包的 luna_pinyin 家族靠的是那組互斥的 radio。
  //    要繁體時得挑**一種**繁體 —— 這不是一個設定項,使用者選的是
  //    「繁體」,是哪一種由他的語言設定檔決定。
  const char* on = kZhHant;
  if (simplified) {
    on = kZhHans;
  } else if (langid_hint == 0x0C04u || langid_hint == 0x1404u) {
    on = kZhHantHK;  // 香港 / 澳門字形
  } else if (langid_hint == 0x0404u) {
    on = kZhHantTW;  // 臺灣字形
  }

  // 先關再開:互斥寫進順序裡,比靠「應該看不到中間態」可靠。
  for (int i = 0; i < kVariantOptionCount; ++i) {
    if (std::string(kVariantOptions[i]) == on) continue;
    out.push_back({kVariantOptions[i], false});
  }
  out.push_back({on, true});
  return out;
}

std::vector<OptionAssign> BuildOptionPlan(const SchemaChoice& choice,
                                          uint32_t langid,
                                          Tri punctuation,
                                          bool ascii_mode) {
  std::vector<OptionAssign> out;
  // 1. 簡繁。set_variant 為假 = 使用者說「我自己管」,一個都不碰。
  if (choice.set_variant) out = PlanVariant(choice.simplified, langid);
  // 2. 標點。kUnset = followSchema = 整項不出現。
  if (punctuation != Tri::kUnset)
    out.push_back({"ascii_punct", punctuation == Tri::kTrue});
  // 3. 中英。**永遠**在計畫裡 —— 少了它,兩條路徑的計畫長度就會不同,
  //    而 SameOptions 第一件事就是比長度。那正是這一支要修的東西。
  out.push_back({"ascii_mode", ascii_mode});
  return out;
}

std::vector<OptionAssign> UpdateVariantInPlan(
    const std::vector<OptionAssign>& plan, bool set_variant, bool simplified,
    uint32_t langid) {
  // 說明見標頭。新的簡繁那一組整組擺在最前面(BuildOptionPlan 的位置),
  // 舊計畫裡屬於那一組的通通丟掉,其餘照原順序接在後面。
  std::vector<OptionAssign> out;
  if (set_variant) out = PlanVariant(simplified, langid);
  for (const OptionAssign& a : plan) {
    const std::string name(a.option ? a.option : "");
    if (name == kSimplification) continue;
    bool is_radio = false;
    for (int i = 0; i < kVariantOptionCount; ++i)
      if (name == kVariantOptions[i]) is_radio = true;
    if (is_radio) continue;
    out.push_back(a);
  }
  return out;
}

}  // namespace rimewin
