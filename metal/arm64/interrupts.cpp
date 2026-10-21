/* Traps, faults and the tick on AArch64 (metal/arm64/interrupts.cpp).
 *
 * All sixteen vectors land in metal_trap with the frame the stub built and the
 * vector index stamped into it. The frame metal_trap RETURNS is the one that
 * gets eret'd into, which is how a timer interrupt taken in one process can
 * resume a different one.
 */
#include "net.hpp"
#include "aios/metal/trap.hpp"

#include "fdt.hpp"
#include "gic.hpp"
#include "mmio.hpp"
#include "kio.hpp"
#include "psci.hpp"
#include "timer.hpp"
#include "uart.hpp"

namespace {

/* Per core. A single shared counter is both a race between four cores and a
 * lie to whoever reads it: the shell measuring "ticks per second" would see
 * the whole machine's interrupt rate and call it the tick rate. */
volatile uint64_t g_ticks[8] = {};
uint32_t g_hz = 100;
bool g_live = false;
uint64_t g_spurious = 0;
uint64_t g_faults = 0;

unsigned this_cpu() {
  uint64_t mpidr;
  asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
  return static_cast<unsigned>(mpidr & 7);
}

/* ESR_EL1 exception classes this kernel has something to say about. */
const char* ec_name(uint32_t ec) {
  switch (ec) {
    case 0x00: return "unknown";
    case 0x07: return "SIMD/FP access trapped";
    case 0x0E: return "illegal execution state";
    case 0x15: return "SVC (AArch64)";
    case 0x18: return "MSR/MRS trapped";
    case 0x20: return "instruction abort, lower EL";
    case 0x21: return "instruction abort, same EL";
    case 0x22: return "PC alignment fault";
    case 0x24: return "data abort, lower EL";
    case 0x25: return "data abort, same EL";
    case 0x26: return "SP alignment fault";
    case 0x2C: return "floating point, AArch64";
    case 0x3C: return "BRK instruction";
    default:   return "unhandled exception class";
  }
}

const char* dfsc_name(uint32_t dfsc) {
  switch (dfsc & 0x3F) {
    case 0x04: case 0x05: case 0x06: case 0x07: return "translation fault";
    case 0x09: case 0x0A: case 0x0B: return "access flag fault";
    case 0x0D: case 0x0E: case 0x0F: return "permission fault";
    case 0x10: return "external abort";
    case 0x21: return "alignment fault";
    default: return "abort";
  }
}

const char* vector_name(uint64_t v) {
  static const char* kind[4] = {"sync", "irq", "fiq", "serror"};
  static const char* origin[4] = {"EL1t", "EL1h", "EL0/64", "EL0/32"};
  static char buf[24];
  const char* k = kind[v & 3];
  const char* o = origin[(v >> 2) & 3];
  unsigned i = 0;
  while (*o && i < sizeof(buf) - 8) buf[i++] = *o++;
  buf[i++] = ' ';
  while (*k && i < sizeof(buf) - 1) buf[i++] = *k++;
  buf[i] = '\0';
  return buf;
}

}  /* namespace */

