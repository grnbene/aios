#pragma once
// The kernel: the machine on which intelligence exists. It owns identity,
// memory, state, channels, compute, capabilities and storage, and drives bound
// intelligences through the cognitive cycle. It has no notion of agents,
// tools, prompts, or any particular intelligence.
#include <memory>
#include "aios/kernel/residency_controller.hpp"
#include "aios/hal/sync.hpp"
#include <string>
#include <unordered_map>
#include "aios/hal/host.hpp"
#include "aios/core/ids.hpp"
#include "aios/memory/memory_fabric.hpp"
#include "aios/capability/capability.hpp"
#include "aios/capability/authority.hpp"
#include "aios/state/state.hpp"
#include "aios/entity/entity.hpp"
#include "aios/channel/state_channel.hpp"
#include "aios/compute/compute_graph.hpp"
#include "aios/compute/device.hpp"
#include "aios/compute/scheduler.hpp"
#include "aios/storage/object_store.hpp"
#include "aios/intelligence/intelligence.hpp"
#include "aios/node/node.hpp"
#include "aios/fleet/telemetry.hpp"
#include "aios/crypto/curve25519.hpp"
#include "aios/fleet/relay.hpp"
#include "aios/kernel/watchdog.hpp"
#include <atomic>

namespace aios {
struct LinkIdentity;   // aios/node/secure.hpp

class Surface;

struct KernelConfig {
  std::string root;                 // persistent root: identity, persistent memory, checkpoints, object store
  uint64_t default_memory_quota = 1ull << 30;   // bytes a spawned entity may allocate by default
  uint32_t slots = 0;
  ResidencyConfig residency;
};

struct CycleReport {
  EntityID entity;
  std::string intelligence;
  StateID context, perceived, result;
  GraphID graph;
  ExecutionReport execution;
  bool computed = false;
  uint64_t published_sequence = 0;
  bool published = false;
  uint64_t elapsed_ns = 0;
};

// What a resident is created holding. Applied when the resident first exists;
// afterwards its authority is whatever the ledger says it is -- except that a
// later boot with a profile that lists more tops the resident up with what it
// lacks (additive: nothing is ever taken away by a boot).
struct AuthorityGrant {
  ResourceRef resource;
  uint16_t rights = 0;
  uint64_t scope_limit = 0;
};
using AuthorityProfile = std::vector<AuthorityGrant>;

struct MindBinding {
  std::unique_ptr<Intelligence> mind;
  StateID context;         // the entity's canonical Context state, owned by the kernel's cycle
  ChannelID output;        // where results are published, if any
  uint64_t cycles = 0;
  // Serialises the perceive→think→act sequence for THIS mind. The three steps
  // are three round-trips over one pipe; two cycles running at once would
  // interleave their frames and desync the channel. Held only for the duration
  // of a cycle, never while binding — a slow model call must not block bind or
  // status. unique_ptr because hal::Mutex is neither copyable nor movable and
  // the binding is moved into the map.
  std::unique_ptr<hal::Mutex> lock = std::make_unique<hal::Mutex>();
};

class Kernel : private CapabilityObserver {
 public:
  static Result<std::unique_ptr<Kernel>> boot(const KernelConfig& cfg);
  ~Kernel() override;

  // Identity of this kernel instance; persistent across boots at the same root.
  const EntityID& self() const { return self_; }
  // This machine's Ed25519 identity (<root>/node.identity, readable by this account only), made at first start.
  // Null when the root could not hold it (the kernel still runs; nothing uses the key yet); node_key_problem() says why.
  const crypto::Point32* node_public_key() const { return node_key_ok_ ? &node_pub_ : nullptr; }
  const std::string& node_key_problem() const { return node_key_problem_; }
  // The hub's identity key, pinned for the next fleet_join: the member then takes peer keys from rosters it signs.
  void set_fleet_hub_key(const crypto::Point32& key) { hub_key_ = key; have_hub_key_ = true; }
  Result<crypto::Sig64> node_sign(ByteSpan message) const;
  uint64_t boot_ns() const { return boot_ns_; }
  uint64_t now() const;
  uint64_t uptime_ns() const;
  const KernelConfig& config() const { return cfg_; }

