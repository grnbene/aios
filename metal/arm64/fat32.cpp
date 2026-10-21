/* FAT32 filesystem driver (metal/arm64/fat32.cpp).
 *
 * A real FAT32 over sdcard_read_block/sdcard_write_block: validated boot
 * sector, FAT chain walking, real directory entries, cluster allocation.
 * The previous file kept "files" in a RAM table, listed an invented
 * KERNEL8.IMG/START.ELF/SYSTEM.LOG directory whatever card was present, and
 * fat32_write_file() dumped its first 512 bytes into cluster 10 of the real
 * card with no directory entry - silently corrupting whichever file owned
 * that cluster. Nothing it reported about the volume was read from the
 * volume.
 *
 * Scope: 8.3 names, files anywhere in an existing directory tree (no mkdir),
 * long-file-name entries are skipped on read and never written. A name that
 * does not fit 8.3 is refused rather than mangled.
 */
#include "fat32.hpp"

#include "kio.hpp"
#include "sdcard.hpp"

namespace {

#pragma pack(push, 1)
struct MbrPartition {
  uint8_t  status;
  uint8_t  chs_first[3];
  uint8_t  type;
  uint8_t  chs_last[3];
  uint32_t lba_first;
  uint32_t sector_count;
};

struct Fat32Bpb {
  uint8_t  jmp[3];
  char     oem[8];
  uint16_t bytes_per_sector;
  uint8_t  sectors_per_cluster;
  uint16_t reserved_sectors;
  uint8_t  num_fats;
  uint16_t root_entry_count;
  uint16_t total_sectors_16;
  uint8_t  media_type;
  uint16_t sectors_per_fat_16;
  uint16_t sectors_per_track;
  uint16_t num_heads;
  uint32_t hidden_sectors;
  uint32_t total_sectors_32;
  uint32_t sectors_per_fat_32;
  uint16_t ext_flags;
  uint16_t fs_version;
  uint32_t root_cluster;
  uint16_t fs_info_sector;
  uint16_t backup_boot_sector;
  uint8_t  reserved[12];
  uint8_t  drive_number;
  uint8_t  reserved1;
  uint8_t  boot_signature;
  uint32_t volume_id;
  char     volume_label[11];
  char     fs_type[8];
};

struct FatDirEntry {
  char     name[11];   /* 8 + 3, space padded */
  uint8_t  attr;
  uint8_t  nt_res;
  uint8_t  create_time_tenth;
  uint16_t create_time;
  uint16_t create_date;
  uint16_t access_date;
  uint16_t first_cluster_hi;
  uint16_t write_time;
  uint16_t write_date;
  uint16_t first_cluster_lo;
  uint32_t file_size;
};
#pragma pack(pop)

constexpr uint8_t  ATTR_READ_ONLY = 0x01;
constexpr uint8_t  ATTR_HIDDEN    = 0x02;
constexpr uint8_t  ATTR_VOLUME_ID = 0x08;
constexpr uint8_t  ATTR_DIRECTORY = 0x10;
constexpr uint8_t  ATTR_ARCHIVE   = 0x20;
constexpr uint8_t  ATTR_LFN       = 0x0F;
constexpr uint32_t FAT_EOC        = 0x0FFFFFFFu;
constexpr uint32_t FAT_MASK       = 0x0FFFFFFFu;
constexpr uint32_t FREE_UNKNOWN   = 0xFFFFFFFFu;

Fat32FsStatus g_fs = {};
uint32_t g_fat_size = 0;      /* sectors per FAT copy */
uint8_t  g_num_fats = 0;
uint32_t g_fsinfo_sector = 0; /* absolute; 0 = none */
uint32_t g_next_free_hint = 2;

uint8_t g_buf[512] __attribute__((aligned(8)));       /* directory / data sector */
uint8_t g_fat_buf[512] __attribute__((aligned(8)));   /* one cached FAT sector */
uint32_t g_fat_cached = 0;                             /* absolute sector held in g_fat_buf, 0 = none */

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
void wr32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16); p[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t cluster_sector(uint32_t c) {
  return g_fs.data_start_sector + (c - 2u) * g_fs.sectors_per_cluster;
}
bool cluster_valid(uint32_t c) { return c >= 2 && c < g_fs.total_clusters + 2; }

/* The cached FAT sector is write-back: fat_set() marks it dirty, and it
 * reaches the card (both FAT copies) when another FAT sector is needed or
 * before a directory entry or FSInfo is written - so an entry never points
 * at a chain the card does not have. With this volume's 512-byte clusters,
 * writing both FATs on every fat_set() was three block writes per 512 bytes
 * of data: 100 KB took 3 s. */
bool g_fat_dirty = false;
bool fat_flush() {
  if (!g_fat_dirty || !g_fat_cached) return true;
  const uint32_t rel = g_fat_cached - g_fs.fat_start_sector;
  for (uint8_t i = 0; i < g_num_fats; ++i) {
    if (!sdcard_write_block(g_fs.fat_start_sector + i * g_fat_size + rel, g_fat_buf)) return false;
  }
  g_fat_dirty = false;
  return true;
}

bool fat_load(uint32_t abs_sector) {
  if (g_fat_cached == abs_sector) return true;
  if (!fat_flush()) return false;
  if (!sdcard_read_block(abs_sector, g_fat_buf)) return false;
  g_fat_cached = abs_sector;
  return true;
}

bool fat_get(uint32_t c, uint32_t& next) {
  const uint32_t off = c * 4u;
  if (!fat_load(g_fs.fat_start_sector + off / 512u)) return false;
  next = rd32(g_fat_buf + (off % 512u)) & FAT_MASK;
  return true;
}

bool fat_set(uint32_t c, uint32_t v) {
  const uint32_t off = c * 4u;
  const uint32_t rel = off / 512u;
  if (!fat_load(g_fs.fat_start_sector + rel)) return false;
  const uint32_t old = rd32(g_fat_buf + (off % 512u));
  wr32(g_fat_buf + (off % 512u), (old & 0xF0000000u) | (v & FAT_MASK));
  g_fat_dirty = true;
  return true;
}

void fsinfo_update() {
  fat_flush();
  if (!g_fsinfo_sector) return;
  if (!sdcard_read_block(g_fsinfo_sector, g_buf)) return;
  if (rd32(g_buf) != 0x41615252u || rd32(g_buf + 484) != 0x61417272u) return;
  wr32(g_buf + 488, g_fs.free_clusters);
  wr32(g_buf + 492, g_next_free_hint);
  sdcard_write_block(g_fsinfo_sector, g_buf);
}

bool alloc_cluster(uint32_t& out) {
  uint32_t start = cluster_valid(g_next_free_hint) ? g_next_free_hint : 2;
  const uint32_t end = g_fs.total_clusters + 2;
  for (uint32_t pass = 0; pass < 2; ++pass) {
    for (uint32_t c = start; c < end; ++c) {
      uint32_t v;
      if (!fat_get(c, v)) return false;
      if (v == 0) {
        if (!fat_set(c, FAT_EOC)) return false;
        out = c;
        g_next_free_hint = c + 1;
        if (g_fs.free_clusters != FREE_UNKNOWN && g_fs.free_clusters) g_fs.free_clusters--;
        return true;
      }
    }
    start = 2;
  }
  return false;
}

bool free_chain(uint32_t c) {
  unsigned guard = 0;
  while (cluster_valid(c) && guard++ < g_fs.total_clusters) {
    uint32_t next;
    if (!fat_get(c, next)) return false;
    if (!fat_set(c, 0)) return false;
    if (g_fs.free_clusters != FREE_UNKNOWN) g_fs.free_clusters++;
    if (next >= 0x0FFFFFF8u) break;
    c = next;
  }
  return true;
}

bool zero_cluster(uint32_t c) {
  uint8_t zero[512] = {};
  for (uint32_t s = 0; s < g_fs.sectors_per_cluster; ++s) {
    if (!sdcard_write_block(cluster_sector(c) + s, zero)) return false;
  }
  return true;
}

/* "wifi.cfg" -> "WIFI    CFG". Refuses names that do not fit. */
bool to_83(const char* name, size_t len, char out[11]) {
  for (int i = 0; i < 11; ++i) out[i] = ' ';
  size_t dot = len;
  for (size_t i = 0; i < len; ++i) if (name[i] == '.') dot = i;
  const size_t base_len = dot;
  const size_t ext_len = (dot < len) ? len - dot - 1 : 0;
  if (base_len == 0 || base_len > 8 || ext_len > 3) return false;
  auto put = [&](char c, int at) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (c < 0x20 || c == '"' || c == '*' || c == '+' || c == ',' || c == '/' || c == ':' || c == ';' ||
        c == '<' || c == '=' || c == '>' || c == '?' || c == '[' || c == '\\' || c == ']' || c == '|') return false;
    out[at] = c;
    return true;
  };
  for (size_t i = 0; i < base_len; ++i) if (!put(name[i], static_cast<int>(i))) return false;
  for (size_t i = 0; i < ext_len; ++i) if (!put(name[dot + 1 + i], static_cast<int>(8 + i))) return false;
  return true;
}

