#pragma once
// SSH, as a facility of this operating system.
//
// This is a real SSH-2 client: version exchange, algorithm negotiation, a
// curve25519-sha256 key exchange, an ssh-ed25519 host key checked against the
// machine's own known-hosts file, publickey authentication with an Ed25519
// identity, the binary packet protocol under chacha20-poly1305@openssh.com,
// and a session channel that runs a command with flow control. It speaks to
// OpenSSH's sshd; nothing here shells out to /usr/bin/ssh, and every byte goes
// over hal::net, so it runs on the bare-metal substrate's own TCP/IP stack as
// well as on a hosted one.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"
#include "aios/crypto/curve25519.hpp"
#include "aios/crypto/nistp.hpp"
#include "aios/crypto/rsa.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/net.hpp"

namespace aios::net {

// An SSH identity this machine holds: Ed25519 (seed/pub), RSA (rsa) or ECDSA over
// nistp256/384/521 (ec_curve, ec_d, ec_pub).
struct SshIdentity {
  std::string type = "ssh-ed25519";            // "ssh-ed25519", "ssh-rsa", "ecdsa-sha2-nistp256|384|521"
  crypto::Scalar32 seed{};
  crypto::Point32 pub{};
  crypto::RsaPrivateKey rsa{};
  crypto::NistCurve ec_curve = crypto::NistCurve::P256;
  ByteBuffer ec_d;                             // ECDSA private scalar, big-endian
  ByteBuffer ec_pub;                           // ECDSA public point, uncompressed (0x04 || X || Y)
  std::string comment;
  bool valid = false;
  ByteBuffer public_blob() const;              // public key wire blob
  std::string public_line() const;             // the authorized_keys line
  std::string fingerprint() const;             // SHA256:... as OpenSSH prints it
};

// OpenSSH-format, PKCS#1, SEC1 or PKCS#8 PEM private key, unencrypted or encrypted with a passphrase.
// Status::Denied ("needs its passphrase") when it is encrypted and `passphrase` is empty or wrong.
Result<SshIdentity> ssh_parse_private_key(ByteSpan pem, const std::string& passphrase = {});
// Whether a private key file's text is encrypted (no passphrase needed to tell).
bool ssh_private_key_encrypted(ByteSpan pem);
// The public key blob of an authorized_keys/.pub line ("type base64 [comment]"), and its type.
Result<std::pair<std::string, ByteBuffer>> ssh_parse_public_line(const std::string& line);
// The key type a public blob carries (its first string).
std::string ssh_blob_type(ByteSpan blob);
// SHA256:... of a public blob, as OpenSSH prints it.
std::string ssh_fingerprint(ByteSpan blob);
// Checks an SSH signature blob (string alg, string sig) over `data` against public blob `key`:
// ssh-ed25519, rsa-sha2-256/512 (and ssh-rsa), ecdsa-sha2-nistp256/384/521.
bool ssh_verify_signature(ByteSpan key, ByteSpan signature, ByteSpan data);

// Something that can prove possession of a key: a key file here, or a key an agent holds.
class SshSigner {
 public:
  virtual ~SshSigner() = default;
  virtual ByteBuffer public_blob() const = 0;
  virtual std::string key_type() const = 0;      // ssh-ed25519, ssh-rsa, ecdsa-sha2-nistp256, ...
  virtual std::string describe() const = 0;      // "/Users/x/.ssh/id_ed25519" or "agent: comment"
  // The SSH signature blob (string alg, string sig) over `data`. `algorithm` is the public key algorithm
  // being used (rsa-sha2-512 / rsa-sha2-256 for an RSA key; the key type otherwise). A file signer whose key is
  // encrypted asks for the passphrase here (once, through hal::tty_ask), not before: a key the server would
  // not take is never unlocked.
  virtual Result<ByteBuffer> sign(ByteSpan data, const std::string& algorithm) = 0;
};
// A key already held in memory.
std::unique_ptr<SshSigner> ssh_identity_signer(SshIdentity id, std::string where);
// A key file: its public half from <path>.pub, or from the private file when unencrypted. Status::NotFound when
// the file does not exist; an encrypted key without a .pub beside it is unlocked at once (as ssh does).
// `batch`: never prompt (an encrypted key that needs a passphrase is then skipped with Status::Denied at sign).
Result<std::unique_ptr<SshSigner>> ssh_file_signer(hal::Host& host, const std::string& path, bool batch);
class SshAgent;
struct SshAgentKey;
// A key held by an agent; the agent is shared by every key it holds.
std::unique_ptr<SshSigner> ssh_agent_signer(std::shared_ptr<SshAgent> agent, SshAgentKey key);
// A new identity from the machine's own randomness, and its PEM.
SshIdentity ssh_generate_identity(hal::Host& host, const std::string& comment);
std::string ssh_write_private_key(const SshIdentity& id);

// The identity this machine holds at `path`, generated and written there if it
// has none yet (private half narrowed to this machine, public half beside it in
// authorized_keys form). Key material is the network layer's business, not the
// command surface's, so the file handling lives here.
Result<SshIdentity> ssh_load_identity(hal::Host& host, const std::string& path, bool create_if_missing,
                                      const std::string& comment = {},
                                      const std::string& passphrase = {});

struct SshKnownHost {
  std::string host;          // "name" or "[name]:port", as OpenSSH writes it
  std::string algorithm;
  std::string fingerprint;   // SHA256:...
};
// Every host this machine has met, read out of its known-hosts file.
Result<std::vector<SshKnownHost>> ssh_known_hosts(hal::Host& host, const std::string& path);

struct SshTarget {
  std::string user;
  std::string host;
  uint16_t port = 22;
  std::string str() const;
};
Result<SshTarget> ssh_parse_target(const std::string& s, const std::string& default_user);

struct SshHostKey {
  std::string algorithm;      // ssh-ed25519
  ByteBuffer blob;
  std::string fingerprint;    // SHA256:...
  bool first_seen = false;    // the machine had never met this host before
};

// How a session is opened, authenticated and its host checked, as OpenSSH would
// do it for a destination resolved through ssh_config (net/ssh_config.hpp).
struct SshConnectOptions {
  // Tried in order with publickey: each is first offered without a signature, and signs only when the server
  // answers that it would accept it (USERAUTH_PK_OK), so a refused key costs no passphrase prompt.
  std::vector<std::unique_ptr<SshSigner>> signers;
  // Decides whether the key the server proved it holds is the right one for this host (known_hosts and
  // StrictHostKeyChecking: ssh_accept_host_key in net/ssh_known_hosts.hpp). Required: a session with no one
  // to decide refuses every host.
  std::function<Result<void>(const SshHostKey& key)> verify_host_key;
  // Offered in this order, each filtered to what the client implements; empty = the client's own order.
  // A caller puts the algorithms the host is already known under first (ssh_known_host_algorithms).
  std::vector<std::string> host_key_algorithms, kex_algorithms, ciphers;
  // ProxyJump hops, nearest first, each ALREADY connected and authenticated by the caller (every hop is its own
  // destination, resolved through ssh_config with its own keys and host check; hop n+1 was connected with
  // connect_over through hop n's open_direct_tcpip). When set, connect(target, options) opens a direct-tcpip
  // channel on the last hop to target.host:target.port and runs the session over it; the session keeps the hops
  // (moved out of the options) alive for as long as it lives.
  std::vector<std::unique_ptr<class SshSession>> jumps;
  int timeout_ms = 20000;
};

class SshSession {
 public:
  // `known_hosts` is the file the single-identity connect() below checks and records hosts in (the machine's
  // own, for aios-sh's `ssh` with key=); connect(target, options) uses the options' files instead.
  SshSession(hal::Host& host, std::string known_hosts);
  ~SshSession();

