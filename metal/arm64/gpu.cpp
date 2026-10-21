/* virtio-gpu 2D driver (metal/arm64/gpu.cpp).
 *
 * The minimum that puts a pixel on a screen with no firmware help: ask the
 * device what displays exist, create a host-side 2D resource, attach our own
 * pages as its backing store, point a scanout at it, and then per frame
 * transfer the dirty rectangle to the host and flush it.
 *
 * This is the piece that was missing everywhere — QEMU included. On a board
 * with no UEFI and no GOP, something has to own the display controller; on
 * QEMU virt that something is this, and on hardware whose firmware already lit
 * the panel it is the simple-framebuffer path in fb.cpp instead.
 */
#include "virtio.hpp"

#include "fdt.hpp"
#include "kio.hpp"
#include "mmu.hpp"
#include "timer.hpp"

namespace {

constexpr uint32_t VIRTIO_ID_GPU = 16;

constexpr uint32_t CMD_GET_DISPLAY_INFO      = 0x0100;
constexpr uint32_t CMD_RESOURCE_CREATE_2D    = 0x0101;
constexpr uint32_t CMD_SET_SCANOUT           = 0x0103;
constexpr uint32_t CMD_RESOURCE_FLUSH        = 0x0104;
constexpr uint32_t CMD_TRANSFER_TO_HOST_2D   = 0x0105;
constexpr uint32_t CMD_RESOURCE_ATTACH_BACKING = 0x0106;

constexpr uint32_t RESP_OK_NODATA       = 0x1100;
constexpr uint32_t RESP_OK_DISPLAY_INFO = 0x1101;

/* Byte order in memory is B,G,R,X, so a little-endian u32 store of
 * 0x00RRGGBB lands the channels where the device expects them. */
constexpr uint32_t FORMAT_B8G8R8X8 = 2;

constexpr uint32_t RESOURCE_ID = 1;

struct CtrlHdr {
  uint32_t type;
  uint32_t flags;
  uint64_t fence_id;
  uint32_t ctx_id;
  uint32_t padding;
};

struct Rect { uint32_t x, y, width, height; };

struct RespDisplayInfo {
  CtrlHdr hdr;
  struct { Rect r; uint32_t enabled; uint32_t flags; } pmodes[16];
};

struct ResourceCreate2D {
  CtrlHdr hdr;
  uint32_t resource_id;
  uint32_t format;
  uint32_t width;
  uint32_t height;
};

struct AttachBacking {
  CtrlHdr hdr;
  uint32_t resource_id;
  uint32_t nr_entries;
};

struct MemEntry {
  uint64_t addr;
  uint32_t length;
  uint32_t padding;
};

struct SetScanout {
  CtrlHdr hdr;
  Rect r;
  uint32_t scanout_id;
  uint32_t resource_id;
};

struct TransferToHost2D {
  CtrlHdr hdr;
  Rect r;
  uint64_t offset;
  uint32_t resource_id;
  uint32_t padding;
};

struct ResourceFlush {
  CtrlHdr hdr;
  Rect r;
  uint32_t resource_id;
  uint32_t padding;
};

VirtioDevice g_dev;
uint32_t g_width = 0;
uint32_t g_height = 0;
uint32_t* g_fb = nullptr;
uint64_t g_fb_bytes = 0;
bool g_live = false;
uint64_t g_flushes = 0;
uint64_t g_flush_ns = 0;

/* One page of command scratch and one of response scratch, so a command is
 * never assembled on a stack the device would have to reach into. */
uint8_t* g_cmd = nullptr;
uint8_t* g_resp = nullptr;

void zero(void* p, uint64_t n) {
  auto* b = static_cast<uint8_t*>(p);
  for (uint64_t i = 0; i < n; ++i) b[i] = 0;
}

/* Send one command, wait for its response, and report whether the device was
 * happy. `resp_bytes` is how much of the response we care about. */
bool gpu_cmd(uint32_t type, const void* body, uint32_t body_len, uint32_t resp_bytes,
             uint32_t expect = RESP_OK_NODATA) {
  auto* hdr = reinterpret_cast<CtrlHdr*>(g_cmd);
  zero(g_cmd, body_len ? body_len : sizeof(CtrlHdr));
  if (body && body_len) {
    const auto* src = static_cast<const uint8_t*>(body);
    for (uint32_t i = 0; i < body_len; ++i) g_cmd[i] = src[i];
  }
  hdr->type = type;
  hdr->flags = 0;
  hdr->fence_id = 0;
  hdr->ctx_id = 0;
  hdr->padding = 0;

  zero(g_resp, resp_bytes);

  const VirtioBuf chain[2] = {
      {reinterpret_cast<uint64_t>(g_cmd), body_len ? body_len : uint32_t{sizeof(CtrlHdr)}, false},
      {reinterpret_cast<uint64_t>(g_resp), resp_bytes, true},
  };
  if (virtio_request(g_dev, 0, chain, 2) < 0) return false;

  const auto* rh = reinterpret_cast<const CtrlHdr*>(g_resp);
  if (rh->type != expect) {
    kprintf("[gpu] command %04x answered %04x\n", static_cast<uint64_t>(type),
            static_cast<uint64_t>(rh->type));
    return false;
  }
  return true;
}

}  /* namespace */

uint32_t gpu_width()  { return g_width; }
uint32_t gpu_height() { return g_height; }
uint32_t* gpu_pixels(){ return g_fb; }
bool gpu_live()       { return g_live; }
uint64_t gpu_flushes(){ return g_flushes; }
uint64_t gpu_flush_ns(){ return g_flush_ns; }
uint64_t gpu_base()   { return g_dev.base; }
uint32_t gpu_version(){ return g_dev.version; }

