#pragma once
/* Virtual memory and the physical frame allocator for AArch64 bare metal. */
#include <cstddef>
#include <cstdint>

/* Mapping attributes, as the callers think of them rather than as descriptor
 * bits. */
enum MapFlags : uint32_t {
  MAP_NORMAL   = 0,        /* cacheable DRAM */
  MAP_DEVICE   = 1u << 0,  /* Device-nGnRnE MMIO */
  MAP_USER     = 1u << 1,  /* EL0 may read/write */
  MAP_EXEC     = 1u << 2,  /* executable at whichever EL owns it */
  MAP_RO       = 1u << 3,  /* read-only */
  MAP_WC       = 1u << 4,  /* Normal non-cacheable: write-combining (framebuffers, GPU memory) */
};

extern "C" {
/* Build the kernel address space from the device tree and turn the MMU on. */
void metal_mmu_init(uint64_t dtb_ptr);
/* Before releasing a secondary core: what metal_mmu_init_percpu reads, in memory. */
void metal_mmu_publish_percpu();
/* Point a secondary core at the address space core 0 already built. Each core
 * has its own MAIR/TCR/TTBR/SCTLR and comes out of reset with the MMU off. */
void metal_mmu_init_percpu();
bool metal_mmu_live();

/* Physical frame allocator over DRAM above the kernel image. Allocations are
 * page-granular, contiguous, and zeroed. */
void* metal_alloc_pages(size_t bytes);
void metal_free_pages(void* p, size_t bytes);
size_t metal_heap_used();
size_t metal_heap_total();
size_t metal_heap_frames_free();

uint64_t metal_fdt_ram_base();
uint64_t metal_fdt_ram_size();
uint64_t metal_fdt_gicd_base();
uint64_t metal_fdt_gicr_base();
uint64_t metal_fdt_uart_base();
}

/* Map `size` bytes at `va` onto `pa`. va, pa and size are rounded out to 4 KiB.
 * Returns false when a page table could not be allocated. */
bool mmu_map(uint64_t va, uint64_t pa, uint64_t size, uint32_t flags);
/* Remove a user mapping and invalidate the TLB for it. */
void mmu_unmap(uint64_t va, uint64_t size);
/* Walk the tables the way the hardware does; 0 when nothing is mapped. */
uint64_t mmu_translate(uint64_t va);