  Result<void> connect(const SshTarget& target, const SshIdentity& identity, int timeout_ms = 20000);
  Result<void> connect(const SshTarget& target, SshConnectOptions& options);
  // Over an already open byte stream (a ProxyJump hop's channel) instead of a new TCP connection.
  Result<void> connect_over(std::unique_ptr<hal::Connection> stream, const SshTarget& target, SshConnectOptions& options);
  // The session's one channel as a direct-tcpip forward to host:port (instead of exec), handed back as a byte
  // stream. The session must outlive it: the stream takes ownership of nothing.
  Result<std::unique_ptr<hal::Connection>> open_direct_tcpip(const std::string& host, uint16_t port);
  // Which signer the server accepted ("" before authentication).
  const std::string& authenticated_with() const { return authenticated_with_; }
  // `env`: variables asked for in the remote command's environment first (SSH "env" requests, no reply awaited, as
  // OpenSSH sends them). A server that does not accept one ignores it: git asks for protocol v2 this way
  // (GIT_PROTOCOL=version=2) and a server that refuses simply answers v0.
  Result<void> exec(const std::string& command, const std::vector<std::pair<std::string, std::string>>& env = {});

  Result<void> send(ByteSpan data);          // to the remote command's stdin
  Result<void> send_eof();
  // Whatever has arrived. An empty buffer means nothing yet (or, once eof() is
  // true, that the remote is done).
  Result<ByteBuffer> recv(int timeout_ms);
  bool eof() const { return channel_eof_ || channel_closed_; }
  bool closed() const { return channel_closed_; }
  // Keep reading until the remote closes the channel. The exit status arrives
  // in a channel request AFTER the last byte of output, so a caller that stops
  // at EOF never learns how the command ended.
  Result<void> wait_close(int timeout_ms);
  void disconnect();

