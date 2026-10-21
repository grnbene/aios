// virtio-gpu, 2D: the kernel bringing up a display on hardware whose mode
// nothing else in the boot path could set.
//
// Why this exists. The loader can only hand over a framebuffer for a device
// its firmware has a GOP for, and the kernel's own DISPI mode-set
// (metal_display_best_mode) only speaks to Bochs-class adapters — QEMU's
// stdvga, Bochs, VirtualBox. On a machine whose display is a virtio-gpu, both
// of those come up empty and the OS falls back to an 80x25 text console: no
// desktop, no pixels. That is the gap this closes, and it is the same gap a
// real GPU presents — a device the kernel has to program itself, through its
// own queues, or go without a screen.
//
// virtio-gpu is virtio 1.0 only: there is no legacy I/O BAR to poke like
// virtio-blk and virtio-net do, so this carries the modern transport as well —
// PCI capabilities pointing into MMIO BARs, a split virtqueue whose three
// rings are handed over as physical addresses, and a notify address computed
// from the queue's own offset. Everything is polled; there is no interrupt
// handler in this kernel to wake.
//
// The scanout is a resource backed by pages this kernel allocated, so the
// compositor draws into ordinary memory and the device is told to go and look
// at it. That means a frame is not finished when the last pixel is written:
// it has to be transferred and flushed, which is what metal_virtio_gpu_present
// does at the end of every composited frame.
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" void* metal_alloc_pages(size_t bytes);
extern "C" void metal_console_write(const char* s, size_t n);
extern "C" bool metal_map_device_1g(uint64_t phys);

