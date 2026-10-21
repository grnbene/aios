#pragma once
#include <array>
#include <cstdint>
#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"

namespace aios::crypto {

struct EcSignature {
  std::array<uint8_t, 32> r{};
  std::array<uint8_t, 32> s{};
  int recovery_id = 0;
};

bool ec_seckey_valid(const uint8_t sec[32]);

// Compressed is 33 bytes (0x02/0x03 || x), uncompressed 65 (0x04 || x || y).
Result<ByteBuffer> ec_pubkey_create(const uint8_t sec[32], bool compressed);
Result<ByteBuffer> ec_pubkey_convert(ByteSpan pubkey, bool compressed);

// Deterministic per RFC 6979 with SHA-256, low-S normalised. The same key and
// message always produce the same signature, which is what makes a differential
// comparison against another implementation exact rather than statistical.
Result<EcSignature> ecdsa_sign(const uint8_t sec[32], const uint8_t msg32[32]);
bool ecdsa_verify(ByteSpan pubkey, const uint8_t msg32[32], const EcSignature& sig);
Result<ByteBuffer> ecdsa_recover(const EcSignature& sig, const uint8_t msg32[32], bool compressed);

ByteBuffer ecdsa_der_encode(const EcSignature& sig);
Result<EcSignature> ecdsa_der_decode(ByteSpan der);

// The Ethereum account address: the low 20 bytes of the keccak digest of the
// uncompressed public key with its 0x04 prefix removed.
Result<std::array<uint8_t, 20>> eth_address_from_pubkey(ByteSpan pubkey);

}  // namespace aios::crypto
