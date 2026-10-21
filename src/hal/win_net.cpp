// The network face of the Windows host. Same contract as linux_net.cpp /
// mac_net.cpp, over Winsock: WSAPoll stands in for poll(), ioctlsocket's
// FIONBIO for the O_NONBLOCK fcntl dance, and closesocket for close(). No
// io_uring equivalent exists here, so this always takes the poll path every
// other host falls back to. AF_UNIX sockets are native on Windows 10 1803+/
// Server 2019+ (afunix.h); older hosts simply fail that socket() call, which
// surfaces as the IoError net.hpp already documents for "not supported".
#include "aios/hal/net.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <windows.h>
#include <cerrno>
#include <cstring>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace aios::hal {
namespace {

void ensure_winsock() {
  static std::once_flag once;
  std::call_once(once, [] { WSADATA d; ::WSAStartup(MAKEWORD(2, 2), &d); });
}

std::string sock_error(int err) {
  char buf[256];
  const DWORD n = ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, (DWORD)err, 0, buf, sizeof buf, nullptr);
  std::string msg = n ? std::string(buf, n) : ("winsock error " + std::to_string(err));
  while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
  return msg;
}
std::string last_sock_error() { return sock_error(::WSAGetLastError()); }
void set_nonblocking(SOCKET s, bool on) { u_long mode = on ? 1 : 0; ::ioctlsocket(s, FIONBIO, &mode); }

class TcpConnection final : public Connection {
 public:
  TcpConnection(SOCKET fd, std::string peer) : fd_(fd), peer_(std::move(peer)) {
    BOOL one = TRUE;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
  }
  ~TcpConnection() override { close(); }
  bool send_all(ByteSpan d) override {
    size_t off = 0;
    while (off < d.size()) {
      const int want = (int)std::min<size_t>(d.size() - off, 1 << 20);
      const int n = ::send(fd_, (const char*)d.data() + off, want, 0);
      if (n < 0) { dead_ = true; return false; }
      off += (size_t)n;
    }
    return true;
  }
  bool recv_all(MutableByteSpan d, int timeout_ms) override {
    size_t off = 0;
    while (off < d.size()) {
      WSAPOLLFD pf{fd_, POLLIN, 0};
      const int pr = ::WSAPoll(&pf, 1, timeout_ms);
      if (pr == 0) return false;        // timeout: no data yet — NOT a drop
      if (pr < 0) { dead_ = true; return false; }
      const int want = (int)std::min<size_t>(d.size() - off, 1 << 20);
      const int n = ::recv(fd_, (char*)d.data() + off, want, 0);
      if (n == 0) { dead_ = true; return false; }   // peer closed
      if (n < 0) { dead_ = true; return false; }    // error (RST, etc.)
      off += (size_t)n;
    }
    return true;
  }
  long recv_some(MutableByteSpan d, int timeout_ms) override {
    if (d.size() == 0) return 0;
    WSAPOLLFD pf{fd_, POLLIN, 0};
    const int pr = ::WSAPoll(&pf, 1, timeout_ms);
    if (pr == 0) return 0;            // nothing yet
    if (pr < 0) { dead_ = true; return -1; }
    const int n = ::recv(fd_, (char*)d.data(), (int)std::min<size_t>(d.size(), 1 << 20), 0);
    if (n == 0) { dead_ = true; return 0; }   // closed: alive() says so
    if (n < 0) { dead_ = true; return -1; }
    return (long)n;
  }
  std::string peer() const override { return peer_; }
  bool alive() const override { return fd_ != INVALID_SOCKET && !dead_; }
  void close() override { if (fd_ != INVALID_SOCKET) { ::closesocket(fd_); fd_ = INVALID_SOCKET; } dead_ = true; }
 private:
  SOCKET fd_;
  std::string peer_;
  std::atomic<bool> dead_{false};
};

std::string addr_str(const sockaddr_in& a) { char buf[64]; ::InetNtopA(AF_INET, (void*)&a.sin_addr, buf, sizeof buf); return std::string(buf) + ":" + std::to_string(ntohs(a.sin_port)); }

class TcpListener final : public Listener {
 public:
  TcpListener(SOCKET fd, uint16_t port) : fd_(fd), port_(port) {}
  ~TcpListener() override { close(); }
  Result<std::unique_ptr<Connection>> accept(int timeout_ms) override {
    WSAPOLLFD pf{fd_, POLLIN, 0};
    const int pr = ::WSAPoll(&pf, 1, timeout_ms);
    if (pr == 0) return Result<std::unique_ptr<Connection>>(Status::Busy);
    if (pr < 0) return Result<std::unique_ptr<Connection>>(Status::IoError, last_sock_error());
    sockaddr_in a{}; int al = sizeof a;
    const SOCKET c = ::accept(fd_, (sockaddr*)&a, &al);
    if (c == INVALID_SOCKET) return Result<std::unique_ptr<Connection>>(Status::IoError, last_sock_error());
    return std::unique_ptr<Connection>(new TcpConnection(c, addr_str(a)));
  }
  uint16_t port() const override { return port_; }
  void close() override { if (fd_ != INVALID_SOCKET) { ::closesocket(fd_); fd_ = INVALID_SOCKET; } }
 private:
  SOCKET fd_;
  uint16_t port_;
};

