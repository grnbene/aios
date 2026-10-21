// Symmetric multiprocessing on bare metal: bring up the application processors
// through the Local APIC, then let hal::Thread dispatch work across all cores.
// Nothing above the HAL changes; the CPU device simply reports more lanes and
// the scheduler's waves and the device's parallel_for run truly in parallel.
#include "aios/hal/sync.hpp"
#include <cstdint>
#include <cstring>
#include <functional>
#include <new>

extern "C" void metal_console_write(const char* s, size_t n);
extern "C" [[noreturn]] void metal_panic(const char*);
extern uint8_t ap_trampoline_start[], ap_trampoline_end[];

namespace {
inline void out(const char* s) { metal_console_write(s, __builtin_strlen(s)); }
inline uint64_t rdtsc() { uint32_t a, d; asm volatile("rdtsc" : "=a"(a), "=d"(d)); return ((uint64_t)d << 32) | a; }
extern "C" uint64_t metal_tsc_hz();
void udelay(uint64_t us) { uint64_t hz = metal_tsc_hz(); uint64_t end = rdtsc() + (hz / 1000000) * us; while (rdtsc() < end) asm volatile("pause"); }

// ---- Local APIC (xAPIC MMIO) ----
volatile uint32_t* LAPIC = (volatile uint32_t*)0xFEE00000ull;
uint32_t lapic_read(uint32_t reg) { return LAPIC[reg / 4]; }
void lapic_write(uint32_t reg, uint32_t v) { LAPIC[reg / 4] = v; (void)lapic_read(0x20); }
uint32_t lapic_id() { return lapic_read(0x20) >> 24; }

constexpr uint32_t TRAMP = 0x8000;
constexpr uint32_t OFF_PML4 = 0x08, OFF_STACK = 0x10, OFF_ENTRY = 0x18, OFF_FLAG = 0x20;

// ---- ACPI: find the processor list in the MADT ----
uint8_t acpi_sum(const uint8_t* p, size_t n) { uint8_t s = 0; for (size_t i = 0; i < n; ++i) s += p[i]; return s; }
const uint8_t* g_rsdp_from_loader = nullptr;   // set from the multiboot2 ACPI tags

const uint8_t* find_rsdp() {
  // Under UEFI the RSDP is NOT in the legacy BIOS area; the bootloader hands it
  // to us instead. Prefer what the loader gave, then fall back to the scan.
  if (g_rsdp_from_loader && std::memcmp(g_rsdp_from_loader, "RSD PTR ", 8) == 0) return g_rsdp_from_loader;
  auto scan = [](uint64_t start, uint64_t end) -> const uint8_t* {
    for (uint64_t a = start; a + 20 <= end; a += 16) { const uint8_t* p = (const uint8_t*)a; if (std::memcmp(p, "RSD PTR ", 8) == 0 && acpi_sum(p, 20) == 0) return p; }
    return nullptr;
  };
  uint16_t ebda_seg = *(volatile uint16_t*)0x40E;
  if (ebda_seg) { const uint8_t* r = scan((uint64_t)ebda_seg << 4, ((uint64_t)ebda_seg << 4) + 1024); if (r) return r; }
  return scan(0xE0000, 0x100000);
}
constexpr int MAX_CPUS = 32;

// The MADT describes both the processors and the interrupt controllers, so the
// table lookup is shared: metal/interrupts.cpp reads the same table to find the
// IOAPIC and the overrides that say which line an ISA IRQ really arrives on.
const uint8_t* find_madt() {
  const uint8_t* rsdp = find_rsdp();
  if (!rsdp) return nullptr;
  const uint8_t* madt = nullptr;
  auto is = [](const uint8_t* t, const char* sig) { return std::memcmp(t, sig, 4) == 0; };
  uint8_t revision = rsdp[15];
  // ACPI 2.0+ offers an XSDT with 64-bit pointers; ACPI 1.0 only the RSDT.
  uint64_t xsdt = revision >= 2 ? *(const uint64_t*)(rsdp + 24) : 0;
  if (xsdt) {
    const uint8_t* t = (const uint8_t*)xsdt;
    uint32_t len = *(const uint32_t*)(t + 4);
    uint32_t n = len > 36 ? (len - 36) / 8 : 0;
    for (uint32_t i = 0; i < n; ++i) {
      uint64_t addr; std::memcpy(&addr, t + 36 + 8 * i, 8);   // the array is not 8-byte aligned
      const uint8_t* tbl = (const uint8_t*)addr;
      if (is(tbl, "APIC")) madt = tbl;
    }
  }
  if (!madt) {
    uint32_t rsdt = *(const uint32_t*)(rsdp + 16);
    if (rsdt) {
      const uint8_t* t = (const uint8_t*)(uint64_t)rsdt;
      uint32_t len = *(const uint32_t*)(t + 4);
      uint32_t n = len > 36 ? (len - 36) / 4 : 0;
      const uint32_t* e = (const uint32_t*)(t + 36);
      for (uint32_t i = 0; i < n; ++i) { const uint8_t* tbl = (const uint8_t*)(uint64_t)e[i]; if (is(tbl, "APIC")) madt = tbl; }
    }
  }
  return madt;
}

int collect_apic_ids(uint8_t ids[MAX_CPUS]) {
  const uint8_t* madt = find_madt();
  if (!madt) return 0;
  uint32_t len = *(const uint32_t*)(madt + 4);
  int count = 0;
  const uint8_t* p = madt + 44;
  while (p < madt + len && count < MAX_CPUS) {
    uint8_t type = p[0], elen = p[1];
    if (elen < 2) break;
    if (type == 0) { uint8_t apic_id = p[3]; uint32_t flags = *(const uint32_t*)(p + 4); if (flags & 1) ids[count++] = apic_id; }
    p += elen;
  }
  return count;
}

// ---- work pool: one inbox per AP; the BSP also runs work inline ----
struct Inbox { std::function<void()>* volatile job; volatile int done; volatile int live; volatile uint64_t ran; };
Inbox g_inbox[MAX_CPUS];
int g_naps = 0;
volatile int g_started = 0;
uint8_t* g_ap_stacks = nullptr;
constexpr uint64_t AP_STACK = 64 * 1024;

extern "C" void* metal_alloc_pages(size_t);
void* g_ap_tls[MAX_CPUS] = {};   // each AP's thread pointer, built before it is woken
}  // namespace
extern "C" void* metal_make_tls_block();
extern "C" void metal_set_fs_base(void* tp);

