// Tasks: address spaces of their own, ring 3, and a scheduler that takes the
// CPU back from everything — the console included.
//
// The console is not outside this any more. It is a task like the others, it
// gets a slice like the others, and the timer takes the CPU away from it in the
// middle of whatever it is doing. That is what makes `host spawn` mean
// something: the prompt comes straight back and the program it started keeps
// running between keystrokes.
//
// A user task is loaded into page tables that are only its own, entered at
// privilege level 3 where the instructions that own the machine simply fault,
// and the only way it can reach the kernel is the system-call gate — which is
// where its authority is checked, in ring 0, on the entity it runs as.
//
// The kernel's own mapping is shared into every address space as SUPERVISOR
// pages: an interrupt taken in user code has to land on kernel text and a
// kernel stack, and that is the only reason it is there. A process cannot read
// it; the U bit says so and the processor enforces it.
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/metal/process.hpp"
#include "aios/metal/trap.hpp"
#include "aios/userland/elf.hpp"
#include "aios/intelligence/world_model.hpp"

extern "C" void metal_console_write(const char* s, size_t n);
extern "C" [[noreturn]] void metal_panic(const char* what);
extern "C" void syscall_entry();

namespace {

using namespace aios;

constexpr uint64_t PAGE = 4096;
constexpr uint64_t P_PRESENT = 1ull << 0, P_WRITE = 1ull << 1, P_USER = 1ull << 2, P_NX = 1ull << 63;
// User space begins where the kernel's identity map ends, at the next entry of
// the top-level table: nothing of the kernel is even reachable from there.
constexpr uint64_t USER_BASE = 0x8000000000ull;
constexpr uint64_t USER_END = 0x8100000000ull;
constexpr uint64_t USER_STACK_TOP = 0x8010000000ull;
constexpr uint64_t USER_MMAP_BASE = 0x8020000000ull;
constexpr uint64_t USER_STACK_PAGES = 16;      // 64 KiB
constexpr uint64_t KERNEL_STACK_BYTES = 32768;
constexpr int MAX_SIGNALS = 32;
constexpr int MAX_FDS = 16;

// ---- pages ------------------------------------------------------------------
// Every page here is identity mapped by the boot tables, so its physical
// address is its address. That is what makes a page table something this code
// can simply write into.
uint8_t* g_pool = nullptr;
uint64_t g_pool_pages = 0, g_pool_next = 0, g_pool_live = 0;
std::vector<void*> g_free_pages;

void* page_take() {
  if (!g_free_pages.empty()) {
    void* p = g_free_pages.back();
    g_free_pages.pop_back();
    std::memset(p, 0, PAGE);
    ++g_pool_live;
    return p;
  }
  if (g_pool_next >= g_pool_pages) return nullptr;
  void* p = g_pool + g_pool_next * PAGE;
  ++g_pool_next;
  ++g_pool_live;
  std::memset(p, 0, PAGE);
  return p;
}
void page_give(void* p) { g_free_pages.push_back(p); if (g_pool_live) --g_pool_live; }

// ---- address spaces ---------------------------------------------------------
uint64_t g_kernel_cr3 = 0;
uint64_t* table_at(uint64_t entry) { return (uint64_t*)(entry & 0x000FFFFFFFFFF000ull); }

bool map_page(uint64_t* pml4, uint64_t va, uint64_t pa, uint64_t flags, std::vector<void*>& owned) {
  uint64_t* level = pml4;
  for (int shift = 39; shift > 12; shift -= 9) {
    uint64_t index = (va >> shift) & 0x1FF;
    if (!(level[index] & P_PRESENT)) {
      void* page = page_take();
      if (!page) return false;
      owned.push_back(page);
      level[index] = (uint64_t)page | P_PRESENT | P_WRITE | P_USER;
    }
    level = table_at(level[index]);
  }
  level[(va >> 12) & 0x1FF] = (pa & 0x000FFFFFFFFFF000ull) | flags | P_PRESENT;
  return true;
}

// ---- the descriptor tables --------------------------------------------------
struct __attribute__((packed)) Tss {
  uint32_t reserved0;
  uint64_t rsp0, rsp1, rsp2;
  uint64_t reserved1;
  uint64_t ist[7];
  uint64_t reserved2;
  uint16_t reserved3, iomap_base;
};
struct __attribute__((packed)) DescriptorPointer { uint16_t limit; uint64_t base; };

alignas(16) uint64_t g_gdt[9];
alignas(16) Tss g_tss;
// What the entry stubs reach through gs. fpu_area is where the register file
// of whatever is currently running gets saved: the stubs do it on the way in and
// on the way out, because by the time the scheduler runs, kernel C++ has already
// used those registers for its own arithmetic.
struct Cpu { uint64_t scratch, kernel_rsp, user_rsp; void* fpu_area; };
Cpu& cpu() { return *(Cpu*)metal_cpu_block(); }

void wrmsr(uint32_t msr, uint64_t value) {
  asm volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}
uint64_t rdmsr(uint32_t msr) {
  uint32_t lo, hi;
  asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((uint64_t)hi << 32) | lo;
}

void gdt_init() {
  g_gdt[0] = 0;
  g_gdt[1] = 0x00AF9A000000FFFFull;   // ring 0 code — 0x08
  g_gdt[2] = 0x00CF92000000FFFFull;   // ring 0 data — 0x10
  g_gdt[3] = 0;                       // 0x18: the base SYSCALL/SYSRET counts from
  g_gdt[4] = 0x00CFF2000000FFFFull;   // ring 3 data — 0x20 (0x23 with RPL 3)
  g_gdt[5] = 0x00AFFA000000FFFFull;   // ring 3 code — 0x28 (0x2B with RPL 3)
  std::memset(&g_tss, 0, sizeof(g_tss));
  g_tss.iomap_base = sizeof(g_tss);
  uint64_t base = (uint64_t)&g_tss, limit = sizeof(g_tss) - 1;
  g_gdt[6] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ull << 40) | (((limit >> 16) & 0xF) << 48) |
             (((base >> 24) & 0xFF) << 56);
  g_gdt[7] = (base >> 32) & 0xFFFFFFFF;
  g_gdt[8] = 0;
  DescriptorPointer p{(uint16_t)(sizeof(g_gdt) - 1), (uint64_t)&g_gdt[0]};
  asm volatile("lgdt %0" : : "m"(p));
  // The data selectors can simply be reloaded; CS cannot, so it is replaced by
  // returning into it.
  asm volatile(
      "movw $0x10, %%ax\n\t"
      "movw %%ax, %%ds\n\t movw %%ax, %%es\n\t movw %%ax, %%ss\n\t"
      "pushq $0x08\n\t"
      "leaq 1f(%%rip), %%rax\n\t"
      "pushq %%rax\n\t"
      "lretq\n\t"
      "1:\n\t" : : : "rax", "memory");
  asm volatile("ltr %0" : : "r"((uint16_t)0x30));
  g_tss.ist[0] = metal_fault_stack_top();
  metal_use_fault_stack();
}

