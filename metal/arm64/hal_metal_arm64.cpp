/* aios::hal implementations for the ARM64 bare-metal substrate - the start
 * of the same role metal/hal_metal.cpp plays for x86 bare metal: the glue
 * between src/'s portable code and this specific machine's primitives.
 * Only what's needed to link and run src/entity (and whatever else follows
 * it) lives here so far; this grows as more of src/ gets pulled in. */
#include "aios/hal/sync.hpp"
#include <cstdint>

namespace aios::hal {

// Ticket spinlock: correct uncontended (the only case today - one core is
// online per `info`) and correct once smp_start() brings up the others.
// `yield` is AArch64's hint that this core is spinning, not a real wait -
// same role x86's `pause` plays in the reference implementation this was
// ported from (metal/hal_metal.cpp).
Mutex::Mutex() { for (auto& b : storage_) b = 0; }
Mutex::~Mutex() {}
void Mutex::lock() {
  auto* next = reinterpret_cast<volatile uint32_t*>(storage_);
  auto* serving = reinterpret_cast<volatile uint32_t*>(storage_ + 4);
  uint32_t ticket = __atomic_fetch_add(next, 1, __ATOMIC_ACQ_REL);
  while (__atomic_load_n(serving, __ATOMIC_ACQUIRE) != ticket) asm volatile("yield");
}
void Mutex::unlock() {
  auto* serving = reinterpret_cast<volatile uint32_t*>(storage_ + 4);
  __atomic_fetch_add(serving, 1, __ATOMIC_RELEASE);
}

// This core's id for RecursiveMutex: MPIDR_EL1's affinity bits.
static uint64_t metal_cpu_token() { uint64_t v; asm volatile("mrs %0, mpidr_el1" : "=r"(v)); return v & 0xffffff; }
#define AIOS_CPU_RELAX() asm volatile("yield")
#include "../sync_common.inc"

}  // namespace aios::hal
