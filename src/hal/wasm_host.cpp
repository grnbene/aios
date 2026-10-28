// The WebAssembly host (Emscripten, in a browser or Node). The counterpart of linux_host.cpp and mac_host.cpp for a
// machine that is a page: files live on Emscripten's in-memory file system, the clocks and the random bytes are the
// page's, and what a page does not have - durable mappings, shared memory, buses, interfaces - is answered as
// unsupported rather than made up.
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "aios/hal/host.hpp"

namespace aios::hal {

namespace {

class WasmHost final : public Host {
 public:
  WasmHost() {
    topo_.logical_cpus = 1;
    topo_.arch = "wasm32";
    topo_.host_name = "browser";
    topo_.total_memory = 2ull << 30;     // what a wasm32 page can address, at most
    topo_.available_memory = topo_.total_memory;
  }

  // A private mapping is memory; a shared or durable one needs a machine a page is not.
  Result<Mapping> map(uint64_t size, MappingKind kind, const std::string& path) override {
    if (size == 0) return Result<Mapping>(Status::Invalid, "zero-size mapping");
    if (kind != MappingKind::Private) return Result<Mapping>(Status::Unsupported, "shared and persistent mappings are not available in a browser");
    void* p = std::calloc(1, size);
    if (!p) return Result<Mapping>(Status::Exhausted, "out of memory");
    Mapping m;
    m.base = p;
    m.size = size;
    m.kind = kind;
    m.path = path;
    return m;
  }
  void unmap(Mapping& m) override { std::free(m.base); m.base = nullptr; m.size = 0; }
  Result<void> sync(const Mapping&) override { return Ok(); }

  Result<void> write_file(const std::string& path, ByteSpan data) override { return write_all(path, data, O_WRONLY | O_CREAT | O_TRUNC); }
  Result<void> append_file(const std::string& path, ByteSpan data) override { return write_all(path, data, O_WRONLY | O_CREAT | O_APPEND); }

  Result<void> write_at_unsynced(const std::string& path, uint64_t offset, ByteSpan data) override {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT, 0644);
    if (fd < 0) return fail(path);
    ssize_t n = data.empty() ? 0 : ::pwrite(fd, data.data(), data.size(), (off_t)offset);
    const bool ok = n == (ssize_t)data.size();
    ::close(fd);
    return ok ? Result<void>(Ok()) : fail(path);
  }

  Result<void> create_exclusive(const std::string& path, ByteSpan data) override {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return Result<void>(errno == EEXIST ? Status::Conflict : Status::IoError, path + ": " + std::strerror(errno));
    const bool ok = put(fd, data);
    ::close(fd);
    return ok ? Result<void>(Ok()) : fail(path);
  }

  Result<ByteBuffer> read_file(const std::string& path) override {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return Result<ByteBuffer>(errno == ENOENT ? Status::NotFound : Status::IoError, path + ": " + std::strerror(errno));
    ByteBuffer out;
    uint8_t buf[1 << 16];
    for (;;) {
      ssize_t n = ::read(fd, buf, sizeof buf);
      if (n < 0) { if (errno == EINTR) continue; std::string e = std::strerror(errno); ::close(fd); return Result<ByteBuffer>(Status::IoError, path + ": " + e); }
      if (n == 0) break;
      out.insert(out.end(), buf, buf + n);
    }
    ::close(fd);
    return out;
  }

  Result<std::vector<DirEntry>> list_dir_typed(const std::string& path) override {
    DIR* d = ::opendir(path.c_str());
    if (!d) return Result<std::vector<DirEntry>>(errno == ENOENT ? Status::NotFound : Status::IoError, path + ": " + std::strerror(errno));
    std::vector<DirEntry> out;
    while (dirent* e = ::readdir(d)) {
      const std::string name = e->d_name;
      if (name == "." || name == "..") continue;
      char kind = '?';
      if (e->d_type == DT_DIR) kind = 'd';
      else if (e->d_type == DT_REG) kind = 'f';
      else if (e->d_type == DT_LNK) kind = 'l';
      out.push_back({name, kind});
    }
    ::closedir(d);
    return out;
  }
  Result<std::vector<std::string>> list_dir(const std::string& path) override {
    auto typed = list_dir_typed(path);
    if (!typed.ok()) return Result<std::vector<std::string>>(typed.error().status, typed.error().message);
    std::vector<std::string> names;
    for (auto& e : *typed) names.push_back(e.name);
    return names;
  }

