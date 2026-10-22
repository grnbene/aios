#include "aios/crypto/rsa.hpp"

#include <cstring>
#include <vector>

#include "aios/crypto/sha256.hpp"
#include "aios/crypto/sha512.hpp"

#if defined(_MSC_VER) && !defined(__clang__)
#include "aios/crypto/portable_uint128.hpp"
#define AIOS_U128 aios::crypto::portable::U128
#else
// __extension__: GCC's -Wpedantic reports every unsigned __int128 otherwise.
__extension__ typedef unsigned __int128 aios_u128_t;
#define AIOS_U128 aios_u128_t
#endif

namespace aios::crypto {
namespace {

// ---------------------------------------------------------------------------
// A big unsigned integer, as wide as the key it holds. Reduction is long
// division, the same choice made for the NIST curves and for the same reason:
// a verification a few milliseconds slower is invisible next to a network round
// trip, and a reduction that is subtly wrong is a machine that trusts the wrong
// certificate. A public exponent is 17 bits, so a verification is about
// eighteen modular multiplications.
// ---------------------------------------------------------------------------
using Limbs = std::vector<uint64_t>;      // little-endian limbs

Limbs from_bytes(ByteSpan b) {
  Limbs n((b.size() + 7) / 8, 0);
  for (size_t i = 0; i < b.size(); ++i) {
    size_t from_end = b.size() - 1 - i;
    n[from_end / 8] |= (uint64_t)b[i] << (8 * (from_end % 8));
  }
  while (n.size() > 1 && n.back() == 0) n.pop_back();
  if (n.empty()) n.push_back(0);
  return n;
}

void trim(Limbs& n) {
  while (n.size() > 1 && n.back() == 0) n.pop_back();
}

int cmp(const Limbs& a, const Limbs& b) {
  size_t n = a.size() > b.size() ? a.size() : b.size();
  for (size_t i = n; i-- > 0;) {
    uint64_t x = i < a.size() ? a[i] : 0;
    uint64_t y = i < b.size() ? b[i] : 0;
    if (x != y) return x > y ? 1 : -1;
  }
  return 0;
}

void sub_into(Limbs& a, const Limbs& b) {
  AIOS_U128 borrow = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    AIOS_U128 t = (AIOS_U128)a[i] - (i < b.size() ? b[i] : 0) - borrow;
    a[i] = (uint64_t)t;
    borrow = (t >> 64) ? 1 : 0;
  }
  trim(a);
}

void shl1(Limbs& a) {
  uint64_t carry = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    uint64_t next = a[i] >> 63;
    a[i] = (a[i] << 1) | carry;
    carry = next;
  }
  if (carry) a.push_back(carry);
}

int bit_at(const Limbs& a, size_t i) {
  size_t limb = i / 64;
  return limb < a.size() ? (int)((a[limb] >> (i % 64)) & 1) : 0;
}

size_t top_bit(const Limbs& a) {
  for (size_t i = a.size() * 64; i-- > 0;)
    if (bit_at(a, i)) return i;
  return 0;
}

bool is_zero(const Limbs& a) {
  for (uint64_t v : a)
    if (v) return false;
  return true;
}

Limbs mul(const Limbs& a, const Limbs& b) {
  Limbs out(a.size() + b.size(), 0);
  for (size_t i = 0; i < a.size(); ++i) {
    if (!a[i]) continue;
    AIOS_U128 carry = 0;
    for (size_t j = 0; j < b.size(); ++j) {
      AIOS_U128 t = (AIOS_U128)a[i] * b[j] + out[i + j] + carry;
      out[i + j] = (uint64_t)t;
      carry = t >> 64;
    }
    for (size_t k = i + b.size(); carry && k < out.size(); ++k) {
      AIOS_U128 t = (AIOS_U128)out[k] + carry;
      out[k] = (uint64_t)t;
      carry = t >> 64;
    }
  }
  trim(out);
  return out;
}

