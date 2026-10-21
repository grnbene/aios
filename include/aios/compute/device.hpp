#pragma once
#include <atomic>
#include <string>
#include <vector>
// A Device executes operations against memory the fabric exposes to it. The
// DeviceFabric is the roster of everything that can compute. CPU today; GPU,
// NPU and remote devices register through the same interface.
#include <memory>
#include "aios/hal/sync.hpp"
#include <functional>
#include <unordered_map>
#include "aios/compute/compute_graph.hpp"
#include "aios/memory/memory_fabric.hpp"

namespace aios {

struct DeviceCapacity {
  uint32_t lanes = 1;            // concurrent execution lanes (cores, SMs, ...)
  uint64_t memory = 0;           // bytes addressable by the device
  uint64_t flops_per_second = 0; // rough, for placement
  MemoryType native_memory = MemoryType::System;
  // The device runs on operands where they are - host memory windows it reads
  // (and uploads from) itself - so moving them first gains nothing. CUDA and
  // Vulkan work this way; OpenCL works on its own provider's buffers. (Only
  // OpenCL provides "Device" memory, so migrating toward a CUDA device's
  // native type moved its operands into OpenCL's buffers, which CUDA never
  // reads, and every CPU operation after it read them back through them.)
  bool reads_host_memory = false;
};

struct ExecutionSample {
  OperationID op;
  DeviceID device;
  uint64_t started_ns = 0;
  uint64_t finished_ns = 0;
  uint32_t lanes_used = 1;
};

// A custom kernel operates on windows the fabric handed to the device.
struct KernelArgs {
  const Operation& op;
  std::vector<MemoryFabric::Window> inputs;
  std::vector<MemoryFabric::Window> outputs;
  uint32_t lanes;
};
using CustomKernel = std::function<Result<void>(KernelArgs&)>;

// Something else that holds memory on a device right now: a pid on this
// host, or a peer's entity. It is what "what is loaded on that GPU" means.
struct DeviceTenant {
  uint32_t pid = 0;
  uint64_t bytes = 0;
  std::string name;
};

struct DeviceMemoryStats {
  uint64_t total_bytes = 0;
  uint64_t free_bytes = 0;
  uint64_t used_bytes = 0;
  // Where the numbers came from ("cuMemGetInfo", "nvml", "vulkan-budget",
  // "/proc/meminfo", "metal"). Empty means the base-class capacity echo below,
  // which is a capacity, not a measurement.
  std::string source;
  std::vector<DeviceTenant> tenants;
};

// The name behind a custom kernel's hash, for what this machine advertises:
// every register_kernel notes its name here (process-wide), so telemetry lists
// the kernels the devices really run instead of a list compiled into it.
void note_custom_kernel_name(const std::string& name);
std::string custom_kernel_name(uint64_t hash);   // "" when no name was noted

class Device {
 public:
  virtual ~Device() = default;
  virtual const DeviceID& id() const = 0;
  virtual DeviceType type() const = 0;
  virtual const std::string& name() const = 0;
  virtual const DeviceCapacity& capacity() const = 0;
  virtual bool supports(const Operation& op) const = 0;
  virtual Result<void> execute(const Operation& op, MemoryFabric& fabric, uint32_t lanes) = 0;
  virtual Result<void> register_kernel(const std::string& name, CustomKernel k) = 0;
  // The named kernels this device runs (fnv1a of each name): what a fleet peer
  // is told, so its remote device for this one accepts exactly those.
  virtual std::vector<uint64_t> custom_kernels() const { return {}; }
  virtual uint32_t busy_lanes() const = 0;
  virtual Result<DeviceMemoryStats> memory_stats() const {
    DeviceMemoryStats s;
    s.total_bytes = capacity().memory;
    s.free_bytes = capacity().memory;
    s.used_bytes = 0;
    return s;
  }
};

namespace hal { class Host; }

std::unique_ptr<Device> make_cpu_device(const DeviceID& id, uint32_t lanes, uint64_t memory, hal::Host* host = nullptr);

// Accelerators reached through OpenCL (GPU, NPU/accelerator, or an OpenCL CPU
// runtime). Loaded at runtime; absent runtime = no devices, no error. The first
// device found also provides MemoryType::Device to the fabric.
struct AcceleratorReport { std::vector<std::string> devices; std::string note; };
AcceleratorReport register_opencl_devices(class DeviceFabric& devices, MemoryFabric& memory, hal::Host& host, IdGenerator& ids);

class DeviceFabric {
 public:
  void add(std::unique_ptr<Device> d);
  // Accelerator registration is deferred: `loader` runs once, on the first
  // query that could be answered by a non-CPU device (all, count, of_type of
  // anything but CPU, get of an id not yet known), or on ensure_loaded().
  // A process that only runs on the CPU never opens a GPU runtime.
  void set_lazy_loader(std::function<void()> loader);
  void ensure_loaded();
  Device* get(const DeviceID& id);
  std::vector<Device*> all();
  // The devices registered so far; never triggers the lazy loader.
  std::vector<Device*> all_loaded();
  std::vector<Device*> of_type(DeviceType t);
  size_t count();
 private:
  mutable hal::Mutex mu_;
  hal::Mutex load_mu_;
  std::function<void()> loader_;
  std::atomic<bool> loaded_{true};
  std::vector<std::unique_ptr<Device>> devices_;
};

}  // namespace aios