void syscall_init() {
  constexpr uint32_t IA32_EFER = 0xC0000080, IA32_STAR = 0xC0000081, IA32_LSTAR = 0xC0000082,
                     IA32_FMASK = 0xC0000084, IA32_GS_BASE = 0xC0000101, IA32_KERNEL_GS_BASE = 0xC0000102;
  wrmsr(IA32_EFER, rdmsr(IA32_EFER) | 1ull | (1ull << 11));    // SCE, and NX so a page can be non-executable
  wrmsr(IA32_STAR, ((uint64_t)0x18 << 48) | ((uint64_t)0x08 << 32));
  wrmsr(IA32_LSTAR, (uint64_t)&syscall_entry);
  wrmsr(IA32_FMASK, 0x700ull);                                  // interrupts off, direction clear, on entry
  // gs was pointed at the per-CPU block when the IDT went in, before the first
  // interrupt; nothing to do here but leave it alone.
  (void)IA32_GS_BASE; (void)IA32_KERNEL_GS_BASE;
}

// The floating-point unit is a register file like any other, and two tasks that
// both use it will otherwise read each other's arithmetic.
void fpu_init() {
  uint64_t cr4;
  asm volatile("movq %%cr4, %0" : "=r"(cr4));
  asm volatile("movq %0, %%cr4" : : "r"(cr4 | (1ull << 9) | (1ull << 10)));   // OSFXSR, OSXMMEXCPT
}

// ---- tasks ------------------------------------------------------------------
enum class Life { Ready, Running, Zombie };

// A file this machine has open. Small enough to hold whole: a read takes a copy
// of what is on disk, a write accumulates, and closing puts it back through the
// same host boundary everything else uses.
struct OpenFile {
  std::string path;
  ByteBuffer data;
  uint64_t offset = 0;
  bool writable = false, dirty = false, console = false;
};

struct Mapping { void* frame; uint64_t flags; };

struct Task {
  int pid = 0, ppid = 0;
  std::string name;
  bool is_console = false;
  bool quiet = false;                        // fd 1/2 go nowhere (the idle learner's workload)
  Life life = Life::Ready;
  TrapFrame frame{};
  uint64_t cr3 = 0;
  uint8_t* kernel_stack = nullptr;
  uint64_t kernel_stack_top = 0;
  uint64_t ticks = 0, syscalls = 0;
  uint32_t slice_left = 0;                   // ticks this task still holds the CPU for
  int exit_code = 0;
  std::string ended;
  std::unique_ptr<Surface> actor;
  std::vector<void*> pages;                 // page tables and frames, all of them
  std::map<uint64_t, Mapping> user_map;     // what is mapped where, so fork can copy it
  uint64_t brk = 0, mmap_next = USER_MMAP_BASE;
  std::vector<std::shared_ptr<OpenFile>> fds;
  uint64_t sig_pending = 0;
  uint64_t handler[MAX_SIGNALS] = {};
  uint64_t trampoline = 0;
  bool in_signal = false;
  TrapFrame saved_frame{};                  // what a signal handler will return to
  alignas(16) uint8_t fpu[512] = {};
  bool reaped_by_parent = false;
  uint64_t wake_at = 0;                      // asleep until the kernel's clock reaches this
  uint64_t image_pages = 0;
};

std::vector<std::unique_ptr<Task>> g_tasks;
Task* g_current = nullptr;
Task* g_console = nullptr;
int g_next_pid = 1;
uint64_t g_switches = 0;
// How many consecutive timer ticks a task keeps the CPU. 1 was the old
// behaviour: reschedule on every tick, no quantum at all. The JEPA policy head
// drives this through metal_set_scheduler_slice(), which is the first thing in
// this OS whose behaviour the world model actually changes.
uint32_t g_slice_ticks = 1;
Kernel* g_kernel = nullptr;

Task* find_task(int pid) {
  for (auto& t : g_tasks) if (t->pid == pid) return t.get();
  return nullptr;
}

