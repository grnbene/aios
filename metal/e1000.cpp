// Intel 8254x (e1000) over memory-mapped registers: raw Ethernet frames in and
// out, polled, no interrupts. This is the first NIC this kernel drives that is
// real silicon and not a paravirtual device — the same hal::FrameDevice the
// fabric and the TCP/IP stack already run on, so nothing above changes. The
// receive and transmit rings are the card's own descriptor rings in our
// memory; the kernel reads frames out of them directly, which is the whole
// point: there is no stack between the wire and the fabric to bypass.
#include "aios/hal/fabric.hpp"
#include <cstring>
#include <memory>

extern "C" void* metal_alloc_pages(size_t bytes);

namespace {
inline void outl(uint16_t p, uint32_t v) { asm volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
inline uint32_t inl(uint16_t p) { uint32_t v; asm volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
uint32_t pci_read(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off) { outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC)); return inl(0xCFC); }
void pci_write(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off, uint32_t v) { outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC)); outl(0xCFC, v); }

// Register offsets, 8254x family datasheet.
constexpr uint32_t CTRL = 0x0000, STATUS = 0x0008, EERD = 0x0014, ICR = 0x00C0, IMC = 0x00D8;
constexpr uint32_t RCTL = 0x0100, TCTL = 0x0400, TIPG = 0x0410;
constexpr uint32_t RDBAL = 0x2800, RDBAH = 0x2804, RDLEN = 0x2808, RDH = 0x2810, RDT = 0x2818;
constexpr uint32_t TDBAL = 0x3800, TDBAH = 0x3804, TDLEN = 0x3808, TDH = 0x3810, TDT = 0x3818;
constexpr uint32_t RAL0 = 0x5400, RAH0 = 0x5404, MTA = 0x5200;
constexpr uint32_t CTRL_SLU = 1u << 6, CTRL_RST = 1u << 26;
constexpr uint32_t RCTL_EN = 1u << 1, RCTL_BAM = 1u << 15, RCTL_SECRC = 1u << 26;   // 2048-byte buffers: BSIZE=0, BSEX=0
constexpr uint32_t TCTL_EN = 1u << 1, TCTL_PSP = 1u << 3, TCTL_CT = 0x10u << 4, TCTL_COLD = 0x40u << 12;
constexpr uint8_t RXD_DD = 1, RXD_EOP = 2, TXD_EOP = 1, TXD_IFCS = 2, TXD_RS = 8, TXD_DD = 1;
constexpr size_t BUF = 2048, RX_N = 32, TX_N = 8;

struct __attribute__((packed)) RxDesc { uint64_t addr; uint16_t len; uint16_t csum; uint8_t status; uint8_t errors; uint16_t special; };
struct __attribute__((packed)) TxDesc { uint64_t addr; uint16_t len; uint8_t cso; uint8_t cmd; uint8_t status; uint8_t css; uint16_t special; };

