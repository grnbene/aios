#include "out.hpp"

namespace aios::cli {

std::string fmt_u64(uint64_t v) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = char('0' + v % 10); v /= 10; } while (v); return std::string(b + i); }
std::string fmt_i64(int64_t v) { if (v < 0) return "-" + fmt_u64((uint64_t)(-(v + 1)) + 1); return fmt_u64((uint64_t)v); }

static std::string digits_fixed(double av, int prec) {
  // av >= 0. Scale, round, split.
  double scale = 1; for (int i = 0; i < prec; ++i) scale *= 10;
  double scaled = av * scale + 0.5;
  if (scaled > 1.8e19) return "inf";
  uint64_t m = (uint64_t)scaled;
  uint64_t ip = (uint64_t)(m / (uint64_t)scale), fp = m - ip * (uint64_t)scale;
  std::string s = fmt_u64(ip);
  if (prec > 0) { std::string f = fmt_u64(fp); while ((int)f.size() < prec) f = "0" + f; s += "." + f; }
  return s;
}

std::string fmt_double(double v, int sig, bool fixed, int prec) {
  if (v != v) return "nan";
  if (v > 1.7976931348623157e308) return "inf";
  if (v < -1.7976931348623157e308) return "-inf";
  std::string sign = v < 0 ? "-" : "";
  double av = v < 0 ? -v : v;
  if (fixed) return sign + digits_fixed(av, prec < 0 ? 0 : prec);
  if (av == 0) return "0";
  if (sig < 1) sig = 1;
  if (sig > 17) sig = 17;
  int e = 0;                                   // decimal exponent: av = m × 10^e, 1 <= m < 10
  double m = av;
  while (m >= 10) { m /= 10; ++e; }
  while (m < 1) { m *= 10; --e; }
  double scale = 1; for (int i = 1; i < sig; ++i) scale *= 10;
  uint64_t d = (uint64_t)(m * scale + 0.5);
  if (d >= (uint64_t)(scale * 10)) { d /= 10; ++e; }
  std::string ds = fmt_u64(d);                  // sig digits
  while (ds.size() > 1 && ds.back() == '0') ds.pop_back();
  if (e >= -5 && e < sig) {
    std::string s;
    if (e < 0) { s = "0."; for (int i = -1; i > e; --i) s += "0"; s += ds; }
    else { s = ds.substr(0, (size_t)e + 1); while ((int)s.size() < e + 1) s += "0"; if (ds.size() > (size_t)e + 1) s += "." + ds.substr((size_t)e + 1); }
    return sign + s;
  }
  std::string s = ds.substr(0, 1); if (ds.size() > 1) s += "." + ds.substr(1);
  s += "e"; s += e < 0 ? "-" : "+"; std::string es = fmt_u64((uint64_t)(e < 0 ? -e : e)); if (es.size() < 2) es = "0" + es;
  return sign + s + es;
}

}  // namespace aios::cli
