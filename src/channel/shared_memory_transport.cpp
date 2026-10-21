// A ring of fixed-size frame slots laid out inside a Shared memory object.
// Header and slots live in the region itself, so another address space that
// maps the same object sees the same ring.
#include "aios/channel/state_channel.hpp"
#include <cstring>
#include <atomic>

namespace aios {
namespace {

struct RingHeader {
  uint32_t magic;       // 'AOSR'
  uint32_t capacity;
  uint64_t slot_bytes;
  uint64_t head;        // next sequence to write
  uint64_t tail;        // oldest sequence still present
  uint64_t dropped;
};
constexpr uint32_t RING_MAGIC = 0x52534F41u;

struct SlotHeader {
  uint64_t sequence;
  uint64_t publisher_hi, publisher_lo;
  uint64_t state_hi, state_lo;
  uint8_t kind;
  uint8_t pad[7];
  uint64_t generation;
  uint64_t published_ns;
  uint32_t payload_len;
  uint32_t memory_count;
};

class SharedMemoryTransport final : public Transport {
 public:
  const char* name() const override { return "shared-memory"; }
  uint64_t region_bytes(uint32_t capacity, uint64_t slot_bytes) const override { return sizeof(RingHeader) + uint64_t(capacity) * (sizeof(SlotHeader) + slot_bytes); }

  Result<void> format(MemoryFabric& fabric, const MemoryID& region, uint32_t capacity, uint64_t slot_bytes) override {
    auto w = fabric.window(region);
    if (!w.ok()) return w.error();
    uint64_t need = region_bytes(capacity, slot_bytes);
    if (w->size < need) return Result<void>(Status::Invalid, "region too small for ring: need " + std::to_string(need) + " bytes");
    RingHeader h{RING_MAGIC, capacity, slot_bytes, 0, 0, 0};
    std::memcpy(w->base, &h, sizeof h);
    return Ok();
  }

  Result<uint64_t> publish(MemoryFabric& fabric, const MemoryID& region, const StateFrame& f) override {
    auto w = fabric.window(region);
    if (!w.ok()) return w.error();
    RingHeader* h = reinterpret_cast<RingHeader*>(w->base);
    if (h->magic != RING_MAGIC) return Result<uint64_t>(Status::Corrupt, "ring header");
    uint64_t body = f.payload.size() + f.memory.size() * 16;
    if (body > h->slot_bytes) return Result<uint64_t>(Status::Invalid, "frame (" + std::to_string(body) + " bytes) exceeds slot size " + std::to_string(h->slot_bytes));
    uint64_t seq = h->head;
    uint8_t* slot = w->base + sizeof(RingHeader) + (seq % h->capacity) * (sizeof(SlotHeader) + h->slot_bytes);
    SlotHeader sh{};
    sh.sequence = seq;
    sh.publisher_hi = f.publisher.hi; sh.publisher_lo = f.publisher.lo;
    sh.state_hi = f.state.hi; sh.state_lo = f.state.lo;
    sh.kind = (uint8_t)f.kind; sh.generation = f.generation; sh.published_ns = f.published_ns;
    sh.payload_len = (uint32_t)f.payload.size(); sh.memory_count = (uint32_t)f.memory.size();
    std::memcpy(slot, &sh, sizeof sh);
    uint8_t* p = slot + sizeof(SlotHeader);
    if (!f.payload.empty()) std::memcpy(p, f.payload.data(), f.payload.size());
    p += f.payload.size();
    for (auto& m : f.memory) { std::memcpy(p, &m.hi, 8); std::memcpy(p + 8, &m.lo, 8); p += 16; }
    // Publication order: the body is complete before the head advances.
    std::atomic_thread_fence(std::memory_order_release);
    h->head = seq + 1;
    if (h->head - h->tail > h->capacity) { h->tail = h->head - h->capacity; h->dropped++; }
    fabric.touch(region);
    return seq;
  }

  Result<StateFrame> consume(MemoryFabric& fabric, const MemoryID& region, ChannelCursor& cursor) override {
    auto w = fabric.window(region);
    if (!w.ok()) return w.error();
    RingHeader* h = reinterpret_cast<RingHeader*>(w->base);
    if (h->magic != RING_MAGIC) return Result<StateFrame>(Status::Corrupt, "ring header");
    std::atomic_thread_fence(std::memory_order_acquire);
    if (cursor.next_sequence < h->tail) { cursor.lost += h->tail - cursor.next_sequence; cursor.next_sequence = h->tail; }
    if (cursor.next_sequence >= h->head) return Result<StateFrame>(Status::Busy, "no new frames");
    uint64_t seq = cursor.next_sequence;
    const uint8_t* slot = w->base + sizeof(RingHeader) + (seq % h->capacity) * (sizeof(SlotHeader) + h->slot_bytes);
    SlotHeader sh;
    std::memcpy(&sh, slot, sizeof sh);
    if (sh.sequence != seq) return Result<StateFrame>(Status::Corrupt, "slot sequence mismatch");
    StateFrame f;
    f.sequence = seq;
    f.publisher = EntityID{Id128{sh.publisher_hi, sh.publisher_lo}};
    f.state = StateID{Id128{sh.state_hi, sh.state_lo}};
    f.kind = (StateKind)sh.kind; f.generation = sh.generation; f.published_ns = sh.published_ns;
    const uint8_t* p = slot + sizeof(SlotHeader);
    f.payload.assign(p, p + sh.payload_len);
    p += sh.payload_len;
    for (uint32_t i = 0; i < sh.memory_count; ++i) { Id128 m; std::memcpy(&m.hi, p, 8); std::memcpy(&m.lo, p + 8, 8); p += 16; f.memory.push_back(MemoryID{m}); }
    cursor.next_sequence = seq + 1;
    return f;
  }

  Result<void> stats(MemoryFabric& fabric, const MemoryID& region, uint64_t& head, uint64_t& tail, uint64_t& dropped) override {
    auto w = fabric.window(region);
    if (!w.ok()) return w.error();
    RingHeader* h = reinterpret_cast<RingHeader*>(w->base);
    head = h->head; tail = h->tail; dropped = h->dropped;
    return Ok();
  }
};

}  // namespace

std::unique_ptr<Transport> make_shared_memory_transport() { return std::make_unique<SharedMemoryTransport>(); }

}  // namespace aios
