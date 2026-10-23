#pragma once

#include <cstdint>
#include "aios/hal/sync.hpp"
#include "aios/hal/host.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <functional>
#include <mutex>

namespace aios::sidecar {

struct EventMessage {
  uint64_t sequence{0};
  uint64_t timestamp_ns{0};
  uint32_t topic_id{0};
  uint32_t payload_len{0};
  uint8_t payload[232]{0}; // Inlined fixed payload for zero-allocation fast path
};

static_assert(sizeof(EventMessage) == 256, "EventMessage must be 256 bytes");

class EventPartition {
public:
  explicit EventPartition(size_t ring_capacity = 65536);
  ~EventPartition();
  EventPartition(const EventPartition&) = delete;
  EventPartition& operator=(const EventPartition&) = delete;

  // Publish an event message into the partition ring
  uint64_t publish(uint32_t topic_id, const void* data, size_t len);

  // Read message at specific sequence
  bool read_at(uint64_t sequence, EventMessage& out_msg) const;

  uint64_t head_sequence() const { return head_sequence_.load(std::memory_order_relaxed); }
  size_t capacity() const { return capacity_; }

private:
  size_t capacity_{0};
  // The ring is mapped, not allocated: a mapping's untouched pages read as zero (what a fresh EventMessage is) and
  // cost nothing until a slot on them is written, so a topic no one publishes to costs no memory. Allocated with
  // new[] it was zero-filled up front: 16 MB resident per partition, 224 MB of aios-engine's 278 MB at rest for 37 MB
  // of tables (14 partitions, nothing published to most of them).
  EventMessage* ring_{nullptr};
  bool mapped_{false};
  hal::Mapping mapping_{};   // the ring's pages, from the host (zero until touched)
  std::atomic<uint64_t> head_sequence_{0};
};

class EventFabric {
public:
  EventFabric();
  ~EventFabric() = default;

  // Register topic and return topic_id
  uint32_t register_topic(const std::string& name, size_t partitions = 1);

  // Publish message to topic (hashes key for partition selection)
  uint64_t publish(const std::string& topic_name, const void* data, size_t len, uint32_t partition_key = 0);

  // Poll consumer subscription
  struct ConsumerCursor {
    uint32_t topic_id{0};
    uint32_t partition_idx{0};
    uint64_t last_consumed_seq{0};
  };

  std::vector<EventMessage> poll(ConsumerCursor& cursor, size_t max_batch = 100);

  size_t total_topics() const;

private:
  mutable aios::hal::Mutex mutex_;
  std::unordered_map<std::string, uint32_t> topic_ids_;
  std::vector<std::string> topic_names_;
  std::unordered_map<uint32_t, std::vector<std::unique_ptr<EventPartition>>> topic_partitions_;
};

} // namespace aios::sidecar
