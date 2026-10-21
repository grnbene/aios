#pragma once
// The AOS volume: AOS's own persistent store on a block device. Log-structured
// — every write is an appended, checksummed record; mount replays the active
// log; when a log half fills, the live set is rewritten into the other half
// and the superblock flips. It presents the file API the kernel's HAL needs
// (write-file-atomically, append, read, list, mkdir, exists) with no
// filesystem borrowed from any host.
#include <map>
#include <set>
#include <string>
#include <vector>
#include "aios/hal/block.hpp"
#include "aios/core/bytes.hpp"

namespace aios::hal {

struct VolumeInfo {
  std::string label;
  uint64_t sectors = 0, half_sectors = 0, generation = 0, records = 0, files = 0, bytes_live = 0, log_used_sectors = 0;
  uint32_t active_half = 0;
  uint32_t compactions = 0;
};

class Volume {
 public:
  static bool probe(BlockDevice& dev, std::string* label = nullptr);
  static Result<void> format(BlockDevice& dev, const std::string& label);
  Result<void> mount(BlockDevice& dev);
  bool mounted() const { return dev_ != nullptr; }
  const VolumeInfo& info() const { return info_; }

  Result<void> write_file(const std::string& path, ByteSpan data);
  Result<void> append_file(const std::string& path, ByteSpan data);
  Result<ByteBuffer> read_file(const std::string& path) const;
  Result<std::vector<std::string>> list_dir(const std::string& path) const;
  Result<void> make_dir(const std::string& path);
  Result<void> remove(const std::string& path);
  bool exists(const std::string& path) const;
  bool is_dir(const std::string& path) const;
  Result<uint64_t> size(const std::string& path) const;

 private:
  enum RecType : uint8_t { Set = 1, Append = 2, Dir = 3, Del = 4 };
  Result<void> append_record(RecType t, const std::string& path, ByteSpan data);
  Result<void> compact();
  Result<void> write_superblock();
  uint64_t half_start(uint32_t half) const { return 1 + (uint64_t)half * info_.half_sectors; }
  BlockDevice* dev_ = nullptr;
  VolumeInfo info_;
  uint64_t write_sector_ = 0;    // next free sector within the active half (absolute LBA)
  std::map<std::string, ByteBuffer> files_;
  std::set<std::string> dirs_;
};

}  // namespace aios::hal
