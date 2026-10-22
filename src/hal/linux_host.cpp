#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
// The Linux host. The only translation unit in the kernel that includes
// Linux/POSIX headers other than the standard library.
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <malloc.h>
#include <signal.h>
#include "aios/hal/host.hpp"
// AIOS_HAL_HOST_ONLY: the host layer on its own (the standalone grain build), without
// the telemetry it feeds aios's world model. aios itself never defines it.
#if !defined(AIOS_HAL_HOST_ONLY)
#include "aios/intelligence/world_model.hpp"
#endif
#include <cstdio>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <unistd.h>
#include "aios/hal/sync.hpp"
#include <dirent.h>
#include <algorithm>
#include <dlfcn.h>
#include <time.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>

namespace aios::hal {

namespace {

// glibc only grew the memfd_create wrapper in 2.27, but el7 (glibc 2.17) still
// has the syscall itself, backported into its 3.10 kernel. Call it directly
// there so one binary covers both.
#if defined(__GLIBC__) && (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 27))
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
int aios_memfd_create(const char* name, unsigned int flags) {
  return (int)::syscall(SYS_memfd_create, name, flags);
}
#else
int aios_memfd_create(const char* name, unsigned int flags) {
  return ::memfd_create(name, flags);
}
#endif

class LinuxHost final : public Host {
 public:
  LinuxHost() { discover(); }

