// windows/common/net_policy.h — 離線守門的**判斷**與連網紀錄的格式
//
// 這一份是 Android 端 `net/NetworkGate.kt` + `net/NetworkLog.kt` 的移植。
// **邏輯照抄,不重新發明** —— 四端對「什麼算一次連線」的定義必須一樣,
// 否則使用者拿兩台裝置對照我們的紀錄時會看到兩套規則。
//
// 真正開 socket 的那一半在 `windows/service/net_gate.cc`,
// 那是整個專案**唯一**允許碰 WinHTTP 的翻譯單元(見下面的「單一出口」)。
// 這裡只有純判斷,所以在 Ubuntu 上就測得完。
//
// ── ⚠ 只記錄真的發生過的連線 ────────────────────────────────────
//
// Android 端 NetworkGate.kt 的原文(逐字):
//
//     // 刻意**不**記進連網紀錄:被擋下來的嘗試不是一次連網。
//     // 若把它記進去,「開關從沒開過 → 紀錄是空的」這句話就不成立了,
//     // 而那句話正是使用者驗證我們的方式。
//
// 具體規則:**只有在真的呼叫了連線 API(WinHttpSendRequest)之後才記一筆。**
//
//   記:連線失敗、HTTP 非 200、轉址(每一跳一筆)、超過大小上限、成功。
//   不記:被總開關擋下、網址解析不出來、scheme 不是 http/https、
//         以及任何只讀本機狀態的查詢。
//
// 而且紀錄裡**只有主機名**,沒有路徑、沒有查詢字串、沒有任何使用者輸入。
// 一個以隱私為賣點的程式把使用者的行為細節寫進本機檔案「以求透明」,
// 是自相矛盾的。
//
// ── 沒有主機白名單 ──────────────────────────────────────────────
//
// 這一點很容易「順手加上去」,但 Android 端刻意沒有,桌面端也不加:
// 索引來源是使用者可以改的,轉址可以落在任何主機。設計上讓目的地
// **看得見**(每一跳都記、主機名給使用者看),而不是限制它。
// 限制清單會給人「我們在替你把關」的錯覺,而它擋不住我們自己。
//
// 受限的只有 scheme:每一跳都必須是 http 或 https。
//
// ── 單一連網出口 ────────────────────────────────────────────────
//
// 主張是:整個 windows/ 底下,只有一個 .cc 檔碰得到網路 API。
// 由 `windows/audit_offline_win.sh` 在 CI 上用 grep 驗證,並且另外用
// `dumpbin /imports` 斷言 **`rime_tsf.dll` 沒有任何網路相依** ——
// 那個 DLL 住在每一個宿主進程裡(含瀏覽器與提權進程),它有網路能力
// 這件事光用讀原始碼是不會有人發現的。
//
#ifndef RIMEWIN_NET_POLICY_H_
#define RIMEWIN_NET_POLICY_H_

#include <cstdint>
#include <string>
#include <vector>

