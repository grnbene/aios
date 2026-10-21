/* Portable libm for the bare-metal kernels (metal/runtime.cpp on x86,
 * metal/arm64/cxx_runtime.cpp on AArch64). Each includer defines sqrt and
 * sqrtf itself - the one architecture-specific instruction - before this.
 *
 * Checked against the host C library by metal/tools/libm-check.py, which
 * compiles THIS file on the host and compares every function over dense
 * grids and the edge cases. The version this replaced was wrong where scene
 * code lives: atan/asin/acos/atan2 up to 0.8% off (a slowly converging
 * series near 1), exp() returning -6.6e303 for inputs in (-745, -708),
 * floor/ceil/round returning -9.2e18 for large or infinite inputs (the
 * float->int cast saturates), pow(0,0) and negative bases NaN, log(inf) 709,
 * and sin/cos NaN for large arguments.
 *
 * Everything is written with ordinary double arithmetic plus bit access;
 * no host headers. */

namespace libm_detail {
inline uint64_t bits(double x) { union { double d; uint64_t u; } v; v.d = x; return v.u; }
inline double from_bits(uint64_t u) { union { double d; uint64_t u; } v; v.u = u; return v.d; }
inline bool is_nan(double x) { return x != x; }
inline bool is_inf(double x) { return (bits(x) & 0x7FFFFFFFFFFFFFFFull) == 0x7FF0000000000000ull; }
inline bool is_finite(double x) { return (bits(x) & 0x7FF0000000000000ull) != 0x7FF0000000000000ull; }
inline double copy_sign(double mag, double sgn) {
  return from_bits((bits(mag) & 0x7FFFFFFFFFFFFFFFull) | (bits(sgn) & 0x8000000000000000ull));
}
const double kNaN = from_bits(0x7FF8000000000000ull);
const double kInf = from_bits(0x7FF0000000000000ull);
const double kPi = 3.141592653589793238, kHalfPi = 1.570796326794896619, kQuarterPi = 0.785398163397448310;
/* x * 2^n for any n, through the subnormal range. */
inline double scale2(double x, int n) {
  while (n > 1000) { x *= from_bits(static_cast<uint64_t>(1023 + 1000) << 52); n -= 1000; }
  while (n < -1000) { x *= from_bits(static_cast<uint64_t>(1023 - 1000) << 52); n += 1000; }
  return x * from_bits(static_cast<uint64_t>(1023 + n) << 52);
}
}  // namespace libm_detail

double fabs(double x) { return libm_detail::from_bits(libm_detail::bits(x) & 0x7FFFFFFFFFFFFFFFull); }
float fabsf(float x) { return x < 0 ? -x : x; }

/* Values at or beyond 2^52 are already integers, and NaN/inf pass through:
 * the old cast to long long saturated them to -9.2e18. */
double floor(double x) {
  if (!(fabs(x) < 4503599627370496.0)) return x;
  const long long i = static_cast<long long>(x);
  const double d = static_cast<double>(i);
  return (x < 0 && x != d) ? d - 1.0 : d;
}
double ceil(double x) {
  if (!(fabs(x) < 4503599627370496.0)) return x;
  const double f = floor(x);
  return f == x ? x : f + 1.0;
}
double round(double x) {
  if (!(fabs(x) < 4503599627370496.0)) return x;
  return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5);
}
double trunc(double x) { return x < 0 ? ceil(x) : floor(x); }
float floorf(float x) { return static_cast<float>(floor(static_cast<double>(x))); }
float ceilf(float x) { return static_cast<float>(ceil(static_cast<double>(x))); }
float roundf(float x) { return static_cast<float>(round(static_cast<double>(x))); }

/* Exact remainder by scaled subtraction - correct for every finite pair,
 * where a / b cast to long long was not. */
double fmod(double a, double b) {
  using namespace libm_detail;
  if (is_nan(a) || is_nan(b) || is_inf(a) || b == 0) return kNaN;
  if (is_inf(b)) return a;
  double r = fabs(a);
  const double d = fabs(b);
  if (r < d) return a;
  while (r >= d) {
    double t = d;
    while (t <= r * 0.5) t += t;
    r -= t;
  }
  return copy_sign(r, a);
}

