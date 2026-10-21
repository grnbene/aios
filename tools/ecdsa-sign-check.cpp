// ECDSA signing over P-256, P-384 and P-521 against OpenSSL 3, byte for byte.
// OpenSSL's pkeyutl with nonce-type:1 signs with RFC 6979's deterministic k, as
// crypto::ecdsa_sign does when given no k: the same key and digest must give the
// same DER signature from both, and each must verify under the other. A k that
// is random, reused or derived any other way shows up here as a mismatch, and a
// k that is predictable is a private key handed to whoever holds one signature.
// Also RFC 6979 A.2.5's published P-256/SHA-256 vector, which needs no OpenSSL.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "aios/core/base64.hpp"
#include "aios/crypto/nistp.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/crypto/sha512.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/process.hpp"

using namespace aios;
using namespace aios::crypto;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}

std::vector<uint8_t> unhex(const std::string& s) {
  std::vector<uint8_t> v;
  auto d = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
  for (size_t i = 0; i + 1 < s.size(); i += 2) v.push_back((uint8_t)(d(s[i]) * 16 + d(s[i + 1])));
  return v;
}
std::string hex(const ByteBuffer& b) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (uint8_t x : b) { s += d[x >> 4]; s += d[x & 15]; }
  return s;
}
std::string read_text(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
void write_bytes(const std::string& path, const ByteBuffer& b) {
  std::ofstream f(path, std::ios::binary);
  f.write((const char*)b.data(), (std::streamsize)b.size());
}
// The DER between a PEM file's BEGIN and END lines.
ByteBuffer pem_body(const std::string& text) {
  size_t b = text.find("-----BEGIN");
  b = text.find('\n', b);
  size_t e = text.find("-----END", b);
  std::string body;
  for (size_t i = b + 1; i < e; ++i)
    if (text[i] != '\n' && text[i] != '\r') body += text[i];
  auto der = base64_decode(body);
  return der.ok() ? *der : ByteBuffer();
}

std::string openssl() {
  std::vector<std::string> names;
  if (const char* env = std::getenv("OPENSSL3"); env && *env) names.push_back(env);
  for (const char* n : {"/opt/homebrew/opt/openssl@3/bin/openssl", "/usr/local/opt/openssl@3/bin/openssl", "openssl"}) names.push_back(n);
  std::string p = hal::find_program(names);
  if (p.empty()) return p;
  auto v = hal::run_tool({p, "version"}, 10000);
  // nonce-type:1 (RFC 6979) arrived in OpenSSL 3.2; LibreSSL and older OpenSSL cannot be the oracle
  if (!v.ok() || v->output.rfind("OpenSSL 3.", 0) != 0 || v->output.rfind("OpenSSL 3.0", 0) == 0 || v->output.rfind("OpenSSL 3.1", 0) == 0) return "";
  return p;
}

void rfc6979_vector() {
  // RFC 6979 A.2.5: P-256, SHA-256, message "sample"
  auto x = unhex("c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721");
  std::string msg = "sample";
  auto h = sha256(ByteSpan((const uint8_t*)msg.data(), msg.size()));
  auto sig = ecdsa_sign(NistCurve::P256, ByteSpan(x.data(), x.size()), ByteSpan(h.data(), h.size()));
  const std::string want = "3046022100efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716022100f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8";
  check(sig.ok() && hex(*sig) == want, "RFC 6979 A.2.5 P-256/SHA-256 \"sample\": r and s as published" +
                                           (sig.ok() ? std::string() : ": " + sig.error().message));
}

struct CurveCase {
  NistCurve curve;
  const char* openssl_name;
  const char* digest;
};

}  // namespace

