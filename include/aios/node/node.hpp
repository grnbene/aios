#pragma once
// Distribution: another kernel is reached exactly like a device, a memory
// provider and a channel transport. A NodeService lets remote kernels act
// here as a guest entity (capability-bound like any other); a RemoteNode makes
// a remote kernel's devices, memory and channels available locally. Local and
// distributed operations are the same kernel operations.
#include <memory>
#include <functional>
#include "aios/hal/sync.hpp"
#include <string>
#include <vector>
#include <atomic>
#include "aios/core/ids.hpp"
#include "aios/core/result.hpp"
#include "aios/hal/net.hpp"
#include "aios/compute/device.hpp"
#include "aios/channel/state_channel.hpp"
#include "aios/node/secure.hpp"
#include "aios/fleet/telemetry.hpp"

namespace aios {

class Kernel;

struct PeerInfo { EntityID guest; std::string address; Id128 kernel; uint64_t requests = 0; bool connected = true; };

class NodeService {
 public:
  NodeService(Kernel& k, std::string token);
  // The entity that admits migrated entities: what they may hold here is
  // re-derived from its authority. It outlives any one peer session.
  EntityID admission();
  ~NodeService();
  // loopback_only binds 127.0.0.1 alone; otherwise every interface.
  Result<uint16_t> start(uint16_t port, bool loopback_only = false);          // threaded: accepts and serves in the background
  Result<uint16_t> start_polled(uint16_t port, bool loopback_only = false);   // single-threaded: the owner calls poll() regularly
  void poll();                                    // accept pending peers, handle one frame per connected peer
  void enable();                                  // mark running without binding a port (for relayed peers)
  // Accept a second token as well (a node serving one token that then joins a
  // fleet must also admit peers presenting the cluster key through the hub).
  void add_token(const std::string& token);
  bool listening() const { return listener_ != nullptr; }
  void serve_connection(std::shared_ptr<hal::Connection> c);  // serve one externally-provided connection (e.g. a relay circuit)
  // The same, without a thread: handshake now, then poll() steps it. For a
  // kernel with no core to spare (the Pi's relay circuits).
  bool adopt_polled(std::shared_ptr<hal::Connection> c);
  void stop();
  // What a peer's Command request runs: the embedder's own command line (the
  // Pi's console). Unset - the default, and every hosted kernel - a node
  // refuses commands: holding the cluster key reaches the node protocol, not
  // a shell, unless the machine itself says otherwise.
  using CommandRunner = std::function<int(const std::string& command, std::string& output)>;
  void set_command_runner(CommandRunner r) { runner_ = std::move(r); }
  // What a peer's Sql request runs: one statement on this machine's SQL
  // engine, answered with the encoded result (userland/machine_sql.cpp owns
  // the format). Unset, the node refuses SQL.
  using SqlRunner = std::function<Result<ByteBuffer>(const std::string& sql)>;
  void set_sql_runner(SqlRunner r) { sql_runner_ = std::move(r); }
  std::vector<PeerInfo> peers() const;
  uint16_t port() const { return port_; }
 private:
  struct Session;
  void accept_loop();
  void serve(std::unique_ptr<hal::Connection> c);
  std::shared_ptr<Session> admit(std::shared_ptr<hal::Connection> c);
  bool step(Session& s, int timeout_ms);
  void end_session(Session& s);   // its guest (and all it holds here) goes with it
  std::vector<std::shared_ptr<Session>> polled_;
  bool polled_mode_ = false;
  CommandRunner runner_;
  SqlRunner sql_runner_;
  Kernel& k_;
  std::vector<std::string> tokens_;
  EntityID admission_;   // any one admits a peer; guarded by mu_
  std::unique_ptr<hal::Listener> listener_;
  hal::Thread acceptor_;
  // The connection travels alongside its thread so stop() can close it: a
  // session blocks in step()'s hour-long recv timeout, and only closing the
  // socket out from under it — the same trick already used to unblock the
  // acceptor's own poll() — returns it before that hour is up.
  // A threaded session: its connection, its thread, and whether it has
  // finished - finished ones are reaped when the next one starts, so a node
  // serving peer after peer does not keep every one it ever had.
  struct Threaded { std::shared_ptr<hal::Connection> conn; hal::Thread t; std::shared_ptr<std::atomic<bool>> done; };
  std::vector<Threaded> sessions_;
  void start_session(std::shared_ptr<hal::Connection> conn);   // mu_ held
  std::atomic<bool> running_{false};
  uint16_t port_ = 0;
  mutable hal::Mutex mu_;
  std::vector<PeerInfo> peers_;
};

struct RemoteDeviceInfo {
  DeviceID id; DeviceType type; std::string name; uint32_t lanes; uint64_t memory;
  std::vector<uint64_t> kernels;   // named kernels it runs (fnv1a); empty from a peer too old to say
};

class RemoteNode {
 public:
  RemoteNode(std::shared_ptr<hal::Connection> c, std::string address);
  Result<void> hello(const std::string& token, const Id128& my_kernel, hal::Host& host);
  // 120s default: a real fear for command()/sql_on() (a remote command or
  // query legitimately takes a while) is not appropriate for a quick status
  // ping - telemetry() passes its own, much shorter, bound instead.
  Result<ByteBuffer> call(uint8_t type, ByteSpan payload, int timeout_ms = 120000);
  // Ask this peer for its live load, or hand it a recovery point to hold.
  Result<NodeTelemetry> telemetry();
  Result<void> replicate(const EntityID& entity, uint64_t generation, ByteSpan image);
  Result<void> mem_patch(const MemoryID& remote, uint64_t offset, ByteSpan delta);
  Result<uint64_t> state_delta(const StateID& state, StateKind kind, uint64_t target_gen, ByteSpan delta_payload, const std::vector<MemoryID>& memory_refs);
  Result<std::pair<bool, StateObject>> state_sync(const StateID& state, uint64_t client_gen);
  // Run one of the peer's own commands (only a node whose embedder installed
  // a command runner accepts): its exit code and everything it printed.
  Result<std::pair<int, std::string>> command(const std::string& line);
  Result<ByteBuffer> query(const std::string& sql, int timeout_ms = 120000);
  const std::string& address() const { return address_; }
  const Id128& kernel() const { return kernel_; }
  // False once the underlying link (a relay circuit, say) has been torn down —
  // lets the kernel notice a peer went dead across a hub restart and re-reach it.
  bool alive() const { return c_ && c_->alive(); }
  const std::vector<RemoteDeviceInfo>& devices() const { return devices_; }
  uint64_t calls() const { return calls_; }
  // One call at a time crosses a link, so a job that runs for hours (a named
  // kernel) must not hold the link everything else to this peer uses. Whoever
  // made this node says how to open another authenticated link to the same
  // peer; a long job runs on a sibling, and the shared link stays free.
  using Opener = std::function<Result<std::shared_ptr<RemoteNode>>()>;
  void set_opener(Opener o) { opener_ = std::move(o); }
  bool can_open_sibling() const { return (bool)opener_; }
  // What the peer said it speaks (nodeproto::kFeat*); 0 from an older peer.
  uint32_t features() const { return features_; }
  Result<std::shared_ptr<RemoteNode>> open_sibling() const {
    if (!opener_) return Result<std::shared_ptr<RemoteNode>>(Status::Unsupported, "no way to open another link to " + address_);
    return opener_();
  }
  void close();
 private:
  std::shared_ptr<hal::Connection> c_;
  std::unique_ptr<Link> link_;
  std::string address_;
  Id128 kernel_;
  std::vector<RemoteDeviceInfo> devices_;
  hal::Mutex mu_;
  uint64_t calls_ = 0;
  Opener opener_;
  uint32_t features_ = 0;
};

// Registers the remote kernel's devices, its memory with the kernel's one
// MemoryType::Remote provider, and a "network:<address>" channel transport.
// Every attached peer can provide remote memory; new regions go to the one
// chosen (the first attached, until choose_remote_memory says otherwise), and
// each region stays with the peer that holds it.
struct RemoteAttachment { std::string address; std::vector<std::string> devices; bool provides_memory = false; std::string transport; };
Result<RemoteAttachment> attach_remote_node(Kernel& k, std::shared_ptr<RemoteNode> node);
// Where new remote memory goes: k.nodes()[node_index]. Returns its address.
Result<std::string> choose_remote_memory(Kernel& k, size_t node_index);
// The address new remote memory goes to ("" when no peer provides any).
std::string remote_memory_node(Kernel& k);

// Executes a host command using standard process execution and captures output.
int execute_host_command(const std::string& command, std::string& output);

}  // namespace aios
