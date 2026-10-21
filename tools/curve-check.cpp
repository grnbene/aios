// The published vectors for the crypto floor the SSH transport stands on:
// RFC 7748 (X25519), RFC 8032 (Ed25519) and FIPS 180-4 (SHA-512). If any of
// this drifts, the machine cannot talk to an OpenSSH server at all — and it
// would drift silently, because a key exchange that computes the wrong shared
// secret looks exactly like a network fault.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "aios/crypto/curve25519.hpp"
#include "aios/crypto/nistp.hpp"
#include "aios/crypto/sha512.hpp"

using namespace aios;
using namespace aios::crypto;

namespace {

std::vector<uint8_t> unhex(const std::string& s) {
  std::vector<uint8_t> v;
  auto d = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
  for (size_t i = 0; i + 1 < s.size(); i += 2) v.push_back((uint8_t)(d(s[i]) * 16 + d(s[i + 1])));
  return v;
}

std::string hex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
  return s;
}

int failures = 0;
void check(bool cond, const char* what, const std::string& got = {}) {
  if (cond) { std::printf("  ok    %s\n", what); return; }
  ++failures;
  std::printf("  FAIL  %s%s%s\n", what, got.empty() ? "" : " — got ", got.c_str());
}

Scalar32 scalar_of(const std::vector<uint8_t>& v) {
  Scalar32 s{};
  for (size_t i = 0; i < 32 && i < v.size(); ++i) s[i] = v[i];
  return s;
}

}  // namespace

