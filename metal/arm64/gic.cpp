/* GICv2 / GICv3 driver (metal/arm64/gic.cpp).
 *
 * Every register poke goes through mmio.hpp rather than a volatile pointer, so
 * none of it compiles to a writeback addressing mode. See the note there: a
 * post-indexed store to MMIO has no instruction syndrome, and a hypervisor
 * cannot emulate what it cannot decode.
 */
#include "gic.hpp"

#include "fdt.hpp"
#include "kio.hpp"
#include "mmio.hpp"

namespace {

/* Distributor, shared by both versions. */
constexpr uint64_t GICD_CTLR      = 0x0000;
constexpr uint64_t GICD_TYPER     = 0x0004;
constexpr uint64_t GICD_IGROUPR   = 0x0080;
constexpr uint64_t GICD_ISENABLER = 0x0100;
constexpr uint64_t GICD_ICENABLER = 0x0180;
constexpr uint64_t GICD_IPRIORITY = 0x0400;
constexpr uint64_t GICD_ITARGETSR = 0x0800;   /* v2 only */
constexpr uint64_t GICD_SGIR      = 0x0F00;   /* v2 only */
constexpr uint64_t GICD_IROUTER   = 0x6000;   /* v3 only */

/* v2 CPU interface. */
constexpr uint64_t GICC_CTLR = 0x0000;
constexpr uint64_t GICC_PMR  = 0x0004;
constexpr uint64_t GICC_IAR  = 0x000C;
constexpr uint64_t GICC_EOIR = 0x0010;

/* v3 redistributor. */
constexpr uint64_t GICR_CTLR       = 0x0000;
constexpr uint64_t GICR_TYPER      = 0x0008;
constexpr uint64_t GICR_WAKER      = 0x0014;
constexpr uint64_t GICR_STRIDE     = 0x20000;   /* RD_base + SGI_base */
constexpr uint64_t GICR_SGI_OFF    = 0x10000;
constexpr uint64_t GICR_IGROUPR0   = GICR_SGI_OFF + 0x0080;
constexpr uint64_t GICR_ISENABLER0 = GICR_SGI_OFF + 0x0100;
constexpr uint64_t GICR_ICENABLER0 = GICR_SGI_OFF + 0x0180;
constexpr uint64_t GICR_IPRIORITY  = GICR_SGI_OFF + 0x0400;

int g_version = 0;
uint64_t g_gicd = 0;
uint64_t g_gicc = 0;
uint64_t g_gicr = 0;
uint64_t g_my_rd = 0;
uint32_t g_num_int = 32;

/* GICD_CTLR.RWP: a write to enable/routing state is not in force until this
 * clears. Skipping the wait is how an interrupt that was "enabled" never
 * arrives. */
void gicd_wait_rwp() {
  if (g_version != 3) return;
  for (unsigned i = 0; i < 1000000; ++i)
    if ((mmio_read32(g_gicd + GICD_CTLR) & (1u << 31)) == 0) return;
}

void gicr_wait_rwp(uint64_t rd) {
  for (unsigned i = 0; i < 1000000; ++i)
    if ((mmio_read32(rd + GICR_CTLR) & (1u << 3)) == 0) return;
}

/* This core's redistributor frame. The frames are a contiguous array and the
 * last one has GICR_TYPER.Last set; match on the affinity in TYPER[63:32]. */
uint64_t gicr_for_this_cpu() {
  uint64_t mpidr;
  asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
  const uint64_t aff = ((mpidr >> 32) & 0xFFull) << 24 | ((mpidr >> 16) & 0xFFull) << 16 |
                       ((mpidr >> 8) & 0xFFull) << 8 | (mpidr & 0xFFull);
  uint64_t rd = g_gicr;
  for (unsigned i = 0; i < 64; ++i) {
    const uint64_t typer = mmio_read64(rd + GICR_TYPER);
    if (((typer >> 32) & 0xFFFFFFFFull) == aff) return rd;
    if (typer & (1ull << 4)) break;   /* Last */
    rd += GICR_STRIDE;
  }
  return g_gicr;
}

}  /* namespace */

