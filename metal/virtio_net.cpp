// virtio-net over the legacy PCI I/O transport: raw Ethernet frames in and
// out, polled. The AOS fabric runs directly on it; there is no IP stack.
#include "aios/hal/fabric.hpp"
#include <cstring>
#include <memory>

extern "C" void* metal_alloc_pages(size_t bytes);

namespace {
inline void outb(uint16_t p, uint8_t v) { asm volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
inline void outw(uint16_t p, uint16_t v) { asm volatile("outw %0, %1" : : "a"(v), "Nd"(p)); }
inline void outl(uint16_t p, uint32_t v) { asm volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
inline uint8_t inb(uint16_t p) { uint8_t v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
inline uint16_t inw(uint16_t p) { uint16_t v; asm volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
inline uint32_t inl(uint16_t p) { uint32_t v; asm volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
uint32_t pci_read(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off) { outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC)); return inl(0xCFC); }
void pci_write(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off, uint32_t v) { outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC)); outl(0xCFC, v); }

struct __attribute__((packed)) VringDesc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };
struct __attribute__((packed)) VringUsedElem { uint32_t id; uint32_t len; };
constexpr uint16_t DESC_NEXT = 1, DESC_WRITE = 2;
constexpr uint32_t F_MAC = 1u << 5;
constexpr size_t NET_HDR = 10, BUF = 2048, RX_BUFS = 32;
// How many rx buffers the driver keeps posted to the device, and therefore how
// many it can refill behind a single doorbell. This is what the policy head's
// a_t[1] (SubstrateActuation::nic_prefetch_enabled) actually selects: with
// prefetch off the device is only ever holding a handful of buffers and the
// driver kicks it about once per frame; with prefetch on the ring is kept full
// and refills go out in batches. The counters below make the difference
// readable rather than asserted.
constexpr uint16_t RX_WINDOW_LAZY = 4;
constexpr uint16_t RX_WINDOW_PREFETCH = (uint16_t)RX_BUFS;

struct Queue {
  uint16_t size = 0, last_used = 0;
  VringDesc* desc = nullptr; uint16_t* avail_idx = nullptr; uint16_t* avail_ring = nullptr; uint16_t* used_idx = nullptr; VringUsedElem* used_ring = nullptr;
  bool init(uint16_t io, uint16_t index) {
    outw(io + 0x0E, index); size = inw(io + 0x0C);
    if (!size || size > 1024) return false;
    size_t desc_bytes = 16 * size, avail_bytes = 6 + 2 * size, used_off = ((desc_bytes + avail_bytes) + 4095) & ~4095ull, total = used_off + 6 + 8 * size;
    uint8_t* ring = (uint8_t*)metal_alloc_pages((total + 4095) & ~4095ull); if (!ring) return false;
    desc = (VringDesc*)ring; avail_idx = (uint16_t*)(ring + desc_bytes + 2); avail_ring = (uint16_t*)(ring + desc_bytes + 4);
    used_idx = (uint16_t*)(ring + used_off + 2); used_ring = (VringUsedElem*)(ring + used_off + 4);
    outl(io + 0x08, (uint32_t)((uintptr_t)ring >> 12));
    return true;
  }
};

class VirtioNet final : public aios::hal::FrameDevice {
 public:
  bool init(uint16_t io, uint32_t bus, uint32_t dev, uint32_t fn) {
    io_ = io;
    pci_write(bus, dev, fn, 0x04, pci_read(bus, dev, fn, 0x04) | 0x4);
    outb(io_ + 0x12, 0); outb(io_ + 0x12, 1); outb(io_ + 0x12, 1 | 2);
    uint32_t feats = inl(io_ + 0x00);
    outl(io_ + 0x04, feats & F_MAC);   // no offloads, no mergeable buffers: plain frames
    if (!rx_.init(io_, 0) || !tx_.init(io_, 1)) return false;
    for (int i = 0; i < 6; ++i) mac_[i] = inb(io_ + 0x14 + i);
    bufs_ = (uint8_t*)metal_alloc_pages(BUF * (RX_BUFS + 2));
    // RX: descriptor pairs (header, frame) per buffer. Every usable slot starts
    // on the free list; only `window_` of them are handed to the device, so how
    // many are outstanding is a decision the driver keeps making, not a
    // constant baked in at init.
    for (uint16_t i = 0; i < RX_BUFS && 2 * i + 1 < rx_.size; ++i) free_[free_count_++] = i;
    slots_ = free_count_;
    if (window_ > slots_) window_ = slots_;
    refill_rx();
    outb(io_ + 0x12, 1 | 2 | 4);
    name_ = "virtio-net";
    return true;
  }
  const aios::hal::Mac& mac() const override { return mac_; }
  const std::string& name() const override { return name_; }
  bool send(const uint8_t* frame, size_t len) override {
    if (len > BUF - NET_HDR) return false;
    uint8_t* b = bufs_ + BUF * RX_BUFS;                        // one TX buffer; we wait for completion
    std::memset(b, 0, NET_HDR); std::memcpy(b + NET_HDR, frame, len);
    tx_.desc[0] = {(uint64_t)(uintptr_t)b, (uint32_t)NET_HDR, DESC_NEXT, 1};
    tx_.desc[1] = {(uint64_t)(uintptr_t)(b + NET_HDR), (uint32_t)len, 0, 0};
    tx_.avail_ring[*tx_.avail_idx % tx_.size] = 0;
    asm volatile("" ::: "memory"); (*tx_.avail_idx)++; asm volatile("mfence" ::: "memory");
    outw(io_ + 0x10, 1);
    uint16_t want = tx_.last_used + 1;
    for (uint64_t spins = 0; *tx_.used_idx != want; ++spins) { asm volatile("pause"); if (spins > 200000000ull) return false; }
    tx_.last_used = want;
    return true;
  }
  int recv(uint8_t* out, size_t cap) override {
    asm volatile("mfence" ::: "memory");
    // How far BEHIND the device this driver is running: frames sitting in the
    // used ring waiting to be taken. This is not loss and must not be used as
    // loss — it was, for one measurement, and it read exactly 0.5 of every
    // buffer posted at both window settings, because "the device is ahead of
    // me" is the normal state of a driver under load and has nothing to do with
    // whether anything was dropped.
    //
    // What actually loses frames — the device with no buffer to put one in —
    // leaves no trace on this side at all. virtio-net reports it only through
    // the control queue's statistics, which this driver does not negotiate, so
    // it is NOT OBSERVABLE here and `jepa status` says so rather than offering
    // a number that means something else.
    {
      const uint16_t pending = (uint16_t)(*rx_.used_idx - rx_.last_used);
      if (pending > behind_max_) behind_max_ = pending;
      behind_total_ += pending;
      ++behind_samples_;
    }
    if (*rx_.used_idx == rx_.last_used) return 0;
    VringUsedElem e = rx_.used_ring[rx_.last_used % rx_.size];
    rx_.last_used++;
    uint16_t slot = (uint16_t)(e.id / 2);
    int len = (int)e.len - (int)NET_HDR; if (len < 0) len = 0;
    if ((size_t)len > cap) len = (int)cap;
    std::memcpy(out, bufs_ + BUF * slot + NET_HDR, (size_t)len);
    // The buffer is ours again. Whether it goes straight back to the device,
    // and how many of its neighbours go with it, is the prefetch decision.
    if (free_count_ < RX_BUFS) free_[free_count_++] = slot;
    if (posted_) --posted_;
    // Refill on a LOW-WATER MARK, not on every frame.
    //
    // This called refill_rx() after each frame, so the device was never more
    // than one buffer short and the batch below could only ever post one and
    // ring one doorbell. Measured under live frames the driver did exactly that:
    // 33,174 buffers over 33,174 kicks with the window wide open — one kick per
    // buffer, and the wide window bought nothing at all in notifications, which
    // is the thing it exists to buy.
    //
    // Half the window is the mark. The device keeps at least that many, so it
    // still has somewhere to put a frame, and the refill that follows posts a
    // real batch behind one MMIO write. A window of 4 batches 2; a window of 32
    // batches 16.
    if (posted_ <= (uint16_t)(window_ / 2) || !posted_) refill_rx();
    if (!posted_) ++starved_;   // the window drained: the device has nowhere to put the next frame
    return len;
  }

