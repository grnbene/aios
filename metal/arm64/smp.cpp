/* Secondary core bringup over PSCI (metal/arm64/smp.cpp).
 *
 * A core started by CPU_ON re-enters _start with the MMU off and its own
 * SCTLR/TTBR and GIC redistributor untouched, so everything core 0 did to
 * itself has to be done again here, per core, before it can be counted online.
 */
#include "fdt.hpp"
#include "gic.hpp"
#include "kio.hpp"
#include "mmu.hpp"
#include "psci.hpp"
#include "timer.hpp"

extern "C" char _start[];

namespace {

volatile uint64_t g_online = 1;        /* bit N set once core N reports in */
volatile uint32_t g_online_lock = 0;

/* ---- hal::Thread's dispatch inbox, C-ABI only ----------------------------
 * This file stays freestanding (-nostdinc++, no STL) because arm64_secondary_
 * main runs with its own core's MMU off until metal_mmu_init_percpu() returns
 * - the same -mstrict-align justification core 0's own boot has. std::
 * function needs real STL, so it cannot live here; a plain function pointer
 * + opaque context can, and that is the entire seam hal::Thread (implemented
 * in metal/arm64/hal_thread_arm64.cpp, which DOES get the STL flags) needs to
 * hand a real job to an idle secondary core - the same job-inbox design
 * metal/smp.cpp already proved for x86's LAPIC/SIPI cores, adapted to PSCI. */
struct Inbox {
  void (*fn)(void*);
  void* ctx;
  volatile int32_t ready;   /* 1 once fn/ctx are safe to read (release-published) */
  volatile int32_t done;    /* 1 once fn(ctx) has returned */
  volatile int32_t busy;    /* 1 from reservation until the caller reaps it */
  volatile int32_t live;    /* 1 once this core has entered its dispatch loop */
  volatile int32_t detached;/* 1: no one will reap it; the next reservation does, once done */
};
constexpr unsigned MAX_SMP_CORES = 8;
Inbox g_inbox[MAX_SMP_CORES];
void* g_core_tls[MAX_SMP_CORES] = {};   /* built by core 0 in smp_start */

}  /* namespace */

uint64_t g_online_at_ns[8];     /* when each core marked itself online (cpus) */
/* Bring-up progress per core, one cache line each, cleaned to memory after
 * every write so core 0 can read it even from a core whose MMU or
 * coherency is not up yet. */
struct alignas(64) CoreStep { volatile uint64_t step; uint64_t pad[7]; };
CoreStep g_core_step[8];
inline void core_step(uint64_t cpu, uint64_t step) {
  if (cpu >= 8) return;
  g_core_step[cpu].step = step;
  asm volatile("dc civac, %0; dsb sy" :: "r"(&g_core_step[cpu]) : "memory");
}
uint64_t g_released_at_ns[8];   /* when smp_start released it */

/* `cpus test`: the same job on every secondary core at once, then on core 0
 * one after another - proof the cores run work, and what it buys. */
namespace {
volatile uint64_t g_test_result[8];
uint64_t spin_fib(uint64_t n) { return n < 2 ? n : spin_fib(n - 1) + spin_fib(n - 2); }
void test_job(void* ctx) {
  const uint64_t cpu = reinterpret_cast<uint64_t>(ctx);
  g_test_result[cpu & 7] = spin_fib(27);
}
}  // namespace
extern "C" int smp_thread_reserve();
extern "C" void smp_thread_dispatch(int slot, void (*fn)(void*), void* ctx);
extern "C" int smp_thread_done(int slot);
extern "C" void smp_thread_reap(int slot);
void smp_test() {
  int slots[8]; int n = 0;
  for (int s; n < 7 && (s = smp_thread_reserve()) >= 0;) slots[n++] = s;
  if (n == 0) { kputs("  cpus test: no secondary core is online (run `smp`)\n"); return; }
  const uint64_t t0 = metal_now_ns();
  for (int k = 0; k < n; ++k) smp_thread_dispatch(slots[k], test_job, reinterpret_cast<void*>(static_cast<uint64_t>(slots[k])));
  for (int k = 0; k < n; ++k) while (!smp_thread_done(slots[k])) asm volatile("yield");
  const uint64_t par = metal_now_ns() - t0;
  for (int k = 0; k < n; ++k) smp_thread_reap(slots[k]);
  const uint64_t t1 = metal_now_ns();
  for (int k = 0; k < n; ++k) test_job(reinterpret_cast<void*>(static_cast<uint64_t>(0)));
  const uint64_t ser = metal_now_ns() - t1;
  bool ok = true;
  for (int k = 0; k < n; ++k) ok = ok && g_test_result[slots[k]] == 196418;
  kprintf("  cpus test: fib(27) x%u - on %u cores at once %u ms, one after another on core 0 %u ms (%s)\n",
          static_cast<uint64_t>(n), static_cast<uint64_t>(n), par / 1000000ull, ser / 1000000ull,
          ok ? "every core's answer right" : "A CORE GOT IT WRONG");
}

