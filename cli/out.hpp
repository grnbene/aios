#pragma once
// A host-independent output stream for the command surface: formats into a
// buffer and hands lines to a sink (stdout on Linux, the console on bare
// metal). No iostreams, no printf.
#include <string>
#include <functional>
#include <cstdint>
#include <type_traits>

namespace aios::cli {

struct W { int n; };        // left-justify the next item in a field of n
struct Fixed { int prec; }; // next floating value: fixed, prec decimals
struct Prec { int sig; };   // next floating value: sig significant digits

std::string fmt_double(double v, int sig, bool fixed, int prec);
std::string fmt_u64(uint64_t v);
std::string fmt_i64(int64_t v);

struct Style { const char* code; };

inline Style Reset() { return Style{"\033[0m"}; }
inline Style Bold() { return Style{"\033[1m"}; }
inline Style Dim() { return Style{"\033[2m"}; }
inline Style Red() { return Style{"\033[31m"}; }
inline Style Green() { return Style{"\033[32m"}; }
inline Style Yellow() { return Style{"\033[33m"}; }
inline Style Blue() { return Style{"\033[34m"}; }
inline Style Cyan() { return Style{"\033[36m"}; }
inline Style Header() { return Style{"\033[1;36m"}; }

class Out {
 public:
  using Sink = std::function<void(const std::string&)>;
  explicit Out(Sink sink) : sink_(std::move(sink)) {}
  ~Out() { flush(); }
  void set_colour(bool c) { colour_enabled_ = c; }
  bool colour_enabled() const { return colour_enabled_; }
  Style style(Style st) const { return colour_enabled_ ? st : Style{""}; }
  Out& operator<<(Style st) { if (colour_enabled_ && st.code && st.code[0]) emit(st.code); return *this; }
  Out& operator<<(const std::string& s) { emit(s); return *this; }
  Out& operator<<(const char* s) { emit(s ? s : ""); return *this; }
  Out& operator<<(char c) { emit(std::string(1, c)); return *this; }
  Out& operator<<(bool b) { emit(b ? "true" : "false"); return *this; }
  Out& operator<<(double v) { emit(fmt_double(v, sig_, fixed_, prec_)); fixed_ = false; sig_ = 6; return *this; }
  Out& operator<<(float v) { return *this << (double)v; }
  template <typename T, typename = std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>>>
  Out& operator<<(T v) { if constexpr (std::is_signed_v<T>) emit(fmt_i64((int64_t)v)); else emit(fmt_u64((uint64_t)v)); return *this; }
  Out& operator<<(W w) { width_ = w.n; return *this; }
  Out& operator<<(Fixed f) { fixed_ = true; prec_ = f.prec; return *this; }
  Out& operator<<(Prec p) { fixed_ = false; sig_ = p.sig; return *this; }
  void flush() { if (!buf_.empty()) { sink_(buf_); buf_.clear(); } }
 private:
  void emit(std::string s) {
    if (width_ > 0) { while ((int)s.size() < width_) s.push_back(' '); width_ = 0; }
    buf_ += s;
    if (buf_.find('\n') != std::string::npos || buf_.size() > 4096) flush();
  }
  Sink sink_;
  std::string buf_;
  int width_ = 0, sig_ = 6, prec_ = 2;
  bool fixed_ = false;
  bool colour_enabled_ = false;
};

}  // namespace aios::cli