Limbs mod(const Limbs& a, const Limbs& m) {
  if (cmp(a, m) < 0) return a;
  Limbs rem(m.size() + 1, 0);
  size_t top = top_bit(a);
  for (size_t i = top + 1; i-- > 0;) {
    shl1(rem);
    rem[0] |= (uint64_t)bit_at(a, i);
    if (cmp(rem, m) >= 0) sub_into(rem, m);
  }
  trim(rem);
  return rem;
}

Limbs mod_pow(const Limbs& base, const Limbs& exp, const Limbs& m) {
  Limbs result{1};
  Limbs b = mod(base, m);
  size_t top = top_bit(exp);
  for (size_t i = top + 1; i-- > 0;) {
    result = mod(mul(result, result), m);
    if (bit_at(exp, i)) result = mod(mul(result, b), m);
  }
  return result;
}

ByteBuffer to_bytes(const Limbs& n, size_t width) {
  ByteBuffer out(width, 0);
  for (size_t i = 0; i < width; ++i) {
    size_t from_end = width - 1 - i;
    out[i] = (from_end / 8 < n.size()) ? (uint8_t)(n[from_end / 8] >> (8 * (from_end % 8))) : 0;
  }
  return out;
}

// A DER integer, leading zeroes stripped.
ByteBuffer der_int(ByteSpan v) {
  size_t i = 0;
  while (i + 1 < v.size() && v[i] == 0) ++i;
  return ByteBuffer(v.begin() + (long)i, v.end());
}

size_t digest_size(RsaDigest d) { return d == RsaDigest::Sha256 ? 32 : (d == RsaDigest::Sha384 ? 48 : 64); }

ByteBuffer hash_of(RsaDigest d, ByteSpan m) {
  if (d == RsaDigest::Sha256) { auto h = sha256(m); return ByteBuffer(h.begin(), h.end()); }
  if (d == RsaDigest::Sha384) { auto h = sha384(m); return ByteBuffer(h.begin(), h.end()); }
  auto h = sha512(m);
  return ByteBuffer(h.begin(), h.end());
}

// The DigestInfo prefix PKCS#1 v1.5 wraps each hash in.
ByteSpan digest_info_prefix(RsaDigest d) {
  static const uint8_t p256[] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65,
                                 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
  static const uint8_t p384[] = {0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65,
                                 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30};
  static const uint8_t p512[] = {0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65,
                                 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40};
  if (d == RsaDigest::Sha256) return ByteSpan(p256, sizeof(p256));
  if (d == RsaDigest::Sha384) return ByteSpan(p384, sizeof(p384));
  return ByteSpan(p512, sizeof(p512));
}

// MGF1, the mask generator PSS is defined over.
ByteBuffer mgf1(RsaDigest d, ByteSpan seed, size_t len) {
  ByteBuffer out;
  uint32_t counter = 0;
  while (out.size() < len) {
    ByteBuffer in(seed.begin(), seed.end());
    in.push_back((uint8_t)(counter >> 24));
    in.push_back((uint8_t)(counter >> 16));
    in.push_back((uint8_t)(counter >> 8));
    in.push_back((uint8_t)counter);
    ++counter;
    auto block = hash_of(d, ByteSpan(in.data(), in.size()));
    out.insert(out.end(), block.begin(), block.end());
  }
  out.resize(len);
  return out;
}

}  // namespace

size_t RsaPublicKey::bits() const { return modulus.empty() ? 0 : modulus.size() * 8; }
bool RsaPublicKey::valid() const { return modulus.size() >= 64 && !exponent.empty(); }

size_t RsaPrivateKey::bits() const { return modulus.empty() ? 0 : modulus.size() * 8; }
bool RsaPrivateKey::valid() const { return modulus.size() >= 64 && !private_exponent.empty(); }

namespace {

bool asn1_tlv(ByteSpan der, size_t& p, uint8_t want, ByteSpan& body) {
  if (p + 2 > der.size() || der[p] != want) return false;
  ++p;
  size_t len = der[p++];
  if (len & 0x80) {
    size_t n = len & 0x7f;
    if (n == 0 || n > 4 || p + n > der.size()) return false;
    len = 0;
    for (size_t i = 0; i < n; ++i) len = (len << 8) | der[p++];
  }
  if (p + len > der.size()) return false;
  body = der.subspan(p, len);
  p += len;
  return true;
}

}  // namespace

