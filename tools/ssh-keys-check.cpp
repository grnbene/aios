// SSH keys against OpenSSH's own ssh-keygen. Every key type it makes (ed25519,
// rsa, ecdsa 256/384/521), in every form it writes them (OpenSSH format plain
// and passphrase-encrypted under each cipher, legacy PEM plain and encrypted,
// PKCS#8 plain and encrypted), must parse here to the public key ssh-keygen -y
// prints and the fingerprint ssh-keygen -l prints; a wrong passphrase must be
// refused as one. Signatures go both ways through SSHSIG, ssh-keygen -Y's
// signature file: ssh-keygen's signatures verify with ssh_verify_signature,
// and ours are accepted by ssh-keygen -Y check-novalidate.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "aios/core/base64.hpp"
#include "aios/crypto/sha512.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/process.hpp"
#include "aios/net/ssh.hpp"

using namespace aios;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}

std::string read_text(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
void write_text(const std::string& path, const std::string& t) {
  std::ofstream f(path, std::ios::binary);
  f << t;
}
std::string first_line(const std::string& s) {
  std::string l = s.substr(0, s.find('\n'));
  while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
  return l;
}
// "type base64" without the comment
std::string key_part(const std::string& line) {
  size_t a = line.find(' ');
  if (a == std::string::npos) return line;
  size_t b = line.find(' ', a + 1);
  return b == std::string::npos ? line : line.substr(0, b);
}

hal::ToolResult run(const std::vector<std::string>& argv, const std::string& input_file = {}) {
  hal::ProcessSpawn spec;
  if (input_file.empty()) spec.argv = argv;
  else spec.argv = {"/bin/sh", "-c", "exec \"$0\" \"$@\" < '" + input_file + "'"}, spec.argv.insert(spec.argv.end(), argv.begin(), argv.end());
  auto r = hal::run_tool(spec, 120000);
  if (!r.ok()) return hal::ToolResult{127, r.error().message};
  return *r;
}

// SSHSIG (PROTOCOL.sshsig): the blob that is signed, and the armoured file around a signature.
ByteBuffer sshsig_signed_data(const std::string& ns, const std::string& hash_alg, ByteSpan message) {
  ByteBuffer out;
  auto str = [&](ByteSpan s) {
    uint32_t n = (uint32_t)s.size();
    out.push_back((uint8_t)(n >> 24)); out.push_back((uint8_t)(n >> 16)); out.push_back((uint8_t)(n >> 8)); out.push_back((uint8_t)n);
    out.insert(out.end(), s.begin(), s.end());
  };
  const char* magic = "SSHSIG";
  out.insert(out.end(), magic, magic + 6);
  str(ByteSpan((const uint8_t*)ns.data(), ns.size()));
  str(ByteSpan());
  str(ByteSpan((const uint8_t*)hash_alg.data(), hash_alg.size()));
  auto h = crypto::sha512(message);
  str(ByteSpan(h.data(), h.size()));
  return out;
}
struct SigReader {
  ByteSpan d;
  size_t p = 0;
  bool bad = false;
  uint32_t u32() {
    if (p + 4 > d.size()) { bad = true; return 0; }
    uint32_t v = ((uint32_t)d[p] << 24) | ((uint32_t)d[p + 1] << 16) | ((uint32_t)d[p + 2] << 8) | d[p + 3];
    p += 4;
    return v;
  }
  ByteSpan str() {
    uint32_t n = u32();
    if (bad || p + n > d.size()) { bad = true; return {}; }
    ByteSpan s = d.subspan(p, n);
    p += n;
    return s;
  }
};
ByteBuffer armour_body(const std::string& text) {
  std::string body;
  size_t b = text.find("-----BEGIN SSH SIGNATURE-----");
  size_t e = text.find("-----END SSH SIGNATURE-----");
  if (b == std::string::npos || e == std::string::npos) return {};
  for (size_t i = text.find('\n', b) + 1; i < e; ++i)
    if (text[i] != '\n' && text[i] != '\r') body += text[i];
  auto d = base64_decode(body);
  return d.ok() ? *d : ByteBuffer();
}
std::string sshsig_file(ByteSpan pub, const std::string& ns, ByteSpan signature) {
  ByteBuffer out;
  auto u32 = [&](uint32_t n) { out.push_back((uint8_t)(n >> 24)); out.push_back((uint8_t)(n >> 16)); out.push_back((uint8_t)(n >> 8)); out.push_back((uint8_t)n); };
  auto str = [&](ByteSpan s) { u32((uint32_t)s.size()); out.insert(out.end(), s.begin(), s.end()); };
  const char* magic = "SSHSIG";
  out.insert(out.end(), magic, magic + 6);
  u32(1);
  str(pub);
  str(ByteSpan((const uint8_t*)ns.data(), ns.size()));
  str(ByteSpan());
  std::string ha = "sha512";
  str(ByteSpan((const uint8_t*)ha.data(), ha.size()));
  str(signature);
  std::string b64 = base64_encode(ByteSpan(out.data(), out.size()));
  std::string text = "-----BEGIN SSH SIGNATURE-----\n";
  for (size_t i = 0; i < b64.size(); i += 70) text += b64.substr(i, 70) + "\n";
  return text + "-----END SSH SIGNATURE-----\n";
}

struct KeyCase {
  std::string type;     // ssh-keygen -t
  std::string bits;     // -b, or ""
  std::string format;   // "" (OpenSSH), "PEM", "PKCS8"
  std::string cipher;   // -Z for the OpenSSH format, or ""
  bool encrypted;
};

}  // namespace