  Result<Mapping> map(uint64_t size, MappingKind kind, const std::string& path) override {
    if (size == 0) return Result<Mapping>(Status::Invalid, "zero-size mapping");
    Mapping m;
    m.size = size;
    m.kind = kind;
    switch (kind) {
      case MappingKind::Private: {
        void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return Result<Mapping>(Status::Exhausted, std::string("mmap: ") + strerror(errno));
        m.base = p;
        return m;
      }
      case MappingKind::Shared: {
        int fd = aios_memfd_create("aios-shared", MFD_CLOEXEC);
        if (fd < 0) return Result<Mapping>(Status::IoError, std::string("memfd_create: ") + strerror(errno));
        if (::ftruncate(fd, (off_t)size) != 0) { ::close(fd); return Result<Mapping>(Status::Exhausted, strerror(errno)); }
        void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) { ::close(fd); return Result<Mapping>(Status::Exhausted, std::string("mmap shared: ") + strerror(errno)); }
        m.base = p;
        m.handle = fd;
        return m;
      }
      case MappingKind::Persistent: {
        if (path.empty()) return Result<Mapping>(Status::Invalid, "persistent mapping needs a path");
        int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) return Result<Mapping>(Status::IoError, path + ": " + strerror(errno));
        struct stat st{};
        if (::fstat(fd, &st) == 0 && (uint64_t)st.st_size < size) {
          if (::ftruncate(fd, (off_t)size) != 0) { ::close(fd); return Result<Mapping>(Status::IoError, strerror(errno)); }
        }
        void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) { ::close(fd); return Result<Mapping>(Status::Exhausted, std::string("mmap file: ") + strerror(errno)); }
        m.base = p;
        m.handle = fd;
        m.path = path;
        return m;
      }
    }
    return Result<Mapping>(Status::Invalid);
  }

  Result<std::shared_ptr<const FileView>> view_file(const std::string& path) override {
    struct Mapped final : FileView {
      ~Mapped() override { if (data && size) ::munmap(const_cast<uint8_t*>(data), size); }
    };
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return Result<std::shared_ptr<const FileView>>(errno == ENOENT ? Status::NotFound : Status::IoError, path + ": " + strerror(errno));
    struct stat st{};
    if (::fstat(fd, &st) != 0) { const int e = errno; ::close(fd); return Result<std::shared_ptr<const FileView>>(Status::IoError, path + ": " + strerror(e)); }
    auto v = std::make_shared<Mapped>();
    if (st.st_size > 0) {
      void* p = ::mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
      if (p == MAP_FAILED) { const int e = errno; ::close(fd); return Result<std::shared_ptr<const FileView>>(Status::IoError, path + ": mmap: " + strerror(e)); }
      v->data = static_cast<const uint8_t*>(p);
      v->size = (size_t)st.st_size;
    }
    ::close(fd);   // the mapping keeps the file
    return std::shared_ptr<const FileView>(std::move(v));
  }

  void unmap(Mapping& m) override {
    if (m.base) ::munmap(m.base, m.size);
    if (m.handle >= 0) ::close(m.handle);
    m.base = nullptr;
    m.handle = -1;
  }

  Result<void> sync(const Mapping& m) override {
    if (m.kind != MappingKind::Persistent || !m.base) return Ok();
    if (::msync(m.base, m.size, MS_SYNC) != 0) return Result<void>(Status::IoError, strerror(errno));
    return Ok();
  }

  Result<void> set_access(const std::string& path, FileAccess access) override {
    // a directory needs its x bit to be entered: private is 0700 there, as ~/.ssh is
    struct stat st{};
    const bool dir = ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    mode_t m = access == FileAccess::Private ? (dir ? 0700 : 0600) : (access == FileAccess::Executable || dir ? 0755 : 0644);
    if (::chmod(path.c_str(), m) != 0) return Result<void>(Status::IoError, path + ": " + strerror(errno));
    return Ok();
  }

  Result<std::string> read_symlink(const std::string& path) override {
    char buf[4096];
    ssize_t n = ::readlink(path.c_str(), buf, sizeof(buf));
    if (n < 0) return Result<std::string>(Status::IoError, path + ": " + strerror(errno));
    return std::string(buf, (size_t)n);
  }

  Result<void> link_file(const std::string& src, const std::string& dst) override {
    drop_append_fd(dst);
    if (::link(src.c_str(), dst.c_str()) == 0) return Ok();
    if (errno == EEXIST) return Result<void>(Status::IoError, dst + ": " + strerror(errno));
    return Host::link_file(src, dst);   // another filesystem, or none that links: copy
  }

  Result<void> make_symlink(const std::string& target, const std::string& at) override {
    ::unlink(at.c_str());
    if (::symlink(target.c_str(), at.c_str()) != 0) return Result<void>(Status::IoError, at + ": " + strerror(errno));
    return Ok();
  }

  // Open append descriptors, one per path, kept between appends: the SQL
  // redo log took an open, a write and a close per statement, and its sync
  // another open and close - four system calls around one 100-byte write.
  // Whatever replaces or removes the file (write_file's rename over it,
  // remove, rename_file, link_file, create_exclusive) drops the descriptor
  // first, so the next append opens the file that is there then. A holder
  // keeps the descriptor alive until its own write or sync is done.
  struct AppendFd {
    int fd = -1;
    bool positional = false;   // opened without O_APPEND, for write_at_unsynced (pwrite on an O_APPEND descriptor appends)
    bool direct = false;       // opened O_DIRECT, for write_at_direct
    ~AppendFd() { if (fd >= 0) ::close(fd); }
  };
  std::mutex append_mu_;
  std::unordered_map<std::string, std::shared_ptr<AppendFd>> append_fds_;
  std::shared_ptr<AppendFd> append_fd(const std::string& path, bool create, bool positional = false, bool direct = false) {
    std::lock_guard<std::mutex> lk(append_mu_);
    auto it = append_fds_.find(path);
    if (it != append_fds_.end() && ((it->second->positional == positional && it->second->direct == direct) || !create))
      return it->second;
    if (!create) return nullptr;
    if (it != append_fds_.end()) append_fds_.erase(it);   // the other mode: reopened the way this write needs
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | (positional ? 0 : O_APPEND) | (direct ? O_DIRECT : 0), 0644);
    if (fd < 0) return nullptr;
    if (append_fds_.size() >= 8) append_fds_.clear();
    auto h = std::make_shared<AppendFd>();
    h->fd = fd;
    h->positional = positional;
    h->direct = direct;
    append_fds_[path] = h;
    return h;
  }
  void drop_append_fd(const std::string& path) {
    std::lock_guard<std::mutex> lk(append_mu_);
    append_fds_.erase(path);
  }
  static Result<void> write_all(int fd, ByteSpan data) {
    size_t off = 0;
    while (off < data.size()) {
      ssize_t n = ::write(fd, data.data() + off, data.size() - off);
      if (n < 0) { if (errno == EINTR) continue; return Result<void>(Status::IoError, strerror(errno)); }
      off += (size_t)n;
    }
    return Ok();
  }

  Result<void> write_file(const std::string& path, ByteSpan data) override { return write_file_impl(path, data, true); }
  Result<void> write_file_unsynced(const std::string& path, ByteSpan data) override { return write_file_impl(path, data, false); }
  // One flush of the filesystem the tree is on, for what write_file_unsynced left in the page cache.
  void sync_tree(const std::string& path) override {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) { ::sync(); return; }
    if (::syncfs(fd) != 0) ::sync();
    ::close(fd);
  }

 Result<void> write_file_impl(const std::string& path, ByteSpan data, bool durable) {
    drop_append_fd(path);
    // A temp name of this write's own: two threads writing the same path (the same git object
    // fetched twice at once) must not truncate each other's file before its rename.
    static std::atomic<uint64_t> seq{0};
    std::string tmp = path + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(seq.fetch_add(1));
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return Result<void>(Status::IoError, tmp + ": " + strerror(errno));
    // A big durable file (a checkpoint's 40 MB table) goes to the medium a
    // few MB at a time, each chunk written back before the next: one fsync
    // of the whole file queued 40 MB ahead of the redo log's fdatasync on
    // the same disk, and a COMMIT waited 100-250 ms behind it.
    // 1 MB at a time, and before each chunk any commit flushing meanwhile
    // goes first (waited out, up to a few ms): a commit then queues behind at
    // most one short chunk, not behind 4 MB transfers back to back.
    constexpr size_t kPace = 1u << 20;
    const bool paced = durable && data.size() > 2 * kPace;
    size_t off = 0;
    while (off < data.size()) {
      if (paced)
        for (int w = 0; w < 40 && aios::hal::foreground_io().load(std::memory_order_acquire) > 0; ++w) ::usleep(100);
      const size_t want = paced ? std::min(kPace, data.size() - off) : data.size() - off;
      ssize_t n = ::write(fd, data.data() + off, want);
      if (n < 0) { const int e = errno; if (e == EINTR) continue; ::close(fd); ::unlink(tmp.c_str()); return Result<void>(Status::IoError, strerror(e)); }
      off += (size_t)n;
      if (paced) ::sync_file_range(fd, (off64_t)(off - (size_t)n), (off64_t)n, SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
    }
    if (durable) ::fsync(fd);
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) { const int e = errno; ::unlink(tmp.c_str()); return Result<void>(Status::IoError, strerror(e)); }
    return Ok();
  }

  Result<void> write_file_plain(const std::string& path, ByteSpan data) override {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return Result<void>(Status::IoError, path + ": " + strerror(errno));
    size_t off = 0;
    while (off < data.size()) {
      ssize_t n = ::write(fd, data.data() + off, data.size() - off);
      if (n < 0) { const int e = errno; if (e == EINTR) continue; ::close(fd); return Result<void>(Status::IoError, path + ": " + strerror(e)); }
      off += (size_t)n;
    }
    ::close(fd);
    return Ok();
  }

  // O_EXCL makes the existence check and the create one step, so of two takers exactly one gets the file. The data
  // goes into the file itself, not a temp renamed into place: a rename would replace a lock someone else just took.
  Result<void> create_exclusive(const std::string& path, ByteSpan data) override { return create_exclusive_impl(path, data, true); }
  Result<void> create_exclusive_unsynced(const std::string& path, ByteSpan data) override { return create_exclusive_impl(path, data, false); }
  Result<void> create_exclusive_impl(const std::string& path, ByteSpan data, bool durable) {
    drop_append_fd(path);
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
      const int e = errno;
      return Result<void>(e == EEXIST ? Status::Conflict : Status::IoError, path + ": " + strerror(e));
    }
    size_t off = 0;
    while (off < data.size()) {
      ssize_t n = ::write(fd, data.data() + off, data.size() - off);
      if (n < 0) {
        if (errno == EINTR) continue;
        const int e = errno;
        ::close(fd);
        ::unlink(path.c_str());   // ours, half written: leaving it would hold the lock for nobody
        return Result<void>(Status::IoError, path + ": " + strerror(e));
      }
      off += (size_t)n;
    }
    if (durable) ::fsync(fd);
    ::close(fd);
    return Ok();
  }

  Result<void> append_file(const std::string& path, ByteSpan data) override {
    auto h = append_fd(path, true);
    if (!h) return Result<void>(Status::IoError, path + ": " + strerror(errno));
    auto w = write_all(h->fd, data);
    if (!w.ok()) return w;
    ::fdatasync(h->fd);
    return Ok();
  }

  Result<void> append_file_unsynced(const std::string& path, ByteSpan data) override {
    auto h = append_fd(path, true);
    if (!h) return Result<void>(Status::IoError, path + ": " + strerror(errno));
    return write_all(h->fd, data);
  }

  Result<void> write_at_unsynced(const std::string& path, uint64_t offset, ByteSpan data) override {
    auto h = append_fd(path, true, true);
    if (!h) return Result<void>(Status::IoError, path + ": " + strerror(errno));
    size_t off = 0;
    while (off < data.size()) {
      ssize_t n = ::pwrite(h->fd, data.data() + off, data.size() - off, (off_t)(offset + off));
      if (n < 0) { if (errno == EINTR) continue; return Result<void>(Status::IoError, path + ": " + strerror(errno)); }
      off += (size_t)n;
    }
    return Ok();
  }

  // Space set aside for a log: the blocks are allocated and the size set
  // now (fallocate, unwritten extents), so writes within it change no
  // metadata and read as zeros where nothing was written yet.
  Result<void> preallocate(const std::string& path, uint64_t bytes) override {
    auto h = append_fd(path, true, true);
    if (!h) return Result<void>(Status::IoError, path + ": " + strerror(errno));
    if (::fallocate(h->fd, 0, 0, (off_t)bytes) != 0 && ::posix_fallocate(h->fd, 0, (off_t)bytes) != 0)
      return Result<void>(Status::IoError, path + ": " + strerror(errno));
    return Ok();
  }

  Result<void> write_at_direct(const std::string& path, uint64_t offset, ByteSpan data) override {
    if ((offset | data.size() | reinterpret_cast<uintptr_t>(data.data())) & 4095)
      return Result<void>(Status::Invalid, path + ": a direct write must be 4096-aligned");
    auto h = append_fd(path, true, true, true);
    if (!h) return Result<void>(errno == EINVAL ? Status::Unsupported : Status::IoError, path + ": " + strerror(errno));
    size_t off = 0;
    while (off < data.size()) {
      ssize_t n = ::pwrite(h->fd, data.data() + off, data.size() - off, (off_t)(offset + off));
      if (n < 0) {
        const int e = errno;
        if (e == EINTR) continue;
        if (e == EINVAL) { drop_append_fd(path); return Result<void>(Status::Unsupported, path + ": " + strerror(e)); }
        return Result<void>(Status::IoError, path + ": " + strerror(e));
      }
      off += (size_t)n;
    }
    return Ok();
  }

  Result<void> write_zeros(const std::string& path, uint64_t bytes) override {
    drop_append_fd(path);
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return Result<void>(Status::IoError, path + ": " + strerror(errno));
    constexpr size_t kChunk = 1u << 20;
    static const std::vector<uint8_t> zeros(kChunk, 0);
    for (uint64_t off = 0; off < bytes;) {
      for (int w = 0; w < 40 && aios::hal::foreground_io().load(std::memory_order_acquire) > 0; ++w) ::usleep(100);
      const size_t want = (size_t)std::min<uint64_t>(kChunk, bytes - off);
      ssize_t n = ::pwrite(fd, zeros.data(), want, (off_t)off);
      if (n < 0) { const int e = errno; if (e == EINTR) continue; ::close(fd); return Result<void>(Status::IoError, path + ": " + strerror(e)); }
      ::sync_file_range(fd, (off64_t)off, (off64_t)n, SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
      off += (uint64_t)n;
    }
    const int rc = ::fsync(fd);
    const int e = errno;
    ::close(fd);
    if (rc != 0) return Result<void>(Status::IoError, path + ": " + strerror(e));
    return Ok();
  }

  Result<void> sync_file(const std::string& path) override {
    auto h = append_fd(path, false);
    int fd = h ? h->fd : ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return Result<void>(Status::IoError, path + ": " + strerror(errno));
    const int rc = ::fdatasync(fd);
    const int e = errno;
    if (!h) ::close(fd);
    if (rc != 0) return Result<void>(Status::IoError, path + ": " + strerror(e));
    return Ok();
  }

  Result<ByteBuffer> read_range(const std::string& path, uint64_t offset, uint64_t len) override {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return Result<ByteBuffer>(errno == ENOENT ? Status::NotFound : Status::IoError, path + ": " + strerror(errno));
    ByteBuffer out;
    out.resize((size_t)len);
    size_t got = 0;
    while (got < len) {
      ssize_t n = ::pread(fd, out.data() + got, (size_t)(len - got), (off_t)(offset + got));
      if (n < 0) { ::close(fd); return Result<ByteBuffer>(Status::IoError, strerror(errno)); }
      if (n == 0) break;   // short read: the file ends here
      got += (size_t)n;
    }
    ::close(fd);
    out.resize(got);
    return out;
  }

  Result<ByteBuffer> read_file(const std::string& path) override {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return Result<ByteBuffer>(errno == ENOENT ? Status::NotFound : Status::IoError, path + ": " + strerror(errno));
    ByteBuffer out;
    uint8_t buf[1 << 16];
    for (;;) {
      ssize_t n = ::read(fd, buf, sizeof buf);
      if (n < 0) { ::close(fd); return Result<ByteBuffer>(Status::IoError, strerror(errno)); }
      if (n == 0) break;
      out.insert(out.end(), buf, buf + n);
    }
    ::close(fd);
    return out;
  }

  Result<std::vector<DirEntry>> list_dir_typed(const std::string& path) override {
    DIR* d = ::opendir(path.c_str());
    if (!d) return Result<std::vector<DirEntry>>(errno == ENOENT ? Status::NotFound : Status::IoError, path + ": " + strerror(errno));
    std::vector<DirEntry> out;
    while (dirent* e = ::readdir(d)) {
      if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
      char kind = '?';
#ifdef DT_DIR
      if (e->d_type == DT_DIR) kind = 'd';
      else if (e->d_type == DT_LNK) kind = 'l';
      else if (e->d_type != DT_UNKNOWN) kind = 'f';
#endif
      out.push_back(DirEntry{e->d_name, kind});
    }
    ::closedir(d);
    return out;
  }

  Result<std::vector<std::string>> list_dir(const std::string& path) override {
    DIR* d = ::opendir(path.c_str());
    if (!d) return Result<std::vector<std::string>>(errno == ENOENT ? Status::NotFound : Status::IoError, path + ": " + strerror(errno));
    std::vector<std::string> out;
    while (dirent* e = ::readdir(d)) {
      std::string n = e->d_name;
      if (n != "." && n != "..") out.push_back(n);
    }
    ::closedir(d);
    return out;
  }

  Result<void> make_dir(const std::string& path) override {
    { struct stat st{}; if (::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return Ok(); }   // there already: no walk
    std::string cur;
    std::stringstream ss(path);
    std::string part;
    if (!path.empty() && path[0] == '/') cur = "/";
    while (std::getline(ss, part, '/')) {
      if (part.empty()) continue;
      cur += part + "/";
      if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return Result<void>(Status::IoError, cur + ": " + strerror(errno));
    }
    return Ok();
  }

  bool exists(const std::string& path) override { struct stat st{}; return ::stat(path.c_str(), &st) == 0; }
  // lstat, not stat: a link is described as a link, and its size is the length
  // of the name it holds — which is exactly what a repository records for it.
  Result<FileInfo> stat(const std::string& path) override {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) return Result<FileInfo>(Status::NotFound, path);
    FileInfo fi{S_ISDIR(st.st_mode), (uint64_t)st.st_size, S_ISLNK(st.st_mode), (st.st_mode & S_IXUSR) != 0};
    fi.inode = (uint64_t)st.st_ino; fi.mtime_ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec; fi.disk_bytes = (uint64_t)st.st_blocks * 512;
    fi.mode = (uint32_t)st.st_mode; fi.uid = (uint32_t)st.st_uid; fi.gid = (uint32_t)st.st_gid;
    return fi;
  }
  Result<Mapping> map_named(const std::string& name, uint64_t size, bool create) override {
    const std::string n = (!name.empty() && name[0] == '/') ? name : "/" + name;
    if (create) ::shm_unlink(n.c_str());
    int fd = ::shm_open(n.c_str(), create ? (O_CREAT | O_RDWR) : O_RDWR, 0660);
    if (fd < 0) return Result<Mapping>(Status::IoError, n + ": " + strerror(errno));
    if (create && ::ftruncate(fd, (off_t)size) != 0) { ::close(fd); return Result<Mapping>(Status::IoError, n + ": " + strerror(errno)); }
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { ::close(fd); return Result<Mapping>(Status::IoError, n + ": " + strerror(errno)); }
    Mapping m; m.base = p; m.size = size; m.kind = MappingKind::Shared; m.handle = fd; m.path = n;
    return m;
  }
  void unlink_named(const std::string& name) override {
    const std::string n = (!name.empty() && name[0] == '/') ? name : "/" + name;
    ::shm_unlink(n.c_str());
  }
  std::string env(const char* name) override { const char* v = ::getenv(name); return v ? std::string(v) : std::string(); }
  bool process_alive(uint64_t pid) override { return !(::kill((pid_t)pid, 0) != 0 && errno == ESRCH); }
  void log_error(const std::string& line) override { std::string l = line + "\n"; (void)!::write(2, l.data(), l.size()); }
  void release_free_memory() override {
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
  }
  Result<void> remove(const std::string& path) override {
    drop_append_fd(path);
    if (::remove(path.c_str()) != 0) return Result<void>(Status::IoError, path + ": " + strerror(errno));
    return Ok();
  }
  // Freed 4 MB at a time from the end, a commit's flush let through before
  // each step: unlinking 100 MB of table files at once on a disk mounted with
  // discard stalled every commit behind the trim for 10-15 ms.
  Result<void> remove_gently(const std::string& path) override {
    drop_append_fd(path);
    int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
      struct stat st{};
      if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        constexpr off_t kStep = (off_t)4 << 20;
        for (off_t len = st.st_size; len > kStep;) {
          for (int w = 0; w < 40 && aios::hal::foreground_io().load(std::memory_order_acquire) > 0; ++w) ::usleep(100);
          len -= kStep;
          if (::ftruncate(fd, len) != 0) break;
        }
      }
      ::close(fd);
    }
    if (::remove(path.c_str()) != 0) return Result<void>(Status::IoError, path + ": " + strerror(errno));
    return Ok();
  }

  Result<void> rename_file(const std::string& src, const std::string& dst) override {
    drop_append_fd(src);
    drop_append_fd(dst);
    if (::rename(src.c_str(), dst.c_str()) != 0) return Result<void>(Status::IoError, src + ": " + strerror(errno));
    return Ok();
  }

  uint64_t monotonic_ns() override { timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts); return uint64_t(ts.tv_sec) * 1000000000ull + ts.tv_nsec; }
  uint64_t wall_ns() override { timespec ts{}; clock_gettime(CLOCK_REALTIME, &ts); return uint64_t(ts.tv_sec) * 1000000000ull + ts.tv_nsec; }

  void random(MutableByteSpan out) override {
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    size_t off = 0;
    if (fd >= 0) {
      while (off < out.size()) {
        ssize_t n = ::read(fd, out.data() + off, out.size() - off);
        if (n <= 0) break;
        off += (size_t)n;
      }
      ::close(fd);
    }
    // Fallback mixes the clock if the device was unavailable.
    for (; off < out.size(); ++off) out[off] = uint8_t(monotonic_ns() >> (8 * (off % 8)));
  }

  const Topology& topology() override { return topo_; }

  uint64_t available_bytes() override {
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo) {
      std::string line;
      uint64_t mem_avail_kb = 0;
      uint64_t mem_free_kb = 0;
      uint64_t cached_kb = 0;
      uint64_t buffers_kb = 0;
      uint64_t sreclaimable_kb = 0;
      while (std::getline(meminfo, line)) {
        if (line.rfind("MemAvailable:", 0) == 0) {
          std::istringstream is(line.substr(13));
          is >> mem_avail_kb;
        } else if (line.rfind("MemFree:", 0) == 0) {
          std::istringstream is(line.substr(8));
          is >> mem_free_kb;
        } else if (line.rfind("Cached:", 0) == 0) {
          std::istringstream is(line.substr(7));
          is >> cached_kb;
        } else if (line.rfind("Buffers:", 0) == 0) {
          std::istringstream is(line.substr(8));
          is >> buffers_kb;
        } else if (line.rfind("SReclaimable:", 0) == 0) {
          std::istringstream is(line.substr(13));
          is >> sreclaimable_kb;
        }
      }
      if (mem_avail_kb > 0) return mem_avail_kb * 1024ull;
      if (mem_free_kb > 0 || cached_kb > 0) return (mem_free_kb + cached_kb + buffers_kb + sreclaimable_kb) * 1024ull;
    }
    struct sysinfo si{};
    if (::sysinfo(&si) == 0) {
      return uint64_t(si.freeram + si.bufferram) * si.mem_unit;
    }
    return topo_.available_memory;
  }

  // Linux answers for a mounted filesystem through statvfs; /proc/mounts walks
  // the ones that exist. The volume holding the path the caller named is put
  // first and marked, because that is the one persistence depends on.
  Result<std::vector<StorageVolume>> storage(const std::string& of_interest) override {
    std::vector<StorageVolume> out;
    std::ifstream mounts("/proc/mounts");
    if (!mounts) return Result<std::vector<StorageVolume>>(Status::IoError, "cannot read /proc/mounts");
    std::string line;
    while (std::getline(mounts, line)) {
      std::istringstream is(line);
      std::string source, point, type;
      if (!(is >> source >> point >> type)) continue;
      // Kernel bookkeeping filesystems describe no medium; they would only add noise.
      static const char* synthetic[] = {"proc", "sysfs", "devtmpfs", "devpts", "cgroup", "cgroup2",
                                        "securityfs", "pstore", "bpf", "tracefs", "debugfs", "configfs",
                                        "fusectl", "mqueue", "hugetlbfs", "autofs", "binfmt_misc", "nsfs", "squashfs"};
      bool skip = false;
      for (const char* s : synthetic) if (type == s) { skip = true; break; }
      if (skip) continue;
      struct statvfs vfs{};
      if (::statvfs(point.c_str(), &vfs) != 0 || vfs.f_blocks == 0) continue;
      StorageVolume v;
      v.name = source;
      v.path = point;
      v.kind = "filesystem";
      v.block_size = (uint32_t)vfs.f_frsize;
      v.capacity = (uint64_t)vfs.f_blocks * vfs.f_frsize;
      v.used = (uint64_t)(vfs.f_blocks - vfs.f_bavail) * vfs.f_frsize;
      out.push_back(v);
    }
    if (!of_interest.empty()) {
      size_t best = out.size(), best_len = 0;
      for (size_t i = 0; i < out.size(); ++i) {
        if (of_interest.rfind(out[i].path, 0) == 0 && out[i].path.size() >= best_len) { best = i; best_len = out[i].path.size(); }
      }
      if (best < out.size()) {
        out[best].holds_root = true;
        StorageVolume held = out[best];
        out.erase(out.begin() + (long)best);
        out.insert(out.begin(), held);
      }
    }
    return out;
  }

  // The machine's own buses, out of sysfs. The bare-metal HAL answers this by
  // scanning PCI configuration space itself; here Linux owns the buses, so the
  // honest answer is what Linux says is on them — including which driver has
  // each function, which is the part a person actually wants to know.
  Result<std::vector<HardwareDevice>> hardware() override {
    std::vector<HardwareDevice> out;
    const std::string root = "/sys/bus/pci/devices";
    DIR* d = ::opendir(root.c_str());
    if (!d) return out;                       // no PCI bus exposed: not an error
    auto first_line = [](const std::string& path) {
      std::ifstream in(path);
      std::string v;
      std::getline(in, v);
      return v;
    };
    while (dirent* e = ::readdir(d)) {
      std::string slot = e->d_name;
      if (slot.empty() || slot[0] == '.') continue;
      std::string dir = root + "/" + slot;
      std::string cls = first_line(dir + "/class");
      unsigned long code = std::strtoul(cls.c_str(), nullptr, 16);
      unsigned base = static_cast<unsigned>((code >> 16) & 0xFF);
      unsigned sub = static_cast<unsigned>((code >> 8) & 0xFF);
      HardwareDevice hw;
      // sysfs names the slot with its domain; the bus address is the part that
      // matches what a bare-metal scan would print.
      hw.slot = slot.size() > 5 ? slot.substr(5) : slot;
      switch (base) {
        case 0x01: hw.kind = "storage"; break;
        case 0x02: hw.kind = "ethernet"; break;
        case 0x03: hw.kind = "display"; break;
        case 0x04: hw.kind = "multimedia"; break;
        case 0x06: hw.kind = (sub == 0x00) ? "host bridge" : "bridge"; break;
        case 0x08: hw.kind = "system"; break;
        case 0x0C: hw.kind = (sub == 0x03) ? "usb" : "serial bus"; break;
        case 0x0D: hw.kind = "wireless"; break;
        default: hw.kind = "class " + cls.substr(0, 6); break;
      }
      std::string vendor = first_line(dir + "/vendor");
      std::string device = first_line(dir + "/device");
      auto trim0x = [](const std::string& v) { return v.rfind("0x", 0) == 0 ? v.substr(2) : v; };
      hw.name = trim0x(vendor) + " " + trim0x(device);
      char link[512]{};
      ssize_t n = ::readlink((dir + "/driver").c_str(), link, sizeof link - 1);
      if (n > 0) {
        std::string target(link, static_cast<size_t>(n));
        size_t at = target.rfind('/');
        hw.driver = (at == std::string::npos) ? target : target.substr(at + 1);
      }
      out.push_back(hw);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end(),
              [](const HardwareDevice& a, const HardwareDevice& b) { return a.slot < b.slot; });
    return out;
  }

  Result<std::vector<NetworkInterface>> network() override {
    std::vector<NetworkInterface> out;
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return Result<std::vector<NetworkInterface>>(Status::IoError, strerror(errno));
    // One entry per interface: the addresses arrive as separate records, so an
    // interface already seen is filled in rather than repeated.
    for (ifaddrs* a = list; a; a = a->ifa_next) {
      if (!a->ifa_name) continue;
      NetworkInterface* iface = nullptr;
      for (auto& e : out) if (e.name == a->ifa_name) { iface = &e; break; }
      if (!iface) { out.push_back(NetworkInterface{}); iface = &out.back(); iface->name = a->ifa_name; }
      iface->up = (a->ifa_flags & IFF_UP) != 0 && (a->ifa_flags & IFF_RUNNING) != 0;
      iface->loopback = (a->ifa_flags & IFF_LOOPBACK) != 0;
      if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET && iface->address.empty()) {
        char buf[INET_ADDRSTRLEN]{};
        auto* in4 = reinterpret_cast<sockaddr_in*>(a->ifa_addr);
        if (::inet_ntop(AF_INET, &in4->sin_addr, buf, sizeof buf)) iface->address = buf;
      }
    }
    ::freeifaddrs(list);
    // The hardware address, link state and speed live in sysfs, not in the
    // address list; a virtual interface simply has none of them.
    for (auto& iface : out) {
      auto first_line = [&](const std::string& file) {
        std::ifstream in("/sys/class/net/" + iface.name + "/" + file);
        std::string v;
        std::getline(in, v);
        return v;
      };
      iface.hardware = first_line("address");
      std::string mtu = first_line("mtu");
      if (!mtu.empty()) iface.mtu = (uint32_t)std::strtoul(mtu.c_str(), nullptr, 10);
      std::string speed = first_line("speed");
      long mbit = speed.empty() ? -1 : std::strtol(speed.c_str(), nullptr, 10);
      if (mbit > 0) iface.speed_bps = (uint64_t)mbit * 1000000ull;
    }
    return out;
  }

 private:
  void discover() {
    long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    topo_.logical_cpus = n > 0 ? (uint32_t)n : 1;
    struct sysinfo si{};
    if (::sysinfo(&si) == 0) {
      topo_.total_memory = uint64_t(si.totalram) * si.mem_unit;
      topo_.available_memory = uint64_t(si.freeram) * si.mem_unit;
    }
    utsname u{};
    if (::uname(&u) == 0) { topo_.arch = u.machine; topo_.host_name = u.nodename; }
    // NUMA from sysfs when present; a single node otherwise.
    auto nodes = list_dir("/sys/devices/system/node");
    if (nodes.ok()) {
      for (auto& name : *nodes) {
        if (name.rfind("node", 0) != 0) continue;
        NumaNode node{};
        node.id = (uint32_t)std::stoul(name.substr(4));
        std::ifstream mem("/sys/devices/system/node/" + name + "/meminfo");
        std::string line;
        while (std::getline(mem, line)) {
          auto p = line.find("MemTotal:");
          if (p != std::string::npos) { std::istringstream is(line.substr(p + 9)); uint64_t kb = 0; is >> kb; node.memory_bytes = kb * 1024; }
        }
        std::ifstream cl("/sys/devices/system/node/" + name + "/cpulist");
        std::string cpus;
        std::getline(cl, cpus);
        std::stringstream cs(cpus);
        std::string range;
        while (std::getline(cs, range, ',')) {
          auto dash = range.find('-');
          if (dash == std::string::npos) { if (!range.empty()) node.cpus.push_back((uint32_t)std::stoul(range)); }
          else { uint32_t a = (uint32_t)std::stoul(range.substr(0, dash)), b = (uint32_t)std::stoul(range.substr(dash + 1)); for (uint32_t c = a; c <= b; ++c) node.cpus.push_back(c); }
        }
        topo_.numa.push_back(node);
      }
    }
    if (topo_.numa.empty()) {
      NumaNode node{0, topo_.total_memory, {}};
      for (uint32_t c = 0; c < topo_.logical_cpus; ++c) node.cpus.push_back(c);
      topo_.numa.push_back(node);
    }
  }
  Topology topo_;
};

}  // namespace

