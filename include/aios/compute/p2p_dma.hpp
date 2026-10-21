#pragma once
// Peer-to-peer transfers between two things on the bus.
//
// Whether one device can write into another's memory without the bytes going
// up through system RAM is a property of the fabric between them. This asks
// the machine; it does not assume. What it replaced returned the constants
// 0xe0000000 and 0xf0000000 as "BAR addresses" and added the byte count to a
// counter, on any machine, having moved nothing.
#include <cstdint>
#include <string>
#include <vector>
#include "aios/core/result.hpp"
#include "aios/hal/pci.hpp"

namespace aios::compute {

struct DMAMapping {
  std::string source_slot;      // the bus addresses of the two endpoints
  std::string target_slot;
  uint64_t source_bar_addr = 0; // where each function actually answers
  uint64_t target_bar_addr = 0;
  uint64_t source_bar_size = 0;
  uint64_t target_bar_size = 0;
  uint64_t bytes = 0;
  bool active = false;          // the fabric will forward between them
  std::string path;             // why, either way
};

struct BusFunction {
  std::string slot;
  uint16_t vendor = 0;
  uint16_t device = 0;
  std::string kind;
  std::string driver;
  std::string upstream;
  std::vector<hal::PciBar> bars;
};

class P2PAcceleratorDMA {
 public:
  P2PAcceleratorDMA();

  // Everything on this machine's bus, with the base addresses it was actually
  // programmed with.
  Result<std::vector<BusFunction>> enumerate() const;

  // Whether these two can reach each other, and where each of them answers.
  // Refused, with the reason, when the fabric will not forward between them.
  Result<DMAMapping> map_p2p_region(const std::string& src_slot, const std::string& dst_slot, uint64_t bytes);

  // Moves the bytes. There is no software path for this: either the substrate
  // provides one for these two endpoints or the transfer does not happen.
  Result<uint64_t> transfer_direct(const DMAMapping& mapping);

  uint64_t bytes_transferred() const { return total_transfers_; }

 private:
  uint64_t total_transfers_ = 0;
};

}  // namespace aios::compute
