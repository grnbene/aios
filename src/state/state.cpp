#include "aios/state/state.hpp"
#include "aios/hal/sync.hpp"
#include <algorithm>

namespace aios {

const char* state_kind_name(StateKind k) {
  switch (k) { case StateKind::Working: return "working"; case StateKind::Persistent: return "persistent"; case StateKind::Context: return "context"; case StateKind::Relationship: return "relationship"; case StateKind::Execution: return "execution"; }
  return "?";
}
bool parse_state_kind(const std::string& s, StateKind& out) {
  if (s == "working") out = StateKind::Working; else if (s == "persistent") out = StateKind::Persistent; else if (s == "context") out = StateKind::Context;
  else if (s == "relationship") out = StateKind::Relationship; else if (s == "execution") out = StateKind::Execution; else return false;
  return true;
}

uint64_t StateObject::fingerprint() const {
  uint64_t h = fnv1a(payload);
  for (auto& m : memory) { h = fnv1a(ByteSpan(reinterpret_cast<const uint8_t*>(&m.hi), 8), h); h = fnv1a(ByteSpan(reinterpret_cast<const uint8_t*>(&m.lo), 8), h); }
  return h;
}

void StateObject::encode(Writer& w) const {
  w.id(id); w.id(owner); w.u8((uint8_t)kind); w.u64(generation); w.u64(created_ns); w.u64(updated_ns);
  w.bytes(payload);
  w.u32((uint32_t)memory.size());
  for (auto& m : memory) w.id(m);
}
Result<StateObject> StateObject::decode(Reader& r) {
  StateObject s;
  s.id = StateID{r.id()}; s.owner = EntityID{r.id()}; s.kind = (StateKind)r.u8(); s.generation = r.u64(); s.created_ns = r.u64(); s.updated_ns = r.u64();
  auto p = r.bytes();
  s.payload.assign(p.begin(), p.end());
  uint32_t n = r.u32();
  if (!r.ok() || n > (1u << 20)) return Result<StateObject>(Status::Corrupt, "state record");
  for (uint32_t i = 0; i < n; ++i) s.memory.push_back(MemoryID{r.id()});
  if (!r.ok()) return Result<StateObject>(Status::Corrupt, "state record");
  return s;
}

StateStore::StateStore(IdGenerator& ids) : ids_(ids) {}

void StateStore::changed(const StateID& id) const {
  if (!persist_) return;
  StateObject copy;
  bool present = false;
  {
    hal::LockGuard g(mu_);
    if (!durable_.count(id)) return;
    auto it = states_.find(id);
    if (it != states_.end()) { copy = it->second; present = true; }
  }
  persist_(id, present ? &copy : nullptr);
}

bool StateStore::set_durable(const StateID& id, bool on) {
  {
    hal::LockGuard g(mu_);
    if (!states_.count(id)) return false;
    if (on) durable_.insert(id);
  }
  if (on) { changed(id); return true; }
  if (persist_) {
    bool was = false;
    { hal::LockGuard g(mu_); was = durable_.count(id) > 0; }
    if (was) persist_(id, nullptr);   // no longer kept: its file goes
  }
  hal::LockGuard g(mu_);
  durable_.erase(id);
  return true;
}

bool StateStore::is_durable(const StateID& id) const {
  hal::LockGuard g(mu_);
  return durable_.count(id) > 0;
}

Result<StateID> StateStore::create(const EntityID& owner, StateKind kind, uint64_t now_ns, ByteSpan payload) {
  StateObject s;
  s.id = ids_.next_as<StateID>();
  s.owner = owner; s.kind = kind; s.created_ns = now_ns; s.updated_ns = now_ns;
  s.payload.assign(payload.begin(), payload.end());
  StateID id = s.id;
  { hal::LockGuard g(mu_); states_[id] = std::move(s); }
  changed(id);
  return id;
}

Result<StateID> StateStore::install(const StateObject& s) {
  {
    hal::LockGuard g(mu_);
    if (states_.count(s.id)) return Result<StateID>(Status::Conflict, "state identity already present");
    states_[s.id] = s;
  }
  changed(s.id);
  return s.id;
}

const StateObject* StateStore::get(const StateID& id) const {
  hal::LockGuard g(mu_);
  auto it = states_.find(id);
  return it == states_.end() ? nullptr : &it->second;
}

Result<StateObject> StateStore::copy(const StateID& id) const {
  hal::LockGuard g(mu_);
  auto it = states_.find(id);
  if (it == states_.end()) return Result<StateObject>(Status::NotFound, "state " + id.short_str());
  return it->second;
}

Result<void> StateStore::set_payload(const StateID& id, ByteSpan payload, uint64_t now_ns) {
  {
  hal::LockGuard g(mu_);
  auto it = states_.find(id);
  if (it == states_.end()) return Result<void>(Status::NotFound, "state " + id.short_str());
  it->second.payload.assign(payload.begin(), payload.end());
  it->second.generation++;
  it->second.updated_ns = now_ns;
  }
  changed(id);
  return Ok();
}

Result<void> StateStore::attach_memory(const StateID& id, const MemoryID& mem, uint64_t now_ns) {
  {
  hal::LockGuard g(mu_);
  auto it = states_.find(id);
  if (it == states_.end()) return Result<void>(Status::NotFound, "state " + id.short_str());
  auto& v = it->second.memory;
  if (std::find(v.begin(), v.end(), mem) == v.end()) v.push_back(mem);
  it->second.generation++;
  it->second.updated_ns = now_ns;
  }
  changed(id);
  return Ok();
}

Result<void> StateStore::detach_memory(const StateID& id, const MemoryID& mem, uint64_t now_ns) {
  {
  hal::LockGuard g(mu_);
  auto it = states_.find(id);
  if (it == states_.end()) return Result<void>(Status::NotFound, "state " + id.short_str());
  auto& v = it->second.memory;
  v.erase(std::remove(v.begin(), v.end(), mem), v.end());
  it->second.generation++;
  it->second.updated_ns = now_ns;
  }
  changed(id);
  return Ok();
}

Result<uint64_t> StateStore::apply_delta(const StateID& id, const EntityID& owner, StateKind kind, uint64_t target_gen, ByteSpan payload, const std::vector<MemoryID>& mems, uint64_t now_ns) {
  uint64_t gen = 0;
  {
  hal::LockGuard g(mu_);
  auto it = states_.find(id);
  if (it == states_.end()) {
    StateObject s;
    s.id = id;
    s.owner = owner;
    s.kind = kind;
    s.generation = target_gen ? target_gen : 1;
    s.created_ns = now_ns;
    s.updated_ns = now_ns;
    if (!payload.empty()) s.payload.assign(payload.begin(), payload.end());
    s.memory = mems;
    states_[id] = std::move(s);
    gen = states_[id].generation;
  } else {
  auto& s = it->second;
  if (!payload.empty()) s.payload.assign(payload.begin(), payload.end());
  for (const auto& m : mems) {
    if (std::find(s.memory.begin(), s.memory.end(), m) == s.memory.end()) {
      s.memory.push_back(m);
    }
  }
  s.generation = target_gen ? target_gen : (s.generation + 1);
  s.updated_ns = now_ns;
  gen = s.generation;
  }
  }
  changed(id);
  return gen;
}

Result<void> StateStore::destroy(const StateID& id) {
  {
    hal::LockGuard g(mu_);
    if (!states_.erase(id)) return Result<void>(Status::NotFound, "state " + id.short_str());
  }
  changed(id);   // a durable one: its file goes too
  { hal::LockGuard g(mu_); durable_.erase(id); }
  return Ok();
}

Result<void> StateStore::destroy_owned(const EntityID& owner) {
  std::vector<StateID> gone;
  {
    hal::LockGuard g(mu_);
    for (auto it = states_.begin(); it != states_.end();) { if (it->second.owner == owner) { gone.push_back(it->first); it = states_.erase(it); } else ++it; }
  }
  for (auto& id : gone) changed(id);
  { hal::LockGuard g(mu_); for (auto& id : gone) durable_.erase(id); }
  return Ok();
}

std::vector<StateObject> StateStore::list(const EntityID& owner) const {
  hal::LockGuard g(mu_);
  std::vector<StateObject> out;
  for (auto& [id, s] : states_) if (owner.is_null() || s.owner == owner) out.push_back(s);
  std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.created_ns < b.created_ns; });
  return out;
}
size_t StateStore::count() const { hal::LockGuard g(mu_); return states_.size(); }

}  // namespace aios
