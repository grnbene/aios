#pragma once
// A MemoryObject is the unit of possession. An entity holds MemoryIDs; the
// address is the kernel's business and is exposed only to devices.
#include <cstdint>
#include "aios/core/ids.hpp"
#include "aios/hal/host.hpp"

namespace aios {

enum class MemoryType : uint8_t { System = 0, Device = 1, Shared = 2, Persistent = 3, Remote = 4 };
const char* memory_type_name(MemoryType t);
bool parse_memory_type(const std::string& s, MemoryType& out);

// Rights an entity may hold over a memory object (subset of capability rights).
enum MemPerm : uint8_t { MemRead = 1, MemWrite = 2, MemShare = 4, MemTransfer = 8 };

struct MemoryLocation {
  MemoryType type = MemoryType::System;
  DeviceID device;         // null for host memory
  uint32_t numa_node = 0;
};

struct MemoryObject {
  MemoryID id;
  uint64_t size = 0;
  MemoryType type = MemoryType::System;
  MemoryLocation location;
  uint8_t permissions = MemRead | MemWrite;
  EntityID owner;
  uint64_t generation = 0;   // bumped on every write / migration
  uint64_t created_ns = 0;
  // Kernel-private: never leaks across the kernel API.
  hal::Mapping mapping;
  void* base() const { return mapping.base; }
};

}  // namespace aios
