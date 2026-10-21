// RFC 8439 test vectors for the node link's primitives, then an encrypt/decrypt round trip and tamper detection.
#include "aios/node/secure.hpp"
#include <cstdio>
#include <cstring>
using namespace aios; using namespace aios::crypto;
static int fails = 0;
static void check(bool ok, const char* what) { printf("  %s  %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
static bool hex_eq(const uint8_t* b, const char* hex, size_t n) { for (size_t i = 0; i < n; ++i) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); if (b[i] != v) return false; } return true; }
int main() {
  // ChaCha20 block function, RFC 8439 §2.3.2
  Key k; for (int i = 0; i < 32; ++i) k[i] = (uint8_t)i;
  Nonce n = {0, 0, 0, 9, 0, 0, 0, 0x4a, 0, 0, 0, 0};
  uint8_t z[64] = {0}; chacha20_xor(k, n, 1, z, 64);
  check(hex_eq(z, "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4ed2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e", 64), "ChaCha20 block test vector");
  // Poly1305, RFC 8439 §2.5.2
  std::array<uint8_t, 32> pk; const char* pkh = "85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b"; for (int i = 0; i < 32; ++i) { unsigned v; sscanf(pkh + 2 * i, "%2x", &v); pk[i] = (uint8_t)v; }
  std::string msg = "Cryptographic Forum Research Group";
  auto tag = poly1305(pk, span_of(msg));
  check(hex_eq(tag.data(), "a8061dc1305136c6c22b8baf0c0127a9", 16), "Poly1305 test vector");
  // AEAD, RFC 8439 §2.8.2
  Key ak; for (int i = 0; i < 32; ++i) ak[i] = (uint8_t)(0x80 + i);
  Nonce an = {0x07, 0, 0, 0, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47};
  uint8_t aad[12] = {0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7};
  std::string pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
  auto ct = aead_encrypt(ak, an, ByteSpan(aad, 12), span_of(pt));
  check(ct.size() == pt.size() + 16 && hex_eq(ct.data(), "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6", 32), "AEAD ciphertext test vector (first 32 bytes)");
  check(hex_eq(ct.data() + pt.size(), "1ae10b594f09e26a7e902ecbd0600691", 16), "AEAD tag test vector");
  auto back = aead_decrypt(ak, an, ByteSpan(aad, 12), ct);
  check(back.ok() && string_of(*back) == pt, "AEAD decrypt round trip");
  ct[5] ^= 1; check(!aead_decrypt(ak, an, ByteSpan(aad, 12), ct).ok(), "tampered ciphertext rejected");
  Key d1 = derive_key("lab-shared-secret", span_of(std::string("salt")));
  Key d2 = derive_key("lab-shared-secreT", span_of(std::string("salt")));
  check(d1 != d2, "derived keys differ for different tokens");
  printf("%s\n", fails ? "SECURE: FAIL" : "SECURE: PASS");
  return fails ? 1 : 0;
}
