#include "zip_reader.h"

#include <cstring>

namespace rimewin {
namespace {

/* ══════════════════════════════════════════════════════════════════
 * DEFLATE(RFC 1951)
 *
 * 寫法照 Mark Adler 的 puff 那一套「canonical Huffman = 每個碼長的
 * 個數 + 依序排好的符號」——它不建查表,所以沒有查表大小算錯這種
 * 一眼看不出來的溢位;每一次取位元、每一次寫輸出都當場檢查邊界。
 *
 * ⚠ 這是這條線上唯一直接吃網路位元組的解析器。三件事是刻意的:
 *   · 輸入用不到就是不讀 —— 任何一次讀取都先檢查 incnt < inlen;
 *   · 輸出有硬上限 max_out,到頂立刻停(宣告的大小是對方給的);
 *   · 回溯距離要檢查不得超過**目前已輸出的長度**,
 *     否則 `dist` 很大的一段壓縮資料會從緩衝區前面讀出去。
 * ════════════════════════════════════════════════════════════════ */

constexpr int kMaxBits = 15;
constexpr int kMaxLCodes = 286;
constexpr int kMaxDCodes = 30;
constexpr int kMaxCodes = kMaxLCodes + kMaxDCodes;
constexpr int kFixLCodes = 288;

struct Huffman {
  short count[kMaxBits + 1];
  short symbol[kMaxCodes];
};

class Inflater {
 public:
  Inflater(const uint8_t* src, size_t n, int64_t max_out, std::string* out)
      : in_(src), inlen_(n), max_out_(max_out), out_(out) {}

  InflateResult Run() {
    InflateResult r;
    int last = 0;
    do {
      if (!Bits(1, &last)) return Err(r);
      int type = 0;
      if (!Bits(2, &type)) return Err(r);
      bool ok = false;
      if (type == 0) ok = Stored();
      else if (type == 1) ok = Fixed();
      else if (type == 2) ok = Dynamic();
      else err_ = "invalid deflate block type 3";
      if (!ok) return Err(r);
    } while (!last);
    r.ok = true;
    return r;
  }

 private:
  InflateResult Err(InflateResult r) {
    r.ok = false;
    r.over_limit = over_limit_;
    r.error = err_.empty() ? "malformed deflate stream" : err_;
    return r;
  }

  bool Bits(int need, int* out) {
    int32_t val = bitbuf_;
    while (bitcnt_ < need) {
      if (incnt_ >= inlen_) {
        err_ = "deflate stream ended in the middle of a block";
        return false;
      }
      val |= static_cast<int32_t>(in_[incnt_++]) << bitcnt_;
      bitcnt_ += 8;
    }
    bitbuf_ = static_cast<int32_t>(static_cast<uint32_t>(val) >> need);
    bitcnt_ -= need;
    *out = static_cast<int>(val & ((1L << need) - 1));
    return true;
  }

  bool Emit(char c) {
    if (static_cast<int64_t>(out_->size()) >= max_out_) {
      over_limit_ = true;
      err_ = "the entry inflates past the size limit (the declared size lied)";
      return false;
    }
    out_->push_back(c);
    return true;
  }

  bool Stored() {
    bitbuf_ = 0;
    bitcnt_ = 0;
    if (incnt_ + 4 > inlen_) {
      err_ = "stored block header is truncated";
      return false;
    }
    const unsigned len = static_cast<unsigned>(in_[incnt_]) |
                         (static_cast<unsigned>(in_[incnt_ + 1]) << 8);
    const unsigned nlen = static_cast<unsigned>(in_[incnt_ + 2]) |
                          (static_cast<unsigned>(in_[incnt_ + 3]) << 8);
    incnt_ += 4;
    if ((len ^ 0xFFFFu) != nlen) {
      err_ = "stored block length and its complement disagree";
      return false;
    }
    if (incnt_ + len > inlen_) {
      err_ = "stored block runs past the end of the input";
      return false;
    }
    if (static_cast<int64_t>(out_->size()) + static_cast<int64_t>(len) > max_out_) {
      over_limit_ = true;
      err_ = "the entry inflates past the size limit (the declared size lied)";
      return false;
    }
    out_->append(reinterpret_cast<const char*>(in_ + incnt_), len);
    incnt_ += len;
    return true;
  }

  bool Decode(const Huffman& h, int* sym) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kMaxBits; ++len) {
      int b = 0;
      if (!Bits(1, &b)) return false;
      code |= b;
      const int count = h.count[len];
      if (code - count < first) {
        *sym = h.symbol[index + (code - first)];
        return true;
      }
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
    }
    err_ = "ran out of Huffman codes";
    return false;
  }

