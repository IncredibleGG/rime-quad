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

// 線路版本。任何不相容的變更都必須遞增,HELLO 對不上就整條連線放棄
// (而放棄的行為是「按鍵原樣放行」,不是「吃掉按鍵」——見 client_link.h)。
inline constexpr uint32_t kProtocolVersion = 1;

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
