#pragma once
// The hardware host boundary. Phase 1 the host is Linux; the kernel reaches
// it ONLY through this interface, so replacing Linux with a bare-metal layer
// means replacing this file's implementation and nothing above it.
#include <cstdint>
#include <cstddef>
#include <string>
#include <algorithm>
#include <vector>
#include <memory>
#include "aios/core/result.hpp"
#include "aios/core/bytes.hpp"

namespace aios::hal {

// A file's bytes, read-only, for as long as the view lives: mapped where the
// host can map files (pages come in as they are touched), else read whole.
struct FileView {
  virtual ~FileView() = default;
  const uint8_t* data = nullptr;
  size_t size = 0;
};

enum class MappingKind : uint8_t {
  Private,     // anonymous, this address space only
  Shared,      // anonymous shared object; may be handed to another address space
  Persistent,  // backed by durable storage at a path
};

struct Mapping {
  void* base = nullptr;
  uint64_t size = 0;
  MappingKind kind = MappingKind::Private;
  int handle = -1;         // host object handle when kind != Private
  std::string path;        // for Persistent
  void* device_ctx = nullptr;   // provider-private (e.g. an accelerator buffer handle)
  const void* device_owner = nullptr;   // which backend's provider device_ctx belongs to
};

struct NumaNode { uint32_t id; uint64_t memory_bytes; std::vector<uint32_t> cpus; };

struct Topology {
  uint32_t logical_cpus = 1;
  uint64_t total_memory = 0;
  uint64_t available_memory = 0;
  std::vector<NumaNode> numa;
  std::string arch;
  std::string host_name;   // the hosting system, informational only
};

// Storage and network are resources of the machine, the same way CPUs and
// memory are: an entity must be able to ask what EXISTS, not only what it has
// already used. Both are enumerated here so every substrate answers the same
// question — a bare-metal kernel with one virtio disk and a hosted kernel on a
// file tree describe themselves in the same terms.
struct StorageVolume {
  std::string name;          // what the machine calls it: a device, a volume label, a path
  std::string path;          // where the kernel reaches it
  std::string kind;          // "volume" (AOS's own), "filesystem" (the host's), "device" (raw)
  uint64_t capacity = 0;     // bytes the medium holds, 0 when the host cannot say
  uint64_t used = 0;         // bytes accounted for
  uint32_t block_size = 0;
  bool holds_root = false;   // the kernel's persistent root lives here
};

// A function found on one of the machine's buses. Every field is read from the
// bus or from the substrate that owns it; nothing here is inferred.
struct HardwareDevice {
  std::string slot;      // "00:03.0" — where it physically answers
  std::string kind;      // display, ethernet, storage, bridge, ... from the class code
  std::string name;      // vendor and device, named where they are known
  std::string driver;    // what has claimed it, empty when nothing has
};

struct NetworkInterface {
  std::string name;
  std::string address;       // the address the machine answers on, in the host's notation
  std::string hardware;      // MAC or equivalent, empty when there is none
  uint64_t speed_bps = 0;    // 0 when the host cannot say
  uint32_t mtu = 0;
  bool up = false;
  bool loopback = false;
  bool carries_fabric = false;   // AOS's own node fabric runs over this one
};

class Host {
 public:
  virtual ~Host() = default;
  virtual Result<Mapping> map(uint64_t size, MappingKind kind, const std::string& path = {}) = 0;
  virtual void unmap(Mapping& m) = 0;
  virtual Result<void> sync(const Mapping& m) = 0;   // flush persistent mapping