  // ---- the lever ----------------------------------------------------------
  // Called from metal_actuate() with the policy head's decoded a_t[1]. Growing
  // the window takes effect immediately; shrinking it cannot revoke buffers the
  // device already owns (virtio has no un-post), so it decays as frames arrive.
  void set_prefetch(bool on) {
    uint16_t w = on ? RX_WINDOW_PREFETCH : RX_WINDOW_LAZY;
    if (w > slots_) w = slots_;
    if (w == window_ && on == prefetch_) return;
    prefetch_ = on;
    window_ = w;
    ++window_changes_;
    refill_rx();
  }
 private:
  // Publish one buffer to the device WITHOUT ringing the doorbell — the batch
  // does that once at the end, which is where prefetch buys its notifications.
  void queue_rx(uint16_t slot) {
    uint8_t* b = bufs_ + BUF * slot;
    uint16_t d = (uint16_t)(2 * slot);
    rx_.desc[d] = {(uint64_t)(uintptr_t)b, (uint32_t)NET_HDR, (uint16_t)(DESC_NEXT | DESC_WRITE), (uint16_t)(d + 1)};
    rx_.desc[d + 1] = {(uint64_t)(uintptr_t)(b + NET_HDR), (uint32_t)(BUF - NET_HDR), DESC_WRITE, 0};
    rx_.avail_ring[*rx_.avail_idx % rx_.size] = d;
    asm volatile("" ::: "memory"); (*rx_.avail_idx)++;
  }

  // Top the device back up to the current window in ONE batch behind ONE
  // doorbell. Called on the low-water mark above, so the ring really is several
  // buffers short by the time this runs and the batch is a batch.
  void refill_rx() {
    uint16_t n = 0;
    while (posted_ < window_ && free_count_) {
      queue_rx(free_[--free_count_]);
      ++posted_; ++n;
    }
    if (!n) return;
    asm volatile("mfence" ::: "memory");
    outw(io_ + 0x10, 0);
    ++kicks_;
    buffers_posted_ += n;
    if (n > max_batch_) max_batch_ = n;
  }

