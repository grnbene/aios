// git fsmonitor--daemon: a process that watches a work tree and tells every command which paths may have changed since
// it last looked, so `status`, the change layer's snapshot and the operation log stat only those instead of every
// file in the repository. git's own name and its core.fsmonitor setting; the protocol is aios-git's.
//
// The daemon keeps every change it has seen numbered; a token names a point in that sequence ("aios:<epoch>:<seq>").
// A command asks "what since <token>?" and gets the paths, or "everything" (the daemon restarted, the watcher lost
// events, or the token is older than what is kept). The command keeps, in $GIT_DIR/aios-fsmonitor, the token it got
// and the paths it found different from the index then (still-modified and untracked files, which change no further
// and so raise no event): what it looks at next time is those, plus what changed since, plus index entries with no
// stat information.
//
// In the change layer the daemon also snapshots: when the files have been quiet for change.snapshotDelay ms (300)
// after an edit, it runs `git change snapshot --auto`, so @ follows the work tree as it is edited, each settled
// state an operation in the log (undoable like any other) without a command having to run. That snapshot takes no
// lock (it swaps @ by compare-and-swap): when a command is running, that command snapshots anyway.
// change.autoSnapshot false turns it off.
//
// It also keeps every saved version of every file: once a file written (or renamed into place, as editors save) has
// been still for 25 ms, its content goes into the object store and a line "<ms> <blob> <path>" into
// $GIT_DIR/aios-journal, so a version the snapshot's settling skipped over can still be had back (git journal). A
// watcher says a file was written, not what it held: saves less than 25 ms apart are kept as the last of them, and a
// file read while it changes is read again. Ignored files that are not tracked, and files over
// change.journalMaxSize (8 MiB), are left out; change.journal false turns it off.
//
// The watcher is the system's own: inotify on Linux (one watch per directory), FSEvents on macOS (file-level events
// for the whole tree), ReadDirectoryChangesW on Windows (the tree, recursively). Commands reach the daemon over a
// Unix-domain socket on all three (Windows has had AF_UNIX since Windows 10 1803).
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <afunix.h>
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <dirent.h>
#include <climits>
#if defined(__linux__)
#include <sys/inotify.h>
#elif defined(__APPLE__)
// (AssertMacros.h, which CoreServices brings in, would otherwise define check/verify/require as macros)
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#endif
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include "aios/crypto/sha1.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/process.hpp"
#include "cli.hpp"

#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
#define AIOS_FSMONITOR 1
#endif

