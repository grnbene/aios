/* virtio-mmio transport and split virtqueues (metal/arm64/virtio.cpp). */
#include "virtio.hpp"

#include "fdt.hpp"
#include "kio.hpp"
#include "mmio.hpp"
#include "mmu.hpp"
#include "timer.hpp"

namespace {

constexpr uint64_t R_MAGIC         = 0x000;
constexpr uint64_t R_VERSION       = 0x004;
constexpr uint64_t R_DEVICE_ID     = 0x008;
constexpr uint64_t R_DEV_FEAT      = 0x010;
constexpr uint64_t R_DEV_FEAT_SEL  = 0x014;
constexpr uint64_t R_DRV_FEAT      = 0x020;
constexpr uint64_t R_DRV_FEAT_SEL  = 0x024;
constexpr uint64_t R_GUEST_PGSZ    = 0x028;   /* v1 */
constexpr uint64_t R_QUEUE_SEL     = 0x030;
constexpr uint64_t R_QUEUE_NUM_MAX = 0x034;
constexpr uint64_t R_QUEUE_NUM     = 0x038;
constexpr uint64_t R_QUEUE_ALIGN   = 0x03C;   /* v1 */
constexpr uint64_t R_QUEUE_PFN     = 0x040;   /* v1 */
constexpr uint64_t R_QUEUE_READY   = 0x044;   /* v2 */
constexpr uint64_t R_QUEUE_NOTIFY  = 0x050;
constexpr uint64_t R_INT_STATUS    = 0x060;
constexpr uint64_t R_INT_ACK       = 0x064;
constexpr uint64_t R_STATUS        = 0x070;
constexpr uint64_t R_Q_DESC_LO     = 0x080;   /* v2 */
constexpr uint64_t R_Q_DESC_HI     = 0x084;
constexpr uint64_t R_Q_DRIVER_LO   = 0x090;
constexpr uint64_t R_Q_DRIVER_HI   = 0x094;
constexpr uint64_t R_Q_DEVICE_LO   = 0x0A0;
constexpr uint64_t R_Q_DEVICE_HI   = 0x0A4;
constexpr uint64_t R_CONFIG        = 0x100;

constexpr uint32_t VIRTIO_MAGIC = 0x74726976;   /* "virt" */

constexpr uint32_t S_ACK         = 1;
constexpr uint32_t S_DRIVER      = 2;
constexpr uint32_t S_DRIVER_OK   = 4;
constexpr uint32_t S_FEATURES_OK = 8;
constexpr uint32_t S_FAILED      = 128;

constexpr uint64_t F_VERSION_1 = 1ull << 32;

constexpr uint16_t VRING_DESC_F_NEXT  = 1;
constexpr uint16_t VRING_DESC_F_WRITE = 2;

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

void barrier() { asm volatile("dsb sy" ::: "memory"); }

}  /* namespace */

uint32_t virtio_config32(const VirtioDevice& d, uint32_t offset) {
  return mmio_read32(d.base + R_CONFIG + offset);
}

bool virtio_open(uint32_t device_id, VirtioDevice& out) {
  const FdtInfo& f = fdt_info();
  for (uint32_t i = 0; i < f.virtio_count; ++i) {
    const uint64_t base = f.virtio[i].base;
    if (mmio_read32(base + R_MAGIC) != VIRTIO_MAGIC) continue;
    if (mmio_read32(base + R_DEVICE_ID) != device_id) continue;

    out = VirtioDevice{};
    out.base = base;
    out.version = mmio_read32(base + R_VERSION);
    out.device_id = device_id;
    out.intid = f.virtio[i].intid;

    mmio_write32(base + R_STATUS, 0);                    /* reset */
    mmio_write32(base + R_STATUS, S_ACK);
    mmio_write32(base + R_STATUS, S_ACK | S_DRIVER);

    mmio_write32(base + R_DEV_FEAT_SEL, 0);
    uint64_t feat = mmio_read32(base + R_DEV_FEAT);
    mmio_write32(base + R_DEV_FEAT_SEL, 1);
    feat |= static_cast<uint64_t>(mmio_read32(base + R_DEV_FEAT)) << 32;
    out.features = feat;

    /* Accept nothing optional. VIRTIO_F_VERSION_1 is required by a version 2
     * transport and must NOT be set on a legacy one. */
    const uint64_t want = (out.version >= 2) ? (feat & F_VERSION_1) : 0;
    mmio_write32(base + R_DRV_FEAT_SEL, 0);
    mmio_write32(base + R_DRV_FEAT, static_cast<uint32_t>(want));
    mmio_write32(base + R_DRV_FEAT_SEL, 1);
    mmio_write32(base + R_DRV_FEAT, static_cast<uint32_t>(want >> 32));

    if (out.version >= 2) {
      mmio_write32(base + R_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK);
      if (!(mmio_read32(base + R_STATUS) & S_FEATURES_OK)) {
        mmio_write32(base + R_STATUS, S_FAILED);
        kprintf("[virtio] %p refused the feature set\n", base);
        continue;
      }
    }

    out.present = true;
    return true;
  }
  return false;
}

