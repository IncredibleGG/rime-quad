// windows/common/schema_choice.h — 輸入模式 ↔ 方案 ↔ 簡繁
//
// ⚠ **這一份實作的是 `docs/settings-model.md` §4,不是自己一套。**
//   那份規範由 macOS 端寫,Windows 端照做。鍵名、優先順序、字集判定
//   全部照它;實作上的差異(見下面「與 §4.5 的一處差異」)已回報到
//   `docs/coordination.md` §5,等它裁決。
//
// ── 這裡在解什麼問題 ────────────────────────────────────────────
//
// Windows 的輸入法設定檔是**每個語言各註冊一份**的(見 tsf/guids.h):
// 0x0404 zh-Hant-TW、0x0804 zh-Hans-CN、0x0C04 zh-Hant-HK。使用者在
// 語言列上選的是其中一份,而**服務進程原本不知道是哪一份** —— 於是
// 預設方案永遠是 schema_list 的第一項,**簡體使用者選了簡體那一份、
// 打出來全是繁體字。** 那是使用者實際回報過的缺陷。
//
// `settings-model.md` §4 開頭記著四端各錯了一次:macOS 註冊了兩個輸入模式
// 但兩個都載入繁體方案;Windows 只註冊了一個 langid,連選都沒得選。
// 共同點是「畫面完全正常、自動化全過,錯的只有打出來是哪一種字」。
//
// ── ⚠ 與 §4.5 的一處差異(已回報,等裁決)────────────────────────
//
// §4.5 說「設 `simplification`」。**光設它在本專案打包的方案上沒有作用。**
// luna_pinyin 家族沒有那個開關,它用的是一組互斥的 radio:
//
//     - options: [ zh_hant, zh_hans, zh_hant_hk, zh_hant_tw ]
//       states:  [ 傳統漢字, 简化字,  香港字形,   臺灣字形 ]
//
// Android 端在模擬器上實測過:只送 `simplification`,打 guojia 出來還是
// 「國家」。而 `rs_set_option` **不會**替你維持 radio 的互斥 —— 那是
// librime 的 switcher 在使用者從選單裡選的時候才做的。
//
// 所以本檔的**決策**照 §4.5(要不要簡體、還是完全不碰),**套用的機制**
// 照 Android 驗證過的做法(simplification + 整組 radio,同組其他的設 false)。
// 這不是另一套模型,是同一個模型的正確接法。§4.5 的措辭要不要補,
// 由規範的擁有者決定。
//
// ── 本檔刻意不 include windows.h ─────────────────────────────────
//
// langid 只是一個 uint16。整條判斷因此是純邏輯,在 Ubuntu 上就跑得完測試。
//
#ifndef RIMEWIN_SCHEMA_CHOICE_H_
#define RIMEWIN_SCHEMA_CHOICE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace rimewin {

// §4.2 / §4.3 的字集。⚠ 認不出來一律 kUnspecified,**不要預設繁體** ——
// 猜錯的代價是使用者打出他不要的字,而且完全不知道為什麼。
enum class CharSet { kUnspecified = 0, kHant, kHans };

// §4.2:Windows 的來源是 TSF language profile 的 langid。
// 0 (不知道) 與非中文一律 kUnspecified。
CharSet CharSetOfLangId(uint32_t langid);

// §4.3 第 2 層:方案 id 的命名慣例。
//   `*_tw` `*_hk` `*_trad` `bopomofo*` → hant
//   `*_cn` `*_sc` `*_simp`             → hans
//   其餘                                → unspecified
//
// ⚠ **不要加「含 pinyin 就是簡體」這種規則**(規範明著寫的):
//   `luna_pinyin` 的預設輸出是**繁體**,猜錯會讓 simplification 被設成
//   相反的值,比不猜更糟。
//
// ⚠ 第 1 層(市集索引的 BCP 47 標籤)Windows 端還沒有 —— 方案市集這一輪
//   沒做,所以沒有已安裝紀錄可以查。有了之後那一層優先於本函式。
CharSet CharSetOfSchemaId(const std::string& schema_id);

// `text.variant` 的三態(§3「文字」)。
enum class VariantPref { kFollowInputMode = 0, kTraditional, kSimplified };

const char* VariantPrefToken(VariantPref v);  // 設定檔字面值
VariantPref VariantPrefFromToken(const std::string& s);

// §3「輸入方案」的 B 層設定。鍵名見 settings.h。
struct SchemaPreference {
  bool follow_input_mode = true;   // schemas.followInputMode,預設 true
  std::string pinned_global;       // schemas.pinnedGlobal
  std::string pinned_hant;         // schemas.pinnedHant
  std::string pinned_hans;         // schemas.pinnedHans
  VariantPref variant = VariantPref::kFollowInputMode;  // text.variant
};

