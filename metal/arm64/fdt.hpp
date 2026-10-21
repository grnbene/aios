#pragma once
/* Flattened device tree discovery.
 *
 * Every address this kernel touches — DRAM, the PL011, the GIC, the virtio
 * transports — comes out of the DTB the firmware left in x0. Hardcoding the
 * QEMU virt constants instead makes a kernel that only ever runs on QEMU virt
 * and lies about it when it does not.
 */
#include <cstddef>
#include <cstdint>

struct FdtVirtio {
  uint64_t base;
  uint64_t size;
  uint32_t intid;   /* GIC INTID, i.e. the SPI number already offset by 32 */
};

struct FdtInfo {
  bool valid;
  uint64_t dtb;
  uint32_t dtb_size;

  uint64_t ram_base;
  uint64_t ram_size;

  uint64_t uart_base;
  uint64_t uart_size;
  uint32_t uart_intid;

  int gic_version;      /* 2 or 3; 0 when no interrupt controller was found */
  uint64_t gicd_base;   /* distributor, both versions */
  uint64_t gicc_base;   /* v2 CPU interface */
  uint64_t gicr_base;   /* v3 redistributor */

  uint32_t cpu_count;
  uint64_t cpu_mpidr[8];

  uint64_t timer_intid_phys;   /* PPI for the EL1 physical timer */

  bool psci_present;
  bool psci_use_hvc;           /* PSCI "method" = hvc, else smc */

  /* A framebuffer the firmware already set up and left running. This is how a
   * board whose panel was brought up by U-Boot hands the display over without
   * the kernel needing a display-controller driver of its own. */
  uint64_t fb_base;
  uint64_t fb_size;
  uint32_t fb_width;
  uint32_t fb_height;
  uint32_t fb_stride;          /* bytes per line */
  char fb_format[20];

  uint64_t fwcfg_base;         /* qemu,fw-cfg-mmio, which is how ramfb is set up */

  FdtVirtio virtio[32];
  uint32_t virtio_count;

  char model[64];
};

/* Parse the DTB at `dtb`. Returns false and leaves `out.valid` false when the
 * pointer is not a device tree. */
bool fdt_parse(uint64_t dtb, FdtInfo& out);

/* The tree this kernel booted on. */
const FdtInfo& fdt_info();

/* A Broadcom (BCM283x/2711) board - a Raspberry Pi, or QEMU raspi3b, which
 * may boot with no device tree at all. The Pi drivers (SDHOST, GPIO pads,
 * mailbox) poke fixed addresses in that peripheral window; on another board
 * (QEMU virt: PL011 at 0x09000000) the same reads are external aborts. */
inline bool board_has_bcm_peripherals() {
  const FdtInfo& f = fdt_info();
  return !f.valid || (f.uart_base >> 24) == 0x3F || (f.uart_base >> 24) == 0xFE;
}
