/* Floating point to and from text for the bare-metal runtimes: an exact
 * printf formatter for %f/%e/%g and a correctly rounded strtod. Both runtimes
 * include it twice: once with AIOS_FLOAT_FORMAT inside their printf namespace
 * (after Sink and emit_uint), once with AIOS_FLOAT_PARSE inside extern "C". */
#if defined(AIOS_FLOAT_FORMAT)
/* Doubles, exactly. A double is m * 2^e; its decimal expansion is finite, so
 * the digits are generated with plain bignum arithmetic (integer part by
 * repeated division, fraction by repeated *10) and rounded half-to-even on
 * the exact value - what glibc prints. The version this replaces scaled by
 * 10^prec into a uint64_t, so anything past 1.8e19 / 10^prec (time() at
 * %.12g: 1790297283.652) printed as UINT64_MAX's digits, and it read %g's
 * precision as decimal places rather than significant digits. */
struct DecDigits {
  char d[1500];   /* significant digits, no leading zeros */
  int n = 0;      /* how many */
  int x = 0;      /* power of ten of d[0] */
};

void exact_digits(double v, DecDigits& out) {
  out.n = 0; out.x = 0;
  uint64_t bits; __builtin_memcpy(&bits, &v, 8);
  const int be = static_cast<int>((bits >> 52) & 0x7FF);
  uint64_t m = bits & ((1ull << 52) - 1);
  int e;
  if (be == 0) { e = -1074; } else { m |= 1ull << 52; e = be - 1075; }
  if (m == 0) return;
  /* integer part as a little-endian bignum of 32-bit words */
  uint32_t big[40] = {0}; int bw = 0;
  uint32_t frac[40] = {0}; int fbits = 0;   /* fraction = frac / 2^fbits */
  if (e >= 0) {
    big[0] = static_cast<uint32_t>(m); big[1] = static_cast<uint32_t>(m >> 32); bw = 2;
    for (int k = 0; k < e; ++k) {          /* shift left one bit at a time: e <= 971 */
      uint32_t carry = 0;
      for (int w = 0; w < bw; ++w) { const uint32_t nc = big[w] >> 31; big[w] = (big[w] << 1) | carry; carry = nc; }
      if (carry) big[bw++] = carry;
    }
  } else {
    fbits = -e;
    const uint64_t ip = fbits >= 64 ? 0 : (m >> fbits);
    const uint64_t fp = fbits >= 64 ? m : (m & ((1ull << fbits) - 1));
    big[0] = static_cast<uint32_t>(ip); big[1] = static_cast<uint32_t>(ip >> 32); bw = 2;
    frac[0] = static_cast<uint32_t>(fp); frac[1] = static_cast<uint32_t>(fp >> 32);
  }
  while (bw > 0 && big[bw - 1] == 0) --bw;
  /* integer digits, least significant first, then reversed */
  char idig[320]; int in = 0;
  while (bw > 0) {
    uint64_t rem = 0;
    for (int w = bw - 1; w >= 0; --w) { const uint64_t cur = (rem << 32) | big[w]; big[w] = static_cast<uint32_t>(cur / 1000000000u); rem = cur % 1000000000u; }
    while (bw > 0 && big[bw - 1] == 0) --bw;
    for (int k = 0; k < 9; ++k) { idig[in++] = static_cast<char>('0' + rem % 10); rem /= 10; if (bw == 0 && rem == 0) break; }
  }
  while (in > 0 && idig[in - 1] == '0') --in;
  for (int k = in - 1; k >= 0; --k) out.d[out.n++] = idig[k];
  out.x = in - 1;
  /* fraction digits: frac * 10, the bits that spill past fbits are the digit */
  if (fbits > 0) {
    const int fw = (fbits + 31) / 32 + 1;
    int pos = -1;
    for (;;) {
      bool any = false;
      for (int w = 0; w < fw; ++w) if (frac[w]) { any = true; break; }
      if (!any || out.n >= static_cast<int>(sizeof out.d)) break;
      uint64_t carry = 0;
      for (int w = 0; w < fw; ++w) { const uint64_t cur = static_cast<uint64_t>(frac[w]) * 10u + carry; frac[w] = static_cast<uint32_t>(cur); carry = cur >> 32; }
      const int hw = fbits / 32, hb = fbits % 32;
      uint32_t digit = static_cast<uint32_t>((static_cast<uint64_t>(frac[hw]) | (static_cast<uint64_t>(frac[hw + 1]) << 32)) >> hb);
      frac[hw] &= hb ? ((1u << hb) - 1) : 0u;
      frac[hw + 1] = 0;
      if (out.n == 0) { if (digit == 0) { --pos; continue; } out.x = pos; }
      out.d[out.n++] = static_cast<char>('0' + digit);
      --pos;
    }
  }
}

