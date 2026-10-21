/* fw_cfg and ramfb (metal/arm64/ramfb.cpp).
 *
 * The other shape a display comes in: instead of the kernel driving a display
 * controller, it hands somebody else a pointer to its own memory and that
 * somebody scans it out. ramfb models exactly what a board with working
 * firmware does — U-Boot lights the panel and publishes a linear framebuffer —
 * so the code path that will run on real iron gets exercised here, on QEMU,
 * where a mistake costs a rebuild instead of a head unit.
 *
 * Everything in the fw_cfg interface is BIG-endian, on a little-endian CPU.
 * That is not a detail to skim: a byte-swap missed on the selector silently
 * reads the wrong file and a byte-swap missed in the ramfb config gives a
 * display 16 million pixels wide.
 */
#include "fdt.hpp"
#include "kio.hpp"
#include "mmio.hpp"
#include "mmu.hpp"

namespace {

constexpr uint64_t FW_CFG_DATA = 0x00;
constexpr uint64_t FW_CFG_SEL  = 0x08;
constexpr uint64_t FW_CFG_DMA  = 0x10;

constexpr uint16_t FW_CFG_SIGNATURE = 0x0000;
constexpr uint16_t FW_CFG_ID        = 0x0001;
constexpr uint16_t FW_CFG_FILE_DIR  = 0x0019;

constexpr uint32_t DMA_ERROR  = 1;
constexpr uint32_t DMA_READ   = 2;
constexpr uint32_t DMA_SKIP   = 4;
constexpr uint32_t DMA_SELECT = 8;
constexpr uint32_t DMA_WRITE  = 16;

/* DRM_FORMAT_XRGB8888: in memory B,G,R,X, so a little-endian u32 of
 * 0x00RRGGBB — the same layout the rest of the display code uses. */
constexpr uint32_t FOURCC_XRGB8888 = 0x34325258;

uint64_t g_base = 0;
bool g_dma = false;

struct DmaAccess {          /* all fields big-endian */
  uint32_t control;
  uint32_t length;
  uint64_t address;
};

struct FileEntry {          /* big-endian, from FW_CFG_FILE_DIR */
  uint32_t size;
  uint16_t select;
  uint16_t reserved;
  char name[56];
};

struct RamfbCfg {           /* big-endian, written to the "etc/ramfb" file */
  uint64_t addr;
  uint32_t fourcc;
  uint32_t flags;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
} __attribute__((packed));

uint32_t bswap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}
uint64_t bswap64(uint64_t v) {
  return (static_cast<uint64_t>(bswap32(static_cast<uint32_t>(v))) << 32) |
         bswap32(static_cast<uint32_t>(v >> 32));
}


/* The DMA descriptor itself is read by the device, so it has to live in memory
 * the device can reach and be flushed before the doorbell. */
DmaAccess* g_desc = nullptr;

void dma_kick(uint32_t control, uint32_t length, uint64_t address) {
  g_desc->control = bswap32(control);
  g_desc->length  = bswap32(length);
  g_desc->address = bswap64(address);
  asm volatile("dsb sy" ::: "memory");

  /* The DMA address register is written big-endian, high half first: the
   * device starts the transfer on the write to the low half. */
  const uint64_t d = reinterpret_cast<uint64_t>(g_desc);
  mmio_write32(g_base + FW_CFG_DMA, bswap32(static_cast<uint32_t>(d >> 32)));
  mmio_write32(g_base + FW_CFG_DMA + 4, bswap32(static_cast<uint32_t>(d)));
  asm volatile("dsb sy" ::: "memory");

  /* Control is zeroed by the device on success; anything else with the ERROR
   * bit set means it refused. */
  for (unsigned i = 0; i < 1000000; ++i) {
    const uint32_t c = bswap32(g_desc->control);
    if (c == 0 || (c & DMA_ERROR)) return;
  }
}

