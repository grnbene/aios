// An intelligence that lives in another process. The adapter speaks the frame
// protocol over the child's stdio; the kernel sees only an Intelligence. An
// out-of-kernel mind contributes no kernel-schedulable computation — its
// computation happens on its own substrate — so `think` yields no graph and
// the peer's reply is carried into `act` as a thought.
#include "aios/intelligence/intelligence.hpp"
#include "aios/intelligence/frame_protocol.hpp"
#include "aios/kernel/surface.hpp"
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <map>

namespace aios {
namespace {

std::wstring to_wide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? (size_t)n : 0, L'\0');
  if (n > 0) { ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n); w.resize((size_t)n - 1); }
  return w;
}
std::wstring win_quote(const std::string& a) {
  const std::wstring w = to_wide(a);
  if (!w.empty() && w.find_first_of(L" \t\"") == std::wstring::npos) return w;
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (wchar_t c : w) {
    if (c == L'\\') { ++backslashes; out += c; continue; }
    if (c == L'"') { out.append(backslashes + 1, L'\\'); out += c; backslashes = 0; continue; }
    backslashes = 0;
    out += c;
  }
  out.append(backslashes, L'\\');
  out += L'"';
  return out;
}
std::wstring build_cmdline(const std::vector<std::string>& argv) {
  std::wstring line;
  for (const auto& a : argv) { if (!line.empty()) line += L' '; line += win_quote(a); }
  return line;
}

