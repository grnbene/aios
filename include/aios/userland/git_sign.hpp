#pragma once
// SSH signatures on git objects, as git writes them with gpg.format=ssh: an "SSHSIG" signature (OpenSSH's
// PROTOCOL.sshsig, namespace "git", SHA-512) over the commit as it would be without its gpgsig header, armored
// and carried in that header. Stock git checks them (`git verify-commit` with gpg.ssh.allowedSignersFile), and so
// does anything holding the signer's public key line. Ed25519 keys only.
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aios/core/result.hpp"
#include "aios/net/ssh.hpp"

namespace aios::userland {

// "-----BEGIN SSH SIGNATURE-----" ... over `message` in namespace `ns`.
Result<std::string> sshsig_sign(const net::SshIdentity& id, std::string_view ns, std::string_view message);

struct SshSigCheck {
  bool ok = false;
  std::string key;           // the signer's public key line ("ssh-ed25519 AAAA...")
  std::string fingerprint;   // "SHA256:..." as OpenSSH prints it
  std::string problem;       // why not, when !ok
};
// Whether `armored` is a good signature over `message` in namespace `ns`, and by which key. Any key: whether that
// key may sign is the caller's question (allowed signers).
SshSigCheck sshsig_verify(std::string_view armored, std::string_view ns, std::string_view message);

// A public key line ("ssh-ed25519 AAAA... comment") to its canonical "type base64" form and fingerprint; nullopt
// when it is not one.
struct SshPublicKey { std::string key; std::string fingerprint; };
std::optional<SshPublicKey> ssh_public_key(std::string_view line);

// A commit or tag object's bytes split into what was signed and its signature ("" when unsigned): the gpgsig
// header and its continuation lines removed, as git does before checking.
struct SignedPayload { std::string payload; std::string signature; };
SignedPayload git_split_signature(std::string_view object);
// `payload` (an unsigned commit) with `signature` added as its gpgsig header, after its other headers.
std::string git_add_signature(std::string_view payload, std::string_view signature);

// gpg.ssh.allowedSignersFile: "principal[,principal...] [options] keytype base64key [comment]" lines. The
// principals a key may sign as (empty: none).
std::vector<std::string> ssh_allowed_principals(std::string_view allowed_signers, std::string_view key);

}  // namespace aios::userland