  // Rotate transport encryption keys (RFC 4253 §9) over the active encrypted session.
  Result<void> rekey(int timeout_ms = 20000);

  const SshHostKey& host_key() const { return host_key_; }
  const std::string& server_version() const { return server_ident_; }
  const std::string& stderr_text() const { return stderr_; }
  int exit_status() const { return exit_status_; }
  uint64_t bytes_in() const { return bytes_in_; }
  uint64_t bytes_out() const { return bytes_out_; }
  const std::string& cipher() const { return cipher_; }
  const std::string& kex() const { return kex_; }

  // The session's one channel, for the stream open_direct_tcpip hands out.
  bool channel_alive() const { return channel_open_ && !channel_closed_; }
  size_t channel_buffered() const { return rx_data_.size(); }

 private:
  Result<void> write_packet(ByteSpan payload);
  Result<ByteBuffer> read_packet(int timeout_ms);      // one payload, headers stripped
  Result<void> pump(int timeout_ms);                   // read one packet and dispatch it
  Result<void> handshake(const SshTarget& t, int timeout_ms);
  Result<void> do_kex(const SshTarget* target_if_initial, const ByteBuffer* server_kexinit_opt, int timeout_ms);
  Result<void> authenticate(const SshTarget& t, const SshIdentity& id, int timeout_ms);
  Result<void> authenticate_signers(const SshTarget& t, std::vector<std::unique_ptr<SshSigner>>& signers, int timeout_ms);
  Result<void> service_request(int timeout_ms);
  Result<void> check_host_key(const SshTarget& t);
  Result<void> open_channel(const std::string& type, ByteSpan extra, int timeout_ms);
  void configure(const SshConnectOptions& options);
  Result<ByteBuffer> read_exact(size_t n, int timeout_ms);

  hal::Host& host_;
  std::string known_hosts_;
  std::unique_ptr<hal::Connection> conn_;

  bool encrypted_ = false;
  // chacha20-poly1305@openssh.com: 64 bytes each, K_2 || K_1. aes*-gcm@openssh.com: the AES key, and a 12-byte
  // IV whose last 8 bytes count packets.
  std::vector<uint8_t> key_c2s_, key_s2c_;
  std::vector<uint8_t> iv_c2s_, iv_s2c_;
  std::string cipher_c2s_, cipher_s2c_;
  std::string cipher_c2s_pending_, cipher_s2c_pending_;   // agreed in KEXINIT, live from NEWKEYS
  uint32_t seq_out_ = 0, seq_in_ = 0;
  // What this session offers in KEXINIT, in order (from SshConnectOptions, else the client's own).
  std::string offer_kex_, offer_host_keys_, offer_ciphers_;
  std::function<Result<void>(const SshHostKey&)> verify_host_key_;
  std::vector<std::unique_ptr<SshSession>> jumps_;   // the ProxyJump hops this session is carried through

  std::string client_ident_, server_ident_;
  ByteBuffer session_id_;
  SshHostKey host_key_;
  std::string cipher_, kex_;
  std::string server_sig_algs_;
  std::string host_key_alg_;   // what the key exchange agreed on

  uint32_t local_channel_ = 0, remote_channel_ = 0;
  uint32_t local_window_ = 0, remote_window_ = 0, remote_max_packet_ = 32768;
  bool channel_open_ = false, channel_eof_ = false, channel_closed_ = false;
  int exit_status_ = -1;
  ByteBuffer rx_data_;
  std::string stderr_;
  uint64_t bytes_in_ = 0, bytes_out_ = 0;
  uint64_t rekey_bytes_ = 0;
  uint64_t last_kex_ns_ = 0;
  std::string authenticated_with_;
};

}  // namespace aios::net