namespace rimewin {

// 用途。**封閉集合,不接受自由文字** —— 自由文字遲早會有人把 URL
// 或使用者輸入塞進去,而那是紀錄檔最不該有的東西。
enum class NetPurpose {
  kStoreIndex = 0,   // 瀏覽方案市集(取索引)
  kStorePackage,     // 下載方案套件
  kUpdateManifest,   // 查有沒有新版本(取線上的版本資訊)
  kUpdateSetup,      // 下載新版本的安裝程式
};

enum class NetOutcome {
  kOk = 0,
  kFailed,
  kRedirected,  // 既不是成功也不是失敗,但**是**一次真的連線
};

const char* NetPurposeToken(NetPurpose p);   // "STORE_INDEX" …
const char* NetPurposeText(NetPurpose p);    // 給人看的中文
bool NetPurposeFromToken(const std::string& s, NetPurpose* out);
const char* NetOutcomeToken(NetOutcome o);
const char* NetOutcomeText(NetOutcome o);
bool NetOutcomeFromToken(const std::string& s, NetOutcome* out);

struct NetLogEntry {
  int64_t at_ms = 0;
  std::string host;      // **只有主機名**
  NetPurpose purpose = NetPurpose::kStoreIndex;
  std::string label;     // 例如套件名。可為空
  NetOutcome outcome = NetOutcome::kFailed;
  int64_t bytes = 0;     // 只有 kOk 有意義
  std::string detail;    // 例如 "HTTP 404"。可為空
};

// 欄位長度上限。**這是安全控制,不是排版** ——
// host 來自轉址目標,是對方控制得了的字串;不做清理的話,
// 一個換行就能在紀錄裡偽造出多一筆看起來無害的連線。
constexpr size_t kMaxLogHost = 120;
constexpr size_t kMaxLogLabel = 80;
constexpr size_t kMaxLogDetail = 120;
constexpr size_t kDefaultMaxLogEntries = 500;

// TSV,七欄,一筆一行。選 TSV 不選 JSON 的理由與 Android 相同:
// 使用者(或稽核的人)直接 `type` 出來就看得懂。
std::string EncodeLogLine(const NetLogEntry& e);
// 欄數不是七、時間戳不是數字、用途或結果認不得 → 回 false。
// **絕不拋例外**:一行壞掉不可以毀掉整份紀錄。
bool DecodeLogLine(const std::string& line, NetLogEntry* out);

// 整份紀錄的讀寫(對字串,不碰檔案)。壞行安靜丟掉。
std::vector<NetLogEntry> DecodeLog(const std::string& text);
std::string EncodeLog(const std::vector<NetLogEntry>& entries);
// 追加並套用上限:超過就丟**最舊**的。回傳結果。
std::vector<NetLogEntry> AppendCapped(const std::vector<NetLogEntry>& cur,
                                      const NetLogEntry& add,
                                      size_t max_entries = kDefaultMaxLogEntries);

// ── 每一跳的判斷 ────────────────────────────────────────────────

enum class HopVerdict {
  kProceed = 0,     // 可以連。**只有這一種會產生紀錄**
  kBlockedBySwitch, // 總開關是關的。不記錄
  kBadUrl,          // 解析不出來。不記錄(還沒連線)
  kBadScheme,       // 不是 http/https。不記錄(還沒連線)
};

// enabled 由呼叫端從設定讀來。⚠ 讀不到、例外、未初始化 → 一律傳 false。
// 「不知道」必須等於「關」,不可以等於「開」。
HopVerdict CheckHop(bool enabled, const std::string& url);

// 轉址上限。++redirects > kMaxRedirects,所以跟得動 5 次轉址、最多 6 次連線。
constexpr int kMaxRedirects = 5;

// 大小上限。宣告的大小(Content-Length)只是參考,**真正的牆是邊收邊數**:
// 宣告值是對方給的。
constexpr int64_t kMaxIndexBytes = 4 * 1024 * 1024;
constexpr int64_t kMaxPackageBytes = 256 * 1024 * 1024;

// 只取主機名。**永不回傳空字串** —— 解析不出來就回原字串的前 60 個字元。
// 紀錄裡出現一個讀不懂的值,比出現一個空欄位好:空欄位看起來像被藏起來了。
std::string HostOf(const std::string& url);

// scheme 是不是 http / https(大小寫不拘)。
bool SchemeAllowed(const std::string& url);

// 相對網址解析。base 必須是絕對網址。
// 解析不出來就回傳 rel 原樣(呼叫端下一跳會再驗一次 scheme)。
std::string ResolveUrl(const std::string& base, const std::string& rel);

// 索引檔的 base_url + 檔名 → 絕對網址。
// 1) file 本身是 http(s):// 開頭 → 原樣;
// 2) base_url 非空 → 以它為基底;
// 3) 否則以 index_url 為基底。
std::string ResolvePackageUrl(const std::string& index_url,
                              const std::string& base_url,
                              const std::string& file);

}  // namespace rimewin

#endif  // RIMEWIN_NET_POLICY_H_
