// PCI Express on Windows: not probed (SetupAPI could enumerate it; not done
// here). An empty bus, not an error — same as host.hpp's own hardware()
// default for a substrate that cannot enumerate its bus. Mapping a BAR and
// peer-to-peer reachability need privilege most hosts don't have anyway, so
// both are refused outright rather than guessed at.
#include "aios/hal/pci.hpp"

namespace aios::hal {

Result<std::vector<PciFunction>> pci_functions() { return std::vector<PciFunction>{}; }

Result<PciMapping> pci_map_bar(const std::string&, uint8_t, uint64_t) {
  return Result<PciMapping>(Status::Unsupported, "PCI BAR mapping is not implemented on Windows");
}
void pci_unmap_bar(PciMapping& m) { m.base = nullptr; m.size = 0; m.handle = -1; }

Result<bool> pci_peer_reachable(const std::string&, const std::string&, std::string& why) {
  why = "PCI topology is not probed on Windows";
  return false;
}

}  // namespace aios::hal
