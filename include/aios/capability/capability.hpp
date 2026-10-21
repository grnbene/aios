#pragma once
// Security is "what can this entity actually touch", expressed as unforgeable
// capabilities the kernel issues, attenuates, and revokes. There is no root.
#include <cstdint>
#include "aios/hal/sync.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include "aios/core/ids.hpp"
#include "aios/core/result.hpp"
#include "aios/core/bytes.hpp"

namespace aios {

enum class ResourceKind : uint8_t { Memory = 0, ComputeDevice = 1, Channel = 2, StorageObject = 3, Entity = 4, Network = 5, State = 6, Kernel = 7 };
const char* resource_kind_name(ResourceKind k);
bool parse_resource_kind(const std::string& s, ResourceKind& out);

// A null id means "every resource of this kind" (a class capability).
struct ResourceRef {
  ResourceKind kind = ResourceKind::Memory;
  Id128 id;
  bool covers(const ResourceRef& r) const { return kind == r.kind && (id.is_null() || id == r.id); }
  std::string str() const;
};

inline Id128 disk_id(const std::string& name) {
  Id128 parsed;
  if (Id128::parse(name, parsed)) return parsed;
  uint64_t h1 = fnv1a(span_of(name), 0xcbf29ce484222325ull);
  uint64_t h2 = fnv1a(span_of(name), 0x100000001b3ull);
  if (h1 == 0 && h2 == 0) h2 = 1;
  return Id128{h1, h2};
}

enum Right : uint16_t {
  R_Read = 1 << 0,
  R_Write = 1 << 1,
  R_Execute = 1 << 2,
  R_Grant = 1 << 3,       // may delegate an attenuated copy
  R_Destroy = 1 << 4,
  R_Publish = 1 << 5,
  R_Consume = 1 << 6,
  R_Allocate = 1 << 7,
  R_Schedule = 1 << 8,
  R_Checkpoint = 1 << 9,
  R_All = 0x3FF,
};
std::string rights_str(uint16_t r);
bool parse_rights(const std::string& csv, uint16_t& out);

struct Capability {
  CapabilityID id;
  EntityID holder;
  ResourceRef resource;
  uint16_t rights = 0;
  uint64_t scope_limit = 0;      // e.g. bytes for Allocate; 0 = unbounded
  uint64_t scope_used = 0;
  uint64_t expires_ns = 0;       // wall clock; 0 = never
  CapabilityID parent;           // delegated from
  uint64_t issued_ns = 0;

  void encode(Writer& w) const;
  static Result<Capability> decode(Reader& r);
};

// Told about every capability that comes into or goes out of existence, so
// authority can be persisted and audited by whoever owns the manager.
class CapabilityObserver {
 public:
  virtual ~CapabilityObserver() = default;
  virtual void issued(const Capability& c) = 0;
  virtual void revoked(const Capability& c) = 0;
};

class CapabilityManager {
 public:
  explicit CapabilityManager(IdGenerator& ids);
  void observe(CapabilityObserver* o) { observer_ = o; }

  // Kernel authority. Only the kernel calls this directly.
  Result<CapabilityID> grant(const EntityID& holder, ResourceRef res, uint16_t rights, uint64_t now_ns, uint64_t expires_ns = 0, uint64_t scope_limit = 0);
  Result<CapabilityID> install(const Capability& cap);   // restore path
  // An entity delegates a subset of one of its capabilities to another entity.
  // scope_limit narrows the source's scope (0 = inherit); it may never widen it.
  // `narrowed`, when given, is the resource the new capability covers instead of the source's own; it must lie within
  // the source's (a class capability can hand out one object of its kind, never more). Delegation may only attenuate:
  // fewer rights, a smaller scope, a narrower resource. Without it the delegate covers exactly what the source does.
  Result<CapabilityID> delegate(const CapabilityID& from, const EntityID& to, uint16_t rights, uint64_t now_ns, uint64_t expires_ns = 0, uint64_t scope_limit = 0,
                                const ResourceRef* narrowed = nullptr);
  // True when `id` is `ancestor` or was delegated (transitively) from it.
  bool descends(const CapabilityID& id, const CapabilityID& ancestor) const;
  Result<void> revoke(const CapabilityID& id);           // cascades to delegated children
  Result<void> revoke_all(const EntityID& holder);

  bool check(const EntityID& holder, const ResourceRef& res, uint16_t rights, uint64_t now_ns) const;
  Result<void> require(const EntityID& holder, const ResourceRef& res, uint16_t rights, uint64_t now_ns) const;
  // Consumes scope for scoped capabilities (e.g. allocation budgets). The
  // amount is charged to the chosen capability AND to every bounded ancestor
  // it was delegated from, so a delegated budget is spent out of its parent's.
  Result<void> consume(const EntityID& holder, const ResourceRef& res, uint16_t rights, uint64_t amount, uint64_t now_ns);
  // As consume, returning the capability charged (null when the chain is unbounded).
  Result<CapabilityID> charge(const EntityID& holder, const ResourceRef& res, uint16_t rights, uint64_t amount, uint64_t now_ns);
  // Gives `amount` back to `charged` and its ancestors (e.g. a failed allocation).
  void refund(const CapabilityID& charged, uint64_t amount);
  // Remember what an object cost, so releasing it returns the scope.
  void record_charge(const Id128& object, const CapabilityID& charged, uint64_t amount);
  void refund_object(const Id128& object);

  std::vector<Capability> list(const EntityID& holder) const;
  const Capability* get(const CapabilityID& id) const;
  size_t count() const;

 private:
  IdGenerator& ids_;
  CapabilityObserver* observer_ = nullptr;
  mutable hal::Mutex mu_;
  std::unordered_map<CapabilityID, Capability, TypedIdHash<5>> caps_;
  struct Charge { std::vector<CapabilityID> chain; uint64_t amount = 0; };
  std::unordered_map<Id128, Charge, Id128Hash> charges_;
  uint64_t room_locked(const Capability& c) const;
  std::vector<CapabilityID> chain_locked(const CapabilityID& from) const;
  void refund_chain_locked(const std::vector<CapabilityID>& chain, uint64_t amount);
};

}  // namespace aios
