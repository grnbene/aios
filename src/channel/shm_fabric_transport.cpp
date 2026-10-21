// High-performance Lock-Free Shared Memory Event Fabric Transport
// Backed by 64-byte cacheline-aligned slots and atomic sequences for zero-copy IPC.
#include "aios/channel/state_channel.hpp"
#include <cstring>
#include <atomic>

namespace aios {
namespace {

constexpr uint32_t SHM_FABRIC_MAGIC = 0x46425341u; // 'ASBF' (AIOS Shm Fabric)

struct alignas(64) ShmFabricHeader {
  uint32_t magic;
  uint32_t capacity;
  uint64_t slot_bytes;
  std::atomic<uint64_t> head;
  std::atomic<uint64_t> tail;
  std::atomic<uint64_t> dropped;
  uint8_t pad[24];
};

struct alignas(64) ShmFabricSlotHeader {
  std::atomic<uint64_t> sequence;
  uint64_t publisher_hi;
  uint64_t publisher_lo;
  uint64_t state_hi;
  uint64_t state_lo;
  uint8_t kind;
  uint8_t pad[7];
  uint64_t generation;
  uint64_t published_ns;
  uint32_t payload_len;
  uint32_t memory_count;
};

class ShmFabricTransport final : public Transport {
 public:
  const char* name() const override { return "shm-fabric"; }

  uint64_t region_bytes(uint32_t capacity, uint64_t slot_bytes) const override {
    uint64_t aligned_slot = (sizeof(ShmFabricSlotHeader) + slot_bytes + 63) & ~uint64_t(63);
    return sizeof(ShmFabricHeader) + uint64_t(capacity) * aligned_slot;
  }

  Result<void> format(MemoryFabric& fabric, const MemoryID& region, uint32_t capacity, uint64_t slot_bytes) override {
    auto w = fabric.window(region);
    if (!w.ok()) return w.error();
    uint64_t need = region_bytes(capacity, slot_bytes);
    if (w->size < need) {
      return Result<void>(Status::Invalid, "region too small for shm-fabric: need " + std::to_string(need) + " bytes");
    }
    ShmFabricHeader* h = reinterpret_cast<ShmFabricHeader*>(w->base);
    std::memset(w->base, 0, sizeof(ShmFabricHeader));
    h->magic = SHM_FABRIC_MAGIC;
    h->capacity = capacity;
    h->slot_bytes = slot_bytes;
    h->head.store(0, std::memory_order_relaxed);
    h->tail.store(0, std::memory_order_relaxed);
    h->dropped.store(0, std::memory_order_relaxed);
    return Ok();
  }

  Result<uint64_t> publish(MemoryFabric& fabric, const MemoryID& region, const StateFrame& f) override {
    auto w = fabric.window(region);
    if (!w.ok()) return w.error();
    ShmFabricHeader* h = reinterpret_cast<ShmFabricHeader*>(w->base);
    if (h->magic != SHM_FABRIC_MAGIC) return Result<uint64_t>(Status::Corrupt, "shm-fabric header magic mismatch");

    uint64_t body = f.payload.size() + f.memory.size() * 16;
    if (body > h->slot_bytes) {
      return Result<uint64_t>(Status::Invalid, "frame (" + std::to_string(body) + " bytes) exceeds slot size " + std::to_string(h->slot_bytes));
    }

    uint64_t seq = h->head.fetch_add(1, std::memory_order_acq_rel);
    uint64_t aligned_slot = (sizeof(ShmFabricSlotHeader) + h->slot_bytes + 63) & ~uint64_t(63);
    uint8_t* slot = w->base + sizeof(ShmFabricHeader) + (seq % h->capacity) * aligned_slot;

    ShmFabricSlotHeader* sh = reinterpret_cast<ShmFabricSlotHeader*>(slot);
    sh->publisher_hi = f.publisher.hi;
    sh->publisher_lo = f.publisher.lo;
    sh->state_hi = f.state.hi;
    sh->state_lo = f.state.lo;
    sh->kind = (uint8_t)f.kind;
    sh->generation = f.generation;
    sh->published_ns = f.published_ns;
    sh->payload_len = (uint32_t)f.payload.size();
    sh->memory_count = (uint32_t)f.memory.size();

    uint8_t* p = slot + sizeof(ShmFabricSlotHeader);
    if (!f.payload.empty()) {
      std::memcpy(p, f.payload.data(), f.payload.size());
    }
    p += f.payload.size();
    for (const auto& m : f.memory) {
      std::memcpy(p, &m.hi, 8);
      std::memcpy(p + 8, &m.lo, 8);
      p += 16;
    }

    // Mark slot ready with its sequence number using release semantics
    sh->sequence.store(seq, std::memory_order_release);

    // Update tail if ring wraps around
    uint64_t cur_tail = h->tail.load(std::memory_order_relaxed);
    if (seq >= cur_tail + h->capacity) {
      uint64_t new_tail = seq - h->capacity + 1;
      if (h->tail.compare_exchange_strong(cur_tail, new_tail, std::memory_order_relaxed)) {
        h->dropped.fetch_add(new_tail - cur_tail, std::memory_order_relaxed);
      }
    }

    fabric.touch(region);
    return seq;
  }

