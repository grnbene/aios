// The network face of the macOS host. Same contract as linux_net.cpp; Darwin
// lacks the Linux socket extensions, so descriptor flags are set explicitly
// and a dead peer is silenced with SO_NOSIGPIPE instead of a per-send flag.
#include "aios/hal/net.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

#include <atomic>

namespace aios::hal {
namespace {

void set_cloexec(int fd) { ::fcntl(fd, F_SETFD, FD_CLOEXEC); }

class TcpConnection final : public Connection {
 public:
  TcpConnection(int fd, std::string peer) : fd_(fd), peer_(std::move(peer)) {
    int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    // Darwin has no MSG_NOSIGNAL: writing to a closed peer must not raise a
    // signal, so the socket carries the option for the life of the stream.
    ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
  }
  ~TcpConnection() override { close(); }
  bool send_all(ByteSpan d) override {
    size_t off = 0;
    while (off < d.size()) { ssize_t n = ::send(fd_, d.data() + off, d.size() - off, 0); if (n < 0) { if (errno == EINTR) continue; dead_ = true; return false; } off += (size_t)n; }
    return true;
  }
  bool recv_all(MutableByteSpan d, int timeout_ms) override {
    size_t off = 0;
    while (off < d.size()) {
      pollfd pf{fd_, POLLIN, 0};
      int pr = ::poll(&pf, 1, timeout_ms);
      if (pr == 0) return false;                                   // timeout: no data yet — NOT a drop
      if (pr < 0) { if (errno == EINTR) continue; dead_ = true; return false; }
      ssize_t n = ::recv(fd_, d.data() + off, d.size() - off, 0);
      if (n == 0) { dead_ = true; return false; }                  // peer closed (FIN)
      if (n < 0) { if (errno == EINTR) continue; dead_ = true; return false; }  // error (RST, etc.)
      off += (size_t)n;
    }
    return true;
  }
  long recv_some(MutableByteSpan d, int timeout_ms) override {
    if (d.size() == 0) return 0;
    for (;;) {
      pollfd pf{fd_, POLLIN, 0};
      const int pr = ::poll(&pf, 1, timeout_ms);
      if (pr == 0) return 0;                                       // nothing yet
      if (pr < 0) { if (errno == EINTR) continue; dead_ = true; return -1; }
      const ssize_t n = ::recv(fd_, d.data(), d.size(), 0);
      if (n == 0) { dead_ = true; return 0; }                      // closed: alive() says so
      if (n < 0) { if (errno == EINTR) continue; dead_ = true; return -1; }
      return static_cast<long>(n);
    }
  }
  std::string peer() const override { return peer_; }
  bool alive() const override { return fd_ >= 0 && !dead_; }
  int poll_fd() const override { return fd_; }
  void close() override { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } dead_ = true; }
 private:
  int fd_;
  std::string peer_;
  std::atomic<bool> dead_{false};
};

std::string addr_str(const sockaddr_in& a) { char buf[64]; inet_ntop(AF_INET, &a.sin_addr, buf, sizeof buf); return std::string(buf) + ":" + std::to_string(ntohs(a.sin_port)); }

class TcpListener final : public Listener {
 public:
  TcpListener(int fd, uint16_t port) : fd_(fd), port_(port) {}
  ~TcpListener() override { close(); }
  Result<std::unique_ptr<Connection>> accept(int timeout_ms) override {
    pollfd pf{fd_, POLLIN, 0};
    int pr = ::poll(&pf, 1, timeout_ms);
    if (pr == 0) return Result<std::unique_ptr<Connection>>(Status::Busy);
    if (pr < 0) return Result<std::unique_ptr<Connection>>(Status::IoError, strerror(errno));
    sockaddr_in a{}; socklen_t al = sizeof a;
    int c = ::accept(fd_, (sockaddr*)&a, &al);
    if (c < 0) return Result<std::unique_ptr<Connection>>(Status::IoError, strerror(errno));
    set_cloexec(c);
    return std::unique_ptr<Connection>(new TcpConnection(c, addr_str(a)));
  }
  uint16_t port() const override { return port_; }
  void close() override { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
 private:
  int fd_;
  uint16_t port_;
};

class UnixListener final : public Listener {
 public:
  UnixListener(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}
  ~UnixListener() override { close(); }
  Result<std::unique_ptr<Connection>> accept(int timeout_ms) override {
    pollfd pf{fd_, POLLIN, 0};
    int pr = ::poll(&pf, 1, timeout_ms);
    if (pr == 0) return Result<std::unique_ptr<Connection>>(Status::Busy);
    if (pr < 0) return Result<std::unique_ptr<Connection>>(Status::IoError, strerror(errno));
    // Darwin has no accept4(): the CLOEXEC flag goes on after, same as
    // TcpListener::accept above.
    int c = ::accept(fd_, nullptr, nullptr);
    if (c < 0) return Result<std::unique_ptr<Connection>>(Status::IoError, strerror(errno));
    set_cloexec(c);
    // TcpConnection's TCP_NODELAY/SO_NOSIGPIPE setsockopt calls fail
    // harmlessly on AF_UNIX (ENOPROTOOPT), same as on Linux.
    return std::unique_ptr<Connection>(new TcpConnection(c, "unix:" + path_));
  }
  uint16_t port() const override { return 0; }
  void close() override { if (fd_ >= 0) { ::close(fd_); fd_ = -1; ::unlink(path_.c_str()); } }
 private:
  int fd_;
  std::string path_;
};
}  // namespace

Result<std::unique_ptr<Listener>> net_listen_unix(const std::string& path) {
  sockaddr_un a{};
  if (path.size() >= sizeof a.sun_path) return Result<std::unique_ptr<Listener>>(Status::Invalid, "socket path too long");
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return Result<std::unique_ptr<Listener>>(Status::IoError, strerror(errno));
  set_cloexec(fd);
  a.sun_family = AF_UNIX;
  std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
  ::unlink(path.c_str());   // a stale socket from a previous run
  if (::bind(fd, (sockaddr*)&a, sizeof a) != 0 || ::listen(fd, 1024) != 0) { std::string e = strerror(errno); ::close(fd); return Result<std::unique_ptr<Listener>>(Status::IoError, "listen on " + path + ": " + e); }
  ::chmod(path.c_str(), 0666);   // the web server's user is not ours
  return std::unique_ptr<Listener>(new UnixListener(fd, path));
}

Result<std::unique_ptr<Listener>> net_listen(uint16_t port, bool loopback_only) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return Result<std::unique_ptr<Listener>>(Status::IoError, strerror(errno));
  set_cloexec(fd);
  int one = 1; ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY); a.sin_port = htons(port);
  if (::bind(fd, (sockaddr*)&a, sizeof a) != 0 || ::listen(fd, 1024) != 0) { std::string e = strerror(errno); ::close(fd); return Result<std::unique_ptr<Listener>>(Status::IoError, "listen on " + std::to_string(port) + ": " + e); }
  socklen_t al = sizeof a; ::getsockname(fd, (sockaddr*)&a, &al);
  return std::unique_ptr<Listener>(new TcpListener(fd, ntohs(a.sin_port)));
}