void compute_yield() {}   // a hosted job has a thread of its own; nothing to keep alive
std::unique_ptr<Host> make_host() { return std::make_unique<LinuxHost>(); }
// One line to this substrate's console.
void host_note(const std::string& text) { std::fputs(text.c_str(), stderr); std::fflush(stderr); }

void* load_library(const std::string& name) { return dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL); }
void* library_symbol(void* handle, const std::string& symbol) { return handle ? dlsym(handle, symbol.c_str()) : nullptr; }
void unload_library(void* handle) { if (handle) dlclose(handle); }
std::string library_error() { const char* e = dlerror(); return e ? e : ""; }

namespace {
// The hosted build's instruments. Same contract as the bare-metal provider in
// metal/main.cpp, read from /proc instead of this kernel's own counters, so a
// developer shell reports measurements rather than a sine wave. There is no
// actuation sink here on purpose: a hosted process does not own the scheduler,
// and claiming otherwise is the exact failure this interface exists to prevent.
namespace {
std::atomic<int> g_settle_pressure{1};
// The most runnable work per processor seen since the last telemetry read.
//
// An instantaneous count cannot work here: the settler joins its threads before
// the second telemetry read is taken, so the machine is idle again by the time
// anything looks at it, and dim 7 reported 0.09 while thirty-two threads had
// just been fighting over four processors. Every other instrument in this
// provider is a DELTA over the interval; this is the same idea for a quantity
// that has no cumulative counter — a peak, sampled while the work is happening,
// reported once and then cleared.
std::atomic<unsigned long long> g_peak_runnable{0};

unsigned long long read_procs_running() {
  if (FILE* f = std::fopen("/proc/stat", "r")) {
    char line[256];
    unsigned long long running = 0;
    while (std::fgets(line, sizeof line, f)) {
      if (std::sscanf(line, "procs_running %llu", &running) == 1) break;
    }
    std::fclose(f);
    return running;
  }
  return 0;
}

#if !defined(AIOS_HAL_HOST_ONLY)
intelligence::SubstrateTelemetry linux_telemetry() {
  intelligence::SubstrateTelemetry t;
  static unsigned long long last_idle = 0, last_total = 0;

  if (FILE* f = std::fopen("/proc/stat", "r")) {
    unsigned long long u = 0, n = 0, sy = 0, id = 0, io = 0, irq = 0, sirq = 0;
    if (std::fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &sy, &id, &io, &irq, &sirq) == 7) {
      unsigned long long total = u + n + sy + id + io + irq + sirq;
      unsigned long long d_total = total - last_total, d_idle = id - last_idle;
      if (d_total) t.cpu_load = 1.0f - (float)((double)d_idle / (double)d_total);
      last_total = total; last_idle = id;
    }
    std::fclose(f);
  }

