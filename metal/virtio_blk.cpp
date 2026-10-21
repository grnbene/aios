// virtio-blk over the legacy PCI I/O transport: the first block driver of the
// bare-metal substrate. Polled, one request at a time; enough to give the
// kernel a persistent root with no host anywhere.
#include "aios/hal/block.hpp"
#include <cstring>
#include <memory>

extern "C" void* metal_alloc_pages(size_t bytes);

namespace {
inline void outb(uint16_t p, uint8_t v) { asm volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
inline void outw(uint16_t p, uint16_t v) { asm volatile("outw %0, %1" : : "a"(v), "Nd"(p)); }
inline void outl(uint16_t p, uint32_t v) { asm volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
inline uint8_t inb(uint16_t p) { uint8_t v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
inline uint16_t inw(uint16_t p) { uint16_t v; asm volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
inline uint32_t inl(uint16_t p) { uint32_t v; asm volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
uint32_t pci_read(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off) { outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC)); return inl(0xCFC); }
void pci_write(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off, uint32_t v) { outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC)); outl(0xCFC, v); }

struct __attribute__((packed)) VringDesc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };
struct __attribute__((packed)) VringUsedElem { uint32_t id; uint32_t len; };
struct __attribute__((packed)) BlkReq { uint32_t type; uint32_t reserved; uint64_t sector; };
constexpr uint16_t DESC_NEXT = 1, DESC_WRITE = 2;
constexpr uint32_t BLK_IN = 0, BLK_OUT = 1, BLK_FLUSH = 4;
constexpr uint32_t F_FLUSH = 1u << 9;
constexpr size_t CHUNK = 64 * 1024;

class VirtioBlk final : public aios::hal::BlockDevice {
 public:
  bool init(uint16_t io, uint32_t bus, uint32_t dev, uint32_t fn) {
    io_ = io;
    pci_write(bus, dev, fn, 0x04, pci_read(bus, dev, fn, 0x04) | 0x4);   // bus master
    outb(io_ + 0x12, 0); outb(io_ + 0x12, 1); outb(io_ + 0x12, 1 | 2);   // reset, ack, driver
    uint32_t feats = inl(io_ + 0x00);
    has_flush_ = feats & F_FLUSH;
    outl(io_ + 0x04, feats & F_FLUSH);
    outw(io_ + 0x0E, 0);
    qsize_ = inw(io_ + 0x0C);
    if (qsize_ == 0 || qsize_ > 1024) return false;
    size_t desc_bytes = 16 * qsize_, avail_bytes = 6 + 2 * qsize_;
    size_t used_off = ((desc_bytes + avail_bytes) + 4095) & ~4095ull;
    size_t total = used_off + 6 + 8 * qsize_;
    uint8_t* ring = (uint8_t*)metal_alloc_pages((total + 4095) & ~4095ull);
    if (!ring) return false;
    desc_ = (VringDesc*)ring; avail_idx_ = (uint16_t*)(ring + desc_bytes + 2); avail_ring_ = (uint16_t*)(ring + desc_bytes + 4);
    used_idx_ = (uint16_t*)(ring + used_off + 2); used_ring_ = (VringUsedElem*)(ring + used_off + 4);
    outl(io_ + 0x08, (uint32_t)((uintptr_t)ring >> 12));
    outb(io_ + 0x12, 1 | 2 | 4);                                            // driver ok
    uint32_t lo = inl(io_ + 0x14), hi = inl(io_ + 0x18);
    sectors_ = ((uint64_t)hi << 32) | lo;
    hdr_ = (BlkReq*)metal_alloc_pages(4096); status_ = (uint8_t*)hdr_ + 512; bounce_ = (uint8_t*)metal_alloc_pages(CHUNK);
    name_ = "virtio-blk";
    return sectors_ > 0;
  }
  const std::string& name() const override { return name_; }
  uint64_t sectors() const override { return sectors_; }
  aios::Result<void> read(uint64_t lba, uint32_t count, uint8_t* out) override {
    while (count) { uint32_t n = count * 512 > CHUNK ? CHUNK / 512 : count; auto r = io(BLK_IN, lba, n, nullptr); if (!r.ok()) return r; std::memcpy(out, bounce_, n * 512); out += n * 512; lba += n; count -= n; }
    return aios::Ok();
  }
  aios::Result<void> write(uint64_t lba, uint32_t count, const uint8_t* data) override {
    while (count) { uint32_t n = count * 512 > CHUNK ? CHUNK / 512 : count; std::memcpy(bounce_, data, n * 512); auto r = io(BLK_OUT, lba, n, nullptr); if (!r.ok()) return r; data += n * 512; lba += n; count -= n; }
    return aios::Ok();
  }
  aios::Result<void> flush() override { if (!has_flush_) return aios::Ok(); return io(BLK_FLUSH, 0, 0, nullptr); }
 private:
  aios::Result<void> io(uint32_t type, uint64_t lba, uint32_t count, void*) {
    if (type != BLK_FLUSH && lba + count > sectors_) return aios::Result<void>(aios::Status::Invalid, "past end of device");
    hdr_->type = type; hdr_->reserved = 0; hdr_->sector = lba; *status_ = 0xFF;
    desc_[0] = {(uint64_t)(uintptr_t)hdr_, 16, DESC_NEXT, 1};
    int n = 1;
    if (type != BLK_FLUSH) { desc_[1] = {(uint64_t)(uintptr_t)bounce_, count * 512, (uint16_t)(DESC_NEXT | (type == BLK_IN ? DESC_WRITE : 0)), 2}; n = 2; }
    desc_[n] = {(uint64_t)(uintptr_t)status_, 1, DESC_WRITE, 0};
    avail_ring_[*avail_idx_ % qsize_] = 0;
    asm volatile("" ::: "memory");
    (*avail_idx_)++;
    asm volatile("mfence" ::: "memory");
    outw(io_ + 0x10, 0);
    uint16_t want = last_used_ + 1;
    for (uint64_t spins = 0; *used_idx_ != want; ++spins) { asm volatile("pause"); if (spins > 400000000ull) return aios::Result<void>(aios::Status::IoError, "virtio-blk request timed out"); }
    last_used_ = want;
    asm volatile("mfence" ::: "memory");
    if (*status_ != 0) return aios::Result<void>(aios::Status::IoError, *status_ == 2 ? "virtio-blk: unsupported request" : "virtio-blk: I/O error");
    return aios::Ok();
  }
  uint16_t io_ = 0, qsize_ = 0, last_used_ = 0;
  bool has_flush_ = false;
  VringDesc* desc_ = nullptr; uint16_t* avail_idx_ = nullptr; uint16_t* avail_ring_ = nullptr; uint16_t* used_idx_ = nullptr; VringUsedElem* used_ring_ = nullptr;
  BlkReq* hdr_ = nullptr; uint8_t* status_ = nullptr; uint8_t* bounce_ = nullptr;
  uint64_t sectors_ = 0;
  std::string name_;
};
}  // namespace

// The first virtio-blk disk on the PCI bus, or null.
std::unique_ptr<aios::hal::BlockDevice> metal_probe_virtio_blk() {
  for (uint32_t bus = 0; bus < 4; ++bus) for (uint32_t dev = 0; dev < 32; ++dev) for (uint32_t fn = 0; fn < 8; ++fn) {
    uint32_t id = pci_read(bus, dev, fn, 0);
    if ((id & 0xFFFF) != 0x1af4) { if (fn == 0 && id == 0xFFFFFFFF) break; continue; }
    uint32_t device = id >> 16;
    if (device != 0x1001) continue;                       // transitional virtio-blk (legacy I/O BAR)
    uint32_t bar0 = pci_read(bus, dev, fn, 0x10);
    if (!(bar0 & 1)) continue;
    auto d = std::make_unique<VirtioBlk>();
    if (d->init((uint16_t)(bar0 & ~3u), bus, dev, fn)) return d;
  }
  return nullptr;
}
