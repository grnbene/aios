#pragma once
// The OS's native encoding for anything that must leave a register: a compact
// little-endian binary layout. No text protocols, no schemas negotiated at
// runtime; both sides are the same kernel or a peer that speaks this layout.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <span>
#include "aios/core/ids.hpp"
#include "aios/core/result.hpp"

namespace aios {

using ByteBuffer = std::vector<uint8_t>;
using ByteSpan = std::span<const uint8_t>;
using MutableByteSpan = std::span<uint8_t>;

inline ByteSpan span_of(const std::string& s) { return ByteSpan(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
inline std::string string_of(ByteSpan b) { return std::string(reinterpret_cast<const char*>(b.data()), b.size()); }

class Writer {
 public:
  Writer() = default;
  explicit Writer(ByteBuffer& out) : buf_(&out) {}
  void u8(uint8_t v) { buf().push_back(v); }
  void u16(uint16_t v) { for (int i = 0; i < 2; ++i) buf().push_back(uint8_t(v >> (8 * i))); }
  void u32(uint32_t v) { for (int i = 0; i < 4; ++i) buf().push_back(uint8_t(v >> (8 * i))); }
  void u64(uint64_t v) { for (int i = 0; i < 8; ++i) buf().push_back(uint8_t(v >> (8 * i))); }
  void f64(double v) { uint64_t u; std::memcpy(&u, &v, 8); u64(u); }
  void f32(float v) { uint32_t u; std::memcpy(&u, &v, 4); u32(u); }
  void id(const Id128& v) { u64(v.hi); u64(v.lo); }
  void bytes(ByteSpan b) { u64(b.size()); buf().insert(buf().end(), b.begin(), b.end()); }
  void raw(ByteSpan b) { buf().insert(buf().end(), b.begin(), b.end()); }
  void str(const std::string& s) { bytes(span_of(s)); }
  ByteBuffer& buf() { return buf_ ? *buf_ : own_; }
  ByteBuffer take() { return buf_ ? ByteBuffer(*buf_) : std::move(own_); }
 private:
  ByteBuffer* buf_ = nullptr;
  ByteBuffer own_;
};

class Reader {
 public:
  explicit Reader(ByteSpan data) : data_(data) {}
  bool ok() const { return ok_; }
  size_t remaining() const { return data_.size() - pos_; }
  size_t position() const { return pos_; }
  ByteSpan rest() const { return data_.subspan(pos_); }   // what is left to read, without reading it
  uint8_t u8() { uint8_t v = 0; take(&v, 1); return v; }
  uint16_t u16() { uint16_t v = 0; take_le(v); return v; }
  uint32_t u32() { uint32_t v = 0; take_le(v); return v; }
  uint64_t u64() { uint64_t v = 0; take_le(v); return v; }
  double f64() { uint64_t u = u64(); double v; std::memcpy(&v, &u, 8); return v; }
  float f32() { uint32_t u = u32(); float v; std::memcpy(&v, &u, 4); return v; }
  Id128 id() { Id128 v; v.hi = u64(); v.lo = u64(); return v; }
  ByteSpan bytes() {
    uint64_t n = u64();
    if (!ok_ || n > remaining()) { ok_ = false; return {}; }
    ByteSpan s = data_.subspan(pos_, n);
    pos_ += n;
    return s;
  }
  ByteSpan raw(size_t n) {
    if (n > remaining()) { ok_ = false; return {}; }
    ByteSpan s = data_.subspan(pos_, n);
    pos_ += n;
    return s;
  }
  std::string str() { return string_of(bytes()); }
 private:
  template <typename T> void take_le(T& v) {
    uint8_t tmp[sizeof(T)];
    if (!take(tmp, sizeof(T))) return;
    uint64_t acc = 0;
    for (size_t i = 0; i < sizeof(T); ++i) acc |= uint64_t(tmp[i]) << (8 * i);
    v = T(acc);
  }
  bool take(uint8_t* dst, size_t n) {
    if (!ok_ || n > remaining()) { ok_ = false; return false; }
    std::memcpy(dst, data_.data() + pos_, n);
    pos_ += n;
    return true;
  }
  ByteSpan data_;
  size_t pos_ = 0;
  bool ok_ = true;
};

// FNV-1a: used for content fingerprints and for naming registered kernels.
uint64_t fnv1a(ByteSpan data, uint64_t seed = 0xcbf29ce484222325ull);
inline uint64_t fnv1a(std::string_view sv) { return fnv1a(ByteSpan(reinterpret_cast<const uint8_t*>(sv.data()), sv.size())); }
inline uint64_t fnv1a(const char* s) { return fnv1a(std::string_view(s)); }
inline uint64_t fnv1a(const std::string& s) { return fnv1a(span_of(s)); }

std::string hex(ByteSpan b, size_t max = 64);

}  // namespace aios