Result<RsaPublicKey> rsa_parse_public_key(ByteSpan der) {
  // SEQUENCE { INTEGER modulus, INTEGER exponent }
  size_t p = 0;
  ByteSpan seq;
  if (!asn1_tlv(der, p, 0x30, seq)) return Result<RsaPublicKey>(Status::Corrupt, "not an RSAPublicKey sequence");
  p = 0;
  ByteSpan n, e;
  if (!asn1_tlv(seq, p, 0x02, n) || !asn1_tlv(seq, p, 0x02, e))
    return Result<RsaPublicKey>(Status::Corrupt, "an RSA key without a modulus and an exponent");
  RsaPublicKey key;
  key.modulus = der_int(n);
  key.exponent = der_int(e);
  if (!key.valid()) return Result<RsaPublicKey>(Status::Invalid, "the RSA key is too small to be one");
  return key;
}

Result<RsaPrivateKey> rsa_parse_private_key(ByteSpan der) {
  size_t p = 0;
  ByteSpan seq;
  if (!asn1_tlv(der, p, 0x30, seq)) return Result<RsaPrivateKey>(Status::Corrupt, "not a private key sequence");

  // Check if this is PKCS#8 PrivateKeyInfo: SEQUENCE { version, AlgorithmIdentifier, OCTET STRING }
  size_t sp = 0;
  ByteSpan v_elem;
  if (!asn1_tlv(seq, sp, 0x02, v_elem)) return Result<RsaPrivateKey>(Status::Corrupt, "missing version in sequence");
  
  ByteSpan next_elem;
  size_t check_p = sp;
  if (asn1_tlv(seq, check_p, 0x30, next_elem)) {
    // PKCS#8: next is AlgorithmIdentifier sequence, followed by OCTET STRING containing RSAPrivateKey
    ByteSpan octets;
    if (!asn1_tlv(seq, check_p, 0x04, octets))
      return Result<RsaPrivateKey>(Status::Corrupt, "PKCS#8 missing private key octet string");
    return rsa_parse_private_key(octets);
  }

  // PKCS#1 RSAPrivateKey: SEQUENCE { version, n, e, d, p, q, dP, dQ, qInv }
  ByteSpan n, e, d, p_prime, q_prime, dp, dq, qinv;
  if (!asn1_tlv(seq, sp, 0x02, n) ||
      !asn1_tlv(seq, sp, 0x02, e) ||
      !asn1_tlv(seq, sp, 0x02, d)) {
    return Result<RsaPrivateKey>(Status::Corrupt, "incomplete RSAPrivateKey elements");
  }

  RsaPrivateKey key;
  key.modulus = der_int(n);
  key.public_exponent = der_int(e);
  key.private_exponent = der_int(d);

  if (asn1_tlv(seq, sp, 0x02, p_prime)) key.prime1 = der_int(p_prime);
  if (asn1_tlv(seq, sp, 0x02, q_prime)) key.prime2 = der_int(q_prime);
  if (asn1_tlv(seq, sp, 0x02, dp)) key.exponent1 = der_int(dp);
  if (asn1_tlv(seq, sp, 0x02, dq)) key.exponent2 = der_int(dq);
  if (asn1_tlv(seq, sp, 0x02, qinv)) key.coefficient = der_int(qinv);

  if (!key.valid()) return Result<RsaPrivateKey>(Status::Invalid, "invalid RSA private key");
  return key;
}

Result<ByteBuffer> rsa_public_op(const RsaPublicKey& key, ByteSpan signature) {
  if (!key.valid()) return Result<ByteBuffer>(Status::Invalid, "no usable RSA key");
  if (signature.size() != key.modulus.size())
    return Result<ByteBuffer>(Status::Invalid, "the signature is not the width of the modulus");
  Limbs n = from_bytes(ByteSpan(key.modulus.data(), key.modulus.size()));
  Limbs e = from_bytes(ByteSpan(key.exponent.data(), key.exponent.size()));
  Limbs s = from_bytes(signature);
  if (is_zero(n) || cmp(s, n) >= 0) return Result<ByteBuffer>(Status::Invalid, "signature out of range");
  Limbs m = mod_pow(s, e, n);
  return to_bytes(m, key.modulus.size());
}

