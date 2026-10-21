#include "aios/crypto/sha1.hpp"
#include <cstring>

// The SHA instructions, where the processor has them. Each hardware path is compiled for its instructions alone
// (a target attribute on that one function), so the rest of the build keeps its baseline flags and a machine without
// them never executes a byte of it: which path runs is decided once, at run time, from what the processor reports.
#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__)) && !defined(__wasi__)
#define AIOS_SHA1_X86 1
#include <cpuid.h>
#include <immintrin.h>
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__)) && (defined(__linux__) || defined(__APPLE__))
#define AIOS_SHA1_ARM 1
#include <arm_neon.h>
#if defined(__linux__)
#include <sys/auxv.h>
#ifndef HWCAP_SHA1
#define HWCAP_SHA1 (1 << 5)
#endif
#else
#include <sys/sysctl.h>
#endif
#endif

namespace aios::crypto {

namespace {

inline uint32_t rotl(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }

// One block, in software. The eighty rounds are four runs of twenty with their own function and constant, written as four loops
// with nothing to decide inside them, over a schedule kept in sixteen words: about twice as fast as one loop that
// picks the function each round, which matters when a fetch hashes every object of a repository.
void sha1_transform(uint32_t state[5], const uint8_t data[64]) {
  uint32_t w[16];
  for (int i = 0; i < 16; ++i) {
    w[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
           ((uint32_t)data[i * 4 + 2] << 8) | ((uint32_t)data[i * 4 + 3]);
  }
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
#define AIOS_SHA1_W(i) (w[(i) & 15] = rotl(w[((i) + 13) & 15] ^ w[((i) + 8) & 15] ^ w[((i) + 2) & 15] ^ w[(i) & 15], 1))
#define AIOS_SHA1_STEP(f, k, x) { uint32_t t = rotl(a, 5) + (f) + e + (k) + (x); e = d; d = c; c = rotl(b, 30); b = a; a = t; }
  for (int i = 0; i < 16; ++i) AIOS_SHA1_STEP(d ^ (b & (c ^ d)), 0x5a827999u, w[i])
  for (int i = 16; i < 20; ++i) AIOS_SHA1_STEP(d ^ (b & (c ^ d)), 0x5a827999u, AIOS_SHA1_W(i))
  for (int i = 20; i < 40; ++i) AIOS_SHA1_STEP(b ^ c ^ d, 0x6ed9eba1u, AIOS_SHA1_W(i))
  for (int i = 40; i < 60; ++i) AIOS_SHA1_STEP((b & c) | (d & (b | c)), 0x8f1bbcdcu, AIOS_SHA1_W(i))
  for (int i = 60; i < 80; ++i) AIOS_SHA1_STEP(b ^ c ^ d, 0xca62c1d6u, AIOS_SHA1_W(i))
#undef AIOS_SHA1_STEP
#undef AIOS_SHA1_W
  state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

void sha1_blocks_soft(uint32_t state[5], const uint8_t* data, size_t blocks) {
  for (; blocks; --blocks, data += 64) sha1_transform(state, data);
}

#if defined(AIOS_SHA1_X86)
// SHA-NI: sha1rnds4 does four rounds, sha1nexte derives the next four rounds' E, and sha1msg1/sha1msg2 with an xor
// extend the message schedule four words at a time. The state is kept as ABCD in one register (A in the top lane) and
// E in the top lane of another, which is the layout the instructions expect. Twenty groups of four rounds; the group
// number picks the round function and which of the four schedule registers is current, so the macro below, given a
// constant group, expands to exactly the instruction sequence Intel's reference code spells out by hand.
__attribute__((target("sha,sse4.1")))
void sha1_blocks_shani(uint32_t state[5], const uint8_t* data, size_t blocks) {
  const __m128i kByteSwap = _mm_set_epi64x(0x0001020304050607LL, 0x08090a0b0c0d0e0fLL);
  __m128i abcd = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i*)state), 0x1b);
  __m128i e[2] = {_mm_set_epi32((int)state[4], 0, 0, 0), _mm_setzero_si128()};
  for (; blocks; --blocks, data += 64) {
    const __m128i abcd_saved = abcd, e_saved = e[0];
    __m128i m[4];
    for (int i = 0; i < 4; ++i) m[i] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data + 16 * i)), kByteSwap);