struct SchemaChoice {
  // 空 = 不換方案(沿用 librime 現在選的)。
  std::string schema_id;
  // 要不要碰 simplification 這一組。false = **完全不呼叫 rs_set_option**。
  // ⚠ 「不碰」與「設成 false」不是同一件事(§3 的 followSchema 那一條):
  //    很多方案根本沒有那個開關,而有些方案的預設是 true。
  bool set_variant = false;
  bool simplified = false;   // set_variant 為 true 時才有意義
  const char* source = "";   // 只給日誌與 --print-choice,不參與判斷
};

// §4.4 的四層,由高到低:
//
//   1. 使用者為「這個輸入模式」釘的方案(pinnedHant / pinnedHans)
//   2. 使用者釘的單一方案(pinnedGlobal)
//   3. 已啟用清單中第一個字集相符的方案
//   4. 已啟用清單的第一個
//
// · 1 與 2 都是**使用者說的話**,即使字集不符也照做 —— 這時靠簡繁開關
//   把字集補齊,**不要偷偷換掉他選的方案**。
// · 釘的方案已經不在清單上 → 當作沒釘,往下走。不要選一個清單上沒有的東西。
// · 清單是空的 → 什麼都不做(呼叫端要在畫面上說「還沒有任何方案」)。
// · follow_input_mode 關掉時:方案只看 pinnedGlobal,而且**連簡繁都不碰**
//   (§4.5 最後一條:半套比不做更難理解)。
// 只做 §4.5 的那一格:要不要碰簡繁、以及要不要簡體。
// 抽出來是因為兩個地方要用同一份:建 session 的時候(ChooseSchema),
// 以及使用者在設定裡改了之後對**現有的**每一個 session 重套一次。
// 兩份會漂移,而漂移的症狀是「改設定當下沒變、換個程式就變了」。
bool DecideVariant(uint32_t langid, const SchemaPreference& pref,
                   bool* simplified);

SchemaChoice ChooseSchema(uint32_t langid,
                          const std::vector<std::string>& available,
                          const SchemaPreference& pref);

// ── 把決策翻成一串 rs_set_option ────────────────────────────────
//
// 見檔頭「與 §4.5 的一處差異」。simplified 之外還要維持 radio 的互斥,
// 否則 zh_hans 與 zh_hant_tw 同時為真,t2s 之後會再串一次 t2tw。
//
// hint 是「要繁體的時候,選哪一種繁體」:0x0C04 的使用者該拿到香港字形。
// 它只在 simplified == false 時有作用,而且**不是一個設定項** ——
// 使用者選的是「繁體」,是哪一種繁體由他的語言設定檔決定。
struct OptionAssign {
  const char* option;
  bool value;
};
std::vector<OptionAssign> PlanVariant(bool simplified, uint32_t langid_hint);

// radio group 的四個成員(luna_pinyin.schema.yaml 的 `options:`)。
extern const char* const kVariantOptions[4];
constexpr int kVariantOptionCount = 4;

// ── 服務暖機要各暖一組的 langid ──────────────────────────────────
//
// ⚠ 這份清單放在**純邏輯層**是刻意的,而理由是一次真的缺陷:
//
//   舊版的預熱只用 langid 0 暖一次,註解寫著「三份語言設定檔選到的都是
//   同一個方案,所以涵蓋得到全部三種使用者」。那句話**是對的,但不相干**:
//   貴的東西不在方案那一格,在**選項**。langid 0 走到 CharSet::kUnspecified,
//   DecideVariant 直接回 false,於是一個選項都不套;而真的使用者
//   (0x0804)會套上 simplification / zh_hans 那一組,那一組要載入
//   OpenCC 的 .ocd2 字典 —— 第一次用到才載。
//
//   結果是:**每一個新使用者的第一次打字**都在 SESSION_NEW 的 300 毫秒
//   預算裡付那筆錢,逾時、fail-open,打出一串英文,而且沒有任何錯誤訊息。
//   (2026-08-09,CI 的 §5c 冷啟動那一格。)
//
//   放在這裡,tests/test_schema_choice.cc 才驗得到「暖的那一組與真的用的
//   那一組是同一組」—— 而那是 Ubuntu 上跑得動的純邏輯,不必等一輪 CI。
extern const uint32_t kWarmUpLangIds[4];
constexpr int kWarmUpLangIdCount = 4;

// 給日誌用的短名,例如 0x0804 → "zh-Hans-CN"。認不得回傳 "?"。
const char* LangIdName(uint32_t langid);

}  // namespace rimewin

#endif  // RIMEWIN_SCHEMA_CHOICE_H_
