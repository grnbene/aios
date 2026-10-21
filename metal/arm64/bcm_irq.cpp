/* Broadcom BCM2836 / BCM2837 Interrupt Controller Implementation (metal/arm64/bcm_irq.cpp).
 */
#include "bcm_irq.hpp"

#include "fdt.hpp"
#include "kio.hpp"
#include "mmio.hpp"

namespace {

/* BCM2837 Peripheral Interrupt Controller (ARM Control block) */
constexpr uint64_t RPI3_IRQ_BASE   = 0x3F00B200ull;
constexpr uint64_t RPI4_IRQ_BASE   = 0xFE00B200ull;

constexpr uint64_t IRQ_BASIC_PEND  = 0x00;
constexpr uint64_t IRQ_PEND1       = 0x04;
constexpr uint64_t IRQ_PEND2       = 0x08;
constexpr uint64_t FIQ_CTRL        = 0x0C;
constexpr uint64_t IRQ_ENABLE1     = 0x10;
constexpr uint64_t IRQ_ENABLE2     = 0x14;
constexpr uint64_t IRQ_ENABLE_BASIC= 0x18;
constexpr uint64_t IRQ_DISABLE1    = 0x1C;
constexpr uint64_t IRQ_DISABLE2    = 0x20;
constexpr uint64_t IRQ_DISABLE_BASIC = 0x24;

/* BCM2836/2837 Core Local Interrupt Controller */
constexpr uint64_t RPI3_LOCAL_BASE = 0x40000000ull;
constexpr uint64_t CORE0_TIMER_IRQ = 0x40;
constexpr uint64_t CORE0_IRQ_SRC   = 0x60;

/* Key Peripheral IRQ Numbers (GPU IRQ 0-63) */
constexpr uint32_t IRQ_GPU_TIMER0  = 0;
constexpr uint32_t IRQ_GPU_TIMER1  = 1;
constexpr uint32_t IRQ_GPU_TIMER2  = 2;
constexpr uint32_t IRQ_GPU_TIMER3  = 3;
constexpr uint32_t IRQ_GPU_USB     = 9;   /* DWC2 USB host controller */
constexpr uint32_t IRQ_GPU_AUX     = 29;  /* Mini-UART / SPI1 / SPI2 */
constexpr uint32_t IRQ_GPU_I2C_SPI = 43;
constexpr uint32_t IRQ_GPU_SDIO    = 56;  /* SDHOST / EMMC */
constexpr uint32_t IRQ_GPU_UART0   = 57;  /* PL011 UART */

uint64_t get_irq_base() {
  const FdtInfo& f = fdt_info();
  if (f.valid && f.uart_base == 0xFE201000ull) return RPI4_IRQ_BASE;
  return RPI3_IRQ_BASE;
}

BcmIrqStats g_stats = {};
bool g_initialized = false;

}  /* namespace */

void bcm_irq_init() {
  const uint64_t base = get_irq_base();

  /* Disable all peripheral IRQs initially */
  mmio_write32(base + IRQ_DISABLE1, 0xFFFFFFFFu);
  mmio_write32(base + IRQ_DISABLE2, 0xFFFFFFFFu);
  mmio_write32(base + IRQ_DISABLE_BASIC, 0xFFFFFFFFu);

  /* Enable DWC2 USB (GPU IRQ 9) and SDIO/EMMC (GPU IRQ 56) */
  bcm_irq_enable_gpu(IRQ_GPU_USB);
  bcm_irq_enable_gpu(IRQ_GPU_SDIO);

  g_initialized = true;
}

void bcm_irq_enable_gpu(uint32_t irq_num) {
  const uint64_t base = get_irq_base();
  if (irq_num < 32) {
    mmio_write32(base + IRQ_ENABLE1, 1u << irq_num);
  } else if (irq_num < 64) {
    mmio_write32(base + IRQ_ENABLE2, 1u << (irq_num - 32));
  }
}

void bcm_irq_disable_gpu(uint32_t irq_num) {
  const uint64_t base = get_irq_base();
  if (irq_num < 32) {
    mmio_write32(base + IRQ_DISABLE1, 1u << irq_num);
  } else if (irq_num < 64) {
    mmio_write32(base + IRQ_DISABLE2, 1u << (irq_num - 32));
  }
}

bool bcm_irq_is_gpu_pending(uint32_t irq_num) {
  const uint64_t base = get_irq_base();
  if (irq_num < 32) {
    return (mmio_read32(base + IRQ_PEND1) & (1u << irq_num)) != 0;
  } else if (irq_num < 64) {
    return (mmio_read32(base + IRQ_PEND2) & (1u << (irq_num - 32))) != 0;
  }
  return false;
}

void bcm_irq_dispatch() {
  if (!g_initialized) return;

  const uint64_t base = get_irq_base();
  const uint32_t p1 = mmio_read32(base + IRQ_PEND1);
  const uint32_t p2 = mmio_read32(base + IRQ_PEND2);

  if (p1 & (1u << IRQ_GPU_USB)) {
    ++g_stats.usb_irqs;
    ++g_stats.total_irqs;
  }

  if (p2 & (1u << (IRQ_GPU_SDIO - 32))) {
    ++g_stats.sdio_irqs;
    ++g_stats.total_irqs;
  }

  if (p2 & (1u << (IRQ_GPU_UART0 - 32))) {
    ++g_stats.uart_irqs;
    ++g_stats.total_irqs;
  }
}

const BcmIrqStats& bcm_irq_stats() {
  return g_stats;
}

void bcm_irq_report() {
  kputs("  bcm interrupt controller:\n");
  kprintf("    total irqs  : %u\n", static_cast<uint64_t>(g_stats.total_irqs));
  kprintf("    usb irqs    : %u (GPU IRQ 9)\n", static_cast<uint64_t>(g_stats.usb_irqs));
  kprintf("    sdio irqs   : %u (GPU IRQ 56)\n", static_cast<uint64_t>(g_stats.sdio_irqs));
  kprintf("    uart irqs   : %u (GPU IRQ 57)\n", static_cast<uint64_t>(g_stats.uart_irqs));
}
