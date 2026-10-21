// NVML, bound at runtime. libnvidia-ml ships with the driver on every box that
// has one, and is what nvidia-smi itself reads, so what this reports is what an
// operator sees in nvidia-smi: including memory held by OTHER processes, which
// cuMemGetInfo only reports as "not free".
#include "aios/compute/gpu_telemetry.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/sync.hpp"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>

namespace aios::compute {
namespace {

struct nvmlDevice_st; using nvmlDevice_t = nvmlDevice_st*;
using nvmlReturn_t = int;
constexpr nvmlReturn_t NVML_SUCCESS_ = 0;
constexpr nvmlReturn_t NVML_ERROR_INSUFFICIENT_SIZE_ = 7;

struct nvmlMemory_t { unsigned long long total, free, used; };
struct nvmlProcessInfo_v1_t { unsigned int pid; unsigned long long usedGpuMemory; };
// v2/v3 add gpuInstanceId/computeInstanceId after the two fields above; the
// driver fills whichever layout the symbol name asks for. We ask for v1 by
// name and fall back to the unversioned symbol, whose layout on drivers where
// it exists is the same two fields.

struct Nvml {
  void* lib = nullptr;
  bool loaded = false, tried = false;
  std::string why;
  nvmlReturn_t (*Init)() = nullptr;
  nvmlReturn_t (*DeviceGetHandleByPciBusId)(const char*, nvmlDevice_t*) = nullptr;
  nvmlReturn_t (*DeviceGetMemoryInfo)(nvmlDevice_t, nvmlMemory_t*) = nullptr;
  nvmlReturn_t (*DeviceGetComputeRunningProcesses)(nvmlDevice_t, unsigned int*, nvmlProcessInfo_v1_t*) = nullptr;
  nvmlReturn_t (*DeviceGetGraphicsRunningProcesses)(nvmlDevice_t, unsigned int*, nvmlProcessInfo_v1_t*) = nullptr;
  hal::Mutex mu;