  // 回傳 false = 這組碼長不合法。
  static bool Construct(Huffman* h, const short* length, int n) {
    for (int len = 0; len <= kMaxBits; ++len) h->count[len] = 0;
    for (int s = 0; s < n; ++s) h->count[length[s]]++;
    if (h->count[0] == n) return true;  // 全部長度 0:空的碼表
    // 檢查是否 over-subscribed。
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
      left <<= 1;
      left -= h->count[len];
      if (left < 0) return false;
    }
    short offs[kMaxBits + 1];
    offs[1] = 0;
    for (int len = 1; len < kMaxBits; ++len) {
      offs[len + 1] = static_cast<short>(offs[len] + h->count[len]);
    }
    for (int s = 0; s < n; ++s) {
      if (length[s] != 0) h->symbol[offs[length[s]]++] = static_cast<short>(s);
    }
    return true;
  }

  bool Codes(const Huffman& lencode, const Huffman& distcode) {
    static const short kLens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19,
                                    23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
                                    131, 163, 195, 227, 258};
    static const short kLext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short kDists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49,
                                     65, 97, 129, 193, 257, 385, 513, 769, 1025,
                                     1537, 2049, 3073, 4097, 6145, 8193, 12289,
                                     16385, 24577};
    static const short kDext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12,
                                    13, 13};
    while (true) {
      int sym = 0;
      if (!Decode(lencode, &sym)) return false;
      if (sym < 0) return false;
      if (sym < 256) {
        if (!Emit(static_cast<char>(sym))) return false;
        continue;
      }
      if (sym == 256) return true;  // 區塊結束
      sym -= 257;
      if (sym >= 29) {
        err_ = "invalid length symbol";
        return false;
      }
      int extra = 0;
      if (!Bits(kLext[sym], &extra)) return false;
      const int len = kLens[sym] + extra;

      int dsym = 0;
      if (!Decode(distcode, &dsym)) return false;
      if (dsym < 0 || dsym >= 30) {
        err_ = "invalid distance symbol";
        return false;
      }
      if (!Bits(kDext[dsym], &extra)) return false;
      const int dist = kDists[dsym] + extra;
      if (static_cast<int64_t>(dist) > static_cast<int64_t>(out_->size())) {
        // ⚠ 這一條就是「從緩衝區前面讀出去」的那道牆。
        err_ = "back-reference points before the start of the output";
        return false;
      }
      size_t from = out_->size() - static_cast<size_t>(dist);
      for (int k = 0; k < len; ++k) {
        // 逐位元組複製:重疊(dist < len)在 DEFLATE 裡是合法而且常見的。
        const char c = (*out_)[from++];
        if (!Emit(c)) return false;
      }
    }
  }

  bool Fixed() {
    static Huffman lencode, distcode;
    static bool built = false;
    if (!built) {
      short lengths[kFixLCodes];
      int s = 0;
      for (; s < 144; ++s) lengths[s] = 8;
      for (; s < 256; ++s) lengths[s] = 9;
      for (; s < 280; ++s) lengths[s] = 7;
      for (; s < kFixLCodes; ++s) lengths[s] = 8;
      Construct(&lencode, lengths, kFixLCodes);
      for (s = 0; s < kMaxDCodes; ++s) lengths[s] = 5;
      Construct(&distcode, lengths, kMaxDCodes);
      built = true;
    }
    return Codes(lencode, distcode);
  }

  bool Dynamic() {
    static const short kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                     11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = 0, ndist = 0, ncode = 0;
    if (!Bits(5, &nlen)) return false;
    if (!Bits(5, &ndist)) return false;
    if (!Bits(4, &ncode)) return false;
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > kMaxLCodes || ndist > kMaxDCodes) {
      err_ = "too many length or distance codes";
      return false;
    }

    short lengths[kMaxCodes];
    for (int i = 0; i < kMaxCodes; ++i) lengths[i] = 0;
    for (int i = 0; i < ncode; ++i) {
      int v = 0;
      if (!Bits(3, &v)) return false;
      lengths[kOrder[i]] = static_cast<short>(v);
    }
    for (int i = ncode; i < 19; ++i) lengths[kOrder[i]] = 0;

    Huffman lencode, distcode;
    if (!Construct(&lencode, lengths, 19)) {
      err_ = "the code-length code is over-subscribed";
      return false;
    }

    int index = 0;
    while (index < nlen + ndist) {
      int sym = 0;
      if (!Decode(lencode, &sym)) return false;
      if (sym < 16) {
        lengths[index++] = static_cast<short>(sym);
        continue;
      }
      short len = 0;
      int rep = 0;
      if (sym == 16) {
        if (index == 0) {
          err_ = "repeat code 16 with no previous length";
          return false;
        }
        len = lengths[index - 1];
        if (!Bits(2, &rep)) return false;
        rep += 3;
      } else if (sym == 17) {
        if (!Bits(3, &rep)) return false;
        rep += 3;
      } else {
        if (!Bits(7, &rep)) return false;
        rep += 11;
      }
      if (index + rep > nlen + ndist) {
        err_ = "too many lengths in the dynamic block header";
        return false;
      }
      while (rep-- > 0) lengths[index++] = len;
    }
    if (lengths[256] == 0) {
      err_ = "the dynamic block has no end-of-block code";
      return false;
    }
    if (!Construct(&lencode, lengths, nlen)) {
      err_ = "the literal/length code is over-subscribed";
      return false;
    }
    if (!Construct(&distcode, lengths + nlen, ndist)) {
      err_ = "the distance code is over-subscribed";
      return false;
    }
    return Codes(lencode, distcode);
  }

  const uint8_t* in_;
  size_t inlen_;
  size_t incnt_ = 0;
  int32_t bitbuf_ = 0;
  int bitcnt_ = 0;
  int64_t max_out_;
  std::string* out_;
  std::string err_;
  bool over_limit_ = false;
};