void from_83(const char raw[11], char* dst, size_t dst_max) {
  size_t idx = 0;
  for (int i = 0; i < 8 && raw[i] != ' ' && idx + 1 < dst_max; ++i) dst[idx++] = raw[i];
  if (raw[8] != ' ') {
    if (idx + 1 < dst_max) dst[idx++] = '.';
    for (int i = 8; i < 11 && raw[i] != ' ' && idx + 1 < dst_max; ++i) dst[idx++] = raw[i];
  }
  dst[idx] = '\0';
}

bool name_eq(const char a[11], const char b[11]) {
  for (int i = 0; i < 11; ++i) if (a[i] != b[i]) return false;
  return true;
}

struct EntryLoc {
  uint32_t sector;   /* absolute sector holding the entry */
  unsigned index;    /* 0..15 within that sector */
  FatDirEntry entry;
};

/* Walk one directory's cluster chain; visit(entry, loc) returns true to stop. */
template <typename F>
bool walk_dir(uint32_t dir_cluster, F&& visit, bool& stopped, uint32_t* last_cluster = nullptr) {
  stopped = false;
  uint32_t c = dir_cluster;
  unsigned guard = 0;
  while (cluster_valid(c) && guard++ < g_fs.total_clusters) {
    for (uint32_t s = 0; s < g_fs.sectors_per_cluster; ++s) {
      const uint32_t sec = cluster_sector(c) + s;
      if (!sdcard_read_block(sec, g_buf)) return false;
      for (unsigned i = 0; i < 16; ++i) {
        EntryLoc loc;
        loc.sector = sec;
        loc.index = i;
        const auto* e = reinterpret_cast<const FatDirEntry*>(g_buf + i * 32);
        loc.entry = *e;
        if (visit(loc)) { stopped = true; if (last_cluster) *last_cluster = c; return true; }
        if (static_cast<uint8_t>(e->name[0]) == 0x00) { if (last_cluster) *last_cluster = c; return true; }
      }
    }
    uint32_t next;
    if (!fat_get(c, next)) return false;
    if (last_cluster) *last_cluster = c;
    if (next >= 0x0FFFFFF8u) break;
    c = next;
  }
  return true;
}

