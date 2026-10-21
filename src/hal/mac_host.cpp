// The macOS host. The Darwin counterpart of linux_host.cpp: the only
// translation unit of this build that includes Darwin/POSIX headers other
// than the standard library. Nothing above hal/ knows which one is linked.
#include <malloc/malloc.h>
#include <signal.h>
#include "aios/hal/host.hpp"
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>
#include <time.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <sstream>

namespace aios::hal {

namespace {

// Darwin has no memfd_create. A shared object is a POSIX shared-memory
// segment, sized once and unlinked immediately: the name leaves the namespace
// while the descriptor stays valid, so the region is anonymous yet can still
// be handed to another address space. If the segment cannot be created the
// fallback is an unlinked temporary file, which has the same properties.
int anonymous_shared_fd(uint64_t size, std::string& error) {
  static std::atomic<uint32_t> seq{0};
  // SHM_NAME_MAX is 31 on Darwin, so the name is kept short deliberately.
  char name[32];
  std::snprintf(name, sizeof name, "/aios.%d.%u", (int)::getpid(), seq.fetch_add(1));
  int fd = ::shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd >= 0) {
    // Darwin allows exactly one resize, and only before the first mapping.
    if (::ftruncate(fd, (off_t)size) == 0) {
      ::shm_unlink(name);
      ::fcntl(fd, F_SETFD, FD_CLOEXEC);
      return fd;
    }
    error = std::string("ftruncate shared: ") + strerror(errno);
    ::close(fd);
    ::shm_unlink(name);
  } else {
    error = std::string("shm_open: ") + strerror(errno);
  }
  const char* tmp = ::getenv("TMPDIR");
  std::string tpl = std::string(tmp && *tmp ? tmp : "/tmp") + "/aios-shared-XXXXXX";
  std::vector<char> path(tpl.begin(), tpl.end());
  path.push_back('\0');
  fd = ::mkstemp(path.data());
  if (fd < 0) return -1;
  ::unlink(path.data());
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  if (::ftruncate(fd, (off_t)size) != 0) { error = strerror(errno); ::close(fd); return -1; }
  error.clear();
  return fd;
}

// Durability on Darwin: fsync only schedules the write with the drive, so ask
// for the full barrier first and fall back when the file system refuses it.
void flush_fd(int fd) {
  if (::fcntl(fd, F_FULLFSYNC, 0) == -1) ::fsync(fd);
}

class MacHost final : public Host {
 public:
  MacHost() { discover(); }