Task* pick_next(Task* after) {
  if (g_tasks.empty()) return nullptr;
  size_t start = 0;
  for (size_t i = 0; i < g_tasks.size(); ++i)
    if (g_tasks[i].get() == after) { start = i + 1; break; }
  Task* sleeping = nullptr;
  uint64_t now = metal_ticks();
  for (size_t n = 0; n < g_tasks.size(); ++n) {
    Task* t = g_tasks[(start + n) % g_tasks.size()].get();
    if (t->life == Life::Zombie) continue;
    if (t->wake_at > now) { if (!sleeping) sleeping = t; continue; }   // asleep, and something else may run
    return t;
  }
  return sleeping;   // everything is asleep; the console is never asleep, so this is rare
}

void enter_task(Task* t) {
  g_current = t;
  t->slice_left = g_slice_ticks;
  t->life = Life::Running;
  cpu().fpu_area = t->fpu;
  if (!t->is_console) {
    g_tss.rsp0 = t->kernel_stack_top;
    cpu().kernel_rsp = t->kernel_stack_top;
  }
  asm volatile("movq %0, %%cr3" : : "r"(t->cr3) : "memory");
}

// A user buffer the kernel is about to touch: inside user space, AND every page
// of it mapped in the calling task (writable, when the kernel will write it).
// The bounds alone were not enough — the kernel dereferences these pointers in
// ring 0, so an in-range but unmapped address was a kernel page fault, and a
// read-only text page was something the kernel would happily write into.
bool user_range_ok(uint64_t addr, uint64_t len, bool write = false) {
  if (len > (1u << 20) || addr < USER_BASE || addr + len < addr || addr + len > USER_END) return false;
  if (!g_current || g_current->is_console) return false;
  for (uint64_t va = addr & ~(PAGE - 1); va < addr + len; va += PAGE) {
    auto it = g_current->user_map.find(va);
    if (it == g_current->user_map.end()) return false;
    if (write && !(it->second.flags & P_WRITE)) return false;
  }
  return true;
}

// A signal with a handler is delivered by building a call to it on the task's
// own stack: the frame it was interrupted in is saved below, the handler runs
// with the signal number as its argument, and its return address is a
// trampoline that asks the kernel to put everything back.
void deliver_signal(Task* t, TrapFrame* f) {
  for (int sig = 1; sig < MAX_SIGNALS; ++sig) {
    if (!(t->sig_pending & (1ull << sig))) continue;
    // A caught signal arriving while a handler is still running waits for that
    // handler's sigreturn (which delivers it); it used to kill the task here.
    if (t->handler[sig] && t->trampoline && t->in_signal) continue;
    t->sig_pending &= ~(1ull << sig);
    if (!t->handler[sig] || !t->trampoline) {
      // No handler: the default action for every signal this machine has is to
      // end the task, and it says which signal did it.
      t->life = Life::Zombie;
      t->exit_code = -sig;
      t->ended = "killed by signal " + std::to_string(sig);
      return;
    }
    t->saved_frame = *f;
    t->in_signal = true;
    uint64_t sp = (f->rsp - 256) & ~15ull;
    sp -= 8;
    // The stack pointer is the task's to set, so it is checked like any other
    // user pointer before ring 0 writes the return address through it — a task
    // that pointed rsp at kernel memory used to get the trampoline written there.
    if (!user_range_ok(sp, 8, true)) {
      t->in_signal = false;
      t->life = Life::Zombie;
      t->exit_code = -sig;
      t->ended = "signal " + std::to_string(sig) + " could not be delivered: its stack pointer is not writable user memory";
      return;
    }
    *(uint64_t*)sp = t->trampoline;      // where the handler returns to
    f->rsp = sp;
    f->rip = t->handler[sig];
    f->rdi = (uint64_t)sig;
    return;
  }
}

TrapFrame* schedule_from(TrapFrame* f) {
  Task* current = g_current;
  if (current) {
    if (current->life != Life::Zombie && current->sig_pending) deliver_signal(current, f);
    current->frame = *f;
  }
  Task* next = pick_next(current);
  if (!next) metal_panic("nothing left to run, not even the console");
  if (next == current) {
    current->life = Life::Running;
    return f;                                    // no switch: resume exactly where it was
  }
  ++g_switches;
  if (current && current->life == Life::Running) current->life = Life::Ready;
  enter_task(next);
  return &next->frame;
}

TrapFrame* on_tick(TrapFrame* f) {
  if (!g_current) return f;
  ++g_current->ticks;
  // A tick is a scheduling OPPORTUNITY, not automatically a switch. The task
  // holds the CPU until its slice runs out, so a larger slice really does mean
  // fewer context switches — that is what makes this lever measurable.
  if (g_current->slice_left > 1) {
    --g_current->slice_left;
    if (g_current->life != Life::Zombie && !g_current->sig_pending) return f;
  }
  g_current->slice_left = g_slice_ticks;
  return schedule_from(f);
}

TrapFrame* on_fault(TrapFrame* f) {
  if (!g_current || g_current->is_console) return nullptr;   // not ours to explain
  Task* t = g_current;
  // A fault a task has asked to hear about is a signal; otherwise it ends here.
  if (f->vector == 14 && t->handler[11] && !t->in_signal) {
    t->sig_pending |= (1ull << 11);
    return schedule_from(f);
  }
  metal_report_frame("[metal] task fault: ", f);
  t->life = Life::Zombie;
  t->ended = f->vector == 14   ? "page fault — it touched memory it does not have"
             : f->vector == 13 ? "general protection fault — it tried an instruction ring 3 may not run"
             : f->vector == 6  ? "invalid opcode"
             : f->vector == 0  ? "divide by zero"
                               : "processor exception";
  t->exit_code = -(int)f->vector;
  return schedule_from(f);
}

