// Drives the AOS volume on a file-backed block device: format, write, append,
// remount, verify, fill past a half to force compaction, remount, verify.
#include "aios/hal/volume.hpp"
#include <cstdio>
#include <cstring>
using namespace aios; using namespace aios::hal;
static int fails = 0;
static void check(bool ok, const char* what) { printf("  %s  %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "/tmp/aios-volume.img";
  ::remove(path);
  auto dev = make_file_block_device(path, 4 << 20);   // 4 MiB: two 2 MiB halves
  if (!dev.ok()) { printf("device: %s\n", dev.error().str().c_str()); return 1; }
  check(!Volume::probe(**dev), "blank device is not a volume");
  check(Volume::format(**dev, "check").ok(), "format");
  std::string label; check(Volume::probe(**dev, &label) && label == "check", "probe finds the label");
  { Volume v; check(v.mount(**dev).ok(), "mount"); 
    check(v.write_file("/aios/root/kernel.identity", span_of(std::string("ID"))).ok(), "write_file");
    check(v.append_file("/aios/root/store/objects.log", span_of(std::string("rec1|"))).ok() && v.append_file("/aios/root/store/objects.log", span_of(std::string("rec2|"))).ok(), "append_file ×2");
    check(v.make_dir("/aios/root/checkpoints/e1").ok(), "make_dir");
    check(v.write_file("/aios/root/kernel.identity", span_of(std::string("ID2"))).ok(), "overwrite");
    auto l = v.list_dir("/aios/root"); check(l.ok() && l->size() == 3, "list_dir sees 3 entries");
  }
  { Volume v; check(v.mount(**dev).ok(), "remount");
    auto r = v.read_file("/aios/root/kernel.identity"); check(r.ok() && string_of(*r) == "ID2", "overwrite survived remount");
    auto a = v.read_file("/aios/root/store/objects.log"); check(a.ok() && string_of(*a) == "rec1|rec2|", "appends survived remount in order");
    check(v.exists("/aios/root/checkpoints/e1"), "directory survived remount");
    printf("  records=%llu used=%llu sectors gen=%llu\n", (unsigned long long)v.info().records, (unsigned long long)v.info().log_used_sectors, (unsigned long long)v.info().generation);
    // Fill: 3000 × 1 KiB writes to the same 10 files → ~3 MiB of log in a 2 MiB half → compaction.
    ByteBuffer kb(1024, 'x');
    bool ok = true; for (int i = 0; i < 3000 && ok; ++i) { std::string p = "/aios/root/memory/" + std::to_string(i % 10) + ".mem"; kb[0] = (uint8_t)(i & 0xFF); ok = v.write_file(p, kb).ok(); }
    check(ok, "3000 writes accepted");
    check(v.info().compactions >= 1, "compaction happened");
    printf("  compactions=%u active_half=%u records=%llu used=%llu sectors\n", v.info().compactions, v.info().active_half, (unsigned long long)v.info().records, (unsigned long long)v.info().log_used_sectors);
  }
  { Volume v; check(v.mount(**dev).ok(), "remount after compaction");
    auto r = v.read_file("/aios/root/memory/9.mem"); check(r.ok() && r->size() == 1024 && (*r)[0] == (uint8_t)(2999 & 0xFF), "latest write to each file survived compaction");
    auto id = v.read_file("/aios/root/kernel.identity"); check(id.ok() && string_of(*id) == "ID2", "older file survived compaction");
    auto a = v.read_file("/aios/root/store/objects.log"); check(a.ok() && string_of(*a) == "rec1|rec2|", "appended log survived compaction");
  }
  // Torn tail: corrupt the checksum of the last record and remount — the log must end cleanly before it.
  { Volume v; v.mount(**dev); uint64_t used = v.info().log_used_sectors; uint32_t half = v.info().active_half;
    (void)used; (void)half;
    check(v.write_file("/torn", span_of(std::string("will be torn"))).ok(), "write a record to tear");
    uint64_t lba = 1 + (uint64_t)v.info().active_half * v.info().half_sectors + v.info().log_used_sectors - 1;
    uint8_t sec[512]; (*dev)->read(lba, 1, sec);
    sec[48 + 2] ^= 0xFF;   /* inside the checksummed payload */
    (*dev)->write(lba, 1, sec); (*dev)->flush();
  }
  { Volume v; check(v.mount(**dev).ok(), "mount with a torn tail"); check(!v.exists("/torn"), "torn record dropped, everything before it kept"); check(v.exists("/aios/root/kernel.identity"), "earlier records intact"); }
  printf("%s\n", fails ? "VOLUME: FAIL" : "VOLUME: PASS");
  dev->reset();                      // close the device before removing its file
  if (argc <= 1) ::remove(path);     // our own scratch image; a path the caller named is theirs to keep
  return fails ? 1 : 0;
}
