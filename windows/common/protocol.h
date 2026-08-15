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
// ── v3 加了什麼 ──────────────────────────────────────────────────
//
// v3 的 HELLO 多帶一個欄位:**啟用我們的那一條 TSF 執行緒的 tid**。
// 服務端拿它跟 GetForegroundWindow() 那一條執行緒比,才答得出
// 「使用者此刻正在用的那一個輸入位置上,啟用中的是不是我們」——
// 也就是懸浮那一橫該不該顯示(common/bar_owner.h)。
//
// ⚠ 為什麼是 tid 不是 pid:輸入法在 Windows 上是 **per-thread** 的
//   (系統設定裡「讓我為每個應用程式視窗使用不同的輸入法」把這件事做成
//   使用者看得見的事實)。用 pid 的話,同一支程式的另一個視窗上使用者
//   正在用微軟拼音,我們照樣會顯示 —— 那正是這一輪在修的錯,只是
//   作用域小一階。
//
// ⚠ 相容性與 v2 那一段一模一樣:舊服務收到 v3 會在「剛好用完」那一關
//   整則丟掉,而新 DLL 會降級重試(ipc_client.cc 的 Handshake);
//   新服務收到 v1 / v2 就照它宣告的版本解,host_tid 留 0。
//   **0 必須等於「報不出來」**,而 bar_owner.h 對報不出 tid 的用戶端
//   退回去比 pid —— 精確度差一階,但不會讓舊 DLL 的使用者失去那一橫。
//
// ── v4 加了什麼 ──────────────────────────────────────────────────
//
// v4 **沒有動 HELLO 的位元組佈局**,它只多了一則客戶端→服務端的訊息:
// kProfileState(0x0F)。宿主用它說出「這條執行緒上啟用中的**不再是**
// 我們」—— 在 v4 之前,那句話唯一的表達方式是把在場連線關掉,而
// 「關掉」與「宿主死了 / 宿主被砍掉 / 宿主根本沒載入我們」在服務端
// 長得一模一樣。那個歧義就是 #111:服務端只好把「查不到」讀成
// 「是別人的」,於是截圖工具、工作列、桌面一搶到前景那一橫就消失。
//
// ⚠ **升版本身不會擋掉任何舊 DLL**,這一點查過而不是推測的:
//   · 舊 DLL(proto=2/3)→ 新服務:pipe_server.cc 的關卡是
//     `h.proto < kMinProtocolVersion || h.proto > kProtocolVersion`,
//     2 與 3 都落在 [1,4] 內 → **接受**,而且回的是協商出來的
//     `ok.proto = h.proto`,用戶端那一格 `ok.proto != proto_` 也對得上。
//     舊 DLL 一切照舊,只是永遠不會送 kProfileState。
//   · 新 DLL(proto=4)→ 舊服務(上限 3):因為 v4 沒有新欄位,舊服務的
//     DecodeHello 解得完(它走 proto>=3 那一支,AtEnd() 成立),接著才
//     在版本區間那一關回 kError 並關連線 → PresenceLink 讀到的不是
//     HELLO_OK → kRejected → `--proto_` 降到 3 重試 → 成功。
//   所以 v4 是**純加法**:兩個方向都退化成「少一項功能」,不是「連不上」。
inline constexpr uint32_t kProtocolVersion = 4;

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
  // v4 起。單向,沒有回覆,走 EncodeSimple / DecodeSimple 的形狀
  // (`op` + `seq` + 一個 u64)。那個 u64 **不是 session**,是一個布林:
  //
  //     0 = 這條連線所在的那條執行緒上,啟用中的**不再是我們**
  //     1 = 又是我們了
  //
  // ⚠ 這是服務端唯一拿得到的「別人的」正面證據。少了它,服務端只能從
  //   「這條執行緒上查不到任何在場連線」推出「使用者切走了」,而那個
  //   推論對截圖工具、工作列、桌面、以及**任何在升級前就開著、還抱著
  //   舊 DLL 的宿主**全部是錯的(#111)。
  //
  // ⚠ 只有協商到的版本 >= 4 才准送 —— v3 以下的服務收到不認得的 op 會
  //   回 kError 並關掉連線(kOpenSettings 在 v2 立過同一條規矩)。
  kProfileState = 0x0F,
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

  // ── proto >= 3 才在線路上 ────────────────────────────────────
  //
  // 啟用我們的那一條 TSF 執行緒。**不是**宿主的主執行緒,也不是送這則
  // HELLO 的那條背景執行緒 —— 是呼叫 ActivateEx 的那一條。
  // 0 = 報不出來(v1 / v2 的 DLL)。見上面 v3 那一段。
  uint32_t host_tid = 0;
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
  // 簡繁那一格的內容可不可信(§12.10.4 的三態)。
  //
  //   為假 → **整格不顯示**(kStSimplified 這時沒有意義,不得拿來畫繁體)
  //   為真 → 看 kStSimplified
  //
  // ⚠ 為什麼要多一個位元:那一格有三態,而一個 kStSimplified 表不出三態。
  //   「不知道」與「繁體」是兩件事 —— 引擎剛載入一個沒有宣告字形開關的
  //   方案時,我們**確實**不知道它在做哪一種轉換,此時畫任何一個字都是
  //   在替一件沒有發生的事作證。
  //
  // ⚠ 這是**純加法**:它住在既有的那個 u32 裡,不改變任何欄位的位置或
  //   長度。DLL 與服務可能來自不同的建置(那個 DLL 住在瀏覽器裡,而
  //   瀏覽器可以開著好幾天),舊的那一端只是看不見這個位元,不會解錯。
  //   tests/test_proto_compat.cc 兩個方向都驗。
  kStVariantKnown = 1u << 6,
  // ── 「引擎沒有回答」與「引擎回答了、它不要這顆鍵」是兩件事 ──────
  //
  // ⚠ 這一格是 §13c 那個「ni好」的根。在它之前,線路上這兩件事長得
  //   **一模一樣**(Result.handled = false):
  //
  //     · 引擎回答了、它不要這顆鍵 —— 英數模式下的每一顆字母、
  //       朗月拼音底下的數字。這條路每天都會走到,而 DLL 唯一正確的
  //       反應是**自己把那個字元補進文件**(它在 OnTestKeyDown 已經
  //       宣告吃掉了,見 key_eat_policy.h)。
  //     · 引擎根本沒表態 —— service/engine.cc 的三個出口:部署中的
  //       fail-open(:617)、CallKeyBounded 逾時(:684)、以及工作跑完
  //       但 Find(id) 不認得那個 session(:676)。
  //
  //   DLL 分不出來,所以只好一律補字元。而其中一半的情況引擎根本沒
  //   表態 —— 於是使用者升級之後在 LINE 的訊息框裡拿到「ni好」:
  //   前兩個字母是引擎沒回答時我們替他打的,後面「好」是引擎回來之後
  //   上屏的。**沒有紅字、沒有提示,那一橫還顯示「中」。**
  //   整串英文(nihao1)他一眼看得出輸入法沒作用;半串看起來像他自己
  //   打錯字。這與 Android 的 #105 是同一個缺陷,只差平台。
  //
  // ── 為什麼是一個位元,不是 proto v5 多一個欄位 ────────────────
  //
  // ⚠ key_deadline.h:201-212 否決過「多一個協議欄位」,理由是 Result 的
  //   DecodeResult 要求「剛好用完」—— 在尾巴**加欄位**會讓舊 DLL 整則
  //   丟掉。那個理由對**加欄位**成立,對這裡不成立:這個位元住在既有的
  //   那個 u32 裡,位元組佈局一個位元都沒有變,舊 DLL 照樣解得完、
  //   只是看不見它 —— 與 kStVariantKnown 完全同一種加法,
  //   而 tests/test_proto_compat.cc 兩個方向都驗。
  //
  // ⚠ 極性只准是這一個方向:**有這個位元 = 確定沒回答**。
  //   不可以反過來寫成「沒有這個位元 = 確定回答了」——
  //   舊服務送來的每一份快照這一格都是 0,而它們大多是真的回答過的。
  //   common/service_state.cc 那一段(「舊服務配新 DLL 時它一定是 0」)
  //   講的正是這件事:新位元只能當**負面證據**用。
  kStKeyNotAnswered = 1u << 7,
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
