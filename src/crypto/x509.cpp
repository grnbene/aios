#include "aios/crypto/x509.hpp"

#include <cstring>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include "aios/core/base64.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/crypto/rsa.hpp"
#include "aios/crypto/sha512.hpp"

namespace aios::crypto {
namespace {

// ---------------------------------------------------------------------------
// DER: tag, length, value. Nothing here allocates a parse tree; the reader
// walks the bytes and hands back spans of the original, because a certificate's
// signature covers the ORIGINAL bytes and a re-encoding of them is a different
// message.
// ---------------------------------------------------------------------------
struct Der {
  ByteSpan d;
  size_t pos = 0;
  bool bad = false;

  bool done() const { return pos >= d.size(); }
  uint8_t peek() { return pos < d.size() ? d[pos] : 0; }

  // One element: its tag, and the span of its contents.
  bool next(uint8_t& tag, ByteSpan& body, ByteSpan* whole = nullptr) {
    size_t start = pos;
    if (pos + 2 > d.size()) { bad = true; return false; }
    tag = d[pos++];
    size_t len = d[pos++];
    if (len & 0x80) {
      size_t n = len & 0x7f;
      if (n == 0 || n > 4 || pos + n > d.size()) { bad = true; return false; }
      len = 0;
      for (size_t i = 0; i < n; ++i) len = (len << 8) | d[pos++];
    }
    if (pos + len > d.size()) { bad = true; return false; }
    body = d.subspan(pos, len);
    pos += len;
    if (whole) *whole = d.subspan(start, pos - start);
    return true;
  }
  // The contents of the next element, required to have this tag.
  bool expect(uint8_t want, ByteSpan& body, ByteSpan* whole = nullptr) {
    uint8_t tag = 0;
    if (!next(tag, body, whole)) return false;
    if (tag != want) { bad = true; return false; }
    return true;
  }
};

bool oid_is(ByteSpan oid, std::initializer_list<uint8_t> bytes) {
  if (oid.size() != bytes.size()) return false;
  size_t i = 0;
  for (uint8_t b : bytes)
    if (oid[i++] != b) return false;
  return true;
}

SigAlg sig_alg_of(ByteSpan oid) {
  if (oid_is(oid, {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02})) return SigAlg::EcdsaSha256;
  if (oid_is(oid, {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x03})) return SigAlg::EcdsaSha384;
  if (oid_is(oid, {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b})) return SigAlg::RsaSha256;
  if (oid_is(oid, {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0c})) return SigAlg::RsaSha384;
  if (oid_is(oid, {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0d})) return SigAlg::RsaSha512;
  if (oid_is(oid, {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0a})) return SigAlg::RsaPss;
  return SigAlg::Unknown;
}

// An integer as the signature carries it: leading zero padding removed.
ByteBuffer trim_int(ByteSpan v) {
  size_t i = 0;
  while (i + 1 < v.size() && v[i] == 0) ++i;
  return ByteBuffer(v.begin() + (long)i, v.end());
}

// "YYMMDDHHMMSSZ" or "YYYYMMDDHHMMSSZ" to nanoseconds since the epoch.
uint64_t parse_time(uint8_t tag, ByteSpan v) {
  std::string s = string_of(v);
  int year = 0, mon = 0, day = 0, hh = 0, mm = 0, ss = 0;
  auto two = [&](size_t at) { return (s.size() > at + 1) ? (s[at] - '0') * 10 + (s[at + 1] - '0') : 0; };
  size_t i = 0;
  if (tag == 0x17) {                    // UTCTime: two-digit year
    int yy = two(0);
    year = yy < 50 ? 2000 + yy : 1900 + yy;
    i = 2;
  } else {                              // GeneralizedTime: four-digit year
    year = two(0) * 100 + two(2);
    i = 4;
  }
  mon = two(i); day = two(i + 2); hh = two(i + 4); mm = two(i + 6); ss = two(i + 8);
  // Days from the civil calendar (Howard Hinnant's algorithm), then seconds.
  int y = year - (mon <= 2);
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (unsigned)((153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + day - 1);
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
  int64_t secs = days * 86400 + hh * 3600 + mm * 60 + ss;
  return secs > 0 ? (uint64_t)secs * 1000000000ull : 0;
}

// The CN out of a Name, for saying who a certificate is for in a message.
std::string common_name(ByteSpan name_der) {
  Der rdn{name_der};
  while (!rdn.done()) {
    uint8_t tag = 0;
    ByteSpan set;
    if (!rdn.next(tag, set)) break;
    Der inner{set};
    while (!inner.done()) {
      ByteSpan pair;
      if (!inner.expect(0x30, pair)) break;
      Der kv{pair};
      ByteSpan oid, val;
      if (!kv.expect(0x06, oid)) break;
      uint8_t vtag = 0;
      if (!kv.next(vtag, val)) break;
      if (oid_is(oid, {0x55, 0x04, 0x03})) return string_of(val);      // id-at-commonName
    }
  }
  return {};
}

void parse_extensions(ByteSpan exts, Certificate& c) {
  Der seq{exts};
  ByteSpan list;
  if (!seq.expect(0x30, list)) return;
  Der items{list};
  while (!items.done()) {
    ByteSpan ext;
    if (!items.expect(0x30, ext)) return;
    Der e{ext};
    ByteSpan oid;
    if (!e.expect(0x06, oid)) return;
    ByteSpan body;
    uint8_t tag = 0;
    if (!e.next(tag, body)) return;
    if (tag == 0x01) { if (!e.next(tag, body)) return; }               // skip the critical flag
    if (tag != 0x04) continue;                                          // extnValue is an OCTET STRING
    if (oid_is(oid, {0x55, 0x1d, 0x11})) {                              // subjectAltName
      Der san{body};
      ByteSpan names;
      if (!san.expect(0x30, names)) continue;
      Der n{names};
      while (!n.done()) {
        uint8_t nt = 0;
        ByteSpan nv;
        if (!n.next(nt, nv)) break;
        if (nt == 0x82) c.dns_names.push_back(string_of(nv));           // dNSName
        else if (nt == 0x87 && (nv.size() == 4 || nv.size() == 16))     // iPAddress
          c.ip_addresses.push_back(ByteBuffer(nv.begin(), nv.end()));
      }
    } else if (oid_is(oid, {0x55, 0x1d, 0x13})) {                       // basicConstraints
      Der bc{body};
      ByteSpan inner;
      if (!bc.expect(0x30, inner)) continue;
      Der b{inner};
      if (!b.done()) {
        uint8_t bt = 0;
        ByteSpan bv;
        if (b.next(bt, bv) && bt == 0x01 && bv.size() == 1) c.is_ca = bv[0] != 0;
      }
    }
  }
}

}  // namespace

const char* sig_alg_name(SigAlg a) {
  switch (a) {
    case SigAlg::EcdsaSha256: return "ecdsa-with-SHA256";
    case SigAlg::EcdsaSha384: return "ecdsa-with-SHA384";
    case SigAlg::RsaSha256: return "sha256WithRSAEncryption";
    case SigAlg::RsaSha384: return "sha384WithRSAEncryption";
    default: return "an algorithm this machine does not implement";
  }
}

Result<Certificate> x509_parse(ByteSpan der) {
  Certificate c;
  c.der.assign(der.begin(), der.end());
  Der top{der};
  ByteSpan cert_body;
  if (!top.expect(0x30, cert_body)) return Result<Certificate>(Status::Corrupt, "not a DER SEQUENCE");
  Der cert{cert_body};

  ByteSpan tbs_body, tbs_whole;
  if (!cert.expect(0x30, tbs_body, &tbs_whole))
    return Result<Certificate>(Status::Corrupt, "no tbsCertificate");
  c.tbs.assign(tbs_whole.begin(), tbs_whole.end());

  Der tbs{tbs_body};
  ByteSpan field;
  uint8_t tag = 0;
  if (!tbs.next(tag, field)) return Result<Certificate>(Status::Corrupt, "truncated tbsCertificate");
  if (tag == 0xa0) { if (!tbs.next(tag, field)) return Result<Certificate>(Status::Corrupt, "no serial"); }
  // `field` now holds the serial number; the signature algorithm follows.
  ByteSpan alg;
  if (!tbs.expect(0x30, alg)) return Result<Certificate>(Status::Corrupt, "no inner signature algorithm");
  ByteSpan issuer, issuer_whole;
  if (!tbs.expect(0x30, issuer, &issuer_whole)) return Result<Certificate>(Status::Corrupt, "no issuer");
  c.issuer.assign(issuer_whole.begin(), issuer_whole.end());
  c.issuer_cn = common_name(issuer);

  ByteSpan validity;
  if (!tbs.expect(0x30, validity)) return Result<Certificate>(Status::Corrupt, "no validity");
  {
    Der v{validity};
    uint8_t t1 = 0, t2 = 0;
    ByteSpan nb, na;
    if (v.next(t1, nb)) c.not_before_ns = parse_time(t1, nb);
    if (v.next(t2, na)) c.not_after_ns = parse_time(t2, na);
  }

  ByteSpan subject, subject_whole;
  if (!tbs.expect(0x30, subject, &subject_whole)) return Result<Certificate>(Status::Corrupt, "no subject");
  c.subject.assign(subject_whole.begin(), subject_whole.end());
  c.subject_cn = common_name(subject);

  ByteSpan spki;
  if (!tbs.expect(0x30, spki)) return Result<Certificate>(Status::Corrupt, "no subjectPublicKeyInfo");
  {
    Der s{spki};
    ByteSpan algid;
    if (!s.expect(0x30, algid)) return Result<Certificate>(Status::Corrupt, "no key algorithm");
    Der a{algid};
    ByteSpan keytype, params;
    if (!a.expect(0x06, keytype)) return Result<Certificate>(Status::Corrupt, "no key algorithm oid");
    bool ec = oid_is(keytype, {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01});   // id-ecPublicKey
    if (ec) {
      if (!a.expect(0x06, params)) return Result<Certificate>(Status::Corrupt, "an EC key with no curve");
      if (!nist_curve_from_oid(params, c.key_curve))
        return Result<Certificate>(Status::Unsupported, "the key names a curve this machine does not implement");
      ByteSpan bits;
      if (!s.expect(0x03, bits)) return Result<Certificate>(Status::Corrupt, "no key bits");
      if (bits.empty()) return Result<Certificate>(Status::Corrupt, "empty key");
      c.key_point.assign(bits.begin() + 1, bits.end());     // drop the unused-bits count
      c.key_is_ec = true;
    }
    else if (oid_is(keytype, {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01})) {   // rsaEncryption
      ByteSpan bits;
      if (!s.expect(0x03, bits)) return Result<Certificate>(Status::Corrupt, "no key bits");
      if (bits.empty()) return Result<Certificate>(Status::Corrupt, "empty key");
      auto rsa = rsa_parse_public_key(bits.subspan(1));      // drop the unused-bits count
      if (!rsa.ok()) return Result<Certificate>(rsa.error());
      c.rsa_key = *rsa;
      c.key_is_rsa = true;
    }
    // A key type this machine does not implement parses to "no key here";
    // whether that is fatal is the caller's decision, and it says so with the
    // algorithm's real name.
  }

  // Extensions live in [3]; anything before them is skipped.
  while (!tbs.done()) {
    ByteSpan body;
    uint8_t t = 0;
    if (!tbs.next(t, body)) break;
    if (t == 0xa3) { parse_extensions(body, c); break; }
  }

  ByteSpan outer_alg;
  if (!cert.expect(0x30, outer_alg)) return Result<Certificate>(Status::Corrupt, "no signature algorithm");
  {
    Der a{outer_alg};
    ByteSpan oid;
    if (!a.expect(0x06, oid)) return Result<Certificate>(Status::Corrupt, "no signature algorithm oid");
    c.sig_alg = sig_alg_of(oid);
  }
  ByteSpan sig;
  if (!cert.expect(0x03, sig)) return Result<Certificate>(Status::Corrupt, "no signature");
  if (!sig.empty() && (c.sig_alg == SigAlg::RsaSha256 || c.sig_alg == SigAlg::RsaSha384 ||
                       c.sig_alg == SigAlg::RsaSha512 || c.sig_alg == SigAlg::RsaPss)) {
    c.sig_raw.assign(sig.begin() + 1, sig.end());            // drop the unused-bits count
  }
  if (!sig.empty() && (c.sig_alg == SigAlg::EcdsaSha256 || c.sig_alg == SigAlg::EcdsaSha384)) {
    Der s{sig.subspan(1)};                                   // drop the unused-bits count
    ByteSpan pair;
    if (!s.expect(0x30, pair)) return Result<Certificate>(Status::Corrupt, "malformed ECDSA signature");
    Der rs{pair};
    ByteSpan r, sv;
    if (!rs.expect(0x02, r) || !rs.expect(0x02, sv))
      return Result<Certificate>(Status::Corrupt, "malformed ECDSA signature integers");
    c.sig_r = trim_int(r);
    c.sig_s = trim_int(sv);
  }
  return c;
}

Result<std::vector<Certificate>> x509_parse_bundle(ByteSpan pem) {
  std::vector<Certificate> out;
  std::string text = string_of(pem);
  const std::string begin = "-----BEGIN CERTIFICATE-----";
  const std::string end = "-----END CERTIFICATE-----";
  size_t at = 0;
  while ((at = text.find(begin, at)) != std::string::npos) {
    size_t body = at + begin.size();
    size_t stop = text.find(end, body);
    if (stop == std::string::npos) break;
    std::string b64;
    for (char ch : text.substr(body, stop - body))
      if (ch != '\n' && ch != '\r' && ch != ' ') b64 += ch;
    at = stop + end.size();
    auto der = base64_decode(b64);
    if (!der.ok()) continue;                        // one unreadable entry is not a broken store
    auto c = x509_parse(ByteSpan(der->data(), der->size()));
    if (c.ok()) out.push_back(*c);
  }
  if (out.empty()) return Result<std::vector<Certificate>>(Status::NotFound, "no certificates in this bundle");
  return out;
}

Result<PrivateKey> x509_parse_private_key(ByteSpan pem_or_der) {
  ByteBuffer der_bytes;
  std::string text = string_of(pem_or_der);
  size_t begin_pos = text.find("-----BEGIN ");
  if (begin_pos != std::string::npos) {
    size_t header_end = text.find("-----", begin_pos + 11);
    if (header_end != std::string::npos) {
      size_t body = header_end + 5;
      size_t stop = text.find("-----END ", body);
      if (stop != std::string::npos) {
        std::string b64;
        for (char ch : text.substr(body, stop - body))
          if (ch != '\n' && ch != '\r' && ch != ' ') b64 += ch;
        auto decoded = base64_decode(b64);
        if (decoded.ok()) der_bytes = std::move(*decoded);
      }
    }
  }

  ByteSpan der = der_bytes.empty() ? pem_or_der : ByteSpan(der_bytes.data(), der_bytes.size());

  auto ec_res = ec_parse_private_key(der);
  if (ec_res.ok()) {
    PrivateKey pk;
    pk.is_ec = true;
    pk.ec_key = std::move(*ec_res);
    return pk;
  }

  auto rsa_res = rsa_parse_private_key(der);
  if (rsa_res.ok()) {
    PrivateKey pk;
    pk.is_rsa = true;
    pk.rsa_key = std::move(*rsa_res);
    return pk;
  }

  return Result<PrivateKey>(Status::Invalid, "unrecognised private key format (neither EC nor RSA)");
}

bool x509_signed_by(const Certificate& cert, const Certificate& issuer) {
  ByteSpan tbs(cert.tbs.data(), cert.tbs.size());
  if (issuer.key_is_rsa) {
    if (cert.sig_raw.empty()) return false;
    ByteSpan sig(cert.sig_raw.data(), cert.sig_raw.size());
    switch (cert.sig_alg) {
      case SigAlg::RsaSha256: return rsa_verify_pkcs1(issuer.rsa_key, RsaDigest::Sha256, tbs, sig);
      case SigAlg::RsaSha384: return rsa_verify_pkcs1(issuer.rsa_key, RsaDigest::Sha384, tbs, sig);
      case SigAlg::RsaSha512: return rsa_verify_pkcs1(issuer.rsa_key, RsaDigest::Sha512, tbs, sig);
      case SigAlg::RsaPss:
        // The digest lives in the algorithm parameters; SHA-256 is what every
        // certificate seen in practice uses, and a wrong guess simply fails.
        return rsa_verify_pss(issuer.rsa_key, RsaDigest::Sha256, tbs, sig);
      default: return false;                        // an EC signature under an RSA key
    }
  }
  if (!issuer.key_is_ec) return false;
  if (cert.sig_r.empty() || cert.sig_s.empty()) return false;
  if (cert.sig_alg == SigAlg::EcdsaSha256) {
    auto h = sha256(tbs);
    return ecdsa_verify(issuer.key_curve, ByteSpan(issuer.key_point.data(), issuer.key_point.size()),
                        ByteSpan(h.data(), h.size()), ByteSpan(cert.sig_r.data(), cert.sig_r.size()),
                        ByteSpan(cert.sig_s.data(), cert.sig_s.size()));
  }
  if (cert.sig_alg == SigAlg::EcdsaSha384) {
    auto h = sha384(tbs);
    return ecdsa_verify(issuer.key_curve, ByteSpan(issuer.key_point.data(), issuer.key_point.size()),
                        ByteSpan(h.data(), h.size()), ByteSpan(cert.sig_r.data(), cert.sig_r.size()),
                        ByteSpan(cert.sig_s.data(), cert.sig_s.size()));
  }
  return false;
}

bool x509_covers_host(const Certificate& cert, const std::string& host) {
  // An address, not a name: only an iPAddress SAN can cover it (RFC 6125 6.2.1),
  // never a dNSName or the CN that happens to read the same.
  {
    std::string h = host;
    if (h.size() > 2 && h.front() == '[' && h.back() == ']') h = h.substr(1, h.size() - 2);
    uint8_t a4[4], a6[16];
    if (::inet_pton(AF_INET, h.c_str(), a4) == 1) {
      for (const auto& ip : cert.ip_addresses) if (ip.size() == 4 && std::memcmp(ip.data(), a4, 4) == 0) return true;
      return false;
    }
    if (::inet_pton(AF_INET6, h.c_str(), a6) == 1) {
      for (const auto& ip : cert.ip_addresses) if (ip.size() == 16 && std::memcmp(ip.data(), a6, 16) == 0) return true;
      return false;
    }
  }
  auto lower = [](std::string s) { for (auto& ch : s) if (ch >= 'A' && ch <= 'Z') ch += 32; return s; };
  std::string want = lower(host);
  for (const auto& raw : cert.dns_names) {
    std::string name = lower(raw);
    if (name == want) return true;
    // One leftmost wildcard, matching exactly one label — "*.a.com" is not a
    // licence for "b.c.a.com", and never for the bare domain.
    if (name.size() > 2 && name[0] == '*' && name[1] == '.') {
      std::string suffix = name.substr(1);          // ".a.com"
      if (want.size() > suffix.size() && want.compare(want.size() - suffix.size(), suffix.size(), suffix) == 0) {
        std::string label = want.substr(0, want.size() - suffix.size());
        if (label.find('.') == std::string::npos) return true;
      }
    }
  }
  // No SAN at all: fall back to the subject CN, the way the world did before
  // SAN was mandatory.
  return cert.dns_names.empty() && !cert.subject_cn.empty() && lower(cert.subject_cn) == want;
}

ChainResult x509_verify_chain(const std::vector<Certificate>& chain, const std::vector<Certificate>& anchors,
                              const std::string& host, uint64_t now_ns) {
  ChainResult r;
  if (chain.empty()) { r.why = "the peer sent no certificate"; return r; }
  const Certificate& leaf = chain.front();

  if (!host.empty() && !x509_covers_host(leaf, host)) {
    std::string names;
    for (auto& n : leaf.dns_names) names += (names.empty() ? "" : ", ") + n;
    for (auto& ip : leaf.ip_addresses) {
      char b[64] = {0};
      ::inet_ntop(ip.size() == 4 ? AF_INET : AF_INET6, ip.data(), b, sizeof b);
      names += (names.empty() ? "" : ", ") + std::string(b);
    }
    r.why = "the certificate is for " + (names.empty() ? leaf.subject_cn : names) + ", not " + host;
    return r;
  }
  for (const auto& c : chain) {
    if (now_ns && c.not_after_ns && now_ns > c.not_after_ns) {
      r.why = "the certificate for " + c.subject_cn + " expired";
      return r;
    }
    if (now_ns && c.not_before_ns && now_ns < c.not_before_ns) {
      r.why = "the certificate for " + c.subject_cn + " is not valid yet";
      return r;
    }
  }

  // Walk up: each certificate must be signed by the next, and the last must be
  // signed by something already trusted.
  for (size_t i = 0; i < chain.size(); ++i) {
    const Certificate& c = chain[i];
    for (const auto& a : anchors) {
      if (a.subject != c.issuer) continue;
      if (!x509_signed_by(c, a)) continue;
      r.trusted = true;
      r.anchor = a.subject_cn.empty() ? std::string("a certificate in this machine's trust store") : a.subject_cn;
      return r;
    }
    if (i + 1 >= chain.size()) {
      r.why = "the chain ends at " + c.subject_cn + ", issued by " + c.issuer_cn +
              ", which is not in this machine's trust store";
      return r;
    }
    const Certificate& up = chain[i + 1];
    if (up.subject != c.issuer) {
      r.why = "the chain is broken: " + c.subject_cn + " names " + c.issuer_cn + " as its issuer, and the next "
              "certificate is " + up.subject_cn;
      return r;
    }
    if (!up.is_ca) { r.why = up.subject_cn + " signed a certificate without being an authority"; return r; }
    if (!x509_signed_by(c, up)) {
      r.why = "the signature on " + c.subject_cn + " (" + sig_alg_name(c.sig_alg) + ") does not verify against " +
              up.subject_cn;
      return r;
    }
  }
  r.why = "no trusted certificate was reached";
  return r;
}

}  // namespace aios::crypto