std::string user_string(uint64_t ptr, uint64_t len) {
  if (!user_range_ok(ptr, len) || len > 4096) return {};
  return std::string((const char*)ptr, (size_t)len);
}

int fd_install(Task* t, std::shared_ptr<OpenFile> f) {
  for (size_t i = 0; i < t->fds.size(); ++i)
    if (!t->fds[i]) { t->fds[i] = std::move(f); return (int)i; }
  if (t->fds.size() >= MAX_FDS) return -1;
  t->fds.push_back(std::move(f));
  return (int)t->fds.size() - 1;
}

bool may(Task* t, ResourceKind kind, uint16_t rights) {
  return t->actor && t->actor->may(ResourceRef{kind, {}}, rights).ok();
}

// Give a task fresh, zeroed, writable pages at `va`.
bool grow(Task* t, uint64_t va, uint64_t bytes) {
  uint64_t first = va & ~(PAGE - 1);
  uint64_t last = (va + bytes + PAGE - 1) & ~(PAGE - 1);
  for (uint64_t page_va = first; page_va < last; page_va += PAGE) {
    if (t->user_map.count(page_va)) continue;
    void* frame = page_take();
    if (!frame) return false;
    t->pages.push_back(frame);
    uint64_t flags = P_USER | P_WRITE | P_NX;
    if (!map_page((uint64_t*)t->cr3, page_va, (uint64_t)frame, flags, t->pages)) return false;
    t->user_map[page_va] = Mapping{frame, flags};
  }
  return true;
}

Result<void> load_image(Task* t, ByteSpan image, const userland::ElfImage& elf) {
  std::map<uint64_t, void*> frames;
  for (const auto& seg : elf.segments) {
    uint64_t flags = P_USER;
    if (seg.writable) flags |= P_WRITE;
    if (!seg.executable) flags |= P_NX;
    // The pages a segment occupies, not the pages one page-length steps from
    // its base: a segment starting at 0x…3fe0 with 0x28 bytes of memory lives
    // in TWO pages, and stepping from its base finds only the first. A .bss
    // that crosses a page boundary is exactly where this shows up.
    uint64_t first = seg.vaddr & ~(PAGE - 1);
    uint64_t last = (seg.vaddr + seg.mem_size + PAGE - 1) & ~(PAGE - 1);
    for (uint64_t va = first; va < last; va += PAGE) {
      void*& frame = frames[va];
      if (!frame) {
        frame = page_take();
        if (!frame) return Result<void>(Status::Exhausted, "no pages left to load this program into");
        t->pages.push_back(frame);
        ++t->image_pages;
      }
      if (!map_page((uint64_t*)t->cr3, va, (uint64_t)frame, flags, t->pages))
        return Result<void>(Status::Exhausted, "no pages left for this program's page tables");
      t->user_map[va] = Mapping{frame, flags};
    }
    // Copied after mapping so a segment sharing a page with another does not
    // lose what the first one put there.
    for (uint64_t i = 0; i < seg.file_size; ++i) {
      uint64_t va = seg.vaddr + i;
      uint8_t* frame = (uint8_t*)frames[va & ~(PAGE - 1)];
      frame[va & (PAGE - 1)] = image[(size_t)(seg.file_offset + i)];
    }
  }
  for (uint64_t i = 0; i < USER_STACK_PAGES; ++i)
    if (!grow(t, USER_STACK_TOP - (i + 1) * PAGE, PAGE))
      return Result<void>(Status::Exhausted, "no pages left for this program's stack");

  t->brk = (elf.highest + PAGE - 1) & ~(PAGE - 1);
  t->mmap_next = USER_MMAP_BASE;
  t->frame = TrapFrame{};
  t->frame.rip = elf.entry;
  t->frame.cs = 0x2B;                    // ring 3 code
  t->frame.ss = 0x23;                    // ring 3 data
  t->frame.rflags = 0x202;               // interrupts enabled, and it cannot turn them off
  t->frame.rsp = USER_STACK_TOP - 16;
  std::memset(t->fpu, 0, sizeof(t->fpu));
  t->fpu[0] = 0x7F; t->fpu[1] = 0x03;    // a clean x87 control word
  t->fpu[24] = 0x80; t->fpu[25] = 0x1F;  // and the default MXCSR
  return Ok();
}

void release_address_space(Task* t) {
  for (void* page : t->pages) page_give(page);
  t->pages.clear();
  t->user_map.clear();
  t->image_pages = 0;
}

}  // namespace

// ---- what the rest of the machine calls ------------------------------------

// The scheduler lever the world model pulls. Bounded here, in the kernel, so a
// bad prediction cannot starve the machine: 1..16 ticks, never zero.
extern "C" void metal_set_scheduler_slice(uint32_t ticks) {
  if (ticks < 1) ticks = 1;
  if (ticks > 16) ticks = 16;
  g_slice_ticks = ticks;
  if (g_current) g_current->slice_left = ticks;
}
extern "C" uint32_t metal_scheduler_slice() { return g_slice_ticks; }
// How many tasks are runnable right now — ready or running. This machine's own
// scheduler knows it exactly, which is what makes "runnable work per
// processor" a measurement here rather than an estimate off a one-minute
// average as it has to be on a hosted box.
extern "C" uint32_t metal_ready_tasks() {
  uint32_t n = 0;
  for (auto& t : g_tasks) {
    if (t && (t->life == Life::Ready || t->life == Life::Running)) ++n;
  }
  return n;
}
extern "C" uint64_t metal_switches() { return g_switches; }
extern "C" uint64_t metal_task_ticks_total() {
  uint64_t n = 0;
  for (auto& t : g_tasks) n += t->ticks;
  return n;
}