double exp(double x) {
  using namespace libm_detail;
  if (is_nan(x)) return x;
  if (x > 709.782712893384) return kInf;
  if (x < -745.1332191019412) return 0.0;
  const double ln2_hi = 0.6931471803691238, ln2_lo = 1.9082149292705877e-10;
  const double kf = floor(x * 1.4426950408889634 + 0.5);
  const int k = static_cast<int>(kf);
  const double r = (x - kf * ln2_hi) - kf * ln2_lo;         /* |r| <= ln2/2 */
  double t = 1.0, s = 1.0;
  for (int i = 1; i <= 14; ++i) { t *= r / i; s += t; }
  return scale2(s, k);                                      /* handles subnormal results */
}
float expf(float x) { return static_cast<float>(exp(static_cast<double>(x))); }

double log(double x) {
  using namespace libm_detail;
  if (is_nan(x)) return x;
  if (x < 0) return kNaN;
  if (x == 0) return -kInf;
  if (is_inf(x)) return x;
  int e = 0;
  if (x < 2.2250738585072014e-308) { x *= 18014398509481984.0; e = -54; }  /* subnormal: scale by 2^54 */
  uint64_t u = bits(x);
  e += static_cast<int>((u >> 52) & 0x7FF) - 1023;
  u = (u & 0x000FFFFFFFFFFFFFull) | (0x3FFull << 52);
  double m = from_bits(u);
  if (m > 1.4142135623730951) { m *= 0.5; e += 1; }
  const double f = (m - 1) / (m + 1), f2 = f * f;
  double s = 0, t = f;
  for (int i = 1; i < 40; i += 2) { s += t / i; t *= f2; }
  return 2 * s + e * 0.6931471805599453;
}
float logf(float x) { return static_cast<float>(log(static_cast<double>(x))); }
double log2(double x) { return log(x) / 0.6931471805599453; }
float log2f(float x) { return static_cast<float>(log2(static_cast<double>(x))); }
double log10(double x) { return log(x) / 2.302585092994046; }
float log10f(float x) { return static_cast<float>(log10(static_cast<double>(x))); }

double pow(double a, double b) {
  using namespace libm_detail;
  if (b == 0) return 1.0;                       /* includes pow(NaN, 0) and pow(0, 0) */
  if (a == 1.0) return 1.0;
  if (is_nan(a) || is_nan(b)) return kNaN;
  const bool b_int = fabs(b) < 9007199254740992.0 && floor(b) == b;
  const bool b_odd = b_int && fmod(fabs(b), 2.0) == 1.0;
  if (a == 0) {
    if (b > 0) return b_odd ? a : 0.0;
    return b_odd ? copy_sign(kInf, a) : kInf;
  }
  if (is_inf(b)) {
    const double m = fabs(a);
    if (m == 1.0) return 1.0;
    return ((m > 1.0) == (b > 0)) ? kInf : 0.0;
  }
  if (is_inf(a)) {
    if (a > 0) return b > 0 ? kInf : 0.0;
    const double r = b > 0 ? kInf : 0.0;
    return b_odd ? -r : r;
  }
  /* Small integer powers by squaring: exact whenever the answer is
   * representable (pow(10, 2) is 100, not exp(2 * log(10)) =
   * 100.00000000000001, which is what made round(x, 2) miss). */
  if (b_int && fabs(b) <= 64) {
    uint64_t n = static_cast<uint64_t>(fabs(b));
    double base = a, r = 1.0;
    while (n) { if (n & 1) r *= base; base *= base; n >>= 1; }
    return b < 0 ? 1.0 / r : r;
  }
  if (a < 0) {
    if (!b_int) return kNaN;
    const double r = exp(b * log(-a));
    return b_odd ? -r : r;
  }
  return exp(b * log(a));
}
float powf(float a, float b) { return static_cast<float>(pow(static_cast<double>(a), static_cast<double>(b))); }