/* Strip "/sd/" or "/" and split into directory components + final name. */
const char* strip_prefix(const char* path) {
  if (path[0] == '/' && path[1] == 's' && path[2] == 'd' && (path[3] == '/' || path[3] == '\0')) path += 3;
  while (*path == '/') ++path;
  return path;
}

bool find_in_dir(uint32_t dir_cluster, const char name83[11], EntryLoc& out) {
  bool stopped = false;
  bool ok = walk_dir(dir_cluster, [&](const EntryLoc& loc) {
    const uint8_t first = static_cast<uint8_t>(loc.entry.name[0]);
    if (first == 0xE5 || first == 0x00) return false;
    if ((loc.entry.attr & ATTR_LFN) == ATTR_LFN || (loc.entry.attr & ATTR_VOLUME_ID)) return false;
    if (!name_eq(loc.entry.name, name83)) return false;
    out = loc;
    return true;
  }, stopped);
  return ok && stopped;
}

/* Resolve every component but the last to a directory cluster. */
bool resolve_parent(const char* path, uint32_t& dir_cluster, const char*& leaf, size_t& leaf_len) {
  const char* p = strip_prefix(path);
  dir_cluster = g_fs.root_cluster;
  while (true) {
    const char* seg = p;
    while (*p && *p != '/') ++p;
    const size_t seg_len = static_cast<size_t>(p - seg);
    if (*p == '\0') {
      leaf = seg;
      leaf_len = seg_len;
      return seg_len > 0;
    }
    ++p;
    if (seg_len == 0) continue;
    char n83[11];
    if (!to_83(seg, seg_len, n83)) return false;
    EntryLoc loc;
    if (!find_in_dir(dir_cluster, n83, loc) || !(loc.entry.attr & ATTR_DIRECTORY)) return false;
    const uint32_t c = (static_cast<uint32_t>(loc.entry.first_cluster_hi) << 16) | loc.entry.first_cluster_lo;
    dir_cluster = c ? c : g_fs.root_cluster;
  }
}