// Jobs completed by application processors, and how many APs did any work.
// The same table, for whoever else needs it.
extern "C" const void* metal_acpi_madt() { return find_madt(); }

extern "C" uint64_t metal_ap_jobs() { uint64_t t = 0; for (int i = 0; i < g_naps; ++i) t += g_inbox[i].ran; return t; }
extern "C" int metal_ap_participants() { int n = 0; for (int i = 0; i < g_naps; ++i) if (g_inbox[i].ran) n++; return n; }

extern "C" void ap_main() {
  int slot = __atomic_fetch_add(&g_started, 1, __ATOMIC_ACQ_REL);   // 0..naps-1, dense
  metal_set_fs_base(g_ap_tls[slot]);
  g_inbox[slot].live = 1;
  asm volatile("mfence" ::: "memory");
  for (;;) {
    std::function<void()>* job = g_inbox[slot].job;
    if (job) { (*job)(); g_inbox[slot].ran++; __atomic_store_n(&g_inbox[slot].done, 1, __ATOMIC_RELEASE); __atomic_store_n(&g_inbox[slot].job, (std::function<void()>*)nullptr, __ATOMIC_RELEASE); }
    else asm volatile("pause");
  }
}

// The bootloader's ACPI pointer (multiboot2 tag 14 or 15), given before init.
extern "C" void metal_smp_set_rsdp(const void* rsdp) { g_rsdp_from_loader = (const uint8_t*)rsdp; }

