#pragma once
// The ssh-agent protocol (draft-miller-ssh-agent), client side: the keys a
// person has loaded into their agent sign for this machine's client without the
// private half ever leaving the agent. The agent is reached at $SSH_AUTH_SOCK
// (a Unix socket) or, on Windows, the OpenSSH agent's named pipe
// \\.\pipe\openssh-ssh-agent.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"

namespace aios::net {

struct SshAgentKey {
  ByteBuffer blob;        // the public key's wire blob
  std::string algorithm;  // its type, read from the blob (ssh-ed25519, ssh-rsa, ecdsa-sha2-nistp256, ...)
  std::string comment;
};

class SshAgent {
 public:
  // `where`: a socket path or pipe name; "" = $SSH_AUTH_SOCK (on Windows, the OpenSSH agent's pipe).
  // Status::NotFound when there is no agent to reach (unset variable, socket gone).
  static Result<std::unique_ptr<SshAgent>> connect(const std::string& where = {}, int timeout_ms = 5000);
  virtual ~SshAgent() = default;
  virtual Result<std::vector<SshAgentKey>> identities() = 0;
  // The signature over `data` as the agent returns it: the SSH signature blob (string algorithm, string sig).
  // `algorithm` picks the RSA hash: "rsa-sha2-256" or "rsa-sha2-512" set the agent's flags; others are ignored.
  virtual Result<ByteBuffer> sign(ByteSpan key_blob, ByteSpan data, const std::string& algorithm) = 0;
};

}  // namespace aios::net
