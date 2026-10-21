#pragma once
/* virtio-mmio transport and split virtqueues, shared by every virtio driver.
 *
 * QEMU virt exposes virtio over MMIO rather than PCI, and defaults to the
 * LEGACY (version 1) register interface while newer setups present version 2.
 * They differ in how a queue's address reaches the device — one page frame
 * number versus three split addresses — so both live here once instead of
 * being half-remembered in each driver.
 */
#include <cstddef>
#include <cstdint>

struct VringDesc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
};

struct VringAvail {
  uint16_t flags;
  uint16_t idx;
  uint16_t ring[];
};

struct VringUsedElem {
  uint32_t id;
  uint32_t len;
};

struct VringUsed {
  uint16_t flags;
  uint16_t idx;
  VringUsedElem ring[];
};

struct VirtQueue {
  uint16_t size = 0;
  VringDesc* desc = nullptr;
  VringAvail* avail = nullptr;
  VringUsed* used = nullptr;
  uint16_t avail_idx = 0;
  uint16_t used_seen = 0;
};

struct VirtioDevice {
  bool present = false;
  uint64_t base = 0;
  uint32_t version = 0;
  uint32_t device_id = 0;
  uint32_t intid = 0;
  uint64_t features = 0;
  VirtQueue q[2];
};

/* One buffer in a descriptor chain. `device_writes` is the WRITE flag: it says
 * the DEVICE fills this buffer in, which is the opposite of what the name
 * suggests the first time you read the spec. */
struct VirtioBuf {
  uint64_t addr;
  uint32_t len;
  bool device_writes;
};

/* Find the first virtio-mmio transport in the device tree carrying this device
 * id, reset it, and negotiate. Leaves the device in DRIVER state, before
 * DRIVER_OK, so queues can still be configured. */
bool virtio_open(uint32_t device_id, VirtioDevice& out);

/* Device-specific config space, which starts at offset 0x100. */
uint32_t virtio_config32(const VirtioDevice& d, uint32_t offset);

bool virtio_queue_init(VirtioDevice& d, unsigned qidx, uint16_t want_size);

/* DRIVER_OK: the device may start using the queues. */
void virtio_start(VirtioDevice& d);

/* Submit one descriptor chain and poll it to completion. Returns the used
 * length, or -1 if the device did not answer. One request is outstanding at a
 * time, which is why the chain can always start at descriptor 0. */
int64_t virtio_request(VirtioDevice& d, unsigned qidx, const VirtioBuf* bufs, unsigned n);