// This process's environment with each "NAME=value" of `extra` set over it, as a CreateProcess
// environment block — the Windows equivalent of merged_environment+execvp's envp on the POSIX side.
std::vector<wchar_t> build_env_block(const std::vector<std::string>& extra) {
  std::map<std::wstring, std::wstring, bool (*)(const std::wstring&, const std::wstring&)> vars(
      [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
  if (LPWCH block = ::GetEnvironmentStringsW()) {
    for (LPWCH p = block; *p; ) {
      const size_t len = wcslen(p);
      const wchar_t* eq = wcschr(p, L'=');
      if (eq && eq != p) vars[std::wstring(p, eq - p)] = std::wstring(eq + 1);
      p += len + 1;
    }
    ::FreeEnvironmentStringsW(block);
  }
  for (const auto& kv : extra) {
    const auto eq = kv.find('=');
    if (eq != std::string::npos) vars[to_wide(kv.substr(0, eq))] = to_wide(kv.substr(eq + 1));
  }
  std::vector<wchar_t> out;
  for (const auto& [k, v] : vars) {
    std::wstring line = k + L"=" + v;
    out.insert(out.end(), line.begin(), line.end());
    out.push_back(L'\0');
  }
  out.push_back(L'\0');
  return out;
}

// PeekNamedPipe-polled read/write over an anonymous pipe HANDLE: the same
// "blocking with a timeout" contract frame_protocol.hpp's fd-based
// write_frame/read_frame give the POSIX build (there via poll()), rewritten
// here because those take an `int fd`, not a HANDLE.
bool write_all(HANDLE h, const uint8_t* p, size_t n) {
  while (n) {
    DWORD put = 0;
    if (!::WriteFile(h, p, (DWORD)std::min<size_t>(n, 1u << 20), &put, nullptr) || put == 0) return false;
    p += put; n -= put;
  }
  return true;
}
bool read_all(HANDLE h, uint8_t* p, size_t n, int timeout_ms) {
  int waited = 0;
  while (n) {
    DWORD avail = 0;
    if (!::PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) return false;  // closed or broken
    if (avail == 0) {
      if (timeout_ms >= 0 && waited >= timeout_ms) return false;
      ::Sleep(5);
      waited += 5;
      continue;
    }
    DWORD got = 0;
    if (!::ReadFile(h, p, (DWORD)std::min<size_t>(n, avail), &got, nullptr) || got == 0) return false;
    p += got; n -= got; waited = 0;
  }
  return true;
}
bool write_frame(HANDLE h, frame::Type t, const std::vector<uint8_t>& payload) {
  uint8_t hdr[9];
  std::memcpy(hdr, frame::MAGIC, 4);
  hdr[4] = (uint8_t)t;
  const uint32_t len = (uint32_t)payload.size();
  for (int i = 0; i < 4; ++i) hdr[5 + i] = uint8_t(len >> (8 * i));
  return write_all(h, hdr, 9) && (payload.empty() || write_all(h, payload.data(), payload.size()));
}
bool read_frame(HANDLE h, frame::Type& t, std::vector<uint8_t>& payload, int timeout_ms) {
  uint8_t hdr[9];
  if (!read_all(h, hdr, 9, timeout_ms)) return false;
  if (std::memcmp(hdr, frame::MAGIC, 4) != 0) return false;
  t = (frame::Type)hdr[4];
  uint32_t len = 0;
  for (int i = 0; i < 4; ++i) len |= uint32_t(hdr[5 + i]) << (8 * i);
  if (len > frame::MAX_PAYLOAD) return false;
  payload.resize(len);
  return len == 0 || read_all(h, payload.data(), len, timeout_ms);
}

struct Launched { HANDLE process; HANDLE in_write; HANDLE out_read; std::string name; };

Result<Launched> launch_peer(const std::vector<std::string>& argv, const std::vector<std::string>& env, int timeout_ms) {
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE in_read = nullptr, in_write = nullptr, out_read = nullptr, out_write = nullptr;
  if (!::CreatePipe(&in_read, &in_write, &sa, 0) || !::CreatePipe(&out_read, &out_write, &sa, 0))
    return Result<Launched>(Status::IoError, "CreatePipe failed");
  // Only the ends the child inherits should be inheritable from the parent's
  // side too (CreatePipe above makes both ends of each pipe inheritable);
  // narrow it back down so a later child this process starts does not also
  // inherit our end of this one.
  ::SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
  ::SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in_read;
  si.hStdOutput = out_write;
  si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);   // untouched, same as the POSIX build leaves fd 2 alone

  std::wstring cmdline = build_cmdline(argv);
  std::vector<wchar_t> envblock = build_env_block(env);
  PROCESS_INFORMATION pi{};
  const BOOL ok = ::CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
      CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, envblock.data(), nullptr, &si, &pi);
  ::CloseHandle(in_read);
  ::CloseHandle(out_write);
  if (!ok) {
    const DWORD err = ::GetLastError();
    ::CloseHandle(in_write); ::CloseHandle(out_read);
    return Result<Launched>(Status::IoError, argv[0] + ": CreateProcess failed (error " + std::to_string(err) + ")");
  }
  ::CloseHandle(pi.hThread);

  // Handshake: the peer names itself.
  if (!write_frame(in_write, frame::Hello, {})) {
    ::CloseHandle(in_write); ::CloseHandle(out_read);
    ::TerminateProcess(pi.hProcess, 1); ::WaitForSingleObject(pi.hProcess, 5000); ::CloseHandle(pi.hProcess);
    return Result<Launched>(Status::IoError, "peer refused hello");
  }
  frame::Type t;
  ByteBuffer hello;
  if (!read_frame(out_read, t, hello, timeout_ms) || t != frame::Hello) {
    ::CloseHandle(in_write); ::CloseHandle(out_read);
    ::TerminateProcess(pi.hProcess, 1); ::WaitForSingleObject(pi.hProcess, 5000); ::CloseHandle(pi.hProcess);
    return Result<Launched>(Status::IoError, t == frame::Error ? "peer error: " + string_of(hello) : "peer did not answer hello: " + argv[0]);
  }
  return Launched{pi.hProcess, in_write, out_read, string_of(hello)};
}

class ExternalIntelligence final : public Intelligence {
 public:
  ExternalIntelligence(std::vector<std::string> argv, std::vector<std::string> env, HANDLE process, HANDLE in_write, HANDLE out_read,
                       std::string name, uint64_t timeout_ms)
    : argv_(std::move(argv)), env_(std::move(env)), process_(process), to_child_(in_write), from_child_(out_read),
      name_(std::move(name)), timeout_ms_((int)timeout_ms) {}
  ~ExternalIntelligence() override { teardown(); }
  std::string name() const override { return "external:" + name_; }

