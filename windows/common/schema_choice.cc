// windows/common/schema_choice.cc — 純邏輯,不含任何平台 API。

#include "schema_choice.h"

#include <algorithm>

namespace rimewin {
namespace {

// 與 luna_pinyin.schema.yaml 的 `options:` 順序一致。
constexpr const char* kZhHant = "zh_hant";
constexpr const char* kZhHans = "zh_hans";
constexpr const char* kZhHantHK = "zh_hant_hk";
constexpr const char* kZhHantTW = "zh_hant_tw";

// librime 內建 simplifier 的預設 option 名稱。本專案打包的方案都沒有用它,
// 但第三方方案(五筆·簡入繁出之類)有,所以照樣送 —— 送了不存在的 option
// 對 librime 是無害的。
constexpr const char* kSimplification = "simplification";

struct LangRow {
  uint32_t langid;
  const char* name;
  const char* first;
  const char* second;
  Variant variant;
};

// 見標頭的表。
constexpr LangRow kLangRows[] = {
    {0x0404u, "zh-Hant-TW", "luna_pinyin_tw", "luna_pinyin", Variant::kHantTW},
    {0x0804u, "zh-Hans-CN", "luna_pinyin", "luna_pinyin_tw", Variant::kHans},
    {0x0C04u, "zh-Hant-HK", "luna_pinyin", "luna_pinyin_tw", Variant::kHantHK},
    {0x1004u, "zh-Hans-SG", "luna_pinyin", "luna_pinyin_tw", Variant::kHans},
    {0x1404u, "zh-Hant-MO", "luna_pinyin", "luna_pinyin_tw", Variant::kHantHK},
};

const LangRow* FindRow(uint32_t langid) {
  for (const LangRow& r : kLangRows)
    if (r.langid == langid) return &r;
  return nullptr;
}

bool Contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

const char* const kVariantOptions[4] = {kZhHant, kZhHans, kZhHantHK, kZhHantTW};

const char* VariantOptionName(Variant v) {
  switch (v) {
    case Variant::kHant: return kZhHant;
    case Variant::kHans: return kZhHans;
    case Variant::kHantHK: return kZhHantHK;
    case Variant::kHantTW: return kZhHantTW;
    case Variant::kFollow: break;
  }
  return nullptr;
}

Variant VariantFromOptionName(const std::string& name) {
  if (name == kZhHant) return Variant::kHant;
  if (name == kZhHans) return Variant::kHans;
  if (name == kZhHantHK) return Variant::kHantHK;
  if (name == kZhHantTW) return Variant::kHantTW;
  return Variant::kFollow;
}

// 設定檔的字面值與 option 名稱刻意相同。多一套代號就多一張要對的表,
// 而那張表錯了以後的症狀是「設定看起來存了,但打出來沒變」。
const char* VariantToken(Variant v) {
  const char* n = VariantOptionName(v);
  return n ? n : "";
}

Variant VariantFromToken(const std::string& token) {
  return VariantFromOptionName(token);
}

std::vector<OptionAssign> PlanVariant(Variant want, Variant saved_variant) {
  std::vector<OptionAssign> out;
  if (want == Variant::kFollow) return out;  // 沒有意見 = 一個都不送

  // ⚠ 還原只在 want 是**泛稱的「繁體」**(kHant)時才做。
  //
  //   kHantTW / kHantHK 是明確的要求(來自 langid 或使用者在設定裡選的),
  //   拿 saved 蓋掉它等於忽略使用者剛剛按的那一下。
  //   而 kHant 是「簡繁切換」那顆鍵切回來時的請求 —— 它只說「不要簡體」,
  //   沒說要哪一種繁體,這時本來停在臺灣字形的人就該回到臺灣字形。
  //   (硬設 zh_hant 的話,他會安靜地落到傳統漢字:差別小到當下不會發現,
  //    只覺得「有幾個字變了」。Android 端踩過同一個坑。)
  Variant target = want;
  if (want == Variant::kHant && saved_variant != Variant::kFollow &&
      saved_variant != Variant::kHans) {
    target = saved_variant;
  }

  // 1. 先送 simplification。真的有這個開關的方案靠它,沒有的方案無視它。
  out.push_back({kSimplification, target == Variant::kHans});

  // 2. radio group:選中的設 true,同組其他三個設 false。
  //    順序上先關再開,免得中途出現「兩個都是真」的一瞬間 ——
  //    librime 是同執行緒同步的,理論上看不到中間態,但把不變式寫進順序裡
  //    比靠「應該看不到」可靠。
  const std::string on = VariantOptionName(target) ? VariantOptionName(target) : "";
  for (int i = 0; i < kVariantOptionCount; ++i) {
    if (on == kVariantOptions[i]) continue;
    out.push_back({kVariantOptions[i], false});
  }
  if (!on.empty()) out.push_back({VariantOptionName(target), true});
  return out;
}

bool IsChineseLangId(uint32_t langid) {
  // Windows 的 LANGID = (SUBLANG << 10) | PRIMARYLANG,LANG_CHINESE = 0x04。
  return (langid & 0x3FFu) == 0x04u;
}

const char* LangIdName(uint32_t langid) {
  const LangRow* r = FindRow(langid);
  return r ? r->name : "?";
}

SchemaChoice DefaultForLangId(uint32_t langid,
                              const std::vector<std::string>& available) {
  SchemaChoice out;
  if (!IsChineseLangId(langid)) {
    out.source = "langid-not-chinese";
    return out;
  }
  const LangRow* r = FindRow(langid);
  if (!r) {
    // 中文但不是我們列過的 sublang(例如 0x0004 中性)。字形不表示意見,
    // 方案也不表示意見 —— 猜錯的成本比不猜高。
    out.source = "langid-chinese-unknown-sublang";
    return out;
  }
  out.variant = r->variant;
  out.source = "langid";
  if (available.empty()) {
    // 還沒部署完就問。仍然給第一順位:選不到 librime 會拒絕,而拒絕看得到。
    out.schema_id = r->first;
    out.source = "langid-no-list";
    return out;
  }
  if (Contains(available, r->first)) {
    out.schema_id = r->first;
  } else if (Contains(available, r->second)) {
    out.schema_id = r->second;
  }
  // 兩個都沒有 → schema_id 留空,交給 schema_list 第一項。
  // 但**字形仍然套用** —— 使用者裝了第三方的簡體方案時,
  // 「他選的是簡體那一份」這個資訊照樣有用。
  return out;
}

SchemaChoice ChooseSchema(uint32_t langid,
                          const std::vector<std::string>& available,
                          const SchemaPreference& pref) {
  SchemaChoice out = DefaultForLangId(langid, available);

  // ── 方案 ────────────────────────────────────────────────────
  if (!pref.forced_schema.empty()) {
    out.schema_id = pref.forced_schema;
    out.source = "settings-forced-schema";
  } else {
    for (const auto& kv : pref.last_used) {
      if (kv.first == langid && !kv.second.empty()) {
        out.schema_id = kv.second;
        out.source = "last-used";
        break;
      }
    }
  }

  // ── 字形 ────────────────────────────────────────────────────
  if (pref.forced_variant != Variant::kFollow) {
    out.variant = pref.forced_variant;
    // source 只描述方案是怎麼來的;字形另有來源時在這裡合併說明,
    // 免得日誌上看到 "langid" 卻其實是設定覆寫的。
    out.source = (out.source == std::string("settings-forced-schema"))
                     ? "settings"
                     : "settings-forced-variant";
  }
  return out;
}

}  // namespace rimewin
