#pragma once
// The PCI Express bus, as this machine's substrate reports it.
//
// Base addresses, sizes and topology come off the bus; nothing here is a
// constant. A caller asking whether two functions can reach each other's
// memory gets the substrate's answer, not an assumption — peer-to-peer DMA
// between two endpoints is a property of the fabric between them, and a
// machine that does not know cannot claim it does.
#include <cstdint>
#include <string>
#include <vector>
#include "aios/core/result.hpp"

namespace aios::hal {

struct PciBar {
  uint8_t index = 0;
  uint64_t address = 0;      // where the function answers, as programmed
  uint64_t size = 0;
  bool memory = true;        // false: an I/O port range
  bool prefetchable = false;
  bool wide = false;         // a 64-bit BAR
};

struct PciFunction {
  std::string slot;          // 0000:01:00.0, as the bus names it
  uint16_t vendor = 0;
  uint16_t device = 0;
  uint32_t class_code = 0;
  std::string name;          // what the substrate calls it, when it knows
  std::string driver;        // what has claimed it, empty when nothing has
  std::vector<PciBar> bars;
  int numa_node = -1;
  std::string upstream;      // the bridge or root port above it
};

Result<std::vector<PciFunction>> pci_functions();

struct PciMapping {
  void* base = nullptr;
  uint64_t size = 0;
  std::string slot;
  uint8_t bar = 0;
  int handle = -1;
};

// Maps a function's BAR so the machine can address it directly. Refused where
// the substrate will not allow it, which is most of the time without privilege.
Result<PciMapping> pci_map_bar(const std::string& slot, uint8_t bar, uint64_t bytes = 0);
void pci_unmap_bar(PciMapping& m);

// Whether one function can address the other's memory without the transfer
// passing through system RAM. `why` is filled either way.
Result<bool> pci_peer_reachable(const std::string& a, const std::string& b, std::string& why);

}  // namespace aios::hal