class UnixListener final : public Listener {
 public:
  UnixListener(SOCKET fd, std::string path) : fd_(fd), path_(std::move(path)) {}
  ~UnixListener() override { close(); }
  Result<std::unique_ptr<Connection>> accept(int timeout_ms) override {
    WSAPOLLFD pf{fd_, POLLIN, 0};
    const int pr = ::WSAPoll(&pf, 1, timeout_ms);
    if (pr == 0) return Result<std::unique_ptr<Connection>>(Status::Busy);
    if (pr < 0) return Result<std::unique_ptr<Connection>>(Status::IoError, last_sock_error());
    const SOCKET c = ::accept(fd_, nullptr, nullptr);
    if (c == INVALID_SOCKET) return Result<std::unique_ptr<Connection>>(Status::IoError, last_sock_error());
    return std::unique_ptr<Connection>(new TcpConnection(c, "unix:" + path_));
  }
  uint16_t port() const override { return 0; }
  void close() override { if (fd_ != INVALID_SOCKET) { ::closesocket(fd_); fd_ = INVALID_SOCKET; ::DeleteFileA(path_.c_str()); } }
 private:
  SOCKET fd_;
  std::string path_;
};

}  // namespace

Result<std::unique_ptr<Listener>> net_listen_unix(const std::string& path) {
  ensure_winsock();
  sockaddr_un a{};
  if (path.size() >= sizeof a.sun_path) return Result<std::unique_ptr<Listener>>(Status::Invalid, "socket path too long");
  const SOCKET fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET) return Result<std::unique_ptr<Listener>>(Status::IoError, last_sock_error());
  a.sun_family = AF_UNIX;
  std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
  ::DeleteFileA(path.c_str());   // a stale socket from a previous run
  if (::bind(fd, (sockaddr*)&a, sizeof a) != 0 || ::listen(fd, 1024) != 0) {
    const auto e = last_sock_error(); ::closesocket(fd);
    return Result<std::unique_ptr<Listener>>(Status::IoError, "listen on " + path + ": " + e);
  }
  return std::unique_ptr<Listener>(new UnixListener(fd, path));
}

Result<std::unique_ptr<Listener>> net_listen(uint16_t port, bool loopback_only) {
  ensure_winsock();
  const SOCKET fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET) return Result<std::unique_ptr<Listener>>(Status::IoError, last_sock_error());
  BOOL one = TRUE; ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY); a.sin_port = htons(port);
  if (::bind(fd, (sockaddr*)&a, sizeof a) != 0 || ::listen(fd, 1024) != 0) {
    const auto e = last_sock_error(); ::closesocket(fd);
    return Result<std::unique_ptr<Listener>>(Status::IoError, "listen on " + std::to_string(port) + ": " + e);
  }
  int al = sizeof a; ::getsockname(fd, (sockaddr*)&a, &al);
  return std::unique_ptr<Listener>(new TcpListener(fd, ntohs(a.sin_port)));
}

