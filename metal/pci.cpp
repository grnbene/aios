// The PCI bus, on the bare-metal substrate.
//
// A hosted kernel reads the bus out of the substrate's own tables. This one has
// no substrate: it walks configuration space itself through the 0xCF8/0xCFC
// address and data ports, sizes each BAR by the standard write-ones-read-back,
// and works out what is above each function from the bridges it found on the
// way down.
//
// The alternative was to answer Unsupported here and leave the machine unable
// to say what it is made of on the one substrate where it has the most direct
// access to the answer.
#include "aios/hal/pci.hpp"

#include <cstring>

namespace {

inline void outl(uint16_t p, uint32_t v) { asm volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
inline uint32_t inl(uint16_t p) { uint32_t v; asm volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

uint32_t cfg_read(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off) {
  outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
  return inl(0xCFC);
}
void cfg_write(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off, uint32_t v) {
  outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
  outl(0xCFC, v);
}

const char* hexd = "0123456789abcdef";
void put2(std::string& s, uint32_t v) { s += hexd[(v >> 4) & 0xF]; s += hexd[v & 0xF]; }

// The bus address in the notation everything else uses: 0000:bb:dd.f
std::string slot_name(uint32_t bus, uint32_t dev, uint32_t fn) {
  std::string s = "0000:";
  put2(s, bus);
  s += ':';
  put2(s, dev);
  s += '.';
  s += (char)('0' + (fn & 7));
  return s;
}

// A BAR's size is found by writing all ones and reading back which bits the
// function let stick: the lowest set bit is the size. The original value has to
// go back afterwards or the device is left decoding the wrong window.
uint64_t bar_size(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off, bool wide, uint32_t lo, uint32_t hi) {
  cfg_write(bus, dev, fn, off, 0xFFFFFFFFu);
  uint32_t probe_lo = cfg_read(bus, dev, fn, off);
  uint32_t probe_hi = 0;
  if (wide) {
    cfg_write(bus, dev, fn, off + 4, 0xFFFFFFFFu);
    probe_hi = cfg_read(bus, dev, fn, off + 4);
    cfg_write(bus, dev, fn, off + 4, hi);
  }
  cfg_write(bus, dev, fn, off, lo);

  uint64_t mask = ((uint64_t)probe_hi << 32) | (probe_lo & 0xFFFFFFF0u);
  if (!wide) mask |= 0xFFFFFFFF00000000ull;   // the high half is not part of it
  if (mask == 0) return 0;
  return (~mask) + 1;
}

}  // namespace

namespace aios::hal {

Result<std::vector<PciFunction>> pci_functions() {
  std::vector<PciFunction> out;
  // Which bus each bridge leads to, so a function's upstream can be named.
  std::string upstream_of_bus[256];

  for (uint32_t bus = 0; bus < 256; ++bus) {
    for (uint32_t dev = 0; dev < 32; ++dev) {
      uint32_t id0 = cfg_read(bus, dev, 0, 0x00);
      if ((id0 & 0xFFFF) == 0xFFFF) continue;
      uint32_t header0 = cfg_read(bus, dev, 0, 0x0C);
      uint32_t functions = (header0 & 0x00800000u) ? 8 : 1;   // multi-function bit

      for (uint32_t fn = 0; fn < functions; ++fn) {
        uint32_t id = cfg_read(bus, dev, fn, 0x00);
        if ((id & 0xFFFF) == 0xFFFF) continue;

        PciFunction f;
        f.slot = slot_name(bus, dev, fn);
        f.vendor = (uint16_t)(id & 0xFFFF);
        f.device = (uint16_t)(id >> 16);
        f.class_code = cfg_read(bus, dev, fn, 0x08) >> 8;
        f.upstream = upstream_of_bus[bus];
        f.numa_node = -1;
        // Nothing on this machine "claims" a function the way a hosted kernel's
        // driver model does; the machine either drives it or it does not.
        f.driver.clear();

        uint32_t header_type = (cfg_read(bus, dev, fn, 0x0C) >> 16) & 0x7F;
        if (header_type == 0x01) {
          // A bridge. Everything on the bus behind it hangs off this function.
          uint32_t buses = cfg_read(bus, dev, fn, 0x18);
          uint32_t secondary = (buses >> 8) & 0xFF;
          if (secondary > bus && secondary < 256) upstream_of_bus[secondary] = f.slot;
        }

        // Header type 0 has six BARs; a bridge has two.
        uint32_t bar_count = (header_type == 0x01) ? 2 : 6;
        for (uint32_t i = 0; i < bar_count; ++i) {
          uint32_t off = 0x10 + i * 4;
          uint32_t lo = cfg_read(bus, dev, fn, off);
          if (lo == 0) continue;
          PciBar b;
          b.index = (uint8_t)i;
          b.memory = (lo & 0x1) == 0;
          if (!b.memory) {
            b.address = lo & 0xFFFFFFFCu;
            b.size = bar_size(bus, dev, fn, off, false, lo, 0);
            f.bars.push_back(b);
            continue;
          }
          b.wide = ((lo >> 1) & 0x3) == 0x2;
          b.prefetchable = (lo & 0x8) != 0;
          uint32_t hi = b.wide ? cfg_read(bus, dev, fn, off + 4) : 0;
          b.address = (uint64_t)(lo & 0xFFFFFFF0u) | ((uint64_t)hi << 32);
          b.size = bar_size(bus, dev, fn, off, b.wide, lo, hi);
          f.bars.push_back(b);
          if (b.wide) ++i;   // the pair occupies two BAR slots
        }
        out.push_back(std::move(f));
      }
    }
  }
  return out;
}

Result<PciMapping> pci_map_bar(const std::string& slot, uint8_t bar, uint64_t bytes) {
  auto all = pci_functions();
  if (!all.ok()) return all.error();
  for (const auto& f : *all) {
    if (f.slot != slot) continue;
    for (const auto& b : f.bars) {
      if (b.index != bar) continue;
      if (!b.memory) return Result<PciMapping>(Status::Unsupported, "that BAR is an I/O port range, not memory");
      // This kernel is identity mapped, so a device's window is already
      // addressable: the physical address IS the pointer.
      PciMapping m;
      m.base = reinterpret_cast<void*>(b.address);
      m.size = bytes && bytes < b.size ? bytes : b.size;
      m.slot = slot;
      m.bar = bar;
      m.handle = -1;
      return m;
    }
    return Result<PciMapping>(Status::NotFound, "that function has no such BAR");
  }
  return Result<PciMapping>(Status::NotFound, "no function at that slot");
}

void pci_unmap_bar(PciMapping& m) {
  // Nothing to undo: the window was never mapped anywhere, it was already
  // addressable.
  m.base = nullptr;
  m.size = 0;
}

Result<bool> pci_peer_reachable(const std::string& a, const std::string& b, std::string& why) {
  if (a == b) { why = "a function cannot be its own peer"; return false; }
  auto all = pci_functions();
  if (!all.ok()) { why = all.error().message; return all.error(); }

  const PciFunction* fa = nullptr;
  const PciFunction* fb = nullptr;
  for (const auto& f : *all) {
    if (f.slot == a) fa = &f;
    if (f.slot == b) fb = &f;
  }
  if (!fa || !fb) { why = "one of those functions is not on this machine's bus"; return false; }

  if (fa->upstream.empty() && fb->upstream.empty()) {
    why = "both are on the root bus, which forwards between them";
    return true;
  }
  if (fa->upstream != fb->upstream) {
    why = a + " sits behind " + (fa->upstream.empty() ? "the root bus" : fa->upstream) +
          " and " + b + " behind " + (fb->upstream.empty() ? "the root bus" : fb->upstream) +
          "; a transfer between them crosses the root complex";
    return false;
  }
  why = "both are behind " + fa->upstream + ", which can forward between them";
  return true;
}

}  // namespace aios::hal
