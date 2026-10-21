#pragma once
// Block storage is a host facility: sectors in, sectors out. The AOS volume
// (hal/volume.hpp) lives on top of this and owes nothing to any filesystem.
#include <cstdint>
#include <memory>
#include <string>
#include "aios/core/result.hpp"

namespace aios::hal {
class BlockDevice {
 public:
  virtual ~BlockDevice() = default;
  virtual const std::string& name() const = 0;
  virtual uint64_t sectors() const = 0;             // 512-byte sectors
  virtual Result<void> read(uint64_t lba, uint32_t count, uint8_t* out) = 0;
  virtual Result<void> write(uint64_t lba, uint32_t count, const uint8_t* data) = 0;
  virtual Result<void> flush() = 0;
};
constexpr uint32_t SECTOR = 512;
// A block device over a host file (Linux; also for driving the volume code).
Result<std::unique_ptr<BlockDevice>> make_file_block_device(const std::string& path, uint64_t create_bytes = 0);
}  // namespace aios::hal
