// windows/common/schema_choice.h — 「使用者從哪一個語言進來 → 該用哪個方案、哪種字形」
//
// ── 為什麼需要這一份 ──────────────────────────────────────────────
//
// Windows 的輸入法設定檔是**每個語言各註冊一份**的(見 tsf/guids.h):
// 0x0404 zh-Hant-TW、0x0804 zh-Hans-CN、0x0C04 zh-Hant-HK。使用者在
// 語言列上選的是其中一份,而**服務進程原本不知道是哪一份** —— 於是
// 預設方案永遠是 schema_list 的第一項 `luna_pinyin_tw`,
// **簡體使用者選了簡體那一份、打出來全是繁體字。**
// 那是使用者實際回報過的缺陷,不是理論問題。
//
// ── ⚠ 簡繁不是一個 bool,是一組互斥的 radio 開關 ──────────────────
//
// 本專案打包的 luna_pinyin 家族**沒有 `simplification` 這個開關**。
// 它用的是 luna_pinyin.schema.yaml 裡這一組:
//
//     - options: [ zh_hant, zh_hans, zh_hant_hk, zh_hant_tw ]
//       states:  [ 傳統漢字, 简化字,  香港字形,   臺灣字形 ]
//
// 而 `luna_pinyin_tw` / `bopomofo_tw` 只是同一份方案加上
// `switches/@2/reset: 3`(預設落在 zh_hant_tw)。
//
// Android 端在模擬器上實測過:**只送 `simplification` 完全沒有作用** ——
// 打 guojia 出來還是「國家」。所以兩件事都要做:
//
//   1. 送 `simplification`(給真的有這個開關的第三方方案,例如五筆·簡入繁出);
//   2. 在那組 radio 裡選中對的那一個,**並把同組的其他三個關掉**。
//
// 第 2 點的「關掉其他三個」不是防禦性程式碼:`rs_set_option` 只是把某個
// option 設成 true,radio group 的互斥是 librime 的 switcher **在使用者
// 從選單裡選**的時候才做的。我們繞過選單直接設,就得自己維持互斥 ——
// 否則 zh_hans 與 zh_hant_tw 同時為真,兩個 simplifier 會串起來
// (t2s 之後再 t2tw),輸出變成沒有人要的東西。
//
// ── 本檔刻意不 include windows.h ─────────────────────────────────
//
// langid 只是一個 uint16。整條「語言 → 方案」的判斷因此是純邏輯,
// 在 Ubuntu 上就跑得完測試(windows/run_logic_tests.sh)。
// TSF 那一層 CI 驗不了,但這一層驗得了 —— 而這一層正是缺陷所在。
//
#ifndef RIMEWIN_SCHEMA_CHOICE_H_
#define RIMEWIN_SCHEMA_CHOICE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace rimewin {

// ── 字形變體 ────────────────────────────────────────────────────
//
// 值刻意與 librime 的 option 名稱一一對應,而不是自己發明一套代號:
// 這張表要跟 luna_pinyin.schema.yaml 對得上,多一層翻譯就多一個漂移點。
enum class Variant {
  kFollow = 0,  // 不表示意見(沿用方案自己的 reset 值)
  kHant,        // zh_hant      傳統漢字
  kHans,        // zh_hans      简化字
  kHantHK,      // zh_hant_hk   香港字形
  kHantTW,      // zh_hant_tw   臺灣字形
};

// radio group 的四個成員,順序固定。互斥要靠自己維持,所以呼叫端
// 需要一份完整清單去關掉沒選中的那些。
extern const char* const kVariantOptions[4];
constexpr int kVariantOptionCount = 4;

// kFollow 回傳 nullptr;其餘回傳 librime 的 option 名稱。
const char* VariantOptionName(Variant v);

// 由 option 名稱反查。認不得回傳 kFollow。
Variant VariantFromOptionName(const std::string& name);

// 設定檔裡的字面值(見 settings.cc)。kFollow → ""。
const char* VariantToken(Variant v);
Variant VariantFromToken(const std::string& token);

// ── 要送給引擎的那一串 set_option ────────────────────────────────

struct OptionAssign {
  const char* option;
  bool value;
};

// 把「我要這個變體」翻譯成一串 rs_set_option 呼叫。
//
// saved_variant:使用者切到簡體之前停在哪一個變體。切回繁體時要還原它,
//   而**不是**硬設 zh_hant —— 本來停在「臺灣字形」的人繞一圈回來
//   會安靜地落到「傳統漢字」,而那兩者的差別小到他不會馬上發現,
//   只會覺得「有幾個字變了」。(Android 端踩過並修掉的同一個坑。)
//
// want == kFollow 時回傳**空的**清單:一個 set_option 都不送。
//   「沒有意見」與「選繁體」不是同一件事 —— 新裝的機器上把每個人
//   強制設成傳統漢字,等於替使用者做了他沒有做過的決定。
std::vector<OptionAssign> PlanVariant(Variant want, Variant saved_variant);

