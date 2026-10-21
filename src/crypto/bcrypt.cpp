#include "aios/crypto/bcrypt.hpp"

#include <cstdlib>
#include <cstring>
#include <vector>

#include "aios/crypto/sha512.hpp"

namespace aios::crypto {
namespace {

#define BLF_N 16
#define BCRYPT_WORDS 8
#define BCRYPT_HASHSIZE (BCRYPT_WORDS * 4)

struct blf_ctx {
  uint32_t S[4][256];
  uint32_t P[BLF_N + 2];
};

#define F(s, x) ((((s)[(((x) >> 24) & 0xFF)] + (s)[0x100 + (((x) >> 16) & 0xFF)]) ^ (s)[0x200 + (((x) >> 8) & 0xFF)]) + (s)[0x300 + ((x) & 0xFF)])
#define BLFRND(s, p, i, j, n) (i ^= F(s, j) ^ (p)[n])

void Blowfish_encipher(blf_ctx* c, uint32_t* xl, uint32_t* xr) {
  uint32_t Xl = *xl;
  uint32_t Xr = *xr;
  uint32_t* s = c->S[0];
  uint32_t* p = c->P;

  Xl ^= p[0];
  BLFRND(s, p, Xr, Xl, 1);  BLFRND(s, p, Xl, Xr, 2);
  BLFRND(s, p, Xr, Xl, 3);  BLFRND(s, p, Xl, Xr, 4);
  BLFRND(s, p, Xr, Xl, 5);  BLFRND(s, p, Xl, Xr, 6);
  BLFRND(s, p, Xr, Xl, 7);  BLFRND(s, p, Xl, Xr, 8);
  BLFRND(s, p, Xr, Xl, 9);  BLFRND(s, p, Xl, Xr, 10);
  BLFRND(s, p, Xr, Xl, 11); BLFRND(s, p, Xl, Xr, 12);
  BLFRND(s, p, Xr, Xl, 13); BLFRND(s, p, Xl, Xr, 14);
  BLFRND(s, p, Xr, Xl, 15); BLFRND(s, p, Xl, Xr, 16);

  *xl = Xr ^ p[17];
  *xr = Xl;
}

void Blowfish_initstate(blf_ctx* c) {
#include "blowfish_initstate.inc"
  *c = initstate;
}

uint32_t Blowfish_stream2word(const uint8_t* data, uint16_t databytes, uint16_t* current) {
  uint8_t i;
  uint16_t j = *current;
  uint32_t temp = 0;

  for (i = 0; i < 4; i++, j++) {
    if (j >= databytes) j = 0;
    temp = (temp << 8) | data[j];
  }

  *current = j;
  return temp;
}

void Blowfish_expand0state(blf_ctx* c, const uint8_t* key, uint16_t keybytes) {
  uint16_t i, j = 0, k;
  uint32_t temp;
  uint32_t datal = 0, datar = 0;

  for (i = 0; i < BLF_N + 2; i++) {
    temp = Blowfish_stream2word(key, keybytes, &j);
    c->P[i] ^= temp;
  }

  for (i = 0; i < BLF_N + 2; i += 2) {
    Blowfish_encipher(c, &datal, &datar);
    c->P[i] = datal;
    c->P[i + 1] = datar;
  }

  for (i = 0; i < 4; i++) {
    for (k = 0; k < 256; k += 2) {
      Blowfish_encipher(c, &datal, &datar);
      c->S[i][k] = datal;
      c->S[i][k + 1] = datar;
    }
  }
}

void Blowfish_expandstate(blf_ctx* c, const uint8_t* data, uint16_t databytes,
                          const uint8_t* key, uint16_t keybytes) {
  uint16_t i, j = 0, k;
  uint32_t temp;
  uint32_t datal = 0, datar = 0;

  for (i = 0; i < BLF_N + 2; i++) {
    temp = Blowfish_stream2word(key, keybytes, &j);
    c->P[i] ^= temp;
  }

  j = 0;
  datal = 0;
  datar = 0;
  for (i = 0; i < BLF_N + 2; i += 2) {
    datal ^= Blowfish_stream2word(data, databytes, &j);
    datar ^= Blowfish_stream2word(data, databytes, &j);
    Blowfish_encipher(c, &datal, &datar);
    c->P[i] = datal;
    c->P[i + 1] = datar;
  }

  for (i = 0; i < 4; i++) {
    for (k = 0; k < 256; k += 2) {
      datal ^= Blowfish_stream2word(data, databytes, &j);
      datar ^= Blowfish_stream2word(data, databytes, &j);
      Blowfish_encipher(c, &datal, &datar);
      c->S[i][k] = datal;
      c->S[i][k + 1] = datar;
    }
  }
}

void blf_enc(blf_ctx* c, uint32_t* data, uint16_t blocks) {
  for (uint16_t i = 0; i < blocks; i++) {
    Blowfish_encipher(c, data, data + 1);
    data += 2;
  }
}

void bcrypt_hash(uint8_t* sha2pass, uint8_t* sha2salt, uint8_t* out) {
  blf_ctx state;
  uint8_t ciphertext[BCRYPT_HASHSIZE];
  std::memcpy(ciphertext, "OxychromaticBlowfishSwatDynamite", BCRYPT_HASHSIZE);
  uint32_t cdata[BCRYPT_WORDS];
  int i;
  uint16_t j = 0;
  size_t shalen = 64;

  Blowfish_initstate(&state);
  Blowfish_expandstate(&state, sha2salt, shalen, sha2pass, shalen);
  for (i = 0; i < 64; i++) {
    Blowfish_expand0state(&state, sha2salt, shalen);
    Blowfish_expand0state(&state, sha2pass, shalen);
  }

  for (i = 0; i < BCRYPT_WORDS; i++)
    cdata[i] = Blowfish_stream2word(ciphertext, sizeof(ciphertext), &j);
  for (i = 0; i < 64; i++)
    blf_enc(&state, cdata, BCRYPT_WORDS / 2);

  for (i = 0; i < BCRYPT_WORDS; i++) {
    out[4 * i + 3] = (cdata[i] >> 24) & 0xff;
    out[4 * i + 2] = (cdata[i] >> 16) & 0xff;
    out[4 * i + 1] = (cdata[i] >> 8) & 0xff;
    out[4 * i + 0] = cdata[i] & 0xff;
  }

  std::memset(ciphertext, 0, sizeof(ciphertext));
  std::memset(cdata, 0, sizeof(cdata));
  std::memset(&state, 0, sizeof(state));
}

}  // namespace

int bcrypt_pbkdf(const char* pass, size_t passlen, const uint8_t* salt, size_t saltlen,
                 uint8_t* key, size_t keylen, unsigned int rounds) {
  uint8_t sha2pass[64];
  uint8_t sha2salt[64];
  uint8_t out[BCRYPT_HASHSIZE];
  uint8_t tmpout[BCRYPT_HASHSIZE];
  size_t i, j, amt, stride;
  uint32_t count;
  size_t origkeylen = keylen;

  if (rounds < 1) return -1;
  if (passlen == 0 || saltlen == 0 || keylen == 0 ||
      keylen > sizeof(out) * sizeof(out) || saltlen > (1 << 20))
    return -1;

  std::vector<uint8_t> countsalt(saltlen + 4, 0);
  stride = (keylen + sizeof(out) - 1) / sizeof(out);
  amt = (keylen + stride - 1) / stride;

  std::memcpy(countsalt.data(), salt, saltlen);

  // sha2pass = sha512(pass)
  auto hp = sha512(ByteSpan(reinterpret_cast<const uint8_t*>(pass), passlen));
  std::memcpy(sha2pass, hp.data(), 64);

  for (count = 1; keylen > 0; count++) {
    countsalt[saltlen + 0] = (count >> 24) & 0xff;
    countsalt[saltlen + 1] = (count >> 16) & 0xff;
    countsalt[saltlen + 2] = (count >> 8) & 0xff;
    countsalt[saltlen + 3] = count & 0xff;

    auto hs = sha512(ByteSpan(countsalt.data(), saltlen + 4));
    std::memcpy(sha2salt, hs.data(), 64);

    bcrypt_hash(sha2pass, sha2salt, tmpout);
    std::memcpy(out, tmpout, sizeof(out));

    for (i = 1; i < rounds; i++) {
      auto hs2 = sha512(ByteSpan(tmpout, sizeof(tmpout)));
      std::memcpy(sha2salt, hs2.data(), 64);
      bcrypt_hash(sha2pass, sha2salt, tmpout);
      for (j = 0; j < sizeof(out); j++)
        out[j] ^= tmpout[j];
    }

    size_t curr_amt = (amt < keylen) ? amt : keylen;
    for (i = 0; i < curr_amt; i++) {
      size_t dest = i * stride + (count - 1);
      if (dest >= origkeylen) break;
      key[dest] = out[i];
    }
    keylen -= i;
  }

  std::memset(out, 0, sizeof(out));
  std::memset(tmpout, 0, sizeof(tmpout));
  return 0;
}

}  // namespace aios::crypto
