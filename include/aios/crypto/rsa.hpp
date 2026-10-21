#pragma once
// RSA signature verification, because a large part of the internet's
// certificates are still RSA — pypi.org and files.pythonhosted.org among them,
// which is why a machine that only checks ECDSA cannot install a Python
// package.
//
// Verification only, and both padding schemes that matter: PKCS#1 v1.5 (what
// certificates are signed with) and PSS (what a TLS 1.3 server signs its
// handshake with). Keys are whatever width the certificate carries; nothing
// here is fixed at 2048 bits.
#include <cstdint>
#include <string>

#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"

namespace aios::crypto {

struct RsaPublicKey {
  ByteBuffer modulus;      // big-endian, leading zeroes stripped
  ByteBuffer exponent;
  size_t bits() const;
  bool valid() const;
};

struct RsaPrivateKey {
  ByteBuffer modulus;          // n
  ByteBuffer public_exponent;  // e
  ByteBuffer private_exponent; // d
  ByteBuffer prime1;           // p (optional)
  ByteBuffer prime2;           // q (optional)
  ByteBuffer exponent1;        // dP (optional)
  ByteBuffer exponent2;        // dQ (optional)
  ByteBuffer coefficient;      // qInv (optional)
  size_t bits() const;
  bool valid() const;
};

// SubjectPublicKeyInfo's BIT STRING for an rsaEncryption key:
//   RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER }
Result<RsaPublicKey> rsa_parse_public_key(ByteSpan der);

// Parse PKCS#1 RSAPrivateKey or PKCS#8 PrivateKeyInfo (RSA)
Result<RsaPrivateKey> rsa_parse_private_key(ByteSpan der);

enum class RsaDigest { Sha256, Sha384, Sha512 };

// PKCS#1 v1.5: the signature decrypts to a DigestInfo this machine rebuilds and
// compares byte for byte.
bool rsa_verify_pkcs1(const RsaPublicKey& key, RsaDigest digest, ByteSpan message, ByteSpan signature);
// PSS with MGF1 over the same digest and a salt length equal to it, which is
// what every TLS 1.3 implementation uses.
bool rsa_verify_pss(const RsaPublicKey& key, RsaDigest digest, ByteSpan message, ByteSpan signature);

// RSA signing
Result<ByteBuffer> rsa_sign_pkcs1(const RsaPrivateKey& key, RsaDigest digest, ByteSpan message);
Result<ByteBuffer> rsa_sign_pss(const RsaPrivateKey& key, RsaDigest digest, ByteSpan message, ByteSpan salt = {});

// s^e mod n, exposed because it is the whole of RSA verification and worth
// being able to test on its own.
Result<ByteBuffer> rsa_public_op(const RsaPublicKey& key, ByteSpan signature);

// m^d mod n, RSA private exponentiation
Result<ByteBuffer> rsa_private_op(const RsaPrivateKey& key, ByteSpan em);

}  // namespace aios::crypto
