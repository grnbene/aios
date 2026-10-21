#include "aios/sidecar/shm_abi.hpp"
#include "aios/node/node.hpp"
#include "aios/hal/host.hpp"
#include <cstring>
#include <chrono>

namespace aios::sidecar {

ShmRingChannel::ShmRingChannel(std::string name, size_t capacity_bytes, bool create)
    : name_(std::move(name)), is_creator_(create) {
  // Round capacity up to 64-byte alignment
  size_t aligned_cap = (capacity_bytes + 63) & ~size_t(63);
  total_size_ = sizeof(ShmRingHeader) + aligned_cap;

  auto& host = hal::process_host();
  is_anonymous_ = name_.empty() || name_ == "anon" || name_ == "anonymous";
  auto m = is_anonymous_ ? host.map(total_size_, hal::MappingKind::Shared) : host.map_named(name_, total_size_, create);
  if (!m.ok() && !is_anonymous_) {          // no such name to open: a private ring of its own
    is_anonymous_ = true;
    m = host.map(total_size_, hal::MappingKind::Shared);
  }
  if (!m.ok()) return;
  map_ = *m;
  mapped_base_ = static_cast<uint8_t*>(map_.base);

  header_ = reinterpret_cast<ShmRingHeader*>(mapped_base_);
  buffer_base_ = mapped_base_ + sizeof(ShmRingHeader);

  if (is_creator_ || is_anonymous_) {
    header_->write_pos.store(0, std::memory_order_relaxed);
    header_->read_pos.store(0, std::memory_order_relaxed);
    header_->capacity = aligned_cap;
    header_->magic = 0x41494F53;
    header_->version = 1;
    header_->dropped_count.store(0, std::memory_order_relaxed);
    header_->message_count.store(0, std::memory_order_relaxed);
  }
}

ShmRingChannel::~ShmRingChannel() {
  if (!mapped_base_) return;
  hal::process_host().unmap(map_);
  if (is_creator_ && !is_anonymous_) hal::process_host().unlink_named(name_);
}

bool ShmRingChannel::push_frame(uint16_t msg_type, uint32_t stream_id, const void* payload, size_t len) {
  if (!mapped_base_ || !header_) return false;

  size_t total_frame_len = sizeof(ShmFrameHeader) + len;
  size_t cap = header_->capacity;
  if (total_frame_len > cap) {
    header_->dropped_count.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  uint64_t current_w = header_->write_pos.load(std::memory_order_relaxed);
  uint64_t current_r = header_->read_pos.load(std::memory_order_acquire);

  // Check if buffer has enough space without overwriting unread frames
  if (current_w + total_frame_len - current_r > cap) {
    header_->dropped_count.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  uint64_t seq = header_->message_count.fetch_add(1, std::memory_order_relaxed) + 1;
  uint64_t now_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());

  ShmFrameHeader frame_hdr;
  frame_hdr.magic = 0x53484D46;
  frame_hdr.payload_len = static_cast<uint32_t>(len);
  frame_hdr.msg_type = msg_type;
  frame_hdr.schema_version = 1;
  frame_hdr.stream_id = stream_id;
  frame_hdr.timestamp_ns = now_ns;
  frame_hdr.sequence = seq;

  size_t ring_offset = current_w % cap;

  // Write frame header
  if (ring_offset + sizeof(ShmFrameHeader) <= cap) {
    std::memcpy(buffer_base_ + ring_offset, &frame_hdr, sizeof(ShmFrameHeader));
  } else {
    size_t part1 = cap - ring_offset;
    std::memcpy(buffer_base_ + ring_offset, &frame_hdr, part1);
    std::memcpy(buffer_base_, reinterpret_cast<const uint8_t*>(&frame_hdr) + part1, sizeof(ShmFrameHeader) - part1);
  }

  // Write payload
  size_t payload_offset = (ring_offset + sizeof(ShmFrameHeader)) % cap;
  if (len > 0 && payload != nullptr) {
    if (payload_offset + len <= cap) {
      std::memcpy(buffer_base_ + payload_offset, payload, len);
    } else {
      size_t part1 = cap - payload_offset;
      std::memcpy(buffer_base_ + payload_offset, payload, part1);
      std::memcpy(buffer_base_, static_cast<const uint8_t*>(payload) + part1, len - part1);
    }
  }

  // Publish new write position
  header_->write_pos.store(current_w + total_frame_len, std::memory_order_release);
  return true;
}

uint64_t ShmRingChannel::messages_available() const {
  if (!header_) return 0;
  uint64_t r = header_->read_pos.load(std::memory_order_relaxed);
  uint64_t w = header_->write_pos.load(std::memory_order_acquire);
  return (w > r) ? (w - r) : 0;
}

uint64_t ShmRingChannel::dropped_count() const {
  return header_ ? header_->dropped_count.load(std::memory_order_relaxed) : 0;
}

size_t ShmRingChannel::capacity() const {
  return header_ ? header_->capacity : 0;
}

size_t ShmRingChannel::bridge_to_remote(RemoteNode& remote, const StateID& state_target, size_t max_batch) {
  size_t count = 0;
  while (count < max_batch) {
    bool popped = pop_frame([&](const ShmFrameHeader& hdr, const void* payload, size_t len) {
      ByteSpan span{static_cast<const uint8_t*>(payload), len};
      if (hdr.msg_type == 1) { // StateDelta
        (void)remote.state_delta(state_target, StateKind::Working, hdr.sequence, span, {});
      } else if (hdr.msg_type == 2) { // MemPatch (stream_id as memory offset)
        MemoryID mem_id{Id128{state_target.hi, state_target.lo}};
        (void)remote.mem_patch(mem_id, static_cast<uint64_t>(hdr.stream_id), span);
      }
    });
    if (!popped) break;
    count++;
  }
  return count;
}

} // namespace aios::sidecar
