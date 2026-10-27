#include "aios/net/gzip.hpp"

#include <algorithm>
#include <cctype>
#include <set>


namespace aios::net {

// The portable encoder: bare metal (no zlib there) and any host without it.
namespace {

// ---------------------------------------------------------------------------
// CRC-32 (IEEE 802.3, the gzip trailer's variant — same table/polynomial as
// aios::userland::crc32 in archive.cpp) kept as running state across calls,
// so the gzip trailer can be computed without ever holding the whole body.
// ---------------------------------------------------------------------------
uint32_t crc32_table(uint8_t byte_index) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    ready = true;
  }
  return table[byte_index];
}
uint32_t crc32_update(uint32_t running, ByteSpan data) {
  for (uint8_t b : data) running = crc32_table(uint8_t((running ^ b) & 0xff)) ^ (running >> 8);
  return running;
}

// ---------------------------------------------------------------------------
// DEFLATE (RFC 1951): same LZ77-hash-chain + fixed-Huffman approach as
// aios::userland::zlib_deflate (git.cpp), reimplemented here so it can run
// incrementally across feed() calls — see gzip.hpp for why that matters.
// ---------------------------------------------------------------------------
struct BitSink {
  ByteBuffer out;
  uint32_t bits = 0;
  int have = 0;
  void put(uint32_t value, int n) {
    bits |= (value & ((1u << n) - 1)) << have;
    have += n;
    while (have >= 8) { out.push_back((uint8_t)(bits & 0xff)); bits >>= 8; have -= 8; }
  }
  void code(uint32_t c, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; ++i) r |= ((c >> i) & 1) << (n - 1 - i);
    put(r, n);
  }
  void align() { if (have) { out.push_back((uint8_t)(bits & 0xff)); bits = 0; have = 0; } }
};

void fixed_literal(BitSink& b, uint32_t sym) {
  if (sym < 144) b.code(0x30 + sym, 8);
  else if (sym < 256) b.code(0x190 + sym - 144, 9);
  else if (sym < 280) b.code(sym - 256, 7);
  else b.code(0xc0 + sym - 280, 8);
}

const uint16_t kDeflateLenBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const uint8_t  kDeflateLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const uint16_t kDeflateDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const uint8_t  kDeflateDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

constexpr size_t kWindow = 32768, kMinMatch = 3, kMaxMatch = 258, kChain = 96;
constexpr uint32_t kHashSize = 1u << 15;

// Emits one non-final fixed-Huffman block covering local[start, local.size()),
// with matches allowed to reference back into local[0, start) (the carried
// sliding window) as well as within the new data itself. Every block —
// final or not — must end with its own end-of-block symbol per RFC 1951, so
// this always appends one; the caller decides BFINAL before calling.
void emit_block(BitSink& b, const ByteBuffer& local, size_t start) {
  const size_t n = local.size();
  if (n == 0) { fixed_literal(b, 256); return; }
  std::vector<int32_t> head((size_t)kHashSize, -1), prev(n, -1);
  auto hash3 = [&](size_t i) {
    return (uint32_t)(((uint32_t)local[i] << 10) ^ ((uint32_t)local[i + 1] << 5) ^ (uint32_t)local[i + 2]) & (kHashSize - 1);
  };
  // Seed the hash chain with the window prefix so matches starting in the
  // new data can find candidates there, without emitting symbols for it —
  // it was already emitted by an earlier call.
  for (size_t j = 0; j + kMinMatch <= start; ++j) { uint32_t h = hash3(j); prev[j] = head[h]; head[h] = int32_t(j); }

  size_t i = start;
  while (i < n) {
    size_t best_len = 0, best_dist = 0;
    if (i + kMinMatch <= n) {
      uint32_t h = hash3(i);
      int32_t cand = head[h];
      size_t tried = 0;
      while (cand >= 0 && tried < kChain) {
        size_t d = i - (size_t)cand;
        if (d == 0 || d > kWindow) break;
        size_t maxl = n - i;
        if (maxl > kMaxMatch) maxl = kMaxMatch;
        size_t l = 0;
        while (l < maxl && local[(size_t)cand + l] == local[i + l]) ++l;
        if (l > best_len) { best_len = l; best_dist = d; if (l == kMaxMatch) break; }
        cand = prev[(size_t)cand];
        ++tried;
      }
    }
    if (best_len >= kMinMatch) {
      int lc = 28;
      while (lc > 0 && kDeflateLenBase[lc] > best_len) --lc;
      fixed_literal(b, (uint32_t)(257 + lc));
      if (kDeflateLenExtra[lc]) b.put((uint32_t)(best_len - kDeflateLenBase[lc]), kDeflateLenExtra[lc]);
      int dc = 29;
      while (dc > 0 && kDeflateDistBase[dc] > best_dist) --dc;
      b.code((uint32_t)dc, 5);
      if (kDeflateDistExtra[dc]) b.put((uint32_t)(best_dist - kDeflateDistBase[dc]), kDeflateDistExtra[dc]);
      for (size_t k = 0; k < best_len; ++k) {
        if (i + k + kMinMatch <= n) { uint32_t h = hash3(i + k); prev[i + k] = head[h]; head[h] = (int32_t)(i + k); }
      }
      i += best_len;
    } else {
      if (i + kMinMatch <= n) { uint32_t h = hash3(i); prev[i] = head[h]; head[h] = (int32_t)i; }
      fixed_literal(b, (uint32_t)local[i]);
      ++i;
    }
  }
  fixed_literal(b, 256);  // end of block
}

}  // namespace