  virtual Result<void> write_file(const std::string& path, ByteSpan data) = 0;
  // Like write_file without forcing the bytes to disk first: a caller writing thousands of files (the objects of a
  // fetched pack) writes them this way and calls sync_tree once when it is done, instead of paying a flush per file.
  // A host that cannot tell the difference just writes durably.
  virtual Result<void> write_file_unsynced(const std::string& path, ByteSpan data) { return write_file(path, data); }
  // `data` at `offset` of `path`, not flushed (sync_file does that); a host
  // without positional writes appends. A log written this way into space
  // set aside with preallocate never changes the file's size on a commit,
  // so its fdatasync flushes data only, never a size change through the
  // filesystem's journal.
  virtual Result<void> write_at_unsynced(const std::string& path, uint64_t offset, ByteSpan data) { (void)offset; return append_file_unsynced(path, data); }
  virtual Result<void> preallocate(const std::string& path, uint64_t bytes) { (void)path; (void)bytes; return Ok(); }
  // A new file of `bytes` zero bytes actually written and on the medium
  // (paced, stepping aside for commit flushes): a log written over such
  // blocks changes no extent on a commit. Space preallocate sets aside is
  // unwritten, and the first write into each block converted it - a
  // metadata write in every fdatasync, 300 us a commit where 200 do.
  virtual Result<void> write_zeros(const std::string& path, uint64_t bytes) { return preallocate(path, bytes); }
  // `data` at `offset` past the page cache (O_DIRECT): offset, size and the
  // buffer's address all multiples of 4096. Not flushed (sync_file does).
  // Status::Unsupported where the file system or host cannot: the caller
  // writes through the cache instead.
  virtual Result<void> write_at_direct(const std::string& path, uint64_t offset, ByteSpan data) {
    (void)path; (void)offset; (void)data;
    return Result<void>(Status::Unsupported, "no direct writes");
  }
  // remove() for a big file nothing waits on (a superseded checkpoint file):
  // its blocks freed a few MB at a time, stepping aside for commit flushes,
  // so a filesystem that discards freed blocks does not stall the disk.
  virtual Result<void> remove_gently(const std::string& path) { return remove(path); }
  // The file written in place, truncated first, no temp file and no rename
  // and no flush: a working-tree file on checkout, where nothing reads it
  // half-written and git writes it the same way.
  virtual Result<void> write_file_plain(const std::string& path, ByteSpan data) { return write_file_unsynced(path, data); }
  // An append that returns once the bytes are in the file, not once they are on
  // the medium; sync_file then makes everything appended so far durable. The SQL
  // redo log groups the appends of every session into one sync. A host without
  // the distinction appends durably and has nothing left to sync.
  virtual Result<void> append_file_unsynced(const std::string& path, ByteSpan data) { return append_file(path, data); }
  virtual Result<void> sync_file(const std::string& path) { (void)path; return Ok(); }
  // Creates `path` holding `data`, failing with Status::Conflict when it already exists: how a lock file is taken.
  // Checking for the file and then writing it lets two takers both see it absent and both believe they hold it, so a
  // substrate that can create exclusively in one step must override this; the default, for one that cannot, is that
  // check-then-write and only as safe as nobody else racing it. A partly written file is removed, not left as a lock.
  virtual Result<void> create_exclusive(const std::string& path, ByteSpan data) {
    if (exists(path)) return Result<void>(Status::Conflict, path + ": File exists");
    return write_file(path, data);
  }
  // The same for a secret (a signing key): readable by this account only from the moment it exists - not created
  // open and narrowed afterwards. The default, for a substrate without that notion, narrows straight after.
  virtual Result<void> create_exclusive_private(const std::string& path, ByteSpan data) {
    auto r = create_exclusive(path, data);
    if (!r.ok()) return r;
    return set_access(path, FileAccess::Private);
  }
  // The same without waiting for the medium: a lock file git renames into place (a ref), which git does not sync
  // unless core.fsync asks for references.
  virtual Result<void> create_exclusive_unsynced(const std::string& path, ByteSpan data) { return create_exclusive(path, data); }
  // Everything written under `path` is on disk when this returns (nothing to do for a host whose writes are durable).
  virtual void sync_tree(const std::string& path) { (void)path; }
  virtual Result<void> append_file(const std::string& path, ByteSpan data) = 0;
  virtual Result<ByteBuffer> read_file(const std::string& path) = 0;
  virtual Result<std::shared_ptr<const FileView>> view_file(const std::string& path) {
    struct Held final : FileView { ByteBuffer bytes; };
    auto r = read_file(path);
    if (!r.ok()) return Result<std::shared_ptr<const FileView>>(r.error());
    auto v = std::make_shared<Held>();
    v->bytes = std::move(*r);
    v->data = v->bytes.data();
    v->size = v->bytes.size();
    return std::shared_ptr<const FileView>(std::move(v));
  }
  // Part of a file, without reading the whole of it. An append-only log is
  // read this way: the store wants one record, not the 235MB in front of it.
  // The default is correct on any substrate and slow on all of them; a host
  // that can seek should override it.
  virtual Result<ByteBuffer> read_range(const std::string& path, uint64_t offset, uint64_t len) {
    auto all = read_file(path);
    if (!all.ok()) return all.error();
    if (offset >= all->size()) return ByteBuffer{};
    const uint64_t n = std::min<uint64_t>(len, all->size() - offset);
    return ByteBuffer(all->begin() + (size_t)offset, all->begin() + (size_t)(offset + n));
  }
  virtual Result<std::vector<std::string>> list_dir(const std::string& path) = 0;
  // A directory's entries with their kind where the file system says it (readdir's d_type), so a walk need not stat
  // every name: 'd' directory, 'l' symbolic link, 'f' anything else, '?' unknown (stat it).
  struct DirEntry { std::string name; char kind = '?'; };
  virtual Result<std::vector<DirEntry>> list_dir_typed(const std::string& path) {
    auto names = list_dir(path);
    if (!names.ok()) return Result<std::vector<DirEntry>>(names.error());
    std::vector<DirEntry> out;
    out.reserve(names->size());
    for (auto& n : *names) out.push_back(DirEntry{std::move(n), '?'});
    return out;
  }
  virtual Result<void> make_dir(const std::string& path) = 0;
  virtual bool exists(const std::string& path) = 0;
  // What the path IS, without following it: a symbolic link is an entry in its
  // own right, and a machine that silently reports the thing at the other end
  // cannot tell a link from a copy of the file it points at.
  struct FileInfo { bool is_dir = false; uint64_t size = 0; bool is_link = false; bool is_exec = false;
                    uint64_t inode = 0, mtime_ns = 0; uint32_t mode = 0, uid = 0, gid = 0;
                    uint64_t disk_bytes = 0;   // what the file takes on disk (st_blocks * 512), its size where unknown
  };
  virtual Result<FileInfo> stat(const std::string& path) = 0;
  virtual Result<void> remove(const std::string& path) = 0;   // a file or an empty directory
  // dst becomes a second name for src's bytes: a hard link where the substrate
  // has them (instant, and immune to src being replaced or removed afterwards),
  // a copy where it does not.
  virtual Result<void> link_file(const std::string& src, const std::string& dst) {
    auto d = read_file(src);
    if (!d.ok()) return d.error();
    return write_file(dst, ByteSpan(d->data(), d->size()));
  }
  // src takes dst's name, replacing what was there: how a lock file that holds a file's new content
  // becomes that file. A substrate with a rename does it in one step; the default copies (write_file
  // replaces dst whole, so a reader never sees half of it) and then removes src.
  virtual Result<void> rename_file(const std::string& src, const std::string& dst) {
    auto d = read_file(src);
    if (!d.ok()) return d.error();
    auto w = write_file(dst, ByteSpan(d->data(), d->size()));
    if (!w.ok()) return w;
    return remove(src);
  }

