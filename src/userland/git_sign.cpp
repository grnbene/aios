#include "aios/userland/git_sign.hpp"

#include <cstring>

#include "aios/core/base64.hpp"
#include "aios/crypto/curve25519.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/crypto/sha512.hpp"

namespace aios::userland {

namespace {
const char* kBegin = "-----BEGIN SSH SIGNATURE-----";
const char* kEnd = "-----END SSH SIGNATURE-----";

ByteSpan span(std::string_view s) { return ByteSpan((const uint8_t*)s.data(), s.size()); }

struct Writer {
  ByteBuffer b;
  void raw(std::string_view s) { b.insert(b.end(), s.begin(), s.end()); }
  void raw(ByteSpan s) { b.insert(b.end(), s.begin(), s.end()); }
  void u32(uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back((uint8_t)(v >> (i * 8))); }
  void str(std::string_view s) { u32((uint32_t)s.size()); raw(s); }
  void str(ByteSpan s) { u32((uint32_t)s.size()); raw(s); }
};

struct Reader {
  ByteSpan d;
  size_t at = 0;
  bool bad = false;
  bool raw(size_t n, std::string& out) {
    if (bad || d.size() - at < n) { bad = true; return false; }
    out.assign((const char*)d.data() + at, n);
    at += n;
    return true;
  }
  uint32_t u32() {
    if (bad || d.size() - at < 4) { bad = true; return 0; }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8) | d[at + i];
    at += 4;
    return v;
  }
  std::string str() { std::string s; uint32_t n = u32(); raw(n, s); return s; }
};

std::string fingerprint_of(std::string_view blob) {
  auto h = crypto::sha256(span(blob));
  std::string b = base64_encode(ByteSpan(h.data(), h.size()));
  while (!b.empty() && b.back() == '=') b.pop_back();
  return "SHA256:" + b;
}

std::string key_line_of(std::string_view blob) {
  return "ssh-ed25519 " + base64_encode(span(blob));
}

// What the signature covers: the preamble, namespace, reserved, hash algorithm and the message's hash.
ByteBuffer signed_data(std::string_view ns, std::string_view reserved, std::string_view hash, std::string_view message) {
  Writer w;
  w.raw("SSHSIG");
  w.str(ns);
  w.str(reserved);
  w.str(hash);
  if (hash == "sha256") {
    auto h = crypto::sha256(span(message));
    w.str(ByteSpan(h.data(), h.size()));
  } else {
    auto h = crypto::sha512(span(message));
    w.str(ByteSpan(h.data(), h.size()));
  }
  return w.b;
}
}  // namespace

Result<std::string> sshsig_sign(const net::SshIdentity& id, std::string_view ns, std::string_view message) {
  if (!id.valid || id.type != "ssh-ed25519")
    return Result<std::string>(Status::Invalid, "SSH signatures here are made with Ed25519 keys only");
  auto data = signed_data(ns, "", "sha512", message);
  auto sig = crypto::ed25519_sign(ByteSpan(data.data(), data.size()), id.seed, id.pub);
  Writer sigblob;
  sigblob.str(std::string_view("ssh-ed25519"));
  sigblob.str(ByteSpan(sig.data(), sig.size()));
  auto pub = id.public_blob();
  Writer w;
  w.raw("SSHSIG");
  w.u32(1);
  w.str(ByteSpan(pub.data(), pub.size()));
  w.str(ns);
  w.str(std::string_view());
  w.str(std::string_view("sha512"));
  w.str(ByteSpan(sigblob.b.data(), sigblob.b.size()));
  std::string b64 = base64_encode(ByteSpan(w.b.data(), w.b.size()));
  std::string out = std::string(kBegin) + "\n";
  for (size_t i = 0; i < b64.size(); i += 70) out += b64.substr(i, 70) + "\n";
  out += std::string(kEnd) + "\n";
  return out;
}

