// windows/common/sha256.h — SHA-256(純運算,不碰檔案、不碰網路)
//
// ── 為什麼自己寫,而不是叫 BCryptHashData ────────────────────────
//
// 唯一的理由是**它要測得到**。下載回來的那一包對不對,是這條線上最後
// 一個「說得出口的保證」;把它放在只有 Windows 上跑得起來的 API 後面,
// 等於把它交給一個開發時完全碰不到的東西 —— 而這個專案已經抓過太多次
// 「綠燈,因為它沒在跑」。這一份在 Ubuntu 上用 NIST 的公開向量驗,
// 每一輪 windows/run_logic_tests.sh 都會跑到。
//
// 成本很小:SHA-256 是一段沒有選擇餘地的規格,寫錯了向量會立刻紅。
//
// ── ⚠ 它擋得住什麼、擋不住什麼 ─────────────────────────────────
//
// 這一段與 service/net_gate.h 檔頭那一段是同一件事,寫兩次是刻意的:
// 摘要比對很容易被當成「安全驗證」,而它不是。
//
//   · 擋得住:**傳輸損壞與截斷**。半截的下載、壞掉的快取、中途斷線。
//   · 擋不住:**惡意替換**。那個 sha256 與安裝程式來自同一台伺服器、
//     同一條連線;能換掉其中一個的人可以同時換掉另一個。
//
// Windows 這一端沒有程式碼簽章,所以 TLS 是唯一的信任錨。這句話必須
// 出現在**使用者看得到的地方**(UiString::kUpdateTrustAnchor),不是
// 只寫在這裡。
//
#ifndef RIMEWIN_SHA256_H_
#define RIMEWIN_SHA256_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace rimewin {

// 串流式:下載是邊收邊算的,不會為了算摘要把 30 MB 再讀一次。
class Sha256 {
 public:
  Sha256();

  // ⚠ HexDigest() 之後再 Update 會被忽略(而不是安靜地算出一個錯的值)。
  void Update(const void* data, size_t n);

  // 64 個小寫十六進位字元。可以重複呼叫,結果相同。
  std::string HexDigest();

  static std::string HexOf(const std::string& data);

 private:
  void Block(const uint8_t* p);

  uint32_t h_[8];
  uint64_t bits_;
  uint8_t buf_[64];
  size_t buf_n_;
  bool done_;
  std::string hex_;
};

// 64 個十六進位字元(大小寫皆可)。
bool LooksLikeSha256Hex(const std::string& s);

// ⚠ 大小寫不敏感、前後空白不算數。兩端各自產生這個字串(發布端用
//   sha256sum,這裡用上面那支),格式上的細節不該變成「明明是同一個
//   檔案卻說不符」。內容不同才算不符。
bool Sha256HexEqual(const std::string& a, const std::string& b);

}  // namespace rimewin

#endif  // RIMEWIN_SHA256_H_