// Reports how many cores are usable (1 + started APs). Called from metal main.
extern "C" int metal_smp_init() {
  uint8_t ids[MAX_CPUS];
  int n = collect_apic_ids(ids);
  if (n <= 1) return 1;
  lapic_write(0xF0, lapic_read(0xF0) | 0x100);        // enable the LAPIC (spurious vector reg)
  uint32_t bsp = lapic_id();
  // Trampoline into low memory, patched with the shared CR3 and C entry.
  size_t tlen = (size_t)(ap_trampoline_end - ap_trampoline_start);
  std::memcpy((void*)(uint64_t)TRAMP, ap_trampoline_start, tlen);
  uint64_t cr3; asm volatile("mov %%cr3, %0" : "=r"(cr3));
  *(volatile uint32_t*)(uint64_t)(TRAMP + OFF_PML4) = (uint32_t)cr3;
  *(volatile uint64_t*)(uint64_t)(TRAMP + OFF_ENTRY) = (uint64_t)&ap_main;
  g_ap_stacks = (uint8_t*)metal_alloc_pages(AP_STACK * (size_t)n);
  int brought = 0;
  for (int i = 0; i < n; ++i) {
    if (ids[i] == bsp) continue;
    if (g_naps >= MAX_CPUS - 1) break;
    // this AP's stack top
    *(volatile uint64_t*)(uint64_t)(TRAMP + OFF_STACK) = (uint64_t)(g_ap_stacks + AP_STACK * (size_t)(brought + 1));
    int before = g_started;
    g_ap_tls[before] = metal_make_tls_block();   // the slot ap_main will take
    // INIT, then two SIPIs at vector 0x08 (trampoline at 0x8000).
    lapic_write(0x310, (uint32_t)ids[i] << 24); lapic_write(0x300, 0x00004500); udelay(10000);
    lapic_write(0x310, (uint32_t)ids[i] << 24); lapic_write(0x300, 0x00004608); udelay(200);
    lapic_write(0x310, (uint32_t)ids[i] << 24); lapic_write(0x300, 0x00004608);
    for (int spin = 0; spin < 200 && g_started == before; ++spin) udelay(1000);
    if (g_started > before) { g_naps++; brought++; }
  }
  return 1 + g_naps;
}

// hal::Thread on bare metal: dispatch to an idle AP inbox, or run inline when
// the pool is full so the calling core contributes too. join() waits for the AP.
namespace aios::hal {
struct Thread::State { int slot; std::function<void()>* job; };

Thread::Thread(std::function<void()> body) {
  for (int i = 0; i < g_naps; ++i) {
    if (!g_inbox[i].live) continue;
    std::function<void()>* expected = nullptr;
    auto* heap = new std::function<void()>(std::move(body));
    __atomic_store_n(&g_inbox[i].done, 0, __ATOMIC_RELEASE);
    if (__atomic_compare_exchange_n(&g_inbox[i].job, &expected, heap, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
      state_ = new State{i, heap}; started_ = true; return;
    }
    body = std::move(*heap); delete heap;   // that AP was busy; take the body back and try the next
  }
  body();   // no free AP: run on the calling core
  started_ = false;
}
Thread::~Thread() { if (started_) join(); }
Thread::Thread(Thread&& o) noexcept : state_(o.state_), started_(o.started_) { o.state_ = nullptr; o.started_ = false; }
Thread& Thread::operator=(Thread&& o) noexcept { if (this != &o) { if (started_) join(); state_ = o.state_; started_ = o.started_; o.state_ = nullptr; o.started_ = false; } return *this; }
void Thread::join() {
  if (!started_) return;
  while (!__atomic_load_n(&g_inbox[state_->slot].done, __ATOMIC_ACQUIRE)) asm volatile("pause");
  delete state_->job; delete state_; state_ = nullptr; started_ = false;
}
unsigned hardware_threads() { return 1u + (unsigned)g_naps; }
// The AP frees its inbox slot itself when the body ends; the body's closure is
// kept (a few dozen bytes) because nothing waits to delete it.
void Thread::detach() { if (!started_) return; delete state_; state_ = nullptr; started_ = false; }
}  // namespace aios::hal