  hal::Host& host() { return *host_; }
  IdGenerator& ids() { return *ids_; }
  MemoryFabric& memory() { return *memory_; }
  CapabilityManager& capabilities() { return *caps_; }
  StateStore& states() { return *states_; }
  EntityManager& entities() { return *entities_; }
  ChannelManager& channels() { return *channels_; }
  GraphStore& graphs() { return *graphs_; }
  DeviceFabric& devices() { return *devices_; }
  Scheduler& scheduler() { return *scheduler_; }
  PreemptiveSchedulerQuota& quotas() { return *quotas_; }
  ObjectStore& store() { return *store_; }
  ResidencyController& residency() { return *residency_; }
  KernelWatchdog& watchdog() { return *watchdog_; }
  // Registers the accelerators first if nothing has yet (they load lazily).
  const AcceleratorReport& accelerators() const { devices_->ensure_loaded(); return accelerators_; }
  // Authority as a persistent object: residents, their capabilities, and every
  // decision the kernel made about authority since this root was created.
  const AuthorityLedger& authority() const { return *authority_; }
  bool is_resident(const EntityID& id) const { return authority_->is_resident(id); }

  // Residents: entities the kernel keeps across boots with the authority it
  // recorded for them. A substrate binds its human interface to one and acts
  // ONLY through that entity's Surface. `profile` applies at first creation;
  // it is derived from the kernel's own authority, so the tree stays rooted.
  Result<EntityID> resident(const std::string& label, const AuthorityProfile& profile, bool self_improve = false);
  bool may_self_improve(const EntityID& id) const { return authority_->may_self_improve(id); }
  // Self-improvement, GOVERNED. A trusted resident petitions the machine to
  // grow its own authority. The machine decides: only residents it trusts, the
  // grant bounded to policy, the hard wall intact (never kernel ownership), and
  // every decision on the authority ledger. This is the ONLY path that mints to
  // an entity after boot; grant/delegate still only pass on what is already held.
  Result<CapabilityID> petition(const EntityID& who, ResourceRef res, uint16_t rights, uint64_t ttl_ns = 0, bool confirmed = false);

  // ---- KERNEL AUTHORITY ---------------------------------------------------
  // Everything from here to the Surface is the kernel acting as itself. No
  // command surface, console or intelligence reaches these: they act as an
  // entity, through Surface, and the kernel decides from that entity's
  // capabilities (scripts/authority-gate.sh keeps the command surface honest).
  //
  // Entity lifecycle with kernel policy: a spawned entity receives a bounded
  // allocation capability and authority over its own state. Compute and
  // scheduling authority are granted explicitly, never by default.
  Result<EntityID> spawn(const std::string& label, const EntityID& parent = {});
  Result<void> terminate(const EntityID& id);
  // Minting: authority that derives from nothing but the kernel itself.
  Result<CapabilityID> grant(const EntityID& holder, ResourceRef res, uint16_t rights, uint64_t ttl_ns = 0, uint64_t scope_limit = 0);
  Result<void> revoke(const CapabilityID& id);

  // Single-use kernel:destroy capability for a named disk.
  Result<CapabilityID> grant_destroy(const EntityID& holder, const std::string& disk, bool confirmed = true);
  Result<CapabilityID> grant_destroy(const EntityID& holder, const Id128& disk_id, bool confirmed = true);
  Result<CapabilityID> grant_destroy(const EntityID& holder, const std::string& disk, const std::string& confirmation);
  Result<void> consume_destroy(const EntityID& holder, const std::string& disk);
  Result<void> consume_destroy(const EntityID& holder, const Id128& disk_id, const std::string& disk_name = "");

  // The capability-checked API, exercised AS an entity. This is the one door that does NOT ask whether the
  // caller may be that entity: it is for the code that owns the process (a tool's main, the node service
  // binding a session to the guest it just spawned). Anything running on an entity's behalf, a mind, a grain
  // program, a remote peer, must only ever hold the Surface it was given, and reach another entity through
  // Surface::act_as, which requires execute authority over it. Never pass an entity id from outside the
  // process to this function.
  Surface as(const EntityID& entity);

