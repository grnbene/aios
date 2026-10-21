#include "aios/userland/git.hpp"

#include <chrono>
#if !defined(__wasi__)
#include <thread>
#endif
#include "aios/userland/git_blocks.hpp"
#include "aios/userland/git_structural.hpp"
#include "aios/net/gzip.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <unordered_set>
#include "aios/hal/sync.hpp"
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <set>
#include "aios/crypto/sha1.hpp"
#include "aios/hal/host.hpp"
#if defined(__SSE2__)
#include <emmintrin.h>   // adler32: SSE2 is part of every x86-64, so no run-time check is needed
#endif

namespace aios::userland {

std::string git_local_tz(uint64_t when) {
  std::time_t t = (std::time_t)when;
  std::tm local{}, utc{};
#ifdef _WIN32
  localtime_s(&local, &t);
  gmtime_s(&utc, &t);
#else
  localtime_r(&t, &local);
  gmtime_r(&t, &utc);
#endif
  // The offset is local minus UTC, read off the broken-down times (no timegm needed).
  long days = local.tm_year == utc.tm_year ? local.tm_yday - utc.tm_yday : (local.tm_year > utc.tm_year ? 1 : -1);
  long mins = days * 1440 + (local.tm_hour - utc.tm_hour) * 60 + (local.tm_min - utc.tm_min);
  char buf[8];
  long a = mins < 0 ? -mins : mins;
  std::snprintf(buf, sizeof buf, "%c%02ld%02ld", mins < 0 ? '-' : '+', a / 60, a % 60);
  return buf;
}

const char* git_change_name(GitChange c) {
  switch (c) {
    case GitChange::Added: return "new file";
    case GitChange::Modified: return "modified";
    case GitChange::Deleted: return "deleted";
    case GitChange::Untracked: return "untracked";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// zlib envelope
// ---------------------------------------------------------------------------

// Every object read or written is checked with this, so it runs over every byte a clone moves. A block is taken as
// rows of sixteen bytes. Then a gains every byte, and b gains, for each row, sixteen copies of the a that row started
// with (16 * rows * a, plus 16 times the running byte sum at the start of each row: `starts`) and each byte weighted
// by how many of the row's sixteen sums it falls into, 16 for the first down to 1 for the last (`weighted`). No sum
// waits on the one before it, so the rows go through vector registers: with SSE2, which every x86-64 has, about four
// times the byte-at-a-time speed; elsewhere as sixteen column sums the compiler may vectorize itself. Every partial
// sum stays below the byte-at-a-time b, which the 5552 limit keeps under 2^32, so nothing overflows.
uint32_t adler32(ByteSpan data) {
  uint32_t a = 1, b = 0;
  const uint8_t* p = data.data();
  size_t left = data.size();
  while (left) {   // 5552 bytes at a time: the most that cannot overflow before the remainder is taken
    size_t n = std::min<size_t>(left, 5552);
    left -= n;
    if (const uint32_t rows = (uint32_t)(n / 16)) {
#if defined(__SSE2__)
      // psadbw sums the row's bytes; the weights go on with pmaddwd over the bytes widened to sixteen bits.
      const __m128i zero = _mm_setzero_si128();
      const __m128i w_lo = _mm_setr_epi16(16, 15, 14, 13, 12, 11, 10, 9), w_hi = _mm_setr_epi16(8, 7, 6, 5, 4, 3, 2, 1);
      __m128i vsum = zero, vstarts = zero, vweighted = zero;
      for (uint32_t r = 0; r < rows; ++r, p += 16) {
        const __m128i x = _mm_loadu_si128((const __m128i*)p);
        vstarts = _mm_add_epi32(vstarts, vsum);
        vsum = _mm_add_epi32(vsum, _mm_sad_epu8(x, zero));
        vweighted = _mm_add_epi32(vweighted, _mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi8(x, zero), w_lo),
                                                           _mm_madd_epi16(_mm_unpackhi_epi8(x, zero), w_hi)));
      }
      auto hsum = [](__m128i v) {
        v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0x4e));
        v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0xb1));
        return (uint32_t)_mm_cvtsi128_si32(v);
      };
      const uint32_t sum = hsum(vsum), starts = hsum(vstarts), weighted = hsum(vweighted);
#else
      // Per column: the column's byte sum (col) and the sum of its running total at the start of each row (pre).
      uint32_t col[16] = {}, pre[16] = {};
      for (uint32_t r = 0; r < rows; ++r, p += 16)
        for (int k = 0; k < 16; ++k) { pre[k] += col[k]; col[k] += p[k]; }
      uint32_t sum = 0, starts = 0, weighted = 0;
      for (int k = 0; k < 16; ++k) { sum += col[k]; starts += pre[k]; weighted += (uint32_t)(16 - k) * col[k]; }
#endif
      b += 16 * (rows * a + starts) + weighted;
      a += sum;
      n -= (size_t)rows * 16;
    }
    for (; n; --n, ++p) { a += *p; b += a; }
    a %= 65521;
    b %= 65521;
  }
  return (b << 16) | a;
}

// LZ77 with a hash chain, emitted in deflate's fixed Huffman code. Real
// compression, written by this kernel: an object database that stored every
// blob whole would be several times the size of the repository it holds, and
// the packfiles this machine sends to a remote would be absurd on the wire.
namespace {

struct BitSink {
  ByteBuffer out;
  uint32_t bits = 0;
  int have = 0;
  // Deflate packs bit strings from the least significant bit up...
  void put(uint32_t value, int n) {
    bits |= (value & ((1u << n) - 1)) << have;
    have += n;
    while (have >= 8) { out.push_back((uint8_t)(bits & 0xff)); bits >>= 8; have -= 8; }
  }
  // ...but a Huffman code travels most significant bit first, so it is reversed.
  void code(uint32_t c, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; ++i) r |= ((c >> i) & 1) << (n - 1 - i);
    put(r, n);
  }
  void align() { if (have) { out.push_back((uint8_t)(bits & 0xff)); bits = 0; have = 0; } }
};

// The fixed literal/length alphabet of RFC 1951 3.2.6.
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

}  // namespace

namespace {

// ---- deflate, as zlib does it: LZ77 with hash chains and lazy matching, then for each block whichever of dynamic
// Huffman codes, the fixed ones, or no compression at all comes out smallest ----

struct BitOut {
  ByteBuffer& out;
  uint64_t acc = 0;
  int n = 0;
  void put(uint32_t v, int bits) {   // least significant bit first; a Huffman code is passed already reversed
    acc |= (uint64_t)v << n;
    n += bits;
    while (n >= 8) { out.push_back((uint8_t)acc); acc >>= 8; n -= 8; }
  }
  void align() { if (n) { out.push_back((uint8_t)acc); acc = 0; n = 0; } }
};

// Code lengths for these frequencies, none longer than max_len (a Huffman tree, then the longest codes shortened and
// the slack taken back from shorter ones until the code is complete again). A symbol never seen gets 0.
void huff_lengths(const uint32_t* freq, int n, int max_len, uint8_t* len) {
  std::memset(len, 0, (size_t)n);
  std::vector<int> used;
  for (int i = 0; i < n; ++i) if (freq[i]) used.push_back(i);
  if (used.empty()) return;
  if (used.size() == 1) { len[used[0]] = 1; return; }
  const int m = (int)used.size();
  std::vector<uint64_t> weight((size_t)(2 * m));
  std::vector<int> parent((size_t)(2 * m), -1);
  // Two queues: the leaves in order of frequency, and the nodes made so far (which come out in order by themselves).
  std::vector<int> leaves((size_t)m);
  for (int i = 0; i < m; ++i) leaves[(size_t)i] = i;
  std::sort(leaves.begin(), leaves.end(), [&](int a2, int b2) { return freq[used[(size_t)a2]] != freq[used[(size_t)b2]] ? freq[used[(size_t)a2]] < freq[used[(size_t)b2]] : a2 < b2; });
  for (int i = 0; i < m; ++i) weight[(size_t)i] = freq[used[(size_t)i]];
  int li = 0, ni = m, made = m;
  auto take = [&]() {
    if (li < m && (ni >= made || weight[(size_t)leaves[(size_t)li]] <= weight[(size_t)ni])) return leaves[(size_t)li++];
    return ni++;
  };
  for (int k = 0; k < m - 1; ++k) {
    int x = take(), y = take();
    weight[(size_t)made] = weight[(size_t)x] + weight[(size_t)y];
    parent[(size_t)x] = parent[(size_t)y] = made;
    ++made;
  }
  int count[64] = {};
  for (int i = 0; i < m; ++i) {
    int d = 0;
    for (int p = parent[(size_t)i]; p >= 0; p = parent[(size_t)p]) ++d;
    ++count[d > 63 ? 63 : d];
  }
  for (int i = max_len + 1; i < 64; ++i) { count[max_len] += count[i]; count[i] = 0; }
  uint64_t total = 0;
  for (int i = max_len; i > 0; --i) total += (uint64_t)count[i] << (max_len - i);
  while (total != (1ull << max_len)) {
    --count[max_len];
    for (int i = max_len - 1; i > 0; --i) if (count[i]) { --count[i]; count[i + 1] += 2; break; }
    --total;
  }
  // The most frequent symbols get the shortest codes.
  int at = m;
  for (int l = 1; l <= max_len; ++l)
    for (int c = count[l]; c > 0; --c) len[used[(size_t)leaves[(size_t)--at]]] = (uint8_t)l;
}

// The canonical codes for these lengths, each reversed so it can be written least significant bit first.
void huff_codes(const uint8_t* len, int n, uint16_t* code) {
  int count[16] = {};
  for (int i = 0; i < n; ++i) ++count[len[i]];
  count[0] = 0;
  uint32_t next[16] = {};
  uint32_t c = 0;
  for (int l = 1; l < 16; ++l) { c = (c + (uint32_t)count[l - 1]) << 1; next[l] = c; }
  for (int i = 0; i < n; ++i) {
    code[i] = 0;
    if (!len[i]) continue;
    uint32_t v = next[len[i]]++, r = 0;
    for (int b2 = 0; b2 < len[i]; ++b2) r |= ((v >> b2) & 1u) << (len[i] - 1 - b2);
    code[i] = (uint16_t)r;
  }
}

struct DeflateTables {
  uint8_t len_code[259];     // match length -> index into kDeflateLenBase
  uint8_t dist_lo[256];      // distance - 1 (below 256) -> distance code
  uint8_t dist_hi[256];      // (distance - 1) >> 7 -> distance code, for the rest
  uint8_t fixed_lit[288], fixed_dist[30];
  uint16_t fixed_lit_code[288], fixed_dist_code[30];
  DeflateTables() {
    for (int l = 3, c = 0; l <= 258; ++l) { while (c < 28 && kDeflateLenBase[c + 1] <= l) ++c; len_code[l] = (uint8_t)c; }
    auto code_of = [](uint32_t d) { int c = 29; while (c > 0 && kDeflateDistBase[c] > d) --c; return (uint8_t)c; };
    for (uint32_t d = 1; d <= 256; ++d) dist_lo[d - 1] = code_of(d);
    for (uint32_t h = 0; h < 256; ++h) dist_hi[h] = code_of((h << 7) + 1);
    for (int i = 0; i < 288; ++i) fixed_lit[i] = (uint8_t)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
    for (int i = 0; i < 30; ++i) fixed_dist[i] = 5;
    huff_codes(fixed_lit, 288, fixed_lit_code);
    huff_codes(fixed_dist, 30, fixed_dist_code);
  }
  int dist_code(uint32_t d) const { return d <= 256 ? dist_lo[d - 1] : dist_hi[(d - 1) >> 7]; }
};
const DeflateTables& deflate_tables() { static const DeflateTables t; return t; }

// A token: a literal byte, or (bit 31 set) a match of `length` bytes found `distance` back.
inline uint32_t tok_match(uint32_t len, uint32_t dist) { return 0x80000000u | (len << 16) | (dist - 1); }

// One block: the tokens, and the stretch of input they stand for (in case storing it plainly is smallest).
void deflate_block(BitOut& bw, const std::vector<uint32_t>& toks, const uint8_t* raw, size_t raw_len, bool last) {
  const DeflateTables& T = deflate_tables();
  uint32_t lf[286] = {}, df[30] = {};
  uint64_t extra_bits = 0;
  for (uint32_t t : toks) {
    if (t & 0x80000000u) {
      const uint32_t len = (t >> 16) & 0x1ff, dist = (t & 0xffff) + 1;
      const int lc = T.len_code[len], dc = T.dist_code(dist);
      ++lf[257 + lc];
      ++df[dc];
      extra_bits += kDeflateLenExtra[lc] + kDeflateDistExtra[dc];
    } else {
      ++lf[t];
    }
  }
  lf[256] = 1;
  uint8_t ll[286], dl[30];
  huff_lengths(lf, 286, 15, ll);
  huff_lengths(df, 30, 15, dl);
  int hlit = 286, hdist = 30;
  while (hlit > 257 && !ll[hlit - 1]) --hlit;
  while (hdist > 1 && !dl[hdist - 1]) --hdist;
  if (hdist == 1 && !dl[0]) dl[0] = 1;   // no match in the block: the format still wants one distance code

  // The two sets of lengths, run-length coded in the code-length alphabet (16: repeat the last, 17/18: runs of zero).
  struct Cl { uint8_t sym, extra, bits; };
  std::vector<Cl> cl;
  uint32_t cf[19] = {};
  {
    uint8_t all[316];
    std::memcpy(all, ll, (size_t)hlit);
    std::memcpy(all + hlit, dl, (size_t)hdist);
    const int total = hlit + hdist;
    for (int i = 0; i < total;) {
      const uint8_t v = all[i];
      int run = 1;
      while (i + run < total && all[i + run] == v) ++run;
      i += run;
      if (v == 0) {
        while (run >= 11) { int r = run > 138 ? 138 : run; cl.push_back({18, (uint8_t)(r - 11), 7}); ++cf[18]; run -= r; }
        if (run >= 3) { cl.push_back({17, (uint8_t)(run - 3), 3}); ++cf[17]; run = 0; }
        while (run-- > 0) { cl.push_back({0, 0, 0}); ++cf[0]; }
      } else {
        cl.push_back({v, 0, 0}); ++cf[v]; --run;
        while (run >= 3) { int r = run > 6 ? 6 : run; cl.push_back({16, (uint8_t)(r - 3), 2}); ++cf[16]; run -= r; }
        while (run-- > 0) { cl.push_back({v, 0, 0}); ++cf[v]; }
      }
    }
  }
  uint8_t cll[19];
  huff_lengths(cf, 19, 7, cll);
  static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  int hclen = 19;
  while (hclen > 4 && !cll[order[hclen - 1]]) --hclen;

  uint64_t dyn = 3 + 5 + 5 + 4 + (uint64_t)hclen * 3 + extra_bits, fix = 3 + extra_bits;
  for (auto& c : cl) dyn += cll[c.sym] + c.bits;
  for (int i = 0; i < 286; ++i) { dyn += (uint64_t)lf[i] * ll[i]; fix += (uint64_t)lf[i] * T.fixed_lit[i]; }
  for (int i = 0; i < 30; ++i) { dyn += (uint64_t)df[i] * dl[i]; fix += (uint64_t)df[i] * 5; }
  const uint64_t stored = ((uint64_t)raw_len + 5 * (raw_len / 65535 + 1) + 1) * 8;

  if (stored < dyn && stored < fix) {
    size_t off = 0;
    do {
      const size_t n = std::min<size_t>(65535, raw_len - off);
      const bool fin = last && off + n == raw_len;
      bw.put(fin ? 1 : 0, 3);   // BFINAL, BTYPE 00
      bw.align();
      bw.out.push_back((uint8_t)(n & 0xff)); bw.out.push_back((uint8_t)(n >> 8));
      bw.out.push_back((uint8_t)(~n & 0xff)); bw.out.push_back((uint8_t)((~n >> 8) & 0xff));
      bw.out.insert(bw.out.end(), raw + off, raw + off + n);
      off += n;
    } while (off < raw_len);
    return;
  }
  const bool dynamic = dyn < fix;
  uint16_t lcode_buf[286], dcode_buf[30];
  const uint8_t* llen = T.fixed_lit;
  const uint8_t* dlen = T.fixed_dist;
  const uint16_t* lcode = T.fixed_lit_code;
  const uint16_t* dcode = T.fixed_dist_code;
  bw.put((last ? 1u : 0u) | (dynamic ? 4u : 2u), 3);
  if (dynamic) {
    huff_codes(ll, 286, lcode_buf);
    huff_codes(dl, 30, dcode_buf);
    llen = ll; dlen = dl; lcode = lcode_buf; dcode = dcode_buf;
    uint16_t ccode[19];
    huff_codes(cll, 19, ccode);
    bw.put((uint32_t)(hlit - 257), 5);
    bw.put((uint32_t)(hdist - 1), 5);
    bw.put((uint32_t)(hclen - 4), 4);
    for (int i = 0; i < hclen; ++i) bw.put(cll[order[i]], 3);
    for (auto& c : cl) { bw.put(ccode[c.sym], cll[c.sym]); if (c.bits) bw.put(c.extra, c.bits); }
  }
  for (uint32_t t : toks) {
    if (t & 0x80000000u) {
      const uint32_t len = (t >> 16) & 0x1ff, dist = (t & 0xffff) + 1;
      const int lc = T.len_code[len], dc = T.dist_code(dist);
      bw.put(lcode[257 + lc], llen[257 + lc]);
      if (kDeflateLenExtra[lc]) bw.put(len - kDeflateLenBase[lc], kDeflateLenExtra[lc]);
      bw.put(dcode[dc], dlen[dc]);
      if (kDeflateDistExtra[dc]) bw.put(dist - kDeflateDistBase[dc], kDeflateDistExtra[dc]);
    } else {
      bw.put(lcode[t], llen[t]);
    }
  }
  bw.put(lcode[256], llen[256]);
}

// How hard to look for matches, as zlib's levels put it: a match this good cuts the search short (good), one this
// long is taken without looking for a better one starting a byte later (lazy), one this long ends the search (nice),
// and at most this many earlier positions are tried (chain).
struct DeflateEffort { uint32_t good, lazy, nice, chain; };
DeflateEffort deflate_effort(int level) {
  if (level <= 1) return {4, 4, 8, 4};
  if (level <= 3) return {4, 6, 32, 32};
  if (level <= 5) return {8, 16, 32, 32};
  if (level == 6) return {8, 16, 128, 128};
  if (level == 7) return {8, 32, 128, 256};
  if (level == 8) return {32, 128, 258, 1024};
  return {32, 258, 258, 4096};
}

void deflate_stream(BitOut& bw, ByteSpan data, int level) {
  const DeflateEffort E = deflate_effort(level);
  const uint8_t* d = data.data();
  const size_t n = data.size();
  const size_t kWindow = 32768, kMask = kWindow - 1;
  const uint32_t kHashSize = 1u << 15, kNone = 0xffffffffu;
  const size_t kBlockTokens = 1u << 15;
  std::vector<uint32_t> head((size_t)kHashSize, kNone), prev(kWindow, kNone);
  auto hash3 = [&](size_t i) { return (((uint32_t)d[i] << 10) ^ ((uint32_t)d[i + 1] << 5) ^ (uint32_t)d[i + 2]) & (kHashSize - 1); };
  auto insert = [&](size_t i) {
    if (i + 3 > n) return;
    const uint32_t h = hash3(i);
    prev[i & kMask] = head[h];
    head[h] = (uint32_t)i;
  };
  // The longest match for position i that beats `beat` bytes (0 when there is none).
  auto find = [&](size_t i, uint32_t beat, uint32_t& dist) -> uint32_t {
    if (i + 3 > n) return 0;
    const uint32_t maxl = (uint32_t)std::min<size_t>(258, n - i);
    if (beat >= maxl) return 0;
    uint32_t best = beat < 2 ? 2 : beat, found = 0;
    uint32_t chain = beat >= E.good ? E.chain >> 2 : E.chain;
    const uint32_t nice = std::min(E.nice, maxl);
    uint32_t cand = head[hash3(i)];
    while (cand != kNone && chain--) {
      const size_t back = i - cand;
      if (back > kWindow || back == 0) break;
      const uint8_t* a2 = d + cand;
      const uint8_t* b2 = d + i;
      if (a2[best] == b2[best] && a2[0] == b2[0] && a2[1] == b2[1]) {
        uint32_t l = 2;
        while (l < maxl && a2[l] == b2[l]) ++l;
        if (l > best) {
          best = l; found = l; dist = (uint32_t)back;
          if (l >= nice) break;
          if (best >= maxl) break;
        }
      }
      const uint32_t older = prev[cand & kMask];
      if (older == kNone || older >= cand) break;   // the slot has been reused by a later position
      cand = older;
    }
    if (found == 3 && dist > 4096) return 0;   // three bytes from far away cost more than they save
    return found;
  };

  std::vector<uint32_t> toks;
  toks.reserve(kBlockTokens + 2);
  size_t block_start = 0, covered = 0;   // input bytes the tokens so far stand for: [block_start, covered)
  auto flush = [&](bool last) {
    deflate_block(bw, toks, d + block_start, covered - block_start, last);
    toks.clear();
    block_start = covered;
  };
  auto emit_lit = [&](uint8_t b2) { toks.push_back(b2); covered += 1; if (toks.size() >= kBlockTokens) flush(false); };
  auto emit_match = [&](uint32_t len, uint32_t dist) { toks.push_back(tok_match(len, dist)); covered += len; if (toks.size() >= kBlockTokens) flush(false); };

  size_t i = 0;
  bool pending = false;          // a match found at i - 1, held to see whether i has a longer one
  uint32_t plen = 0, pdist = 0;
  while (i < n) {
    uint32_t dist = 0;
    const uint32_t len = find(i, pending ? plen : 0, dist);
    insert(i);
    if (pending) {
      if (len > plen) {
        emit_lit(d[i - 1]);
        plen = len; pdist = dist;
        ++i;
        continue;
      }
      const size_t start = i - 1;
      emit_match(plen, pdist);
      for (size_t k = i + 1; k < start + plen; ++k) insert(k);
      i = start + plen;
      pending = false;
      continue;
    }
    if (len >= 3) {
      if (len >= E.lazy) {
        emit_match(len, dist);
        for (size_t k = i + 1; k < i + len; ++k) insert(k);
        i += len;
      } else {
        pending = true; plen = len; pdist = dist;
        ++i;
      }
    } else {
      emit_lit(d[i]);
      ++i;
    }
  }
  if (pending) emit_match(plen, pdist);
  flush(true);
}

}  // namespace

ByteBuffer zlib_deflate(ByteSpan data, int level) {
  ByteBuffer out;
  out.reserve(data.size() / 2 + 64);
  out.push_back(0x78);        // CM = 8 (deflate), CINFO = 7 (32K window)
  out.push_back(0x9c);        // FLEVEL = 2, no preset dictionary, check bits
  BitOut bw{out};
  deflate_stream(bw, data, level);
  bw.align();
  uint32_t ad = adler32(data);
  out.push_back((uint8_t)(ad >> 24));
  out.push_back((uint8_t)(ad >> 16));
  out.push_back((uint8_t)(ad >> 8));
  out.push_back((uint8_t)ad);
  return out;
}


// A zlib stream whose deflate payload is nothing but stored blocks. Legal
// deflate, so any zlib reader — including real git — accepts it; the object
// database stays byte-identical in identity terms because the id is taken over
// the UNCOMPRESSED serialization, never over these bytes.
ByteBuffer zlib_deflate_stored(ByteSpan data) {
  ByteBuffer out;
  out.push_back(0x78);          // CM = 8 (deflate), CINFO = 7 (32K window)
  out.push_back(0x01);          // FCHECK so (0x78<<8|0x01) % 31 == 0, no preset dict
  size_t off = 0;
  do {
    size_t n = data.size() - off;
    if (n > 65535) n = 65535;
    bool last = (off + n == data.size());
    out.push_back(last ? 1 : 0);                 // BFINAL + BTYPE=00, then byte aligned
    out.push_back((uint8_t)(n & 0xff));
    out.push_back((uint8_t)(n >> 8));
    out.push_back((uint8_t)(~n & 0xff));
    out.push_back((uint8_t)((~n >> 8) & 0xff));
    out.insert(out.end(), data.begin() + off, data.begin() + off + n);
    off += n;
  } while (off < data.size());
  uint32_t ad = adler32(data);
  out.push_back((uint8_t)(ad >> 24));
  out.push_back((uint8_t)(ad >> 16));
  out.push_back((uint8_t)(ad >> 8));
  out.push_back((uint8_t)ad);
  return out;
}