extern "C" {

int metal_gic_version() { return g_version; }

void metal_gic_init() {
  const FdtInfo& fdt = fdt_info();
  g_version = fdt.gic_version;
  g_gicd = fdt.gicd_base;
  g_gicc = fdt.gicc_base;
  g_gicr = fdt.gicr_base;
  if (g_version == 0 || g_gicd == 0) {
    kputs("[gic] no interrupt controller in the device tree\n");
    return;
  }

  const uint32_t typer = mmio_read32(g_gicd + GICD_TYPER);
  g_num_int = ((typer & 0x1F) + 1) * 32;
  if (g_num_int > 1020) g_num_int = 1020;

  if (g_version == 3) {
    /* Affinity routing on, both groups enabled. */
    mmio_write32(g_gicd + GICD_CTLR, (1u << 4) | (1u << 1) | (1u << 0));
    gicd_wait_rwp();
  } else {
    mmio_write32(g_gicd + GICD_CTLR, 1);
  }

  /* Every SPI: group 1, mid priority, masked until something asks for it. */
  for (uint32_t i = 32; i < g_num_int; i += 32) {
    mmio_write32(g_gicd + GICD_IGROUPR + (i / 32) * 4, 0xFFFFFFFFu);
    mmio_write32(g_gicd + GICD_ICENABLER + (i / 32) * 4, 0xFFFFFFFFu);
  }
  for (uint32_t i = 32; i < g_num_int; ++i) {
    mmio_write8(g_gicd + GICD_IPRIORITY + i, 0xA0);
    if (g_version == 2) mmio_write8(g_gicd + GICD_ITARGETSR + i, 0x01);
    else mmio_write64(g_gicd + GICD_IROUTER + i * 8, 0);   /* affinity 0.0.0.0 */
  }
  gicd_wait_rwp();

  metal_gic_init_percpu();

  kprintf("[gic] GICv%d distributor %p, %u interrupt ids\n",
          g_version, g_gicd, static_cast<uint64_t>(g_num_int));
}

void metal_gic_init_percpu() {
  if (g_version == 3) {
    g_my_rd = gicr_for_this_cpu();

    /* Bring the redistributor out of sleep before touching anything in it. */
    uint32_t waker = mmio_read32(g_my_rd + GICR_WAKER);
    waker &= ~(1u << 1);              /* ProcessorSleep = 0 */
    mmio_write32(g_my_rd + GICR_WAKER, waker);
    for (unsigned i = 0; i < 1000000; ++i)
      if ((mmio_read32(g_my_rd + GICR_WAKER) & (1u << 2)) == 0) break;  /* ChildrenAsleep */

    /* SGIs and PPIs are configured in the redistributor, not the distributor. */
    mmio_write32(g_my_rd + GICR_IGROUPR0, 0xFFFFFFFFu);
    mmio_write32(g_my_rd + GICR_ICENABLER0, 0xFFFFFFFFu);
    for (uint32_t i = 0; i < 32; ++i) mmio_write8(g_my_rd + GICR_IPRIORITY + i, 0xA0);
    gicr_wait_rwp(g_my_rd);

    /* System register interface. ICC_SRE_EL1.SRE must be set before any other
     * ICC_* register is even accessible. */
    uint64_t sre;
    asm volatile("mrs %0, s3_0_c12_c12_5" : "=r"(sre));
    asm volatile("msr s3_0_c12_c12_5, %0" :: "r"(sre | 1ull));
    asm volatile("isb");

    asm volatile("msr s3_0_c4_c6_0, %0" :: "r"(0xF0ull));   /* ICC_PMR_EL1 */
    asm volatile("msr s3_0_c12_c12_7, %0" :: "r"(1ull));    /* ICC_IGRPEN1_EL1 */
    asm volatile("isb");
  } else if (g_version == 2) {
    for (uint32_t i = 0; i < 32; ++i) mmio_write8(g_gicd + GICD_IPRIORITY + i, 0xA0);
    mmio_write32(g_gicc + GICC_PMR, 0xF0);
    mmio_write32(g_gicc + GICC_CTLR, 1);
  }
}

void metal_gic_enable_int(uint32_t intid) {
  if (!g_version) return;
  if (g_version == 3 && intid < 32) {
    mmio_write32(g_my_rd + GICR_ISENABLER0, 1u << intid);
    gicr_wait_rwp(g_my_rd);
    return;
  }
  mmio_write32(g_gicd + GICD_ISENABLER + (intid / 32) * 4, 1u << (intid % 32));
  gicd_wait_rwp();
}

void metal_gic_disable_int(uint32_t intid) {
  if (!g_version) return;
  if (g_version == 3 && intid < 32) {
    mmio_write32(g_my_rd + GICR_ICENABLER0, 1u << (intid % 32));
    gicr_wait_rwp(g_my_rd);
    return;
  }
  mmio_write32(g_gicd + GICD_ICENABLER + (intid / 32) * 4, 1u << (intid % 32));
  gicd_wait_rwp();
}

uint32_t metal_gic_ack_int() {
  if (g_version == 3) {
    uint64_t iar;
    asm volatile("mrs %0, s3_0_c12_c12_0" : "=r"(iar));   /* ICC_IAR1_EL1 */
    return static_cast<uint32_t>(iar & 0xFFFFFF);
  }
  if (g_version == 2) return mmio_read32(g_gicc + GICC_IAR) & 0x3FF;
  return 1023;
}

void metal_gic_eoi_int(uint32_t intid) {
  if (g_version == 3) {
    asm volatile("msr s3_0_c12_c12_1, %0" :: "r"(static_cast<uint64_t>(intid)));  /* ICC_EOIR1 */
    asm volatile("isb");
  } else if (g_version == 2) {
    mmio_write32(g_gicc + GICC_EOIR, intid);
  }
}

void metal_gic_send_sgi(uint32_t intid, uint32_t target_cpu) {
  if (g_version == 3) {
    const uint64_t val = (static_cast<uint64_t>(intid & 0xF) << 24) | (1ull << (target_cpu & 0xF));
    asm volatile("msr s3_0_c12_c11_5, %0" :: "r"(val));   /* ICC_SGI1R_EL1 */
    asm volatile("isb");
  } else if (g_version == 2) {
    mmio_write32(g_gicd + GICD_SGIR, ((1u << (target_cpu & 0xFF)) << 16) | (intid & 0xF));
  }
}

}  /* extern "C" */
