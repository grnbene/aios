#pragma once
#include <cstddef>
#include <cstdint>

extern "C" {
void metal_uart_init(uint64_t mmio_base);
void metal_uart_rx_interrupt(bool on);
void metal_console_write(const char* s, size_t n);
/* Everything written to the console also goes here, once a display exists, so
 * the shell is on the glass and the serial port at the same time. */
void metal_console_mirror(void (*sink)(const char* s, size_t n));

/* Retained boot log, so the kernel's diagnostics can be read off the screen
 * when there is no serial cable. klog_line(0) is the oldest retained line. */
size_t klog_line_count();
const char* klog_line(size_t i);
int metal_console_read();
bool metal_console_pending();
void metal_console_flush();
}