  Result<Mapping> map(uint64_t size, MappingKind kind, const std::string& path) override {
    if (size == 0) return Result<Mapping>(Status::Invalid, "zero-size mapping");
    Mapping m;
    m.size = size;
    m.kind = kind;
    switch (kind) {
      case MappingKind::Private: {
        void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) return Result<Mapping>(Status::Exhausted, std::string("mmap: ") + strerror(errno));
        m.base = p;
        return m;
      }
      case MappingKind::Shared: {
        std::string err;
        int fd = anonymous_shared_fd(size, err);
        if (fd < 0) return Result<Mapping>(Status::IoError, err.empty() ? "no shared memory on this host" : err);
        void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) { std::string e = strerror(errno); ::close(fd); return Result<Mapping>(Status::Exhausted, "mmap shared: " + e); }
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
          if (::ftruncate(fd, (off_t)size) != 0) { std::string e = strerror(errno); ::close(fd); return Result<Mapping>(Status::IoError, e); }
        }
        void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) { std::string e = strerror(errno); ::close(fd); return Result<Mapping>(Status::Exhausted, "mmap file: " + e); }
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
    if (m.handle >= 0) flush_fd(m.handle);
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
    if (::link(src.c_str(), dst.c_str()) == 0) return Ok();
    if (errno == EEXIST) return Result<void>(Status::IoError, dst + ": " + strerror(errno));
    return Host::link_file(src, dst);   // another filesystem, or none that links: copy
  }

  Result<void> make_symlink(const std::string& target, const std::string& at) override {
    ::unlink(at.c_str());
    if (::symlink(target.c_str(), at.c_str()) != 0) return Result<void>(Status::IoError, at + ": " + strerror(errno));
    return Ok();
  }

  Result<void> write_file(const std::string& path, ByteSpan data) override {
    std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return Result<void>(Status::IoError, tmp + ": " + strerror(errno));
    size_t off = 0;
    while (off < data.size()) {
      ssize_t n = ::write(fd, data.data() + off, data.size() - off);
      if (n < 0) { if (errno == EINTR) continue; std::string e = strerror(errno); ::close(fd); return Result<void>(Status::IoError, e); }
      off += (size_t)n;
    }
    flush_fd(fd);
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) return Result<void>(Status::IoError, strerror(errno));
    return Ok();
  }

  // O_EXCL makes the existence check and the create one step, so of two takers exactly one gets the file. The data
  // goes into the file itself, not a temp renamed into place: a rename would replace a lock someone else just took.
  Result<void> create_exclusive(const std::string& path, ByteSpan data) override { return create_exclusive_impl(path, data, true); }
  Result<void> create_exclusive_unsynced(const std::string& path, ByteSpan data) override { return create_exclusive_impl(path, data, false); }
  Result<void> create_exclusive_impl(const std::string& path, ByteSpan data, bool durable) {
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
    if (durable) flush_fd(fd);
    ::close(fd);
    return Ok();
  }

  Result<void> append_file(const std::string& path, ByteSpan data) override {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return Result<void>(Status::IoError, path + ": " + strerror(errno));
    size_t off = 0;
    while (off < data.size()) {
      ssize_t n = ::write(fd, data.data() + off, data.size() - off);
      if (n < 0) { if (errno == EINTR) continue; std::string e = strerror(errno); ::close(fd); return Result<void>(Status::IoError, e); }
      off += (size_t)n;
    }
    flush_fd(fd);
    ::close(fd);
    return Ok();
  }

  Result<ByteBuffer> read_file(const std::string& path) override {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return Result<ByteBuffer>(errno == ENOENT ? Status::NotFound : Status::IoError, path + ": " + strerror(errno));
    ByteBuffer out;
    uint8_t buf[1 << 16];
    for (;;) {
      ssize_t n = ::read(fd, buf, sizeof buf);
      if (n < 0) { if (errno == EINTR) continue; std::string e = strerror(errno); ::close(fd); return Result<ByteBuffer>(Status::IoError, e); }
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
    fi.inode = (uint64_t)st.st_ino; fi.mtime_ns = (uint64_t)st.st_mtimespec.tv_sec * 1000000000ull + (uint64_t)st.st_mtimespec.tv_nsec; fi.disk_bytes = (uint64_t)st.st_blocks * 512;
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
  void release_free_memory() override { malloc_zone_pressure_relief(nullptr, 0); }
  std::string env(const char* name) override { const char* v = ::getenv(name); return v ? std::string(v) : std::string(); }
  bool process_alive(uint64_t pid) override { return !(::kill((pid_t)pid, 0) != 0 && errno == ESRCH); }
  void log_error(const std::string& line) override { std::string l = line + "\n"; (void)!::write(2, l.data(), l.size()); }
  Result<void> remove(const std::string& path) override { if (::remove(path.c_str()) != 0) return Result<void>(Status::IoError, path + ": " + strerror(errno)); return Ok(); }
  Result<void> rename_file(const std::string& src, const std::string& dst) override {
    if (::rename(src.c_str(), dst.c_str()) != 0) return Result<void>(Status::IoError, src + ": " + strerror(errno));
    return Ok();
  }

  uint64_t monotonic_ns() override { timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts); return uint64_t(ts.tv_sec) * 1000000000ull + (uint64_t)ts.tv_nsec; }
  uint64_t wall_ns() override { timespec ts{}; clock_gettime(CLOCK_REALTIME, &ts); return uint64_t(ts.tv_sec) * 1000000000ull + (uint64_t)ts.tv_nsec; }

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
    uint32_t free_pages = 0, inactive_pages = 0;
    uint64_t page = (uint64_t)::getpagesize();
    if (sysctl_value("vm.page_free_count", free_pages)) {
      sysctl_value("vm.page_inactive_count", inactive_pages);
      return ((uint64_t)free_pages + inactive_pages) * page;
    }
    return topo_.available_memory;
  }

  // Darwin answers for a mounted filesystem through statfs; getmntinfo walks
  // the ones that exist. The volume holding the kernel's root is named first
  // and marked, because that is the one an entity's persistence depends on.
  Result<std::vector<StorageVolume>> storage(const std::string& of_interest) override {
    std::vector<StorageVolume> out;
    struct statfs* mounts = nullptr;
    int n = ::getmntinfo(&mounts, MNT_NOWAIT);
    if (n <= 0) return Result<std::vector<StorageVolume>>(Status::IoError, "cannot enumerate mounts");
    const std::string& root = of_interest;
    // The longest mount point that prefixes the root is the one holding it.
    size_t best = std::string::npos, best_len = 0;
    if (!root.empty()) {
      for (int i = 0; i < n; ++i) {
        std::string mp = mounts[i].f_mntonname;
        if (root.rfind(mp, 0) == 0 && mp.size() >= best_len) { best = (size_t)i; best_len = mp.size(); }
      }
    }
    for (int i = 0; i < n; ++i) {
      // Synthetic filesystems describe no medium; they would only add noise.
      if (mounts[i].f_blocks == 0) continue;
      StorageVolume v;
      v.name = mounts[i].f_mntfromname;
      v.path = mounts[i].f_mntonname;
      v.kind = "filesystem";
      v.block_size = (uint32_t)mounts[i].f_bsize;
      v.capacity = (uint64_t)mounts[i].f_blocks * mounts[i].f_bsize;
      v.used = (uint64_t)(mounts[i].f_blocks - mounts[i].f_bavail) * mounts[i].f_bsize;
      v.holds_root = ((size_t)i == best);
      if (v.holds_root) out.insert(out.begin(), v); else out.push_back(v);
    }
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
      if (!a->ifa_addr) continue;
      if (a->ifa_addr->sa_family == AF_INET && iface->address.empty()) {
        char buf[INET_ADDRSTRLEN]{};
        auto* in4 = reinterpret_cast<sockaddr_in*>(a->ifa_addr);
        if (::inet_ntop(AF_INET, &in4->sin_addr, buf, sizeof buf)) iface->address = buf;
      } else if (a->ifa_addr->sa_family == AF_LINK) {
        auto* dl = reinterpret_cast<sockaddr_dl*>(a->ifa_addr);
        const uint8_t* mac = reinterpret_cast<const uint8_t*>(LLADDR(dl));
        if (dl->sdl_alen == 6) {
          char buf[18]{};
          std::snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
          iface->hardware = buf;
        }
        if (a->ifa_data) iface->mtu = (uint32_t)reinterpret_cast<if_data*>(a->ifa_data)->ifi_mtu;
      }
    }
    ::freeifaddrs(list);
    return out;
  }

 private:
  template <typename T>
  static bool sysctl_value(const char* name, T& out) {
    T v{};
    size_t len = sizeof v;
    if (::sysctlbyname(name, &v, &len, nullptr, 0) != 0 || len != sizeof v) return false;
    out = v;
    return true;
  }

  void discover() {
    int32_t cpus = 0;
    if (sysctl_value("hw.logicalcpu", cpus) && cpus > 0) topo_.logical_cpus = (uint32_t)cpus;
    else { long n = ::sysconf(_SC_NPROCESSORS_ONLN); topo_.logical_cpus = n > 0 ? (uint32_t)n : 1; }
    int64_t memsize = 0;
    if (sysctl_value("hw.memsize", memsize) && memsize > 0) topo_.total_memory = (uint64_t)memsize;
    uint32_t free_pages = 0, inactive_pages = 0;
    uint64_t page = (uint64_t)::getpagesize();
    if (sysctl_value("vm.page_free_count", free_pages)) {
      sysctl_value("vm.page_inactive_count", inactive_pages);
      topo_.available_memory = ((uint64_t)free_pages + inactive_pages) * page;
    }
    utsname u{};
    if (::uname(&u) == 0) { topo_.arch = u.machine; topo_.host_name = u.nodename; }
    // Darwin exposes no NUMA topology; this host is one node over every CPU.
    NumaNode node{0, topo_.total_memory, {}};
    for (uint32_t c = 0; c < topo_.logical_cpus; ++c) node.cpus.push_back(c);
    topo_.numa.push_back(node);
  }
  Topology topo_;
};

