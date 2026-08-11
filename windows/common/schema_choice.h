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

// ── 一整份選項計畫:建 session 的兩條路徑只能有這一份 ──────────
//
// ⚠ 這一支存在的理由是一個真的缺陷,不是整理。
//
//   service/main.cc 暖機那一段有一句註解寫著「這一段必須與 pipe_server 的
//   kSessionNew 逐字相同」。它已經漂了:pipe_server 後來補上了
//   `ascii_mode`(使用者在那一橫上切成英文之後,新開的程式也要是英文的),
//   而暖機那一份沒有跟著補。
//
//   後果不是「暖機少設一個選項」,是**整個預熱失效**:
//   Engine::TakeSpareSession 用 SameOptions 比對計畫,而 SameOptions 第一件事
//   就是比長度。長度不同 → 每一個預熱好的備用 session 都被判成過期、
//   當場丟掉、當場重建,而 rs_session_create 量到過 442~753 毫秒,
//   SESSION_NEW 的預算是 300 毫秒。也就是說:那筆錢又回到使用者
//   等待的那一趟裡,而畫面上什麼異狀都沒有。
//
//   「一句註解要求兩段程式碼逐字相同」本來就不是一個守得住的約定。
//
// 順序是契約的一部分(SameOptions 是**逐項**比對,不是集合比對):
//
//     1. 簡繁那一組(choice.set_variant 為真時才有,見 PlanVariant)
//     2. ascii_punct(punctuation != kUnset 時才有)
//     3. ascii_mode(**永遠有**)
//
// ⚠ punctuation 的 kUnset = followSchema = **完全不出現在計畫裡**。
//   設成 false 不是同一件事:很多方案根本沒有那個開關,而有些方案的
//   預設是 true。
//
// ⚠ ascii_mode 沒有「不干預」那一態 —— 它是一個模式,不是三態偏好,
//   而它的預設(false = 中文)就是 librime 的預設。
//
// ⚠ Tri 定義在 settings.h,而 settings.h 反過來 include 本檔 ——
//   所以這裡用不透明宣告,不能 include 它。scoped enum 的底層型別
//   預設就是 int,兩處宣告一致。
enum class Tri : int;

std::vector<OptionAssign> BuildOptionPlan(const SchemaChoice& choice,
                                          uint32_t langid,
                                          Tri punctuation,
                                          bool ascii_mode);

// ── 改完簡繁設定之後,備用池的那一份計畫要怎麼更新 ────────────────
//
// ⚠ 這一支存在的理由是 ac4a721(「改完簡繁設定之後,備用 session 池不再
//   整池報廢」)在**它唯一針對的情境下恆定失效**,而那個失效一支自動化
//   測試都看不到 —— 那段程式碼在 service/engine.cc 裡,Ubuntu 上編不動。
//   抽成純函式之後,run_logic_tests.sh 就驗得到。
//
//   舊做法是「逐項就地覆蓋 value、保留舊順序」。但 PlanVariant 的**名字
//   順序**隨 simplified / langid 而變(「先關再開」把被跳過的那一個排到
//   最後),而 SameOptions 是**逐項**比對:
//
//     備用池的計畫(繁,0x0404)
//       simplification=0 zh_hant=0 zh_hans=0 zh_hant_hk=0 zh_hant_tw=1
//     就地覆蓋成簡體之後
//       simplification=1 zh_hant=0 zh_hans=1 zh_hant_hk=0 zh_hant_tw=0
//     SESSION_NEW 重算的計畫
//       simplification=1 zh_hant=0 zh_hant_hk=0 zh_hant_tw=0 zh_hans=1
//
//   值全對、順序不同 → SameOptions 回 false → 備用池仍然整池報廢。
//   簡繁只要**真的變了**,順序就一定變,所以那一段從來沒有生效過。
//
// 正確的做法是把簡繁那一組**整組換掉**,而且擺回它在 BuildOptionPlan
// 裡的位置(整份計畫的最前面),其餘照原順序保留。這樣算出來的計畫與
// SESSION_NEW 那一趟重算的**逐項相同**。
//
// ⚠ set_variant 為 false(使用者把 followInputMode 關掉 = 「我自己管」)
//   時要把那一組**整組拿掉**,不是留著舊值 —— BuildOptionPlan 在那個
//   情境下產出的計畫裡一個 variant 選項都沒有,留著長度就對不上,
//   一樣整池報廢。這是同一個缺陷的第二個入口,舊做法連碰都沒碰到
//   (它在 !set_variant 時直接 continue)。
//
// ⚠ 為什麼不改成「讓 SameOptions 與順序無關」:那一份計畫不只是比對用的
//   鍵,它同時是**重放腳本** —— Engine::MakeSpareOnEngineThread 會照它
//   逐項 rs_set_option。而順序在那裡是有作用的(rs_set_option 不維持
//   radio 的互斥,「先關再開」是刻意的)。把比對改成集合比對,等於接受
//   一份**套用順序是錯的**計畫,把缺陷藏起來而不是修掉。所以留著嚴格的
//   逐項比對,修產出那一端。
std::vector<OptionAssign> UpdateVariantInPlan(
    const std::vector<OptionAssign>& plan, bool set_variant, bool simplified,
    uint32_t langid);

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

