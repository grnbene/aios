#pragma once
// The machine's state at the moment an interrupt, a fault or a system call took
// it. The order is the order the entry stubs push it in, and the last five
// members are what the processor itself pushed — which is why handing a
// different frame back to the stub resumes a different thread of execution.
#include <cstdint>

#ifdef __aarch64__
struct TrapFrame {
  uint64_t x[31];     // x0-x30
  uint64_t sp_el0;    // Stack pointer EL0
  uint64_t elr_el1;   // Exception Link Register (return address)
  uint64_t spsr_el1;  // Saved Program Status Register
  uint64_t esr_el1;   // Exception Syndrome Register
  uint64_t far_el1;   // Fault Address Register
  uint64_t vector;    // Exception vector index (0-15)
  uint64_t q[32][2];  // 128-bit NEON VFP registers (q0-q31)
};
#else
struct TrapFrame {
  uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
  uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
  uint64_t vector, error_code;
  uint64_t rip, cs, rflags, rsp, ss;
};
#endif

extern "C" {
// Installed by the process layer; null until there is one.
extern TrapFrame* (*metal_tick_hook)(TrapFrame*);
extern TrapFrame* (*metal_syscall_hook)(TrapFrame*);
extern TrapFrame* (*metal_user_fault_hook)(TrapFrame*);

void metal_idt_init();
uint64_t metal_ticks();
uint32_t metal_tick_hz();
bool metal_interrupts_live();
void metal_report_frame(const char* lead, TrapFrame* f);
void metal_halt_until_interrupt();
// Taking the machine down. The command surface used to do this with x86 `outb`
// written inline, which was the ONLY architecture-specific code in the whole
// core+cli tree and the one thing that stopped it compiling for AArch64. The
// mechanism is per-machine — port writes on a PC, a PSCI call on ARM — so it
// belongs behind the same kind of hook `metal_halt_until_interrupt` already is.
[[noreturn]] void metal_power_reboot();
[[noreturn]] void metal_power_off();
uint64_t metal_fault_stack_top();
void metal_use_fault_stack();
// The per-CPU block gs points at: { scratch, kernel_rsp, user_rsp, fpu_area }.
void* metal_cpu_block();
// Move off the legacy PIC onto the IOAPIC, if ACPI describes one.
bool metal_ioapic_init();
bool metal_ioapic_live();
[[noreturn]] void metal_frame_enter(TrapFrame* f);

// setjmp and longjmp for one specific purpose: the console enters the scheduler
// by saving itself here, and a process that exits with nothing left to run
// comes back to exactly this point from inside an interrupt.
#ifdef __aarch64__
struct KernelContext { uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29, lr, sp; };
#else
struct KernelContext { uint64_t rip, rsp, rbp, rbx, r12, r13, r14, r15; };
#endif
int metal_ctx_save(KernelContext* c);
[[noreturn]] void metal_ctx_restore(KernelContext* c);
}
