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

namespace {

static const char Base64Code[] =
"./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

static const uint8_t index_64[128] = {
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255,   0,   1,  54,  55,
     56,  57,  58,  59,  60,  61,  62,  63, 255, 255,
    255, 255, 255, 255, 255,   2,   3,   4,   5,   6,
      7,   8,   9,  10,  11,  12,  13,  14,  15,  16,
     17,  18,  19,  20,  21,  22,  23,  24,  25,  26,
     27, 255, 255, 255, 255, 255, 255,  28,  29,  30,
     31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
     41,  42,  43,  44,  45,  46,  47,  48,  49,  50,
     51,  52,  53, 255, 255, 255, 255, 255
};

static void encode_bcrypt_base64(char *buffer, const uint8_t *data, size_t len) {
    const uint8_t *p = data;
    size_t i = 0;
    while (i < len) {
        uint32_t c1 = *p++;
        *buffer++ = Base64Code[(c1 >> 2) & 0x3f];
        uint32_t c1_rem = (c1 & 0x03) << 4;
        if (++i >= len) {
            *buffer++ = Base64Code[c1_rem & 0x3f];
            break;
        }
        uint32_t c2 = *p++;
        *buffer++ = Base64Code[(c1_rem | (c2 >> 4)) & 0x3f];
        uint32_t c2_rem = (c2 & 0x0f) << 2;
        if (++i >= len) {
            *buffer++ = Base64Code[c2_rem & 0x3f];
            break;
        }
        uint32_t c3 = *p++;
        *buffer++ = Base64Code[(c2_rem | (c3 >> 6)) & 0x3f];
        *buffer++ = Base64Code[c3 & 0x3f];
        ++i;
    }
    *buffer = '\0';
}

static bool decode_bcrypt_base64(uint8_t *buffer, size_t len, const char *data) {
    const char *p = data;
    size_t i = 0;
    while (i < len) {
        if (!*p || (unsigned char)*p >= 128) return false;
        uint8_t c1 = index_64[(unsigned char)*p++];
        if (c1 == 255) return false;
        if (!*p || (unsigned char)*p >= 128) return false;
        uint8_t c2 = index_64[(unsigned char)*p++];
        if (c2 == 255) return false;
        buffer[i++] = (c1 << 2) | ((c2 & 0x30) >> 4);
        if (i >= len) break;
        if (!*p || (unsigned char)*p >= 128) return false;
        uint8_t c3 = index_64[(unsigned char)*p++];
        if (c3 == 255) return false;
        buffer[i++] = ((c2 & 0x0f) << 4) | ((c3 & 0x3c) >> 2);
        if (i >= len) break;
        if (!*p || (unsigned char)*p >= 128) return false;
        uint8_t c4 = index_64[(unsigned char)*p++];
        if (c4 == 255) return false;
        buffer[i++] = ((c3 & 0x03) << 6) | c4;
    }
    return true;
}

}  // namespace

bool bcrypt_hash(const char* pass, size_t passlen, const uint8_t salt16[16],
                 unsigned int cost, char out60[61]) {
  if (cost < 4 || cost > 31) return false;
  if (!pass) return false;
  if (passlen > 72) passlen = 72;

  std::vector<uint8_t> key(passlen + 1, 0);
  std::memcpy(key.data(), pass, passlen);
  key[passlen] = 0;
  uint16_t keybytes = static_cast<uint16_t>(passlen + 1);

  blf_ctx state;
  Blowfish_initstate(&state);
  Blowfish_expandstate(&state, salt16, 16, key.data(), keybytes);

  uint64_t rounds = 1ULL << cost;
  for (uint64_t k = 0; k < rounds; ++k) {
    Blowfish_expand0state(&state, key.data(), keybytes);
    Blowfish_expand0state(&state, salt16, 16);
  }

  uint32_t cdata[6];
  uint16_t j = 0;
  const uint8_t magic[24] = {
    'O', 'r', 'p', 'h', 'e', 'a', 'n', 'B',
    'e', 'h', 'o', 'l', 'd', 'e', 'r', 'S',
    'c', 'r', 'y', 'D', 'o', 'u', 'b', 't'
  };
  for (int i = 0; i < 6; ++i) {
    cdata[i] = Blowfish_stream2word(magic, 24, &j);
  }
  for (int i = 0; i < 64; ++i) {
    blf_enc(&state, cdata, 3);
  }

  uint8_t raw_hash[24];
  for (int i = 0; i < 6; ++i) {
    raw_hash[4 * i + 0] = (cdata[i] >> 24) & 0xff;
    raw_hash[4 * i + 1] = (cdata[i] >> 16) & 0xff;
    raw_hash[4 * i + 2] = (cdata[i] >> 8) & 0xff;
    raw_hash[4 * i + 3] = cdata[i] & 0xff;
  }

  std::snprintf(out60, 8, "$2b$%02u$", cost);
  encode_bcrypt_base64(out60 + 7, salt16, 16);
  encode_bcrypt_base64(out60 + 7 + 22, raw_hash, 23);
  out60[60] = '\0';
  return true;
}

bool bcrypt_verify(const char* pass, size_t passlen, const char* hash) {
  if (!pass || !hash) return false;
  if (std::strlen(hash) != 60) return false;
  if (std::strncmp(hash, "$2a$", 4) != 0 && std::strncmp(hash, "$2b$", 4) != 0) return false;
  if (hash[6] != '$') return false;

  unsigned int cost = 0;
  if (hash[4] >= '0' && hash[4] <= '9' && hash[5] >= '0' && hash[5] <= '9') {
    cost = (hash[4] - '0') * 10 + (hash[5] - '0');
  } else {
    return false;
  }
  if (cost < 4 || cost > 31) return false;

  uint8_t salt[16];
  if (!decode_bcrypt_base64(salt, 16, hash + 7)) return false;

  char computed[61];
  if (!bcrypt_hash(pass, passlen, salt, cost, computed)) return false;

  // Constant-time comparison
  uint8_t diff = 0;
  for (size_t i = 0; i < 60; ++i) {
    diff |= static_cast<uint8_t>(computed[i] ^ hash[i]);
  }
  return diff == 0;
}

}  // namespace aios::crypto
