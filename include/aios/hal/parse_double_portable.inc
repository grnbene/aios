// A double read from text with no C library, for a substrate that has none (bare metal): up to 19 significant
// digits scaled by a power of ten. Within an ulp or so of strtod - not the exact rounding a host library gives,
// which is why hosted builds use theirs. Included by the bare-metal HALs only.
inline double aios_parse_double_portable(const char* s, char** end) {
  const char* p = s;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') ++p;
  bool neg = false;
  if (*p == '+' || *p == '-') { neg = *p == '-'; ++p; }
  auto ieq = [](const char* a, const char* w) { for (; *w; ++a, ++w) if ((*a | 0x20) != *w) return false; return true; };
  if (ieq(p, "inf")) { p += ieq(p, "infinity") ? 8 : 3; if (end) *end = (char*)p; return neg ? -__builtin_inf() : __builtin_inf(); }
  if (ieq(p, "nan")) { p += 3; if (end) *end = (char*)p; return __builtin_nan(""); }
  unsigned long long mant = 0;
  int digits = 0, exp10 = 0;
  bool any = false;
  for (; *p >= '0' && *p <= '9'; ++p, any = true) {
    if (digits < 19) { mant = mant * 10 + (unsigned)(*p - '0'); if (mant) ++digits; } else ++exp10;
  }
  if (*p == '.') {
    ++p;
    for (; *p >= '0' && *p <= '9'; ++p, any = true)
      if (digits < 19) { mant = mant * 10 + (unsigned)(*p - '0'); if (mant) ++digits; --exp10; }
  }
  if (!any) { if (end) *end = (char*)s; return 0.0; }
  if (*p == 'e' || *p == 'E') {
    const char* q = p + 1;
    bool eneg = false;
    if (*q == '+' || *q == '-') { eneg = *q == '-'; ++q; }
    if (*q >= '0' && *q <= '9') {
      int e = 0;
      for (; *q >= '0' && *q <= '9'; ++q) if (e < 100000) e = e * 10 + (*q - '0');
      exp10 += eneg ? -e : e;
      p = q;
    }
  }
  if (end) *end = (char*)p;
  long double v = (long double)mant, scale = 1.0L, base = 10.0L;
  for (int n = exp10 < 0 ? -exp10 : exp10; n > 0; n >>= 1, base *= base) if (n & 1) scale *= base;
  v = exp10 < 0 ? v / scale : v * scale;
  return neg ? -(double)v : (double)v;
}
