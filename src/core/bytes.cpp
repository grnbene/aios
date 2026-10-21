#include "aios/core/bytes.hpp"

namespace aios {
uint64_t fnv1a(ByteSpan data, uint64_t seed) {
  uint64_t h = seed;
  for (uint8_t b : data) { h ^= b; h *= 0x100000001b3ull; }
  return h;
}
std::string hex(ByteSpan b, size_t max) {
  static const char* H = "0123456789abcdef";
  std::string s;
  size_t n = b.size() < max ? b.size() : max;
  for (size_t i = 0; i < n; ++i) { s.push_back(H[b[i] >> 4]); s.push_back(H[b[i] & 15]); }
  if (b.size() > max) s += "…";
  return s;
}
}  // namespace aios
