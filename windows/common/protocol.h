// windows/common/protocol.h — 瘦 DLL 與服務進程之間的線路格式
//
// 這份檔案**刻意不引入 windows.h**。理由是驗證:TSF 在 CI 上驗不了,
// 但「編解碼對不對」是純邏輯,可以在任何機器上編譯並跑測試 ——
// 包含開發用的 Ubuntu。凡是能純邏輯化的都必須純邏輯化,
// 這是本輪對「CI 驗不了 TSF」的主要因應手段(見 windows/README.md)。
//
// ── 為什麼不直接傳結構體 ────────────────────────────────────────
//
// DLL 被載入到**每一個**接受文字輸入的進程裡,而服務進程是另外一支執行檔。
// 兩者可能來自不同的建置(使用者更新了一半、舊的 DLL 還被某個長壽的
// 宿主進程持有著)。直接 memcpy 結構體等於把「兩邊的編譯器與版本永遠一致」
// 當成前提,而那個前提會在最難查的時候破掉。所以一律明確序列化,
// 並在 HELLO 就把版本對清楚。
//
// ── 邊界 ────────────────────────────────────────────────────────
//
//   DLL 側     : TSF 協議 + 按鍵映射 + 本檔的編解碼 + 具名管道用戶端。
//                沒有 librime、沒有 YAML、沒有字型、沒有視窗。
//   服務進程側 : 本檔的編解碼 + rime_shell + librime + 候選窗。
//
#ifndef RIMEWIN_PROTOCOL_H_
#define RIMEWIN_PROTOCOL_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rimewin {

// 線路版本。
//
// ── v2 加了什麼、以及為什麼可以加 ────────────────────────────────
//
// v2 的 HELLO 多帶兩個欄位:使用者是從哪一個語言設定檔(langid /
// profile GUID)進來的。服務進程據此挑預設方案 —— 沒有這兩個欄位時
// 簡體使用者選了 zh-Hans 那一份,打出來全是繁體字(使用者實際回報過)。
//
// ⚠ **DLL 與服務可能來自不同的建置**(使用者更新到一半、或某個長壽的
//   宿主進程還握著舊的 DLL)。所以「加欄位」不是改一下結構體就好:
//
//   · 舊服務 + 新 DLL:舊服務的 DecodeHello 要求「剛好用完」,
//     多出來的兩個欄位會讓它整則丟掉、關掉連線。
//     → 所以新 DLL 在握手失敗時會**降級重試**一次 v1(見 ipc_client.cc
//       的 Handshake),v1 的位元組佈局一個位元都不變。
//       結果是:功能少一項(沒有 langid),但輸入法照常能用。
//   · 新服務 + 舊 DLL:新服務接受 [kMinProtocolVersion, kProtocolVersion]
//     區間內的版本,收到 proto=1 就照 v1 的佈局解,langid 留 0
//     (= 沒有意見,退回 schema_list 第一項)。
//
//   兩個方向都不會變成「按鍵被吃掉」——HELLO 對不上的既有行為仍然是
//   整條連線放棄,而放棄 = 按鍵原樣放行(見 link_state.h)。
//
// 編解碼由 Hello::proto 自己決定要不要讀尾巴,所以格式是自描述的:
// 拿 v1 的解碼器去解 v2 的訊息會在「剛好用完」那一關失敗,而不是
// 讀到半截當成有效資料。
inline constexpr uint32_t kProtocolVersion = 2;

// 服務端仍然接受的最舊版本。降到這個以下就真的不相容了。
inline constexpr uint32_t kMinProtocolVersion = 1;

// 單一訊息的上限。候選頁最多幾十個項目,1 MiB 綽綽有餘。
// 設上限的目的不是省記憶體,是讓「對面壞掉了送來一個 4GB 的長度」
// 這件事在解析的第一步就停下來,而不是變成一次巨大的配置。
inline constexpr uint32_t kMaxFrameBytes = 1u << 20;

