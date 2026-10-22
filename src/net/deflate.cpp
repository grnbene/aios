#include "aios/net/deflate.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#if defined(AIOS_HAVE_LIBDEFLATE)
#include <libdeflate.h>
#endif
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
// ---------------------------------------------------------------------------
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

bool inflate_raw_tree(ByteSpan whole, size_t offset, ByteBuffer& out, size_t* end_offset, size_t expect, std::string* why) {
  auto fail = [&](const char* w) { if (why) *why = w; return false; };
  if (offset > whole.size()) return fail("inflate past the end of the buffer");
  ByteSpan data = whole.subspan(offset);
  BitReader br{data};
  Huff fixed_lit, fixed_dist;
  bool fixed_ready = false;

  for (;;) {
    int final_block = br.bit();
    int type = (int)br.bits(2);
    if (br.bad) return fail("truncated deflate stream");

    if (type == 0) {
      br.align();
      if (br.pos + 4 > br.d.size()) return fail("truncated stored block");
      uint32_t len = (uint32_t)br.d[br.pos] | ((uint32_t)br.d[br.pos + 1] << 8);
      br.pos += 4;
      if (br.pos + len > br.d.size()) return fail("stored block runs past end");
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
          if (sym < 0 || br.bad) return fail("bad code-length symbol");
          if (sym < 16) { lengths[n++] = (uint8_t)sym; continue; }
          int rep = 0; uint8_t val = 0;
          if (sym == 16) { if (n == 0) return fail("repeat with no previous length"); val = lengths[n - 1]; rep = 3 + (int)br.bits(2); }
          else if (sym == 17) { rep = 3 + (int)br.bits(3); }
          else { rep = 11 + (int)br.bits(7); }
          if (n + rep > hlit + hdist) return fail("code lengths overflow");
          while (rep--) lengths[n++] = val;
        }
        huff_build(dyn_lit, lengths, hlit);
        huff_build(dyn_dist, lengths + hlit, hdist);
        lit = &dyn_lit; dist = &dyn_dist;
      }

      for (;;) {
        int sym = huff_decode(br, *lit);
        if (sym < 0 || br.bad) return fail("bad literal/length symbol");
        if (sym < 256) { out.push_back((uint8_t)sym); continue; }
        if (sym == 256) break;
        sym -= 257;
        if (sym >= 29) return fail("invalid length symbol");
        uint32_t len = kLenBase[sym] + br.bits(kLenExtra[sym]);
        int dsym = huff_decode(br, *dist);
        if (dsym < 0 || dsym >= 30) return fail("invalid distance symbol");
        uint32_t d = kDistBase[dsym] + br.bits(kDistExtra[dsym]);
        if (d > out.size()) return fail("distance before start of output");
        const size_t at = out.size(), from = at - d;
        out.resize(at + len);
        uint8_t* o = out.data();
        if (d >= len) std::memcpy(o + at, o + from, len);            // the two ranges are apart
        else for (uint32_t i = 0; i < len; ++i) o[at + i] = o[from + i];   // overlapping: the copy feeds itself
      }
    } else {
      return fail("reserved deflate block type");
    }
    if (final_block) break;
  }
  if (br.bad) return fail("truncated deflate stream");
  if (end_offset) *end_offset = offset + br.consumed();   // every byte the stream used, the last one whole
  return true;
}


bool inflate_raw_exact(ByteSpan data, uint8_t* out, size_t expect) {
#if defined(AIOS_HAVE_LIBDEFLATE)
  {
    struct D {
      libdeflate_decompressor* d = libdeflate_alloc_decompressor();
      ~D() { if (d) libdeflate_free_decompressor(d); }
    };
    thread_local D dec;
    if (dec.d) {
      size_t got = 0;
      if (libdeflate_deflate_decompress(dec.d, data.data(), data.size(), out, expect, &got) == LIBDEFLATE_SUCCESS && got == expect) return true;
      // anything else: zlib or the decoder below says why, and the answer must agree
    }
  }
#endif
#if defined(AIOS_HAVE_ZLIB)
  {
    ByteBuffer zb;
    size_t consumed = 0;
    if (zlib_inflate_raw(data, &zb, &consumed, expect)) {
      if (zb.size() != expect) return false;
      std::memcpy(out, zb.data(), expect);
      return true;
    }
  }
#endif
  ByteBuffer tree;
  tree.reserve(expect);
  if (!inflate_raw_tree(data, 0, tree, nullptr, expect, nullptr) || tree.size() != expect) return false;
  std::memcpy(out, tree.data(), expect);
  return true;
}

Result<ByteBuffer> zlib_inflate(ByteSpan data) {
  if (data.size() < 6) return Result<ByteBuffer>(Status::Corrupt, "zlib stream too short");
  const uint8_t cmf = data[0], flg = data[1];
  if ((cmf & 0x0f) != 8) return Result<ByteBuffer>(Status::Corrupt, "not a deflate stream");
  if (((uint32_t)cmf << 8 | flg) % 31 != 0) return Result<ByteBuffer>(Status::Corrupt, "bad zlib header check");
  if (flg & 0x20) return Result<ByteBuffer>(Status::Unsupported, "zlib preset dictionary");
#if defined(AIOS_HAVE_ZLIB)
  {
    ByteBuffer out;
    size_t used = 0;
    if (zlib_inflate_z(data, &out, &used, 0)) return out;
  }
#endif
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