bool write_entry(const EntryLoc& loc) {
  if (!fat_flush()) return false;                 /* the chain first, then the entry naming it */
  if (!sdcard_read_block(loc.sector, g_buf)) return false;
  auto* e = reinterpret_cast<FatDirEntry*>(g_buf + loc.index * 32);
  *e = loc.entry;
  return sdcard_write_block(loc.sector, g_buf);
}

/* A free (0xE5 or 0x00) slot in the directory, extending it by one cluster
 * if the chain is full. */
bool find_free_slot(uint32_t dir_cluster, EntryLoc& out) {
  bool stopped = false;
  uint32_t last = dir_cluster;
  bool ok = walk_dir(dir_cluster, [&](const EntryLoc& loc) {
    const uint8_t first = static_cast<uint8_t>(loc.entry.name[0]);
    if (first == 0xE5 || first == 0x00) { out = loc; return true; }
    return false;
  }, stopped, &last);
  if (!ok) return false;
  if (stopped) return true;
  uint32_t nc;
  if (!alloc_cluster(nc) || !zero_cluster(nc) || !fat_set(last, nc)) return false;
  out.sector = cluster_sector(nc);
  out.index = 0;
  out.entry = {};
  return true;
}

bool validate_bpb(const uint8_t* sec, const Fat32Bpb*& bpb) {
  if (sec[510] != 0x55 || sec[511] != 0xAA) return false;
  bpb = reinterpret_cast<const Fat32Bpb*>(sec);
  if (bpb->bytes_per_sector != 512) return false;
  const uint8_t spc = bpb->sectors_per_cluster;
  if (spc == 0 || (spc & (spc - 1)) != 0 || spc > 128) return false;
  if (bpb->num_fats == 0 || bpb->num_fats > 2) return false;
  if (bpb->reserved_sectors == 0) return false;
  if (bpb->sectors_per_fat_16 != 0 || bpb->root_entry_count != 0) return false; /* FAT12/16 */
  if (bpb->sectors_per_fat_32 == 0 || bpb->root_cluster < 2) return false;
  const uint32_t total = bpb->total_sectors_32 ? bpb->total_sectors_32 : bpb->total_sectors_16;
  if (total == 0) return false;
  return true;
}

bool mount_at(uint32_t lba) {
  if (!sdcard_read_block(lba, g_buf)) return false;
  const Fat32Bpb* bpb = nullptr;
  if (!validate_bpb(g_buf, bpb)) return false;

  const uint32_t total = bpb->total_sectors_32 ? bpb->total_sectors_32 : bpb->total_sectors_16;
  g_fs.lba_start = lba;
  g_fs.sectors_per_cluster = bpb->sectors_per_cluster;
  g_fs.bytes_per_cluster = g_fs.sectors_per_cluster * 512u;
  g_fat_size = bpb->sectors_per_fat_32;
  g_num_fats = bpb->num_fats;
  g_fs.fat_start_sector = lba + bpb->reserved_sectors;
  g_fs.data_start_sector = g_fs.fat_start_sector + g_num_fats * g_fat_size;
  g_fs.root_cluster = bpb->root_cluster;
  const uint32_t data_sectors = total - (bpb->reserved_sectors + g_num_fats * g_fat_size);
  g_fs.total_clusters = data_sectors / g_fs.sectors_per_cluster;
  g_fsinfo_sector = (bpb->fs_info_sector && bpb->fs_info_sector != 0xFFFF) ? lba + bpb->fs_info_sector : 0;

  for (int i = 0; i < 11; ++i) g_fs.volume_label[i] = bpb->volume_label[i];
  g_fs.volume_label[11] = '\0';
  for (int i = 10; i >= 0 && g_fs.volume_label[i] == ' '; --i) g_fs.volume_label[i] = '\0';
  return true;
}

}  /* namespace */