namespace {

// Bits come out of the stream least significant first. Up to eight bytes are held at a time, so a code is looked up
// and dropped in a couple of operations rather than assembled a bit at a time (a pack of hundreds of megabytes is
// inflated once to index it: bit by bit that took most of a minute).
struct BitReader {
  ByteSpan d;
  size_t pos = 0;
  uint64_t buf = 0;
  int have = 0;
  bool bad = false;
  void fill() { while (have <= 56 && pos < d.size()) { buf |= (uint64_t)d[pos++] << have; have += 8; } }
  // The next n bits without taking them; past the end of the data they read as zeros (the taker then finds too few).
  uint32_t peek(int n) { if (have < n) fill(); return (uint32_t)(buf & ((1u << n) - 1)); }
  void drop(int n) { if (n > have) { bad = true; have = 0; buf = 0; return; } buf >>= n; have -= n; }
  int bit() {
    if (have < 1) { fill(); if (have < 1) { bad = true; return 0; } }
    int b = (int)(buf & 1); buf >>= 1; --have; return b;
  }
  uint32_t bits(int n) {
    if (n == 0) return 0;
    if (have < n) { fill(); if (have < n) { bad = true; have = 0; buf = 0; return 0; } }
    uint32_t v = (uint32_t)(buf & ((1u << n) - 1)); buf >>= n; have -= n; return v;
  }
  // To the next byte boundary, with the whole bytes held given back: what follows is read from d at pos.
  void align() { int r = have % 8; buf >>= r; have -= r; pos -= (size_t)(have / 8); have = 0; buf = 0; }
  // Bytes of the stream used so far: a byte only partly read counts, bytes held and untouched do not.
  size_t consumed() const { return pos - (size_t)(have / 8); }
};

// A canonical Huffman code: counts and symbols for the bit-by-bit walk (codes longer than kFastBits), and a table
// indexed by the next kFastBits bits of the stream for every shorter code: (symbol << 4) | length, 0 where none.
const int kFastBits = 9;
struct Huff { uint16_t counts[16]; std::vector<uint16_t> symbols; uint16_t fast[1 << kFastBits]; };

void huff_build(Huff& h, const uint8_t* lengths, int n) {
  for (int i = 0; i < 16; ++i) h.counts[i] = 0;
  for (int i = 0; i < n; ++i) h.counts[lengths[i]]++;
  h.counts[0] = 0;
  uint16_t offs[16];
  offs[0] = 0; offs[1] = 0;
  for (int i = 1; i < 15; ++i) offs[i + 1] = (uint16_t)(offs[i] + h.counts[i]);
  h.symbols.assign((size_t)n, 0);
  for (int i = 0; i < n; ++i) if (lengths[i]) h.symbols[offs[lengths[i]]++] = (uint16_t)i;
  // The codes themselves, in order of length then symbol; each short one fills every table slot whose low bits are
  // the code as the stream carries it (most significant bit first, so reversed here).
  std::memset(h.fast, 0, sizeof(h.fast));
  uint32_t next[16];
  uint32_t code = 0;
  next[0] = 0;
  for (int len = 1; len < 16; ++len) { code = (code + h.counts[len - 1]) << 1; next[len] = code; }
  for (int i = 0; i < n; ++i) {
    int len = lengths[i];
    if (!len) continue;
    uint32_t c = next[len]++;
    if (len > kFastBits || (c >> len)) continue;   // long, or an over-subscribed set: the walk below decides
    uint32_t r = 0;
    for (int b = 0; b < len; ++b) r |= ((c >> b) & 1u) << (len - 1 - b);
    for (uint32_t v = r; v < (1u << kFastBits); v += 1u << len) h.fast[v] = (uint16_t)((i << 4) | len);
  }
}

int huff_decode(BitReader& br, const Huff& h) {
  if (uint16_t e = h.fast[br.peek(kFastBits)]) {
    int len = e & 15;
    if (len <= br.have) { br.drop(len); return e >> 4; }
    br.bad = true;   // the stream ends inside this code
    return -1;
  }
  int code = 0, first = 0, index = 0;
  for (int len = 1; len < 16; ++len) {
    code |= br.bit();
    int count = h.counts[len];
    if (code - first < count) return h.symbols[(size_t)(index + (code - first))];
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  return -1;
}

const uint16_t kLenBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const uint8_t  kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const uint16_t kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const uint8_t  kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

}  // namespace

Result<ByteBuffer> zlib_inflate(ByteSpan data) { return zlib_inflate_at(data, 0, nullptr); }

// The same inflater, starting at an offset inside a larger buffer and reporting
// where the stream ended. A packfile is a run of zlib streams laid end to end
// with no length in front of them, so the only way to find the next object is
// to be told where the last one stopped.
Result<ByteBuffer> zlib_inflate_at(ByteSpan whole, size_t offset, size_t* end_offset, size_t expect) {
  if (offset > whole.size()) return Result<ByteBuffer>(Status::Invalid, "inflate past the end of the buffer");
  ByteSpan data = whole.subspan(offset);
  if (data.size() < 6) return Result<ByteBuffer>(Status::Corrupt, "zlib stream too short");
  uint8_t cmf = data[0], flg = data[1];
  if ((cmf & 0x0f) != 8) return Result<ByteBuffer>(Status::Corrupt, "not a deflate stream");
  if (((uint32_t)cmf << 8 | flg) % 31 != 0) return Result<ByteBuffer>(Status::Corrupt, "bad zlib header check");
  if (flg & 0x20) return Result<ByteBuffer>(Status::Unsupported, "zlib preset dictionary");
#if defined(AIOS_HAVE_ZLIB)
  {
    ByteBuffer out;
    size_t used = 0;
    if (net::zlib_inflate_z(data, &out, &used, expect)) {
      if (end_offset) *end_offset = offset + used;
      return out;
    }
  }
#endif
  size_t inner_end = 0;
  auto out = inflate_raw(whole, offset + 2, &inner_end, expect);
  if (!out.ok()) return out;
  // The stream ends with the Adler-32 of what it inflates to: checked, so a damaged object is an error and never
  // content (a flipped bit that still inflates used to be read as the file).
  const size_t end = inner_end + 4;
  if (end > whole.size()) return Result<ByteBuffer>(Status::Corrupt, "zlib stream ends before its checksum");
  const uint8_t* t = whole.data() + inner_end;
  const uint32_t want = ((uint32_t)t[0] << 24) | ((uint32_t)t[1] << 16) | ((uint32_t)t[2] << 8) | (uint32_t)t[3];
  if (adler32(ByteSpan(out->data(), out->size())) != want) return Result<ByteBuffer>(Status::Corrupt, "zlib checksum does not match: the data is damaged");
  if (end_offset) *end_offset = end;
  return out;
}

// The deflate stream itself: no zlib header, no gzip header, just the blocks.
Result<ByteBuffer> inflate_raw(ByteSpan whole, size_t offset, size_t* end_offset, size_t expect) {
  if (offset > whole.size()) return Result<ByteBuffer>(Status::Invalid, "inflate past the end of the buffer");
  ByteSpan data = whole.subspan(offset);
#if defined(AIOS_HAVE_ZLIB)
  // The system zlib where the host has one: a clone inflates every object of
  // the pack, and the decoder below took a fifth of its time. The decoder
  // stays for a host without zlib, and for anything zlib refuses.
  {
    ByteBuffer out;
    size_t used = 0;
    if (net::zlib_inflate_raw(data, &out, &used, expect)) {
      if (end_offset) *end_offset = offset + used;
      return out;
    }
  }
#endif

  BitReader br{data};
  ByteBuffer out;
  if (expect) out.reserve(expect);
  Huff fixed_lit, fixed_dist;
  bool fixed_ready = false;

  for (;;) {
    int final_block = br.bit();
    int type = (int)br.bits(2);
    if (br.bad) return Result<ByteBuffer>(Status::Corrupt, "truncated deflate stream");

    if (type == 0) {
      br.align();
      if (br.pos + 4 > br.d.size()) return Result<ByteBuffer>(Status::Corrupt, "truncated stored block");
      uint32_t len = (uint32_t)br.d[br.pos] | ((uint32_t)br.d[br.pos + 1] << 8);
      br.pos += 4;
      if (br.pos + len > br.d.size()) return Result<ByteBuffer>(Status::Corrupt, "stored block runs past end");
      out.insert(out.end(), br.d.begin() + br.pos, br.d.begin() + br.pos + len);
      br.pos += len;
    } else if (type == 1 || type == 2) {
      Huff dyn_lit, dyn_dist;
      const Huff* lit;
      const Huff* dist;
      if (type == 1) {
        if (!fixed_ready) {
          uint8_t l[288];
          for (int i = 0; i < 144; ++i) l[i] = 8;
          for (int i = 144; i < 256; ++i) l[i] = 9;
          for (int i = 256; i < 280; ++i) l[i] = 7;
          for (int i = 280; i < 288; ++i) l[i] = 8;
          huff_build(fixed_lit, l, 288);
          uint8_t d2[30];
          for (int i = 0; i < 30; ++i) d2[i] = 5;
          huff_build(fixed_dist, d2, 30);
          fixed_ready = true;
        }
        lit = &fixed_lit; dist = &fixed_dist;
      } else {
        int hlit = (int)br.bits(5) + 257;
        int hdist = (int)br.bits(5) + 1;
        int hclen = (int)br.bits(4) + 4;
        static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
        uint8_t clen[19];
        std::memset(clen, 0, sizeof(clen));
        for (int i = 0; i < hclen; ++i) clen[order[i]] = (uint8_t)br.bits(3);
        Huff code_huff;
        huff_build(code_huff, clen, 19);
        uint8_t lengths[320];
        std::memset(lengths, 0, sizeof(lengths));
        int n = 0;
        while (n < hlit + hdist) {
          int sym = huff_decode(br, code_huff);
          if (sym < 0 || br.bad) return Result<ByteBuffer>(Status::Corrupt, "bad code-length symbol");
          if (sym < 16) { lengths[n++] = (uint8_t)sym; continue; }
          int rep = 0; uint8_t val = 0;
          if (sym == 16) { if (n == 0) return Result<ByteBuffer>(Status::Corrupt, "repeat with no previous length"); val = lengths[n - 1]; rep = 3 + (int)br.bits(2); }
          else if (sym == 17) { rep = 3 + (int)br.bits(3); }
          else { rep = 11 + (int)br.bits(7); }
          if (n + rep > hlit + hdist) return Result<ByteBuffer>(Status::Corrupt, "code lengths overflow");
          while (rep--) lengths[n++] = val;
        }
        huff_build(dyn_lit, lengths, hlit);
        huff_build(dyn_dist, lengths + hlit, hdist);
        lit = &dyn_lit; dist = &dyn_dist;
      }

      for (;;) {
        int sym = huff_decode(br, *lit);
        if (sym < 0 || br.bad) return Result<ByteBuffer>(Status::Corrupt, "bad literal/length symbol");
        if (sym < 256) { out.push_back((uint8_t)sym); continue; }
        if (sym == 256) break;
        sym -= 257;
        if (sym >= 29) return Result<ByteBuffer>(Status::Corrupt, "invalid length symbol");
        uint32_t len = kLenBase[sym] + br.bits(kLenExtra[sym]);
        int dsym = huff_decode(br, *dist);
        if (dsym < 0 || dsym >= 30) return Result<ByteBuffer>(Status::Corrupt, "invalid distance symbol");
        uint32_t d = kDistBase[dsym] + br.bits(kDistExtra[dsym]);
        if (d > out.size()) return Result<ByteBuffer>(Status::Corrupt, "distance before start of output");
        const size_t at = out.size(), from = at - d;
        out.resize(at + len);
        uint8_t* o = out.data();
        if (d >= len) std::memcpy(o + at, o + from, len);            // the two ranges are apart
        else for (uint32_t i = 0; i < len; ++i) o[at + i] = o[from + i];   // overlapping: the copy feeds itself
      }
    } else {
      return Result<ByteBuffer>(Status::Corrupt, "reserved deflate block type");
    }
    if (final_block) break;
  }
  if (br.bad) return Result<ByteBuffer>(Status::Corrupt, "truncated deflate stream");
  if (end_offset) *end_offset = offset + br.consumed();   // every byte the stream used, the last one whole
  return out;
}

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

namespace {

std::string hex20(const std::array<uint8_t, 20>& h) {
  // two digits per table load, written into a buffer the string is made from once
  static const auto pairs = [] {
    std::array<char, 512> t{};
    const char* d = "0123456789abcdef";
    for (int i = 0; i < 256; ++i) { t[2 * i] = d[i >> 4]; t[2 * i + 1] = d[i & 0xf]; }
    return t;
  }();
  char buf[40];
  for (int i = 0; i < 20; ++i) std::memcpy(buf + 2 * i, pairs.data() + 2 * h[i], 2);
  return std::string(buf, 40);
}

namespace {
// Each byte's value as a hex digit, or -1: one table load per digit (ids are converted for every index entry, tree
// entry and pack lookup).
struct HexTable {
  int8_t v[256];
  HexTable() {
    for (int i = 0; i < 256; ++i) v[i] = -1;
    for (int i = 0; i < 10; ++i) v['0' + i] = (int8_t)i;
    for (int i = 0; i < 6; ++i) { v['a' + i] = (int8_t)(10 + i); v['A' + i] = (int8_t)(10 + i); }
  }
};
const HexTable kHex;
}  // namespace

bool hex_to_20(const std::string& hex, uint8_t out[20]) {
  if (hex.size() != 40) return false;
  const unsigned char* p = (const unsigned char*)hex.data();
  int bad = 0;
  for (int i = 0; i < 20; ++i) {
    const int hi = kHex.v[p[2 * i]], lo = kHex.v[p[2 * i + 1]];
    bad |= hi | lo;
    out[i] = (uint8_t)((hi << 4) | (lo & 15));
  }
  return bad >= 0;
}

std::string raw_to_hex(const uint8_t* p) {
  std::array<uint8_t, 20> a{};
  std::memcpy(a.data(), p, 20);
  return hex20(a);
}

std::string u64_str(uint64_t v) {
  if (v == 0) return "0";
  char b[24];
  int i = 24;
  while (v) { b[--i] = (char)('0' + (v % 10)); v /= 10; }
  return std::string(b + i, (size_t)(24 - i));
}

std::string oct_mode(uint32_t mode) {
  if (mode == 0) return "0";
  char b[16];
  int i = 16;
  while (mode) { b[--i] = (char)('0' + (mode & 7)); mode >>= 3; }
  return std::string(b + i, (size_t)(16 - i));
}

uint64_t parse_u64_dec(const std::string& s, size_t from, size_t to) {
  uint64_t v = 0;
  for (size_t i = from; i < to && i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') break;
    v = v * 10 + (uint64_t)(s[i] - '0');
  }
  return v;
}

std::vector<std::string> split_lines(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == '\n') { out.push_back(cur); cur.clear(); }
    else cur.push_back(c);
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// Collapses "." segments and a trailing slash; the kernel names paths the way
// the caller typed them otherwise (no host cwd exists to resolve against).
std::string clean_path(const std::string& p) {
  const bool absolute = !p.empty() && p[0] == '/';
  std::string out;
  size_t i = 0;
  while (i < p.size()) {
    size_t j = p.find('/', i);
    std::string seg = p.substr(i, j == std::string::npos ? std::string::npos : j - i);
    if (!seg.empty() && seg != ".") { if (!out.empty()) out += "/"; out += seg; }
    if (j == std::string::npos) break;
    i = j + 1;
  }
  if (absolute) return "/" + out;              // one leading slash, never two
  return out.empty() ? "." : out;
}

void be32(ByteBuffer& b, uint32_t v) {
  b.push_back((uint8_t)(v >> 24)); b.push_back((uint8_t)(v >> 16));
  b.push_back((uint8_t)(v >> 8));  b.push_back((uint8_t)v);
}
uint32_t rd_be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

std::string join_path(const std::string& a, const std::string& b) {
  if (b.empty()) return a;
  if (a.empty() || a == ".") return b;
  return a + "/" + b;
}

}  // namespace

// ---------------------------------------------------------------------------
// Git
// ---------------------------------------------------------------------------

Git::Git(Kernel& kernel, Surface* actor) : kernel_(kernel), actor_(actor) {}

// The object database is storage. An entity reaches it only with the authority
// the kernel gave it; with no actor bound (an internal, kernel-side use) the
// question does not arise.
Result<void> Git::may(uint16_t rights) const {
  if (!actor_) return Ok();
  auto r = actor_->may(ResourceRef{ResourceKind::StorageObject, {}}, rights);
  if (!r.ok())
    return Result<void>(Status::Denied, "entity " + actor_->self().short_str() + " may not " +
                                            ((rights & R_Write) ? "write" : "read") +
                                            " the object database: " + r.error().str());
  return Ok();
}

void Git::load_alternates(const std::string& objects_dir, int depth) const {
  if (depth > 5) return;   // git stops following chains of alternates too
  auto& h = kernel_.host();
  auto raw = h.read_file(objects_dir + "/info/alternates");
  if (!raw.ok()) return;
  std::string text((const char*)raw->data(), raw->size());
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    // absolute, or relative to the objects directory that names it
    std::string dir = (line[0] == '/' || (line.size() > 1 && line[1] == ':')) ? line : clean_path(objects_dir + "/" + line);
    if (dir == objects_dir || dir == common_ + "/objects") continue;
    if (std::find(alt_dirs_.begin(), alt_dirs_.end(), dir) != alt_dirs_.end()) continue;
    alt_dirs_.push_back(dir);
    load_alternates(dir, depth + 1);
  }
}

const std::vector<std::string>& Git::alternate_object_dirs() const {
  if (!alts_loaded_) {
    alts_loaded_ = true;
    load_alternates(common_ + "/objects", 0);
  }
  return alt_dirs_;
}

Result<void> Git::add_alternate(const std::string& objects_dir) {
  if (auto r = may(R_Write); !r.ok()) return r;
  auto& h = kernel_.host();
  const std::string dir = clean_path(objects_dir);
  const std::string file = common_ + "/objects/info/alternates";
  std::string text;
  if (auto cur = h.read_file(file); cur.ok()) text.assign((const char*)cur->data(), cur->size());
  if (!text.empty() && text.back() != '\n') text += '\n';
  text += dir + "\n";
  if (auto r = h.make_dir(common_ + "/objects/info"); !r.ok() && !h.exists(common_ + "/objects/info")) return r;
  if (auto r = h.write_file(file, span_of(text)); !r.ok()) return r;
  alts_loaded_ = false;
  alt_dirs_.clear();
  packs_scanned_ = false;
  packs_.clear();
  return Ok();
}

void Git::end_bulk_write() const {
  if (bulk_depth_ > 0 && --bulk_depth_ == 0) {
    // The files this bulk write made, and the directories that name them:
    // syncing the whole filesystem (syncfs) waited on every other program's
    // dirty pages too - 28 ms for a push of three objects, 200 with a
    // database checkpoint in flight beside it.
    auto& h = kernel_.host();
    // Past a few dozen files (a pack unpacked into loose objects) one flush
    // of the filesystem is the cheaper of the two.
    constexpr size_t kSyncEach = 64;
    if (bulk_written_.size() > kSyncEach) {
      h.sync_tree(common_ + "/objects");
      bulk_written_.clear();
      return;
    }
    std::unordered_set<std::string> dirs;
    for (const auto& p : bulk_written_) {
      h.sync_file(p);
      if (size_t slash = p.rfind('/'); slash != std::string::npos) dirs.insert(p.substr(0, slash));
    }
    for (const auto& d : dirs) h.sync_file(d);
    if (!dirs.empty()) h.sync_file(common_ + "/objects");
    bulk_written_.clear();
  }
}

std::string Git::object_path(const std::string& oid) const {
  return common_ + "/objects/" + oid.substr(0, 2) + "/" + oid.substr(2);
}

std::string Git::ref_path(const std::string& ref) const {
  return (ref.rfind("refs/", 0) == 0 ? common_ : git_) + "/" + ref;
}

Result<void> Git::init(const std::string& dir, const std::string& branch, bool bare) {
  ensure_index();
  if (auto r = may(R_Write); !r.ok()) return r;
  auto& h = kernel_.host();
  work_ = clean_path(dir);
  git_ = bare ? work_ : work_ + "/.git";
  common_ = git_;
  bare_ = bare;
  if (h.exists(git_ + "/HEAD")) return bare ? open_dirs(git_, "") : open(work_);

  auto mk = [&](const std::string& p) -> Result<void> { return h.make_dir(p); };
  if (auto r = mk(work_); !r.ok()) return r;
  if (auto r = mk(git_); !r.ok()) return r;
  if (auto r = mk(common_ + "/objects"); !r.ok()) return r;
  if (auto r = mk(common_ + "/objects/info"); !r.ok()) return r;
  if (auto r = mk(common_ + "/objects/pack"); !r.ok()) return r;
  if (auto r = mk(git_ + "/refs"); !r.ok()) return r;
  if (auto r = mk(git_ + "/refs/heads"); !r.ok()) return r;
  if (auto r = mk(git_ + "/refs/tags"); !r.ok()) return r;

  head_ref_ = "refs/heads/" + branch;
  head_detached_.clear();
  index_.clear();

  std::string head = "ref: " + head_ref_ + "\n";
  if (auto r = h.write_file(git_ + "/HEAD", span_of(head)); !r.ok()) return r;
  // What git itself writes on this kind of machine: NTFS has no exec bit, no ordinary symlinks, and
  // does not tell File.txt from file.txt.
#ifdef _WIN32
  const char* host_core = "\tfilemode = false\n\tsymlinks = false\n\tignorecase = true\n";
#else
  const char* host_core = "\tfilemode = true\n";
#endif
  std::string cfg = std::string("[core]\n\trepositoryformatversion = 0\n") + host_core +
                    (bare ? "\tbare = true\n" : "\tbare = false\n\tlogallrefupdates = true\n");
  config_loaded_ = false;
  if (auto r = h.write_file(common_ + "/config", span_of(cfg)); !r.ok()) return r;
  std::string desc = "Unnamed repository; edit this file 'description' to name the repository.\n";
  if (auto r = h.write_file(git_ + "/description", span_of(desc)); !r.ok()) return r;
  if (auto r = mk(git_ + "/info"); !r.ok()) return r;
  // The empty directories git itself leaves (programs put hooks in hooks/ without making it first).
  if (auto r = mk(git_ + "/hooks"); !r.ok()) return r;
  if (auto r = mk(git_ + "/branches"); !r.ok()) return r;
  load_settings();
  return Ok();
}

Result<void> Git::open(const std::string& dir) {
  if (auto r = may(R_Read); !r.ok()) return r;
  auto& h = kernel_.host();
  std::string work = clean_path(dir);
  std::string dotgit = work + "/.git";
  auto st = h.stat(dotgit);
  if (st.ok() && !st->is_dir) {
    // A linked work tree (git worktree add) or a submodule: ".git" is a file naming the real git dir.
    auto f = h.read_file(dotgit);
    if (!f.ok()) return Result<void>(f.error());
    std::string s = trim(string_of(ByteSpan(f->data(), f->size())));
    if (s.rfind("gitdir:", 0) != 0) return Result<void>(Status::Corrupt, dotgit + ": not a gitdir file");
    std::string gd = trim(s.substr(7));
    const bool abs = (!gd.empty() && gd[0] == '/') || (gd.size() > 1 && gd[1] == ':');
    dotgit = abs ? clean_path(gd) : clean_path(work + "/" + gd);
  }
  return open_dirs(dotgit, work);
}