namespace aios_git {

using namespace aios;

namespace {

std::string str_of(const ByteBuffer& b) { return std::string((const char*)b.data(), b.size()); }

std::string socket_path(Ctx& c) {
  std::string p = c.repo->git_dir() + "/fsmonitor--daemon.ipc";
  if (p.size() < 100) return p;
  // a socket's name is short everywhere (104 to 108 bytes): a long one is named by the work tree's hash instead
  auto sum = crypto::sha1(ByteSpan((const uint8_t*)c.top.data(), c.top.size()));
  static const char* hx = "0123456789abcdef";
  std::string h;
  for (int i = 0; i < 8; ++i) { h += hx[sum[i] >> 4]; h += hx[sum[i] & 15]; }
  std::error_code ec;
  std::string tmp = std::filesystem::temp_directory_path(ec).generic_string();
  if (ec || tmp.empty()) tmp = "/tmp";
  if (tmp.back() == '/') tmp.pop_back();
  return tmp + "/aios-fsmonitor-" + h + ".sock";
}

#ifdef AIOS_FSMONITOR

// ---- sockets ----------------------------------------------------------------------------------------------------
#if defined(_WIN32)
using sock_t = SOCKET;
const sock_t kNoSock = INVALID_SOCKET;
void close_sock(sock_t s) { ::closesocket(s); }
bool sockets_ready() {
  static bool ok = [] { WSADATA w; return ::WSAStartup(MAKEWORD(2, 2), &w) == 0; }();
  return ok;
}
#else
using sock_t = int;
const sock_t kNoSock = -1;
void close_sock(sock_t s) { ::close(s); }
bool sockets_ready() { return true; }
#endif

sock_t unix_socket() {
  if (!sockets_ready()) return kNoSock;
  sock_t s = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (s == kNoSock) return s;
#if defined(__APPLE__)
  int one = 1;
  ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);   // a reader gone away is an error, not a signal
#endif
#if !defined(_WIN32)
  ::fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
  return s;
}

sockaddr_un address(const std::string& path) {
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
  return a;
}

sock_t connect_to(const std::string& path) {
  sock_t fd = unix_socket();
  if (fd == kNoSock) return fd;
  sockaddr_un a = address(path);
  if (::connect(fd, (sockaddr*)&a, sizeof a) != 0) { close_sock(fd); return kNoSock; }
  return fd;
}

bool send_all(sock_t fd, const std::string& s) {
  size_t off = 0;
#if defined(__linux__)
  const int flags = MSG_NOSIGNAL;
#else
  const int flags = 0;
#endif
  while (off < s.size()) {
    auto n = ::send(fd, s.data() + off, (int)(s.size() - off), flags);
    if (n <= 0) {
#if !defined(_WIN32)
      if (errno == EINTR) continue;
#endif
      return false;
    }
    off += (size_t)n;
  }
  return true;
}

std::string recv_all(sock_t fd) {
  std::string out;
  char buf[65536];
  while (true) {
    auto n = ::recv(fd, buf, (int)sizeof buf, 0);
#if !defined(_WIN32)
    if (n < 0 && errno == EINTR) continue;
#endif
    if (n <= 0) break;
    out.append(buf, (size_t)n);
  }
  return out;
}

void end_sending(sock_t fd) {
#if defined(_WIN32)
  ::shutdown(fd, SD_SEND);
#else
  ::shutdown(fd, SHUT_WR);
#endif
}

std::optional<std::string> ask(const std::string& sock, const std::string& request) {
  sock_t fd = connect_to(sock);
  if (fd == kNoSock) return std::nullopt;
  bool ok = send_all(fd, request + "\n");
  end_sending(fd);
  std::string reply = ok ? recv_all(fd) : "";
  close_sock(fd);
  if (!ok) return std::nullopt;
  return reply;
}

// This program's own path: what the background snapshot runs.
std::string self_path() {
#if defined(__linux__)
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
  return n > 0 ? std::string(buf, (size_t)n) : std::string();
#elif defined(__APPLE__)
  char buf[4096];
  uint32_t size = sizeof buf;
  if (_NSGetExecutablePath(buf, &size) != 0) return "";
  char real[PATH_MAX];
  return ::realpath(buf, real) ? std::string(real) : std::string(buf);
#else
  wchar_t buf[4096];
  DWORD n = ::GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof buf / sizeof buf[0]));
  if (n == 0 || n >= sizeof buf / sizeof buf[0]) return "";
  auto u8 = std::filesystem::path(std::wstring(buf, n)).generic_u8string();
  return std::string(u8.begin(), u8.end());
#endif
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::filesystem::path fs_path(const std::string& utf8) {
#if defined(_WIN32)
  int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), w.data(), n);
  return std::filesystem::path(w);
#else
  return std::filesystem::path(utf8);
#endif
}

// ---- what a watcher reports -------------------------------------------------------------------------------------
struct WatchEvent {
  std::string rel;          // relative to the top, '/' separated ("" the top itself)
  bool saved = false;       // a file written or put in place (a candidate for the journal)
  bool new_dir = false;     // a directory came into being (inotify must be told to watch it)
};

// ---------------------------------------------------------------------------------------------------------------
// the daemon
// ---------------------------------------------------------------------------------------------------------------
struct Daemon {
  Daemon(Ctx& ctx, std::string t, std::string s) : c(ctx), top(std::move(t)), sock(std::move(s)) {}
  Ctx& c;
  std::string top, sock;
  sock_t lsn = kNoSock;
  std::string epoch;
  uint64_t seq = 0, floor = 0;                   // tokens older than floor are answered "everything"
  std::vector<std::pair<uint64_t, std::string>> changes;
  bool watches_exhausted = false;
  size_t watching = 0;                           // directories (inotify) or trees (the others) watched
  std::string watcher_name;
  // background snapshots of @ (the change layer)
  bool auto_snapshot = true;
  long delay_ms = 300;
  int64_t due = -1;                              // when the next snapshot runs (ms, steady clock); -1 none
  std::optional<hal::ProcessHandle> child;
  uint64_t snapshots = 0;
  std::string self;
  // the journal of saved versions
  bool journal_on = false;
  long long journal_max = 8 << 20;
  uint64_t journaled = 0;
  int64_t journal_due = -1;
  std::set<std::string> saved;                        // paths saved since the last drain
  std::map<std::string, std::string> last_blob;       // path -> the blob last journaled for it