  Result<StateID> perceive(Surface& s, const StateObject& context) override {
    auto reply = exchange(frame::Perceive, context.payload);
    if (!reply.ok()) return reply.error();
    return s.create_state(StateKind::Context, *reply);
  }
  Result<GraphID> think(Surface&, const StateObject& perceived) override {
    auto reply = exchange(frame::Think, perceived.payload);
    if (!reply.ok()) return reply.error();
    thought_ = *reply;
    return GraphID{};
  }
  Result<StateID> act(Surface& s, const StateObject& perceived, const ComputeGraph*, const ExecutionReport*) override {
    ByteBuffer req;
    Writer w(req);
    w.bytes(perceived.payload);
    w.bytes(thought_);
    auto reply = exchange(frame::Act, req);
    if (!reply.ok()) return reply.error();
    return s.create_state(StateKind::Working, *reply);
  }

  // Same resync-by-respawn contract as the POSIX build's exchange(): a wrong-
  // type reply means a stale frame from a timed-out exchange is sitting ahead
  // of us in the pipe, not a corrupt channel, so this respawns onto a fresh
  // pipe and retries once rather than failing for the mind's whole lifetime.
  Result<ByteBuffer> exchange(frame::Type t, const ByteBuffer& payload) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (faulted_) { auto r = respawn(); if (!r.ok()) return Result<ByteBuffer>(r.status(), "reviving mind bridge: " + r.error().message); }
      if (!write_frame(to_child_, t, payload)) { faulted_ = true; return Result<ByteBuffer>(Status::IoError, "peer closed its input"); }
      frame::Type rt;
      ByteBuffer reply;
      if (!read_frame(from_child_, rt, reply, timeout_ms_)) { faulted_ = true; return Result<ByteBuffer>(Status::IoError, "no reply from peer within " + std::to_string(timeout_ms_) + " ms (or peer exited)"); }
      if (rt == frame::Error) return Result<ByteBuffer>(Status::IoError, "peer error: " + string_of(reply));
      if (rt == t) return reply;
      faulted_ = true;
    }
    return Result<ByteBuffer>(Status::Corrupt, "peer replied with the wrong frame type (channel could not be resynchronised)");
  }

 private:
  void teardown() {
    if (to_child_) { ::CloseHandle(to_child_); to_child_ = nullptr; }
    if (from_child_) { ::CloseHandle(from_child_); from_child_ = nullptr; }
    if (process_) {
      if (::WaitForSingleObject(process_, 1000) == WAIT_TIMEOUT) ::TerminateProcess(process_, 1);
      ::WaitForSingleObject(process_, 5000);
      ::CloseHandle(process_);
      process_ = nullptr;
    }
  }
  Result<void> respawn() {
    teardown();
    auto l = launch_peer(argv_, env_, timeout_ms_);
    if (!l.ok()) return Error{l.status(), l.error().message};
    process_ = l->process; to_child_ = l->in_write; from_child_ = l->out_read;
    faulted_ = false;
    return Ok();
  }

  std::vector<std::string> argv_;
  std::vector<std::string> env_;
  HANDLE process_;
  HANDLE to_child_, from_child_;
  std::string name_;
  int timeout_ms_;
  bool faulted_ = false;
  ByteBuffer thought_;
};

}  // namespace

Result<std::unique_ptr<Intelligence>> spawn_external_intelligence(const std::vector<std::string>& argv, uint64_t timeout_ms) {
  return spawn_external_intelligence(argv, std::vector<std::string>{}, timeout_ms);
}

