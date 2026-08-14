// windows/common/protocol.cc — 線路格式的編解碼(純邏輯,不含任何平台 API)

#include "protocol.h"

#include <cstring>

namespace rimewin {
namespace {

// ── 寫 ──────────────────────────────────────────────────────────
// 一律小端。不用 memcpy 整個結構體,不受編譯器對齊與位元組序影響。
void PutU8(std::string* s, uint8_t v) { s->push_back(static_cast<char>(v)); }

void PutU32(std::string* s, uint32_t v) {
  for (int i = 0; i < 4; ++i) s->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void PutU64(std::string* s, uint64_t v) {
  for (int i = 0; i < 8; ++i) s->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void PutI32(std::string* s, int32_t v) { PutU32(s, static_cast<uint32_t>(v)); }

void PutStr(std::string* s, const std::string& v) {
  PutU32(s, static_cast<uint32_t>(v.size()));
  s->append(v);
}

void PutBool(std::string* s, bool v) { PutU8(s, v ? 1 : 0); }

std::string Head(Op op, uint32_t seq) {
  std::string s;
  s.reserve(32);
  PutU8(&s, static_cast<uint8_t>(op));
  PutU32(&s, seq);
  return s;
}

// ── 讀 ──────────────────────────────────────────────────────────
// 每一個取值都先檢查剩餘長度。位元組來自另一個進程,不可信。
class Cursor {
 public:
  Cursor(const std::string& s) : p_(s.data()), end_(s.data() + s.size()) {}

  bool U8(uint8_t* out) {
    if (Left() < 1) return Fail();
    *out = static_cast<uint8_t>(*p_++);
    return true;
  }
  bool U32(uint32_t* out) {
    if (Left() < 4) return Fail();
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
      v |= static_cast<uint32_t>(static_cast<unsigned char>(p_[i])) << (8 * i);
    p_ += 4;
    *out = v;
    return true;
  }
  bool U64(uint64_t* out) {
    if (Left() < 8) return Fail();
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
      v |= static_cast<uint64_t>(static_cast<unsigned char>(p_[i])) << (8 * i);
    p_ += 8;
    *out = v;
    return true;
  }
  bool I32(int32_t* out) {
    uint32_t v = 0;
    if (!U32(&v)) return false;
    *out = static_cast<int32_t>(v);
    return true;
  }
  bool Bool(bool* out) {
    uint8_t v = 0;
    if (!U8(&v)) return false;
    *out = (v != 0);
    return true;
  }
  bool Str(std::string* out) {
    uint32_t n = 0;
    if (!U32(&n)) return false;
    // n 是對面給的。少了這個檢查,一個壞掉的 4GB 長度會變成一次巨大的配置
    // (或是 length_error 例外,而在 DLL 裡那等於宿主崩潰)。
    if (n > kMaxFrameBytes || Left() < static_cast<size_t>(n)) return Fail();
    out->assign(p_, n);
    p_ += n;
    return true;
  }
  // 解碼結束時要求「剛好用完」。多出來的位元組代表雙方對格式的理解不同,
  // 那種情形寧可整則丟掉,也不要拿前半段當成有效訊息用下去。
  bool AtEnd() const { return ok_ && p_ == end_; }
  bool ok() const { return ok_; }

 private:
  size_t Left() const { return static_cast<size_t>(end_ - p_); }
  bool Fail() {
    ok_ = false;
    p_ = end_;
    return false;
  }
  const char* p_;
  const char* end_;
  bool ok_ = true;
};

bool ReadHead(Cursor* c, Op want, uint32_t* seq) {
  uint8_t op = 0;
  if (!c->U8(&op)) return false;
  if (static_cast<Op>(op) != want) return false;
  return c->U32(seq);
}

void PutSnapshot(std::string* s, const Snapshot& v) {
  PutBool(s, v.has_commit);
  PutStr(s, v.commit_text);
  PutStr(s, v.preedit);
  PutI32(s, v.sel_start);
  PutI32(s, v.sel_end);
  PutI32(s, v.caret);
  PutU32(s, static_cast<uint32_t>(v.items.size()));
  for (const Candidate& c : v.items) {
    PutStr(s, c.text);
    PutStr(s, c.comment);
    PutStr(s, c.label);
  }
  PutI32(s, v.page_no);
  PutI32(s, v.highlighted);
  PutBool(s, v.is_last_page);
  PutStr(s, v.schema_id);
  PutStr(s, v.schema_name);
  PutU32(s, v.status_flags);
}

bool GetSnapshot(Cursor* c, Snapshot* v) {
  uint32_t n = 0;
  if (!c->Bool(&v->has_commit)) return false;
  if (!c->Str(&v->commit_text)) return false;
  if (!c->Str(&v->preedit)) return false;
  if (!c->I32(&v->sel_start)) return false;
  if (!c->I32(&v->sel_end)) return false;
  if (!c->I32(&v->caret)) return false;
  if (!c->U32(&n)) return false;
  // 候選數的上限:一頁最多幾十個。這個檢查與 Str 裡那個是同一個道理 ——
  // 先擋住 reserve(4e9),而不是等著它丟 bad_alloc。
  if (n > 4096) return false;
  v->items.clear();
  v->items.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    if (!c->Str(&v->items[i].text)) return false;
    if (!c->Str(&v->items[i].comment)) return false;
    if (!c->Str(&v->items[i].label)) return false;
  }
  if (!c->I32(&v->page_no)) return false;
  if (!c->I32(&v->highlighted)) return false;
  if (!c->Bool(&v->is_last_page)) return false;
  if (!c->Str(&v->schema_id)) return false;
  if (!c->Str(&v->schema_name)) return false;
  if (!c->U32(&v->status_flags)) return false;
  return true;
}

}  // namespace

// ─────────────────────────── 編碼 ───────────────────────────

std::string EncodeHello(uint32_t seq, const Hello& m) {
  std::string s = Head(Op::kHello, seq);
  PutU32(&s, m.proto);
  PutU32(&s, m.shell_abi);
  PutU32(&s, m.host_pid);
  PutStr(&s, m.host_exe);
  // ⚠ v1 的位元組佈局到上面為止,**一個位元都不可以動**。
  //   新欄位只在 m.proto >= 2 時才寫 —— 降級重試(ipc_client.cc)靠的
  //   就是「同一支程式送得出 v1 訊息」這件事。
  if (m.proto >= 2) {
    PutU32(&s, m.input_langid);
    PutStr(&s, m.profile_guid);
  }
  // ⚠ 同一條規矩再套一次:v2 的位元組佈局到上面為止,一個位元都不動。
  if (m.proto >= 3) PutU32(&s, m.host_tid);
  return s;
}

std::string EncodeHelloOk(uint32_t seq, const HelloOk& m) {
  std::string s = Head(Op::kHelloOk, seq);
  PutU32(&s, m.proto);
  PutU32(&s, m.shell_abi);
  PutStr(&s, m.service_version);
  return s;
}

std::string EncodeSessionNew(uint32_t seq) { return Head(Op::kSessionNew, seq); }

std::string EncodeSessionOk(uint32_t seq, const SessionOk& m) {
  std::string s = Head(Op::kSessionOk, seq);
  PutU64(&s, m.session);
  return s;
}

std::string EncodeSessionEnd(uint32_t seq, uint64_t session) {
  return EncodeSimple(seq, Op::kSessionEnd, session);
}

std::string EncodeSimple(uint32_t seq, Op op, uint64_t session) {
  std::string s = Head(op, seq);
  PutU64(&s, session);
  return s;
}

std::string EncodeKey(uint32_t seq, const KeyReq& m) {
  std::string s = Head(Op::kKey, seq);
  PutU64(&s, m.session);
  PutI32(&s, m.keysym);
  PutU32(&s, m.mods);
  return s;
}

std::string EncodeArg(uint32_t seq, Op op, const ArgReq& m) {
  std::string s = Head(op, seq);
  PutU64(&s, m.session);
  PutI32(&s, m.arg);
  return s;
}

std::string EncodeCaretRect(uint32_t seq, const CaretRect& m) {
  std::string s = Head(Op::kCaretRect, seq);
  PutU64(&s, m.session);
  PutI32(&s, m.left);
  PutI32(&s, m.top);
  PutI32(&s, m.right);
  PutI32(&s, m.bottom);
  return s;
}

std::string EncodeSelectSchema(uint32_t seq, const SchemaReq& m) {
  std::string s = Head(Op::kSelectSchema, seq);
  PutU64(&s, m.session);
  PutStr(&s, m.schema_id);
  return s;
}

std::string EncodePing(uint32_t seq) { return Head(Op::kPing, seq); }
std::string EncodePong(uint32_t seq) { return Head(Op::kPong, seq); }

std::string EncodeResult(uint32_t seq, const Result& m) {
  std::string s = Head(Op::kResult, seq);
  PutBool(&s, m.handled);
  PutSnapshot(&s, m.snap);
  return s;
}

std::string EncodeError(uint32_t seq, const ErrorMsg& m) {
  std::string s = Head(Op::kError, seq);
  PutU32(&s, m.code);
  PutStr(&s, m.text);
  return s;
}

// ─────────────────────────── 解碼 ───────────────────────────

bool PeekHeader(const std::string& payload, Op* op, uint32_t* seq) {
  Cursor c(payload);
  uint8_t o = 0;
  if (!c.U8(&o)) return false;
  if (!c.U32(seq)) return false;
  *op = static_cast<Op>(o);
  return true;
}

bool DecodeHello(const std::string& p, uint32_t* seq, Hello* out) {
  Cursor c(p);
  if (!ReadHead(&c, Op::kHello, seq)) return false;
  if (!c.U32(&out->proto)) return false;
  if (!c.U32(&out->shell_abi)) return false;
  if (!c.U32(&out->host_pid)) return false;
  if (!c.Str(&out->host_exe)) return false;
  // 格式是自描述的:讀不讀尾巴由訊息自己宣告的版本決定,
  // 不是由「我這一版編譯時知道什麼」決定。
  //
  // ⚠ 這裡不可以寫成「有剩就讀」。那樣的話一則被截斷或被塞了垃圾的
  //   v1 訊息會被當成 v2 解,而下面的 AtEnd() 就再也擋不住它。
  if (out->proto >= 2) {
    if (!c.U32(&out->input_langid)) return false;
    if (!c.Str(&out->profile_guid)) return false;
  } else {
    out->input_langid = 0;
    out->profile_guid.clear();
  }
  // 同上:讀不讀由訊息自己宣告的版本決定,不是「有剩就讀」。
  if (out->proto >= 3) {
    if (!c.U32(&out->host_tid)) return false;
  } else {
    out->host_tid = 0;
  }
  return c.AtEnd();
}

bool DecodeHelloOk(const std::string& p, uint32_t* seq, HelloOk* out) {
  Cursor c(p);
  if (!ReadHead(&c, Op::kHelloOk, seq)) return false;
  if (!c.U32(&out->proto)) return false;
  if (!c.U32(&out->shell_abi)) return false;
  if (!c.Str(&out->service_version)) return false;
  return c.AtEnd();
}

bool DecodeSessionOk(const std::string& p, uint32_t* seq, SessionOk* out) {
  Cursor c(p);
  if (!ReadHead(&c, Op::kSessionOk, seq)) return false;
  if (!c.U64(&out->session)) return false;
  return c.AtEnd();
}

bool DecodeKey(const std::string& p, uint32_t* seq, KeyReq* out) {
  Cursor c(p);
  if (!ReadHead(&c, Op::kKey, seq)) return false;
  if (!c.U64(&out->session)) return false;
  if (!c.I32(&out->keysym)) return false;
  if (!c.U32(&out->mods)) return false;
  return c.AtEnd();
}

bool DecodeArg(const std::string& p, uint32_t* seq, ArgReq* out) {
  Cursor c(p);
  uint8_t op = 0;
  if (!c.U8(&op)) return false;
  switch (static_cast<Op>(op)) {
    case Op::kSelectCandidate:
    case Op::kHighlight:
    case Op::kChangePage:
    case Op::kFocus:
      break;
    default:
      return false;
  }
  if (!c.U32(seq)) return false;
  if (!c.U64(&out->session)) return false;
  if (!c.I32(&out->arg)) return false;
  return c.AtEnd();
}

bool DecodeCaretRect(const std::string& p, uint32_t* seq, CaretRect* out) {
  Cursor c(p);
  if (!ReadHead(&c, Op::kCaretRect, seq)) return false;
  if (!c.U64(&out->session)) return false;
  if (!c.I32(&out->left)) return false;
  if (!c.I32(&out->top)) return false;
  if (!c.I32(&out->right)) return false;
  if (!c.I32(&out->bottom)) return false;
  return c.AtEnd();
}

bool DecodeSelectSchema(const std::string& p, uint32_t* seq, SchemaReq* out) {
  Cursor c(p);
  if (!ReadHead(&c, Op::kSelectSchema, seq)) return false;
  if (!c.U64(&out->session)) return false;
  if (!c.Str(&out->schema_id)) return false;
  return c.AtEnd();
}

bool DecodeSimple(const std::string& p, uint32_t* seq, uint64_t* session) {
  Cursor c(p);
  uint8_t op = 0;
  if (!c.U8(&op)) return false;
  switch (static_cast<Op>(op)) {
    case Op::kSessionEnd:
    case Op::kCommitComposition:
    case Op::kClear:
    case Op::kOpenSettings:  // v2 起。單向,沒有回覆。
      break;
    default:
      return false;
  }
  if (!c.U32(seq)) return false;
  if (!c.U64(session)) return false;
  return c.AtEnd();
}

bool DecodeResult(const std::string& p, uint32_t* seq, Result* out) {
  Cursor c(p);
  if (!ReadHead(&c, Op::kResult, seq)) return false;
  if (!c.Bool(&out->handled)) return false;
  if (!GetSnapshot(&c, &out->snap)) return false;
  return c.AtEnd();
}

bool DecodeError(const std::string& p, uint32_t* seq, ErrorMsg* out) {
  Cursor c(p);
  if (!ReadHead(&c, Op::kError, seq)) return false;
  if (!c.U32(&out->code)) return false;
  if (!c.Str(&out->text)) return false;
  return c.AtEnd();
}

// ─────────────────────────── 分幀 ───────────────────────────

std::string Frame(const std::string& payload) {
  std::string s;
  s.reserve(payload.size() + 4);
  PutU32(&s, static_cast<uint32_t>(payload.size()));
  s.append(payload);
  return s;
}

bool FrameReader::Feed(const char* data, size_t len) {
  if (broken_) return false;
  buf_.append(data, len);
  // 已消費的前綴定期回收,免得長連線上 buf_ 無限成長。
  if (consumed_ > (1u << 16) && consumed_ * 2 > buf_.size()) {
    buf_.erase(0, consumed_);
    consumed_ = 0;
  }
  return true;
}

bool FrameReader::Next(std::string* out) {
  if (broken_) return false;
  if (buf_.size() - consumed_ < 4) return false;
  const unsigned char* p =
      reinterpret_cast<const unsigned char*>(buf_.data()) + consumed_;
  uint32_t len = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                 (static_cast<uint32_t>(p[2]) << 16) |
                 (static_cast<uint32_t>(p[3]) << 24);
  if (len > kMaxFrameBytes) {
    // 不嘗試重新同步。串流一旦錯位,任何「往後找找看」的補救都只是拿垃圾
    // 當訊息解;唯一安全的動作是讓呼叫端關掉連線重來。
    broken_ = true;
    return false;
  }
  if (buf_.size() - consumed_ < 4 + static_cast<size_t>(len)) return false;
  out->assign(buf_, consumed_ + 4, len);
  consumed_ += 4 + len;
  return true;
}

}  // namespace rimewin
