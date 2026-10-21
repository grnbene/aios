// hal::UdpSocket over BSD sockets - shared verbatim by linux_net.cpp and
// mac_net.cpp (included inside namespace aios::hal, after their headers).
namespace posix_udp {

inline bool resolve_v4(const std::string& host, in_addr& out) {
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

class Socket final : public UdpSocket {
 public:
  Socket(int fd, uint16_t port) : fd_(fd), port_(port) {}
  ~Socket() override { close(); }
  bool send_to(const std::string& host, uint16_t port, ByteSpan data, std::string& error) override {
    if (fd_ < 0) { error = "socket is closed"; return false; }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    if (!resolve_v4(host, to.sin_addr)) { error = "cannot resolve " + host; return false; }
    const ssize_t n = ::sendto(fd_, data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
    if (n < 0) { error = std::string("sendto: ") + std::strerror(errno); return false; }
    return true;
  }
  Result<Datagram> recv_from(size_t max_len, int timeout_ms) override {
    if (fd_ < 0) return Result<Datagram>(Status::Invalid, "socket is closed");
    pollfd pf{fd_, POLLIN, 0};
    int pr;
    do { pr = ::poll(&pf, 1, timeout_ms); } while (pr < 0 && errno == EINTR);
    if (pr == 0) return Result<Datagram>(Status::Busy, "nothing arrived");
    if (pr < 0) return Result<Datagram>(Status::IoError, std::string("poll: ") + std::strerror(errno));
    std::string buf(max_len ? max_len : 1, '\0');
    sockaddr_in from{};
    socklen_t fl = sizeof from;
    const ssize_t n = ::recvfrom(fd_, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&from), &fl);
    if (n < 0) return Result<Datagram>(Status::IoError, std::string("recvfrom: ") + std::strerror(errno));
    buf.resize(static_cast<size_t>(n));
    char ip[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
    Datagram d;
    d.data = std::move(buf);
    d.host = ip;
    d.port = ntohs(from.sin_port);
    return d;
  }
  uint16_t port() const override { return port_; }
  void close() override { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
 private:
  int fd_;
  uint16_t port_;
};

}  // namespace posix_udp

Result<std::unique_ptr<UdpSocket>> udp_open(const std::string& bind_host, uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return Result<std::unique_ptr<UdpSocket>>(Status::IoError, std::string("socket: ") + std::strerror(errno));
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (!posix_udp::resolve_v4(bind_host, a.sin_addr)) { ::close(fd); return Result<std::unique_ptr<UdpSocket>>(Status::NotFound, "cannot resolve " + bind_host); }
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) {
    const std::string why = std::strerror(errno);
    ::close(fd);
    return Result<std::unique_ptr<UdpSocket>>(Status::IoError, "bind " + std::to_string(port) + ": " + why);
  }
  socklen_t al = sizeof a;
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &al);
  return std::unique_ptr<UdpSocket>(new posix_udp::Socket(fd, ntohs(a.sin_port)));
}
