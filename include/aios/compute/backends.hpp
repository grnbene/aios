#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <memory>
#include "aios/core/result.hpp"
#include "aios/compute/device.hpp"
#include "aios/hal/sync.hpp"

namespace aios::compute {

// Vulkan compute, on the GPU. Like the CUDA backend, the loader is bound at
// runtime and the device state lives behind a pointer, so this compiles on a
// machine with no Vulkan headers and runs on one with no Vulkan driver.
class VulkanComputeBackend final : public Device {
 public:
  VulkanComputeBackend(const DeviceID& id, int ordinal = 0);
  ~VulkanComputeBackend() override;
  VulkanComputeBackend(const VulkanComputeBackend&) = delete;
  VulkanComputeBackend& operator=(const VulkanComputeBackend&) = delete;

  // Empty when this machine really is dispatching SPIR-V; otherwise why not.
  const std::string& unavailable() const { return unavailable_; }
  bool on_gpu() const { return unavailable_.empty(); }

  struct Gpu;
  std::shared_ptr<Gpu> gpu() const { return gpu_; }

  const DeviceID& id() const override { return id_; }
  DeviceType type() const override { return DeviceType::GPU; }
  const std::string& name() const override { return name_; }
  const DeviceCapacity& capacity() const override { return cap_; }
  bool supports(const Operation& op) const override;
  Result<void> execute(const Operation& op, MemoryFabric& fabric, uint32_t lanes) override;
  Result<void> register_kernel(const std::string& name, CustomKernel k) override;
  uint32_t busy_lanes() const override;
  Result<DeviceMemoryStats> memory_stats() const override;

 private:
  DeviceID id_;
  std::string name_ = "Vulkan (no device)";
  std::string unavailable_;
  DeviceCapacity cap_;
  std::shared_ptr<Gpu> gpu_;
  mutable hal::Mutex mu_;
  std::unordered_map<uint64_t, CustomKernel> kernels_;
  mutable std::atomic<uint32_t> busy_lanes_{0};
};

// Registers all Vulkan compute devices found on this machine.
AcceleratorReport register_vulkan_devices(DeviceFabric& devices, MemoryFabric& memory, hal::Host& host, IdGenerator& ids);

// CUDA, on the card. The device-side state lives behind a pointer so nothing
// above the compute layer has to see a CUDA type, and so a build on a machine
// with no NVIDIA driver anywhere still compiles: the driver API is bound at
// runtime, not linked.
class CudaComputeBackend final : public Device {
 public:
  CudaComputeBackend(const DeviceID& id, int ordinal = 0);
  ~CudaComputeBackend() override;
  CudaComputeBackend(const CudaComputeBackend&) = delete;
  CudaComputeBackend& operator=(const CudaComputeBackend&) = delete;

  // Empty when this machine really is running CUDA; otherwise why it is not.
  // A caller that needs the GPU must ask, because execute() will refuse.
  const std::string& unavailable() const { return unavailable_; }
  bool on_gpu() const { return unavailable_.empty(); }

  struct Gpu;
  std::shared_ptr<Gpu> gpu() const { return gpu_; }

  const DeviceID& id() const override { return id_; }
  DeviceType type() const override { return DeviceType::GPU; }
  const std::string& name() const override { return name_; }
  const DeviceCapacity& capacity() const override { return cap_; }
  bool supports(const Operation& op) const override;
  Result<void> execute(const Operation& op, MemoryFabric& fabric, uint32_t lanes) override;
  Result<void> register_kernel(const std::string& name, CustomKernel k) override;
  uint32_t busy_lanes() const override;
  Result<DeviceMemoryStats> memory_stats() const override;

 private:
  DeviceID id_;
  std::string name_ = "CUDA (no device)";
  std::string unavailable_;
  DeviceCapacity cap_;
  std::shared_ptr<Gpu> gpu_;
  mutable hal::Mutex mu_;
  std::unordered_map<uint64_t, CustomKernel> kernels_;
  mutable std::atomic<uint32_t> busy_lanes_{0};
};

// Registers every CUDA device this machine has with the fabric.
AcceleratorReport register_cuda_devices(DeviceFabric& devices, MemoryFabric& memory, hal::Host& host, IdGenerator& ids);

// Apple Metal compute backend (macOS / Apple Silicon / Mac GPUs).
class MetalComputeBackend final : public Device {
 public:
  MetalComputeBackend(const DeviceID& id, int ordinal = 0);
  ~MetalComputeBackend() override;
  MetalComputeBackend(const MetalComputeBackend&) = delete;
  MetalComputeBackend& operator=(const MetalComputeBackend&) = delete;

  const std::string& unavailable() const { return unavailable_; }
  bool on_gpu() const { return unavailable_.empty(); }

  struct Gpu;
  std::shared_ptr<Gpu> gpu() const { return gpu_; }

  const DeviceID& id() const override { return id_; }
  DeviceType type() const override { return DeviceType::GPU; }
  const std::string& name() const override { return name_; }
  const DeviceCapacity& capacity() const override { return cap_; }
  bool supports(const Operation& op) const override;
  Result<void> execute(const Operation& op, MemoryFabric& fabric, uint32_t lanes) override;
  Result<void> register_kernel(const std::string& name, CustomKernel k) override;
  uint32_t busy_lanes() const override;
  Result<DeviceMemoryStats> memory_stats() const override;

 private:
  DeviceID id_;
  std::string name_ = "Metal (no device)";
  std::string unavailable_;
  DeviceCapacity cap_;
  std::shared_ptr<Gpu> gpu_;
  mutable hal::Mutex mu_;
  std::unordered_map<uint64_t, CustomKernel> kernels_;
  mutable std::atomic<uint32_t> busy_lanes_{0};
};

// Registers Apple Metal devices on Darwin hosts.
AcceleratorReport register_metal_devices(DeviceFabric& devices, MemoryFabric& memory, hal::Host& host, IdGenerator& ids);

}  // namespace aios::compute