Result<ByteBuffer> rsa_private_op(const RsaPrivateKey& key, ByteSpan em) {
  if (!key.valid()) return Result<ByteBuffer>(Status::Invalid, "no usable RSA private key");
  if (em.size() != key.modulus.size())
    return Result<ByteBuffer>(Status::Invalid, "the padded input is not the width of the modulus");
  Limbs n = from_bytes(ByteSpan(key.modulus.data(), key.modulus.size()));
  Limbs d = from_bytes(ByteSpan(key.private_exponent.data(), key.private_exponent.size()));
  Limbs m = from_bytes(em);
  if (is_zero(n) || cmp(m, n) >= 0) return Result<ByteBuffer>(Status::Invalid, "message representative out of range");
  Limbs s = mod_pow(m, d, n);
  return to_bytes(s, key.modulus.size());
}

bool rsa_verify_pkcs1(const RsaPublicKey& key, RsaDigest digest, ByteSpan message, ByteSpan signature) {
  auto em = rsa_public_op(key, signature);
  if (!em.ok()) return false;
  auto hash = hash_of(digest, message);
  ByteSpan prefix = digest_info_prefix(digest);

  // EM = 0x00 || 0x01 || PS (0xff…) || 0x00 || DigestInfo
  ByteBuffer want;
  want.push_back(0x00);
  want.push_back(0x01);
  size_t body = prefix.size() + hash.size();
  if (em->size() < body + 11) return false;
  size_t pad = em->size() - body - 3;
  for (size_t i = 0; i < pad; ++i) want.push_back(0xff);
  want.push_back(0x00);
  want.insert(want.end(), prefix.begin(), prefix.end());
  want.insert(want.end(), hash.begin(), hash.end());
  if (want.size() != em->size()) return false;
  unsigned diff = 0;
  for (size_t i = 0; i < want.size(); ++i) diff |= (unsigned)(want[i] ^ (*em)[i]);
  return diff == 0;
}

Result<ByteBuffer> rsa_sign_pkcs1(const RsaPrivateKey& key, RsaDigest digest, ByteSpan message) {
  if (!key.valid()) return Result<ByteBuffer>(Status::Invalid, "invalid RSA private key");
  size_t mod_len = key.modulus.size();
  auto hash = hash_of(digest, message);
  ByteSpan prefix = digest_info_prefix(digest);
  size_t body = prefix.size() + hash.size();
  if (mod_len < body + 11) return Result<ByteBuffer>(Status::Invalid, "RSA modulus too short for PKCS#1 v1.5 digest");

  ByteBuffer em;
  em.reserve(mod_len);
  em.push_back(0x00);
  em.push_back(0x01);
  size_t pad = mod_len - body - 3;
  for (size_t i = 0; i < pad; ++i) em.push_back(0xff);
  em.push_back(0x00);
  em.insert(em.end(), prefix.begin(), prefix.end());
  em.insert(em.end(), hash.begin(), hash.end());

  return rsa_private_op(key, ByteSpan(em.data(), em.size()));
}

