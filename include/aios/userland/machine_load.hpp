#pragma once
// What the machine is actually doing to itself, read from the machine.
//
// Every figure here carries a `known` flag, and NOTHING defaults to a number.
// That is the whole point of this header. The sysinfo organ reports GPU
// utilisation from /sys/class/drm/*/device/gpu_busy_percent, which is an AMD
// attribute that does not exist on an NVIDIA machine -- so on this host it
// reported a confident, permanent 0.0% while the card sat at 84% and 83 C.
// A surface that cannot read a thing must say so; it must never print a zero
// it did not measure.
#include <cstdint>
#include <string>

namespace aios {
namespace hal { class Host; }

namespace userland {

// How hard the machine is being pushed, as one legible step. The emblem shows
// this and nothing else: an orbit cannot encode a percentage, but it can say
// "go and look" from across a room.
enum class LoadState { Calm, Strained, Alarmed };

struct MachineLoad {
  // Busy fraction across all cores, from /proc/stat deltas between samples.
  bool  cpu_known = false;    float cpu_pct = 0.0f;
  // From NVML when the card answers. No NVML, no reading -- not a zero.
  bool  gpu_known = false;    float gpu_pct = 0.0f;
  bool  gpu_temp_known = false;  int gpu_temp_c = 0;
  // The volume the kernel's root actually lives on, by statvfs.
  bool  disk_known = false;   float disk_pct = 0.0f;
  bool  mem_known = false;    uint64_t mem_used = 0, mem_total = 0;

  float mem_pct() const {
    return (mem_known && mem_total) ? (float)((double)mem_used / (double)mem_total * 100.0) : 0.0f;
  }
  // The worst thing true of the machine right now. An unknown axis cannot
  // raise the state -- silence is not an alarm.
  LoadState state() const;
  // Which axis set that state, for the readout to point at. Empty when calm.
  std::string worst() const;
};

// Samples the machine on a cadence. Call tick() as often as you like; it does
// the work at most once per interval, because reading /proc/stat and asking a
// GPU driver are syscalls, not frame work.
class MachineLoadSampler {
 public:
  // `now_ms` is the caller's monotonic clock. Returns true when a fresh sample
  // was taken (so a caller can mark its surface dirty only when it changed).
  bool tick(hal::Host& host, uint64_t now_ms, const std::string& root_path);
  const MachineLoad& latest() const { return cur_; }

 private:
  MachineLoad cur_;
  uint64_t last_ms_ = 0;
  // /proc/stat is cumulative, so a percentage needs the previous reading.
  uint64_t prev_busy_ = 0, prev_total_ = 0;
  bool have_prev_ = false;
  static constexpr uint64_t kIntervalMs = 1000;
};

}  // namespace userland
}  // namespace aios