int main() {
  std::printf("SHA-512 (FIPS 180-4)\n");
  {
    auto h = sha512(ByteSpan((const uint8_t*)"abc", 3));
    std::string want = "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                       "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f";
    check(hex(h.data(), 64) == want, "sha512(\"abc\")", hex(h.data(), 64));
  }
  {
    std::string msg(1000000, 'a');
    auto h = sha512(ByteSpan((const uint8_t*)msg.data(), msg.size()));
    std::string want = "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
                       "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b";
    check(hex(h.data(), 64) == want, "sha512(one million 'a')", hex(h.data(), 64));
  }

  std::printf("X25519 (RFC 7748 §5.2, §6.1)\n");
  {
    auto a = scalar_of(unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"));
    auto b = scalar_of(unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb"));
    Point32 pub_a = x25519_base(a), pub_b = x25519_base(b);
    check(hex(pub_a.data(), 32) == "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a",
          "alice's public value", hex(pub_a.data(), 32));
    check(hex(pub_b.data(), 32) == "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f",
          "bob's public value", hex(pub_b.data(), 32));
    Point32 k1{}, k2{};
    bool o1 = x25519(k1, a, pub_b), o2 = x25519(k2, b, pub_a);
    check(o1 && o2 && std::memcmp(k1.data(), k2.data(), 32) == 0 &&
              hex(k1.data(), 32) == "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742",
          "both sides agree on the shared secret", hex(k1.data(), 32));
    // A small-order point must be reported, not silently used as a key.
    Point32 zero{}, out{};
    check(!x25519(out, a, zero), "a small-order peer point is refused");
  }

  std::printf("Ed25519 (RFC 8032 §7.1)\n");
  struct Vec { const char* seed; const char* pub; const char* msg; const char* sig; };
  const Vec vectors[] = {
      {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
       "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
       "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
      {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
       "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
       "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
      {"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
       "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
       "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
  };
  for (auto& v : vectors) {
    auto seed = scalar_of(unhex(v.seed));
    auto msg = unhex(v.msg);
    ByteSpan m(msg.data(), msg.size());
    auto pub = ed25519_public_from_seed(seed);
    check(hex(pub.data(), 32) == v.pub, "public key from seed", hex(pub.data(), 32));
    auto sig = ed25519_sign(m, seed, pub);
    check(hex(sig.data(), 64) == v.sig, "signature", hex(sig.data(), 64));
    check(ed25519_verify(m, sig, pub), "it verifies");
    auto bad = sig;
    bad[7] ^= 1;
    check(!ed25519_verify(m, bad, pub), "a flipped bit in the signature does not verify");
    auto wrong_key = pub;
    wrong_key[3] ^= 1;
    check(!ed25519_verify(m, sig, wrong_key), "another key does not verify it");
    if (!msg.empty()) {
      auto tampered = msg;
      tampered[0] ^= 1;
      check(!ed25519_verify(ByteSpan(tampered.data(), tampered.size()), sig, pub),
            "a changed message does not verify");
    }
  }
  {
    // S must be reduced modulo the group order: an unreduced S is the classic
    // malleability, a second valid-looking signature for the same message.
    auto seed = scalar_of(unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"));
    auto pub = ed25519_public_from_seed(seed);
    auto sig = ed25519_sign(ByteSpan((const uint8_t*)"", 0), seed, pub);
    Sig64 malleable = sig;
    // add L to S: L = 2^252 + 27742317777372353535851937790883648493
    static const uint8_t L[32] = {0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
                                  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10};
    unsigned carry = 0;
    for (int i = 0; i < 32; ++i) {
      unsigned s = (unsigned)malleable[32 + (size_t)i] + L[i] + carry;
      malleable[32 + (size_t)i] = (uint8_t)(s & 0xff);
      carry = s >> 8;
    }
    check(!ed25519_verify(ByteSpan((const uint8_t*)"", 0), malleable, pub),
          "S + L is rejected (no signature malleability)");
  }

  // The NIST prime curves, which is what a certificate chain is signed with.
  // Vectors from RFC 6979 A.2.5 and A.2.6, message "sample".
  std::printf("ECDSA over the NIST curves (RFC 6979)\n");
  {
    auto pub = unhex("04"
                     "60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6"
                     "7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299");
    auto digest = unhex("AF2BDBE1AA9B6EC1E2ADE1D694F41FC71A831D0268E9891562113D8A62ADD1BF");
    auto r = unhex("EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716");
    auto sv = unhex("F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8");
    ByteSpan P(pub.data(), pub.size()), D(digest.data(), digest.size());
    ByteSpan R(r.data(), r.size()), S(sv.data(), sv.size());
    check(nist_point_valid(NistCurve::P256, P), "P-256 public key is on the curve");
    check(ecdsa_verify(NistCurve::P256, P, D, R, S), "P-256 signature verifies");
    auto bad = digest;
    bad[0] ^= 1;
    check(!ecdsa_verify(NistCurve::P256, P, ByteSpan(bad.data(), bad.size()), R, S),
          "P-256 rejects a changed digest");
    auto badr = r;
    badr[31] ^= 1;
    check(!ecdsa_verify(NistCurve::P256, P, D, ByteSpan(badr.data(), badr.size()), S),
          "P-256 rejects a changed signature");
  }
  {
    auto pub = unhex("04"
                     "EC3A4E415B4E19A4568618029F427FA5DA9A8BC4AE92E02E06AAE5286B300C64"
                     "DEF8F0EA9055866064A254515480BC13"
                     "8015D9B72D7D57244EA8EF9AC0C621896708A59367F9DFB9F54CA84B3F1C9DB1"
                     "288B231C3AE0D4FE7344FD2533264720");
    auto digest = unhex("9A9083505BC92276AEC4BE312696EF7BF3BF603F4BBD381196A029F340585312"
                        "313BCA4A9B5B890EFEE42C77B1EE25FE");   // SHA-384("sample")
    auto r = unhex("94EDBB92A5ECB8AAD4736E56C691916B3F88140666CE9FA73D64C4EA95AD133C"
                   "81A648152E44ACF96E36DD1E80FABE46");
    auto sv = unhex("99EF4AEB15F178CEA1FE40DB2603138F130E740A19624526203B6351D0A3A94F"
                    "A329C145786E679E7B82C71A38628AC8");
    ByteSpan P(pub.data(), pub.size()), D(digest.data(), digest.size());
    ByteSpan R(r.data(), r.size()), S(sv.data(), sv.size());
    check(nist_point_valid(NistCurve::P384, P), "P-384 public key is on the curve");
    check(ecdsa_verify(NistCurve::P384, P, D, R, S), "P-384 signature verifies");
    auto bad = digest;
    bad[10] ^= 1;
    check(!ecdsa_verify(NistCurve::P384, P, ByteSpan(bad.data(), bad.size()), R, S),
          "P-384 rejects a changed digest");
    // The two curves must not be confused for one another.
    check(!ecdsa_verify(NistCurve::P256, P, D, R, S), "a P-384 key is not accepted as P-256");
  }
  {
    // SHA-384, which the intermediate certificates in a real chain sign with.
    auto h = sha384(ByteSpan((const uint8_t*)"abc", 3));
    std::string want = "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
                       "8086072ba1e7cc2358baeca134c825a7";
    check(hex(h.data(), 48) == want, "sha384(\"abc\")", hex(h.data(), 48));
  }

  std::printf(failures ? "CURVE: FAIL (%d)\n" : "CURVE: PASS\n", failures);
  return failures ? 1 : 0;
}
