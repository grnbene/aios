// known_hosts as OpenSSH's hostfile.c reads it and sshconnect.c decides with it.
// A line is [marker] names keytype base64 [comment]. The names are a comma list
// of patterns (* ? and ! negation, case-insensitive) or one |1|salt|hash entry,
// HMAC-SHA1 of the name keyed by the salt. A @revoked line refuses its key for
// every name it matches; an @cert-authority line names a CA and never matches a
// plain host key. Every line of every file is looked at: a revoked key anywhere
// is revoked, the key itself anywhere is known, and only then does a line for the
// same name with another key of the same type make it changed.
#include "aios/net/ssh_known_hosts.hpp"

#include <algorithm>
#include <cstring>

#include "aios/core/base64.hpp"
#include "aios/crypto/sha1.hpp"
#include "aios/hal/tty.hpp"
#include "aios/net/ssh.hpp"

namespace aios::net {
namespace {

std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return s;
}

bool glob_nocase(const std::string& p, size_t i, const std::string& s, size_t j) {
  while (i < p.size()) {
    if (p[i] == '*') {
      while (i < p.size() && p[i] == '*') ++i;
      if (i == p.size()) return true;
      for (size_t k = j; k <= s.size(); ++k)
        if (glob_nocase(p, i, s, k)) return true;
      return false;
    }
    if (j >= s.size()) return false;
    if (p[i] != '?' && std::tolower((unsigned char)p[i]) != std::tolower((unsigned char)s[j])) return false;
    ++i;
    ++j;
  }
  return j == s.size();
}

std::array<uint8_t, 20> hmac_sha1(ByteSpan key, ByteSpan msg) {
  uint8_t k[64];
  std::memset(k, 0, sizeof k);
  if (key.size() > 64) { auto h = crypto::sha1(key); std::memcpy(k, h.data(), h.size()); }
  else std::memcpy(k, key.data(), key.size());
  ByteBuffer in(64), out(64);
  for (int i = 0; i < 64; ++i) { in[(size_t)i] = k[i] ^ 0x36; out[(size_t)i] = k[i] ^ 0x5c; }
  in.insert(in.end(), msg.begin(), msg.end());
  auto ih = crypto::sha1(ByteSpan(in));
  out.insert(out.end(), ih.begin(), ih.end());
  return crypto::sha1(ByteSpan(out));
}

enum class Marker { None, Revoked, CertAuthority };

struct Line {
  Marker marker = Marker::None;
  std::string names;     // the host field as written
  std::string type;      // the key type the line names
  ByteBuffer blob;
  int number = 0;
};

// One line of a known_hosts file; false (with the reason) for one ssh would skip.
bool parse_line(const std::string& raw, Line& out, std::string& why) {
  std::string s = raw;
  while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t i = s.find_first_not_of(" \t");
  if (i == std::string::npos || s[i] == '#') { why.clear(); return false; }
  auto word = [&]() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    size_t b = i;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
    return s.substr(b, i - b);
  };
  std::string w = word();
  if (w == "@revoked") { out.marker = Marker::Revoked; w = word(); }
  else if (w == "@cert-authority") { out.marker = Marker::CertAuthority; w = word(); }
  else if (!w.empty() && w[0] == '@') { why = "unknown marker " + w; return false; }
  out.names = w;
  out.type = word();
  std::string b64 = word();
  if (out.names.empty() || out.type.empty() || b64.empty()) { why = "not a known_hosts line"; return false; }
  auto blob = base64_decode(b64);
  if (!blob.ok() || blob->empty()) { why = "the key is not base64"; return false; }
  if (ssh_blob_type(ByteSpan(*blob)) != out.type) { why = "the key's type is not the one the line names"; return false; }
  out.blob = std::move(*blob);
  return true;
}

// hostfile.c's match: a |1| hash compared with HMAC-SHA1(salt, name); else a pattern list where a negated match
// rules the line out and any positive match rules it in.
bool names_match(const std::string& names, const std::string& name) {
  if (names.rfind("|1|", 0) == 0) {
    size_t bar = names.find('|', 3);
    if (bar == std::string::npos) return false;
    auto salt = base64_decode(names.substr(3, bar - 3));
    auto want = base64_decode(names.substr(bar + 1));
    if (!salt.ok() || !want.ok() || salt->size() != 20 || want->size() != 20) return false;
    // ssh looks names up lowercased, and ssh-keygen -H lowercases before hashing
    const std::string n = lower(name);
    auto got = hmac_sha1(ByteSpan(*salt), ByteSpan((const uint8_t*)n.data(), n.size()));
    return std::equal(got.begin(), got.end(), want->begin());
  }
  const std::string n = lower(name);
  bool hit = false;
  size_t i = 0;
  while (i <= names.size()) {
    size_t j = names.find(',', i);
    if (j == std::string::npos) j = names.size();
    std::string p = names.substr(i, j - i);
    i = j + 1;
    if (p.empty()) continue;
    bool neg = p[0] == '!';
    if (neg) p.erase(p.begin());
    if (glob_nocase(p, 0, n, 0)) {
      if (neg) return false;
      hit = true;
    }
  }
  return hit;
}

