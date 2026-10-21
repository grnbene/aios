#pragma once
// known_hosts, as OpenSSH writes and reads it (sshd(8) "SSH_KNOWN_HOSTS FILE
// FORMAT"): plain host lists ("name,name2"), "[name]:port" for a port other than
// 22, hashed entries ("|1|<salt b64>|<HMAC-SHA1(salt, name) b64>"), wildcard and
// negated patterns, and the @cert-authority and @revoked markers. Several files
// are consulted (the user's, then the system's); a key for a name may appear on
// more than one line, and any one of them matching is a match.
#include <cstdint>
#include <string>
#include <vector>

#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"
#include "aios/hal/host.hpp"

namespace aios::net {

enum class SshHostKeyState {
  Known,      // a line for this name holds exactly this key
  Changed,    // lines for this name hold other keys of the same algorithm, and none holds this one
  Unknown,    // no line names this host with a key of this algorithm
  Revoked,    // an @revoked line holds this key
};

struct SshHostKeyCheck {
  SshHostKeyState state = SshHostKeyState::Unknown;
  std::string file;     // where the deciding line is (Known, Changed, Revoked)
  int line = 0;         // 1-based
  // Algorithms this host is already known under, in the order the files name them: the client offers these
  // first in key exchange, as OpenSSH does, so a host known by its ed25519 key is not met again by its RSA key.
  std::vector<std::string> known_algorithms;
};

// The name a host is recorded under: "host" for port 22, "[host]:port" otherwise.
std::string ssh_known_hosts_name(const std::string& host, uint16_t port);

// `algorithm` is the key's type as it appears in a known_hosts line (ssh-ed25519, ssh-rsa,
// ecdsa-sha2-nistp256, ...), `blob` the key's wire blob. A file that does not exist is skipped; one that
// cannot be read, or a line that cannot be parsed, is skipped with the reason added to `warnings`
// (OpenSSH carries on past a bad line, and so does this).
SshHostKeyCheck ssh_check_host_key(hal::Host& host, const std::vector<std::string>& files, const std::string& name,
                                   const std::string& algorithm, ByteSpan blob, std::vector<std::string>* warnings = nullptr);

// Algorithms `name` is known under across `files` (for ordering the key exchange before any key is seen).
std::vector<std::string> ssh_known_host_algorithms(hal::Host& host, const std::vector<std::string>& files,
                                                   const std::string& name);

// Appends a line for `name` to `file`, creating it (and its directory, mode 0700) with mode 0600 if missing.
// `hashed`: the name is written as a |1| hash with a fresh salt (HashKnownHosts yes).
Result<void> ssh_add_known_host(hal::Host& host, const std::string& file, const std::string& name,
                                const std::string& algorithm, ByteSpan blob, bool hashed);

// "|1|salt|hash" for `name` with the given 20-byte salt (exposed so a check can compare with ssh-keygen -H).
std::string ssh_hash_host_name(const std::string& name, ByteSpan salt);

struct SshHostKeyPolicy {
  std::vector<std::string> user_files;     // a newly accepted key is written to the first
  std::vector<std::string> global_files;
  std::string strict = "ask";              // StrictHostKeyChecking: yes | no | ask | accept-new | off
  bool hash = false;                       // HashKnownHosts
  bool batch = false;                      // BatchMode: nobody to ask, so "ask" refuses an unknown host
};

// What ssh does when a server has proved it holds `blob`: Known → Ok. Revoked → refused. Changed → refused with
// OpenSSH's "REMOTE HOST IDENTIFICATION HAS CHANGED" warning naming the file and line, under every setting
// (as ssh refuses a changed key for publickey sessions). Unknown → yes refuses; accept-new, no and off record it
// in the first user file and go on; ask puts OpenSSH's question to the terminal (hal::tty_prompt): "yes" or the
// key's SHA256 fingerprint records it, anything else (or no terminal, or batch) refuses.
// `name` is what the host is recorded under (ssh_known_hosts_name, or the HostKeyAlias); `shown` is the name
// used in messages. Messages that ssh prints as warnings go to `notes`.
Result<void> ssh_accept_host_key(hal::Host& host, const SshHostKeyPolicy& policy, const std::string& name,
                                 const std::string& shown, const std::string& algorithm, ByteSpan blob,
                                 std::vector<std::string>* notes = nullptr);

}  // namespace aios::net
