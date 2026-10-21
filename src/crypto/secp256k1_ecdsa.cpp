#include "aios/crypto/secp256k1.hpp"
#include <cstring>
#include "aios/crypto/hmac_sha256.hpp"
#include "aios/crypto/keccak256.hpp"
#include "secp256k1_math.hpp"

namespace aios::crypto {

using namespace k1;

namespace {

const uint8_t N_BYTES[32] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                             0xFF, 0xFF, 0xFF, 0xFF, 0xFE, 0xBA, 0xAE, 0xDC, 0xE6, 0xAF, 0x48,
                             0xA0, 0x3B, 0xBF, 0xD2, 0x5E, 0x8C, 0xD0, 0x36, 0x41, 0x41};

Result<Ge> parse_pubkey(ByteSpan pubkey) {
  Ge point;
  ge_set_infinity(point);
  if (pubkey.size() == 33 && (pubkey[0] == 0x02 || pubkey[0] == 0x03)) {
    Fe x;
    if (!fe_set_b32(x, pubkey.data() + 1)) {
      return Result<Ge>(Status::Invalid, "public key x coordinate is not a field element");
    }
    if (!ge_set_x_parity(point, x, pubkey[0] == 0x03)) {
      return Result<Ge>(Status::Invalid, "public key x coordinate is not on the curve");
    }
    return point;
  }
  if (pubkey.size() == 65 && pubkey[0] == 0x04) {
    Fe x, y;
    if (!fe_set_b32(x, pubkey.data() + 1) || !fe_set_b32(y, pubkey.data() + 33)) {
      return Result<Ge>(Status::Invalid, "public key coordinates are not field elements");
    }
    point.x = x;
    point.y = y;
    point.inf = false;
    if (!ge_is_valid(point)) return Result<Ge>(Status::Invalid, "public key is not on the curve");
    return point;
  }
  return Result<Ge>(Status::Invalid, "public key must be 33 or 65 bytes");
}

ByteBuffer serialize_ge(const Ge& point, bool compressed) {
  ByteBuffer out;
  uint8_t xb[32], yb[32];
  fe_get_b32(xb, point.x);
  fe_get_b32(yb, point.y);
  if (compressed) {
    out.push_back(fe_is_odd(point.y) ? 0x03 : 0x02);
    out.insert(out.end(), xb, xb + 32);
  } else {
    out.push_back(0x04);
    out.insert(out.end(), xb, xb + 32);
    out.insert(out.end(), yb, yb + 32);
  }
  return out;
}

}  // namespace

bool ec_seckey_valid(const uint8_t sec[32]) {
  Scalar d;
  bool over = sc_set_b32(d, sec);
  return !over && !sc_is_zero(d);
}

Result<ByteBuffer> ec_pubkey_create(const uint8_t sec[32], bool compressed) {
  if (!ec_seckey_valid(sec)) return Result<ByteBuffer>(Status::Invalid, "private key out of range");
  Scalar d;
  sc_set_b32(d, sec);
  Gej pj;
  ecmul_gen(pj, d);
  Ge point;
  gej_to_ge(point, pj);
  if (point.inf) return Result<ByteBuffer>(Status::Invalid, "private key maps to infinity");
  return serialize_ge(point, compressed);
}

Result<ByteBuffer> ec_pubkey_convert(ByteSpan pubkey, bool compressed) {
  auto point = parse_pubkey(pubkey);
  if (!point.ok()) return Result<ByteBuffer>(point.error());
  return serialize_ge(*point, compressed);
}

Result<EcSignature> ecdsa_sign(const uint8_t sec[32], const uint8_t msg32[32]) {
  if (!ec_seckey_valid(sec)) return Result<EcSignature>(Status::Invalid, "private key out of range");

  Scalar d;
  sc_set_b32(d, sec);
  Scalar z;
  sc_set_b32(z, msg32);

  uint8_t z_octets[32];
  sc_get_b32(z_octets, z);

  Rfc6979 drbg(ByteSpan(sec, 32), ByteSpan(z_octets, 32));

  for (int attempt = 0; attempt < 1000; ++attempt) {
    uint8_t candidate[32];
    drbg.generate(candidate);

    Scalar k;
    bool over = sc_set_b32(k, candidate);
    if (over || sc_is_zero(k)) {
      drbg.retry();
      continue;
    }

    Gej rj;
    ecmul_gen(rj, k);
    Ge rp;
    gej_to_ge(rp, rj);
    if (rp.inf) {
      drbg.retry();
      continue;
    }

    Scalar r;
    bool r_overflowed = false;
    fe_to_scalar(r, rp.x, r_overflowed);
    if (sc_is_zero(r)) {
      drbg.retry();
      continue;
    }

    Scalar kinv, rd, sum, s;
    sc_inv(kinv, k);
    sc_mul(rd, r, d);
    sc_add(sum, z, rd);
    sc_mul(s, kinv, sum);
    if (sc_is_zero(s)) {
      drbg.retry();
      continue;
    }

    int recid = (fe_is_odd(rp.y) ? 1 : 0) | (r_overflowed ? 2 : 0);
    if (sc_is_high(s)) {
      sc_neg(s, s);
      recid ^= 1;
    }

    EcSignature sig;
    sc_get_b32(sig.r.data(), r);
    sc_get_b32(sig.s.data(), s);
    sig.recovery_id = recid;
    return sig;
  }

  return Result<EcSignature>(Status::Exhausted, "no valid nonce found");
}

