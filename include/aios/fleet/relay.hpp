#pragma once
// The fleet overlay: one reachable hub is both the rendezvous and the relay.
//
// A member opens ONE outbound connection to the hub and, from that alone,
// becomes reachable by kernel identity and learns who else is present — no
// inbound port, no hole-punching, no per-peer address or token. The hub
// multiplexes many byte-circuits over each member's single connection and
// forwards them blindly: the node link's own ChaCha20-Poly1305 handshake runs
// end to end inside each circuit, so the hub only ever moves ciphertext. It is
// a dumb switch, authenticated by the cluster key, that happens to also keep
// the roster.
//
// This is what replaces "ssh -L and a hardcoded IP": configure one hub and one
// cluster key, and the fleet finds and reaches itself.
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include "aios/hal/net.hpp"
#include "aios/core/ids.hpp"
#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"

namespace aios {

// The relay/rendezvous. Runs on a member with a reachable address (in
// production, the always-on console). Tracks who is attached and forwards
// circuits between them.
class RelayHub {
 public:
  RelayHub(std::string cluster_key);
  // Per-member attach credentials, re-read on every attach and every few
  // seconds: one line per member, `<member-id> <key> [ak=<hex P-256 point>]
  // [pcrs=<hex sha256>]`. With a file set, only listed members attach, each
  // with its own key (the cluster key no longer does), and a member removed
  // from the file is dropped. With require_attest, each must also present a
  // TPM quote over the hub's challenge, signed by its listed `ak`, and over
  // its listed `pcrs` digest when one is given.
  void set_members_file(std::string path);
  void set_require_attest(bool on);
  ~RelayHub();
  Result<uint16_t> start(uint16_t port, bool loopback_only = false);   // loopback_only: 127.0.0.1 alone
  void stop();
  uint16_t port() const;
  size_t attached() const;                 // members currently attached
  std::vector<Id128> members() const;      // their kernel identities
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// A member's side of the overlay: keep one attach connection to the hub, learn
// the roster, open circuits to peers by identity, and accept circuits peers
// open to us. Each circuit is a hal::Connection carrying opaque bytes.
class RelayClient {
 public:
  RelayClient(std::string cluster_key, Id128 self);
  // This member's own attach key, for a hub that keeps a members file. The
  // cluster key still protects circuits end to end.
  void set_member_key(std::string key);
  // Present a TPM quote over the hub's challenge on every attach.
  void set_attest(bool on);
  ~RelayClient();
  // Attach to a hub. After this returns Ok, the roster is populated and
  // incoming circuits are accepted in the background.
  // keep_trying: when the hub cannot be reached now, return Ok anyway and keep
  // attaching in the background (attached() turns true when it succeeds).
  Result<void> attach(const std::string& hub_host, uint16_t hub_port, bool keep_trying = false);
  // The same, with no background thread: the caller runs poll() often (at
  // least every few seconds - the hub reaps a member that stops beating).
  // For a kernel with no core to spare for the reader.
  Result<void> attach_polled(const std::string& hub_host, uint16_t hub_port);
  void poll();
  bool attached() const;
  bool trying() const;                // joined, but not attached right now (first attach or reconnecting)
  std::string last_error() const;     // why the last attach/reconnect failed ("" once attached)
  const std::string& hub_address() const;

  // Everyone the hub currently knows, minus ourselves. Discovered, not configured.
  std::vector<Id128> roster() const;

  // Open a circuit to a peer reachable via the hub. The returned Connection
  // carries bytes end to end; a secure node link runs on top unchanged.
  Result<std::unique_ptr<hal::Connection>> open(const Id128& target);
  // A peer opened a circuit to us — hand back the next one, or null if none
  // pending. Each is a Connection ready for the node service's responder side.
  std::unique_ptr<hal::Connection> accept_incoming();

  // Dispatch a command across a relayed circuit to target peer.
  Result<std::pair<int, std::string>> command(const Id128& target, const std::string& line, int timeout_ms = 120000);
  // Dispatch a query across a relayed circuit to target peer.
  Result<ByteBuffer> query(const Id128& target, const std::string& sql, int timeout_ms = 120000);

  // Command runner for incoming command requests across relayed circuits.
  using CommandRunner = std::function<int(const std::string& command, std::string& output)>;
  void set_command_runner(CommandRunner r);

  void stop();
  struct Impl;                 // defined in relay.cpp; shared so circuits outlive the client handle
 private:
  void* holder_ = nullptr;     // owns a std::shared_ptr<Impl>
};

}  // namespace aios