/* ══════════════════════════════════════════════════════════════════
 * 小工具
 * ════════════════════════════════════════════════════════════════ */

uint32_t U16(const std::string& b, size_t off) {
  return static_cast<uint32_t>(static_cast<unsigned char>(b[off])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(b[off + 1])) << 8);
}

uint32_t U32(const std::string& b, size_t off) {
  return static_cast<uint32_t>(static_cast<unsigned char>(b[off])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(b[off + 1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(b[off + 2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(b[off + 3])) << 24);
}

constexpr uint32_t kEocdSig = 0x06054b50u;
constexpr uint32_t kCdSig = 0x02014b50u;
constexpr uint32_t kLocalSig = 0x04034b50u;
constexpr uint32_t kZip64LocatorSig = 0x07064b50u;
constexpr size_t kMaxComment = 0xFFFF;

}  // namespace

InflateResult Inflate(const uint8_t* src, size_t n, int64_t max_out,
                      std::string* out) {
  out->clear();
  if (max_out < 0) max_out = 0;
  Inflater inf(src, n, max_out, out);
  InflateResult r = inf.Run();
  if (!r.ok) out->clear();
  return r;
}

uint32_t Crc32(const std::string& data) {
  static uint32_t table[256];
  static bool built = false;
  if (!built) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
    built = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (char ch : data) {
    c = table[(c ^ static_cast<unsigned char>(ch)) & 0xFFu] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

bool ReadZipCentralDirectory(const std::string& b, std::vector<ZipEntry>* out,
                             std::string* err) {
  out->clear();
  err->clear();
  const size_t len = b.size();
  if (len < 22) {
    *err = "file is too small to be a zip";
    return false;
  }

  const size_t tail_len = len < (kMaxComment + 22) ? len : (kMaxComment + 22);
  const size_t tail_start = len - tail_len;
  size_t eocd = std::string::npos;
  for (size_t i = len - 22 + 1; i-- > tail_start;) {
    if (U32(b, i) == kEocdSig) { eocd = i; break; }
  }
  if (eocd == std::string::npos) {
    *err = "no End Of Central Directory record - not a zip file";
    return false;
  }
  if (eocd >= 20 && U32(b, eocd - 20) == kZip64LocatorSig) {
    *err = "ZIP64 is not supported (no package here should be that large)";
    return false;
  }
  const uint32_t count = U16(b, eocd + 10);
  const uint32_t cd_size = U32(b, eocd + 12);
  const uint32_t cd_offset = U32(b, eocd + 16);
  if (count == 0xFFFFu || cd_size == 0xFFFFFFFFu || cd_offset == 0xFFFFFFFFu) {
    *err = "the central directory uses ZIP64 fields, which are not supported";
    return false;
  }
  if (static_cast<uint64_t>(cd_offset) + cd_size > len) {
    *err = "the central directory runs past the end of the file";
    return false;
  }
  if (cd_size > 16u * 1024 * 1024) {
    *err = "the central directory is too large";
    return false;
  }

  size_t p = cd_offset;
  const size_t end = cd_offset + cd_size;
  out->reserve(count < 4096 ? count : 4096);
  while (p + 46 <= end) {
    if (U32(b, p) != kCdSig) break;
    const uint32_t version_made_by = U16(b, p + 4);
    const uint32_t method = U16(b, p + 10);
    const uint32_t crc = U32(b, p + 16);
    const uint32_t comp = U32(b, p + 20);
    const uint32_t uncomp = U32(b, p + 24);
    const uint32_t name_len = U16(b, p + 28);
    const uint32_t extra_len = U16(b, p + 30);
    const uint32_t comment_len = U16(b, p + 32);
    const uint32_t external = U32(b, p + 38);
    const uint32_t local = U32(b, p + 42);
    if (p + 46 + name_len > end) {
      *err = "the central directory is truncated";
      return false;
    }
    if (comp == 0xFFFFFFFFu || uncomp == 0xFFFFFFFFu || local == 0xFFFFFFFFu) {
      *err = "an entry uses ZIP64 fields, which are not supported";
      return false;
    }
    ZipEntry e;
    e.name.assign(b, p + 46, name_len);
    e.method = static_cast<uint16_t>(method);
    e.crc32 = crc;
    e.compressed_size = comp;
    e.uncompressed_size = uncomp;
    e.local_offset = local;
    e.unix_mode = (external >> 16) & 0xFFFFu;
    e.host_os = static_cast<int>((version_made_by >> 8) & 0xFFu);
    out->push_back(std::move(e));
    p += 46 + name_len + extra_len + comment_len;
  }
  if (out->size() != count) {
    *err = "the central directory declares " + std::to_string(count) +
           " entries but only " + std::to_string(out->size()) + " were read";
    out->clear();
    return false;
  }
  return true;
}

bool ExtractZipEntry(const std::string& b, const ZipEntry& e, int64_t max_out,
                     std::string* out, std::string* err) {
  out->clear();
  err->clear();
  const size_t len = b.size();
  const size_t lo = static_cast<size_t>(e.local_offset);
  if (e.local_offset < 0 || lo + 30 > len) {
    *err = "local header offset is outside the file";
    return false;
  }
  if (U32(b, lo) != kLocalSig) {
    *err = "no local file header at the offset the central directory gave";
    return false;
  }
  const uint32_t flags = U16(b, lo + 6);
  const uint32_t lmethod = U16(b, lo + 8);
  const uint32_t lname_len = U16(b, lo + 26);
  const uint32_t lextra_len = U16(b, lo + 28);
  if (lo + 30 + lname_len > len) {
    *err = "the local header is truncated";
    return false;
  }
  // ⚠ 兩份中繼資料必須一致。不一致時直接失敗,不猜哪一份是真的 ——
  //   「檢查看中央目錄、解壓看 local header」正是繞過檢查的標準作法。
  if (b.compare(lo + 30, lname_len, e.name) != 0) {
    *err = "the local header names a different entry than the central directory";
    return false;
  }
  if (lmethod != e.method) {
    *err = "the local header and the central directory disagree on the compression method";
    return false;
  }
  if ((flags & 0x0001u) != 0) {
    *err = "the entry is encrypted";
    return false;
  }

  const size_t data = lo + 30 + lname_len + lextra_len;
  if (e.compressed_size < 0 ||
      data + static_cast<size_t>(e.compressed_size) > len) {
    *err = "the entry data runs past the end of the file";
    return false;
  }

  if (e.method == 0) {
    if (e.compressed_size != e.uncompressed_size) {
      *err = "a stored entry declares different compressed and uncompressed sizes";
      return false;
    }
    if (e.uncompressed_size > max_out) {
      *err = "the entry is larger than the per-entry limit";
      return false;
    }
    out->assign(b, data, static_cast<size_t>(e.compressed_size));
  } else if (e.method == 8) {
    const InflateResult r =
        Inflate(reinterpret_cast<const uint8_t*>(b.data()) + data,
                static_cast<size_t>(e.compressed_size), max_out, out);
    if (!r.ok) {
      *err = r.error;
      return false;
    }
  } else {
    *err = "unsupported compression method " + std::to_string(e.method);
    return false;
  }

  // 大小與 CRC 都拿中央目錄宣告的值核對。**這不是安全控制**
  // (兩個欄位都是打包端寫的),它擋的是傳輸損壞與我們自己解錯。
  if (static_cast<int64_t>(out->size()) != e.uncompressed_size) {
    *err = "inflated size " + std::to_string(out->size()) +
           " does not match the declared " + std::to_string(e.uncompressed_size);
    out->clear();
    return false;
  }
  if (Crc32(*out) != e.crc32) {
    *err = "CRC32 mismatch (the archive is damaged)";
    out->clear();
    return false;
  }
  return true;
}

}  // namespace rimewin