bool dma_read(uint16_t selector, void* dst, uint32_t len) {
  dma_kick((static_cast<uint32_t>(selector) << 16) | DMA_SELECT | DMA_READ, len,
           reinterpret_cast<uint64_t>(dst));
  return (bswap32(g_desc->control) & DMA_ERROR) == 0;
}

bool dma_write(uint16_t selector, const void* src, uint32_t len) {
  dma_kick((static_cast<uint32_t>(selector) << 16) | DMA_SELECT | DMA_WRITE, len,
           reinterpret_cast<uint64_t>(const_cast<void*>(src)));
  return (bswap32(g_desc->control) & DMA_ERROR) == 0;
}

bool name_is(const char* a, const char* b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}

}  /* namespace */

/* Set up a ramfb scanout over `pixels`. Returns false when this machine has no
 * fw_cfg, no DMA, or no ramfb device. */
bool ramfb_setup(uint32_t* pixels, uint32_t width, uint32_t height) {
  g_base = fdt_info().fwcfg_base;
  if (!g_base) return false;

  /* "QEMU" in the signature file, or this is not fw_cfg at all. */
  mmio_write16(g_base + FW_CFG_SEL, __builtin_bswap16(FW_CFG_SIGNATURE));
  char sig[4] = {};
  for (unsigned i = 0; i < 4; ++i) sig[i] = static_cast<char>(mmio_read8(g_base + FW_CFG_DATA));
  if (sig[0] != 'Q' || sig[1] != 'E' || sig[2] != 'M' || sig[3] != 'U') return false;

  mmio_write16(g_base + FW_CFG_SEL, __builtin_bswap16(FW_CFG_ID));
  uint32_t id = 0;
  for (unsigned i = 0; i < 4; ++i)
    id |= static_cast<uint32_t>(mmio_read8(g_base + FW_CFG_DATA)) << (8 * i);
  g_dma = (id & 2) != 0;
  if (!g_dma) return false;   /* ramfb can only be configured through DMA */

  if (!g_desc) {
    g_desc = static_cast<DmaAccess*>(metal_alloc_pages(4096));
    if (!g_desc) return false;
  }

  /* Walk the file directory for "etc/ramfb" and keep its selector. */
  uint32_t count_be = 0;
  if (!dma_read(FW_CFG_FILE_DIR, &count_be, 4)) return false;
  const uint32_t count = bswap32(count_be);
  if (!count || count > 4096) return false;

  auto* dir = static_cast<uint8_t*>(metal_alloc_pages(4 + count * sizeof(FileEntry)));
  if (!dir) return false;
  if (!dma_read(FW_CFG_FILE_DIR, dir, 4 + count * sizeof(FileEntry))) return false;

  uint16_t ramfb_sel = 0;
  const auto* entries = reinterpret_cast<const FileEntry*>(dir + 4);
  for (uint32_t i = 0; i < count; ++i) {
    if (name_is(entries[i].name, "etc/ramfb")) {
      ramfb_sel = __builtin_bswap16(entries[i].select);
      break;
    }
  }
  metal_free_pages(dir, 4 + count * sizeof(FileEntry));
  if (!ramfb_sel) return false;

  auto* cfg = static_cast<RamfbCfg*>(metal_alloc_pages(4096));
  if (!cfg) return false;
  cfg->addr   = bswap64(reinterpret_cast<uint64_t>(pixels));
  cfg->fourcc = bswap32(FOURCC_XRGB8888);
  cfg->flags  = 0;
  cfg->width  = bswap32(width);
  cfg->height = bswap32(height);
  cfg->stride = bswap32(width * 4);
  if (!dma_write(ramfb_sel, cfg, sizeof(RamfbCfg))) return false;

  kprintf("[ramfb] fw_cfg at %p, etc/ramfb selector %u: %u x %u scanning out of %p\n",
          g_base, static_cast<uint64_t>(ramfb_sel), width, height,
          reinterpret_cast<uint64_t>(pixels));
  return true;
}

uint64_t ramfb_fwcfg_base() { return g_base; }
