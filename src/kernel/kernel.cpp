#include "aios/kernel/kernel.hpp"
#include "aios/node/secure.hpp"
#include "aios/compute/backends.hpp"
#include "aios/compute/erp_kernels.hpp"
#include "aios/compute/sat_kernels.hpp"
#include "aios/hal/sync.hpp"
#include "aios/kernel/surface.hpp"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <unordered_map>

namespace aios {

namespace {
hal::Mutex& kernel_names_mu() { static hal::Mutex m; return m; }
std::unordered_map<uint64_t, std::string>& kernel_names() { static std::unordered_map<uint64_t, std::string> m; return m; }
}  // namespace
void note_custom_kernel_name(const std::string& name) {
  hal::LockGuard g(kernel_names_mu());
  kernel_names()[fnv1a(name)] = name;
}
std::string custom_kernel_name(uint64_t hash) {
  hal::LockGuard g(kernel_names_mu());
  auto it = kernel_names().find(hash);
  return it == kernel_names().end() ? std::string() : it->second;
}


Result<std::unique_ptr<Kernel>> Kernel::boot(const KernelConfig& cfg) {
  std::unique_ptr<Kernel> k(new Kernel());
  auto r = k->init(cfg);
  if (!r.ok()) return r.error();
  return k;
}

Kernel::~Kernel() {
  // Links stop using this kernel's key and roster before they go.
  if (link_identity_ && link_identity() == link_identity_) set_link_identity(nullptr);
  // Minds and the node service go first: they hold threads and external
  // resources that reference the kernel.
  relay_accepting_ = false;
  if (relay_accept_.joinable()) relay_accept_.join();
  if (relay_) relay_->stop();
  if (hub_) hub_->stop();
  if (service_) service_->stop();
  minds_.clear();
}

Result<uint16_t> Kernel::serve(uint16_t port, const std::string& token, bool polled, bool loopback_only) {
  if (token.size() < 16) return Result<uint16_t>(Status::Invalid, "node token must be at least 16 characters (it keys the encrypted link)");
  if (service_ && service_->listening()) return Result<uint16_t>(Status::Busy, "already serving on port " + std::to_string(service_->port()));
  // A fleet member already has a (relay-only) service keyed by the cluster key:
  // give it a port and this token too, rather than a second service.
  if (service_) {
    service_->add_token(token);
    return polled ? service_->start_polled(port, loopback_only) : service_->start(port, loopback_only);
  }
  service_ = std::make_unique<NodeService>(*this, token);
  auto p = polled ? service_->start_polled(port, loopback_only) : service_->start(port, loopback_only);
  if (!p.ok()) service_.reset();
  return p;
}

Result<RemoteAttachment> Kernel::connect(const std::string& host, uint16_t port, const std::string& token) {
  auto c = hal::net_connect(host, port, 5000);
  if (!c.ok()) return c.error();
  if (token.size() < 16) return Result<RemoteAttachment>(Status::Invalid, "node token must be at least 16 characters");
  auto node = std::make_shared<RemoteNode>(std::shared_ptr<hal::Connection>(std::move(*c)), host + ":" + std::to_string(port));
  AIOS_TRY(node->hello(token, self_, *host_));
  node->set_opener([this, host, port, token]() -> Result<std::shared_ptr<RemoteNode>> {
    auto c2 = hal::net_connect(host, port, 5000);
    if (!c2.ok()) return c2.error();
    auto n2 = std::make_shared<RemoteNode>(std::shared_ptr<hal::Connection>(std::move(*c2)), host + ":" + std::to_string(port));
    AIOS_TRY(n2->hello(token, self_, *host_));
    return n2;
  });
  if (node->kernel() == self_) return Result<RemoteAttachment>(Status::Conflict, "that node is this kernel");
  auto a = attach_remote_node(*this, node);
  if (a.ok()) nodes_.push_back(node);
  return a;
}

Result<void> Kernel::init(const KernelConfig& cfg) {
  cfg_ = cfg;
  host_ = hal::make_host();
  if (cfg_.root.empty()) return Result<void>(Status::Invalid, "kernel needs a persistent root");
  AIOS_TRY(host_->make_dir(cfg_.root));
  uint8_t entropy[8];
  host_->random(MutableByteSpan(entropy, 8));
  uint64_t e = 0; for (int i = 0; i < 8; ++i) e = (e << 8) | entropy[i];
  boot_ns_ = host_->wall_ns();
  boot_mono_ns_ = host_->monotonic_ns();
  ids_ = std::make_unique<IdGenerator>(e, boot_ns_);
  memory_ = std::make_unique<MemoryFabric>(*host_, *ids_);
  memory_->register_provider(make_system_ram_provider(*host_));
  memory_->register_provider(make_shared_memory_provider(*host_));
  memory_->register_provider(make_persistent_memory_provider(*host_, cfg_.root + "/memory"));
  caps_ = std::make_unique<CapabilityManager>(*ids_);
  states_ = std::make_unique<StateStore>(*ids_);
  entities_ = std::make_unique<EntityManager>(*ids_);
  channels_ = std::make_unique<ChannelManager>(*ids_, *memory_);
  channels_->register_transport(make_shared_memory_transport());
  channels_->register_transport(make_shm_fabric_transport());
  graphs_ = std::make_unique<GraphStore>(*ids_);
  devices_ = std::make_unique<DeviceFabric>();
  const auto& topo = host_->topology();
  devices_->add(make_cpu_device(ids_->next_as<DeviceID>(), topo.logical_cpus, topo.total_memory, host_.get()));
  // Named kernels every machine runs, so any fleet peer can place them here.
  for (auto* d : devices_->of_type(DeviceType::CPU)) {
    (void)sat::register_sat_kernels(*d);
#if !defined(AIOS_METAL)
    (void)erp::register_erp_kernels(*d);
#endif
  }
  // Accelerators join the fabric on first use, not at boot: mapping the OpenCL,
  // CUDA and Vulkan runtimes costs hundreds of MB, and a node that only ever
  // runs CPU work (ASQL) never asks for a device. See DeviceFabric::set_lazy_loader.
  devices_->set_lazy_loader([this] {
    // AIOS_NO_ACCEL: a CPU-only node. No runtime is opened for OpenCL, CUDA or
    // Vulkan — a statically linked build cannot dlopen one safely, and a
    // compute server with no card has nothing to find.
    const std::string no_accel_s = host_->env("AIOS_NO_ACCEL");
    const char* no_accel = no_accel_s.empty() ? nullptr : no_accel_s.c_str();
    if (no_accel && no_accel[0] && no_accel[0] != '0') {
      accelerators_.note = "accelerators disabled (AIOS_NO_ACCEL)";
    } else {
      accelerators_ = register_opencl_devices(*devices_, *memory_, *host_, *ids_);
      // CUDA, where there is a card and a driver for it. The backend used to be
      // constructed by hand at one command and never joined the fabric at all, so
      // nothing the machine scheduled could ever land on the GPU through it.
      {
        auto cuda = ::aios::compute::register_cuda_devices(*devices_, *memory_, *host_, *ids_);
        for (auto& name : cuda.devices) accelerators_.devices.push_back(name);
        // The note is kept even when some devices registered: "device 1:
        // cuCtxCreate: out of memory" is the whole story of a card another
        // process has filled, and dropping it made the card look absent.
        if (!cuda.note.empty()) {
          accelerators_.note += (accelerators_.note.empty() ? "" : "; ") + std::string("CUDA: ") + cuda.note;
        }
        auto vulkan = ::aios::compute::register_vulkan_devices(*devices_, *memory_, *host_, *ids_);
        for (auto& name : vulkan.devices) accelerators_.devices.push_back(name);
        // The note is kept even when some devices registered: "device 1:
        // cuCtxCreate: out of memory" is the whole story of a card another
        // process has filled, and dropping it made the card look absent.
        if (!vulkan.note.empty()) {
          accelerators_.note += (accelerators_.note.empty() ? "" : "; ") + std::string("Vulkan: ") + vulkan.note;
        }
        auto metal = ::aios::compute::register_metal_devices(*devices_, *memory_, *host_, *ids_);
        for (auto& name : metal.devices) accelerators_.devices.push_back(name);
        // The note is kept even when some devices registered: "device 1:
        // cuCtxCreate: out of memory" is the whole story of a card another
        // process has filled, and dropping it made the card look absent.
        if (!metal.note.empty()) {
          accelerators_.note += (accelerators_.note.empty() ? "" : "; ") + std::string("Metal: ") + metal.note;
        }
      }
    }
  });
  quotas_ = std::make_unique<PreemptiveSchedulerQuota>();
  scheduler_ = std::make_unique<Scheduler>(*host_, *devices_, *memory_, *caps_, *quotas_);
  // Scheduling capacity: how many movable jobs this node will run at once.
  // Defaults to logical CPUs; an operator can constrain it to model a small node.
  if (cfg_.slots > 0) slots_ = cfg_.slots;
  store_ = std::make_unique<ObjectStore>(*host_, *ids_, cfg_.root + "/store", cfg_.residency);
  AIOS_TRY(store_->open());
  residency_ = std::make_unique<ResidencyController>(*host_, *entities_, *store_);
  authority_ = std::make_unique<AuthorityLedger>(*host_, cfg_.root + "/authority.log");
  AIOS_TRY(authority_->open());
  watchdog_ = std::make_unique<KernelWatchdog>(*this);
  caps_->observe(this);
  AIOS_TRY(load_or_create_identity());
  load_or_create_node_key();
  {
    // Every link this process makes signs with this machine's key and checks peers against the hub's signed roster.
    auto id = std::make_shared<LinkIdentity>();
    if (node_key_ok_) id->sign = [this](ByteSpan m) { auto s = node_sign(m); return s.ok() ? ByteBuffer(s->begin(), s->end()) : ByteBuffer(); };
    id->peer_key = [this](const Id128& peer, std::array<uint8_t, 32>& key) { return relay_ && relay_->peer_key(peer, key); };
    id->peer_roles = [this](const Id128& peer) {
      std::array<uint8_t, 32> key{};
      return relay_ && relay_->peer_key(peer, key) ? relay_->peer_roles(peer) : std::string();
    };
    id->v2_only = host_->env("AIOS_FLEET_V2_ONLY") == "1";
    link_identity_ = id;
    set_link_identity(id);
  }
  AIOS_TRY(replay_authority());
  // Kept states (Surface::keep_state: a resident's, by its owner) live in the
  // root, one file per state, so the console's `kv` and `state create ...
  // persistent` are there on the next boot. Other entities do not survive a
  // boot, so neither does their state. Memory
  // references are dropped on the way back in: memory objects belong to the
  // life that made them.
  {
    const std::string dir = cfg_.root + "/states";
    if (auto names = host_->list_dir(dir); names.ok()) {
      for (auto& n : *names) {
        if (n.size() < 7 || n.compare(n.size() - 6, 6, ".state") != 0) continue;
        auto bytes = host_->read_file(dir + "/" + n);
        if (!bytes.ok()) continue;
        Reader r(*bytes);
        auto st = StateObject::decode(r);
        if (!st.ok() || !authority_->is_resident(st->owner) || !entities_->exists(st->owner)) continue;
        st->memory.clear();
        if (states_->get(st->id)) continue;
        StateObject restored = *st;
        if (states_->install(restored).ok()) {   // no persist hook yet: nothing is rewritten
          states_->set_durable(restored.id, true);
          entities_->add_state(restored.owner, restored.id);
        }
      }
    }
    states_->persist_with([this, dir](const StateID& id, const StateObject* now) {
      const std::string path = dir + "/" + id.str() + ".state";
      if (!now) { if (host_->exists(path)) (void)host_->remove(path); return; }
      if (!authority_ || !authority_->is_resident(now->owner)) return;
      (void)host_->make_dir(dir);
      ByteBuffer b; Writer w(b); now->encode(w);
      (void)host_->write_file(path, b);
    });
  }
  return Ok();
}

// ---- authority bookkeeping: the capability manager tells the kernel, the
// kernel tells the ledger (for residents) and keeps entity records in step.
void Kernel::issued(const Capability& c) {
  if (replaying_ || !authority_) return;
  if (authority_->is_resident(c.holder)) authority_->record_capability(c);
}
void Kernel::revoked(const Capability& c) {
  if (entities_) entities_->remove_capability(c.holder, c.id);
  if (replaying_ || !authority_) return;
  authority_->record_revoked(c.id);
}
void Kernel::note(AuthorityEvent::Kind kind, const EntityID& actor, const EntityID& subject, ResourceRef res, uint16_t rights, const CapabilityID& cap, const std::string& why) {
  if (!authority_) return;
  AuthorityEvent e; e.kind = kind; e.ns = now(); e.actor = actor; e.subject = subject; e.resource = res; e.rights = rights; e.capability = cap; e.note = why;
  authority_->record_event(e);
}

Result<void> Kernel::replay_authority() {
  replaying_ = true;
  for (auto& r : authority_->residents()) {
    if (entities_->exists(r.id)) continue;
    Entity e; e.id = r.id; e.label = r.label; e.created_ns = r.created_ns; e.status = EntityStatus::Active;
    entities_->install(e);
    entities_->relate(r.id, self_, Rel_Parent, now());
    entities_->relate(self_, r.id, Rel_Child, now());
  }
  // Capabilities over objects that did not survive the boot (memory, states,
  // channels of the previous life) are not replayed; class capabilities and
  // authority over entities/kernel that still exist are.
  std::vector<Capability> caps = authority_->capabilities();
  for (auto& c : caps) {
    if (!entities_->exists(c.holder)) continue;
    bool live = c.resource.id.is_null() || c.resource.kind == ResourceKind::Kernel ||
                (c.resource.kind == ResourceKind::Entity && entities_->exists(EntityID{c.resource.id}));
    if (!live) continue;
    if (caps_->install(c).ok()) entities_->add_capability(c.holder, c.id);
  }
  replaying_ = false;
  return Ok();
}

Result<EntityID> Kernel::resident(const std::string& label, const AuthorityProfile& profile, bool self_improve) {
  // Each grant of the profile is DERIVED from the kernel's own class capability
  // over that resource — the kernel gives from what it holds, like everyone.
  auto give = [&](const EntityID& id, const AuthorityGrant& g, const std::string& why) {
    CapabilityID source;
    for (auto& kc : caps_->list(self_)) if (kc.resource.covers(g.resource) && (kc.rights & R_Grant) && (g.rights & ~kc.rights) == 0) { source = kc.id; break; }
    Result<CapabilityID> c = source.is_null()
        ? caps_->grant(id, g.resource, g.rights, now(), 0, g.scope_limit)
        : caps_->delegate(source, id, g.rights, now(), 0, g.scope_limit, &g.resource);
    if (c.ok()) { entities_->add_capability(id, *c); note(AuthorityEvent::Derived, self_, id, g.resource, g.rights, *c, why); }
  };
  if (const Resident* r = authority_->resident(label)) {
    if (!entities_->exists(r->id)) {
      Entity e; e.id = r->id; e.label = r->label; e.created_ns = r->created_ns; e.status = EntityStatus::Active;
      AIOS_TRY(entities_->install(e));
    }
    // A resident outlives the binary that created it, and a newer binary may ask more of it than the one that made it
    // (a service that now hands out storage rights, say). Whatever the profile holds that this resident does not yet,
    // the kernel now derives for it, on the record. Only ever additive: authority the profile no longer lists stays
    // until someone revokes it, so a boot never takes away what a running program may be relying on.
    for (auto& g : profile) {
      if (!caps_->check(r->id, g.resource, g.rights, now())) give(r->id, g, "resident " + label + " now holds this (its profile grew)");
    }
    return r->id;
  }
  auto id = entities_->create(label, now());
  if (!id.ok()) return id.error();
  entities_->relate(*id, self_, Rel_Parent, now());
  entities_->relate(self_, *id, Rel_Child, now());
  authority_->record_resident(Resident{*id, label, now(), self_improve});
  for (auto& g : profile) give(*id, g, "resident " + label + " created holding this");
  return id;
}

// Governed self-improvement. The machine — not the entity — decides.
Result<CapabilityID> Kernel::petition(const EntityID& who, ResourceRef res, uint16_t rights, uint64_t ttl_ns, bool confirmed) {
  if (!entities_->exists(who)) return Result<CapabilityID>(Status::NotFound, "entity " + who.short_str());
  if (!authority_->may_self_improve(who)) {
    std::string why = "the machine refuses: self-improvement is reserved to residents it trusts; " + who.short_str() + " is not one — ask a resident that holds this to grant it";
    note(AuthorityEvent::Refused, who, who, res, rights, {}, why);
    return Result<CapabilityID>(Status::Denied, why);
  }
  // The hard wall: kernel OWNERSHIP is never granted, to anyone, ever.
  const uint16_t KERNEL_OWNERSHIP = R_Write | R_Destroy | R_Publish | R_Consume | R_Allocate;
  if (res.kind == ResourceKind::Kernel && (rights & KERNEL_OWNERSHIP)) {
    // The one exception: kernel:destroy over ONE named disk, single use, for
    // the console alone (an operator at it), and only when the request names
    // the disk back. A mind or any other resident is refused however it asks,
    // and the kernel's own identity is never a "disk".
    if (rights == R_Destroy && !res.id.is_null()) {
      const Resident* console = authority_->resident("console");
      if (!console || console->id != who || res.id == self_) {
        std::string why = "the machine refuses: kernel:destroy over a disk is granted only to the console, for a disk it names";
        note(AuthorityEvent::Refused, who, who, res, rights, {}, why);
        return Result<CapabilityID>(Status::Denied, why);
      }
      if (confirmed) {
        // Unused, it lapses: a confirmation is for now, not for later.
        const uint64_t ttl = ttl_ns && ttl_ns < 300000000000ull ? ttl_ns : 300000000000ull;
        auto c = caps_->grant(who, res, R_Destroy, now(), now() + ttl, 1);
        if (c.ok()) {
          entities_->add_capability(who, *c);
          note(AuthorityEvent::Minted, self_, who, res, R_Destroy, *c, "kernel:destroy granted for disk after explicit confirmation");
        }
        return c;
      } else {
        std::string why = "the machine refuses: destroying a disk needs it named back (confirm=<disk>)";
        note(AuthorityEvent::Refused, who, who, res, rights, {}, why);
        return Result<CapabilityID>(Status::Denied, why);
      }
    }
    std::string why = "the machine refuses: kernel ownership (" + rights_str(rights & KERNEL_OWNERSHIP) + ") is never granted — the kernel entity alone holds it";
    note(AuthorityEvent::Refused, who, who, res, rights, {}, why);
    return Result<CapabilityID>(Status::Denied, why);
  }
  // What the machine is willing to mint, bounded by policy per resource.
  uint16_t granted = rights; uint64_t scope = 0; uint64_t expiry = ttl_ns ? now() + ttl_ns : 0;
  switch (res.kind) {
    case ResourceKind::Kernel:
      granted = rights & (R_Read | R_Execute | R_Checkpoint | R_Schedule | R_Grant);   // access, never ownership
      break;
    case ResourceKind::Memory:
      if (rights & R_Allocate) scope = cfg_.default_memory_quota ? cfg_.default_memory_quota : (4ull << 30);   // a ceiling, never unbounded
      break;
    default: break;   // state/channel/compute/entity/storage/network: OS-internal reach, granted as asked
  }
  if (granted == 0) { std::string why = "the machine grants none of the requested rights over " + res.str(); note(AuthorityEvent::Refused, who, who, res, rights, {}, why); return Result<CapabilityID>(Status::Denied, why); }
  auto c = caps_->grant(who, res, granted, now(), expiry, scope);
  if (c.ok()) { entities_->add_capability(who, *c); note(AuthorityEvent::Minted, who, who, res, granted, *c, "the machine granted this petition under its self-improvement policy" + std::string(scope ? " (bounded)" : "")); }
  return c;
}

Result<void> Kernel::load_or_create_identity() {
  std::string path = cfg_.root + "/kernel.identity";
  auto existing = host_->read_file(path);
  if (existing.ok()) {
    Reader r(*existing);
    uint32_t magic = r.u32();
    Id128 id = r.id();
    if (magic != 0x4B534F41u || !r.ok()) return Result<void>(Status::Corrupt, "kernel identity file");
    self_ = EntityID{id};
  } else {
    self_ = ids_->next_as<EntityID>();
    ByteBuffer b; Writer w(b); w.u32(0x4B534F41u); w.id(self_);
    AIOS_TRY(host_->write_file(path, b));
  }
  Entity e;
  e.id = self_; e.label = "kernel"; e.created_ns = now(); e.status = EntityStatus::Active;
  AIOS_TRY(entities_->install(e));
  // The kernel entity is the first resident. It holds every class capability;
  // everything else is derived from those. Minted once per root, then replayed.
  if (!authority_->resident("kernel")) authority_->record_resident(Resident{self_, "kernel", now()});
  bool held = false;
  for (auto& c : authority_->capabilities()) if (c.holder == self_) { held = true; break; }
  if (!held) {
    for (auto kind : {ResourceKind::Memory, ResourceKind::ComputeDevice, ResourceKind::Channel, ResourceKind::StorageObject, ResourceKind::Entity, ResourceKind::Network, ResourceKind::State, ResourceKind::Kernel}) {
      auto c = caps_->grant(self_, ResourceRef{kind, {}}, R_All, now());
      if (c.ok()) { entities_->add_capability(self_, *c); note(AuthorityEvent::Minted, self_, self_, ResourceRef{kind, {}}, R_All, *c, "kernel root authority"); }
    }
  }
  return Ok();
}

// node.identity: a magic word and the 32-byte Ed25519 seed, created exclusive and private (never readable by
// others, not even for a moment). A file that is there but unreadable or malformed is left alone - replacing it
// would silently change who this machine is - and the kernel runs without a key.
static constexpr uint32_t kNodeIdentityMagic = 0x4449534Eu;  // "NSID"

void Kernel::load_or_create_node_key() {
  const std::string path = cfg_.root + "/node.identity";
  node_key_ok_ = false;
  if (host_->exists(path)) {
    auto d = host_->read_file(path);
    if (!d.ok()) { node_key_problem_ = path + ": " + d.error().message; return; }
    Reader r(*d);
    const uint32_t magic = r.u32();
    if (magic != kNodeIdentityMagic || d->size() != 4 + node_seed_.size()) { node_key_problem_ = path + ": not a node identity file (left as it is)"; return; }
    std::memcpy(node_seed_.data(), d->data() + 4, node_seed_.size());
  } else {
    host_->random(MutableByteSpan(node_seed_.data(), node_seed_.size()));
    ByteBuffer b; Writer w(b); w.u32(kNodeIdentityMagic);
    b.insert(b.end(), node_seed_.begin(), node_seed_.end());
    auto wr = host_->create_exclusive_private(path, b);
    std::fill(b.begin(), b.end(), 0);
    if (!wr.ok()) { node_seed_.fill(0); node_key_problem_ = path + ": " + wr.error().message; return; }
  }
  node_pub_ = crypto::ed25519_public_from_seed(node_seed_);
  node_key_ok_ = true;
  node_key_problem_.clear();
}

Result<crypto::Sig64> Kernel::node_sign(ByteSpan message) const {
  if (!node_key_ok_) return Result<crypto::Sig64>(Status::NotFound, "no node identity: " + node_key_problem_);
  return crypto::ed25519_sign(message, node_seed_, node_pub_);
}

uint64_t Kernel::now() const { return host_->wall_ns(); }
// Monotonic: the wall clock jumps (NTP's first sync on the Pi moved it from the
// build-date fallback to the real date - 22 days of "uptime" on a board up for
// a minute), and an uptime must not.
uint64_t Kernel::uptime_ns() const { return host_->monotonic_ns() - boot_mono_ns_; }

Result<EntityID> Kernel::spawn(const std::string& label, const EntityID& parent) {
  auto id = entities_->create(label, now());
  if (!id.ok()) return id.error();
  EntityID p = parent.is_null() ? self_ : parent;
  if (entities_->exists(p)) {
    entities_->relate(*id, p, Rel_Parent, now());
    entities_->relate(p, *id, Rel_Child, now());
  }
  // Baseline authority: bounded allocation, own state, own channels, storage read.
  auto c1 = caps_->grant(*id, ResourceRef{ResourceKind::Memory, {}}, R_Allocate, now(), 0, cfg_.default_memory_quota);
  auto c2 = caps_->grant(*id, ResourceRef{ResourceKind::State, {}}, R_Read | R_Write | R_Allocate | R_Destroy, now());
  auto c3 = caps_->grant(*id, ResourceRef{ResourceKind::Channel, {}}, R_Allocate, now());
  auto c4 = caps_->grant(*id, ResourceRef{ResourceKind::StorageObject, {}}, R_Read, now());
  for (auto* c : {&c1, &c2, &c3, &c4}) if (c->ok()) entities_->add_capability(*id, **c);
  return id;
}

Result<void> Kernel::terminate(const EntityID& id) {
  if (id == self_) return Result<void>(Status::Denied, "the kernel entity cannot be terminated from inside");
  if (authority_->is_resident(id)) return Result<void>(Status::Denied, "entity " + id.short_str() + " is a resident: the kernel keeps it across boots");
  if (!entities_->exists(id)) return Result<void>(Status::NotFound, "entity " + id.short_str());
  unbind_mind(id);
  // Its memory goes back to whichever budgets paid for it (a parent that
  // delegated it scope gets that scope back).
  if (const Entity* ent = entities_->get(id)) for (auto& m : ent->memory) caps_->refund_object(m);
  channels_->destroy_owned(id);
  graphs_->destroy_owned(id);
  states_->destroy_owned(id);
  memory_->release_owned(id);
  caps_->revoke_all(id);
  entities_->set_status(id, EntityStatus::Terminated);
  return entities_->remove(id);
}

Result<CapabilityID> Kernel::grant(const EntityID& holder, ResourceRef res, uint16_t rights, uint64_t ttl_ns, uint64_t scope_limit) {
  if (!entities_->exists(holder)) return Result<CapabilityID>(Status::NotFound, "entity " + holder.short_str());
  if (res.kind == ResourceKind::Kernel && (rights & R_Destroy) && !res.id.is_null() && scope_limit == 0) {
    scope_limit = 1;
  }
  auto c = caps_->grant(holder, res, rights, now(), ttl_ns ? now() + ttl_ns : 0, scope_limit);
  if (c.ok()) {
    entities_->add_capability(holder, *c);
    if (res.kind == ResourceKind::Kernel && (rights & R_Destroy)) {
      note(AuthorityEvent::Minted, self_, holder, res, rights, *c, "kernel:destroy granted after explicit confirmation");
    }
  }
  return c;
}

Result<CapabilityID> Kernel::grant_destroy(const EntityID& holder, const Id128& disk_id, bool confirmed) {
  if (!entities_->exists(holder)) return Result<CapabilityID>(Status::NotFound, "entity " + holder.short_str());
  ResourceRef res{ResourceKind::Kernel, disk_id};
  if (!confirmed) {
    std::string why = "the kernel refuses: destruction requires explicit confirmation";
    note(AuthorityEvent::Refused, self_, holder, res, R_Destroy, {}, why);
    return Result<CapabilityID>(Status::Denied, why);
  }
  auto c = caps_->grant(holder, res, R_Destroy, now(), 0, 1);
  if (c.ok()) {
    entities_->add_capability(holder, *c);
    note(AuthorityEvent::Minted, self_, holder, res, R_Destroy, *c, "kernel:destroy granted after explicit confirmation");
  }
  return c;
}

Result<CapabilityID> Kernel::grant_destroy(const EntityID& holder, const std::string& disk, bool confirmed) {
  if (!entities_->exists(holder)) return Result<CapabilityID>(Status::NotFound, "entity " + holder.short_str());
  Id128 d_id = disk_id(disk);
  ResourceRef res{ResourceKind::Kernel, d_id};
  if (!confirmed) {
    std::string why = "the kernel refuses: destruction of disk '" + disk + "' requires explicit confirmation";
    note(AuthorityEvent::Refused, self_, holder, res, R_Destroy, {}, why);
    return Result<CapabilityID>(Status::Denied, why);
  }
  auto c = caps_->grant(holder, res, R_Destroy, now(), 0, 1);
  if (c.ok()) {
    entities_->add_capability(holder, *c);
    note(AuthorityEvent::Minted, self_, holder, res, R_Destroy, *c, "kernel:destroy granted for disk '" + disk + "' after explicit confirmation");
  }
  return c;
}

Result<CapabilityID> Kernel::grant_destroy(const EntityID& holder, const std::string& disk, const std::string& confirmation) {
  // Confirmation is the disk named back, nothing looser.
  const bool confirmed = !disk.empty() && (confirmation == disk || confirmation == "confirm=" + disk);
  return grant_destroy(holder, disk, confirmed);
}

Result<void> Kernel::consume_destroy(const EntityID& holder, const std::string& disk) {
  return consume_destroy(holder, disk_id(disk), disk);
}

Result<void> Kernel::consume_destroy(const EntityID& holder, const Id128& disk_id, const std::string& disk_name) {
  ResourceRef target{ResourceKind::Kernel, disk_id};
  const Capability* found = nullptr;
  for (const auto& c : caps_->list(holder)) {
    if (c.resource.covers(target) && (c.rights & R_Destroy)) {
      if (c.expires_ns && now() >= c.expires_ns) continue;
      if (c.scope_limit && c.scope_used >= c.scope_limit) continue;
      found = &c;
      break;
    }
  }
  if (!found) {
    std::string name = disk_name.empty() ? target.str() : disk_name;
    return Result<void>(Status::Denied, "the kernel refuses: entity " + holder.short_str() + " holds no destroy capability over " + name);
  }
  CapabilityID cid = found->id;
  ResourceRef cres = found->resource;

  if (found->scope_limit > 0) {
    caps_->charge(holder, cres, R_Destroy, 1, now());
  }
  // Expire immediately so the console cannot destroy anything else
  if (!cres.id.is_null() || found->scope_limit > 0) {
    caps_->revoke(cid);
  }

  std::string name = disk_name.empty() ? cres.str() : disk_name;
  note(AuthorityEvent::Revoked, holder, holder, cres, R_Destroy, cid, "consumed single-use destroy capability for disk " + name);
  return Ok();
}

Result<void> Kernel::revoke(const CapabilityID& id) {
  const Capability* c = caps_->get(id);
  if (!c) return Result<void>(Status::NotFound, "capability " + id.short_str());
  return caps_->revoke(id);   // the observer keeps entity records in step, cascade included
}

Result<EntityID> Kernel::migrate(const EntityID& id, size_t node_index, bool keep_local) {
  if (id == self_) return Result<EntityID>(Status::Denied, "the kernel entity cannot migrate");
  if (authority_->is_resident(id)) return Result<EntityID>(Status::Denied, "a resident is bound to this machine; it cannot migrate");
  if (node_index >= nodes_.size()) return Result<EntityID>(Status::NotFound, "no peer node #" + std::to_string(node_index) + " (connect one first)");
  auto img = capture(id);
  if (!img.ok()) return img.error();
  ByteBuffer body; Writer w(body); w.bytes(*img);
  auto reply = nodes_[node_index]->call(10 /*nodeproto::Migrate*/, body);
  if (!reply.ok()) return reply.error();
  Reader r(*reply); EntityID remote{r.id()};
  if (!r.ok()) return Result<EntityID>(Status::Corrupt, "peer did not return the migrated identity");
  if (!keep_local) { auto t = terminate(id); if (!t.ok()) return t.error(); }
  return remote;
}

// ---- FLEET: telemetry and recovery points ---------------------------------

uint32_t Kernel::slots() const {
  return slots_ ? slots_ : host_->topology().logical_cpus;
}

NodeTelemetry Kernel::telemetry() const {
  const auto& topo = host_->topology();
  NodeTelemetry t;
  t.kernel = self_;
  t.logical_cpus = topo.logical_cpus;
  t.slots = slots();
  t.total_memory = topo.total_memory;
  t.available_memory = topo.available_memory;
  // Real host memory pressure, not the fabric's own MemoryObject byte count
  // (memory_->stats().bytes) - nothing today allocates distributed job
  // memory through that fabric, so it reads zero on every real host,
  // including one running a live, memory-hungry compute/CDC process. That
  // silently zeroed pressure()'s memory axis fleet-wide (Orchestrator
  // placement decisions effectively ran on job-count alone) and made every
  // Fleet-tab "Memory in use" column read 0/<total> regardless of the
  // machine's actual load. Falls back to the fabric figure only if the host
  // genuinely can't report real usage (available > total, or total == 0).
  t.memory_in_use = (topo.total_memory > 0 && topo.available_memory <= topo.total_memory)
                         ? (topo.total_memory - topo.available_memory)
                         : memory_->stats().bytes;
  // Movable load: alive entities that are neither the kernel, a bound resident,
  // nor a peer's guest session. These are the jobs the scheduler may place.
  uint32_t movable = 0;
  for (const auto& e : entities_->list()) {
    if (e.id == self_) continue;
    if (authority_->is_resident(e.id)) continue;
    if (e.status == EntityStatus::Terminated) continue;
    if (e.label.rfind("peer:", 0) == 0) continue;
    ++movable;
  }
  t.movable_entities = movable;
  t.heartbeat = ++heartbeat_;
  t.wall_ns = host_->wall_ns();
  // What the devices actually run: each one's registered kernels, by name
  // (a kernel registered without a noted name shows as its hash).
  for (auto* dev : devices_->all_loaded()) {
    for (uint64_t h : dev->custom_kernels()) {
      std::string name = custom_kernel_name(h);
      if (name.empty()) { char b[24]; std::snprintf(b, sizeof b, "kernel:%016llx", (unsigned long long)h); name = b; }
      if (std::find(t.custom_kernels.begin(), t.custom_kernels.end(), name) == t.custom_kernels.end()) t.custom_kernels.push_back(name);
    }
  }
  {
    hal::LockGuard g(hosted_dbs_mu_);
    t.hosted_databases = hosted_dbs_;
  }
  return t;
}

void Kernel::register_hosted_db(const NodeTelemetry::HostedDbInfo& db) {
  hal::LockGuard g(hosted_dbs_mu_);
  for (auto& existing : hosted_dbs_) {
    if (existing.db_name == db.db_name) {
      existing = db;
      return;
    }
  }
  hosted_dbs_.push_back(db);
}

void Kernel::unregister_hosted_db(const std::string& db_name) {
  hal::LockGuard g(hosted_dbs_mu_);
  for (auto it = hosted_dbs_.begin(); it != hosted_dbs_.end(); ++it) {
    if (it->db_name == db_name) {
      hosted_dbs_.erase(it);
      return;
    }
  }
}

void Kernel::clear_hosted_dbs() {
  hal::LockGuard g(hosted_dbs_mu_);
  hosted_dbs_.clear();
}

std::vector<NodeTelemetry::HostedDbInfo> Kernel::hosted_databases() const {
  hal::LockGuard g(hosted_dbs_mu_);
  return hosted_dbs_;
}

Result<void> Kernel::store_replica(const Id128& source_kernel, const EntityID& entity, uint64_t generation, ByteSpan image) {
  hal::LockGuard g(replicas_mu_);
  for (size_t i = 0; i < replica_meta_.size(); ++i) {
    if (replica_meta_[i].source_kernel == source_kernel && replica_meta_[i].entity == entity) {
      replica_meta_[i].generation = generation;
      replica_meta_[i].received_ns = host_->wall_ns();
      replica_meta_[i].bytes = image.size();
      replica_images_[i].assign(image.begin(), image.end());
      return Ok();
    }
  }
  Replica r; r.source_kernel = source_kernel; r.entity = entity; r.generation = generation;
  r.received_ns = host_->wall_ns(); r.bytes = image.size();
  replica_meta_.push_back(r);
  replica_images_.emplace_back(image.begin(), image.end());
  return Ok();
}

std::vector<Kernel::Replica> Kernel::replicas() const {
  hal::LockGuard g(replicas_mu_);
  return replica_meta_;
}

bool Kernel::holds_replica(const EntityID& entity) const {
  hal::LockGuard g(replicas_mu_);
  for (const auto& r : replica_meta_) if (r.entity == entity) return true;
  return false;
}

Result<EntityID> Kernel::recover_replica(const EntityID& entity) {
  ByteBuffer image;
  {
    hal::LockGuard g(replicas_mu_);
    size_t idx = replica_meta_.size();
    for (size_t i = 0; i < replica_meta_.size(); ++i) if (replica_meta_[i].entity == entity) { idx = i; break; }
    if (idx == replica_meta_.size()) return Result<EntityID>(Status::NotFound, "no recovery point for " + entity.short_str());
    image = replica_images_[idx];
    replica_meta_.erase(replica_meta_.begin() + idx);
    replica_images_.erase(replica_images_.begin() + idx);
  }
  // Restored verbatim: the kernel itself is the restorer, so identity, state,
  // memory, capabilities and execution position come back exactly as the dead
  // node checkpointed them. The job reappears — it is not a new job.
  return restore_image(image);
}

// ---- FLEET OVERLAY: one hub, one key, the rest discovered -----------------

Result<uint16_t> Kernel::relay_hub(uint16_t port, const std::string& cluster_key, bool loopback_only,
                                   const std::string& members_file, bool require_attest) {
  if (cluster_key.size() < 16) return Result<uint16_t>(Status::Invalid, "cluster key must be at least 16 characters");
  if (hub_) return Result<uint16_t>(Status::Busy, "already hosting a hub on port " + std::to_string(hub_->port()));
  if (require_attest && members_file.empty())
    return Result<uint16_t>(Status::Invalid, "attest=require needs members=<file>: each member's attestation key is listed there");
  hub_ = std::make_unique<RelayHub>(cluster_key);
  if (!members_file.empty()) {
    if (!host_->exists(members_file)) { hub_.reset(); return Result<uint16_t>(Status::NotFound, members_file + ": no members file"); }
    hub_->set_members_file(members_file);
  }
  hub_->set_require_attest(require_attest);
  if (node_key_ok_) hub_->set_signer([this](ByteSpan m) { auto s = node_sign(m); return s.ok() ? ByteBuffer(s->begin(), s->end()) : ByteBuffer(); });
  auto p = hub_->start(port, loopback_only);
  if (!p.ok()) { hub_.reset(); return p.error(); }
  cluster_key_ = cluster_key;
  return *p;
}

Result<void> Kernel::fleet_join(const std::string& hub_host, uint16_t hub_port, const std::string& cluster_key, bool keep_trying,
                                const std::string& member_key, bool attest) {
  if (cluster_key.size() < 16) return Result<void>(Status::Invalid, "cluster key must be at least 16 characters");
  if (relay_ && relay_->attached()) return Result<void>(Status::Busy, "already attached to a fleet hub");
  // A join still trying (or one whose link dropped) is replaced: its accept
  // thread holds a pointer to the old client, so it stops first.
  if (relay_) {
    relay_accepting_ = false;
    if (relay_accept_.joinable()) relay_accept_.join();
    relay_->stop();
    relay_.reset();
  }
  cluster_key_ = cluster_key;
  relay_ = std::make_unique<RelayClient>(cluster_key, self_);
  if (!member_key.empty()) relay_->set_member_key(member_key);
  relay_->set_attest(attest);
  if (have_hub_key_) relay_->set_hub_key(hub_key_);
  if (node_key_ok_) relay_->set_signer([this](ByteSpan m) { auto s = node_sign(m); return s.ok() ? ByteBuffer(s->begin(), s->end()) : ByteBuffer(); });
  auto a = relay_->attach(hub_host, hub_port, keep_trying);
  if (!a.ok()) { relay_.reset(); return a.error(); }
  // Peers that open a circuit to us become guests exactly like a direct peer.
  // They present the CLUSTER key: a service already serving a `node serve`
  // token must accept that too, or every peer is turned away at the handshake
  // and this machine is unreachable through the hub.
  if (!service_) service_ = std::make_unique<NodeService>(*this, cluster_key);
  else service_->add_token(cluster_key);
  service_->enable();
  relay_accepting_ = true;
  RelayClient* rc = relay_.get();
  NodeService* svc = service_.get();
  std::atomic<bool>* run = &relay_accepting_;
  relay_accept_ = hal::Thread([rc, svc, run] {
    while (run->load()) {
      auto c = rc->accept_incoming();
      if (c) svc->serve_connection(std::shared_ptr<hal::Connection>(std::move(c)));
      else hal::sleep_ms(20);
    }
  });
  return Ok();
}

void Kernel::add_direct_route(const std::string& member_prefix, const std::string& host, uint16_t port) {
  for (auto& d : direct_routes_) if (d.member == member_prefix) { d.host = host; d.port = port; return; }
  direct_routes_.push_back(DirectRoute{member_prefix, host, port});
}

Result<std::shared_ptr<RemoteNode>> Kernel::open_member(const Id128& kernel_id) {
  using R = Result<std::shared_ptr<RemoteNode>>;
  // The link is named for the member ("relay:<id>", its fleet name), not for the way it is carried: devices,
  // selectors and the remote memory provider keep their names when a direct link is replaced by a hub circuit.
  const std::string name = "relay:" + kernel_id.short_str();
  std::string direct_err;
  for (const auto& d : direct_routes_) {
    if (kernel_id.str().compare(0, d.member.size(), d.member) != 0) continue;
    auto c = hal::net_connect(d.host, d.port, 3000);
    if (!c.ok()) { direct_err = c.error().message; break; }
    auto n = std::make_shared<RemoteNode>(std::shared_ptr<hal::Connection>(std::move(*c)), name);
    auto h = n->hello(cluster_key_, self_, *host_);
    if (!h.ok()) { direct_err = h.error().message; break; }
    if (n->kernel() != kernel_id) { n->close(); direct_err = "a different kernel (" + n->kernel().short_str() + ") answered there"; break; }
    return n;
  }
  if (!direct_err.empty())
    std::fprintf(stderr, "fleet: direct link to %s failed (%s); through the hub\n", kernel_id.short_str().c_str(), direct_err.c_str());
  if (!relay_ || !relay_->attached())
    return R(Status::Busy, "not attached to a fleet hub" + (direct_err.empty() ? std::string() : "; direct link: " + direct_err));
  auto conn = relay_->open(kernel_id);
  if (!conn.ok()) return conn.error();
  auto n = std::make_shared<RemoteNode>(std::shared_ptr<hal::Connection>(std::move(*conn)), name);
  // Same token-authenticated, end-to-end-encrypted handshake as a direct link — the hub only ever sees ciphertext.
  auto h = n->hello(cluster_key_, self_, *host_);
  if (!h.ok()) return h.error();
  return n;
}

Result<Id128> Kernel::find_member(const std::string& prefix) {
  auto starts = [&prefix](const Id128& id) { return !prefix.empty() && id.str().compare(0, prefix.size(), prefix) == 0; };
  std::vector<Id128> hits;
  for (auto& id : fleet_roster()) if (starts(id)) hits.push_back(id);
  if (hits.empty())   // no hub (or it does not list it): a member already linked is still that member
    for (auto& n : nodes_) if (starts(n->kernel()) && std::find(hits.begin(), hits.end(), n->kernel()) == hits.end()) hits.push_back(n->kernel());
  if (hits.empty()) {   // never linked: its direct route says who it is
    for (const auto& d : direct_routes_) {
      if (d.member.compare(0, prefix.size(), prefix) != 0 && prefix.compare(0, d.member.size(), d.member) != 0) continue;
      auto c = hal::net_connect(d.host, d.port, 3000);
      if (!c.ok()) continue;
      RemoteNode probe(std::shared_ptr<hal::Connection>(std::move(*c)), d.host);
      if (probe.hello(cluster_key_, self_, *host_).ok() && starts(probe.kernel())) hits.push_back(probe.kernel());
      probe.close();
    }
  }
  if (hits.empty()) return Result<Id128>(Status::NotFound, "no fleet member " + prefix);
  if (hits.size() > 1) return Result<Id128>(Status::Conflict, "more than one fleet member starts with " + prefix);
  return hits[0];
}

Result<size_t> Kernel::reach(const Id128& kernel_id) {
  if (kernel_id == self_) return Result<size_t>(Status::Invalid, "that identity is this kernel");
  long stale = -1;
  for (size_t i = 0; i < nodes_.size(); ++i) if (nodes_[i]->kernel() == kernel_id) {
    if (nodes_[i]->alive()) return i;   // live link — reuse it
    stale = (long)i; break;             // dead link (hub restart / blip) — replace in place below
  }
  auto opened = open_member(kernel_id);
  if (!opened.ok()) return opened.error();
  auto node = *opened;
  node->set_opener([this, kernel_id]() { return open_member(kernel_id); });
  auto at = attach_remote_node(*this, node);
  if (!at.ok()) return at.error();
  if (stale >= 0) { nodes_[(size_t)stale] = node; return (size_t)stale; }   // swap the dead node out, keep index stable
  nodes_.push_back(node);
  return nodes_.size() - 1;
}

void Kernel::fleet_sync() {
  if (!relay_ || !relay_->attached()) return;
  for (auto& id : relay_->roster()) {
    if (id == self_) continue;
    bool have = false;
    for (auto& n : nodes_) if (n->kernel() == id && n->alive()) { have = true; break; }
    if (!have) reach(id);   // best-effort; re-reaches a peer whose link died on a hub restart
  }
}

Surface Kernel::as(const EntityID& entity) {
  // Untrusted escalation guard: verify that the entity exists in the registry
  // and is not a null or forged entity ID.
  if (entity.is_null() || (entities_ && !entities_->exists(entity))) {
    // If an invalid/non-existent entity is requested, surface falls back to an empty entity
    // which has 0 capabilities and cannot execute privileged kernel operations.
    return Surface(*this, EntityID{});
  }
  return Surface(*this, entity);
}

Result<void> Kernel::bind_mind(const EntityID& id, std::unique_ptr<Intelligence> mind, const ChannelID& output) {
  if (!entities_->exists(id)) return Result<void>(Status::NotFound, "entity " + id.short_str());
  if (!mind) return Result<void>(Status::Invalid, "no intelligence");

  bool exists = false;
  {
    hal::LockGuard g(minds_mu_);
    if (minds_.find(id) != minds_.end()) exists = true;
  }
  if (exists) {
    return swap_mind(id, std::move(mind), output);
  }

  // The canonical context: the entity's newest Context state, or a fresh one.
  // Intelligences may create Context states of their own; the cycle reads and
  // writes only this one, so successive minds see each other's results.
  StateID context;
  for (auto& st : states_->list(id)) if (st.kind == StateKind::Context) context = st.id;
  {
    hal::LockGuard g(minds_mu_);
    auto it = minds_.find(id);
    if (it != minds_.end() && !it->second.context.is_null() && states_->get(it->second.context)) context = it->second.context;
  }
  if (context.is_null()) {
    auto c = as(id).create_state(StateKind::Context);
    if (!c.ok()) return c.error();
    context = *c;
  }
  hal::LockGuard g(minds_mu_);
  MindBinding b;
  b.mind = std::move(mind);
  b.context = context;
  b.output = output;
  minds_[id] = std::move(b);
  return Ok();
}

Result<void> Kernel::swap_mind(const EntityID& id, std::unique_ptr<Intelligence> new_mind, const ChannelID& output) {
  if (!entities_->exists(id)) return Result<void>(Status::NotFound, "entity " + id.short_str());
  if (!new_mind) return Result<void>(Status::Invalid, "no intelligence");

  MindBinding* b = nullptr;
  hal::Mutex* mind_lock = nullptr;
  {
    hal::LockGuard g(minds_mu_);
    auto it = minds_.find(id);
    if (it == minds_.end()) {
      return bind_mind(id, std::move(new_mind), output);
    }
    b = &it->second;
    mind_lock = b->lock.get();
  }

  hal::LockGuard mg(*mind_lock);
  hal::LockGuard g(minds_mu_);
  b->mind = std::move(new_mind);
  if (!output.is_null()) {
    b->output = output;
  }
  return Ok();
}

Result<void> Kernel::unbind_mind(const EntityID& id) {
  hal::LockGuard g(minds_mu_);
  minds_.erase(id);
  return Ok();
}

const MindBinding* Kernel::mind(const EntityID& id) const {
  hal::LockGuard g(minds_mu_);
  auto it = minds_.find(id);
  return it == minds_.end() ? nullptr : &it->second;
}

Result<CycleReport> Kernel::cycle(const EntityID& id) {
  MindBinding* b;
  hal::Mutex* mind_lock;
  {
    hal::LockGuard g(minds_mu_);
    auto it = minds_.find(id);
    if (it == minds_.end()) return Result<CycleReport>(Status::NotFound, "entity " + id.short_str() + " has no bound intelligence");
    b = &it->second;
    mind_lock = b->lock.get();
  }
  // Serialise the whole perceive→think→act sequence for this mind. Two callers
  // hitting the same mind at once would interleave three-round-trip exchanges on
  // one pipe and corrupt the channel ("wrong frame type"). Taken after releasing
  // minds_mu_ so a slow cycle blocks only other cycles of the SAME mind, never
  // binding, status, or other minds.
  hal::LockGuard mg(*mind_lock);
  uint64_t t0 = host_->monotonic_ns();
  CycleReport rep;
  rep.entity = id;
  rep.intelligence = b->mind->name();
  Surface s = as(id);

  // 1. Context: the canonical Context state chosen at bind time.
  auto ctx = states_->copy(b->context);
  if (!ctx.ok()) return Result<CycleReport>(Status::NotFound, "the entity's context state no longer exists");
  StateObject context = *ctx;
  rep.context = context.id;

  // 2. Perceive.
  auto perceived = b->mind->perceive(s, context);
  if (!perceived.ok()) return Error{perceived.status(), "perceive: " + perceived.error().message};
  rep.perceived = *perceived;
  auto pstate = states_->copy(*perceived);
  if (!pstate.ok()) return Result<CycleReport>(Status::Invalid, "intelligence returned an unknown state from perceive");

  // 3. Think → a graph the kernel schedules.
  auto graph = b->mind->think(s, *pstate);
  if (!graph.ok()) return Error{graph.status(), "think: " + graph.error().message};
  rep.graph = *graph;
  ComputeGraph gcopy;
  bool have_graph = false;
  if (!graph->is_null()) {
    auto g = graphs_->copy(*graph);
    if (!g.ok()) return Result<CycleReport>(Status::Invalid, "intelligence returned an unknown graph from think");
    gcopy = *g;
    have_graph = !gcopy.operations.empty();
  }
  if (have_graph) {
    auto ex = s.execute(*graph);
    if (!ex.ok()) return Error{ex.status(), "execute: " + ex.error().message};
    rep.execution = *ex;
    rep.computed = true;
    if (!ex->failure.empty()) return Result<CycleReport>(Status::IoError, "execute: " + ex->failure);
  }

  // 4. Act → the next state.
  auto result = b->mind->act(s, *pstate, have_graph ? &gcopy : nullptr, rep.computed ? &rep.execution : nullptr);
  if (!result.ok()) return Error{result.status(), "act: " + result.error().message};
  rep.result = *result;
  if (!states_->get(*result)) return Result<CycleReport>(Status::Invalid, "intelligence returned an unknown state from act");

  // 5. The result becomes the entity's context for the next cycle and is
  //    published into its output channel if one is bound.
  {
    auto rs = states_->copy(*result);
    if (rs.ok()) {
      s.set_state(context.id, rs->payload);
      for (auto& m : rs->memory) s.attach(context.id, m);
    }
  }
  if (!b->output.is_null()) {
    auto pub = s.publish(b->output, *result);
    if (pub.ok()) { rep.published = true; rep.published_sequence = *pub; }
  }
  b->cycles++;
  rep.elapsed_ns = host_->monotonic_ns() - t0;
  return rep;
}

}  // namespace aios
