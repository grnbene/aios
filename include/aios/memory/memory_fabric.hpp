#pragma once
// The MemoryFabric unifies system RAM, shared, persistent, device and remote
// memory behind MemoryObjects. Phase 1 ships the host-backed providers; device
// and remote providers register through the same interface.
#include <unordered_map>
#include "aios/hal/sync.hpp"
#include <memory>
#include <vector>
#include <functional>
#include "aios/memory/memory_object.hpp"
#include "aios/core/result.hpp"
#include "aios/core/bytes.hpp"

namespace aios {

class MemoryProvider {
 public:
  virtual ~MemoryProvider() = default;
  virtual MemoryType type() const = 0;
  virtual Result<hal::Mapping> allocate(uint64_t size, const MemoryID& id) = 0;
  // Gives the mapping back. Anything the provider laid down OUTSIDE the
  // mapping -- a persistent object's backing file -- outlives this call.
  virtual void release(hal::Mapping& m) = 0;
  // The object is gone for good: its identity will never be allocated again,
  // so the backing store goes with it. Shutdown uses release(), destruction
  // uses this; the difference is whether the bytes are meant to come back.
  virtual void destroy(hal::Mapping& m) { release(m); }
  virtual uint64_t capacity() const = 0;
};

struct MemoryStats {
  uint64_t objects = 0;
  uint64_t bytes = 0;
  uint64_t bytes_by_type[5] = {0, 0, 0, 0, 0};
  uint64_t migrations = 0;
};

class MemoryFabric {
 public:
  MemoryFabric(hal::Host& host, IdGenerator& ids);
  ~MemoryFabric();

  void register_provider(std::unique_ptr<MemoryProvider> p);
  bool has_provider(MemoryType t) const;
  MemoryProvider* provider(MemoryType t) const;   // the registered one, or null

  Result<MemoryID> allocate(const EntityID& owner, uint64_t size, MemoryType type = MemoryType::System, uint8_t perms = MemRead | MemWrite);
  // Recreates an object with a KNOWN identity (restore path). Fails on collision.
  Result<MemoryID> allocate_with_id(const MemoryID& id, const EntityID& owner, uint64_t size, MemoryType type, uint8_t perms, uint64_t generation);
  Result<void> release(const MemoryID& id);
  Result<void> release_owned(const EntityID& owner);

  const MemoryObject* lookup(const MemoryID& id) const;
  Result<void> write(const MemoryID& id, uint64_t offset, ByteSpan data);
  Result<ByteBuffer> read(const MemoryID& id, uint64_t offset, uint64_t length) const;
  Result<void> fill(const MemoryID& id, uint8_t byte);
  Result<void> transfer_owner(const MemoryID& id, const EntityID& new_owner);

  // Moves the object's contents to another memory type/location keeping the
  // identity. Entities never observe the move except through `generation`.
  Result<void> migrate(const MemoryID& id, MemoryType to);

  // Device-side access. Only devices and transports may use this.
  struct Window { uint8_t* base; uint64_t size; };
  Result<Window> window(const MemoryID& id);

  struct CompactResult {
    uint64_t objects_checked = 0;
    uint64_t objects_released = 0;
    uint64_t bytes_reclaimed = 0;
  };
  // Reclaims memory nothing can reach any more. `owner_alive` answers whether
  // an entity id still exists; without it the sweep can only see objects with
  // no owner at all, which is the rarer half of the problem. The fabric does
  // not know about entities, so the kernel supplies the oracle.
  using OwnerAlive = std::function<bool(const EntityID&)>;
  CompactResult compact_garbage(OwnerAlive owner_alive = {});

  std::vector<MemoryObject> list(const EntityID& owner = {}) const;
  MemoryStats stats() const;
  void touch(const MemoryID& id);   // bump generation without a kernel write (device wrote)

 private:
  hal::Host& host_;
  IdGenerator& ids_;
  mutable hal::Mutex mu_;
  std::unordered_map<MemoryID, MemoryObject, TypedIdHash<2>> objects_;
  std::unordered_map<uint8_t, std::unique_ptr<MemoryProvider>> providers_;
  uint64_t migrations_ = 0;
};

// Host-backed providers.
std::unique_ptr<MemoryProvider> make_system_ram_provider(hal::Host& host);
std::unique_ptr<MemoryProvider> make_shared_memory_provider(hal::Host& host);
std::unique_ptr<MemoryProvider> make_persistent_memory_provider(hal::Host& host, const std::string& directory);

}  // namespace aios