/* Keep `keep` leading digits, rounding half to even on the exact value. */
void round_digits(DecDigits& g, int keep) {
  if (keep >= g.n) return;
  if (keep < 0) { g.n = 0; return; }
  const int r = g.d[keep] - '0';
  bool sticky = false;
  for (int k = keep + 1; k < g.n; ++k) if (g.d[k] != '0') { sticky = true; break; }
  const int prev = keep > 0 ? g.d[keep - 1] - '0' : 0;
  const bool up = r > 5 || (r == 5 && (sticky || (prev & 1)));
  g.n = keep;
  if (!up) return;
  int k = g.n - 1;
  while (k >= 0 && g.d[k] == '9') { g.d[k] = '0'; --k; }
  if (k >= 0) { g.d[k]++; return; }
  for (int q = g.n; q > 0; --q) g.d[q] = g.d[q - 1];
  g.d[0] = '1'; g.n = g.n + 1; g.x++;
  if (keep > 0 && g.n > keep) g.n = keep;
}

void emit_fixed(Sink& s, const DecDigits& g, int prec, bool alt) {
  const int top = g.n > 0 && g.x > 0 ? g.x : 0;
  for (int k = top; k >= -prec; --k) {
    if (k == -1) s.put('.');
    const int at = g.x - k;
    s.put(g.n > 0 && at >= 0 && at < g.n ? g.d[at] : '0');
  }
  if (prec == 0 && alt) s.put('.');   /* '#': the point stays even with nothing after it */
}

void emit_sci(Sink& s, const DecDigits& g, int prec, bool upper, bool alt) {
  s.put(g.n > 0 ? g.d[0] : '0');
  if (prec > 0 || alt) s.put('.');
  for (int k = 1; k <= prec; ++k) s.put(k < g.n ? g.d[k] : '0');
  s.put(upper ? 'E' : 'e');
  int x = g.n > 0 ? g.x : 0;
  s.put(x < 0 ? '-' : '+');
  if (x < 0) x = -x;
  emit_uint(s, static_cast<unsigned long long>(x), 10, false, 2);
}

