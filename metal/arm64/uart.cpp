/* PL011 UART and Raspberry Pi Mini-UART console (metal/arm64/uart.cpp).
 *
 * Brought up before the MMU and before the device tree is parsed.
 * Supports Raspberry Pi 3 (0x3F201000), Raspberry Pi 4 (0xFE201000), and QEMU (0x09000000).
 */
#include "uart.hpp"

#include "mmio.hpp"

namespace {

constexpr uint64_t UART_DR    = 0x00;
constexpr uint64_t UART_FR    = 0x18;
constexpr uint64_t UART_IBRD  = 0x24;
constexpr uint64_t UART_FBRD  = 0x28;
constexpr uint64_t UART_LCR_H = 0x2C;
constexpr uint64_t UART_CR    = 0x30;
constexpr uint64_t UART_IMSC  = 0x38;
constexpr uint64_t UART_ICR   = 0x44;

constexpr uint32_t FR_TXFF = 1u << 5;
constexpr uint32_t FR_RXFE = 1u << 4;
constexpr uint32_t FR_BUSY = 1u << 3;

/* Raspberry Pi GPIO registers */
constexpr uint64_t RPI3_GPIO_BASE = 0x3F200000ull;
constexpr uint64_t RPI4_GPIO_BASE = 0xFE200000ull;

/* Mini UART registers */
constexpr uint64_t RPI3_AUX_ENABLES = 0x3F215004ull;
constexpr uint64_t RPI3_AUX_MU_IO   = 0x3F215040ull;
constexpr uint64_t RPI3_AUX_MU_IER  = 0x3F215044ull;
constexpr uint64_t RPI3_AUX_MU_IIR  = 0x3F215048ull;
constexpr uint64_t RPI3_AUX_MU_LCR  = 0x3F21504Cull;
constexpr uint64_t RPI3_AUX_MU_MCR  = 0x3F215050ull;
constexpr uint64_t RPI3_AUX_MU_LSR  = 0x3F215054ull;
constexpr uint64_t RPI3_AUX_MU_CNTL = 0x3F215060ull;
constexpr uint64_t RPI3_AUX_MU_BAUD = 0x3F215068ull;

uint64_t g_uart = 0x3F201000ull; /* Default to Pi 3 UART0 */
bool g_is_rpi = true;
void (*g_mirror)(const char*, size_t) = nullptr;

void rpi_gpio_uart_setup(uint64_t gpio_base) {
  /* Set GPIO 14 and 15 to Alt0 (PL011 UART0: function 4 = 0b100) */
  uint32_t gpfsel1 = mmio_read32(gpio_base + 0x04);
  gpfsel1 &= ~((7u << 12) | (7u << 15));
  gpfsel1 |= (4u << 12) | (4u << 15);
  mmio_write32(gpio_base + 0x04, gpfsel1);

  /* Disable pull up/down on pins 14 and 15 */
  mmio_write32(gpio_base + 0x94, 0);
  for (int i = 0; i < 150; ++i) { asm volatile("nop"); }
  mmio_write32(gpio_base + 0x98, (1u << 14) | (1u << 15));
  for (int i = 0; i < 150; ++i) { asm volatile("nop"); }
  mmio_write32(gpio_base + 0x98, 0);
}

void rpi_miniuart_init() {
  mmio_write32(RPI3_AUX_ENABLES, mmio_read32(RPI3_AUX_ENABLES) | 1);
  mmio_write32(RPI3_AUX_MU_CNTL, 0);
  mmio_write32(RPI3_AUX_MU_LCR, 3); /* 8-bit mode */
  mmio_write32(RPI3_AUX_MU_MCR, 0);
  mmio_write32(RPI3_AUX_MU_IER, 0);
  mmio_write32(RPI3_AUX_MU_IIR, 0xC6);
  mmio_write32(RPI3_AUX_MU_BAUD, 270); /* 115200 @ 250MHz */
  mmio_write32(RPI3_AUX_MU_CNTL, 3); /* TX & RX enable */
}

}  /* namespace */

