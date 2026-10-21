#pragma once
// SHA-512 (FIPS 180-4). Ed25519 is defined over it, so the machine needs its
// own: nothing here links a crypto library.
#include <array>
#include <cstdint>
#include "aios/core/bytes.hpp"

namespace aios::crypto {

std::array<uint8_t, 64> sha512(ByteSpan data);
// SHA-384: the same compression function under a different initial state,
// truncated. Certificate chains sign with it (ecdsa-with-SHA384), so a machine
// that cannot compute it cannot check who it is talking to.
std::array<uint8_t, 48> sha384(ByteSpan data);

// Streaming, for the places that hash more than one buffer in a row.
class Sha512 {
 public:
  explicit Sha512(bool truncated384 = false) : sha384_(truncated384) { reset(); }
  void reset();
  void update(ByteSpan data);
  std::array<uint8_t, 64> finish();

 private:
  void block(const uint8_t* p);
  bool sha384_ = false;
  uint64_t h_[8];
  uint8_t buf_[128];
  size_t buffered_ = 0;
  uint64_t total_ = 0;   // bytes
};

}  // namespace aios::crypto
