#include "aios/userland/machine_load.hpp"
#include "aios/hal/host.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>


namespace aios {
namespace userland {

namespace {

// Where each axis stops being comfortable and starts being worth saying. These
// are the only numbers in this file that were chosen rather than measured, and
// they are here, named, in one place, rather than sprinkled through a painter.
constexpr float kCpuStrained = 70.0f,  kCpuAlarmed = 92.0f;
constexpr float kGpuStrained = 80.0f,  kGpuAlarmed = 95.0f;
constexpr int   kGpuTempStrained = 80, kGpuTempAlarmed = 88;   // degrees C
constexpr float kDiskStrained = 85.0f, kDiskAlarmed = 95.0f;
constexpr float kMemStrained = 85.0f,  kMemAlarmed = 95.0f;

// --- NVML, opened at run time -----------------------------------------------
//
// dlopen'd rather than linked: this tree carries no third-party headers and no
// third-party link dependencies, and a machine with no NVIDIA driver must build
// and run identically. When the library is absent every GPU field stays
// UNKNOWN, which is what the readout prints.
#if !defined(AIOS_METAL)
struct Nvml {
  void* lib = nullptr;
  void* dev = nullptr;
  bool  ready = false;
  bool  tried = false;

  // The four entry points needed, declared here so no NVIDIA header is
  // included. nvmlDevice_t is an opaque pointer; nvmlReturn_t is an int enum
  // whose only value this cares about is 0 == success.
  int (*init)(void) = nullptr;
  int (*handle_by_index)(unsigned int, void**) = nullptr;
  int (*utilization)(void*, unsigned int*) = nullptr;    // {gpu, memory}
  int (*temperature)(void*, int, unsigned int*) = nullptr;

