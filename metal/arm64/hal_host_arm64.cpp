/* aios::hal::Host for ARM64 bare metal - the minimum src/fleet/relay.cpp
 * actually calls (monotonic_ns(), random()) implemented for real; the rest
 * of the interface answers honestly (Unsupported/empty) rather than being
 * left unimplemented, faked, or made to panic a caller that has every
 * right to ask "does this substrate have directories?" and get a real
 * answer instead of a crash. File I/O routes to the real FAT32 driver
 * where there is one to route to - so it starts working the day storage
 * does, without anyone having to remember to come back and wire it up. */
#include "aios/hal/host.hpp"
#include "aios/hal/process.hpp"
#include "fat32.hpp"
#include "kio.hpp"
#include "mmu.hpp"
#include "net.hpp"
#include "lan9514.hpp"
#include "sdcard.hpp"
#include "timer.hpp"
extern "C" bool metal_wall_unix_ns(int64_t* out);
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

extern "C" uint64_t metal_ticks();
extern "C" uint32_t metal_tick_hz();

void net_breadcrumb(const char* msg);   // net.cpp: a line to the LAN log (udp/42425)
namespace aios::hal {

namespace {
/* Kernel::boot()'s own bookkeeping (kernel.identity, the authority ledger)
 * needs SOMEWHERE that write_file() then read_file() agrees with itself -
 * routing it through fat32_write_file()/fat32_read_file() made that
 * "somewhere" the physical SD card, and fat32_write_file() only ever
 * returns false for one reason: !g_fs.mounted. This board's SD card is
 * not reliably mounting at the point Kernel::boot() runs (real hardware:
 * confirmed halting on "fat32_write_file failed for /aios/root/
 * kernel.identity" the first time this path was ever exercised) - and
 * fixing THAT is a real, separate driver question, not something to paper
 * over by loosening fat32.cpp's honest mount gate (other commands - `ls`,
 * `sd` - correctly depend on it telling the truth about whether a card is
 * actually there).
 *
 * kernel_boot.cpp already documents this board as having no persistent
 * volume yet ("in-memory: no AOS volume/virtio disk on this board yet") -
 * so this is that promise, kept: a small in-memory table, entirely
 * separate from fat32.cpp's own g_dynamic_files (that one is real SD
 * bookkeeping and must stay honest about mount state), that always works
 * regardless of what the SD card is doing. Kernel state does not survive a
 * reboot yet - except the kernel's identity, which is_identity() below keeps
 * on the card. */
/* The files, kept whole in memory and - once the card is mounted - on it too.
 * The card's FAT32 takes 8.3 names only, so each path gets a short name
 * (A + 7 hex digits + .DAT) and AIOSFS.IDX keeps "SHORTNAME<TAB>path" for
 * every one. A file written before the card mounted is written out when it
 * does. Before this, the kernel's files were 32 in-memory slots of 64 KB:
 * the SQL engine's redo log outgrew a slot, and nothing - its tables, its
 * Things - survived a reboot. */
struct StoredFile {
  std::vector<uint8_t> data;
  bool loaded = false;      // data holds the file (else it is only on the card)
  bool on_card = false;     // the card has this content
};
std::map<std::string, StoredFile> g_files;
std::map<std::string, std::string> g_card_name;   // path -> 8.3 name
std::map<std::string, std::string> g_card_path;   // 8.3 name -> path
bool g_index_loaded = false;
constexpr const char* kIndexFile = "AIOSFS.IDX";

bool card_ready() { return fat32_status().mounted; }

void save_index() {
  std::string text;
  for (const auto& kv : g_card_name) text += kv.second + "\t" + kv.first + "\n";
  if (!fat32_write_file(kIndexFile, text.data(), text.size())) kputs("[host] could not write AIOSFS.IDX\n");
}

std::string short_name_for(const std::string& path) {
  auto it = g_card_name.find(path);
  if (it != g_card_name.end()) return it->second;
  uint32_t h = 2166136261u;
  for (unsigned char c : path) { h ^= c; h *= 16777619u; }
  static const char* hex = "0123456789ABCDEF";
  for (;;) {
    char n[13] = "A0000000.DAT";
    uint32_t v = h & 0x0FFFFFFFu;
    for (int i = 7; i >= 1; --i) { n[i] = hex[v & 15]; v >>= 4; }
    if (!g_card_path.count(n)) {
      g_card_name[path] = n;
      g_card_path[n] = path;
      save_index();
      return n;
    }
    h = h * 31u + 7u;   // taken by another path: the next name
  }
}

void flush_to_card(const std::string& path, StoredFile& f) {
  if (!card_ready() || f.on_card || !f.loaded) return;
  const std::string n = short_name_for(path);
  if (fat32_write_file(n.c_str(), f.data.data(), f.data.size())) f.on_card = true;
  else kputs("[host] a file could not be written to the card - it lasts until reboot\n");
}

// Once the card is there: what it already holds, and what was written before it mounted.
void sync_with_card() {
  if (g_index_loaded || !card_ready()) return;
  g_index_loaded = true;
  size_t size = 0, got = 0;
  if (fat32_file_size(kIndexFile, size) && size > 0) {
    std::vector<char> buf(size);
    if (fat32_read_file(kIndexFile, buf.data(), size, got) && got == size) {
      std::string text(buf.begin(), buf.end());
      size_t at = 0;
      while (at < text.size()) {
        size_t nl = text.find('\n', at);
        std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? text.size() : nl + 1;
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string n = line.substr(0, tab), path = line.substr(tab + 1);
        g_card_name[path] = n;
        g_card_path[n] = path;
        auto& f = g_files[path];
        if (!f.loaded) f.on_card = true;   // a newer in-memory copy wins below
      }
    }
  }
  for (auto& kv : g_files) if (kv.second.loaded && !kv.second.on_card) flush_to_card(kv.first, kv.second);
}

StoredFile* find_file(const std::string& path) {
  sync_with_card();
  auto it = g_files.find(path);
  if (it == g_files.end()) return nullptr;
  StoredFile& f = it->second;
  if (!f.loaded && f.on_card && card_ready()) {
    auto n = g_card_name.find(path);
    size_t size = 0, got = 0;
    if (n != g_card_name.end() && fat32_file_size(n->second.c_str(), size)) {
      f.data.resize(size);
      if (size == 0 || (fat32_read_file(n->second.c_str(), f.data.data(), size, got) && got == size)) f.loaded = true;
    }
  }
  return f.loaded ? &f : nullptr;
}

/* One exception to the in-memory root: the kernel's identity lives on the
 * card (KERNELID.BIN, 8.3) when the card is mounted - a board that got a new
 * identity every boot was a new stranger to the fleet every boot, and every
 * peer that remembered the old one kept a dead entry for it. */
bool is_identity(const std::string& path) {
  static const std::string tail = "/kernel.identity";
  return path.size() >= tail.size() && path.compare(path.size() - tail.size(), tail.size(), tail) == 0;
}
constexpr const char* kIdentityFile = "KERNELID.BIN";
}  // namespace

#include "aios/hal/parse_double_portable.inc"
double parse_double(const char* s, char** end) { return aios_parse_double_portable(s, end); }
void host_note(const std::string& text) {
  kputs(text.c_str());
  kputs("\n");
  // and to the LAN log (udp/42425), where the board can be watched without
  // its serial console: link state, fleet notes
  std::string line = text;
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  if (!line.empty()) net_breadcrumb(line.c_str());
}

void release_idle_stack() {}
void sleep_ms(uint32_t ms) {
  uint32_t hz = metal_tick_hz(); if (!hz) hz = 100;
  uint64_t ticks_needed = (static_cast<uint64_t>(ms) * hz) / 1000;
  if (ticks_needed == 0) ticks_needed = 1;
  uint64_t start = metal_ticks();
  while (metal_ticks() - start < ticks_needed) asm volatile("yield");
}

namespace {

class Arm64Host : public Host {
 public:
  // Memory for the memory fabric (tensors a fleet job stages here, a
  // channel's region): the board's heap, zeroed, 16-byte aligned; blocks of
  // 256 KiB and up come from the page allocator and go back to it on unmap,
  // so job after job does not grow the board. One address space: Private and
  // Shared are the same thing here. Persistent is backed by the card store -
  // re-mapping a path brings its contents back, sync() writes them out.
  Result<Mapping> map(uint64_t size, MappingKind kind, const std::string& path) override {
    if (size == 0) return Result<Mapping>(Status::Invalid, "zero-size mapping");
    void* p = std::malloc((size_t)size);
    if (!p) return Result<Mapping>(Status::Exhausted, "the board is out of memory for " + std::to_string(size) + " bytes");
    std::memset(p, 0, (size_t)size);
    Mapping m;
    m.base = p;
    m.size = size;
    m.kind = kind;
    m.path = path;
    if (kind == MappingKind::Persistent && !path.empty()) {
      auto f = read_file(path);
      if (f.ok()) std::memcpy(p, f->data(), std::min<size_t>(f->size(), (size_t)size));
    }
    return m;
  }
  void unmap(Mapping& m) override {
    if (m.base) std::free(m.base);
    m.base = nullptr;
    m.size = 0;
  }
  Result<void> sync(const Mapping& m) override {
    if (m.kind != MappingKind::Persistent || m.path.empty() || !m.base) return Ok();
    return write_file(m.path, ByteSpan(static_cast<const uint8_t*>(m.base), (size_t)m.size));
  }

