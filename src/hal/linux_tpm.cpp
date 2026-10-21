// The TPM on a Linux substrate.
//
// Three places one can be: the kernel's resource-managed device, the raw
// device, or a software TPM on a socket (swtpm, which speaks the same command
// protocol over TCP and is a real TPM 2.0 implementation, not a simulation of
// the answers). Whichever answers first is the one the machine has.
#include "aios/hal/tpm.hpp"
#include "aios/hal/sync.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace aios::hal {
namespace {

struct Tpm {
  int fd = -1;
  bool is_socket = false;
  std::string where;
};

Mutex& tpm_mutex() { static Mutex m; return m; }
Tpm& tpm() { static Tpm t; return t; }

int open_device(const char* path) {
  int fd = ::open(path, O_RDWR | O_CLOEXEC);
  return fd;
}

// swtpm's command port. Raw TPM 2.0 command buffers, one per write, response
// per read — the same conversation the device file carries.
int open_swtpm(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) { ::close(fd); return -1; }
  return fd;
}

bool write_all(int fd, const uint8_t* p, size_t n) {
  while (n) {
    ssize_t w = ::write(fd, p, n);
    if (w < 0) { if (errno == EINTR) continue; return false; }
    if (w == 0) return false;
    p += w;
    n -= (size_t)w;
  }
  return true;
}

}  // namespace

Result<std::string> tpm_open() {
  LockGuard l(tpm_mutex());
  Tpm& t = tpm();
  if (t.fd >= 0) return t.where;

  for (const char* path : {"/dev/tpmrm0", "/dev/tpm0"}) {
    int fd = open_device(path);
    if (fd >= 0) { t.fd = fd; t.is_socket = false; t.where = path; return t.where; }
  }
  // The port swtpm's --server option listens on by default.
  for (uint16_t port : {(uint16_t)2321, (uint16_t)2322}) {
    int fd = open_swtpm(port);
    if (fd >= 0) {
      t.fd = fd;
      t.is_socket = true;
      t.where = "swtpm on 127.0.0.1:" + std::to_string(port);
      return t.where;
    }
  }
  return Result<std::string>(Status::NotFound,
      "no TPM on this machine (looked for /dev/tpmrm0, /dev/tpm0, and a software TPM on 127.0.0.1:2321)");
}

std::string tpm_where() {
  LockGuard l(tpm_mutex());
  return tpm().fd >= 0 ? tpm().where : std::string();
}

Result<std::vector<uint8_t>> tpm_transmit(const std::vector<uint8_t>& command, uint32_t timeout_ms) {
  LockGuard l(tpm_mutex());
  Tpm& t = tpm();
  if (t.fd < 0) return Result<std::vector<uint8_t>>(Status::NotFound, "no TPM is open");
  if (command.size() < 10) return Result<std::vector<uint8_t>>(Status::Invalid, "a TPM command is at least a header");

  if (!write_all(t.fd, command.data(), command.size())) {
    return Result<std::vector<uint8_t>>(Status::IoError, std::string("writing to the TPM: ") + std::strerror(errno));
  }

  // The device returns one whole response per read; a socket may not, so the
  // header's own size field says when to stop.
  std::vector<uint8_t> out(4096);
  pollfd pf{t.fd, POLLIN, 0};
  int pr = ::poll(&pf, 1, (int)timeout_ms);
  if (pr == 0) return Result<std::vector<uint8_t>>(Status::Busy, "the TPM did not answer in time");
  if (pr < 0) return Result<std::vector<uint8_t>>(Status::IoError, std::string("polling the TPM: ") + std::strerror(errno));

  ssize_t n = ::read(t.fd, out.data(), out.size());
  if (n < 10) return Result<std::vector<uint8_t>>(Status::IoError, "the TPM returned less than a header");
  out.resize((size_t)n);

  uint32_t want = ((uint32_t)out[2] << 24) | ((uint32_t)out[3] << 16) | ((uint32_t)out[4] << 8) | out[5];
  while (t.is_socket && out.size() < want && want <= (1u << 20)) {
    size_t have = out.size();
    out.resize(want);
    ssize_t more = ::read(t.fd, out.data() + have, want - have);
    if (more <= 0) { out.resize(have); break; }
    out.resize(have + (size_t)more);
  }
  return out;
}

void tpm_close() {
  LockGuard l(tpm_mutex());
  Tpm& t = tpm();
  if (t.fd >= 0) ::close(t.fd);
  t.fd = -1;
  t.where.clear();
}

}  // namespace aios::hal
