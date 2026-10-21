#pragma once
// What is actually resident on a GPU right now, from the vendor's management
// library rather than from a runtime's own view of its own context. The CUDA,
// OpenCL and Vulkan backends all end up describing the same physical card; the
// PCI bus id is the one name they agree on, so it is the key here.
#include <string>
#include <vector>
#include "aios/compute/device.hpp"

namespace aios::compute {

// Fills used/total/free and the tenant list for the NVIDIA card at `pci_bus_id`
// ("0000:01:00.0" form, case-insensitive). Fails cleanly when there is no NVML
// on this host, or the bus id is not an NVIDIA device it knows.
Result<DeviceMemoryStats> nvidia_memory_by_pci(const std::string& pci_bus_id);

// The processes holding memory on that card (compute + graphics contexts), by
// pid, bytes and the /proc comm name. Empty when NVML cannot say.
std::vector<DeviceTenant> nvidia_tenants_by_pci(const std::string& pci_bus_id);

// Normalises "01:00.0", "0000:01:00.0", "00000000:01:00.0" to "0000:01:00.0".
std::string normalize_pci_bus_id(const std::string& raw);

}  // namespace aios::compute