extern "C" {

TrapFrame* (*metal_tick_hook)(TrapFrame*) = nullptr;
TrapFrame* (*metal_syscall_hook)(TrapFrame*) = nullptr;
TrapFrame* (*metal_user_fault_hook)(TrapFrame*) = nullptr;

void metal_idt_init() {
  metal_gic_init();
  metal_timer_init(g_hz);
  if (metal_gic_version() > 0) {
    metal_gic_enable_int(metal_timer_intid());
    asm volatile("msr daifclr, #2" ::: "memory");   /* unmask IRQ */
  }
  g_live = true;
}

uint64_t metal_ticks()            { return g_ticks[this_cpu()]; }
uint64_t metal_ticks_cpu(unsigned c) { return g_ticks[c & 7]; }
uint32_t metal_tick_hz()      { return g_hz; }
bool metal_interrupts_live()  { return g_live; }
uint64_t metal_spurious()     { return g_spurious; }
uint64_t metal_fault_count()  { return g_faults; }

void metal_halt_until_interrupt() { asm volatile("wfi"); }

/* On QEMU virt the level above us is PSCI and SYSTEM_RESET/SYSTEM_OFF work.
 * On a Raspberry Pi there is no EL3 PSCI handler in the stock armstub: the
 * SMC never returns and the board sat in `wfe` forever - "reset" from the
 * console hung the machine until someone pulled the plug. The Pi's reset
 * lever is the BCM2835 power-management watchdog, which is what Linux's
 * bcm2835-wdt writes; halt is the same reset with RSTS partition 63, which
 * the firmware treats as "stay off". */
namespace {
constexpr uint32_t PM_PASSWORD          = 0x5A000000u;
constexpr uint64_t PM_RSTC              = 0x1C;
constexpr uint64_t PM_RSTS              = 0x20;
constexpr uint64_t PM_WDOG              = 0x24;
constexpr uint32_t PM_RSTC_WRCFG_CLR    = 0xFFFFFFCFu;
constexpr uint32_t PM_RSTC_WRCFG_FULL   = 0x00000020u;
constexpr uint32_t PM_RSTS_PARTITION_63 = 0x00000555u;

bool pi_pm_base(uint64_t& base) {
  const FdtInfo& f = fdt_info();
  if (f.valid && f.uart_base == 0xFE201000ull) { base = 0xFE100000ull; return true; } /* Pi 4 */
  if (!f.valid || f.uart_base == 0x3F201000ull || !f.psci_present) { base = 0x3F100000ull; return true; } /* Pi 3 */
  return false; /* virt: PSCI is real there */
}

[[noreturn]] void pi_watchdog_reset(bool halt) {
  uint64_t pm = 0;
  pi_pm_base(pm);
  if (halt) {
    const uint32_t rsts = mmio_read32(pm + PM_RSTS);
    mmio_write32(pm + PM_RSTS, PM_PASSWORD | rsts | PM_RSTS_PARTITION_63);
  }
  mmio_write32(pm + PM_WDOG, PM_PASSWORD | 10u); /* ~10 ticks of the 65 kHz watchdog */
  const uint32_t rstc = mmio_read32(pm + PM_RSTC);
  mmio_write32(pm + PM_RSTC, PM_PASSWORD | (rstc & PM_RSTC_WRCFG_CLR) | PM_RSTC_WRCFG_FULL);
  for (;;) asm volatile("wfe");
}
}  // namespace

[[noreturn]] void metal_power_reboot() {
  uint64_t pm = 0;
  if (pi_pm_base(pm)) pi_watchdog_reset(false);
  metal_psci_system_reset();
}
[[noreturn]] void metal_power_off() {
  uint64_t pm = 0;
  if (pi_pm_base(pm)) pi_watchdog_reset(true);
  metal_psci_system_off();
}

/* Hardware watchdog as a hang breaker: armed once the main loop is running,
 * petted from the loop and from net_poll(). If the loop stops for ~16 s
 * (a hang, not a slow frame) the PM block resets the board, which then
 * netboots - no one has to walk to the power cable. 0xFFFFF ticks of the
 * 65536 Hz watchdog clock is the maximum, ~16 s. */
namespace { bool g_wdog_armed = false; }
extern "C" void metal_watchdog_pet() {
  if (!g_wdog_armed) return;
  uint64_t pm = 0;
  if (!pi_pm_base(pm)) return;
  mmio_write32(pm + PM_WDOG, PM_PASSWORD | 0xFFFFFu);
}
extern "C" void metal_watchdog_arm() {
  uint64_t pm = 0;
  if (!pi_pm_base(pm)) return;
  /* QEMU's raspi PM model resets IMMEDIATELY on this write instead of
   * counting down, so arming there kills every emulated boot. Real BCM2837
   * silicon runs the generic timer at 19.2 MHz; QEMU at 62.5 MHz. */
  if (metal_cntfrq() != 19200000u) {
    kputs("[wdog] not real Pi silicon (timer != 19.2 MHz) - watchdog not armed\n");
    return;
  }
  mmio_write32(pm + PM_WDOG, PM_PASSWORD | 0xFFFFFu);
  const uint32_t rstc = mmio_read32(pm + PM_RSTC);
  mmio_write32(pm + PM_RSTC, PM_PASSWORD | (rstc & PM_RSTC_WRCFG_CLR) | PM_RSTC_WRCFG_FULL);
  g_wdog_armed = true;
  kputs("[wdog] hardware watchdog armed: ~15 s without a main-loop pass resets the board\n");
}

extern "C" void crashlog(const char* line);

namespace {
/* Fault summary for the SD crash log: the screen dies with the board. */
void crashlog_fault(const char* lead, const TrapFrame* f) {
  char b[160];
  size_t n = 0;
  auto put = [&](const char* s) { while (*s && n + 1 < sizeof(b)) b[n++] = *s++; };
  auto hex = [&](uint64_t v) {
    put("0x");
    for (int i = 60; i >= 0; i -= 4) { const unsigned d = (v >> i) & 0xF; if (n + 1 < sizeof(b)) b[n++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10); }
  };
  put("FAULT "); put(lead ? lead : "trap");
  put(" pc="); hex(f->elr_el1);
  put(" esr="); hex(f->esr_el1);
  put(" far="); hex(f->far_el1);
  put(" lr="); hex(f->x[30]);
  b[n] = 0;
  crashlog(b);
  /* crashlog() only queues the breadcrumb; nothing drains the TX ring once
   * we panic, so push it out now or the fault is invisible. */
  net_breadcrumb_flush();
}
}  // namespace

void metal_report_frame(const char* lead, TrapFrame* f) {
  if (!f) return;
  const uint32_t ec = static_cast<uint32_t>((f->esr_el1 >> 26) & 0x3F);
  kprintf("\n%s: %s [%s]\n", lead ? lead : "trap", ec_name(ec), vector_name(f->vector));
  kprintf("  ESR  %016x  EC %02x  ISS %06x\n",
          f->esr_el1, static_cast<uint64_t>(ec), f->esr_el1 & 0x1FFFFFFull);
  if (ec == 0x24 || ec == 0x25 || ec == 0x20 || ec == 0x21) {
    kprintf("  FAR  %016x  %s%s\n", f->far_el1,
            dfsc_name(static_cast<uint32_t>(f->esr_el1 & 0x3F)),
            (f->esr_el1 & (1ull << 6)) ? " on write" : " on read");
  }
  kprintf("  ELR  %016x  SPSR %016x  SP_EL0 %016x\n",
          f->elr_el1, f->spsr_el1, f->sp_el0);
  for (unsigned i = 0; i < 31; i += 4) {
    kprintf("  x%u%s", static_cast<uint64_t>(i), i < 10 ? " " : "");
    for (unsigned k = i; k < i + 4 && k < 31; ++k) kprintf(" %016x", f->x[k]);
    kputs("\n");
  }
}

[[noreturn]] void metal_panic(const char* what) {
  asm volatile("msr daifset, #2" ::: "memory");
  /* Out over the network first: a panic that only reached the serial port
   * looked like a silent freeze (the STL-heap exhaustion that hung the
   * desktop did exactly that). */
  {
    char b[160] = "PANIC ";
    size_t n = 6;
    for (const char* q = what ? what : "(no reason given)"; *q && n + 1 < sizeof(b); ++q) b[n++] = *q;
    b[n] = 0;
    crashlog(b);
    net_breadcrumb_flush();
  }
  kprintf("\n[panic] %s\n", what ? what : "(no reason given)");
  metal_console_flush();
  for (;;) asm volatile("wfe");
}

TrapFrame* metal_trap(TrapFrame* f) {
  if (!f) metal_panic("trap with no frame");

  const uint64_t vec = f->vector;
  const uint32_t ec  = static_cast<uint32_t>((f->esr_el1 >> 26) & 0x3F);
  const bool from_el0 = (f->spsr_el1 & 0xF) == 0;

  /* IRQ is vector index 1 within each of the four groups. */
  if ((vec & 3) == 1) {
    const uint32_t intid = metal_gic_ack_int();
    if (intid >= 1020) { ++g_spurious; return f; }   /* spurious: no EOI */

    if (intid == metal_timer_intid()) {
      g_ticks[this_cpu()] = g_ticks[this_cpu()] + 1;
      metal_timer_reload();
      metal_gic_eoi_int(intid);
      if (metal_tick_hook) f = metal_tick_hook(f);
      return f;
    }
    metal_gic_eoi_int(intid);
    return f;
  }

  if (ec == 0x15) {   /* SVC */
    if (metal_syscall_hook) return metal_syscall_hook(f);
    f->x[0] = static_cast<uint64_t>(-1);
    return f;
  }

  ++g_faults;

  if (from_el0) {
    /* A process died. That is not the machine's problem, so long as the
     * scheduler is there to take the CPU back. */
    if (metal_user_fault_hook) return metal_user_fault_hook(f);
    /* Network breadcrumb first: the UART/fbcon report below can itself
     * stall, and then nothing would ever leave the board. */
    crashlog_fault("EL0 fault, no handler", f);
    metal_report_frame("user fault with no handler", f);
    metal_panic("EL0 fault and no scheduler to return to");
  }

  crashlog_fault("kernel exception", f);
  metal_report_frame("kernel exception", f);
  metal_panic("unhandled exception at EL1");
}

}  /* extern "C" */
