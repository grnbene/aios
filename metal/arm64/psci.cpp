/* PSCI and SMP primitives (metal/arm64/psci.cpp).
 *
 * The conduit is HVC or SMC depending on what the firmware published in the
 * device tree's psci node. Guessing HVC on a machine booted straight to EL1
 * with no hypervisor raises an undefined-instruction exception instead of
 * starting a core, so the "method" property is not optional reading.
 */
#include "psci.hpp"

#include "fdt.hpp"

namespace {

constexpr uint32_t PSCI_VERSION_FN   = 0x84000000;
constexpr uint32_t PSCI_CPU_ON_64    = 0xC4000003;
constexpr uint32_t PSCI_AFFINITY_64  = 0xC4000004;
constexpr uint32_t PSCI_SYSTEM_OFF   = 0x84000008;
constexpr uint32_t PSCI_SYSTEM_RESET = 0x84000009;

int64_t psci_call(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3) {
  /* Decide the conduit BEFORE binding the argument registers. x0-x3 are
   * call-clobbered, so a function call placed between the assignments and the
   * asm destroys the arguments — which is exactly what reading the conduit
   * inside the `if` did: every call arrived at the firmware with a garbage
   * function id and came back NOT_SUPPORTED. */
  const bool use_hvc = fdt_info().psci_use_hvc;

  register uint64_t x0 asm("x0") = fn;
  register uint64_t x1 asm("x1") = a1;
  register uint64_t x2 asm("x2") = a2;
  register uint64_t x3 asm("x3") = a3;
  if (use_hvc) {
    asm volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
  } else {
    asm volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
  }
  return static_cast<int64_t>(x0);
}

}  /* namespace */

extern "C" {

uint32_t metal_psci_version() {
  if (!fdt_info().psci_present) return 0;
  return static_cast<uint32_t>(psci_call(PSCI_VERSION_FN, 0, 0, 0));
}

int metal_psci_cpu_on(uint64_t target_mpidr, uint64_t entry_point, uint64_t context_id) {
  if (!fdt_info().psci_present) return -1;
  return static_cast<int>(psci_call(PSCI_CPU_ON_64, target_mpidr, entry_point, context_id));
}

int metal_psci_affinity_info(uint64_t target_mpidr) {
  if (!fdt_info().psci_present) return -1;
  return static_cast<int>(psci_call(PSCI_AFFINITY_64, target_mpidr, 0, 0));
}

[[noreturn]] void metal_psci_system_off() {
  psci_call(PSCI_SYSTEM_OFF, 0, 0, 0);
  for (;;) asm volatile("wfe");
}

[[noreturn]] void metal_psci_system_reset() {
  psci_call(PSCI_SYSTEM_RESET, 0, 0, 0);
  for (;;) asm volatile("wfe");
}

/* Acquire needs load-acquire exclusive so nothing inside the critical section
 * is speculated ahead of it; release needs store-release so everything in it
 * is visible before the lock drops. */
void metal_spinlock_lock(volatile uint32_t* lock) {
  uint32_t old;
  uint32_t fail;
  asm volatile(
      "1: ldaxr   %w0, [%2]\n"
      "   cbnz    %w0, 2f\n"
      "   stxr    %w1, %w3, [%2]\n"
      "   cbnz    %w1, 1b\n"
      "   b       3f\n"
      "2: wfe\n"
      "   b       1b\n"
      "3:\n"
      : "=&r"(old), "=&r"(fail)
      : "r"(lock), "r"(1u)
      : "memory");
}

bool metal_spinlock_try(volatile uint32_t* lock) {
  uint32_t old;
  uint32_t fail;
  asm volatile(
      "   ldaxr   %w0, [%2]\n"
      "   cbnz    %w0, 1f\n"
      "   stxr    %w1, %w3, [%2]\n"
      "   b       2f\n"
      "1: mov     %w1, #1\n"
      "   clrex\n"
      "2:\n"
      : "=&r"(old), "=&r"(fail)
      : "r"(lock), "r"(1u)
      : "memory");
  return old == 0 && fail == 0;
}

void metal_spinlock_unlock(volatile uint32_t* lock) {
  asm volatile("stlr wzr, [%0]\n  sev" :: "r"(lock) : "memory");
}

}  /* extern "C" */
