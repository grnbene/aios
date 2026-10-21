#pragma once
/* Broadcom BCM2836 / BCM2837 / BCM2711 Local and Peripheral Interrupt Controller
 * (metal/arm64/bcm_irq.hpp).
 *
 * Implements routing and acknowledgment for hardware peripheral IRQs (USB, SDHOST,
 * Mini-UART, PL011, Mailbox) and local ARM core timer interrupts on Raspberry Pi.
 */
#include <cstdint>

struct BcmIrqStats {
  uint64_t total_irqs;
  uint64_t usb_irqs;
  uint64_t timer_irqs;
  uint64_t sdio_irqs;
  uint64_t uart_irqs;
};

/* Initialize Broadcom interrupt controller registers and unmask critical lines */
void bcm_irq_init();

/* Enable / disable specific peripheral IRQ numbers (GPU IRQs 0-63) */
void bcm_irq_enable_gpu(uint32_t irq_num);
void bcm_irq_disable_gpu(uint32_t irq_num);

/* Check if a specific peripheral IRQ is pending */
bool bcm_irq_is_gpu_pending(uint32_t irq_num);

/* Service pending Broadcom IRQs (called from trap/idle handlers) */
void bcm_irq_dispatch();

/* Get interrupt statistics */
const BcmIrqStats& bcm_irq_stats();

/* Shell diagnostic output */
void bcm_irq_report();