#define AIOS_SHA1_NI(g)                                                                             \
    {                                                                                               \
      if ((g) == 0) e[0] = _mm_add_epi32(e[0], m[0]);                                               \
      else e[(g) & 1] = _mm_sha1nexte_epu32(e[(g) & 1], m[(g) % 4]);                                \
      e[((g) + 1) & 1] = abcd;                                                                      \
      if ((g) >= 3 && (g) <= 18) m[((g) + 1) % 4] = _mm_sha1msg2_epu32(m[((g) + 1) % 4], m[(g) % 4]); \
      abcd = _mm_sha1rnds4_epu32(abcd, e[(g) & 1], (g) / 5);                                        \
      if ((g) >= 1 && (g) <= 16) m[((g) + 3) % 4] = _mm_sha1msg1_epu32(m[((g) + 3) % 4], m[(g) % 4]); \
      if ((g) >= 2 && (g) <= 17) m[((g) + 2) % 4] = _mm_xor_si128(m[((g) + 2) % 4], m[(g) % 4]);   \
    }
    AIOS_SHA1_NI(0) AIOS_SHA1_NI(1) AIOS_SHA1_NI(2) AIOS_SHA1_NI(3) AIOS_SHA1_NI(4)
    AIOS_SHA1_NI(5) AIOS_SHA1_NI(6) AIOS_SHA1_NI(7) AIOS_SHA1_NI(8) AIOS_SHA1_NI(9)
    AIOS_SHA1_NI(10) AIOS_SHA1_NI(11) AIOS_SHA1_NI(12) AIOS_SHA1_NI(13) AIOS_SHA1_NI(14)
    AIOS_SHA1_NI(15) AIOS_SHA1_NI(16) AIOS_SHA1_NI(17) AIOS_SHA1_NI(18) AIOS_SHA1_NI(19)
#undef AIOS_SHA1_NI
    e[0] = _mm_sha1nexte_epu32(e[0], e_saved);
    abcd = _mm_add_epi32(abcd, abcd_saved);
  }
  _mm_storeu_si128((__m128i*)state, _mm_shuffle_epi32(abcd, 0x1b));
  state[4] = (uint32_t)_mm_extract_epi32(e[0], 3);
}

// CPUID leaf 7 reports the SHA extensions (EBX bit 29); the code also uses SSSE3's byte shuffle and SSE4.1's lane
// extract, from leaf 1 (ECX bits 9 and 19). Every processor with SHA has both, but asking costs nothing.
bool cpu_has_sha1() {
  unsigned a, b, c, d;
  if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
  if (!(c & (1u << 9)) || !(c & (1u << 19))) return false;
  if (__get_cpuid_max(0, nullptr) < 7) return false;
  __cpuid_count(7, 0, a, b, c, d);
  return (b & (1u << 29)) != 0;
}
#endif

#if defined(AIOS_SHA1_ARM)
// The ARMv8 crypto extensions: sha1c/sha1p/sha1m do four rounds of the choose, parity and majority functions,
// sha1h gives the next E, and sha1su0/sha1su1 extend the schedule. The round constant is added to the schedule words
// two groups ahead, as Arm's reference code does, so the add is off the critical path of the rounds.
#if defined(__ARM_FEATURE_SHA2) || defined(__ARM_FEATURE_CRYPTO)
#define AIOS_SHA1_ARM_TARGET
#elif defined(__clang__)
#define AIOS_SHA1_ARM_TARGET __attribute__((target("crypto")))
#else
#define AIOS_SHA1_ARM_TARGET __attribute__((target("+crypto")))
#endif
AIOS_SHA1_ARM_TARGET
void sha1_blocks_armv8(uint32_t state[5], const uint8_t* data, size_t blocks) {
  const uint32x4_t k[4] = {vdupq_n_u32(0x5a827999u), vdupq_n_u32(0x6ed9eba1u), vdupq_n_u32(0x8f1bbcdcu), vdupq_n_u32(0xca62c1d6u)};
  uint32x4_t abcd = vld1q_u32(state);
  uint32_t e[2] = {state[4], 0};
  for (; blocks; --blocks, data += 64) {
    const uint32x4_t abcd_saved = abcd;
    const uint32_t e_saved = e[0];
    uint32x4_t m[4];
    for (int i = 0; i < 4; ++i) m[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 16 * i)));
    uint32x4_t tmp[2] = {vaddq_u32(m[0], k[0]), vaddq_u32(m[1], k[0])};
#define AIOS_SHA1_ARM_ROUNDS(g)                                                                       \
    {                                                                                                 \
      e[((g) + 1) & 1] = vsha1h_u32(vgetq_lane_u32(abcd, 0));                                         \
      if ((g) < 5) abcd = vsha1cq_u32(abcd, e[(g) & 1], tmp[(g) & 1]);                                \
      else if ((g) < 10 || (g) >= 15) abcd = vsha1pq_u32(abcd, e[(g) & 1], tmp[(g) & 1]);             \
      else abcd = vsha1mq_u32(abcd, e[(g) & 1], tmp[(g) & 1]);                                        \
      if ((g) <= 17) tmp[(g) & 1] = vaddq_u32(m[((g) + 2) % 4], k[((g) + 2) / 5]);                    \
      if ((g) >= 1 && (g) <= 16) m[((g) + 3) % 4] = vsha1su1q_u32(m[((g) + 3) % 4], m[((g) + 2) % 4]); \
      if ((g) <= 15) m[(g) % 4] = vsha1su0q_u32(m[(g) % 4], m[((g) + 1) % 4], m[((g) + 2) % 4]);      \
    }
    AIOS_SHA1_ARM_ROUNDS(0) AIOS_SHA1_ARM_ROUNDS(1) AIOS_SHA1_ARM_ROUNDS(2) AIOS_SHA1_ARM_ROUNDS(3)
    AIOS_SHA1_ARM_ROUNDS(4) AIOS_SHA1_ARM_ROUNDS(5) AIOS_SHA1_ARM_ROUNDS(6) AIOS_SHA1_ARM_ROUNDS(7)
    AIOS_SHA1_ARM_ROUNDS(8) AIOS_SHA1_ARM_ROUNDS(9) AIOS_SHA1_ARM_ROUNDS(10) AIOS_SHA1_ARM_ROUNDS(11)
    AIOS_SHA1_ARM_ROUNDS(12) AIOS_SHA1_ARM_ROUNDS(13) AIOS_SHA1_ARM_ROUNDS(14) AIOS_SHA1_ARM_ROUNDS(15)
    AIOS_SHA1_ARM_ROUNDS(16) AIOS_SHA1_ARM_ROUNDS(17) AIOS_SHA1_ARM_ROUNDS(18) AIOS_SHA1_ARM_ROUNDS(19)