  Result<void> write_file(const std::string& path, ByteSpan data) override {
    if (is_identity(path) && card_ready() && !fat32_write_file(kIdentityFile, data.data(), data.size()))
      kputs("[host] could not save the kernel identity to the card - it lasts until reboot\n");
    sync_with_card();
    StoredFile& f = g_files[path];
    f.data.assign(data.begin(), data.end());
    f.loaded = true;
    f.on_card = false;
    if (!is_identity(path)) flush_to_card(path, f);
    return Ok();
  }
  Result<void> append_file(const std::string& path, ByteSpan data) override {
    auto existing = read_file(path);
    ByteBuffer merged;
    if (existing.ok()) merged = std::move(*existing);
    merged.insert(merged.end(), data.begin(), data.end());
    return write_file(path, ByteSpan(merged.data(), merged.size()));
  }
  Result<ByteBuffer> read_file(const std::string& path) override {
    if (is_identity(path) && card_ready()) {
      size_t size = 0, got = 0;
      if (fat32_file_size(kIdentityFile, size) && size > 0 && size <= 4096) {
        ByteBuffer b(size);
        if (fat32_read_file(kIdentityFile, b.data(), size, got) && got == size) return b;
      }
    }
    StoredFile* f = find_file(path);
    if (!f) return Result<ByteBuffer>(Status::NotFound, "read_file: no such file: " + path);
    return ByteBuffer(f->data.begin(), f->data.end());
  }
  // The names directly inside `dir` (a path's own hierarchy - a flat store of
  // whole paths underneath), as a directory listing gives them.
  Result<std::vector<std::string>> list_dir(const std::string& dir) override {
    sync_with_card();
    std::string pre = dir;
    while (!pre.empty() && pre.back() == '/') pre.pop_back();
    pre += "/";
    std::vector<std::string> names;
    for (const auto& kv : g_files) {
      if (kv.first.compare(0, pre.size(), pre) != 0) continue;
      std::string rest = kv.first.substr(pre.size());
      const size_t slash = rest.find('/');
      if (slash != std::string::npos) rest = rest.substr(0, slash);
      if (!rest.empty() && (names.empty() || names.back() != rest) &&
          std::find(names.begin(), names.end(), rest) == names.end())
        names.push_back(rest);
    }
    return names;
  }
  // A flat namespace has nothing to CREATE for a directory - write_file/
  // read_file already take the whole path as one opaque flat key, slashes
  // included, so "aios/root/x" is just a filename this substrate does not
  // parse for structure. Ok() here is the honest answer, not a lie: the
  // operation this call represents (make sure this prefix is usable)
  // trivially holds, nothing was skipped or faked. Returning Unsupported
  // instead halted every boot on this hardware - Kernel::init() (kernel.cpp)
  // treats make_dir() failing as fatal for ANY root, there is no substrate
  // that can pass a non-empty cfg.root without this succeeding. list_dir()
  // below is the one place a real hierarchy would actually be missed, and
  // it already only ever lists "/" - honest about that limitation where it
  // matters, unlike here where there was nothing to be honest ABOUT.
  Result<void> make_dir(const std::string&) override { return Ok(); }
  bool exists(const std::string& path) override {
    sync_with_card();
    if (g_files.count(path)) return true;
    // a directory: some path underneath it
    std::string pre = path;
    while (!pre.empty() && pre.back() == '/') pre.pop_back();
    pre += "/";
    auto it = g_files.lower_bound(pre);
    return it != g_files.end() && it->first.compare(0, pre.size(), pre) == 0;
  }
  Result<FileInfo> stat(const std::string& path) override {
    StoredFile* f = find_file(path);
    if (!f) return Result<FileInfo>(Status::NotFound, "stat: no such file: " + path);
    FileInfo fi; fi.size = f->data.size();
    return fi;
  }
  Result<void> remove(const std::string& path) override {
    sync_with_card();
    auto it = g_files.find(path);
    if (it == g_files.end()) return Result<void>(Status::NotFound, "remove: no such file: " + path);
    g_files.erase(it);
    auto n = g_card_name.find(path);
    if (n != g_card_name.end()) {
      if (card_ready()) fat32_delete_file(n->second.c_str());
      g_card_path.erase(n->second);
      g_card_name.erase(n);
      if (card_ready()) save_index();
    }
    return Ok();
  }