Result<void> Git::open_dirs(const std::string& git_dir, const std::string& work_tree) {
  if (auto r = may(R_Read); !r.ok()) return r;
  auto& h = kernel_.host();
  git_ = clean_path(git_dir);
  work_ = work_tree.empty() ? std::string() : clean_path(work_tree);
  bare_ = work_.empty();
  if (!h.exists(git_ + "/HEAD"))
    return Result<void>(Status::NotFound, git_ + ": not a git repository (no HEAD)");
  common_ = git_;
  if (auto c = h.read_file(git_ + "/commondir"); c.ok()) {
    std::string cd = trim(string_of(ByteSpan(c->data(), c->size())));
    const bool abs = (!cd.empty() && cd[0] == '/') || (cd.size() > 1 && cd[1] == ':');
    // clean_path does not fold "..": do it here, a component at a time.
    std::string joined = abs ? cd : git_ + "/" + cd;
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= joined.size()) {
      size_t j = joined.find('/', i);
      if (j == std::string::npos) j = joined.size();
      std::string seg = joined.substr(i, j - i);
      if (seg == "..") { if (!parts.empty() && !parts.back().empty()) parts.pop_back(); }
      else if (seg != "." && !(seg.empty() && !parts.empty())) parts.push_back(seg);
      i = j + 1;
    }
    common_.clear();
    for (size_t k = 0; k < parts.size(); ++k) common_ += (k ? "/" : "") + parts[k];
  }
  if (auto r = read_head(); !r.ok()) return r;
  load_settings();
  if (bare_ && index_file_.empty()) { index_.clear(); unmerged_.clear(); index_loaded_ = true; return Ok(); }
  index_loaded_ = false;   // read when first needed (ensure_index)
  return Ok();
}

Result<void> Git::read_head() {
  auto r = kernel_.host().read_file(git_ + "/HEAD");
  if (!r.ok()) return Result<void>(r.error());
  std::string s = trim(string_of(ByteSpan(r->data(), r->size())));
  if (s.rfind("ref:", 0) == 0) { head_ref_ = trim(s.substr(4)); head_detached_.clear(); }
  else { head_detached_ = s; head_ref_.clear(); }
  return Ok();
}

std::string Git::branch() const {
  if (!head_detached_.empty()) return "";
  const std::string pfx = "refs/heads/";
  return head_ref_.rfind(pfx, 0) == 0 ? head_ref_.substr(pfx.size()) : head_ref_;
}

Result<std::string> Git::ref_oid(const std::string& ref) const {
  auto r = kernel_.host().read_file(ref_path(ref));
  if (r.ok()) {
    std::string s = trim(string_of(ByteSpan(r->data(), r->size())));
    if (s.rfind("ref:", 0) == 0) return ref_oid(trim(s.substr(4)));   // a symbolic ref chain
    if (s.size() != 40) return Result<std::string>(Status::Corrupt, ref + ": not an object id");
    return s;
  }
  // A repository git has packed (clone, gc) keeps its refs in one file.
  if (const std::string* oid = packed_find(ref)) return *oid;
  return Result<std::string>(Status::NotFound, "no such ref: " + ref);
}

// The packed refs, each name once (the first line naming it wins), in file order: sorted by name when the file is
// (packed_sorted_), which is how git writes it. Looked up by binary search then; a hash map only for a file that isn't.
const std::vector<std::pair<std::string, std::string>>& Git::packed_refs() const {
  auto& h = kernel_.host();
  const std::string path = common_ + "/packed-refs";
  auto st = h.stat(path);
  if (!st.ok()) {
    packed_refs_.clear();
    packed_list_.clear();
    packed_sorted_ = true;
    packed_refs_loaded_ = true;
    packed_refs_mtime_ = 0;
    packed_refs_size_ = 0;
    return packed_list_;
  }
  if (packed_refs_loaded_ && st->mtime_ns == packed_refs_mtime_ && st->size == packed_refs_size_ && st->mtime_ns != 0) return packed_list_;
  packed_refs_.clear();
  packed_list_.clear();
  packed_sorted_ = true;
  packed_refs_loaded_ = true;
  packed_refs_mtime_ = st->mtime_ns;
  packed_refs_size_ = st->size;
  auto packed = h.read_file(path);
  if (!packed.ok()) return packed_list_;
  // One pass over the file: "<40 hex> <name>" lines, '#' headers and '^' peeled lines skipped.
  const char* p = (const char*)packed->data();
  const char* end = p + packed->size();
  packed_list_.reserve(packed->size() / 64 + 1);
  while (p < end) {
    const char* eol = (const char*)std::memchr(p, '\n', (size_t)(end - p));
    if (!eol) eol = end;
    const char* q = eol;
    if (q > p && q[-1] == '\r') --q;
    if (q - p > 41 && *p != '#' && *p != '^' && p[40] == ' ') {
      const char* n = p + 41;
      while (n < q && (*n == ' ' || *n == '\t')) ++n;
      const char* ne = q;
      while (ne > n && (ne[-1] == ' ' || ne[-1] == '\t')) --ne;
      std::string name(n, ne);
      if (packed_sorted_ && !packed_list_.empty() && !(packed_list_.back().first < name)) packed_sorted_ = false;
      packed_list_.emplace_back(std::move(name), std::string(p, 40));
    }
    p = eol + 1;
  }
  if (!packed_sorted_) {
    std::vector<std::pair<std::string, std::string>> once;
    once.reserve(packed_list_.size());
    for (auto& e : packed_list_)
      if (packed_refs_.emplace(e.first, e.second).second) once.push_back(std::move(e));
    packed_list_ = std::move(once);
  }
  return packed_list_;
}

const std::string* Git::packed_find(const std::string& ref) const {
  const auto& list = packed_refs();
  if (packed_sorted_) {
    auto it = std::lower_bound(list.begin(), list.end(), ref,
                               [](const std::pair<std::string, std::string>& e, const std::string& k) { return e.first < k; });
    return it != list.end() && it->first == ref ? &it->second : nullptr;
  }
  auto it = packed_refs_.find(ref);
  return it == packed_refs_.end() ? nullptr : &it->second;
}

Result<std::vector<GitRemoteRefPair>> Git::refs() const {
  std::vector<GitRemoteRefPair> out;
  auto& h = kernel_.host();
  // Walk .git/refs the way git does: every file under it names a ref.
  std::vector<std::string> stack{"refs"};
  while (!stack.empty()) {
    std::string rel = stack.back();
    stack.pop_back();
    auto names = h.list_dir_typed(common_ + "/" + rel);
    if (!names.ok()) continue;
    for (auto& e : *names) {
      const std::string& n = e.name;
      if (n == "." || n == "..") continue;
      std::string child = rel + "/" + n;
      if (e.kind == 'd') { stack.push_back(child); continue; }
      if (e.kind != 'f') {
        auto st = h.stat(common_ + "/" + child);
        if (st.ok() && st->is_dir) { stack.push_back(child); continue; }
      }
      auto oid = ref_oid(child);
      if (oid.ok()) out.push_back(GitRemoteRefPair{child, *oid});
    }
  }
  // Packed refs that no loose file overrides (a clone or `git gc` keeps most refs only there).
  auto by_name = [](const GitRemoteRefPair& a, const GitRemoteRefPair& b) { return a.name < b.name; };
  packed_refs();
  if (packed_list_.empty()) { std::sort(out.begin(), out.end(), by_name); return out; }
  if (!packed_sorted_) {
    std::unordered_set<std::string> loose;
    for (auto& r : out) loose.insert(r.name);
    for (const auto& [name, oid] : packed_list_)
      if (!loose.count(name)) out.push_back(GitRemoteRefPair{name, oid});
    std::sort(out.begin(), out.end(), by_name);
    return out;
  }
  // packed-refs is written sorted (git's "sorted" trait): merge the few loose refs into it, a loose one winning.
  std::sort(out.begin(), out.end(), by_name);
  std::vector<GitRemoteRefPair> merged;
  merged.reserve(out.size() + packed_list_.size());
  size_t i = 0;
  for (const auto& [name, oid] : packed_list_) {
    while (i < out.size() && out[i].name < name) merged.push_back(std::move(out[i++]));
    if (i < out.size() && out[i].name == name) { merged.push_back(std::move(out[i++])); continue; }
    merged.push_back(GitRemoteRefPair{name, oid});
  }
  while (i < out.size()) merged.push_back(std::move(out[i++]));
  return merged;
}

Result<std::string> Git::head_oid() const {
  if (!head_detached_.empty()) return head_detached_;
  auto r = ref_oid(head_ref_);
  if (!r.ok() && r.status() == Status::NotFound)
    return Result<std::string>(Status::NotFound, "unborn branch " + head_ref_);
  return r;
}

Result<std::string> Git::resolve(const std::string& rev) const {
  std::string s = trim(rev);
  if (s.empty() || s == "HEAD" || s == "@") return head_oid();
  if (s.rfind("refs/", 0) == 0) return ref_oid(s);
  if (auto r = ref_oid("refs/heads/" + s); r.ok()) return r;
  if (auto r = ref_oid("refs/tags/" + s); r.ok()) return r;
  auto o = read_object(s);
  if (!o.ok()) return Result<std::string>(o.error());
  return o->oid;
}

// The staging area IS git's index: a DIRC v2 file real git reads and writes.
// The stat fields this kernel cannot model (ctime, dev, ino, uid, gid) are
// written as zero, which git treats as "stat-dirty" and resolves by comparing
// content — the object id in the entry is the truth, and it is exact.
Result<void> Git::load_index() {
  index_loaded_ = true;
  index_.clear();
  unmerged_.clear();
  cache_tree_.clear();
  cache_tree_fp_ = 0;
  index_mtime_ns_ = 0;
  if (auto st = kernel_.host().stat(index_path()); st.ok()) index_mtime_ns_ = st->mtime_ns;
  auto r = kernel_.host().read_file(index_path());
  if (!r.ok()) {
    // No index file: in a repository written elsewhere the staged state is
    // simply what HEAD holds, which is what git itself would reconstruct.
    auto head = head_oid();
    if (head.ok()) {
      auto c = read_commit(*head);
      if (c.ok()) flatten_tree(c->tree, "", index_);
    }
    return Ok();
  }
  const ByteBuffer& b = *r;
  if (b.size() < 12 || std::memcmp(b.data(), "DIRC", 4) != 0)
    return Result<void>(Status::Corrupt, git_ + "/index: not a DIRC index");
  uint32_t version = rd_be32(b.data() + 4), count = rd_be32(b.data() + 8);
  if (version != 2 && version != 3)
    return Result<void>(Status::Unsupported, "git index version " + u64_str(version) + " is not one this kernel reads");
  size_t pos = 12;
  index_.reserve(std::min<size_t>(count, b.size() / 62));
  for (uint32_t i = 0; i < count; ++i) {
    size_t start = pos;
    if (pos + 62 > b.size()) return Result<void>(Status::Corrupt, "index entry runs past end of file");
    uint64_t mtime_ns = (uint64_t)rd_be32(b.data() + pos + 8) * 1000000000ull + rd_be32(b.data() + pos + 12);
    uint32_t mode = rd_be32(b.data() + pos + 24);
    uint64_t size = rd_be32(b.data() + pos + 36);
    std::string oid = raw_to_hex(b.data() + pos + 40);
    uint16_t flags = (uint16_t)(((uint16_t)b[pos + 60] << 8) | b[pos + 61]);
    pos += 62;
    uint16_t ext = 0;
    if (version >= 3 && (flags & 0x4000)) {             // extended flags
      if (pos + 2 > b.size()) return Result<void>(Status::Corrupt, "index entry runs past end of file");
      ext = (uint16_t)(((uint16_t)b[pos] << 8) | b[pos + 1]);
      pos += 2;
    }
    size_t namelen = flags & 0x0fff;
    std::string name;
    if (namelen < 0x0fff) {
      if (pos + namelen > b.size()) return Result<void>(Status::Corrupt, "index name runs past end of file");
      name.assign((const char*)b.data() + pos, namelen);
      pos += namelen;
    } else {
      size_t e = pos;
      while (e < b.size() && b[e]) ++e;
      name.assign((const char*)b.data() + pos, e - pos);
      pos = e;
    }
    pos = start + (((pos - start) + 8) & ~(size_t)7);    // NUL padded to a multiple of eight
    if (pos > b.size()) return Result<void>(Status::Corrupt, "index padding runs past end of file");
    GitIndexEntry e{std::move(name), std::move(oid), mode, size, mtime_ns};
    e.assume_valid = (flags & 0x8000) != 0;
    e.skip_worktree = (ext & 0x4000) != 0;
    e.intent_to_add = (ext & 0x2000) != 0;
    e.stage = (uint8_t)((flags >> 12) & 3);
    if (e.stage) unmerged_.push_back(std::move(e));      // an unresolved merge stage, not a staged file
    else index_.push_back(std::move(e));
  }
  // A split index keeps most entries in another file this reader does not follow: refuse rather than
  // report every path in it as deleted.
  if (b.size() >= pos + 8 + 20 && std::memcmp(b.data() + pos, "link", 4) == 0)
    return Result<void>(Status::Unsupported, git_ + "/index is a split index (core.splitIndex), which this kernel does not read");
  // git's cache-tree (the TREE extension): the tree id of every directory git has not touched since it last wrote
  // one, so the index's root tree need not be hashed again from all its entries. This kernel writes no extensions:
  // one in the file is git's, which invalidates a directory whenever an entry under it changes.
  while (pos + 8 <= b.size() - std::min<size_t>(b.size(), 20)) {
    const uint8_t* sig = b.data() + pos;
    const size_t len = rd_be32(b.data() + pos + 4);
    if (pos + 8 + len > b.size()) break;
    if (std::memcmp(sig, "TREE", 4) == 0) {
      const uint8_t* q = b.data() + pos + 8;
      const uint8_t* qe = q + len;
      // pre-order: "<name>\0<entries> <subtrees>\n" and the 20-byte id when <entries> is not -1
      std::function<bool(const std::string&)> node = [&](const std::string& parent) -> bool {
        const uint8_t* nul = (const uint8_t*)std::memchr(q, 0, (size_t)(qe - q));
        if (!nul) return false;
        std::string name((const char*)q, (size_t)(nul - q));
        q = nul + 1;
        const uint8_t* nl = (const uint8_t*)std::memchr(q, '\n', (size_t)(qe - q));
        if (!nl) return false;
        std::string counts((const char*)q, (size_t)(nl - q));
        q = nl + 1;
        const size_t sp = counts.find(' ');
        if (sp == std::string::npos) return false;
        const long count = std::strtol(counts.c_str(), nullptr, 10);
        const long subs = std::strtol(counts.c_str() + sp + 1, nullptr, 10);
        const std::string path = parent.empty() ? name : parent + "/" + name;
        if (count >= 0) {
          if (qe - q < 20) return false;
          cache_tree_[path] = {count, raw_to_hex(q)};
          q += 20;
        }
        for (long i = 0; i < subs; ++i) if (!node(path)) return false;
        return true;
      };
      if (!node("")) cache_tree_.clear();
    }
    pos += 8 + len;
  }
  if (!cache_tree_.empty()) cache_tree_fp_ = index_fingerprint(index_);
  return Ok();
}

// What the cache-tree was read against: every staged entry's path, id and mode (an entry changed in memory since,
// by any route, makes the cache unusable rather than wrong).
uint64_t Git::index_fingerprint(const std::vector<GitIndexEntry>& entries) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](uint64_t v) { h = (h ^ v) * 0x9e3779b97f4a7c15ull; h ^= h >> 29; };
  auto bytes = [&](const std::string& t) {   // eight bytes at a time, the tail zero-padded
    size_t i = 0;
    for (; i + 8 <= t.size(); i += 8) { uint64_t w; std::memcpy(&w, t.data() + i, 8); mix(w); }
    if (i < t.size()) { uint64_t w = 0; std::memcpy(&w, t.data() + i, t.size() - i); mix(w); }
  };
  for (const auto& e : entries) {
    bytes(e.path);
    bytes(e.oid);
    mix(e.mode | (e.intent_to_add ? 0x80000000ull : 0) | ((uint64_t)e.path.size() << 32));
  }
  return h ^ entries.size();
}

void Git::set_index(std::vector<GitIndexEntry> entries) {
  ensure_index();
  index_ = std::move(entries);
  for (auto& e : index_) e.stage = 0;
}

void Git::set_unmerged(std::vector<GitIndexEntry> entries) { unmerged_ = std::move(entries); }

Result<void> Git::write_index() { return save_index(); }

Result<void> Git::save_index() {
  ensure_index();
  // A path is either staged (stage 0) or unmerged (stages 1-3), never both.
  if (!unmerged_.empty()) {
    std::set<std::string> conflicted;
    for (auto& u : unmerged_) conflicted.insert(u.path);
    index_.erase(std::remove_if(index_.begin(), index_.end(), [&](const GitIndexEntry& e) { return conflicted.count(e.path) > 0; }),
                 index_.end());
  }
  std::sort(index_.begin(), index_.end(), [](const GitIndexEntry& a, const GitIndexEntry& b) { return a.path < b.path; });
  std::vector<const GitIndexEntry*> all;
  for (auto& e : index_) all.push_back(&e);
  for (auto& e : unmerged_) all.push_back(&e);
  std::sort(all.begin(), all.end(), [](const GitIndexEntry* a, const GitIndexEntry* b) {
    return a->path != b->path ? a->path < b->path : a->stage < b->stage;
  });
  bool extended = false;
  for (auto* e : all) if (e->skip_worktree || e->intent_to_add) extended = true;
  ByteBuffer b;
  const char* magic = "DIRC";
  b.insert(b.end(), magic, magic + 4);
  be32(b, extended ? 3 : 2);
  be32(b, (uint32_t)all.size());
  for (auto* ep : all) {
    auto& e = *ep;
    size_t start = b.size();
    // What git records: the entry's own size when it knows it (a symlink's
    // size is the length of its target, not of the file it points at), and
    // otherwise what the work tree says.
    uint64_t size = e.size;
    if (size == 0 && e.stage == 0) {
      auto st = kernel_.host().stat(join_path(work_, e.path));
      if (st.ok()) size = st->size;
    }
    // ctime, dev, ino, uid and gid are not modelled and stay zero: git treats such an entry as
    // stat-dirty and compares content. mtime is kept, for this reader's own stat cache.
    be32(b, 0); be32(b, 0);            // ctime seconds, nanoseconds
    be32(b, (uint32_t)(e.mtime_ns / 1000000000ull)); be32(b, (uint32_t)(e.mtime_ns % 1000000000ull));
    be32(b, 0); be32(b, 0);            // dev, ino
    be32(b, e.mode);
    be32(b, 0); be32(b, 0);            // uid, gid
    be32(b, (uint32_t)size);
    uint8_t raw[20];
    if (!hex_to_20(e.oid, raw)) return Result<void>(Status::Corrupt, "index holds a malformed id for " + e.path);
    b.insert(b.end(), raw, raw + 20);
    uint16_t flags = (uint16_t)(e.path.size() < 0x0fff ? e.path.size() : 0x0fff);
    flags |= (uint16_t)((e.stage & 3) << 12);
    if (e.assume_valid) flags |= 0x8000;
    const uint16_t ext = (uint16_t)((e.skip_worktree ? 0x4000 : 0) | (e.intent_to_add ? 0x2000 : 0));
    if (ext) flags |= 0x4000;
    b.push_back((uint8_t)(flags >> 8));
    b.push_back((uint8_t)flags);
    if (ext) { b.push_back((uint8_t)(ext >> 8)); b.push_back((uint8_t)ext); }
    b.insert(b.end(), e.path.begin(), e.path.end());
    do { b.push_back(0); } while ((b.size() - start) % 8 != 0);
  }
  auto sum = crypto::sha1(ByteSpan(b.data(), b.size()));
  b.insert(b.end(), sum.begin(), sum.end());
  // hal's write_file writes beside and renames over: a reader never sees half an index.
  // synced only when core.fsync names the index (index, added, all): git's default leaves it to the page cache
  auto& wh = kernel_.host();
  if (auto r = settings_.fsync_index ? wh.write_file(index_path(), ByteSpan(b.data(), b.size()))
                                     : wh.write_file_unsynced(index_path(), ByteSpan(b.data(), b.size()));
      !r.ok())
    return r;
  if (auto st = kernel_.host().stat(index_path()); st.ok()) index_mtime_ns_ = st->mtime_ns;
  return Ok();
}

Result<std::string> Git::write_object(const std::string& type, ByteSpan payload) {
  ByteBuffer serialized;
  std::string header = type + " " + u64_str(payload.size());
  serialized.reserve(header.size() + 1 + payload.size());
  serialized.insert(serialized.end(), header.begin(), header.end());
  serialized.push_back(0);
  serialized.insert(serialized.end(), payload.begin(), payload.end());

  std::string oid = hex20(crypto::sha1(ByteSpan(serialized.data(), serialized.size())));
  if (mem_.count(oid)) return oid;
  auto& h = kernel_.host();
  std::string path = object_path(oid);
  if (h.exists(path)) return oid;   // content addressed: identical bytes, identical file
  // Already stored elsewhere (a pack, or a repository this one borrows from): compressing and writing it again only made
  // a duplicate loose file (every push and unpack of objects the store already had).
  {
    uint8_t want[20];
    if (hex_to_20(oid, want) && in_some_pack(want)) return oid;
    for (const auto& alt : alternate_object_dirs())
      if (h.exists(alt + "/" + oid.substr(0, 2) + "/" + oid.substr(2))) return oid;
  }
  if (memory_writes_) {
    mem_bytes_ += payload.size();
    mem_.emplace(oid, std::make_pair(type, ByteBuffer(payload.begin(), payload.end())));
    return oid;
  }
  const std::string fanout = common_ + "/objects/" + oid.substr(0, 2);
  if (!fanout_made_.count(fanout)) {   // one mkdir per fan-out directory, not one walk of the path per object
    if (auto r = h.make_dir(fanout); !r.ok()) return Result<std::string>(r.error());
    fanout_made_.insert(fanout);
  }
  ByteBuffer z = zlib_deflate(ByteSpan(serialized.data(), serialized.size()));
  auto w = bulk_depth_ > 0 ? h.write_file_unsynced(path, ByteSpan(z.data(), z.size())) : h.write_file(path, ByteSpan(z.data(), z.size()));
  if (!w.ok()) return Result<std::string>(w.error());
  if (bulk_depth_ > 0) bulk_written_.push_back(path);
  return oid;
}

// The loose file of `oid` in `from`, placed as this repository's loose file
// of it; false when `from` has no loose file for it (a pack, memory) or the
// copy fails, and the caller rewrites the object the long way.
bool Git::copy_loose_object_from(const Git& from, const std::string& oid) {
  if (oid.size() != 40 || memory_writes_) return false;
  auto& h = kernel_.host();
  const std::string src = from.object_path(oid);
  auto bytes = h.read_file(src);
  if (!bytes.ok() || bytes->empty()) return false;
  const std::string fanout = common_ + "/objects/" + oid.substr(0, 2);
  if (!fanout_made_.count(fanout)) {
    if (auto r = h.make_dir(fanout); !r.ok()) return false;
    fanout_made_.insert(fanout);
  }
  const std::string path = object_path(oid);
  auto w = bulk_depth_ > 0 ? h.write_file_unsynced(path, ByteSpan(bytes->data(), bytes->size()))
                           : h.write_file(path, ByteSpan(bytes->data(), bytes->size()));
  if (w.ok() && bulk_depth_ > 0) bulk_written_.push_back(path);
  return w.ok();
}

std::vector<std::string> Git::memory_children(const std::string& oid) const {
  std::vector<std::string> out;
  auto it = mem_.find(oid);
  if (it == mem_.end()) return out;
  const auto& [type, p] = it->second;
  if (type == "commit") {
    // header lines up to the blank line: "tree X", "parent Y"
    size_t i = 0;
    while (i < p.size()) {
      size_t nl = i;
      while (nl < p.size() && p[nl] != '\n') ++nl;
      if (nl == i) break;
      std::string line((const char*)p.data() + i, nl - i);
      if (line.rfind("tree ", 0) == 0 || line.rfind("parent ", 0) == 0) {
        std::string id = line.substr(line.find(' ') + 1);
        if (mem_.count(id)) out.push_back(id);
      }
      i = nl + 1;
    }
  } else if (type == "tree") {
    size_t i = 0;
    while (i < p.size()) {
      size_t sp = i;
      while (sp < p.size() && p[sp] != ' ') ++sp;
      size_t nul = sp;
      while (nul < p.size() && p[nul] != 0) ++nul;
      if (nul + 21 > p.size()) break;
      const bool gitlink = std::string((const char*)p.data() + i, sp - i) == "160000";
      std::array<uint8_t, 20> raw{};
      std::copy(p.data() + nul + 1, p.data() + nul + 21, raw.begin());
      std::string id = hex20(raw);
      if (!gitlink && mem_.count(id)) out.push_back(id);
      i = nul + 21;
    }
  } else if (type == "tag") {
    std::string text((const char*)p.data(), p.size());
    if (text.rfind("object ", 0) == 0) {
      std::string id = text.substr(7, 40);
      if (mem_.count(id)) out.push_back(id);
    }
  }
  return out;
}