#undef AIOS_SHA1_ARM_ROUNDS
    e[0] += e_saved;
    abcd = vaddq_u32(abcd, abcd_saved);
  }
  vst1q_u32(state, abcd);
  state[4] = e[0];
}

bool cpu_has_sha1() {
#if defined(__linux__)
  return (getauxval(AT_HWCAP) & HWCAP_SHA1) != 0;
#else
  // Every Apple arm64 processor has the SHA-1 instructions; the sysctl is asked anyway, and only a definite "no" is
  // believed, since older systems do not have the name at all.
  int v = 1;
  size_t len = sizeof(v);
  if (sysctlbyname("hw.optional.arm.FEAT_SHA1", &v, &len, nullptr, 0) != 0) return true;
  return v != 0;
#endif
}
#endif

using BlocksFn = void (*)(uint32_t*, const uint8_t*, size_t);

// Chosen on first use and kept: a static local, so the probe runs once even when several threads hash at once.
BlocksFn sha1_blocks_impl() {
  static const BlocksFn fn = [] {
#if defined(AIOS_SHA1_X86)
    if (cpu_has_sha1()) return &sha1_blocks_shani;
#elif defined(AIOS_SHA1_ARM)
    if (cpu_has_sha1()) return &sha1_blocks_armv8;
#endif
    return &sha1_blocks_soft;
  }();
  return fn;
}

}  // namespace

void sha1_init(SHA1Context& ctx) {
  ctx.state[0] = 0x67452301; ctx.state[1] = 0xefcdab89;
  ctx.state[2] = 0x98badcfe; ctx.state[3] = 0x10325476;
  ctx.state[4] = 0xc3d2e1f0;
  ctx.count = 0;
  std::memset(ctx.buffer, 0, sizeof(ctx.buffer));
}

void sha1_update(SHA1Context& ctx, const uint8_t* data, size_t len) {
  const BlocksFn blocks = sha1_blocks_impl();
  size_t have = (size_t)(ctx.count % 64);
  ctx.count += len;
  size_t off = 0;
  if (have) {
    size_t need = 64 - have;
    if (len < need) { std::memcpy(ctx.buffer + have, data, len); return; }
    std::memcpy(ctx.buffer + have, data, need);
    blocks(ctx.state, ctx.buffer, 1);
    off = need;
  }
  // Whole blocks straight from the caller's buffer, all in one call, so the hardware paths keep the state in
  // registers across them.
  const size_t whole = (len - off) / 64;
  if (whole) { blocks(ctx.state, data + off, whole); off += whole * 64; }
  if (off < len) std::memcpy(ctx.buffer, data + off, len - off);
}

void sha1_final(SHA1Context& ctx, uint8_t hash[20]) {
  uint64_t bits = ctx.count * 8;
  size_t have = (size_t)(ctx.count % 64);
  uint8_t pad[128];
  std::memset(pad, 0, sizeof(pad));
  pad[0] = 0x80;
  size_t padlen = (have < 56) ? (56 - have) : (120 - have);
  sha1_update(ctx, pad, padlen);
  uint8_t len_be[8];
  for (int i = 0; i < 8; ++i) len_be[i] = (uint8_t)(bits >> (56 - 8 * i));
  sha1_update(ctx, len_be, 8);
  for (int i = 0; i < 5; ++i) {
    hash[i * 4]     = (uint8_t)(ctx.state[i] >> 24);
    hash[i * 4 + 1] = (uint8_t)(ctx.state[i] >> 16);
    hash[i * 4 + 2] = (uint8_t)(ctx.state[i] >> 8);
    hash[i * 4 + 3] = (uint8_t)(ctx.state[i]);
  }
}

std::array<uint8_t, 20> sha1(ByteSpan data) {
  SHA1Context ctx;
  sha1_init(ctx);
  sha1_update(ctx, data.data(), data.size());
  std::array<uint8_t, 20> out{};
  sha1_final(ctx, out.data());
  return out;
}

}  // namespace aios::crypto
