// macOS does not let a program enumerate or map the PCI bus. There is a bus
// under this machine, and nothing here can see it, which is the answer.
#include "aios/hal/pci.hpp"

namespace aios::hal {

Result<std::vector<PciFunction>> pci_functions() {
  return Result<std::vector<PciFunction>>(Status::Unsupported,
      "this substrate does not expose the PCI bus to a program");
}
Result<PciMapping> pci_map_bar(const std::string&, uint8_t, uint64_t) {
  return Result<PciMapping>(Status::Unsupported, "this substrate does not let a program map a device BAR");
}
void pci_unmap_bar(PciMapping&) {}
Result<bool> pci_peer_reachable(const std::string&, const std::string&, std::string& why) {
  why = "this substrate does not expose the PCI bus, so it cannot say";
  return Result<bool>(Status::Unsupported, why);
}

}  // namespace aios::hal