Result<uint32_t> resolve_ipv4(const std::string& host, int timeout_ms) {
  (void)timeout_ms;
  addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return Result<uint32_t>(Status::NotFound, "cannot resolve " + host);
  const uint32_t a = (uint32_t)((sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
  ::freeaddrinfo(res);
  return a;
}

Result<std::unique_ptr<Connection>> net_connect(const std::string& host, uint16_t port, int timeout_ms) {
  addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return Result<std::unique_ptr<Connection>>(Status::NotFound, "cannot resolve " + host);
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { ::freeaddrinfo(res); return Result<std::unique_ptr<Connection>>(Status::IoError, strerror(errno)); }
  set_cloexec(fd);
  int fl = ::fcntl(fd, F_GETFL);
  ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
  if (rc != 0 && errno == EINPROGRESS) {
    pollfd pf{fd, POLLOUT, 0};
    if (::poll(&pf, 1, timeout_ms) <= 0) { ::close(fd); ::freeaddrinfo(res); return Result<std::unique_ptr<Connection>>(Status::Busy, "connect timed out"); }
    int err = 0; socklen_t el = sizeof err; ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err) { ::close(fd); ::freeaddrinfo(res); return Result<std::unique_ptr<Connection>>(Status::IoError, strerror(err)); }
  } else if (rc != 0) { std::string e = strerror(errno); ::close(fd); ::freeaddrinfo(res); return Result<std::unique_ptr<Connection>>(Status::IoError, e); }
  ::fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
  std::string peer = addr_str(*(sockaddr_in*)res->ai_addr);
  ::freeaddrinfo(res);
  return std::unique_ptr<Connection>(new TcpConnection(fd, peer));
}
Result<std::unique_ptr<Connection>> net_connect_unix(const std::string& path, int timeout_ms) {
  sockaddr_un a{};
  if (path.size() >= sizeof a.sun_path) return Result<std::unique_ptr<Connection>>(Status::Invalid, "socket path too long");
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return Result<std::unique_ptr<Connection>>(Status::IoError, strerror(errno));
  set_cloexec(fd);
  a.sun_family = AF_UNIX;
  std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
  int fl = ::fcntl(fd, F_GETFL);
  ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  int rc = ::connect(fd, (sockaddr*)&a, sizeof a);
  if (rc != 0 && (errno == EINPROGRESS || errno == EAGAIN)) {
    pollfd pf{fd, POLLOUT, 0};
    if (::poll(&pf, 1, timeout_ms) <= 0) { ::close(fd); return Result<std::unique_ptr<Connection>>(Status::Busy, "connect timed out"); }
    int err = 0; socklen_t el = sizeof err; ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err == ENOENT || err == ECONNREFUSED) {
      ::close(fd);
      return Result<std::unique_ptr<Connection>>(Status::NotFound,
                                                 path + ": nothing is listening on this unix socket");
    }
    if (err) { ::close(fd); return Result<std::unique_ptr<Connection>>(Status::IoError, strerror(err)); }
  } else if (rc != 0) {
    if (errno == ENOENT || errno == ECONNREFUSED) {
      ::close(fd);
      return Result<std::unique_ptr<Connection>>(Status::NotFound,
                                                 path + ": nothing is listening on this unix socket");
    }
    std::string e = strerror(errno); ::close(fd); return Result<std::unique_ptr<Connection>>(Status::IoError, path + ": " + e);
  }
  ::fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
  return std::unique_ptr<Connection>(new TcpConnection(fd, "unix:" + path));
}
// Darwin has no named pipes: the OpenSSH agent is behind the unix socket above.
Result<std::unique_ptr<Connection>> net_connect_pipe(const std::string& name, int) {
  return Result<std::unique_ptr<Connection>>(Status::Unsupported,
                                            name + ": named pipes exist only on Windows, where the agent lives behind one");
}
#include "posix_udp.inc"
void net_service(int timeout_ms) { if (timeout_ms > 0) ::usleep(static_cast<useconds_t>(timeout_ms) * 1000); }
const char* net_backend() { return "poll"; }
}  // namespace aios::hal