class E1000 final : public aios::hal::FrameDevice {
 public:
  bool init(uintptr_t mmio, uint32_t bus, uint32_t dev, uint32_t fn, uint16_t device_id) {
    mmio_ = mmio; device_id_ = device_id;
    pci_write(bus, dev, fn, 0x04, pci_read(bus, dev, fn, 0x04) | 0x6);   // memory space + bus master
    wr(IMC, 0xFFFFFFFFu);
    wr(CTRL, rd(CTRL) | CTRL_RST);
    for (int i = 0; i < 100000 && (rd(CTRL) & CTRL_RST); ++i) asm volatile("pause");
    wr(IMC, 0xFFFFFFFFu); (void)rd(ICR);
    wr(CTRL, (rd(CTRL) | CTRL_SLU) & ~(1u << 3));                          // link up, no LRST
    if (!read_mac()) return false;
    for (uint32_t i = 0; i < 128; ++i) wr(MTA + 4 * i, 0);
    // Rings: the card wants 128-byte alignment and a length that is a multiple
    // of 128 bytes; a page of each covers both.
    rx_ = (RxDesc*)metal_alloc_pages(4096); tx_ = (TxDesc*)metal_alloc_pages(4096);
    bufs_ = (uint8_t*)metal_alloc_pages(BUF * (RX_N + TX_N));
    if (!rx_ || !tx_ || !bufs_) return false;
    for (size_t i = 0; i < RX_N; ++i) { rx_[i] = {}; rx_[i].addr = (uint64_t)(uintptr_t)(bufs_ + BUF * i); }
    for (size_t i = 0; i < TX_N; ++i) { tx_[i] = {}; tx_[i].status = TXD_DD; }
    wr(RDBAL, (uint32_t)(uintptr_t)rx_); wr(RDBAH, (uint32_t)((uint64_t)(uintptr_t)rx_ >> 32)); wr(RDLEN, (uint32_t)(RX_N * sizeof(RxDesc)));
    wr(RDH, 0); wr(RDT, RX_N - 1); rx_next_ = 0;
    wr(TDBAL, (uint32_t)(uintptr_t)tx_); wr(TDBAH, (uint32_t)((uint64_t)(uintptr_t)tx_ >> 32)); wr(TDLEN, (uint32_t)(TX_N * sizeof(TxDesc)));
    wr(TDH, 0); wr(TDT, 0); tx_next_ = 0;
    wr(TIPG, 0x0060200Au);
    wr(TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    wr(RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
    name_ = "e1000";
    return true;
  }
  const aios::hal::Mac& mac() const override { return mac_; }
  const std::string& name() const override { return name_; }
  bool send(const uint8_t* frame, size_t len) override {
    if (len > BUF) return false;
    TxDesc& d = tx_[tx_next_];
    for (uint64_t spins = 0; !(d.status & TXD_DD); ++spins) { asm volatile("pause"); if (spins > 200000000ull) return false; }
    uint8_t* b = bufs_ + BUF * (RX_N + tx_next_);
    std::memcpy(b, frame, len);
    d.addr = (uint64_t)(uintptr_t)b; d.len = (uint16_t)len; d.cmd = TXD_EOP | TXD_IFCS | TXD_RS; d.status = 0; d.cso = d.css = 0; d.special = 0;
    asm volatile("mfence" ::: "memory");
    tx_next_ = (tx_next_ + 1) % TX_N;
    wr(TDT, (uint32_t)tx_next_);
    for (uint64_t spins = 0; !(d.status & TXD_DD); ++spins) { asm volatile("pause"); if (spins > 200000000ull) return false; }
    ++frames_out_;
    return true;
  }
  int recv(uint8_t* out, size_t cap) override {
    asm volatile("mfence" ::: "memory");
    RxDesc& d = rx_[rx_next_];
    if (!(d.status & RXD_DD)) return 0;
    int len = d.len; if ((size_t)len > cap) len = (int)cap;
    // A frame split over descriptors (bigger than one buffer) is not something
    // the fabric sends; the piece is handed up and the rest follows as frames
    // of its own rather than being silently stitched.
    if (d.status & RXD_EOP) ++frames_in_;
    std::memcpy(out, bufs_ + BUF * rx_next_, (size_t)len);
    d.status = 0;
    asm volatile("mfence" ::: "memory");
    wr(RDT, (uint32_t)rx_next_);                   // this slot is the card's again
    rx_next_ = (rx_next_ + 1) % RX_N;
    return len;
  }
  uint16_t device_id() const { return device_id_; }
  uint64_t frames_in() const { return frames_in_; }
  uint64_t frames_out() const { return frames_out_; }
  bool link_up() const { return rd(STATUS) & 2; }
 private:
  uint32_t rd(uint32_t off) const { return *(volatile uint32_t*)(mmio_ + off); }
  void wr(uint32_t off, uint32_t v) { *(volatile uint32_t*)(mmio_ + off) = v; }
  // The address the card already filtered on (firmware or QEMU loads RA0), or
  // failing that the EEPROM words.
  bool read_mac() {
    uint32_t lo = rd(RAL0), hi = rd(RAH0);
    if (!(hi & (1u << 31))) {
      for (int w = 0; w < 3; ++w) {
        wr(EERD, 1u | ((uint32_t)w << 8));
        uint32_t v = 0; int i = 0;
        for (; i < 100000; ++i) { v = rd(EERD); if (v & (1u << 4)) break; asm volatile("pause"); }
        if (i == 100000) return false;
        uint16_t word = (uint16_t)(v >> 16);
        if (w < 2) lo |= (uint32_t)word << (16 * w); else hi = word;
      }
      wr(RAL0, lo); wr(RAH0, hi | (1u << 31));
    }
    for (int i = 0; i < 4; ++i) mac_[i] = (uint8_t)(lo >> (8 * i));
    mac_[4] = (uint8_t)hi; mac_[5] = (uint8_t)(hi >> 8);
    return true;
  }
  uintptr_t mmio_ = 0; uint16_t device_id_ = 0;
  RxDesc* rx_ = nullptr; TxDesc* tx_ = nullptr; uint8_t* bufs_ = nullptr;
  size_t rx_next_ = 0, tx_next_ = 0;
  uint64_t frames_in_ = 0, frames_out_ = 0;
  aios::hal::Mac mac_{}; std::string name_;
};
E1000* g_e1000 = nullptr;
// The classic 8254x parts QEMU and real boards expose. e1000e (8257x) has a
// different register layout and is not claimed here.
bool is_e1000(uint16_t id) {
  switch (id) {
    case 0x100E: case 0x100F: case 0x1004: case 0x100C: case 0x1015: case 0x1016: case 0x1017:
    case 0x1026: case 0x1027: case 0x1028: case 0x1075: case 0x1076: case 0x1077: case 0x1078: case 0x1079: case 0x107A: case 0x107B: case 0x1010: case 0x1011: case 0x1012: case 0x1013: case 0x1018: case 0x1019: case 0x101A: case 0x101D: case 0x1107: case 0x1112:
      return true;
    default: return false;
  }
}
}  // namespace

// What the PCI pane says is driving an Intel NIC: only the device this
// kernel actually bound, not every id in the table.
extern "C" bool metal_e1000_drives(uint16_t device_id) { return g_e1000 && g_e1000->device_id() == device_id; }
extern "C" bool metal_e1000_link_up() { return g_e1000 && g_e1000->link_up(); }

std::unique_ptr<aios::hal::FrameDevice> metal_probe_e1000() {
  for (uint32_t bus = 0; bus < 4; ++bus) for (uint32_t dev = 0; dev < 32; ++dev) for (uint32_t fn = 0; fn < 8; ++fn) {
    uint32_t id = pci_read(bus, dev, fn, 0);
    if ((id & 0xFFFF) != 0x8086) { if (fn == 0 && id == 0xFFFFFFFF) break; continue; }
    if (!is_e1000((uint16_t)(id >> 16))) continue;
    uint32_t bar0 = pci_read(bus, dev, fn, 0x10); if (bar0 & 1) continue;   // BAR0 must be memory
    uintptr_t base = bar0 & ~0xFu;
    if (((bar0 >> 1) & 3) == 2) base |= (uintptr_t)pci_read(bus, dev, fn, 0x14) << 32;   // 64-bit BAR
    // This kernel is identity mapped: the physical window IS the pointer.
    auto d = std::make_unique<E1000>();
    if (d->init(base, bus, dev, fn, (uint16_t)(id >> 16))) { g_e1000 = d.get(); return d; }
  }
  return nullptr;
}