  uint16_t io_ = 0; Queue rx_, tx_; uint8_t* bufs_ = nullptr; aios::hal::Mac mac_{}; std::string name_;
  // rx posting state: the free slots we hold, how many the device holds, and
  // the window the policy head set.
  uint16_t free_[RX_BUFS]{}; uint16_t free_count_ = 0; uint16_t slots_ = 0;
  uint16_t posted_ = 0; uint16_t window_ = RX_WINDOW_LAZY; bool prefetch_ = false;
  // what the console reads back
  uint64_t kicks_ = 0, buffers_posted_ = 0, starved_ = 0, window_changes_ = 0;
  // How far behind the device this driver runs: a backlog, not a loss.
  uint64_t behind_total_ = 0, behind_samples_ = 0;
  uint16_t behind_max_ = 0;
  uint16_t max_batch_ = 0;
 public:
  // How full the rx ring is right now: frames the device has handed us that
  // nobody has read yet, over the ring size. This is the world model's dim 1.
  float rx_depth() const {
    if (!rx_.size) return 0.0f;
    uint16_t queued = (uint16_t)(*rx_.used_idx - rx_.last_used);
    float d = (float)queued / (float)rx_.size;
    return d > 1.0f ? 1.0f : d;
  }
  // What the console reads back to see the flag land in the driver.
  bool prefetch() const { return prefetch_; }
  uint16_t rx_window() const { return window_; }
  uint16_t rx_posted() const { return posted_; }
  uint64_t rx_behind_mean() const { return behind_samples_ ? behind_total_ / behind_samples_ : 0ull; }
  uint16_t rx_behind_max() const { return behind_max_; }
  uint16_t rx_slots() const { return slots_; }
  uint16_t rx_max_batch() const { return max_batch_; }
  uint64_t rx_kicks() const { return kicks_; }
  uint64_t rx_buffers_posted() const { return buffers_posted_; }
  uint64_t rx_starved() const { return starved_; }
  uint64_t rx_window_changes() const { return window_changes_; }
};
namespace { VirtioNet* g_net = nullptr; }
}  // namespace

extern "C" float metal_nic_rx_depth() { return g_net ? g_net->rx_depth() : 0.0f; }
extern "C" bool metal_nic_present() { return g_net != nullptr; }

// The policy head's a_t[1], arriving through metal_actuate(). Returns false when
// there is no NIC on this machine, so the caller can say so instead of claiming
// an actuation that never happened.
extern "C" bool metal_set_nic_prefetch(bool on) {
  if (!g_net) return false;
  g_net->set_prefetch(on);
  return true;
}

// Readback for the console: every one of these is a driver counter, not a
// setting echoed back.
extern "C" bool metal_nic_prefetch() { return g_net && g_net->prefetch(); }
extern "C" uint32_t metal_nic_rx_window() { return g_net ? g_net->rx_window() : 0u; }
extern "C" uint32_t metal_nic_rx_posted() { return g_net ? g_net->rx_posted() : 0u; }
extern "C" uint32_t metal_nic_rx_slots() { return g_net ? g_net->rx_slots() : 0u; }
extern "C" uint32_t metal_nic_rx_max_batch() { return g_net ? g_net->rx_max_batch() : 0u; }
extern "C" uint64_t metal_nic_rx_kicks() { return g_net ? g_net->rx_kicks() : 0ull; }
extern "C" uint64_t metal_nic_rx_buffers_posted() { return g_net ? g_net->rx_buffers_posted() : 0ull; }
extern "C" uint64_t metal_nic_rx_starved() { return g_net ? g_net->rx_starved() : 0ull; }
extern "C" uint64_t metal_nic_rx_behind_mean() { return g_net ? g_net->rx_behind_mean() : 0ull; }
extern "C" uint32_t metal_nic_rx_behind_max() { return g_net ? g_net->rx_behind_max() : 0u; }
extern "C" uint64_t metal_nic_rx_window_changes() { return g_net ? g_net->rx_window_changes() : 0ull; }

std::unique_ptr<aios::hal::FrameDevice> metal_probe_virtio_net() {
  for (uint32_t bus = 0; bus < 4; ++bus) for (uint32_t dev = 0; dev < 32; ++dev) for (uint32_t fn = 0; fn < 8; ++fn) {
    uint32_t id = pci_read(bus, dev, fn, 0);
    if ((id & 0xFFFF) != 0x1af4) { if (fn == 0 && id == 0xFFFFFFFF) break; continue; }
    if ((id >> 16) != 0x1000) continue;                        // transitional virtio-net
    uint32_t bar0 = pci_read(bus, dev, fn, 0x10); if (!(bar0 & 1)) continue;
    auto d = std::make_unique<VirtioNet>();
    if (d->init((uint16_t)(bar0 & ~3u), bus, dev, fn)) { g_net = d.get(); return d; }
  }
  return nullptr;
}
