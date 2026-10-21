#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include "aios/core/bytes.hpp"

namespace aios::crypto {

std::array<uint8_t, 32> hmac_sha256(ByteSpan key, ByteSpan message);

// PBKDF2 (RFC 8018) with HMAC-SHA256 as the PRF. Fills `out` with `out_len`
// derived bytes. The standard password KDF; checked against the RFC 7914 §11
// PBKDF2-HMAC-SHA256 vectors.
void pbkdf2_hmac_sha256(ByteSpan password, ByteSpan salt, uint32_t iterations, uint8_t* out, size_t out_len);

// RFC 6979 HMAC_DRBG over SHA-256. The caller supplies the already-formatted
// key and message octet strings; the generator hands back 32-byte candidates
// and the caller decides whether each one is in range.
class Rfc6979 {
 public:
  Rfc6979(ByteSpan key_octets, ByteSpan message_octets);
  void generate(uint8_t out[32]);
  void retry();

 private:
  std::array<uint8_t, 32> k_;
  std::array<uint8_t, 32> v_;
};

}  // namespace aios::crypto
