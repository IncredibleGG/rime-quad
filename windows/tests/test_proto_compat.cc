// windows/tests/test_proto_compat.cc — 線路版本 1 ⇄ 2 的相容性
//
// **DLL 與服務可能來自不同的建置。** 使用者更新到一半、或某個長壽的宿主
// 進程還握著舊的 DLL —— 這不是理論,那個 DLL 住在瀏覽器裡,而瀏覽器可以
// 開著好幾天。所以「HELLO 加兩個欄位」必須把兩個方向都想清楚:
//
//   舊服務 + 新 DLL → 新 DLL 降級重試 v1(功能少一項,輸入法照常能用)
//   新服務 + 舊 DLL → 服務照 v1 解,langid 留 0 = 沒有意見
//
// 而**任何一種對不上,結果都必須是「按鍵原樣放行」而不是「按鍵被吃掉」**。
// 那條不變式由 link_state.h 守,這裡守的是它前面那一格:編解碼本身。

#include <string>

#include "../common/protocol.h"
#include "check.h"

using namespace rimewin;

namespace {

// 手寫一則 **v1** 的 HELLO,一個位元組一個位元組地擺。
//
// 不呼叫 EncodeHello 是刻意的:這一則的意義是「舊版的 DLL 送出來的東西
// 長這樣」,而舊版的程式碼已經不在這棵樹裡了。用今天的編碼器去產生它,
// 等於拿被測物去定義期望值 —— 哪天 v1 的佈局被改壞,這個測試會跟著改壞
// 而不會紅。
std::string HandWrittenV1Hello(uint32_t seq, uint32_t abi, uint32_t pid,
                               const std::string& exe) {
  std::string s;
  auto u32 = [&s](uint32_t v) {
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  };
  s.push_back(static_cast<char>(0x01));  // Op::kHello
  u32(seq);
  u32(1);  // proto = 1
  u32(abi);
  u32(pid);
  u32(static_cast<uint32_t>(exe.size()));
  s += exe;
  return s;
}

// **舊服務的解碼器**,照 v1 的原始碼逐行重寫。
//
// 為什麼要在測試裡養一份舊的解碼器:v2 的 DecodeHello 是版本感知的,
// 拿它去問「舊的會不會拒絕」永遠會得到「會接受」。要證明相容性,
// 就得有一份真的只認得 v1 的東西 —— 而它必須是手寫的,
// 因為舊版的程式碼已經不在這棵樹裡了。
bool DecodeHelloAsV1Only(const std::string& p, uint32_t* seq, uint32_t* proto) {
  size_t i = 0;
  auto u8 = [&](uint8_t* out) {
    if (p.size() - i < 1) return false;
    *out = static_cast<uint8_t>(p[i++]);
    return true;
  };
  auto u32 = [&](uint32_t* out) {
    if (p.size() - i < 4) return false;
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k)
      v |= static_cast<uint32_t>(static_cast<unsigned char>(p[i + k])) << (8 * k);
    i += 4;
    *out = v;
    return true;
  };
  uint8_t op = 0;
  if (!u8(&op) || op != 0x01) return false;
  if (!u32(seq)) return false;
  uint32_t abi = 0, pid = 0, n = 0;
  if (!u32(proto)) return false;
  if (!u32(&abi)) return false;
  if (!u32(&pid)) return false;
  if (!u32(&n)) return false;
  if (p.size() - i < n) return false;
  i += n;
  return i == p.size();  // v1 的「剛好用完」
}

}  // namespace

TEST(Proto_v2_is_the_current_version_and_v1_is_still_accepted) {
  CHECK_INT(kProtocolVersion, 2);
  CHECK_INT(kMinProtocolVersion, 1);
  CHECK(kMinProtocolVersion <= kProtocolVersion);
}

TEST(Proto_new_service_decodes_an_old_client_hello) {
  const std::string wire = HandWrittenV1Hello(7, 1, 4242, "C:\\a\\notepad.exe");
  uint32_t seq = 0;
  Hello h;
  CHECK(DecodeHello(wire, &seq, &h));
  CHECK_INT(seq, 7);
  CHECK_INT(h.proto, 1);
  CHECK_INT(h.host_pid, 4242);
  CHECK_STR(h.host_exe, "C:\\a\\notepad.exe");
  // ⚠ 舊用戶端沒有 langid。0 必須等於「沒有意見」,不可以等於任何一個
  //   具體語言 —— 否則舊 DLL 的使用者會拿到一個他沒選過的字形。
  CHECK_INT(h.input_langid, 0);
  CHECK(h.profile_guid.empty());
}