  // Context switches per second, normalised against 20k/s as a busy machine.
  static unsigned long long last_ctxt = 0;
  static struct timespec last_ts = {0, 0};
  if (FILE* f = std::fopen("/proc/stat", "r")) {
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
      unsigned long long v = 0;
      if (std::sscanf(line, "ctxt %llu", &v) == 1) {
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        double dt = (double)(now.tv_sec - last_ts.tv_sec) + (double)(now.tv_nsec - last_ts.tv_nsec) / 1e9;
        if (last_ctxt && dt > 0.0) {
          float rate = (float)(((double)(v - last_ctxt) / dt) / 20000.0);
          t.ctx_switch_rate = rate > 1.0f ? 1.0f : rate;
        }
        last_ctxt = v; last_ts = now;
        break;
      }
    }
    std::fclose(f);
  }

  struct sysinfo si{};
  if (sysinfo(&si) == 0 && si.totalram)
    t.mem_pressure = 1.0f - (float)((double)si.freeram / (double)si.totalram);

  // rx bytes are a flow, not a ring depth; report the delta against 100 MB/s so
  // dim 1 still means "how busy is the NIC" on this substrate.
  static unsigned long long last_rx = 0;
  if (FILE* f = std::fopen("/proc/net/dev", "r")) {
    char line[512]; unsigned long long rx_total = 0;
    while (std::fgets(line, sizeof line, f)) {
      char iface[64]; unsigned long long rx = 0;
      if (std::sscanf(line, " %63[^:]: %llu", iface, &rx) == 2 && std::strncmp(iface, "lo", 2) != 0)
        rx_total += rx;
    }
    std::fclose(f);
    if (last_rx && rx_total >= last_rx) {
      float d = (float)((double)(rx_total - last_rx) / (100.0 * 1024.0 * 1024.0));
      t.nic_ring_depth = d > 1.0f ? 1.0f : d;
    }
    last_rx = rx_total;
  }