  // Checkpoint the whole entity (identity, state, memory contents, capabilities,
  // graphs, channels, execution position) to the persistent root; restore it
  // with the SAME identity into this or another kernel.
  // A restore performed on behalf of `restorer` (any entity but the kernel)
  // re-derives the image's capabilities from the restorer's own authority and
  // drops what cannot be derived: an image is not a way to smuggle authority.
  Result<std::string> checkpoint(const EntityID& id);
  Result<EntityID> restore(const std::string& path, const EntityID& restorer = {});
  // Put a LIVE entity back to a checkpoint, keeping its identity.
  //
  // `restore` refuses over an entity that is already here, and `terminate`
  // refuses a resident outright — the kernel keeps residents across boots. So
  // between them there was no way to put the one entity a person would actually
  // want to rewind, the resident mind, back to a moment it had been. This is
  // that: the entity's contents go (states, memory, capabilities, graphs,
  // channels), the entity itself and its 128-bit identity stay, and the image's
  // contents are installed under that same identity — including the position it
  // was at in the graph it was running. Authority still does not travel in an
  // image: whatever is not the image's own is re-derived from `actor` or
  // dropped on the record, exactly as a restore does.
  //
  // The image is verified BEFORE anything is torn down, so a corrupt file
  // cannot cost a person the running entity it was meant to rescue.
  Result<EntityID> rewind(const std::string& path, const EntityID& actor = {});
  Result<ByteBuffer> capture(const EntityID& id);          // image in memory
  Result<EntityID> restore_image(ByteSpan image, const EntityID& restorer = {});

  // Distribution: serve this kernel to peers, or attach a peer's kernel here.
  // loopback_only: bind 127.0.0.1 alone instead of every interface.
  Result<uint16_t> serve(uint16_t port, const std::string& token, bool polled = false, bool loopback_only = false);
  Result<RemoteAttachment> connect(const std::string& host, uint16_t port, const std::string& token);
  NodeService* service() const { return service_.get(); }
  // Move a running entity to a peer kernel over the node link: capture its
  // image here, restore it there (verbatim — the link is token-authenticated),
  // and — unless keep_local — terminate it here so it has truly MOVED, not cloned.
  Result<EntityID> migrate(const EntityID& id, size_t node_index, bool keep_local = false);
  const std::vector<std::shared_ptr<RemoteNode>>& nodes() const { return nodes_; }

  // ---- FLEET OVERLAY: membership and reachability without hardcoding --------
  // Become the fleet's hub: a reachable rendezvous that also relays circuits
  // between members. In production this is the always-on console.
  Result<uint16_t> relay_hub(uint16_t port, const std::string& cluster_key, bool loopback_only = false,
                             const std::string& members_file = {}, bool require_attest = false);
  RelayHub* hub() const { return hub_.get(); }
  // Join a fleet by attaching to a hub with the shared cluster key. From then
  // on peers are DISCOVERED (the roster) and REACHED (relayed circuits) with no
  // per-peer address or token — one hub, one key, the rest is learned.
  // keep_trying: a hub that is down now is retried in the background (Ok is
  // returned; in_fleet() turns true once attached).
  Result<void> fleet_join(const std::string& hub_host, uint16_t hub_port, const std::string& cluster_key, bool keep_trying = false,
                          const std::string& member_key = {}, bool attest = false);
  bool in_fleet() const { return relay_ && relay_->attached(); }
  // Joined but not attached right now: the first attach is still being tried,
  // or the hub link dropped and is reconnecting.
  bool fleet_reconnecting() const { return relay_ && relay_->trying(); }
  std::vector<Id128> fleet_roster() const { return relay_ ? relay_->roster() : std::vector<Id128>{}; }
  RelayClient* relay() const { return relay_.get(); }
  // Ensure a node link exists to a peer kernel, over a relay circuit, opening
  // and handshaking one if needed. Returns its index in nodes(). Idempotent.
  Result<size_t> reach(const Id128& kernel_id);
  // Reach every roster peer not yet connected, so the scheduler sees the whole
  // fleet through nodes(). Best-effort: unreachable peers are skipped.
  void fleet_sync();

  // ---- FLEET: live load and recovery points --------------------------------
  // This kernel's live self-report: static capacity (cpus, memory, slots) plus
  // what is running here now. A peer asks for it over the node link; the
  // orchestrator compares reports across the fleet to place work.
  NodeTelemetry telemetry() const;
  // Slots = how many movable jobs this node will run concurrently. Defaults to
  // logical CPUs; an operator (or a bare-metal build) may set it smaller to
  // model a constrained node. This is scheduling policy, not a hardware limit.
  void set_slots(uint32_t s) { slots_ = s; }
  uint32_t slots() const;

  // Fleet DB registry: advertise local database services in telemetry
  void register_hosted_db(const NodeTelemetry::HostedDbInfo& db);
  void unregister_hosted_db(const std::string& db_name);
  void clear_hosted_dbs();
  std::vector<NodeTelemetry::HostedDbInfo> hosted_databases() const;

