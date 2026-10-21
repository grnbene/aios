#pragma once
// X.509, far enough to answer one question honestly: is the machine on the
// other end of this connection the one it claims to be?
//
// That means parsing DER (not a library's idea of it), reading the fields a
// decision actually rests on — who signed it, who it is for, when it is valid,
// what key it carries, which names it covers — and checking the signature chain
// up to a certificate this machine already trusts. Anything this does not
// implement is a refusal with the reason, never a pass.
#include <cstdint>
#include <string>
#include <vector>

#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"
#include "aios/crypto/nistp.hpp"
#include "aios/crypto/rsa.hpp"

namespace aios::crypto {

enum class SigAlg { EcdsaSha256, EcdsaSha384, RsaSha256, RsaSha384, RsaSha512, RsaPss, Unknown };

struct Certificate {
  ByteBuffer der;               // the whole thing, as it arrived
  ByteBuffer tbs;               // exactly the bytes the signature covers
  ByteBuffer issuer;            // the issuer Name, DER, for chaining
  ByteBuffer subject;           // the subject Name, DER
  std::string subject_cn;       // for saying who this is, in a message a person reads
  std::string issuer_cn;
  SigAlg sig_alg = SigAlg::Unknown;
  ByteBuffer sig_r, sig_s;      // ECDSA signature integers
  NistCurve key_curve = NistCurve::P256;
  bool key_is_ec = false;
  ByteBuffer key_point;         // uncompressed EC point
  bool key_is_rsa = false;
  RsaPublicKey rsa_key;
  ByteBuffer sig_raw;           // an RSA signature is one integer, not two
  std::vector<std::string> dns_names;
  std::vector<ByteBuffer> ip_addresses;   // iPAddress SANs: 4 bytes (IPv4) or 16 (IPv6)
  bool is_ca = false;
  uint64_t not_before_ns = 0, not_after_ns = 0;
};

Result<Certificate> x509_parse(ByteSpan der);
// Every certificate in a PEM bundle; the machine's trust store is one of these.
Result<std::vector<Certificate>> x509_parse_bundle(ByteSpan pem);

struct PrivateKey {
  bool is_ec = false;
  EcPrivateKey ec_key;
  bool is_rsa = false;
  RsaPrivateKey rsa_key;
  bool valid() const { return (is_ec && ec_key.valid()) || (is_rsa && rsa_key.valid()); }
};

// Parse a private key from PEM or DER
Result<PrivateKey> x509_parse_private_key(ByteSpan pem_or_der);

// Does `cert` carry a signature made by `issuer`'s key?
bool x509_signed_by(const Certificate& cert, const Certificate& issuer);
// Does this certificate cover `host` (exact name or a leftmost wildcard)?
bool x509_covers_host(const Certificate& cert, const std::string& host);

struct ChainResult {
  bool trusted = false;
  std::string anchor;      // the trusted certificate the chain reached
  std::string why;         // when it is not trusted, what stopped it
};
// The leaf first, as TLS sends them. `now_ns` is wall time; a zero means the
// machine cannot tell the time and dates are reported but not enforced.
ChainResult x509_verify_chain(const std::vector<Certificate>& chain,
                              const std::vector<Certificate>& anchors,
                              const std::string& host, uint64_t now_ns);

const char* sig_alg_name(SigAlg a);

}  // namespace aios::crypto