namespace {

inline void outl(uint16_t p, uint32_t v) { asm volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
inline uint32_t inl(uint16_t p) { uint32_t v; asm volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

uint32_t pci_read(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off) {
  outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
  return inl(0xCFC);
}
void pci_write(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off, uint32_t v) {
  outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
  outl(0xCFC, v);
}

template <typename T>
volatile T* at(uint64_t addr) { return reinterpret_cast<volatile T*>(static_cast<uintptr_t>(addr)); }

void barrier() { asm volatile("mfence" ::: "memory"); }

// ---- the modern virtio PCI transport ---------------------------------------
constexpr uint8_t CAP_VENDOR = 0x09;
constexpr uint8_t CFG_COMMON = 1, CFG_NOTIFY = 2, CFG_DEVICE = 4;

// virtio_pci_common_cfg, by byte offset.
constexpr uint32_t C_DEV_FEAT_SEL = 0x00, C_DEV_FEAT = 0x04, C_DRV_FEAT_SEL = 0x08, C_DRV_FEAT = 0x0C,
                   C_NUM_QUEUES = 0x12, C_STATUS = 0x14, C_QUEUE_SEL = 0x16, C_QUEUE_SIZE = 0x18,
                   C_QUEUE_ENABLE = 0x1C, C_QUEUE_NOTIFY_OFF = 0x1E, C_QUEUE_DESC = 0x20,
                   C_QUEUE_DRIVER = 0x28, C_QUEUE_DEVICE = 0x30;

constexpr uint8_t ST_ACK = 1, ST_DRIVER = 2, ST_DRIVER_OK = 4, ST_FEATURES_OK = 8;

struct __attribute__((packed)) VringDesc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };
struct __attribute__((packed)) VringUsedElem { uint32_t id; uint32_t len; };
constexpr uint16_t DESC_NEXT = 1, DESC_WRITE = 2;

// ---- virtio-gpu commands ----------------------------------------------------
constexpr uint32_t CMD_GET_DISPLAY_INFO = 0x0100, CMD_RESOURCE_CREATE_2D = 0x0101,
                   CMD_RESOURCE_FLUSH = 0x0104, CMD_TRANSFER_TO_HOST_2D = 0x0105,
                   CMD_RESOURCE_ATTACH_BACKING = 0x0106, CMD_SET_SCANOUT = 0x0103;
constexpr uint32_t RESP_OK_NODATA = 0x1100, RESP_OK_DISPLAY_INFO = 0x1101;
constexpr uint32_t FORMAT_B8G8R8X8 = 2;   // our ARGB word, little-endian, is B,G,R,X in memory

struct __attribute__((packed)) CtrlHdr {
  uint32_t type; uint32_t flags; uint64_t fence_id; uint32_t ctx_id; uint32_t padding;
};
struct __attribute__((packed)) Rect { uint32_t x, y, width, height; };
struct __attribute__((packed)) ResourceCreate2D { CtrlHdr hdr; uint32_t resource_id, format, width, height; };
struct __attribute__((packed)) MemEntry { uint64_t addr; uint32_t length; uint32_t padding; };
struct __attribute__((packed)) AttachBacking { CtrlHdr hdr; uint32_t resource_id, nr_entries; MemEntry entry; };
struct __attribute__((packed)) SetScanout { CtrlHdr hdr; Rect r; uint32_t scanout_id, resource_id; };
struct __attribute__((packed)) TransferToHost2D { CtrlHdr hdr; Rect r; uint64_t offset; uint32_t resource_id, padding; };
struct __attribute__((packed)) ResourceFlush { CtrlHdr hdr; Rect r; uint32_t resource_id, padding; };
struct __attribute__((packed)) DisplayOne { Rect r; uint32_t enabled, flags; };
struct __attribute__((packed)) DisplayInfo { CtrlHdr hdr; DisplayOne pmodes[16]; };

struct Gpu {
  uint64_t common = 0, notify = 0, device_cfg = 0;
  uint32_t notify_mult = 0;
  uint16_t notify_off = 0;
  uint16_t qsize = 0;
  VringDesc* desc = nullptr;
  uint16_t* avail = nullptr;      // flags, idx, ring[]
  uint8_t* used = nullptr;        // flags, idx, ring[]
  uint16_t avail_shadow = 0, used_seen = 0;
  uint8_t* cmd = nullptr;         // request scratch
  uint8_t* rsp = nullptr;         // response scratch
  uint32_t width = 0, height = 0;
  uint8_t* fb = nullptr;
  bool ready = false;

  uint8_t status() { return *at<uint8_t>(common + C_STATUS); }
  void set_status(uint8_t v) { *at<uint8_t>(common + C_STATUS) = v; barrier(); }

  // One command, posted and waited for. Two descriptors: what the device
  // reads, and where it writes the answer.
  bool run(size_t req_len, size_t rsp_len) {
    memset(rsp, 0, rsp_len);
    desc[0].addr = (uint64_t)(uintptr_t)cmd;
    desc[0].len = (uint32_t)req_len;
    desc[0].flags = DESC_NEXT;
    desc[0].next = 1;
    desc[1].addr = (uint64_t)(uintptr_t)rsp;
    desc[1].len = (uint32_t)rsp_len;
    desc[1].flags = DESC_WRITE;
    desc[1].next = 0;

    uint16_t* avail_idx = avail + 1;
    uint16_t* avail_ring = avail + 2;
    avail_ring[avail_shadow % qsize] = 0;
    ++avail_shadow;
    barrier();
    *avail_idx = avail_shadow;
    barrier();
    *at<uint16_t>(notify + (uint64_t)notify_off * notify_mult) = 0;

    volatile uint16_t* used_idx = (volatile uint16_t*)(used + 2);
    for (uint64_t spin = 0; spin < 400000000ull; ++spin) {
      if (*used_idx != used_seen) {
        used_seen = *used_idx;
        barrier();
        return true;
      }
      asm volatile("pause");
    }
    return false;                 // the device never answered
  }

  bool simple(uint32_t type, size_t req_len) {
    ((CtrlHdr*)cmd)->type = type;
    if (!run(req_len, sizeof(CtrlHdr))) return false;
    return ((CtrlHdr*)rsp)->type == RESP_OK_NODATA;
  }
};

Gpu g;

// The capability list, walked for the three structures this driver needs.
bool map_caps(uint32_t bus, uint32_t dev, uint32_t fn) {
  uint32_t bars[6];
  for (int i = 0; i < 6; ++i) bars[i] = pci_read(bus, dev, fn, 0x10 + 4 * i);
  uint8_t cap = (uint8_t)(pci_read(bus, dev, fn, 0x34) & 0xFF);
  int guard = 0;
  while (cap && cap != 0xFF && ++guard < 48) {
    uint32_t w0 = pci_read(bus, dev, fn, cap);
    if ((w0 & 0xFF) == CAP_VENDOR) {
      uint8_t cfg_type = (uint8_t)((w0 >> 24) & 0xFF);
      uint32_t w1 = pci_read(bus, dev, fn, cap + 4);
      uint8_t bar = (uint8_t)(w1 & 0xFF);
      uint32_t offset = pci_read(bus, dev, fn, cap + 8);
      if (bar < 6) {
        uint64_t base = bars[bar] & ~0xFull;
        // A 64-bit memory BAR carries its high half in the next slot.
        if (((bars[bar] >> 1) & 3) == 2 && bar + 1 < 6)
          base |= (uint64_t)bars[bar + 1] << 32;
        if (base) {
          // The BAR may be anywhere in the 64-bit space; make sure it is
          // mapped before anything dereferences it.
          metal_map_device_1g(base + offset);
          if (cfg_type == CFG_COMMON) g.common = base + offset;
          else if (cfg_type == CFG_DEVICE) g.device_cfg = base + offset;
          else if (cfg_type == CFG_NOTIFY) {
            g.notify = base + offset;
            g.notify_mult = pci_read(bus, dev, fn, cap + 16);
          }
        }
      }
    }
    cap = (uint8_t)((w0 >> 8) & 0xFF);
  }
  return g.common && g.notify;
}

bool setup_queue() {
  *at<uint16_t>(g.common + C_QUEUE_SEL) = 0;
  barrier();
  uint16_t size = *at<uint16_t>(g.common + C_QUEUE_SIZE);
  if (!size || size > 1024) return false;
  g.qsize = size;

  // Modern virtio hands the three rings over separately, so they are three
  // separate page-aligned allocations rather than one packed block.
  void* d = metal_alloc_pages(((size_t)size * sizeof(VringDesc) + 4095) & ~4095ull);
  void* a = metal_alloc_pages(4096);
  void* u = metal_alloc_pages((((size_t)size * sizeof(VringUsedElem) + 16) + 4095) & ~4095ull);
  if (!d || !a || !u) return false;
  g.desc = (VringDesc*)d;
  g.avail = (uint16_t*)a;
  g.used = (uint8_t*)u;

  *at<uint64_t>(g.common + C_QUEUE_DESC) = (uint64_t)(uintptr_t)d;
  *at<uint64_t>(g.common + C_QUEUE_DRIVER) = (uint64_t)(uintptr_t)a;
  *at<uint64_t>(g.common + C_QUEUE_DEVICE) = (uint64_t)(uintptr_t)u;
  g.notify_off = *at<uint16_t>(g.common + C_QUEUE_NOTIFY_OFF);
  *at<uint16_t>(g.common + C_QUEUE_ENABLE) = 1;
  barrier();
  return true;
}

}  // namespace

// Bring the device up and give the kernel a framebuffer on it. Returns false
// and changes nothing if there is no virtio-gpu, or if it refuses any step —
// the caller keeps whatever display it already had.
extern "C" bool metal_virtio_gpu_init(uint32_t want_w, uint32_t want_h,
                                      uint64_t* out_base, uint32_t* out_pitch,
                                      uint32_t* out_w, uint32_t* out_h) {
  if (g.ready) return false;
  uint32_t bus = 0, dev = 0, fn = 0;
  bool found = false;
  for (bus = 0; bus < 8 && !found; ++bus)
    for (dev = 0; dev < 32 && !found; ++dev)
      for (fn = 0; fn < 8 && !found; ++fn) {
        uint32_t id = pci_read(bus, dev, fn, 0);
        // 1af4:1050 is virtio-gpu on the modern (virtio 1.0) device-id range.
        if ((id & 0xFFFF) != 0x1af4) { if (fn == 0 && id == 0xFFFFFFFFu) break; continue; }
        uint32_t device = id >> 16;
        if (device == 0x1050 || device == 0x1010) found = true;
      }
  if (!found) return false;
  --fn; --dev; --bus;   // the loops leave the counters one past the hit

  // Memory space and bus mastering: without the latter the device cannot read
  // the rings, and every command times out.
  pci_write(bus, dev, fn, 0x04, pci_read(bus, dev, fn, 0x04) | 0x6);
  if (!map_caps(bus, dev, fn)) return false;

  g.set_status(0);
  g.set_status(ST_ACK);
  g.set_status(ST_ACK | ST_DRIVER);

  // The only feature this driver needs is virtio 1.0 itself (bit 32).
  *at<uint32_t>(g.common + C_DRV_FEAT_SEL) = 1;
  *at<uint32_t>(g.common + C_DRV_FEAT) = 1u;          // VIRTIO_F_VERSION_1
  *at<uint32_t>(g.common + C_DRV_FEAT_SEL) = 0;
  *at<uint32_t>(g.common + C_DRV_FEAT) = 0;
  g.set_status(ST_ACK | ST_DRIVER | ST_FEATURES_OK);
  if (!(g.status() & ST_FEATURES_OK)) return false;   // the device refused them

  if (!setup_queue()) return false;
  g.cmd = (uint8_t*)metal_alloc_pages(4096);
  g.rsp = (uint8_t*)metal_alloc_pages(4096);
  if (!g.cmd || !g.rsp) return false;
  g.set_status(ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);

  // What the display says it is. A mode it reports is worth more than a mode
  // we would like, but an unconfigured virtio-gpu reports 0x0 — in which case
  // the asked-for size stands.
  uint32_t w = want_w, h = want_h;
  memset(g.cmd, 0, sizeof(CtrlHdr));
  ((CtrlHdr*)g.cmd)->type = CMD_GET_DISPLAY_INFO;
  if (g.run(sizeof(CtrlHdr), sizeof(DisplayInfo))) {
    DisplayInfo* di = (DisplayInfo*)g.rsp;
    if (di->hdr.type == RESP_OK_DISPLAY_INFO && di->pmodes[0].enabled &&
        di->pmodes[0].r.width >= 640 && di->pmodes[0].r.height >= 480) {
      w = di->pmodes[0].r.width;
      h = di->pmodes[0].r.height;
    }
  }

  size_t bytes = (size_t)w * h * 4;
  uint8_t* fb = (uint8_t*)metal_alloc_pages((bytes + 4095) & ~4095ull);
  if (!fb) return false;

  // A 2D resource, backed by that memory, made the scanout.
  memset(g.cmd, 0, sizeof(ResourceCreate2D));
  auto* rc = (ResourceCreate2D*)g.cmd;
  rc->resource_id = 1;
  rc->format = FORMAT_B8G8R8X8;
  rc->width = w;
  rc->height = h;
  if (!g.simple(CMD_RESOURCE_CREATE_2D, sizeof(ResourceCreate2D))) return false;

  memset(g.cmd, 0, sizeof(AttachBacking));
  auto* ab = (AttachBacking*)g.cmd;
  ab->resource_id = 1;
  ab->nr_entries = 1;
  ab->entry.addr = (uint64_t)(uintptr_t)fb;
  ab->entry.length = (uint32_t)bytes;
  if (!g.simple(CMD_RESOURCE_ATTACH_BACKING, sizeof(AttachBacking))) return false;

  memset(g.cmd, 0, sizeof(SetScanout));
  auto* ss = (SetScanout*)g.cmd;
  ss->r = {0, 0, w, h};
  ss->scanout_id = 0;
  ss->resource_id = 1;
  if (!g.simple(CMD_SET_SCANOUT, sizeof(SetScanout))) return false;

  g.width = w;
  g.height = h;
  g.fb = fb;
  g.ready = true;
  if (out_base) *out_base = (uint64_t)(uintptr_t)fb;
  if (out_pitch) *out_pitch = w * 4;
  if (out_w) *out_w = w;
  if (out_h) *out_h = h;
  return true;
}

// Whether this kernel is the thing driving the display.
extern "C" bool metal_virtio_gpu_active() { return g.ready; }

// The device is looking at our memory, but it does not watch it: a frame is
// only on the glass once it has been transferred and flushed.
extern "C" void metal_virtio_gpu_present() {
  if (!g.ready) return;
  memset(g.cmd, 0, sizeof(TransferToHost2D));
  auto* tr = (TransferToHost2D*)g.cmd;
  tr->r = {0, 0, g.width, g.height};
  tr->offset = 0;
  tr->resource_id = 1;
  if (!g.simple(CMD_TRANSFER_TO_HOST_2D, sizeof(TransferToHost2D))) return;

  memset(g.cmd, 0, sizeof(ResourceFlush));
  auto* fl = (ResourceFlush*)g.cmd;
  fl->r = {0, 0, g.width, g.height};
  fl->resource_id = 1;
  g.simple(CMD_RESOURCE_FLUSH, sizeof(ResourceFlush));
}