void emit_double(Sink& s, double v, int prec, char conv, bool alt) {
  const bool upper = conv == 'F' || conv == 'E' || conv == 'G';
  uint64_t bits; __builtin_memcpy(&bits, &v, 8);
  const bool neg = bits >> 63;
  if (((bits >> 52) & 0x7FF) == 0x7FF) {
    if (neg && (bits & ((1ull << 52) - 1)) == 0) s.put('-');
    s.puts_((bits & ((1ull << 52) - 1)) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf"));
    return;
  }
  if (neg) s.put('-');
  if (prec < 0) prec = 6;
  if (prec > 400) prec = 400;
  DecDigits g;
  exact_digits(neg ? -v : v, g);
  const char c = static_cast<char>(conv | 32);
  if (c == 'f') { round_digits(g, g.x + 1 + prec); emit_fixed(s, g, prec, alt); return; }
  if (c == 'e') { round_digits(g, prec + 1); emit_sci(s, g, prec, upper, alt); return; }
  /* %g: P significant digits; scientific when the exponent is < -4 or >= P;
   * trailing zeros go unless '#'. */
  const int P = prec == 0 ? 1 : prec;
  round_digits(g, P);
  const int x = g.n > 0 ? g.x : 0;
  char tmp[480];
  Sink t{tmp, sizeof tmp, 0};
  if (x < -4 || x >= P) emit_sci(t, g, P - 1, upper, alt); else emit_fixed(t, g, P - 1 - x, alt);
  int len = static_cast<int>(t.n < sizeof tmp ? t.n : sizeof tmp - 1);
  int epos = len;
  for (int k = 0; k < len; ++k) if (tmp[k] == 'e' || tmp[k] == 'E') { epos = k; break; }
  int keep_to = epos;
  bool has_dot = false;
  for (int k = 0; k < epos; ++k) if (tmp[k] == '.') has_dot = true;
  if (!alt && has_dot) {
    while (keep_to > 0 && tmp[keep_to - 1] == '0') --keep_to;
    if (keep_to > 0 && tmp[keep_to - 1] == '.') --keep_to;
  }
  for (int k = 0; k < keep_to; ++k) s.put(tmp[k]);
  for (int k = epos; k < len; ++k) s.put(tmp[k]);
}

#endif
#if defined(AIOS_FLOAT_PARSE)
/* Correctly rounded, like glibc: the decimal is an exact big integer ratio
 * A/B, the top 66 bits of the quotient are found by shift-subtract with a
 * sticky bit for the remainder, and that is rounded half-to-even to 53 bits
 * (fewer for subnormals). The version this replaces summed digit * 0.1^k, so
 * "3.14" and "0.3" came out one ulp off and a grain literal did not equal
 * the same number computed any other way. */
}  // extern "C" (the helpers below must not get C linkage)
namespace strtod_detail {
constexpr int W = 150;                        /* 4800 bits: 10^1100 plus headroom */
struct Big {
  uint32_t w[W]; int n;
  void set(uint64_t v) { n = 0; while (v) { w[n++] = static_cast<uint32_t>(v); v >>= 32; } }
  void mul_small(uint32_t m, uint32_t add) {
    uint64_t c = add;
    for (int k = 0; k < n; ++k) { const uint64_t t = static_cast<uint64_t>(w[k]) * m + c; w[k] = static_cast<uint32_t>(t); c = t >> 32; }
    if (c && n < W) w[n++] = static_cast<uint32_t>(c);
  }
  void shl(int b) {
    if (b <= 0 || n == 0) return;
    const int ws = b / 32, bs = b % 32;
    int nn = n + ws + 1; if (nn > W) nn = W;
    for (int k = nn - 1; k >= 0; --k) {
      const int src = k - ws;
      uint32_t hi = (src >= 0 && src < n) ? w[src] : 0, lo = (src - 1 >= 0 && src - 1 < n) ? w[src - 1] : 0;
      w[k] = bs ? (hi << bs) | (lo >> (32 - bs)) : hi;
    }
    n = nn; while (n > 0 && w[n - 1] == 0) --n;
  }
  int bits() const { if (!n) return 0; return (n - 1) * 32 + (32 - __builtin_clz(w[n - 1])); }
};
int cmp(const Big& a, const Big& b) {
  if (a.n != b.n) return a.n < b.n ? -1 : 1;
  for (int k = a.n - 1; k >= 0; --k) if (a.w[k] != b.w[k]) return a.w[k] < b.w[k] ? -1 : 1;
  return 0;
}
void sub(Big& a, const Big& b) {                /* a -= b, a >= b */
  int64_t br = 0;
  for (int k = 0; k < a.n; ++k) {
    int64_t t = static_cast<int64_t>(a.w[k]) - (k < b.n ? b.w[k] : 0) - br;
    br = t < 0; if (t < 0) t += (1ll << 32);
    a.w[k] = static_cast<uint32_t>(t);
  }
  while (a.n > 0 && a.w[a.n - 1] == 0) --a.n;
}
double make(bool neg, uint64_t bits) { bits |= neg ? (1ull << 63) : 0; double d; __builtin_memcpy(&d, &bits, 8); return d; }
bool ieq(const char* p, const char* word) { for (; *word; ++p, ++word) if ((*p | 32) != *word) return false; return true; }
}  // namespace strtod_detail
extern "C" {

double strtod(const char* s, char** end) {
  using namespace strtod_detail;
  const char* p = s;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\v' || *p == '\f') ++p;
  bool neg = false;
  if (*p == '+' || *p == '-') { neg = *p == '-'; ++p; }
  if (ieq(p, "inf")) { p += ieq(p, "infinity") ? 8 : 3; if (end) *end = const_cast<char*>(p); return make(neg, 0x7FFull << 52); }
  if (ieq(p, "nan")) { p += 3; if (end) *end = const_cast<char*>(p); return make(neg, 0x7FF8ull << 48); }
  /* digits -> D (up to 800 significant, the rest only matter as "nonzero") and E */
  static constexpr int kMaxDigits = 800;
  Big A; A.n = 0;
  Big B; B.set(1);
  int nd = 0, e10 = 0, bexp = 0; bool any = false, trunc_nonzero = false;
  auto hexv = [](char c) { return c >= '0' && c <= '9' ? c - '0' : ((c | 32) >= 'a' && (c | 32) <= 'f') ? (c | 32) - 'a' + 10 : -1; };
  if (p[0] == '0' && (p[1] | 32) == 'x' && (hexv(p[2]) >= 0 || (p[2] == '.' && hexv(p[3]) >= 0))) {
    /* hex float: 0xH.HHHp[+-]D, value = digits * 2^(p - 4 * fraction digits) */
    p += 2;
    for (; hexv(*p) >= 0; ++p) { if (nd == 0 && *p == '0') continue; if (nd < 300) { A.mul_small(16, static_cast<uint32_t>(hexv(*p))); ++nd; } else { bexp += 4; if (*p != '0') trunc_nonzero = true; } }
    if (*p == '.') { ++p; for (; hexv(*p) >= 0; ++p) { if (nd == 0 && *p == '0') { bexp -= 4; continue; } if (nd < 300) { A.mul_small(16, static_cast<uint32_t>(hexv(*p))); ++nd; bexp -= 4; } else if (*p != '0') trunc_nonzero = true; } }
    if ((*p | 32) == 'p') {
      const char* q = p + 1; bool eneg = false;
      if (*q == '+' || *q == '-') { eneg = *q == '-'; ++q; }
      if (*q >= '0' && *q <= '9') { int e = 0; for (; *q >= '0' && *q <= '9'; ++q) if (e < 100000) e = e * 10 + (*q - '0'); bexp += eneg ? -e : e; p = q; }
    }
    if (end) *end = const_cast<char*>(p);
    if (A.n == 0) return make(neg, 0);
    if (A.bits() + bexp > 1025) return make(neg, 0x7FFull << 52);
    if (A.bits() + bexp < -1080) return make(neg, 0);
    goto divide;
  }
  for (; *p >= '0' && *p <= '9'; ++p) {
    any = true;
    if (nd == 0 && *p == '0') continue;
    if (nd < kMaxDigits) { A.mul_small(10, static_cast<uint32_t>(*p - '0')); ++nd; } else { ++e10; if (*p != '0') trunc_nonzero = true; }
  }
  if (*p == '.') {
    ++p;
    for (; *p >= '0' && *p <= '9'; ++p) {
      any = true;
      if (nd == 0 && *p == '0') { --e10; continue; }
      if (nd < kMaxDigits) { A.mul_small(10, static_cast<uint32_t>(*p - '0')); ++nd; --e10; } else if (*p != '0') trunc_nonzero = true;
    }
  }
  if (!any) { if (end) *end = const_cast<char*>(s); return 0.0; }
  if (*p == 'e' || *p == 'E') {
    const char* q = p + 1; bool eneg = false;
    if (*q == '+' || *q == '-') { eneg = *q == '-'; ++q; }
    if (*q >= '0' && *q <= '9') {
      int e = 0;
      for (; *q >= '0' && *q <= '9'; ++q) if (e < 100000) e = e * 10 + (*q - '0');
      e10 += eneg ? -e : e;
      p = q;
    }
  }
  if (end) *end = const_cast<char*>(p);
  if (A.n == 0) return make(neg, 0);
  if (nd + e10 > 310) return make(neg, 0x7FFull << 52);        /* >= 1e310: inf */
  if (nd + e10 < -330) return make(neg, 0);                    /* < 1e-330: 0 */
  /* value = A / B * 2^bexp, all exact */
  for (int k = 0; k < e10; ++k) A.mul_small(10, 0);
  for (int k = 0; k < -e10; ++k) B.mul_small(10, 0);
divide:
  /* scale so the quotient has 65-66 bits */
  const int sh = 65 - (A.bits() - B.bits());
  if (sh > 0) A.shl(sh); else B.shl(-sh);
  unsigned __int128 q = 0;
  for (int j = 66; j >= 0; --j) {
    Big t = B; t.shl(j);
    if (cmp(A, t) >= 0) { sub(A, t); q |= static_cast<unsigned __int128>(1) << j; }
  }
  const bool sticky = A.n != 0 || trunc_nonzero;
  /* value = q * 2^-sh; round q to the mantissa width */
  int L = 0; { unsigned __int128 t = q; while (t) { ++L; t >>= 1; } }
  const int e2 = L - 1 - sh + bexp;
  const int keep = e2 >= -1022 ? 53 : 53 - (-1022 - e2);
  if (keep < 0) return make(neg, 0);
  const int drop = L - keep;                                   /* >= 12: L >= 65 */
  uint64_t mant = drop >= 128 ? 0 : static_cast<uint64_t>(q >> drop);
  const unsigned __int128 rem = drop >= 128 ? q : (q & ((static_cast<unsigned __int128>(1) << drop) - 1));
  const unsigned __int128 half = static_cast<unsigned __int128>(1) << (drop - 1);
  if (rem > half || (rem == half && (sticky || (mant & 1)))) ++mant;
  if (keep < 53) return make(neg, mant);                       /* subnormal (mant == 2^52 is the smallest normal) */
  int ex = e2;
  if (mant == (1ull << 53)) { mant >>= 1; ++ex; }
  if (ex > 1023) return make(neg, 0x7FFull << 52);
  return make(neg, (static_cast<uint64_t>(ex + 1023) << 52) | (mant & ((1ull << 52) - 1)));
}
#endif
