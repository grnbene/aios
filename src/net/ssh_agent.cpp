// The ssh-agent client (draft-miller-ssh-agent): the keys a person has loaded
// into their agent sign for this machine's client without the private half ever
// leaving the agent. The agent is a Unix socket at $SSH_AUTH_SOCK or, on
// Windows, the OpenSSH agent's named pipe.
//
// Every message is length-framed, both the ones this file writes and the ones
// it reads: a reply is one 4-byte length, one type byte and its fields, each
// field framed again. Nothing here signs anything itself — it asks.
#include "aios/net/ssh_agent.hpp"

#include <cstdlib>
#include <string>
#include <memory>
#include <utility>
#include <vector>

#include <algorithm>

#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"
#include "aios/hal/net.hpp"
#include "aios/net/ssh.hpp"   // SshSigner, so ssh_agent_signer can hand one back

namespace aios::net {
namespace {

// draft-miller-ssh-agent message numbers.
enum : uint8_t {
  AGENT_FAILURE = 5,
  AGENT_SUCCESS = 6,
  AGENTC_REQUEST_IDENTITIES = 11,
  AGENT_IDENTITIES_ANSWER = 12,
  AGENTC_SIGN_REQUEST = 13,
  AGENT_SIGN_RESPONSE = 14,
};
// The sign request's flags: which hash an RSA key signs with.
enum : uint32_t {
  AGENT_RSA_SHA2_256 = 2,
  AGENT_RSA_SHA2_512 = 4,
  };
// draft-miller-ssh-agent's limit on one message, either way.
constexpr uint32_t AGENT_FRAME_MAX = 256 * 1024;

// The algorithm an agent signature blob names for `key` when the caller has not
// asked for one: an RSA key has to be told which hash, and sha2-512 is the default.
std::string default_agent_algorithm(const SshAgentKey& key) {
  if (key.algorithm == "ssh-rsa") return "rsa-sha2-512";
  return key.algorithm;
}

// The agent's own encoding: SSH wire strings and integers, length-framed
// either way (the outer frame is a message, the inner ones are fields).
struct AgentBuf {
  ByteBuffer b;
  void u32(uint32_t v) {
    b.push_back((uint8_t)(v >> 24)); b.push_back((uint8_t)(v >> 16));
    b.push_back((uint8_t)(v >> 8));  b.push_back((uint8_t)v);
  }
  void str(ByteSpan s) {
    u32((uint32_t)s.size());
    b.insert(b.end(), s.begin(), s.end());
  }
  void text(const std::string& s) { str(span_of(s)); }
};

struct AgentCursor {
  ByteSpan d;
  size_t pos = 0;
  bool bad = false;
  uint32_t u32() {
    if (bad || pos + 4 > d.size()) { bad = true; return 0; }
    uint32_t v = ((uint32_t)d[pos] << 24) | ((uint32_t)d[pos + 1] << 16) | ((uint32_t)d[pos + 2] << 8) |
                 (uint32_t)d[pos + 3];
    pos += 4;
    return v;
  }
  ByteSpan str() {
    uint32_t n = u32();
    if (bad || pos + n > d.size()) { bad = true; return {}; }
    ByteSpan s = d.subspan(pos, n);
    pos += n;
    return s;
  }
};

// The socket or pipe the agent answers on, and the framing every message wears.
class AgentConnection {
 public:
  explicit AgentConnection(std::unique_ptr<hal::Connection> c)
      : conn_(std::move(c)), where_(conn_->peer()) {}

  const std::string& where() const { return where_; }

  Result<void> send(uint8_t type, ByteSpan body) {
    AgentBuf f;
    f.u32((uint32_t)(1 + body.size()));
    f.b.push_back(type);
    f.b.insert(f.b.end(), body.begin(), body.end());
    if (!conn_->send_all(ByteSpan(f.b.data(), f.b.size())))
      return Result<void>(Status::IoError, where_ + ": the agent closed the connection while writing a message");
    return Result<void>(Status::Ok);
  }

