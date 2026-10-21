/* AArch64 stage-1 translation and the physical frame allocator.
 *
 * One 39-bit address space on TTBR0: the kernel identity-maps DRAM and the
 * device window, and user mappings live in the same tables at a VA above every
 * physical address, distinguished by AP/UXN/PXN rather than by a second set of
 * tables. That is what lets an exception from EL0 run kernel code without a
 * TTBR switch, and it means a user page can never alias a kernel one.
 *
 * TTBR1 is switched off (TCR_EL1.EPD1) rather than pointed at the same tables:
 * a walk through the top half is a bug, and it should fault like one.
 */
#include "mmu.hpp"

#include "fdt.hpp"
#include "kio.hpp"

namespace {

constexpr uint64_t PAGE_SIZE  = 4096;
constexpr uint64_t BLOCK_2M   = 2ull * 1024 * 1024;
constexpr uint64_t BLOCK_1G   = 1024ull * 1024 * 1024;

/* MAIR_EL1 attribute slots. */
constexpr unsigned ATTR_DEVICE_nGnRnE = 0;
constexpr unsigned ATTR_DEVICE_nGnRE  = 1;
constexpr unsigned ATTR_NORMAL_WB     = 2;
constexpr unsigned ATTR_NORMAL_NC     = 3;
constexpr uint64_t MAIR_VALUE = (0x00ull << (8 * ATTR_DEVICE_nGnRnE)) |
                                (0x04ull << (8 * ATTR_DEVICE_nGnRE))  |
                                (0xFFull << (8 * ATTR_NORMAL_WB))     |
                                (0x44ull << (8 * ATTR_NORMAL_NC));

/* Descriptor bits. */
constexpr uint64_t D_VALID   = 1ull << 0;
constexpr uint64_t D_TABLE   = 1ull << 1;   /* at L0-L2: table, not block */
constexpr uint64_t D_PAGE    = 1ull << 1;   /* at L3: page */
constexpr uint64_t D_AF      = 1ull << 10;
constexpr uint64_t D_SH_INNER= 3ull << 8;
constexpr uint64_t D_SH_OUTER= 2ull << 8;
constexpr uint64_t D_AP_RW_EL1  = 0ull << 6;
constexpr uint64_t D_AP_RW_ALL  = 1ull << 6;
constexpr uint64_t D_AP_RO_EL1  = 2ull << 6;
constexpr uint64_t D_AP_RO_ALL  = 3ull << 6;
constexpr uint64_t D_PXN     = 1ull << 53;
constexpr uint64_t D_UXN     = 1ull << 54;
constexpr uint64_t D_ADDR_MASK = 0x0000FFFFFFFFF000ull;

/* 39-bit VA: L1 indexes 1 GiB, L2 indexes 2 MiB, L3 indexes 4 KiB. */
constexpr unsigned VA_BITS = 39;
constexpr uint64_t T0SZ    = 64 - VA_BITS;

constexpr unsigned L2_STATIC_TABLES = 20;   /* 20 GiB of block-mapped space */

alignas(4096) __attribute__((section(".pagetables"))) uint64_t g_l1[512];
alignas(4096) __attribute__((section(".pagetables"))) uint64_t g_l2[L2_STATIC_TABLES][512];
unsigned g_l2_used = 0;

bool g_mmu_live = false;
uint64_t g_tcr = 0;      /* what core 0 programmed, replayed onto secondaries */
uint64_t g_ttbr = 0;

/* ---- physical frame allocator ---------------------------------------- */

uint64_t g_frame_base = 0;     /* first allocatable frame, page aligned */
uint64_t g_frame_count = 0;
uint8_t* g_bitmap = nullptr;   /* one bit per frame, 1 = in use */
uint64_t g_frames_used = 0;
uint64_t g_next_hint = 0;

bool frame_test(uint64_t i) { return (g_bitmap[i >> 3] >> (i & 7)) & 1; }
void frame_set(uint64_t i)  { g_bitmap[i >> 3] |= static_cast<uint8_t>(1u << (i & 7)); }
void frame_clear(uint64_t i){ g_bitmap[i >> 3] &= static_cast<uint8_t>(~(1u << (i & 7))); }

void zero_pages(void* p, uint64_t bytes) {
  auto* q = static_cast<uint64_t*>(p);
  for (uint64_t i = 0; i < bytes / 8; ++i) q[i] = 0;
}

uint64_t attr_for(uint32_t flags) {
  uint64_t d = D_VALID | D_AF;
  if (flags & MAP_DEVICE) {
    d |= (static_cast<uint64_t>(ATTR_DEVICE_nGnRnE) << 2) | D_SH_OUTER;
  } else if (flags & MAP_WC) {
    d |= (static_cast<uint64_t>(ATTR_NORMAL_NC) << 2) | D_SH_OUTER;
  } else {
    d |= (static_cast<uint64_t>(ATTR_NORMAL_WB) << 2) | D_SH_INNER;
  }
  const bool user = (flags & MAP_USER) != 0;
  const bool ro   = (flags & MAP_RO) != 0;
  if (user) d |= ro ? D_AP_RO_ALL : D_AP_RW_ALL;
  else      d |= ro ? D_AP_RO_EL1 : D_AP_RW_EL1;

  if (flags & MAP_EXEC) {
    /* Executable for exactly one privilege level: a user page that EL1 could
     * also execute is how a kernel ends up running user-supplied code. */
    d |= user ? D_PXN : D_UXN;
  } else {
    d |= D_PXN | D_UXN;
  }
  return d;
}

uint64_t* table_at(uint64_t entry) {
  return reinterpret_cast<uint64_t*>(entry & D_ADDR_MASK);
}

/* An L2 table from the static pool before the frame allocator exists, from the
 * frame allocator after. */
uint64_t* new_table() {
  if (g_l2_used < L2_STATIC_TABLES) {
    uint64_t* t = g_l2[g_l2_used++];
    zero_pages(t, 4096);
    return t;
  }
  void* p = metal_alloc_pages(PAGE_SIZE);
  return static_cast<uint64_t*>(p);
}

}  /* namespace */