Result<size_t> Git::persist_memory(const std::string& root) {
  size_t written = 0;
  std::vector<std::string> todo{root};
  const bool was = memory_writes_;
  memory_writes_ = false;
  while (!todo.empty()) {
    std::string id = todo.back();
    todo.pop_back();
    auto it = mem_.find(id);
    if (it == mem_.end()) continue;
    for (auto& child : memory_children(id)) todo.push_back(child);
    auto node = mem_.extract(it);
    mem_bytes_ -= node.mapped().second.size();
    auto w = write_object(node.mapped().first, ByteSpan(node.mapped().second.data(), node.mapped().second.size()));
    if (!w.ok()) { memory_writes_ = was; mem_.insert(std::move(node)); return Result<size_t>(w.error()); }
    ++written;
  }
  memory_writes_ = was;
  return written;
}

void Git::drop_memory(const std::vector<std::string>& keep) {
  if (mem_.empty()) return;
  std::unordered_set<std::string> live;
  std::vector<std::string> todo(keep.begin(), keep.end());
  while (!todo.empty()) {
    std::string id = todo.back();
    todo.pop_back();
    if (!mem_.count(id) || !live.insert(id).second) continue;
    for (auto& child : memory_children(id)) todo.push_back(child);
  }
  for (auto it = mem_.begin(); it != mem_.end();) {
    if (live.count(it->first)) { ++it; continue; }
    mem_bytes_ -= it->second.second.size();
    it = mem_.erase(it);
  }
}

Result<GitObject> Git::read_object(const std::string& id_or_prefix) const {
  auto r = read_object_here(id_or_prefix);
  if (r.ok() || r.status() != Status::NotFound || id_or_prefix.size() != 40 || !missing_hook_ || in_missing_hook_) return r;
  // a partial clone: the object is the promisor remote's to give
  in_missing_hook_ = true;
  bool got = missing_hook_({id_or_prefix});
  in_missing_hook_ = false;
  if (!got) return r;
  rescan_packs();
  return read_object_here(id_or_prefix);
}

bool Git::has_object(const std::string& oid) const {
  if (oid.size() != 40) return false;
  if (mem_.count(oid)) return true;
  uint8_t raw[20];
  for (int i = 0; i < 20; ++i) {
    auto nib = [](char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1; };
    int hi = nib(oid[2 * i]), lo = nib(oid[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    raw[i] = (uint8_t)(hi << 4 | lo);
  }
  // The mapped pack indexes first (a binary search, no system call), then the loose paths.
  if (in_some_pack(raw)) return true;
  auto& h = kernel_.host();
  if (h.exists(object_path(oid))) return true;
  for (const auto& alt : alternate_object_dirs())
    if (h.exists(alt + "/" + oid.substr(0, 2) + "/" + oid.substr(2))) return true;
  load_blocks();
  uint32_t idx = 0;
  for (auto& b : blocks_) if (b->find(oid, idx)) return true;
  return false;
}

void Git::load_blocks() const {
  if (blocks_scanned_) return;
  blocks_scanned_ = true;
  blocks_.clear();
  auto& h = kernel_.host();
  auto names = h.list_dir(common_ + "/objects/blocks");
  if (!names.ok()) return;
  std::vector<std::string> sorted = *names;
  std::sort(sorted.begin(), sorted.end());
  for (auto& n : sorted) {
    if (n.size() < 5 || n.compare(n.size() - 4, 4, ".blk") != 0) continue;
    auto b = GitBlockFile::open(h, common_ + "/objects/blocks/" + n);
    if (b.ok()) blocks_.push_back(*b);
  }
}

std::vector<std::string> Git::local_block_files() const {
  load_blocks();
  std::vector<std::string> out;
  for (auto& b : blocks_) out.push_back(b->path());
  return out;
}

std::vector<std::string> Git::local_block_ids() const {
  load_blocks();
  std::vector<std::string> out;
  for (auto& b : blocks_) for (uint32_t i = 0; i < b->count(); ++i) out.push_back(b->oid_at(i));
  return out;
}

namespace {
bool scan_commit(const uint8_t* d, size_t n, GitCommit& c, bool meta_only);
bool idx_offset_at(const uint8_t* idx, size_t idx_size, uint32_t version, uint32_t count, uint32_t i, uint64_t& off);
}  // namespace

bool Git::commit_meta(const std::string& oid, std::string& tree, std::vector<std::string>& parents, uint64_t& commit_time) const {
  load_blocks();
  uint32_t idx = 0;
  for (auto& b : blocks_) if (b->find(oid, idx)) return b->commit_meta(idx, tree, parents, commit_time);
  // In a pack, stored whole (as commits nearly always are): only the head of the stream is inflated, up to the end of
  // the committer line. git inflates every commit in full, message and signature, to read its first four lines.
  GitCommit c;
  if (packed_commit_head(oid, c)) {
    tree = std::move(c.tree);
    parents = std::move(c.parents);
    commit_time = c.committer_time;
    return true;
  }
  // Anywhere else: the header lines of the object, nothing of the author or the message kept.
  auto o = read_object(oid);
  if (!o.ok() || o->type != "commit") return false;
  scan_commit(o->payload.data(), o->payload.size(), c, true);
  if (c.tree.empty()) return false;
  tree = std::move(c.tree);
  parents = std::move(c.parents);
  commit_time = c.committer_time;
  return true;
}

bool Git::packed_commit_head(const std::string& oid, GitCommit& c) const {
#if defined(AIOS_HAVE_ZLIB)
  uint8_t want[20];
  if (!mem_.empty() || oid.size() != 40 || !hex_to_20(oid, want)) return false;
  load_pack_indexes();
  for (auto& pf : packs_) {
    const int64_t found = pack_find(pf, want);
    if (found < 0) continue;
    uint64_t at = 0;
    if (!idx_offset_at(pf.idx, pf.idx_size, pf.idx_version, pf.count, (uint32_t)found, at)) return false;
    if (!map_pack(pf).ok()) return false;
    const ByteSpan pack(pf.pack_data(), pf.pack_size() - 20);
    size_t p = (size_t)at;
    if (p >= pack.size()) return false;
    uint8_t b = pack[p++];
    if (((b >> 4) & 7) != 1) return false;   // a delta, or not a commit: the full read
    uint64_t size = b & 15;
    int shift = 4;
    while ((b & 0x80) && p < pack.size()) { b = pack[p++]; size |= (uint64_t)(b & 0x7f) << shift; shift += 7; }
    uint8_t head[1024];
    size_t got = 0;
    thread_local ByteBuffer whole;
    const uint8_t* text = head;
    // libdeflate inflates the whole commit faster than zlib inflates its head; zlib stops at 1 KB
    if (size <= (64u << 10)) {
      whole.resize((size_t)size);
      if (net::zlib_inflate_known(pack.subspan(p), whole.data(), (size_t)size, nullptr, &got)) text = whole.data();
      else got = 0;
    }
    if (!got) {
      const size_t want_n = (size_t)std::min<uint64_t>(size, sizeof head);
      if (!net::zlib_inflate_prefix(pack.subspan(p), head, want_n, &got) || got == 0) return false;
      text = head;
    }
    // the committer line must be whole in what was inflated
    const std::string_view h((const char*)text, got);
    const size_t cl = h.find("\ncommitter ");
    if (cl == std::string_view::npos || h.find('\n', cl + 1) == std::string_view::npos) return false;
    if (h.find("\n\n") != std::string_view::npos && h.find("\n\n") < cl) return false;   // "committer" in the message
    scan_commit(text, got, c, true);
    return !c.tree.empty();
  }
#else
  (void)oid; (void)c;
#endif
  return false;
}

std::optional<GitSyntaxOutline> Git::outline(const std::string& oid, const std::string& path) const {
  std::string lang = git_structural_language(path);
  if (lang.empty()) return std::nullopt;
  std::string key = lang + " " + oid;
  if (auto it = outlines_.find(key); it != outlines_.end()) { ++outline_counts_.remembered; return it->second; }
  std::optional<GitSyntaxOutline> got;
  load_blocks();
  uint32_t idx = 0;
  for (auto& b : blocks_) {
    if (!b->find(oid, idx)) continue;
    if (auto o = b->outline(idx); o && o->language == lang) { got = std::move(o); ++outline_counts_.from_blocks; }
    break;
  }
  if (!got) {
    auto o = read_object(oid);
    if (o.ok() && o->type == "blob") {
      got = git_structural_outline(path, std::string_view((const char*)o->payload.data(), o->payload.size()));
      ++outline_counts_.parsed;
    }
  }
  if (outlines_.size() > 4096) outlines_.clear();
  outlines_[key] = got;
  return got;
}

void Git::prefetch(const std::vector<std::string>& oids) const {
  if (!missing_hook_ || in_missing_hook_) return;
  std::vector<std::string> missing;
  for (auto& o : oids) if (!has_object(o)) missing.push_back(o);
  if (missing.empty()) return;
  in_missing_hook_ = true;
  bool got = missing_hook_(missing);
  in_missing_hook_ = false;
  if (got) rescan_packs();
}

Result<GitObject> Git::read_object_here(const std::string& id_or_prefix) const {
  std::string oid;
  auto& h = kernel_.host();
  if (!mem_.empty() && id_or_prefix.size() == 40) {
    if (auto it = mem_.find(id_or_prefix); it != mem_.end()) {
      GitObject o;
      o.oid = it->first;
      o.type = it->second.first;
      o.payload = it->second.second;
      return o;
    }
  }
  if (id_or_prefix.size() == 40) {
    oid = id_or_prefix;
  } else {
    if (id_or_prefix.size() < 4) return Result<GitObject>(Status::Invalid, "object id prefix needs at least 4 characters");
    std::string dir = id_or_prefix.substr(0, 2), rest = id_or_prefix.substr(2);
    auto names = h.list_dir(common_ + "/objects/" + dir);
    std::vector<std::string> hits;
    if (names.ok())
      for (auto& n : *names) if (n.rfind(rest, 0) == 0) hits.push_back(dir + n);
    for (const auto& alt : alternate_object_dirs())
      if (auto an = h.list_dir(alt + "/" + dir); an.ok())
        for (auto& n : *an) if (n.rfind(rest, 0) == 0) hits.push_back(dir + n);
    // ...and in the packs, where a gc'd repository keeps everything.
    std::string lower = id_or_prefix;
    for (char& ch : lower) if (ch >= 'A' && ch <= 'F') ch = (char)(ch - 'A' + 'a');
    packed_ids_with_prefix(lower, hits);
    for (const auto& [id, _] : mem_) if (id.rfind(lower, 0) == 0) hits.push_back(id);
    std::sort(hits.begin(), hits.end());
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
    if (hits.empty()) return Result<GitObject>(Status::NotFound, "no object matches " + id_or_prefix);
    if (hits.size() > 1) return Result<GitObject>(Status::Conflict, "object id prefix " + id_or_prefix + " is ambiguous");
    oid = hits[0];
  }

  // The packs first (one binary search in a mapped index), then the loose file: a cloned or gc'd repository keeps
  // nearly everything packed, and every read paid a failed open() of the loose path before finding it there.
  {
    uint8_t want[20];
    if (hex_to_20(oid, want)) {
      auto packed = read_packed_raw(want, oid, 0);
      if (packed.ok() || packed.status() != Status::NotFound) return packed;
    }
  }
  auto raw = h.read_file(object_path(oid));
  if (!raw.ok())
    for (const auto& alt : alternate_object_dirs()) {
      raw = h.read_file(alt + "/" + oid.substr(0, 2) + "/" + oid.substr(2));
      if (raw.ok()) break;
    }
  if (!raw.ok()) {
    // Not loose: after `git gc` (or any clone made by real git) objects live
    // in packfiles. This used to stop here, so a gc'd repository could not
    // even show its log.
    auto packed = read_packed(oid);
    if (packed.ok() || packed.status() != Status::NotFound) return packed;
    load_blocks();
    for (auto& b : blocks_) {
      uint32_t idx = 0;
      if (b->find(oid, idx)) return b->read(idx);
    }
    return Result<GitObject>(Status::NotFound, "object " + oid + " is not in this database");
  }
  auto inflated = zlib_inflate(ByteSpan(raw->data(), raw->size()));
  if (!inflated.ok()) return Result<GitObject>(inflated.error());

  const ByteBuffer& b = *inflated;
  size_t nul = 0;
  while (nul < b.size() && b[nul] != 0) ++nul;
  if (nul >= b.size()) return Result<GitObject>(Status::Corrupt, "object " + oid + " has no header terminator");
  std::string header((const char*)b.data(), nul);
  size_t sp = header.find(' ');
  if (sp == std::string::npos) return Result<GitObject>(Status::Corrupt, "object " + oid + " has a malformed header");

  GitObject o;
  o.oid = oid;
  o.type = header.substr(0, sp);
  uint64_t declared = parse_u64_dec(header, sp + 1, header.size());
  o.payload.assign(b.begin() + (long)nul + 1, b.end());
  if (o.payload.size() != declared)
    return Result<GitObject>(Status::Corrupt, "object " + oid + ": header says " + u64_str(declared) + " bytes, payload is " + u64_str(o.payload.size()));
  return o;
}

// ---------------------------------------------------------------------------
// packfiles: .idx lookup, then the object (and its delta chain) out of .pack
// ---------------------------------------------------------------------------
namespace {
const char* pack_type_name(int t) {
  switch (t) { case 1: return "commit"; case 2: return "tree"; case 3: return "blob"; case 4: return "tag"; default: return ""; }
}
uint64_t delta_varint(ByteSpan d, size_t& p, bool& bad) {
  uint64_t v = 0;
  int shift = 0;
  for (;;) {
    if (p >= d.size()) { bad = true; return 0; }
    uint8_t c = d[p++];
    v |= (uint64_t)(c & 0x7f) << shift;
    if (!(c & 0x80)) break;
    shift += 7;
    if (shift > 63) { bad = true; return 0; }
  }
  return v;
}
}  // namespace

Result<ByteBuffer> git_apply_delta(ByteSpan base, ByteSpan delta) {
  size_t p = 0;
  bool bad = false;
  uint64_t base_size = delta_varint(delta, p, bad);
  uint64_t result_size = delta_varint(delta, p, bad);
  if (bad) return Result<ByteBuffer>(Status::Corrupt, "truncated delta header");
  if (base_size != base.size())
    return Result<ByteBuffer>(Status::Corrupt,
                             "delta expects a " + u64_str(base_size) + "-byte base, the object is " + u64_str(base.size()));
  ByteBuffer out;
  out.reserve(result_size);
  while (p < delta.size()) {
    uint8_t op = delta[p++];
    if (op & 0x80) {
      uint64_t off = 0, len = 0;
      for (int i = 0; i < 4; ++i)
        if (op & (1 << i)) { if (p >= delta.size()) return Result<ByteBuffer>(Status::Corrupt, "truncated copy offset"); off |= (uint64_t)delta[p++] << (8 * i); }
      for (int i = 0; i < 3; ++i)
        if (op & (0x10 << i)) { if (p >= delta.size()) return Result<ByteBuffer>(Status::Corrupt, "truncated copy size"); len |= (uint64_t)delta[p++] << (8 * i); }
      if (len == 0) len = 0x10000;
      if (off + len > base.size()) return Result<ByteBuffer>(Status::Corrupt, "delta copies past the end of its base");
      out.insert(out.end(), base.begin() + (long)off, base.begin() + (long)(off + len));
    } else {
      if (op == 0) return Result<ByteBuffer>(Status::Corrupt, "delta instruction 0 is reserved");
      if (p + op > delta.size()) return Result<ByteBuffer>(Status::Corrupt, "truncated delta insert");
      out.insert(out.end(), delta.begin() + (long)p, delta.begin() + (long)(p + op));
      p += op;
    }
  }
  if (out.size() != result_size)
    return Result<ByteBuffer>(Status::Corrupt,
                             "delta produced " + u64_str(out.size()) + " bytes, its header says " + u64_str(result_size));
  return out;
}

void Git::load_pack_indexes() const {
  if (packs_scanned_) return;
  packs_scanned_ = true;
  std::vector<std::string> dirs = {common_ + "/objects/pack"};
  for (const auto& alt : alternate_object_dirs()) dirs.push_back(alt + "/pack");
  auto& h = kernel_.host();
  for (const auto& dir : dirs) {
    auto names = h.list_dir(dir);
    if (!names.ok()) continue;
    for (auto& n : *names) {
      if (n.size() < 5 || n.compare(n.size() - 4, 4, ".idx") != 0) continue;
      auto idx = h.view_file(dir + "/" + n);
      if (!idx.ok() || (*idx)->size < 8 + 256 * 4) continue;
      PackFile pf;
      pf.pack_path = dir + "/" + n.substr(0, n.size() - 4) + ".pack";
      pf.idx_view = std::move(*idx);
      pf.idx = pf.idx_view->data;
      pf.idx_size = pf.idx_view->size;
      const uint8_t* d = pf.idx;
      size_t fan;
      if (d[0] == 0xff && d[1] == 't' && d[2] == 'O' && d[3] == 'c') {
        pf.idx_version = rd_be32(d + 4);
        if (pf.idx_version != 2) continue;
        fan = 8;
      } else {
        pf.idx_version = 1;
        fan = 0;
      }
      pf.count = rd_be32(d + fan + 255 * 4);
      const size_t need = pf.idx_version == 2 ? 8 + 1024 + (size_t)pf.count * 28 : 1024 + (size_t)pf.count * 24;
      if (pf.idx_size < need) continue;
      packs_.push_back(std::move(pf));
    }
  }
}

namespace {
// The idx's sorted id table: where the id of entry i starts.
const uint8_t* idx_oid_at(const uint8_t* idx, uint32_t version, uint32_t i) {
  return version == 2 ? idx + 8 + 1024 + (size_t)i * 20 : idx + 1024 + (size_t)i * 24 + 4;
}
}  // namespace

namespace {
// Hex digits two raw ids share from the start.
int common_hex(const uint8_t* x, const uint8_t* y) {
  for (int i = 0; i < 20; ++i) {
    if (x[i] == y[i]) continue;
    return 2 * i + ((x[i] >> 4) == (y[i] >> 4) ? 1 : 0);
  }
  return 40;
}
int hex_val(char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; }
}  // namespace

void Git::packed_ids_with_prefix(const std::string& prefix, std::vector<std::string>& out) const {
  load_pack_indexes();
  // The prefix as raw bytes, zero-filled: the first id it could name. A binary search finds it in each pack's sorted
  // ids, and the ids that follow are walked while they still start with the prefix (every id was hex-converted for
  // each lookup: a 107k-object pack per abbreviated id).
  uint8_t lo_key[20] = {0};
  bool ok = prefix.size() <= 40;
  for (size_t i = 0; ok && i < prefix.size(); ++i) {
    const int v = hex_val(prefix[i]);
    if (v < 0) { ok = false; break; }
    lo_key[i / 2] |= (uint8_t)(i % 2 ? v : v << 4);
  }
  for (auto& pf : packs_) {
    if (!ok) break;
    uint32_t lo = 0, hi = pf.count;
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      if (std::memcmp(idx_oid_at(pf.idx, pf.idx_version, mid), lo_key, 20) < 0) lo = mid + 1; else hi = mid;
    }
    for (uint32_t i = lo; i < pf.count; ++i) {
      std::string hex = raw_to_hex(idx_oid_at(pf.idx, pf.idx_version, i));
      if (hex.compare(0, prefix.size(), prefix) != 0) break;
      out.push_back(std::move(hex));
    }
  }
  load_blocks();
  for (auto& b : blocks_) b->ids_with_prefix(prefix, out);
}

namespace {
// Entry i's offset in its pack, from the .idx (v1 or v2, with v2's table of offsets past 2 GB). False when the
// index is damaged.
bool idx_offset_at(const uint8_t* idx, size_t idx_size, uint32_t version, uint32_t count, uint32_t i, uint64_t& off) {
  const uint8_t* d = idx;
  if (version != 2) { off = rd_be32(d + 1024 + (size_t)i * 24); return true; }
  const size_t off_tab = 8 + 1024 + (size_t)count * 24;
  uint32_t o32 = rd_be32(d + off_tab + (size_t)i * 4);
  if (!(o32 & 0x80000000u)) { off = o32; return true; }
  const size_t big = off_tab + (size_t)count * 4 + (size_t)(o32 & 0x7fffffffu) * 8;
  if (big + 8 > idx_size) return false;
  off = ((uint64_t)rd_be32(d + big) << 32) | rd_be32(d + big + 4);
  return true;
}
}  // namespace

std::vector<std::string> Git::local_loose_ids() const {
  std::vector<std::string> out;
  auto& h = kernel_.host();
  auto fanout = h.list_dir(common_ + "/objects");
  if (!fanout.ok()) return out;
  auto hex = [](const std::string& s) {
    for (char ch : s) if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    return true;
  };
  for (auto& d : *fanout) {
    if (d.size() != 2 || !hex(d)) continue;   // info/ and pack/ are not fan-out directories
    auto names = h.list_dir(common_ + "/objects/" + d);
    if (!names.ok()) continue;
    for (auto& n : *names) if (n.size() == 38 && hex(n)) out.push_back(d + n);   // (a write in progress has a longer name)
  }
  return out;
}

std::vector<std::string> Git::local_packs() const {
  std::vector<std::string> out;
  load_pack_indexes();
  const std::string own = common_ + "/objects/pack/";
  for (auto& pf : packs_)
    if (pf.pack_path.rfind(own, 0) == 0 && pf.pack_path.size() > 5) out.push_back(pf.pack_path.substr(0, pf.pack_path.size() - 5));
  return out;
}

std::vector<std::string> Git::local_packed_ids() const {
  std::vector<std::string> out;
  load_pack_indexes();
  const std::string own = common_ + "/objects/pack/";
  for (auto& pf : packs_) {
    if (pf.pack_path.rfind(own, 0) != 0) continue;
    for (uint32_t i = 0; i < pf.count; ++i) out.push_back(raw_to_hex(idx_oid_at(pf.idx, pf.idx_version, i)));
  }
  return out;
}

int64_t Git::pack_find(const PackFile& pf, const uint8_t want[20]) {
  const size_t fan = pf.idx_version == 2 ? 8 : 0;
  const uint8_t* d = pf.idx;
  uint32_t lo = want[0] == 0 ? 0 : rd_be32(d + fan + (size_t)(want[0] - 1) * 4);
  uint32_t hi = rd_be32(d + fan + (size_t)want[0] * 4);
  if (hi > pf.count || lo > hi) return -1;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    int c = std::memcmp(idx_oid_at(pf.idx, pf.idx_version, mid), want, 20);
    if (c == 0) return mid;
    if (c < 0) lo = mid + 1; else hi = mid;
  }
  return -1;
}

Result<void> Git::map_pack(const PackFile& pf) const {
  if (pf.pack_view) return {};
  auto p = kernel_.host().view_file(pf.pack_path);
  if (!p.ok()) return Result<void>(Status::IoError, "cannot read " + pf.pack_path + ": " + p.error().str());
  if ((*p)->size < 32 || std::memcmp((*p)->data, "PACK", 4) != 0) return Result<void>(Status::Corrupt, pf.pack_path + " is not a packfile");
  pf.pack_view = std::move(*p);
  return {};
}