// Every parseable line of `file`, with the reasons for the ones skipped.
std::vector<Line> read_lines(hal::Host& host, const std::string& file, std::vector<std::string>* warnings) {
  std::vector<Line> out;
  if (!host.exists(file)) return out;
  auto data = host.read_file(file);
  if (!data.ok()) {
    if (warnings) warnings->push_back(file + ": " + data.error().message);
    return out;
  }
  std::string text(data->begin(), data->end());
  int number = 0;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    ++number;
    Line l;
    std::string why;
    if (parse_line(text.substr(start, end - start), l, why)) {
      l.number = number;
      out.push_back(std::move(l));
    } else if (!why.empty() && warnings) {
      warnings->push_back(file + ":" + std::to_string(number) + ": " + why);
    }
    start = end + 1;
  }
  return out;
}

// OpenSSH's own name for a key's type in messages: ED25519, ECDSA, RSA.
std::string type_label(const std::string& type) {
  if (type == "ssh-ed25519") return "ED25519";
  if (type.rfind("ecdsa-sha2-", 0) == 0) return "ECDSA";
  if (type == "ssh-rsa") return "RSA";
  if (type.rfind("sk-ssh-ed25519", 0) == 0) return "ED25519-SK";
  if (type.rfind("sk-ecdsa", 0) == 0) return "ECDSA-SK";
  return type;
}

}  // namespace

std::string ssh_known_hosts_name(const std::string& host, uint16_t port) {
  if (port == 0 || port == 22) return host;
  return "[" + host + "]:" + std::to_string(port);
}

SshHostKeyCheck ssh_check_host_key(hal::Host& host, const std::vector<std::string>& files, const std::string& name,
                                   const std::string& algorithm, ByteSpan blob, std::vector<std::string>* warnings) {
  SshHostKeyCheck out;
  std::string changed_file, known_file;
  int changed_line = 0, known_line = 0;
  for (const auto& file : files) {
    for (const auto& l : read_lines(host, file, warnings)) {
      if (!names_match(l.names, name)) continue;
      const bool same_key = l.blob.size() == blob.size() && std::equal(l.blob.begin(), l.blob.end(), blob.begin());
      if (l.marker == Marker::Revoked) {
        if (same_key) {
          out.state = SshHostKeyState::Revoked;
          out.file = file;
          out.line = l.number;
          return out;
        }
        continue;
      }
      if (l.marker == Marker::CertAuthority) continue;   // signs certificates; never a host's own key
      if (std::find(out.known_algorithms.begin(), out.known_algorithms.end(), l.type) == out.known_algorithms.end())
        out.known_algorithms.push_back(l.type);
      if (l.type != algorithm) continue;
      if (same_key) {
        if (known_file.empty()) { known_file = file; known_line = l.number; }
      } else if (changed_file.empty()) {
        changed_file = file;
        changed_line = l.number;
      }
    }
  }
  if (!known_file.empty()) { out.state = SshHostKeyState::Known; out.file = known_file; out.line = known_line; }
  else if (!changed_file.empty()) { out.state = SshHostKeyState::Changed; out.file = changed_file; out.line = changed_line; }
  return out;
}

std::vector<std::string> ssh_known_host_algorithms(hal::Host& host, const std::vector<std::string>& files, const std::string& name) {
  std::vector<std::string> out;
  for (const auto& file : files)
    for (const auto& l : read_lines(host, file, nullptr))
      if (l.marker == Marker::None && names_match(l.names, name) && std::find(out.begin(), out.end(), l.type) == out.end())
        out.push_back(l.type);
  return out;
}

std::string ssh_hash_host_name(const std::string& name_in, ByteSpan salt) {
  const std::string name = lower(name_in);   // as ssh-keygen -H and ssh's own hashing take it
  auto mac = hmac_sha1(salt, ByteSpan((const uint8_t*)name.data(), name.size()));
  return "|1|" + base64_encode(salt) + "|" + base64_encode(ByteSpan(mac.data(), mac.size()));
}