  uint64_t monotonic_ns() override {
    /* The free-running counter. This counted timer-IRQ ticks, which never
     * advance on the Pi (no GIC): every kernel timeout and lease measured
     * against this clock saw zero forever. */
    return metal_now_ns();
  }
  uint64_t wall_ns() override {
    /* NTP time once net.cpp has it; before that an openly fixed epoch plus
     * uptime (it used to be only that, a 2025 date, forever). */
    int64_t ns = 0;
    if (metal_wall_unix_ns(&ns) && ns > 0) return static_cast<uint64_t>(ns);
    return 1788393600000000000ull + metal_now_ns();
  }
  void random(MutableByteSpan out) override {
    // No hardware RNG confirmed on this board - xorshift64* seeded from the
    // tick counter. Fine for the relay link's nonce-uniqueness needs, NOT a
    // claim of cryptographic-quality entropy; a TRNG/RNDR source should
    // replace this the day one is confirmed present.
    static uint64_t state = 0;
    if (state == 0) state = metal_ticks() ^ 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < out.size(); ++i) {
      state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
      out[i] = static_cast<uint8_t>((state * 0x2545F4914F6CDD1Dull) >> 56);
    }
  }
  Result<std::vector<StorageVolume>> storage(const std::string&) override {
    /* The card as the drivers see it: the FAT32 volume's real size and use
     * when mounted, the raw card's size when not. This reported "sd0",
     * capacity 0, which the Machine window printed as "0 MiB". */
    std::vector<StorageVolume> out;
    const SdCardStatus& sd = sdcard_status();
    const Fat32FsStatus& fs = fat32_status();
    if (!sd.initialized) return out;
    StorageVolume v;
    v.name = fs.mounted && fs.volume_label[0] ? fs.volume_label : "sd0";
    v.path = fs.mounted ? "/sd" : "sd0";
    v.kind = fs.mounted ? "filesystem" : "device";
    v.block_size = fs.mounted ? fs.bytes_per_cluster : 512;
    if (fs.mounted && fs.total_clusters) {
      v.capacity = static_cast<uint64_t>(fs.total_clusters) * fs.bytes_per_cluster;
      if (fs.free_clusters != 0xFFFFFFFFu)
        v.used = v.capacity - static_cast<uint64_t>(fs.free_clusters) * fs.bytes_per_cluster;
    } else {
      v.capacity = sd.capacity_bytes;
    }
    v.holds_root = false;   /* the kernel root is still in memory on this board */
    out.push_back(v);
    return out;
  }
  Result<std::vector<NetworkInterface>> network() override {
    /* eth0 and wlan0 from the network stack. This returned an empty list,
     * which the Machine window rendered as "none on this machine" while
     * eth0 was up and serving the console. */
    std::vector<NetworkInterface> out;
    auto add = [&](const NetInterfaceInfo& i, const char* name, uint64_t speed) {
      if (!i.initialized) return;
      NetworkInterface n;
      n.name = name;
      char buf[24];
      if (i.ip_cfg.dhcp_bound) { ip_to_str(i.ip_cfg.ip, buf, sizeof(buf)); n.address = buf; }
      mac_to_str(i.mac, buf, sizeof(buf)); n.hardware = buf;
      n.speed_bps = speed;
      n.mtu = 1500;
      n.up = i.link_up || i.link_state == NetLinkState::Associated;
      out.push_back(n);
    };
    add(net_eth_info(), "eth0", static_cast<uint64_t>(lan9514_info().link_mbps) * 1000000ull);
    add(net_wifi_info(), "wlan0", 0);
    return out;
  }
  const Topology& topology() override {
    static Topology t;
    t.logical_cpus = 4;   // from the device tree, per `info` - not re-derived here
    t.arch = "aarch64";
    t.host_name = "aios-metal-arm64";
    /* The page allocator's own figures - the desktop's RAM readout printed
     * "RAM --" on this board because these were left at zero. */
    t.total_memory = metal_heap_total();
    t.available_memory = metal_heap_total() - metal_heap_used();
    return t;
  }
  uint64_t available_bytes() override { return metal_heap_total() - metal_heap_used(); }
};

}  // namespace

std::unique_ptr<Host> make_host() { return std::make_unique<Arm64Host>(); }

void* load_library(const std::string&) { return nullptr; }
void* library_symbol(void*, const std::string&) { return nullptr; }
void unload_library(void*) {}
std::string library_error() { return "no dynamic loader on this substrate"; }

// No second program to run: one address space, no loader for command lines. A caller that would run one (ssh_config's
// Match exec) is told so, as on the x86 metal substrate; the service manager that would supervise one is not built here.
Result<ProcessHandle> spawn_process(const ProcessSpawn&) {
  return Result<ProcessHandle>(Status::Unsupported, "the arm64 metal substrate runs no other programs");
}
Result<ProcessStatus> process_status(ProcessHandle) {
  return Result<ProcessStatus>(Status::NotFound, "no processes on this substrate");
}
Result<void> signal_process(ProcessHandle, ProcessSignal) {
  return Result<void>(Status::NotFound, "no processes on this substrate");
}
Result<void> release_process(ProcessHandle) {
  return Result<void>(Status::NotFound, "no processes on this substrate");
}

}  // namespace aios::hal
