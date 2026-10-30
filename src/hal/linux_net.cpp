#include "aios/hal/net.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <vector>
#include <string>
#include <cctype>
#if __has_include(<linux/io_uring.h>)
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#define AIOS_HAVE_URING 1
#endif

namespace aios::hal {
namespace {
#ifdef AIOS_HAVE_URING
// A submission/completion ring shared with the kernel, driven with the raw
// syscalls so there is no liburing to carry. One per connection: the recv
// below used to be a recv() that got EAGAIN and then a poll() — two crossings
// into the kernel per statement when the bytes were not already there — and
// is now a single io_uring_enter that returns with the bytes in rbuf_. A
// kernel that refuses (seccomp, pre-5.6) leaves ok() false and the poll path
// below carries on exactly as before.
struct Uring {
  int fd = -1;
  io_uring_params p{};
  uint8_t* sq = nullptr; uint8_t* cq = nullptr; io_uring_sqe* sqes = nullptr;
  size_t sq_bytes = 0, cq_bytes = 0;
  unsigned *sq_tail = nullptr, *sq_mask = nullptr, *sq_array = nullptr, *cq_head = nullptr, *cq_tail = nullptr, *cq_mask = nullptr;
  io_uring_cqe* cqes = nullptr;
  unsigned queued = 0;
  bool init(unsigned entries) {
    std::memset(&p, 0, sizeof p);
    fd = (int)::syscall(SYS_io_uring_setup, entries, &p);
    if (fd < 0) return false;
    sq_bytes = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    cq_bytes = p.cq_off.cqes + p.cq_entries * sizeof(io_uring_cqe);
    const bool single = p.features & IORING_FEAT_SINGLE_MMAP;
    if (single) sq_bytes = cq_bytes = std::max(sq_bytes, cq_bytes);
    sq = (uint8_t*)::mmap(nullptr, sq_bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    if (sq == MAP_FAILED) { sq = nullptr; shut(); return false; }
    cq = single ? sq : (uint8_t*)::mmap(nullptr, cq_bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
    if (cq == MAP_FAILED) { cq = nullptr; shut(); return false; }
    sqes = (io_uring_sqe*)::mmap(nullptr, p.sq_entries * sizeof(io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
    if (sqes == MAP_FAILED) { sqes = nullptr; shut(); return false; }
    sq_tail = (unsigned*)(sq + p.sq_off.tail); sq_mask = (unsigned*)(sq + p.sq_off.ring_mask); sq_array = (unsigned*)(sq + p.sq_off.array);
    cq_head = (unsigned*)(cq + p.cq_off.head); cq_tail = (unsigned*)(cq + p.cq_off.tail); cq_mask = (unsigned*)(cq + p.cq_off.ring_mask);
    cqes = (io_uring_cqe*)(cq + p.cq_off.cqes);
    return true;
  }
  bool ok() const { return fd >= 0; }
  io_uring_sqe* sqe() {
    const unsigned tail = *sq_tail + queued, idx = tail & *sq_mask;
    io_uring_sqe* s = &sqes[idx]; std::memset(s, 0, sizeof *s); sq_array[idx] = idx; ++queued;
    return s;
  }
  // Publishes what sqe() queued and waits for `want` completions.
  int submit_wait(unsigned want) {
    __atomic_store_n(sq_tail, *sq_tail + queued, __ATOMIC_RELEASE);
    const unsigned n = queued; queued = 0;
    return (int)::syscall(SYS_io_uring_enter, fd, n, want, IORING_ENTER_GETEVENTS, nullptr, 0);
  }
  bool pop(io_uring_cqe& out) {
    const unsigned head = *cq_head;
    if (head == __atomic_load_n(cq_tail, __ATOMIC_ACQUIRE)) return false;
    out = cqes[head & *cq_mask];
    __atomic_store_n(cq_head, head + 1, __ATOMIC_RELEASE);
    return true;
  }
  void shut() {
    if (sqes) ::munmap(sqes, p.sq_entries * sizeof(io_uring_sqe));
    if (cq && cq != sq) ::munmap(cq, cq_bytes);
    if (sq) ::munmap(sq, sq_bytes);
    if (fd >= 0) ::close(fd);
    sq = cq = nullptr; sqes = nullptr; fd = -1;
  }
  ~Uring() { shut(); }
};
// io_uring on request only (AIOS_NET_URING=1). For the request-and-answer
// shape of a SQL connection the plain path below - one blocking recv per
// request, one send per answer - measured faster on every workload
// (sysbench point lookups 185k/s against 165k/s on the ring, a tenth less
// CPU per query): a ring submit and wait is more work than a recv.
bool uring_wanted() {
  static const bool on = [] { const char* e = std::getenv("AIOS_NET_URING"); return e && *e == '1'; }();
  return on;
}
// The calling thread's ring, made on first use and kept for the thread's
// life; null when the kernel refuses one (the poll path then carries on).
// Per thread rather than per connection: each send and recv below is one
// submit that waits for its own completions before returning, so a thread
// never has two operations in flight, and no other thread ever reaps its
// ring - the property the two rings per connection were for (a connection
// read on one thread and written on another must not trade completions).
// Two rings per connection was two io_uring_setup, five mmaps and five
// munmaps per connect: the web server opens a FastCGI connection per
// request, and that set-up and teardown (munmap shooting down TLBs on every
// core) was half its CPU, 7.2k req/s on /ok.php where nginx did 15.7k.
Uring* thread_ring() {
  struct Slot { Uring r; bool tried = false; };
  thread_local Slot slot;
  if (!slot.tried) { slot.tried = true; if (uring_wanted()) slot.r.init(4); }
  return slot.r.ok() ? &slot.r : nullptr;
}
#endif

class TcpConnection final : public Connection {
 public:
  TcpConnection(int fd, std::string peer) : fd_(fd), peer_(std::move(peer)) {
    // Rings belong to threads, not connections (thread_ring): a connection
    // read on one thread while another writes it drives two rings, never one
    // shared - a single shared ring once returned 4 MiB wrong and a 50 ms
    // timeout in 0.0 ms with the connection marked dead.
    int one = 1; ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    // Keepalive: a mirror streaming a table from its master for half an hour
    // crosses NATs that forget a quiet mapping, and a peer that vanished is
    // noticed in ~90 s instead of never. Idle 30 s, probe every 10 s, 6 probes.
    ::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    int idle = 30, intvl = 10, cnt = 6;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    ::setsockopt(fd_, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    ::setsockopt(fd_, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
    open_conns().fetch_add(1, std::memory_order_relaxed);
  }
  ~TcpConnection() override { close(); if (fd_ >= 0) ::close(fd_); open_conns().fetch_sub(1, std::memory_order_relaxed); }
  bool send_all(ByteSpan d) override {
    size_t off = 0;
#ifdef AIOS_HAVE_URING
    std::lock_guard<std::mutex> lk(tx_mu_);
    Uring* ring = thread_ring();
    while (ring && off < d.size()) {
      io_uring_sqe* s = ring->sqe();
      s->opcode = IORING_OP_SEND; s->fd = fd_; s->addr = (uint64_t)(uintptr_t)(d.data() + off); s->len = (uint32_t)(d.size() - off); s->msg_flags = MSG_NOSIGNAL;
      if (ring->submit_wait(1) < 0) { if (errno == EINTR) continue; dead_ = true; return false; }
      io_uring_cqe c{}; if (!ring->pop(c)) continue;
      if (c.res == -EINTR) continue;
      if (c.res < 0) { dead_ = true; return false; }
      off += (size_t)c.res;
    }
#endif
    while (off < d.size()) { ssize_t n = ::send(fd_, d.data() + off, d.size() - off, MSG_NOSIGNAL); if (n < 0) { if (errno == EINTR) continue; dead_ = true; return false; } off += (size_t)n; }
    return true;
  }
  // Reads go through a small buffer, filled by whatever the socket holds:
  // a MySQL packet's 4-byte header and its body usually arrive together, and
  // reading them separately, each after its own poll(), was two poll + two
  // recv system calls per statement where one of each does. The socket is
  // tried before it is waited on, as MariaDB does.
  bool recv_all(MutableByteSpan d, int timeout_ms) override {
    size_t off = 0;
    while (off < d.size()) {
      if (rpos_ < rlen_) {
        const size_t take = std::min(d.size() - off, rlen_ - rpos_);
        std::memcpy(d.data() + off, rbuf_ + rpos_, take);
        rpos_ += take;
        off += take;
        continue;
      }
      const int r = fill(timeout_ms);
      if (r == 0) return false;                                    // timeout: no data yet — NOT a drop
      if (r < 0) return false;                                     // closed or error: dead_ is set
    }
    return true;
  }
  long recv_some(MutableByteSpan d, int timeout_ms) override {
    if (d.size() == 0) return 0;
    if (rpos_ >= rlen_) {
      const int r = fill(timeout_ms);
      if (r == 0) return 0;                                        // nothing yet
      if (r < 0) return dead_ ? (last_eof_ ? 0 : -1) : -1;
    }
    const size_t take = std::min(d.size(), rlen_ - rpos_);
    std::memcpy(d.data(), rbuf_ + rpos_, take);
    rpos_ += take;
    return static_cast<long>(take);
  }
  std::string peer() const override { return peer_; }
  // Distinguishes a live-but-idle socket from a dropped one: false only once a
  // recv/send has actually seen EOF or an error. Callers poll on a short timeout
  // and use this to tell "nothing yet" from "the peer is gone".
  bool alive() const override { return fd_ >= 0 && !dead_; }
  int poll_fd() const override { return fd_; }
  // shutdown, not close: it wakes a thread blocked in send/recv/poll on this
  // socket (a bare ::close does not), and the descriptor stays ours until the
  // destructor, so a call still running cannot land on a reused number.
  void close() override { if (fd_ >= 0 && !dead_.exchange(true)) ::shutdown(fd_, SHUT_RDWR); dead_ = true; }
 private:
  // 1: bytes buffered. 0: timed out with nothing. -1: closed (last_eof_) or
  // failed; dead_ set either way.
  int fill(int timeout_ms) {
    rpos_ = rlen_ = 0;
#ifdef AIOS_HAVE_URING
    // One recv, linked to a timeout so the kernel cancels whichever loses.
    // Both complete, so the wait is for two and the recv's result decides.
    Uring* ring = thread_ring();
    while (ring) {
      io_uring_sqe* s = ring->sqe();
      s->opcode = IORING_OP_RECV; s->fd = fd_; s->addr = (uint64_t)(uintptr_t)rbuf_; s->len = sizeof rbuf_; s->user_data = 1;
      __kernel_timespec ts{}; unsigned want = 1;
      if (timeout_ms >= 0) {
        s->flags |= IOSQE_IO_LINK;
        io_uring_sqe* t = ring->sqe();
        t->opcode = IORING_OP_LINK_TIMEOUT; t->addr = (uint64_t)(uintptr_t)&ts; t->len = 1; t->user_data = 2;
        ts.tv_sec = timeout_ms / 1000; ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        want = 2;
      }
      int r;
      do { r = ring->submit_wait(want); } while (r < 0 && errno == EINTR);
      if (r < 0) { dead_ = true; return -1; }
      int res = -EAGAIN; io_uring_cqe c{};
      while (ring->pop(c)) if (c.user_data == 1) res = c.res;
      if (res > 0) { rlen_ = (size_t)res; return 1; }
      if (res == 0) { dead_ = true; last_eof_ = true; return -1; }
      if (res == -ECANCELED || res == -ETIME) return 0;                 // the timeout won
      if (res == -EINTR || res == -EAGAIN) continue;
      dead_ = true; return -1;
    }
#endif
    // One blocking recv, the wait bounded by the socket's own receive
    // timeout (set once per distinct wait): a non-blocking try, a poll and
    // the recv were three system calls on every request that had not
    // arrived yet, which between a client's answer and its next question
    // is every request.
    if (const unsigned spin = spin_us(); spin && timeout_ms != 0 && spin_take()) {
      // Poll briefly before sleeping: a client that answers within the spin
      // costs no sleep and no wake on either side. Only a few connections
      // spin at once - when many are busy the spin would only take the CPU
      // the clients need to ask the next question.
      struct Give { ~Give() { spinners().fetch_sub(1, std::memory_order_relaxed); } } give;
      const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(spin);
      do {
        const ssize_t n = ::recv(fd_, rbuf_, sizeof rbuf_, MSG_DONTWAIT);
        if (n > 0) { rlen_ = (size_t)n; return 1; }
        if (n == 0) { dead_ = true; last_eof_ = true; return -1; }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { dead_ = true; return -1; }
        for (int i = 0; i < 32; ++i) {   // a spin hint: pause on x86, yield on arm64 (the Pi), nothing elsewhere
#if defined(__x86_64__) || defined(__i386__)
          __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
          __asm__ __volatile__("yield");
#endif
        }
      } while (std::chrono::steady_clock::now() < until);
    }
    if (timeout_ms != rcv_timeout_ms_) {
      timeval tv{};
      if (timeout_ms > 0) { tv.tv_sec = timeout_ms / 1000; tv.tv_usec = (timeout_ms % 1000) * 1000; }
      else if (timeout_ms == 0) { tv.tv_usec = 1; }   // "right now": the shortest wait the socket allows
      if (::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0) rcv_timeout_ms_ = timeout_ms;
    }
    for (;;) {
      const ssize_t n = ::recv(fd_, rbuf_, sizeof rbuf_, 0);
      if (n > 0) { rlen_ = (size_t)n; return 1; }
      if (n == 0) { dead_ = true; last_eof_ = true; return -1; }  // peer closed (FIN)
      if (errno == EINTR) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK) { dead_ = true; return -1; }  // error (RST, etc.)
      // The timeout ran out - or the descriptor is non-blocking (a timeout
      // of forever cannot run out): then wait the poll way, as before.
      if (rcv_timeout_ms_ == timeout_ms && timeout_ms >= 0) return 0;
      pollfd pf{fd_, POLLIN, 0};
      const int pr = ::poll(&pf, 1, timeout_ms);
      if (pr == 0) return 0;
      if (pr < 0) { if (errno == EINTR) continue; dead_ = true; return -1; }
    }
  }
  static std::atomic<unsigned>& spinners() { static std::atomic<unsigned> n{0}; return n; }
  static std::atomic<unsigned>& open_conns() { static std::atomic<unsigned> n{0}; return n; }
  static bool spin_take() {
    // Spinning pays only while the machine has idle cores: a connection or
    // two per four cores. Past that the clients need the CPU.
    static const unsigned room = [] { long n = ::sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (unsigned)(n / 2) : 1u; }();
    if (open_conns().load(std::memory_order_relaxed) > room) return false;
    static const unsigned cap = [] { const char* e = std::getenv("AIOS_NET_SPIN_MAX"); return e ? (unsigned)std::strtoul(e, nullptr, 10) : std::max(1u, room / 2); }();
    unsigned c = spinners().load(std::memory_order_relaxed);
    while (c < cap) if (spinners().compare_exchange_weak(c, c + 1, std::memory_order_relaxed)) return true;
    return false;
  }
  static unsigned spin_us() {
    // A sleep and a wake cost a few tens of microseconds on a virtual
    // machine (the idle vCPU halts); a client asking its next question
    // within 30 us is answered without either. AIOS_NET_SPIN_US=0: never.
    static const unsigned v = [] { const char* e = std::getenv("AIOS_NET_SPIN_US"); return e ? (unsigned)std::strtoul(e, nullptr, 10) : 30u; }();
    return v;
  }
  int fd_;
  std::string peer_;
  std::atomic<bool> dead_{false};
  bool last_eof_ = false;
  int rcv_timeout_ms_ = -2;   // the SO_RCVTIMEO the socket holds (-1 forever; -2 never set)
  unsigned char rbuf_[16384];
  size_t rpos_ = 0, rlen_ = 0;
#ifdef AIOS_HAVE_URING
  std::mutex tx_mu_;   // one send_all at a time per connection, so two senders' bytes never interleave
#endif
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
    int c = ::accept4(fd_, (sockaddr*)&a, &al, SOCK_CLOEXEC);
    if (c < 0) return Result<std::unique_ptr<Connection>>(Status::IoError, strerror(errno));
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
    int c = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (c < 0) return Result<std::unique_ptr<Connection>>(Status::IoError, strerror(errno));
    // TcpConnection's TCP_NODELAY setsockopt fails harmlessly on AF_UNIX.
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
  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return Result<std::unique_ptr<Listener>>(Status::IoError, strerror(errno));
  a.sun_family = AF_UNIX;
  std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
  ::unlink(path.c_str());   // a stale socket from a previous run
  if (::bind(fd, (sockaddr*)&a, sizeof a) != 0 || ::listen(fd, 1024) != 0) { std::string e = strerror(errno); ::close(fd); return Result<std::unique_ptr<Listener>>(Status::IoError, "listen on " + path + ": " + e); }
  ::chmod(path.c_str(), 0666);   // the web server's user is not ours
  return std::unique_ptr<Listener>(new UnixListener(fd, path));
}

Result<std::unique_ptr<Listener>> net_listen(uint16_t port, bool loopback_only) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return Result<std::unique_ptr<Listener>>(Status::IoError, strerror(errno));
  int one = 1; ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY); a.sin_port = htons(port);
  if (::bind(fd, (sockaddr*)&a, sizeof a) != 0 || ::listen(fd, 1024) != 0) { std::string e = strerror(errno); ::close(fd); return Result<std::unique_ptr<Listener>>(Status::IoError, "listen on " + std::to_string(port) + ": " + e); }
  socklen_t al = sizeof a; ::getsockname(fd, (sockaddr*)&a, &al);
  return std::unique_ptr<Listener>(new TcpListener(fd, ntohs(a.sin_port)));
}

#if defined(AIOS_STATIC_LINK)
namespace {
// The engine's own resolver, for a statically linked build (see resolve_ipv4 in net.hpp).
std::string read_small_file(const char* path) {
  std::string out;
  int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return out;
  char buf[4096];
  for (ssize_t n; (n = ::read(fd, buf, sizeof buf)) > 0 && out.size() < (1u << 20);) out.append(buf, (size_t)n);
  ::close(fd);
  return out;
}
std::vector<std::string> fields_of(const std::string& line) {
  std::vector<std::string> f;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size() || line[i] == '#' || line[i] == ';') break;
    size_t j = i;
    while (j < line.size() && line[j] != ' ' && line[j] != '\t' && line[j] != '#') ++j;
    f.push_back(line.substr(i, j - i));
    i = j;
  }
  return f;
}
bool same_name(std::string a, std::string b) {
  if (!a.empty() && a.back() == '.') a.pop_back();
  if (!b.empty() && b.back() == '.') b.pop_back();
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
  return true;
}
// One A-record query to one server over UDP: the first address in the answer (a CNAME chain in the same answer
// is followed by taking its A record).
bool dns_a(uint32_t server, const std::string& name, int timeout_ms, uint32_t* out) {
  int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  std::vector<uint8_t> q;
  // an unpredictable query id, so an answer cannot be guessed
  uint16_t id = 0;
  {
    const int rnd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    const bool got = rnd >= 0 && ::read(rnd, &id, sizeof id) == (ssize_t)sizeof id;
    if (rnd >= 0) ::close(rnd);
    if (!got) id = (uint16_t)(::getpid() ^ (uint16_t)std::chrono::steady_clock::now().time_since_epoch().count());
  }
  q.push_back((uint8_t)(id >> 8)); q.push_back((uint8_t)id);
  q.push_back(0x01); q.push_back(0x00);           // recursion desired
  q.push_back(0); q.push_back(1);                 // one question
  for (int i = 0; i < 6; ++i) q.push_back(0);
  size_t s = 0;
  while (s <= name.size()) {
    size_t e = name.find('.', s);
    if (e == std::string::npos) e = name.size();
    if (e > s) {
      if (e - s > 63) { ::close(fd); return false; }
      q.push_back((uint8_t)(e - s));
      q.insert(q.end(), name.begin() + (long)s, name.begin() + (long)e);
    }
    s = e + 1;
  }
  q.push_back(0);
  q.push_back(0); q.push_back(1);                 // QTYPE A
  q.push_back(0); q.push_back(1);                 // QCLASS IN
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(53); a.sin_addr.s_addr = server;
  if (::sendto(fd, q.data(), q.size(), 0, (sockaddr*)&a, sizeof a) != (ssize_t)q.size()) { ::close(fd); return false; }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  uint8_t r[1500];
  for (;;) {
    const int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) break;
    pollfd pf{fd, POLLIN, 0};
    if (::poll(&pf, 1, left) <= 0) break;
    sockaddr_in from{}; socklen_t fl = sizeof from;
    const ssize_t n = ::recvfrom(fd, r, sizeof r, 0, (sockaddr*)&from, &fl);
    if (n < 12 || from.sin_addr.s_addr != server || ((r[0] << 8) | r[1]) != id) continue;   // not the answer to this
    if ((r[3] & 0x0f) != 0) break;                                                           // NXDOMAIN, SERVFAIL ...
    const size_t qd = (size_t)((r[4] << 8) | r[5]), an = (size_t)((r[6] << 8) | r[7]);
    size_t i = 12;
    auto skip_name = [&]() -> bool {
      while (i < (size_t)n) {
        const uint8_t l = r[i];
        if ((l & 0xc0) == 0xc0) { i += 2; return i <= (size_t)n; }
        if (l == 0) { ++i; return true; }
        i += 1 + l;
      }
      return false;
    };
    bool bad = false;
    for (size_t k = 0; k < qd && !bad; ++k) { if (!skip_name() || i + 4 > (size_t)n) bad = true; else i += 4; }
    for (size_t k = 0; k < an && !bad; ++k) {
      if (!skip_name() || i + 10 > (size_t)n) { bad = true; break; }
      const uint16_t type = (uint16_t)((r[i] << 8) | r[i + 1]), rdlen = (uint16_t)((r[i + 8] << 8) | r[i + 9]);
      i += 10;
      if (i + rdlen > (size_t)n) { bad = true; break; }
      if (type == 1 && rdlen == 4) { std::memcpy(out, r + i, 4); ::close(fd); return true; }
      i += rdlen;
    }
    break;
  }
  ::close(fd);
  return false;
}
}  // namespace

Result<uint32_t> resolve_ipv4(const std::string& host, int timeout_ms) {
  in_addr lit{};
  if (::inet_pton(AF_INET, host.c_str(), &lit) == 1) return (uint32_t)lit.s_addr;
  if (host.empty()) return Result<uint32_t>(Status::Invalid, "no host name");
  // /etc/hosts first, as the host's own resolver would
  {
    const std::string hosts = read_small_file("/etc/hosts");
    size_t a = 0;
    while (a < hosts.size()) {
      size_t e = hosts.find('\n', a);
      if (e == std::string::npos) e = hosts.size();
      const auto f = fields_of(hosts.substr(a, e - a));
      a = e + 1;
      if (f.size() < 2 || ::inet_pton(AF_INET, f[0].c_str(), &lit) != 1) continue;
      for (size_t k = 1; k < f.size(); ++k) if (same_name(f[k], host)) return (uint32_t)lit.s_addr;
    }
  }
  if (same_name(host, "localhost")) return (uint32_t)htonl(INADDR_LOOPBACK);
  // then DNS: the servers and search domains /etc/resolv.conf names (127.0.0.1 when it names none)
  std::vector<uint32_t> servers;
  std::vector<std::string> search;
  {
    const std::string rc = read_small_file("/etc/resolv.conf");
    size_t a = 0;
    while (a < rc.size()) {
      size_t e = rc.find('\n', a);
      if (e == std::string::npos) e = rc.size();
      const auto f = fields_of(rc.substr(a, e - a));
      a = e + 1;
      if (f.size() >= 2 && f[0] == "nameserver" && ::inet_pton(AF_INET, f[1].c_str(), &lit) == 1) servers.push_back((uint32_t)lit.s_addr);
      else if (f.size() >= 2 && (f[0] == "search" || f[0] == "domain")) search.assign(f.begin() + 1, f.end());
    }
  }
  if (servers.empty()) servers.push_back((uint32_t)htonl(INADDR_LOOPBACK));
  std::vector<std::string> names;
  const bool dotted = host.find('.') != std::string::npos;
  if (dotted) names.push_back(host);
  for (const auto& d : search) names.push_back(host + "." + d);
  if (!dotted) names.push_back(host);
  const int per = std::max(500, timeout_ms / (int)std::max<size_t>(1, servers.size() * names.size()));
  for (const auto& n : names)
    for (uint32_t s : servers) {
      uint32_t out = 0;
      if (dns_a(s, n, per, &out)) return out;
    }
  return Result<uint32_t>(Status::NotFound, "cannot resolve " + host);
}
#else
Result<uint32_t> resolve_ipv4(const std::string& host, int timeout_ms) {
  (void)timeout_ms;
  addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return Result<uint32_t>(Status::NotFound, "cannot resolve " + host);
  const uint32_t a = (uint32_t)((sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
  ::freeaddrinfo(res);
  return a;
}
#endif

Result<std::unique_ptr<Connection>> net_connect(const std::string& host, uint16_t port, int timeout_ms) {
  auto addr = resolve_ipv4(host, timeout_ms);
  if (!addr.ok()) return Result<std::unique_ptr<Connection>>(Status::NotFound, "cannot resolve " + host);
  sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(port); to.sin_addr.s_addr = *addr;
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) return Result<std::unique_ptr<Connection>>(Status::IoError, strerror(errno));
  int rc = ::connect(fd, (sockaddr*)&to, sizeof to);
  if (rc != 0 && errno == EINPROGRESS) {
    pollfd pf{fd, POLLOUT, 0};
    if (::poll(&pf, 1, timeout_ms) <= 0) { ::close(fd); return Result<std::unique_ptr<Connection>>(Status::Busy, "connect timed out"); }
    int err = 0; socklen_t el = sizeof err; ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err) { ::close(fd); return Result<std::unique_ptr<Connection>>(Status::IoError, strerror(err)); }
  } else if (rc != 0) { std::string e = strerror(errno); ::close(fd); return Result<std::unique_ptr<Connection>>(Status::IoError, e); }
  int fl = ::fcntl(fd, F_GETFL); ::fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
  return std::unique_ptr<Connection>(new TcpConnection(fd, addr_str(to)));
}
Result<std::unique_ptr<Connection>> net_connect_unix(const std::string& path, int timeout_ms) {
  sockaddr_un a{};
  if (path.size() >= sizeof a.sun_path) return Result<std::unique_ptr<Connection>>(Status::Invalid, "socket path too long");
  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) return Result<std::unique_ptr<Connection>>(Status::IoError, strerror(errno));
  a.sun_family = AF_UNIX;
  std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
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
  int fl = ::fcntl(fd, F_GETFL); ::fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
  return std::unique_ptr<Connection>(new TcpConnection(fd, "unix:" + path));
}
// Named pipes are Windows' answer to the unix socket the OpenSSH agent listens
// on: this substrate has the socket, so there is nothing behind this call.
Result<std::unique_ptr<Connection>> net_connect_pipe(const std::string& name, int) {
  return Result<std::unique_ptr<Connection>>(
      Status::Unsupported, name + ": named pipes exist only on Windows, where the agent lives behind one");
}
#include "posix_udp.inc"
void net_service(int timeout_ms) { if (timeout_ms > 0) ::usleep(static_cast<useconds_t>(timeout_ms) * 1000); }
// What a connection on THIS kernel actually drives its bytes through: probed
// by opening a ring, not read off a compile flag.
const char* net_backend() {
#ifdef AIOS_HAVE_URING
  static const char* b = [] { if (!uring_wanted()) return "poll (AIOS_NET_URING=0)"; Uring r; return r.init(2) ? "io_uring" : "poll (io_uring_setup refused)"; }();
  return b;
#else
  return "poll";
#endif
}
}  // namespace aios::hal