extern "C" {

void metal_uart_init(uint64_t mmio_base) {
  if (mmio_base) g_uart = mmio_base;

  if (g_uart == 0x3F201000ull) {
    g_is_rpi = true;
    rpi_gpio_uart_setup(RPI3_GPIO_BASE);
    rpi_miniuart_init();
  } else if (g_uart == 0xFE201000ull) {
    g_is_rpi = true;
    rpi_gpio_uart_setup(RPI4_GPIO_BASE);
  } else {
    g_is_rpi = false;
  }

  mmio_write32(g_uart + UART_CR, 0);        /* off while reconfiguring */
  mmio_write32(g_uart + UART_ICR, 0x7FF);   /* clear every pending condition */

  if (g_is_rpi) {
    /* 115200 baud from 48 MHz UART reference clock on Raspberry Pi:
     * 48e6 / (16 * 115200) = 26.04166 -> IBRD 26, FBRD round(0.04166*64) = 3 */
    mmio_write32(g_uart + UART_IBRD, 26);
    mmio_write32(g_uart + UART_FBRD, 3);
  } else {
    /* 115200 baud from 24 MHz reference clock on QEMU virt: */
    mmio_write32(g_uart + UART_IBRD, 13);
    mmio_write32(g_uart + UART_FBRD, 1);
  }

  mmio_write32(g_uart + UART_LCR_H, (3u << 5) | (1u << 4));   /* 8n1, FIFOs on */
  mmio_write32(g_uart + UART_IMSC, 0);                        /* polled */
  mmio_write32(g_uart + UART_CR, (1u << 0) | (1u << 8) | (1u << 9));  /* EN|TXE|RXE */
}

void metal_uart_rx_interrupt(bool on) {
  mmio_write32(g_uart + UART_IMSC, on ? (1u << 4) : 0);   /* RXIM */
  mmio_write32(g_uart + UART_ICR, 1u << 4);
}

void metal_console_mirror(void (*sink)(const char*, size_t)) { g_mirror = sink; }

/* Boot log ring.
 *
 * The UART is this kernel's only diagnostic channel and it needs a USB-TTL
 * cable nobody has to hand. Every byte the kernel prints is kept here too, so
 * the desktop can render the boot log on the HDMI output - which is the one
 * output an operator with no cable, no keyboard and no network still has. */
namespace {
constexpr size_t KLOG_LINES = 96;
constexpr size_t KLOG_COLS  = 118;
char   g_klog[KLOG_LINES][KLOG_COLS];
size_t g_klog_head  = 0;   /* line currently being filled */
size_t g_klog_col   = 0;
size_t g_klog_count = 0;   /* total lines committed, may exceed KLOG_LINES */

void klog_putc(char c) {
  if (c == '\r') return;
  if (c == '\n') {
    g_klog[g_klog_head][g_klog_col] = '\0';
    g_klog_head = (g_klog_head + 1) % KLOG_LINES;
    g_klog_col = 0;
    g_klog[g_klog_head][0] = '\0';
    ++g_klog_count;
    return;
  }
  if (g_klog_col + 1 < KLOG_COLS) {
    g_klog[g_klog_head][g_klog_col++] = c;
    g_klog[g_klog_head][g_klog_col] = '\0';
  }
}
}  /* namespace */

size_t klog_line_count() {
  return g_klog_count < KLOG_LINES ? g_klog_count : KLOG_LINES;
}

const char* klog_line(size_t i) {
  size_t have = klog_line_count();
  if (i >= have) return "";
  /* i == 0 is the oldest retained line */
  size_t oldest = (g_klog_head + KLOG_LINES - have) % KLOG_LINES;
  return g_klog[(oldest + i) % KLOG_LINES];
}

/* Bounded TX waits. These spun forever: a UART whose FIFO never drains
 * (unclocked, flow-controlled, nobody listening on a netbooted board) turned
 * any kputs() - including the fault reporter's - into a machine hang. A UART
 * that times out is marked stuck and skipped rather than paid for per byte. */
static bool g_pl011_stuck = false, g_mu_stuck = false;
static bool pl011_wait_tx() {
  if (g_pl011_stuck) return false;
  for (uint32_t i = 0; i < 200000; ++i)
    if ((mmio_read32(g_uart + UART_FR) & FR_TXFF) == 0) return true;
  g_pl011_stuck = true;
  return false;
}
static bool mu_wait_tx() {
  if (g_mu_stuck) return false;
  for (uint32_t i = 0; i < 200000; ++i)
    if (mmio_read32(RPI3_AUX_MU_LSR) & 0x20) return true;
  g_mu_stuck = true;
  return false;
}

void metal_console_write(const char* s, size_t n) {
  for (size_t i = 0; i < n; ++i) klog_putc(s[i]);
  for (size_t i = 0; i < n; ++i) {
    if (s[i] == '\n') {
      if (pl011_wait_tx()) mmio_write32(g_uart + UART_DR, '\r');
      if (g_is_rpi && g_uart == 0x3F201000ull) {
        if (mu_wait_tx()) mmio_write32(RPI3_AUX_MU_IO, '\r');
      }
    }
    if (pl011_wait_tx()) mmio_write32(g_uart + UART_DR, static_cast<uint8_t>(s[i]));
    if (g_is_rpi && g_uart == 0x3F201000ull) {
      if (mu_wait_tx()) mmio_write32(RPI3_AUX_MU_IO, static_cast<uint8_t>(s[i]));
    }
  }
  if (g_mirror) g_mirror(s, n);
}

int metal_console_read() {
  if ((mmio_read32(g_uart + UART_FR) & FR_RXFE) == 0) {
    return static_cast<int>(mmio_read32(g_uart + UART_DR) & 0xFF);
  }
  if (g_is_rpi && g_uart == 0x3F201000ull) {
    if (mmio_read32(RPI3_AUX_MU_LSR) & 0x01) {
      return static_cast<int>(mmio_read32(RPI3_AUX_MU_IO) & 0xFF);
    }
  }
  return -1;
}

bool metal_console_pending() {
  if ((mmio_read32(g_uart + UART_FR) & FR_RXFE) == 0) return true;
  if (g_is_rpi && g_uart == 0x3F201000ull) {
    if (mmio_read32(RPI3_AUX_MU_LSR) & 0x01) return true;
  }
  return false;
}

void metal_console_flush() {
  while (mmio_read32(g_uart + UART_FR) & FR_BUSY) {}
}

}  /* extern "C" */