// The Windows agent's pipe, over ReadFile/WriteFile: a pipe handle is not a
// socket, so this is its own Connection rather than a TcpConnection.
class PipeConnection final : public Connection {
 public:
  PipeConnection(HANDLE h, std::string peer) : h_(h), peer_(std::move(peer)) {}
  ~PipeConnection() override { close(); }
  bool send_all(ByteSpan d) override {
    size_t off = 0;
    while (off < d.size()) {
      DWORD wrote = 0;
      const DWORD want = (DWORD)std::min<size_t>(d.size() - off, 1u << 20);
      if (!::WriteFile(h_, d.data() + off, want, &wrote, nullptr) || wrote == 0) { dead_ = true; return false; }
      off += wrote;
    }
    return true;
  }
  bool recv_all(MutableByteSpan d, int timeout_ms) override {
    size_t off = 0;
    while (off < d.size()) {
      const long got = recv_some(MutableByteSpan(d.data() + off, d.size() - off), timeout_ms);
      if (got <= 0) return false;
      off += (size_t)got;
    }
    return true;
  }
  long recv_some(MutableByteSpan d, int timeout_ms) override {
    if (d.size() == 0) return 0;
    // An anonymous read on a byte-mode pipe blocks with no timeout, so wait by peeking: nothing there yet is
    // nothing yet (0); a broken pipe is the agent gone (-1).
    const ULONGLONG deadline = ::GetTickCount64() + (ULONGLONG)(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
      DWORD avail = 0;
      if (!::PeekNamedPipe(h_, nullptr, 0, nullptr, &avail, nullptr)) { dead_ = true; return -1; }
      if (avail > 0) {
        DWORD got = 0;
        const DWORD want = (DWORD)std::min<size_t>(d.size(), avail);
        if (!::ReadFile(h_, d.data(), want, &got, nullptr)) { dead_ = true; return -1; }
        return (long)got;
      }
      if (timeout_ms >= 0 && ::GetTickCount64() >= deadline) return 0;
      ::Sleep(1);
    }
  }
  std::string peer() const override { return peer_; }
  bool alive() const override { return h_ != INVALID_HANDLE_VALUE && !dead_; }
  void close() override {
    if (h_ != INVALID_HANDLE_VALUE) { ::FlushFileBuffers(h_); ::CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
    dead_ = true;
  }
 private:
  HANDLE h_;
  std::string peer_;
  std::atomic<bool> dead_{false};
};

Result<std::unique_ptr<Connection>> net_connect_pipe(const std::string& name, int timeout_ms) {
  ensure_winsock();
  // The OpenSSH agent's pipe may not exist yet: a named pipe server is created
  // when its first client arrives, so wait for it briefly before opening.
  const DWORD wait_ms = timeout_ms <= 0 ? 0 : (DWORD)timeout_ms;
  if (!::WaitNamedPipeW(std::wstring(name.begin(), name.end()).c_str(), wait_ms)) {
    const DWORD e = ::GetLastError();
    if (e == ERROR_SEM_TIMEOUT) return Result<std::unique_ptr<Connection>>(Status::Busy, name + ": the agent is busy with other clients");
    if (e == ERROR_FILE_NOT_FOUND)
      return Result<std::unique_ptr<Connection>>(Status::NotFound, name + ": no agent is listening on this pipe (" + sock_error((int)e) + ")");
    return Result<std::unique_ptr<Connection>>(Status::IoError, name + ": " + sock_error((int)e));
  }
  const HANDLE h = ::CreateFileW(std::wstring(name.begin(), name.end()).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD e = ::GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PIPE_BUSY)
      return Result<std::unique_ptr<Connection>>(Status::NotFound, name + ": no agent is listening on this pipe (" + sock_error((int)e) + ")");
    return Result<std::unique_ptr<Connection>>(Status::IoError, name + ": " + sock_error((int)e));
  }
  DWORD mode = PIPE_READMODE_BYTE;
  ::SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
  return std::unique_ptr<Connection>(new PipeConnection(h, name));
}

Result<std::unique_ptr<Connection>> net_connect(const std::string& host, uint16_t port, int timeout_ms) {
  ensure_winsock();
  addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
    return Result<std::unique_ptr<Connection>>(Status::NotFound, "cannot resolve " + host);
  const SOCKET fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET) { ::freeaddrinfo(res); return Result<std::unique_ptr<Connection>>(Status::IoError, last_sock_error()); }
  set_nonblocking(fd, true);
  int rc = ::connect(fd, res->ai_addr, (int)res->ai_addrlen);
  if (rc != 0 && ::WSAGetLastError() == WSAEWOULDBLOCK) {
    WSAPOLLFD pf{fd, POLLOUT, 0};
    if (::WSAPoll(&pf, 1, timeout_ms) <= 0) { ::closesocket(fd); ::freeaddrinfo(res); return Result<std::unique_ptr<Connection>>(Status::Busy, "connect timed out"); }
    int err = 0; int el = sizeof err; ::getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
    if (err) { const auto e = sock_error(err); ::closesocket(fd); ::freeaddrinfo(res); return Result<std::unique_ptr<Connection>>(Status::IoError, e); }
  } else if (rc != 0) {
    const auto e = last_sock_error(); ::closesocket(fd); ::freeaddrinfo(res);
    return Result<std::unique_ptr<Connection>>(Status::IoError, e);
  }
  set_nonblocking(fd, false);
  std::string peer = addr_str(*(sockaddr_in*)res->ai_addr);
  ::freeaddrinfo(res);
  return std::unique_ptr<Connection>(new TcpConnection(fd, peer));
}