TEST(Proto_v1_encoder_output_is_byte_identical_to_the_old_layout) {
  // 降級重試靠的是「同一支程式送得出 v1 訊息」。若 v1 的位元組佈局被動過,
  // 舊服務就解不開,而症狀是「更新之後某些程式裡輸入法整個消失」。
  Hello h;
  h.proto = 1;
  h.shell_abi = 1;
  h.host_pid = 4242;
  h.host_exe = "C:\\a\\notepad.exe";
  h.input_langid = 0x0804;      // 設了也不該被寫出去
  h.profile_guid = "{ABC}";     // 同上
  const std::string got = EncodeHello(7, h);
  CHECK_STR(got, HandWrittenV1Hello(7, 1, 4242, "C:\\a\\notepad.exe"));
}

TEST(Proto_old_service_rejects_a_v2_hello_cleanly) {
  // 舊解碼器對 v1 的訊息照常運作 —— 先證明這份重寫的東西不是恆假。
  uint32_t seq = 0, proto = 0;
  CHECK(DecodeHelloAsV1Only(HandWrittenV1Hello(9, 1, 1, "x"), &seq, &proto));
  CHECK_INT(seq, 9);
  CHECK_INT(proto, 1);

  // 而 v2 的訊息它整則丟掉(尾巴多出來,「剛好用完」不成立)。
  // 丟掉 = 關掉連線 = 用戶端 fail-open = **按鍵原樣放行**。
  // 這正是我們要的:功能少一項,而不是按鍵消失。
  Hello h;
  h.proto = 2;
  h.shell_abi = 1;
  h.host_pid = 1;
  h.host_exe = "x";
  h.input_langid = 0x0804;
  h.profile_guid = "{4F78BA11-E997-4BD7-8B97-F4553ABC0B18}";
  const std::string v2 = EncodeHello(9, h);
  CHECK(!DecodeHelloAsV1Only(v2, &seq, &proto));
  // 而且它比 v1 長:欄位真的加上去了(否則上面那條會因為別的理由通過)。
  CHECK(v2.size() > HandWrittenV1Hello(9, 1, 1, "x").size());
}

TEST(Proto_v2_hello_roundtrip) {
  Hello h;
  h.proto = 2;
  h.shell_abi = 1;
  h.host_pid = 999;
  h.host_exe = "C:\\Program Files\\Some App\\app.exe";
  h.input_langid = 0x0C04;
  h.profile_guid = "{C6B736EB-38E3-4041-B59B-ECF91AD8E28A}";
  uint32_t seq = 0;
  Hello back;
  CHECK(DecodeHello(EncodeHello(3, h), &seq, &back));
  CHECK_INT(seq, 3);
  CHECK_INT(back.proto, 2);
  CHECK_INT(back.input_langid, 0x0C04);
  CHECK_STR(back.profile_guid, h.profile_guid);
  CHECK_STR(back.host_exe, h.host_exe);
}

TEST(Proto_truncated_v2_hello_is_rejected_not_half_read) {
  // 宣告 proto=2 卻少了尾巴 = 對面壞了或有人在中間動手腳。
  // 半讀半猜是最糟的選擇:langid 會變成一個從未被寫入的值。
  Hello h;
  h.proto = 2;
  h.shell_abi = 1;
  h.host_pid = 1;
  h.host_exe = "x";
  h.input_langid = 0x0404;
  const std::string full = EncodeHello(1, h);
  for (size_t cut = 1; cut < full.size(); ++cut) {
    uint32_t seq = 0;
    Hello back;
    CHECK(!DecodeHello(full.substr(0, cut), &seq, &back));
  }
}

TEST(Proto_open_settings_op_value_is_pinned) {
  // 這個 op 只在協商到 v2 之後才准送。值寫死在測試裡,是因為改了它
  // 等於改了線路格式,而症狀會是「語言列的設定按鈕在舊服務上把連線弄斷」。
  CHECK_INT(static_cast<int>(Op::kOpenSettings), 0x0E);
  const std::string p = EncodeSimple(5, Op::kOpenSettings, 77);
  Op op;
  uint32_t seq = 0;
  CHECK(PeekHeader(p, &op, &seq));
  CHECK(op == Op::kOpenSettings);
  CHECK_INT(seq, 5);
  uint64_t sess = 0;
  CHECK(DecodeSimple(p, &seq, &sess));
  CHECK_INT(sess, 77);
}