enum class Op : uint8_t {
  // 用戶端 → 服務
  kHello = 0x01,
  kSessionNew = 0x02,
  kSessionEnd = 0x03,
  kKey = 0x04,
  kSelectCandidate = 0x05,
  kCommitComposition = 0x06,
  kClear = 0x07,
  kCaretRect = 0x08,
  kFocus = 0x09,
  kPing = 0x0A,
  kChangePage = 0x0B,
  kHighlight = 0x0C,
  kSelectSchema = 0x0D,
  // v2 起。單向,沒有回覆:叫服務進程把設定視窗叫出來(語言列按鈕按下去
  // 走的就是這一條)。⚠ 只有在協商到的版本 >= 2 時才可以送 ——
  // v1 的服務收到不認得的 op 會回錯誤並關掉連線。
  kOpenSettings = 0x0E,
  // 服務 → 用戶端
  kHelloOk = 0x81,
  kSessionOk = 0x82,
  kResult = 0x84,
  kPong = 0x8A,
  kError = 0xFF,
};

// ─────────────────────────── 訊息 ───────────────────────────

struct Hello {
  uint32_t proto = kProtocolVersion;
  // 用戶端編譯時看到的 RIME_SHELL_ABI_VERSION。服務端拿自己的
  // rs_abi_version() 對,不符就拒絕 —— 這是 rime_shell.h 檔頭要求的協商,
  // 只是跨了進程,所以得走線路。
  uint32_t shell_abi = 0;
  uint32_t host_pid = 0;
  std::string host_exe;  // 只作診斷用,服務端不得據此改變行為

  // ── proto >= 2 才在線路上 ────────────────────────────────────
  //
  // 使用者是從哪一個語言設定檔啟用這個輸入法的。0 = 不知道
  // (取不到、或對面是 v1 的 DLL)。**0 必須等於「沒有意見」,
  // 不可以等於任何一個具體語言** —— 猜錯的話,使用者會看到
  // 一個他沒選過的字形,而且找不到是誰改的。
  uint32_t input_langid = 0;
  // 設定檔的 GUID,大寫含大括號。目前只作診斷:langid 已經夠決定方案了,
  // 但三份 profile 各有自己的 GUID,日後要分辨「同一個語言底下的哪一份」
  // 時線路上已經有這個值,不必再動一次協議。
  std::string profile_guid;
};

struct HelloOk {
  uint32_t proto = kProtocolVersion;
  uint32_t shell_abi = 0;
  std::string service_version;
};

struct SessionOk {
  uint64_t session = 0;
};

struct KeyReq {
  uint64_t session = 0;
  int32_t keysym = 0;
  uint32_t mods = 0;  // RS_MOD_* 位元(rime_shell.h),不是 librime 的遮罩
};

// select / highlight / change_page / focus 共用:一個 session 加一個整數參數。
struct ArgReq {
  uint64_t session = 0;
  int32_t arg = 0;
};

// 指定方案。前端(或驗證用的 probe)要能明確選一個方案 ——
// librime 會把「上次選的方案」記在使用者目錄的 user.yaml 裡,
// 所以「不指定」的結果取決於那個目錄的歷史,不是一件確定的事。
struct SchemaReq {
  uint64_t session = 0;
  std::string schema_id;
};

struct CaretRect {
  uint64_t session = 0;
  int32_t left = 0, top = 0, right = 0, bottom = 0;
};

struct Candidate {
  std::string text;
  std::string comment;
  std::string label;
};

// rs_status 的布林旗標打包成位元,省得每加一個就動線路格式。
enum StatusFlag : uint32_t {
  kStComposing = 1u << 0,
  kStAsciiMode = 1u << 1,
  kStFullShape = 1u << 2,
  kStSimplified = 1u << 3,
  kStAsciiPunct = 1u << 4,
  kStDisabled = 1u << 5,
};

struct Snapshot {
  bool has_commit = false;
  std::string commit_text;

  std::string preedit;
  int32_t sel_start = 0, sel_end = 0, caret = 0;

  std::vector<Candidate> items;
  int32_t page_no = 0;
  int32_t highlighted = -1;
  bool is_last_page = true;

  std::string schema_id;
  std::string schema_name;
  uint32_t status_flags = 0;
};

struct Result {
  // 宿主要不要吃掉這顆按鍵。
  bool handled = false;
  Snapshot snap;
};

struct ErrorMsg {
  uint32_t code = 0;
  std::string text;
};