// ── 語言設定檔 → 預設方案 ───────────────────────────────────────

struct SchemaChoice {
  // 空字串 = 沒有意見,交給 schema_list 的第一項。
  std::string schema_id;
  Variant variant = Variant::kFollow;
  // 這個決定是怎麼來的。只給日誌與 --print-choice 用,不參與判斷。
  const char* source = "";
};

// 使用者在設定介面裡的覆寫。
struct SchemaPreference {
  // 「所有語言都用這個方案」。空 = 跟隨語言設定檔(預設)。
  std::string forced_schema;
  // 「所有語言都用這種字形」。kFollow = 跟隨語言設定檔(預設)。
  Variant forced_variant = Variant::kFollow;
  // 上一次在這個 langid 底下實際用的方案(使用者按 Ctrl+` 換過的)。
  // 空 = 沒有記錄。key 是 langid。
  std::vector<std::pair<uint32_t, std::string>> last_used;
};

// ⚠ 優先順序(這一段是規範,改了要同步改 windows/README.md 與
//    docs/coordination.md §5 的回報):
//
//   方案:
//     1. 設定介面的「所有語言都用這個方案」(forced_schema)
//     2. 這個 langid 上一次實際用的方案(last_used[langid])
//        —— 使用者按 Ctrl+` 換過就記下來,換到別的 app 不該被打回去
//     3. langid 推導的預設(下表)
//     4. 都不適用 → 空字串,交給 schema_list 第一項
//
//   字形:
//     1. 設定介面的「所有語言都用這種字形」(forced_variant)
//     2. langid 推導的預設(下表)
//     3. 都不適用 → kFollow,不送任何 set_option
//
//   ⚠ 第 1 項一旦設了就**對每一個語言都成立**。理由:使用者會在設定裡
//     選那一項,正是因為他不要我們替他猜。
//
// langid 推導表(只認中文;primary language 不是 0x04 一律沒有意見):
//
//   | LANGID | 語言        | 方案(依序取第一個裝得到的)      | 字形       |
//   |--------|-------------|----------------------------------|------------|
//   | 0x0404 | zh-Hant-TW  | luna_pinyin_tw → luna_pinyin     | zh_hant_tw |
//   | 0x0804 | zh-Hans-CN  | luna_pinyin → luna_pinyin_tw     | zh_hans    |
//   | 0x0C04 | zh-Hant-HK  | luna_pinyin → luna_pinyin_tw     | zh_hant_hk |
//   | 0x1004 | zh-Hans-SG  | luna_pinyin → luna_pinyin_tw     | zh_hans    |
//   | 0x1404 | zh-Hant-MO  | luna_pinyin → luna_pinyin_tw     | zh_hant_hk |
//
// SG 與 MO 目前**沒有註冊**(見 tsf/guids.h),表裡仍然列出來:哪天加了
// profile,這一格已經是對的,而不是安靜地落到「沒有意見」。
//
// ⚠ 候選清單刻意**不含** bopomofo_tw 與 t9_pinyin。前者是注音(鍵位完全
//   不同,拿它當拼音使用者的預設等於鍵盤壞了),後者是行動端的九宮格。
//   兩者都是使用者可以自己選的,但不可以是我們替他選的。
//
// available:目前 rs_schema_list() 列得出來的方案 id。空的話視為「還沒
//   部署完,不知道有什麼」,此時仍然回傳表上的第一順位 —— 選不到 librime
//   會拒絕,而拒絕是可見的;安靜地不選才是查不出來的那一種。
SchemaChoice ChooseSchema(uint32_t langid,
                          const std::vector<std::string>& available,
                          const SchemaPreference& pref);

// 只做「langid → 預設」那一格,不套使用者覆寫。給測試與診斷用。
SchemaChoice DefaultForLangId(uint32_t langid,
                              const std::vector<std::string>& available);

// langid 是不是我們認得的中文語言。
bool IsChineseLangId(uint32_t langid);

// 給日誌用的短名,例如 0x0804 → "zh-Hans-CN"。認不得回傳 "?"。
const char* LangIdName(uint32_t langid);

}  // namespace rimewin

#endif  // RIMEWIN_SCHEMA_CHOICE_H_