bool virtio_queue_init(VirtioDevice& d, unsigned qidx, uint16_t want_size) {
  if (qidx >= 2) return false;
  mmio_write32(d.base + R_QUEUE_SEL, qidx);
  const uint32_t max = mmio_read32(d.base + R_QUEUE_NUM_MAX);
  if (max == 0) return false;
  const uint16_t qsize = want_size <= max ? want_size : static_cast<uint16_t>(max);

  const uint64_t desc_bytes  = sizeof(VringDesc) * qsize;
  const uint64_t avail_bytes = 6 + 2ull * qsize;
  const uint64_t used_off    = align_up(desc_bytes + avail_bytes, 4096);
  const uint64_t used_bytes  = 6 + 8ull * qsize;
  const uint64_t total       = used_off + used_bytes;

  auto* mem = static_cast<uint8_t*>(metal_alloc_pages(align_up(total, 4096)));
  if (!mem) return false;

  VirtQueue& q = d.q[qidx];
  q.size  = qsize;
  q.desc  = reinterpret_cast<VringDesc*>(mem);
  q.avail = reinterpret_cast<VringAvail*>(mem + desc_bytes);
  q.used  = reinterpret_cast<VringUsed*>(mem + used_off);
  q.avail_idx = 0;
  q.used_seen = 0;

  mmio_write32(d.base + R_QUEUE_NUM, qsize);
  if (d.version == 1) {
    mmio_write32(d.base + R_GUEST_PGSZ, 4096);
    mmio_write32(d.base + R_QUEUE_ALIGN, 4096);
    /* Legacy hands the device one page frame number for the whole ring, which
     * is why the used ring had to be page aligned inside the allocation. */
    mmio_write32(d.base + R_QUEUE_PFN, static_cast<uint32_t>(reinterpret_cast<uint64_t>(mem) >> 12));
  } else {
    const uint64_t pd = reinterpret_cast<uint64_t>(q.desc);
    const uint64_t pa = reinterpret_cast<uint64_t>(q.avail);
    const uint64_t pu = reinterpret_cast<uint64_t>(q.used);
    mmio_write32(d.base + R_Q_DESC_LO, static_cast<uint32_t>(pd));
    mmio_write32(d.base + R_Q_DESC_HI, static_cast<uint32_t>(pd >> 32));
    mmio_write32(d.base + R_Q_DRIVER_LO, static_cast<uint32_t>(pa));
    mmio_write32(d.base + R_Q_DRIVER_HI, static_cast<uint32_t>(pa >> 32));
    mmio_write32(d.base + R_Q_DEVICE_LO, static_cast<uint32_t>(pu));
    mmio_write32(d.base + R_Q_DEVICE_HI, static_cast<uint32_t>(pu >> 32));
    mmio_write32(d.base + R_QUEUE_READY, 1);
  }

  q.avail->flags = 0;
  q.avail->idx = 0;
  q.used->flags = 0;
  q.used->idx = 0;
  return true;
}

void virtio_start(VirtioDevice& d) {
  mmio_write32(d.base + R_STATUS, d.version >= 2
      ? (S_ACK | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK)
      : (S_ACK | S_DRIVER | S_DRIVER_OK));
}

int64_t virtio_request(VirtioDevice& d, unsigned qidx, const VirtioBuf* bufs, unsigned n) {
  if (!d.present || qidx >= 2 || n == 0) return -1;
  VirtQueue& q = d.q[qidx];
  if (!q.size || n > q.size) return -1;

  for (unsigned i = 0; i < n; ++i) {
    q.desc[i].addr = bufs[i].addr;
    q.desc[i].len = bufs[i].len;
    q.desc[i].flags = static_cast<uint16_t>((bufs[i].device_writes ? VRING_DESC_F_WRITE : 0) |
                                            (i + 1 < n ? VRING_DESC_F_NEXT : 0));
    q.desc[i].next = static_cast<uint16_t>(i + 1);
  }

  q.avail->ring[q.avail_idx % q.size] = 0;
  barrier();                        /* descriptors visible before idx moves */
  q.avail->idx = static_cast<uint16_t>(q.avail_idx + 1);
  ++q.avail_idx;
  barrier();

  mmio_write32(d.base + R_QUEUE_NOTIFY, qidx);

  const uint64_t deadline = metal_now_ns() + 2000000000ull;
  while (q.used->idx == q.used_seen) {
    if (metal_now_ns() > deadline) return -1;
    asm volatile("yield" ::: "memory");
    barrier();
  }
  const uint32_t len = q.used->ring[q.used_seen % q.size].len;
  q.used_seen = q.used->idx;

  const uint32_t isr = mmio_read32(d.base + R_INT_STATUS);
  if (isr) mmio_write32(d.base + R_INT_ACK, isr);

  return static_cast<int64_t>(len);
}
