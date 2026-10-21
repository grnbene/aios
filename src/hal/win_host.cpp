// The Windows host. The only translation unit in the kernel that includes
// Win32 headers other than the standard library (besides the other win_*.cpp
// HAL files). Mirrors linux_host.cpp / mac_host.cpp's contract exactly; see
// aios/hal/host.hpp for what each method promises.
#include "aios/hal/host.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX   // windef.h's min/max macros would otherwise swallow every std::min/std::max call below
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <algorithm>
#include <sstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace aios::hal {

namespace {

std::wstring to_wide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? (size_t)n : 0, L'\0');
  if (n > 0) { ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n); w.resize((size_t)n - 1); }
  return w;
}
std::string to_utf8(const std::wstring& w) {
  if (w.empty()) return std::string();
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? (size_t)n : 0, '\0');
  if (n > 0) { ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr); s.resize((size_t)n - 1); }
  return s;
}
std::string win_error(DWORD err) {
  LPWSTR buf = nullptr;
  const DWORD n = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, err, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
  std::string msg = n ? to_utf8(std::wstring(buf, n)) : ("error " + std::to_string(err));
  if (buf) ::HeapFree(::GetProcessHeap(), 0, buf);
  while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
  return msg;
}
uint64_t filetime_to_unix_ns(const FILETIME& ft) {
  const uint64_t ticks = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;   // 100ns since 1601-01-01
  constexpr uint64_t kEpochDiff100ns = 116444736000000000ull;                     // 1601 -> 1970
  return (ticks - kEpochDiff100ns) * 100ull;
}

class WindowsHost final : public Host {
 public:
  WindowsHost() { discover(); }

