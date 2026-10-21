#include "aios/entity/entity.hpp"
#include "aios/hal/sync.hpp"
#include <algorithm>

namespace aios {

const char* entity_status_name(EntityStatus s) {
  switch (s) { case EntityStatus::Dormant: return "dormant"; case EntityStatus::Active: return "active"; case EntityStatus::Suspended: return "suspended"; case EntityStatus::Checkpointed: return "checkpointed"; case EntityStatus::Terminated: return "terminated"; }
  return "?";
}
const char* relation_kind_name(uint32_t k) {
  switch (k) { case Rel_Parent: return "parent"; case Rel_Child: return "child"; case Rel_Peer: return "peer"; case Rel_Consumes: return "consumes"; case Rel_Provides: return "provides"; case Rel_Mind: return "mind"; }
  return "custom";
}
bool parse_relation_kind(const std::string& s, uint32_t& out) {
  if (s == "parent") out = Rel_Parent; else if (s == "child") out = Rel_Child; else if (s == "peer") out = Rel_Peer;
  else if (s == "consumes") out = Rel_Consumes; else if (s == "provides") out = Rel_Provides; else if (s == "mind") out = Rel_Mind;
  else { if (s.empty()) return false; uint64_t v = 0; for (char c : s) { if (c < '0' || c > '9') return false; v = v * 10 + (uint64_t)(c - '0'); if (v > 0xFFFFFFFFull) return false; } out = (uint32_t)v; }
  return true;
}

void Entity::encode(Writer& w) const {
  w.id(id); w.str(label); w.u64(created_ns); w.u64(generation); w.u8((uint8_t)status);
  w.u32((uint32_t)states.size()); for (auto& s : states) w.id(s);
  w.u32((uint32_t)memory.size()); for (auto& m : memory) w.id(m);
  w.u32((uint32_t)capabilities.size()); for (auto& c : capabilities) w.id(c);
  w.id(execution); w.u64(execution_position);
  w.u32((uint32_t)relationships.size()); for (auto& r : relationships) { w.id(r.peer); w.u32(r.kind); w.u64(r.since_ns); }
  w.u64(checkpoints); w.u64(restores);
}
Result<Entity> Entity::decode(Reader& r) {
  Entity e;
  e.id = EntityID{r.id()}; e.label = r.str(); e.created_ns = r.u64(); e.generation = r.u64(); e.status = (EntityStatus)r.u8();
  uint32_t n = r.u32(); if (!r.ok() || n > (1u << 20)) return Result<Entity>(Status::Corrupt, "entity states"); for (uint32_t i = 0; i < n; ++i) e.states.push_back(StateID{r.id()});
  n = r.u32(); if (!r.ok() || n > (1u << 20)) return Result<Entity>(Status::Corrupt, "entity memory"); for (uint32_t i = 0; i < n; ++i) e.memory.push_back(MemoryID{r.id()});
  n = r.u32(); if (!r.ok() || n > (1u << 20)) return Result<Entity>(Status::Corrupt, "entity caps"); for (uint32_t i = 0; i < n; ++i) e.capabilities.push_back(CapabilityID{r.id()});
  e.execution = GraphID{r.id()}; e.execution_position = r.u64();
  n = r.u32(); if (!r.ok() || n > (1u << 20)) return Result<Entity>(Status::Corrupt, "entity relationships");
  for (uint32_t i = 0; i < n; ++i) { Relationship rel; rel.peer = EntityID{r.id()}; rel.kind = r.u32(); rel.since_ns = r.u64(); e.relationships.push_back(rel); }
  e.checkpoints = r.u64(); e.restores = r.u64();
  if (!r.ok()) return Result<Entity>(Status::Corrupt, "entity record");
  return e;
}

EntityManager::EntityManager(IdGenerator& ids) : ids_(ids) {}

Result<EntityID> EntityManager::create(const std::string& label, uint64_t now_ns) {
  Entity e;
  e.id = ids_.next_as<EntityID>();
  e.label = label;
  e.created_ns = now_ns;
  e.status = EntityStatus::Active;
  EntityID id = e.id;
  hal::LockGuard g(mu_);
  entities_[id] = std::move(e);
  return id;
}

Result<EntityID> EntityManager::install(const Entity& e) {
  hal::LockGuard g(mu_);
  if (entities_.count(e.id)) return Result<EntityID>(Status::Conflict, "entity " + e.id.short_str() + " already exists in this kernel");
  entities_[e.id] = e;
  return e.id;
}

const Entity* EntityManager::get(const EntityID& id) const {
  hal::LockGuard g(mu_);
  auto it = entities_.find(id);
  return it == entities_.end() ? nullptr : &it->second;
}
Result<Entity> EntityManager::copy(const EntityID& id) const {
  hal::LockGuard g(mu_);
  auto it = entities_.find(id);
  if (it == entities_.end()) return Result<Entity>(Status::NotFound, "entity " + id.short_str());
  return it->second;
}
bool EntityManager::exists(const EntityID& id) const { hal::LockGuard g(mu_); return entities_.count(id) > 0; }
std::vector<Entity> EntityManager::list() const {
  hal::LockGuard g(mu_);
  std::vector<Entity> out;
  for (auto& [id, e] : entities_) out.push_back(e);
  std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.created_ns < b.created_ns; });
  return out;
}
size_t EntityManager::count() const { hal::LockGuard g(mu_); return entities_.size(); }

