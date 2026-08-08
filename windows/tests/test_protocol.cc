// windows/tests/test_protocol.cc — 線路格式
//
// 這一組測試的假想敵不是「我方寫錯」,是「對面壞掉或不是我方」。
// DLL 跑在**每一個**宿主進程裡,解析錯誤在那裡等於宿主崩潰。

#include "../common/protocol.h"

#include <string>
#include <vector>

#include "check.h"

using namespace rimewin;

namespace {

Snapshot SampleSnapshot() {
  Snapshot s;
  s.has_commit = true;
  s.commit_text = "你好";
  s.preedit = "ㄋㄧˇㄏㄠˇ";
  s.sel_start = 3;
  s.sel_end = 9;
  s.caret = 9;
  s.items = {{"你好", "ni hao", "1"}, {"妳好", "", "2"}, {"", "", ""}};
  s.page_no = 2;
  s.highlighted = 1;
  s.is_last_page = false;
  s.schema_id = "bopomofo_tw";
  s.schema_name = "注音";
  s.status_flags = kStComposing | kStSimplified;
  return s;
}

}  // namespace

TEST(protocol_result_roundtrip) {
  Result in;
  in.handled = true;
  in.snap = SampleSnapshot();

  const std::string p = EncodeResult(4242, in);
  uint32_t seq = 0;
  Result out;
  CHECK(DecodeResult(p, &seq, &out));
  CHECK_INT(seq, 4242);
  CHECK(out.handled);
  CHECK_STR(out.snap.commit_text, "你好");
  CHECK(out.snap.has_commit);
  CHECK_STR(out.snap.preedit, "ㄋㄧˇㄏㄠˇ");
  CHECK_INT(out.snap.sel_start, 3);
  CHECK_INT(out.snap.caret, 9);
  CHECK_INT(out.snap.items.size(), 3);
  CHECK_STR(out.snap.items[0].text, "你好");
  CHECK_STR(out.snap.items[0].comment, "ni hao");
  CHECK_STR(out.snap.items[1].comment, "");
  CHECK_STR(out.snap.items[2].label, "");
  CHECK_INT(out.snap.page_no, 2);
  CHECK_INT(out.snap.highlighted, 1);
  CHECK(!out.snap.is_last_page);
  CHECK_STR(out.snap.schema_id, "bopomofo_tw");
  CHECK_STR(out.snap.schema_name, "注音");
  CHECK_INT(out.snap.status_flags, kStComposing | kStSimplified);
}

TEST(protocol_all_message_roundtrips) {
  {
    Hello m;
    m.shell_abi = 1;
    m.host_pid = 1234;
    m.host_exe = "C:\\Windows\\notepad.exe";
    uint32_t seq = 0;
    Hello out;
    CHECK(DecodeHello(EncodeHello(1, m), &seq, &out));
    CHECK_INT(seq, 1);
    CHECK_INT(out.proto, kProtocolVersion);
    CHECK_INT(out.shell_abi, 1);
    CHECK_INT(out.host_pid, 1234);
    CHECK_STR(out.host_exe, "C:\\Windows\\notepad.exe");
  }
  {
    HelloOk m;
    m.shell_abi = 1;
    m.service_version = "0.2.0";
    uint32_t seq = 0;
    HelloOk out;
    CHECK(DecodeHelloOk(EncodeHelloOk(2, m), &seq, &out));
    CHECK_STR(out.service_version, "0.2.0");
  }
  {
    SessionOk m;
    m.session = 0x0123456789ABCDEFull;
    uint32_t seq = 0;
    SessionOk out;
    CHECK(DecodeSessionOk(EncodeSessionOk(3, m), &seq, &out));
    CHECK(out.session == 0x0123456789ABCDEFull);
  }
  {
    KeyReq m;
    m.session = 7;
    m.keysym = 0x01004F60;
    m.mods = 0x25;
    uint32_t seq = 0;
    KeyReq out;
    CHECK(DecodeKey(EncodeKey(4, m), &seq, &out));
    CHECK_INT(out.keysym, 0x01004F60);
    CHECK_INT(out.mods, 0x25);
  }
  {
    // 負數必須原樣往返:highlighted 的「沒有候選」就是 -1。
    ArgReq m;
    m.session = 9;
    m.arg = -1;
    uint32_t seq = 0;
    ArgReq out;
    CHECK(DecodeArg(EncodeArg(5, Op::kHighlight, m), &seq, &out));
    CHECK_INT(out.arg, -1);
  }
  {
    CaretRect m;
    m.session = 11;
    m.left = -100;
    m.top = 20;
    m.right = -80;
    m.bottom = 40;
    uint32_t seq = 0;
    CaretRect out;
    CHECK(DecodeCaretRect(EncodeCaretRect(6, m), &seq, &out));
    CHECK_INT(out.left, -100);   // 多螢幕時螢幕座標可以是負的
    CHECK_INT(out.right, -80);
  }
  {
    uint64_t sid = 0;
    uint32_t seq = 0;
    CHECK(DecodeSimple(EncodeSimple(7, Op::kCommitComposition, 33), &seq, &sid));
    CHECK_INT(sid, 33);
  }
  {
    ErrorMsg m;
    m.code = 7;
    m.text = "沒有這個 session";
    uint32_t seq = 0;
    ErrorMsg out;
    CHECK(DecodeError(EncodeError(8, m), &seq, &out));
    CHECK_STR(out.text, "沒有這個 session");
  }
}