bool mmu_map(uint64_t va, uint64_t pa, uint64_t size, uint32_t flags) {
  const uint64_t end = (va + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  va &= ~(PAGE_SIZE - 1);
  pa &= ~(PAGE_SIZE - 1);
  const uint64_t attr = attr_for(flags);

  while (va < end) {
    const uint64_t l1i = (va >> 30) & 0x1FF;
    const uint64_t l2i = (va >> 21) & 0x1FF;
    const uint64_t l3i = (va >> 12) & 0x1FF;

    /* A 1 GiB span that is aligned and whole gets a block descriptor at L1,
     * which costs one entry instead of 512 plus a table. On systems where address 0
     * contains both DRAM and MMIO (like Raspberry Pi), use L2 2MB blocks instead. */
    if (l2i == 0 && l3i == 0 && (pa % BLOCK_1G) == 0 && end - va >= BLOCK_1G &&
        (g_l1[l1i] == 0) && (pa != 0)) {
      g_l1[l1i] = pa | attr;
      va += BLOCK_1G; pa += BLOCK_1G;
      continue;
    }

    if ((g_l1[l1i] & D_VALID) == 0) {
      uint64_t* t = new_table();
      if (!t) return false;
      g_l1[l1i] = reinterpret_cast<uint64_t>(t) | D_VALID | D_TABLE;
    } else if ((g_l1[l1i] & D_TABLE) == 0) {
      /* Already a 1 GiB block; splitting it is not something any caller here
       * needs, and silently ignoring the request would hide a real conflict. */
      return false;
    }
    uint64_t* l2 = table_at(g_l1[l1i]);

    if (l3i == 0 && (pa % BLOCK_2M) == 0 && end - va >= BLOCK_2M &&
        (l2[l2i] & D_VALID) == 0) {
      l2[l2i] = pa | attr;
      va += BLOCK_2M; pa += BLOCK_2M;
      continue;
    }

    if ((l2[l2i] & D_VALID) == 0) {
      uint64_t* t = new_table();
      if (!t) return false;
      l2[l2i] = reinterpret_cast<uint64_t>(t) | D_VALID | D_TABLE;
    } else if ((l2[l2i] & D_TABLE) == 0) {
      return false;
    }
    uint64_t* l3 = table_at(l2[l2i]);
    l3[l3i] = pa | attr | D_PAGE;

    va += PAGE_SIZE; pa += PAGE_SIZE;
  }

  asm volatile("dsb ishst" ::: "memory");
  if (g_mmu_live) {
    asm volatile("tlbi vmalle1is; dsb ish; isb" ::: "memory");
  }
  return true;
}

void mmu_unmap(uint64_t va, uint64_t size) {
  const uint64_t end = (va + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  va &= ~(PAGE_SIZE - 1);
  while (va < end) {
    const uint64_t l1i = (va >> 30) & 0x1FF;
    const uint64_t l2i = (va >> 21) & 0x1FF;
    const uint64_t l3i = (va >> 12) & 0x1FF;
    if ((g_l1[l1i] & D_VALID) && (g_l1[l1i] & D_TABLE)) {
      uint64_t* l2 = table_at(g_l1[l1i]);
      if ((l2[l2i] & D_VALID) && (l2[l2i] & D_TABLE)) {
        table_at(l2[l2i])[l3i] = 0;
      } else if (l2[l2i] & D_VALID) {
        l2[l2i] = 0;
      }
    }
    asm volatile("tlbi vaae1is, %0" :: "r"(va >> 12) : "memory");
    va += PAGE_SIZE;
  }
  asm volatile("dsb ish; isb" ::: "memory");
}

uint64_t mmu_translate(uint64_t va) {
  const uint64_t l1i = (va >> 30) & 0x1FF;
  const uint64_t l2i = (va >> 21) & 0x1FF;
  const uint64_t l3i = (va >> 12) & 0x1FF;
  const uint64_t e1 = g_l1[l1i];
  if (!(e1 & D_VALID)) return 0;
  if (!(e1 & D_TABLE)) return (e1 & D_ADDR_MASK) | (va & (BLOCK_1G - 1));
  const uint64_t e2 = table_at(e1)[l2i];
  if (!(e2 & D_VALID)) return 0;
  if (!(e2 & D_TABLE)) return (e2 & D_ADDR_MASK) | (va & (BLOCK_2M - 1));
  const uint64_t e3 = table_at(e2)[l3i];
  if (!(e3 & D_VALID)) return 0;
  return (e3 & D_ADDR_MASK) | (va & (PAGE_SIZE - 1));
}

extern "C" {

extern char __kernel_end[];

/* The frame allocator, one core at a time (see cxx_runtime's HeapLock). */
volatile int g_pages_lock = 0;
struct PagesLock {
  PagesLock() { while (__atomic_test_and_set(&g_pages_lock, __ATOMIC_ACQUIRE)) asm volatile("yield"); }
  ~PagesLock() { __atomic_clear(&g_pages_lock, __ATOMIC_RELEASE); }
};

void* metal_alloc_pages(size_t bytes) {
  if (!g_bitmap || bytes == 0) return nullptr;
  PagesLock lock;
  const uint64_t want = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

  for (uint64_t pass = 0; pass < 2; ++pass) {
    uint64_t i = pass == 0 ? g_next_hint : 0;
    const uint64_t limit = pass == 0 ? g_frame_count : g_next_hint;
    while (i + want <= (pass == 0 ? g_frame_count : limit)) {
      uint64_t run = 0;
      while (run < want && !frame_test(i + run)) ++run;
      if (run == want) {
        for (uint64_t k = 0; k < want; ++k) frame_set(i + k);
        g_frames_used += want;
        g_next_hint = i + want;
        void* p = reinterpret_cast<void*>(g_frame_base + i * PAGE_SIZE);
        zero_pages(p, want * PAGE_SIZE);
        return p;
      }
      i += run + 1;
    }
  }
  return nullptr;
}

void metal_free_pages(void* p, size_t bytes) {
  if (!p || !g_bitmap) return;
  PagesLock lock;
  const uint64_t addr = reinterpret_cast<uint64_t>(p);
  if (addr < g_frame_base) return;
  uint64_t i = (addr - g_frame_base) / PAGE_SIZE;
  const uint64_t n = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
  for (uint64_t k = 0; k < n && i + k < g_frame_count; ++k) {
    if (frame_test(i + k)) { frame_clear(i + k); --g_frames_used; }
  }
  if (i < g_next_hint) g_next_hint = i;
}

size_t metal_heap_used()        { return g_frames_used * PAGE_SIZE; }
size_t metal_heap_total()       { return g_frame_count * PAGE_SIZE; }
size_t metal_heap_frames_free() { return g_frame_count - g_frames_used; }
bool metal_mmu_live()           { return g_mmu_live; }

uint64_t metal_fdt_ram_base() { return fdt_info().ram_base; }
uint64_t metal_fdt_ram_size() { return fdt_info().ram_size; }
uint64_t metal_fdt_gicd_base(){ return fdt_info().gicd_base; }
uint64_t metal_fdt_gicr_base(){ return fdt_info().gicr_base; }
uint64_t metal_fdt_uart_base(){ return fdt_info().uart_base; }

/* A core released from the firmware's spin table starts with its caches off
 * and reads these before its MMU is on: push them from core 0's caches to
 * memory first. */
void metal_mmu_publish_percpu() {
  asm volatile("dc civac, %0" :: "r"(&g_tcr) : "memory");
  asm volatile("dc civac, %0" :: "r"(&g_ttbr) : "memory");
  asm volatile("dsb sy" ::: "memory");
}

void metal_mmu_init_percpu() {
  if (!g_ttbr) return;
  asm volatile("msr mair_el1, %0" :: "r"(MAIR_VALUE));
  asm volatile("msr tcr_el1, %0"  :: "r"(g_tcr));
  asm volatile("msr ttbr0_el1, %0" :: "r"(g_ttbr));
  asm volatile("msr ttbr1_el1, %0" :: "r"(g_ttbr));
  asm volatile("dsb ish" ::: "memory");
  asm volatile("tlbi vmalle1" ::: "memory");
  asm volatile("dsb ish; isb" ::: "memory");
  uint64_t sctlr;
  asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
  sctlr |= (1ull << 0) | (1ull << 2) | (1ull << 12);
  sctlr &= ~(1ull << 1);
  asm volatile("msr sctlr_el1, %0" :: "r"(sctlr));
  asm volatile("isb" ::: "memory");
}

void metal_mmu_init(uint64_t dtb_ptr) {
  const FdtInfo& fdt = fdt_info();
  uint64_t ram_base = fdt.valid ? fdt.ram_base : 0x40000000ull;
  uint64_t ram_size = fdt.valid ? fdt.ram_size : 0x08000000ull;

  for (uint64_t i = 0; i < 512; ++i) g_l1[i] = 0;
  g_l2_used = 0;

  if (ram_base == 0) {
    /* Raspberry Pi: DRAM starts at 0x0, peripherals at 0x3F000000 (Pi3) / 0xFE000000 (Pi4) */
    mmu_map(0, 0, ram_size, MAP_NORMAL | MAP_EXEC);
    /* 0x3E000000-0x3F000000 is VideoCore memory, where the firmware puts
     * the framebuffer - not MMIO. As Device memory every store to it went
     * out alone (the 8 MB present of each desktop frame took 24 ms) and an
     * unaligned one faulted. Normal non-cacheable lets stores combine and
     * needs no cache maintenance for the GPU to see them. (If a smaller GPU
     * split made this ARM RAM, the RAM mapping above already owns it and
     * this leaves it be.) */
    mmu_map(0x3E000000ull, 0x3E000000ull, 0x01000000ull, MAP_WC);
    mmu_map(0x3F000000ull, 0x3F000000ull, 0x02000000ull, MAP_DEVICE); /* Pi 3 MMIO + local timers */
    mmu_map(0xFD000000ull, 0xFD000000ull, 0x03000000ull, MAP_DEVICE); /* Pi 4 MMIO */
  } else {
    /* QEMU virt: everything below DRAM is memory-mapped I/O */
    mmu_map(0, 0, ram_base, MAP_DEVICE);
    mmu_map(ram_base, ram_base, ram_size, MAP_NORMAL | MAP_EXEC);
  }

  /* The DTB itself usually sits in DRAM and is already covered; when firmware
   * put it elsewhere, map the page it lives on so fdt_info stays readable. */
  if (dtb_ptr && (dtb_ptr < ram_base || dtb_ptr >= ram_base + ram_size)) {
    mmu_map(dtb_ptr & ~(PAGE_SIZE - 1), dtb_ptr & ~(PAGE_SIZE - 1), 2 * PAGE_SIZE, MAP_NORMAL);
  }

  asm volatile("msr mair_el1, %0" :: "r"(MAIR_VALUE));

  /* IPS has to come from the CPU: claiming 48-bit output on a part that only
   * implements 40 makes every walk fault. */
  uint64_t mmfr0;
  asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
  uint64_t ips = mmfr0 & 0xF;
  if (ips > 5) ips = 5;

  const uint64_t tcr =
      (T0SZ << 0)   |
      (1ull << 8)   |  /* IRGN0 = WB WA */
      (1ull << 10)  |  /* ORGN0 = WB WA */
      (3ull << 12)  |  /* SH0   = inner shareable */
      (0ull << 14)  |  /* TG0   = 4 KiB */
      (T0SZ << 16)  |
      (1ull << 23)  |  /* EPD1: no walks through TTBR1 at all */
      (2ull << 30)  |  /* TG1   = 4 KiB (encoding differs from TG0) */
      (ips  << 32);
  asm volatile("msr tcr_el1, %0" :: "r"(tcr));

  const uint64_t ttbr = reinterpret_cast<uint64_t>(g_l1);
  g_tcr = tcr;
  g_ttbr = ttbr;
  asm volatile("msr ttbr0_el1, %0" :: "r"(ttbr));
  asm volatile("msr ttbr1_el1, %0" :: "r"(ttbr));

  /* The tables were written with the caches off, so they are in memory; the
   * walker is about to read them cacheably. Order the stores, drop every stale
   * translation, then turn it on. */
  asm volatile("dsb ishst" ::: "memory");
  asm volatile("tlbi vmalle1" ::: "memory");
  asm volatile("dsb ish; isb" ::: "memory");

  uint64_t sctlr;
  asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
  sctlr |= (1ull << 0) |   /* M: MMU */
           (1ull << 2) |   /* C: data cache */
           (1ull << 12);   /* I: instruction cache */
  sctlr &= ~(1ull << 1);   /* A: strict alignment checking off */
  asm volatile("msr sctlr_el1, %0" :: "r"(sctlr));
  asm volatile("isb" ::: "memory");

  g_mmu_live = true;

  /* Frames start after the kernel image and after the DTB, page aligned. The
   * bitmap lives in the first frames it describes. */
  uint64_t first_free = reinterpret_cast<uint64_t>(__kernel_end);
  if (dtb_ptr >= ram_base && dtb_ptr < ram_base + ram_size) {
    const uint64_t dtb_end = dtb_ptr + (fdt.dtb_size ? fdt.dtb_size : 0x100000);
    if (dtb_end > first_free) first_free = dtb_end;
  }
  first_free = (first_free + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  const uint64_t ram_end = ram_base + ram_size;
  const uint64_t total_frames = (ram_end - first_free) / PAGE_SIZE;
  const uint64_t bitmap_bytes = (total_frames + 7) / 8;
  const uint64_t bitmap_frames = (bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

  g_bitmap = reinterpret_cast<uint8_t*>(first_free);
  for (uint64_t i = 0; i < bitmap_bytes; ++i) g_bitmap[i] = 0;

  g_frame_base = first_free + bitmap_frames * PAGE_SIZE;
  g_frame_count = total_frames - bitmap_frames;
  g_frames_used = 0;
  g_next_hint = 0;
}

}  /* extern "C" */
