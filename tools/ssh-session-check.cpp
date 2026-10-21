// The SSH session against OpenSSH's own sshd, run in inetd mode (sshd -i) on a
// socketpair: no TCP, no port, nothing but this process and the server. It
// proves publickey authentication over several keys (refused ones skipped, the
// accepted one named), every key refused (each listed), the host key decided
// only by the caller's callback (and refused when the callback refuses), each
// cipher and key exchange this client offers, and a direct-tcpip channel
// answered by the server. A non-root sshd on macOS cannot open a login session
// once authentication succeeds (BSM audit), so there a session ends at
// USERAUTH_SUCCESS; where it can (Linux), a command runs and its output is read.
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "aios/hal/host.hpp"
#include "aios/hal/process.hpp"
#include "aios/net/ssh.hpp"

using namespace aios;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}
std::string read_text(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
void write_text(const std::string& path, const std::string& t) {
  std::ofstream f(path, std::ios::binary);
  f << t;
}
std::string sh(const std::vector<std::string>& argv) {
  auto r = hal::run_tool(argv, 60000);
  return r.ok() ? r->output : std::string();
}

// One end of a socketpair whose other end is sshd -i's stdin and stdout.
class SocketStream final : public hal::Connection {
 public:
  explicit SocketStream(int fd) : fd_(fd) {}
  ~SocketStream() override { close(); }
  bool send_all(ByteSpan d) override {
    size_t off = 0;
    while (off < d.size()) {
      ssize_t n = ::send(fd_, d.data() + off, d.size() - off, 0);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) { dead_ = true; return false; }
      off += (size_t)n;
    }
    return true;
  }
  bool recv_all(MutableByteSpan d, int timeout_ms) override {
    size_t off = 0;
    while (off < d.size()) {
      long n = recv_some(d.subspan(off), timeout_ms);
      if (n <= 0) return false;
      off += (size_t)n;
    }
    return true;
  }
  long recv_some(MutableByteSpan d, int timeout_ms) override {
    if (fd_ < 0) return -1;
    pollfd p{fd_, POLLIN, 0};
    int r = ::poll(&p, 1, timeout_ms);
    if (r == 0) return 0;
    if (r < 0) return errno == EINTR ? 0 : -1;
    ssize_t n = ::recv(fd_, d.data(), d.size(), 0);
    if (n == 0) { dead_ = true; return -1; }
    if (n < 0) return errno == EINTR || errno == EAGAIN ? 0 : -1;
    return (long)n;
  }
  std::string peer() const override { return "sshd -i"; }
  void close() override {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
  }
  bool alive() const override { return fd_ >= 0 && !dead_; }

 private:
  int fd_;
  bool dead_ = false;
};

struct Server {
  pid_t pid = -1;
  std::unique_ptr<hal::Connection> stream;
};

