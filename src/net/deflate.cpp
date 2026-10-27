#include "aios/net/deflate.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "aios/net/gzip.hpp"

namespace aios::net {

namespace {

const uint16_t kDeflateLenBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const uint8_t  kDeflateLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const uint16_t kDeflateDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const uint8_t  kDeflateDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

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

size_t deflate_raw_append(ByteSpan data, int level, ByteBuffer& out) {
  const size_t before = out.size();
  BitOut bw{out};
  deflate_stream(bw, data, level);
  bw.align();
  return out.size() - before;
}

// ---------------------------------------------------------------------------
// inflate
//
// Built for speed the way libdeflate's decoder is, without it: a 64-bit bit buffer topped up eight bytes at a
// time, two-level decode tables whose entries carry a literal or a length's base and extra-bit count (one lookup a
// symbol), matches copied a word at a time where the distance allows, the output written through a pointer into a
// buffer sized once when the size is known (a store chunk, a link frame) and doubled when it is not. On chi's 1.65
// GB of ASQL table chunks the decoder this replaces ran at 403 MB/s against libdeflate's 1215.
// ---------------------------------------------------------------------------
namespace {

// Bits come out least significant first. refill() tops the buffer up to at least 56 bits while input lasts; the
// bits above `have` are the stream's own next bits (an eight-byte load counted as fewer bytes), so OR-ing the same
// bytes in again later changes nothing.
struct Bits {
  const uint8_t* d;
  size_t n;
  size_t pos = 0;      // bytes counted into `have`
  uint64_t buf = 0;
  int have = 0;
  void refill() {
    if (pos + 8 <= n) {
      uint64_t w;
      std::memcpy(&w, d + pos, 8);
      buf |= w << have;
      const int take = (63 - have) >> 3;
      pos += (size_t)take;
      have += take * 8;
    } else {
      while (have <= 56 && pos < n) { buf |= (uint64_t)d[pos++] << have; have += 8; }
    }
  }
  uint32_t peek(int k) const { return (uint32_t)(buf & ((1ull << k) - 1)); }
  bool take(int k, uint32_t* v) {   // false: the stream ends inside these bits
    if (k > have) return false;
    *v = (uint32_t)(buf & ((1ull << k) - 1));
    buf = k == 64 ? 0 : buf >> k;
    have -= k;
    return true;
  }
  bool drop(int k) { if (k > have) return false; buf >>= k; have -= k; return true; }
  // To the next byte boundary, whole bytes held given back: what follows is read from d at pos.
  void align() { const int r = have & 7; buf >>= r; have -= r; pos -= (size_t)(have >> 3); have = 0; buf = 0; }
  size_t consumed() const { return pos - (size_t)(have >> 3); }
};

// A decode table entry (uint32): bits 0-3 the code's length in bits (0: no code here), bit 4 a pointer to a
// subtable (bits 16-31 its first entry), bits 5-6 the kind, bits 8-11 extra bits to read, bits 16-31 the value
// (a literal byte, or the base of a length or distance).
enum : uint32_t { kSub = 1u << 4, kLit = 1u << 5, kBase = 2u << 5, kEob = 3u << 5, kKindMask = 3u << 5 };
constexpr int kLitBits = 10, kDistBits = 8;   // primary table widths; longer codes go through a subtable

const uint16_t kLenBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const uint8_t  kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const uint16_t kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const uint8_t  kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

struct Table {
  std::vector<uint32_t> e;   // the primary table, then the subtables
  int bits = 0;
};

// What symbol `sym` of an alphabet decodes to: for the literal/length alphabet a literal, the end of the block or a
// length; for the distance alphabet a distance. 0 for a symbol the format does not use (286, 287, 30, 31).
uint32_t sym_entry(bool litlen, int sym) {
  if (litlen) {
    if (sym < 256) return kLit | ((uint32_t)sym << 16);
    if (sym == 256) return kEob;
    if (sym <= 285) return kBase | ((uint32_t)kLenExtra[sym - 257] << 8) | ((uint32_t)kLenBase[sym - 257] << 16);
    return 0;
  }
  if (sym < 30) return kBase | ((uint32_t)kDistExtra[sym] << 8) | ((uint32_t)kDistBase[sym] << 16);
  return 0;
}

// A canonical Huffman code from its lengths. False for an over-subscribed set; an incomplete one is allowed (the
// format permits it for a distance code of one symbol): its missing codes stay 0, and a stream that uses one fails.
bool build(Table& t, const uint8_t* lengths, int n, bool litlen, int bits) {
  int count[16] = {0};
  for (int i = 0; i < n; ++i) count[lengths[i]]++;
  count[0] = 0;
  int left = 1;
  for (int len = 1; len < 16; ++len) { left = (left << 1) - count[len]; if (left < 0) return false; }
  uint32_t next[16];
  uint32_t code = 0;
  next[0] = 0;
  for (int len = 1; len < 16; ++len) { code = (code + (uint32_t)count[len - 1]) << 1; next[len] = code; }
  t.bits = bits;
  t.e.assign((size_t)1 << bits, 0);
  const int sub_bits = 15 - bits;
  for (int sym = 0; sym < n; ++sym) {
    const int len = lengths[sym];
    if (!len) continue;
    const uint32_t c = next[len]++;
    uint32_t r = 0;   // the code as the stream carries it: most significant bit first, so reversed
    for (int b = 0; b < len; ++b) r |= ((c >> b) & 1u) << (len - 1 - b);
    const uint32_t v = sym_entry(litlen, sym) | (uint32_t)len;
    if (len <= bits) {
      for (uint32_t i = r; i < ((uint32_t)1 << bits); i += (uint32_t)1 << len) t.e[i] = v;
      continue;
    }
    const uint32_t lo = r & (((uint32_t)1 << bits) - 1);
    if (!(t.e[lo] & kSub)) {   // this prefix's subtable: 2^(15-bits) entries, indexed by the next bits
      const size_t at = t.e.size();
      t.e.resize(at + ((size_t)1 << sub_bits), 0);
      t.e[lo] = kSub | ((uint32_t)at << 16);
    }
    const size_t base = t.e[lo] >> 16;
    const uint32_t hi = r >> bits;
    for (uint32_t i = hi; i < ((uint32_t)1 << sub_bits); i += (uint32_t)1 << (len - bits)) t.e[base + i] = v;
  }
  return true;
}

// The entry for the next code (and its length), or 0.
inline uint32_t lookup(const Table& t, uint64_t buf) {
  uint32_t e = t.e[buf & (((uint64_t)1 << t.bits) - 1)];
  if (e & kSub) e = t.e[(e >> 16) + ((buf >> t.bits) & (((uint64_t)1 << (15 - t.bits)) - 1))];
  return e;
}

const Table& fixed_litlen() {
  static const Table t = [] {
    uint8_t l[288];
    for (int i = 0; i < 144; ++i) l[i] = 8;
    for (int i = 144; i < 256; ++i) l[i] = 9;
    for (int i = 256; i < 280; ++i) l[i] = 7;
    for (int i = 280; i < 288; ++i) l[i] = 8;
    Table x;
    build(x, l, 288, true, kLitBits);
    return x;
  }();
  return t;
}
const Table& fixed_dist() {
  static const Table t = [] {
    uint8_t l[30];
    for (int i = 0; i < 30; ++i) l[i] = 5;
    Table x;
    build(x, l, 30, false, kDistBits);
    return x;
  }();
  return t;
}

}  // namespace

namespace {

// Where the decoder writes: [base, end) is the room it has; `o` the next byte. A growable output (a ByteBuffer
// whose final size is not known) moves when it grows; a fixed one (inflate_raw_exact: a store chunk, its size known)
// is written in place, never past `end`.
struct Out {
  uint8_t* base;
  uint8_t* o;
  uint8_t* end;
  ByteBuffer* vec = nullptr;   // growable: the buffer, its bytes from `start` on
  size_t start = 0;
  size_t max_out = SIZE_MAX;
  size_t produced() const { return (size_t)(o - base); }
  // Room for k more bytes (plus 16 of slack in a growable buffer for the word copies): false past max_out or past a
  // fixed buffer's end.
  bool room(size_t k) {
    if ((size_t)(end - o) >= k) return true;
    if (!vec) return false;
    const size_t have = produced();
    if (have + k > max_out) return false;
    size_t want = std::max(have + k, (size_t)(end - base) * 2);
    if (max_out != SIZE_MAX) want = std::min(want, max_out);
    vec->resize(start + want + 16);
    base = vec->data() + start;
    o = base + have;
    end = base + want;
    return true;
  }
};

const char* inflate_blocks(Bits& br, Out& out) {
  Table dyn_lit, dyn_dist;
  for (;;) {
    br.refill();
    uint32_t final_block = 0, type = 0;
    if (!br.take(1, &final_block) || !br.take(2, &type)) return "truncated deflate stream";

    if (type == 0) {   // stored
      br.align();
      if (br.pos + 4 > br.n) return "truncated stored block";
      const uint32_t len = (uint32_t)br.d[br.pos] | ((uint32_t)br.d[br.pos + 1] << 8);
      const uint32_t nlen = (uint32_t)br.d[br.pos + 2] | ((uint32_t)br.d[br.pos + 3] << 8);
      if ((len ^ 0xFFFFu) != nlen) return "stored block length check failed";
      br.pos += 4;
      if (br.pos + len > br.n) return "stored block runs past end";
      if (!out.room(len)) return "inflates past the size limit";
      std::memcpy(out.o, br.d + br.pos, len);
      out.o += len;
      br.pos += len;
      if (final_block) return nullptr;
      continue;
    }
    if (type == 3) return "reserved deflate block type";

    const Table* lit = &fixed_litlen();
    const Table* dist = &fixed_dist();
    if (type == 2) {   // dynamic: the code lengths, themselves Huffman coded
      uint32_t hlit, hdist, hclen;
      br.refill();
      if (!br.take(5, &hlit) || !br.take(5, &hdist) || !br.take(4, &hclen)) return "truncated deflate stream";
      hlit += 257; hdist += 1; hclen += 4;
      if (hlit > 286 || hdist > 30) return "too many length or distance codes";
      static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
      uint8_t cl[19] = {0};
      for (uint32_t i = 0; i < hclen; ++i) {
        br.refill();
        uint32_t v;
        if (!br.take(3, &v)) return "truncated deflate stream";
        cl[order[i]] = (uint8_t)v;
      }
      // The code-length code: symbols 0..18, at most 7 bits; a plain table of 128 with (symbol << 16) | length.
      uint32_t clt[128] = {0};
      {
        int count[16] = {0};
        for (int i = 0; i < 19; ++i) count[cl[i]]++;
        count[0] = 0;
        int left = 1;
        for (int len = 1; len < 16; ++len) { left = (left << 1) - count[len]; if (left < 0) return "bad code length code"; }
        uint32_t next[16]; uint32_t code = 0; next[0] = 0;
        for (int len = 1; len < 16; ++len) { code = (code + (uint32_t)count[len - 1]) << 1; next[len] = code; }
        for (int sym = 0; sym < 19; ++sym) {
          const int len = cl[sym];
          if (!len) continue;
          const uint32_t c = next[len]++;
          uint32_t r = 0;
          for (int b = 0; b < len; ++b) r |= ((c >> b) & 1u) << (len - 1 - b);
          for (uint32_t i = r; i < 128; i += (uint32_t)1 << len) clt[i] = ((uint32_t)sym << 16) | (uint32_t)len;
        }
      }
      uint8_t lengths[286 + 30] = {0};
      uint32_t n = 0;
      while (n < hlit + hdist) {
        br.refill();
        const uint32_t e = clt[br.peek(7)];
        const int len = (int)(e & 15);
        if (!len || !br.drop(len)) return "bad code length symbol";
        const uint32_t sym = e >> 16;
        uint32_t rep = 0, val = 0;
        if (sym < 16) { lengths[n++] = (uint8_t)sym; continue; }
        if (sym == 16) {
          if (n == 0) return "repeat with no previous length";
          if (!br.take(2, &rep)) return "truncated deflate stream";
          rep += 3; val = lengths[n - 1];
        } else if (sym == 17) {
          if (!br.take(3, &rep)) return "truncated deflate stream";
          rep += 3;
        } else {
          if (!br.take(7, &rep)) return "truncated deflate stream";
          rep += 11;
        }
        if (n + rep > hlit + hdist) return "code lengths run past their count";
        while (rep--) lengths[n++] = (uint8_t)val;
      }
      if (lengths[256] == 0) return "no end-of-block code";
      if (!build(dyn_lit, lengths, (int)hlit, true, kLitBits) || !build(dyn_dist, lengths + hlit, (int)hdist, false, kDistBits))
        return "bad Huffman code lengths";
      lit = &dyn_lit;
      dist = &dyn_dist;
    }

    const uint32_t* lt = lit->e.data();
    const uint32_t* dt = dist->e.data();
    constexpr uint64_t kLitMask = ((uint64_t)1 << kLitBits) - 1, kLitSub = ((uint64_t)1 << (15 - kLitBits)) - 1;
    constexpr uint64_t kDistMask = ((uint64_t)1 << kDistBits) - 1, kDistSub = ((uint64_t)1 << (15 - kDistBits)) - 1;
    auto lit_at = [lt](uint64_t b) { uint32_t e = lt[b & kLitMask]; if (e & kSub) e = lt[(e >> 16) + ((b >> kLitBits) & kLitSub)]; return e; };
    auto dist_at = [dt](uint64_t b) { uint32_t e = dt[b & kDistMask]; if (e & kSub) e = dt[(e >> 16) + ((b >> kDistBits) & kDistSub)]; return e; };
    constexpr size_t kMargin = 258 + 16;   // a whole match plus a word copy's overrun

    for (;;) {
      // Fast path: eight more input bytes and a whole match's room: no bound checked per byte, the bit buffer
      // in registers, up to three literals a refill (56 bits; a literal code is at most 15).
      if (br.pos + 16 <= br.n && (size_t)(out.end - out.o) >= kMargin) {
        uint64_t buf = br.buf;
        int have = br.have;
        uint8_t* o = out.o;
        uint8_t* const fast_end = out.end - kMargin;
        const uint8_t* const in_end = br.d + br.n - 16;   // room for two eight-byte loads an iteration (one before a length)
        const char* err = nullptr;
        bool eob = false;
        while (o <= fast_end && br.d + br.pos <= in_end) {
          { uint64_t w; std::memcpy(&w, br.d + br.pos, 8); buf |= w << have; const int t = (63 - have) >> 3; br.pos += (size_t)t; have += t * 8; }
          uint32_t e = lit_at(buf);
          int len = (int)(e & 15);
          if (!len) { err = "bad literal/length symbol"; break; }
          buf >>= len; have -= len;
          if ((e & kKindMask) == kLit) {
            *o++ = (uint8_t)(e >> 16);
            e = lit_at(buf);
            if ((e & kKindMask) != kLit) goto not_literal;
            len = (int)(e & 15);
            buf >>= len; have -= len;
            *o++ = (uint8_t)(e >> 16);
            e = lit_at(buf);
            if ((e & kKindMask) != kLit) goto not_literal;
            len = (int)(e & 15);
            buf >>= len; have -= len;
            *o++ = (uint8_t)(e >> 16);
            continue;
          not_literal:
            len = (int)(e & 15);
            if (!len) { err = "bad literal/length symbol"; break; }
            if (have < 48) {   // a length and a distance may need 48 bits: refill first
              { uint64_t w; std::memcpy(&w, br.d + br.pos, 8); buf |= w << have; const int t = (63 - have) >> 3; br.pos += (size_t)t; have += t * 8; }
            }
            buf >>= len; have -= len;
          }
          {
            const uint32_t kind = e & kKindMask;
            if (kind == kEob) { eob = true; break; }
            if (kind != kBase) { err = "invalid length symbol"; break; }
            const int xl = (int)((e >> 8) & 15);
            const uint32_t mlen = (e >> 16) + (uint32_t)(buf & ((1u << xl) - 1));
            buf >>= xl; have -= xl;
            const uint32_t de = dist_at(buf);
            const int dl = (int)(de & 15);
            if (!dl || (de & kKindMask) != kBase) { err = "invalid distance symbol"; break; }
            buf >>= dl; have -= dl;
            const int xd = (int)((de >> 8) & 15);
            const size_t d = (de >> 16) + (size_t)(buf & ((1u << xd) - 1));
            buf >>= xd; have -= xd;
            if (d > (size_t)(o - out.base)) { err = "distance before start of output"; break; }
            const uint8_t* src = o - d;
            if (d >= 8) {   // a word at a time; up to 7 bytes past the match land in the margin and are overwritten later
              uint8_t* w = o;
              uint8_t* const stop = o + mlen;
              do { uint64_t v; std::memcpy(&v, src, 8); std::memcpy(w, &v, 8); src += 8; w += 8; } while (w < stop);
            } else if (d == 1) {
              std::memset(o, *src, mlen);
            } else {
              for (uint32_t i = 0; i < mlen; ++i) o[i] = src[i];   // overlapping: the copy feeds itself
            }
            o += mlen;
          }
        }
        br.buf = buf;
        br.have = have;
        out.o = o;
        if (err) return err;
        if (eob) break;
        continue;
      }
      // Careful path: the last bytes of the input or the output, every take and write checked.
      br.refill();
      uint32_t e = lit_at(br.buf);
      int len = (int)(e & 15);
      if (!len || !br.drop(len)) return "bad literal/length symbol";
      const uint32_t kind = e & kKindMask;
      if (kind == kLit) {
        if (!out.room(1)) return "inflates past the size limit";
        *out.o++ = (uint8_t)(e >> 16);
        continue;
      }
      if (kind == kEob) break;
      if (kind != kBase) return "invalid length symbol";
      uint32_t extra = 0;
      if (!br.take((int)((e >> 8) & 15), &extra)) return "truncated deflate stream";
      const uint32_t mlen = (e >> 16) + extra;
      const uint32_t de = dist_at(br.buf);
      len = (int)(de & 15);
      if (!len || (de & kKindMask) != kBase || !br.drop(len)) return "invalid distance symbol";
      if (!br.take((int)((de >> 8) & 15), &extra)) return "truncated deflate stream";
      const size_t d = (de >> 16) + extra;
      if (d > out.produced()) return "distance before start of output";
      if (!out.room(mlen)) return "inflates past the size limit";
      const uint8_t* src = out.o - d;
      for (uint32_t i = 0; i < mlen; ++i) out.o[i] = src[i];
      out.o += mlen;
    }
    if (final_block) return nullptr;
  }
}

}  // namespace

bool inflate_raw_tree(ByteSpan whole, size_t offset, ByteBuffer& out, size_t* end_offset, size_t expect, std::string* why,
                      size_t max_out) {
  auto fail = [&](const char* w) { if (why) *why = w; return false; };
  if (offset > whole.size()) return fail("inflate past the end of the buffer");
  Bits br{whole.data() + offset, whole.size() - offset};
  const size_t start = out.size();
  // Room up front: the whole answer when its size is known, else a guess that doubles as it is passed.
  size_t room = expect ? expect : std::max<size_t>(64 << 10, (whole.size() - offset) * 4);
  if (max_out != SIZE_MAX) room = std::min(room, max_out);
  out.resize(start + room + 16);
  Out o{out.data() + start, out.data() + start, out.data() + start + room, &out, start, max_out};
  const char* err = inflate_blocks(br, o);
  out.resize(start + o.produced());
  if (err) return fail(err);
  if (end_offset) *end_offset = offset + br.consumed();   // every byte the stream used, the last one whole
  return true;
}

// Straight into the caller's buffer (a store chunk: its size known): no copy, no growing, never a byte past it.
static bool inflate_raw_into(ByteSpan data, uint8_t* dst, size_t expect) {
  Bits br{data.data(), data.size()};
  Out o{dst, dst, dst + expect};
  return inflate_blocks(br, o) == nullptr && o.produced() == expect;
}


uint32_t crc32_ieee(ByteSpan data, uint32_t running) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[n] = c;
    }
    return t;
  }();
  uint32_t c = running ^ 0xFFFFFFFFu;
  for (uint8_t b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

ByteBuffer gzip_encode(ByteSpan data, int level) {
  ByteBuffer out = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0xFF};   // deflate, no name, no time, OS unknown
  out.reserve(data.size() / 2 + 32);
  deflate_raw_append(data, level, out);
  const uint32_t crc = crc32_ieee(data), size = static_cast<uint32_t>(data.size());
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(crc >> (8 * i)));
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(size >> (8 * i)));
  return out;
}

