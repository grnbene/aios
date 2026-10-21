#pragma once
#include <array>
#include <cstdint>

#include "aios/core/bytes.hpp"

namespace aios::crypto {

// AES-128-GCM (NIST SP 800-38D) with a 96-bit nonce and a 128-bit tag.
// Portable software AES: table S-box lookups are not cache-timing hardened,
// so callers that can negotiate ChaCha20-Poly1305 should prefer it.
using Aes128Key = std::array<uint8_t, 16>;
using Aes256Key = std::array<uint8_t, 32>;
using GcmNonce = std::array<uint8_t, 12>;

// Returns ciphertext || tag.
ByteBuffer aes128_gcm_seal(const Aes128Key& key, const GcmNonce& nonce, ByteSpan aad, ByteSpan plaintext);
// `sealed` is ciphertext || tag; false (and `out` untouched) when the tag does not verify.
bool aes128_gcm_open(const Aes128Key& key, const GcmNonce& nonce, ByteSpan aad, ByteSpan sealed, ByteBuffer& out);
ByteBuffer aes256_gcm_seal(const Aes256Key& key, const GcmNonce& nonce, ByteSpan aad, ByteSpan plaintext);
bool aes256_gcm_open(const Aes256Key& key, const GcmNonce& nonce, ByteSpan aad, ByteSpan sealed, ByteBuffer& out);

// AES-CTR mode (128, 192, 256 bits): 128-bit big-endian counter increment.
void aes_ctr_crypt(ByteSpan key, ByteSpan iv, ByteSpan in, MutableByteSpan out);

// AES-CBC (128, 192, 256 bits) without padding: `in` is whole blocks. False for a bad key, IV or length.
// For reading the keys other tools encrypt with it (PEM, PKCS#8, OpenSSH's aes*-cbc), not for new protocols.
bool aes_cbc_decrypt(ByteSpan key, ByteSpan iv, ByteSpan in, MutableByteSpan out);
bool aes_cbc_encrypt(ByteSpan key, ByteSpan iv, ByteSpan in, MutableByteSpan out);

}  // namespace aios::crypto
