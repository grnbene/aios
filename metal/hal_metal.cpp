// hal::Host for bare metal x86-64: UART console, TSC clock, RDRAND entropy,
// mappings from the kernel's own memory, and a memory-resident file tree for
// the persistent root. No storage or network drivers: those are later
// milestones and deliberately absent from the first boot.
#include "aios/hal/host.hpp"
#include "aios/hal/net.hpp"
#include "aios/hal/sync.hpp"
#include "aios/hal/volume.hpp"
#include "aios/hal/fabric.hpp"
#include "aios/net/inet.hpp"
#include "aios/hal/keys.hpp"
#include "aios/hal/tty.hpp"
#include "aios/hal/account.hpp"
extern aios::net::NetStack* metal_net;   // defined in metal/main.cpp (global scope)
#include <map>
#include <vector>
#include <string>

extern "C" void metal_console_write(const char* s, size_t n);
extern "C" void* metal_alloc_pages(size_t bytes);
extern "C" size_t metal_heap_used();

namespace {
inline void outb(uint16_t port, uint8_t v) { asm volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
inline uint8_t inb(uint16_t port) { uint8_t v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
// Who is actually driving the screen — needed by the bus report, which is far
// above the code that answers these.
extern "C" bool metal_virtio_gpu_active();
extern "C" bool metal_e1000_drives(uint16_t device_id);
namespace { bool g_kernel_set_mode = false; }

inline void outw(uint16_t port, uint16_t v) { asm volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
inline uint16_t inw(uint16_t port) { uint16_t v; asm volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
inline void outl(uint16_t port, uint32_t v) { asm volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }
inline uint32_t inl(uint16_t port) { uint32_t v; asm volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
inline uint64_t rdtsc() { uint32_t lo, hi; asm volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }
constexpr uint16_t COM1 = 0x3F8;
uint64_t tsc_hz = 0;

// ---- The wall clock: the CMOS real-time clock, read once at the first ask and
// carried forward on the monotonic clock. The RTC is taken to hold UTC, as
// QEMU's (-rtc base=utc, its default) and a Linux-installed PC's do.
extern "C" uint64_t metal_ticks();
extern "C" uint32_t metal_tick_hz();
extern "C" uint64_t metal_now_ns() {
  if (tsc_hz) return (rdtsc() / (tsc_hz / 1000000ull)) * 1000ull;
  const uint32_t hz = metal_tick_hz() ? metal_tick_hz() : 100;
  const uint64_t t = metal_ticks();
  return t / hz * 1000000000ull + (t % hz) * (1000000000ull / hz);
}
namespace {
inline void rtc_outb(uint16_t p, uint8_t v) { asm volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
inline uint8_t rtc_inb(uint16_t p) { uint8_t v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
uint8_t cmos(uint8_t reg) { rtc_outb(0x70, (uint8_t)(0x80 | reg)); return rtc_inb(0x71); }   // bit 7 keeps NMI masked
struct RtcTime { uint8_t sec, min, hour, day, mon, year, century; };
RtcTime rtc_raw() {
  for (int spin = 0; spin < 1000000 && (cmos(0x0A) & 0x80); ++spin) {}   // wait out an update in progress
  return RtcTime{cmos(0x00), cmos(0x02), cmos(0x04), cmos(0x07), cmos(0x08), cmos(0x09), cmos(0x32)};
}
int64_t days_from_civil_utc(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}
// Unix seconds from the RTC, or -1 when what it holds is not a date.
int64_t rtc_unix_seconds() {
  RtcTime a = rtc_raw(), b = rtc_raw();
  for (int tries = 0; tries < 5 && (a.sec != b.sec || a.min != b.min || a.hour != b.hour || a.day != b.day); ++tries) { a = b; b = rtc_raw(); }
  const uint8_t status_b = cmos(0x0B);
  const bool binary = status_b & 0x04, h24 = status_b & 0x02;
  auto bcd = [&](uint8_t v) { return binary ? v : (uint8_t)((v >> 4) * 10 + (v & 0x0F)); };
  const bool pm = !h24 && (b.hour & 0x80);
  int hour = bcd((uint8_t)(b.hour & 0x7F));
  if (!h24) hour = (hour % 12) + (pm ? 12 : 0);
  const int sec = bcd(b.sec), min = bcd(b.min), day = bcd(b.day), mon = bcd(b.mon);
  int year = bcd(b.year);
  const int century = bcd(b.century);
  year += (century >= 19 && century <= 30) ? century * 100 : 2000;
  if (sec > 59 || min > 59 || hour > 23 || day < 1 || day > 31 || mon < 1 || mon > 12 || year < 2000) return -1;
  return days_from_civil_utc(year, (unsigned)mon, (unsigned)day) * 86400 + hour * 3600 + min * 60 + sec;
}
int64_t g_rtc_base_ns = 0;      // wall clock at g_rtc_at_ns on the monotonic clock
uint64_t g_rtc_at_ns = 0;
int g_rtc_state = 0;            // 0 unread, 1 good, -1 no usable RTC
}  // namespace
extern "C" bool metal_wall_unix_ns(int64_t* out) {
  if (g_rtc_state == 0) {
    const int64_t s = rtc_unix_seconds();
    g_rtc_at_ns = metal_now_ns();
    g_rtc_state = s < 0 ? -1 : 1;
    g_rtc_base_ns = s < 0 ? 0 : s * 1000000000ll;
  }
  if (g_rtc_state < 0) return false;
  // The TSC calibration may arrive after the first read; the base was taken
  // on whichever clock metal_now_ns had then, and both count from boot.
  if (out) *out = g_rtc_base_ns + (int64_t)(metal_now_ns() - g_rtc_at_ns);
  return true;
}
uint64_t g_total_memory = 0;
uint32_t g_cpus = 1;
}
aios::hal::Volume* metal_volume = nullptr;        // mounted AOS volume, if any
aios::hal::BlockDevice* metal_disk = nullptr;     // the first disk, if any
// Defined below, next to the byte-stream calls that use it; the host needs it
// earlier than that to describe the machine's network.
extern aios::hal::Fabric* metal_fabric;

extern "C" void metal_console_init() {
  outb(COM1 + 1, 0x00); outb(COM1 + 3, 0x80); outb(COM1 + 0, 0x01); outb(COM1 + 1, 0x00);   // 115200
  outb(COM1 + 3, 0x03); outb(COM1 + 2, 0xC7); outb(COM1 + 4, 0x0B);
}
extern "C" void metal_display_write(const char* s, size_t n);
extern "C" void metal_console_write(const char* s, size_t n) {
  for (size_t i = 0; i < n; ++i) { while (!(inb(COM1 + 5) & 0x20)) {} if (s[i] == '\n') outb(COM1, '\r'); outb(COM1, (uint8_t)s[i]); }
  metal_display_write(s, n);
}
extern "C" uint64_t metal_rdtsc() { return rdtsc(); }
extern "C" int metal_console_read() { return (inb(COM1 + 5) & 1) ? inb(COM1) : -1; }

// Calibrate the TSC against PIT channel 2 for ~50 ms.
extern "C" void metal_clock_init() {
  outb(0x61, (inb(0x61) & 0xFD) | 0x01);   // gate on, speaker off
  outb(0x43, 0xB0);                         // ch2, lobyte/hibyte, mode 0
  uint16_t count = 59659;                   // 1193182 Hz × 0.05 s
  outb(0x42, count & 0xFF); outb(0x42, count >> 8);
  uint64_t t0 = rdtsc();
  while (!(inb(0x61) & 0x20)) {}
  uint64_t t1 = rdtsc();
  tsc_hz = (t1 - t0) * 20;
  if (tsc_hz < 100000000ull) tsc_hz = 1000000000ull;   // implausible → assume 1 GHz, still monotonic
}
extern "C" void metal_set_memory(uint64_t total, uint32_t cpus) { g_total_memory = total; g_cpus = cpus; }
extern "C" uint32_t metal_rand32() {
  uint32_t c=0; asm volatile("mov $1,%%eax; cpuid":"=c"(c)::"eax","ebx","edx");
  if (c & (1u<<30)) { uint32_t v; uint8_t ok; for (int i=0;i<10;++i){ asm volatile("rdrand %0; setc %1":"=r"(v),"=qm"(ok)); if(ok) return v; } }
  uint64_t t=rdtsc(); t^=t>>17; t*=0x9E3779B97F4A7C15ull; return (uint32_t)(t>>32);
}
extern "C" uint64_t metal_tsc_hz() { return tsc_hz; }

namespace aios::hal {

namespace {
struct MemNode { bool dir = false; ByteBuffer data; std::map<std::string, MemNode> children; };

std::vector<std::string> split_path(const std::string& p) { std::vector<std::string> parts; std::string cur; for (char c : p) { if (c == '/') { if (!cur.empty()) parts.push_back(cur); cur.clear(); } else cur.push_back(c); } if (!cur.empty()) parts.push_back(cur); return parts; }

class MetalHost final : public Host {
 public:
  MetalHost() { root_.dir = true; topo_.logical_cpus = g_cpus; topo_.total_memory = g_total_memory; topo_.available_memory = g_total_memory; topo_.arch = "x86_64"; topo_.host_name = "bare-metal"; topo_.numa.push_back(NumaNode{0, g_total_memory, {0}}); }

  Result<Mapping> map(uint64_t size, MappingKind kind, const std::string& path) override {
    if (size == 0) return Result<Mapping>(Status::Invalid, "zero-size mapping");
    Mapping m; m.size = size; m.kind = kind; m.path = path;
    // Persistent mappings are memory-resident until a block driver exists; a
    // mapping re-opened by path sees its previous contents.
    if (kind == MappingKind::Persistent) {
      auto it = persistent_.find(path);
      if (it != persistent_.end() && it->second.second >= size) { m.base = it->second.first; return m; }
    }
    void* p = metal_alloc_pages((size + 4095) & ~4095ull);
    if (!p) return Result<Mapping>(Status::Exhausted, "out of kernel memory");
    m.base = p;
    if (kind == MappingKind::Persistent) {
      persistent_[path] = {p, size};
      // A persistent mapping re-opened by path comes back with its contents from the volume.
      if (vol()) { auto f = vol()->read_file(path); if (f.ok()) std::memcpy(p, f->data(), std::min<size_t>(f->size(), size)); }
    }
    return m;
  }
  void unmap(Mapping& m) override { m.base = nullptr; }   // pages return when a page allocator with reclaim lands
  Result<void> sync(const Mapping& m) override {
    if (m.kind != MappingKind::Persistent || !m.base || !vol()) return Ok();
    return vol()->write_file(m.path, ByteSpan(static_cast<const uint8_t*>(m.base), m.size));
  }
  Volume* vol() { return metal_volume && metal_volume->mounted() ? metal_volume : nullptr; }
  // The RAM tree's contents move onto a freshly mounted volume (the live session continues on disk).
  Result<void> adopt_volume() {
    if (!vol()) return Result<void>(Status::NotFound, "no mounted volume");
    std::vector<std::pair<std::string, MemNode*>> stack{{"", &root_}};
    while (!stack.empty()) {
      auto [path, n] = stack.back(); stack.pop_back();
      for (auto& [name, child] : n->children) { std::string cp = path + "/" + name; if (child.dir) { AIOS_TRY(vol()->make_dir(cp)); stack.push_back({cp, &child}); } else AIOS_TRY(vol()->write_file(cp, child.data)); }
    }
    for (auto& [path, pm] : persistent_) AIOS_TRY(vol()->write_file(path, ByteSpan(static_cast<const uint8_t*>(pm.first), pm.second)));
    return Ok();
  }

  MemNode* find(const std::string& path, bool create_dirs) {
    MemNode* n = &root_;
    for (auto& part : split_path(path)) {
      auto it = n->children.find(part);
      if (it == n->children.end()) { if (!create_dirs) return nullptr; MemNode d; d.dir = true; it = n->children.emplace(part, d).first; }
      n = &it->second;
    }
    return n;
  }
  Result<void> write_file(const std::string& path, ByteSpan data) override {
    if (vol()) return vol()->write_file(path, data);
    auto parts = split_path(path); if (parts.empty()) return Result<void>(Status::Invalid);
    std::string name = parts.back(); parts.pop_back();
    std::string dir; for (auto& p : parts) dir += "/" + p;
    MemNode* d = find(dir, true); MemNode f; f.data.assign(data.begin(), data.end()); d->children[name] = f; return Ok();
  }
  Result<void> append_file(const std::string& path, ByteSpan data) override {
    if (vol()) return vol()->append_file(path, data);
    MemNode* f = find(path, false);
    if (!f) { return write_file(path, data); }
    f->data.insert(f->data.end(), data.begin(), data.end()); return Ok();
  }
  Result<ByteBuffer> read_file(const std::string& path) override { if (vol()) return vol()->read_file(path); MemNode* f = find(path, false); if (!f || f->dir) return Result<ByteBuffer>(Status::NotFound, path); return f->data; }
  Result<std::vector<std::string>> list_dir(const std::string& path) override { if (vol()) return vol()->list_dir(path); MemNode* d = find(path, false); if (!d || !d->dir) return Result<std::vector<std::string>>(Status::NotFound, path); std::vector<std::string> out; for (auto& [k, v] : d->children) out.push_back(k); return out; }
  Result<void> make_dir(const std::string& path) override { if (vol()) return vol()->make_dir(path); find(path, true); return Ok(); }
  bool exists(const std::string& path) override { if (vol()) return vol()->exists(path); return find(path, false) != nullptr; }
  Result<FileInfo> stat(const std::string& path) override {
    if (vol()) { if (vol()->is_dir(path)) return FileInfo{true, 0}; auto sz = vol()->size(path); if (!sz.ok()) return sz.error(); return FileInfo{false, *sz}; }
    MemNode* n = find(path, false); if (!n) return Result<FileInfo>(Status::NotFound, path); return FileInfo{n->dir, (uint64_t)n->data.size()};
  }
  Result<void> remove(const std::string& path) override {
    if (vol()) return vol()->remove(path);
    auto parts = split_path(path); if (parts.empty()) return Result<void>(Status::Invalid);
    std::string name = parts.back(); parts.pop_back(); std::string dir; for (auto& p : parts) dir += "/" + p;
    MemNode* d = find(dir, false); if (!d) return Result<void>(Status::NotFound, path);
    auto it = d->children.find(name); if (it == d->children.end()) return Result<void>(Status::NotFound, path);
    if (it->second.dir && !it->second.children.empty()) return Result<void>(Status::Invalid, "directory not empty");
    d->children.erase(it); return Ok();
  }

  uint64_t monotonic_ns() override { return tsc_hz ? (rdtsc() / (tsc_hz / 1000000ull)) * 1000ull : rdtsc(); }
  uint64_t wall_ns() override {
    int64_t ns = 0;
    if (metal_wall_unix_ns(&ns) && ns > 0) return (uint64_t)ns;
    return 1788393600ull * 1000000000ull + monotonic_ns();   // no readable RTC: the build epoch plus uptime
  }
  void random(MutableByteSpan out) override {
    uint32_t a, b, c, d; asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    bool rdrand = c & (1u << 30);
    for (size_t i = 0; i < out.size(); i += 8) {
      uint64_t v; unsigned char ok = 0;
      if (rdrand) asm volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
      if (!ok) v = rdtsc() * 0x9E3779B97F4A7C15ull ^ (uint64_t)i;
      for (size_t k = 0; k < 8 && i + k < out.size(); ++k) out[i + k] = (uint8_t)(v >> (8 * k));
    }
  }
  // Free memory is asked for, not remembered: the heap grows while the machine
  // runs, and a gauge on the desktop that never moved would be a picture.
  const Topology& topology() override {
    size_t used = metal_heap_used();
    topo_.available_memory = g_total_memory > used ? g_total_memory - used : 0;
    return topo_;
  }

  // With no operating system underneath, the machine's storage IS the disk and
  // whatever AOS has written on it. The raw device is reported alongside the
  // AOS volume when one is mounted, so the same two questions the hosted
  // substrates answer are answerable here.
  Result<std::vector<StorageVolume>> storage(const std::string& of_interest) override {
    std::vector<StorageVolume> out;
    if (metal_disk) {
      StorageVolume dev;
      dev.name = metal_disk->name();
      dev.kind = "device";
      dev.block_size = SECTOR;
      dev.capacity = metal_disk->sectors() * SECTOR;
      out.push_back(dev);
    }
    StorageVolume root;
    root.path = "/";
    root.kind = "volume";
    root.block_size = SECTOR;
    root.holds_root = true;
    if (vol()) {
      const auto& i = vol()->info();
      root.name = i.label.empty() ? "AIOS" : i.label;
      root.capacity = i.sectors * SECTOR;
      root.used = i.bytes_live;
    } else {
      // Until a disk is claimed the root is memory, which is what the machine
      // says at boot; it belongs in the answer rather than only in the banner.
      root.name = "memory";
      root.block_size = 0;
      root.capacity = g_total_memory;
      root.used = metal_heap_used();
    }
    out.insert(out.begin(), root);
    return out;
  }

  // Every function on the machine's PCI buses, read out of configuration space
  // through the 0xCF8/0xCFC window. This is the machine describing its own
  // hardware: nothing here comes from a table of what a QEMU guest usually
  // has, and on real iron it reports that iron.
  Result<std::vector<HardwareDevice>> hardware() override {
    std::vector<HardwareDevice> out;
    auto cfg = [](uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off) {
      outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
      return inl(0xCFC);
    };
    auto hex = [](uint32_t v, int digits) {
      const char* d = "0123456789abcdef";
      std::string s;
      for (int i = digits - 1; i >= 0; --i) s.push_back(d[(v >> (i * 4)) & 0xF]);
      return s;
    };
    auto dec2 = [](uint32_t v) {
      std::string s;
      s.push_back(static_cast<char>('0' + (v / 10) % 10));
      s.push_back(static_cast<char>('0' + v % 10));
      return s;
    };
    for (uint32_t bus = 0; bus < 8; ++bus) {
      for (uint32_t dev = 0; dev < 32; ++dev) {
        uint32_t id0 = cfg(bus, dev, 0, 0);
        if ((id0 & 0xFFFF) == 0xFFFF) continue;
        uint32_t header = (cfg(bus, dev, 0, 0x0C) >> 16) & 0xFF;
        uint32_t fns = (header & 0x80) ? 8u : 1u;
        for (uint32_t fn = 0; fn < fns; ++fn) {
          uint32_t id = cfg(bus, dev, fn, 0);
          uint32_t vendor = id & 0xFFFF, device = (id >> 16) & 0xFFFF;
          if (vendor == 0xFFFF) continue;
          uint32_t cls = cfg(bus, dev, fn, 0x08);
          uint32_t base = (cls >> 24) & 0xFF, sub = (cls >> 16) & 0xFF;
          HardwareDevice d;
          d.slot = hex(bus, 2) + ":" + hex(dev, 2) + "." + hex(fn, 1);
          switch (base) {
            case 0x00: d.kind = "legacy"; break;
            case 0x01: d.kind = "storage"; break;
            case 0x02: d.kind = "ethernet"; break;
            case 0x03: d.kind = "display"; break;
            case 0x04: d.kind = "multimedia"; break;
            case 0x06: d.kind = (sub == 0x00) ? "host bridge" : (sub == 0x01 ? "isa bridge" : "bridge"); break;
            case 0x07: d.kind = "serial"; break;
            case 0x08: d.kind = "system"; break;
            case 0x0C: d.kind = (sub == 0x03) ? "usb" : "serial bus"; break;
            case 0x0D: d.kind = "wireless"; break;
            default: d.kind = "class " + hex(base, 2); break;
          }
          const char* vname = nullptr;
          switch (vendor) {
            case 0x8086: vname = "Intel"; break;
            case 0x1234: vname = "Bochs/QEMU"; break;
            case 0x1af4: vname = "Virtio"; break;
            case 0x1b36: vname = "Red Hat"; break;
            case 0x10de: vname = "NVIDIA"; break;
            case 0x1002: vname = "AMD"; break;
            case 0x15ad: vname = "VMware"; break;
            case 0x1414: vname = "Microsoft"; break;
            case 0x1022: vname = "AMD"; break;
            default: break;
          }
          d.name = (vname ? std::string(vname) : hex(vendor, 4)) + " " + hex(device, 4);
          // What this kernel has actually bound, rather than what Linux would
          // have bound: the two virtio drivers are the only claimants here.
          if (vendor == 0x1af4 && device == 0x1001) d.driver = "aios virtio-blk";
          else if (vendor == 0x1af4 && device == 0x1000) d.driver = "aios virtio-net";
          else if (vendor == 0x8086 && metal_e1000_drives(device)) d.driver = "aios e1000";
          else if (base == 0x03) {
            // Say what is actually driving the screen. Crediting the loader for
            // a framebuffer this kernel brought up itself is exactly the kind
            // of thing this pane exists to stop.
            if (vendor == 0x1af4 && (device == 0x1050 || device == 0x1010) && metal_virtio_gpu_active())
              d.driver = "aios virtio-gpu";
            else if (g_kernel_set_mode)
              d.driver = "aios dispi mode-set";
            else
              d.driver = "loader framebuffer";
          }
          (void)dec2;
          out.push_back(d);
        }
      }
    }
    return out;
  }

  // The NIC carries AOS's own fabric over raw Ethernet: there is no IP stack
  // to have an address in, so the hardware address is the whole answer.
  Result<std::vector<NetworkInterface>> network() override {
    std::vector<NetworkInterface> out;
    if (!metal_fabric) return out;
    FrameDevice& dev = metal_fabric->device();
    NetworkInterface n;
    n.name = dev.name();
    n.hardware = mac_str(dev.mac());
    n.mtu = 1500;
    n.up = true;
    n.carries_fabric = true;
    out.push_back(n);
    return out;
  }
 private:
  MemNode root_;
  std::map<std::string, std::pair<void*, uint64_t>> persistent_;
  Topology topo_;
};
}  // namespace

MetalHost* g_host = nullptr;
void compute_yield() {}   // this image runs no fleet link yet: nothing to keep alive while a job computes
std::unique_ptr<Host> make_host() { auto h = std::make_unique<MetalHost>(); if (!g_host) g_host = h.get(); return h; }
}  // namespace aios::hal
// Called by the host tool after formatting: the running session moves onto the volume.
aios::Result<void> metal_adopt_volume() { return aios::hal::g_host ? aios::hal::g_host->adopt_volume() : aios::Result<void>(aios::Status::NotFound, "no host"); }
namespace aios::hal {

// No dynamic loading, no network on the first boot.
#include "aios/hal/parse_double_portable.inc"
double parse_double(const char* s, char** end) { return aios_parse_double_portable(s, end); }
void host_note(const std::string& text) { metal_console_write(text.c_str(), text.size()); }
void* load_library(const std::string&) { return nullptr; }
void* library_symbol(void*, const std::string&) { return nullptr; }
void unload_library(void*) {}
std::string library_error() { return "no dynamic loading on bare metal"; }
}  // namespace aios::hal
aios::hal::Fabric* metal_fabric = nullptr;   // set by main once a NIC is found
namespace aios::hal {
// The fabric has no loopback interface to bind alone: on metal a listener is
// reachable by whatever the NIC can hear, and the request token carries the
// restriction instead.
void net_service(int timeout_ms) { sleep_ms(timeout_ms > 0 ? static_cast<uint32_t>(timeout_ms) : 0); }
Result<std::unique_ptr<UdpSocket>> udp_open(const std::string&, uint16_t) {
  return Result<std::unique_ptr<UdpSocket>>(Status::Unsupported, "no UDP sockets on the x86 metal substrate yet");
}
Result<std::unique_ptr<Listener>> net_listen(uint16_t port, bool /*loopback_only*/) {
  if (!metal_fabric) return Result<std::unique_ptr<Listener>>(Status::Unsupported, "no network device on this host");
  return metal_fabric->listen(port);
}
// No controlling terminal to put a question to: the console belongs to the shell,
// not to a library call in the middle of a protocol. ssh treats this as nobody to ask.
// No accounts and no host name: one machine, one owner, nothing to look a person up in.
Result<LocalAccount> local_account() {
  return Result<LocalAccount>(Status::Unsupported, "no accounts on the x86 metal substrate");
}
std::string local_hostname() { return {}; }
Result<std::string> tty_ask(const std::string&, bool) {
  return Result<std::string>(Status::Unsupported, "no terminal to ask on the x86 metal substrate");
}
// No filesystem sockets on this substrate. src/ calls these (the ASQL wire
// server's unix socket, the FastCGI client's unix:/path), so they must link.
Result<std::unique_ptr<Listener>> net_listen_unix(const std::string&) {
  return Result<std::unique_ptr<Listener>>(Status::Unsupported, "no unix sockets on the x86 metal substrate");
}
Result<std::unique_ptr<Connection>> net_connect_unix(const std::string&, int) {
  return Result<std::unique_ptr<Connection>>(Status::Unsupported, "no unix sockets on the x86 metal substrate");
}
// Named pipes are a Windows facility; nothing on this substrate has one.
Result<std::unique_ptr<Connection>> net_connect_pipe(const std::string&, int) {
  return Result<std::unique_ptr<Connection>>(Status::Unsupported, "no named pipes on the x86 metal substrate");
}
// host = a peer MAC ("52:54:00:12:34:56") or "any" (broadcast: the first kernel listening on the port answers).
Result<std::unique_ptr<Connection>> net_connect(const std::string& host, uint16_t port, int timeout_ms) {
  // A MAC (or "any") is an AOS fabric peer; anything else (an IP or a hostname)
  // goes through AOS's own TCP/IP stack — a real internet host, no host sockets.
  Mac peer{}; bool bcast = host == "any" || host == "broadcast" || host == "*";
  if (bcast || parse_mac(host, peer)) {
    if (!metal_fabric) return Result<std::unique_ptr<Connection>>(Status::Unsupported, "no network device on this host");
    return metal_fabric->connect(peer, bcast, port, timeout_ms);
  }
  if (!metal_net || !metal_net->up()) return Result<std::unique_ptr<Connection>>(Status::Unsupported, "TCP/IP is not up (host net up)");
  auto s = metal_net->connect(host, port, timeout_ms);
  if (!s.ok()) return s.error();
  return std::unique_ptr<Connection>(std::move(*s));
}

// Ticket spinlock: correct on one core (uncontended) and on many (APs contend).
Mutex::Mutex() { for (auto& b : storage_) b = 0; }
Mutex::~Mutex() {}
void Mutex::lock() {
  auto* next = reinterpret_cast<volatile uint32_t*>(storage_);
  auto* serving = reinterpret_cast<volatile uint32_t*>(storage_ + 4);
  uint32_t ticket = __atomic_fetch_add(next, 1, __ATOMIC_ACQ_REL);
  while (__atomic_load_n(serving, __ATOMIC_ACQUIRE) != ticket) asm volatile("pause");
}
void Mutex::unlock() {
  auto* serving = reinterpret_cast<volatile uint32_t*>(storage_ + 4);
  __atomic_fetch_add(serving, 1, __ATOMIC_RELEASE);
}
void release_idle_stack() {}
void sleep_ms(uint32_t ms) { uint64_t end = rdtsc() + (tsc_hz / 1000) * ms; while (rdtsc() < end) {} }
// This core's id for RecursiveMutex: the initial APIC id from CPUID leaf 1.
static uint64_t metal_cpu_token() {
  uint32_t a = 1, b, c = 0, d;
  asm volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
  return b >> 24;
}
#define AIOS_CPU_RELAX() asm volatile("pause")
#include "sync_common.inc"

}  // namespace aios::hal

// ---- display and keyboard: the console on a screen, PS/2 keys ----
// Two ways to have a screen: a linear framebuffer the loader set up (the only
// option under UEFI, where VGA text mode does not exist) rendered with an 8x16
// bitmap font, or the legacy VGA text buffer at 0xB8000 on a BIOS boot. The
// same text grid drives both; ANSI escape sequences are stripped (clear and
// home are honoured) so the coloured command-surface output stays readable.
#include "font8x16.h"
namespace {
volatile uint16_t* const VGA = (volatile uint16_t*)0xB8000;
struct Framebuffer { volatile uint8_t* base = nullptr; uint32_t pitch = 0, width = 0, height = 0; uint8_t bpp = 0; } fb;
int cols = 80, rows = 25;
int vga_row = 0, vga_col = 0; bool vga_on = true;
const uint32_t DEFAULT_FG = 0x00D0D0D0, DEFAULT_BG = 0x00101418;
static const uint32_t ANSI_PALETTE[16] = {
  0x00181C20, // 0: Black (dark slate)
  0x00FF5555, // 1: Red
  0x0050FA7B, // 2: Green
  0x00F1FA8C, // 3: Yellow
  0x00BD93F9, // 4: Blue / Purple
  0x00FF79C6, // 5: Magenta
  0x008BE9FD, // 6: Cyan
  0x00D0D0D0, // 7: Light Gray
  0x006272A4, // 8: Bright Black / Gray
  0x00FF6E6E, // 9: Bright Red
  0x0069FF94, // 10: Bright Green
  0x00FFFFA5, // 11: Bright Yellow
  0x00D6ACFF, // 12: Bright Blue
  0x00FF92D0, // 13: Bright Magenta
  0x00A4FFFF, // 14: Bright Cyan
  0x00FFFFFF  // 15: Bright White
};

static uint32_t cur_fg = DEFAULT_FG;
static uint32_t cur_bg = DEFAULT_BG;
static bool cur_bold = false;
static uint8_t cur_vga_attr = 0x07;

void fb_pixel(uint32_t x, uint32_t y, uint32_t rgb) {
  volatile uint8_t* p = fb.base + (uint64_t)y * fb.pitch + (uint64_t)x * (fb.bpp / 8);
  if (fb.bpp == 32) *(volatile uint32_t*)p = rgb;
  else if (fb.bpp == 24) { p[0] = (uint8_t)rgb; p[1] = (uint8_t)(rgb >> 8); p[2] = (uint8_t)(rgb >> 16); }
  else if (fb.bpp == 16) *(volatile uint16_t*)p = (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}
const uint8_t* glyph_for(uint32_t cp) {
  if (cp >= 32 && cp < 127) return FONT8X16[cp - 32];
  for (auto& e : FONT_EXTRA) if (e.codepoint == cp) return e.rows;
  return FONT8X16['?' - 32];
}
void fb_glyph(int row, int col, uint32_t c) {
  const uint8_t* g = glyph_for(c);
  for (int y = 0; y < 16; ++y) for (int x = 0; x < 8; ++x) fb_pixel((uint32_t)(col * 8 + x), (uint32_t)(row * 16 + y), (g[y] & (0x80 >> x)) ? cur_fg : cur_bg);
}
void fb_fill_rows(int from_row, int to_row) {   // [from, to) rows of text cells → background
  uint32_t y0 = (uint32_t)from_row * 16, y1 = (uint32_t)to_row * 16;
  for (uint32_t y = y0; y < y1 && y < fb.height; ++y) for (uint32_t x = 0; x < fb.width; ++x) fb_pixel(x, y, cur_bg);
}
void fb_scroll() {
  uint64_t line = (uint64_t)fb.pitch * 16, total = (uint64_t)fb.pitch * (uint64_t)(rows * 16);
  volatile uint8_t* d = fb.base; const volatile uint8_t* sp = fb.base + line;
  for (uint64_t i = 0; i + line < total; i += 8) *(volatile uint64_t*)(d + i) = *(const volatile uint64_t*)(sp + i);
  fb_fill_rows(rows - 1, rows);
}
void cell(int row, int col, uint32_t c) { if (fb.base) fb_glyph(row, col, c); else VGA[row * 80 + col] = (uint16_t)(((uint16_t)cur_vga_attr << 8) | (c < 127 ? c : (c == 0xB7 ? 0xFA : c == 0x2192 ? 0x1A : '?'))); }
void cursor_show() {
  if (fb.base) { for (int x = 0; x < 8; ++x) fb_pixel((uint32_t)(vga_col * 8 + x), (uint32_t)(vga_row * 16 + 15), cur_fg); return; }
  uint16_t pos = (uint16_t)(vga_row * 80 + vga_col); outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)pos); outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)(pos >> 8));
}
void cursor_hide() { if (fb.base) cell(vga_row, vga_col, ' '); }
void display_clear() {
  if (fb.base) fb_fill_rows(0, rows); else for (int i = 0; i < 25 * 80; ++i) VGA[i] = (uint16_t)(((uint16_t)cur_vga_attr << 8) | 0x20);
  vga_row = vga_col = 0;
}
// ANSI CSI parser: ESC [ params final. Honours color SGR ("m"), "2J" (clear), "K" (line clear), "H" (home).
// UTF-8 is decoded so one character is one cell whatever its byte length.
int esc_state = 0; char esc_buf[16]; int esc_len = 0;
uint32_t utf_cp = 0; int utf_more = 0;
void put_cp(uint32_t cp);
void vga_put(char ch) {
  unsigned char b = (unsigned char)ch;
  if (utf_more) {
    if ((b & 0xC0) == 0x80) { utf_cp = (utf_cp << 6) | (b & 0x3F); if (--utf_more == 0) put_cp(utf_cp); return; }
    utf_more = 0; put_cp('?');   // malformed: fall through and treat this byte afresh
  }
  if (b >= 0xF0) { utf_cp = b & 0x07; utf_more = 3; return; }
  if (b >= 0xE0) { utf_cp = b & 0x0F; utf_more = 2; return; }
  if (b >= 0xC0) { utf_cp = b & 0x1F; utf_more = 1; return; }
  put_cp(b);
}
static void handle_sgr() {
  esc_buf[esc_len] = 0;
  if (esc_len == 0) {
    cur_fg = DEFAULT_FG; cur_bg = DEFAULT_BG; cur_bold = false; cur_vga_attr = 0x07;
    return;
  }
  const char* p = esc_buf;
  while (*p) {
    int code = 0;
    while (*p >= '0' && *p <= '9') { code = code * 10 + (*p - '0'); p++; }
    if (*p == ';') p++;
    if (code == 0) {
      cur_fg = DEFAULT_FG; cur_bg = DEFAULT_BG; cur_bold = false; cur_vga_attr = 0x07;
    } else if (code == 1) {
      cur_bold = true;
      cur_vga_attr |= 0x08;
    } else if (code >= 30 && code <= 37) {
      int idx = (code - 30) + (cur_bold ? 8 : 0);
      cur_fg = ANSI_PALETTE[idx];
      cur_vga_attr = (cur_vga_attr & 0xF0) | (uint8_t)((code - 30) | (cur_bold ? 8 : 0));
    } else if (code == 39) {
      cur_fg = DEFAULT_FG;
      cur_vga_attr = (cur_vga_attr & 0xF0) | 0x07;
    } else if (code >= 40 && code <= 47) {
      cur_bg = ANSI_PALETTE[code - 40];
      cur_vga_attr = (cur_vga_attr & 0x0F) | (uint8_t)((code - 40) << 4);
    } else if (code == 49) {
      cur_bg = DEFAULT_BG;
      cur_vga_attr &= 0x0F;
    } else if (code >= 90 && code <= 97) {
      int idx = (code - 90) + 8;
      cur_fg = ANSI_PALETTE[idx];
      cur_vga_attr = (cur_vga_attr & 0xF0) | (uint8_t)((code - 90) | 8);
    } else if (code >= 100 && code <= 107) {
      int idx = (code - 100) + 8;
      cur_bg = ANSI_PALETTE[idx];
      cur_vga_attr = (cur_vga_attr & 0x0F) | (uint8_t)(((code - 100) | 8) << 4);
    }
  }
}
void put_cp(uint32_t cp) {
  char c = cp < 128 ? (char)cp : 0;
  if (esc_state == 1) { if (c == '[') { esc_state = 2; esc_len = 0; } else esc_state = 0; return; }
  if (esc_state == 2) {
    if ((c >= '0' && c <= '9') || c == ';' || c == '?') { if (esc_len < 15) esc_buf[esc_len++] = c; return; }
    esc_state = 0;
    if (c == 'J') display_clear();
    else if (c == 'H') { vga_row = vga_col = 0; }
    else if (c == 'K') { for (int col = vga_col; col < cols; ++col) cell(vga_row, col, ' '); }
    else if (c == 'm') { handle_sgr(); }
    return;
  }
  if (c == 27) { esc_state = 1; return; }
  if (c == '\n') { vga_col = 0; vga_row++; }
  else if (c == '\r') { vga_col = 0; }
  else if (c == '\b') { if (vga_col > 0) { vga_col--; cell(vga_row, vga_col, ' '); } }
  else if (cp >= 32) { cell(vga_row, vga_col, cp); if (++vga_col >= cols) { vga_col = 0; vga_row++; } }
  if (vga_row >= rows) {
    if (fb.base) fb_scroll(); else { for (int i = 0; i < 24 * 80; ++i) VGA[i] = VGA[i + 80]; for (int i = 24 * 80; i < 25 * 80; ++i) VGA[i] = 0x0720; }
    vga_row = rows - 1;
  }
}
// ---- PS/2 controller: the keyboard on the first port, the mouse on AUX ------
// One 8042 serves both devices and has one output buffer. Bit 5 of the status
// register says which device the byte waiting there came from, so the only
// correct way to read it is to latch the status, read the data port, and route
// the byte by the status you latched — reading the data port without looking is
// how a mouse packet ends up interpreted as keystrokes. Everything here is
// polled: no IDT is installed on this substrate yet, so IRQ1/IRQ12 stay masked
// and the console loop (or the desktop's frame loop) drains the buffer instead.
constexpr uint16_t PS2_DATA = 0x60, PS2_CMD = 0x64;
constexpr uint8_t PS2_ST_OUTPUT_FULL = 0x01, PS2_ST_INPUT_FULL = 0x02, PS2_ST_FROM_AUX = 0x20;

inline void io_wait() { outb(0x80, 0); }   // an unused port: one bus cycle of delay
bool ps2_wait_writable() { for (int i = 0; i < 200000; ++i) { if (!(inb(PS2_CMD) & PS2_ST_INPUT_FULL)) return true; io_wait(); } return false; }
void ps2_command(uint8_t c) { ps2_wait_writable(); outb(PS2_CMD, c); }
void ps2_write(uint8_t d) { ps2_wait_writable(); outb(PS2_DATA, d); }

// PS/2 scancode set 1, US layout.
const char KEYMAP[128] = { 0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b', '\t', 'q','w','e','r','t','y','u','i','o','p','[',']', '\n', 0, 'a','s','d','f','g','h','j','k','l',';','\'','`', 0, '\\', 'z','x','c','v','b','n','m',',','.','/', 0, '*', 0, ' ' };
const char KEYMAP_SHIFT[128] = { 0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b', '\t', 'Q','W','E','R','T','Y','U','I','O','P','{','}', '\n', 0, 'A','S','D','F','G','H','J','K','L',':','"','~', 0, '|', 'Z','X','C','V','B','N','M','<','>','?', 0, '*', 0, ' ' };
bool shift_down = false;
bool ctrl_down = false;
bool alt_down = false;
static bool e0_prefix = false;

// Keys queued for whoever asks next. A ring, because one PS/2 pump feeds both
// the text console (which wants characters) and the desktop (which wants every
// key, arrows and function keys included).
// Anything outside interrupt context takes the line down for the few
// instructions it needs, because the ring below now has a producer that can
// interrupt it.
struct IrqOff {
  uint64_t flags;
  IrqOff() { asm volatile("pushfq; popq %0; cli" : "=r"(flags)); }
  ~IrqOff() { if (flags & 0x200) asm volatile("sti"); }
};

constexpr int KEY_RING = 64;
int key_ring[KEY_RING]; int key_head = 0, key_tail = 0;
void key_push(int k) { int n = (key_head + 1) % KEY_RING; if (n != key_tail) { key_ring[key_head] = k; key_head = n; } }
int key_pop() { IrqOff off; if (key_head == key_tail) return -1; int k = key_ring[key_tail]; key_tail = (key_tail + 1) % KEY_RING; return k; }

// The mouse. Absolute position is kept here rather than in the compositor: the
// hardware reports movement, and the thing that knows the screen's size is the
// thing that owns the framebuffer.
struct MouseState {
  int x = 0, y = 0;
  uint8_t buttons = 0;          // bit 0 left, bit 1 right, bit 2 middle
  int wheel = 0;                // notches accumulated since the last read, + is up
  bool present = false;
  int packet_bytes = 3;         // 4 once the wheel is negotiated
  uint8_t packet[4] = {}; int have = 0;
  uint32_t bound_w = 640, bound_h = 480;
} mouse;

// The next byte the AUX device sent. Keyboard bytes that arrive in between are
// pushed to the key ring instead of being thrown away: initialisation happens
// while the firmware's leftovers may still be in the queue.
int aux_read(int spins = 400000);
void keyboard_scancode(uint8_t sc);
int aux_read(int spins) {
  for (int i = 0; i < spins; ++i) {
    uint8_t st = inb(PS2_CMD);
    if (!(st & PS2_ST_OUTPUT_FULL)) { io_wait(); continue; }
    uint8_t v = inb(PS2_DATA);
    if (st & PS2_ST_FROM_AUX) return v;
    keyboard_scancode(v);
  }
  return -1;
}
// 0xD4 tells the controller the next byte it is written goes to the AUX device.
int aux_command(uint8_t cmd) { ps2_command(0xD4); ps2_write(cmd); return aux_read(); }   // 0xFA = ACK

void mouse_clamp() {
  if (mouse.x < 0) mouse.x = 0; if (mouse.y < 0) mouse.y = 0;
  if (mouse.bound_w && mouse.x > (int)mouse.bound_w - 1) mouse.x = (int)mouse.bound_w - 1;
  if (mouse.bound_h && mouse.y > (int)mouse.bound_h - 1) mouse.y = (int)mouse.bound_h - 1;
}

// One movement packet: [flags, dx, dy] and, on an IntelliMouse, [.., dz].
void mouse_byte(uint8_t b) {
  if (mouse.have == 0 && !(b & 0x08)) return;   // bit 3 of byte 0 is always set: resync on it
  mouse.packet[mouse.have++] = b;
  if (mouse.have < mouse.packet_bytes) return;
  mouse.have = 0;
  uint8_t f = mouse.packet[0];
  if (f & 0xC0) return;                          // X or Y overflowed: the packet is junk
  int dx = (int)mouse.packet[1] - ((f & 0x10) ? 256 : 0);
  int dy = (int)mouse.packet[2] - ((f & 0x20) ? 256 : 0);
  if (mouse.packet_bytes == 4) {
    int z = mouse.packet[3] & 0x0F;
    if (z & 0x08) z -= 16;                       // 4-bit signed
    mouse.wheel -= z;                            // the wire counts down-positive; we hand up-positive out
  }
  mouse.x += dx;
  mouse.y -= dy;                                 // PS/2 counts Y upwards, the screen downwards
  mouse_clamp();
  mouse.buttons = (uint8_t)(f & 0x07);
}

void keyboard_scancode(uint8_t sc) {
  if (sc == 0xE0) { e0_prefix = true; return; }
  if (e0_prefix) {
    e0_prefix = false;
    if (sc & 0x80) return;                        // extended release
    switch (sc) {
      case 0x48: key_push(aios::hal::KeyUp); return;
      case 0x50: key_push(aios::hal::KeyDown); return;
      case 0x4B: key_push(aios::hal::KeyLeft); return;
      case 0x4D: key_push(aios::hal::KeyRight); return;
      case 0x47: key_push(aios::hal::KeyHome); return;
      case 0x4F: key_push(aios::hal::KeyEnd); return;
      case 0x49: key_push(aios::hal::KeyPageUp); return;
      case 0x51: key_push(aios::hal::KeyPageDown); return;
      case 0x53: key_push(aios::hal::KeyDelete); return;
      case 0x1C: key_push(aios::hal::KeyEnter); return;   // the keypad's Enter
      default: return;
    }
  }
  if (sc == 0x2A || sc == 0x36) { shift_down = true; return; }
  if (sc == 0xAA || sc == 0xB6) { shift_down = false; return; }
  // Ctrl and Alt were not tracked at all, so this machine had no copy, no
  // paste, and no way to move by the word — the keys simply produced letters.
  if (sc == 0x1D) { ctrl_down = true; return; }
  if (sc == 0x9D) { ctrl_down = false; return; }
  if (sc == 0x38) { alt_down = true; return; }
  if (sc == 0xB8) { alt_down = false; return; }
  if (sc & 0x80) return;                          // key release
  // Function keys and the keypad arrows (which arrive without the E0 prefix
  // when Num Lock is off) have no ASCII to be, so they are named keys.
  switch (sc) {
    // Enter, Tab, Backspace and Escape are named keys, not characters. The
    // keymap gives Enter '\n', which a text console is happy with and a
    // desktop is not: it has to tell "submit this line" apart from a byte.
    case 0x1C: key_push(aios::hal::KeyEnter); return;
    case 0x01: key_push(aios::hal::KeyEscape); return;
    case 0x0E: key_push(aios::hal::KeyBackspace); return;
    case 0x0F: key_push(aios::hal::KeyTab); return;
    case 0x3B: key_push(aios::hal::KeyF1); return;
    case 0x3C: key_push(aios::hal::KeyF2); return;
    case 0x3D: key_push(aios::hal::KeyF3); return;
    case 0x3E: key_push(aios::hal::KeyF4); return;
    case 0x3F: key_push(aios::hal::KeyF5); return;
    case 0x40: key_push(aios::hal::KeyF6); return;
    case 0x41: key_push(aios::hal::KeyF7); return;
    case 0x42: key_push(aios::hal::KeyF8); return;
    case 0x43: key_push(aios::hal::KeyF9); return;
    case 0x44: key_push(aios::hal::KeyF10); return;
    case 0x48: key_push(aios::hal::KeyUp); return;
    case 0x50: key_push(aios::hal::KeyDown); return;
    case 0x4B: key_push(aios::hal::KeyLeft); return;
    case 0x4D: key_push(aios::hal::KeyRight); return;
    default: break;
  }
  char c = shift_down ? KEYMAP_SHIFT[sc & 0x7F] : KEYMAP[sc & 0x7F];
  if (!c) return;
  // Ctrl+<letter> is the ASCII control code that letter names, which is what
  // every other input backend here sends and what the desktop's copy, cut,
  // paste and line-kill handlers read.
  if (ctrl_down && c >= 'a' && c <= 'z') { key_push(c - 'a' + 1); return; }
  if (ctrl_down && c >= 'A' && c <= 'Z') { key_push(c - 'A' + 1); return; }
  key_push((unsigned char)c);
}

// The controller now raises IRQ1 and IRQ12, so this drain runs from an
// interrupt as well as from whoever polls — and the ring it feeds has one
// consumer that must not be interrupted mid-update. Anything outside interrupt
// context takes the line down for the few instructions it needs.
void ps2_drain() {
  for (int guard = 0; guard < 128; ++guard) {
    uint8_t st = inb(PS2_CMD);
    if (!(st & PS2_ST_OUTPUT_FULL)) return;
    uint8_t b = inb(PS2_DATA);
    if (st & PS2_ST_FROM_AUX) mouse_byte(b); else keyboard_scancode(b);
  }
}

// Drain whatever the controller has buffered, routing each byte to its device.
void ps2_pump() { IrqOff off; ps2_drain(); }

// Bring the AUX port and the mouse on it up. Bounds are the screen's, because
// the cursor cannot leave it.
bool ps2_mouse_init(uint32_t bound_w, uint32_t bound_h) {
  // The whole handshake below is polled: it writes a byte and waits for the
  // device's reply on the data port. Now that the controller raises IRQ12, an
  // interrupt handler would drain those replies before this loop ever saw them
  // and every step would look like it failed — so the line stays down until the
  // mouse is up, and whatever queued meanwhile is drained when it comes back.
  IrqOff off;
  mouse.bound_w = bound_w; mouse.bound_h = bound_h;
  mouse.x = (int)bound_w / 2; mouse.y = (int)bound_h / 2;
  mouse.have = 0; mouse.wheel = 0; mouse.buttons = 0;
  ps2_command(0xA8);                                   // enable the AUX port
  // The controller's configuration byte. It answers on the first port, not AUX,
  // so it is read straight off the data port rather than through aux_read.
  ps2_command(0x20);
  int conf = -1;
  for (int i = 0; i < 200000; ++i) { uint8_t st = inb(PS2_CMD); if (st & PS2_ST_OUTPUT_FULL) { conf = inb(PS2_DATA); break; } io_wait(); }
  if (conf >= 0) {
    conf &= ~0x20;                                     // clear "AUX clock disabled"
    conf |= 0x03;                                      // raise IRQ1 and IRQ12: there is an IDT now
    ps2_command(0x60); ps2_write((uint8_t)conf);
  }
  if (aux_command(0xF6) != 0xFA) return false;         // restore defaults
  // The IntelliMouse knock: three sample-rate writes in this exact order make a
  // wheel mouse switch to 4-byte packets, and asking for its id afterwards says
  // whether it did (id 3) or is a plain 3-byte mouse (id 0).
  aux_command(0xF3); aux_command(200);
  aux_command(0xF3); aux_command(100);
  aux_command(0xF3); aux_command(80);
  aux_command(0xF2);
  int id = aux_read(200000);
  mouse.packet_bytes = 3;
  aux_command(0xF3); aux_command(100);                 // 100 samples/s: smooth without flooding a polled loop
  if (aux_command(0xF4) != 0xFA) return false;         // start streaming movement
  mouse.present = true;
  return true;
}

// The text console wants characters. Named keys (arrows, function keys) are
// meaningless to it and are dropped here; the desktop reads them with
// metal_key_poll instead.
int keyboard_read() {
  ps2_pump();
  for (;;) { int k = key_pop(); if (k < 0) return -1; if (!aios::hal::key_is_special(k)) return k; }
}
}  // namespace

// The loader's framebuffer (Multiboot2 tag 8). Type 1 = direct RGB; anything
// else (indexed, EGA text) is left to the VGA path.
// The display adapter's own mode registers, so the KERNEL decides the
// resolution instead of inheriting whatever the firmware's GOP happened to
// offer. Under OVMF that list is 640x480, 800x600 and 1024x768 and nothing
// else, which is why a 32bpp truecolour desktop kept coming up at 1024x768 no
// matter what the loader was asked for.
//
// This is the Bochs DISPI interface: QEMU's stdvga (1234:1111), Bochs, and
// VirtualBox (80ee:beef) all implement it, through one index/data port pair.
// The framebuffer stays where the adapter's BAR0 already put it — the same
// memory the loader handed us — so only the geometry changes.
namespace {
constexpr uint16_t DISPI_INDEX = 0x01CE, DISPI_DATA = 0x01CF;
constexpr uint16_t DISPI_ID = 0, DISPI_XRES = 1, DISPI_YRES = 2, DISPI_BPP = 3,
                   DISPI_ENABLE = 4, DISPI_VIRT_WIDTH = 6, DISPI_X_OFF = 8, DISPI_Y_OFF = 9;
constexpr uint16_t DISPI_DISABLED = 0x00, DISPI_ENABLED = 0x01, DISPI_LFB = 0x40;

uint16_t dispi_read(uint16_t index) { outw(DISPI_INDEX, index); return inw(DISPI_DATA); }
void dispi_write(uint16_t index, uint16_t value) { outw(DISPI_INDEX, index); outw(DISPI_DATA, value); }

bool dispi_present() {
  uint16_t id = dispi_read(DISPI_ID);
  return id >= 0xB0C0 && id <= 0xB0C5;
}

// Ask for one mode. Returns the geometry the adapter actually took, which is
// the only thing worth believing: a mode it cannot do is refused in the
// readback, not in a return code.
bool dispi_set(uint32_t w, uint32_t h, uint32_t* got_w, uint32_t* got_h) {
  dispi_write(DISPI_ENABLE, DISPI_DISABLED);
  dispi_write(DISPI_XRES, (uint16_t)w);
  dispi_write(DISPI_YRES, (uint16_t)h);
  dispi_write(DISPI_BPP, 32);
  dispi_write(DISPI_VIRT_WIDTH, (uint16_t)w);
  dispi_write(DISPI_X_OFF, 0);
  dispi_write(DISPI_Y_OFF, 0);
  dispi_write(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB);
  *got_w = dispi_read(DISPI_XRES);
  *got_h = dispi_read(DISPI_YRES);
  return *got_w == w && *got_h == h && dispi_read(DISPI_BPP) == 32;
}
}  // namespace

// The best mode this adapter will take, tried largest first, and only if it
// needs no more video memory than the adapter has. Called once the loader's
// framebuffer is known, so a failure leaves that mode untouched.
extern "C" bool metal_display_best_mode(uint32_t* out_w, uint32_t* out_h, uint32_t* out_pitch) {
  if (!fb.base || fb.bpp != 32 || !dispi_present()) return false;
  struct Mode { uint32_t w, h; };
  static const Mode WANTED[] = {{1920, 1080}, {1600, 900}, {1440, 900}, {1280, 1024}, {1280, 800}};
  // The adapter's memory is whatever its BAR mapped; QEMU's stdvga carries
  // 16 MiB by default and 1920x1080x32 needs 8.3 MiB. Anything we cannot
  // prove fits is not attempted.
  const uint64_t budget = 16ull << 20;
  for (const auto& m : WANTED) {
    if ((uint64_t)m.w * m.h * 4ull > budget) continue;
    if (m.w <= fb.width && m.h <= fb.height) break;      // the loader already did better
    uint32_t gw = 0, gh = 0;
    if (!dispi_set(m.w, m.h, &gw, &gh)) continue;
    fb.width = gw;
    fb.height = gh;
    fb.pitch = gw * 4;
    g_kernel_set_mode = true;
    // The text console shares this framebuffer, so its grid moves with it.
    cols = (int)(gw / 8);
    rows = (int)(gh / 16);
    if (cols > 250) cols = 250;
    display_clear();
    if (out_w) *out_w = gw;
    if (out_h) *out_h = gh;
    if (out_pitch) *out_pitch = fb.pitch;
    return true;
  }
  return false;
}

// A framebuffer the device does not scan out of by itself needs telling when a
// frame is done. Null for a plain linear framebuffer, which the CRTC reads on
// its own; set for virtio-gpu, whose scanout is a resource that has to be
// transferred and flushed.
namespace { void (*g_present)() = nullptr; }

extern "C" void metal_display_present() { if (g_present) g_present(); }

// Install a framebuffer this kernel obtained for itself, rather than one the
// loader handed over.
extern "C" void metal_display_adopt(uint64_t addr, uint32_t pitch, uint32_t width, uint32_t height, void (*present)()) {
  if (!addr || width < 8 || height < 16) return;
  fb.base = (volatile uint8_t*)(uintptr_t)addr;
  fb.pitch = pitch;
  fb.width = width;
  fb.height = height;
  fb.bpp = 32;
  g_present = present;
  cols = (int)(width / 8);
  rows = (int)(height / 16);
  if (cols > 250) cols = 250;
  display_clear();
  metal_display_present();
}

extern "C" void metal_display_init(uint64_t addr, uint32_t pitch, uint32_t width, uint32_t height, uint8_t bpp, uint8_t type) {
  if (type != 1 || !addr || (bpp != 32 && bpp != 24 && bpp != 16) || width < 8 || height < 16) return;
  fb.base = (volatile uint8_t*)(uintptr_t)addr; fb.pitch = pitch; fb.width = width; fb.height = height; fb.bpp = bpp;
  cols = (int)(width / 8); rows = (int)(height / 16);
  if (cols > 250) cols = 250;
}
extern "C" bool metal_display_describe(uint32_t* width, uint32_t* height, uint32_t* bpp, uint32_t* text_cols, uint32_t* text_rows) {
  if (width) *width = fb.width; if (height) *height = fb.height; if (bpp) *bpp = fb.bpp; if (text_cols) *text_cols = (uint32_t)cols; if (text_rows) *text_rows = (uint32_t)rows;
  return fb.base != nullptr;
}
extern "C" void metal_vga_clear() { display_clear(); cursor_show(); }
extern "C" void metal_display_write(const char* s, size_t n) {
  if (!vga_on) return;
  cursor_hide();
  for (size_t i = 0; i < n; ++i) vga_put(s[i]);
  cursor_show();
  // One flush for the whole write: a scanout that has to be pushed cannot
  // afford a round trip per character.
  metal_display_present();
}
// One character from serial or keyboard, or -1.
extern "C" int metal_console_getc() { int c = metal_console_read(); if (c >= 0) return c; return keyboard_read(); }

// ---- the screen and the input devices, as the desktop uses them -------------
// The text console and the graphical desktop share one framebuffer, so the
// desktop takes it: graphics mode stops metal_console_write from drawing text
// over the composited frame (serial keeps every byte either way, which is how
// a headless drive still sees what the machine is saying).
extern "C" void metal_display_graphics(bool on) { vga_on = !on; }
extern "C" bool metal_display_graphics_on() { return !vga_on; }

// One composited ARGB frame onto the real display memory. 32bpp is the common
// case and gets a straight row copy; 24 and 16 go through the pixel packer.
extern "C" void metal_display_blit(const uint32_t* argb, uint32_t w, uint32_t h) {
  if (!fb.base || !argb || !w || !h) return;
  uint32_t cw = w < fb.width ? w : fb.width;
  uint32_t ch = h < fb.height ? h : fb.height;
  if (fb.bpp == 32) {
    if (fb.pitch == cw * 4 && w == cw) {
      memcpy((void*)fb.base, argb, (size_t)cw * ch * 4);
    } else {
      for (uint32_t y = 0; y < ch; ++y) {
        memcpy((void*)(fb.base + (uint64_t)y * fb.pitch), argb + (uint64_t)y * w, (size_t)cw * 4);
      }
    }
    metal_display_present();
    return;
  }
  for (uint32_t y = 0; y < ch; ++y)
    for (uint32_t x = 0; x < cw; ++x) fb_pixel(x, y, argb[(uint64_t)y * w + x] & 0x00FFFFFFu);
  metal_display_present();
}

// What the keyboard and mouse interrupts call. Already in interrupt context:
// the line is down, so no guard and no re-entry.
extern "C" void metal_ps2_isr() { ps2_drain(); }

// Let the controller raise IRQ1 and IRQ12 even where no mouse is brought up —
// a serial-only console still wants its keyboard to arrive by itself.
extern "C" void metal_ps2_irq_enable() {
  IrqOff off;
  ps2_command(0x20);
  for (int i = 0; i < 200000; ++i) {
    uint8_t st = inb(PS2_CMD);
    if (st & PS2_ST_OUTPUT_FULL) {
      int conf = inb(PS2_DATA);
      conf |= 0x01;                                    // IRQ1: the keyboard
      ps2_command(0x60); ps2_write((uint8_t)conf);
      return;
    }
    io_wait();
  }
}

// Bring the PS/2 mouse up, bounded to the screen it will be drawn on.
extern "C" bool metal_input_init(uint32_t bound_w, uint32_t bound_h) { return ps2_mouse_init(bound_w, bound_h); }
extern "C" bool metal_mouse_present() { return mouse.present; }
// The mouse as it is right now, plus the wheel notches since the last call
// (which are consumed: a notch is an event, not a state).
extern "C" bool metal_mouse_poll(int* x, int* y, int* buttons, int* wheel) {
  ps2_pump();
  if (x) *x = mouse.x;
  if (y) *y = mouse.y;
  if (buttons) *buttons = mouse.buttons;
  if (wheel) { *wheel = mouse.wheel; mouse.wheel = 0; }
  return mouse.present;
}
extern "C" void metal_mouse_set_bounds(uint32_t w, uint32_t h) { mouse.bound_w = w; mouse.bound_h = h; mouse_clamp(); }
// Every key, named ones included — what a desktop needs and the text console
// does not. Serial bytes come through here too, so a headless drive can type at
// the desktop exactly as a person at the keyboard does.
extern "C" int metal_key_poll() {
  int c = metal_console_read();
  if (c >= 0) return c == '\n' ? (int)aios::hal::KeyEnter : c;
  ps2_pump();
  return key_pop();
}
// Whether the "by the word" modifier is held right now. Alt+Left and Left are
// the same key with the same code, so the desktop has to ask.
extern "C" bool metal_key_word_modifier() { return alt_down; }
extern "C" uint64_t metal_monotonic_ms() { return tsc_hz ? rdtsc() / (tsc_hz / 1000ull) : 0; }
// Seconds of wall clock, the same answer MetalHost::wall_ns gives.
extern "C" uint64_t metal_wall_seconds() {
  int64_t ns = 0;
  if (metal_wall_unix_ns(&ns) && ns > 0) return (uint64_t)ns / 1000000000ull;
  return 1788393600ull + (tsc_hz ? rdtsc() / tsc_hz : 0);
}

// Sound. This machine has no audio driver yet — no HDA, no AC97, nothing on
// the bus that has been claimed — and no speech engine to reach. Saying so is
// the whole contribution: the layer above used to answer a sine wave and a
// canned sentence, and a caller could not tell that from a working microphone.
#include "aios/hal/audio.hpp"
namespace aios::hal {
Result<std::vector<AudioEndpoint>> audio_endpoints() { return std::vector<AudioEndpoint>{}; }
Result<std::vector<int16_t>> audio_capture(uint32_t, size_t, uint32_t) {
  return Result<std::vector<int16_t>>(Status::Unsupported, "no audio input driver on this machine");
}
Result<void> audio_play(const int16_t*, size_t, uint32_t) {
  return Result<void>(Status::Unsupported, "no audio output driver on this machine");
}
Result<std::vector<SpeechVoice>> speech_voices() {
  return Result<std::vector<SpeechVoice>>(Status::Unsupported, "no speech synthesizer on this machine");
}
Result<SpeechAudio> speech_synthesize(const std::string&, const std::string&, uint32_t, uint32_t) {
  return Result<SpeechAudio>(Status::Unsupported, "no speech synthesizer on this machine");
}
Result<std::string> speech_recognizer() {
  return Result<std::string>(Status::Unsupported, "no speech recogniser on this machine");
}
Result<SpeechTranscript> speech_recognize(const int16_t*, size_t, uint32_t, const std::string&) {
  return Result<SpeechTranscript>(Status::Unsupported, "no speech recogniser on this machine");
}
}  // namespace aios::hal

// The TPM. This machine has no TPM driver: nothing on its buses has been
// claimed as one, and there is no software TPM to reach either. The attestor
// above produces no quote here, which is the honest answer.
#include "aios/hal/tpm.hpp"
namespace aios::hal {
Result<std::string> tpm_open() {
  return Result<std::string>(Status::NotFound, "no TPM driver on this machine");
}
std::string tpm_where() { return {}; }
Result<std::vector<uint8_t>> tpm_transmit(const std::vector<uint8_t>&, uint32_t) {
  return Result<std::vector<uint8_t>>(Status::NotFound, "no TPM is open");
}
void tpm_close() {}
}  // namespace aios::hal

// The clipboard, on a machine that is the only program on the display. There
// is nobody else to share it with, so the machine keeps its own and copy and
// paste work inside it exactly as they do on a hosted substrate.
#include "aios/hal/clipboard.hpp"
namespace aios::hal {
namespace {
Mutex& metal_clip_mutex() { static Mutex m; return m; }
std::string& metal_clipboard() { static std::string s; return s; }
}  // namespace
Result<std::string> clipboard_get() {
  LockGuard l(metal_clip_mutex());
  return metal_clipboard();
}
Result<void> clipboard_set(const std::string& text) {
  LockGuard l(metal_clip_mutex());
  metal_clipboard() = text;
  return Ok();
}
}  // namespace aios::hal

// The service manager's process boundary. This machine runs programs — see
// metal/process.cpp — but it runs ELF images it already holds, named by the
// machine's own filesystem, not command lines resolved against a PATH. Until
// the service manager can hand this layer an image, saying so is the honest
// answer; inventing a pid would leave a supervisor watching nothing.
#include "aios/hal/process.hpp"
namespace aios::hal {
Result<ProcessHandle> spawn_process(const ProcessSpawn&) {
  return Result<ProcessHandle>(Status::Unsupported,
      "bare metal runs ELF images through process_spawn, not command lines; a service unit must name an image");
}
Result<ProcessStatus> process_status(ProcessHandle) {
  return Result<ProcessStatus>(Status::NotFound, "no supervised processes on this substrate");
}
Result<void> signal_process(ProcessHandle, ProcessSignal) {
  return Result<void>(Status::NotFound, "no supervised processes on this substrate");
}
Result<void> release_process(ProcessHandle) {
  return Result<void>(Status::NotFound, "no supervised processes on this substrate");
}
}  // namespace aios::hal

// Intelligence adapters that need processes or dynamic loading do not exist on
// this substrate. An out-of-kernel mind reaches a bare-metal kernel as a peer
// node once the network lands; plugins are linked statically.
#include "aios/intelligence/intelligence.hpp"
namespace aios {
Result<std::unique_ptr<Intelligence>> load_intelligence_plugin(const std::string&) { return Result<std::unique_ptr<Intelligence>>(Status::Unsupported, "no dynamic loading on bare metal; link the mind statically"); }
Result<std::unique_ptr<Intelligence>> spawn_external_intelligence(const std::vector<std::string>&, uint64_t) { return Result<std::unique_ptr<Intelligence>>(Status::Unsupported, "no processes on bare metal; an external mind attaches as a peer node"); }
}