bool fat32_mount() {
  /* Idempotent. The card lives on the SDHOST controller, its own block, so
   * a re-mount no longer resets the Arasan the Wi-Fi radio hangs off. Still:
   * do not re-touch hardware that already came up. */
  if (g_fs.mounted) return true;

  g_fs = {};
  g_fat_cached = 0;
  g_fat_dirty = false;
  g_fsinfo_sector = 0;

  if (!sdcard_init()) return false;

  if (!sdcard_read_block(0, g_buf)) {
    kputs("[fs] sector 0 unreadable\n");
    return false;
  }

  bool found = false;
  if (g_buf[510] == 0x55 && g_buf[511] == 0xAA) {
    /* Superfloppy first (a FAT32 VBR also carries 0x55AA), then MBR
     * partitions by type. */
    const Fat32Bpb* bpb = nullptr;
    if (validate_bpb(g_buf, bpb)) {
      found = mount_at(0);
    } else {
      MbrPartition parts[4];
      for (int i = 0; i < 4; ++i) {
        const uint8_t* p = g_buf + 0x1BE + i * 16;
        parts[i].type = p[4];
        parts[i].lba_first = rd32(p + 8);
        parts[i].sector_count = rd32(p + 12);
      }
      for (int i = 0; i < 4 && !found; ++i) {
        if (parts[i].lba_first == 0 || parts[i].sector_count == 0) continue;
        if (parts[i].type != 0x0B && parts[i].type != 0x0C && parts[i].type != 0x0E && parts[i].type != 0xEF) continue;
        found = mount_at(parts[i].lba_first);
      }
    }
  }
  if (!found) {
    kputs("[fs] no FAT32 volume: neither sector 0 nor any MBR partition carries a valid FAT32 boot sector\n");
    g_fs = {};
    return false;
  }

  /* Free space: FSInfo if it is sane, else count the FAT (bounded to keep a
   * 32 GB card from taking a minute to mount; beyond that it is unknown). */
  g_fs.free_clusters = FREE_UNKNOWN;
  g_next_free_hint = 2;
  if (g_fsinfo_sector && sdcard_read_block(g_fsinfo_sector, g_buf) &&
      rd32(g_buf) == 0x41615252u && rd32(g_buf + 484) == 0x61417272u) {
    const uint32_t fc = rd32(g_buf + 488);
    const uint32_t nf = rd32(g_buf + 492);
    if (fc != 0xFFFFFFFFu && fc <= g_fs.total_clusters) g_fs.free_clusters = fc;
    if (cluster_valid(nf)) g_next_free_hint = nf;
  }
  if (g_fs.free_clusters == FREE_UNKNOWN && g_fat_size <= 2048) {
    uint32_t free_count = 0;
    for (uint32_t c = 2; c < g_fs.total_clusters + 2; ++c) {
      uint32_t v;
      if (!fat_get(c, v)) break;
      if (v == 0) ++free_count;
    }
    g_fs.free_clusters = free_count;
  }

  /* Root directory census: what is actually there. */
  uint32_t files = 0;
  bool stopped = false;
  walk_dir(g_fs.root_cluster, [&](const EntryLoc& loc) {
    const uint8_t first = static_cast<uint8_t>(loc.entry.name[0]);
    if (first == 0xE5 || first == 0x00) return false;
    if ((loc.entry.attr & ATTR_LFN) == ATTR_LFN || (loc.entry.attr & ATTR_VOLUME_ID)) return false;
    ++files;
    return false;
  }, stopped);
  g_fs.total_files_indexed = files;

  g_fs.mounted = true;
  return true;
}