int main() {
  rfc6979_vector();

  std::string ossl = openssl();
  if (ossl.empty()) {
    std::printf("FAIL  no OpenSSL 3.2+ to compare with (set OPENSSL3=<path>)\n");
    return 1;
  }
  auto dir = hal::scratch_path("");
  if (!dir.ok()) { std::printf("FAIL  no scratch directory\n"); return 1; }
  std::string tmp = *dir + ".d";
  hal::process_host().make_dir(tmp);

  const CurveCase cases[] = {{NistCurve::P256, "prime256v1", "sha256"},
                             {NistCurve::P384, "secp384r1", "sha384"},
                             {NistCurve::P521, "secp521r1", "sha512"}};
  for (const auto& cc : cases) {
    for (int round = 0; round < 3; ++round) {
      std::string tag = std::string(nist_curve_name(cc.curve)) + " #" + std::to_string(round);
      std::string key = tmp + "/k.pem", dig = tmp + "/d.bin", ours = tmp + "/ours.der", theirs = tmp + "/theirs.der";
      auto g = hal::run_tool({ossl, "ecparam", "-name", cc.openssl_name, "-genkey", "-noout", "-out", key}, 30000);
      check(g.ok() && g->exit_code == 0, tag + ": openssl made a key");
      auto parsed = ec_parse_private_key(ByteSpan(pem_body(read_text(key))));
      check(parsed.ok() && parsed->curve == cc.curve, tag + ": the SEC1 key parses as " + nist_curve_name(cc.curve));
      if (!parsed.ok()) continue;

      std::string msg = "converge " + tag + " " + std::to_string(round * 7919);
      ByteSpan m((const uint8_t*)msg.data(), msg.size());
      ByteBuffer digest;
      if (cc.curve == NistCurve::P256) { auto d = sha256(m); digest.assign(d.begin(), d.end()); }
      else if (cc.curve == NistCurve::P384) { auto d = sha384(m); digest.assign(d.begin(), d.end()); }
      else { auto d = sha512(m); digest.assign(d.begin(), d.end()); }
      write_bytes(dig, digest);

      auto sig = ecdsa_sign(cc.curve, ByteSpan(parsed->d), ByteSpan(digest));
      check(sig.ok(), tag + ": signs");
      if (!sig.ok()) continue;
      write_bytes(ours, *sig);
      auto o = hal::run_tool({ossl, "pkeyutl", "-sign", "-inkey", key, "-in", dig, "-out", theirs,
                              "-pkeyopt", "nonce-type:1", "-pkeyopt", std::string("digest:") + cc.digest}, 30000);
      check(o.ok() && o->exit_code == 0, tag + ": openssl signs with RFC 6979" + (o.ok() ? " " + o->output : std::string()));
      std::string t = read_text(theirs);
      ByteBuffer theirs_sig(t.begin(), t.end());
      check(*sig == theirs_sig, tag + ": the same DER signature as OpenSSL's deterministic one");

      auto v = hal::run_tool({ossl, "pkeyutl", "-verify", "-inkey", key, "-in", dig, "-sigfile", ours}, 30000);
      check(v.ok() && v->exit_code == 0, tag + ": OpenSSL verifies ours");

      auto pub = ecdsa_public_key(cc.curve, ByteSpan(parsed->d));
      check(pub.ok() && nist_point_valid(cc.curve, ByteSpan(*pub)), tag + ": the public point is on the curve");
      // r and s out of OpenSSL's DER, for our verify
      ByteSpan d(theirs_sig);
      size_t p = 2;
      if (d.size() > 1 && d[1] == 0x81) p = 3;
      auto integer = [&](ByteBuffer& out) {
        if (p + 2 > d.size() || d[p] != 0x02) return false;
        size_t n = d[p + 1];
        p += 2;
        if (p + n > d.size()) return false;
        out.assign(d.begin() + (long)p, d.begin() + (long)(p + n));
        p += n;
        return true;
      };
      ByteBuffer r, s;
      bool parsed_rs = integer(r) && integer(s);
      check(parsed_rs && pub.ok() && ecdsa_verify(cc.curve, ByteSpan(*pub), ByteSpan(digest), ByteSpan(r), ByteSpan(s)),
            tag + ": we verify OpenSSL's");
      digest[0] ^= 1;
      check(parsed_rs && pub.ok() && !ecdsa_verify(cc.curve, ByteSpan(*pub), ByteSpan(digest), ByteSpan(r), ByteSpan(s)),
            tag + ": and refuse it over another digest");
    }
  }
  std::printf("%s\n", failures ? "FAILED" : "all ECDSA signing checks pass");
  return failures ? 1 : 0;
}
