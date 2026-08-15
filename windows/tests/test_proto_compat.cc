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
// kStKeyNotAnswered 那幾條要證明它**不會**影響那一橫的「這份快照能不能信」。
#include "../common/service_state.h"
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

TEST(Proto_v4_is_the_current_version_and_v1_is_still_accepted) {
  // v4 = 多了 kProfileState(#111:「別人的」必須是宿主說出來的)。
  // ⚠ 最舊接受版本**沒有跟著往上動**,而且不准動:使用者機器上每一個
  //   升級前就開著的行程都還載著舊的 rime_tsf.dll,把下限往上抬等於
  //   把它們全部踢掉,而 Explorer.exe 幾乎永遠不會重開。
  CHECK_INT(kProtocolVersion, 4);
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
  h.host_tid = 4343;            // 同上(v3 的尾巴)
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

// ── v3:tid 真的上線路,而且 v2 的解碼器整則丟掉 ────────────────
TEST(Proto_v3_hello_carries_the_activating_thread) {
  Hello h;
  h.proto = 3;
  h.shell_abi = 1;
  h.host_pid = 4242;
  h.host_exe = "C:\\a\\notepad.exe";
  h.input_langid = 0x0404;
  h.profile_guid = "{C6B736EB-38E3-4041-B59B-ECF91AD8E28A}";
  h.host_tid = 4343;
  uint32_t seq = 0;
  Hello back;
  const std::string wire = EncodeHello(5, h);
  CHECK(DecodeHello(wire, &seq, &back));
  CHECK_INT(back.proto, 3);
  // ⚠ 這一格就是那一橫的作用域。少了它,判準只剩 pid,而輸入法是
  //   per-thread 的 —— 同一支程式的另一個視窗上使用者用的是別的輸入法。
  CHECK_INT(back.host_tid, 4343);

  // 同一份內容宣告成 v2 就**不該**帶 tid(降級重試靠的是這件事)。
  Hello as_v2 = h;
  as_v2.proto = 2;
  const std::string v2 = EncodeHello(5, as_v2);
  CHECK(v2.size() + 4 == wire.size());
  Hello back2;
  CHECK(DecodeHello(v2, &seq, &back2));
  CHECK_INT(back2.host_tid, 0);  // 0 = 報不出來,不是「執行緒 0」
}

// 宣告 v3 卻少了尾巴 → 整則丟掉,不是半讀半猜。
TEST(Proto_truncated_v3_hello_is_rejected_not_half_read) {
  Hello h;
  h.proto = 3;
  h.shell_abi = 1;
  h.host_pid = 1;
  h.host_exe = "x";
  h.input_langid = 0x0404;
  h.host_tid = 9;
  const std::string full = EncodeHello(1, h);
  int seen = 0;
  for (size_t cut = 1; cut < full.size(); ++cut) {
    uint32_t seq = 0;
    Hello back;
    CHECK(!DecodeHello(full.substr(0, cut), &seq, &back));
    ++seen;
  }
  CHECK(seen > 20);
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


// ─────────────────────────────────────────────────────────────
// kStVariantKnown:「那一格不顯示」也要有線路表示
// ─────────────────────────────────────────────────────────────
//
// 簡繁那一格現在有三態(简 / 繁 / 整格不顯示),而線路上原本只有一個
// kStSimplified —— 一個位元表不出三態。加一個 bit 6 表示「這一格的
// 內容是可信的」:
//
//     kStVariantKnown 為假 → 整格不顯示(kStSimplified 這時沒有意義)
//     kStVariantKnown 為真 → 看 kStSimplified
//
// ⚠ 這必須是**純加法**:DLL 與服務可能來自不同的建置(那個 DLL 住在
//   瀏覽器裡,而瀏覽器可以開著好幾天)。加一個旗標位元不得改變任何
//   欄位的位置或長度,否則舊的那一端會從錯的偏移量讀起 —— 而那不是
//   「少一個功能」,是整則訊息解錯,後果由 link_state 兜成 fail-open,
//   使用者看到的是打不出中文。

namespace {

// **舊解碼器**只認得 bit 0..5。它不是「把 bit 6 讀成別的東西」,
// 是**根本不知道有 bit 6**,所以拿到的 flags 要先罩掉。
constexpr uint32_t kV1StatusMask = kStComposing | kStAsciiMode | kStFullShape |
                                   kStSimplified | kStAsciiPunct | kStDisabled;

Result SampleResult(uint32_t flags) {
  Result r;
  r.handled = true;
  r.snap.has_commit = true;
  r.snap.commit_text = "commit";
  r.snap.preedit = "nihao";
  r.snap.sel_start = 1;
  r.snap.sel_end = 2;
  r.snap.caret = 3;
  Candidate c1;
  c1.text = "cand-one";
  c1.comment = "cmt";
  c1.label = "1";
  Candidate c2;
  c2.text = "cand-two";
  r.snap.items.push_back(c1);
  r.snap.items.push_back(c2);
  r.snap.page_no = 2;
  r.snap.highlighted = 1;
  r.snap.is_last_page = false;
  r.snap.schema_id = "luna_pinyin_tw";
  r.snap.schema_name = "schema-name";
  r.snap.status_flags = flags;
  return r;
}

void CheckSnapshotBodyIntact(const Snapshot& got) {
  CHECK(got.has_commit);
  CHECK_STR(got.commit_text, "commit");
  CHECK_STR(got.preedit, "nihao");
  CHECK_INT(got.sel_start, 1);
  CHECK_INT(got.sel_end, 2);
  CHECK_INT(got.caret, 3);
  CHECK_INT(static_cast<int>(got.items.size()), 2);
  CHECK_STR(got.items[0].text, "cand-one");
  CHECK_STR(got.items[0].comment, "cmt");
  CHECK_STR(got.items[0].label, "1");
  CHECK_STR(got.items[1].text, "cand-two");
  CHECK_INT(got.page_no, 2);
  CHECK_INT(got.highlighted, 1);
  CHECK(!got.is_last_page);
  CHECK_STR(got.schema_id, "luna_pinyin_tw");
  CHECK_STR(got.schema_name, "schema-name");
}

}  // namespace

TEST(proto_variant_known_bit_does_not_collide) {
  // 新的位元必須是 bit 6,而且不得與任何既有的旗標重疊。
  CHECK_INT(static_cast<int>(kStVariantKnown), 1 << 6);
  CHECK_INT(static_cast<int>(kStVariantKnown & kV1StatusMask), 0);
  // 既有六個一個都不能動(改了值就等於改線路格式)。
  CHECK_INT(static_cast<int>(kStComposing), 1 << 0);
  CHECK_INT(static_cast<int>(kStAsciiMode), 1 << 1);
  CHECK_INT(static_cast<int>(kStFullShape), 1 << 2);
  CHECK_INT(static_cast<int>(kStSimplified), 1 << 3);
  CHECK_INT(static_cast<int>(kStAsciiPunct), 1 << 4);
  CHECK_INT(static_cast<int>(kStDisabled), 1 << 5);
}

TEST(proto_variant_known_bit_is_purely_additive) {
  const uint32_t base = kStComposing | kStAsciiMode | kStSimplified;
  const std::string without = EncodeResult(7, SampleResult(base));
  const std::string with =
      EncodeResult(7, SampleResult(base | kStVariantKnown));

  // ⚠ 這一條是核心:多一個旗標位元**不得改變訊息長度**,
  //   因為它住在一個既有的 u32 裡。長度變了就表示有人加了欄位,
  //   而那會讓舊的那一端從錯的偏移量讀起。
  CHECK_INT(static_cast<int>(with.size()), static_cast<int>(without.size()));

  // 而且兩則只差在那個 u32 的一個位元組上。
  int differing = 0;
  for (size_t i = 0; i < with.size(); ++i)
    if (with[i] != without[i]) ++differing;
  CHECK_INT(differing, 1);
}

TEST(proto_old_decoder_survives_the_variant_known_bit) {
  const uint32_t flags =
      kStComposing | kStAsciiMode | kStSimplified | kStVariantKnown;
  const std::string wire = EncodeResult(11, SampleResult(flags));

  uint32_t seq = 0;
  Result got;
  CHECK(DecodeResult(wire, &seq, &got));
  CHECK_INT(static_cast<int>(seq), 11);
  CHECK(got.handled);
  CHECK_INT(static_cast<int>(got.snap.status_flags), static_cast<int>(flags));
  CheckSnapshotBodyIntact(got.snap);

  // **舊那一端**:它不知道有 bit 6,所以把 flags 罩掉。
  // 罩掉之後其餘欄位必須一個字都沒變 —— 那就是「純加法」的意思。
  const uint32_t as_old = got.snap.status_flags & kV1StatusMask;
  CHECK_INT(static_cast<int>(as_old), static_cast<int>(kStComposing | kStAsciiMode | kStSimplified));
  CheckSnapshotBodyIntact(got.snap);
}

TEST(proto_variant_unknown_still_round_trips) {
  // 「整格不顯示」= kStVariantKnown 為假。這一態也要過得了線路,
  // 而且不得被誤解成「繁體」。
  const std::string wire = EncodeResult(3, SampleResult(kStComposing));
  uint32_t seq = 0;
  Result got;
  CHECK(DecodeResult(wire, &seq, &got));
  CHECK_INT(static_cast<int>(got.snap.status_flags & kStVariantKnown), 0);
  CHECK_INT(static_cast<int>(got.snap.status_flags & kStSimplified), 0);
  CheckSnapshotBodyIntact(got.snap);

  // 三種組合都要能來回,而且互相分得開。
  const uint32_t kCases[3] = {0u, kStVariantKnown,
                              kStVariantKnown | kStSimplified};
  int seen = 0;
  for (uint32_t f : kCases) {
    uint32_t s2 = 0;
    Result r2;
    CHECK(DecodeResult(EncodeResult(1, SampleResult(f)), &s2, &r2));
    CHECK_INT(static_cast<int>(r2.snap.status_flags), static_cast<int>(f));
    ++seen;
  }
  CHECK_INT(seen, 3);
}

// ══ v4:升版**不准**把還抱著舊 DLL 的宿主擋在門外 ═══════════════
//
// 這件事不是推論出來的,是查過 pipe_server.cc 的 HELLO 分支之後寫下來的:
// 那道關卡是 `h.proto < kMinProtocolVersion || h.proto > kProtocolVersion`,
// 而它回的是**協商出來的** `ok.proto = h.proto`。所以舊 DLL 宣告 2 或 3
// 都落在區間內 → 接受 → 用戶端那一格 `ok.proto != proto_` 也對得上。
//
// ⚠ 這一支存在的理由:使用者機器上**每一個升級前就開著的行程**都還載著
//   舊的 rime_tsf.dll,而 Explorer.exe 幾乎永遠不會重開。如果升版會把它們
//   擋掉,那升版本身就是一個比 #111 更大的迴歸。
TEST(Proto_v4_bump_still_accepts_the_dlls_that_never_got_restarted) {
  // 服務端那道關卡的兩個邊,逐字照抄自 pipe_server.cc。
  for (uint32_t p = kMinProtocolVersion; p <= kProtocolVersion; ++p) {
    const bool refused = (p < kMinProtocolVersion || p > kProtocolVersion);
    CHECK(!refused);
  }
  // 使用者機器上真的存在的那兩種舊 DLL。
  CHECK(2 >= kMinProtocolVersion && 2 <= kProtocolVersion);
  CHECK(3 >= kMinProtocolVersion && 3 <= kProtocolVersion);
  // ⚠ v4 是**純加法**:它一個 Hello 欄位都沒加,所以 v3 的位元組佈局
  //   一個位元都沒動 —— 舊服務解得完一則 v4 的 HELLO(它只會在版本
  //   區間那一關拒絕,而那條路徑是 PresenceLink 早就在處理的降版重試)。
  Hello v3;
  v3.proto = 3;
  v3.shell_abi = 1;
  v3.host_pid = 4242;
  v3.host_exe = "C:\\Windows\\Explorer.EXE";
  v3.input_langid = 0x0404;
  v3.profile_guid = "{C6B736EB-38E3-4041-B59B-ECF91AD8E28A}";
  v3.host_tid = 4343;
  Hello v4 = v3;
  v4.proto = 4;
  const std::string w3 = EncodeHello(7, v3);
  const std::string w4 = EncodeHello(7, v4);
  CHECK_INT(w3.size(), w4.size());  // 沒有新欄位 = 沒有新位元組

  // 舊 DLL 的 HELLO 在**這一版**的解碼器底下照樣完整:host_tid 還在,
  // 也就是那一橫的 per-thread 精確度對它們一點都沒少。
  uint32_t seq = 0;
  Hello back;
  CHECK(DecodeHello(w3, &seq, &back));
  CHECK_INT(back.proto, 3);
  CHECK_INT(back.host_tid, 4343);
  CHECK_STR(back.host_exe, v3.host_exe);
}

// ── kProfileState:走既有的 EncodeSimple 形狀,而且不撞任何一個 op ──
TEST(Proto_profile_state_rides_the_existing_simple_shape) {
  // 0 = 這條執行緒上啟用中的**不再是**我們。
  const std::string away = EncodeSimple(9, Op::kProfileState, 0);
  uint32_t seq = 0;
  uint64_t v = 123;
  CHECK(DecodeSimple(away, &seq, &v));
  CHECK_INT(seq, 9);
  CHECK_INT(v, 0);
  // 1 = 又是我們了。
  const std::string back = EncodeSimple(10, Op::kProfileState, 1);
  CHECK(DecodeSimple(back, &seq, &v));
  CHECK_INT(seq, 10);
  CHECK_INT(v, 1);

  // ⚠ op 不可以跟既有的任何一個撞:撞了的話一則「讓位」會被當成
  //   「開設定」,使用者切走輸入法時設定視窗會自己跳出來。
  CHECK(static_cast<uint8_t>(Op::kProfileState) !=
        static_cast<uint8_t>(Op::kOpenSettings));
  CHECK_INT(static_cast<uint8_t>(Op::kProfileState), 0x0F);

  // 截斷 / 多餘位元組一律整則丟掉,不是半讀半猜。
  int seen = 0;
  for (size_t cut = 1; cut < away.size(); ++cut) {
    CHECK(!DecodeSimple(away.substr(0, cut), &seq, &v));
    ++seen;
  }
  CHECK(seen > 8);
  CHECK(!DecodeSimple(away + std::string(1, '\0'), &seq, &v));
}

// ─────────────────────────────────────────────────────────────
// kStKeyNotAnswered:「引擎沒有回答」也要有線路表示
// ─────────────────────────────────────────────────────────────
//
// 使用者升級之後打「你好」拿到「ni好」。成因是線路上 handled=false 蓋著
// 兩件事(見 common/protocol.h 的 kStKeyNotAnswered),而 DLL 對其中一件
// 的正確反應是把原始字母補進文件、對另一件是絕對不可以。
//
// ⚠ 這一格與 kStVariantKnown 是**同一種加法**:一個既有 u32 裡的位元,
//   位元組佈局一個位元都不變。key_deadline.h:201-212 當初否決的是
//   「在 Result 尾巴加欄位」(舊 DLL 的 DecodeResult 要求剛好用完,會整則
//   丟掉)—— 那個理由對加欄位成立,對加位元不成立,而下面三條就是證明。

TEST(proto_key_not_answered_bit_does_not_collide) {
  CHECK_INT(static_cast<int>(kStKeyNotAnswered), 1 << 7);
  CHECK_INT(static_cast<int>(kStKeyNotAnswered & kV1StatusMask), 0);
  // 與 bit 6 也不可以撞。
  CHECK_INT(static_cast<int>(kStKeyNotAnswered & kStVariantKnown), 0);
}

TEST(proto_key_not_answered_bit_is_purely_additive) {
  const uint32_t base = kStDisabled;
  const std::string without = EncodeResult(9, SampleResult(base));
  const std::string with =
      EncodeResult(9, SampleResult(base | kStKeyNotAnswered));
  // 長度不變 —— 它住在既有的那個 u32 裡。長度變了 = 有人加了欄位,
  // 而那會讓舊 DLL 的 DecodeResult 在「剛好用完」那一關整則丟掉。
  CHECK_INT(static_cast<int>(with.size()), static_cast<int>(without.size()));
  int differing = 0;
  for (size_t i = 0; i < with.size(); ++i)
    if (with[i] != without[i]) ++differing;
  CHECK_INT(differing, 1);
}

TEST(proto_old_decoder_survives_the_key_not_answered_bit) {
  // **新服務 → 舊 DLL**:舊 DLL 解得完,只是看不見那個位元 ——
  // 它的行為與這一輪之前完全一樣(照舊補字元)。這正是「不會讓任何
  // 既有組合變得更差」的那句話的證據。
  const uint32_t flags = kStDisabled | kStKeyNotAnswered;
  Result src = SampleResult(flags);
  src.handled = false;  // 沒回答的那一份,handled 一定是 false。
  const std::string wire = EncodeResult(13, src);

  uint32_t seq = 0;
  Result got;
  CHECK(DecodeResult(wire, &seq, &got));
  CHECK_INT(static_cast<int>(seq), 13);
  CHECK(!got.handled);
  CHECK_INT(static_cast<int>(got.snap.status_flags), static_cast<int>(flags));
  CheckSnapshotBodyIntact(got.snap);

  const uint32_t as_old = got.snap.status_flags & kV1StatusMask;
  CHECK_INT(static_cast<int>(as_old), static_cast<int>(kStDisabled));
  CheckSnapshotBodyIntact(got.snap);
}

TEST(proto_key_not_answered_absent_means_unknown_not_answered) {
  // ⚠ 極性。**舊服務 → 新 DLL**:舊服務永遠不送這個位元,而它送來的
  //   每一份大多是真的回答過的。所以「沒有這個位元」只能讀成
  //   「照舊處理」,不可以讀成「確定沒回答」——
  //   讀反了的話,舊服務配新 DLL 會變成每一顆字母鍵都沒有反應。
  const std::string wire = EncodeResult(4, SampleResult(kStComposing));
  uint32_t seq = 0;
  Result got;
  CHECK(DecodeResult(wire, &seq, &got));
  CHECK_INT(static_cast<int>(got.snap.status_flags & kStKeyNotAnswered), 0);

  // 兩態都要來回得了,而且分得開。
  const uint32_t kCases[2] = {kStDisabled, kStDisabled | kStKeyNotAnswered};
  for (uint32_t f : kCases) {
    uint32_t s2 = 0;
    Result r2;
    CHECK(DecodeResult(EncodeResult(1, SampleResult(f)), &s2, &r2));
    CHECK_INT(static_cast<int>(r2.snap.status_flags), static_cast<int>(f));
  }
}

// ⚠ 這個位元**不可以**影響那一橫的「這份快照能不能信」判斷 ——
//   它是給 DLL 用的,而狀態列那一格看的是 kStDisabled。
//   兩件事混在一起的話,一個健康引擎的逾時會讓那一橫整排消失。
TEST(proto_key_not_answered_does_not_change_snapshot_usability) {
  CHECK(SnapshotFlagsAreUsable(kStKeyNotAnswered));
  CHECK(SnapshotFlagsAreUsable(kStComposing | kStKeyNotAnswered));
  // 帶著 kStDisabled 的仍然不可用(那一格的判準沒有被動到)。
  CHECK(!SnapshotFlagsAreUsable(kStDisabled | kStKeyNotAnswered));
}
