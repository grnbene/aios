#pragma once
/* Memory-mapped I/O accessors.
 *
 * These are inline asm, not pointer dereferences, and that is not stylistic.
 * A loop over a `volatile uint8_t*` lets the compiler pick post-indexed
 * addressing — `strb w2, [x0], #1` — and the architecture leaves ESR.ISV
 * CLEAR for any access that writes back its base register. A hypervisor
 * trapping that MMIO is then handed a data abort with no instruction syndrome:
 * it cannot tell which register, what width, or which direction, so it cannot
 * emulate the access at all.
 *
 * QEMU's HVF backend reacts to that by aborting outright
 * (`assert(isv)` in hvf_handle_exception), which is how this was found: the
 * GICv3 priority loop compiled to a post-indexed STRB and killed the VM the
 * moment the kernel ran under Apple's hypervisor instead of under emulation.
 * Under TCG the same code is fine, because TCG never needs the syndrome.
 *
 * This is the same reason Linux's readl/writel are inline asm. Every register
 * poke in this kernel goes through here.
 */
#include <cstdint>

static inline void mmio_write8(uint64_t addr, uint8_t v) {
  asm volatile("strb %w0, [%1]" : : "rZ"(v), "r"(addr) : "memory");
}
static inline void mmio_write16(uint64_t addr, uint16_t v) {
  asm volatile("strh %w0, [%1]" : : "rZ"(v), "r"(addr) : "memory");
}
static inline void mmio_write32(uint64_t addr, uint32_t v) {
  asm volatile("str %w0, [%1]" : : "rZ"(v), "r"(addr) : "memory");
}
static inline void mmio_write64(uint64_t addr, uint64_t v) {
  asm volatile("str %x0, [%1]" : : "rZ"(v), "r"(addr) : "memory");
}

static inline uint8_t mmio_read8(uint64_t addr) {
  uint32_t v;
  asm volatile("ldrb %w0, [%1]" : "=r"(v) : "r"(addr) : "memory");
  return static_cast<uint8_t>(v);
}
static inline uint16_t mmio_read16(uint64_t addr) {
  uint32_t v;
  asm volatile("ldrh %w0, [%1]" : "=r"(v) : "r"(addr) : "memory");
  return static_cast<uint16_t>(v);
}
static inline uint32_t mmio_read32(uint64_t addr) {
  uint32_t v;
  asm volatile("ldr %w0, [%1]" : "=r"(v) : "r"(addr) : "memory");
  return v;
}
static inline uint64_t mmio_read64(uint64_t addr) {
  uint64_t v;
  asm volatile("ldr %x0, [%1]" : "=r"(v) : "r"(addr) : "memory");
  return v;
}