int main() {
  auto base = hal::scratch_path("");
  if (!base.ok()) { std::printf("FAIL  no scratch directory\n"); return 1; }
  const std::string tmp = *base + ".d";
  hal::process_host().make_dir(tmp);
  const std::string pass = "correct horse battery staple";

  std::vector<KeyCase> cases;
  const std::vector<std::pair<std::string, std::string>> types = {
      {"ed25519", ""}, {"rsa", "3072"}, {"ecdsa", "256"}, {"ecdsa", "384"}, {"ecdsa", "521"}};
  for (auto& [t, b] : types) {
    cases.push_back({t, b, "", "", false});
    for (const char* z : {"aes256-ctr", "aes128-ctr", "aes256-cbc", "aes256-gcm@openssh.com", "chacha20-poly1305@openssh.com"})
      cases.push_back({t, b, "", z, true});
    if (t != "ed25519") {   // ssh-keygen writes ed25519 only in its own format
      cases.push_back({t, b, "PEM", "", false});
      cases.push_back({t, b, "PEM", "", true});
    }
    cases.push_back({t, b, "PKCS8", "", false});
    cases.push_back({t, b, "PKCS8", "", true});
  }

  int n = 0;
  for (const auto& kc : cases) {
    std::string tag = kc.type + (kc.bits.empty() ? "" : "-" + kc.bits) + (kc.format.empty() ? " openssh" : " " + kc.format) +
                      (kc.encrypted ? " encrypted" + (kc.cipher.empty() ? std::string() : " " + kc.cipher) : " plain");
    std::string key = tmp + "/k" + std::to_string(n++);
    std::vector<std::string> gen = {"ssh-keygen", "-q", "-t", kc.type, "-N", kc.encrypted ? pass : "", "-C", "check " + tag, "-f", key};
    if (!kc.bits.empty()) { gen.push_back("-b"); gen.push_back(kc.bits); }
    if (!kc.format.empty()) { gen.push_back("-m"); gen.push_back(kc.format); }
    if (!kc.cipher.empty()) { gen.push_back("-Z"); gen.push_back(kc.cipher); }
    auto g = run(gen);
    if (g.exit_code != 0) {
      // a format this ssh-keygen does not write (an older OpenSSH, a LibreSSL build) is skipped, not failed
      std::printf("skip  %s: ssh-keygen cannot write it here: %s\n", tag.c_str(), first_line(g.output).c_str());
      continue;
    }
    std::string pem = read_text(key);
    ByteSpan pem_span((const uint8_t*)pem.data(), pem.size());
    check(net::ssh_private_key_encrypted(pem_span) == kc.encrypted, tag + ": seen as " + (kc.encrypted ? "encrypted" : "plain"));
    auto id = net::ssh_parse_private_key(pem_span, kc.encrypted ? pass : "");
    check(id.ok(), tag + ": parses" + (id.ok() ? std::string() : ": " + id.error().message));
    if (!id.ok()) continue;
    if (kc.encrypted) {
      auto wrong = net::ssh_parse_private_key(pem_span, "not the passphrase");
      check(!wrong.ok() && wrong.error().status == Status::Denied, tag + ": a wrong passphrase is refused as one");
      auto none = net::ssh_parse_private_key(pem_span, "");
      check(!none.ok() && none.error().status == Status::Denied, tag + ": no passphrase is refused as one");
    }
    auto y = run({"ssh-keygen", "-y", "-P", kc.encrypted ? pass : "", "-f", key});
    check(y.exit_code == 0 && key_part(id->public_line()) == key_part(first_line(y.output)),
          tag + ": public key equals ssh-keygen -y");
    auto l = run({"ssh-keygen", "-l", "-E", "sha256", "-f", key + ".pub"});
    check(l.exit_code == 0 && l.output.find(id->fingerprint()) != std::string::npos, tag + ": fingerprint equals ssh-keygen -l (" + id->fingerprint() + ")");
    auto parsed_pub = net::ssh_parse_public_line(read_text(key + ".pub"));
    check(parsed_pub.ok() && parsed_pub->second == id->public_blob() && net::ssh_blob_type(ByteSpan(parsed_pub->second)) == id->type,
          tag + ": the .pub line parses to the same key");

    // ssh-keygen signs, we verify
    std::string msg_path = key + ".msg";
    std::string message = "converge sshsig " + tag;
    write_text(msg_path, message);
    auto s = run({"ssh-keygen", "-Y", "sign", "-n", "chk", "-P", kc.encrypted ? pass : "", "-f", key, msg_path});
    std::string sig_text = read_text(msg_path + ".sig");
    ByteBuffer blob = armour_body(sig_text);
    // "SSHSIG" version publickey namespace reserved hash_algorithm signature
    SigReader r{ByteSpan(blob)};
    r.p = 6;
    r.u32();
    ByteSpan sig_pub = r.str();
    r.str();
    r.str();
    ByteSpan ha = r.str();
    ByteSpan their_sig = r.str();
    std::string hash_alg((const char*)ha.data(), ha.size());
    ByteSpan msg_span((const uint8_t*)message.data(), message.size());
    bool verified = false;
    if (s.exit_code == 0 && !r.bad && hash_alg == "sha512") {
      ByteBuffer signed_data = sshsig_signed_data("chk", hash_alg, msg_span);
      verified = net::ssh_verify_signature(sig_pub, their_sig, ByteSpan(signed_data));
      ByteBuffer other = sshsig_signed_data("chk", hash_alg, ByteSpan((const uint8_t*)"tampered", 8));
      check(!net::ssh_verify_signature(sig_pub, their_sig, ByteSpan(other)), tag + ": their signature fails over other data");
    }
    check(verified, tag + ": ssh-keygen's SSHSIG signature verifies here");

    // we sign, ssh-keygen verifies
    auto signer = net::ssh_identity_signer(*id, key);
    std::string alg = id->type == "ssh-rsa" ? "rsa-sha2-512" : id->type;
    ByteBuffer to_sign = sshsig_signed_data("chk", "sha512", msg_span);
    auto ours = signer->sign(ByteSpan(to_sign), alg);
    check(ours.ok() && net::ssh_verify_signature(ByteSpan(id->public_blob()), ByteSpan(*ours), ByteSpan(to_sign)), tag + ": our signature verifies here");
    if (ours.ok()) {
      ByteBuffer pub = id->public_blob();
      write_text(key + ".ours.sig", sshsig_file(ByteSpan(pub), "chk", ByteSpan(*ours)));
      auto v = run({"ssh-keygen", "-Y", "check-novalidate", "-n", "chk", "-s", key + ".ours.sig"}, msg_path);
      check(v.exit_code == 0, tag + ": ssh-keygen accepts our SSHSIG signature" + (v.exit_code ? ": " + first_line(v.output) : std::string()));
    }

    // a key file signer: the public half without the passphrase, the private half only when signing
    auto fs = net::ssh_file_signer(hal::process_host(), key, true);
    check(fs.ok() && (*fs)->public_blob() == id->public_blob(), tag + ": the file signer knows the public key without unlocking");
    if (fs.ok() && kc.encrypted) {
      auto denied = (*fs)->sign(ByteSpan(to_sign), alg);
      check(!denied.ok() && denied.error().status == Status::Denied, tag + ": in batch mode an encrypted key is not unlocked");
    }
  }
  std::printf("%s\n", failures ? "FAILED" : "all ssh key checks pass");
  return failures ? 1 : 0;
}