namespace libm_detail {
inline double sin_core(double r) { double t = r, s = r, r2 = r * r; for (int i = 3; i <= 21; i += 2) { t *= -r2 / static_cast<double>((i - 1) * i); s += t; } return s; }
inline double cos_core(double r) { double t = 1.0, s = 1.0, r2 = r * r; for (int i = 2; i <= 20; i += 2) { t *= -r2 / static_cast<double>((i - 1) * i); s += t; } return s; }
/* 2/pi to 1280 bits (generated by exact integer arithmetic; verified
 * against the published expansion a2f9836e 4e441529 fc2757d1 ...). */
const uint64_t kTwoOverPi[20] = {
  0xa2f9836e4e441529ull, 0xfc2757d1f534ddc0ull, 0xdb6295993c439041ull,
  0xfe5163abdebbc561ull, 0xb7246e3a424dd2e0ull, 0x06492eea09d1921cull,
  0xfe1deb1cb129a73eull, 0xe88235f52ebb4484ull, 0xe99c7026b45f7e41ull,
  0x3991d639835339f4ull, 0x9c845f8bbdf9283bull, 0x1ff897ffde05980full,
  0xef2f118b5a0a6d1full, 0x6d367ecf27cb09b7ull, 0x4f463f669e5fea2dull,
  0x7527bac7ebe5f17bull, 0x3d0739f78a5292eaull, 0x6bfb5fb11f8d5d08ull,
  0x56033046fc7b6babull, 0xf0cfbc209af4361dull
};

/* Payne-Hanek: x * 2/pi mod 4, exactly, for large |x|. With x = m * 2^E
 * (m a 53-bit integer), bits of 2/pi weighted 2^2 and above contribute only
 * multiples of 4, so a 192-bit window of 2/pi starting just below that
 * point, times m, carries the quadrant and 128 bits of fraction. */
inline void reduce_payne_hanek(double ax, double& r, int& q) {
  const uint64_t u = bits(ax);
  const int E = static_cast<int>((u >> 52) & 0x7FF) - 1075;
  const uint64_t m = (u & 0x000FFFFFFFFFFFFFull) | 0x0010000000000000ull;
  const int start = E - 1 > 1 ? E - 1 : 1;       /* first 2/pi bit (1-based) that matters */
  /* P = 192 bits of 2/pi from bit `start`, as three limbs, high first. */
  uint64_t P[3];
  for (int k = 0; k < 3; ++k) {
    const int bit0 = start - 1 + 64 * k;          /* 0-based index of this limb's top bit */
    const int w = bit0 / 64, sh = bit0 % 64;
    const uint64_t hi = w < 20 ? kTwoOverPi[w] : 0, lo = w + 1 < 20 ? kTwoOverPi[w + 1] : 0;
    P[k] = sh ? (hi << sh) | (lo >> (64 - sh)) : hi;
  }
  /* m * P -> 256 bits, prod[0] most significant. */
  uint64_t prod[4] = {0, 0, 0, 0};
  unsigned __int128 carry = 0;
  for (int k = 2; k >= 0; --k) {
    const unsigned __int128 t = static_cast<unsigned __int128>(m) * P[k] + carry;
    prod[k + 1] = static_cast<uint64_t>(t);
    carry = t >> 64;
  }
  prod[0] = static_cast<uint64_t>(carry);
  /* The binary point sits b bits above the product's least significant bit. */
  const int b = start + 191 - E;
  auto bit_at = [&](int pos) -> uint64_t {        /* pos 0 = LSB of the 256-bit product */
    if (pos < 0 || pos > 255) return 0;
    return (prod[3 - pos / 64] >> (pos % 64)) & 1u;
  };
  auto word_below = [&](int top) -> uint64_t {    /* 64 bits ending just below bit `top` */
    uint64_t v = 0;
    for (int i = 1; i <= 64; ++i) v = (v << 1) | bit_at(top - i);
    return v;
  };
  int quad = static_cast<int>(bit_at(b) | (bit_at(b + 1) << 1));
  const uint64_t f_hi = word_below(b), f_lo = word_below(b - 64);
  /* fraction in [0,1) -> signed in [-0.5, 0.5) with the quadrant adjusted */
  double frac = static_cast<double>(f_hi) * 5.421010862427522e-20 + static_cast<double>(f_lo) * 2.938735877055719e-39;
  if (frac >= 0.5) { frac -= 1.0; quad = (quad + 1) & 3; }
  r = frac * 1.5707963267948966;
  q = quad;
}

/* x = n*pi/2 + r, |r| <= pi/4. Cody-Waite with pi/2 in three parts while n
 * is small enough for n*p1 to be exact; Payne-Hanek beyond. */
inline void reduce_quadrant(double x, double& r, int& q) {
  if (fabs(x) > 4.0e5) {
    reduce_payne_hanek(fabs(x), r, q);
    if (x < 0) { r = -r; q = (4 - q) & 3; }
    return;
  }
  const double n = round(x * 0.6366197723675814);
  const double p1 = 1.5707963267341256, p2 = 6.077100506506192e-11, p3 = 2.0222662487959506e-21;
  r = ((x - n * p1) - n * p2) - n * p3;
  long long qi = static_cast<long long>(fmod(n, 4.0));
  if (qi < 0) qi += 4;
  q = static_cast<int>(qi);
}
/* atan on [0, 1]: fold [tan(pi/8), 1] onto [-tan(pi/8), 0] through
 * atan(x) = pi/4 + atan((x-1)/(x+1)); the series then converges fast. */
inline double atan_01(double x) {
  double base = 0.0;
  if (x > 0.41421356237309503) { x = (x - 1.0) / (x + 1.0); base = kQuarterPi; }
  const double x2 = x * x;
  double t = x, s = 0.0;
  for (int i = 1; i < 60; i += 2) {
    s += ((i >> 1) & 1 ? -t : t) / static_cast<double>(i);
    t *= x2;
    if (fabs(t) < 1e-22) break;
  }
  return base + s;
}
}  // namespace libm_detail