// ── 換方案之前,簡繁偏好要拿哪一份 ──────────────────────────────
//
// ⚠ 這一支存在的理由是 648c02c(「換方案洗掉簡繁」)這個**使用者看得到
//   的行為改變**,它唯一的守門在 windows/verify_installer.sh §6g 案例二
//   —— 一支只有 Windows 跑得動的腳本。實跑覆核過:把 pipe_server.cc 的
//   那一行刪掉,三支守門全綠。也就是說那個修法在開發機上等於沒有守門。
//
// ── 它在守什麼 ─────────────────────────────────────────────────
//
//   `Engine::SelectAndApply` 換完方案要重套簡繁(librime 每次載入方案都會
//   把 switches 重設回方案宣告的值),而它用的是 `Engine::variant_pref_`。
//   那是**設定的複本**,engine.h 自己寫著「不是真相的來源,真相在設定檔」,
//   更新它的只有服務啟動時與設定視窗改簡繁時兩處。設定檔在服務跑著的
//   時候被別人改掉(設定視窗有一顆「用記事本開啟設定檔」),這份複本就
//   過期了 —— 而換一次方案就會拿過期的那一份把使用者剛選的簡繁洗掉。
//
//   SESSION_NEW 那一條路每一次都重讀設定檔,所以它是對的;
//   SESSION_SELECT_SCHEMA 那一條沒有。這一支就是「那一條路該拿哪一份」
//   這個判斷本身,抽出來讓 Ubuntu 上的 run_logic_tests.sh 驗得到。
//
// ⚠ 這一支**驗不到**「pipe_server.cc 真的呼叫了它」。那一格由
//   windows/audit_single_source.sh 的規則 3 在原始碼層面守
//   (windows/service/ 在 Ubuntu 上編不起來,所以只能這樣守)。
//   兩件事缺一不可:這裡守判斷、那裡守接線。
struct VariantPrefPick {
  // 要交給 Engine::SetVariantPref 的那一份。
  SchemaPreference use;
  // true = 用的是設定檔那一份(正常情況)。false = 讀不到設定,只能沿用
  // 引擎手上的複本 —— fail-open:讀不到設定不該讓換方案整個失敗。
  bool from_settings_file = false;
  // 複本與設定檔不同 = 複本過期了。**只給日誌**,不參與判斷 ——
  // 判斷永遠是「設定檔贏」,不是「不一樣的時候才更新」。
  // (「只有不一樣才更新」與「一律更新」在結果上相同,但前者要求兩邊的
  //  比較函式永遠正確;比較函式漏一個欄位,症狀就是這個缺陷本身。)
  bool engine_copy_was_stale = false;
};

// 逐欄位比對。給 engine_copy_was_stale 用,順便讓「漏了哪一欄」測得到。
bool SameSchemaPreference(const SchemaPreference& a, const SchemaPreference& b);

VariantPrefPick PickVariantPrefForSchemaSwitch(
    bool settings_readable, const SchemaPreference& on_disk,
    const SchemaPreference& engine_copy);

}  // namespace rimewin

#endif  // RIMEWIN_SCHEMA_CHOICE_H_