struct GzipStreamEncoder::Impl {
  BitSink sink;
  size_t drained = 0;
  ByteBuffer window;   // tail of everything fed so far, up to kWindow bytes
  uint32_t crc = 0xffffffffu;
  uint64_t total_len = 0;
  bool header_written = false;
};

GzipStreamEncoder::GzipStreamEncoder() : impl_(std::make_unique<Impl>()) {}
GzipStreamEncoder::~GzipStreamEncoder() = default;

ByteBuffer GzipStreamEncoder::feed(ByteSpan data, bool sync_flush) {
  Impl& s = *impl_;
  if (!s.header_written) {
    s.sink.out.push_back(0x1f); s.sink.out.push_back(0x8b);  // magic
    s.sink.out.push_back(0x08);                               // CM = deflate
    s.sink.out.push_back(0x00);                               // FLG = none
    s.sink.out.push_back(0); s.sink.out.push_back(0); s.sink.out.push_back(0); s.sink.out.push_back(0);  // MTIME
    s.sink.out.push_back(0x00);                               // XFL
    s.sink.out.push_back(0xff);                               // OS = unknown
    s.header_written = true;
  }
  if (!data.empty()) {
    s.crc = crc32_update(s.crc, data);
    s.total_len += data.size();

    ByteBuffer local;
    local.reserve(s.window.size() + data.size());
    local.insert(local.end(), s.window.begin(), s.window.end());
    local.insert(local.end(), data.begin(), data.end());
    size_t start = s.window.size();

    s.sink.put(0, 1);  // BFINAL = 0 — more blocks follow
    s.sink.put(1, 2);  // BTYPE = 01 — fixed Huffman
    emit_block(s.sink, local, start);
    if (sync_flush) s.sink.align();

    size_t keep = std::min(kWindow, local.size());
    s.window.assign(local.end() - keep, local.end());
  }

  ByteBuffer out(s.sink.out.begin() + s.drained, s.sink.out.end());
  s.sink.out.erase(s.sink.out.begin(), s.sink.out.begin() + s.drained);
  s.drained = s.sink.out.size();
  return out;
}

ByteBuffer GzipStreamEncoder::finish() {
  Impl& s = *impl_;
  // An empty body still needs a valid (empty) gzip stream. feed() drains the
  // header it writes, so its return value is the start of this output.
  ByteBuffer out = s.header_written ? ByteBuffer{} : feed({});
  s.sink.put(1, 1);  // BFINAL = 1
  s.sink.put(1, 2);  // BTYPE = 01 — fixed Huffman, empty block (just end-of-block)
  fixed_literal(s.sink, 256);
  s.sink.align();

  uint32_t crc = s.crc ^ 0xffffffffu;
  uint32_t isize = uint32_t(s.total_len);  // mod 2^32, per RFC 1952
  for (int i = 0; i < 4; ++i) s.sink.out.push_back(uint8_t(crc >> (8 * i)));
  for (int i = 0; i < 4; ++i) s.sink.out.push_back(uint8_t(isize >> (8 * i)));

  out.insert(out.end(), s.sink.out.begin() + s.drained, s.sink.out.end());
  s.drained = s.sink.out.size();
  return out;
}


ByteBuffer gzip_compress_whole(ByteSpan data) {
  GzipStreamEncoder enc;
  ByteBuffer out = enc.feed(data);
  ByteBuffer tail = enc.finish();
  out.insert(out.end(), tail.begin(), tail.end());
  return out;
}

namespace {
bool icontains(const std::string& hay, const std::string& needle) {
  auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                         [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
  return it != hay.end();
}
bool compressible_mime(const std::string& mime) {
  static const std::set<std::string> kTypes = {
      "text/html", "text/css", "text/plain", "text/xml", "text/javascript",
      "application/javascript", "application/json", "application/xml",
      "application/xhtml+xml", "image/svg+xml",
  };
  std::string base = mime;
  auto semi = base.find(';');
  if (semi != std::string::npos) base.resize(semi);
  return kTypes.count(base) != 0;
}
}  // namespace

std::string negotiate_content_encoding(const std::string& accept_encoding_header, const std::string& mime) {
  if (!compressible_mime(mime)) return "";
  if (icontains(accept_encoding_header, "gzip")) return "gzip";
  return "";
}

}  // namespace aios::net