  // events from a watcher thread (FSEvents, ReadDirectoryChangesW), taken by the loop
  std::mutex mu;
  std::vector<WatchEvent> pending;
  bool pending_overflow = false;
  std::atomic<bool> stopping{false};

  static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  void journal() {
    if (saved.empty()) return;
    std::string lines;
    auto& h = c.host();
    // an ignored file counts only when it is tracked (added with -f): the index, read afresh, says (rarely needed)
    std::optional<std::set<std::string>> tracked;
    auto is_tracked = [&](const std::string& rel) {
      if (!tracked) {
        tracked.emplace();
        userland::Git fresh(*c.kernel);
        if (fresh.open(top).ok()) for (auto& e : fresh.index()) tracked->insert(e.path);
      }
      return tracked->count(rel) != 0;
    };
    for (const auto& rel : saved) {
      if (rel.empty() || in_git_dir(rel)) continue;
      if (c.repo->is_ignored(rel, false) && !is_tracked(rel)) continue;
      // read what stands: the same size and time before and after the read, or it is read again
      aios::Result<ByteBuffer> bytes(aios::Status::NotFound, "");
      bool stable = false;
      for (int tries = 0; tries < 4 && !stable; ++tries) {
        auto st = h.stat(top + "/" + rel);
        if (!st.ok() || st->is_dir || (long long)st->size > journal_max) break;
        bytes = h.read_file(top + "/" + rel);
        auto again = h.stat(top + "/" + rel);
        stable = bytes.ok() && again.ok() && again->size == st->size && again->mtime_ns == st->mtime_ns && bytes->size() == st->size;
        if (!stable) sleep_ms(5);
      }
      if (!stable) continue;
      auto oid = c.repo->write_object("blob", ByteSpan(bytes->data(), bytes->size()));
      if (!oid.ok()) continue;
      auto it = last_blob.find(rel);
      if (it != last_blob.end() && it->second == *oid) continue;   // saved again, unchanged
      last_blob[rel] = *oid;
      lines += std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) +
               " " + *oid + " " + rel + "\n";
      ++journaled;
    }
    saved.clear();
    if (lines.empty()) return;
    (void)h.append_file(c.repo->git_dir() + "/aios-journal", ByteSpan((const uint8_t*)lines.data(), lines.size()));
  }

  void snapshot_now() {
    if (self.empty()) return;
    hal::ProcessSpawn spec;
    spec.argv = {self, "change", "snapshot", "--auto"};
    spec.working_dir = top;
    spec.env = {{"AIOS_GIT_LOCK_TRY", "1"}};
    auto p = hal::spawn_process(spec);
    if (p.ok()) child = *p;
  }
  void reap() {
    if (!child) return;
    auto st = hal::process_status(*child);
    if (!st.ok() || st->running) return;
    hal::release_process(*child);
    child.reset();
    if (st->exited && st->exit_code == 0) ++snapshots;
    // another command held the repository: it snapshotted what it saw; look again once it is done
    if (st->exited && st->exit_code == 75 && due < 0) due = now_ms() + delay_ms;
  }

  static bool in_git_dir(const std::string& rel) { return rel == ".git" || rel.rfind(".git/", 0) == 0; }

  void note(const std::string& rel) {
    changes.push_back({seq, rel});
    // keep the last million; anything older answers "everything"
    if (changes.size() > 1000000) {
      size_t drop = changes.size() / 2;
      floor = changes[drop].first;
      changes.erase(changes.begin(), changes.begin() + (long)drop);
    }
  }

  void overflowed() {
    // events were lost: every token from before is worth nothing
    epoch += "+";
    changes.clear();
    floor = ++seq;
  }

  // One batch of events: one step of the sequence. Returns whether anything in the work tree changed.
  bool take(const std::vector<WatchEvent>& evs) {
    bool any = false;
    for (const auto& e : evs) {
      if (in_git_dir(e.rel)) continue;
      if (!any) { ++seq; any = true; }
      note(e.rel);
      if (journal_on && e.saved) saved.insert(e.rel);
#if defined(__linux__)
      if (e.new_dir) watch_tree(e.rel);
#endif
    }
    return any;
  }

  // ---- the watchers ---------------------------------------------------------------------------------------------