SshSigCheck sshsig_verify(std::string_view armored, std::string_view ns, std::string_view message) {
  SshSigCheck r;
  size_t b = armored.find(kBegin), e = armored.find(kEnd);
  if (b == std::string_view::npos || e == std::string_view::npos || e < b) { r.problem = "not an SSH signature"; return r; }
  std::string b64;
  for (char ch : armored.substr(b + std::strlen(kBegin), e - b - std::strlen(kBegin)))
    if (ch != '\n' && ch != '\r' && ch != ' ' && ch != '\t') b64 += ch;
  auto blob = base64_decode(b64);
  if (!blob.ok()) { r.problem = "the signature's armor is not base64"; return r; }
  Reader rd{ByteSpan(blob->data(), blob->size())};
  std::string magic;
  rd.raw(6, magic);
  uint32_t version = rd.u32();
  std::string pub = rd.str(), sig_ns = rd.str(), reserved = rd.str(), hash = rd.str(), sigblob = rd.str();
  if (rd.bad || magic != "SSHSIG") { r.problem = "malformed SSH signature"; return r; }
  if (version != 1) { r.problem = "SSH signature version " + std::to_string(version) + " (this reads 1)"; return r; }
  if (sig_ns != ns) { r.problem = "signature is for namespace \"" + sig_ns + "\", not \"" + std::string(ns) + "\""; return r; }
  Reader pk{span(pub)};
  std::string ktype = pk.str(), kbytes = pk.str();
  if (pk.bad || ktype != "ssh-ed25519" || kbytes.size() != 32) { r.problem = "signed with a " + ktype + " key (this checks ssh-ed25519)"; return r; }
  r.key = key_line_of(pub);
  r.fingerprint = fingerprint_of(pub);
  Reader sg{span(sigblob)};
  std::string stype = sg.str(), sbytes = sg.str();
  if (sg.bad || stype != "ssh-ed25519" || sbytes.size() != 64) { r.problem = "malformed ssh-ed25519 signature"; return r; }
  if (hash != "sha512" && hash != "sha256") { r.problem = "unknown hash algorithm " + hash; return r; }
  ByteBuffer data = signed_data(ns, reserved, hash, message);
  crypto::Sig64 sig;
  crypto::Point32 key;
  std::memcpy(sig.data(), sbytes.data(), 64);
  std::memcpy(key.data(), kbytes.data(), 32);
  if (!crypto::ed25519_verify(ByteSpan(data.data(), data.size()), sig, key)) { r.problem = "bad signature"; return r; }
  r.ok = true;
  return r;
}

std::optional<SshPublicKey> ssh_public_key(std::string_view line) {
  size_t sp = line.find(' ');
  if (sp == std::string_view::npos || line.substr(0, sp) != "ssh-ed25519") return std::nullopt;
  std::string_view rest = line.substr(sp + 1);
  size_t end = rest.find_first_of(" \t\r\n");
  std::string b64(rest.substr(0, end));
  auto blob = base64_decode(b64);
  if (!blob.ok()) return std::nullopt;
  std::string bs((const char*)blob->data(), blob->size());
  Reader rd{span(bs)};
  std::string t = rd.str(), k = rd.str();
  if (rd.bad || t != "ssh-ed25519" || k.size() != 32 || rd.at != bs.size()) return std::nullopt;
  return SshPublicKey{key_line_of(bs), fingerprint_of(bs)};
}

SignedPayload git_split_signature(std::string_view object) {
  SignedPayload out;
  size_t eoh = object.find("\n\n");
  std::string_view headers = eoh == std::string_view::npos ? object : object.substr(0, eoh + 1);
  size_t i = 0;
  bool in_sig = false;
  while (i < headers.size()) {
    size_t nl = headers.find('\n', i);
    size_t len = (nl == std::string_view::npos ? headers.size() : nl + 1) - i;
    std::string_view line = headers.substr(i, len);
    if (in_sig && !line.empty() && line[0] == ' ') {
      out.signature += line.substr(1);
    } else if (line.substr(0, 7) == "gpgsig ") {
      in_sig = true;
      out.signature += line.substr(7);
    } else {
      in_sig = false;
      out.payload += line;
    }
    i += len;
  }
  if (eoh != std::string_view::npos) out.payload += object.substr(eoh + 1);
  return out;
}

std::string git_add_signature(std::string_view payload, std::string_view signature) {
  size_t eoh = payload.find("\n\n");
  size_t at = eoh == std::string_view::npos ? payload.size() : eoh + 1;
  std::string sig(signature);
  while (!sig.empty() && sig.back() == '\n') sig.pop_back();
  std::string header = "gpgsig ";
  for (char ch : sig) { header += ch; if (ch == '\n') header += ' '; }
  header += "\n";
  std::string out(payload.substr(0, at));
  out += header;
  out += payload.substr(at);
  return out;
}

std::vector<std::string> ssh_allowed_principals(std::string_view allowed, std::string_view key) {
  auto want = ssh_public_key(key);
  std::vector<std::string> out;
  if (!want) return out;
  size_t i = 0;
  while (i < allowed.size()) {
    size_t nl = allowed.find('\n', i);
    std::string line(allowed.substr(i, nl == std::string_view::npos ? std::string_view::npos : nl - i));
    i = nl == std::string_view::npos ? allowed.size() : nl + 1;
    size_t s = line.find_first_not_of(" \t");
    if (s == std::string::npos || line[s] == '#') continue;
    line = line.substr(s);
    size_t at = line.find("ssh-ed25519 ");
    if (at == std::string::npos) continue;
    auto k = ssh_public_key(std::string_view(line).substr(at));
    if (!k || k->key != want->key) continue;
    std::string principals = line.substr(0, line.find_first_of(" \t"));
    size_t p = 0;
    while (p <= principals.size()) {
      size_t comma = principals.find(',', p);
      std::string one = principals.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
      if (!one.empty()) out.push_back(one);
      if (comma == std::string::npos) break;
      p = comma + 1;
    }
  }
  return out;
}

}  // namespace aios::userland