  // Packet loss, at BOTH layers this machine can lose at, from the kernel's own
  // counters:
  //
  //   /proc/net/dev   errs + drop + fifo — the interface dropped it
  //   /proc/net/snmp  Udp InErrors       — it reached the socket and the socket
  //                                        had no room, which is receive-buffer
  //                                        overflow and is where load actually
  //                                        turns into loss on a busy machine
  //
  // The first version of this read /proc/net/dev alone, which cannot see the
  // second — so the condition generator produced real, kernel-counted loss that
  // the instrument was structurally blind to, and the reward term stayed zero
  // while the machine was dropping a hundred thousand datagrams.
  {
    static unsigned long long last_pkts = 0, last_bad = 0;
    unsigned long long pkts = 0, bad = 0;

    if (FILE* f = std::fopen("/proc/net/dev", "r")) {
      char line[512];
      while (std::fgets(line, sizeof line, f)) {
        char iface[64];
        unsigned long long rb, rp, re, rd, rf, rfr, rc, rm;
        if (std::sscanf(line, " %63[^:]: %llu %llu %llu %llu %llu %llu %llu %llu",
                        iface, &rb, &rp, &re, &rd, &rf, &rfr, &rc, &rm) == 9) {
          if (std::strncmp(iface, "lo", 2) == 0) continue;
          pkts += rp;
          bad += re + rd + rf;
        }
      }
      std::fclose(f);
    }

    if (FILE* f = std::fopen("/proc/net/snmp", "r")) {
      char line[1024];
      bool header = false;
      while (std::fgets(line, sizeof line, f)) {
        if (std::strncmp(line, "Udp:", 4) != 0) continue;
        if (!header) { header = true; continue; }   // the first Udp: line names the columns
        unsigned long long in_dg = 0, no_ports = 0, in_err = 0;
        if (std::sscanf(line, "Udp: %llu %llu %llu", &in_dg, &no_ports, &in_err) == 3) {
          pkts += in_dg;
          // InErrors already includes RcvbufErrors on this kernel, so counting
          // both would count the same lost datagram twice.
          bad += in_err;
        }
        break;
      }
      std::fclose(f);
    }

    if (last_pkts && (pkts > last_pkts || bad > last_bad)) {
      const double dp = (double)(pkts >= last_pkts ? pkts - last_pkts : 0);
      const double db = (double)(bad >= last_bad ? bad - last_bad : 0);
      if (dp + db > 0.0) {
        const float loss = (float)(db / (dp + db));
        t.packet_loss = loss > 1.0f ? 1.0f : loss;
      }
    }
    last_pkts = pkts;
    last_bad = bad;
  }