  // One reply, read in full; its length is the outermost frame and every field
  // inside it is framed again.
  Result<ByteBuffer> read_message(int timeout_ms) {
    uint8_t hdr[4];
    if (!conn_->recv_all(MutableByteSpan(hdr, 4), timeout_ms))
      return Result<ByteBuffer>(Status::IoError, where_ + ": no reply from the agent (closed or timed out)");
    uint32_t len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
    if (len == 0) return Result<ByteBuffer>(Status::Corrupt, where_ + ": the agent sent an empty message");
    if (len > AGENT_FRAME_MAX)
      return Result<ByteBuffer>(Status::Corrupt, where_ + ": the agent's reply of " + std::to_string(len) +
                                                     " bytes is over the 256 KiB the protocol allows");
    ByteBuffer msg(len);
    if (!conn_->recv_all(MutableByteSpan(msg.data(), msg.size()), timeout_ms))
      return Result<ByteBuffer>(Status::IoError, where_ + ": the agent's reply was cut short before " +
                                                   std::to_string(len) + " bytes");
    return msg;
  }

 private:
  std::unique_ptr<hal::Connection> conn_;
  std::string where_;
};

Result<std::string> env_value(const char* name) {
  const char* v = std::getenv(name);
  if (v == nullptr || *v == '\0')
    return Result<std::string>(Status::NotFound, std::string("the environment does not set ") + name);
  return std::string(v);
}

class Agent final : public SshAgent {
 public:
  explicit Agent(std::unique_ptr<AgentConnection> c) : c_(std::move(c)) {}

  Result<std::vector<SshAgentKey>> identities() override {
    AIOS_TRY(c_->send(AGENTC_REQUEST_IDENTITIES, {}));
    auto msg = c_->read_message(5000);
    if (!msg.ok()) return msg.error();
    ByteBuffer& m = *msg;
    if (m[0] == AGENT_FAILURE)
      return Result<std::vector<SshAgentKey>>(Status::IoError, c_->where() + ": the agent refused the identity request");
    if (m[0] != AGENT_IDENTITIES_ANSWER)
      return Result<std::vector<SshAgentKey>>(Status::Corrupt, c_->where() + ": the agent answered " +
                                                                           std::to_string(m[0]) +
                                                                           " to an identity request, not 12");
    AgentCursor p{ByteSpan(m.data() + 1, m.size() - 1)};
    uint32_t n = p.u32();
    std::vector<SshAgentKey> out;
    out.reserve(n < 64 ? n : 64);
    for (uint32_t i = 0; i < n; ++i) {
      ByteSpan blob = p.str();
      std::string comment = string_of(p.str());
      if (p.bad)
        return Result<std::vector<SshAgentKey>>(Status::Corrupt, c_->where() + ": identity " + std::to_string(i) +
                                                                    " of " + std::to_string(n) +
                                                                    " is not length-framed as the protocol says");
      // The type is an SSH string at the head of the blob, as in authorized_keys.
      SshAgentKey k;
      k.blob.assign(blob.begin(), blob.end());
      k.comment = std::move(comment);
      AgentCursor q{blob};
      k.algorithm = string_of(q.str());
      if (q.bad)
        return Result<std::vector<SshAgentKey>>(Status::Corrupt, c_->where() + ": identity " + std::to_string(i) +
                                                                    "'s key blob does not open with its type");
      out.push_back(std::move(k));
    }
    return out;
  }