// A runtime is asked for by the name the substrate uses. Darwin spells the
// same libraries differently, so the translation lives here rather than above
// the boundary: libfoo.so[.N] is also tried as libfoo.dylib, and the system
// accelerator runtime is a framework rather than a file on a search path.
std::vector<std::string> library_candidates(const std::string& name) {
  std::vector<std::string> out{name};
  auto so = name.find(".so");
  if (so != std::string::npos) {
    std::string stem = name.substr(0, so);
    out.push_back(stem + ".dylib");
    if (stem == "libOpenCL" || stem == "libopencl")
      out.push_back("/System/Library/Frameworks/OpenCL.framework/OpenCL");
  }
  return out;
}

thread_local std::string last_library_error;

}  // namespace

void compute_yield() {}   // a hosted job has a thread of its own; nothing to keep alive
std::unique_ptr<Host> make_host() { return std::make_unique<MacHost>(); }

// One line to this substrate's console.
void host_note(const std::string& text) { std::fputs(text.c_str(), stderr); std::fflush(stderr); }

void* load_library(const std::string& name) {
  last_library_error.clear();
  for (const auto& candidate : library_candidates(name)) {
    if (void* h = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL)) return h;
    const char* e = dlerror();
    if (e) last_library_error = e;
  }
  return nullptr;
}
void* library_symbol(void* handle, const std::string& symbol) { return handle ? dlsym(handle, symbol.c_str()) : nullptr; }
void unload_library(void* handle) { if (handle) dlclose(handle); }
std::string library_error() {
  const char* e = dlerror();
  if (e) return e;
  return last_library_error;
}

}  // namespace aios::hal
