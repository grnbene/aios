// The repository's command lock (see take_repo_lock in cli.hpp): an advisory lock on $GIT_COMMON_DIR/aios.lock, held
// until the process exits. flock on Linux and macOS, LockFileEx on Windows.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <filesystem>
#include <thread>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <cerrno>
#endif
#include <cstdlib>

#include "cli.hpp"

namespace aios_git {

namespace {
#if defined(_WIN32)
HANDLE open_lock_file(const std::string& path) {
  int n = ::MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), w.data(), n);
  // shared for reading and writing: the lock is the byte range, not the open
  return ::CreateFileW(w.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}
// The whole of the file's range, exclusively; without waiting (the caller loops).
bool try_lock(HANDLE h) {
  OVERLAPPED ov{};
  return ::LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &ov) != 0;
}
void set_env(const char* k, const std::string& v) { ::_putenv_s(k, v.c_str()); }
#else
void set_env(const char* k, const std::string& v) { ::setenv(k, v.c_str(), 1); }
#endif
}  // namespace

bool take_repo_lock(Ctx& c, bool wait) {
  if (!c.repo) return true;
  const char* held = std::getenv("AIOS_GIT_LOCK_HELD");
  std::string path = c.repo->common_dir() + "/aios.lock";
  if (held && path == held) return true;   // a program the holder started: it is ours already
#if defined(_WIN32)
  HANDLE h = open_lock_file(path);
  if (h == INVALID_HANDLE_VALUE) return true;   // a read-only repository: nothing here to protect
  for (int waited = 0;; ++waited) {
    if (try_lock(h)) break;
    if (::GetLastError() != ERROR_LOCK_VIOLATION && ::GetLastError() != ERROR_IO_PENDING) { ::CloseHandle(h); return true; }
    if (!wait) { ::CloseHandle(h); return false; }
    if (waited == 40) c.err << "waiting for another git command in this repository to finish...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  // kept open (and so locked) until this process exits
#else
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return true;                  // a read-only repository: nothing here to protect
  for (int waited = 0;; ++waited) {
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) break;
    if (errno != EWOULDBLOCK && errno != EINTR) { ::close(fd); return true; }
    if (!wait) { ::close(fd); return false; }
    if (waited == 40) c.err << "waiting for another git command in this repository to finish...\n";
    ::usleep(25000);
  }
  // kept open (and so locked) until this process exits
#endif
  set_env("AIOS_GIT_LOCK_HELD", path);
  c.env["AIOS_GIT_LOCK_HELD"] = path;
  return true;
}

bool repo_lock_free(Ctx& c) {
  if (!c.repo) return true;
  const char* held = std::getenv("AIOS_GIT_LOCK_HELD");
  std::string path = c.repo->common_dir() + "/aios.lock";
  if (held && path == held) return true;
#if defined(_WIN32)
  HANDLE h = open_lock_file(path);
  if (h == INVALID_HANDLE_VALUE) return true;
  const bool free = try_lock(h);
  ::CloseHandle(h);   // (closing lets it go)
  return free;
#else
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return true;
  const bool free = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
  ::close(fd);   // (closing lets it go)
  return free;
#endif
}

}  // namespace aios_git