  Result<ByteBuffer> sign(ByteSpan key_blob, ByteSpan data, const std::string& algorithm) override {
    AgentBuf body;
    body.str(key_blob);
    body.str(data);
    uint32_t flags = 0;
    if (algorithm == "rsa-sha2-256") flags = AGENT_RSA_SHA2_256;
    else if (algorithm == "rsa-sha2-512") flags = AGENT_RSA_SHA2_512;
    body.u32(flags);
    AIOS_TRY(c_->send(AGENTC_SIGN_REQUEST, ByteSpan(body.b.data(), body.b.size())));
    auto msg = c_->read_message(30000);
    if (!msg.ok()) return msg.error();
    ByteBuffer& m = *msg;
    if (m[0] == AGENT_FAILURE)
      return Result<ByteBuffer>(Status::NotFound, c_->where() + ": the agent holds no key with this blob");
    if (m[0] != AGENT_SIGN_RESPONSE)
      return Result<ByteBuffer>(Status::Corrupt, c_->where() + ": the agent answered " + std::to_string(m[0]) +
                                                       " to a sign request, not 14");
    AgentCursor p{ByteSpan(m.data() + 1, m.size() - 1)};
    ByteSpan sig = p.str();
    if (p.bad) return Result<ByteBuffer>(Status::Corrupt, c_->where() + ": the signature is not a framed string");
    return ByteBuffer(sig.begin(), sig.end());
  }

 private:
  std::unique_ptr<AgentConnection> c_;
};

// Signing through the agent: the private half never leaves it, so the signer
// carries only the public blob it signs with and the agent it shares.
class AgentSigner final : public SshSigner {
 public:
  AgentSigner(SshAgentKey key, std::shared_ptr<SshAgent> agent)
      : key_(std::move(key)), blob_(key_.blob), agent_(std::move(agent)) {}
  ByteBuffer public_blob() const override { return blob_; }
  Result<ByteBuffer> sign(ByteSpan data, const std::string& algorithm) override {
    if (!agent_) return Result<ByteBuffer>(Status::Invalid, "ssh_agent_signer: the agent is gone");
    return agent_->sign(ByteSpan(blob_.data(), blob_.size()), data,
                        algorithm.empty() ? default_agent_algorithm(key_) : algorithm);
  }
  std::string where() const { return "ssh-agent: " + key_.comment; }
  std::string key_type() const override { return key_.algorithm; }
  std::string describe() const override {
    return "agent: " + (key_.comment.empty() ? key_.algorithm : key_.comment);
  }

 private:
  SshAgentKey key_;   // the algorithm and comment describe the key the agent holds
  ByteBuffer blob_;
  std::shared_ptr<SshAgent> agent_;
};

}  // namespace

Result<std::unique_ptr<SshAgent>> SshAgent::connect(const std::string& where, int timeout_ms) {
  Result<std::unique_ptr<hal::Connection>> conn = [&]() -> Result<std::unique_ptr<hal::Connection>> {
    if (!where.empty()) {
#if defined(_WIN32)
      return hal::net_connect_pipe(where, timeout_ms);
#else
      return hal::net_connect_unix(where, timeout_ms);
#endif
    }
#if defined(_WIN32)
    return hal::net_connect_pipe("\\\\.\\pipe\\openssh-ssh-agent", timeout_ms);
#else
    auto sock = env_value("SSH_AUTH_SOCK");
    if (!sock.ok()) return Result<std::unique_ptr<hal::Connection>>(sock.error());
    return hal::net_connect_unix(*sock, timeout_ms);
#endif
  }();
  if (!conn.ok()) return conn.error();
  return std::unique_ptr<SshAgent>(new Agent(std::make_unique<AgentConnection>(std::move(*conn))));
}

// The session's view of an agent key: the same interface a key file has, with the
// private half staying in the agent. Both types are complete here, so the
// declaration in ssh.hpp can be defined next to the protocol it speaks.
std::unique_ptr<SshSigner> ssh_agent_signer(std::shared_ptr<SshAgent> agent, SshAgentKey key) {
  if (!agent) return std::unique_ptr<SshSigner>();
  return std::unique_ptr<SshSigner>(new AgentSigner(std::move(key), std::move(agent)));
}

}  // namespace aios::net
