// NVM Express over PCIe, on the bare-metal substrate.
//
// The driver this replaces (src/hal/nvme.cpp) was a std::vector<uint8_t> and
// two memcpys. It had no PCIe configuration access, no BAR, no doorbells, no
// queues and no controller — and it reported a gigabyte of capacity from a
// member initialiser on a machine with no NVMe device in it at all.
//
// This is the real thing: the PCI bus is scanned for a class-010802 function,
// its BAR0 is taken as the controller's register block, the controller is
// reset and re-enabled with the admin queue's physical addresses in ASQ and
// ACQ, Identify is issued to learn the namespace's real capacity and block
// format, an I/O queue pair is created with Create-CQ and Create-SQ, and reads
// and writes go out as NVM commands with PRP entries, rung through the
// doorbell registers and waited on by the completion queue's phase bit.
//
// Polled, one command in flight. There is no interrupt handler here yet and no
// pretence of one.
#include "aios/hal/block.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

extern "C" void* metal_alloc_pages(size_t bytes);
extern "C" void metal_console_write(const char* s, size_t n);
extern "C" uint64_t metal_monotonic_ms();

namespace {

inline void outl(uint16_t p, uint32_t v) { asm volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
inline uint32_t inl(uint16_t p) { uint32_t v; asm volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

uint32_t pci_read(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off) {
  outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
  return inl(0xCFC);
}
void pci_write(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off, uint32_t v) {
  outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
  outl(0xCFC, v);
}

// The controller's registers, at the offsets NVMe defines within BAR0.
constexpr uint32_t REG_CAP = 0x00;      // capabilities, 64-bit
constexpr uint32_t REG_CC = 0x14;       // configuration
constexpr uint32_t REG_CSTS = 0x1C;     // status
constexpr uint32_t REG_AQA = 0x24;      // admin queue attributes
constexpr uint32_t REG_ASQ = 0x28;      // admin submission queue base
constexpr uint32_t REG_ACQ = 0x30;      // admin completion queue base

constexpr uint32_t CC_EN = 1u << 0;
constexpr uint32_t CSTS_RDY = 1u << 0;
constexpr uint32_t CSTS_CFS = 1u << 1;   // controller fatal status

constexpr uint8_t ADMIN_CREATE_SQ = 0x01;
constexpr uint8_t ADMIN_CREATE_CQ = 0x05;
constexpr uint8_t ADMIN_IDENTIFY  = 0x06;
constexpr uint8_t NVM_FLUSH = 0x00;
constexpr uint8_t NVM_WRITE = 0x01;
constexpr uint8_t NVM_READ  = 0x02;

constexpr uint32_t QUEUE_DEPTH = 32;
constexpr uint32_t PAGE = 4096;

// A submission entry is 64 bytes and a completion entry 16, laid out exactly
// as the specification numbers their dwords.
struct __attribute__((packed)) SubmissionEntry {
  uint8_t opcode;
  uint8_t flags;
  uint16_t command_id;
  uint32_t nsid;
  uint64_t reserved;
  uint64_t metadata;
  uint64_t prp1;
  uint64_t prp2;
  uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};
static_assert(sizeof(SubmissionEntry) == 64, "an NVMe submission entry is 64 bytes");

struct __attribute__((packed)) CompletionEntry {
  uint32_t result;
  uint32_t reserved;
  uint16_t sq_head;
  uint16_t sq_id;
  uint16_t command_id;
  uint16_t status;      // the phase bit is bit 0; the status code sits above it
};
static_assert(sizeof(CompletionEntry) == 16, "an NVMe completion entry is 16 bytes");

void say(const char* s) { metal_console_write(s, __builtin_strlen(s)); }

// Memory-mapped register access. Volatile because the controller changes these
// underneath us and the compiler must not cache them.
inline uint32_t mmio32(uint64_t base, uint32_t off) {
  return *reinterpret_cast<volatile uint32_t*>(base + off);
}
inline void mmio32_set(uint64_t base, uint32_t off, uint32_t v) {
  *reinterpret_cast<volatile uint32_t*>(base + off) = v;
}
inline uint64_t mmio64(uint64_t base, uint32_t off) {
  return (uint64_t)mmio32(base, off) | ((uint64_t)mmio32(base, off + 4) << 32);
}
inline void mmio64_set(uint64_t base, uint32_t off, uint64_t v) {
  mmio32_set(base, off, (uint32_t)v);
  mmio32_set(base, off + 4, (uint32_t)(v >> 32));
}

struct Queue {
  SubmissionEntry* sq = nullptr;
  CompletionEntry* cq = nullptr;
  uint64_t sq_phys = 0, cq_phys = 0;
  uint32_t sq_tail = 0, cq_head = 0;
  uint8_t phase = 1;          // flips each time the completion queue wraps
  uint32_t id = 0;
  uint32_t depth = QUEUE_DEPTH;
};

class Nvme final : public aios::hal::BlockDevice {
 public:
  bool init(uint32_t bus, uint32_t dev, uint32_t fn) {
    // BAR0 is the register block: a memory BAR whose low bits are type flags,
    // not address, and which is 64-bit when bits 2:1 read 10b.
    uint32_t bar0 = pci_read(bus, dev, fn, 0x10);
    uint32_t bar1 = pci_read(bus, dev, fn, 0x14);
    if (bar0 & 0x1) return false;                      // an I/O BAR is not a controller
    base_ = (uint64_t)(bar0 & 0xFFFFFFF0u);
    if (((bar0 >> 1) & 0x3) == 0x2) base_ |= ((uint64_t)bar1 << 32);
    if (!base_) return false;

    // Memory space and bus mastering, or the controller can neither be
    // addressed nor fetch the queues we are about to hand it.
    pci_write(bus, dev, fn, 0x04, pci_read(bus, dev, fn, 0x04) | 0x6);

    uint64_t cap = mmio64(base_, REG_CAP);
    doorbell_stride_ = 4u << ((uint32_t)((cap >> 32) & 0xF));
    uint32_t max_entries = (uint32_t)(cap & 0xFFFF) + 1;
    uint64_t timeout_ms = ((cap >> 24) & 0xFF) * 500ull;
    if (timeout_ms == 0) timeout_ms = 20000;
    if (max_entries < QUEUE_DEPTH) return false;

    // Reset: clear CC.EN and wait for CSTS.RDY to fall.
    mmio32_set(base_, REG_CC, mmio32(base_, REG_CC) & ~CC_EN);
    if (!wait_ready(false, timeout_ms)) { say("[nvme] the controller would not reset\n"); return false; }

    if (!alloc_queue(admin_, 0)) return false;
    mmio32_set(base_, REG_AQA, ((QUEUE_DEPTH - 1) << 16) | (QUEUE_DEPTH - 1));
    mmio64_set(base_, REG_ASQ, admin_.sq_phys);
    mmio64_set(base_, REG_ACQ, admin_.cq_phys);

    // CC: NVM command set, 4 KiB pages, round-robin arbitration, 2^6-byte
    // submission entries, 2^4-byte completion entries, then enable.
    uint32_t cc = (6u << 16) | (4u << 20) | CC_EN;
    mmio32_set(base_, REG_CC, cc);
    if (!wait_ready(true, timeout_ms)) { say("[nvme] the controller would not come ready\n"); return false; }

    if (!identify_namespace()) { say("[nvme] Identify failed\n"); return false; }
    if (!create_io_queue()) { say("[nvme] the controller refused an I/O queue pair\n"); return false; }

    ready_ = true;
    return true;
  }

  const std::string& name() const override { return name_; }
  uint64_t sectors() const override { return sectors_; }

  aios::Result<void> read(uint64_t lba, uint32_t count, uint8_t* out) override {
    return transfer(lba, count, out, false);
  }
  aios::Result<void> write(uint64_t lba, uint32_t count, const uint8_t* data) override {
    return transfer(lba, count, const_cast<uint8_t*>(data), true);
  }
  aios::Result<void> flush() override {
    if (!ready_) return aios::Result<void>(aios::Status::IoError, "no NVMe controller");
    SubmissionEntry cmd{};
    cmd.opcode = NVM_FLUSH;
    cmd.nsid = 1;
    uint16_t status = 0;
    if (!submit(io_, cmd, status)) {
      return aios::Result<void>(aios::Status::IoError, "the controller did not answer the flush");
    }
    if (status) return aios::Result<void>(aios::Status::IoError, "flush failed: " + status_text(status));
    return aios::Ok();
  }

 private:
  bool wait_ready(bool want, uint64_t timeout_ms) {
    uint64_t deadline = metal_monotonic_ms() + timeout_ms;
    for (;;) {
      uint32_t csts = mmio32(base_, REG_CSTS);
      if (csts & CSTS_CFS) return false;
      if (((csts & CSTS_RDY) != 0) == want) return true;
      if (metal_monotonic_ms() > deadline) return false;
    }
  }

  bool alloc_queue(Queue& q, uint32_t id) {
    // The controller reads these by physical address; this kernel is identity
    // mapped, so the pointer IS the physical address.
    void* sq = metal_alloc_pages(QUEUE_DEPTH * sizeof(SubmissionEntry));
    void* cq = metal_alloc_pages(QUEUE_DEPTH * sizeof(CompletionEntry));
    if (!sq || !cq) return false;
    std::memset(sq, 0, QUEUE_DEPTH * sizeof(SubmissionEntry));
    std::memset(cq, 0, QUEUE_DEPTH * sizeof(CompletionEntry));
    q.sq = reinterpret_cast<SubmissionEntry*>(sq);
    q.cq = reinterpret_cast<CompletionEntry*>(cq);
    q.sq_phys = reinterpret_cast<uint64_t>(sq);
    q.cq_phys = reinterpret_cast<uint64_t>(cq);
    q.sq_tail = 0;
    q.cq_head = 0;
    q.phase = 1;
    q.id = id;
    q.depth = QUEUE_DEPTH;
    return true;
  }

  uint32_t sq_doorbell(uint32_t qid) const { return 0x1000 + (2 * qid) * doorbell_stride_; }
  uint32_t cq_doorbell(uint32_t qid) const { return 0x1000 + (2 * qid + 1) * doorbell_stride_; }

  // One command, rung through and waited for. The completion queue's phase bit
  // flips when the controller writes an entry, which is how a driver with no
  // interrupt handler knows the command is done.
  bool submit(Queue& q, SubmissionEntry cmd, uint16_t& status) {
    cmd.command_id = next_id_++;
    q.sq[q.sq_tail] = cmd;
    q.sq_tail = (q.sq_tail + 1) % q.depth;
    asm volatile("" ::: "memory");
    mmio32_set(base_, sq_doorbell(q.id), q.sq_tail);

    uint64_t deadline = metal_monotonic_ms() + 10000;
    for (;;) {
      volatile CompletionEntry& c = q.cq[q.cq_head];
      if ((c.status & 1) == q.phase) {
        status = (uint16_t)(c.status >> 1);
        q.cq_head = (q.cq_head + 1) % q.depth;
        if (q.cq_head == 0) q.phase ^= 1;
        asm volatile("" ::: "memory");
        mmio32_set(base_, cq_doorbell(q.id), q.cq_head);
        return true;
      }
      if (mmio32(base_, REG_CSTS) & CSTS_CFS) return false;
      if (metal_monotonic_ms() > deadline) return false;
    }
  }

  bool identify_namespace() {
    void* buf = metal_alloc_pages(PAGE);
    if (!buf) return false;
    std::memset(buf, 0, PAGE);

    // Identify Controller (CNS 1), for the model string it reports.
    SubmissionEntry cmd{};
    cmd.opcode = ADMIN_IDENTIFY;
    cmd.prp1 = reinterpret_cast<uint64_t>(buf);
    cmd.cdw10 = 1;
    uint16_t status = 0;
    if (!submit(admin_, cmd, status) || status) return false;
    {
      char trimmed[41] = {0};
      std::memcpy(trimmed, reinterpret_cast<const char*>(buf) + 24, 40);   // MN
      int end = 40;
      while (end > 0 && (trimmed[end - 1] == ' ' || trimmed[end - 1] == 0)) trimmed[--end] = 0;
      name_ = std::string("nvme:") + trimmed;
    }

    // Identify Namespace 1 (CNS 0), for its size and block format.
    std::memset(buf, 0, PAGE);
    SubmissionEntry ns{};
    ns.opcode = ADMIN_IDENTIFY;
    ns.nsid = 1;
    ns.prp1 = reinterpret_cast<uint64_t>(buf);
    ns.cdw10 = 0;
    if (!submit(admin_, ns, status) || status) return false;

    const uint8_t* b = reinterpret_cast<const uint8_t*>(buf);
    uint64_t nsze = 0;
    for (int i = 0; i < 8; ++i) nsze |= (uint64_t)b[i] << (8 * i);   // blocks in the namespace
    uint8_t flbas = b[26] & 0xF;                                      // which LBA format is in use
    const uint8_t* lbaf = b + 128 + flbas * 4;
    uint8_t lbads = lbaf[2];                                          // 2^lbads bytes per block
    block_bytes_ = (lbads >= 9 && lbads <= 16) ? (1u << lbads) : 512u;
    blocks_ = nsze;
    // Everything above works in 512-byte sectors, whatever the medium's format.
    sectors_ = (blocks_ * block_bytes_) / aios::hal::SECTOR;
    return blocks_ > 0;
  }

  bool create_io_queue() {
    if (!alloc_queue(io_, 1)) return false;

    // The completion queue must exist before the submission queue that names
    // it, or Create-SQ comes back "invalid queue identifier".
    SubmissionEntry cq{};
    cq.opcode = ADMIN_CREATE_CQ;
    cq.prp1 = io_.cq_phys;
    cq.cdw10 = ((QUEUE_DEPTH - 1) << 16) | io_.id;
    cq.cdw11 = 0x1;                       // physically contiguous, interrupts off
    uint16_t status = 0;
    if (!submit(admin_, cq, status) || status) return false;

    SubmissionEntry sq{};
    sq.opcode = ADMIN_CREATE_SQ;
    sq.prp1 = io_.sq_phys;
    sq.cdw10 = ((QUEUE_DEPTH - 1) << 16) | io_.id;
    sq.cdw11 = (io_.id << 16) | 0x1;      // completes into that CQ, contiguous
    if (!submit(admin_, sq, status) || status) return false;
    return true;
  }

  // PRP: the first entry addresses the head of the buffer and the second the
  // next page. Staging through one page-pair keeps every transfer to two PRPs
  // and needs no PRP list at all.
  aios::Result<void> transfer(uint64_t lba, uint32_t count, uint8_t* data, bool writing) {
    if (!ready_) return aios::Result<void>(aios::Status::IoError, "no NVMe controller was found on this machine");
    if (!data) return aios::Result<void>(aios::Status::Invalid, "null buffer");
    if (lba + count > sectors_) return aios::Result<void>(aios::Status::Invalid, "LBA out of bounds");
    if (count == 0) return aios::Ok();

    if (!bounce_) {
      bounce_ = reinterpret_cast<uint8_t*>(metal_alloc_pages(2 * PAGE));
      if (!bounce_) return aios::Result<void>(aios::Status::Exhausted, "no memory for the transfer buffer");
    }
    const uint32_t per_block = block_bytes_ / aios::hal::SECTOR;
    if (per_block == 0) {
      return aios::Result<void>(aios::Status::Corrupt, "the namespace reports a block smaller than a sector");
    }
    if (lba % per_block || count % per_block) {
      return aios::Result<void>(aios::Status::Invalid, "a transfer must start and end on a medium block");
    }

    uint64_t done = 0;
    while (done < count) {
      uint32_t chunk = (uint32_t)std::min<uint64_t>(count - done, (2 * PAGE) / aios::hal::SECTOR);
      chunk -= chunk % per_block;
      if (chunk == 0) return aios::Result<void>(aios::Status::Invalid, "the medium's block is larger than one transfer");
      uint32_t bytes = chunk * aios::hal::SECTOR;

      if (writing) std::memcpy(bounce_, data + done * aios::hal::SECTOR, bytes);

      SubmissionEntry cmd{};
      cmd.opcode = writing ? NVM_WRITE : NVM_READ;
      cmd.nsid = 1;
      cmd.prp1 = reinterpret_cast<uint64_t>(bounce_);
      cmd.prp2 = bytes > PAGE ? reinterpret_cast<uint64_t>(bounce_ + PAGE) : 0;
      uint64_t start_block = (lba + done) / per_block;
      cmd.cdw10 = (uint32_t)start_block;
      cmd.cdw11 = (uint32_t)(start_block >> 32);
      cmd.cdw12 = (chunk / per_block) - 1;   // zero-based block count

      uint16_t status = 0;
      if (!submit(io_, cmd, status)) {
        return aios::Result<void>(aios::Status::IoError, "the controller did not answer");
      }
      if (status) {
        return aios::Result<void>(aios::Status::IoError,
            std::string(writing ? "write" : "read") + " failed: " + status_text(status));
      }
      if (!writing) std::memcpy(data + done * aios::hal::SECTOR, bounce_, bytes);
      done += chunk;
    }
    return aios::Ok();
  }

  static std::string status_text(uint16_t status) {
    uint8_t code = (uint8_t)(status & 0xFF);
    uint8_t type = (uint8_t)((status >> 8) & 0x7);
    const char* what = "unknown";
    if (type == 0) {
      switch (code) {
        case 0x00: what = "success"; break;
        case 0x01: what = "invalid command opcode"; break;
        case 0x02: what = "invalid field"; break;
        case 0x0B: what = "invalid namespace"; break;
        case 0x80: what = "LBA out of range"; break;
        default: break;
      }
    } else if (type == 1) {
      what = "a media or data-integrity error";
    } else if (type == 2) {
      what = "a path error";
    }
    return std::string(what) + " (type " + std::to_string(type) + ", code " + std::to_string(code) + ")";
  }

  uint64_t base_ = 0;
  uint32_t doorbell_stride_ = 4;
  Queue admin_, io_;
  uint16_t next_id_ = 1;
  uint64_t blocks_ = 0;
  uint32_t block_bytes_ = 512;
  uint64_t sectors_ = 0;
  uint8_t* bounce_ = nullptr;
  bool ready_ = false;
  std::string name_ = "nvme";
};

}  // namespace

// Scans the bus for an NVM Express controller — PCI class 01, subclass 08,
// programming interface 02 — and brings the first one it finds up. Null when
// this machine has none, which is an answer and not a failure.
extern "C" aios::hal::BlockDevice* metal_nvme_probe() {
  for (uint32_t bus = 0; bus < 256; ++bus) {
    for (uint32_t dev = 0; dev < 32; ++dev) {
      for (uint32_t fn = 0; fn < 8; ++fn) {
        uint32_t id = pci_read(bus, dev, fn, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        uint32_t cls = pci_read(bus, dev, fn, 0x08) >> 8;   // class, subclass, prog-if
        if (cls != 0x010802) continue;
        auto* n = new Nvme();
        if (n->init(bus, dev, fn)) {
          say("[nvme] ");
          say(n->name().c_str());
          say(" up\n");
          return n;
        }
        delete n;
      }
    }
  }
  return nullptr;
}