extern "C" void metal_process_init(size_t pool_bytes) {
  asm volatile("movq %%cr3, %0" : "=r"(g_kernel_cr3));
  uint64_t pages = pool_bytes / PAGE;
  uint8_t* raw = (uint8_t*)::operator new(pool_bytes + PAGE);
  g_pool = (uint8_t*)(((uint64_t)raw + PAGE - 1) & ~(PAGE - 1));
  g_pool_pages = pages;
  gdt_init();
  syscall_init();
  fpu_init();
}

namespace aios::metal {
namespace {

// The system calls. Six became twenty-one: this is the whole interface between
// a program and this operating system, and every one of them that touches the
// machine goes through the Surface of the entity the task runs as.
TrapFrame* on_syscall(TrapFrame* f) {
  Task* t = g_current;
  if (!t || t->is_console) { f->rax = (uint64_t)-1; return f; }
  ++t->syscalls;
  uint64_t nr = f->rax, a = f->rdi, b = f->rsi, c = f->rdx;
  switch (nr) {
    case 0: {                                        // write(fd, buffer, length)
      if (!user_range_ok(b, c)) { f->rax = (uint64_t)-1; break; }
      if (a == 1 || a == 2) { if (!t->quiet) metal_console_write((const char*)b, (size_t)c); f->rax = c; break; }
      if (a >= t->fds.size() || !t->fds[a] || !t->fds[a]->writable) { f->rax = (uint64_t)-1; break; }
      auto& file = *t->fds[a];
      if (file.offset + c > file.data.size()) file.data.resize(file.offset + c);
      std::memcpy(file.data.data() + file.offset, (const void*)b, (size_t)c);
      file.offset += c;
      file.dirty = true;
      f->rax = c;
      break;
    }
    case 1:                                          // exit(code)
      t->exit_code = (int)(int64_t)a;
      t->life = Life::Zombie;
      return schedule_from(f);
    case 2: f->rax = (uint64_t)t->pid; break;
    case 3: return schedule_from(f);                 // yield
    case 4: f->rax = metal_ticks(); break;
    case 5: {                                        // allocate a state object — if this entity may
      if (!user_range_ok(a, b) || !t->actor) { f->rax = (uint64_t)-1; break; }
      ByteBuffer payload((const uint8_t*)a, (const uint8_t*)a + b);
      auto id = t->actor->create_state(StateKind::Working, ByteSpan(payload.data(), payload.size()));
      if (!id.ok()) { f->rax = (uint64_t)-2; break; }
      uint64_t tail = 0;
      std::string text = id->str();
      for (size_t i = text.size() > 4 ? text.size() - 4 : 0; i < text.size(); ++i) {
        char ch = text[i];
        tail = tail * 16 + (uint64_t)((ch >= '0' && ch <= '9') ? ch - '0' : (ch | 32) - 'a' + 10);
      }
      f->rax = tail;
      break;
    }
    case 6: {                                        // open(path, len, writable)
      std::string path = user_string(a, b);
      if (path.empty() || !g_kernel) { f->rax = (uint64_t)-1; break; }
      bool writable = c != 0;
      if (!may(t, ResourceKind::StorageObject, writable ? R_Write : R_Read)) { f->rax = (uint64_t)-2; break; }
      auto file = std::make_shared<OpenFile>();
      file->path = g_kernel->config().root + "/fs" + (path[0] == '/' ? path : "/" + path);
      file->writable = writable;
      auto have = g_kernel->host().read_file(file->path);
      if (have.ok()) file->data = *have;
      else if (!writable) { f->rax = (uint64_t)-3; break; }
      int fd = fd_install(t, file);
      f->rax = fd < 0 ? (uint64_t)-1 : (uint64_t)fd;
      break;
    }
    case 7: {                                        // read(fd, buffer, length)
      if (a >= t->fds.size() || !t->fds[a] || !user_range_ok(b, c, true)) { f->rax = (uint64_t)-1; break; }
      auto& file = *t->fds[a];
      uint64_t n = file.offset >= file.data.size() ? 0 : file.data.size() - file.offset;
      if (n > c) n = c;
      if (n) std::memcpy((void*)b, file.data.data() + file.offset, (size_t)n);
      file.offset += n;
      f->rax = n;
      break;
    }
    case 8: {                                        // close(fd) — a dirty file is written back here
      if (a >= t->fds.size() || !t->fds[a]) { f->rax = (uint64_t)-1; break; }
      auto file = t->fds[a];
      t->fds[a] = nullptr;
      if (file.use_count() == 1 && file->dirty && g_kernel) {
        std::string dir = file->path.substr(0, file->path.rfind('/'));
        g_kernel->host().make_dir(dir);
        auto w = g_kernel->host().write_file(file->path, ByteSpan(file->data.data(), file->data.size()));
        f->rax = w.ok() ? 0 : (uint64_t)-1;
        break;
      }
      f->rax = 0;
      break;
    }
    case 9: {                                        // lseek(fd, offset, whence)
      if (a >= t->fds.size() || !t->fds[a]) { f->rax = (uint64_t)-1; break; }
      auto& file = *t->fds[a];
      uint64_t at = c == 1 ? file.offset + b : c == 2 ? file.data.size() + b : b;
      if (at > file.data.size()) at = file.data.size();
      file.offset = at;
      f->rax = at;
      break;
    }
    case 10: {                                       // brk(addr) — 0 asks where it is
      if (a == 0) { f->rax = t->brk; break; }
      if (a < t->brk || a > USER_STACK_TOP - USER_STACK_PAGES * PAGE) { f->rax = (uint64_t)-1; break; }
      if (!grow(t, t->brk, a - t->brk)) { f->rax = (uint64_t)-1; break; }
      t->brk = (a + PAGE - 1) & ~(PAGE - 1);
      f->rax = t->brk;
      break;
    }
    case 11: {                                       // mmap(length) — anonymous, zeroed, writable
      uint64_t len = (a + PAGE - 1) & ~(PAGE - 1);
      // Written so it cannot wrap: `mmap_next + len` overflowed for a huge length,
      // passed, and moved mmap_next below user space into the shared kernel tables.
      if (!len || t->mmap_next > USER_END || len > USER_END - t->mmap_next) { f->rax = (uint64_t)-1; break; }
      uint64_t at = t->mmap_next;
      if (!grow(t, at, len)) { f->rax = (uint64_t)-1; break; }
      t->mmap_next += len;
      f->rax = at;
      break;
    }
    case 12: {                                       // fork()
      auto child = std::make_unique<Task>();
      child->pid = g_next_pid++;
      child->ppid = t->pid;
      child->name = t->name;
      child->quiet = t->quiet;
      child->actor = std::make_unique<Surface>(*t->actor);
      uint64_t* pml4 = (uint64_t*)page_take();
      if (!pml4) { f->rax = (uint64_t)-1; break; }
      child->pages.push_back(pml4);
      pml4[0] = ((uint64_t*)g_kernel_cr3)[0];
      child->cr3 = (uint64_t)pml4;
      // Every page copied outright. There is no copy-on-write here yet, and
      // pretending otherwise would be the lie; what there is, is a child whose
      // memory is genuinely its own from the first instruction.
      bool ok = true;
      for (const auto& [va, m] : t->user_map) {
        void* frame = page_take();
        if (!frame) { ok = false; break; }
        std::memcpy(frame, m.frame, PAGE);
        child->pages.push_back(frame);
        if (!map_page(pml4, va, (uint64_t)frame, m.flags, child->pages)) { ok = false; break; }
        child->user_map[va] = Mapping{frame, m.flags};
      }
      if (!ok) {
        for (void* page : child->pages) page_give(page);
        f->rax = (uint64_t)-1;
        break;
      }
      child->kernel_stack = (uint8_t*)::operator new(KERNEL_STACK_BYTES);
      child->kernel_stack_top = ((uint64_t)child->kernel_stack + KERNEL_STACK_BYTES) & ~15ull;
      child->brk = t->brk;
      child->mmap_next = t->mmap_next;
      child->fds = t->fds;                           // the same open files, shared, as a fork does
      child->trampoline = t->trampoline;
      std::memcpy(child->handler, t->handler, sizeof(child->handler));
      std::memcpy(child->fpu, t->fpu, sizeof(child->fpu));
      child->frame = *f;
      child->frame.rax = 0;                          // the child is told it is the child
      child->life = Life::Ready;
      f->rax = (uint64_t)child->pid;
      g_tasks.push_back(std::move(child));
      break;
    }
    case 13: {                                       // exec(path, len)
      std::string path = user_string(a, b);
      if (path.empty() || !g_kernel) { f->rax = (uint64_t)-1; break; }
      std::string full = g_kernel->config().root + "/fs" + (path[0] == '/' ? path : "/" + path);
      auto file = userland::elf_open(*g_kernel, t->actor.get(), full);
      if (!file.ok()) { f->rax = (uint64_t)-2; break; }
      if (!file->image.static_linked || file->image.lowest < USER_BASE || file->image.highest > USER_END) {
        f->rax = (uint64_t)-3;
        break;
      }
      // The address space goes, the task stays: same pid, same open files, same
      // authority, a different program.
      uint64_t* pml4 = (uint64_t*)page_take();
      if (!pml4) { f->rax = (uint64_t)-1; break; }
      release_address_space(t);
      t->pages.push_back(pml4);
      pml4[0] = ((uint64_t*)g_kernel_cr3)[0];
      t->cr3 = (uint64_t)pml4;
      auto loaded = load_image(t, ByteSpan(file->bytes.data(), file->bytes.size()), file->image);
      if (!loaded.ok()) {
        t->life = Life::Zombie;
        t->ended = "exec failed with the old image already gone: " + loaded.error().message;
        t->exit_code = -1;
        return schedule_from(f);
      }
      t->name = path;
      t->in_signal = false;
      asm volatile("movq %0, %%cr3" : : "r"(t->cr3) : "memory");
      *f = t->frame;
      break;
    }
    case 14: {                                       // wait(status) — one finished child
      for (auto& other : g_tasks) {
        if (other->ppid != t->pid || other->life != Life::Zombie || other->reaped_by_parent) continue;
        other->reaped_by_parent = true;
        if (user_range_ok(a, 8, true)) *(int64_t*)a = other->exit_code;
        f->rax = (uint64_t)other->pid;
        return f;
      }
      bool any = false;
      for (auto& other : g_tasks) if (other->ppid == t->pid && !other->reaped_by_parent) any = true;
      if (!any) { f->rax = (uint64_t)-1; break; }    // nothing to wait for
      // A child is still running, so this really blocks: the instruction is
      // rewound and the task gives the CPU up. It re-enters the same call when
      // it is scheduled again, and only leaves when there is a child to report.
      f->rax = nr;
      f->rip -= 2;                                   // the syscall instruction is two bytes
      return schedule_from(f);
    }
    case 15: {                                       // kill(pid, signal)
      Task* target = find_task((int)a);
      if (!target || target->is_console || target->life == Life::Zombie || b >= MAX_SIGNALS) {
        f->rax = (uint64_t)-1;
        break;
      }
      target->sig_pending |= (1ull << b);
      target->wake_at = 0;                           // a signal wakes it
      if (target == t) { f->rax = 0; return schedule_from(f); }   // to itself: delivered now
      f->rax = 0;
      break;
    }
    case 16: {                                       // sigaction(signal, handler, trampoline)
      if (a == 0 || a >= MAX_SIGNALS) { f->rax = (uint64_t)-1; break; }
      if (a == 9) { f->rax = (uint64_t)-1; break; }  // nothing catches this one
      t->handler[a] = b;
      if (c) t->trampoline = c;
      f->rax = 0;
      break;
    }
    case 17:                                         // sigreturn()
      if (!t->in_signal) { f->rax = (uint64_t)-1; break; }
      t->in_signal = false;
      *f = t->saved_frame;
      if (t->sig_pending) deliver_signal(t, f);       // one that was held back while the handler ran
      if (t->life == Life::Zombie) return schedule_from(f);
      break;
    case 18: f->rax = (uint64_t)t->ppid; break;
    case 19:                                         // sleep(ticks)
      t->wake_at = metal_ticks() + a;
      f->rax = 0;
      return schedule_from(f);
    case 20: {                                       // persist(buf, len) — the capability demonstration
      if (!user_range_ok(a, b)) { f->rax = (uint64_t)-1; break; }
      if (!may(t, ResourceKind::StorageObject, R_Write)) { f->rax = (uint64_t)-2; break; }
      std::string path = g_kernel->config().root + "/fs/tmp";
      g_kernel->host().make_dir(path);
      path += "/proc-" + std::to_string(t->pid) + ".txt";
      auto w = g_kernel->host().write_file(path, ByteSpan((const uint8_t*)a, (size_t)b));
      f->rax = w.ok() ? b : (uint64_t)-1;
      break;
    }
    case 21: {                                       // SYS_JEPA_PREDICT (s_in, a_in, s_out)
      constexpr size_t state_bytes = intelligence::kSubstrateStateDim * sizeof(float);
      constexpr size_t act_bytes = intelligence::kActionVectorDim * sizeof(float);
      if (!user_range_ok(a, state_bytes) || !user_range_ok(b, act_bytes) || !user_range_ok(c, state_bytes, true)) {
        f->rax = (uint64_t)-1;
        break;
      }
      intelligence::SubstrateState s_in;
      s_in.vector.assign((const float*)a, (const float*)a + intelligence::kSubstrateStateDim);
      std::vector<float> a_in((const float*)b, (const float*)b + intelligence::kActionVectorDim);
      auto* model = intelligence::get_global_jepa_world_model();
      auto s_hat = model->predict_next_state(s_in, a_in);
      std::memcpy((void*)c, s_hat.vector.data(), state_bytes);
      f->rax = 0;
      break;
    }
    case 22: {                                       // SYS_JEPA_STATE (buf, len)
      // The count is bounded BEFORE it is multiplied: b * sizeof(float) wrapped
      // for a huge b, so a 4-byte range passed the check and the full state was
      // copied over whatever followed it.
      size_t count = b < intelligence::kSubstrateStateDim ? (size_t)b : intelligence::kSubstrateStateDim;
      if (!user_range_ok(a, count * sizeof(float), true)) { f->rax = (uint64_t)-1; break; }
      auto* region = intelligence::get_global_shared_state_region();
      std::memcpy((void*)a, region->state_vector, count * sizeof(float));
      f->rax = count;
      break;
    }
    case 23:                                         // SYS_JEPA_SHM () — withdrawn
      // This returned the kernel heap address of the shared region: a pointer
      // into supervisor memory that a ring-3 task can only fault on, and a leak
      // of the kernel's layout. There is no user mapping of the region; read it
      // with SYS_JEPA_STATE, which copies it out under a range check.
      f->rax = (uint64_t)-1;
      break;
    default: f->rax = (uint64_t)-1; break;
  }
  return f;
}

}  // namespace

void process_become_console(Kernel& kernel, const Surface& console) {
  g_kernel = &kernel;
  auto t = std::make_unique<Task>();
  t->pid = 0;
  t->name = "console";
  t->is_console = true;
  t->cr3 = g_kernel_cr3;
  t->actor = std::make_unique<Surface>(console);
  t->life = Life::Running;
  asm volatile("fxsave (%0)" : : "r"(t->fpu) : "memory");
  cpu().fpu_area = t->fpu;
  g_console = t.get();
  g_current = t.get();
  g_tasks.push_back(std::move(t));
  // From here the console is scheduled like anything else: the timer will take
  // the CPU away from it in the middle of whatever it is doing.
  metal_tick_hook = on_tick;
  metal_syscall_hook = on_syscall;
  metal_user_fault_hook = on_fault;
}

Result<int> process_spawn(const std::string& name, ByteSpan image, const Surface& actor, bool quiet) {
  auto elf = userland::elf_read(image);
  if (!elf.ok()) return Result<int>(elf.error());
  if (!elf->static_linked)
    return Result<int>(Status::Unsupported,
                       "this program asks for the interpreter " + elf->interpreter +
                           "; this machine has no dynamic loader, so link it statically");
  if (elf->lowest < USER_BASE || elf->highest > USER_END)
    return Result<int>(Status::Invalid,
                       "this program wants to live outside the address range user space occupies on this machine");

  auto t = std::make_unique<Task>();
  struct ReturnPages {
    Task* t;
    bool keep = false;
    ~ReturnPages() { if (!keep) for (void* page : t->pages) page_give(page); }
  } give_back{t.get()};
  t->pid = g_next_pid++;
  t->ppid = g_current ? g_current->pid : 0;
  t->name = name;
  t->quiet = quiet;
  t->actor = std::make_unique<Surface>(actor);

  uint64_t* pml4 = (uint64_t*)page_take();
  if (!pml4) return Result<int>(Status::Exhausted, "no pages left for another address space");
  t->pages.push_back(pml4);
  // The kernel's own mapping, shared and supervisor-only: an interrupt taken in
  // ring 3 has to land somewhere, and this is the only reason it is here.
  pml4[0] = ((uint64_t*)g_kernel_cr3)[0];
  t->cr3 = (uint64_t)pml4;

  if (auto r = load_image(t.get(), image, *elf); !r.ok()) return Result<int>(r.error());

  t->kernel_stack = (uint8_t*)::operator new(KERNEL_STACK_BYTES);
  t->kernel_stack_top = ((uint64_t)t->kernel_stack + KERNEL_STACK_BYTES) & ~15ull;
  t->life = Life::Ready;

  int pid = t->pid;
  give_back.keep = true;
  g_tasks.push_back(std::move(t));
  return pid;
}

// Wait for every task but the console to finish. The console does this by
// halting: the timer takes the CPU off it, the others run, and it wakes to look
// again — which is a wait, not a spin.
uint64_t process_wait_all() {
  uint64_t before = g_switches;
  for (;;) {
    bool any = false;
    for (auto& t : g_tasks) if (!t->is_console && t->life != Life::Zombie) any = true;
    if (!any) break;
    metal_halt_until_interrupt();
  }
  return g_switches - before;
}

uint64_t process_wait(const std::vector<int>& pids) {
  uint64_t before = g_switches;
  for (;;) {
    bool any = false;
    for (int pid : pids) { Task* t = find_task(pid); if (t && !t->is_console && t->life != Life::Zombie) any = true; }
    if (!any) break;
    metal_halt_until_interrupt();
  }
  return g_switches - before;
}

bool process_any_live() {
  for (auto& t : g_tasks) if (!t->is_console && t->life != Life::Zombie) return true;
  return false;
}

Result<void> process_signal(int pid, int signal) {
  Task* t = find_task(pid);
  if (!t || t->is_console) return Result<void>(Status::NotFound, "no such process");
  if (t->life == Life::Zombie) return Result<void>(Status::Invalid, "that process has already finished");
  if (signal <= 0 || signal >= MAX_SIGNALS) return Result<void>(Status::Invalid, "no such signal");
  t->sig_pending |= (1ull << signal);
  t->wake_at = 0;
  return Ok();
}

std::vector<ProcessReport> process_list() {
  std::vector<ProcessReport> out;
  for (auto& t : g_tasks) {
    if (t->is_console) continue;
    out.push_back({t->pid, t->ppid, t->name,
                   t->life == Life::Ready ? "ready" : t->life == Life::Running ? "running" : "finished",
                   t->ended, t->ticks, t->syscalls, (uint64_t)t->pages.size(), t->exit_code});
  }
  return out;
}

ProcessReport process_console() {
  return {g_console->pid, 0, g_console->name, "running", "", g_console->ticks, 0, 0, 0};
}

size_t process_reap() {
  size_t n = 0;
  for (auto it = g_tasks.begin(); it != g_tasks.end();) {
    if (!(*it)->is_console && (*it)->life == Life::Zombie) {
      for (void* page : (*it)->pages) page_give(page);
      ::operator delete((*it)->kernel_stack);
      if (g_current == it->get()) g_current = g_console;
      it = g_tasks.erase(it);
      ++n;
    } else ++it;
  }
  return n;
}

size_t process_reap(const std::vector<int>& pids) {
  size_t n = 0;
  for (auto it = g_tasks.begin(); it != g_tasks.end();) {
    bool mine = false;
    for (int pid : pids) if ((*it)->pid == pid) mine = true;
    if (mine && !(*it)->is_console && (*it)->life == Life::Zombie) {
      for (void* page : (*it)->pages) page_give(page);
      ::operator delete((*it)->kernel_stack);
      if (g_current == it->get()) g_current = g_console;
      it = g_tasks.erase(it);
      ++n;
    } else ++it;
  }
  return n;
}

void process_memory(uint64_t* pages_total, uint64_t* pages_live, uint64_t* switches) {
  if (pages_total) *pages_total = g_pool_pages;
  if (pages_live) *pages_live = g_pool_live;
  if (switches) *switches = g_switches;
}

}  // namespace aios::metal