// ─────────────────────────── 編碼 ───────────────────────────
//
// 每則 payload 的前 5 個位元組固定是 `u8 op` + `u32 seq`(小端)。
// seq 由用戶端遞增,服務端原樣回填 —— 逾時之後遲到的回覆才認得出來要丟掉。
// (沒有 seq 的話,一次逾時會讓之後每一次請求都讀到前一次的答案,
//  而那種錯位在使用者眼裡是「輸入法偶爾慢一拍」,幾乎查不出來。)

std::string EncodeHello(uint32_t seq, const Hello& m);
std::string EncodeHelloOk(uint32_t seq, const HelloOk& m);
std::string EncodeSessionNew(uint32_t seq);
std::string EncodeSessionOk(uint32_t seq, const SessionOk& m);
std::string EncodeSessionEnd(uint32_t seq, uint64_t session);
std::string EncodeKey(uint32_t seq, const KeyReq& m);
std::string EncodeArg(uint32_t seq, Op op, const ArgReq& m);
std::string EncodeCaretRect(uint32_t seq, const CaretRect& m);
std::string EncodeSelectSchema(uint32_t seq, const SchemaReq& m);
// EncodeHello 依 m.proto 決定要不要寫尾巴的兩個欄位,所以同一支程式
// 可以產生 v1 與 v2 兩種訊息 —— 降級重試靠的就是這件事。
std::string EncodeSimple(uint32_t seq, Op op, uint64_t session);
std::string EncodePing(uint32_t seq);
std::string EncodePong(uint32_t seq);
std::string EncodeResult(uint32_t seq, const Result& m);
std::string EncodeError(uint32_t seq, const ErrorMsg& m);

// ─────────────────────────── 解碼 ───────────────────────────
//
// 全部回傳 bool。**沒有例外、沒有 assert** —— 這些函式會在 DLL 裡跑,
// 而 DLL 在宿主進程裡,拋出去的例外會變成宿主崩潰。
// 位元組來自另一個進程,一律視為不可信輸入。

bool PeekHeader(const std::string& payload, Op* op, uint32_t* seq);

bool DecodeHello(const std::string& p, uint32_t* seq, Hello* out);
bool DecodeHelloOk(const std::string& p, uint32_t* seq, HelloOk* out);
bool DecodeSessionOk(const std::string& p, uint32_t* seq, SessionOk* out);
bool DecodeKey(const std::string& p, uint32_t* seq, KeyReq* out);
bool DecodeArg(const std::string& p, uint32_t* seq, ArgReq* out);
bool DecodeCaretRect(const std::string& p, uint32_t* seq, CaretRect* out);
bool DecodeSelectSchema(const std::string& p, uint32_t* seq, SchemaReq* out);
bool DecodeSimple(const std::string& p, uint32_t* seq, uint64_t* session);
bool DecodeResult(const std::string& p, uint32_t* seq, Result* out);
bool DecodeError(const std::string& p, uint32_t* seq, ErrorMsg* out);

// ─────────────────────────── 分幀 ───────────────────────────
//
// 具名管道即使開在訊息模式,ReadFile 仍可能因為緩衝區太小而給出半則訊息
// (ERROR_MORE_DATA)。所以線路上一律自己帶長度:`u32 len` + payload。
// 這個類別把「位元組流 → 一則則 payload」獨立出來,好單獨測
// 「切成一個位元組一個位元組餵進來」與「長度欄位是垃圾」兩種情形。

std::string Frame(const std::string& payload);

class FrameReader {
 public:
  // 追加收到的位元組。回傳 false 代表串流已經不可信(長度超過上限),
  // 呼叫端唯一正確的反應是關掉這條連線 —— 不要嘗試重新同步,
  // 重新同步等於拿垃圾當訊息解。
  bool Feed(const char* data, size_t len);

  // 取出下一則完整的 payload;沒有完整訊息時回傳 false。
  bool Next(std::string* out);

  bool broken() const { return broken_; }
  size_t buffered() const { return buf_.size() - consumed_; }

 private:
  std::string buf_;
  size_t consumed_ = 0;
  bool broken_ = false;
};

}  // namespace rimewin

#endif  // RIMEWIN_PROTOCOL_H_
