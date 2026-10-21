#include "aios/compute/p2p_dma.hpp"

namespace aios::compute {
namespace {

std::string kind_of(uint32_t class_code) {
  switch ((class_code >> 16) & 0xFF) {
    case 0x01: return "storage";
    case 0x02: return "ethernet";
    case 0x03: return "display";
    case 0x04: return "multimedia";
    case 0x06: return "bridge";
    case 0x08: return "system";
    case 0x0C: return "serial bus";
    default: return "device";
  }
}

}  // namespace

P2PAcceleratorDMA::P2PAcceleratorDMA() {}

Result<std::vector<BusFunction>> P2PAcceleratorDMA::enumerate() const {
  auto fns = hal::pci_functions();
  if (!fns.ok()) return fns.error();
  std::vector<BusFunction> out;
  for (const auto& f : *fns) {
    BusFunction b;
    b.slot = f.slot;
    b.vendor = f.vendor;
    b.device = f.device;
    b.kind = kind_of(f.class_code);
    b.driver = f.driver;
    b.upstream = f.upstream;
    b.bars = f.bars;
    out.push_back(std::move(b));
  }
  return out;
}

Result<DMAMapping> P2PAcceleratorDMA::map_p2p_region(const std::string& src_slot,
                                                     const std::string& dst_slot, uint64_t bytes) {
  auto fns = hal::pci_functions();
  if (!fns.ok()) return fns.error();

  const hal::PciFunction* src = nullptr;
  const hal::PciFunction* dst = nullptr;
  for (const auto& f : *fns) {
    if (f.slot == src_slot) src = &f;
    if (f.slot == dst_slot) dst = &f;
  }
  if (!src) return Result<DMAMapping>(Status::NotFound, "no function at " + src_slot + " on this bus");
  if (!dst) return Result<DMAMapping>(Status::NotFound, "no function at " + dst_slot + " on this bus");

  // The window each end offers is its first prefetchable memory BAR, which is
  // the one an aperture for peer traffic lives behind; failing that, its first
  // memory BAR.
  auto window = [](const hal::PciFunction& f) -> const hal::PciBar* {
    const hal::PciBar* first_memory = nullptr;
    for (const auto& b : f.bars) {
      if (!b.memory) continue;
      if (!first_memory) first_memory = &b;
      if (b.prefetchable) return &b;
    }
    return first_memory;
  };
  const hal::PciBar* sb = window(*src);
  const hal::PciBar* db = window(*dst);
  if (!sb) return Result<DMAMapping>(Status::Unsupported, src_slot + " exposes no memory window");
  if (!db) return Result<DMAMapping>(Status::Unsupported, dst_slot + " exposes no memory window");

  DMAMapping m;
  m.source_slot = src_slot;
  m.target_slot = dst_slot;
  m.source_bar_addr = sb->address;
  m.target_bar_addr = db->address;
  m.source_bar_size = sb->size;
  m.target_bar_size = db->size;
  m.bytes = bytes;

  if (bytes > db->size) {
    m.path = "the target's window is " + std::to_string(db->size) + " bytes; " +
             std::to_string(bytes) + " will not fit in it";
    m.active = false;
    return m;
  }

  std::string why;
  auto reachable = hal::pci_peer_reachable(src_slot, dst_slot, why);
  m.path = why;
  m.active = reachable.ok() && *reachable;
  return m;
}

Result<uint64_t> P2PAcceleratorDMA::transfer_direct(const DMAMapping& mapping) {
  if (!mapping.active) {
    return Result<uint64_t>(Status::Unsupported,
        "these two cannot reach each other: " + mapping.path);
  }
  // Reaching a peer's window means writing to it as a bus master, which this
  // machine cannot arrange from where it runs: a hosted process is not
  // permitted to program another function's DMA engine, and there is no
  // software substitute — copying through system memory is the thing
  // peer-to-peer exists to avoid, and calling that a P2P transfer is what the
  // counter here used to do.
  return Result<uint64_t>(Status::Denied,
      "the mapping is valid (" + mapping.path + ") but this substrate will not let a program "
      "master the bus; the transfer needs the kernel's p2pdma path or a driver of our own");
}

}  // namespace aios::compute