#if defined(__linux__)
  // Every directory is watched but .git's: an ignored one too, since a file in it can still be tracked (added with
  // -f) and an ignore rule can change. When the kernel has no watch left to give (fs.inotify.max_user_watches), every
  // answer is "everything" from then on: slower, never wrong.
  int ino = -1;
  std::unordered_map<int, std::string> wd_dir;   // watch -> directory (relative, "" the top)

  bool start_watching() {
    watcher_name = "inotify";
    ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino < 0) { c.err << "fatal: inotify: " << std::strerror(errno) << "\n"; return false; }
    watch_tree("");
    return true;
  }
  void stop_watching() { if (ino >= 0) ::close(ino); }

  void watch_tree(const std::string& rel) {
    std::string full = rel.empty() ? top : top + "/" + rel;
    int wd = inotify_add_watch(ino, full.c_str(),
                               IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO | IN_CREATE | IN_DELETE |
                                   IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_EXCL_UNLINK);
    if (wd < 0) {
      if (errno == ENOSPC) watches_exhausted = true;
      return;
    }
    wd_dir[wd] = rel;
    watching = wd_dir.size();
    DIR* d = ::opendir(full.c_str());
    if (!d) return;
    while (dirent* e = ::readdir(d)) {
      std::string n = e->d_name;
      if (n == "." || n == "..") continue;
      std::string r = rel.empty() ? n : rel + "/" + n;
      bool is_dir = e->d_type == DT_DIR;
      if (e->d_type == DT_UNKNOWN) { struct stat st; is_dir = ::lstat((top + "/" + r).c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
      if (!is_dir || in_git_dir(r)) continue;
      watch_tree(r);
    }
    ::closedir(d);
  }

  // What the kernel has queued, as one batch.
  bool drain() {
    alignas(inotify_event) char buf[1 << 16];
    std::vector<WatchEvent> evs;
    while (true) {
      ssize_t n = ::read(ino, buf, sizeof buf);
      if (n <= 0) break;
      for (char* p = buf; p < buf + n;) {
        auto* ev = (inotify_event*)p;
        p += sizeof(inotify_event) + ev->len;
        if (ev->mask & IN_Q_OVERFLOW) { overflowed(); continue; }
        auto it = wd_dir.find(ev->wd);
        if (it == wd_dir.end()) continue;
        std::string dir = it->second;
        if (ev->mask & IN_IGNORED) { wd_dir.erase(it); watching = wd_dir.size(); continue; }
        if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) { evs.push_back({dir}); continue; }
        if (!ev->len) continue;
        std::string name = ev->name;
        WatchEvent w;
        w.rel = dir.empty() ? name : dir + "/" + name;
        w.saved = !(ev->mask & IN_ISDIR) && (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO));
        w.new_dir = (ev->mask & IN_ISDIR) && (ev->mask & (IN_CREATE | IN_MOVED_TO));
        evs.push_back(std::move(w));
      }
    }
    return take(evs);
  }
#else
  // FSEvents and ReadDirectoryChangesW watch the whole tree from one place and report on a thread of their own; the
  // loop takes what they queued.
  void queue(std::vector<WatchEvent> evs, bool overflow) {
    std::lock_guard<std::mutex> g(mu);
    if (overflow) pending_overflow = true;
    for (auto& e : evs) pending.push_back(std::move(e));
  }
  bool drain() {
    std::vector<WatchEvent> evs;
    bool over = false;
    {
      std::lock_guard<std::mutex> g(mu);
      evs.swap(pending);
      over = pending_overflow;
      pending_overflow = false;
    }
    if (over) overflowed();
    return take(evs);
  }
  bool has_pending() {
    std::lock_guard<std::mutex> g(mu);
    return !pending.empty() || pending_overflow;
  }
#endif