  Result<Mapping> map(uint64_t size, MappingKind kind, const std::string& path) override {
    if (size == 0) return Result<Mapping>(Status::Invalid, "zero-size mapping");
    Mapping m;
    m.size = size;
    m.kind = kind;
    switch (kind) {
      case MappingKind::Private: {
        void* p = ::VirtualAlloc(nullptr, (SIZE_T)size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!p) return Result<Mapping>(Status::Exhausted, "VirtualAlloc: " + win_error(::GetLastError()));
        m.base = p;
        return m;
      }
      case MappingKind::Shared: {
        HANDLE section = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            (DWORD)(size >> 32), (DWORD)(size & 0xFFFFFFFFu), nullptr);
        if (!section) return Result<Mapping>(Status::IoError, "CreateFileMapping: " + win_error(::GetLastError()));
        void* p = ::MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)size);
        if (!p) { ::CloseHandle(section); return Result<Mapping>(Status::Exhausted, "MapViewOfFile: " + win_error(::GetLastError())); }
        m.base = p;
        m.handle = static_cast<int>(reinterpret_cast<intptr_t>(section));   // every Windows handle fits in 32 bits by contract
        return m;
      }
      case MappingKind::Persistent: {
        if (path.empty()) return Result<Mapping>(Status::Invalid, "persistent mapping needs a path");
        HANDLE file = ::CreateFileW(to_wide(path).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return Result<Mapping>(Status::IoError, path + ": " + win_error(::GetLastError()));
        LARGE_INTEGER cur{};
        ::GetFileSizeEx(file, &cur);
        if ((uint64_t)cur.QuadPart < size) {
          LARGE_INTEGER want{}; want.QuadPart = (LONGLONG)size;
          if (!::SetFilePointerEx(file, want, nullptr, FILE_BEGIN) || !::SetEndOfFile(file)) {
            const auto e = win_error(::GetLastError()); ::CloseHandle(file);
            return Result<Mapping>(Status::IoError, e);
          }
        }
        HANDLE section = ::CreateFileMappingW(file, nullptr, PAGE_READWRITE, (DWORD)(size >> 32), (DWORD)(size & 0xFFFFFFFFu), nullptr);
        if (!section) { const auto e = win_error(::GetLastError()); ::CloseHandle(file); return Result<Mapping>(Status::Exhausted, "CreateFileMapping: " + e); }
        void* p = ::MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)size);
        if (!p) { const auto e = win_error(::GetLastError()); ::CloseHandle(section); ::CloseHandle(file); return Result<Mapping>(Status::Exhausted, "MapViewOfFile: " + e); }
        m.base = p;
        m.handle = static_cast<int>(reinterpret_cast<intptr_t>(section));
        m.path = path;
        m.device_ctx = file;   // stashed here (provider-private) so sync()/unmap() can flush/close it too
        return m;
      }
    }
    return Result<Mapping>(Status::Invalid);
  }

  void unmap(Mapping& m) override {
    if (m.base) ::UnmapViewOfFile(m.base);
    if (m.handle >= 0) ::CloseHandle(reinterpret_cast<HANDLE>(static_cast<intptr_t>(m.handle)));
    if (m.device_ctx) ::CloseHandle(reinterpret_cast<HANDLE>(m.device_ctx));
    m.base = nullptr;
    m.handle = -1;
    m.device_ctx = nullptr;
  }

  Result<void> sync(const Mapping& m) override {
    if (m.kind != MappingKind::Persistent || !m.base) return Ok();
    if (!::FlushViewOfFile(m.base, (SIZE_T)m.size)) return Result<void>(Status::IoError, "FlushViewOfFile: " + win_error(::GetLastError()));
    if (m.device_ctx && !::FlushFileBuffers(reinterpret_cast<HANDLE>(m.device_ctx)))
      return Result<void>(Status::IoError, "FlushFileBuffers: " + win_error(::GetLastError()));
    return Ok();
  }

  Result<void> set_access(const std::string& path, FileAccess access) override {
    // NTFS has no POSIX mode bits; the one real distinction this host can make
    // is read-only vs not. Private and Executable both stay writable by the
    // owner, same as the default (mirrors Host::set_access's Ok-and-do-nothing
    // on substrates with nothing to narrow — here there is only one knob).
    (void)access;
    (void)path;
    return Ok();
  }

  Result<void> write_file(const std::string& path, ByteSpan data) override {
    const std::string tmp = path + ".tmp";
    HANDLE f = ::CreateFileW(to_wide(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return Result<void>(Status::IoError, tmp + ": " + win_error(::GetLastError()));
    size_t off = 0;
    while (off < data.size()) {
      DWORD n = 0;
      const DWORD want = (DWORD)std::min<size_t>(data.size() - off, 1u << 20);
      if (!::WriteFile(f, data.data() + off, want, &n, nullptr)) { const auto e = win_error(::GetLastError()); ::CloseHandle(f); return Result<void>(Status::IoError, e); }
      off += n;
    }
    ::FlushFileBuffers(f);
    ::CloseHandle(f);
    if (!::MoveFileExW(to_wide(tmp).c_str(), to_wide(path).c_str(), MOVEFILE_REPLACE_EXISTING))
      return Result<void>(Status::IoError, "rename: " + win_error(::GetLastError()));
    return Ok();
  }

  // CREATE_NEW makes the existence check and the create one step, so of two takers exactly one gets the file. The
  // data goes into the file itself, not a temp moved into place: a replacing move would clobber a lock just taken.
  Result<void> create_exclusive(const std::string& path, ByteSpan data) override {
    HANDLE f = ::CreateFileW(to_wide(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
      const DWORD e = ::GetLastError();
      return Result<void>(e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS ? Status::Conflict : Status::IoError,
                          path + ": " + win_error(e));
    }
    size_t off = 0;
    while (off < data.size()) {
      DWORD n = 0;
      const DWORD want = (DWORD)std::min<size_t>(data.size() - off, 1u << 20);
      if (!::WriteFile(f, data.data() + off, want, &n, nullptr)) {
        const auto e = win_error(::GetLastError());
        ::CloseHandle(f);
        ::DeleteFileW(to_wide(path).c_str());   // ours, half written: leaving it would hold the lock for nobody
        return Result<void>(Status::IoError, path + ": " + e);
      }
      off += n;
    }
    ::FlushFileBuffers(f);
    ::CloseHandle(f);
    return Ok();
  }

  Result<void> append_file(const std::string& path, ByteSpan data) override {
    HANDLE f = ::CreateFileW(to_wide(path).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return Result<void>(Status::IoError, path + ": " + win_error(::GetLastError()));
    size_t off = 0;
    while (off < data.size()) {
      DWORD n = 0;
      const DWORD want = (DWORD)std::min<size_t>(data.size() - off, 1u << 20);
      if (!::WriteFile(f, data.data() + off, want, &n, nullptr)) { const auto e = win_error(::GetLastError()); ::CloseHandle(f); return Result<void>(Status::IoError, e); }
      off += n;
    }
    ::FlushFileBuffers(f);
    ::CloseHandle(f);
    return Ok();
  }

  Result<ByteBuffer> read_range(const std::string& path, uint64_t offset, uint64_t len) override {
    HANDLE f = ::CreateFileW(to_wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
      const DWORD err = ::GetLastError();
      return Result<ByteBuffer>(err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ? Status::NotFound : Status::IoError, path + ": " + win_error(err));
    }
    LARGE_INTEGER pos{}; pos.QuadPart = (LONGLONG)offset;
    ::SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
    ByteBuffer out;
    out.resize((size_t)len);
    size_t got = 0;
    while (got < len) {
      DWORD n = 0;
      const DWORD want = (DWORD)std::min<uint64_t>(len - got, 1u << 20);
      if (!::ReadFile(f, out.data() + got, want, &n, nullptr)) { const auto e = win_error(::GetLastError()); ::CloseHandle(f); return Result<ByteBuffer>(Status::IoError, e); }
      if (n == 0) break;
      got += n;
    }
    ::CloseHandle(f);
    out.resize(got);
    return out;
  }

  Result<ByteBuffer> read_file(const std::string& path) override {
    HANDLE f = ::CreateFileW(to_wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
      const DWORD err = ::GetLastError();
      return Result<ByteBuffer>(err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ? Status::NotFound : Status::IoError, path + ": " + win_error(err));
    }
    ByteBuffer out;
    uint8_t buf[1 << 16];
    for (;;) {
      DWORD n = 0;
      if (!::ReadFile(f, buf, sizeof buf, &n, nullptr)) { const auto e = win_error(::GetLastError()); ::CloseHandle(f); return Result<ByteBuffer>(Status::IoError, e); }
      if (n == 0) break;
      out.insert(out.end(), buf, buf + n);
    }
    ::CloseHandle(f);
    return out;
  }

  Result<std::vector<std::string>> list_dir(const std::string& path) override {
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(to_wide(path + "/*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
      const DWORD err = ::GetLastError();
      return Result<std::vector<std::string>>(err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ? Status::NotFound : Status::IoError, path + ": " + win_error(err));
    }
    std::vector<std::string> out;
    do {
      const std::string n = to_utf8(fd.cFileName);
      if (n != "." && n != "..") out.push_back(n);
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return out;
  }

  Result<void> make_dir(const std::string& path) override {
    std::string cur;
    std::stringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '/')) {
      if (part.empty()) { cur += "/"; continue; }
      cur += part + "/";
      if (cur.size() <= 3 && cur[1] == ':') continue;   // "C:/" always exists
      if (!::CreateDirectoryW(to_wide(cur).c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS)
        return Result<void>(Status::IoError, cur + ": " + win_error(::GetLastError()));
    }
    return Ok();
  }

  bool exists(const std::string& path) override { return ::GetFileAttributesW(to_wide(path).c_str()) != INVALID_FILE_ATTRIBUTES; }

  Result<FileInfo> stat(const std::string& path) override {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!::GetFileAttributesExW(to_wide(path).c_str(), GetFileExInfoStandard, &a)) return Result<FileInfo>(Status::NotFound, path);
    FileInfo fi;
    fi.is_dir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    fi.size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    fi.is_link = (a.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    // FILETIME counts 100 ns ticks from 1601; the Unix epoch is 11644473600 s later.
    const uint64_t ticks = (uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    if (ticks > 116444736000000000ull) fi.mtime_ns = (ticks - 116444736000000000ull) * 100;
    // No POSIX exec bit on NTFS: a file this host would actually run.
    const auto dot = path.find_last_of('.');
    if (dot != std::string::npos) {
      std::string ext = path.substr(dot);
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
      fi.is_exec = ext == ".exe" || ext == ".bat" || ext == ".cmd" || ext == ".com";
    }
    return fi;
  }

  Result<void> remove(const std::string& path) override {
    const DWORD attrs = ::GetFileAttributesW(to_wide(path).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return Result<void>(Status::IoError, path + ": " + win_error(::GetLastError()));
    const BOOL ok = (attrs & FILE_ATTRIBUTE_DIRECTORY) ? ::RemoveDirectoryW(to_wide(path).c_str()) : ::DeleteFileW(to_wide(path).c_str());
    if (!ok) return Result<void>(Status::IoError, path + ": " + win_error(::GetLastError()));
    return Ok();
  }
  Result<void> rename_file(const std::string& src, const std::string& dst) override {
    if (!::MoveFileExW(to_wide(src).c_str(), to_wide(dst).c_str(), MOVEFILE_REPLACE_EXISTING))
      return Result<void>(Status::IoError, src + ": " + win_error(::GetLastError()));
    return Ok();
  }

  uint64_t monotonic_ns() override {
    LARGE_INTEGER freq{}, now{};
    ::QueryPerformanceFrequency(&freq);
    ::QueryPerformanceCounter(&now);
    return freq.QuadPart ? (uint64_t)((double)now.QuadPart * 1e9 / (double)freq.QuadPart) : 0;
  }
  uint64_t wall_ns() override {
    FILETIME ft{};
    ::GetSystemTimePreciseAsFileTime(&ft);
    return filetime_to_unix_ns(ft);
  }

  void random(MutableByteSpan out) override {
    if (out.empty()) return;
    if (BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, out.data(), (ULONG)out.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return;
    for (size_t i = 0; i < out.size(); ++i) out[i] = uint8_t(monotonic_ns() >> (8 * (i % 8)));   // same clock-mixed fallback as the other hosts
  }

  const Topology& topology() override { return topo_; }

  uint64_t available_bytes() override {
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof ms;
    return ::GlobalMemoryStatusEx(&ms) ? ms.ullAvailPhys : topo_.available_memory;
  }

  // Each fixed/remote drive the host reports, through its free-space call;
  // no /proc/mounts equivalent exists, so this is as exhaustive as it gets.
  Result<std::vector<StorageVolume>> storage(const std::string& of_interest) override {
    std::vector<StorageVolume> out;
    const DWORD mask = ::GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
      if (!(mask & (1u << i))) continue;
      const std::wstring root = std::wstring(1, wchar_t('A' + i)) + L":\\";
      const UINT type = ::GetDriveTypeW(root.c_str());
      if (type != DRIVE_FIXED && type != DRIVE_REMOTE && type != DRIVE_REMOVABLE) continue;
      ULARGE_INTEGER free_bytes{}, total_bytes{};
      if (!::GetDiskFreeSpaceExW(root.c_str(), &free_bytes, &total_bytes, nullptr)) continue;
      StorageVolume v;
      v.name = to_utf8(root);
      v.path = std::string(1, char('A' + i)) + ":/";
      v.kind = "filesystem";
      v.block_size = 4096;
      v.capacity = total_bytes.QuadPart;
      v.used = total_bytes.QuadPart - free_bytes.QuadPart;
      out.push_back(v);
    }
    if (!of_interest.empty()) {
      size_t best = out.size(), best_len = 0;
      for (size_t i = 0; i < out.size(); ++i) {
        if (of_interest.size() >= 2 && of_interest[1] == ':' && std::tolower((unsigned char)of_interest[0]) == std::tolower((unsigned char)out[i].path[0]) &&
            out[i].path.size() >= best_len) { best = i; best_len = out[i].path.size(); }
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

  Result<std::vector<NetworkInterface>> network() override {
    ULONG size = 0;
    if (::GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW)
      return Result<std::vector<NetworkInterface>>(Status::IoError, "GetAdaptersAddresses: size probe failed");
    std::vector<uint8_t> buf(size);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (::GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST, nullptr, addrs, &size) != NO_ERROR)
      return Result<std::vector<NetworkInterface>>(Status::IoError, "GetAdaptersAddresses failed");
    std::vector<NetworkInterface> out;
    for (auto* a = addrs; a; a = a->Next) {
      NetworkInterface iface;
      iface.name = to_utf8(a->FriendlyName ? a->FriendlyName : L"");
      iface.up = a->OperStatus == IfOperStatusUp;
      iface.loopback = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
      iface.mtu = a->Mtu;
      if (a->PhysicalAddressLength == 6) {
        char mac[18];
        std::snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", a->PhysicalAddress[0], a->PhysicalAddress[1],
            a->PhysicalAddress[2], a->PhysicalAddress[3], a->PhysicalAddress[4], a->PhysicalAddress[5]);
        iface.hardware = mac;
      }
      for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
        if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
        char buf4[64] = {};
        auto* in4 = reinterpret_cast<SOCKADDR_IN*>(u->Address.lpSockaddr);
        if (::InetNtopA(AF_INET, &in4->sin_addr, buf4, sizeof buf4)) iface.address = buf4;
        break;
      }
      out.push_back(iface);
    }
    return out;
  }

 private:
  void discover() {
    SYSTEM_INFO si{};
    ::GetNativeSystemInfo(&si);
    topo_.logical_cpus = si.dwNumberOfProcessors > 0 ? si.dwNumberOfProcessors : 1;
    switch (si.wProcessorArchitecture) {
      case PROCESSOR_ARCHITECTURE_AMD64: topo_.arch = "x86_64"; break;
      case PROCESSOR_ARCHITECTURE_ARM64: topo_.arch = "arm64"; break;
      case PROCESSOR_ARCHITECTURE_INTEL: topo_.arch = "x86"; break;
      default: topo_.arch = "unknown"; break;
    }
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
    if (::GetComputerNameW(name, &len)) topo_.host_name = to_utf8(std::wstring(name, len));
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof ms;
    if (::GlobalMemoryStatusEx(&ms)) { topo_.total_memory = ms.ullTotalPhys; topo_.available_memory = ms.ullAvailPhys; }
    // No NUMA topology probe here (GetNumaHighestNodeNumber etc.): one node
    // covering every CPU and all memory, same fallback the other hosts use
    // when they find none either.
    NumaNode node{0, topo_.total_memory, {}};
    for (uint32_t c = 0; c < topo_.logical_cpus; ++c) node.cpus.push_back(c);
    topo_.numa.push_back(node);
  }
  Topology topo_;
};

// A runtime is asked for by the name the substrate uses (POSIX spelling,
// libfoo.so[.N]); translated here rather than above the boundary, same as
// mac_host.cpp's library_candidates. A name already ending .dll (several
// callers already list a Windows name alongside their .so ones) passes through.
std::vector<std::string> library_candidates(const std::string& name) {
  std::vector<std::string> out{name};
  const auto so = name.find(".so");
  if (so != std::string::npos) {
    std::string stem = name.substr(0, so);
    if (stem.rfind("lib", 0) == 0) stem = stem.substr(3);
    out.push_back(stem + ".dll");
  }
  return out;
}

thread_local std::string last_library_error;

}  // namespace

void compute_yield() {}   // a hosted job has a thread of its own; nothing to keep alive
std::unique_ptr<Host> make_host() { return std::make_unique<WindowsHost>(); }

void host_note(const std::string& text) { std::fputs(text.c_str(), stderr); std::fflush(stderr); }

void* load_library(const std::string& name) {
  last_library_error.clear();
  for (const auto& candidate : library_candidates(name)) {
    if (HMODULE h = ::LoadLibraryW(to_wide(candidate).c_str())) return (void*)h;
    last_library_error = candidate + ": " + win_error(::GetLastError());
  }
  return nullptr;
}
void* library_symbol(void* handle, const std::string& symbol) {
  return handle ? (void*)::GetProcAddress((HMODULE)handle, symbol.c_str()) : nullptr;
}
void unload_library(void* handle) { if (handle) ::FreeLibrary((HMODULE)handle); }
std::string library_error() { return last_library_error; }

}  // namespace aios::hal