  template <typename Fn> bool bind(Fn& slot, const char* name) {
    slot = reinterpret_cast<Fn>(hal::library_symbol(lib, name));
    return slot != nullptr;
  }
  bool load() {
    if (tried) return loaded;
    tried = true;
    for (const char* n : {"libnvidia-ml.so.1", "libnvidia-ml.so", "nvml.dll"}) {
      lib = hal::load_library(n);
      if (lib) break;
    }
    if (!lib) { why = "no NVML (libnvidia-ml) on this host"; return false; }
    bool ok = true;
    ok &= bind(Init, "nvmlInit_v2") || bind(Init, "nvmlInit");
    ok &= bind(DeviceGetHandleByPciBusId, "nvmlDeviceGetHandleByPciBusId_v2") || bind(DeviceGetHandleByPciBusId, "nvmlDeviceGetHandleByPciBusId");
    ok &= bind(DeviceGetMemoryInfo, "nvmlDeviceGetMemoryInfo");
    if (!bind(DeviceGetComputeRunningProcesses, "nvmlDeviceGetComputeRunningProcesses")) DeviceGetComputeRunningProcesses = nullptr;
    if (!bind(DeviceGetGraphicsRunningProcesses, "nvmlDeviceGetGraphicsRunningProcesses")) DeviceGetGraphicsRunningProcesses = nullptr;
    if (!ok) { why = "NVML is present but missing symbols"; return false; }
    if (Init() != NVML_SUCCESS_) { why = "nvmlInit failed"; return false; }
    loaded = true;
    return true;
  }
};

Nvml& nvml() { static Nvml n; return n; }

std::string comm_of(uint32_t pid) {
#if defined(AIOS_METAL)
  // No /proc on metal, and no libc fopen to link; a tenant pid has no name here.
  (void)pid;
  return {};
#else
  auto b = hal::process_host().read_file("/proc/" + std::to_string(pid) + "/comm");
  if (!b.ok()) return {};
  std::string name(b->begin(), b->end());
  while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
  return name;
#endif
}

Result<nvmlDevice_t> handle_for(const std::string& pci) {
  auto& n = nvml();
  hal::LockGuard g(n.mu);
  if (!n.load()) return Result<nvmlDevice_t>(Status::Unsupported, n.why);
  nvmlDevice_t dev = nullptr;
  std::string id = normalize_pci_bus_id(pci);
  if (n.DeviceGetHandleByPciBusId(id.c_str(), &dev) != NVML_SUCCESS_) {
    return Result<nvmlDevice_t>(Status::NotFound, "NVML knows no device at " + id);
  }
  return dev;
}

void collect(nvmlReturn_t (*fn)(nvmlDevice_t, unsigned int*, nvmlProcessInfo_v1_t*), nvmlDevice_t dev, std::vector<DeviceTenant>& out) {
  if (!fn) return;
  unsigned int count = 0;
  nvmlReturn_t r = fn(dev, &count, nullptr);
  if (r != NVML_SUCCESS_ && r != NVML_ERROR_INSUFFICIENT_SIZE_) return;
  if (count == 0) return;
  // Over-allocate: the versioned structs are larger than v1 and a driver that
  // fills the v3 layout for the unversioned name must not run off the end.
  std::vector<nvmlProcessInfo_v1_t> infos(count * 4 + 8);
  unsigned int cap = count + 4;
  if (fn(dev, &cap, infos.data()) != NVML_SUCCESS_) return;
  for (unsigned int i = 0; i < cap && i < infos.size(); ++i) {
    if (infos[i].pid == 0) continue;
    bool dup = false;
    for (auto& t : out) if (t.pid == infos[i].pid) { dup = true; break; }
    if (dup) continue;
    DeviceTenant t;
    t.pid = infos[i].pid;
    // NVML reports ~0 when the driver cannot attribute memory per process.
    t.bytes = infos[i].usedGpuMemory == ~0ull ? 0 : infos[i].usedGpuMemory;
    t.name = comm_of(t.pid);
    out.push_back(std::move(t));
  }
}

}  // namespace

std::string normalize_pci_bus_id(const std::string& raw) {
  std::string s;
  for (char c : raw) if (!std::isspace((unsigned char)c)) s += (char)std::tolower((unsigned char)c);
  // domain:bus:device.function; domain may be missing or 8 wide.
  size_t first = s.find(':');
  size_t second = s.find(':', first == std::string::npos ? 0 : first + 1);
  if (first == std::string::npos) return s;
  if (second == std::string::npos) return "0000:" + s;          // "01:00.0"
  std::string domain = s.substr(0, first);
  while (domain.size() > 4 && domain[0] == '0') domain.erase(0, 1);
  while (domain.size() < 4) domain = "0" + domain;
  return domain + s.substr(first);
}

Result<DeviceMemoryStats> nvidia_memory_by_pci(const std::string& pci_bus_id) {
  auto h = handle_for(pci_bus_id);
  if (!h.ok()) return h.error();
  auto& n = nvml();
  hal::LockGuard g(n.mu);
  nvmlMemory_t m{};
  if (n.DeviceGetMemoryInfo(*h, &m) != NVML_SUCCESS_) return Result<DeviceMemoryStats>(Status::IoError, "nvmlDeviceGetMemoryInfo failed");
  DeviceMemoryStats s;
  s.total_bytes = m.total; s.free_bytes = m.free; s.used_bytes = m.used;
  s.source = "nvml";
  collect(n.DeviceGetComputeRunningProcesses, *h, s.tenants);
  collect(n.DeviceGetGraphicsRunningProcesses, *h, s.tenants);
  std::sort(s.tenants.begin(), s.tenants.end(), [](const DeviceTenant& a, const DeviceTenant& b) { return a.bytes > b.bytes; });
  return s;
}

std::vector<DeviceTenant> nvidia_tenants_by_pci(const std::string& pci_bus_id) {
  auto s = nvidia_memory_by_pci(pci_bus_id);
  if (!s.ok()) return {};
  return s->tenants;
}

}  // namespace aios::compute
