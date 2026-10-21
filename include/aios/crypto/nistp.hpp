#pragma once
// ECDSA over the NIST prime curves, because that is what a certificate chain is
// signed with. A machine that cannot check these cannot know who it is talking
// to over TLS — it can only hope.
//
// P-256, P-384 and P-521 share one implementation: the same short-Weierstrass
// group (y^2 = x^3 - 3x + b) over a different prime, so the curve is a parameter
// rather than a copy of the file. It checks signatures on certificates and SSH
// host keys, and signs with TLS and SSH ECDSA keys.
#include <cstdint>
#include <string>

#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"

namespace aios::crypto {

enum class NistCurve { P256, P384, P521 };

struct EcPrivateKey {
  NistCurve curve = NistCurve::P256;
  ByteBuffer d;  // scalar in big-endian
  bool valid() const;
};

// Parse SEC1 ECPrivateKey or PKCS#8 PrivateKeyInfo (EC)
Result<EcPrivateKey> ec_parse_private_key(ByteSpan der);

// Compute uncompressed public key point 0x04 || X || Y from scalar d
Result<ByteBuffer> ecdsa_public_key(NistCurve curve, ByteSpan private_key_scalar);

// `pub` is the uncompressed point from a SubjectPublicKeyInfo: 0x04 || X || Y.
// `sig_r`/`sig_s` are the two integers of an ECDSA signature, big-endian, and
// `digest` is the hash of what was signed, already computed with the digest the
// signature algorithm names.
bool ecdsa_verify(NistCurve curve, ByteSpan pub, ByteSpan digest, ByteSpan sig_r, ByteSpan sig_s);

// Sign `digest` with private key scalar `d`. Returns DER-encoded SEQUENCE { INTEGER r, INTEGER s }.
// With no `k_nonce`, k is RFC 6979's deterministic nonce (HMAC-SHA-256/384/512 for P-256/384/521): never a
// random or reused one, either of which would give the private key away.
Result<ByteBuffer> ecdsa_sign(NistCurve curve, ByteSpan private_key_scalar, ByteSpan digest, ByteSpan k_nonce = {});

// The point on the curve? A public key that is not is not a key at all.
bool nist_point_valid(NistCurve curve, ByteSpan pub);

// ECDH: the x-coordinate of d * peer_pub (uncompressed 0x04||X||Y), checked on the curve first.
Result<ByteBuffer> ecdh_shared_x(NistCurve curve, ByteSpan private_key_scalar, ByteSpan peer_pub);

const char* nist_curve_name(NistCurve c);
// The curve a SubjectPublicKeyInfo names, by its OID bytes. False when this
// machine does not implement it, which is a refusal, not a pass.
bool nist_curve_from_oid(ByteSpan oid, NistCurve& out);
// The curve explicit ECParameters spell out (SEC 1 C.2: the prime, the coefficient b, the base point and the
// order, big-endian), as LibreSSL writes them instead of a curve OID. False for any curve but these three.
bool nist_curve_from_params(ByteSpan prime, ByteSpan b, ByteSpan base_point, ByteSpan order, NistCurve& out);

}  // namespace aios::crypto
