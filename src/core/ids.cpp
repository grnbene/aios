#include "aios/core/ids.hpp"
#include <atomic>

namespace aios {

static const char* HEX = "0123456789abcdef";

std::string Id128::str() const {
  std::string s(32, '0');
  for (int i = 0; i < 16; ++i) {
    s[i] = HEX[(hi >> (60 - i * 4)) & 0xF];
    s[16 + i] = HEX[(lo >> (60 - i * 4)) & 0xF];
  }
  return s;
}

std::string Id128::short_str() const { return str().substr(0, 12); }

bool Id128::parse(const std::string& in, Id128& out) {
  std::string s;
  for (char c : in) if (c != '-') s.push_back(c);
  if (s.size() != 32) return false;
  uint64_t v[2] = {0, 0};
  for (size_t i = 0; i < 32; ++i) {
    char c = s[i];
    uint64_t d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
    else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
    else return false;
    v[i / 16] = (v[i / 16] << 4) | d;
  }
  out.hi = v[0];
  out.lo = v[1];
  return true;
}

IdGenerator::IdGenerator(uint64_t boot_entropy, uint64_t boot_ns)
    : entropy_(boot_entropy ^ (boot_ns * 0xD6E8FEB86659FD93ull)), counter_(boot_ns & 0xFFFFFFFFull) {}

Id128 IdGenerator::next() {
  // hi carries the boot entropy; lo carries a monotonic counter mixed with it so
  // ids never collide within a boot and are unpredictable across boots.
  uint64_t c = ++counter_;
  uint64_t lo = (c << 20) ^ (entropy_ >> 44) ^ ((entropy_ & 0xFFFFF));
  uint64_t hi = entropy_ ^ (c * 0x9E3779B97F4A7C15ull);
  if (hi == 0 && lo == 0) lo = 1;
  return Id128{hi, lo};
}

}  // namespace aios
