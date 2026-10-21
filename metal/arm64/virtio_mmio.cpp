/* virtio-mmio block device (metal/arm64/virtio_mmio.cpp).
 *
 * The transport and the virtqueue live in virtio.cpp; what is left here is the
 * block protocol itself: a 16-byte header, a data buffer, and a one-byte
 * status the device writes.
 */
#include "virtio.hpp"

#include "kio.hpp"
#include "mmu.hpp"
#include "timer.hpp"

namespace {

constexpr uint32_t VIRTIO_ID_BLOCK = 2;

constexpr uint32_t BLK_T_IN  = 0;
constexpr uint32_t BLK_T_OUT = 1;

constexpr uint64_t F_BLK_RO = 1ull << 5;

struct BlkReqHeader {
  uint32_t type;
  uint32_t reserved;
  uint64_t sector;
};

VirtioDevice g_dev;
uint64_t g_capacity = 0;
bool g_read_only = false;

BlkReqHeader* g_hdr = nullptr;
uint8_t* g_status = nullptr;
uint8_t* g_bounce = nullptr;   /* one sector of DMA-visible scratch */

bool blk_request(uint32_t type, uint64_t sector, uint8_t* buf) {
  if (!g_dev.present) return false;

  g_hdr->type = type;
  g_hdr->reserved = 0;
  g_hdr->sector = sector;
  *g_status = 0xFF;

  if (type == BLK_T_OUT) for (unsigned i = 0; i < 512; ++i) g_bounce[i] = buf[i];

  const VirtioBuf chain[3] = {
      {reinterpret_cast<uint64_t>(g_hdr), sizeof(BlkReqHeader), false},
      {reinterpret_cast<uint64_t>(g_bounce), 512, type == BLK_T_IN},
      {reinterpret_cast<uint64_t>(g_status), 1, true},
  };
  if (virtio_request(g_dev, 0, chain, 3) < 0) return false;

  if (type == BLK_T_IN) for (unsigned i = 0; i < 512; ++i) buf[i] = g_bounce[i];
  return *g_status == 0;
}

}  /* namespace */

void blk_init() {
  if (!virtio_open(VIRTIO_ID_BLOCK, g_dev)) {
    kputs("[blk] virtio-mmio block device not present (using native SD/EMMC)\n");
    return;
  }
  g_read_only = (g_dev.features & F_BLK_RO) != 0;
  g_capacity = static_cast<uint64_t>(virtio_config32(g_dev, 0)) |
               (static_cast<uint64_t>(virtio_config32(g_dev, 4)) << 32);

  if (!virtio_queue_init(g_dev, 0, 8)) {
    kprintf("[blk] %p has no usable virtqueue\n", g_dev.base);
    g_dev.present = false;
    return;
  }

  g_hdr    = static_cast<BlkReqHeader*>(metal_alloc_pages(4096));
  g_bounce = static_cast<uint8_t*>(metal_alloc_pages(4096));
  if (!g_hdr || !g_bounce) { g_dev.present = false; return; }
  g_status = reinterpret_cast<uint8_t*>(g_hdr) + 2048;

  virtio_start(g_dev);

  char buf[32];
  kprintf("[blk] virtio-mmio v%u block device at %p: %s, %u sectors%s\n",
          static_cast<uint64_t>(g_dev.version), g_dev.base,
          khuman(g_capacity * 512, buf, sizeof(buf)), g_capacity,
          g_read_only ? ", read only" : "");
}

void blk_report() {
  if (!g_dev.present) {
    kputs("  no block device. Start QEMU with -drive ... -device virtio-blk-device.\n");
    return;
  }
  char buf[32];
  kprintf("  virtio-mmio v%u at %p, intid %u\n", static_cast<uint64_t>(g_dev.version),
          g_dev.base, static_cast<uint64_t>(g_dev.intid));
  kprintf("  features %p, capacity %u sectors (%s)%s\n", g_dev.features, g_capacity,
          khuman(g_capacity * 512, buf, sizeof(buf)), g_read_only ? ", read only" : "");

  auto* page = static_cast<uint8_t*>(metal_alloc_pages(4096));
  auto* back = static_cast<uint8_t*>(metal_alloc_pages(4096));
  auto* check = static_cast<uint8_t*>(metal_alloc_pages(4096));
  if (!page || !back || !check) { kputs("  out of memory\n"); return; }

  const uint64_t sector = g_capacity > 64 ? 64 : 1;

  /* Read what is there first, so the write below can be put back. */
  uint64_t t0 = metal_now_ns();
  const bool r0 = blk_request(BLK_T_IN, sector, back);
  const uint64_t t_read = metal_now_ns() - t0;
  kprintf("  read  sector %u: %s in %u us\n", sector, r0 ? "ok" : "FAILED", t_read / 1000);
  if (!r0) return;

  if (g_read_only) { kputs("  device is read only; not writing\n"); return; }

  const uint64_t stamp = metal_cntpct();
  for (unsigned i = 0; i < 512; ++i) page[i] = static_cast<uint8_t>(i * 7 + (stamp & 0xFF));

  t0 = metal_now_ns();
  const bool w = blk_request(BLK_T_OUT, sector, page);
  const uint64_t t_write = metal_now_ns() - t0;
  kprintf("  write sector %u: %s in %u us\n", sector, w ? "ok" : "FAILED", t_write / 1000);
  if (!w) return;

  for (unsigned i = 0; i < 512; ++i) check[i] = 0;
  const bool r1 = blk_request(BLK_T_IN, sector, check);
  unsigned diff = 0;
  for (unsigned i = 0; i < 512; ++i) if (check[i] != page[i]) ++diff;
  kprintf("  read back: %s, %u of 512 bytes differ\n", r1 ? "ok" : "FAILED",
          static_cast<uint64_t>(diff));
  kprintf("  first bytes written %02x %02x %02x %02x, read %02x %02x %02x %02x\n",
          static_cast<uint64_t>(page[0]), static_cast<uint64_t>(page[1]),
          static_cast<uint64_t>(page[2]), static_cast<uint64_t>(page[3]),
          static_cast<uint64_t>(check[0]), static_cast<uint64_t>(check[1]),
          static_cast<uint64_t>(check[2]), static_cast<uint64_t>(check[3]));

  /* Put the sector back the way it was found. */
  blk_request(BLK_T_OUT, sector, back);
  kputs("  original sector contents restored\n");

  metal_free_pages(page, 4096);
  metal_free_pages(back, 4096);
  metal_free_pages(check, 4096);
}