bool rsa_verify_pss(const RsaPublicKey& key, RsaDigest digest, ByteSpan message, ByteSpan signature) {
  auto em_res = rsa_public_op(key, signature);
  if (!em_res.ok()) return false;
  ByteBuffer em = *em_res;
  const size_t hlen = digest_size(digest);
  const size_t slen = hlen;                        // every TLS implementation salts with hLen
  const size_t embits = key.bits() - 1;            // the modulus is used to its full width
  const size_t emlen = (embits + 7) / 8;
  if (em.size() < emlen) return false;
  // A leading zero byte appears when the modulus is a whole number of bytes.
  if (em.size() > emlen) {
    for (size_t i = 0; i < em.size() - emlen; ++i)
      if (em[i] != 0) return false;
    em.erase(em.begin(), em.begin() + (long)(em.size() - emlen));
  }
  if (emlen < hlen + slen + 2) return false;
  if (em.back() != 0xbc) return false;

  size_t dblen = emlen - hlen - 1;
  ByteSpan masked_db(em.data(), dblen);
  ByteSpan h(em.data() + dblen, hlen);
  // The unused bits of the leftmost byte must be zero.
  size_t spare = 8 * emlen - embits;
  if (spare && (masked_db[0] >> (8 - spare)) != 0) return false;

  auto mask = mgf1(digest, h, dblen);
  ByteBuffer db(masked_db.begin(), masked_db.end());
  for (size_t i = 0; i < dblen; ++i) db[i] ^= mask[i];
  if (spare) db[0] = (uint8_t)(db[0] & (0xff >> spare));

  size_t zeros = dblen - slen - 1;
  for (size_t i = 0; i < zeros; ++i)
    if (db[i] != 0) return false;
  if (db[zeros] != 0x01) return false;
  ByteSpan salt(db.data() + zeros + 1, slen);

  // H' = Hash(0x00 * 8 || mHash || salt)
  auto mhash = hash_of(digest, message);
  ByteBuffer m_prime(8, 0);
  m_prime.insert(m_prime.end(), mhash.begin(), mhash.end());
  m_prime.insert(m_prime.end(), salt.begin(), salt.end());
  auto want = hash_of(digest, ByteSpan(m_prime.data(), m_prime.size()));
  unsigned diff = 0;
  for (size_t i = 0; i < hlen; ++i) diff |= (unsigned)(want[i] ^ h[i]);
  return diff == 0;
}

Result<ByteBuffer> rsa_sign_pss(const RsaPrivateKey& key, RsaDigest digest, ByteSpan message, ByteSpan salt_input) {
  if (!key.valid()) return Result<ByteBuffer>(Status::Invalid, "invalid RSA private key");
  const size_t hlen = digest_size(digest);
  const size_t slen = hlen;
  const size_t embits = key.bits() - 1;
  const size_t emlen = (embits + 7) / 8;
  if (emlen < hlen + slen + 2) return Result<ByteBuffer>(Status::Invalid, "modulus too small for PSS with this digest");

  ByteBuffer salt;
  if (salt_input.size() == slen) {
    salt.assign(salt_input.begin(), salt_input.end());
  } else {
    // Generate deterministic salt from message hash + private exponent
    salt.resize(slen);
    auto seed_h = hash_of(digest, message);
    for (size_t i = 0; i < slen; ++i) salt[i] = (uint8_t)(seed_h[i % seed_h.size()] ^ (i * 0x5a));
  }

  auto mhash = hash_of(digest, message);
  ByteBuffer m_prime(8, 0);
  m_prime.insert(m_prime.end(), mhash.begin(), mhash.end());
  m_prime.insert(m_prime.end(), salt.begin(), salt.end());
  auto h = hash_of(digest, ByteSpan(m_prime.data(), m_prime.size()));

  size_t dblen = emlen - hlen - 1;
  ByteBuffer db(dblen, 0);
  size_t zeros = dblen - slen - 1;
  db[zeros] = 0x01;
  std::memcpy(db.data() + zeros + 1, salt.data(), slen);

  auto mask = mgf1(digest, ByteSpan(h.data(), h.size()), dblen);
  for (size_t i = 0; i < dblen; ++i) db[i] ^= mask[i];
  size_t spare = 8 * emlen - embits;
  if (spare) db[0] = (uint8_t)(db[0] & (0xff >> spare));

  ByteBuffer em(db.begin(), db.end());
  em.insert(em.end(), h.begin(), h.end());
  em.push_back(0xbc);

  if (key.modulus.size() > em.size()) {
    ByteBuffer full(key.modulus.size() - em.size(), 0);
    full.insert(full.end(), em.begin(), em.end());
    em = std::move(full);
  }

  return rsa_private_op(key, ByteSpan(em.data(), em.size()));
}

}  // namespace aios::crypto
