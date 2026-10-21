#include "aios/crypto/keccak256.hpp"
#include <cstring>

namespace aios::crypto {

namespace {

const uint64_t RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808AULL,
    0x8000000080008000ULL, 0x000000000000808BULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008AULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000AULL,
    0x000000008000808BULL, 0x800000000000008BULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800AULL, 0x800000008000000AULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};

const int ROTC[24] = {1,  3,  6,  10, 15, 21, 28, 36, 45, 55, 2,  14,
                      27, 41, 56, 8,  25, 43, 62, 18, 39, 61, 20, 44};

const int PILN[24] = {10, 7,  11, 17, 18, 3, 5,  16, 8,  21, 24, 4,
                      15, 23, 19, 13, 12, 2, 20, 14, 22, 9,  6,  1};

uint64_t rotl64(uint64_t x, int n) { return n == 0 ? x : (x << n) | (x >> (64 - n)); }

void keccak_f(uint64_t a[25]) {
  for (int round = 0; round < 24; ++round) {
    uint64_t bc[5];
    for (int i = 0; i < 5; ++i) bc[i] = a[i] ^ a[i + 5] ^ a[i + 10] ^ a[i + 15] ^ a[i + 20];
    for (int i = 0; i < 5; ++i) {
      uint64_t d = bc[(i + 4) % 5] ^ rotl64(bc[(i + 1) % 5], 1);
      for (int j = 0; j < 25; j += 5) a[j + i] ^= d;
    }

    uint64_t t = a[1];
    for (int i = 0; i < 24; ++i) {
      int j = PILN[i];
      uint64_t held = a[j];
      a[j] = rotl64(t, ROTC[i]);
      t = held;
    }

    for (int j = 0; j < 25; j += 5) {
      for (int i = 0; i < 5; ++i) bc[i] = a[j + i];
      for (int i = 0; i < 5; ++i) a[j + i] ^= ~bc[(i + 1) % 5] & bc[(i + 2) % 5];
    }

    a[0] ^= RC[round];
  }
}

}  // namespace

std::array<uint8_t, 32> keccak256(ByteSpan data) {
  const size_t rate = 136;
  uint64_t st[25];
  std::memset(st, 0, sizeof(st));

  uint8_t block[136];
  size_t offset = 0;
  size_t len = data.size();
  const uint8_t* p = data.data();

  while (len - offset >= rate) {
    for (size_t i = 0; i < rate; ++i) ((uint8_t*)st)[i] ^= p[offset + i];
    keccak_f(st);
    offset += rate;
  }

  size_t rem = len - offset;
  std::memset(block, 0, rate);
  if (rem > 0) std::memcpy(block, p + offset, rem);
  block[rem] ^= 0x01;
  block[rate - 1] ^= 0x80;
  for (size_t i = 0; i < rate; ++i) ((uint8_t*)st)[i] ^= block[i];
  keccak_f(st);

  std::array<uint8_t, 32> out{};
  std::memcpy(out.data(), st, 32);
  return out;
}

}  // namespace aios::crypto
