#pragma once
// The AIOS KERNEL API: every operation an entity (or the intelligence bound to
// it) can request of the kernel. Each call is authorised by the entity's
// capabilities. There are no tools here; there are primitives.
#include "aios/kernel/kernel.hpp"

namespace aios {

class Surface {
 public:
  const EntityID& self() const { return who_; }
  Kernel& kernel() { return k_; }
  uint64_t now() const { return k_.now(); }

  // Memory
  Result<MemoryID> allocate(uint64_t size, MemoryType type = MemoryType::System);
  Result<void> write(const MemoryID& m, uint64_t offset, ByteSpan data);
  Result<ByteBuffer> read(const MemoryID& m, uint64_t offset, uint64_t length);
  Result<void> release(const MemoryID& m);
  Result<void> migrate(const MemoryID& m, MemoryType to);
  Result<CapabilityID> share(const MemoryID& m, const EntityID& to, uint16_t rights);   // delegate rights over memory
  Result<const MemoryObject*> describe(const MemoryID& m);

  // Tensors are views: allocation + description.
  Result<Tensor> tensor(DType dtype, const std::vector<uint64_t>& shape, MemoryType type = MemoryType::System);

  // State
  Result<StateID> create_state(StateKind kind, ByteSpan payload = {});
  Result<StateObject> state(const StateID& s);
  Result<void> set_state(const StateID& s, ByteSpan payload);
  // Keep a state across boots, in <root>/states/. Only a resident's state can
  // be kept (no other entity outlives the process), and only by its owner.
  Result<void> keep_state(const StateID& s);
  // Remove one of this entity's own states, and its file in the root if it was kept.
  Result<void> drop_state(const StateID& s);
  Result<void> attach(const StateID& s, const MemoryID& m);
  Result<uint64_t> apply_state_delta(const StateID& s, StateKind kind, uint64_t target_gen, ByteSpan payload, const std::vector<MemoryID>& mems);
  std::vector<StateObject> states();

  // Channels
  Result<ChannelID> create_channel(uint32_t capacity, uint64_t slot_bytes, const std::string& transport = "shared-memory");
  Result<void> subscribe(const ChannelID& c);
  Result<uint64_t> publish(const ChannelID& c, const StateID& s);
  Result<StateFrame> consume(const ChannelID& c);
  Result<CapabilityID> admit(const ChannelID& c, const EntityID& to, uint16_t rights);   // let another entity publish/consume

  // Compute
  Result<GraphID> create_graph();
  Result<void> destroy_graph(const GraphID& g);
  Result<OperationID> add_operation(const GraphID& g, Operation op);
  Result<ExecutionReport> execute(const GraphID& g, const SchedulePolicy& policy = {});
  Result<std::vector<Placement>> plan(const GraphID& g);

  // Relationships
  Result<void> relate(const EntityID& peer, uint32_t kind);

  // Storage
  Result<ObjectID> store(StoredObject obj);
  Result<StoredObject> fetch(const ObjectID& id);
  Result<ObjectID> snapshot(const StateID& s, std::vector<float> embedding = {});

  // ---- Authority: the kernel decides, from what this entity already holds ----
  // Delegation of one specific capability this entity holds.
  Result<CapabilityID> delegate(const CapabilityID& cap, const EntityID& to, uint16_t rights, uint64_t ttl_ns = 0);
  // Give `to` authority over `res`. Nothing is minted: the kernel looks for a
  // capability THIS entity holds that covers res, carries grant, and holds
  // every requested right, and derives the new one from it. Otherwise it
  // refuses and says why. An entity cannot give what it does not have — to
  // anyone, itself included.
  Result<CapabilityID> grant(const EntityID& to, ResourceRef res, uint16_t rights, uint64_t ttl_ns = 0, uint64_t scope_limit = 0);
  // "I need X." The kernel answers from current authority: the capability that
  // already permits X, a narrower one derived from what is held, or a refusal.
  // Every request and every refusal is on the record.
  Result<CapabilityID> request(ResourceRef res, uint16_t rights, uint64_t ttl_ns = 0, bool confirmed = false);
  // Revoke a capability this entity holds, or one that descends from one it holds.
  Result<void> revoke(const CapabilityID& cap);
  std::vector<Capability> authority() const;                  // what this entity holds
  Result<void> may(ResourceRef res, uint16_t rights) const;   // the check alone, as the kernel would apply it
  // The check, recorded: an action carried out on this entity's behalf by something outside the kernel (a broker
  // editing a file, running a command) asks here first. Allowed when this entity holds a capability covering
  // (res, rights); refused (Denied) when not. Either way a ledger event (Exercised / Refused) carries `note`, so
  // what an entity was permitted and what it was stopped from doing is on the record.
  Result<void> exercise(ResourceRef res, uint16_t rights, const std::string& note = "");