  // A recovery point: another kernel's movable job, checkpointed here but NOT
  // run, so that if that kernel dies its work can reappear on a survivor from
  // the last replicated position. Replicas are keyed by (source kernel, entity).
  struct Replica { Id128 source_kernel; EntityID entity; uint64_t generation = 0; uint64_t received_ns = 0; uint64_t bytes = 0; };
  Result<void> store_replica(const Id128& source_kernel, const EntityID& entity, uint64_t generation, ByteSpan image);
  std::vector<Replica> replicas() const;                       // metadata only
  bool holds_replica(const EntityID& entity) const;
  // Bring a replicated job back to life here at its checkpointed position and
  // drop the stored image. The entity returns with the SAME identity it had on
  // the dead node — it has moved, not been re-created.
  Result<EntityID> recover_replica(const EntityID& entity);

  // Intelligence: bind or dynamic hot-swap a replaceable mind to an entity and run its cycle.
  Result<void> bind_mind(const EntityID& id, std::unique_ptr<Intelligence> mind, const ChannelID& output = {});
  Result<void> swap_mind(const EntityID& id, std::unique_ptr<Intelligence> new_mind, const ChannelID& output = {});
  Result<void> unbind_mind(const EntityID& id);
  const MindBinding* mind(const EntityID& id) const;
  Result<CycleReport> cycle(const EntityID& id);

 private:
  Kernel() = default;
  Result<void> init(const KernelConfig& cfg);
  Result<void> load_or_create_identity();
  void load_or_create_node_key();
  Result<void> replay_authority();
  void issued(const Capability& c) override;
  void revoked(const Capability& c) override;
  void note(AuthorityEvent::Kind kind, const EntityID& actor, const EntityID& subject, ResourceRef res, uint16_t rights, const CapabilityID& cap, const std::string& why);
  friend class Surface;

  KernelConfig cfg_;
  std::unique_ptr<hal::Host> host_;
  std::unique_ptr<IdGenerator> ids_;
  std::unique_ptr<MemoryFabric> memory_;
  std::unique_ptr<CapabilityManager> caps_;
  std::unique_ptr<StateStore> states_;
  std::unique_ptr<EntityManager> entities_;
  std::unique_ptr<ChannelManager> channels_;
  std::unique_ptr<GraphStore> graphs_;
  std::unique_ptr<DeviceFabric> devices_;
  std::unique_ptr<PreemptiveSchedulerQuota> quotas_;
  std::unique_ptr<Scheduler> scheduler_;
  std::unique_ptr<ObjectStore> store_;
  std::unique_ptr<ResidencyController> residency_;
  std::unique_ptr<AuthorityLedger> authority_;
  std::unique_ptr<KernelWatchdog> watchdog_;
  bool replaying_ = false;
  AcceleratorReport accelerators_;
  std::unique_ptr<NodeService> service_;
  std::vector<std::shared_ptr<RemoteNode>> nodes_;
  std::unique_ptr<RelayHub> hub_;            // set when this kernel is the fleet hub
  std::unique_ptr<RelayClient> relay_;       // set when this kernel has joined a fleet
  std::string cluster_key_;                  // the one shared secret; also the node-link token
  bool have_hub_key_ = false;                // fleet join pins the hub's identity key (its signed roster)
  crypto::Point32 hub_key_{};
  std::shared_ptr<const LinkIdentity> link_identity_;   // what this kernel installed for its links
  hal::Thread relay_accept_;                 // hands relayed incoming circuits to the node service
  std::atomic<bool> relay_accepting_{false};
  uint32_t slots_ = 0;                       // 0 => report logical_cpus
  mutable uint64_t heartbeat_ = 0;           // advances each telemetry() report
  std::vector<Replica> replica_meta_;
  std::vector<ByteBuffer> replica_images_;   // parallel to replica_meta_
  mutable hal::Mutex replicas_mu_;
  EntityID self_;
  crypto::Scalar32 node_seed_{};
  crypto::Point32 node_pub_{};
  bool node_key_ok_ = false;
  std::string node_key_problem_;
  uint64_t boot_ns_ = 0;        // wall clock at boot (a timestamp)
  uint64_t boot_mono_ns_ = 0;   // monotonic clock at boot (what uptime counts from)
  mutable hal::Mutex minds_mu_;
  std::unordered_map<EntityID, MindBinding, TypedIdHash<1>> minds_;
  mutable hal::Mutex hosted_dbs_mu_;
  std::vector<NodeTelemetry::HostedDbInfo> hosted_dbs_;
};

}  // namespace aios
