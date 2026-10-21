// Interrupts, faults and the tick — the thing this machine did not have.
//
// Before this, every driver polled and nothing could take the CPU back: a page
// fault did not report, it simply stopped the machine (that is exactly how the
// unmapped virtio BAR behaved), and there was no clock to preempt anything
// with. An IDT is installed here, the legacy PIC is remapped out of the
// exception range it collides with, the timer runs at 100 Hz, and PS/2 input
// arrives because the controller raised a line rather than because someone
// remembered to look.
//
// The frame the stubs build is deliberately replaceable: metal_trap returns the
// frame to resume, so the scheduler can hand back a different process's frame
// and iretq lands in that one instead.
#include <cstddef>
#include <cstdint>

#include "aios/metal/trap.hpp"

extern "C" void metal_console_write(const char* s, size_t n);
extern "C" [[noreturn]] void metal_panic(const char* what);
extern "C" void* isr_stubs[];
extern "C" const void* metal_acpi_madt();
extern "C" void metal_ps2_isr();

namespace {

inline void outb(uint16_t p, uint8_t v) { asm volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
inline uint8_t inb(uint16_t p) { uint8_t v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
inline void io_wait() { outb(0x80, 0); }

struct __attribute__((packed)) IdtEntry {
  uint16_t offset_low;
  uint16_t selector;
  uint8_t ist;
  uint8_t flags;
  uint16_t offset_mid;
  uint32_t offset_high;
  uint32_t zero;
};
struct __attribute__((packed)) DescriptorPointer { uint16_t limit; uint64_t base; };

IdtEntry g_idt[256];
// The per-CPU block the entry stubs reach through gs. It belongs here, with the
// stubs that use it, and it must be valid BEFORE the first interrupt: the stubs
// save the floating-point register file through it, and a gs base of zero makes
// that a write to whatever address 24 happens to hold.
struct __attribute__((packed)) MetalCpu {
  uint64_t scratch, kernel_rsp, user_rsp;
  void* fpu_area;
};
alignas(64) MetalCpu g_cpu;
alignas(16) uint8_t g_boot_fpu[512];
// A stack of its own for the double fault. The usual way to hit vector 8 is to
// have run out of stack, and a handler that reports on the stack that just
// overflowed produces a triple fault and a silent reboot instead of a message.
alignas(16) uint8_t g_fault_stack[16384];
volatile uint64_t g_ticks = 0;
uint32_t g_hz = 100;

const char* exception_name(uint64_t v) {
  switch (v) {
    case 0: return "divide error";
    case 1: return "debug";
    case 2: return "non-maskable interrupt";
    case 3: return "breakpoint";
    case 4: return "overflow";
    case 5: return "bound range exceeded";
    case 6: return "invalid opcode";
    case 7: return "device not available";
    case 8: return "double fault";
    case 10: return "invalid TSS";
    case 11: return "segment not present";
    case 12: return "stack-segment fault";
    case 13: return "general protection fault";
    case 14: return "page fault";
    case 16: return "x87 floating-point exception";
    case 17: return "alignment check";
    case 18: return "machine check";
    case 19: return "SIMD floating-point exception";
    default: return "exception";
  }
}

void put(const char* s) { size_t n = 0; while (s[n]) ++n; metal_console_write(s, n); }
void put_hex(uint64_t v) {
  char buf[19] = "0x";
  static const char* d = "0123456789abcdef";
  for (int i = 0; i < 16; ++i) buf[2 + i] = d[(v >> (60 - 4 * i)) & 0xF];
  metal_console_write(buf, 18);
}
void put_dec(uint64_t v) {
  char buf[24];
  int n = 0;
  if (!v) buf[n++] = '0';
  while (v) { buf[n++] = (char)('0' + v % 10); v /= 10; }
  for (int i = 0; i < n / 2; ++i) { char t = buf[i]; buf[i] = buf[n - 1 - i]; buf[n - 1 - i] = t; }
  metal_console_write(buf, (size_t)n);
}

void wrmsr(uint32_t msr, uint64_t value) {
  asm volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

void idt_set(int vector, void* handler, uint8_t flags) {
  uint64_t a = (uint64_t)handler;
  g_idt[vector].offset_low = (uint16_t)a;
  g_idt[vector].selector = 0x08;
  g_idt[vector].ist = 0;
  g_idt[vector].flags = flags;
  g_idt[vector].offset_mid = (uint16_t)(a >> 16);
  g_idt[vector].offset_high = (uint32_t)(a >> 32);
  g_idt[vector].zero = 0;
}

// The 8259s wake up delivering IRQ0-15 on vectors 8-15, which are the CPU's own
// exception vectors — a keystroke would arrive indistinguishable from a double
// fault. Move them to 32-47 before a single interrupt is enabled.
void pic_remap() {
  uint8_t mask1 = inb(0x21), mask2 = inb(0xA1);
  outb(0x20, 0x11); io_wait(); outb(0xA0, 0x11); io_wait();
  outb(0x21, 32);   io_wait(); outb(0xA1, 40);   io_wait();
  outb(0x21, 4);    io_wait(); outb(0xA1, 2);    io_wait();
  outb(0x21, 0x01); io_wait(); outb(0xA1, 0x01); io_wait();
  (void)mask1; (void)mask2;
  // Everything masked but the timer, the keyboard, the cascade the mouse's
  // controller hangs off, and the mouse itself.
  outb(0x21, (uint8_t)~((1 << 0) | (1 << 1) | (1 << 2)));
  outb(0xA1, (uint8_t)~(1 << 4));
}

void pit_init(uint32_t hz) {
  uint32_t divisor = 1193182u / hz;
  outb(0x43, 0x36);                          // channel 0, lo/hi, square wave
  outb(0x40, (uint8_t)(divisor & 0xFF));
  outb(0x40, (uint8_t)(divisor >> 8));
}

// Which controller is delivering. The PIC brings the machine up because it
// needs no tables to work; the IOAPIC takes over once ACPI has been read and
// says where it is.
volatile uint32_t* LAPIC = (volatile uint32_t*)0xFEE00000ull;
volatile uint32_t* IOAPIC = nullptr;
bool g_lapic_eoi = false;

void eoi(uint64_t vector) {
  if (g_lapic_eoi) { LAPIC[0xB0 / 4] = 0; return; }
  if (vector >= 40) outb(0xA0, 0x20);
  outb(0x20, 0x20);
}

uint32_t ioapic_read(uint32_t reg) { IOAPIC[0] = reg; return IOAPIC[4]; }
void ioapic_write(uint32_t reg, uint32_t value) { IOAPIC[0] = reg; IOAPIC[4] = value; }

}  // namespace

// Set by the process layer once it exists; while they are null this machine has
// interrupts and faults and nothing to schedule, which is exactly stage one.
extern "C" {
TrapFrame* (*metal_tick_hook)(TrapFrame*) = nullptr;
TrapFrame* (*metal_syscall_hook)(TrapFrame*) = nullptr;
TrapFrame* (*metal_user_fault_hook)(TrapFrame*) = nullptr;
}

extern "C" uint64_t metal_ticks() { return g_ticks; }
extern "C" uint32_t metal_tick_hz() { return g_hz; }
extern "C" bool metal_interrupts_live() { uint64_t f; asm volatile("pushfq; popq %0" : "=r"(f)); return (f & 0x200) != 0; }

extern "C" void metal_report_frame(const char* lead, TrapFrame* f) {
  uint64_t cr2;
  asm volatile("movq %%cr2, %0" : "=r"(cr2));
  put(lead);
  put(exception_name(f->vector));
  put(" (vector ");
  put_dec(f->vector);
  put(", error ");
  put_hex(f->error_code);
  put(")\n         rip=");
  put_hex(f->rip);
  put(" cs=");
  put_hex(f->cs);
  put(" rflags=");
  put_hex(f->rflags);
  put("\n         rsp=");
  put_hex(f->rsp);
  put(" cr2=");
  put_hex(cr2);
  put(" rax=");
  put_hex(f->rax);
  put("\n");
}

// Every vector arrives here, with the machine's whole state in a frame it may
// replace. What comes back is what iretq resumes.
extern "C" TrapFrame* metal_trap(TrapFrame* f) {
  if (f->vector == 256) {                        // the system call gate
    if (metal_syscall_hook) return metal_syscall_hook(f);
    f->rax = (uint64_t)-1;
    return f;
  }
  if (f->vector < 32) {
    // A fault in ring 3 is the process's problem and it is ended; a fault in
    // ring 0 is the kernel's, and the kernel says so instead of stopping dead.
    if ((f->cs & 3) == 3 && metal_user_fault_hook) {
      if (TrapFrame* next = metal_user_fault_hook(f)) return next;
    }
    metal_report_frame("\n[metal] FAULT in the kernel: ", f);
    metal_panic("unhandled processor exception");
  }
  if (f->vector == 255) return f;                // the LAPIC's spurious vector: no acknowledgement is owed
  uint64_t irq = f->vector - 32;
  if (irq == 0) {
    ++g_ticks;
    eoi(f->vector);
    if (metal_tick_hook) return metal_tick_hook(f);
    return f;
  }
  if (irq == 1 || irq == 12) {                   // keyboard, mouse
    metal_ps2_isr();
    eoi(f->vector);
    return f;
  }
  eoi(f->vector);
  return f;
}

extern "C" void metal_idt_init() {
  for (int v = 0; v < 48; ++v) idt_set(v, isr_stubs[v], 0x8E);
  idt_set(255, isr_stubs[48], 0x8E);             // the LAPIC's spurious vector
  DescriptorPointer p{(uint16_t)(sizeof(g_idt) - 1), (uint64_t)&g_idt[0]};
  asm volatile("lidt %0" : : "m"(p));
  // Before a single interrupt: gs must point at a real per-CPU block, with
  // somewhere real to put the floating-point registers of whatever is running.
  g_cpu.fpu_area = g_boot_fpu;
  asm volatile("fxsave (%0)" : : "r"(g_boot_fpu) : "memory");
  wrmsr(0xC0000101, (uint64_t)&g_cpu);      // IA32_GS_BASE
  wrmsr(0xC0000102, 0);                     // IA32_KERNEL_GS_BASE
  pic_remap();
  pit_init(g_hz);
  asm volatile("sti");
}

// Once there is a task-state segment to hold it, the double fault switches to a
// stack that cannot be the one that just overflowed.
extern "C" uint64_t metal_fault_stack_top() { return ((uint64_t)g_fault_stack + sizeof(g_fault_stack)) & ~15ull; }
extern "C" void metal_use_fault_stack() { g_idt[8].ist = 1; }

// Wait for the next tick with the CPU stopped rather than spinning on it. This
// is only correct because there is now something to wake it up.
// Move off the legacy PIC and onto the interrupt controller the machine
// actually has. Everything here is conditional on ACPI describing an IOAPIC:
// if it does not, the PIC keeps the machine running and this says so.
extern "C" bool metal_ioapic_init() {
  const uint8_t* madt = (const uint8_t*)metal_acpi_madt();
  if (!madt) return false;
  uint32_t len = *(const uint32_t*)(madt + 4);
  uint64_t ioapic_base = 0;
  uint32_t gsi_base = 0;
  // An ISA interrupt does not necessarily arrive on the line of the same
  // number: the firmware says so with an override, and QEMU really does move
  // the timer's IRQ 0 onto global interrupt 2.
  struct Override { uint8_t source; uint32_t gsi; uint16_t flags; };
  Override overrides[16];
  int n_over = 0;
  for (const uint8_t* p = madt + 44; p < madt + len && p[1] >= 2; p += p[1]) {
    if (p[0] == 1 && !ioapic_base) { ioapic_base = *(const uint32_t*)(p + 4); gsi_base = *(const uint32_t*)(p + 8); }
    if (p[0] == 2 && n_over < 16) {
      overrides[n_over].source = p[3];
      overrides[n_over].gsi = *(const uint32_t*)(p + 4);
      overrides[n_over].flags = *(const uint16_t*)(p + 8);
      ++n_over;
    }
  }
  if (!ioapic_base) return false;
  IOAPIC = (volatile uint32_t*)ioapic_base;
  uint32_t entries = ((ioapic_read(1) >> 16) & 0xFF) + 1;

  // The local APIC has to be enabled and have somewhere to put a spurious
  // interrupt before anything is routed to it.
  LAPIC[0xF0 / 4] = LAPIC[0xF0 / 4] | 0x100 | 0xFF;
  uint8_t bsp = (uint8_t)(LAPIC[0x20 / 4] >> 24);

  // The 8259s stop delivering entirely. Both of them, every line.
  outb(0x21, 0xFF);
  outb(0xA1, 0xFF);

  auto route = [&](uint8_t isa_irq, uint8_t vector) {
    uint32_t gsi = isa_irq;
    uint16_t flags = 0;
    for (int i = 0; i < n_over; ++i)
      if (overrides[i].source == isa_irq) { gsi = overrides[i].gsi; flags = overrides[i].flags; }
    if (gsi < gsi_base || gsi - gsi_base >= entries) return;
    uint32_t index = 0x10 + 2 * (gsi - gsi_base);
    uint32_t low = vector;                                  // fixed delivery, physical, unmasked
    if ((flags & 0x3) == 0x3) low |= (1u << 13);            // active low
    if ((flags & 0xC) == 0xC) low |= (1u << 15);            // level triggered
    ioapic_write(index + 1, (uint32_t)bsp << 24);
    ioapic_write(index, low);
  };
  route(0, 32);    // the timer
  route(1, 33);    // the keyboard
  route(12, 44);   // the mouse
  g_lapic_eoi = true;
  return true;
}

extern "C" bool metal_ioapic_live() { return g_lapic_eoi; }

// The process layer fills in the kernel stack and the save area; the block
// itself is the interrupt layer's, because the entry stubs are.
extern "C" void* metal_cpu_block() { return &g_cpu; }

extern "C" void metal_halt_until_interrupt() { asm volatile("hlt"); }

// Reset by pulsing the 8042 keyboard controller's reset line, which every PC
// still answers to.
extern "C" [[noreturn]] void metal_power_reboot() {
  asm volatile("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
  for (;;) asm volatile("hlt");
}

// Four shutdown ports tried in order, because no single one covers every
// machine this runs on: QEMU's isa-debug-exit, ACPI on i440fx, ACPI on q35,
// and the port older QEMU used.
extern "C" [[noreturn]] void metal_power_off() {
  asm volatile("outb %0, %1" : : "a"((uint8_t)0), "Nd"((uint16_t)0xF4));
  asm volatile("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
  asm volatile("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0xB004));
  asm volatile("outw %0, %1" : : "a"((uint16_t)0x3400), "Nd"((uint16_t)0x4004));
  for (;;) asm volatile("hlt");
}