  void open_once() {
    if (tried) return;
    tried = true;
    // The versioned soname first: libnvidia-ml.so without a suffix ships in
    // the -dev package and is absent on a machine that only has the driver.
    // nvml.dll is the Windows driver's own name for the same library (hal's
    // .so->.dll translation would otherwise guess "nvidia-ml.dll", which is
    // not what NVIDIA calls it there), tried through hal:: like the others so
    // this stays the one file in the tree with no dlopen/LoadLibrary of its own.
    for (const char* name : {"libnvidia-ml.so.1", "libnvidia-ml.so", "nvml.dll"}) {
      lib = hal::load_library(name);
      if (lib) break;
    }
    if (!lib) return;
    init            = (int(*)(void))                     hal::library_symbol(lib, "nvmlInit_v2");
    handle_by_index = (int(*)(unsigned int, void**))      hal::library_symbol(lib, "nvmlDeviceGetHandleByIndex_v2");
    utilization     = (int(*)(void*, unsigned int*))      hal::library_symbol(lib, "nvmlDeviceGetUtilizationRates");
    temperature     = (int(*)(void*, int, unsigned int*)) hal::library_symbol(lib, "nvmlDeviceGetTemperature");
    if (!init || !handle_by_index || !utilization) return;
    if (init() != 0) return;
    if (handle_by_index(0, &dev) != 0 || !dev) return;
    ready = true;
  }
};
Nvml& nvml() { static Nvml n; return n; }
#endif

// Busy and total jiffies across all cores, cumulative since boot.
bool read_proc_stat(uint64_t* busy, uint64_t* total) {
#if defined(AIOS_METAL)
  (void)busy; (void)total; return false;
#else
  std::FILE* f = std::fopen("/proc/stat", "r");
  if (!f) return false;
  char line[512];
  bool ok = false;
  if (std::fgets(line, sizeof(line), f) && std::strncmp(line, "cpu ", 4) == 0) {
    unsigned long long v[10] = {0};
    const int n = std::sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                              &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    if (n >= 4) {
      uint64_t t = 0;
      for (int i = 0; i < n; ++i) t += v[i];
      // idle + iowait is the part that was not work.
      const uint64_t idle = v[3] + (n > 4 ? v[4] : 0);
      *total = t;
      *busy = t - idle;
      ok = true;
    }
  }
  std::fclose(f);
  return ok;
#endif
}

int step(float v, float strained, float alarmed) {
  if (v >= alarmed) return 2;
  if (v >= strained) return 1;
  return 0;
}

}  // namespace

LoadState MachineLoad::state() const {
  int worst = 0;
  if (cpu_known)  worst = std::max(worst, step(cpu_pct, kCpuStrained, kCpuAlarmed));
  if (gpu_known)  worst = std::max(worst, step(gpu_pct, kGpuStrained, kGpuAlarmed));
  if (gpu_temp_known)
    worst = std::max(worst, step((float)gpu_temp_c, (float)kGpuTempStrained, (float)kGpuTempAlarmed));
  if (disk_known) worst = std::max(worst, step(disk_pct, kDiskStrained, kDiskAlarmed));
  if (mem_known)  worst = std::max(worst, step(mem_pct(), kMemStrained, kMemAlarmed));
  return worst >= 2 ? LoadState::Alarmed : (worst >= 1 ? LoadState::Strained : LoadState::Calm);
}

std::string MachineLoad::worst() const {
  int best = 0;
  std::string who;
  auto consider = [&](bool known, float v, float s, float a, const char* name) {
    if (!known) return;
    const int lvl = step(v, s, a);
    if (lvl > best) { best = lvl; who = name; }
  };
  consider(cpu_known, cpu_pct, kCpuStrained, kCpuAlarmed, "CPU");
  consider(gpu_known, gpu_pct, kGpuStrained, kGpuAlarmed, "GPU");
  consider(gpu_temp_known, (float)gpu_temp_c, (float)kGpuTempStrained, (float)kGpuTempAlarmed, "GPU TEMP");
  consider(disk_known, disk_pct, kDiskStrained, kDiskAlarmed, "DISK");
  consider(mem_known, mem_pct(), kMemStrained, kMemAlarmed, "MEMORY");
  return who;
}

bool MachineLoadSampler::tick(hal::Host& host, uint64_t now_ms, const std::string& root_path) {
  if (last_ms_ != 0 && now_ms - last_ms_ < kIntervalMs) return false;
  last_ms_ = now_ms;

  // --- CPU, from the delta between this reading and the last ---------------
  uint64_t busy = 0, total = 0;
  if (read_proc_stat(&busy, &total)) {
    if (have_prev_ && total > prev_total_) {
      const double dt = (double)(total - prev_total_);
      const double db = (double)(busy - prev_busy_);
      cur_.cpu_pct = (float)std::clamp(db / dt * 100.0, 0.0, 100.0);
      cur_.cpu_known = true;   // the FIRST sample has no delta and stays unknown
    }
    prev_busy_ = busy; prev_total_ = total; have_prev_ = true;
  }

  // --- memory: available_bytes() reads /proc/meminfo per call. The Topology
  // copy does NOT -- discover() runs once in the host's constructor, so
  // topology().available_memory is a boot snapshot and would report the free
  // memory this machine had when it started.
  const uint64_t tot = host.topology().total_memory;
  if (tot) {
    const uint64_t avail = host.available_bytes();
    cur_.mem_total = tot;
    cur_.mem_used = avail < tot ? tot - avail : 0;
    cur_.mem_known = true;
  }

  // --- the volume the root actually lives on -------------------------------
  auto vols = host.storage(root_path);
  if (vols.ok()) {
    for (const auto& v : *vols) {
      if (!v.holds_root || !v.capacity) continue;
      cur_.disk_pct = (float)((double)v.used / (double)v.capacity * 100.0);
      cur_.disk_known = true;
      break;
    }
  }

  // --- GPU -----------------------------------------------------------------
#if !defined(AIOS_METAL)
  Nvml& n = nvml();
  n.open_once();
  if (n.ready) {
    unsigned int util[2] = {0, 0};
    if (n.utilization(n.dev, util) == 0) {
      cur_.gpu_pct = (float)util[0];
      cur_.gpu_known = true;
    }
    unsigned int temp = 0;
    if (n.temperature && n.temperature(n.dev, 0 /* NVML_TEMPERATURE_GPU */, &temp) == 0) {
      cur_.gpu_temp_c = (int)temp;
      cur_.gpu_temp_known = true;
    }
  }
#endif
  return true;
}

}  // namespace userland
}  // namespace aios
