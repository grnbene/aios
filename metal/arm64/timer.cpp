/* ARM Generic Timer, EL1 physical (metal/arm64/timer.cpp).
 *
 * The physical timer's control registers are CNTP_*_EL0, not CNTP_*_EL1 —
 * the _EL1 spellings are not system registers at all and the assembler rejects
 * them. The counter frequency is read from CNTFRQ_EL0 rather than assumed;
 * QEMU virt runs it at 62.5 MHz, hardware does not.
 */
#include "timer.hpp"

#include "fdt.hpp"

namespace {
uint32_t g_hz = 100;
uint64_t g_cntfrq = 0;
uint64_t g_reload = 0;
uint64_t g_boot_count = 0;
uint32_t g_intid = 30;

/* CNTP_CVAL_EL0 is per core, so the deadline that tracks it has to be too —
 * one shared deadline has four cores overwriting each other's next tick. */
uint64_t g_deadline[8] = {};

unsigned this_cpu() {
  uint64_t mpidr;
  asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
  return static_cast<unsigned>(mpidr & 7);
}
}

extern "C" {

void metal_timer_init(uint32_t hz) {
  g_hz = hz ? hz : 100;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(g_cntfrq));
  if (!g_cntfrq) g_cntfrq = 62500000;
  g_reload = g_cntfrq / g_hz;
  g_intid = static_cast<uint32_t>(fdt_info().timer_intid_phys);
  asm volatile("mrs %0, cntpct_el0" : "=r"(g_boot_count));

  /* Absolute deadlines through CNTP_CVAL, not a countdown through CNTP_TVAL.
   * Reloading TVAL after the handler has run restarts the interval from
   * whenever the handler finished, so every tick leaks the handler's own
   * latency and the clock runs slow — measurably 82 Hz where 100 was asked
   * for. Advancing a deadline cannot drift. */
  uint64_t now;
  asm volatile("mrs %0, cntpct_el0" : "=r"(now));
  const unsigned cpu = this_cpu();
  g_deadline[cpu] = now + g_reload;
  asm volatile("msr cntp_cval_el0, %0" :: "r"(g_deadline[cpu]));
  asm volatile("msr cntp_ctl_el0, %0" :: "r"(1ull));   /* ENABLE=1, IMASK=0 */
  asm volatile("isb");
}

/* A secondary core's timer: its own deadline, on the clock core 0 set up.
 * metal_timer_init() re-zeroes the shared monotonic clock (g_boot_count) -
 * a secondary calling it rewound time on every core, and core 0, spinning
 * on a deadline from the old clock, waited as long as the board had been
 * up. */
void metal_timer_init_percpu() {
  uint64_t now;
  asm volatile("mrs %0, cntpct_el0" : "=r"(now));
  const unsigned cpu = this_cpu();
  g_deadline[cpu] = now + g_reload;
  asm volatile("msr cntp_cval_el0, %0" :: "r"(g_deadline[cpu]));
  asm volatile("msr cntp_ctl_el0, %0" :: "r"(1ull));
  asm volatile("isb");
}

void metal_timer_reload() {
  uint64_t now;
  asm volatile("mrs %0, cntpct_el0" : "=r"(now));
  const unsigned cpu = this_cpu();
  g_deadline[cpu] += g_reload;
  /* If the machine was held off long enough to miss whole periods, resync
   * rather than firing a burst of catch-up interrupts. */
  if (g_deadline[cpu] <= now) g_deadline[cpu] = now + g_reload;
  asm volatile("msr cntp_cval_el0, %0" :: "r"(g_deadline[cpu]));
  asm volatile("msr cntp_ctl_el0, %0" :: "r"(1ull));
}

void metal_timer_stop() {
  asm volatile("msr cntp_ctl_el0, %0" :: "r"(0ull));
  asm volatile("isb");
}

uint64_t metal_cntpct() {
  uint64_t v;
  asm volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
  return v;
}

uint64_t metal_cntfrq() {
  if (!g_cntfrq) {
    asm volatile("mrs %0, cntfrq_el0" : "=r"(g_cntfrq));
    if (!g_cntfrq) g_cntfrq = 62500000;
  }
  return g_cntfrq;
}
uint32_t metal_timer_intid() { return g_intid; }

uint64_t metal_now_ns() {
  const uint64_t f = metal_cntfrq();
  const uint64_t ticks = metal_cntpct() - g_boot_count;
  /* Split the multiply so a machine that has been up for an hour does not
   * overflow the numerator before the divide. */
  return (ticks / f) * 1000000000ull + ((ticks % f) * 1000000000ull) / f;
}

void metal_delay_us(uint64_t us) {
  const uint64_t f = metal_cntfrq();
  const uint64_t target = metal_cntpct() + (f * us) / 1000000ull;
  while (metal_cntpct() < target) asm volatile("yield");
}

}  /* extern "C" */