#if defined(__APPLE__)
  FSEventStreamRef stream = nullptr;
  dispatch_queue_t dq = nullptr;
  std::string real_top;   // FSEvents names paths as the kernel resolves them (/private/var/..., not /var/...)

  // The path relative to the top, or nullopt when it is not under it.
  std::optional<std::string> relative(const std::string& p) const {
    for (const std::string* base : {&real_top, &top}) {
      if (p == *base) return std::string();
      if (p.size() > base->size() && p.compare(0, base->size(), *base) == 0 && p[base->size()] == '/') return p.substr(base->size() + 1);
    }
    return std::nullopt;
  }

  static void fsevents_callback(ConstFSEventStreamRef, void* info, size_t n, void* paths, const FSEventStreamEventFlags flags[],
                                const FSEventStreamEventId[]) {
    auto* d = static_cast<Daemon*>(info);
    char** ps = static_cast<char**>(paths);
    std::vector<WatchEvent> evs;
    bool over = false;
    for (size_t i = 0; i < n; ++i) {
      const FSEventStreamEventFlags f = flags[i];
      if (f & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped)) {
        over = true;
        continue;
      }
      auto rel = d->relative(ps[i]);
      if (!rel) continue;
      WatchEvent w;
      w.rel = *rel;
      w.saved = (f & kFSEventStreamEventFlagItemIsFile) &&
                (f & (kFSEventStreamEventFlagItemModified | kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemRenamed));
      evs.push_back(std::move(w));
    }
    d->queue(std::move(evs), over);
  }

  bool start_watching() {
    watcher_name = "FSEvents";
    char real[PATH_MAX];
    real_top = ::realpath(top.c_str(), real) ? std::string(real) : top;
    CFStringRef path = CFStringCreateWithCString(nullptr, real_top.c_str(), kCFStringEncodingUTF8);
    CFArrayRef paths = CFArrayCreate(nullptr, (const void**)&path, 1, &kCFTypeArrayCallBacks);
    FSEventStreamContext ctx{0, this, nullptr, nullptr, nullptr};
    stream = FSEventStreamCreate(nullptr, &Daemon::fsevents_callback, &ctx, paths, kFSEventStreamEventIdSinceNow, 0.01,
                                 kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer | kFSEventStreamCreateFlagWatchRoot);
    CFRelease(paths);
    CFRelease(path);
    if (!stream) { c.err << "fatal: FSEvents: cannot watch " << top << "\n"; return false; }
    dq = dispatch_queue_create("aios.fsmonitor", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(stream, dq);
    if (!FSEventStreamStart(stream)) { c.err << "fatal: FSEvents: the stream did not start\n"; return false; }
    watching = 1;
    return true;
  }
  void stop_watching() {
    if (!stream) return;
    FSEventStreamStop(stream);
    FSEventStreamInvalidate(stream);
    FSEventStreamRelease(stream);
    stream = nullptr;
    if (dq) dispatch_release(dq);
    dq = nullptr;
  }
#endif

#if defined(_WIN32)
  HANDLE dir = INVALID_HANDLE_VALUE;
  std::thread reader;

  static std::string narrow(const wchar_t* s, size_t n) {
    int m = ::WideCharToMultiByte(CP_UTF8, 0, s, (int)n, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)m, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s, (int)n, out.data(), m, nullptr, nullptr);
    return out;
  }

  bool start_watching() {
    watcher_name = "ReadDirectoryChangesW";
    dir = ::CreateFileW(fs_path(top).c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (dir == INVALID_HANDLE_VALUE) { c.err << "fatal: cannot watch " << top << " (error " << ::GetLastError() << ")\n"; return false; }
    watching = 1;
    reader = std::thread([this] {
      std::vector<DWORD> buf(64 * 1024 / sizeof(DWORD));
      while (!stopping) {
        DWORD got = 0;
        BOOL ok = ::ReadDirectoryChangesW(dir, buf.data(), (DWORD)(buf.size() * sizeof(DWORD)), TRUE,
                                          FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                              FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_ATTRIBUTES,
                                          &got, nullptr, nullptr);
        if (!ok) { if (!stopping) queue({}, true); break; }   // the handle went away (or was cancelled to stop)
        if (got == 0) { queue({}, true); continue; }          // more changed than the buffer held: events were lost
        std::vector<WatchEvent> evs;
        auto* p = reinterpret_cast<const uint8_t*>(buf.data());
        for (;;) {
          auto* fi = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
          std::string rel = narrow(fi->FileName, fi->FileNameLength / sizeof(wchar_t));
          std::replace(rel.begin(), rel.end(), '\\', '/');
          WatchEvent w;
          w.rel = rel;
          w.saved = fi->Action == FILE_ACTION_ADDED || fi->Action == FILE_ACTION_MODIFIED || fi->Action == FILE_ACTION_RENAMED_NEW_NAME;
          evs.push_back(std::move(w));
          if (!fi->NextEntryOffset) break;
          p += fi->NextEntryOffset;
        }
        queue(std::move(evs), false);
      }
    });
    return true;
  }
  void stop_watching() {
    stopping = true;
    if (dir != INVALID_HANDLE_VALUE) ::CancelIoEx(dir, nullptr);
    if (reader.joinable()) reader.join();
    if (dir != INVALID_HANDLE_VALUE) ::CloseHandle(dir);
    dir = INVALID_HANDLE_VALUE;
  }
#endif

  std::string answer(const std::string& req) {
    std::string token = "aios:" + epoch + ":" + std::to_string(seq);
    if (req.rfind("status", 0) == 0) {
      return "watching " + top + "\ntoken " + token + "\nwatches " + std::to_string(watching) + " (" + watcher_name + ")" +
             (watches_exhausted ? " (the kernel's limit was reached: every command looks at everything)" : "") + "\nchanges " +
             std::to_string(changes.size()) + "\nsnapshots " +
             (auto_snapshot ? std::to_string(snapshots) + " (of @, " + std::to_string(delay_ms) + " ms after edits settle)"
                            : std::string("off (change.autoSnapshot)")) + "\njournal " +
             (journal_on ? std::to_string(journaled) + " saved versions kept (git journal)" : std::string("off")) + "\n";
    }
    if (req.rfind("since ", 0) != 0) return "error unknown request\n";
    std::string t = req.substr(6);
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
    std::string head = "token " + token + "\n";
    std::string prefix = "aios:" + epoch + ":";
    if (watches_exhausted || t.rfind(prefix, 0) != 0) return head + "everything\n";
    uint64_t since = std::strtoull(t.c_str() + prefix.size(), nullptr, 10);
    if (since < floor || since > seq) return head + "everything\n";
    std::set<std::string> paths;
    auto it = std::lower_bound(changes.begin(), changes.end(), std::make_pair(since + 1, std::string()));
    for (; it != changes.end(); ++it) paths.insert(it->second);
    std::string out = head + "paths\n";
    for (auto& p : paths) { out += p; out.push_back('\0'); }
    return out;
  }

  // Waits up to `timeout` ms for the watcher or a command: bit 1 the watcher has events, bit 2 a command connected.
  int wait(int timeout) {
#if defined(__linux__)
    pollfd fds[2] = {{ino, POLLIN, 0}, {lsn, POLLIN, 0}};
    int r = ::poll(fds, 2, timeout);
    if (r <= 0) return 0;
    return ((fds[0].revents & POLLIN) ? 1 : 0) | ((fds[1].revents & POLLIN) ? 2 : 0);
#else
    // the watcher's thread cannot wake select(): look at its queue at least every 20 ms
    timeout = std::min(timeout, 20);
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(lsn, &rd);
    timeval tv{timeout / 1000, (timeout % 1000) * 1000};
    int r = ::select((int)lsn + 1, &rd, nullptr, nullptr, &tv);
    return (has_pending() ? 1 : 0) | ((r > 0 && FD_ISSET(lsn, &rd)) ? 2 : 0);
#endif
  }

  int run() {
    std::random_device rd;
    epoch = std::to_string(rd()) + std::to_string(rd());
    auto_snapshot = c.config.get_bool("change.autosnapshot", true);
    journal_on = c.config.get_bool("change.enabled", false) && c.config.get_bool("change.journal", true);
    journal_max = c.config.get_int("change.journalmaxsize", 8 << 20);
    delay_ms = (long)std::max(0LL, c.config.get_int("change.snapshotdelay", 300));
    self = self_path();
    if (self.empty()) auto_snapshot = false;
    if (!start_watching()) return 1;
    std::error_code ec;
    std::filesystem::remove(fs_path(sock), ec);
    lsn = unix_socket();
    sockaddr_un a = address(sock);
    if (lsn == kNoSock || ::bind(lsn, (sockaddr*)&a, sizeof a) != 0 || ::listen(lsn, 64) != 0) {
      c.err << "fatal: cannot listen on " << sock << "\n";
      stop_watching();
      return 1;
    }
    for (;;) {
      int timeout = 60000;
      if (due >= 0) timeout = (int)std::max<int64_t>(0, due - now_ms());
      if (journal_due >= 0) timeout = std::min(timeout, (int)std::max<int64_t>(0, journal_due - now_ms()));
      if (child) timeout = std::min(timeout, 20);
      int ready = wait(timeout);
      reap();
      if (ready & 1) {
        if (drain() && auto_snapshot) due = now_ms() + delay_ms;   // edits: snapshot once they settle
        if (!saved.empty()) journal_due = now_ms() + 25;           // saves: kept once they stand still
      }
      if (journal_due >= 0 && now_ms() >= journal_due) {
        journal_due = -1;
        journal();
      }
      if (due >= 0 && !child && now_ms() >= due) {
        due = -1;
        snapshot_now();
      }
      if (ready & 2) {
        sock_t fd = ::accept(lsn, nullptr, nullptr);
        if (fd == kNoSock) continue;
#if defined(__APPLE__)
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        std::string req = recv_all(fd);
        drain();   // what the watcher queued before this question was asked is part of the answer
        if (req.rfind("quit", 0) == 0) {
          send_all(fd, "bye\n");
          close_sock(fd);
          break;
        }
        send_all(fd, answer(req));
        close_sock(fd);
      }
      // the work tree is gone: nothing left to watch
      if (!std::filesystem::exists(fs_path(top), ec)) break;
    }
    close_sock(lsn);
    std::filesystem::remove(fs_path(sock), ec);
    stop_watching();
    if (child) hal::release_process(*child);
    return 0;
  }
};