bool ecdsa_verify(ByteSpan pubkey, const uint8_t msg32[32], const EcSignature& sig) {
  auto point = parse_pubkey(pubkey);
  if (!point.ok()) return false;

  Scalar r, s, z;
  if (sc_set_b32(r, sig.r.data()) || sc_is_zero(r)) return false;
  if (sc_set_b32(s, sig.s.data()) || sc_is_zero(s)) return false;
  sc_set_b32(z, msg32);

  Scalar sinv, u1, u2;
  sc_inv(sinv, s);
  sc_mul(u1, z, sinv);
  sc_mul(u2, r, sinv);

  Gej rj;
  ecmul_var(rj, *point, u2, u1);
  if (gej_is_infinity(rj)) return false;

  Ge rp;
  gej_to_ge(rp, rj);
  Scalar rx;
  bool ignored = false;
  fe_to_scalar(rx, rp.x, ignored);
  return sc_equal(rx, r);
}

Result<ByteBuffer> ecdsa_recover(const EcSignature& sig, const uint8_t msg32[32], bool compressed) {
  if (sig.recovery_id < 0 || sig.recovery_id > 3) {
    return Result<ByteBuffer>(Status::Invalid, "recovery id out of range");
  }

  Scalar r, s, z;
  if (sc_set_b32(r, sig.r.data()) || sc_is_zero(r)) {
    return Result<ByteBuffer>(Status::Invalid, "signature r out of range");
  }
  if (sc_set_b32(s, sig.s.data()) || sc_is_zero(s)) {
    return Result<ByteBuffer>(Status::Invalid, "signature s out of range");
  }
  sc_set_b32(z, msg32);

  Fe x;
  if (!fe_set_b32(x, sig.r.data())) {
    return Result<ByteBuffer>(Status::Invalid, "signature r is not a field element");
  }
  if (sig.recovery_id & 2) {
    Fe n_fe;
    fe_set_b32(n_fe, N_BYTES);
    fe_add(x, x, n_fe);
  }

  Ge rp;
  if (!ge_set_x_parity(rp, x, (sig.recovery_id & 1) != 0)) {
    return Result<ByteBuffer>(Status::Invalid, "no curve point for this signature");
  }

  Scalar rinv, u1, u2, neg_z;
  sc_inv(rinv, r);
  sc_mul(u1, s, rinv);
  sc_neg(neg_z, z);
  sc_mul(u2, neg_z, rinv);

  Gej qj;
  ecmul_var(qj, rp, u1, u2);
  if (gej_is_infinity(qj)) {
    return Result<ByteBuffer>(Status::Invalid, "recovered key is the point at infinity");
  }

  Ge q;
  gej_to_ge(q, qj);
  return serialize_ge(q, compressed);
}

ByteBuffer ecdsa_der_encode(const EcSignature& sig) {
  auto trim = [](const uint8_t* v) {
    ByteBuffer out;
    size_t i = 0;
    while (i < 31 && v[i] == 0) ++i;
    if (v[i] & 0x80) out.push_back(0x00);
    out.insert(out.end(), v + i, v + 32);
    return out;
  };

  ByteBuffer rb = trim(sig.r.data());
  ByteBuffer sb = trim(sig.s.data());

  ByteBuffer out;
  out.push_back(0x30);
  out.push_back((uint8_t)(4 + rb.size() + sb.size()));
  out.push_back(0x02);
  out.push_back((uint8_t)rb.size());
  out.insert(out.end(), rb.begin(), rb.end());
  out.push_back(0x02);
  out.push_back((uint8_t)sb.size());
  out.insert(out.end(), sb.begin(), sb.end());
  return out;
}

Result<EcSignature> ecdsa_der_decode(ByteSpan der) {
  size_t pos = 0;
  auto need = [&](size_t n) { return pos + n <= der.size(); };

  if (!need(2) || der[pos] != 0x30) return Result<EcSignature>(Status::Invalid, "der: no sequence");
  size_t seq_len = der[pos + 1];
  pos += 2;
  if (pos + seq_len != der.size()) return Result<EcSignature>(Status::Invalid, "der: length mismatch");

  auto read_int = [&](std::array<uint8_t, 32>& out) -> bool {
    if (!need(2) || der[pos] != 0x02) return false;
    size_t len = der[pos + 1];
    pos += 2;
    if (len == 0 || !need(len)) return false;
    if (der[pos] & 0x80) return false;
    if (der[pos] == 0 && len > 1 && !(der[pos + 1] & 0x80)) return false;
    size_t start = (der[pos] == 0 && len > 1) ? 1 : 0;
    size_t body = len - start;
    if (body > 32) return false;
    out.fill(0);
    std::memcpy(out.data() + (32 - body), der.data() + pos + start, body);
    pos += len;
    return true;
  };

  EcSignature sig;
  if (!read_int(sig.r)) return Result<EcSignature>(Status::Invalid, "der: bad r");
  if (!read_int(sig.s)) return Result<EcSignature>(Status::Invalid, "der: bad s");
  return sig;
}

Result<std::array<uint8_t, 20>> eth_address_from_pubkey(ByteSpan pubkey) {
  auto full = ec_pubkey_convert(pubkey, false);
  if (!full.ok()) return Result<std::array<uint8_t, 20>>(full.error());
  auto digest = keccak256(ByteSpan(full->data() + 1, 64));
  std::array<uint8_t, 20> out{};
  std::memcpy(out.data(), digest.data() + 12, 20);
  return out;
}

}  // namespace aios::crypto
