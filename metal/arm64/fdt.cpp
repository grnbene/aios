/* Flattened device tree parser (metal/arm64/fdt.cpp).
 *
 * A single pass over the structure block. Cell sizes are tracked per depth
 * because #address-cells/#size-cells declared on a node describe its
 * CHILDREN's reg properties, not its own — getting that backwards reads the
 * GIC's base address out of the middle of a 64-bit DRAM size.
 */
#include "fdt.hpp"
#include "kio.hpp"

namespace {

constexpr uint32_t FDT_MAGIC      = 0xD00DFEEDu;
constexpr uint32_t FDT_BEGIN_NODE = 1;
constexpr uint32_t FDT_END_NODE   = 2;
constexpr uint32_t FDT_PROP       = 3;
constexpr uint32_t FDT_NOP        = 4;
constexpr uint32_t FDT_END        = 9;

constexpr unsigned MAX_DEPTH = 16;

FdtInfo g_info{};

uint32_t be32(const void* p) {
  const auto* b = static_cast<const uint8_t*>(p);
  return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
         (static_cast<uint32_t>(b[2]) << 8)  |  static_cast<uint32_t>(b[3]);
}

/* Read `cells` big-endian 32-bit cells as one value. reg entries are a run of
 * cells, so this is how both an address and a size come out. */
uint64_t read_cells(const uint8_t* p, uint32_t cells) {
  uint64_t v = 0;
  for (uint32_t i = 0; i < cells; ++i) v = (v << 32) | be32(p + i * 4);
  return v;
}

bool str_eq(const char* a, const char* b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}

bool starts_with(const char* s, const char* prefix) {
  while (*prefix) { if (*s != *prefix) return false; ++s; ++prefix; }
  return true;
}

/* A "compatible" property is a list of NUL-separated strings. */
bool compatible_has(const uint8_t* val, uint32_t len, const char* want) {
  uint32_t i = 0;
  while (i < len) {
    const char* s = reinterpret_cast<const char*>(val + i);
    if (str_eq(s, want)) return true;
    while (i < len && val[i]) ++i;
    ++i;
  }
  return false;
}

void copy_str(char* dst, size_t cap, const char* src) {
  size_t i = 0;
  while (src[i] && i + 1 < cap) { dst[i] = src[i]; ++i; }
  dst[i] = '\0';
}

/* Which node are we inside, and what did we learn about it so far. A node's
 * reg can appear before its compatible, so both are buffered until END_NODE. */
struct NodeState {
  const char* name;
  const uint8_t* reg;
  uint32_t reg_len;
  const uint8_t* compat;
  uint32_t compat_len;
  const uint8_t* interrupts;
  uint32_t interrupts_len;
  const uint8_t* method;
  uint32_t method_len;
  const uint8_t* format;
  uint32_t format_len;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint64_t device_type_is_memory;
  uint64_t mpidr;
  bool has_mpidr;
};

void finish_node(FdtInfo& out, const NodeState& n, uint32_t addr_cells, uint32_t size_cells,
                 unsigned depth, bool in_cpus) {
  const uint8_t* reg = n.reg;
  auto reg_addr = [&](unsigned idx) -> uint64_t {
    return read_cells(reg + idx * (addr_cells + size_cells) * 4, addr_cells);
  };
  auto reg_size = [&](unsigned idx) -> uint64_t {
    return read_cells(reg + (idx * (addr_cells + size_cells) + addr_cells) * 4, size_cells);
  };
  uint32_t reg_entries = 0;
  if (reg && (addr_cells + size_cells) > 0) reg_entries = n.reg_len / ((addr_cells + size_cells) * 4);

  if (n.device_type_is_memory && reg_entries >= 1 && out.ram_size == 0) {
    out.ram_base = reg_addr(0);
    out.ram_size = reg_size(0);
  }

  if (n.compat) {
    if (compatible_has(n.compat, n.compat_len, "arm,pl011") && reg_entries >= 1) {
      uint64_t addr = reg_addr(0);
      /* Translate Raspberry Pi VideoCore 0x7E... bus addresses to physical ARM MMIO */
      if ((addr & 0xFF000000ull) == 0x7E000000ull) {
        addr = (addr & 0x00FFFFFFull) | 0x3F000000ull;
      }
      out.uart_base = addr;
      out.uart_size = reg_size(0);
      if (n.interrupts && n.interrupts_len >= 12) {
        out.uart_intid = 32 + be32(n.interrupts + 4);
      }
    }
    if (compatible_has(n.compat, n.compat_len, "arm,gic-v3") && reg_entries >= 2) {
      out.gic_version = 3;
      out.gicd_base = reg_addr(0);
      out.gicr_base = reg_addr(1);
    } else if ((compatible_has(n.compat, n.compat_len, "arm,cortex-a15-gic") ||
                compatible_has(n.compat, n.compat_len, "arm,gic-400") ||
                compatible_has(n.compat, n.compat_len, "arm,cortex-a7-gic")) &&
               reg_entries >= 2 && out.gic_version == 0) {
      out.gic_version = 2;
      out.gicd_base = reg_addr(0);
      out.gicc_base = reg_addr(1);
    }
    if (compatible_has(n.compat, n.compat_len, "virtio,mmio") && reg_entries >= 1 &&
        out.virtio_count < 32) {
      auto& v = out.virtio[out.virtio_count++];
      v.base = reg_addr(0);
      v.size = reg_size(0);
      v.intid = (n.interrupts && n.interrupts_len >= 12) ? 32 + be32(n.interrupts + 4) : 0;
    }
    if (compatible_has(n.compat, n.compat_len, "qemu,fw-cfg-mmio") && reg_entries >= 1) {
      out.fwcfg_base = reg_addr(0);
    }
    if (compatible_has(n.compat, n.compat_len, "simple-framebuffer") && reg_entries >= 1 &&
        out.fb_base == 0) {
      out.fb_base = reg_addr(0);
      out.fb_size = reg_size(0);
      out.fb_width = n.width;
      out.fb_height = n.height;
      out.fb_stride = n.stride;
      if (n.format) copy_str(out.fb_format, sizeof(out.fb_format),
                             reinterpret_cast<const char*>(n.format));
    }
    if (compatible_has(n.compat, n.compat_len, "arm,armv8-timer") ||
        compatible_has(n.compat, n.compat_len, "arm,armv7-timer")) {
      /* interrupts = <type ppi flags> x4: secure phys, non-secure phys,
       * virtual, hyp. The second triple is the one EL1 owns. */
      if (n.interrupts && n.interrupts_len >= 24) {
        out.timer_intid_phys = 16 + be32(n.interrupts + 16);
      }
    }
    if (compatible_has(n.compat, n.compat_len, "arm,psci-0.2") ||
        compatible_has(n.compat, n.compat_len, "arm,psci-1.0") ||
        compatible_has(n.compat, n.compat_len, "arm,psci")) {
      out.psci_present = true;
      out.psci_use_hvc = n.method && n.method_len >= 3 &&
                         starts_with(reinterpret_cast<const char*>(n.method), "hvc");
    }
  }

  /* Root is depth 1, so /cpus is depth 2 and each /cpus/cpu@N is depth 3. */
  if (in_cpus && depth == 3 && starts_with(n.name, "cpu@") && out.cpu_count < 8) {
    out.cpu_mpidr[out.cpu_count] = n.has_mpidr ? n.mpidr : reg_addr(0);
    ++out.cpu_count;
  }
}

}  /* namespace */