Result<std::unique_ptr<Connection>> net_connect_unix(const std::string& path, int timeout_ms) {
  ensure_winsock();
  sockaddr_un a{};
  if (path.size() >= sizeof a.sun_path) return Result<std::unique_ptr<Connection>>(Status::Invalid, "socket path too long");
  const SOCKET fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET) return Result<std::unique_ptr<Connection>>(Status::IoError, last_sock_error());
  a.sun_family = AF_UNIX;
  std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
  set_nonblocking(fd, true);
  int rc = ::connect(fd, (sockaddr*)&a, sizeof a);
  if (rc != 0 && (::WSAGetLastError() == WSAEWOULDBLOCK)) {
    WSAPOLLFD pf{fd, POLLOUT, 0};
    if (::WSAPoll(&pf, 1, timeout_ms) <= 0) { ::closesocket(fd); return Result<std::unique_ptr<Connection>>(Status::Busy, "connect timed out"); }
    int err = 0; int el = sizeof err; ::getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
    if (err) { const auto e = sock_error(err); ::closesocket(fd); return Result<std::unique_ptr<Connection>>(Status::IoError, e); }
  } else if (rc != 0) {
    const auto e = last_sock_error(); ::closesocket(fd);
    return Result<std::unique_ptr<Connection>>(Status::IoError, path + ": " + e);
  }
  set_nonblocking(fd, false);
  return std::unique_ptr<Connection>(new TcpConnection(fd, "unix:" + path));
}

namespace {
bool resolve_v4(const std::string& host, in_addr& out) {
  if (host.empty() || host == "0.0.0.0") { out.s_addr = htonl(INADDR_ANY); return true; }
  if (host == "255.255.255.255") { out.s_addr = htonl(INADDR_BROADCAST); return true; }
  if (::inet_pton(AF_INET, host.c_str(), &out) == 1) return true;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  addrinfo* res = nullptr;
  if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
  out = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
  ::freeaddrinfo(res);
  return true;
}

class UdpSocketImpl final : public UdpSocket {
 public:
  UdpSocketImpl(SOCKET fd, uint16_t port) : fd_(fd), port_(port) {}
  ~UdpSocketImpl() override { close(); }
  bool send_to(const std::string& host, uint16_t port, ByteSpan data, std::string& error) override {
    if (fd_ == INVALID_SOCKET) { error = "socket is closed"; return false; }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    if (!resolve_v4(host, to.sin_addr)) { error = "cannot resolve " + host; return false; }
    const int n = ::sendto(fd_, (const char*)data.data(), (int)data.size(), 0, (sockaddr*)&to, sizeof to);
    if (n < 0) { error = "sendto: " + last_sock_error(); return false; }
    return true;
  }
  Result<Datagram> recv_from(size_t max_len, int timeout_ms) override {
    if (fd_ == INVALID_SOCKET) return Result<Datagram>(Status::Invalid, "socket is closed");
    WSAPOLLFD pf{fd_, POLLIN, 0};
    const int pr = ::WSAPoll(&pf, 1, timeout_ms);
    if (pr == 0) return Result<Datagram>(Status::Busy, "nothing arrived");
    if (pr < 0) return Result<Datagram>(Status::IoError, "poll: " + last_sock_error());
    std::string buf(max_len ? max_len : 1, '\0');
    sockaddr_in from{};
    int fl = sizeof from;
    const int n = ::recvfrom(fd_, buf.data(), (int)buf.size(), 0, (sockaddr*)&from, &fl);
    if (n < 0) return Result<Datagram>(Status::IoError, "recvfrom: " + last_sock_error());
    buf.resize((size_t)n);
    char ip[INET_ADDRSTRLEN] = {};
    ::InetNtopA(AF_INET, (void*)&from.sin_addr, ip, sizeof ip);
    Datagram d;
    d.data = std::move(buf);
    d.host = ip;
    d.port = ntohs(from.sin_port);
    return d;
  }
  uint16_t port() const override { return port_; }
  void close() override { if (fd_ != INVALID_SOCKET) { ::closesocket(fd_); fd_ = INVALID_SOCKET; } }
 private:
  SOCKET fd_;
  uint16_t port_;
};
}  // namespace

Result<std::unique_ptr<UdpSocket>> udp_open(const std::string& bind_host, uint16_t port) {
  ensure_winsock();
  const SOCKET fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd == INVALID_SOCKET) return Result<std::unique_ptr<UdpSocket>>(Status::IoError, "socket: " + last_sock_error());
  BOOL one = TRUE;
  ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof one);
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (!resolve_v4(bind_host, a.sin_addr)) { ::closesocket(fd); return Result<std::unique_ptr<UdpSocket>>(Status::NotFound, "cannot resolve " + bind_host); }
  if (::bind(fd, (sockaddr*)&a, sizeof a) < 0) {
    const auto why = last_sock_error();
    ::closesocket(fd);
    return Result<std::unique_ptr<UdpSocket>>(Status::IoError, "bind " + std::to_string(port) + ": " + why);
  }
  int al = sizeof a;
  ::getsockname(fd, (sockaddr*)&a, &al);
  return std::unique_ptr<UdpSocket>(new UdpSocketImpl(fd, ntohs(a.sin_port)));
}

void net_service(int timeout_ms) { if (timeout_ms > 0) ::Sleep((DWORD)timeout_ms); }
const char* net_backend() { return "poll"; }

}  // namespace aios::hal