Result<std::unique_ptr<Intelligence>> spawn_external_intelligence(const std::vector<std::string>& argv, const std::vector<std::string>& env, uint64_t timeout_ms) {
  if (argv.empty()) return Result<std::unique_ptr<Intelligence>>(Status::Invalid, "no command");
  if (const char* e = std::getenv("AIOS_BRIDGE_TIMEOUT_MS")) { char* end = nullptr; unsigned long v = std::strtoul(e, &end, 10); if (end != e && v > 0) timeout_ms = v; }
  auto l = launch_peer(argv, env, (int)timeout_ms);
  if (!l.ok()) return Result<std::unique_ptr<Intelligence>>(l.status(), l.error().message);
  return std::unique_ptr<Intelligence>(new ExternalIntelligence(argv, env, l->process, l->in_write, l->out_read, l->name, timeout_ms));
}

}  // namespace aios

#else  // !defined(_WIN32)

#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#else
extern char** environ;
#endif

namespace aios {

namespace frame {
static bool write_all(int fd, const uint8_t* p, size_t n) {
  while (n) { ssize_t w = ::write(fd, p, n); if (w < 0) { if (errno == EINTR) continue; return false; } p += w; n -= (size_t)w; }
  return true;
}
static bool read_all(int fd, uint8_t* p, size_t n, int timeout_ms) {
  while (n) {
    pollfd pf{fd, POLLIN, 0};
    int pr = ::poll(&pf, 1, timeout_ms);
    if (pr <= 0) return false;
    ssize_t r = ::read(fd, p, n);
    if (r <= 0) { if (r < 0 && errno == EINTR) continue; return false; }
    p += r; n -= (size_t)r;
  }
  return true;
}
bool write_frame(int fd, Type t, const std::vector<uint8_t>& payload) {
  uint8_t hdr[9];
  std::memcpy(hdr, MAGIC, 4);
  hdr[4] = t;
  uint32_t len = (uint32_t)payload.size();
  for (int i = 0; i < 4; ++i) hdr[5 + i] = uint8_t(len >> (8 * i));
  return write_all(fd, hdr, 9) && (payload.empty() || write_all(fd, payload.data(), payload.size()));
}
bool read_frame(int fd, Type& t, std::vector<uint8_t>& payload, int timeout_ms) {
  uint8_t hdr[9];
  if (!read_all(fd, hdr, 9, timeout_ms)) return false;
  if (std::memcmp(hdr, MAGIC, 4) != 0) return false;
  t = (Type)hdr[4];
  uint32_t len = 0;
  for (int i = 0; i < 4; ++i) len |= uint32_t(hdr[5 + i]) << (8 * i);
  if (len > MAX_PAYLOAD) return false;
  payload.resize(len);
  return len == 0 || read_all(fd, payload.data(), len, timeout_ms);
}
}  // namespace frame

namespace {

char** process_environment() {
#if defined(__APPLE__)
  return *_NSGetEnviron();
#else
  return environ;
#endif
}
void set_process_environment(char** envp) {
#if defined(__APPLE__)
  *_NSGetEnviron() = envp;
#else
  environ = envp;
#endif
}

// This process's environment with each "NAME=value" of `extra` set over it, as the strings an envp points at.
// Built before the fork: the child, in a process with other threads, may only do async-signal-safe work.
std::vector<std::string> merged_environment(const std::vector<std::string>& extra) {
  std::vector<std::string> out;
  auto name_of = [](const std::string& kv) { return kv.substr(0, kv.find('=')); };
  for (char** e = process_environment(); e && *e; ++e) {
    const std::string kv = *e;
    bool replaced = false;
    for (const auto& x : extra) if (name_of(x) == name_of(kv)) { replaced = true; break; }
    if (!replaced) out.push_back(kv);
  }
  for (const auto& x : extra) if (x.find('=') != std::string::npos) out.push_back(x);
  return out;
}

// Fork the child, wire its stdio to a pair of pipes, and complete the Hello
// handshake so the peer names itself. Used both for the first spawn and for a
// transparent respawn after the channel faults — a respawn is how the mind
// recovers a clean pipe instead of being poisoned for life by one late reply.
struct Launched { pid_t pid; int in_fd; int out_fd; std::string name; };
Result<Launched> launch_peer(const std::vector<std::string>& argv, const std::vector<std::string>& env, int timeout_ms) {
  const std::vector<std::string> envs = merged_environment(env);
  std::vector<char*> envp;
  for (const auto& e : envs) envp.push_back(const_cast<char*>(e.c_str()));
  envp.push_back(nullptr);
  std::vector<char*> args;
  for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  int to_child[2], from_child[2];
  if (::pipe(to_child) != 0 || ::pipe(from_child) != 0) return Result<Launched>(Status::IoError, strerror(errno));
  pid_t pid = ::fork();
  if (pid < 0) { std::string e = strerror(errno); ::close(to_child[0]); ::close(to_child[1]); ::close(from_child[0]); ::close(from_child[1]); return Result<Launched>(Status::IoError, e); }
  if (pid == 0) {
    ::dup2(to_child[0], 0);
    ::dup2(from_child[1], 1);
    ::close(to_child[0]); ::close(to_child[1]); ::close(from_child[0]); ::close(from_child[1]);
    if (!env.empty()) set_process_environment(envp.data());
    ::execvp(args[0], args.data());
    const char* msg = "exec failed\n";
    ssize_t ignored = ::write(2, msg, strlen(msg)); (void)ignored;
    _exit(127);
  }
  ::close(to_child[0]);
  ::close(from_child[1]);
  // Handshake: the peer names itself.
  if (!frame::write_frame(to_child[1], frame::Hello, {})) {
    ::close(to_child[1]); ::close(from_child[0]);
    int st = 0; ::kill(pid, SIGTERM); ::waitpid(pid, &st, 0);
    return Result<Launched>(Status::IoError, "peer refused hello");
  }
  frame::Type t;
  ByteBuffer hello;
  if (!frame::read_frame(from_child[0], t, hello, timeout_ms) || t != frame::Hello) {
    ::close(to_child[1]); ::close(from_child[0]);
    int st = 0; ::waitpid(pid, &st, WNOHANG);
    ::kill(pid, SIGTERM); ::waitpid(pid, &st, 0);
    return Result<Launched>(Status::IoError, t == frame::Error ? "peer error: " + string_of(hello) : "peer did not answer hello: " + argv[0]);
  }
  return Launched{pid, to_child[1], from_child[0], string_of(hello)};
}

class ExternalIntelligence final : public Intelligence {
 public:
  ExternalIntelligence(std::vector<std::string> argv, std::vector<std::string> env, pid_t pid, int in_fd, int out_fd, std::string name, uint64_t timeout_ms)
    : argv_(std::move(argv)), env_(std::move(env)), pid_(pid), to_child_(in_fd), from_child_(out_fd), name_(std::move(name)), timeout_ms_((int)timeout_ms) {}
  ~ExternalIntelligence() override { teardown(); }
  std::string name() const override { return "external:" + name_; }