bool gzip_decode(ByteSpan data, ByteBuffer& out, size_t max_out, std::string* why) {
  auto fail = [&](const std::string& w) { if (why) *why = w; return false; };
  size_t at = 0;
  int members = 0;
  while (at < data.size()) {
    // Trailing zero padding after the last member (tar.gz writers leave it) ends the stream.
    if (members > 0 && std::all_of(data.begin() + at, data.end(), [](uint8_t b) { return b == 0; })) break;
    if (data.size() - at < 18) return fail("truncated gzip member");
    if (data[at] != 0x1F || data[at + 1] != 0x8B) return fail(members ? "garbage after the gzip stream" : "not gzip data (no 1f 8b magic)");
    if (data[at + 2] != 8) return fail("gzip member is not deflate");
    const uint8_t flg = data[at + 3];
    if (flg & 0xE0) return fail("gzip header has reserved flags set");
    size_t p = at + 10;
    if (flg & 0x04) {   // FEXTRA
      if (p + 2 > data.size()) return fail("truncated gzip header");
      const size_t xlen = data[p] | (data[p + 1] << 8);
      p += 2 + xlen;
    }
    for (uint8_t bit : {uint8_t(0x08), uint8_t(0x10)}) {   // FNAME, FCOMMENT: zero-terminated
      if (!(flg & bit)) continue;
      while (p < data.size() && data[p] != 0) ++p;
      ++p;
    }
    if (flg & 0x02) p += 2;   // FHCRC
    if (p > data.size()) return fail("truncated gzip header");
    const size_t start = out.size();
    size_t end = 0;
    std::string w;
    if (!inflate_raw_tree(data, p, out, &end, 0, &w, max_out)) return fail("gzip: " + w);
    if (end + 8 > data.size()) return fail("truncated gzip trailer");
    uint32_t crc = 0, size = 0;
    for (int i = 0; i < 4; ++i) crc |= static_cast<uint32_t>(data[end + i]) << (8 * i);
    for (int i = 0; i < 4; ++i) size |= static_cast<uint32_t>(data[end + 4 + i]) << (8 * i);
    const ByteSpan got(out.data() + start, out.size() - start);
    if (crc32_ieee(got) != crc) return fail("gzip CRC-32 does not match: the data is damaged");
    if (static_cast<uint32_t>(got.size()) != size) return fail("gzip size does not match the trailer");
    at = end + 8;
    ++members;
  }
  if (members == 0) return fail("empty gzip stream");
  return true;
}

