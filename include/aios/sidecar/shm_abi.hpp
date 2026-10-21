#pragma once

#include "aios/hal/host.hpp"
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <atomic>
#include <memory>
#include <cstring>
#include "aios/core/ids.hpp"

namespace aios {
class RemoteNode;
}

namespace aios::sidecar {

// 64-byte aligned header for lockless/atomic ring buffer
struct alignas(64) ShmRingHeader {
  std::atomic<uint64_t> write_pos{0};
  std::atomic<uint64_t> read_pos{0};
  uint64_t capacity{0};
  uint32_t magic{0x41494F53}; // 'AIOS'
  uint32_t version{1};
  std::atomic<uint64_t> dropped_count{0};
  std::atomic<uint64_t> message_count{0};
  uint8_t padding[16]{0};
};

struct alignas(16) ShmFrameHeader {
  uint32_t magic{0x53484D46}; // 'SHMF'
  uint32_t payload_len{0};
  uint16_t msg_type{0};
  uint16_t schema_version{1};
  uint32_t stream_id{0};
  uint64_t timestamp_ns{0};
  uint64_t sequence{0};
};

class ShmRingChannel {
public:
  explicit ShmRingChannel(std::string name, size_t capacity_bytes, bool create = true);
  ~ShmRingChannel();

  ShmRingChannel(const ShmRingChannel&) = delete;
  ShmRingChannel& operator=(const ShmRingChannel&) = delete;

  // Zero-copy binary push
  bool push_frame(uint16_t msg_type, uint32_t stream_id, const void* payload, size_t len);

  // Zero-copy pop callback (invokes reader callback directly with mapped memory slice)
  template <typename F>
  bool pop_frame(F&& handler) {
    if (!mapped_base_ || !header_) return false;
    uint64_t r = header_->read_pos.load(std::memory_order_relaxed);
    uint64_t w = header_->write_pos.load(std::memory_order_acquire);
    if (r >= w) return false; // empty

    size_t cap = header_->capacity;
    size_t ring_offset = r % cap;
    uint8_t* frame_ptr = buffer_base_ + ring_offset;

    ShmFrameHeader frame_hdr;
    if (ring_offset + sizeof(ShmFrameHeader) <= cap) {
      std::memcpy(&frame_hdr, frame_ptr, sizeof(ShmFrameHeader));
    } else {
      // wrapped header
      size_t first_part = cap - ring_offset;
      std::memcpy(&frame_hdr, frame_ptr, first_part);
      std::memcpy(reinterpret_cast<uint8_t*>(&frame_hdr) + first_part, buffer_base_, sizeof(ShmFrameHeader) - first_part);
    }

    if (frame_hdr.magic != 0x53484D46) {
      // Corrupt or out of sync
      header_->read_pos.store(w, std::memory_order_release);
      return false;
    }

    size_t total_frame_len = sizeof(ShmFrameHeader) + frame_hdr.payload_len;
    size_t payload_offset = (ring_offset + sizeof(ShmFrameHeader)) % cap;

    std::vector<uint8_t> contiguous_scratch;
    const void* payload_ptr = nullptr;

    if (payload_offset + frame_hdr.payload_len <= cap) {
      payload_ptr = buffer_base_ + payload_offset;
    } else {
      // Wrap-around payload: copy into contiguous scratch buffer
      contiguous_scratch.resize(frame_hdr.payload_len);
      size_t part1 = cap - payload_offset;
      std::memcpy(contiguous_scratch.data(), buffer_base_ + payload_offset, part1);
      std::memcpy(contiguous_scratch.data() + part1, buffer_base_, frame_hdr.payload_len - part1);
      payload_ptr = contiguous_scratch.data();
    }

    handler(frame_hdr, payload_ptr, frame_hdr.payload_len);

    header_->read_pos.fetch_add(total_frame_len, std::memory_order_release);
    return true;
  }

  uint64_t messages_available() const;
  uint64_t dropped_count() const;
  size_t capacity() const;
  const std::string& name() const { return name_; }

  // Over-the-wire sidecar bridge: drain frames and ship as differential StateDeltas / MemPatches
  size_t bridge_to_remote(RemoteNode& remote, const StateID& state_target, size_t max_batch = 100);

private:
  std::string name_;
  size_t total_size_{0};
  hal::Mapping map_;
  uint8_t* mapped_base_{nullptr};
  ShmRingHeader* header_{nullptr};
  uint8_t* buffer_base_{nullptr};
  bool is_creator_{false};
  bool is_anonymous_{false};
};

} // namespace aios::sidecar