Result<Git::PackedEntry> Git::packed_entry(const std::string& oid) const {
  using R = Result<PackedEntry>;
  uint8_t want[20];
  if (!hex_to_20(oid, want)) return R(Status::Invalid, "malformed object id " + oid);
  load_pack_indexes();
  for (auto& pf : packs_) {
    const int64_t found = pack_find(pf, want);
    if (found < 0) continue;
    if (auto m = map_pack(pf); !m.ok()) return R(m.error());
    if (pf.by_offset.empty() && pf.count) {
      pf.by_offset.reserve(pf.count);
      for (uint32_t i = 0; i < pf.count; ++i) {
        uint64_t o = 0;
        if (!idx_offset_at(pf.idx, pf.idx_size, pf.idx_version, pf.count, i, o)) return R(Status::Corrupt, pf.pack_path + ": large offset past the index");
        pf.by_offset.emplace_back(o, i);
      }
      std::sort(pf.by_offset.begin(), pf.by_offset.end());
    }
    uint64_t off = 0;
    if (!idx_offset_at(pf.idx, pf.idx_size, pf.idx_version, pf.count, (uint32_t)found, off)) return R(Status::Corrupt, pf.pack_path + ": large offset past the index");
    const size_t body_end = pf.pack_size() - 20;   // the trailer is not an entry
    auto here = std::lower_bound(pf.by_offset.begin(), pf.by_offset.end(), std::make_pair(off, (uint32_t)0));
    if (here == pf.by_offset.end() || here->first != off) return R(Status::Corrupt, pf.pack_path + ": the index names an offset it does not list");
    const size_t end = (here + 1) == pf.by_offset.end() ? body_end : (size_t)(here + 1)->first;
    size_t p = (size_t)off;
    if (p >= body_end || end > body_end || end <= p) return R(Status::Corrupt, pf.pack_path + ": offset past the pack");
    const uint8_t* pk = pf.pack_data();
    uint8_t c = pk[p++];
    PackedEntry e;
    e.type = (c >> 4) & 7;
    e.size = c & 15;
    int shift = 4;
    while (c & 0x80) {
      if (p >= end) return R(Status::Corrupt, pf.pack_path + ": truncated object header");
      c = pk[p++];
      e.size |= (uint64_t)(c & 0x7f) << shift;
      shift += 7;
    }
    if (e.type == 6) {
      if (p >= end) return R(Status::Corrupt, pf.pack_path + ": truncated delta offset");
      uint8_t b = pk[p++];
      uint64_t ofs = b & 0x7f;
      while (b & 0x80) {
        if (p >= end) return R(Status::Corrupt, pf.pack_path + ": truncated delta offset");
        b = pk[p++];
        ofs = ((ofs + 1) << 7) | (b & 0x7f);
      }
      if (ofs > off) return R(Status::Corrupt, pf.pack_path + ": offset delta points before the pack");
      auto base = std::lower_bound(pf.by_offset.begin(), pf.by_offset.end(), std::make_pair(off - ofs, (uint32_t)0));
      if (base == pf.by_offset.end() || base->first != off - ofs) return R(Status::Corrupt, pf.pack_path + ": offset delta names no entry");
      e.base_oid = raw_to_hex(idx_oid_at(pf.idx, pf.idx_version, base->second));
      e.type = 7;
    } else if (e.type == 7) {
      if (p + 20 > end) return R(Status::Corrupt, pf.pack_path + ": truncated delta base id");
      e.base_oid = raw_to_hex(pk + p);
      p += 20;
    } else if (e.type < 1 || e.type > 4) {
      return R(Status::Corrupt, pf.pack_path + ": unknown object type " + u64_str((uint64_t)e.type));
    }
    if (p >= end) return R(Status::Corrupt, pf.pack_path + ": an entry with no data");
    e.data = ByteSpan(pk + p, end - p);
    return e;
  }
  return R(Status::NotFound, "object " + oid + " is not in a pack");
}

bool Git::in_some_pack(const uint8_t want[20]) const {
  load_pack_indexes();
  for (auto& pf : packs_) if (pack_find(pf, want) >= 0) return true;
  return false;
}

namespace {
// The first bytes a zlib stream at `off` of `whole` inflates to (at most `max`).
size_t inflate_head(ByteSpan whole, size_t off, uint8_t* out, size_t max) {
  if (off >= whole.size()) return 0;
#if defined(AIOS_HAVE_ZLIB)
  size_t got = 0;
  if (net::zlib_inflate_prefix(whole.subspan(off), out, max, &got)) return got;
#endif
  auto z = zlib_inflate_at(whole, off, nullptr);
  if (!z.ok()) return 0;
  const size_t n = std::min(max, z->size());
  std::memcpy(out, z->data(), n);
  return n;
}
}  // namespace

Result<std::pair<std::string, uint64_t>> Git::object_info(const std::string& oid) const {
  using R = Result<std::pair<std::string, uint64_t>>;
  if (!mem_.empty())
    if (auto it = mem_.find(oid); it != mem_.end()) return std::make_pair(it->second.first, (uint64_t)it->second.second.size());
  uint8_t want[20];
  if (oid.size() == 40 && hex_to_20(oid, want)) {
    load_pack_indexes();
    for (auto& pf : packs_) {
      const int64_t found = pack_find(pf, want);
      if (found < 0) continue;
      uint64_t at = 0;
      if (!idx_offset_at(pf.idx, pf.idx_size, pf.idx_version, pf.count, (uint32_t)found, at)) break;
      if (!map_pack(pf).ok()) break;
      const ByteSpan pack(pf.pack_data(), pf.pack_size() - 20);
      bool have_size = false;
      uint64_t result = 0;
      for (int hop = 0; hop <= 512; ++hop) {
        size_t p = (size_t)at;
        if (p >= pack.size()) break;
        uint8_t c = pack[p++];
        const int t = (c >> 4) & 7;
        uint64_t size = c & 15;
        int shift = 4;
        while ((c & 0x80) && p < pack.size()) { c = pack[p++]; size |= (uint64_t)(c & 0x7f) << shift; shift += 7; }
        if (t >= 1 && t <= 4) return std::make_pair(std::string(pack_type_name(t)), have_size ? result : size);
        uint64_t next_at = 0;
        std::string ref_base;
        if (t == 6) {
          if (p >= pack.size()) break;
          uint8_t b = pack[p++];
          uint64_t ofs = b & 0x7f;
          while ((b & 0x80) && p < pack.size()) { b = pack[p++]; ofs = ((ofs + 1) << 7) | (b & 0x7f); }
          if (ofs > at) break;
          next_at = at - ofs;
        } else if (t == 7) {
          if (p + 20 > pack.size()) break;
          ref_base = raw_to_hex(pack.data() + p);
          p += 20;
        } else {
          break;
        }
        if (!have_size) {
          // The delta starts with its base's size and the result's, two varints: 20 bytes hold both.
          uint8_t head[20];
          const size_t n = inflate_head(pack, p, head, sizeof head);
          size_t q = 0;
          bool bad = false;
          (void)delta_varint(ByteSpan(head, n), q, bad);
          result = delta_varint(ByteSpan(head, n), q, bad);
          if (bad) break;
          have_size = true;
        }
        if (t == 7) {
          auto b = object_info(ref_base);
          if (!b.ok()) return b;
          return std::make_pair(b->first, result);
        }
        at = next_at;
      }
      break;   // damaged: the full read reports why
    }
    // Loose: "<type> <size>\0" at the front of the inflated file.
    auto raw = kernel_.host().read_file(object_path(oid));
    if (raw.ok()) {
      uint8_t head[64];
      const size_t n = inflate_head(ByteSpan(raw->data(), raw->size()), 0, head, sizeof head);
      const std::string h((const char*)head, n);
      const size_t sp = h.find(' '), nul = h.find('\0');
      if (sp != std::string::npos && nul != std::string::npos && sp < nul)
        return std::make_pair(h.substr(0, sp), parse_u64_dec(h, sp + 1, nul));
    }
  }
  auto o = read_object(oid);
  if (!o.ok()) return R(o.error());
  return std::make_pair(o->type, (uint64_t)o->payload.size());
}

int Git::abbrev_len(const std::string& oid, int min_len) const {
  if (min_len >= 40 || oid.size() != 40) return 40;
  uint8_t want[20];
  if (!hex_to_20(oid, want)) return 40;
  int longest = 0;   // the most hex digits another object shares with oid
  load_pack_indexes();
  for (auto& pf : packs_) {
    uint32_t lo = 0, hi = pf.count;
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      if (std::memcmp(idx_oid_at(pf.idx, pf.idx_version, mid), want, 20) < 0) lo = mid + 1; else hi = mid;
    }
    if (lo > 0) longest = std::max(longest, common_hex(idx_oid_at(pf.idx, pf.idx_version, lo - 1), want));
    uint32_t next = lo;
    if (next < pf.count && std::memcmp(idx_oid_at(pf.idx, pf.idx_version, next), want, 20) == 0) ++next;
    if (next < pf.count) longest = std::max(longest, common_hex(idx_oid_at(pf.idx, pf.idx_version, next), want));
  }
  auto loose_dir = [&](const std::string& dir, const std::string& key) {
    auto it = loose_names_.find(key);
    if (it == loose_names_.end()) {
      std::vector<std::string> names;
      if (auto n = kernel_.host().list_dir(dir); n.ok()) names = std::move(*n);
      it = loose_names_.emplace(key, std::move(names)).first;
    }
    for (const auto& n : it->second) {
      if (n.size() != 38 || n.compare(0, 38, oid, 2, 38) == 0) continue;
      int k = 2;
      while (k < 40 && n[(size_t)k - 2] == oid[(size_t)k]) ++k;
      longest = std::max(longest, k);
    }
  };
  const std::string fan = oid.substr(0, 2);
  loose_dir(common_ + "/objects/" + fan, fan);
  for (const auto& alt : alternate_object_dirs()) loose_dir(alt + "/" + fan, alt + "/" + fan);
  for (const auto& [id, _] : mem_) {
    if (id == oid) continue;
    int k = 0;
    while (k < 40 && id[(size_t)k] == oid[(size_t)k]) ++k;
    longest = std::max(longest, k);
  }
  load_blocks();
  if (!blocks_.empty()) {
    std::vector<std::string> same;
    for (auto& b : blocks_) b->ids_with_prefix(fan, same);
    for (const auto& id : same) {
      if (id == oid) continue;
      int k = 0;
      while (k < 40 && id[(size_t)k] == oid[(size_t)k]) ++k;
      longest = std::max(longest, k);
    }
  }
  return std::min(40, std::max(min_len, longest + 1));
}

Result<GitObject> Git::read_packed(const std::string& oid, int depth) const {
  uint8_t want[20];
  if (!hex_to_20(oid, want)) return Result<GitObject>(Status::Invalid, "malformed object id " + oid);
  return read_packed_raw(want, oid, depth);
}

// An object out of the packs. Not re-hashed here: git trusts its packs on
// read (fsck and index-pack check them), and hashing every object read cost
// a log or a blame a SHA-1 of each commit, tree and file version it touched.
Result<GitObject> Git::read_packed_raw(const uint8_t want[20], const std::string& oid, int depth) const {
  if (depth > 512) return Result<GitObject>(Status::Corrupt, "object " + oid + ": delta chain deeper than 512");
  load_pack_indexes();
  for (auto& pf : packs_) {
    const int64_t found = pack_find(pf, want);
    if (found < 0) continue;
    uint64_t off = 0;
    if (!idx_offset_at(pf.idx, pf.idx_size, pf.idx_version, pf.count, (uint32_t)found, off))
      return Result<GitObject>(Status::Corrupt, pf.pack_path + ": large offset past the index");
    if (auto m = map_pack(pf); !m.ok()) return Result<GitObject>(m.error());
    auto u = unpack_at(pf, off, depth);
    if (!u.ok()) return Result<GitObject>(Status::Corrupt, "object " + oid + ": " + u.error().message);
    GitObject o;
    o.oid = oid;
    o.type = pack_type_name(u->first);
    if (u->second.use_count() == 1) o.payload = std::move(const_cast<ByteBuffer&>(*u->second));
    else o.payload = *u->second;
    return o;
  }
  return Result<GitObject>(Status::NotFound, "object " + oid + " is not in this database");
}

Result<std::pair<int, std::shared_ptr<const ByteBuffer>>> Git::unpack_at(const PackFile& pf, uint64_t off, int depth) const {
  using R = Result<std::pair<int, std::shared_ptr<const ByteBuffer>>>;
  constexpr size_t kBaseCacheBytes = 96u << 20;
  const ByteSpan pack(pf.pack_data(), pf.pack_size() - 20);
  // Down the chain to a whole object or a cached base; then the deltas back up, each result kept as a base for the
  // next version that asks for it.
  std::vector<std::pair<uint64_t, ByteBuffer>> deltas;   // (offset of the delta entry, its inflated delta)
  int type = 0;
  std::shared_ptr<const ByteBuffer> base;
  uint64_t at = off;
  // Whether `base` is the object AT `at` (a whole entry, or one found cached there). A REF_DELTA's base comes from
  // elsewhere while `at` is still the delta's own offset: kept under it, a later read of that entry got the base.
  bool base_is_at = true;
  for (int hop = 0;; ++hop) {
    if (hop > 512) return R(Status::Corrupt, "delta chain deeper than 512");
    if (hop > 0 || !deltas.empty()) {
      if (auto c = pf.bases.find(at); c != pf.bases.end()) {
        c->second.used = ++pf.bases_clock;
        type = c->second.type;
        base = c->second.bytes;
        break;
      }
    }
    size_t p = (size_t)at;
    if (p >= pack.size()) return R(Status::Corrupt, pf.pack_path + ": offset past the pack");
    uint8_t c = pack[p++];
    int t = (c >> 4) & 7;
    uint64_t size = c & 15;
    int shift = 4;
    while (c & 0x80) {
      if (p >= pack.size()) return R(Status::Corrupt, pf.pack_path + ": truncated object header");
      c = pack[p++];
      size |= (uint64_t)(c & 0x7f) << shift;
      shift += 7;
    }
    uint64_t next_at = 0;
    std::string ref_base;
    if (t == 6) {
      if (p >= pack.size()) return R(Status::Corrupt, pf.pack_path + ": truncated delta offset");
      uint8_t b = pack[p++];
      uint64_t ofs = b & 0x7f;
      while (b & 0x80) {
        if (p >= pack.size()) return R(Status::Corrupt, pf.pack_path + ": truncated delta offset");
        b = pack[p++];
        ofs = ((ofs + 1) << 7) | (b & 0x7f);
      }
      if (ofs > at) return R(Status::Corrupt, pf.pack_path + ": offset delta points before the pack");
      next_at = at - ofs;
    } else if (t == 7) {
      if (p + 20 > pack.size()) return R(Status::Corrupt, pf.pack_path + ": truncated delta base id");
      ref_base = raw_to_hex(pack.data() + p);
      p += 20;
    }
    size_t end = 0;
    auto z = zlib_inflate_at(pack, p, &end, (size_t)std::min<uint64_t>(size, (uint64_t)256 << 20));
    if (!z.ok()) return R(z.error());
    if (t >= 1 && t <= 4) {
      if (z->size() != size) return R(Status::Corrupt, "packed object at " + u64_str(at) + " inflated to the wrong size");
      type = t;
      base = std::make_shared<const ByteBuffer>(std::move(*z));
      break;
    }
    if (t == 6) { deltas.emplace_back(at, std::move(*z)); at = next_at; continue; }
    if (t == 7) {
      deltas.emplace_back(at, std::move(*z));
      // The base may be anywhere: loose, this pack, another pack.
      auto b = read_object(ref_base);
      if (!b.ok()) return R(Status::Corrupt, "delta base " + ref_base + " is missing (" + b.error().str() + ")");
      type = b->type == "commit" ? 1 : b->type == "tree" ? 2 : b->type == "blob" ? 3 : 4;
      base = std::make_shared<const ByteBuffer>(std::move(b->payload));
      base_is_at = false;
      break;
    }
    return R(Status::Corrupt, pf.pack_path + ": unknown object type " + u64_str((uint64_t)t));
  }
  (void)depth;
  auto keep = [&](uint64_t where, const std::shared_ptr<const ByteBuffer>& bytes) {
    if (bytes->size() > kBaseCacheBytes / 8 || pf.bases.count(where)) return;
    while (pf.bases_bytes + bytes->size() > kBaseCacheBytes && !pf.bases.empty()) {
      // Out of room: the older half goes (an LRU list per entry cost more than it saved).
      std::vector<uint64_t> ages;
      ages.reserve(pf.bases.size());
      for (const auto& [o, b] : pf.bases) ages.push_back(b.used);
      std::nth_element(ages.begin(), ages.begin() + ages.size() / 2, ages.end());
      const uint64_t cut = ages[ages.size() / 2];
      for (auto it = pf.bases.begin(); it != pf.bases.end();) {
        if (it->second.used <= cut) { pf.bases_bytes -= it->second.bytes->size(); it = pf.bases.erase(it); }
        else ++it;
      }
    }
    pf.bases.emplace(where, PackFile::Base{type, bytes, ++pf.bases_clock});
    pf.bases_bytes += bytes->size();
  };
  // The bottom of the chain was read as a base: kept.
  if (!deltas.empty() && base_is_at) keep(at, base);
  for (size_t i = deltas.size(); i-- > 0;) {
    auto full = git_apply_delta(ByteSpan(base->data(), base->size()), ByteSpan(deltas[i].second.data(), deltas[i].second.size()));
    if (!full.ok()) return R(full.error());
    base = std::make_shared<const ByteBuffer>(std::move(*full));
    if (i > 0) keep(deltas[i].first, base);   // a base of the delta above it
  }
  return std::make_pair(type, std::move(base));
}

Result<std::vector<GitTreeEntry>> Git::read_tree(const std::string& oid) const {
  // The empty tree exists in every repository whether or not it was ever
  // written: git names it without storing it (a commit that deletes every file).
  if (oid == "4b825dc642cb6eb9a060e54bf8d69288fbee4904") return std::vector<GitTreeEntry>{};
  auto o = read_object(oid);
  if (!o.ok()) return Result<std::vector<GitTreeEntry>>(o.error());
  if (o->type != "tree") return Result<std::vector<GitTreeEntry>>(Status::Invalid, oid + " is a " + o->type + ", not a tree");
  std::vector<GitTreeEntry> out;
  const ByteBuffer& b = o->payload;
  size_t i = 0;
  while (i < b.size()) {
    size_t sp = i;
    while (sp < b.size() && b[sp] != ' ') ++sp;
    if (sp >= b.size()) return Result<std::vector<GitTreeEntry>>(Status::Corrupt, "tree " + oid + " truncated at a mode");
    uint32_t mode = 0;
    for (size_t k = i; k < sp; ++k) mode = mode * 8 + (uint32_t)(b[k] - '0');
    size_t nul = sp + 1;
    while (nul < b.size() && b[nul] != 0) ++nul;
    if (nul >= b.size() || nul + 21 > b.size()) return Result<std::vector<GitTreeEntry>>(Status::Corrupt, "tree " + oid + " truncated at an entry");
    GitTreeEntry e;
    // git's canon_mode: old trees (git's own first commits among them) recorded modes such as
    // 100664; git reads every regular file as 100644 or 100755, and so must this, or a merge or
    // checkout of that history disagrees with git about what the tree holds.
    switch (mode & 0170000) {
      case 0100000: e.mode = (mode & 0111) ? 0100755 : 0100644; break;
      case 0120000: e.mode = 0120000; break;
      case 0040000: e.mode = 040000; break;
      default: e.mode = 0160000; break;
    }
    e.name.assign((const char*)b.data() + sp + 1, nul - sp - 1);
    e.oid = raw_to_hex(b.data() + nul + 1);
    out.push_back(std::move(e));
    i = nul + 21;
  }
  return out;
}

bool Git::tree_lookup(const std::string& tree, std::string_view name, std::string& oid, uint32_t& mode) const {
  if (tree == "4b825dc642cb6eb9a060e54bf8d69288fbee4904") return false;
  auto o = read_object(tree);
  if (!o.ok() || o->type != "tree") return false;
  const ByteBuffer& b = o->payload;
  size_t i = 0;
  while (i < b.size()) {
    size_t sp = i;
    while (sp < b.size() && b[sp] != ' ') ++sp;
    if (sp >= b.size()) return false;
    size_t nul = sp + 1;
    while (nul < b.size() && b[nul] != 0) ++nul;
    if (nul >= b.size() || nul + 21 > b.size()) return false;
    if (nul - sp - 1 == name.size() && std::memcmp(b.data() + sp + 1, name.data(), name.size()) == 0) {
      uint32_t m = 0;
      for (size_t k = i; k < sp; ++k) m = m * 8 + (uint32_t)(b[k] - '0');
      switch (m & 0170000) {   // canon_mode, as read_tree
        case 0100000: mode = (m & 0111) ? 0100755 : 0100644; break;
        case 0120000: mode = 0120000; break;
        case 0040000: mode = 040000; break;
        default: mode = 0160000; break;
      }
      oid = raw_to_hex(b.data() + nul + 1);
      return true;
    }
    i = nul + 21;
  }
  return false;
}

namespace {
// A commit's bytes read in place, one pass: the header lines up to the blank line, then the message. With
// `meta_only` it stops after the committer line (what a history walk needs: tree, parents, time). Splitting the text
// into a vector of line strings and copying every field out of them was most of the cost of a walk over 13k commits.
bool scan_commit(const uint8_t* d, size_t n, GitCommit& c, bool meta_only) {
  const char* t = (const char*)d;
  size_t at = 0;
  bool seen_committer = false;
  auto ident = [&](size_t from, size_t eol, std::string& who, uint64_t& when, std::string& tz) {
    // "Name <email> <seconds> <±hhmm>"
    size_t gt = std::string_view(t + from, eol - from).rfind('>');
    if (gt == std::string_view::npos) { who.assign(t + from, eol - from); return; }
    gt += from;
    who.assign(t + from, gt + 1 - from);
    size_t p = gt + 1;
    while (p < eol && t[p] == ' ') ++p;
    uint64_t v = 0;
    while (p < eol && t[p] >= '0' && t[p] <= '9') v = v * 10 + (uint64_t)(t[p++] - '0');
    when = v;
    while (p < eol && t[p] == ' ') ++p;
    size_t ze = eol;
    while (ze > p && (t[ze - 1] == ' ' || t[ze - 1] == '\r')) --ze;
    if (ze > p) tz.assign(t + p, ze - p);
  };
  auto trimmed = [&](size_t from, size_t eol) {
    while (from < eol && (t[from] == ' ' || t[from] == '\t')) ++from;
    while (eol > from && (t[eol - 1] == ' ' || t[eol - 1] == '\t' || t[eol - 1] == '\r')) --eol;
    return std::string(t + from, eol - from);
  };
  while (at < n) {
    size_t eol = at;
    while (eol < n && t[eol] != '\n') ++eol;
    if (eol == at) {   // the blank line: the message follows
      if (!meta_only) {
        const size_t m = at + 1 <= n ? at + 1 : n;
        c.raw_message.assign(t + m, n - m);
        c.message = c.raw_message;
        while (!c.message.empty() && c.message.back() == '\n') c.message.pop_back();
      }
      return true;
    }
    const std::string_view line(t + at, eol - at);
    if (line[0] == ' ') {   // a continuation of the header before it (gpgsig, mergetag)
      if (!meta_only && !c.extra_headers.empty()) { c.extra_headers.back().second += '\n'; c.extra_headers.back().second.append(line.substr(1)); }
    } else if (seen_committer) {
      if (meta_only) return true;
      const size_t sp = line.find(' ');
      c.extra_headers.push_back({std::string(line.substr(0, sp)), sp == std::string_view::npos ? std::string() : std::string(line.substr(sp + 1))});
    } else if (line.compare(0, 5, "tree ") == 0) {
      c.tree = trimmed(at + 5, eol);
    } else if (line.compare(0, 7, "parent ") == 0) {
      c.parents.push_back(trimmed(at + 7, eol));
    } else if (line.compare(0, 7, "author ") == 0) {
      if (!meta_only) ident(at + 7, eol, c.author, c.author_time, c.author_tz);
    } else if (line.compare(0, 10, "committer ") == 0) {
      seen_committer = true;
      std::string who, tz = "+0000";
      uint64_t when = 0;
      ident(at + 10, eol, who, when, tz);
      c.committer_time = when;
      if (!meta_only) { c.committer = std::move(who); c.committer_tz = std::move(tz); }
    }
    at = eol + 1;
  }
  return true;
}
}  // namespace

Result<GitCommit> Git::read_commit(const std::string& oid) const {
  auto o = read_object(oid);
  if (!o.ok()) return Result<GitCommit>(o.error());
  if (o->type != "commit") return Result<GitCommit>(Status::Invalid, oid + " is a " + o->type + ", not a commit");
  GitCommit c;
  c.oid = oid;
  scan_commit(o->payload.data(), o->payload.size(), c, false);
  if (c.tree.empty()) return Result<GitCommit>(Status::Corrupt, "commit " + oid + " names no tree");
  return c;
}