extern "C" int smp_thread_reserve() {
  for (unsigned i = 1; i < MAX_SMP_CORES; ++i) {
    if (!g_inbox[i].live) continue;
    if (__atomic_load_n(&g_inbox[i].detached, __ATOMIC_ACQUIRE) && __atomic_load_n(&g_inbox[i].done, __ATOMIC_ACQUIRE)) {
      __atomic_store_n(&g_inbox[i].detached, 0, __ATOMIC_RELEASE);
      __atomic_store_n(&g_inbox[i].ready, 0, __ATOMIC_RELEASE);
      __atomic_store_n(&g_inbox[i].busy, 0, __ATOMIC_RELEASE);
    }
    int32_t expected = 0;
    if (__atomic_compare_exchange_n(&g_inbox[i].busy, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}
extern "C" void smp_thread_dispatch(int slot, void (*fn)(void*), void* ctx) {
  g_inbox[slot].done = 0;
  g_inbox[slot].fn = fn;
  g_inbox[slot].ctx = ctx;
  __atomic_store_n(&g_inbox[slot].ready, 1, __ATOMIC_RELEASE);
  asm volatile("dsb ish; sev" ::: "memory");   /* the core waits in wfe */
}
extern "C" int smp_thread_done(int slot) {
  return __atomic_load_n(&g_inbox[slot].done, __ATOMIC_ACQUIRE);
}
extern "C" void smp_thread_detach(int slot) { __atomic_store_n(&g_inbox[slot].detached, 1, __ATOMIC_RELEASE); }
extern "C" void smp_thread_reap(int slot) {
  __atomic_store_n(&g_inbox[slot].ready, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&g_inbox[slot].busy, 0, __ATOMIC_RELEASE);
}

extern "C" uint64_t metal_ticks_cpu(unsigned cpu);

uint64_t smp_online_mask() { return g_online; }

unsigned smp_online_count() {
  return static_cast<unsigned>(__builtin_popcountll(g_online));
}

extern "C" void* metal_make_tls_block();

extern "C" void arm64_secondary_main(uint64_t cpu) {
  core_step(cpu, 1);
  metal_mmu_init_percpu();
  core_step(cpu, 2);
  /* This core's thread-local storage, prepared by core 0 before the wake. */
  if (cpu < MAX_SMP_CORES && g_core_tls[cpu]) asm volatile("msr tpidr_el0, %0" :: "r"(g_core_tls[cpu]) : "memory");
  core_step(cpu, 3);
  metal_gic_init_percpu();
  core_step(cpu, 4);
  metal_timer_init_percpu();
  core_step(cpu, 5);
  metal_gic_enable_int(metal_timer_intid());
  asm volatile("msr daifclr, #2" ::: "memory");
  core_step(cpu, 6);

  metal_spinlock_lock(&g_online_lock);
  core_step(cpu, 7);
  g_online |= (1ull << (cpu & 63));
  metal_spinlock_unlock(&g_online_lock);
  if (cpu < 8) g_online_at_ns[cpu] = metal_now_ns();
  core_step(cpu, 8);

  if (cpu < MAX_SMP_CORES) __atomic_store_n(&g_inbox[cpu].live, 1, __ATOMIC_RELEASE);

  /* Idle with the MMU and its own timer running, checking for hal::Thread
   * work between naps - `smp` can still show the core executing rather than
   * parked in the firmware's holding pen, and its tick count is kept by the
   * trap handler either way. A dispatched job runs to completion before the
   * core goes back to waiting; there is no preemption between jobs on a
   * single core, which is fine for what this substrate actually spawns
   * threads for (one relay/keepalive loop per core, not a general scheduler). */
  for (;;) {
    if (cpu < MAX_SMP_CORES && __atomic_load_n(&g_inbox[cpu].ready, __ATOMIC_ACQUIRE)) {
      g_inbox[cpu].fn(g_inbox[cpu].ctx);
      __atomic_store_n(&g_inbox[cpu].done, 1, __ATOMIC_RELEASE);
      __atomic_store_n(&g_inbox[cpu].ready, 0, __ATOMIC_RELEASE);
    } else {
      /* wfe, not wfi: nothing here raises an interrupt on the Pi (no GIC),
       * so a wfi core never woke to see a dispatched job. dispatch sev's. */
      asm volatile("wfe");
    }
  }
}

extern "C" void spin_entry();

void smp_report() {
  kprintf("  online mask 0x%x (%u cores)\n", g_online, static_cast<uint64_t>(__builtin_popcountll(g_online)));
  for (unsigned i = 1; i < 4; ++i)
  {
    asm volatile("dc civac, %0; dsb sy" :: "r"(&g_core_step[i]) : "memory");
    kprintf("  cpu%u: released at %u ms, online at %u ms, bring-up step %u/8\n", static_cast<uint64_t>(i),
            g_released_at_ns[i] / 1000000ull, g_online_at_ns[i] / 1000000ull, g_core_step[i].step);
  }
}

namespace {
inline void clean_to_poc(const void* p) { asm volatile("dc civac, %0" :: "r"(p) : "memory"); }

/* Pi 3: the firmware's armstub parks cores 1-3 reading 0xe0, 0xe8, 0xf0 for
 * an entry address (the device tree's "spin-table" enable method). A released
 * core runs with its caches off until it turns its MMU on, so everything it
 * reads before then - the release slot, g_tcr/g_ttbr - has to be pushed out
 * of core 0's caches to memory first. */
void smp_start_spin_table(const FdtInfo& f) {
  metal_mmu_publish_percpu();
  const unsigned n = f.cpu_count > 4 ? 4u : static_cast<unsigned>(f.cpu_count ? f.cpu_count : 4);
  for (unsigned i = 1; i < n; ++i) {
    if (g_online & (1ull << i)) { kprintf("  cpu%u already online\n", static_cast<uint64_t>(i)); continue; }
    if (i < MAX_SMP_CORES && !g_core_tls[i]) g_core_tls[i] = metal_make_tls_block();
    /* The armstub's table is indexed from core 0: 0xd8 is core 0's own
     * slot, core i waits on 0xd8 + 8*i (the device tree's cpu-release-addr:
     * 0xe0, 0xe8, 0xf0). Writing 0xd8 + 8*(i-1) released each core one
     * release late and core 3 never. */
    volatile uint64_t* slot = reinterpret_cast<volatile uint64_t*>(0xd8ull + 8ull * i);
    /* store; dsb; clean the line to memory (the parked core reads with its
     * caches off); dsb; sev. */
    *slot = reinterpret_cast<uint64_t>(&spin_entry);
    g_released_at_ns[i] = metal_now_ns();
    asm volatile("dsb sy" ::: "memory");
    clean_to_poc(const_cast<uint64_t*>(slot));
    asm volatile("dsb sy; sev" ::: "memory");
    /* It runs its boot path with caches off until its MMU is on - slow. */
    const uint64_t t0 = metal_now_ns(), deadline = t0 + 3000000000ull;
    /* Keep signalling until it answers - cheap insurance against a core
     * that looked at its slot just before the store and went back to sleep. */
    while (!(__atomic_load_n(&g_online, __ATOMIC_ACQUIRE) & (1ull << i)) && metal_now_ns() < deadline) {
      asm volatile("dsb sy; sev" ::: "memory");
      metal_delay_us(50);
    }
    const bool up = (__atomic_load_n(&g_online, __ATOMIC_ACQUIRE) & (1ull << i)) != 0;
    asm volatile("dc civac, %0; dsb sy" :: "r"(&g_core_step[i]) : "memory");
    asm volatile("dc civac, %0; dsb sy" :: "r"(slot) : "memory");
    kprintf("  cpu%u (spin table): %s after %u ms (its step %u, slot now %x)\n", static_cast<uint64_t>(i),
            up ? "online with its own mmu and timer" : "did not come up", (metal_now_ns() - t0) / 1000000ull,
            g_core_step[i].step, *slot);
  }
}
}  // namespace

void smp_start() {
  const FdtInfo& f = fdt_info();
  if (!f.psci_present) {
    smp_start_spin_table(f);
    return;
  }
  const uint32_t v = metal_psci_version();
  kprintf("  psci v%u.%u over %s, %u cpus in the device tree\n",
          static_cast<uint64_t>(v >> 16), static_cast<uint64_t>(v & 0xFFFF),
          f.psci_use_hvc ? "hvc" : "smc", static_cast<uint64_t>(f.cpu_count));

  for (uint32_t i = 1; i < f.cpu_count && i < 8; ++i) {
    if (g_online & (1ull << i)) {
      kprintf("  cpu%u already online\n", static_cast<uint64_t>(i));
      continue;
    }
    const uint64_t mpidr = f.cpu_mpidr[i];
    if (i < MAX_SMP_CORES && !g_core_tls[i]) g_core_tls[i] = metal_make_tls_block();
    const int rc = metal_psci_cpu_on(mpidr, reinterpret_cast<uint64_t>(_start), i);
    if (rc != 0 && rc != -4 /* ALREADY_ON */) {
      kprintf("  cpu%u mpidr %p: CPU_ON returned %d\n", static_cast<uint64_t>(i), mpidr,
              static_cast<int64_t>(rc));
      continue;
    }
    /* Wait for the core to set its own bit rather than for CPU_ON to return:
     * the call succeeding only means the firmware accepted the request. */
    const uint64_t deadline = metal_now_ns() + 200000000ull;
    while (!(g_online & (1ull << i)) && metal_now_ns() < deadline) asm volatile("yield");
    kprintf("  cpu%u mpidr %p: %s\n", static_cast<uint64_t>(i), mpidr,
            (g_online & (1ull << i)) ? "online with its own mmu and timer"
                                     : "accepted by psci but never reported in");
  }

  kprintf("  online mask %p, %u cores\n", g_online,
          static_cast<uint64_t>(smp_online_count()));
  for (unsigned i = 0; i < 8; ++i)
    if (g_online & (1ull << i))
      kprintf("    cpu%u has taken %u timer interrupts on its own timer\n",
              static_cast<uint64_t>(i), metal_ticks_cpu(i));
}