  Result<StateFrame> consume(MemoryFabric& fabric, const MemoryID& region, ChannelCursor& cursor) override {
    auto w = fabric.window(region);
    if (!w.ok()) return w.error();
    ShmFabricHeader* h = reinterpret_cast<ShmFabricHeader*>(w->base);
    if (h->magic != SHM_FABRIC_MAGIC) return Result<StateFrame>(Status::Corrupt, "shm-fabric header magic mismatch");

    uint64_t head = h->head.load(std::memory_order_acquire);
    uint64_t tail = h->tail.load(std::memory_order_acquire);

    if (cursor.next_sequence < tail) {
      cursor.lost += tail - cursor.next_sequence;
      cursor.next_sequence = tail;
    }
    if (cursor.next_sequence >= head) {
      return Result<StateFrame>(Status::Busy, "no new frames");
    }

    uint64_t seq = cursor.next_sequence;
    uint64_t aligned_slot = (sizeof(ShmFabricSlotHeader) + h->slot_bytes + 63) & ~uint64_t(63);
    const uint8_t* slot = w->base + sizeof(ShmFabricHeader) + (seq % h->capacity) * aligned_slot;
    const ShmFabricSlotHeader* sh = reinterpret_cast<const ShmFabricSlotHeader*>(slot);

    uint64_t slot_seq = sh->sequence.load(std::memory_order_acquire);
    if (slot_seq != seq) {
      return Result<StateFrame>(Status::Corrupt, "shm-fabric slot sequence mismatch");
    }

    StateFrame f;
    f.sequence = seq;
    f.publisher = EntityID{Id128{sh->publisher_hi, sh->publisher_lo}};
    f.state = StateID{Id128{sh->state_hi, sh->state_lo}};
    f.kind = (StateKind)sh->kind;
    f.generation = sh->generation;
    f.published_ns = sh->published_ns;

    const uint8_t* p = slot + sizeof(ShmFabricSlotHeader);
    f.payload.assign(p, p + sh->payload_len);
    p += sh->payload_len;
    for (uint32_t i = 0; i < sh->memory_count; ++i) {
      Id128 m;
      std::memcpy(&m.hi, p, 8);
      std::memcpy(&m.lo, p + 8, 8);
      p += 16;
      f.memory.push_back(MemoryID{m});
    }

    cursor.next_sequence = seq + 1;
    return f;
  }

  Result<void> stats(MemoryFabric& fabric, const MemoryID& region, uint64_t& head, uint64_t& tail, uint64_t& dropped) override {
    auto w = fabric.window(region);
    if (!w.ok()) return w.error();
    ShmFabricHeader* h = reinterpret_cast<ShmFabricHeader*>(w->base);
    if (h->magic != SHM_FABRIC_MAGIC) return Result<void>(Status::Corrupt, "shm-fabric header magic mismatch");
    head = h->head.load(std::memory_order_relaxed);
    tail = h->tail.load(std::memory_order_relaxed);
    dropped = h->dropped.load(std::memory_order_relaxed);
    return Ok();
  }
};

}  // namespace

std::unique_ptr<Transport> make_shm_fabric_transport() {
  return std::make_unique<ShmFabricTransport>();
}

}  // namespace aios
