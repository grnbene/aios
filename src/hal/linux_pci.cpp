// PCI Express on Linux, read from the bus the kernel already enumerated.
//
// /sys/bus/pci/devices/<slot>/resource is the kernel's own table of where each
// function answers and how much space it claims — the same numbers a
// configuration-space read would produce, already decoded. resourceN can be
// mapped for direct access where the substrate permits it.
#include "aios/hal/pci.hpp"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace aios::hal {
namespace {

std::string read_line(const std::string& path) {
  FILE* f = ::fopen(path.c_str(), "r");
  if (!f) return {};
  char buf[256] = {0};
  if (!::fgets(buf, sizeof buf, f)) { ::fclose(f); return {}; }
  ::fclose(f);
  std::string s(buf);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

uint32_t read_hex(const std::string& path) {
  std::string v = read_line(path);
  if (v.empty()) return 0;
  return (uint32_t)std::strtoul(v.c_str(), nullptr, 16);
}

std::string link_target(const std::string& path) {
  char buf[512] = {0};
  ssize_t n = ::readlink(path.c_str(), buf, sizeof buf - 1);
  if (n <= 0) return {};
  std::string s(buf, (size_t)n);
  auto slash = s.find_last_of('/');
  return slash == std::string::npos ? s : s.substr(slash + 1);
}

// The bridge or root port directly above a function, which is what decides
// whether two endpoints can talk without going up to the host bridge.
std::string upstream_of(const std::string& slot) {
  // /sys/bus/pci/devices/<slot> is a symlink into the device tree; its parent
  // directory is the port above it.
  char buf[1024] = {0};
  std::string link = "/sys/bus/pci/devices/" + slot;
  ssize_t n = ::readlink(link.c_str(), buf, sizeof buf - 1);
  if (n <= 0) return {};
  std::string path(buf, (size_t)n);
  auto last = path.find_last_of('/');
  if (last == std::string::npos || last == 0) return {};
  auto prev = path.find_last_of('/', last - 1);
  if (prev == std::string::npos) return {};
  std::string parent = path.substr(prev + 1, last - prev - 1);
  // A parent that is not a bus address is the root of the tree.
  if (parent.size() < 7 || parent.find(':') == std::string::npos) return {};
  return parent;
}

}  // namespace

Result<std::vector<PciFunction>> pci_functions() {
  const std::string root = "/sys/bus/pci/devices";
  DIR* d = ::opendir(root.c_str());
  if (!d) return Result<std::vector<PciFunction>>(Status::Unsupported, "this machine exposes no PCI bus");

  std::vector<PciFunction> out;
  while (dirent* e = ::readdir(d)) {
    std::string slot = e->d_name;
    if (slot.empty() || slot[0] == '.') continue;
    std::string dir = root + "/" + slot;

    PciFunction f;
    f.slot = slot;
    f.vendor = (uint16_t)read_hex(dir + "/vendor");
    f.device = (uint16_t)read_hex(dir + "/device");
    f.class_code = read_hex(dir + "/class");
    f.driver = link_target(dir + "/driver");
    f.upstream = upstream_of(slot);
    std::string numa = read_line(dir + "/numa_node");
    f.numa_node = numa.empty() ? -1 : (int)std::strtol(numa.c_str(), nullptr, 10);

    // Each line of `resource` is "start end flags", one per BAR, in hex.
    FILE* r = ::fopen((dir + "/resource").c_str(), "r");
    if (r) {
      char line[256];
      for (uint8_t i = 0; i < 6 && ::fgets(line, sizeof line, r); ++i) {
        unsigned long long start = 0, end = 0, flags = 0;
        if (std::sscanf(line, "%llx %llx %llx", &start, &end, &flags) != 3) continue;
        if (start == 0 && end == 0) continue;
        PciBar b;
        b.index = i;
        b.address = start;
        b.size = end >= start ? (end - start + 1) : 0;
        b.memory = (flags & 0x1ull) == 0;          // IORESOURCE_IO is bit 8 in sysfs flags
        b.prefetchable = (flags & 0x2000ull) != 0; // IORESOURCE_PREFETCH
        b.wide = (flags & 0x100ull) != 0;          // IORESOURCE_MEM_64
        f.bars.push_back(b);
      }
      ::fclose(r);
    }
    out.push_back(std::move(f));
  }
  ::closedir(d);
  return out;
}

Result<PciMapping> pci_map_bar(const std::string& slot, uint8_t bar, uint64_t bytes) {
  std::string path = "/sys/bus/pci/devices/" + slot + "/resource" + std::to_string(bar);
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) {
    return Result<PciMapping>(Status::NotFound, slot + " has no BAR " + std::to_string(bar));
  }
  uint64_t size = bytes ? bytes : (uint64_t)st.st_size;
  if (size == 0) return Result<PciMapping>(Status::Invalid, "that BAR claims no space");

  int fd = ::open(path.c_str(), O_RDWR | O_SYNC);
  if (fd < 0) {
    // Read-only is still worth having: the machine can inspect a device's
    // registers without being allowed to drive it.
    fd = ::open(path.c_str(), O_RDONLY | O_SYNC);
    if (fd < 0) {
      return Result<PciMapping>(Status::Denied,
          "the substrate will not open " + path + " (mapping a device's registers needs privilege)");
    }
  }
  void* base = ::mmap(nullptr, (size_t)size, PROT_READ, MAP_SHARED, fd, 0);
  if (base == MAP_FAILED) {
    ::close(fd);
    return Result<PciMapping>(Status::Denied, "the substrate refused to map that BAR");
  }
  PciMapping m;
  m.base = base;
  m.size = size;
  m.slot = slot;
  m.bar = bar;
  m.handle = fd;
  return m;
}

void pci_unmap_bar(PciMapping& m) {
  if (m.base) ::munmap(m.base, (size_t)m.size);
  if (m.handle >= 0) ::close(m.handle);
  m.base = nullptr;
  m.size = 0;
  m.handle = -1;
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

  // Two endpoints can move data between themselves without going up to system
  // memory when the fabric between them forwards it — which on this substrate
  // means a shared upstream port, and the kernel's own p2pdma opinion where it
  // has one.
  if (fa->upstream.empty() || fb->upstream.empty()) {
    why = "one of those functions hangs directly off the host bridge; a transfer between them goes through system memory";
    return false;
  }
  if (fa->upstream != fb->upstream) {
    why = a + " is under " + fa->upstream + " and " + b + " is under " + fb->upstream +
          "; a transfer between them crosses the root complex";
    return false;
  }
  why = "both are under " + fa->upstream + ", which can forward between them";
  return true;
}

}  // namespace aios::hal