// Every file under `rel` that is not ignored. Tracked files are listed even where an ignore rule would
// hide them: git never un-tracks a file because a pattern matches it.
// The index's paths and the directories above them, as views of its own strings (two copies of every path, made
// twice per `status`, were a third of its work).
Git::TrackedView Git::tracked_view() const {
  ensure_index();
  TrackedView v;
  v.files.reserve((index_.size() + unmerged_.size()) * 2);
  v.dirs.reserve(index_.size() / 4 + 16);
  for (auto& e : index_) {
    const std::string_view p(e.path);
    v.files.insert(p);
    for (size_t s = p.find('/'); s != std::string_view::npos; s = p.find('/', s + 1)) v.dirs.insert(p.substr(0, s));
  }
  for (auto& u : unmerged_) v.files.insert(std::string_view(u.path));
  return v;
}

Result<void> Git::scan_worktree(const std::string& rel, std::vector<std::string>& out) const {
  const TrackedView tv = tracked_view();
  return scan_worktree(rel, out, tv, true);
}

Result<void> Git::scan_worktree(const std::string& rel, std::vector<std::string>& out, const TrackedView& tv,
                                bool with_tracked, bool nested) const {
  const auto& tracked_dirs = tv.dirs;
  const auto& tracked = tv.files;
  // Each entry's kind from the directory listing itself (d_type): a stat per name was 2,000 of status's 4,000
  // system calls on redis. Only a file system that does not say is asked.
  std::function<Result<void>(const std::string&)> walk = [&](const std::string& dir) -> Result<void> {
    auto ents = kernel_.host().list_dir_typed(join_path(work_, dir));
    if (!ents.ok()) return Result<void>(ents.error());
    std::sort(ents->begin(), ents->end(), [](const hal::Host::DirEntry& x, const hal::Host::DirEntry& y) { return x.name < y.name; });
    for (auto& ent : *ents) {
      const std::string& n = ent.name;
      if (n == ".git") continue;
      std::string r = dir.empty() ? n : dir + "/" + n;
      bool is_dir = ent.kind == 'd';
      if (ent.kind == '?') {
        auto st = kernel_.host().stat(join_path(work_, r));
        if (!st.ok()) continue;
        is_dir = st->is_dir && !st->is_link;
      }
      if (is_dir) {
        // A nested repository is a separate project, not files of this one.
        const bool known = tracked_dirs.count(std::string_view(r)) > 0;
        if (!known && is_ignored(r, true)) continue;
        if (!known && kernel_.host().exists(join_path(work_, r) + "/.git")) {
          if (nested) out.push_back(r + "/");   // git lists it as one untracked directory
          continue;
        }
        if (auto s = walk(r); !s.ok()) return s;
      } else if (tracked.count(std::string_view(r))) {
        if (with_tracked) out.push_back(std::move(r));
      } else if (!is_ignored(r, false)) {
        out.push_back(std::move(r));
      }
    }
    return Ok();
  };
  return walk(rel);
}

bool Git::is_ignored(const std::string& rel, bool is_dir) const {
  if (!ignore_loaded_) {
    ignore_.load(kernel_.host(), work_, common_, settings_.excludes_file, settings_.ignorecase);
    ignore_loaded_ = true;
  }
  return ignore_.ignored(rel, is_dir);
}

// parse_fsync_components: git's default set (objects without loose objects, derived metadata - no references),
// with what core.fsync adds or ("-x") takes away; "none" clears it. Whether references are in it.
bool Git::fsync_covers_refs(const std::string& list) { return fsync_covers(list, false); }
bool Git::fsync_covers_index(const std::string& list) { return fsync_covers(list, true); }

bool Git::fsync_covers(const std::string& list, bool index) {
  bool refs = false;
  size_t i = 0;
  while (i <= list.size()) {
    size_t comma = list.find(',', i);
    std::string comp = trim(list.substr(i, comma == std::string::npos ? std::string::npos : comma - i));
    i = comma == std::string::npos ? list.size() + 1 : comma + 1;
    for (char& ch : comp) ch = (char)std::tolower((unsigned char)ch);
    const bool neg = !comp.empty() && comp[0] == '-';
    if (neg) comp = comp.substr(1);
    if (comp == "none") { refs = false; continue; }
    const bool covers = index ? (comp == "index" || comp == "added" || comp == "all")
                              : (comp == "reference" || comp == "committed" || comp == "added" || comp == "all");
    if (covers) refs = !neg;
  }
  return refs;
}

void Git::load_settings() {
  settings_ = Settings{};
  auto flag = [&](const char* key, bool def) {
    auto v = get_config(key);
    if (!v.ok()) return def;
    std::string s = *v;
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s == "true" || s == "yes" || s == "on" || s == "1" || s.empty();
  };
  settings_.filemode = flag("core.filemode", true);
  settings_.ignorecase = flag("core.ignorecase", false);
  settings_.symlinks = flag("core.symlinks", true);
  if (auto v = get_config("core.excludesfile"); v.ok()) settings_.excludes_file = *v;
  ignore_loaded_ = false;
  if (auto v = get_config("core.fsync"); v.ok()) { settings_.fsync_refs = fsync_covers_refs(*v); settings_.fsync_index = fsync_covers_index(*v); }
}

Result<std::vector<GitIndexEntry>> Git::add(const std::string& path) {
  ensure_index();
  if (auto r = may(R_Write); !r.ok()) return Result<std::vector<GitIndexEntry>>(r.error());
  auto& h = kernel_.host();
  std::string p = clean_path(path);
  // Accept both "a/file" inside the work tree and the full "work/a/file".
  std::string rel = p;
  if (work_ != "." && rel.rfind(work_ + "/", 0) == 0) rel = rel.substr(work_.size() + 1);
  else if (rel == work_) rel = "";
  if (rel == ".") rel = "";

  std::string full = join_path(work_, rel);
  if (!h.exists(full)) return Result<std::vector<GitIndexEntry>>(Status::NotFound, path + " is not in the work tree at " + work_);

  std::vector<std::string> files;
  auto st = h.stat(full);
  if (!st.ok()) return Result<std::vector<GitIndexEntry>>(st.error());
  if (st->is_dir && !st->is_link) { if (auto r = scan_worktree(rel, files); !r.ok()) return Result<std::vector<GitIndexEntry>>(r.error()); }
  else files.push_back(rel);

  std::map<std::string, size_t> at;
  for (size_t i = 0; i < index_.size(); ++i) at[index_[i].path] = i;
  std::vector<GitIndexEntry> added;
  for (auto& f : files) {
    auto it = at.find(f);
    if (it != at.end() && worktree_matches(index_[it->second])) continue;   // nothing new to hash
    auto staged = stage_entry(f, it != at.end() ? &index_[it->second] : nullptr);
    if (!staged.ok()) return Result<std::vector<GitIndexEntry>>(staged.error());
    GitIndexEntry e = *staged;
    if (it != at.end()) index_[it->second] = e;
    else { at[f] = index_.size(); index_.push_back(e); }
    unmerged_.erase(std::remove_if(unmerged_.begin(), unmerged_.end(), [&](const GitIndexEntry& u) { return u.path == f; }),
                    unmerged_.end());
    added.push_back(e);
  }
  if (auto r = save_index(); !r.ok()) return Result<std::vector<GitIndexEntry>>(r.error());
  return added;
}

Result<GitIndexEntry> Git::stage_entry(const std::string& f, const GitIndexEntry* existing) {
  auto& h = kernel_.host();
  std::string full = join_path(work_, f);
  auto info = h.stat(full);
  if (!info.ok()) return Result<GitIndexEntry>(info.error());
  ByteBuffer content;
  uint32_t mode = 0100644;
  if (info->is_link && settings_.symlinks) {
    auto target = h.read_symlink(full);
    if (!target.ok()) return Result<GitIndexEntry>(target.error());
    content.assign(target->begin(), target->end());
    mode = 0120000;
  } else {
    auto data = h.read_file(full);
    if (!data.ok()) return Result<GitIndexEntry>(data.error());
    content = std::move(*data);
    // git records exactly two file modes. Where the file system has no exec bit (core.filemode
    // false: Windows), the mode already staged is kept, as git does — otherwise every script in the
    // repository would turn 644 the first time it is added there.
    if (!settings_.filemode) mode = existing && existing->mode != 0120000 && existing->mode != 0160000 ? existing->mode : 0100644;
    else if (info->is_exec) mode = 0100755;
    if (!settings_.symlinks && existing && existing->mode == 0120000) mode = 0120000;
  }
  auto oid = write_object("blob", ByteSpan(content.data(), content.size()));
  if (!oid.ok()) return Result<GitIndexEntry>(oid.error());
  GitIndexEntry e{f, *oid, mode, content.size(), info->mtime_ns};
  if (existing) e.skip_worktree = existing->skip_worktree;
  return e;
}

bool Git::worktree_matches(const GitIndexEntry& e, std::string* new_oid) const {
  ensure_index();
  if (e.assume_valid || e.skip_worktree) return true;
  auto& host = kernel_.host();
  std::string full = join_path(work_, e.path);
  auto st = host.stat(full);
  if (!st.ok()) return false;
  if (e.mode == 0160000) return st->is_dir;                 // a submodule: its own repository decides
  if (st->is_dir && !st->is_link) return false;
  const bool link = e.mode == 0120000 && settings_.symlinks;
  // The stat cache: same size, same mtime, and the file was not still being written when the index was
  // (git's "racy git" rule) means the content is what was hashed.
  const bool exec_differs = settings_.filemode && !link && e.mode != 0120000 && ((e.mode == 0100755) != st->is_exec);
  if (!link && e.mtime_ns && st->mtime_ns == e.mtime_ns && st->size == e.size &&
      (index_mtime_ns_ == 0 || e.mtime_ns < index_mtime_ns_))
    return !exec_differs;
  ByteBuffer content;
  if (link) {
    auto target = host.read_symlink(full);
    if (!target.ok()) return false;
    content.assign(target->begin(), target->end());
  } else {
    auto data = host.read_file(full);
    if (!data.ok()) return false;
    content = std::move(*data);
  }
  std::string header = "blob " + u64_str(content.size());
  ByteBuffer ser(header.begin(), header.end());
  ser.push_back(0);
  ser.insert(ser.end(), content.begin(), content.end());
  std::string oid = hex20(crypto::sha1(ByteSpan(ser.data(), ser.size())));
  if (new_oid) *new_oid = oid;
  if (oid != e.oid) return false;
  // Same content; a changed exec bit is still a change where the file system has one.
  return !exec_differs;
}