  // Load imbalance: runnable processes right now over the processors that could
  // be running them.
  //
  // This read /proc/loadavg's one-minute figure, which cannot work for what it
  // is used for: the settler runs for 20 ms and a one-minute exponential
  // average does not move in 20 ms, so the reward's load term was a constant
  // and no policy could ever learn from it. /proc/stat's procs_running is the
  // instantaneous count and it responds inside a single settle.
  {
    const unsigned long long now_running = read_procs_running();
    // The peak the settler saw while the work was actually running, or the
    // instant reading when nothing has been sampled since the last read.
    unsigned long long peak = g_peak_runnable.exchange(0);
    if (now_running > peak) peak = now_running;
    const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    const double per = cpus > 0 ? (double)peak / (double)cpus : (double)peak;
    // 4x oversubscribed is the top of the scale, so the saturation knee the
    // reward uses at 0.25 is exactly one runnable process per processor.
    t.load_imbalance = (float)(per > 4.0 ? 1.0 : per / 4.0);
  }

  t.compute_occupancy = 0.0f;   // no GPU is queried; saying so beats inventing one
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  t.timestamp_ms = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
  return t;
}
#endif  // !AIOS_HAL_HOST_ONLY

// Hosted, this process does not own the scheduler, so there is no actuation
// sink — but the recorder still needs real work to have happened between the
// two telemetry reads, or every /proc delta is 0. This burns a bounded slice of
// CPU across threads so the kernel's own context-switch and jiffy counters
// actually move. It measures a correlation, not a causal lever, and `jepa
// status` already says actuation is NOT BOUND here.
// The conditions this machine learns from, generated rather than waited for.
//
// The settler used to run one fixed burn: four threads on four processors for
// 20 ms, every time. That makes every recorded transition look alike, so the
// policy sees one thin slice of the state space and the two instruments added
// beside it — packet loss and runnable work per processor — never move. A
// policy cannot learn to correct a condition the machine never experiences, and
// no quantity of recorded transitions fixes that: it is a generation problem,
// not a collection problem.
//
// So the settler now runs at a PRESSURE, swept by the recorder, and each level
// is a genuinely different machine to measure:
//
//   0  idle-ish   one thread, a short burn: nothing is contended
//   1  matched    one thread per processor, as before
//   2  contended  three threads per processor, all runnable at once
//   3  thrashing  eight per processor, plus loopback traffic through a socket
//                 buffer small enough that the kernel drops from it
//
// The loopback traffic is real traffic through the real stack with real drops
// counted by the real kernel — the same /proc/net/dev columns dim 6 reads. It
// runs on 127.0.0.1 and touches no interface this machine did not create.
}