  // ---- Acting through others ----
  // A Surface for another entity, if this one holds execute over it. The
  // kernel entity acts only for itself: no entity ever acts as the kernel.
  Result<Surface> act_as(const EntityID& e);

  // ---- Entities ----
  Result<EntityID> spawn(const std::string& label, const EntityID& parent = {});   // entity:allocate
  // A resident of this root (the same identity every boot) holding `profile`:
  // entity:allocate, and this entity must itself hold every right the profile
  // confers - nobody hands out authority it does not have. An existing
  // resident of that label is returned as it is.
  Result<EntityID> resident(const std::string& label, const AuthorityProfile& profile);
  Result<void> terminate(const EntityID& e);                                        // entity(e):destroy

  // ---- Persistence ----
  Result<std::string> checkpoint();                        // of self: kernel:checkpoint
  Result<std::string> checkpoint_of(const EntityID& e);    // kernel:checkpoint + act-as e
  Result<EntityID> restore(const std::string& path);       // kernel:checkpoint + entity:allocate; authority re-derived from this entity's
  Result<EntityID> rewind(const std::string& path);        // the same, over an entity that is still running: identity kept, contents replaced

  // ---- Distribution ----
  Result<uint16_t> serve(uint16_t port, const std::string& token, bool polled = false, bool loopback_only = false);  // network:publish
  Result<RemoteAttachment> connect(const std::string& host, uint16_t port, const std::string& token);
  // Fleet overlay: host the hub, or join a fleet through one. Membership and
  // reachability without hardcoded peers.
  Result<uint16_t> relay_hub(uint16_t port, const std::string& cluster_key, bool loopback_only = false,
                             const std::string& members_file = {}, bool require_attest = false);  // network:publish
  Result<void> fleet_join(const std::string& hub_host, uint16_t hub_port, const std::string& cluster_key, bool keep_trying = false,
                          const std::string& member_key = {}, bool attest = false);  // network:publish + consume
  // Move an entity to a connected peer kernel (checkpoint here, restore there, continue).
  Result<EntityID> migrate(const EntityID& e, size_t node_index, bool keep_local = false);   // network:consume

  // ---- Minds ----
  Result<void> bind_mind(const EntityID& e, std::unique_ptr<Intelligence> mind, const ChannelID& output = {});   // act-as e
  Result<void> swap_mind(const EntityID& e, std::unique_ptr<Intelligence> new_mind, const ChannelID& output = {}); // act-as e
  Result<void> unbind_mind(const EntityID& e);
  Result<CycleReport> cycle(const EntityID& e);

  // ---- The kernel itself ----
  Result<void> inspect() const;   // kernel:read     — see the kernel's registries
  Result<void> operate() const;   // kernel:execute  — operate the machine beneath the kernel
  Result<void> own(const std::string& disk = "") const;       // kernel:destroy  — end or wipe the machine or a named disk
  Result<void> consume_destroy(const std::string& disk) const;

 private:
  friend class Kernel;
  Surface(Kernel& k, EntityID who) : k_(k), who_(who) {}
  Result<void> need(ResourceRef r, uint16_t rights) const;
  Result<void> need_entity() const;
  Result<void> need_actor(const EntityID& e) const;     // may this entity act as e?
  // The delegable source for (res, rights) among this entity's capabilities, or
  // the kernel's refusal spelled out.
  Result<CapabilityID> source_for(ResourceRef res, uint16_t rights, uint64_t scope_limit) const;
  std::string who_str() const;
  Kernel& k_;
  EntityID who_;
};

}  // namespace aios