Result<std::vector<GitStatusEntry>> Git::worktree_changes() const {
  ensure_index();
  std::vector<GitStatusEntry> out;
  // Every entry's stat check, spread over threads as git's preload_index does (at least 500 entries each): the
  // checks are independent and nearly all system call.
  std::vector<char> matches(index_.size(), 0);
  {
    std::atomic<size_t> next{0};
    auto work = [&]() {
      for (;;) {
        const size_t from = next.fetch_add(64);
        if (from >= index_.size()) return;
        const size_t to = std::min(index_.size(), from + 64);
        for (size_t i = from; i < to; ++i) {
          const GitIndexEntry& e = index_[i];
          matches[i] = (e.mtime_ns != 0 && watch_says_unchanged(e.path)) || worktree_matches(e) ? 1 : 2;
        }
      }
    };
#if defined(__wasi__)
    work();
#else
    unsigned n = hal::hardware_threads();
    n = std::max(1u, std::min({n, 20u, (unsigned)(index_.size() / 500)}));
    std::vector<hal::Thread> pool;
    for (unsigned t = 1; t < n; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
#endif
  }
  for (size_t i = 0; i < index_.size(); ++i) {
    const GitIndexEntry& e = index_[i];
    if (matches[i] == 1) continue;
    auto st = kernel_.host().stat(join_path(work_, e.path));
    bool gone = !st.ok() || (st->is_dir && !st->is_link && e.mode != 0160000);
    out.push_back({e.path, gone ? GitChange::Deleted : GitChange::Modified});
  }
  return out;
}

Result<std::vector<std::string>> Git::untracked(bool collapse, bool with_nested) const {
  ensure_index();
  std::vector<std::string> files;
  if (watch_hint_) {
    // only what the watcher saw appear: those files, and everything in a directory it saw appear
    std::set<std::string> tracked_now;
    for (auto& e : index_) tracked_now.insert(e.path);
    for (auto& p : *watch_hint_) {
      auto st = kernel_.host().stat(join_path(work_, p));
      if (!st.ok()) continue;
      if (st->is_dir && !st->is_link) {
        if (is_ignored(p, true)) continue;
        std::vector<std::string> under;
        if (auto r = scan_worktree(p, under); r.ok()) files.insert(files.end(), under.begin(), under.end());
      } else if (!tracked_now.count(p) && !is_ignored(p, false)) {
        files.push_back(p);
      }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
  }
  const TrackedView tv = tracked_view();
  if (!watch_hint_)
    if (auto r = scan_worktree("", files, tv, false, with_nested); !r.ok()) return Result<std::vector<std::string>>(r.error());
  const auto& tracked = tv.files;
  const auto& tracked_dirs = tv.dirs;
  std::vector<std::string> out;
  for (auto& f : files) {
    if (tracked.count(std::string_view(f))) continue;
    if (!collapse) { out.push_back(f); continue; }
    // The shallowest directory on the way down that holds nothing tracked stands for everything in it.
    std::string shown = f;
    for (size_t s = f.find('/'); s != std::string::npos; s = f.find('/', s + 1)) {
      if (!tracked_dirs.count(std::string_view(f).substr(0, s))) { shown = f.substr(0, s) + "/"; break; }
    }
    if (out.empty() || out.back() != shown) out.push_back(shown);
  }
  return out;
}

// Builds the tree objects for the given paths, deepest directories first, and
// returns the root tree's id. Entries are ordered the way git orders them: by
// name, with a directory compared as if its name ended in '/'.
Result<std::string> Git::write_tree_from_entries(const std::vector<GitIndexEntry>& entries) {
  if (auto r = may(R_Write); !r.ok()) return Result<std::string>(r.error());
  return trees_from_entries(entries, true);
}

// The root tree id the index would commit, computed and not written: `status` compares it with HEAD's tree and, the
// same, has nothing staged to look for (it read every tree of HEAD to find that out).
Result<std::string> Git::index_tree_oid() const {
  ensure_index();
  Git* self = const_cast<Git*>(this);
  const bool cached = !cache_tree_.empty() && unmerged_.empty() && index_fingerprint(index_) == cache_tree_fp_ &&
                      std::none_of(index_.begin(), index_.end(), [](const GitIndexEntry& e) { return e.intent_to_add; });
  self->tree_cache_in_use_ = cached ? &cache_tree_ : nullptr;
  auto r = self->trees_from_entries(index_, false);
  self->tree_cache_in_use_ = nullptr;
  return r;
}

Result<std::string> Git::trees_from_entries(const std::vector<GitIndexEntry>& entries, bool write) {
  // The entries in path order (the index's own; sorted here when a caller's are not), so each directory is one
  // contiguous run: its tree is built from views of the paths, no entry copied (a copy of every entry, strings and
  // all, was most of the cost of hashing the index for `status`).
  std::vector<const GitIndexEntry*> order;
  order.reserve(entries.size());
  for (auto& e : entries) if (!e.intent_to_add) order.push_back(&e);   // named by `add -N`, not staged
  if (!std::is_sorted(order.begin(), order.end(), [](const GitIndexEntry* x, const GitIndexEntry* y) { return x->path < y->path; }))
    std::stable_sort(order.begin(), order.end(), [](const GitIndexEntry* x, const GitIndexEntry* y) { return x->path < y->path; });
  struct Ent { std::string_view name; bool dir; std::string oid; uint32_t mode; };
  const auto* cache = tree_cache_in_use_;
  std::function<Result<std::string>(size_t, size_t, size_t)> build = [&](size_t lo, size_t hi, size_t plen) -> Result<std::string> {
    if (cache && lo < hi) {   // a directory git's cache-tree still vouches for, holding exactly these entries
      const std::string dir = plen ? order[lo]->path.substr(0, plen - 1) : std::string();
      if (auto it = cache->find(dir); it != cache->end() && it->second.first == (long)(hi - lo)) return it->second.second;
    }
    std::vector<Ent> ents;
    for (size_t i = lo; i < hi;) {
      std::string_view rest = std::string_view(order[i]->path).substr(plen);
      const size_t slash = rest.find('/');
      if (slash == std::string_view::npos) {
        ents.push_back(Ent{rest, false, order[i]->oid, order[i]->mode});
        ++i;
        continue;
      }
      const std::string_view name = rest.substr(0, slash);
      size_t j = i + 1;
      while (j < hi) {
        std::string_view r2 = std::string_view(order[j]->path).substr(plen);
        if (r2.size() <= slash || r2.compare(0, slash, name) != 0 || r2[slash] != '/') break;
        ++j;
      }
      auto sub = build(i, j, plen + slash + 1);
      if (!sub.ok()) return sub;
      ents.push_back(Ent{name, true, std::move(*sub), 040000});
      i = j;
    }
    // git's tree order: by name, a directory as if its name ended in '/'
    std::sort(ents.begin(), ents.end(), [](const Ent& x, const Ent& y) {
      const size_t n = std::min(x.name.size(), y.name.size());
      const int c = std::memcmp(x.name.data(), y.name.data(), n);
      if (c != 0) return c < 0;
      const unsigned char cx = x.name.size() > n ? (unsigned char)x.name[n] : (x.dir ? '/' : 0);
      const unsigned char cy = y.name.size() > n ? (unsigned char)y.name[n] : (y.dir ? '/' : 0);
      return cx < cy;
    });
    ByteBuffer payload;
    for (auto& e : ents) {
      std::string head = oct_mode(e.mode);
      head += ' ';
      head.append(e.name);
      payload.insert(payload.end(), head.begin(), head.end());
      payload.push_back(0);
      uint8_t raw[20];
      if (!hex_to_20(e.oid, raw)) return Result<std::string>(Status::Corrupt, "index holds a malformed id for " + std::string(e.name));
      payload.insert(payload.end(), raw, raw + 20);
    }
    if (write) return write_object("tree", ByteSpan(payload.data(), payload.size()));
    std::string head = "tree " + u64_str(payload.size());
    ByteBuffer whole(head.begin(), head.end());
    whole.push_back(0);
    whole.insert(whole.end(), payload.begin(), payload.end());
    return hex20(crypto::sha1(ByteSpan(whole.data(), whole.size())));
  };
  return build(0, order.size(), 0);
}

Result<std::string> Git::commit_tree(const std::string& tree, const std::vector<std::string>& parents,
                                     const std::string& message, const std::string& author,
                                     const std::string& committer, uint64_t when) {
  if (auto r = may(R_Write); !r.ok()) return Result<std::string>(r.error());
  if (tree.size() != 40) return Result<std::string>(Status::Invalid, "a commit names a full 40-hex tree id, not '" + tree + "'");
  for (auto& p : parents)
    if (p.size() != 40) return Result<std::string>(Status::Invalid, "a commit names full 40-hex parent ids, not '" + p + "'");
  if (when == 0) when = kernel_.host().wall_ns() / 1000000000ull;
  std::string self = author_name_ + " <" + author_email_ + ">";
  std::string stamp = " " + u64_str(when) + " " + git_local_tz(when) + "\n";
  std::string body;
  body += "tree " + tree + "\n";
  for (auto& p : parents) body += "parent " + p + "\n";
  body += "author " + (author.empty() ? self : author) + stamp;
  body += "committer " + (committer.empty() ? self : committer) + stamp;
  body += "\n";
  body += message;
  if (body.back() != '\n') body += "\n";
  return write_object("commit", span_of(body));
}

Result<GitCommit> Git::commit(const std::string& message) {
  ensure_index();
  if (auto r = may(R_Write); !r.ok()) return Result<GitCommit>(r.error());
  if (index_.empty()) return Result<GitCommit>(Status::Invalid, "nothing staged — `git add <path>` first");
  auto tree = write_tree_from_index();
  if (!tree.ok()) return Result<GitCommit>(tree.error());

  std::string parent;
  auto head = head_oid();
  if (head.ok()) parent = *head;

  uint64_t when = kernel_.host().wall_ns() / 1000000000ull;
  std::string who = author_name_ + " <" + author_email_ + ">";
  auto oid = commit_tree(*tree, parent.empty() ? std::vector<std::string>{} : std::vector<std::string>{parent},
                         message, who, who, when);
  if (!oid.ok()) return Result<GitCommit>(oid.error());

  std::string ref = head_detached_.empty() ? head_ref_ : std::string();
  if (!ref.empty()) {
    std::string dir = ref_path(ref);
    size_t slash = dir.rfind('/');
    if (slash != std::string::npos) kernel_.host().make_dir(dir.substr(0, slash));
    std::string line = *oid + "\n";
    if (auto r = kernel_.host().write_file(dir, span_of(line)); !r.ok()) return Result<GitCommit>(r.error());
  } else {
    head_detached_ = *oid;
    std::string line = *oid + "\n";
    if (auto r = kernel_.host().write_file(git_ + "/HEAD", span_of(line)); !r.ok()) return Result<GitCommit>(r.error());
  }

  GitCommit c;
  c.oid = *oid;
  c.tree = *tree;
  if (!parent.empty()) c.parents.push_back(parent);
  c.author = c.committer = who;
  c.author_time = c.committer_time = when;
  c.message = message;
  return c;
}

Result<std::vector<GitCommit>> Git::log(size_t max_entries) const {
  std::vector<GitCommit> out;
  auto head = head_oid();
  if (!head.ok()) return out;   // an unborn branch has no history, which is not an error
  std::string at = *head;
  while (!at.empty() && out.size() < max_entries) {
    auto c = read_commit(at);
    // The walk ends where this database ends. A shallow clone genuinely has a
    // parent it does not hold, and so does a repository whose older history is
    // packed — history this machine cannot read is history it does not have,
    // and reporting the first commit it could read is the truthful answer. An
    // unreadable HEAD is a different thing and still fails.
    if (!c.ok()) return out.empty() ? Result<std::vector<GitCommit>>(c.error()) : Result<std::vector<GitCommit>>(out);
    out.push_back(*c);
    at = c->parents.empty() ? std::string() : c->parents[0];
  }
  return out;
}

// git check-ref-format, the parts that matter for safety: a ref name becomes a path under .git, and
// refs arrive from remotes, so ".." or an absolute component must never reach the filesystem.
bool Git::valid_ref_name(const std::string& ref) {
  if (ref == "HEAD") return true;
  if (ref.rfind("refs/", 0) != 0 || ref.size() > 255) return false;
  if (ref.back() == '/' || ref.back() == '.') return false;
  if (ref.find("..") != std::string::npos || ref.find("//") != std::string::npos || ref.find("@{") != std::string::npos) return false;
  size_t start = 0;
  while (start <= ref.size()) {
    size_t end = ref.find('/', start);
    if (end == std::string::npos) end = ref.size();
    std::string comp = ref.substr(start, end - start);
    if (comp.empty() || comp.front() == '.') return false;
    if (comp.size() >= 5 && comp.compare(comp.size() - 5, 5, ".lock") == 0) return false;
    start = end + 1;
  }
  for (unsigned char c : ref) {
    if (c < 0x20 || c == 0x7f || c == ' ' || c == '~' || c == '^' || c == ':' || c == '?' || c == '*' || c == '[' || c == '\\') return false;
  }
  return true;
}

Result<void> Git::delete_ref(const std::string& ref) {
  if (auto r = may(R_Write); !r.ok()) return r;
  if (!valid_ref_name(ref) || ref == "HEAD") return Result<void>(Status::Invalid, ref + ": not a deletable ref name");
  std::string path = ref_path(ref);
  if (kernel_.host().exists(path))
    if (auto r = kernel_.host().remove(path); !r.ok()) return r;
  // A packed copy would bring the ref straight back: drop its line (and the peeled "^" line after it).
  auto packed = kernel_.host().read_file(common_ + "/packed-refs");
  if (!packed.ok()) return Ok();
  std::string text = string_of(ByteSpan(packed->data(), packed->size())), kept;
  bool dropped = false, skip_peel = false;
  for (auto& line : split_lines(text)) {
    if (!line.empty() && line[0] == '^') { if (!skip_peel) kept += line + "\n"; continue; }
    skip_peel = line.size() > 41 && trim(line.substr(41)) == ref && line[40] == ' ';
    if (skip_peel) { dropped = true; continue; }
    kept += line + "\n";
  }
  if (!dropped) return Ok();
  packed_refs_loaded_ = false;
  return kernel_.host().write_file(common_ + "/packed-refs", span_of(kept));
}

// Many refs in one file, as a clone writes them (git's initial ref transaction goes to packed-refs): sorted, an
// annotated tag followed by the object it peels to, with git's header. Loose files of the same names are left alone.
Result<void> Git::write_packed_refs(const std::vector<std::pair<std::string, std::string>>& refs) {
  if (auto r = may(R_Write); !r.ok()) return r;
  std::map<std::string, std::string> all;
  for (const auto& [name, oid] : packed_refs()) all.emplace(name, oid);
  for (const auto& [name, oid] : refs) {
    if (!valid_ref_name(name)) return Result<void>(Status::Invalid, name + ": not a valid ref name");
    if (oid.size() != 40) return Result<void>(Status::Invalid, name + ": " + oid + " is not an object id");
    all[name] = oid;
  }
  std::string text = "# pack-refs with: peeled fully-peeled sorted \n";
  for (const auto& [name, oid] : all) {
    text += oid;
    text += ' ';
    text += name;
    text += '\n';
    // fully peeled: through every tag to what is not one
    std::string cur = oid;
    bool tagged = false;
    for (int guard = 0; guard < 16; ++guard) {
      auto info = object_info(cur);
      if (!info.ok() || info->first != "tag") break;
      auto t = read_object(cur);
      if (!t.ok()) break;
      const std::string body((const char*)t->payload.data(), t->payload.size());
      if (body.rfind("object ", 0) != 0 || body.size() < 47) break;
      cur = body.substr(7, 40);
      tagged = true;
    }
    if (tagged) { text += '^'; text += cur; text += '\n'; }
  }
  packed_refs_loaded_ = false;
  auto& h = kernel_.host();
  return settings_.fsync_refs ? h.write_file(common_ + "/packed-refs", span_of(text)) : h.write_file_unsynced(common_ + "/packed-refs", span_of(text));
}

Result<void> Git::update_ref(const std::string& ref, const std::string& oid) {
  return write_ref_locked(ref, oid, nullptr);
}

Result<void> Git::compare_and_swap_ref(const std::string& ref, const std::string& expected, const std::string& oid) {
  return write_ref_locked(ref, oid, &expected);
}

Result<void> Git::write_ref_locked(const std::string& ref, const std::string& oid, const std::string* expected) {
  if (auto r = may(R_Write); !r.ok()) return r;
  if (!valid_ref_name(ref)) return Result<void>(Status::Invalid, ref + ": not a valid ref name");
  if (oid.size() != 40) return Result<void>(Status::Invalid, ref + ": " + oid + " is not an object id");
  std::string path = ref_path(ref);
  size_t slash = path.rfind('/');
  if (slash != std::string::npos) {
    // refs/heads/topic/name needs every directory on the way down.
    std::string dir = path.substr(0, slash);
    std::string acc;
    for (size_t i = 0; i <= dir.size(); ++i) {
      if (i == dir.size() || dir[i] == '/') {
        acc = dir.substr(0, i);
        if (!acc.empty() && acc != "/") kernel_.host().make_dir(acc);
      }
    }
  }
  std::string line = oid + "\n";
  auto& h = kernel_.host();
  const std::string lock = path + ".lock";
  for (int attempt = 0;; ++attempt) {
    auto taken = settings_.fsync_refs ? h.create_exclusive(lock, span_of(line)) : h.create_exclusive_unsynced(lock, span_of(line));
    if (taken.ok()) break;
    if (taken.error().status != Status::Conflict) return taken;
    if (attempt >= 400) return Result<void>(Status::Conflict, "Unable to create '" + lock + "': File exists. Another git process seems to be running in this repository");
#if defined(__wasi__)
    // one thread, one writer: a lock that exists was left by a writer that stopped part way
    return Result<void>(Status::Conflict, "Unable to create '" + lock + "': File exists");
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
#endif
  }
  if (expected) {
    auto cur = ref_oid(ref);
    const std::string now = cur.ok() ? *cur : std::string();
    if (now != *expected) {
      (void)h.remove(lock);
      return Result<void>(Status::Conflict, ref + " is at " + (now.empty() ? std::string("nothing") : now) + ", not " +
                                                (expected->empty() ? std::string("nothing") : *expected));
    }
  }
  auto moved = h.rename_file(lock, path);
  if (!moved.ok()) (void)h.remove(lock);
  return moved;
}

Result<void> Git::compare_and_swap_refs(const std::vector<RefSwap>& swaps_in) {
  if (auto r = may(R_Write); !r.ok()) return r;
  std::vector<RefSwap> swaps = swaps_in;
  std::sort(swaps.begin(), swaps.end(), [](const RefSwap& a, const RefSwap& b) { return a.ref < b.ref; });
  auto& h = kernel_.host();
  std::vector<std::string> held;
  auto release = [&] { for (const auto& l : held) (void)h.remove(l); };
  for (const auto& s : swaps) {
    if (!valid_ref_name(s.ref)) { release(); return Result<void>(Status::Invalid, s.ref + ": not a valid ref name"); }
    if (s.oid.size() != 40) { release(); return Result<void>(Status::Invalid, s.ref + ": " + s.oid + " is not an object id"); }
    const std::string lock = ref_path(s.ref) + ".lock";
    const std::string line = s.oid + "\n";
    {   // every directory on the way down (refs/changes/heads/... for a change seen first here)
      const std::string dir = lock.substr(0, lock.rfind('/'));
      for (size_t i = 1; i <= dir.size(); ++i)
        if (i == dir.size() || dir[i] == '/') h.make_dir(dir.substr(0, i));
    }
    // no waiting here: an optimistic writer that meets another writer starts again rather than queueing behind it
    if (auto taken = settings_.fsync_refs ? h.create_exclusive(lock, span_of(line)) : h.create_exclusive_unsynced(lock, span_of(line)); !taken.ok()) {
      release();
      return Result<void>(Status::Conflict, s.ref + " is being written by another process");
    }
    held.push_back(lock);
    auto cur = ref_oid(s.ref);
    const std::string now = cur.ok() ? *cur : std::string();
    if (now != s.expected) {
      release();
      return Result<void>(Status::Conflict, s.ref + " moved (it is at " + (now.empty() ? std::string("nothing") : now.substr(0, 12)) + ")");
    }
  }
  for (const auto& s : swaps) {
    if (auto r = h.rename_file(ref_path(s.ref) + ".lock", ref_path(s.ref)); !r.ok()) { release(); return r; }
    if (s.ref == "HEAD") { head_detached_ = s.oid; head_ref_.clear(); }
  }
  return Ok();
}

Result<void> Git::set_remote(const std::string& name, const std::string& url) {
  if (auto r = may(R_Write); !r.ok()) return r;
  // Both values are written into .git/config verbatim, so anything that could end the value or the
  // section header (newline, quote, backslash, NUL) would let a caller inject configuration.
  auto bad = [](const std::string& v, const char* extra) {
    for (unsigned char c : v) {
      if (c < 0x20 || c == 0x7f || c == '\\' || std::strchr(extra, c)) return true;
    }
    return false;
  };
  if (name.empty() || bad(name, "\" []/ ")) return Result<void>(Status::Invalid, "invalid remote name");
  if (url.empty() || bad(url, "\"")) return Result<void>(Status::Invalid, "invalid remote url");
  auto existing = kernel_.host().read_file(common_ + "/config");
  std::string text = existing.ok() ? string_of(ByteSpan(existing->data(), existing->size())) : std::string();
  const std::string header = "[remote \"" + name + "\"]";
  size_t at = text.find(header);
  if (at == std::string::npos) {
    text += header + "\n\turl = " + url + "\n\tfetch = +refs/heads/*:refs/remotes/" + name + "/*\n";
  } else {
    // Already recorded: update its url in place (a rotated URL must take effect).
    size_t section_end = text.find("\n[", at + header.size());
    if (section_end == std::string::npos) section_end = text.size();
    size_t url_at = text.find("\turl = ", at);
    if (url_at != std::string::npos && url_at < section_end) {
      size_t line_end = text.find('\n', url_at);
      if (line_end == std::string::npos) line_end = text.size();
      text.replace(url_at, line_end - url_at, "\turl = " + url);
    } else {
      text.insert(at + header.size(), "\n\turl = " + url);
    }
  }
  config_loaded_ = false;
  return kernel_.host().write_file(common_ + "/config", span_of(text));
}

const std::string* Git::config_text() const {
  auto& h = kernel_.host();
  const std::string path = common_ + "/config";
  auto st = h.stat(path);
  if (!st.ok()) { config_loaded_ = false; return nullptr; }
  if (config_loaded_ && st->mtime_ns == config_mtime_ && st->size == config_size_ && st->mtime_ns != 0) return &config_text_;
  auto f = h.read_file(path);
  if (!f.ok()) { config_loaded_ = false; return nullptr; }
  config_text_ = string_of(ByteSpan(f->data(), f->size()));
  config_mtime_ = st->mtime_ns;
  config_size_ = st->size;
  config_loaded_ = true;
  return &config_text_;
}

Result<std::string> Git::remote_url(const std::string& name) const {
  if (auto r = may(R_Read); !r.ok()) return Result<std::string>(r.error());
  const std::string* cfg = config_text();
  if (!cfg) return Result<std::string>(Status::NotFound, git_ + "/config: no configuration to read");
  bool in_section = false;
  for (auto& line : split_lines(*cfg)) {
    std::string t = trim(line);
    if (!t.empty() && t[0] == '[') { in_section = t == "[remote \"" + name + "\"]"; continue; }
    if (!in_section) continue;
    size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    if (trim(t.substr(0, eq)) == "url") return trim(t.substr(eq + 1));
  }
  return Result<std::string>(Status::NotFound, "this repository records no remote named " + name);
}

Result<void> Git::set_head(const std::string& ref) {
  if (auto r = may(R_Write); !r.ok()) return r;
  std::string line = "ref: " + ref + "\n";
  auto& h = kernel_.host();
  if (auto r = settings_.fsync_refs ? h.write_file(git_ + "/HEAD", span_of(line)) : h.write_file_unsynced(git_ + "/HEAD", span_of(line)); !r.ok()) return r;
  head_ref_ = ref;
  head_detached_.clear();
  return Ok();
}

Result<void> Git::set_head_detached(const std::string& oid) {
  if (auto r = may(R_Write); !r.ok()) return r;
  if (oid.size() != 40) return Result<void>(Status::Invalid, oid + " is not an object id");
  if (auto r = write_ref_locked("HEAD", oid, nullptr); !r.ok()) return r;
  head_detached_ = oid;
  head_ref_.clear();
  return Ok();
}

Result<std::vector<GitRemoteRefPair>> Git::branches() const {
  auto all = refs();
  if (!all.ok()) return all;
  std::vector<GitRemoteRefPair> out;
  for (auto& r : *all)
    if (r.name.rfind("refs/heads/", 0) == 0) out.push_back(GitRemoteRefPair{r.name.substr(11), r.oid});
  return out;
}

Result<void> Git::create_branch(const std::string& name, const std::string& at) {
  if (name.empty() || name.find(' ') != std::string::npos || name[0] == '-')
    return Result<void>(Status::Invalid, "'" + name + "' is not a branch name");
  if (ref_oid("refs/heads/" + name).ok())
    return Result<void>(Status::Conflict, "this repository already has a branch " + name);
  auto oid = at.empty() ? head_oid() : resolve(at);
  if (!oid.ok())
    return Result<void>(oid.status() == Status::NotFound && at.empty()
                            ? Error{Status::NotFound, "nothing committed yet: a branch needs a commit to point at"}
                            : oid.error());
  return update_ref("refs/heads/" + name, *oid);
}

Result<void> Git::delete_branch(const std::string& name) {
  if (auto r = may(R_Write); !r.ok()) return r;
  if (branch() == name) return Result<void>(Status::Conflict, "cannot delete " + name + ": it is the branch HEAD is on");
  if (!ref_oid("refs/heads/" + name).ok()) return Result<void>(Status::NotFound, "no branch " + name);
  return delete_ref("refs/heads/" + name);
}

Result<size_t> Git::switch_to(const std::string& rev, bool discard) {
  if (auto r = may(R_Write); !r.ok()) return Result<size_t>(r.error());
  auto target = resolve(rev);
  if (!target.ok()) return Result<size_t>(target.error());
  auto tree = tree_of(*target);
  if (!tree.ok()) return Result<size_t>(tree.error());
  // Local changes that the move does not touch come along, as in git; ones it would overwrite stop it.
  auto moved = checkout_tree(*tree, discard);
  if (!moved.ok()) return moved;
  bool is_branch = ref_oid("refs/heads/" + rev).ok();
  if (is_branch) {
    if (auto r = set_head("refs/heads/" + rev); !r.ok()) return Result<size_t>(r.error());
  } else {
    if (auto r = set_head_detached(*target); !r.ok()) return Result<size_t>(r.error());
  }
  return moved;
}

Result<std::string> Git::tree_of(const std::string& oid) const {
  auto o = read_object(oid);
  if (!o.ok()) return Result<std::string>(o.error());
  if (o->type == "tree") return o->oid;
  if (o->type == "tag") {
    std::string body = string_of(ByteSpan(o->payload.data(), o->payload.size()));
    if (body.rfind("object ", 0) == 0) return tree_of(body.substr(7, 40));
  }
  if (o->type != "commit") return Result<std::string>(Status::Invalid, oid + " is a " + o->type + ", not a commit or tree");
  auto c = read_commit(o->oid);
  if (!c.ok()) return Result<std::string>(c.error());
  return c->tree;
}

Result<GitIndexEntry> Git::write_worktree_file(const std::string& rel, const std::string& oid, uint32_t mode) {
  auto& h = kernel_.host();
  std::string path = join_path(work_, rel);
  if (mode != 0160000 && !in_sparse(rel)) {
    // outside the sparse checkout: not on disk, the index entry stands for it (and its blob is not even read, so a
    // partial clone never fetches it)
    if (auto st = h.stat(path); st.ok() && !st->is_dir) (void)h.remove(path);
    GitIndexEntry e{rel, oid, mode, 0, 0};
    e.skip_worktree = true;
    return e;
  }
  size_t slash = path.rfind('/');
  // The file's directory, made once per checkout: every prefix of it was
  // stat'ed and mkdir'ed for every file (45 mkdirs a file on a 3,000-file
  // tree, more of a clone than the network).
  auto ensure_dir = [&](const std::string& dir) {
    if (worktree_dirs_made_.count(dir)) return;
    // A file standing where a directory must go is in the way (the index said so, or it is untracked).
    for (size_t i = 0; i <= dir.size(); ++i) {
      if (i == dir.size() || dir[i] == '/') {
        std::string acc = dir.substr(0, i);
        if (acc.empty() || acc == "/" || (acc.size() == 2 && acc[1] == ':')) continue;
        if (worktree_dirs_made_.count(acc)) continue;
        auto st = h.stat(acc);
        if (st.ok() && !st->is_dir) (void)h.remove(acc);
        (void)h.make_dir(acc);
        worktree_dirs_made_.insert(acc);
      }
    }
  };
  if (slash != std::string::npos) ensure_dir(path.substr(0, slash));
  if (mode == 0160000) {
    // A gitlink names another repository's commit, and its content is not in this database. git still
    // puts the DIRECTORY there: an uninitialised submodule is an empty directory, not a missing path.
    (void)h.make_dir(path);
    return GitIndexEntry{rel, oid, mode, 0, 0};
  }
  auto blob = read_object(oid);
  if (!blob.ok()) return Result<GitIndexEntry>(blob.error());
  if (auto st = h.stat(path); st.ok() && (st->is_dir || st->is_link)) (void)h.remove(path);
  bool written = false;
  if (mode == 0120000 && settings_.symlinks) {
    std::string target = string_of(ByteSpan(blob->payload.data(), blob->payload.size()));
    auto link = h.make_symlink(target, path);
    if (link.ok()) written = true;
    else if (link.status() != Status::Unsupported) return Result<GitIndexEntry>(link.error());
    // No links here: the target's text is the file, which is what git does with core.symlinks false.
  }
  if (!written) {
    // Not forced to the medium: git does not fsync working-tree files either
    // (an fsync per checked-out file was 3,000 of them on a clone).
    auto r = h.write_file_plain(path, ByteSpan(blob->payload.data(), blob->payload.size()));
    if (!r.ok() && slash != std::string::npos) {   // the directory went since it was made: once more, made again
      worktree_dirs_made_.clear();
      ensure_dir(path.substr(0, slash));
      r = h.write_file_plain(path, ByteSpan(blob->payload.data(), blob->payload.size()));
    }
    if (!r.ok()) return Result<GitIndexEntry>(r.error());
    if (mode == 0100755) h.set_access(path, hal::Host::FileAccess::Executable);
  }
  GitIndexEntry e{rel, oid, mode, blob->payload.size(), 0};
  if (auto st = h.stat(path); st.ok()) e.mtime_ns = st->mtime_ns;
  return e;
}

Result<size_t> Git::checkout_tree(const std::string& tree_oid, bool force) {
  ensure_index();
  if (auto r = may(R_Write); !r.ok()) return Result<size_t>(r.error());
  worktree_dirs_made_.clear();
  if (bare_) return Result<size_t>(Status::Invalid, "a bare repository has no work tree to check out into");
  auto& h = kernel_.host();
  std::vector<GitIndexEntry> target;
  if (auto r = flatten_tree(tree_oid, "", target); !r.ok()) return Result<size_t>(r.error());
  std::map<std::string, const GitIndexEntry*> now;
  for (auto& e : index_) now[e.path] = &e;
  std::set<std::string> wanted;
  for (auto& e : target) wanted.insert(e.path);

  // Refuse before touching anything: what git calls "Your local changes would be overwritten" and
  // "untracked working tree files would be overwritten".
  if (!force) {
    std::vector<std::string> dirty, in_way;
    for (auto& e : target) {
      auto n = now.find(e.path);
      if (n == now.end()) {
        auto st = h.stat(join_path(work_, e.path));
        if (st.ok() && !st->is_dir) {
          GitIndexEntry probe{e.path, e.oid, e.mode};
          if (!worktree_matches(probe)) in_way.push_back(e.path);
        }
      } else if ((n->second->oid != e.oid || n->second->mode != e.mode) && !worktree_matches(*n->second)) {
        dirty.push_back(e.path);
      }
    }
    for (auto& [p, e] : now)
      if (!wanted.count(p) && !worktree_matches(*e)) dirty.push_back(p);
    if (!unmerged_.empty()) return Result<size_t>(Status::Conflict, "you need to resolve your current index first");
    if (!dirty.empty()) {
      std::string list;
      for (auto& d : dirty) list += "\t" + d + "\n";
      return Result<size_t>(Status::Conflict, "Your local changes to the following files would be overwritten by checkout:\n" + list +
                                                  "Please commit your changes or stash them before you switch branches.");
    }
    if (!in_way.empty()) {
      std::string list;
      for (auto& d : in_way) list += "\t" + d + "\n";
      return Result<size_t>(Status::Conflict, "The following untracked working tree files would be overwritten by checkout:\n" + list +
                                                  "Please move or remove them before you switch branches.");
    }
  }

  // Gone from the target: remove, and any directory that leaves empty.
  for (auto& [p, e] : now) {
    if (wanted.count(p)) continue;
    std::string full = join_path(work_, p);
    if (e->mode == 0160000) continue;    // never delete another repository's files
    (void)h.remove(full);
    for (size_t s = p.rfind('/'); s != std::string::npos && s > 0; s = p.rfind('/', s - 1)) {
      auto names = h.list_dir(join_path(work_, p.substr(0, s)));
      bool empty = names.ok();
      if (names.ok()) for (auto& n : *names) if (n != "." && n != "..") { empty = false; break; }
      if (!empty || !h.remove(join_path(work_, p.substr(0, s))).ok()) break;
    }
  }
  if (force) for (auto& u : unmerged_) if (!wanted.count(u.path)) (void)h.remove(join_path(work_, u.path));

  std::vector<GitIndexEntry> next;
  size_t written = 0;
  if (missing_hook_) {
    // a partial clone fetches what it is about to write in one go, not one object per file
    std::vector<std::string> need;
    for (auto& e : target) {
      if (e.mode == 0160000 || !in_sparse(e.path)) continue;
      auto n = now.find(e.path);
      if (n != now.end() && n->second->oid == e.oid) continue;
      need.push_back(e.oid);
    }
    prefetch(need);
  }
  for (auto& e : target) {
    auto n = now.find(e.path);
    if (n != now.end() && n->second->oid == e.oid && n->second->mode == e.mode) {
      // Already staged as the target has it: keep whatever the work tree holds (clean, or a local change
      // being carried across), unless forced back to the target.
      if (!force || worktree_matches(*n->second)) { next.push_back(*n->second); continue; }
    }
    auto w = write_worktree_file(e.path, e.oid, e.mode);
    if (!w.ok()) return Result<size_t>(w.error());
    next.push_back(*w);
    ++written;
  }
  index_ = std::move(next);
  unmerged_.clear();
  if (auto r = save_index(); !r.ok()) return Result<size_t>(r.error());
  return written;
}

// Write out every path the commit's tree names, then record exactly those paths
// in the index so `status` on a fresh clone is clean rather than "everything is
// untracked".
Result<size_t> Git::checkout(const std::string& commit_oid) {
  auto tree = tree_of(commit_oid);
  if (!tree.ok()) return Result<size_t>(tree.error());
  return checkout_tree(*tree, true);
}

Result<void> Git::flatten_tree(const std::string& oid, const std::string& prefix,
                               std::vector<GitIndexEntry>& out) const {
  auto ents = read_tree(oid);
  if (!ents.ok()) return Result<void>(ents.error());
  for (auto& e : *ents) {
    std::string p = prefix.empty() ? e.name : prefix + "/" + e.name;
    if (e.is_dir()) { if (auto r = flatten_tree(e.oid, p, out); !r.ok()) return r; }
    else out.push_back(GitIndexEntry{p, e.oid, e.mode});
  }
  return Ok();
}

Result<GitStatus> Git::status() const {
  ensure_index();
  GitStatus st;
  st.branch = branch();
  st.detached = !head_detached_.empty();

  std::vector<GitIndexEntry> head_files;
  auto head = head_oid();
  if (head.ok()) {
    st.has_commits = true;
    auto c = read_commit(*head);
    if (!c.ok()) return Result<GitStatus>(c.error());
    if (auto r = flatten_tree(c->tree, "", head_files); !r.ok()) return Result<GitStatus>(r.error());
  }

  std::map<std::string, const GitIndexEntry*> in_head, in_index;
  for (auto& e : head_files) in_head[e.path] = &e;
  for (auto& e : index_) in_index[e.path] = &e;
  std::set<std::string> conflicted;
  for (auto& u : unmerged_) conflicted.insert(u.path);
  st.unmerged.assign(conflicted.begin(), conflicted.end());

  // HEAD → index (an unmerged path is reported as unmerged, not as staged)
  for (auto& e : index_) {
    auto h = in_head.find(e.path);
    if (h == in_head.end()) st.staged.push_back({e.path, GitChange::Added});
    else if (h->second->oid != e.oid || h->second->mode != e.mode) st.staged.push_back({e.path, GitChange::Modified});
  }
  for (auto& h : head_files)
    if (!in_index.count(h.path) && !conflicted.count(h.path)) st.staged.push_back({h.path, GitChange::Deleted});

  if (bare_) return Result<GitStatus>(Status::Invalid, "this operation must be run in a work tree");
  // index → work tree
  auto wt = worktree_changes();
  if (!wt.ok()) return Result<GitStatus>(wt.error());
  st.worktree = std::move(*wt);

  auto un = untracked(false);
  if (un.ok()) for (auto& f : *un) st.untracked.push_back({f, GitChange::Untracked});

  auto by_path = [](const GitStatusEntry& a, const GitStatusEntry& b) { return a.path < b.path; };
  std::sort(st.staged.begin(), st.staged.end(), by_path);
  std::sort(st.worktree.begin(), st.worktree.end(), by_path);
  std::sort(st.untracked.begin(), st.untracked.end(), by_path);
  return st;
}

Result<void> Git::rm(const std::string& path, bool cached) {
  ensure_index();
  if (auto r = may(R_Write); !r.ok()) return r;
  std::string p = clean_path(path);
  std::string rel = p;
  if (work_ != "." && rel.rfind(work_ + "/", 0) == 0) rel = rel.substr(work_.size() + 1);
  else if (rel == work_) rel = "";
  if (rel == ".") rel = "";

  bool found = false;
  auto it = std::remove_if(index_.begin(), index_.end(), [&](const GitIndexEntry& e) {
    if (e.path == rel || (!rel.empty() && e.path.rfind(rel + "/", 0) == 0)) {
      found = true;
      return true;
    }
    return false;
  });
  if (!found) return Result<void>(Status::NotFound, path + ": pathspec did not match any files");
  index_.erase(it, index_.end());
  if (auto r = save_index(); !r.ok()) return r;

  if (!cached) {
    std::string full = join_path(work_, rel);
    if (kernel_.host().exists(full)) {
      (void)kernel_.host().remove(full);
    }
  }
  return Ok();
}

Result<void> Git::reset_index(const std::string& commit_oid) {
  ensure_index();
  if (auto r = may(R_Write); !r.ok()) return r;
  std::string oid = commit_oid;
  if (oid.empty()) {
    auto head = head_oid();
    if (!head.ok()) {
      index_.clear();
      return save_index();
    }
    oid = *head;
  } else {
    auto resolved = resolve(oid);
    if (!resolved.ok()) return Result<void>(resolved.error());
    oid = *resolved;
  }
  auto c = read_commit(oid);
  if (!c.ok()) return Result<void>(c.error());
  std::vector<GitIndexEntry> entries;
  if (auto r = flatten_tree(c->tree, "", entries); !r.ok()) return r;
  for (auto& e : entries) {
    auto blob = read_object(e.oid);
    if (blob.ok()) e.size = blob->payload.size();
  }
  index_ = entries;
  return save_index();
}

Result<void> Git::reset_path(const std::string& path, const std::string& commit_oid) {
  ensure_index();
  if (auto r = may(R_Write); !r.ok()) return r;
  std::string p = clean_path(path);
  std::string rel = p;
  if (work_ != "." && rel.rfind(work_ + "/", 0) == 0) rel = rel.substr(work_.size() + 1);
  else if (rel == work_) rel = "";
  if (rel == ".") rel = "";

  std::string oid = commit_oid;
  std::vector<GitIndexEntry> commit_entries;
  if (oid.empty()) {
    auto head = head_oid();
    if (head.ok()) oid = *head;
  } else {
    auto res = resolve(oid);
    if (res.ok()) oid = *res;
  }
  if (!oid.empty()) {
    auto c = read_commit(oid);
    if (c.ok()) (void)flatten_tree(c->tree, "", commit_entries);
  }

  auto it = std::remove_if(index_.begin(), index_.end(), [&](const GitIndexEntry& e) {
    return (e.path == rel || (!rel.empty() && e.path.rfind(rel + "/", 0) == 0));
  });
  index_.erase(it, index_.end());

  for (const auto& ce : commit_entries) {
    if (ce.path == rel || (!rel.empty() && ce.path.rfind(rel + "/", 0) == 0)) {
      GitIndexEntry ne = ce;
      auto blob = read_object(ne.oid);
      if (blob.ok()) ne.size = blob->payload.size();
      index_.push_back(ne);
    }
  }
  return save_index();
}

Result<std::string> Git::get_config(const std::string& key) const {
  if (auto r = may(R_Read); !r.ok()) return Result<std::string>(r.error());
  const std::string* cfg = config_text();
  if (!cfg) return Result<std::string>(Status::NotFound, "config not found");
  const std::string& text = *cfg;

  std::string sec, subsec, var;
  size_t dot1 = key.find('.');
  if (dot1 == std::string::npos) return Result<std::string>(Status::Invalid, "key does not contain a section");
  sec = key.substr(0, dot1);
  size_t dot2 = key.rfind('.');
  if (dot2 != dot1) {
    subsec = key.substr(dot1 + 1, dot2 - (dot1 + 1));
    var = key.substr(dot2 + 1);
  } else {
    var = key.substr(dot1 + 1);
  }

  std::string target_header = subsec.empty() ? "[" + sec + "]" : "[" + sec + " \"" + subsec + "\"]";
  for (auto& c : target_header) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  bool in_sec = false;
  for (const auto& raw_line : split_lines(text)) {
    std::string l = trim(raw_line);
    if (l.empty() || l[0] == '#' || l[0] == ';') continue;
    if (l.front() == '[' && l.back() == ']') {
      std::string low = l;
      for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      in_sec = (low == target_header);
      continue;
    }
    if (!in_sec) continue;
    size_t eq = l.find('=');
    if (eq == std::string::npos) continue;
    std::string k = trim(l.substr(0, eq));
    std::string low_k = k;
    for (auto& c : low_k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string low_var = var;
    for (auto& c : low_var) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (low_k == low_var) {
      return trim(l.substr(eq + 1));
    }
  }
  return Result<std::string>(Status::NotFound, "key not found: " + key);
}

Result<void> Git::set_config(const std::string& key, const std::string& value) {
  if (auto r = may(R_Write); !r.ok()) return r;
  std::string sec, subsec, var;
  size_t dot1 = key.find('.');
  if (dot1 == std::string::npos) return Result<void>(Status::Invalid, "key does not contain a section");
  sec = key.substr(0, dot1);
  size_t dot2 = key.rfind('.');
  if (dot2 != dot1) {
    subsec = key.substr(dot1 + 1, dot2 - (dot1 + 1));
    var = key.substr(dot2 + 1);
  } else {
    var = key.substr(dot1 + 1);
  }

  std::string target_header = subsec.empty() ? "[" + sec + "]" : "[" + sec + " \"" + subsec + "\"]";
  auto existing = kernel_.host().read_file(common_ + "/config");
  std::string text = existing.ok() ? string_of(ByteSpan(existing->data(), existing->size())) : "";

  std::vector<std::string> lines = split_lines(text);
  std::vector<std::string> out;
  bool found_sec = false, found_var = false;
  bool in_sec = false;

  for (size_t i = 0; i < lines.size(); ++i) {
    std::string l = trim(lines[i]);
    if (l.size() >= 2 && l.front() == '[' && l.back() == ']') {
      if (in_sec && !found_var) {
        out.push_back("\t" + var + " = " + value);
        found_var = true;
      }
      std::string low = l;
      for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      std::string target_low = target_header;
      for (auto& c : target_low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      in_sec = (low == target_low);
      if (in_sec) found_sec = true;
      out.push_back(lines[i]);
      continue;
    }
    if (in_sec) {
      size_t eq = l.find('=');
      if (eq != std::string::npos) {
        std::string k = trim(l.substr(0, eq));
        std::string low_k = k;
        for (auto& c : low_k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string low_var = var;
        for (auto& c : low_var) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low_k == low_var) {
          out.push_back("\t" + var + " = " + value);
          found_var = true;
          continue;
        }
      }
    }
    out.push_back(lines[i]);
  }
  if (in_sec && !found_var) {
    out.push_back("\t" + var + " = " + value);
    found_var = true;
  }
  if (!found_sec) {
    if (!out.empty() && !out.back().empty()) out.push_back("");
    out.push_back(target_header);
    out.push_back("\t" + var + " = " + value);
  }

  std::string new_text;
  for (const auto& ln : out) { new_text += ln; new_text += '\n'; }
  config_loaded_ = false;
  return kernel_.host().write_file(common_ + "/config", span_of(new_text));
}

Result<std::vector<std::pair<std::string, std::string>>> Git::list_config() const {
  if (auto r = may(R_Read); !r.ok()) return Result<std::vector<std::pair<std::string, std::string>>>(r.error());
  const std::string* cfg = config_text();
  if (!cfg) return std::vector<std::pair<std::string, std::string>>{};
  const std::string& text = *cfg;

  std::vector<std::pair<std::string, std::string>> res;
  std::string cur_sec, cur_subsec;
  for (const auto& raw_line : split_lines(text)) {
    std::string l = trim(raw_line);
    if (l.empty() || l[0] == '#' || l[0] == ';') continue;
    if (l.front() == '[' && l.back() == ']') {
      std::string inside = l.substr(1, l.size() - 2);
      size_t q = inside.find('"');
      if (q != std::string::npos) {
        cur_sec = trim(inside.substr(0, q));
        size_t q2 = inside.find('"', q + 1);
        cur_subsec = (q2 != std::string::npos) ? inside.substr(q + 1, q2 - (q + 1)) : "";
      } else {
        cur_sec = trim(inside);
        cur_subsec = "";
      }
      continue;
    }
    size_t eq = l.find('=');
    if (eq != std::string::npos && !cur_sec.empty()) {
      std::string k = trim(l.substr(0, eq));
      std::string v = trim(l.substr(eq + 1));
      std::string full_key = cur_subsec.empty() ? cur_sec + "." + k : cur_sec + "." + cur_subsec + "." + k;
      res.push_back({full_key, v});
    }
  }
  return res;
}

Result<std::string> Git::add_note(const std::string& notes_ref, const std::string& target,
                                  const std::string& content, const std::string& author) {
  if (target.empty()) return Result<std::string>(Status::Invalid, "target commit cannot be empty");
  auto target_res = resolve(target);
  if (!target_res.ok()) return Result<std::string>(target_res.error());
  std::string tgt_oid = *target_res;

  auto cur_note_commit_oid = ref_oid(notes_ref);
  std::vector<GitIndexEntry> entries;
  if (cur_note_commit_oid.ok()) {
    auto commit = read_commit(*cur_note_commit_oid);
    if (commit.ok()) (void)flatten_tree(commit->tree, "", entries);
  }

  ByteSpan payload(reinterpret_cast<const uint8_t*>(content.data()), content.size());
  auto blob_oid = write_object("blob", payload);
  if (!blob_oid.ok()) return Result<std::string>(blob_oid.error());

  bool found = false;
  std::string fanout_path;
  if (tgt_oid.size() > 2) fanout_path = tgt_oid.substr(0, 2) + "/" + tgt_oid.substr(2);
  for (auto& entry : entries) {
    if (entry.path == tgt_oid || entry.path == fanout_path) {
      entry.oid = *blob_oid;
      entry.size = content.size();
      found = true;
      break;
    }
  }
  if (!found) {
    GitIndexEntry entry;
    entry.path = tgt_oid;
    entry.oid = *blob_oid;
    entry.mode = 0100644;
    entry.size = content.size();
    entries.push_back(entry);
  }

  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
    return a.path < b.path;
  });

  auto tree_oid = write_tree_from_entries(entries);
  if (!tree_oid.ok()) return Result<std::string>(tree_oid.error());

  std::vector<std::string> parents;
  if (cur_note_commit_oid.ok()) parents.push_back(*cur_note_commit_oid);
  std::string msg = "Notes added by 'git notes add'\n\nNotes for " + tgt_oid + "\n";
  std::string who = author.empty() ? (author_name_ + " <" + author_email_ + ">") : author;
  auto note_commit = commit_tree(*tree_oid, parents, msg, who, who, 0);
  if (!note_commit.ok()) return Result<std::string>(note_commit.error());

  auto upd = update_ref(notes_ref, *note_commit);
  if (!upd.ok()) return Result<std::string>(upd.error());

  return note_commit;
}

Result<std::string> Git::read_note(const std::string& notes_ref, const std::string& target) const {
  auto target_res = resolve(target);
  if (!target_res.ok()) return Result<std::string>(target_res.error());
  std::string tgt_oid = *target_res;

  auto note_commit_oid = ref_oid(notes_ref);
  if (!note_commit_oid.ok()) return Result<std::string>(Status::NotFound, "notes ref not found: " + notes_ref);

  auto commit = read_commit(*note_commit_oid);
  if (!commit.ok()) return Result<std::string>(commit.error());

  std::vector<GitIndexEntry> entries;
  auto flat = flatten_tree(commit->tree, "", entries);
  if (!flat.ok()) return Result<std::string>(flat.error());

  std::string fanout_path;
  if (tgt_oid.size() > 2) fanout_path = tgt_oid.substr(0, 2) + "/" + tgt_oid.substr(2);

  std::string blob_oid;
  for (const auto& entry : entries) {
    if (entry.path == tgt_oid || entry.path == fanout_path) {
      blob_oid = entry.oid;
      break;
    }
  }
  if (blob_oid.empty()) return Result<std::string>(Status::NotFound, "no note found for " + tgt_oid);

  auto obj = read_object(blob_oid);
  if (!obj.ok()) return Result<std::string>(obj.error());
  return std::string(reinterpret_cast<const char*>(obj->payload.data()), obj->payload.size());
}

Result<std::vector<std::pair<std::string, std::string>>> Git::list_notes(const std::string& notes_ref) const {
  auto note_commit_oid = ref_oid(notes_ref);
  if (!note_commit_oid.ok()) return std::vector<std::pair<std::string, std::string>>{};
  auto commit = read_commit(*note_commit_oid);
  if (!commit.ok()) return std::vector<std::pair<std::string, std::string>>{};
  std::vector<GitIndexEntry> entries;
  if (!flatten_tree(commit->tree, "", entries).ok()) return std::vector<std::pair<std::string, std::string>>{};

  std::vector<std::pair<std::string, std::string>> res;
  for (const auto& e : entries) {
    std::string commit_id = e.path;
    if (commit_id.size() == 41 && commit_id[2] == '/') {
      commit_id.erase(2, 1);
    }
    res.push_back({e.oid, commit_id});
  }
  return res;
}

// ---------------------------------------------------------------------------
// which repository the console is in
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// fsck: every loose object, inflated and re-named, then every packed one
// ---------------------------------------------------------------------------

Result<GitFsckReport> Git::fsck() const {
  if (auto r = may(R_Read); !r.ok()) return Result<GitFsckReport>(r.error());
  GitFsckReport rep;
  auto& h = kernel_.host();
  auto fanout = h.list_dir(common_ + "/objects");
  if (!fanout.ok()) return Result<GitFsckReport>(fanout.error());
  for (auto& d : *fanout) {
    if (d.size() != 2) continue;                       // info/, pack/ are not fanout dirs
    bool hex = true;
    for (char ch : d) hex &= (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    if (!hex) continue;
    auto names = h.list_dir(common_ + "/objects/" + d);
    if (!names.ok()) continue;
    for (auto& n : *names) {
      std::string oid = d + n;
      ++rep.objects;
      auto raw = h.read_file(common_ + "/objects/" + d + "/" + n);
      if (!raw.ok()) { rep.broken.push_back(oid + ": unreadable (" + raw.error().str() + ")"); continue; }
      auto inflated = zlib_inflate(ByteSpan(raw->data(), raw->size()));
      if (!inflated.ok()) { rep.broken.push_back(oid + ": " + inflated.error().str()); continue; }
      const ByteBuffer& b = *inflated;
      size_t nul = 0;
      while (nul < b.size() && b[nul] != 0) ++nul;
      if (nul >= b.size()) { rep.broken.push_back(oid + ": no header terminator"); continue; }
      std::string header((const char*)b.data(), nul);
      size_t sp = header.find(' ');
      if (sp == std::string::npos) { rep.broken.push_back(oid + ": malformed header \"" + header + "\""); continue; }
      std::string type = header.substr(0, sp);
      uint64_t declared = parse_u64_dec(header, sp + 1, header.size());
      if (declared != b.size() - nul - 1) {
        rep.broken.push_back(oid + ": header says " + u64_str(declared) + " bytes, payload is " +
                             u64_str(b.size() - nul - 1));
        continue;
      }
      std::string got = hex20(crypto::sha1(ByteSpan(b.data(), b.size())));
      if (got != oid) { rep.broken.push_back(oid + ": content hashes to " + got); continue; }
      if (type == "blob") ++rep.blobs;
      else if (type == "tree") ++rep.trees;
      else if (type == "commit") ++rep.commits;
      else ++rep.other;
    }
  }
  // Packed objects (all of them after a `git gc`): each read back through its
  // delta chain and re-hashed as "<type> <size>\0<payload>", as git names it.
  load_pack_indexes();
  std::vector<std::string> packed;
  packed_ids_with_prefix("", packed);
  for (const auto& oid : packed) {
    ++rep.objects;
    auto obj = read_packed(oid);
    if (!obj.ok()) { rep.broken.push_back(oid + ": packed: " + obj.error().str()); continue; }
    std::string head = obj->type + " " + u64_str(obj->payload.size());
    ByteBuffer whole(head.begin(), head.end());
    whole.push_back(0);
    whole.insert(whole.end(), obj->payload.begin(), obj->payload.end());
    std::string got = hex20(crypto::sha1(ByteSpan(whole.data(), whole.size())));
    if (got != oid) { rep.broken.push_back(oid + ": packed content hashes to " + got); continue; }
    if (obj->type == "blob") ++rep.blobs;
    else if (obj->type == "tree") ++rep.trees;
    else if (obj->type == "commit") ++rep.commits;
    else ++rep.other;
  }
  return rep;
}

// ---------------------------------------------------------------------------
// The registry: the subsystem's kernel identity
// ---------------------------------------------------------------------------

namespace {
const uint8_t kRegMagic[8] = {'A', 'I', 'O', 'S', 'G', 'I', 'T', '1'};
bool is_registry(ByteSpan p) {
  return p.size() >= 8 && std::memcmp(p.data(), kRegMagic, 8) == 0;
}
}  // namespace

GitRegistry::GitRegistry(Surface& actor) : me_(actor) {}

std::string GitRegistry::registry_path() const { return me_.kernel().config().root + "/git/registry"; }

ByteBuffer GitRegistry::encode() const {
  Writer w;
  w.raw(ByteSpan(kRegMagic, 8));
  w.str(current_);
  w.u32((uint32_t)repos_.size());
  for (auto& r : repos_) {
    w.str(r.name); w.str(r.path); w.str(r.branch); w.str(r.head);
    w.u64(r.registered_ns); w.u64(r.commits);
  }
  return w.take();
}

void GitRegistry::decode(ByteSpan b) {
  if (!is_registry(b)) return;
  Reader r(b.subspan(8, b.size() - 8));
  std::string cur = string_of(r.bytes());
  uint32_t n = r.u32();
  std::vector<GitRepoRecord> repos;
  for (uint32_t i = 0; i < n && r.ok(); ++i) {
    GitRepoRecord rec;
    rec.name = string_of(r.bytes());
    rec.path = string_of(r.bytes());
    rec.branch = string_of(r.bytes());
    rec.head = string_of(r.bytes());
    rec.registered_ns = r.u64();
    rec.commits = r.u64();
    repos.push_back(rec);
  }
  if (!r.ok()) return;              // a damaged payload leaves what we had alone
  current_ = cur;
  repos_ = repos;
}

Result<void> GitRegistry::attach() {
  // Reaching the machine's repositories at all is storage authority.
  if (auto r = me_.may(ResourceRef{ResourceKind::StorageObject, {}}, R_Read); !r.ok())
    return Result<void>(Status::Denied, "entity " + me_.self().short_str() +
                                            " may not reach the git subsystem: " + r.error().str());

  // Already in this kernel? A registry that came back in a checkpoint image
  // belongs to the machine, whichever entity happens to hold it.
  for (auto& s : me_.kernel().states().list()) {
    if (!is_registry(ByteSpan(s.payload.data(), s.payload.size()))) continue;
    auto got = me_.state(s.id);                       // capability-checked read
    if (!got.ok()) return Result<void>(got.error());
    state_ = s.id;
    holder_ = s.owner;
    adopted_ = true;
    decode(ByteSpan(got->payload.data(), got->payload.size()));
    return Ok();
  }

  // Not here: seed from the durable copy the last process left under the root.
  auto disk = me_.kernel().host().read_file(registry_path());
  if (disk.ok()) decode(ByteSpan(disk->data(), disk->size()));
  ByteBuffer payload = encode();
  auto id = me_.create_state(StateKind::Persistent, ByteSpan(payload.data(), payload.size()));
  if (!id.ok()) return Result<void>(id.error());
  state_ = *id;
  holder_ = me_.self();
  adopted_ = false;
  return Ok();
}

Result<void> GitRegistry::commit_state() {
  ByteBuffer p = encode();
  if (!state_.is_null()) {
    if (auto r = me_.set_state(state_, ByteSpan(p.data(), p.size())); !r.ok()) return r;
  }
  me_.kernel().host().make_dir(me_.kernel().config().root + "/git");
  return me_.kernel().host().write_file(registry_path(), ByteSpan(p.data(), p.size()));
}

Result<void> GitRegistry::record(const GitRepoRecord& rec, bool make_current) {
  if (auto r = me_.may(ResourceRef{ResourceKind::StorageObject, {}}, R_Write); !r.ok())
    return Result<void>(Status::Denied, "entity " + me_.self().short_str() +
                                            " may not register a repository: " + r.error().str());
  bool found = false;
  for (auto& r : repos_) {
    if (r.path != rec.path) continue;
    uint64_t first = r.registered_ns;
    r = rec;
    if (first) r.registered_ns = first;
    found = true;
    break;
  }
  if (!found) repos_.push_back(rec);
  if (make_current) current_ = rec.path;
  return commit_state();
}

Result<void> GitRegistry::set_current(const std::string& path) {
  for (auto& r : repos_) {
    if (r.path != path) continue;
    current_ = path;
    return commit_state();
  }
  return Result<void>(Status::NotFound, path + " is not a repository of this machine — `git init` it first");
}

const GitRepoRecord* GitRegistry::find(const std::string& name_or_path) const {
  for (auto& r : repos_) if (r.path == name_or_path) return &r;
  for (auto& r : repos_) if (r.name == name_or_path) return &r;
  return nullptr;
}

Result<std::string> GitRegistry::current() {
  if (current_.empty())
    return Result<std::string>(Status::NotFound,
                               "no repository is registered with this kernel — run `git init <name>` first");
  if (!me_.kernel().host().exists(current_ + "/.git/HEAD"))
    return Result<std::string>(Status::NotFound,
                               "the registered repository " + current_ + " is gone from storage (no .git/HEAD)");
  return current_;
}

std::string git_repo_path(Kernel& kernel, const std::string& name_or_path) {
  if (name_or_path.empty()) return name_or_path;
  // "root/…" is the machine's own root, whatever it was mounted as, so a
  // repository over the kernel's own directories names itself the same way on
  // every machine.
  if (name_or_path.rfind("root/", 0) == 0) return kernel.config().root + "/" + name_or_path.substr(5);
  if (name_or_path.find('/') != std::string::npos || name_or_path[0] == '.' || name_or_path[0] == '~')
    return name_or_path;
  return kernel.config().root + "/repos/" + name_or_path;
}

std::string git_registry_line(Kernel& kernel) {
  // Inspection only: no state is created here, so `info` never mints identity.
  ByteBuffer payload;
  std::string held_by;
  for (auto& s : kernel.states().list()) {
    if (!is_registry(ByteSpan(s.payload.data(), s.payload.size()))) continue;
    payload = s.payload;
    held_by = " (kernel state " + s.id.short_str() + " held by " + s.owner.short_str() + ")";
    break;
  }
  if (payload.empty()) {
    auto disk = kernel.host().read_file(kernel.config().root + "/git/registry");
    if (disk.ok() && is_registry(ByteSpan(disk->data(), disk->size()))) {
      payload = *disk;
      held_by = " (durable copy under the root; no kernel state attached yet)";
    }
  }
  if (payload.empty()) return "no repositories registered";
  Reader r(ByteSpan(payload.data() + 8, payload.size() - 8));
  std::string cur = string_of(r.bytes());
  uint32_t n = r.u32();
  std::string branch, head;
  uint64_t commits = 0;
  for (uint32_t i = 0; i < n && r.ok(); ++i) {
    std::string name = string_of(r.bytes()), path = string_of(r.bytes());
    std::string b = string_of(r.bytes()), hd = string_of(r.bytes());
    r.u64();
    uint64_t c = r.u64();
    if (path == cur) { branch = b; head = hd; commits = c; }
  }
  std::string out = u64_str(n) + " repositor" + (n == 1 ? "y" : "ies") + " registered";
  if (!cur.empty()) {
    out += " · current " + cur;
    if (!branch.empty()) out += " @ " + branch;
    out += head.empty() ? " (unborn)" : " " + head.substr(0, 12);
    out += ", " + u64_str(commits) + " commit" + (commits == 1 ? "" : "s") + " by this machine";
  }
  return out + held_by;
}

}  // namespace aios::userland
