#pragma once
// Communication is shared state, not messages. A StateChannel is a region of
// memory into which one entity publishes state frames and from which other
// entities consume them. The Transport decides HOW the region is shared
// (memory today; DMA, device memory or a network fabric later) — the channel
// abstraction does not change.
#include <memory>
#include "aios/hal/sync.hpp"
#include <unordered_map>
#include <vector>
#include "aios/core/ids.hpp"
#include "aios/core/result.hpp"
#include "aios/core/bytes.hpp"
#include "aios/memory/memory_fabric.hpp"
#include "aios/state/state.hpp"

namespace aios {

struct StateFrame {
  uint64_t sequence = 0;
  EntityID publisher;
  StateID state;
  StateKind kind = StateKind::Working;
  uint64_t generation = 0;
  uint64_t published_ns = 0;
  ByteBuffer payload;
  std::vector<MemoryID> memory;   // references, not copies: consumers hold ids
};

struct ChannelCursor {
  EntityID consumer;
  uint64_t next_sequence = 0;
  uint64_t lost = 0;             // frames overwritten before this consumer read them
};

class Transport {
 public:
  virtual ~Transport() = default;
  virtual const char* name() const = 0;
  // Bytes of backing memory a ring of `capacity` frames of `slot_bytes` needs.
  virtual uint64_t region_bytes(uint32_t capacity, uint64_t slot_bytes) const = 0;
  // Formats the backing memory object as an empty ring able to hold `capacity` frames.
  virtual Result<void> format(MemoryFabric& fabric, const MemoryID& region, uint32_t capacity, uint64_t slot_bytes) = 0;
  virtual Result<uint64_t> publish(MemoryFabric& fabric, const MemoryID& region, const StateFrame& f) = 0;
  virtual Result<StateFrame> consume(MemoryFabric& fabric, const MemoryID& region, ChannelCursor& cursor) = 0;
  virtual Result<void> stats(MemoryFabric& fabric, const MemoryID& region, uint64_t& head, uint64_t& tail, uint64_t& dropped) = 0;
};

std::unique_ptr<Transport> make_shared_memory_transport();
std::unique_ptr<Transport> make_shm_fabric_transport();

struct Channel {
  ChannelID id;
  EntityID owner;
  MemoryID region;
  uint32_t capacity = 0;
  uint64_t slot_bytes = 0;
  uint64_t created_ns = 0;
  uint64_t published = 0;
  std::string transport;
  std::vector<ChannelCursor> cursors;
};

class ChannelManager {
 public:
  ChannelManager(IdGenerator& ids, MemoryFabric& fabric);
  void register_transport(std::unique_ptr<Transport> t);
  Result<ChannelID> create(const EntityID& owner, uint32_t capacity, uint64_t slot_bytes, uint64_t now_ns, const std::string& transport = "shared-memory");
  Result<uint64_t> region_bytes(uint32_t capacity, uint64_t slot_bytes, const std::string& transport = "shared-memory") const;
  Result<void> subscribe(const ChannelID& ch, const EntityID& consumer);
  Result<void> unsubscribe(const ChannelID& ch, const EntityID& consumer);
  Result<uint64_t> publish(const ChannelID& ch, const EntityID& publisher, const StateObject& state, uint64_t now_ns);
  Result<StateFrame> consume(const ChannelID& ch, const EntityID& consumer);
  Result<void> destroy(const ChannelID& ch);
  Result<void> install(const Channel& c);   // restore path
  Result<void> destroy_owned(const EntityID& owner);
  const Channel* get(const ChannelID& ch) const;
  std::vector<Channel> list() const;
  Result<void> stats(const ChannelID& ch, uint64_t& head, uint64_t& tail, uint64_t& dropped);
 private:
  IdGenerator& ids_;
  MemoryFabric& fabric_;
  mutable hal::Mutex mu_;
  std::unordered_map<std::string, std::unique_ptr<Transport>> transports_;
  std::unordered_map<ChannelID, Channel, TypedIdHash<4>> channels_;
};

}  // namespace aios
