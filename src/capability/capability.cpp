#include "aios/capability/capability.hpp"
#include "aios/hal/sync.hpp"

namespace aios {

const char* resource_kind_name(ResourceKind k) {
  switch (k) {
    case ResourceKind::Memory: return "memory"; case ResourceKind::ComputeDevice: return "compute";
    case ResourceKind::Channel: return "channel"; case ResourceKind::StorageObject: return "storage";
    case ResourceKind::Entity: return "entity"; case ResourceKind::Network: return "network";
    case ResourceKind::State: return "state"; case ResourceKind::Kernel: return "kernel";
  }
  return "?";
}
bool parse_resource_kind(const std::string& s, ResourceKind& out) {
  static const std::pair<const char*, ResourceKind> table[] = {
    {"memory", ResourceKind::Memory}, {"compute", ResourceKind::ComputeDevice}, {"channel", ResourceKind::Channel},
    {"storage", ResourceKind::StorageObject}, {"entity", ResourceKind::Entity}, {"network", ResourceKind::Network},
    {"state", ResourceKind::State}, {"kernel", ResourceKind::Kernel}};
  for (auto& [n, k] : table) if (s == n) { out = k; return true; }
  return false;
}
std::string ResourceRef::str() const { return std::string(resource_kind_name(kind)) + ":" + (id.is_null() ? "*" : id.short_str()); }

std::string rights_str(uint16_t r) {
  static const std::pair<uint16_t, const char*> names[] = {
    {R_Read, "read"}, {R_Write, "write"}, {R_Execute, "execute"}, {R_Grant, "grant"}, {R_Destroy, "destroy"},
    {R_Publish, "publish"}, {R_Consume, "consume"}, {R_Allocate, "allocate"}, {R_Schedule, "schedule"}, {R_Checkpoint, "checkpoint"}};
  std::string s;
  for (auto& [bit, n] : names) if (r & bit) { if (!s.empty()) s += ","; s += n; }
  return s.empty() ? "none" : s;
}
bool parse_rights(const std::string& csv, uint16_t& out) {
  out = 0;
  size_t start = 0;
  while (start <= csv.size()) {
    size_t comma = csv.find(',', start);
    std::string tok = csv.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    start = comma == std::string::npos ? csv.size() + 1 : comma + 1;
    if (tok.empty()) continue;
    if (tok == "read") out |= R_Read; else if (tok == "write") out |= R_Write; else if (tok == "execute") out |= R_Execute;
    else if (tok == "grant") out |= R_Grant; else if (tok == "destroy") out |= R_Destroy; else if (tok == "publish") out |= R_Publish;
    else if (tok == "consume") out |= R_Consume; else if (tok == "allocate") out |= R_Allocate; else if (tok == "schedule") out |= R_Schedule;
    else if (tok == "checkpoint") out |= R_Checkpoint; else if (tok == "all") out |= R_All; else return false;
  }
  return true;
}

void Capability::encode(Writer& w) const {
  w.id(id); w.id(holder); w.u8((uint8_t)resource.kind); w.id(resource.id); w.u16(rights);
  w.u64(scope_limit); w.u64(scope_used); w.u64(expires_ns); w.id(parent); w.u64(issued_ns);
}
Result<Capability> Capability::decode(Reader& r) {
  Capability c;
  c.id = CapabilityID{r.id()}; c.holder = EntityID{r.id()}; c.resource.kind = (ResourceKind)r.u8(); c.resource.id = r.id(); c.rights = r.u16();
  c.scope_limit = r.u64(); c.scope_used = r.u64(); c.expires_ns = r.u64(); c.parent = CapabilityID{r.id()}; c.issued_ns = r.u64();
  if (!r.ok()) return Result<Capability>(Status::Corrupt, "capability record");
  return c;
}

CapabilityManager::CapabilityManager(IdGenerator& ids) : ids_(ids) {}

Result<CapabilityID> CapabilityManager::grant(const EntityID& holder, ResourceRef res, uint16_t rights, uint64_t now_ns, uint64_t expires_ns, uint64_t scope_limit) {
  if (holder.is_null()) return Result<CapabilityID>(Status::Invalid, "capability needs a holder");
  Capability c;
  c.id = ids_.next_as<CapabilityID>();
  c.holder = holder; c.resource = res; c.rights = rights; c.expires_ns = expires_ns; c.scope_limit = scope_limit; c.issued_ns = now_ns;
  { hal::LockGuard g(mu_); caps_[c.id] = c; }
  if (observer_) observer_->issued(c);
  return c.id;
}

Result<CapabilityID> CapabilityManager::install(const Capability& cap) {
  {
    hal::LockGuard g(mu_);
    if (caps_.count(cap.id)) return Result<CapabilityID>(Status::Conflict, "capability identity already present");
    caps_[cap.id] = cap;
  }
  if (observer_) observer_->issued(cap);
  return cap.id;
}

Result<CapabilityID> CapabilityManager::delegate(const CapabilityID& from, const EntityID& to, uint16_t rights, uint64_t now_ns, uint64_t expires_ns, uint64_t scope_limit, const ResourceRef* narrowed) {
  Capability c;
  {
  hal::LockGuard g(mu_);
  auto it = caps_.find(from);
  if (it == caps_.end()) return Result<CapabilityID>(Status::NotFound, "capability " + from.short_str());
  const Capability& src = it->second;
  if (!(src.rights & R_Grant)) return Result<CapabilityID>(Status::Denied, "source capability lacks grant");
  if (src.expires_ns && now_ns >= src.expires_ns) return Result<CapabilityID>(Status::Expired, "source capability expired");
  if ((rights & ~src.rights) != 0) return Result<CapabilityID>(Status::Denied, "delegation may only attenuate: requested " + rights_str(rights & ~src.rights) + " not held");
  if (narrowed && !src.resource.covers(*narrowed)) return Result<CapabilityID>(Status::Denied, "delegation may only attenuate: " + narrowed->str() + " is outside " + src.resource.str());
  // A child's budget is drawn from what its ancestors have LEFT, not from
  // their nominal limit: a used-up capability cannot mint itself a fresh one.
  // (Its consumption is also charged up the chain — see charge().)
  uint64_t room = room_locked(src);
  if (room == 0) return Result<CapabilityID>(Status::Exhausted, "delegation refused: capability " + from.short_str() + " has no scope remaining (" + std::to_string(src.scope_used) + "/" + std::to_string(src.scope_limit) + " used, including what it has delegated)");
  if (scope_limit && room != UINT64_MAX && scope_limit > room) return Result<CapabilityID>(Status::Denied, "delegation may only attenuate: scope " + std::to_string(scope_limit) + " exceeds the " + std::to_string(room) + " remaining on the held capability");
  c.id = ids_.next_as<CapabilityID>();
  c.holder = to; c.resource = narrowed ? *narrowed : src.resource; c.rights = rights; c.parent = from; c.issued_ns = now_ns;
  c.expires_ns = expires_ns == 0 ? src.expires_ns : (src.expires_ns == 0 ? expires_ns : std::min(expires_ns, src.expires_ns));
  c.scope_limit = scope_limit ? scope_limit : (room == UINT64_MAX ? 0 : room);
  caps_[c.id] = c;
  }
  if (observer_) observer_->issued(c);
  return c.id;
}

bool CapabilityManager::descends(const CapabilityID& id, const CapabilityID& ancestor) const {
  hal::LockGuard g(mu_);
  CapabilityID cur = id;
  for (int depth = 0; depth < 64 && !cur.is_null(); ++depth) {
    if (cur == ancestor) return true;
    auto it = caps_.find(cur);
    if (it == caps_.end()) return false;
    cur = it->second.parent;
  }
  return false;
}

Result<void> CapabilityManager::revoke(const CapabilityID& id) {
  std::vector<Capability> gone;
  {
    hal::LockGuard g(mu_);
    if (!caps_.count(id)) return Result<void>(Status::NotFound, "capability " + id.short_str());
    std::vector<CapabilityID> stack{id};
    while (!stack.empty()) {
      CapabilityID cur = stack.back();
      stack.pop_back();
      auto it = caps_.find(cur);
      if (it != caps_.end()) { gone.push_back(it->second); caps_.erase(it); }
      for (auto& [cid, c] : caps_) if (c.parent == cur) stack.push_back(cid);
    }
  }
  if (observer_) for (auto& c : gone) observer_->revoked(c);
  return Ok();
}

Result<void> CapabilityManager::revoke_all(const EntityID& holder) {
  std::vector<CapabilityID> ids;
  { hal::LockGuard g(mu_); for (auto& [id, c] : caps_) if (c.holder == holder) ids.push_back(id); }
  for (auto& id : ids) revoke(id);
  return Ok();
}

bool CapabilityManager::check(const EntityID& holder, const ResourceRef& res, uint16_t rights, uint64_t now_ns) const {
  hal::LockGuard g(mu_);
  for (auto& [id, c] : caps_) {
    if (c.holder != holder) continue;
    if (!c.resource.covers(res)) continue;
    if (c.expires_ns && now_ns >= c.expires_ns) continue;
    if ((c.rights & rights) != rights) continue;
    if (room_locked(c) == 0) continue;   // used up, here or on an ancestor it draws from
    return true;
  }
  return false;
}

Result<void> CapabilityManager::require(const EntityID& holder, const ResourceRef& res, uint16_t rights, uint64_t now_ns) const {
  if (check(holder, res, rights, now_ns)) return Ok();
  return Result<void>(Status::Denied, "entity " + holder.short_str() + " holds no " + rights_str(rights) + " capability over " + res.str());
}

// Smallest scope left along `c` and every ancestor it was delegated from
// (UINT64_MAX when the whole chain is unbounded). Caller holds mu_.
uint64_t CapabilityManager::room_locked(const Capability& c) const {
  uint64_t room = UINT64_MAX;
  const Capability* cur = &c;
  for (int depth = 0; depth < 64 && cur; ++depth) {
    if (cur->scope_limit) room = std::min(room, cur->scope_used >= cur->scope_limit ? 0 : cur->scope_limit - cur->scope_used);
    if (cur->parent.is_null()) break;
    auto it = caps_.find(cur->parent);
    cur = it == caps_.end() ? nullptr : &it->second;
  }
  return room;
}

Result<CapabilityID> CapabilityManager::charge(const EntityID& holder, const ResourceRef& res, uint16_t rights, uint64_t amount, uint64_t now_ns) {
  hal::LockGuard g(mu_);
  Capability* best = nullptr;
  uint64_t best_room = 0;
  for (auto& [id, c] : caps_) {
    if (c.holder != holder || !c.resource.covers(res)) continue;
    if (c.expires_ns && now_ns >= c.expires_ns) continue;
    if ((c.rights & rights) != rights) continue;
    uint64_t room = room_locked(c);
    if (room == UINT64_MAX) return CapabilityID{};   // unbounded all the way up: no accounting
    if (amount <= room && (!best || room > best_room)) { best = &c; best_room = room; }
  }
  if (!best) return Result<CapabilityID>(Status::Exhausted, "no capability of " + holder.short_str() + " over " + res.str() + " has " + std::to_string(amount) + " scope remaining");
  // Charge the capability and every bounded ancestor: a delegated budget is
  // spent out of the parent's.
  Capability* cur = best;
  for (int depth = 0; depth < 64 && cur; ++depth) {
    if (cur->scope_limit) cur->scope_used += amount;
    if (cur->parent.is_null()) break;
    auto it = caps_.find(cur->parent);
    cur = it == caps_.end() ? nullptr : &it->second;
  }
  return best->id;
}

Result<void> CapabilityManager::consume(const EntityID& holder, const ResourceRef& res, uint16_t rights, uint64_t amount, uint64_t now_ns) {
  auto c = charge(holder, res, rights, amount, now_ns);
  if (!c.ok()) return c.error();
  return Ok();
}

// Ids of `c` and its ancestors, nearest first. Caller holds mu_.
std::vector<CapabilityID> CapabilityManager::chain_locked(const CapabilityID& from) const {
  std::vector<CapabilityID> out;
  CapabilityID cur = from;
  for (int depth = 0; depth < 64 && !cur.is_null(); ++depth) {
    auto it = caps_.find(cur);
    if (it == caps_.end()) break;
    out.push_back(cur);
    cur = it->second.parent;
  }
  return out;
}

void CapabilityManager::refund_chain_locked(const std::vector<CapabilityID>& chain, uint64_t amount) {
  for (auto& id : chain) {
    auto it = caps_.find(id);
    if (it == caps_.end() || !it->second.scope_limit) continue;   // revoked meanwhile, or unbounded
    Capability& c = it->second;
    c.scope_used = c.scope_used > amount ? c.scope_used - amount : 0;
  }
}

void CapabilityManager::refund(const CapabilityID& charged, uint64_t amount) {
  if (charged.is_null() || amount == 0) return;
  hal::LockGuard g(mu_);
  refund_chain_locked(chain_locked(charged), amount);
}

void CapabilityManager::record_charge(const Id128& object, const CapabilityID& charged, uint64_t amount) {
  if (charged.is_null() || amount == 0) return;
  hal::LockGuard g(mu_);
  // The chain is captured now, so a release still refunds the ancestors even
  // if the charged capability itself was revoked in between.
  charges_[object] = Charge{chain_locked(charged), amount};
}

void CapabilityManager::refund_object(const Id128& object) {
  hal::LockGuard g(mu_);
  auto it = charges_.find(object);
  if (it == charges_.end()) return;
  Charge ch = std::move(it->second);
  charges_.erase(it);
  refund_chain_locked(ch.chain, ch.amount);
}

std::vector<Capability> CapabilityManager::list(const EntityID& holder) const {
  hal::LockGuard g(mu_);
  std::vector<Capability> out;
  for (auto& [id, c] : caps_) if (holder.is_null() || c.holder == holder) out.push_back(c);
  return out;
}
const Capability* CapabilityManager::get(const CapabilityID& id) const {
  hal::LockGuard g(mu_);
  auto it = caps_.find(id);
  return it == caps_.end() ? nullptr : &it->second;
}
size_t CapabilityManager::count() const { hal::LockGuard g(mu_); return caps_.size(); }

}  // namespace aios