#endif  // AIOS_FSMONITOR

}  // namespace

// The hint for this command (see the top of this file). Called when a repository is opened with core.fsmonitor on.
void fsmonitor_prepare(Ctx& c) {
#ifdef AIOS_FSMONITOR
  if (!c.repo || c.top.empty()) return;
  auto mode = c.config.get("core.fsmonitor");
  if (!mode) return;
  bool ok = false;
  bool on = Config::parse_bool(*mode, true, &ok);
  if (!(ok && on)) return;
  std::string sock = socket_path(c);
  auto& h = c.host();
  std::string state_path = c.repo->git_dir() + "/aios-fsmonitor";
  std::string old_token;
  std::set<std::string> dirty_old;
  if (auto f = h.read_file(state_path); f.ok()) {
    std::string t = str_of(*f);
    size_t nl = t.find('\n');
    old_token = t.substr(0, nl);
    if (nl != std::string::npos) {
      std::string rest = t.substr(nl + 1);
      size_t i = 0;
      while (i < rest.size()) {
        size_t z = rest.find('\0', i);
        if (z == std::string::npos) z = rest.size();
        if (z > i) dirty_old.insert(rest.substr(i, z - i));
        i = z + 1;
      }
    }
  }
  auto reply = ask(sock, "since " + (old_token.empty() ? std::string("none") : old_token));
  if (!reply || reply->rfind("token ", 0) != 0) return;   // no daemon: look at everything
  size_t nl = reply->find('\n');
  std::string new_token = reply->substr(6, nl - 6);
  std::string body = reply->substr(nl + 1);
  bool everything = body.rfind("everything", 0) == 0 || old_token.empty();
  auto cand = std::make_shared<std::set<std::string>>();
  if (!everything && body.rfind("paths\n", 0) == 0) {
    std::string rest = body.substr(6);
    size_t i = 0;
    while (i < rest.size()) {
      size_t z = rest.find('\0', i);
      if (z == std::string::npos) z = rest.size();
      std::string p = rest.substr(i, z - i);
      if (!p.empty()) {
        // an ignore file changed: what counts as untracked may have changed anywhere
        if (p == ".gitignore" || (p.size() > 11 && p.compare(p.size() - 11, 11, "/.gitignore") == 0)) everything = true;
        cand->insert(p);
      }
      i = z + 1;
    }
    cand->insert(dirty_old.begin(), dirty_old.end());
  }
  if (everything) c.repo->set_watch_hint(nullptr);
  else c.repo->set_watch_hint(cand);
  // what differs from the index now, recorded with the token that came before looking
  std::set<std::string> dirty;
  if (auto ch = c.repo->worktree_changes(); ch.ok()) for (auto& e : *ch) dirty.insert(e.path);
  if (auto un = c.repo->untracked(false); un.ok()) dirty.insert(un->begin(), un->end());
  std::string out = new_token + "\n";
  for (auto& p : dirty) { out += p; out.push_back('\0'); }
  (void)h.write_file(state_path, ByteSpan((const uint8_t*)out.data(), out.size()));
  if (everything) {
    auto exact = std::make_shared<std::set<std::string>>(dirty);
    c.repo->set_watch_hint(exact);
  }
#else
  (void)c;
#endif
}