  // How a file may be reached. A private key written into the kernel root must
  // not be readable by every account on a shared substrate, and a program
  // checked out of a repository has to stay a program. A substrate with no such
  // notion (bare metal: one address space, no other users) answers Ok, because
  // there is nothing there to narrow or to mark.
  enum class FileAccess : uint8_t { Default, Private, Executable };
  virtual Result<void> set_access(const std::string& path, FileAccess access) { (void)path; (void)access; return Ok(); }
  // A symbolic link, where the substrate has them. Refused rather than faked
  // where it does not, so a caller can decide what to do instead.
  virtual Result<void> make_symlink(const std::string& target, const std::string& at) {
    (void)target; (void)at;
    return Result<void>(Status::Unsupported, "this substrate has no symbolic links");
  }
  // What a symbolic link names, which is its content as far as a repository is
  // concerned.
  virtual Result<std::string> read_symlink(const std::string& path) {
    (void)path;
    return Result<std::string>(Status::Unsupported, "this substrate has no symbolic links");
  }

  virtual uint64_t monotonic_ns() = 0;
  virtual uint64_t wall_ns() = 0;
  virtual void random(MutableByteSpan out) = 0;
  virtual const Topology& topology() = 0;
  virtual uint64_t available_bytes() { return topology().available_memory; }
  // Pure, deliberately: a substrate that cannot describe its own storage or
  // network leaves the kernel unable to answer what the machine is, and the
  // gap should be visible at compile time rather than at the prompt.
  // `of_interest` is a path the caller cares about (the kernel's root); the
  // volume holding it comes back first and marked. The host is not told what
  // the kernel keeps where, so it cannot work that out on its own.
  virtual Result<std::vector<StorageVolume>> storage(const std::string& of_interest = {}) = 0;
  virtual Result<std::vector<NetworkInterface>> network() = 0;
  // What the machine finds on its own buses. On bare metal this is a real PCI
  // configuration-space scan; on a hosted substrate it is what that OS says is
  // on the bus. Not pure: a substrate that cannot enumerate a bus answers with
  // nothing and the machine says so, which is different from lying about it.
  virtual Result<std::vector<HardwareDevice>> hardware() { return std::vector<HardwareDevice>{}; }
  // Whether a host process id is running (a pid file's owner). No processes: always "yes".
  virtual bool process_alive(uint64_t pid) { (void)pid; return true; }
  // Return freed allocator pages to the host. No-op where there is no host allocator.
  virtual void release_free_memory() {}
  // A line for the host's error log (stderr on a hosted machine).
  virtual void log_error(const std::string& line) { (void)line; }
  // A setting from the host environment; empty when unset or where there is none.
  virtual std::string env(const char* name) { (void)name; return {}; }
  // Shared memory other processes on this host can open by name. Where there
  // are no other processes (bare metal) it is an ordinary shared mapping.
  virtual Result<Mapping> map_named(const std::string& name, uint64_t size, bool create) {
    (void)name; (void)create; return map(size, MappingKind::Shared);
  }
  virtual void unlink_named(const std::string& name) { (void)name; }
};

std::unique_ptr<Host> make_host();   // provided by the HAL implementation linked into this build
// One Host for code that runs without a kernel (the sidecars and their C ABI).
inline Host& process_host() { static std::unique_ptr<Host> h = make_host(); return *h; }

// A long computation (a solver, a search) calls this every so often. Where a
// thread of its own runs the machine (hosted) it does nothing; on a kernel
// whose one loop also carries the network and the fleet link (the Pi) it
// keeps them alive - the watchdog petted, packets moved, the hub link read -
// so a job of minutes does not look, to the hub, like a machine gone silent.
void compute_yield();

// One line from the machine to whatever this substrate calls a console: a
// hosted build writes it to standard error, a bare-metal kernel puts it on the
// serial console. The core says things this way rather than reaching for stdio
// itself, which is what the zero-host gate is about.
void host_note(const std::string& text);

// A decimal or hexadecimal floating-point number read from text, correctly rounded (strtod's grammar and answer);
// *end past what was read. Exact decimal-to-binary conversion is the host C library's on a hosted machine, which the
// core may not call itself; bare metal brings its own.
double parse_double(const char* s, char** end);

// Runtime-bound host libraries (an accelerator runtime, a plugin). Handles are
// opaque; a null handle means the library is not present on this host.
void* load_library(const std::string& name);
void* library_symbol(void* handle, const std::string& symbol);
void unload_library(void* handle);
std::string library_error();

}  // namespace aios::hal
