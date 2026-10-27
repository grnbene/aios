#include "aios/kernel/surface.hpp"

namespace aios {

Result<void> Surface::need(ResourceRef r, uint16_t rights) const { return k_.caps_->require(who_, r, rights, k_.now()); }
Result<void> Surface::need_entity() const {
  if (!k_.entities_->exists(who_)) return Result<void>(Status::NotFound, "entity " + who_.short_str() + " does not exist");
  return Ok();
}

// ---- memory ----
Result<MemoryID> Surface::allocate(uint64_t size, MemoryType type) {
  AIOS_TRY(need_entity());
  AIOS_TRY(need(ResourceRef{ResourceKind::Memory, {}}, R_Allocate));
  auto charged = k_.caps_->charge(who_, ResourceRef{ResourceKind::Memory, {}}, R_Allocate, size, k_.now());
  if (!charged.ok()) return charged.error();
  auto m = k_.memory_->allocate(who_, size, type);
  if (!m.ok()) { k_.caps_->refund(*charged, size); return m.error(); }   // nothing was allocated: nothing spent
  k_.caps_->record_charge(*m, *charged, size);   // release gives it back
  k_.entities_->add_memory(who_, *m);
  // Ownership yields authority over the object; delegable.
  auto c = k_.caps_->grant(who_, ResourceRef{ResourceKind::Memory, *m}, R_Read | R_Write | R_Grant | R_Destroy, k_.now());
  if (c.ok()) k_.entities_->add_capability(who_, *c);
  return m;
}
Result<void> Surface::write(const MemoryID& m, uint64_t offset, ByteSpan data) {
  AIOS_TRY(need(ResourceRef{ResourceKind::Memory, m}, R_Write));
  return k_.memory_->write(m, offset, data);
}
Result<ByteBuffer> Surface::read(const MemoryID& m, uint64_t offset, uint64_t length) {
  AIOS_TRY(need(ResourceRef{ResourceKind::Memory, m}, R_Read));
  return k_.memory_->read(m, offset, length);
}
Result<void> Surface::release(const MemoryID& m) {
  AIOS_TRY(need(ResourceRef{ResourceKind::Memory, m}, R_Destroy));
  const MemoryObject* o = k_.memory_->lookup(m);
  if (!o) return Result<void>(Status::NotFound, "memory " + m.short_str());
  EntityID owner = o->owner;
  AIOS_TRY(k_.memory_->release(m));
  k_.caps_->refund_object(m);   // the allocation scope it cost is returned
  k_.entities_->remove_memory(owner, m);
  return Ok();
}
Result<void> Surface::migrate(const MemoryID& m, MemoryType to) {
  AIOS_TRY(need(ResourceRef{ResourceKind::Memory, m}, R_Write));
  return k_.memory_->migrate(m, to);
}
Result<CapabilityID> Surface::share(const MemoryID& m, const EntityID& to, uint16_t rights) {
  for (auto& c : k_.caps_->list(who_)) {
    if (c.resource.kind == ResourceKind::Memory && c.resource.id == m && (c.rights & R_Grant)) {
      auto d = k_.caps_->delegate(c.id, to, rights, k_.now());
      if (d.ok()) k_.entities_->add_capability(to, *d);
      return d;
    }
  }
  return Result<CapabilityID>(Status::Denied, "entity " + who_.short_str() + " holds no delegable capability over memory " + m.short_str());
}
Result<const MemoryObject*> Surface::describe(const MemoryID& m) {
  AIOS_TRY(need(ResourceRef{ResourceKind::Memory, m}, R_Read));
  const MemoryObject* o = k_.memory_->lookup(m);
  if (!o) return Result<const MemoryObject*>(Status::NotFound, "memory " + m.short_str());
  return o;
}
Result<Tensor> Surface::tensor(DType dtype, const std::vector<uint64_t>& shape, MemoryType type) {
  Tensor t; t.dtype = dtype; t.shape = shape;
  if (t.elements() == 0) return Result<Tensor>(Status::Invalid, "tensor needs a non-empty shape");
  auto m = allocate(t.bytes(), type);
  if (!m.ok()) return m.error();
  t.memory = *m;
  return t;
}

// ---- state ----
Result<StateID> Surface::create_state(StateKind kind, ByteSpan payload) {
  AIOS_TRY(need_entity());
  AIOS_TRY(need(ResourceRef{ResourceKind::State, {}}, R_Allocate));
  auto s = k_.states_->create(who_, kind, k_.now(), payload);
  if (s.ok()) k_.entities_->add_state(who_, *s);
  return s;
}
Result<StateObject> Surface::state(const StateID& s) {
  auto st = k_.states_->copy(s);
  if (!st.ok()) return st.error();
  if (st->owner != who_) AIOS_TRY(need(ResourceRef{ResourceKind::State, s}, R_Read));
  return st;
}
Result<void> Surface::set_state(const StateID& s, ByteSpan payload) {
  const StateObject* st = k_.states_->get(s);
  if (!st) return Result<void>(Status::NotFound, "state " + s.short_str());
  if (st->owner != who_) AIOS_TRY(need(ResourceRef{ResourceKind::State, s}, R_Write));
  return k_.states_->set_payload(s, payload, k_.now());
}
Result<void> Surface::keep_state(const StateID& s) {
  const StateObject* st = k_.states_->get(s);
  if (!st) return Result<void>(Status::NotFound, "state " + s.short_str());
  if (st->owner != who_) return Result<void>(Status::Denied, "state " + s.short_str() + " belongs to " + st->owner.short_str() + "; only its owner may keep it");
  if (!k_.authority_->is_resident(who_))
    return Result<void>(Status::Denied, "entity " + who_.short_str() + " is not a resident: it does not outlive this process, so neither can its state");
  k_.states_->set_durable(s, true);
  return Ok();
}
Result<void> Surface::drop_state(const StateID& s) {
  const StateObject* st = k_.states_->get(s);
  if (!st) return Result<void>(Status::NotFound, "state " + s.short_str());
  if (st->owner != who_) return Result<void>(Status::Denied, "state " + s.short_str() + " belongs to " + st->owner.short_str() + "; only its owner may drop it");
  return k_.states_->destroy(s);
}
Result<void> Surface::attach(const StateID& s, const MemoryID& m) {
  const StateObject* st = k_.states_->get(s);
  if (!st) return Result<void>(Status::NotFound, "state " + s.short_str());
  if (st->owner != who_) AIOS_TRY(need(ResourceRef{ResourceKind::State, s}, R_Write));
  AIOS_TRY(need(ResourceRef{ResourceKind::Memory, m}, R_Read));
  return k_.states_->attach_memory(s, m, k_.now());
}
Result<uint64_t> Surface::apply_state_delta(const StateID& s, StateKind kind, uint64_t target_gen, ByteSpan payload, const std::vector<MemoryID>& mems) {
  const StateObject* st = k_.states_->get(s);
  if (st && st->owner != who_) AIOS_TRY(need(ResourceRef{ResourceKind::State, s}, R_Write));
  return k_.states_->apply_delta(s, who_, kind, target_gen, payload, mems, k_.now());
}
std::vector<StateObject> Surface::states() { return k_.states_->list(who_); }

// ---- channels ----
Result<ChannelID> Surface::create_channel(uint32_t capacity, uint64_t slot_bytes, const std::string& transport) {
  AIOS_TRY(need_entity());
  AIOS_TRY(need(ResourceRef{ResourceKind::Channel, {}}, R_Allocate));
  if (transport != "shared-memory") AIOS_TRY(need(ResourceRef{ResourceKind::Network, {}}, R_Publish));   // crossing nodes is network authority
  auto bytes = k_.channels_->region_bytes(capacity, slot_bytes, transport);
  if (!bytes.ok()) return bytes.error();
  auto charged = k_.caps_->charge(who_, ResourceRef{ResourceKind::Memory, {}}, R_Allocate, *bytes, k_.now());
  if (!charged.ok()) return charged.error();
  auto c = k_.channels_->create(who_, capacity, slot_bytes, k_.now(), transport);
  if (!c.ok()) { k_.caps_->refund(*charged, *bytes); return c.error(); }
  const Channel* ch = k_.channels_->get(*c);
  if (ch) { k_.entities_->add_memory(who_, ch->region); k_.caps_->record_charge(ch->region, *charged, *bytes); }
  auto cap = k_.caps_->grant(who_, ResourceRef{ResourceKind::Channel, *c}, R_Publish | R_Consume | R_Grant | R_Destroy, k_.now());
  if (cap.ok()) k_.entities_->add_capability(who_, *cap);
  k_.channels_->subscribe(*c, who_);
  return c;
}
Result<void> Surface::subscribe(const ChannelID& c) {
  AIOS_TRY(need(ResourceRef{ResourceKind::Channel, c}, R_Consume));
  return k_.channels_->subscribe(c, who_);
}
Result<uint64_t> Surface::publish(const ChannelID& c, const StateID& s) {
  AIOS_TRY(need(ResourceRef{ResourceKind::Channel, c}, R_Publish));
  auto st = state(s);
  if (!st.ok()) return st.error();
  return k_.channels_->publish(c, who_, *st, k_.now());
}
Result<StateFrame> Surface::consume(const ChannelID& c) {
  AIOS_TRY(need(ResourceRef{ResourceKind::Channel, c}, R_Consume));
  return k_.channels_->consume(c, who_);
}
Result<CapabilityID> Surface::admit(const ChannelID& c, const EntityID& to, uint16_t rights) {
  for (auto& cap : k_.caps_->list(who_)) {
    if (cap.resource.kind == ResourceKind::Channel && cap.resource.id == c && (cap.rights & R_Grant)) {
      auto d = k_.caps_->delegate(cap.id, to, rights, k_.now());
      if (d.ok()) k_.entities_->add_capability(to, *d);
      return d;
    }
  }
  return Result<CapabilityID>(Status::Denied, "entity " + who_.short_str() + " holds no delegable capability over channel " + c.short_str());
}

// ---- compute ----
Result<GraphID> Surface::create_graph() {
  AIOS_TRY(need_entity());
  return k_.graphs_->create(who_, k_.now());
}
Result<void> Surface::destroy_graph(const GraphID& g) {
  AIOS_TRY(need_entity());
  const ComputeGraph* gr = k_.graphs_->get(g);
  if (!gr) return Result<void>(Status::NotFound, "graph " + g.short_str());
  if (gr->owner != who_) return Result<void>(Status::Denied, "graph belongs to another entity");
  return k_.graphs_->destroy(g);
}
Result<OperationID> Surface::add_operation(const GraphID& g, Operation op) {
  const ComputeGraph* gr = k_.graphs_->get(g);
  if (!gr) return Result<OperationID>(Status::NotFound, "graph " + g.short_str());
  if (gr->owner != who_) return Result<OperationID>(Status::Denied, "graph belongs to another entity");
  for (auto& t : op.inputs) AIOS_TRY(need(ResourceRef{ResourceKind::Memory, t.memory}, R_Read));
  for (auto& t : op.outputs) AIOS_TRY(need(ResourceRef{ResourceKind::Memory, t.memory}, R_Write));
  return k_.graphs_->add(g, std::move(op));
}
Result<ExecutionReport> Surface::execute(const GraphID& g, const SchedulePolicy& policy) {
  auto gr = k_.graphs_->copy(g);
  if (!gr.ok()) return gr.error();
  if (gr->owner != who_) return Result<ExecutionReport>(Status::Denied, "graph belongs to another entity");
  auto e = k_.entities_->copy(who_);
  if (!e.ok()) return e.error();
  uint64_t start = (e->execution == g) ? e->execution_position : 0;
  k_.entities_->set_status(who_, EntityStatus::Active);
  auto rep = k_.scheduler_->execute(*gr, who_, start, policy);
  if (!rep.ok()) return rep;
  k_.entities_->set_execution(who_, g, rep->position);
  // Execution state is a kernel object: record the position so it checkpoints.
  ByteBuffer b; Writer w(b); w.id(g); w.u64(rep->position); w.u64(rep->operations_total); w.u8(rep->complete ? 1 : 0);
  bool found = false;
  for (auto& st : k_.states_->list(who_)) if (st.kind == StateKind::Execution) { k_.states_->set_payload(st.id, b, k_.now()); found = true; break; }
  if (!found) { auto s = k_.states_->create(who_, StateKind::Execution, k_.now(), b); if (s.ok()) k_.entities_->add_state(who_, *s); }
  return rep;
}
Result<std::vector<Placement>> Surface::plan(const GraphID& g) {
  auto gr = k_.graphs_->copy(g);
  if (!gr.ok()) return gr.error();
  return k_.scheduler_->plan(*gr, who_);
}

Result<void> Surface::relate(const EntityID& peer, uint32_t kind) {
  AIOS_TRY(need_entity());
  return k_.entities_->relate(who_, peer, kind, k_.now());
}

// ---- storage ----
Result<ObjectID> Surface::store(StoredObject obj) {
  AIOS_TRY(need(ResourceRef{ResourceKind::StorageObject, {}}, R_Write));
  obj.provenance = who_;
  return k_.store_->put(std::move(obj));
}
Result<StoredObject> Surface::fetch(const ObjectID& id) {
  AIOS_TRY(need(ResourceRef{ResourceKind::StorageObject, id}, R_Read));
  return k_.store_->get(id);
}
Result<ObjectID> Surface::snapshot(const StateID& s, std::vector<float> embedding) {
  auto st = state(s);
  if (!st.ok()) return st.error();
  StoredObject o;
  o.source_state = s;
  o.payload = st->payload;
  o.embedding = std::move(embedding);
  o.metadata.emplace_back("kind", state_kind_name(st->kind));
  o.metadata.emplace_back("generation", std::to_string(st->generation));
  // Large state travels with its memory contents.
  for (auto& m : st->memory) {
    auto bytes = read(m, 0, UINT64_MAX);
    if (bytes.ok()) { Writer w(o.payload); w.id(m); w.bytes(*bytes); }
  }
  // Chain to the previous snapshot of this state, if any.
  auto prev = k_.store_->find_state(s);
  if (!prev.empty()) o.derived_from = prev.back();
  return store(std::move(o));
}

// ---- authority ----
std::string Surface::who_str() const {
  const Entity* e = k_.entities_->get(who_);
  return (e ? e->label : std::string("entity")) + " " + who_.short_str();
}

Result<CapabilityID> Surface::delegate(const CapabilityID& cap, const EntityID& to, uint16_t rights, uint64_t ttl_ns) {
  const Capability* c = k_.caps_->get(cap);
  if (!c) return Result<CapabilityID>(Status::NotFound, "capability " + cap.short_str());
  if (c->holder != who_) return Result<CapabilityID>(Status::Denied, "capability " + cap.short_str() + " is held by another entity, not by " + who_str());
  if (!k_.entities_->exists(to)) return Result<CapabilityID>(Status::NotFound, "entity " + to.short_str());
  auto d = k_.caps_->delegate(cap, to, rights, k_.now(), ttl_ns ? k_.now() + ttl_ns : 0);
  if (d.ok()) { k_.entities_->add_capability(to, *d); k_.note(AuthorityEvent::Derived, who_, to, c->resource, rights, *d, "delegated from " + cap.short_str()); }
  return d;
}

Result<CapabilityID> Surface::source_for(ResourceRef res, uint16_t rights, uint64_t scope_limit) const {
  uint16_t held_any = 0, held_grantable = 0;
  bool any = false;
  for (auto& c : k_.caps_->list(who_)) {
    if (!c.resource.covers(res)) continue;
    if (c.expires_ns && k_.now() >= c.expires_ns) continue;
    any = true; held_any |= c.rights;
    if (!(c.rights & R_Grant)) continue;
    held_grantable |= c.rights;
    if ((rights & ~c.rights) != 0) continue;
    if (scope_limit && c.scope_limit && scope_limit > c.scope_limit) continue;
    return c.id;
  }
  std::string me = who_str();
  if (!any) return Result<CapabilityID>(Status::Denied, "the kernel refuses: " + me + " holds no authority over " + res.str());
  if (!held_grantable) return Result<CapabilityID>(Status::Denied, "the kernel refuses: " + me + " holds " + rights_str(held_any) + " over " + res.str() + ", none of it delegable (no grant right)");
  uint16_t missing = rights & ~held_grantable;
  if (missing) return Result<CapabilityID>(Status::Denied, "the kernel refuses: " + me + " holds " + rights_str(held_grantable) + " over " + res.str() + "; " + rights_str(missing) + " is not among them — an entity cannot grant what it does not hold");
  return Result<CapabilityID>(Status::Denied, "the kernel refuses: " + me + " holds " + rights_str(held_grantable) + " over " + res.str() + " but not with scope " + std::to_string(scope_limit));
}

Result<CapabilityID> Surface::grant(const EntityID& to, ResourceRef res, uint16_t rights, uint64_t ttl_ns, uint64_t scope_limit) {
  AIOS_TRY(need_entity());
  if (!k_.entities_->exists(to)) return Result<CapabilityID>(Status::NotFound, "entity " + to.short_str());
  if (rights == 0) return Result<CapabilityID>(Status::Invalid, "no rights requested");
  auto src = source_for(res, rights, scope_limit);
  if (!src.ok()) { k_.note(AuthorityEvent::Refused, who_, to, res, rights, {}, src.error().message); return src; }
  auto d = k_.caps_->delegate(*src, to, rights, k_.now(), ttl_ns ? k_.now() + ttl_ns : 0, scope_limit, &res);
  if (d.ok()) { k_.entities_->add_capability(to, *d); k_.note(AuthorityEvent::Derived, who_, to, res, rights, *d, "granted from " + src->short_str()); }
  return d;
}

Result<CapabilityID> Surface::request(ResourceRef res, uint16_t rights, uint64_t ttl_ns, bool confirmed) {
  AIOS_TRY(need_entity());
  if (rights == 0) return Result<CapabilityID>(Status::Invalid, "no rights requested");
  k_.note(AuthorityEvent::Requested, who_, who_, res, rights, {}, "");
  // Already permitted? Then the answer is the capability that permits it.
  for (auto& c : k_.caps_->list(who_)) {
    if (!c.resource.covers(res) || (c.rights & rights) != rights) continue;
    if (c.expires_ns && k_.now() >= c.expires_ns) continue;
    if (c.scope_limit && c.scope_used >= c.scope_limit) continue;
    k_.note(AuthorityEvent::Admitted, who_, who_, res, rights, c.id, "already held");
    return c.id;
  }
  // Not directly — but derivable from something held? Then a narrower capability.
  auto src = source_for(res, rights, 0);
  if (src.ok()) {
    auto d = k_.caps_->delegate(*src, who_, rights, k_.now(), ttl_ns ? k_.now() + ttl_ns : 0, 0, &res);
    if (d.ok()) { k_.entities_->add_capability(who_, *d); k_.note(AuthorityEvent::Derived, who_, who_, res, rights, *d, "narrowed from " + src->short_str()); }
    return d;
  }
  // Nothing held covers it: this is a petition for NEW authority. The machine
  // governs it (only trusted residents, bounded, hard wall, on the record).
  return k_.petition(who_, res, rights, ttl_ns, confirmed);
}


Result<void> Surface::revoke(const CapabilityID& cap) {
  const Capability* c = k_.caps_->get(cap);
  if (!c) return Result<void>(Status::NotFound, "capability " + cap.short_str());
  // Revocable if: held by this entity; derived from something it holds; or
  // held by an entity this one has destroy authority over (it could end the
  // holder outright, which revokes everything — narrower is fine).
  bool mine = c->holder == who_;
  if (!mine) for (auto& h : k_.caps_->list(who_)) if (k_.caps_->descends(cap, h.id)) { mine = true; break; }
  if (!mine && c->holder != k_.self_ && need(ResourceRef{ResourceKind::Entity, c->holder}, R_Destroy).ok()) mine = true;
  if (!mine) return Result<void>(Status::Denied, "the kernel refuses: capability " + cap.short_str() + " (held by " + c->holder.short_str() + ") is neither held by " + who_str() + ", nor derived from anything it holds, nor held by an entity it may destroy");
  ResourceRef res = c->resource; uint16_t rights = c->rights; EntityID holder = c->holder;
  AIOS_TRY(k_.caps_->revoke(cap));
  k_.note(AuthorityEvent::Revoked, who_, holder, res, rights, cap, "");
  return Ok();
}

std::vector<Capability> Surface::authority() const { return k_.caps_->list(who_); }
Result<void> Surface::may(ResourceRef res, uint16_t rights) const { return need(res, rights); }
Result<void> Surface::exercise(ResourceRef res, uint16_t rights, const std::string& note) {
  AIOS_TRY(need_entity());
  if (rights == 0) return Result<void>(Status::Invalid, "no rights requested");
  auto r = need(res, rights);
  if (!r.ok()) {
    k_.note(AuthorityEvent::Refused, who_, who_, res, rights, {}, note.empty() ? r.error().message : note + ": " + r.error().message);
    return r;
  }
  k_.note(AuthorityEvent::Exercised, who_, who_, res, rights, {}, note);
  return {};
}

// ---- acting through others ----
Result<void> Surface::need_actor(const EntityID& e) const {
  if (e == who_) return Ok();
  if (e == k_.self_) return Result<void>(Status::Denied, "the kernel refuses: the kernel entity acts only for itself; no entity acts as the kernel");
  if (!k_.entities_->exists(e)) return Result<void>(Status::NotFound, "entity " + e.short_str());
  auto r = need(ResourceRef{ResourceKind::Entity, e}, R_Execute);
  if (!r.ok()) return Result<void>(Status::Denied, "the kernel refuses: " + who_str() + " holds no execute authority over entity " + e.short_str() + ", so it cannot act as it");
  return Ok();
}
Result<Surface> Surface::act_as(const EntityID& e) {
  AIOS_TRY(need_actor(e));
  return Surface(k_, e);
}

// ---- entities ----
Result<EntityID> Surface::spawn(const std::string& label, const EntityID& parent) {
  AIOS_TRY(need_entity());
  AIOS_TRY(need(ResourceRef{ResourceKind::Entity, {}}, R_Allocate));
  EntityID p = parent.is_null() ? who_ : parent;
  AIOS_TRY(need_actor(p));
  return k_.spawn(label, p);
}
Result<EntityID> Surface::resident(const std::string& label, const AuthorityProfile& profile) {
  AIOS_TRY(need_entity());
  AIOS_TRY(need(ResourceRef{ResourceKind::Entity, {}}, R_Allocate));
  for (const auto& g : profile) {
    auto r = need(g.resource, g.rights);
    if (!r.ok()) return Result<EntityID>(Status::Denied, "the kernel refuses: " + who_str() + " cannot make resident '" + label +
                                                             "' holding " + rights_str(g.rights) + " over " + g.resource.str() + " - it does not hold that itself");
  }
  return k_.resident(label, profile, /*self_improve=*/false);
}
Result<void> Surface::terminate(const EntityID& e) {
  if (e == who_) return Result<void>(Status::Denied, "the kernel refuses: an entity does not terminate itself through its own surface");
  if (!k_.entities_->exists(e)) return Result<void>(Status::NotFound, "entity " + e.short_str());
  AIOS_TRY(need(ResourceRef{ResourceKind::Entity, e}, R_Destroy));
  return k_.terminate(e);
}

// ---- persistence ----
Result<std::string> Surface::checkpoint() {
  AIOS_TRY(need(ResourceRef{ResourceKind::Kernel, {}}, R_Checkpoint));
  return k_.checkpoint(who_);
}
Result<std::string> Surface::checkpoint_of(const EntityID& e) {
  AIOS_TRY(need(ResourceRef{ResourceKind::Kernel, {}}, R_Checkpoint));
  AIOS_TRY(need_actor(e));
  return k_.checkpoint(e);
}
Result<EntityID> Surface::restore(const std::string& path) {
  AIOS_TRY(need(ResourceRef{ResourceKind::Kernel, {}}, R_Checkpoint));
  AIOS_TRY(need(ResourceRef{ResourceKind::Entity, {}}, R_Allocate));
  return k_.restore(path, who_);
}

Result<EntityID> Surface::rewind(const std::string& path) {
  // The same authority a restore needs, and for the same reason: this installs
  // an entity's contents. Putting one BACK is not a lesser act than bringing
  // one in — it destroys what that entity holds now — so it is not a lesser
  // permission either.
  AIOS_TRY(need(ResourceRef{ResourceKind::Kernel, {}}, R_Checkpoint));
  AIOS_TRY(need(ResourceRef{ResourceKind::Entity, {}}, R_Allocate));
  return k_.rewind(path, who_);
}

// ---- distribution ----
Result<uint16_t> Surface::serve(uint16_t port, const std::string& token, bool polled, bool loopback_only) {
  AIOS_TRY(need(ResourceRef{ResourceKind::Network, {}}, R_Publish));
  return k_.serve(port, token, polled, loopback_only);
}
Result<RemoteAttachment> Surface::connect(const std::string& host, uint16_t port, const std::string& token) {
  AIOS_TRY(need(ResourceRef{ResourceKind::Network, {}}, R_Consume));
  return k_.connect(host, port, token);
}
Result<uint16_t> Surface::relay_hub(uint16_t port, const std::string& cluster_key, bool loopback_only,
                                    const std::string& members_file, bool require_attest) {
  AIOS_TRY(need(ResourceRef{ResourceKind::Network, {}}, R_Publish));
  return k_.relay_hub(port, cluster_key, loopback_only, members_file, require_attest);
}
Result<void> Surface::fleet_join(const std::string& hub_host, uint16_t hub_port, const std::string& cluster_key, bool keep_trying,
                                 const std::string& member_key, bool attest) {
  AIOS_TRY(need(ResourceRef{ResourceKind::Network, {}}, R_Publish));
  AIOS_TRY(need(ResourceRef{ResourceKind::Network, {}}, R_Consume));
  return k_.fleet_join(hub_host, hub_port, cluster_key, keep_trying, member_key, attest);
}

Result<EntityID> Surface::migrate(const EntityID& e, size_t node_index, bool keep_local) {
  AIOS_TRY(need(ResourceRef{ResourceKind::Kernel, {}}, R_Checkpoint));
  AIOS_TRY(need(ResourceRef{ResourceKind::Network, {}}, R_Publish));
  AIOS_TRY(need_actor(e));
  return k_.migrate(e, node_index, keep_local);
}

// ---- minds ----
Result<void> Surface::bind_mind(const EntityID& e, std::unique_ptr<Intelligence> mind, const ChannelID& output) {
  AIOS_TRY(need_actor(e));
  return k_.bind_mind(e, std::move(mind), output);
}
Result<void> Surface::swap_mind(const EntityID& e, std::unique_ptr<Intelligence> new_mind, const ChannelID& output) {
  AIOS_TRY(need_actor(e));
  return k_.swap_mind(e, std::move(new_mind), output);
}
Result<void> Surface::unbind_mind(const EntityID& e) {
  AIOS_TRY(need_actor(e));
  return k_.unbind_mind(e);
}
Result<CycleReport> Surface::cycle(const EntityID& e) {
  AIOS_TRY(need_actor(e));
  return k_.cycle(e);
}

// ---- the kernel itself ----
Result<void> Surface::inspect() const { return need(ResourceRef{ResourceKind::Kernel, {}}, R_Read); }
Result<void> Surface::operate() const { return need(ResourceRef{ResourceKind::Kernel, {}}, R_Execute); }
Result<void> Surface::own(const std::string& disk) const {
  if (disk.empty()) return need(ResourceRef{ResourceKind::Kernel, {}}, R_Destroy);
  return k_.consume_destroy(who_, disk);
}
Result<void> Surface::consume_destroy(const std::string& disk) const {
  return k_.consume_destroy(who_, disk);
}

}  // namespace aios