bool fdt_parse(uint64_t dtb, FdtInfo& out) {
  out = FdtInfo{};
  out.dtb = dtb;
  auto setup_rpi3_fallback = [&]() {
    out.dtb_size = 0;
    copy_str(out.model, sizeof(out.model), "Raspberry Pi 3 (Hardware Substrate)");
    out.ram_base = 0x00000000ull;
    out.ram_size = 0x3B000000ull; /* 944MB DRAM, GPU owns top */
    out.uart_base = 0x3F201000ull;
    out.uart_intid = 57;
    out.timer_intid_phys = 30;
    out.cpu_count = 4;
    out.valid = true;
    g_info = out;
    return true;
  };

  if (!dtb || (dtb & 3)) return setup_rpi3_fallback();

  const auto* base = reinterpret_cast<const uint8_t*>(dtb);
  if (be32(base) != FDT_MAGIC) return setup_rpi3_fallback();

  const uint32_t totalsize   = be32(base + 4);
  const uint32_t off_struct  = be32(base + 8);
  const uint32_t off_strings = be32(base + 12);
  const uint32_t size_struct = be32(base + 36);
  if (totalsize < 64 || off_struct >= totalsize || off_strings >= totalsize) return setup_rpi3_fallback();

  out.dtb_size = totalsize;

  const uint8_t* strings = base + off_strings;
  const uint8_t* p   = base + off_struct;
  const uint8_t* end = p + (size_struct ? size_struct : totalsize - off_struct);

  /* Cells declared by a node apply to its children, so index by depth. The
   * device tree spec's defaults when a parent says nothing are 2 and 1. */
  uint32_t addr_cells[MAX_DEPTH];
  uint32_t size_cells[MAX_DEPTH];
  for (unsigned i = 0; i < MAX_DEPTH; ++i) { addr_cells[i] = 2; size_cells[i] = 1; }

  unsigned depth = 0;
  bool in_cpus = false;
  unsigned cpus_depth = 0;
  NodeState node{};
  NodeState stack[MAX_DEPTH];

  while (p + 4 <= end) {
    const uint32_t token = be32(p);
    p += 4;
    if (token == FDT_END) break;
    if (token == FDT_NOP) continue;

    if (token == FDT_BEGIN_NODE) {
      const char* name = reinterpret_cast<const char*>(p);
      size_t len = 0;
      while (p + len < end && p[len]) ++len;
      p += (len + 1 + 3) & ~static_cast<size_t>(3);

      if (depth < MAX_DEPTH) stack[depth] = node;
      node = NodeState{};
      node.name = name;

      ++depth;
      if (depth < MAX_DEPTH) {
        addr_cells[depth] = addr_cells[depth - 1];
        size_cells[depth] = size_cells[depth - 1];
      }
      if (depth == 2 && str_eq(name, "cpus")) { in_cpus = true; cpus_depth = depth; }
      continue;
    }

    if (token == FDT_END_NODE) {
      if (depth >= 1 && depth < MAX_DEPTH) {
        const uint32_t ac = addr_cells[depth - 1];
        const uint32_t sc = size_cells[depth - 1];
        finish_node(out, node, ac, sc, depth, in_cpus);
        if (in_cpus && depth == cpus_depth) in_cpus = false;
        node = stack[depth - 1];
      }
      if (depth) --depth;
      continue;
    }

    if (token == FDT_PROP) {
      if (p + 8 > end) break;
      const uint32_t len     = be32(p);
      const uint32_t nameoff = be32(p + 4);
      p += 8;
      const uint8_t* val = p;
      p += (len + 3) & ~3u;
      if (p > end) break;

      const char* pname = reinterpret_cast<const char*>(strings + nameoff);

      if (str_eq(pname, "#address-cells") && len >= 4 && depth < MAX_DEPTH) {
        addr_cells[depth] = be32(val);
      } else if (str_eq(pname, "#size-cells") && len >= 4 && depth < MAX_DEPTH) {
        size_cells[depth] = be32(val);
      } else if (str_eq(pname, "reg")) {
        node.reg = val; node.reg_len = len;
      } else if (str_eq(pname, "compatible")) {
        node.compat = val; node.compat_len = len;
      } else if (str_eq(pname, "interrupts")) {
        node.interrupts = val; node.interrupts_len = len;
      } else if (str_eq(pname, "method")) {
        node.method = val; node.method_len = len;
      } else if (str_eq(pname, "format")) {
        node.format = val; node.format_len = len;
      } else if (str_eq(pname, "width") && len >= 4) {
        node.width = be32(val);
      } else if (str_eq(pname, "height") && len >= 4) {
        node.height = be32(val);
      } else if (str_eq(pname, "stride") && len >= 4) {
        node.stride = be32(val);
      } else if (str_eq(pname, "device_type")) {
        node.device_type_is_memory = str_eq(reinterpret_cast<const char*>(val), "memory") ? 1 : 0;
      } else if (str_eq(pname, "model") && depth == 1 && out.model[0] == '\0') {
        copy_str(out.model, sizeof(out.model), reinterpret_cast<const char*>(val));
      }
      continue;
    }

    /* An unknown token means the walk has lost sync; stop rather than march
     * off the end of the blob interpreting garbage as node names. */
    break;
  }

  if (out.cpu_count == 0) out.cpu_count = 1;
  if (out.timer_intid_phys == 0) out.timer_intid_phys = 30;
  if (out.ram_base == 0 && out.ram_size >= 0x3F000000ull) {
    out.ram_size = 0x3B000000ull; /* Cap DRAM to 944MB on Raspberry Pi to avoid MMIO overlap */
  }
  if (out.uart_base == 0 && out.ram_base == 0) {
    out.uart_base = 0x3F201000ull;
  }
  out.valid = out.ram_size != 0;
  g_info = out;
  return out.valid;
}

const FdtInfo& fdt_info() { return g_info; }