bool inflate_raw_exact(ByteSpan data, uint8_t* out, size_t expect) { return inflate_raw_into(data, out, expect); }

bool zlib_inflate_prefix(ByteSpan data, uint8_t* out, size_t max, size_t* got) {
  if (data.size() < 2 || (data[0] & 0x0f) != 8 || (((uint32_t)data[0] << 8) | data[1]) % 31 != 0 || (data[1] & 0x20)) return false;
  Bits br{data.data() + 2, data.size() - 2};
  Out o{out, out, out + max};
  const char* err = inflate_blocks(br, o);
  *got = o.produced();
  // Stopping because `out` is full is what was asked for; anything else that stops it is damage.
  return err == nullptr || (o.produced() == max && std::strcmp(err, "inflates past the size limit") == 0);
}

Result<ByteBuffer> zlib_inflate(ByteSpan data) {
  if (data.size() < 6) return Result<ByteBuffer>(Status::Corrupt, "zlib stream too short");
  const uint8_t cmf = data[0], flg = data[1];
  if ((cmf & 0x0f) != 8) return Result<ByteBuffer>(Status::Corrupt, "not a deflate stream");
  if (((uint32_t)cmf << 8 | flg) % 31 != 0) return Result<ByteBuffer>(Status::Corrupt, "bad zlib header check");
  if (flg & 0x20) return Result<ByteBuffer>(Status::Unsupported, "zlib preset dictionary");
  ByteBuffer out;
  size_t inner_end = 0;
  std::string why;
  if (!inflate_raw_tree(data, 2, out, &inner_end, 0, &why))
    return Result<ByteBuffer>(Status::Corrupt, why.empty() ? std::string("the deflate data is damaged") : why);
  if (inner_end + 4 > data.size()) return Result<ByteBuffer>(Status::Corrupt, "zlib stream ends before its checksum");
  const uint8_t* t = data.data() + inner_end;
  const uint32_t want = ((uint32_t)t[0] << 24) | ((uint32_t)t[1] << 16) | ((uint32_t)t[2] << 8) | (uint32_t)t[3];
  uint32_t a = 1, b = 0;   // Adler-32
  for (size_t i = 0; i < out.size(); ++i) {
    a = (a + out[i]) % 65521;
    b = (b + a) % 65521;
  }
  if (((b << 16) | a) != want) return Result<ByteBuffer>(Status::Corrupt, "zlib checksum does not match: the data is damaged");
  return out;
}

}  // namespace aios::net
