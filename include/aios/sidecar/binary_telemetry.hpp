#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <atomic>
#include <memory>
#include <cstring>
#include <optional>

namespace aios::sidecar {

// Exactly 64 bytes - 1 cache line per telemetry event
struct alignas(64) TelemetryEvent {
  uint64_t timestamp_ns{0};
  uint64_t trace_id{0};
  uint64_t span_id{0};
  uint32_t service_id{0};
  uint16_t event_type{0}; // 1 = HTTP, 2 = DB, 3 = RPC, 4 = QUEUE, 5 = ERROR
  uint16_t status_code{0}; // 200, 404, 500 etc.
  uint32_t duration_us{0};
  uint32_t payload_int{0};
  char tag[24]{0};
};

static_assert(sizeof(TelemetryEvent) == 64, "TelemetryEvent must be exactly 64 bytes");

struct TelemetryStats {
  uint64_t count{0};
  uint64_t error_count{0};
  uint32_t min_duration_us{0};
  uint32_t max_duration_us{0};
  uint32_t p50_duration_us{0};
  uint32_t p95_duration_us{0};
  uint32_t p99_duration_us{0};
  double avg_duration_us{0.0};
};

class BinaryTelemetryCore {
public:
  explicit BinaryTelemetryCore(size_t max_events = 262144); // Default 256k events (~16MB RAM)
  ~BinaryTelemetryCore() = default;

  // Record a fast telemetry event (atomic lockless slot allocation)
  void record(uint64_t trace_id, uint64_t span_id, uint32_t service_id,
              uint16_t event_type, uint16_t status_code, uint32_t duration_us,
              uint32_t payload_int, const char* tag = nullptr);

  // Instant trace query by trace_id across rolling buffer in RAM
  std::vector<TelemetryEvent> query_trace(uint64_t trace_id) const;

  // Instant time-window query with filtering
  std::vector<TelemetryEvent> query_window(uint64_t start_time_ns, uint64_t end_time_ns,
                                          std::optional<uint32_t> service_id = std::nullopt,
                                          std::optional<uint16_t> event_type = std::nullopt) const;

  // Compute stats across a service's recent events in microseconds
  TelemetryStats compute_stats(uint32_t service_id, uint64_t start_time_ns = 0) const;

  size_t capacity() const { return capacity_; }
  uint64_t total_recorded() const { return write_index_.load(std::memory_order_relaxed); }

private:
  size_t capacity_{0};
  std::unique_ptr<TelemetryEvent[]> ring_buffer_;
  std::atomic<uint64_t> write_index_{0};
};

} // namespace aios::sidecar