extern "C" void linux_set_settle_pressure(int level) {
  g_settle_pressure.store(level < 0 ? 0 : (level > 3 ? 3 : level));
}
extern "C" int linux_settle_pressure() { return g_settle_pressure.load(); }

#if !defined(AIOS_HAL_HOST_ONLY)
// Traffic with drops in it: a datagram socket with a receive buffer far smaller
// than what is about to be sent into it, so the kernel discards the overflow and
// counts what it discarded. Nothing here is simulated — the loss dim 6 reports
// is loss this machine actually took.
void linux_flood_loopback(int ms, int burst) {
  int rx = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
  if (rx < 0) return;
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  // A deliberately tiny receive buffer: this is the pressure.
  int small = 2048;
  ::setsockopt(rx, SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
  if (::bind(rx, (struct sockaddr*)&addr, sizeof addr) < 0) { ::close(rx); return; }
  socklen_t alen = sizeof addr;
  if (::getsockname(rx, (struct sockaddr*)&addr, &alen) < 0) { ::close(rx); return; }

  int tx = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (tx < 0) { ::close(rx); return; }
  int big = 1 << 20;
  ::setsockopt(tx, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);

  static unsigned char payload[1400];
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    for (int i = 0; i < burst; ++i) {
      (void)::sendto(tx, payload, sizeof payload, MSG_DONTWAIT, (struct sockaddr*)&addr, sizeof addr);
    }
    // Never drained on purpose: the overflow is the point, and the kernel's own
    // counters are what report it.
  }
  ::close(tx);
  ::close(rx);
}

