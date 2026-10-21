// The controlling terminal on Linux and macOS: /dev/tty, opened for this one
// question, so a prompt reaches the person even while stdin and stdout carry a
// protocol (git's pack stream, for one).
#include "aios/hal/tty.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace aios::hal {
namespace {

// Restores the terminal's modes however the read ends, including on a signal
// that interrupts it (the handler puts the modes back, then lets the signal do
// what it would have done).
struct EchoGuard {
  int fd;
  termios saved{};
  bool active = false;
  EchoGuard(int f, bool hide) : fd(f) {
    if (!hide || ::tcgetattr(fd, &saved) != 0) return;
    termios t = saved;
    t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL);
    if (::tcsetattr(fd, TCSAFLUSH, &t) == 0) active = true;
  }
  ~EchoGuard() {
    if (active) ::tcsetattr(fd, TCSAFLUSH, &saved);
  }
};

volatile sig_atomic_t g_caught = 0;
void note_signal(int sig) { g_caught = sig; }

}  // namespace

Result<std::string> tty_prompt(const std::string& prompt, bool echo) {
  int fd = ::open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (fd < 0) return Result<std::string>(Status::NotFound, "no terminal to ask (/dev/tty: " + std::string(std::strerror(errno)) + ")");

  // While the terminal's echo is off, an interrupt must not leave it off: catch the
  // usual signals for the length of the read, restore, then re-raise.
  const int sigs[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP};
  struct sigaction old[5];
  struct sigaction sa{};
  sa.sa_handler = note_signal;
  sigemptyset(&sa.sa_mask);
  g_caught = 0;
  for (int i = 0; i < 5; ++i) ::sigaction(sigs[i], &sa, &old[i]);

  std::string answer;
  bool failed = false;
  int read_errno = 0;
  {
    EchoGuard guard(fd, !echo);
    for (size_t off = 0; off < prompt.size();) {
      ssize_t w = ::write(fd, prompt.data() + off, prompt.size() - off);
      if (w < 0 && errno == EINTR && !g_caught) continue;
      if (w <= 0) break;
      off += (size_t)w;
    }
    for (;;) {
      char ch;
      ssize_t r = ::read(fd, &ch, 1);
      if (r < 0 && errno == EINTR && !g_caught) continue;
      if (r < 0) { failed = true; read_errno = errno; break; }
      if (r == 0 || ch == '\n' || ch == '\r') break;
      answer += ch;
    }
    if (!echo) (void)!::write(fd, "\n", 1);
  }
  for (int i = 0; i < 5; ++i) ::sigaction(sigs[i], &old[i], nullptr);
  ::close(fd);
  if (g_caught) {
    int sig = g_caught;
    g_caught = 0;
    ::raise(sig);
    return Result<std::string>(Status::Denied, "interrupted");
  }
  if (failed) return Result<std::string>(Status::IoError, std::string("reading the terminal: ") + std::strerror(read_errno));
  return answer;
}

}  // namespace aios::hal