int cmd_fsmonitor_daemon(Ctx& c) {
  std::string sub = c.args.size() > 1 ? c.args[1] : "";
  c.open_repo();
#ifdef AIOS_FSMONITOR
  std::string sock = socket_path(c);
  if (sub == "status") {
    auto r = ask(sock, "status");
    if (!r) { c.out << "fsmonitor-daemon is not watching '" << c.top << "'\n"; return 1; }
    c.out << *r;
    return 0;
  }
  if (sub == "stop") {
    auto r = ask(sock, "quit");
    if (!r) { c.err << "fsmonitor-daemon is not running\n"; return 1; }
    (void)c.host().remove(c.repo->git_dir() + "/aios-fsmonitor");
    return 0;
  }
  if (sub == "run" || sub == "start") {
    if (ask(sock, "status")) { c.out << "fsmonitor-daemon is already watching '" << c.top << "'\n"; return 0; }
    if (!c.config.get("core.fsmonitor")) (void)c.repo->set_config("core.fsmonitor", "true");
    if (sub == "start") {
      c.out.flush();
#if defined(_WIN32)
      // no fork: the daemon is this program again, `run`, in a process of its own that outlives this one
      std::wstring cmd = L"\"" + fs_path(self_path()).wstring() + L"\" fsmonitor--daemon run";
      STARTUPINFOW si{};
      si.cb = sizeof si;
      PROCESS_INFORMATION pi{};
      std::wstring cwd = fs_path(c.top).wstring();
      if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW,
                            nullptr, cwd.c_str(), &si, &pi))
        die(c, "fsmonitor-daemon: cannot start a process (error " + std::to_string(::GetLastError()) + ")");
      ::CloseHandle(pi.hThread);
      ::CloseHandle(pi.hProcess);
#else
      pid_t pid = ::fork();
      if (pid < 0) die(c, std::string("fork: ") + std::strerror(errno));
      if (pid == 0) {
        ::setsid();
        int nul = ::open("/dev/null", O_RDWR);
        if (nul >= 0) { ::dup2(nul, 0); ::dup2(nul, 1); ::dup2(nul, 2); }
        ::signal(SIGHUP, SIG_IGN);
        ::signal(SIGPIPE, SIG_IGN);
        Daemon d(c, c.top, sock);
        ::_exit(d.run());
      }
#endif
      for (int i = 0; i < 200; ++i) {   // until it answers (up to 10 s)
        if (ask(sock, "status")) { c.out << "fsmonitor-daemon is watching '" << c.top << "'\n"; return 0; }
        sleep_ms(50);
      }
      die(c, "fsmonitor-daemon did not start");
    }
#if !defined(_WIN32)
    ::signal(SIGPIPE, SIG_IGN);
#endif
    Daemon d(c, c.top, sock);
    return d.run();
  }
  usage(c, "usage: git fsmonitor--daemon (start | run | stop | status)");
#else
  (void)sub;
  die(c, "fsmonitor--daemon: file watching is implemented on Linux, macOS and Windows only; commands look at every file here");
#endif
}

}  // namespace aios_git