void linux_settle() {
  const long cpus_l = sysconf(_SC_NPROCESSORS_ONLN);
  const int cpus = cpus_l > 0 ? (int)cpus_l : 4;
  const int pressure = g_settle_pressure.load();
  int threads = cpus;
  int ms = 20;
  switch (pressure) {
    case 0: threads = 1; ms = 10; break;
    case 1: threads = cpus; ms = 20; break;
    case 2: threads = cpus * 3; ms = 30; break;
    default: threads = cpus * 8; ms = 40; break;
  }

  std::vector<std::thread> workers;
  std::atomic<uint64_t> sink{0};
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  // One sampler, so what this machine was LIKE under the work is recorded while
  // the work is happening rather than after it has all gone home.
  workers.emplace_back([&] {
    while (std::chrono::steady_clock::now() < deadline) {
      const unsigned long long r = read_procs_running();
      unsigned long long seen = g_peak_runnable.load();
      while (r > seen && !g_peak_runnable.compare_exchange_weak(seen, r)) {}
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });
  for (int i = 0; i < threads; ++i) {
    workers.emplace_back([&] {
      uint64_t acc = 0;
      while (std::chrono::steady_clock::now() < deadline) {
        for (int j = 0; j < 20000; ++j) acc += (uint64_t)j * 2654435761ull;
        std::this_thread::yield();   // provoke the scheduler, which is what dim 2 counts
      }
      sink.fetch_add(acc, std::memory_order_relaxed);
    });
  }
  // At the top of the range, traffic as well as load, so dim 6 has something in
  // it and the two conditions are seen together rather than one at a time.
  if (pressure >= 3) {
    workers.emplace_back([&] { linux_flood_loopback(ms, 64); });
  } else if (pressure == 2) {
    workers.emplace_back([&] { linux_flood_loopback(ms / 2, 16); });
  }
  for (auto& w : workers) w.join();
}

struct RegisterHostedTelemetry {
  RegisterHostedTelemetry() {
    intelligence::set_telemetry_provider(&linux_telemetry);
    intelligence::set_substrate_pressure_hook(&linux_set_settle_pressure);
    intelligence::set_substrate_settle(&linux_settle);
  }
} g_register_hosted_telemetry;
#endif  // !AIOS_HAL_HOST_ONLY
}  // namespace

}  // namespace aios::hal