template <typename F>
Result<void> EntityManager::mutate(const EntityID& id, F f) {
  hal::LockGuard g(mu_);
  auto it = entities_.find(id);
  if (it == entities_.end()) return Result<void>(Status::NotFound, "entity " + id.short_str());
  f(it->second);
  it->second.generation++;
  return Ok();
}

Result<void> EntityManager::set_status(const EntityID& id, EntityStatus s) { return mutate(id, [&](Entity& e) { e.status = s; }); }
Result<void> EntityManager::set_execution(const EntityID& id, const GraphID& gid, uint64_t pos) { return mutate(id, [&](Entity& e) { e.execution = gid; e.execution_position = pos; }); }
Result<void> EntityManager::relate(const EntityID& a, const EntityID& b, uint32_t kind, uint64_t now_ns) {
  if (!exists(b)) return Result<void>(Status::NotFound, "entity " + b.short_str());
  return mutate(a, [&](Entity& e) {
    for (auto& r : e.relationships) if (r.peer == b && r.kind == kind) return;
    e.relationships.push_back(Relationship{b, kind, now_ns});
  });
}
Result<void> EntityManager::unrelate(const EntityID& a, const EntityID& b, uint32_t kind) {
  return mutate(a, [&](Entity& e) { e.relationships.erase(std::remove_if(e.relationships.begin(), e.relationships.end(), [&](const Relationship& r) { return r.peer == b && r.kind == kind; }), e.relationships.end()); });
}
Result<void> EntityManager::add_state(const EntityID& id, const StateID& s) { return mutate(id, [&](Entity& e) { if (std::find(e.states.begin(), e.states.end(), s) == e.states.end()) e.states.push_back(s); }); }
Result<void> EntityManager::add_memory(const EntityID& id, const MemoryID& m) { return mutate(id, [&](Entity& e) { if (std::find(e.memory.begin(), e.memory.end(), m) == e.memory.end()) e.memory.push_back(m); }); }
Result<void> EntityManager::remove_memory(const EntityID& id, const MemoryID& m) { return mutate(id, [&](Entity& e) { e.memory.erase(std::remove(e.memory.begin(), e.memory.end(), m), e.memory.end()); }); }
Result<void> EntityManager::add_capability(const EntityID& id, const CapabilityID& c) { return mutate(id, [&](Entity& e) { if (std::find(e.capabilities.begin(), e.capabilities.end(), c) == e.capabilities.end()) e.capabilities.push_back(c); }); }
Result<void> EntityManager::remove_capability(const EntityID& id, const CapabilityID& c) { return mutate(id, [&](Entity& e) { e.capabilities.erase(std::remove(e.capabilities.begin(), e.capabilities.end(), c), e.capabilities.end()); }); }
Result<void> EntityManager::note_checkpoint(const EntityID& id) { return mutate(id, [&](Entity& e) { e.checkpoints++; }); }
Result<void> EntityManager::note_restore(const EntityID& id) { return mutate(id, [&](Entity& e) { e.restores++; }); }
Result<void> EntityManager::remove(const EntityID& id) {
  hal::LockGuard g(mu_);
  if (!entities_.erase(id)) return Result<void>(Status::NotFound, "entity " + id.short_str());
  for (auto& [eid, e] : entities_) e.relationships.erase(std::remove_if(e.relationships.begin(), e.relationships.end(), [&](const Relationship& r) { return r.peer == id; }), e.relationships.end());
  return Ok();
}

}  // namespace aios