size_t fat32_list(const char* path, Fat32FileInfo* out_entries, size_t max_entries) {
  if (!g_fs.mounted || out_entries == nullptr || max_entries == 0) return 0;

  uint32_t dir = g_fs.root_cluster;
  if (path && *strip_prefix(path)) {
    /* Resolve a directory path: treat the whole thing as components. */
    char tmp[128];
    size_t n = 0;
    const char* p = strip_prefix(path);
    while (p[n] && n + 2 < sizeof(tmp)) { tmp[n] = p[n]; ++n; }
    tmp[n++] = '/'; tmp[n++] = 'x'; tmp[n] = '\0';  /* fake leaf so resolve_parent walks every component */
    const char* leaf; size_t leaf_len;
    if (!resolve_parent(tmp, dir, leaf, leaf_len)) return 0;
  }

  size_t count = 0;
  bool stopped = false;
  walk_dir(dir, [&](const EntryLoc& loc) {
    const uint8_t first = static_cast<uint8_t>(loc.entry.name[0]);
    if (first == 0xE5 || first == 0x00) return false;
    if ((loc.entry.attr & ATTR_LFN) == ATTR_LFN || (loc.entry.attr & ATTR_VOLUME_ID)) return false;
    if (loc.entry.name[0] == '.') return false; /* . and .. */
    Fat32FileInfo& e = out_entries[count++];
    from_83(loc.entry.name, e.name, sizeof(e.name));
    e.size_bytes = loc.entry.file_size;
    e.first_cluster = (static_cast<uint32_t>(loc.entry.first_cluster_hi) << 16) | loc.entry.first_cluster_lo;
    e.is_directory = (loc.entry.attr & ATTR_DIRECTORY) != 0;
    e.is_hidden = (loc.entry.attr & ATTR_HIDDEN) != 0;
    return count >= max_entries;
  }, stopped);
  return count;
}

bool fat32_file_size(const char* filename, size_t& out_size) {
  out_size = 0;
  if (!g_fs.mounted || filename == nullptr) return false;
  uint32_t dir; const char* leaf; size_t leaf_len;
  if (!resolve_parent(filename, dir, leaf, leaf_len)) return false;
  char n83[11];
  if (!to_83(leaf, leaf_len, n83)) return false;
  EntryLoc loc;
  if (!find_in_dir(dir, n83, loc) || (loc.entry.attr & ATTR_DIRECTORY)) return false;
  out_size = loc.entry.file_size;
  return true;
}

bool fat32_read_file(const char* filename, void* dst, size_t max_len, size_t& out_len) {
  out_len = 0;
  if (!g_fs.mounted || filename == nullptr || dst == nullptr) return false;

  uint32_t dir; const char* leaf; size_t leaf_len;
  if (!resolve_parent(filename, dir, leaf, leaf_len)) return false;
  char n83[11];
  if (!to_83(leaf, leaf_len, n83)) return false;
  EntryLoc loc;
  if (!find_in_dir(dir, n83, loc) || (loc.entry.attr & ATTR_DIRECTORY)) return false;

  uint32_t c = (static_cast<uint32_t>(loc.entry.first_cluster_hi) << 16) | loc.entry.first_cluster_lo;
  size_t remaining = loc.entry.file_size < max_len ? loc.entry.file_size : max_len;
  auto* d = static_cast<uint8_t*>(dst);
  unsigned guard = 0;
  while (remaining > 0 && cluster_valid(c) && guard++ < g_fs.total_clusters) {
    for (uint32_t s = 0; s < g_fs.sectors_per_cluster && remaining > 0; ++s) {
      if (!sdcard_read_block(cluster_sector(c) + s, g_buf)) return false;
      const size_t n = remaining < 512 ? remaining : 512;
      for (size_t i = 0; i < n; ++i) d[out_len + i] = g_buf[i];
      out_len += n;
      remaining -= n;
    }
    uint32_t next;
    if (!fat_get(c, next)) return false;
    if (next >= 0x0FFFFFF8u) break;
    c = next;
  }
  return true;
}

