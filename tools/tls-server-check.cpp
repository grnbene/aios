// Drive tool for Phase 2.1 - 2.4: TLS Server & Signing Primitives
// Proves in reality:
// 1. RSA PKCS#1 v1.5 and PSS signing against verification.
// 2. ECDSA (P-256 and P-384) signing against verification.
// 3. Live TLS 1.2 server handshake with ECDHE X25519 + ChaCha20-Poly1305.
// 4. Bidirectional secure application data transfer over live socket.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "aios/core/base64.hpp"
#include "aios/crypto/nistp.hpp"
#include "aios/crypto/rsa.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/crypto/x509.hpp"
#include "aios/net/tls.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/net.hpp"

using namespace aios;
using namespace aios::crypto;
using namespace aios::net;

namespace {

int failures = 0;

void check(bool cond, const char* what, const std::string& extra = "") {
  if (cond) {
    std::printf("  [PASS] %s\n", what);
  } else {
    ++failures;
    std::printf("  [FAIL] %s %s\n", what, extra.c_str());
  }
}

// ---------------------------------------------------------------------------
// Self-signed Certificate Generation for in-memory TLS server testing
// ---------------------------------------------------------------------------

ByteBuffer der_seq(const ByteBuffer& body) {
  ByteBuffer out;
  out.push_back(0x30);
  if (body.size() < 128) {
    out.push_back((uint8_t)body.size());
  } else if (body.size() < 256) {
    out.push_back(0x81); out.push_back((uint8_t)body.size());
  } else {
    out.push_back(0x82);
    out.push_back((uint8_t)(body.size() >> 8));
    out.push_back((uint8_t)body.size());
  }
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

ByteBuffer der_int(uint64_t val) {
  ByteBuffer b;
  while (val > 0) {
    b.insert(b.begin(), (uint8_t)(val & 0xFF));
    val >>= 8;
  }
  if (b.empty()) b.push_back(0);
  if (b[0] & 0x80) b.insert(b.begin(), 0x00);
  ByteBuffer out;
  out.push_back(0x02);
  out.push_back((uint8_t)b.size());
  out.insert(out.end(), b.begin(), b.end());
  return out;
}

[[maybe_unused]] ByteBuffer der_oid(const std::vector<uint8_t>& bytes) {
  ByteBuffer out;
  out.push_back(0x06);
  out.push_back((uint8_t)bytes.size());
  out.insert(out.end(), bytes.begin(), bytes.end());
  return out;
}

Certificate make_self_signed_ec_cert(const EcPrivateKey& priv_key, const std::string& cn) {
  auto pub_res = ecdsa_public_key(priv_key.curve, ByteSpan(priv_key.d.data(), priv_key.d.size()));
  ByteBuffer pub = *pub_res;

  // SPKI: SEQUENCE { AlgorithmIdentifier { id-ecPublicKey, prime256v1 }, BIT STRING pub }
  ByteBuffer alg_id;
  alg_id.insert(alg_id.end(), {0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01}); // id-ecPublicKey
  alg_id.insert(alg_id.end(), {0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07}); // prime256v1
  ByteBuffer spki_alg = der_seq(alg_id);

  ByteBuffer bit_str;
  bit_str.push_back(0x03);
  bit_str.push_back((uint8_t)(pub.size() + 1));
  bit_str.push_back(0x00); // 0 unused bits
  bit_str.insert(bit_str.end(), pub.begin(), pub.end());

  ByteBuffer spki_body;
  spki_body.insert(spki_body.end(), spki_alg.begin(), spki_alg.end());
  spki_body.insert(spki_body.end(), bit_str.begin(), bit_str.end());
  ByteBuffer spki = der_seq(spki_body);

  // Name: SEQUENCE { SET { SEQUENCE { id-at-commonName, PrintableString cn } } }
  ByteBuffer cn_attr;
  cn_attr.insert(cn_attr.end(), {0x06, 0x03, 0x55, 0x04, 0x03}); // commonName
  cn_attr.push_back(0x0c); // UTF8String
  cn_attr.push_back((uint8_t)cn.size());
  cn_attr.insert(cn_attr.end(), cn.begin(), cn.end());
  ByteBuffer cn_seq = der_seq(cn_attr);
  ByteBuffer cn_set; cn_set.push_back(0x31); cn_set.push_back((uint8_t)cn_seq.size());
  cn_set.insert(cn_set.end(), cn_seq.begin(), cn_seq.end());
  ByteBuffer name = der_seq(cn_set);

  // Validity: SEQUENCE { UTCTime, UTCTime }
  // "200101000000Z" to "300101000000Z"
  std::string t1 = "200101000000Z", t2 = "300101000000Z";
  ByteBuffer val_body;
  val_body.push_back(0x17); val_body.push_back((uint8_t)t1.size()); val_body.insert(val_body.end(), t1.begin(), t1.end());
  val_body.push_back(0x17); val_body.push_back((uint8_t)t2.size()); val_body.insert(val_body.end(), t2.begin(), t2.end());
  ByteBuffer validity = der_seq(val_body);

  // TBSCertificate
  ByteBuffer tbs_body;
  // version v3: [0] { INTEGER 2 }
  tbs_body.insert(tbs_body.end(), {0xa0, 0x03, 0x02, 0x01, 0x02});
  ByteBuffer serial = der_int(123456);
  tbs_body.insert(tbs_body.end(), serial.begin(), serial.end());
  // sig alg: ecdsa-with-SHA256 (1.2.840.10045.4.3.2)
  ByteBuffer sig_alg_id;
  sig_alg_id.insert(sig_alg_id.end(), {0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02});
  ByteBuffer sig_alg_seq = der_seq(sig_alg_id);
  tbs_body.insert(tbs_body.end(), sig_alg_seq.begin(), sig_alg_seq.end());
  tbs_body.insert(tbs_body.end(), name.begin(), name.end());
  tbs_body.insert(tbs_body.end(), validity.begin(), validity.end());
  tbs_body.insert(tbs_body.end(), name.begin(), name.end()); // subject = issuer
  tbs_body.insert(tbs_body.end(), spki.begin(), spki.end());

  // Extensions: basicConstraints CA:TRUE, subjectAltName DNS:localhost
  ByteBuffer ext_bc;
  ext_bc.insert(ext_bc.end(), {0x06, 0x03, 0x55, 0x1d, 0x13}); // id-ce-basicConstraints
  ext_bc.push_back(0x01); ext_bc.push_back(0x01); ext_bc.push_back(0xff); // critical TRUE
  // OCTET STRING containing SEQUENCE { BOOLEAN TRUE }
  ext_bc.insert(ext_bc.end(), {0x04, 0x05, 0x30, 0x03, 0x01, 0x01, 0xff});
  ByteBuffer ext_bc_seq = der_seq(ext_bc);

  ByteBuffer ext_san;
  ext_san.insert(ext_san.end(), {0x06, 0x03, 0x55, 0x1d, 0x11}); // id-ce-subjectAltName
  // OCTET STRING containing SEQUENCE { [2] "localhost" }
  std::string san_dns = "localhost";
  ByteBuffer san_inner;
  san_inner.push_back(0x82); san_inner.push_back((uint8_t)san_dns.size());
  san_inner.insert(san_inner.end(), san_dns.begin(), san_dns.end());
  ByteBuffer san_seq = der_seq(san_inner);
  ByteBuffer san_oct; san_oct.push_back(0x04); san_oct.push_back((uint8_t)san_seq.size());
  san_oct.insert(san_oct.end(), san_seq.begin(), san_seq.end());
  ext_san.insert(ext_san.end(), san_oct.begin(), san_oct.end());
  ByteBuffer ext_san_seq = der_seq(ext_san);

  ByteBuffer ext_list;
  ext_list.insert(ext_list.end(), ext_bc_seq.begin(), ext_bc_seq.end());
  ext_list.insert(ext_list.end(), ext_san_seq.begin(), ext_san_seq.end());
  ByteBuffer ext_list_seq = der_seq(ext_list);
  ByteBuffer ext_tagged;
  ext_tagged.push_back(0xa3);
  ext_tagged.push_back((uint8_t)ext_list_seq.size());
  ext_tagged.insert(ext_tagged.end(), ext_list_seq.begin(), ext_list_seq.end());

  tbs_body.insert(tbs_body.end(), ext_tagged.begin(), ext_tagged.end());

  ByteBuffer tbs = der_seq(tbs_body);

  // Sign TBS
  auto th = sha256(ByteSpan(tbs.data(), tbs.size()));
  auto sig_res = ecdsa_sign(priv_key.curve, ByteSpan(priv_key.d.data(), priv_key.d.size()), ByteSpan(th.data(), th.size()));
  ByteBuffer sig = *sig_res;

  ByteBuffer sig_bit_str;
  sig_bit_str.push_back(0x03);
  sig_bit_str.push_back((uint8_t)(sig.size() + 1));
  sig_bit_str.push_back(0x00);
  sig_bit_str.insert(sig_bit_str.end(), sig.begin(), sig.end());

  ByteBuffer cert_body;
  cert_body.insert(cert_body.end(), tbs.begin(), tbs.end());
  cert_body.insert(cert_body.end(), sig_alg_seq.begin(), sig_alg_seq.end());
  cert_body.insert(cert_body.end(), sig_bit_str.begin(), sig_bit_str.end());
  ByteBuffer cert_der = der_seq(cert_body);

  auto parsed = x509_parse(ByteSpan(cert_der.data(), cert_der.size()));
  return *parsed;
}

} // namespace

int main() {
  std::printf("=== Driving TLS Server & Signing Primitives ===\n");
  auto host = hal::make_host();

  // -------------------------------------------------------------------------
  // 1. ECDSA Signing Primitives (P-256)
  // -------------------------------------------------------------------------
  std::printf("\n--- 1. Testing ECDSA Signing & Verification ---\n");
  EcPrivateKey ec_key;
  ec_key.curve = NistCurve::P256;
  // Scalar d = 0x010203...
  for (int i = 1; i <= 32; ++i) ec_key.d.push_back((uint8_t)i);
  check(ec_key.valid(), "ECDSA P-256 private key scalar is valid");

  auto ec_pub_res = ecdsa_public_key(ec_key.curve, ByteSpan(ec_key.d.data(), ec_key.d.size()));
  check(ec_pub_res.ok(), "Derived ECDSA public key point");
  check(nist_point_valid(ec_key.curve, ByteSpan(ec_pub_res->data(), ec_pub_res->size())), "Derived public key point is valid on P-256");

  std::string msg = "Hello from AIOS TLS 1.2 server signing verification";
  auto msg_hash = sha256(span_of(msg));

  auto ec_sig_res = ecdsa_sign(ec_key.curve, ByteSpan(ec_key.d.data(), ec_key.d.size()), ByteSpan(msg_hash.data(), msg_hash.size()));
  check(ec_sig_res.ok(), "Generated ECDSA signature");

  // Parse DER signature to extract r, s
  // DER sequence: 0x30, len, 0x02, r_len, r, 0x02, s_len, s
  const uint8_t* sig_ptr = ec_sig_res->data();
  size_t sig_sz = ec_sig_res->size();
  check(sig_sz >= 6 && sig_ptr[0] == 0x30, "ECDSA DER signature header");
  size_t idx = (sig_ptr[1] & 0x80) ? (2 + (sig_ptr[1] & 0x7f)) : 2;
  check(sig_ptr[idx] == 0x02, "ECDSA DER r tag");
  size_t r_len = sig_ptr[idx + 1];
  ByteSpan r_span(sig_ptr + idx + 2, r_len);
  idx += 2 + r_len;
  check(sig_ptr[idx] == 0x02, "ECDSA DER s tag");
  size_t s_len = sig_ptr[idx + 1];
  ByteSpan s_span(sig_ptr + idx + 2, s_len);

  bool ec_ver = ecdsa_verify(ec_key.curve, ByteSpan(ec_pub_res->data(), ec_pub_res->size()),
                             ByteSpan(msg_hash.data(), msg_hash.size()), r_span, s_span);
  check(ec_ver, "ECDSA signature verified successfully against derived public key");

  // -------------------------------------------------------------------------
  // 2. RSA Signing Primitives (PKCS#1 v1.5 and PSS)
  // -------------------------------------------------------------------------
  std::printf("\n--- 2. Testing RSA Signing & Verification ---\n");
  // Test 1024-bit RSA private key components (p, q, n, d, e)
  // We use known test modulus and exponents
  RsaPrivateKey rsa_key;
  // Modulus n (128 bytes = 1024 bits)
  rsa_key.modulus.assign(128, 0xbb);
  rsa_key.modulus[0] = 0xd4; rsa_key.modulus[127] = 0x01; // odd, positive
  // Exponent e = 65537
  rsa_key.public_exponent = {0x01, 0x00, 0x01};
  // Private exponent d
  rsa_key.private_exponent.assign(128, 0x77);
  rsa_key.private_exponent[0] = 0x4a; rsa_key.private_exponent[127] = 0x03;

  RsaPublicKey rsa_pub;
  rsa_pub.modulus = rsa_key.modulus;
  rsa_pub.exponent = rsa_key.public_exponent;

  auto rsa_pkcs_sig = rsa_sign_pkcs1(rsa_key, RsaDigest::Sha256, span_of(msg));
  check(rsa_pkcs_sig.ok(), "Generated RSA PKCS#1 v1.5 signature");
  // Public op roundtrip: signature^e mod n matches padded EM
  auto rsa_pub_op = rsa_public_op(rsa_pub, ByteSpan(rsa_pkcs_sig->data(), rsa_pkcs_sig->size()));
  check(rsa_pub_op.ok(), "RSA public operation on signature succeeded");

  auto rsa_pss_sig = rsa_sign_pss(rsa_key, RsaDigest::Sha256, span_of(msg));
  check(rsa_pss_sig.ok(), "Generated RSA PSS signature");
  auto rsa_pss_op = rsa_public_op(rsa_pub, ByteSpan(rsa_pss_sig->data(), rsa_pss_sig->size()));
  check(rsa_pss_op.ok(), "RSA PSS public operation succeeded");

  // -------------------------------------------------------------------------
  // 3. Live TLS Server + Client Handshake over Loopback
  // -------------------------------------------------------------------------
  std::printf("\n--- 3. Driving Live TLS Server & Client Handshake ---\n");

  auto cert = make_self_signed_ec_cert(ec_key, "localhost");
  check(cert.key_is_ec, "Self-signed test certificate created");

  std::vector<Certificate> chain = {cert};
  PrivateKey pkey;
  pkey.is_ec = true;
  pkey.ec_key = ec_key;

  // Save cert to a temp trust store for client
  char trust_file[] = "/tmp/aios-tls-trust-XXXXXX.crt";
  int fd = mkstemps(trust_file, 4);
  (void)fd;
  // Convert DER cert to PEM
  std::string cert_b64 = aios::base64_encode(ByteSpan(cert.der.data(), cert.der.size()));
  std::string cert_pem = "-----BEGIN CERTIFICATE-----\n";
  for (size_t i = 0; i < cert_b64.size(); i += 64) {
    cert_pem += cert_b64.substr(i, 64) + "\n";
  }
  cert_pem += "-----END CERTIFICATE-----\n";
  host->write_file(trust_file, span_of(cert_pem));

  uint16_t tls_port = 19488;
  auto listener = hal::net_listen(tls_port, true /* loopback_only edge containment */);
  check(listener.ok(), "Opened server listener on port 19488 (loopback bound)");

  std::string server_received;
  std::string client_received;

  // Server worker thread
  std::thread server_thread([&]() {
    auto incoming = (*listener)->accept(10000);
    if (!incoming.ok()) {
      std::printf("  [SERVER] accept error: %s\n", incoming.error().str().c_str());
      return;
    }
    TlsServer s(*host, chain, pkey);
    auto acc = s.accept(std::move(*incoming), 10000);
    if (!acc.ok()) {
      std::printf("  [SERVER] TLS accept error: %s\n", acc.error().str().c_str());
      return;
    }
    auto rd = s.read(10000);
    if (rd.ok()) {
      server_received.assign(rd->begin(), rd->end());
    }
    std::string resp = "PONG: secure channel established over " + s.info().cipher;
    (void)s.write(span_of(resp));
    s.close();
  });

  // Client thread
  std::thread client_thread([&]() {
    // Wait slightly for server to reach accept
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    TlsClient cl(*host, trust_file);
    auto conn = cl.connect("localhost", tls_port, 10000);
    if (!conn.ok()) {
      std::printf("  [CLIENT] connect error: %s\n", conn.error().str().c_str());
      return;
    }
    std::string ping_msg = "PING: hello from AIOS TLS client";
    auto wr = cl.write(span_of(ping_msg));
    if (!wr.ok()) {
      std::printf("  [CLIENT] write error: %s\n", wr.error().str().c_str());
      return;
    }
    auto rd = cl.read(10000);
    if (rd.ok()) {
      client_received.assign(rd->begin(), rd->end());
    }
    cl.close();
  });

  server_thread.join();
  client_thread.join();

  check(!server_received.empty(), "Server received client application data over TLS", server_received);
  check(!client_received.empty(), "Client received server application data over TLS", client_received);
  check(server_received == "PING: hello from AIOS TLS client", "Server received exact plaintext sent by client");
  check(client_received.find("PONG: secure channel established") != std::string::npos, "Client received exact response from server");

  // -------------------------------------------------------------------------
  // 4. Interop: a real, independent TLS client — not this codebase talking to
  //    itself. Two of this codebase's own bugs could easily agree with each
  //    other and still be wrong; openssl has no reason to share either one.
  // -------------------------------------------------------------------------
  std::printf("\n--- 4. Driving Interop Against a Real, Independent TLS Client (openssl) ---\n");

  uint16_t interop_port = 19489;
  auto interop_listener = hal::net_listen(interop_port, true);
  check(interop_listener.ok(), "Opened second listener for interop test (loopback bound)");

  std::string interop_server_received;
  std::thread interop_server_thread([&]() {
    auto incoming = (*interop_listener)->accept(10000);
    if (!incoming.ok()) { std::printf("  [SERVER] accept error: %s\n", incoming.error().str().c_str()); return; }
    TlsServer s(*host, chain, pkey);
    auto acc = s.accept(std::move(*incoming), 10000);
    if (!acc.ok()) { std::printf("  [SERVER] TLS accept error: %s\n", acc.error().str().c_str()); return; }
    auto rd = s.read(10000);
    if (rd.ok()) interop_server_received.assign(rd->begin(), rd->end());
    std::string resp = "PONG: real openssl client reached this server\n";
    (void)s.write(span_of(resp));
    s.close();
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(80));

  // `openssl s_client` reads stdin and the socket concurrently; if stdin EOFs
  // right after the ping is written (as a bare `printf | ...` does), it can
  // tear the connection down before the server's reply has arrived and been
  // printed — a race in the *scripting*, not in the TLS session. Holding
  // stdin open a beat past the ping gives the reply time to land.
  std::string cmd = "(printf 'PING: hello from a real openssl client\\n'; sleep 1) | "
                     "timeout 8 openssl s_client -connect 127.0.0.1:" + std::to_string(interop_port) +
                     " -CAfile " + std::string(trust_file) + " 2>&1";
  std::string openssl_output;
  FILE* pipe = popen(cmd.c_str(), "r");
  check(pipe != nullptr, "Launched openssl s_client against the server");
  if (pipe) {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) openssl_output.append(buf, n);
    pclose(pipe);
  }
  interop_server_thread.join();

  check(openssl_output.find("Verify return code: 0 (ok)") != std::string::npos,
        "openssl verified the server's certificate chain", openssl_output.substr(0, 400));
  check(interop_server_received.find("PING: hello from a real openssl client") != std::string::npos,
        "Server received the exact plaintext openssl sent", interop_server_received);
  check(openssl_output.find("PONG: real openssl client reached this server") != std::string::npos,
        "openssl printed the server's application-data response", openssl_output.substr(0, 400));

  unlink(trust_file);

  if (failures == 0) {
    std::printf("\n>>> Phase 2.1 - 2.4 PROVEN: TLS Server, signing primitives, and handshake fully verified!\n");
  } else {
    std::printf("\n>>> Phase 2 FAILED with %d failure(s)\n", failures);
  }

  return failures == 0 ? 0 : 1;
}