  Result<void> make_dir(const std::string& path) override {
    if (::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) return Ok();
    return fail(path);
  }
  bool exists(const std::string& path) override { struct stat st{}; return ::stat(path.c_str(), &st) == 0; }

  Result<FileInfo> stat(const std::string& path) override {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) return Result<FileInfo>(Status::NotFound, path);
    FileInfo fi{S_ISDIR(st.st_mode), (uint64_t)st.st_size, S_ISLNK(st.st_mode), (st.st_mode & S_IXUSR) != 0};
    fi.inode = (uint64_t)st.st_ino;
    fi.mtime_ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
    fi.disk_bytes = (uint64_t)st.st_size;
    fi.mode = (uint32_t)st.st_mode;
    fi.uid = (uint32_t)st.st_uid;
    fi.gid = (uint32_t)st.st_gid;
    return fi;
  }
  Result<void> remove(const std::string& path) override {
    if (::unlink(path.c_str()) == 0 || ::rmdir(path.c_str()) == 0) return Ok();
    return Result<void>(errno == ENOENT ? Status::NotFound : Status::IoError, path + ": " + std::strerror(errno));
  }
  Result<void> rename_file(const std::string& src, const std::string& dst) override {
    if (::rename(src.c_str(), dst.c_str()) == 0) return Ok();
    return Result<void>(errno == ENOENT ? Status::NotFound : Status::IoError, src + ": " + std::strerror(errno));
  }

  uint64_t monotonic_ns() override { timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts); return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec); }
  uint64_t wall_ns() override { timespec ts{}; clock_gettime(CLOCK_REALTIME, &ts); return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec); }

  // The page's own source: Emscripten's getentropy is crypto.getRandomValues. No fallback to the clock - a wrong
  // key is worse than no key.
  void random(MutableByteSpan out) override {
    size_t off = 0;
    while (off < out.size()) {
      const size_t n = out.size() - off < 256 ? out.size() - off : 256;
      if (::getentropy(out.data() + off, n) != 0) std::abort();
      off += n;
    }
  }

  const Topology& topology() override { return topo_; }
  Result<std::vector<StorageVolume>> storage(const std::string&) override { return std::vector<StorageVolume>{}; }
  Result<std::vector<NetworkInterface>> network() override { return std::vector<NetworkInterface>{}; }

  std::string env(const char* name) override { const char* v = ::getenv(name); return v ? std::string(v) : std::string(); }
  void log_error(const std::string& line) override { std::string l = line + "\n"; std::fputs(l.c_str(), stderr); }

 private:
  static Result<void> fail(const std::string& path) {
    return Result<void>(errno == ENOENT ? Status::NotFound : Status::IoError, path + ": " + std::strerror(errno));
  }
  static bool put(int fd, ByteSpan data) {
    size_t off = 0;
    while (off < data.size()) {
      ssize_t n = ::write(fd, data.data() + off, data.size() - off);
      if (n < 0) { if (errno == EINTR) continue; return false; }
      off += (size_t)n;
    }
    return true;
  }
  static Result<void> write_all(const std::string& path, ByteSpan data, int flags) {
    int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) return fail(path);
    const bool ok = put(fd, data);
    ::close(fd);
    return ok ? Result<void>(Ok()) : fail(path);
  }

  Topology topo_;
};

thread_local std::string last_library_error;

}  // namespace

void compute_yield() {}   // one thread, no fleet link to keep alive
std::unique_ptr<Host> make_host() { return std::make_unique<WasmHost>(); }

void host_note(const std::string& text) { std::fputs(text.c_str(), stderr); std::fflush(stderr); }
double parse_double(const char* s, char** end) { return std::strtod(s, end); }

// A page loads no shared libraries.
void* load_library(const std::string& name) { last_library_error = name + ": shared libraries are not available in a browser"; return nullptr; }
void* library_symbol(void*, const std::string&) { return nullptr; }
void unload_library(void*) {}
std::string library_error() { return last_library_error; }

}  // namespace aios::hal