  Result<StateID> perceive(Surface& s, const StateObject& context) override {
    auto reply = exchange(frame::Perceive, context.payload);
    if (!reply.ok()) return reply.error();
    return s.create_state(StateKind::Context, *reply);
  }
  Result<GraphID> think(Surface&, const StateObject& perceived) override {
    auto reply = exchange(frame::Think, perceived.payload);
    if (!reply.ok()) return reply.error();
    thought_ = *reply;
    return GraphID{};
  }
  Result<StateID> act(Surface& s, const StateObject& perceived, const ComputeGraph*, const ExecutionReport*) override {
    ByteBuffer req;
    Writer w(req);
    w.bytes(perceived.payload);
    w.bytes(thought_);
    auto reply = exchange(frame::Act, req);
    if (!reply.ok()) return reply.error();
    return s.create_state(StateKind::Working, *reply);
  }

  // One request, one reply, over a persistent pipe — but the pipe carries state
  // between requests, and that is the whole difficulty. If an earlier exchange
  // gave up on its reply before the peer produced it (a model call slower than
  // the timeout, or an abandoned cycle), that reply is now sitting in the pipe
  // ahead of us. A naive read then returns the STALE frame, its type no longer
  // matches what we asked for, and — because every later read stays one frame
  // behind — the mind is broken for the rest of its life: "it worked once, then
  // every question says wrong frame type."
  //
  // So a wrong-type reply is not a fatal corruption; it is the symptom of a
  // dirty pipe. Respawn the peer onto a fresh, empty pipe and try once more.
  // The retry re-runs the request from scratch (perceive/act just echo; think
  // re-asks the model), which is exactly what we want: a clean answer instead
  // of a permanent fault. A hard channel error (peer gone, write refused) also
  // arms the respawn so the NEXT request comes up clean.
  Result<ByteBuffer> exchange(frame::Type t, const ByteBuffer& payload) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (faulted_) { auto r = respawn(); if (!r.ok()) return Result<ByteBuffer>(r.status(), "reviving mind bridge: " + r.error().message); }
      if (!frame::write_frame(to_child_, t, payload)) { faulted_ = true; return Result<ByteBuffer>(Status::IoError, "peer closed its input"); }
      frame::Type rt;
      ByteBuffer reply;
      if (!frame::read_frame(from_child_, rt, reply, timeout_ms_)) { faulted_ = true; return Result<ByteBuffer>(Status::IoError, "no reply from peer within " + std::to_string(timeout_ms_) + " ms (or peer exited)"); }
      if (rt == frame::Error) return Result<ByteBuffer>(Status::IoError, "peer error: " + string_of(reply));
      if (rt == t) return reply;
      // Wrong type: a stale reply from a prior timed-out exchange was buffered
      // ahead of us. Arm a respawn and retry onto a clean pipe.
      faulted_ = true;
    }
    return Result<ByteBuffer>(Status::Corrupt, "peer replied with the wrong frame type (channel could not be resynchronised)");
  }

