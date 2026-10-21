// Block storage on the macOS host: sectors in, sectors out over a host file
// or a raw device. Darwin has no fdatasync, so the flush asks the drive for a
// full barrier and settles for fsync where the file system refuses one.
#include "aios/hal/block.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <cerrno>
#include <cstring>

namespace aios::hal {
namespace {
class FileBlockDevice final : public BlockDevice {
 public:
  FileBlockDevice(int fd, uint64_t sectors, std::string name) : fd_(fd), sectors_(sectors), name_(std::move(name)) {}
  ~FileBlockDevice() override { ::close(fd_); }
  const std::string& name() const override { return name_; }
  uint64_t sectors() const override { return sectors_; }
  Result<void> read(uint64_t lba, uint32_t count, uint8_t* out) override {
    if (lba + count > sectors_) return Result<void>(Status::Invalid, "read past end of device");
    ssize_t n = ::pread(fd_, out, (size_t)count * SECTOR, (off_t)(lba * SECTOR));
    if (n < 0) return Result<void>(Status::IoError, strerror(errno));
    if ((size_t)n < (size_t)count * SECTOR) std::memset(out + n, 0, (size_t)count * SECTOR - (size_t)n);
    return Ok();
  }
  Result<void> write(uint64_t lba, uint32_t count, const uint8_t* data) override {
    if (lba + count > sectors_) return Result<void>(Status::Invalid, "write past end of device");
    if (::pwrite(fd_, data, (size_t)count * SECTOR, (off_t)(lba * SECTOR)) != (ssize_t)((size_t)count * SECTOR)) return Result<void>(Status::IoError, strerror(errno));
    return Ok();
  }
  Result<void> flush() override {
    if (::fcntl(fd_, F_FULLFSYNC, 0) != -1) return Ok();
    if (::fsync(fd_) != 0) return Result<void>(Status::IoError, strerror(errno));
    return Ok();
  }
 private:
  int fd_; uint64_t sectors_; std::string name_;
};
}  // namespace

Result<std::unique_ptr<BlockDevice>> make_file_block_device(const std::string& path, uint64_t create_bytes) {
  int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | (create_bytes ? O_CREAT : 0), 0644);
  if (fd < 0) return Result<std::unique_ptr<BlockDevice>>(Status::IoError, path + ": " + strerror(errno));
  struct stat st{};
  ::fstat(fd, &st);
  uint64_t size = (uint64_t)st.st_size;
  // A raw disk reports no size through stat; Darwin answers with sector counts.
  if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
    uint64_t blocks = 0; uint32_t block_size = 0;
    if (::ioctl(fd, DKIOCGETBLOCKCOUNT, &blocks) == 0 && ::ioctl(fd, DKIOCGETBLOCKSIZE, &block_size) == 0) size = blocks * block_size;
    else { off_t end = ::lseek(fd, 0, SEEK_END); size = end > 0 ? (uint64_t)end : 0; }
  }
  if (size == 0 && create_bytes) { if (::ftruncate(fd, (off_t)create_bytes) != 0) { ::close(fd); return Result<std::unique_ptr<BlockDevice>>(Status::IoError, strerror(errno)); } size = create_bytes; }
  return std::unique_ptr<BlockDevice>(new FileBlockDevice(fd, size / SECTOR, path));
}
}  // namespace aios::hal