Result<void> ssh_add_known_host(hal::Host& host, const std::string& file, const std::string& name,
                                const std::string& algorithm, ByteSpan blob, bool hashed) {
  size_t slash = file.rfind('/');
  if (slash != std::string::npos && slash > 0) {
    std::string dir = file.substr(0, slash);
    if (!host.exists(dir)) {
      if (auto r = host.make_dir(dir); !r.ok()) return Result<void>(r.error().status, dir + ": " + r.error().message);
      if (auto r = host.set_access(dir, hal::Host::FileAccess::Private); !r.ok()) return r;
    }
  }
  std::string names = name;
  if (hashed) {
    uint8_t salt[20];
    host.random(MutableByteSpan(salt, 20));
    names = ssh_hash_host_name(name, ByteSpan(salt, 20));
  }
  std::string line = names + " " + algorithm + " " + base64_encode(blob) + "\n";
  const bool fresh = !host.exists(file);
  if (!fresh) {
    // a file that does not end in a newline would run this line into its last one
    auto have = host.read_file(file);
    if (have.ok() && !have->empty() && have->back() != '\n') line = "\n" + line;
  }
  if (auto r = host.append_file(file, ByteSpan((const uint8_t*)line.data(), line.size())); !r.ok())
    return Result<void>(r.error().status, file + ": " + r.error().message);
  if (fresh) {
    if (auto r = host.set_access(file, hal::Host::FileAccess::Private); !r.ok()) return r;
  }
  return Ok();
}

Result<void> ssh_accept_host_key(hal::Host& host, const SshHostKeyPolicy& policy, const std::string& name,
                                 const std::string& shown, const std::string& algorithm, ByteSpan blob,
                                 std::vector<std::string>* notes) {
  std::vector<std::string> files = policy.user_files;
  files.insert(files.end(), policy.global_files.begin(), policy.global_files.end());
  std::vector<std::string> warnings;
  SshHostKeyCheck c = ssh_check_host_key(host, files, name, algorithm, blob, &warnings);
  const std::string fp = ssh_fingerprint(blob);
  const std::string label = type_label(algorithm);
  const std::string failed = "Host key verification failed.";
  switch (c.state) {
    case SshHostKeyState::Known:
      return Ok();
    case SshHostKeyState::Revoked:
      return Result<void>(Status::Denied,
                          "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
                          "@       WARNING: REVOKED HOST KEY DETECTED!               @\n"
                          "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
                          "The " + label + " host key for " + shown + " is marked as revoked (" + c.file + ":" +
                              std::to_string(c.line) + ").\nThis could mean that a stolen key is being used to impersonate this host.\n" + failed);
    case SshHostKeyState::Changed:
      return Result<void>(Status::Denied,
                          "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
                          "@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @\n"
                          "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
                          "IT IS POSSIBLE THAT SOMEONE IS DOING SOMETHING NASTY!\n"
                          "Someone could be eavesdropping on you right now (man-in-the-middle attack)!\n"
                          "It is also possible that a host key has just been changed.\n"
                          "The fingerprint for the " + label + " key sent by the remote host is\n" + fp + ".\n"
                          "Offending " + label + " key in " + c.file + ":" + std::to_string(c.line) + "\n" +
                          label + " host key for " + shown + " has changed and you have requested strict checking.\n" + failed);
    case SshHostKeyState::Unknown:
      break;
  }
  auto record = [&]() -> Result<void> {
    if (policy.user_files.empty()) {
      if (notes) notes->push_back("Failed to add the host to the list of known hosts (no UserKnownHostsFile).");
      return Ok();
    }
    if (auto r = ssh_add_known_host(host, policy.user_files.front(), name, algorithm, blob, policy.hash); !r.ok()) {
      if (notes) notes->push_back("Failed to add the host to the list of known hosts (" + r.error().message + ").");
      return Ok();
    }
    if (notes) notes->push_back("Warning: Permanently added '" + shown + "' (" + label + ") to the list of known hosts.");
    return Ok();
  };
  if (policy.strict == "accept-new" || policy.strict == "off" || policy.strict == "no") return record();
  if (policy.strict == "yes")
    return Result<void>(Status::Denied, "No " + label + " host key is known for " + shown +
                                            " and you have requested strict checking.\n" + failed);
  // ask
  if (policy.batch) return Result<void>(Status::Denied, "The authenticity of host '" + shown + "' can't be established, and BatchMode allows no question.\n" + failed);
  std::string question = "The authenticity of host '" + shown + "' can't be established.\n" + label + " key fingerprint is " + fp + ".\n";
  if (!c.known_algorithms.empty()) question += "This host key is known by other names or types; its " + label + " key is not.\n";
  question += "Are you sure you want to continue connecting (yes/no/[fingerprint])? ";
  for (;;) {
    auto answer = hal::tty_prompt(question, true);
    if (!answer.ok()) return Result<void>(Status::Denied, "No terminal to ask whether to trust " + shown + "'s " + label + " key " + fp + ".\n" + failed);
    std::string a = *answer;
    while (!a.empty() && (a.back() == ' ' || a.back() == '\r')) a.pop_back();
    while (!a.empty() && a.front() == ' ') a.erase(a.begin());
    if (lower(a) == "yes" || a == fp) return record();
    if (lower(a) == "no") return Result<void>(Status::Denied, failed);
    question = "Please type 'yes', 'no' or the fingerprint: ";
  }
}

}  // namespace aios::net