 private:
  void teardown() {
    if (to_child_ >= 0) { ::close(to_child_); to_child_ = -1; }
    if (from_child_ >= 0) { ::close(from_child_); from_child_ = -1; }
    if (pid_ > 0) {
      int st = 0;
      for (int i = 0; i < 50 && ::waitpid(pid_, &st, WNOHANG) == 0; ++i) ::usleep(20000);
      if (::waitpid(pid_, &st, WNOHANG) == 0) { ::kill(pid_, SIGTERM); ::waitpid(pid_, &st, 0); }
    }
    pid_ = -1;
  }
  Result<void> respawn() {
    teardown();
    auto l = launch_peer(argv_, env_, timeout_ms_);
    if (!l.ok()) return Error{l.status(), l.error().message};
    pid_ = l->pid; to_child_ = l->in_fd; from_child_ = l->out_fd;
    faulted_ = false;
    return Ok();
  }

  std::vector<std::string> argv_;
  std::vector<std::string> env_;
  pid_t pid_;
  int to_child_, from_child_;
  std::string name_;
  int timeout_ms_;
  bool faulted_ = false;
  ByteBuffer thought_;
};

}  // namespace

Result<std::unique_ptr<Intelligence>> spawn_external_intelligence(const std::vector<std::string>& argv, uint64_t timeout_ms) {
  return spawn_external_intelligence(argv, std::vector<std::string>{}, timeout_ms);
}

Result<std::unique_ptr<Intelligence>> spawn_external_intelligence(const std::vector<std::string>& argv, const std::vector<std::string>& env, uint64_t timeout_ms) {
  if (argv.empty()) return Result<std::unique_ptr<Intelligence>>(Status::Invalid, "no command");
  // A machine can tune how long the kernel waits on a reply before it treats the
  // channel as faulted and (on the next exchange) respawns the peer. Lower it on
  // a fast local model; raise it for a slow remote one.
  if (const char* env = std::getenv("AIOS_BRIDGE_TIMEOUT_MS")) { char* end = nullptr; unsigned long v = std::strtoul(env, &end, 10); if (end != env && v > 0) timeout_ms = v; }
  auto l = launch_peer(argv, env, (int)timeout_ms);
  if (!l.ok()) return Result<std::unique_ptr<Intelligence>>(l.status(), l.error().message);
  return std::unique_ptr<Intelligence>(new ExternalIntelligence(argv, env, l->pid, l->in_fd, l->out_fd, l->name, timeout_ms));
}

}  // namespace aios

#endif  // defined(_WIN32)
