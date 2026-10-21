// The ssh-agent client against OpenSSH's own ssh-agent, on a socket in a temp
// directory: the identities it lists are the ones `ssh-add -L` prints, in the
// same order with the same comments; every key it holds signs (RSA under both
// SHA-2 flags), and every signature verifies; a key it does not hold is refused;
// an absent socket or unset SSH_AUTH_SOCK is NotFound (no agent, not an error);
// and an agent-held key logs in to OpenSSH's sshd (inetd mode, a socketpair).
#include <csignal>
#include <cstdio>
#include <cstdlib>
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
#include "aios/hal/sync.hpp"
#include "aios/net/ssh.hpp"
#include "aios/net/ssh_agent.hpp"

using namespace aios;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}
std::string read_text(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
void write_text(const std::string& p, const std::string& t) {
  std::ofstream f(p, std::ios::binary);
  f << t;
}
hal::ToolResult run(const std::vector<std::string>& argv, const std::vector<std::pair<std::string, std::string>>& env = {}) {
  hal::ProcessSpawn spec;
  spec.argv = argv;
  spec.env = env;
  auto r = hal::run_tool(spec, 60000);
  return r.ok() ? *r : hal::ToolResult{127, r.error().message};
}

class SocketStream final : public hal::Connection {
 public:
  explicit SocketStream(int fd) : fd_(fd) {}
  ~SocketStream() override { close(); }
  bool send_all(ByteSpan d) override {
    for (size_t off = 0; off < d.size();) {
      ssize_t n = ::send(fd_, d.data() + off, d.size() - off, 0);
      if (n <= 0) return false;
      off += (size_t)n;
    }
    return true;
  }
  bool recv_all(MutableByteSpan d, int timeout_ms) override {
    for (size_t off = 0; off < d.size();) {
      long n = recv_some(d.subspan(off), timeout_ms);
      if (n <= 0) return false;
      off += (size_t)n;
    }
    return true;
  }
  long recv_some(MutableByteSpan d, int timeout_ms) override {
    pollfd p{fd_, POLLIN, 0};
    int r = ::poll(&p, 1, timeout_ms);
    if (r <= 0) return r == 0 ? 0 : -1;
    ssize_t n = ::recv(fd_, d.data(), d.size(), 0);
    if (n <= 0) { dead_ = true; return -1; }
    return (long)n;
  }
  std::string peer() const override { return "sshd -i"; }
  void close() override { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
  bool alive() const override { return fd_ >= 0 && !dead_; }

 private:
  int fd_;
  bool dead_ = false;
};

}  // namespace

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  auto base = hal::scratch_path("");
  if (!base.ok()) { std::printf("FAIL  no scratch directory\n"); return 1; }
  // a short directory: a unix socket path has a length limit (104 bytes on macOS)
  char tmpl[] = "/tmp/aios-agent-XXXXXX";
  if (!::mkdtemp(tmpl)) { std::printf("FAIL  no temp directory\n"); return 1; }
  const std::string tmp = tmpl;
  const std::string sock = tmp + "/agent.sock";

  // the agent, in the foreground of its own process, on our socket
  hal::ProcessSpawn agent_spec;
  agent_spec.argv = {"ssh-agent", "-D", "-a", sock};
  auto agent_proc = hal::spawn_process(agent_spec);
  if (!agent_proc.ok()) { std::printf("FAIL  could not start ssh-agent: %s\n", agent_proc.error().message.c_str()); return 1; }
  for (int i = 0; i < 300 && !hal::process_host().exists(sock); ++i) hal::sleep_ms(10);
  check(hal::process_host().exists(sock), "ssh-agent is listening on " + sock);

  struct K { std::string type, bits, file; };
  std::vector<K> keys = {{"ed25519", "", tmp + "/k_ed25519"}, {"rsa", "3072", tmp + "/k_rsa"}, {"ecdsa", "256", tmp + "/k_ec256"},
                         {"ecdsa", "384", tmp + "/k_ec384"}, {"ecdsa", "521", tmp + "/k_ec521"}};
  std::vector<std::pair<std::string, std::string>> env = {{"SSH_AUTH_SOCK", sock}};
  for (auto& k : keys) {
    std::vector<std::string> gen = {"ssh-keygen", "-q", "-t", k.type, "-N", "", "-C", "agent " + k.type + k.bits, "-f", k.file};
    if (!k.bits.empty()) { gen.push_back("-b"); gen.push_back(k.bits); }
    check(run(gen).exit_code == 0, "ssh-keygen made " + k.file);
    auto add = run({"ssh-add", k.file}, env);
    check(add.exit_code == 0, "ssh-add " + k.file + (add.exit_code ? ": " + add.output : std::string()));
  }

  auto ag = net::SshAgent::connect(sock);
  check(ag.ok(), "connect to the agent" + (ag.ok() ? std::string() : ": " + ag.error().message));
  if (!ag.ok()) return 1;
  std::shared_ptr<net::SshAgent> agent(std::move(*ag));
  auto ids = agent->identities();
  check(ids.ok(), "the agent lists its identities");
  if (!ids.ok()) return 1;

  // the same keys, order and comments as ssh-add -L
  auto listed = run({"ssh-add", "-L"}, env);
  std::istringstream lines(listed.output);
  std::string l;
  size_t i = 0;
  bool same = true;
  while (std::getline(lines, l)) {
    if (l.empty()) continue;
    auto p = net::ssh_parse_public_line(l);
    std::string comment = l.substr(l.find(' ', l.find(' ') + 1) + 1);
    if (i >= ids->size() || !p.ok() || p->second != (*ids)[i].blob || (*ids)[i].comment != comment || (*ids)[i].algorithm != p->first) same = false;
    ++i;
  }
  check(same && i == ids->size() && i == keys.size(), "identities() equals ssh-add -L: " + std::to_string(ids->size()) + " keys, same order and comments");

  // every key signs, every signature verifies
  ByteBuffer data(1000);
  hal::process_host().random(MutableByteSpan(data));
  for (auto& k : *ids) {
    std::vector<std::string> algs = k.algorithm == "ssh-rsa" ? std::vector<std::string>{"rsa-sha2-256", "rsa-sha2-512"} : std::vector<std::string>{k.algorithm};
    for (auto& alg : algs) {
      auto sig = agent->sign(ByteSpan(k.blob), ByteSpan(data), alg);
      std::string got_alg;
      if (sig.ok()) { auto t = net::ssh_blob_type(ByteSpan(*sig)); got_alg = t; }
      check(sig.ok() && got_alg == alg && net::ssh_verify_signature(ByteSpan(k.blob), ByteSpan(*sig), ByteSpan(data)),
            k.comment + " signs with " + alg + " and the signature verifies" + (sig.ok() ? std::string() : ": " + sig.error().message));
      if (sig.ok()) {
        ByteBuffer other = data;
        other[0] ^= 1;
        check(!net::ssh_verify_signature(ByteSpan(k.blob), ByteSpan(*sig), ByteSpan(other)), k.comment + " (" + alg + "): not over other data");
      }
    }
    auto signer = net::ssh_agent_signer(agent, k);
    std::string alg = k.algorithm == "ssh-rsa" ? "rsa-sha2-512" : k.algorithm;
    auto s2 = signer->sign(ByteSpan(data), alg);
    check(signer->public_blob() == k.blob && signer->key_type() == k.algorithm && s2.ok() &&
              net::ssh_verify_signature(ByteSpan(k.blob), ByteSpan(*s2), ByteSpan(data)),
          "ssh_agent_signer for " + k.comment + " signs through the shared agent (" + signer->describe() + ")");
  }

  // a key the agent does not hold
  run({"ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", tmp + "/stranger"});
  auto stranger = net::ssh_parse_public_line(read_text(tmp + "/stranger.pub"));
  auto refused = stranger.ok() ? agent->sign(ByteSpan(stranger->second), ByteSpan(data), "ssh-ed25519") : Result<ByteBuffer>(Status::Invalid, "");
  check(stranger.ok() && !refused.ok(), "a key the agent does not hold is refused");

  // no agent there
  auto none = net::SshAgent::connect(tmp + "/no-such.sock");
  check(!none.ok() && none.error().status == Status::NotFound, "an absent socket is NotFound");
  const char* saved = std::getenv("SSH_AUTH_SOCK");
  std::string keep = saved ? saved : "";
  ::unsetenv("SSH_AUTH_SOCK");
  auto unset = net::SshAgent::connect();
  check(!unset.ok() && unset.error().status == Status::NotFound, "SSH_AUTH_SOCK unset is NotFound");
  ::setenv("SSH_AUTH_SOCK", sock.c_str(), 1);
  auto from_env = net::SshAgent::connect();
  check(from_env.ok(), "SSH_AUTH_SOCK names the agent when no path is given");
  if (saved) ::setenv("SSH_AUTH_SOCK", keep.c_str(), 1); else ::unsetenv("SSH_AUTH_SOCK");

  // an agent-held key logs in to sshd
  std::string sshd = hal::find_program({"/usr/sbin/sshd", "/usr/local/sbin/sshd"});
  if (!sshd.empty()) {
    run({"ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", tmp + "/host"});
    write_text(tmp + "/authorized_keys", read_text(keys[2].file + ".pub"));   // only the P-256 key
    write_text(tmp + "/sshd_config", "HostKey " + tmp + "/host\nAuthorizedKeysFile " + tmp + "/authorized_keys\nStrictModes no\n"
                                         "UsePAM no\nPidFile none\nPasswordAuthentication no\nKbdInteractiveAuthentication no\nLogLevel ERROR\n");
    int sv[2];
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    pid_t pid = ::fork();
    if (pid == 0) {
      ::dup2(sv[1], 0);
      ::dup2(sv[1], 1);
      ::close(sv[0]);
      ::close(sv[1]);
      std::string cfg = tmp + "/sshd_config";
      ::execl(sshd.c_str(), sshd.c_str(), "-i", "-f", cfg.c_str(), (char*)nullptr);
      ::_exit(127);
    }
    ::close(sv[1]);
    net::SshConnectOptions o;
    for (auto& k : *ids) o.signers.push_back(net::ssh_agent_signer(agent, k));
    o.verify_host_key = [](const net::SshHostKey&) -> Result<void> { return Ok(); };
    std::string user = run({"/usr/bin/id", "-un"}).output;
    while (!user.empty() && user.back() == '\n') user.pop_back();
    net::SshSession s(hal::process_host(), "");
    auto r = s.connect_over(std::unique_ptr<hal::Connection>(new SocketStream(sv[0])), net::SshTarget{user, "sshd-i", 22}, o);
    check(r.ok() && s.authenticated_with().find("agent ecdsa256") != std::string::npos,
          "an agent-held key logs in to sshd (" + (r.ok() ? s.authenticated_with() : r.error().message) + ")");
    s.disconnect();
    int st;
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &st, 0);
  }

  (void)hal::signal_process(*agent_proc, hal::ProcessSignal::Terminate);
  (void)hal::release_process(*agent_proc);
  run({"/bin/rm", "-rf", tmp});
  std::printf("%s\n", failures ? "FAILED" : "all ssh-agent checks pass");
  return failures ? 1 : 0;
}
