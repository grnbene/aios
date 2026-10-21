#pragma once
// A string builder for the kernel core: what std::ostringstream was used for,
// without iostream (which bare metal has no locale or streambuf for).
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

namespace aios {

class StrBuf {
 public:
  StrBuf& operator<<(const std::string& s) { s_ += s; return *this; }
  StrBuf& operator<<(const char* s) { if (s) s_ += s; return *this; }
  StrBuf& operator<<(char c) { s_ += c; return *this; }
  StrBuf& operator<<(bool b) { s_ += b ? "1" : "0"; return *this; }
  StrBuf& operator<<(double d) { char b[32]; std::snprintf(b, sizeof b, "%g", d); s_ += b; return *this; }
  StrBuf& operator<<(float f) { return *this << (double)f; }
  template <class T, class = std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>>>
  StrBuf& operator<<(T v) { s_ += std::to_string(v); return *this; }
  const std::string& str() const { return s_; }
  bool empty() const { return s_.empty(); }
 private:
  std::string s_;
};

// `text` cut at every `delim`; a trailing delimiter adds no empty last piece
// (the same pieces std::getline would have produced).
inline std::vector<std::string> split_lines(const std::string& text, char delim = '\n') {
  std::vector<std::string> out;
  size_t at = 0;
  while (at < text.size()) {
    size_t nl = text.find(delim, at);
    if (nl == std::string::npos) { out.push_back(text.substr(at)); break; }
    out.push_back(text.substr(at, nl - at));
    at = nl + 1;
  }
  return out;
}

}  // namespace aios

// Number parsing for the core, with no libc: digits in `base` (10 or 16) after
// optional whitespace and sign. `end` receives where parsing stopped.
namespace aios {
inline int digit_value(char c, int base) {
  int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 99;
  return v < base ? v : -1;
}
inline unsigned long long to_u64(const char* s, const char** end = nullptr, int base = 10) {
  const char* p = s ? s : "";
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
  bool neg = false;
  if (*p == '+' || *p == '-') { neg = *p == '-'; ++p; }
  if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
  unsigned long long v = 0; const char* start = p;
  for (int d; (d = digit_value(*p, base)) >= 0; ++p) v = v * (unsigned)base + (unsigned)d;
  if (end) *end = p == start ? s : p;
  return neg ? (unsigned long long)(-(long long)v) : v;
}
inline long long to_i64(const char* s, const char** end = nullptr, int base = 10) { return (long long)to_u64(s, end, base); }
inline double to_f64(const char* s, const char** end = nullptr) {
  const char* p = s ? s : "";
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
  bool neg = false;
  if (*p == '+' || *p == '-') { neg = *p == '-'; ++p; }
  const char* start = p;
  double v = 0;
  while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ++p; }
  if (*p == '.') { ++p; double f = 0.1; while (*p >= '0' && *p <= '9') { v += (*p - '0') * f; f *= 0.1; ++p; } }
  if (p == start || (p == start + 1 && start[0] == '.')) { if (end) *end = s; return 0; }
  if (*p == 'e' || *p == 'E') {
    const char* q = p + 1; bool eneg = false;
    if (*q == '+' || *q == '-') { eneg = *q == '-'; ++q; }
    if (*q >= '0' && *q <= '9') {
      int ex = 0; while (*q >= '0' && *q <= '9') { ex = ex * 10 + (*q - '0'); ++q; }
      double scale = 1; for (int i = 0; i < ex; ++i) scale *= 10;
      v = eneg ? v / scale : v * scale; p = q;
    }
  }
  if (end) *end = p;
  return neg ? -v : v;
}
}  // namespace aios