double sin(double x) {
  using namespace libm_detail;
  if (!is_finite(x)) return kNaN;
  double r; int q; reduce_quadrant(x, r, q);
  switch (q) { case 0: return sin_core(r); case 1: return cos_core(r); case 2: return -sin_core(r); default: return -cos_core(r); }
}
double cos(double x) {
  using namespace libm_detail;
  if (!is_finite(x)) return kNaN;
  double r; int q; reduce_quadrant(x, r, q);
  switch (q) { case 0: return cos_core(r); case 1: return -sin_core(r); case 2: return -cos_core(r); default: return sin_core(r); }
}
float sinf(float x) { return static_cast<float>(sin(static_cast<double>(x))); }
float cosf(float x) { return static_cast<float>(cos(static_cast<double>(x))); }
/* GCC fuses a sin and a cos of the same argument into these, so code that
 * never names them still needs them to exist. */
void sincosf(float x, float* s, float* c) { if (s) *s = sinf(x); if (c) *c = cosf(x); }
void sincos(double x, double* s, double* c) { if (s) *s = sin(x); if (c) *c = cos(x); }
double tan(double x) { const double c = cos(x); return c == 0 ? libm_detail::kInf : sin(x) / c; }

double atan(double x) {
  using namespace libm_detail;
  if (is_nan(x)) return x;
  const bool neg = x < 0;
  double a = fabs(x);
  double r;
  if (is_inf(a)) r = kHalfPi;
  else if (a > 1.0) r = kHalfPi - atan_01(1.0 / a);
  else r = atan_01(a);
  return neg ? -r : r;
}
double atan2(double y, double x) {
  using namespace libm_detail;
  if (is_nan(x) || is_nan(y)) return kNaN;
  if (x > 0) return atan(y / x);
  if (x < 0) return (bits(y) >> 63) ? atan(y / x) - kPi : atan(y / x) + kPi;
  if (y > 0) return kHalfPi;
  if (y < 0) return -kHalfPi;
  return (bits(x) >> 63) ? copy_sign(kPi, y) : y;   /* atan2(+-0, -0) = +-pi */
}
double hypot(double a, double b) {
  a = fabs(a); b = fabs(b);
  if (a < b) { const double t = a; a = b; b = t; }
  if (a == 0) return 0.0;
  const double r = b / a;
  return a * sqrt(1.0 + r * r);                 /* no overflow for large inputs */
}
double acos(double x) {
  if (libm_detail::is_nan(x) || x > 1.0 || x < -1.0) return libm_detail::kNaN;
  return atan2(sqrt((1.0 - x) * (1.0 + x)), x);
}
double asin(double x) {
  if (libm_detail::is_nan(x) || x > 1.0 || x < -1.0) return libm_detail::kNaN;
  return atan2(x, sqrt((1.0 - x) * (1.0 + x)));
}
float acosf(float x) { return static_cast<float>(acos(static_cast<double>(x))); }
float asinf(float x) { return static_cast<float>(asin(static_cast<double>(x))); }
float atan2f(float y, float x) { return static_cast<float>(atan2(static_cast<double>(y), static_cast<double>(x))); }

long lround(double x) { return static_cast<long>(round(x)); }
long lroundf(float x) { return lround(static_cast<double>(x)); }
long long llround(double x) { return static_cast<long long>(round(x)); }

double tanh(double x) {
  if (x > 20.0) return 1.0;
  if (x < -20.0) return -1.0;
  const double e = exp(2.0 * x);
  return (e - 1.0) / (e + 1.0);
}
float tanhf(float x) { return static_cast<float>(tanh(static_cast<double>(x))); }
double sinh(double x) { return (exp(x) - exp(-x)) * 0.5; }
float sinhf(float x) { return static_cast<float>(sinh(static_cast<double>(x))); }
double cosh(double x) { return (exp(x) + exp(-x)) * 0.5; }
float coshf(float x) { return static_cast<float>(cosh(static_cast<double>(x))); }