/* Push a rectangle of our backing store to the host and put it on the glass.
 * Two commands, because the device keeps its own copy: TRANSFER moves the
 * pixels, FLUSH makes the scanout show them. */
bool gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  if (!g_live) return false;
  if (x >= g_width || y >= g_height) return false;
  if (x + w > g_width) w = g_width - x;
  if (y + h > g_height) h = g_height - y;
  if (!w || !h) return false;

  const uint64_t t0 = metal_now_ns();

  TransferToHost2D t{};
  t.r = {x, y, w, h};
  t.offset = static_cast<uint64_t>(y) * g_width * 4 + static_cast<uint64_t>(x) * 4;
  t.resource_id = RESOURCE_ID;
  if (!gpu_cmd(CMD_TRANSFER_TO_HOST_2D, &t, sizeof(t), sizeof(CtrlHdr))) return false;

  ResourceFlush fl{};
  fl.r = {x, y, w, h};
  fl.resource_id = RESOURCE_ID;
  if (!gpu_cmd(CMD_RESOURCE_FLUSH, &fl, sizeof(fl), sizeof(CtrlHdr))) return false;

  g_flush_ns += metal_now_ns() - t0;
  ++g_flushes;
  return true;
}

bool gpu_init() {
  if (!virtio_open(VIRTIO_ID_GPU, g_dev)) return false;
  if (!virtio_queue_init(g_dev, 0, 8)) {   /* controlq */
    kprintf("[gpu] %p has no usable control queue\n", g_dev.base);
    g_dev.present = false;
    return false;
  }
  virtio_start(g_dev);

  g_cmd = static_cast<uint8_t*>(metal_alloc_pages(4096));
  g_resp = static_cast<uint8_t*>(metal_alloc_pages(4096));
  if (!g_cmd || !g_resp) return false;

  /* What displays does this device actually have, and how big are they? */
  if (!gpu_cmd(CMD_GET_DISPLAY_INFO, nullptr, 0, sizeof(RespDisplayInfo),
               RESP_OK_DISPLAY_INFO)) {
    kputs("[gpu] the device would not describe its displays\n");
    return false;
  }
  const auto* info = reinterpret_cast<const RespDisplayInfo*>(g_resp);
  if (!info->pmodes[0].enabled) {
    kputs("[gpu] scanout 0 is not enabled\n");
    return false;
  }
  g_width = info->pmodes[0].r.width;
  g_height = info->pmodes[0].r.height;
  if (!g_width || !g_height) return false;

  ResourceCreate2D c{};
  c.resource_id = RESOURCE_ID;
  c.format = FORMAT_B8G8R8X8;
  c.width = g_width;
  c.height = g_height;
  if (!gpu_cmd(CMD_RESOURCE_CREATE_2D, &c, sizeof(c), sizeof(CtrlHdr))) return false;

  g_fb_bytes = static_cast<uint64_t>(g_width) * g_height * 4;
  g_fb = static_cast<uint32_t*>(metal_alloc_pages(g_fb_bytes));
  if (!g_fb) {
    kprintf("[gpu] no room for a %u x %u framebuffer\n", g_width, g_height);
    return false;
  }

  /* The frame allocator hands back one contiguous run, so the backing store is
   * a single memory entry; a scatter list would go here otherwise. */
  auto* ab = reinterpret_cast<AttachBacking*>(g_cmd);
  zero(g_cmd, sizeof(AttachBacking) + sizeof(MemEntry));
  ab->resource_id = RESOURCE_ID;
  ab->nr_entries = 1;
  auto* me = reinterpret_cast<MemEntry*>(g_cmd + sizeof(AttachBacking));
  me->addr = reinterpret_cast<uint64_t>(g_fb);
  me->length = static_cast<uint32_t>(g_fb_bytes);
  me->padding = 0;
  {
    auto* hdr = reinterpret_cast<CtrlHdr*>(g_cmd);
    hdr->type = CMD_RESOURCE_ATTACH_BACKING;
    hdr->flags = 0;
    hdr->fence_id = 0;
    hdr->ctx_id = 0;
    hdr->padding = 0;
    zero(g_resp, sizeof(CtrlHdr));
    const VirtioBuf chain[2] = {
        {reinterpret_cast<uint64_t>(g_cmd), sizeof(AttachBacking) + sizeof(MemEntry), false},
        {reinterpret_cast<uint64_t>(g_resp), sizeof(CtrlHdr), true},
    };
    if (virtio_request(g_dev, 0, chain, 2) < 0) return false;
    if (reinterpret_cast<const CtrlHdr*>(g_resp)->type != RESP_OK_NODATA) {
      kputs("[gpu] the device refused our backing store\n");
      return false;
    }
  }

  SetScanout s{};
  s.r = {0, 0, g_width, g_height};
  s.scanout_id = 0;
  s.resource_id = RESOURCE_ID;
  if (!gpu_cmd(CMD_SET_SCANOUT, &s, sizeof(s), sizeof(CtrlHdr))) return false;

  g_live = true;

  char buf[32];
  kprintf("[gpu] virtio-gpu v%u at %p: %u x %u, %s of backing store at %p\n",
          static_cast<uint64_t>(g_dev.version), g_dev.base, g_width, g_height,
          khuman(g_fb_bytes, buf, sizeof(buf)), reinterpret_cast<uint64_t>(g_fb));
  return true;
}
