#include "aios/sidecar/binary_telemetry.hpp"
#include <chrono>
#include <algorithm>
#include <cstring>
#include <numeric>

namespace aios::sidecar {

BinaryTelemetryCore::BinaryTelemetryCore(size_t max_events)
    : capacity_(max_events) {
  ring_buffer_ = std::make_unique<TelemetryEvent[]>(capacity_);
}

void BinaryTelemetryCore::record(uint64_t trace_id, uint64_t span_id, uint32_t service_id,
                                uint16_t event_type, uint16_t status_code, uint32_t duration_us,
                                uint32_t payload_int, const char* tag) {
  uint64_t idx = write_index_.fetch_add(1, std::memory_order_relaxed);
  size_t slot = idx % capacity_;

  uint64_t now_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());

  TelemetryEvent& ev = ring_buffer_[slot];
  ev.timestamp_ns = now_ns;
  ev.trace_id = trace_id;
  ev.span_id = span_id;
  ev.service_id = service_id;
  ev.event_type = event_type;
  ev.status_code = status_code;
  ev.duration_us = duration_us;
  ev.payload_int = payload_int;
  if (tag) {
    std::strncpy(ev.tag, tag, sizeof(ev.tag) - 1);
    ev.tag[sizeof(ev.tag) - 1] = '\0';
  } else {
    ev.tag[0] = '\0';
  }
}

std::vector<TelemetryEvent> BinaryTelemetryCore::query_trace(uint64_t trace_id) const {
  std::vector<TelemetryEvent> results;
  uint64_t total = write_index_.load(std::memory_order_relaxed);
  size_t scan_count = std::min<size_t>(total, capacity_);
  uint64_t start_idx = (total > capacity_) ? (total - capacity_) : 0;

  for (size_t i = 0; i < scan_count; ++i) {
    size_t slot = (start_idx + i) % capacity_;
    const auto& ev = ring_buffer_[slot];
    if (ev.trace_id == trace_id && ev.timestamp_ns != 0) {
      results.push_back(ev);
    }
  }
  return results;
}

std::vector<TelemetryEvent> BinaryTelemetryCore::query_window(uint64_t start_time_ns, uint64_t end_time_ns,
                                                             std::optional<uint32_t> service_id,
                                                             std::optional<uint16_t> event_type) const {
  std::vector<TelemetryEvent> results;
  uint64_t total = write_index_.load(std::memory_order_relaxed);
  size_t scan_count = std::min<size_t>(total, capacity_);
  uint64_t start_idx = (total > capacity_) ? (total - capacity_) : 0;

  for (size_t i = 0; i < scan_count; ++i) {
    size_t slot = (start_idx + i) % capacity_;
    const auto& ev = ring_buffer_[slot];
    if (ev.timestamp_ns >= start_time_ns && ev.timestamp_ns <= end_time_ns) {
      if (service_id && ev.service_id != *service_id) continue;
      if (event_type && ev.event_type != *event_type) continue;
      results.push_back(ev);
    }
  }
  return results;
}

TelemetryStats BinaryTelemetryCore::compute_stats(uint32_t service_id, uint64_t start_time_ns) const {
  TelemetryStats stats;
  std::vector<uint32_t> durations;

  uint64_t total = write_index_.load(std::memory_order_relaxed);
  size_t scan_count = std::min<size_t>(total, capacity_);
  uint64_t start_idx = (total > capacity_) ? (total - capacity_) : 0;

  uint64_t sum_duration = 0;
  for (size_t i = 0; i < scan_count; ++i) {
    size_t slot = (start_idx + i) % capacity_;
    const auto& ev = ring_buffer_[slot];
    if (ev.service_id == service_id && ev.timestamp_ns >= start_time_ns && ev.timestamp_ns != 0) {
      stats.count++;
      if (ev.status_code >= 400) {
        stats.error_count++;
      }
      durations.push_back(ev.duration_us);
      sum_duration += ev.duration_us;
    }
  }

  if (durations.empty()) return stats;

  std::sort(durations.begin(), durations.end());
  stats.min_duration_us = durations.front();
  stats.max_duration_us = durations.back();
  stats.avg_duration_us = static_cast<double>(sum_duration) / durations.size();

  stats.p50_duration_us = durations[durations.size() * 50 / 100];
  stats.p95_duration_us = durations[durations.size() * 95 / 100];
  stats.p99_duration_us = durations[durations.size() * 99 / 100];

  return stats;
}

} // namespace aios::sidecar