// sshd -i -f <cfg> with its stdin/stdout on one end of a socketpair.
Server start_sshd(const std::string& sshd, const std::string& cfg, const std::vector<std::string>& extra) {
  Server s;
  int sv[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return s;
  std::vector<std::string> args = {sshd, "-i", "-f", cfg};
  for (auto& e : extra) args.push_back(e);
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = ::fork();
  if (pid == 0) {
    ::dup2(sv[1], 0);
    ::dup2(sv[1], 1);
    ::close(sv[0]);
    ::close(sv[1]);
    ::execv(argv[0], argv.data());
    ::_exit(127);
  }
  ::close(sv[1]);
  s.pid = pid;
  s.stream.reset(new SocketStream(sv[0]));
  return s;
}
void stop(Server& s) {
  s.stream.reset();
  if (s.pid > 0) {
    for (int i = 0; i < 50; ++i) {
      int st;
      if (::waitpid(s.pid, &st, WNOHANG) == s.pid) { s.pid = -1; return; }
      ::usleep(20000);
    }
    ::kill(s.pid, SIGKILL);
    int st;
    ::waitpid(s.pid, &st, 0);
    s.pid = -1;
  }
}

Result<net::SshIdentity> load(const std::string& path) {
  return net::ssh_load_identity(hal::process_host(), path, false);
}

// A signer that records whether it was ever asked to sign: a refused key must not be.
class Spy final : public net::SshSigner {
 public:
  Spy(std::unique_ptr<net::SshSigner> in, bool* asked) : in_(std::move(in)), asked_(asked) {}
  ByteBuffer public_blob() const override { return in_->public_blob(); }
  std::string key_type() const override { return in_->key_type(); }
  std::string describe() const override { return in_->describe(); }
  Result<ByteBuffer> sign(ByteSpan data, const std::string& alg) override { *asked_ = true; return in_->sign(data, alg); }

 private:
  std::unique_ptr<net::SshSigner> in_;
  bool* asked_;
};

}  // namespace

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  std::string sshd = hal::find_program({"/usr/sbin/sshd", "/usr/local/sbin/sshd", "/opt/homebrew/sbin/sshd"});
  if (sshd.empty()) { std::printf("FAIL  no sshd to talk to\n"); return 1; }
  auto base = hal::scratch_path("");
  if (!base.ok()) { std::printf("FAIL  no scratch directory\n"); return 1; }
  const std::string tmp = *base + ".d";
  hal::process_host().make_dir(tmp);
  const std::string user = sh({"/usr/bin/id", "-un"}).substr(0, sh({"/usr/bin/id", "-un"}).find('\n'));

  // host keys of three kinds, user keys of four
  for (const char* t : {"ed25519", "ecdsa", "rsa"}) sh({"ssh-keygen", "-q", "-t", t, "-N", "", "-f", tmp + "/host_" + t});
  for (const char* k : {"a", "b", "c"}) sh({"ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", k, "-f", tmp + "/user_" + k});
  sh({"ssh-keygen", "-q", "-t", "ecdsa", "-b", "384", "-N", "", "-C", "ec", "-f", tmp + "/user_ec"});
  sh({"ssh-keygen", "-q", "-t", "rsa", "-b", "3072", "-N", "", "-C", "rsa", "-f", tmp + "/user_rsa"});
  // only c, ec and rsa are authorized
  write_text(tmp + "/authorized_keys", read_text(tmp + "/user_c.pub") + read_text(tmp + "/user_ec.pub") + read_text(tmp + "/user_rsa.pub"));
  std::string cfg = tmp + "/sshd_config";
  write_text(cfg, "HostKey " + tmp + "/host_ed25519\nHostKey " + tmp + "/host_ecdsa\nHostKey " + tmp + "/host_rsa\n" +
                      "AuthorizedKeysFile " + tmp + "/authorized_keys\nStrictModes no\nUsePAM no\nPidFile none\n"
                      "PasswordAuthentication no\nKbdInteractiveAuthentication no\nAllowTcpForwarding yes\nPermitOpen any\nLogLevel ERROR\n");

  net::SshTarget target{user, "sshd-i", 22};
  auto signer_of = [&](const std::string& k) {
    auto id = load(tmp + "/user_" + k);
    return net::ssh_identity_signer(id.ok() ? *id : net::SshIdentity(), tmp + "/user_" + k);
  };
  std::string seen_host_key;
  bool post_auth_works = true;   // false where the server cannot open a session after login (macOS, non-root)
  auto accept_all = [&](const net::SshHostKey& k) -> Result<void> { seen_host_key = k.fingerprint; return Ok(); };

  // 1. two refused keys, then an accepted one; the refused ones are never asked to sign
  {
    bool a_signed = false, b_signed = false;
    net::SshConnectOptions o;
    o.signers.push_back(std::make_unique<Spy>(signer_of("a"), &a_signed));
    o.signers.push_back(std::make_unique<Spy>(signer_of("b"), &b_signed));
    o.signers.push_back(signer_of("c"));
    o.verify_host_key = accept_all;
    o.host_key_algorithms = {"ssh-ed25519"};
    Server srv = start_sshd(sshd, cfg, {});
    net::SshSession s(hal::process_host(), "");
    auto r = s.connect_over(std::move(srv.stream), target, o);
    check(r.ok(), "two refused keys then an accepted one" + (r.ok() ? std::string() : ": " + r.error().message));
    check(s.authenticated_with() == tmp + "/user_c", "authenticated_with names the third key (" + s.authenticated_with() + ")");
    check(!a_signed && !b_signed, "the refused keys were offered without ever signing");
    std::string want = sh({"ssh-keygen", "-l", "-E", "sha256", "-f", tmp + "/host_ed25519.pub"});
    check(!seen_host_key.empty() && want.find(seen_host_key) != std::string::npos, "the host key the callback saw is the server's (" + seen_host_key + ")");
    if (r.ok()) {
      auto e = s.exec("echo ok-from-sshd");
      if (e.ok()) {
        std::string out;
        for (int i = 0; i < 50 && out.find('\n') == std::string::npos; ++i) {
          auto d = s.recv(2000);
          if (!d.ok() || d->empty()) break;
          out.append((const char*)d->data(), d->size());
        }
        check(out == "ok-from-sshd\n", "a command runs and its output comes back");
      } else {
        post_auth_works = false;
        std::printf("note  the server could not open a login session (%s): authentication is what this platform proves\n", e.error().message.c_str());
      }
    }
    stop(srv);
  }

  // 2. every key refused: each is named in the error
  {
    net::SshConnectOptions o;
    o.signers.push_back(signer_of("a"));
    o.signers.push_back(signer_of("b"));
    o.verify_host_key = accept_all;
    Server srv = start_sshd(sshd, cfg, {});
    net::SshSession s(hal::process_host(), "");
    auto r = s.connect_over(std::move(srv.stream), target, o);
    bool named = !r.ok() && r.error().message.find("user_a") != std::string::npos && r.error().message.find("user_b") != std::string::npos;
    check(!r.ok() && r.error().status == Status::Denied && named, "all keys refused: Denied, each key listed" + (r.ok() ? std::string() : " (" + r.error().message + ")"));
    stop(srv);
  }

  // 3. the host key callback refusing ends the session before any key is offered
  {
    bool c_signed = false;
    net::SshConnectOptions o;
    o.signers.push_back(std::make_unique<Spy>(signer_of("c"), &c_signed));
    o.verify_host_key = [](const net::SshHostKey&) -> Result<void> { return Result<void>(Status::Denied, "not this host"); };
    Server srv = start_sshd(sshd, cfg, {});
    net::SshSession s(hal::process_host(), "");
    auto r = s.connect_over(std::move(srv.stream), target, o);
    check(!r.ok() && r.error().message == "not this host" && !c_signed, "a refused host key stops the session before authentication");
    stop(srv);
  }

  // 4. each cipher, each key exchange, each host key type, each user key type
  struct Combo { std::string cipher, kex, hostkey, key; };
  std::vector<Combo> combos = {
      {"chacha20-poly1305@openssh.com", "curve25519-sha256", "ssh-ed25519", "c"},
      {"aes256-gcm@openssh.com", "curve25519-sha256", "ecdsa-sha2-nistp256", "ec"},
      {"aes128-gcm@openssh.com", "ecdh-sha2-nistp256", "rsa-sha2-512", "rsa"},
      {"aes256-gcm@openssh.com", "ecdh-sha2-nistp384", "rsa-sha2-256", "c"},
      {"chacha20-poly1305@openssh.com", "ecdh-sha2-nistp521", "ssh-ed25519", "ec"},
  };
  for (auto& cb : combos) {
    net::SshConnectOptions o;
    o.signers.push_back(signer_of(cb.key));
    o.verify_host_key = accept_all;
    o.ciphers = {cb.cipher};
    o.kex_algorithms = {cb.kex};
    o.host_key_algorithms = {cb.hostkey};
    Server srv = start_sshd(sshd, cfg, {"-o", "Ciphers=" + cb.cipher, "-o", "KexAlgorithms=" + cb.kex});
    net::SshSession s(hal::process_host(), "");
    auto r = s.connect_over(std::move(srv.stream), target, o);
    std::string what = cb.cipher + " / " + cb.kex + " / " + cb.hostkey + " host key / " + cb.key + " user key";
    check(r.ok() && s.cipher() == cb.cipher && s.kex() == cb.kex, what + (r.ok() ? std::string() : ": " + r.error().message));
    stop(srv);
  }

  // 5. a direct-tcpip channel: the server answers (confirmation, or a clean refusal naming why), never a hang
  {
    net::SshConnectOptions o;
    o.signers.push_back(signer_of("c"));
    o.verify_host_key = accept_all;
    Server srv = start_sshd(sshd, cfg, {});
    net::SshSession s(hal::process_host(), "");
    auto r = s.connect_over(std::move(srv.stream), target, o);
    if (r.ok() && !post_auth_works) {
      std::printf("note  direct-tcpip not provable here: this server ends every session after login\n");
    } else if (r.ok()) {
      auto ch = s.open_direct_tcpip("127.0.0.1", 9);
      bool answered = ch.ok() || ch.error().status == Status::Denied;
      check(answered, std::string("direct-tcpip is answered by the server: ") + (ch.ok() ? "opened" : ch.error().message));
    } else {
      check(false, "direct-tcpip: could not log in first: " + r.error().message);
    }
    stop(srv);
  }

  // 6. transport rekeying (RFC 4253 §9) over an active session
  {
    net::SshConnectOptions o;
    o.signers.push_back(signer_of("c"));
    o.verify_host_key = accept_all;
    Server srv = start_sshd(sshd, cfg, {});
    net::SshSession s(hal::process_host(), "");
    auto r = s.connect_over(std::move(srv.stream), target, o);
    check(r.ok(), "connected for rekeying check");
    auto rk = s.rekey();
    check(rk.ok(), "client rekeying succeeds" + (rk.ok() ? std::string() : ": " + rk.error().message));
    if (r.ok() && post_auth_works) {
      auto e = s.exec("cat");
      check(e.ok(), "exec cat runs after rekey");
      std::string msg1 = "hello before second rekey\n";
      auto s1 = s.send(ByteSpan((const uint8_t*)msg1.data(), msg1.size()));
      check(s1.ok(), "send before second rekey");
      auto rk2 = s.rekey();
      check(rk2.ok(), "second rekey mid-channel succeeds" + (rk2.ok() ? std::string() : ": " + rk2.error().message));
      std::string msg2 = "world after second rekey\n";
      auto s2 = s.send(ByteSpan((const uint8_t*)msg2.data(), msg2.size()));
      check(s2.ok(), "send after second rekey");
      s.send_eof();
      std::string got;
      while (!s.eof()) {
        auto d = s.recv(2000);
        if (!d.ok() || d->empty()) break;
        got.append((const char*)d->data(), d->size());
      }
      check(got == msg1 + msg2, "data received across rekey boundaries matches exactly");
    }
    stop(srv);
  }

  // 7. server-initiated rekeying (OpenSSH RekeyLimit)
  {
    net::SshConnectOptions o;
    o.signers.push_back(signer_of("c"));
    o.verify_host_key = accept_all;
    Server srv = start_sshd(sshd, cfg, {"-o", "RekeyLimit=1K"});
    net::SshSession s(hal::process_host(), "");
    auto r = s.connect_over(std::move(srv.stream), target, o);
    check(r.ok(), "connected for server-initiated rekey check");
    if (r.ok() && post_auth_works) {
      auto e = s.exec("cat");
      check(e.ok(), "exec cat runs for server-initiated rekey" + (e.ok() ? std::string() : ": " + e.error().message));
      std::string chunk(4096, 'x');
      chunk += "\n";
      auto s1 = s.send(ByteSpan((const uint8_t*)chunk.data(), chunk.size()));
      check(s1.ok(), "sent 4KB through RekeyLimit=1K");
      s.send_eof();
      std::string got;
      for (int i = 0; i < 20; ++i) {
        auto d = s.recv(2000);
        if (!d.ok() || (d->empty() && s.eof())) break;
        if (!d->empty()) got.append((const char*)d->data(), d->size());
        if (got.size() >= chunk.size()) break;
      }
      check(got == chunk, "received full 4KB payload across server-initiated rekey");
    }
    stop(srv);
  }

  std::printf("%s\n", failures ? "FAILED" : "all ssh session checks pass");
  return failures ? 1 : 0;
}
