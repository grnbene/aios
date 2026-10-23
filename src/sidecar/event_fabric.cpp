#include "aios/sidecar/event_fabric.hpp"
#include <chrono>
#include "aios/hal/sync.hpp"
#include <cstring>
#include <algorithm>

namespace aios::sidecar {

EventPartition::EventPartition(size_t ring_capacity)
    : capacity_(ring_capacity) {
  const size_t bytes = capacity_ * sizeof(EventMessage);
  // Pages from the host: zero, and not resident until touched (a 15 MB ring costs what it holds).
  auto m = hal::process_host().map(bytes, hal::MappingKind::Private);
  if (m.ok() && m->base) { mapping_ = *m; ring_ = static_cast<EventMessage*>(mapping_.base); mapped_ = true; }
  if (!ring_) ring_ = new EventMessage[capacity_]();   // no mappings here: zeroed the old way
}

EventPartition::~EventPartition() {
  if (!ring_) return;
  if (mapped_) { hal::process_host().unmap(mapping_); return; }
  delete[] ring_;
}

uint64_t EventPartition::publish(uint32_t topic_id, const void* data, size_t len) {
  uint64_t seq = head_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
  size_t slot = seq % capacity_;

  uint64_t now_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());

  EventMessage& msg = ring_[slot];
  msg.sequence = seq;
  msg.timestamp_ns = now_ns;
  msg.topic_id = topic_id;
  msg.payload_len = static_cast<uint32_t>(std::min(len, sizeof(msg.payload)));
  if (data != nullptr && msg.payload_len > 0) {
    std::memcpy(msg.payload, data, msg.payload_len);
  }

  return seq;
}

bool EventPartition::read_at(uint64_t sequence, EventMessage& out_msg) const {
  if (sequence == 0) return false;
  uint64_t head = head_sequence_.load(std::memory_order_relaxed);
  if (sequence > head) return false;
  if (head > capacity_ && sequence <= (head - capacity_)) {
    return false; // Lagged past capacity
  }

  size_t slot = sequence % capacity_;
  const EventMessage& msg = ring_[slot];
  if (msg.sequence != sequence) return false;

  out_msg = msg;
  return true;
}

EventFabric::EventFabric() {
  topic_names_.push_back("__system__");
}

uint32_t EventFabric::register_topic(const std::string& name, size_t partitions) {
  std::lock_guard<aios::hal::Mutex> lock(mutex_);
  auto it = topic_ids_.find(name);
  if (it != topic_ids_.end()) {
    return it->second;
  }

  uint32_t id = static_cast<uint32_t>(topic_names_.size());
  topic_names_.push_back(name);
  topic_ids_[name] = id;

  size_t count = std::max<size_t>(1, partitions);
  auto& parts = topic_partitions_[id];
  parts.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    parts.push_back(std::make_unique<EventPartition>());
  }

  return id;
}

uint64_t EventFabric::publish(const std::string& topic_name, const void* data, size_t len, uint32_t partition_key) {
  uint32_t topic_id = 0;
  EventPartition* part_ptr = nullptr;

  {
    std::lock_guard<aios::hal::Mutex> lock(mutex_);
    auto it = topic_ids_.find(topic_name);
    if (it == topic_ids_.end()) {
      topic_id = static_cast<uint32_t>(topic_names_.size());
      topic_names_.push_back(topic_name);
      topic_ids_[topic_name] = topic_id;
      topic_partitions_[topic_id].push_back(std::make_unique<EventPartition>());
      part_ptr = topic_partitions_[topic_id][0].get();
    } else {
      topic_id = it->second;
      const auto& parts = topic_partitions_[topic_id];
      size_t p_idx = partition_key % parts.size();
      part_ptr = parts[p_idx].get();
    }
  }

  return part_ptr ? part_ptr->publish(topic_id, data, len) : 0;
}

std::vector<EventMessage> EventFabric::poll(ConsumerCursor& cursor, size_t max_batch) {
  std::vector<EventMessage> batch;
  EventPartition* part_ptr = nullptr;

  {
    std::lock_guard<aios::hal::Mutex> lock(mutex_);
    auto it = topic_partitions_.find(cursor.topic_id);
    if (it == topic_partitions_.end()) return batch;
    if (cursor.partition_idx >= it->second.size()) return batch;
    part_ptr = it->second[cursor.partition_idx].get();
  }

  if (!part_ptr) return batch;

  uint64_t next_seq = cursor.last_consumed_seq + 1;
  uint64_t head = part_ptr->head_sequence();

  // If consumer fell behind beyond buffer capacity, catch up to oldest available
  if (head > part_ptr->capacity() && next_seq <= (head - part_ptr->capacity())) {
    next_seq = head - part_ptr->capacity() + 1;
  }

  EventMessage msg;
  while (next_seq <= head && batch.size() < max_batch) {
    if (part_ptr->read_at(next_seq, msg)) {
      batch.push_back(msg);
      cursor.last_consumed_seq = next_seq;
      next_seq++;
    } else {
      break;
    }
  }

  return batch;
}

size_t EventFabric::total_topics() const {
  std::lock_guard<aios::hal::Mutex> lock(mutex_);
  return topic_ids_.size();
}

} // namespace aios::sidecar
