#pragma once
// The fundamental OS object. Not a process: an Entity possesses identity,
// state, memory, capabilities, an execution graph and relationships. A model, a
// sensor, a storage service, a hardware controller and a human interface are
// all entities; the kernel does not care which.
#include <string>
#include "aios/hal/sync.hpp"
#include <vector>
#include <unordered_map>
#include "aios/core/ids.hpp"
#include "aios/core/result.hpp"
#include "aios/core/bytes.hpp"

namespace aios {

enum class EntityStatus : uint8_t { Dormant = 0, Active = 1, Suspended = 2, Checkpointed = 3, Terminated = 4 };
const char* entity_status_name(EntityStatus s);

// Relationship kinds are opaque to the kernel beyond a few it needs itself.
enum RelationKind : uint32_t { Rel_Parent = 1, Rel_Child = 2, Rel_Peer = 3, Rel_Consumes = 4, Rel_Provides = 5, Rel_Mind = 6, Rel_User = 0x1000 };
const char* relation_kind_name(uint32_t k);
bool parse_relation_kind(const std::string& s, uint32_t& out);

struct Relationship {
  EntityID peer;
  uint32_t kind = Rel_Peer;
  uint64_t since_ns = 0;
};

struct Entity {
  EntityID id;
  std::string label;                 // opaque human tag; carries no semantics for the kernel
  uint64_t created_ns = 0;
  uint64_t generation = 0;
  EntityStatus status = EntityStatus::Dormant;
  std::vector<StateID> states;
  std::vector<MemoryID> memory;
  std::vector<CapabilityID> capabilities;
  GraphID execution;                 // current execution graph, if any
  uint64_t execution_position = 0;   // operations completed in that graph
  std::vector<Relationship> relationships;
  uint64_t checkpoints = 0;
  uint64_t restores = 0;

  void encode(Writer& w) const;
  static Result<Entity> decode(Reader& r);
};

class EntityManager {
 public:
  explicit EntityManager(IdGenerator& ids);
  Result<EntityID> create(const std::string& label, uint64_t now_ns);
  Result<EntityID> install(const Entity& e);         // restore path: identity preserved
  const Entity* get(const EntityID& id) const;
  Result<Entity> copy(const EntityID& id) const;
  bool exists(const EntityID& id) const;
  std::vector<Entity> list() const;
  size_t count() const;

  Result<void> set_status(const EntityID& id, EntityStatus s);
  Result<void> set_execution(const EntityID& id, const GraphID& g, uint64_t position);
  Result<void> relate(const EntityID& a, const EntityID& b, uint32_t kind, uint64_t now_ns);
  Result<void> unrelate(const EntityID& a, const EntityID& b, uint32_t kind);
  Result<void> add_state(const EntityID& id, const StateID& s);
  Result<void> add_memory(const EntityID& id, const MemoryID& m);
  Result<void> remove_memory(const EntityID& id, const MemoryID& m);
  Result<void> add_capability(const EntityID& id, const CapabilityID& c);
  Result<void> remove_capability(const EntityID& id, const CapabilityID& c);
  Result<void> note_checkpoint(const EntityID& id);
  Result<void> note_restore(const EntityID& id);
  Result<void> remove(const EntityID& id);

 private:
  template <typename F> Result<void> mutate(const EntityID& id, F f);
  IdGenerator& ids_;
  mutable hal::Mutex mu_;
  std::unordered_map<EntityID, Entity, TypedIdHash<1>> entities_;
};

}  // namespace aios
