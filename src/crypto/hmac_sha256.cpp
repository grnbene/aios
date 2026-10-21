#include "aios/crypto/hmac_sha256.hpp"
#include <cstring>
#include "aios/crypto/sha256.hpp"

namespace aios::crypto {

namespace {

void hmac_key_block(ByteSpan key, uint8_t block[64]) {
  std::memset(block, 0, 64);
  if (key.size() > 64) {
    auto h = sha256(key);
    std::memcpy(block, h.data(), 32);
  } else if (key.size() > 0) {
    std::memcpy(block, key.data(), key.size());
  }
}

}  // namespace

std::array<uint8_t, 32> hmac_sha256(ByteSpan key, ByteSpan message) {
  uint8_t block[64];
  hmac_key_block(key, block);

  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; ++i) {
    ipad[i] = (uint8_t)(block[i] ^ 0x36);
    opad[i] = (uint8_t)(block[i] ^ 0x5c);
  }

  SHA256Context inner;
  sha256_init(inner);
  sha256_update(inner, ipad, 64);
  if (message.size() > 0) sha256_update(inner, message.data(), message.size());
  uint8_t inner_hash[32];
  sha256_final(inner, inner_hash);

  SHA256Context outer;
  sha256_init(outer);
  sha256_update(outer, opad, 64);
  sha256_update(outer, inner_hash, 32);
  std::array<uint8_t, 32> out{};
  sha256_final(outer, out.data());
  return out;
}

Rfc6979::Rfc6979(ByteSpan key_octets, ByteSpan message_octets) {
  v_.fill(0x01);
  k_.fill(0x00);

  std::vector<uint8_t> seed;
  seed.reserve(32 + 1 + key_octets.size() + message_octets.size());
  auto build = [&](uint8_t tag) {
    seed.clear();
    seed.insert(seed.end(), v_.begin(), v_.end());
    seed.push_back(tag);
    seed.insert(seed.end(), key_octets.data(), key_octets.data() + key_octets.size());
    seed.insert(seed.end(), message_octets.data(), message_octets.data() + message_octets.size());
  };

  build(0x00);
  k_ = hmac_sha256(ByteSpan(k_.data(), 32), ByteSpan(seed.data(), seed.size()));
  v_ = hmac_sha256(ByteSpan(k_.data(), 32), ByteSpan(v_.data(), 32));
  build(0x01);
  k_ = hmac_sha256(ByteSpan(k_.data(), 32), ByteSpan(seed.data(), seed.size()));
  v_ = hmac_sha256(ByteSpan(k_.data(), 32), ByteSpan(v_.data(), 32));
}

void Rfc6979::generate(uint8_t out[32]) {
  v_ = hmac_sha256(ByteSpan(k_.data(), 32), ByteSpan(v_.data(), 32));
  std::memcpy(out, v_.data(), 32);
}

void Rfc6979::retry() {
  uint8_t seed[33];
  std::memcpy(seed, v_.data(), 32);
  seed[32] = 0x00;
  k_ = hmac_sha256(ByteSpan(k_.data(), 32), ByteSpan(seed, 33));
  v_ = hmac_sha256(ByteSpan(k_.data(), 32), ByteSpan(v_.data(), 32));
}

void pbkdf2_hmac_sha256(ByteSpan password, ByteSpan salt, uint32_t iterations, uint8_t* out, size_t out_len) {
  uint8_t block[64];
  hmac_key_block(password, block);
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; ++i) {
    ipad[i] = (uint8_t)(block[i] ^ 0x36);
    opad[i] = (uint8_t)(block[i] ^ 0x5c);
  }
  // The keyed pad states are the same for every HMAC call, so absorb them once.
  SHA256Context inner_base, outer_base;
  sha256_init(inner_base);
  sha256_update(inner_base, ipad, 64);
  sha256_init(outer_base);
  sha256_update(outer_base, opad, 64);
  auto prf = [&](const uint8_t* a, size_t alen, const uint8_t* b, size_t blen, uint8_t mac[32]) {
    SHA256Context in = inner_base;
    sha256_update(in, a, alen);
    if (blen) sha256_update(in, b, blen);
    uint8_t ih[32];
    sha256_final(in, ih);
    SHA256Context ou = outer_base;
    sha256_update(ou, ih, 32);
    sha256_final(ou, mac);
  };
  if (iterations == 0) iterations = 1;
  uint32_t index = 1;
  for (size_t done = 0; done < out_len; ++index) {
    uint8_t be[4] = {(uint8_t)(index >> 24), (uint8_t)(index >> 16), (uint8_t)(index >> 8), (uint8_t)index};
    uint8_t u[32], t[32];
    prf(salt.data(), salt.size(), be, 4, u);
    std::memcpy(t, u, 32);
    for (uint32_t r = 1; r < iterations; ++r) {
      prf(u, 32, nullptr, 0, u);
      for (int k = 0; k < 32; ++k) t[k] ^= u[k];
    }
    size_t take = out_len - done < 32 ? out_len - done : 32;
    std::memcpy(out + done, t, take);
    done += take;
  }
}

}  // namespace aios::crypto