bool fat32_write_file(const char* filename, const void* src, size_t len) {
  if (!g_fs.mounted || filename == nullptr || (src == nullptr && len)) return false;

  uint32_t dir; const char* leaf; size_t leaf_len;
  if (!resolve_parent(filename, dir, leaf, leaf_len)) {
    kprintf("[fs] write %s: parent directory does not exist\n", filename);
    return false;
  }
  char n83[11];
  if (!to_83(leaf, leaf_len, n83)) {
    kprintf("[fs] write %s: name does not fit 8.3 (long names are not written)\n", filename);
    return false;
  }

  EntryLoc loc;
  bool existing = find_in_dir(dir, n83, loc);
  if (existing) {
    if (loc.entry.attr & ATTR_DIRECTORY) return false;
    const uint32_t old = (static_cast<uint32_t>(loc.entry.first_cluster_hi) << 16) | loc.entry.first_cluster_lo;
    if (old && !free_chain(old)) return false;
  } else {
    if (!find_free_slot(dir, loc)) {
      kprintf("[fs] write %s: no directory slot and no cluster to extend it\n", filename);
      return false;
    }
    loc.entry = {};
  }

  /* Data: one chain, written cluster by cluster. */
  uint32_t first = 0, prev = 0;
  const auto* s = static_cast<const uint8_t*>(src);
  size_t written = 0;
  while (written < len) {
    uint32_t c;
    if (!alloc_cluster(c)) {
      kprintf("[fs] write %s: volume full after %u bytes\n", filename, static_cast<uint64_t>(written));
      if (first) free_chain(first);
      return false;
    }
    if (!first) first = c;
    if (prev && !fat_set(prev, c)) return false;
    prev = c;
    for (uint32_t sec = 0; sec < g_fs.sectors_per_cluster && written < len; ++sec) {
      uint8_t block[512] = {};
      const size_t n = (len - written) < 512 ? (len - written) : 512;
      for (size_t i = 0; i < n; ++i) block[i] = s[written + i];
      if (!sdcard_write_block(cluster_sector(c) + sec, block)) {
        kprintf("[fs] write %s: sector write failed\n", filename);
        free_chain(first);
        return false;
      }
      written += n;
    }
  }

  for (int i = 0; i < 11; ++i) loc.entry.name[i] = n83[i];
  loc.entry.attr = ATTR_ARCHIVE;
  loc.entry.nt_res = 0;
  loc.entry.first_cluster_hi = static_cast<uint16_t>(first >> 16);
  loc.entry.first_cluster_lo = static_cast<uint16_t>(first & 0xFFFFu);
  loc.entry.file_size = static_cast<uint32_t>(len);
  /* No RTC on this board: a fixed valid stamp (2026-01-01 00:00) rather than 0. */
  loc.entry.write_date = loc.entry.create_date = loc.entry.access_date = static_cast<uint16_t>(((2026 - 1980) << 9) | (1 << 5) | 1);
  loc.entry.write_time = loc.entry.create_time = 0;
  if (!write_entry(loc)) return false;

  fsinfo_update();
  if (!existing) g_fs.total_files_indexed++;
  return true;
}

bool fat32_delete_file(const char* filename) {
  if (!g_fs.mounted || filename == nullptr) return false;
  uint32_t dir; const char* leaf; size_t leaf_len;
  if (!resolve_parent(filename, dir, leaf, leaf_len)) return false;
  char n83[11];
  if (!to_83(leaf, leaf_len, n83)) return false;
  EntryLoc loc;
  if (!find_in_dir(dir, n83, loc)) return false;
  if (loc.entry.attr & ATTR_DIRECTORY) return false;
  const uint32_t c = (static_cast<uint32_t>(loc.entry.first_cluster_hi) << 16) | loc.entry.first_cluster_lo;
  if (c && !free_chain(c)) return false;
  loc.entry.name[0] = static_cast<char>(0xE5);
  if (!write_entry(loc)) return false;
  fsinfo_update();
  if (g_fs.total_files_indexed) g_fs.total_files_indexed--;
  return true;
}

const Fat32FsStatus& fat32_status() {
  return g_fs;
}

void fat32_report() {
  char b1[32], b2[32];
  kputs("  fat32 volume:\n");
  if (!g_fs.mounted) {
    kputs("    status      : not mounted\n");
    return;
  }
  kprintf("    status      : mounted at LBA %u (label '%s')\n",
          static_cast<uint64_t>(g_fs.lba_start), g_fs.volume_label[0] ? g_fs.volume_label : "(none)");
  kprintf("    cluster size: %u bytes (%u sectors)\n",
          static_cast<uint64_t>(g_fs.bytes_per_cluster), static_cast<uint64_t>(g_fs.sectors_per_cluster));
  const uint64_t total = static_cast<uint64_t>(g_fs.total_clusters) * g_fs.bytes_per_cluster;
  if (g_fs.free_clusters == FREE_UNKNOWN) {
    kprintf("    capacity    : %s total, free space unknown (no FSInfo, FAT too large to count at mount)\n",
            khuman(total, b1, sizeof(b1)));
  } else {
    kprintf("    capacity    : %s total, %s free\n", khuman(total, b1, sizeof(b1)),
            khuman(static_cast<uint64_t>(g_fs.free_clusters) * g_fs.bytes_per_cluster, b2, sizeof(b2)));
  }
  kprintf("    root dir    : %u entries (8.3 names; long-name entries not shown)\n",
          static_cast<uint64_t>(g_fs.total_files_indexed));
}