TEST(protocol_decoder_rejects_wrong_opcode) {
  // 拿 HELLO 去餵 DecodeResult 必須失敗,而不是解出垃圾。
  Hello h;
  const std::string p = EncodeHello(1, h);
  uint32_t seq = 0;
  Result r;
  CHECK(!DecodeResult(p, &seq, &r));
  // DecodeArg / DecodeSimple 只接受自己那組 opcode。
  ArgReq a;
  CHECK(!DecodeArg(EncodeSimple(1, Op::kClear, 1), &seq, &a));
  uint64_t sid;
  ArgReq aa;
  aa.session = 1;
  CHECK(!DecodeSimple(EncodeArg(1, Op::kSelectCandidate, aa), &seq, &sid));
}

TEST(protocol_decoder_survives_truncation_at_every_offset) {
  // 每一個可能的截斷點都不可以崩、不可以讀過界。
  Result in;
  in.handled = true;
  in.snap = SampleSnapshot();
  const std::string full = EncodeResult(1, in);
  for (size_t n = 0; n < full.size(); ++n) {
    uint32_t seq = 0;
    Result out;
    // 只要求「不崩且回 false」。任何一個截斷版本被判定成功都是解碼器有洞。
    CHECK(!DecodeResult(full.substr(0, n), &seq, &out));
  }
  uint32_t seq = 0;
  Result out;
  CHECK(DecodeResult(full, &seq, &out));  // 完整的那份仍然要過
}

TEST(protocol_decoder_rejects_trailing_garbage) {
  // 尾巴多出來的位元組代表雙方對格式的理解不同。寧可整則丟掉。
  Result in;
  in.snap = SampleSnapshot();
  std::string p = EncodeResult(1, in);
  p.push_back('\0');
  uint32_t seq = 0;
  Result out;
  CHECK(!DecodeResult(p, &seq, &out));
}

TEST(protocol_decoder_rejects_absurd_string_length) {
  // 手工組一則 HELLO,把字串長度欄位填成 0xFFFFFFFF。
  std::string p;
  p.push_back((char)0x01);              // op = kHello
  for (int i = 0; i < 4; ++i) p.push_back(0);   // seq
  for (int i = 0; i < 4; ++i) p.push_back(1);   // proto
  for (int i = 0; i < 4; ++i) p.push_back(1);   // shell_abi
  for (int i = 0; i < 4; ++i) p.push_back(1);   // host_pid
  for (int i = 0; i < 4; ++i) p.push_back((char)0xFF);  // host_exe 長度
  uint32_t seq = 0;
  Hello out;
  // 這裡若沒有守住,結果是一次 4GB 的配置(或 length_error 例外),
  // 而在 DLL 裡例外等於宿主崩潰。
  CHECK(!DecodeHello(p, &seq, &out));
}

TEST(protocol_decoder_rejects_absurd_candidate_count) {
  std::string p;
  p.push_back((char)0x84);                     // op = kResult
  for (int i = 0; i < 4; ++i) p.push_back(0);  // seq
  p.push_back(1);                              // handled
  p.push_back(0);                              // has_commit
  for (int i = 0; i < 4; ++i) p.push_back(0);  // commit_text 長度 0
  for (int i = 0; i < 4; ++i) p.push_back(0);  // preedit 長度 0
  for (int i = 0; i < 12; ++i) p.push_back(0); // sel_start/sel_end/caret
  for (int i = 0; i < 4; ++i) p.push_back((char)0xFF);  // 候選數 = 0xFFFFFFFF
  uint32_t seq = 0;
  Result out;
  CHECK(!DecodeResult(p, &seq, &out));
}

TEST(frame_reader_reassembles_byte_by_byte) {
  // 具名管道可能給出半則訊息,所以分幀必須忍受任意切法。
  const std::string a = Frame("hello");
  const std::string b = Frame("");           // 空 payload 也是合法的一則
  const std::string c = Frame(std::string(300, 'x'));
  const std::string stream = a + b + c;

  FrameReader r;
  std::vector<std::string> got;
  for (size_t i = 0; i < stream.size(); ++i) {
    CHECK(r.Feed(stream.data() + i, 1));
    std::string msg;
    while (r.Next(&msg)) got.push_back(msg);
  }
  CHECK_INT(got.size(), 3);
  CHECK_STR(got[0], "hello");
  CHECK_STR(got[1], "");
  CHECK_INT(got[2].size(), 300);
  CHECK(!r.broken());
}

TEST(frame_reader_breaks_on_absurd_length_and_does_not_resync) {
  FrameReader r;
  const char bad[4] = {(char)0xFF, (char)0xFF, (char)0xFF, (char)0x7F};
  CHECK(r.Feed(bad, 4));
  std::string msg;
  CHECK(!r.Next(&msg));
  CHECK(r.broken());
  // 壞掉之後就是壞掉。刻意**不**嘗試往後找同步點:
  // 那只會拿垃圾當訊息解,而唯一安全的動作是關掉連線。
  const std::string good = Frame("ok");
  CHECK(!r.Feed(good.data(), good.size()));
  CHECK(!r.Next(&msg));
}

TEST(frame_reader_handles_max_size_boundary) {
  // 正常大小要收;長度欄位超過上限就拒絕並且不再重新同步。
  std::string ok_frame = Frame(std::string(64, 'z'));
  FrameReader r;
  CHECK(r.Feed(ok_frame.data(), ok_frame.size()));
  std::string msg;
  CHECK(r.Next(&msg));
  CHECK_INT(msg.size(), 64);

  FrameReader r2;
  const uint32_t too_big = kMaxFrameBytes + 1;
  char hdr[4];
  for (int i = 0; i < 4; ++i) hdr[i] = (char)((too_big >> (8 * i)) & 0xFF);
  CHECK(r2.Feed(hdr, 4));
  CHECK(!r2.Next(&msg));
  CHECK(r2.broken());
}
