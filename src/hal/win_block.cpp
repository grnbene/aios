// A block device over a host file, on Windows. Same contract as
// linux_block.cpp: sectors in, sectors out, backed by CreateFile/ReadFile/
// WriteFile instead of pread/pwrite.
#include "aios/hal/block.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstring>

namespace aios::hal {
namespace {

std::wstring to_wide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? (size_t)n : 0, L'\0');
  if (n > 0) { ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n); w.resize((size_t)n - 1); }
  return w;
}
std::string win_error() {
  const DWORD err = ::GetLastError();
  char buf[256];
  const DWORD n = ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0, buf, sizeof buf, nullptr);
  std::string msg = n ? std::string(buf, n) : ("error " + std::to_string(err));
  while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
  return msg;
}

class FileBlockDevice final : public BlockDevice {
 public:
  FileBlockDevice(HANDLE f, uint64_t sectors, std::string name) : f_(f), sectors_(sectors), name_(std::move(name)) {}
  ~FileBlockDevice() override { ::CloseHandle(f_); }
  const std::string& name() const override { return name_; }
  uint64_t sectors() const override { return sectors_; }
  Result<void> read(uint64_t lba, uint32_t count, uint8_t* out) override {
    if (lba + count > sectors_) return Result<void>(Status::Invalid, "read past end of device");
    OVERLAPPED ov{}; const uint64_t off = lba * SECTOR;
    ov.Offset = (DWORD)(off & 0xFFFFFFFFu); ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    const DWORD want = count * SECTOR;
    if (!::ReadFile(f_, out, want, &got, &ov) && ::GetLastError() != ERROR_HANDLE_EOF) return Result<void>(Status::IoError, win_error());
    if (got < want) std::memset(out + got, 0, want - got);
    return Ok();
  }
  Result<void> write(uint64_t lba, uint32_t count, const uint8_t* data) override {
    if (lba + count > sectors_) return Result<void>(Status::Invalid, "write past end of device");
    OVERLAPPED ov{}; const uint64_t off = lba * SECTOR;
    ov.Offset = (DWORD)(off & 0xFFFFFFFFu); ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD put = 0;
    const DWORD want = count * SECTOR;
    if (!::WriteFile(f_, data, want, &put, &ov) || put != want) return Result<void>(Status::IoError, win_error());
    return Ok();
  }
  Result<void> flush() override { if (!::FlushFileBuffers(f_)) return Result<void>(Status::IoError, win_error()); return Ok(); }
 private:
  HANDLE f_; uint64_t sectors_; std::string name_;
};

}  // namespace

Result<std::unique_ptr<BlockDevice>> make_file_block_device(const std::string& path, uint64_t create_bytes) {
  // OVERLAPPED's Offset/OffsetHigh give pread/pwrite-style positioned I/O even
  // on a synchronous handle (no FILE_FLAG_OVERLAPPED, no event, no
  // GetOverlappedResult): ReadFile/WriteFile still block until done, just at
  // the position named instead of the handle's current one.
  HANDLE f = ::CreateFileW(to_wide(path).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
      create_bytes ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return Result<std::unique_ptr<BlockDevice>>(Status::IoError, path + ": " + win_error());
  LARGE_INTEGER size{};
  ::GetFileSizeEx(f, &size);
  uint64_t bytes = (uint64_t)size.QuadPart;
  if (bytes == 0 && create_bytes) {
    LARGE_INTEGER want{}; want.QuadPart = (LONGLONG)create_bytes;
    if (!::SetFilePointerEx(f, want, nullptr, FILE_BEGIN) || !::SetEndOfFile(f)) { const auto e = win_error(); ::CloseHandle(f); return Result<std::unique_ptr<BlockDevice>>(Status::IoError, e); }
    bytes = create_bytes;
  }
  return std::unique_ptr<BlockDevice>(new FileBlockDevice(f, bytes / SECTOR, path));
}

}  // namespace aios::hal
