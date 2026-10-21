// ssh_config resolution against OpenSSH's own: for each fixture and host, what
// ssh_resolve gives is compared with what `ssh -G -F <fixture> [-l u] [-p n] host`
// prints. Both sides read only the fixture (ssh -F skips the system file, and
// ssh_resolve is told the same). `ssh -G` prints IdentityFile and
// CertificateFile as written (~ and %-tokens unexpanded) and some values in its
// own spelling (false for StrictHostKeyChecking no, none for no timeout, lists
// comma-joined), so those are put in ssh's terms before comparing; nothing else
// is adjusted. `--real` does the same for every Host alias in the person's own
// ~/.ssh/config, reading it only.
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "aios/hal/account.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/process.hpp"
#include "aios/net/ssh_config.hpp"

using namespace aios;

namespace {

int failures = 0;
int compared = 0;

using GOut = std::map<std::string, std::vector<std::string>>;

GOut parse_g(const std::string& text) {
  GOut out;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    start = end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty()) continue;
    size_t sp = line.find(' ');
    out[line.substr(0, sp)].push_back(sp == std::string::npos ? std::string() : line.substr(sp + 1));
  }
  return out;
}
std::string first(const GOut& g, const std::string& k) {
  auto it = g.find(k);
  return it == g.end() || it->second.empty() ? std::string() : it->second.front();
}
std::string joined(const std::vector<std::string>& v, const std::string& sep) {
  std::string out;
  for (auto& s : v) out += (out.empty() ? "" : sep) + s;
  return out;
}

struct Ctx {
  hal::LocalAccount account;
  std::string local_host;
};

// IdentityFile as ssh would open it: ~ and the tokens a fixture uses.
std::string expand_like_ssh(const std::string& raw, const Ctx& c, const net::SshResolved& r) {
  std::string v = raw;
  if (v.rfind("~/", 0) == 0) v = c.account.home + v.substr(1);
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] != '%' || i + 1 >= v.size()) { out += v[i]; continue; }
    char t = v[++i];
    if (t == 'h') out += r.hostname;
    else if (t == 'n') out += r.original_host;
    else if (t == 'p') out += std::to_string(r.port);
    else if (t == 'r') out += r.user;
    else if (t == 'u') out += c.account.name;
    else if (t == 'd') out += c.account.home;
    else if (t == '%') out += '%';
    else { out += '%'; out += t; }
  }
  return out;
}

void expect(const std::string& tag, const std::string& key, const std::string& ours, const std::string& theirs) {
  ++compared;
  if (ours == theirs) return;
  ++failures;
  std::printf("FAIL  %s: %s: ssh says '%s', ssh_resolve says '%s'\n", tag.c_str(), key.c_str(), theirs.c_str(), ours.c_str());
}

void compare(const std::string& tag, const net::SshResolved& r, const GOut& g, const Ctx& c) {
  expect(tag, "hostname", r.hostname, first(g, "hostname"));
  expect(tag, "user", r.user, first(g, "user"));
  expect(tag, "port", std::to_string(r.port), first(g, "port"));
  std::vector<std::string> ids;
  for (auto& raw : g.count("identityfile") ? g.at("identityfile") : std::vector<std::string>()) ids.push_back(expand_like_ssh(raw, c, r));
  expect(tag, "identityfile", joined(r.identity_files, " "), joined(ids, " "));
  std::vector<std::string> certs;
  for (auto& raw : g.count("certificatefile") ? g.at("certificatefile") : std::vector<std::string>()) certs.push_back(expand_like_ssh(raw, c, r));
  expect(tag, "certificatefile", joined(r.certificate_files, " "), joined(certs, " "));
  expect(tag, "identitiesonly", r.identities_only ? "yes" : "no", first(g, "identitiesonly"));
  expect(tag, "userknownhostsfile", joined(r.user_known_hosts_files, " "), first(g, "userknownhostsfile"));
  expect(tag, "globalknownhostsfile", joined(r.global_known_hosts_files, " "), first(g, "globalknownhostsfile"));
  std::string strict = r.strict_host_key_checking == "yes" ? "true" : r.strict_host_key_checking == "off" ? "false" : r.strict_host_key_checking;
  expect(tag, "stricthostkeychecking", strict, first(g, "stricthostkeychecking"));
  expect(tag, "hashknownhosts", r.hash_known_hosts ? "yes" : "no", first(g, "hashknownhosts"));
  expect(tag, "checkhostip", r.check_host_ip ? "yes" : "no", first(g, "checkhostip"));
  expect(tag, "proxyjump", joined(r.proxy_jump, ","), first(g, "proxyjump"));
  if (!r.proxy_command.empty() || g.count("proxycommand")) expect(tag, "proxycommand", r.proxy_command, first(g, "proxycommand"));
  expect(tag, "hostkeyalias", r.host_key_alias, first(g, "hostkeyalias"));
  expect(tag, "connecttimeout", r.connect_timeout_s ? std::to_string(r.connect_timeout_s) : "none", first(g, "connecttimeout"));
  expect(tag, "serveraliveinterval", std::to_string(r.server_alive_interval_s), first(g, "serveraliveinterval"));
  expect(tag, "serveralivecountmax", std::to_string(r.server_alive_count_max), first(g, "serveralivecountmax"));
  expect(tag, "connectionattempts", std::to_string(r.connection_attempts), first(g, "connectionattempts"));
  expect(tag, "batchmode", r.batch_mode ? "yes" : "no", first(g, "batchmode"));
  // IdentityAgent: ssh prints the path it will use; unset is the environment's socket, which ssh -G does not print
  if (g.count("identityagent")) expect(tag, "identityagent", r.identity_agent, first(g, "identityagent"));
  else expect(tag, "identityagent", r.identity_agent, "");
  // an explicit list (no +/-/^) is printed as given; a default or modified one is OpenSSH's own list, not this client's
  if (!r.host_key_algorithms.empty() && first(g, "hostkeyalgorithms") == joined(r.host_key_algorithms, ","))
    expect(tag, "hostkeyalgorithms", joined(r.host_key_algorithms, ","), first(g, "hostkeyalgorithms"));
}

std::string run_ssh_g(const std::vector<std::string>& argv, int& rc) {
  hal::ProcessSpawn spec;
  spec.argv = argv;
  auto r = hal::run_tool(spec, 60000);
  if (!r.ok()) { rc = 127; return r.error().message; }
  rc = r->exit_code;
  return r->output;
}

struct Case {
  std::string tag, fixture, host, user, port;
};

}  // namespace

int main(int argc, char** argv) {
  hal::Host& host = hal::process_host();
  Ctx c;
  auto acct = hal::local_account();
  if (!acct.ok()) { std::printf("FAIL  no local account: %s\n", acct.error().message.c_str()); return 1; }
  c.account = *acct;
  c.local_host = hal::local_hostname();
  std::string ssh = hal::find_program({"/usr/bin/ssh", "/usr/local/bin/ssh", "ssh"});
  if (ssh.empty()) { std::printf("FAIL  no ssh client to compare with\n"); return 1; }

  if (argc > 1 && std::string(argv[1]) == "--real") {
    // every alias the person's own config names (patterns skipped): read only, nothing written
    auto text = host.read_file(c.account.home + "/.ssh/config");
    if (!text.ok()) { std::printf("FAIL  cannot read ~/.ssh/config\n"); return 1; }
    std::string s(text->begin(), text->end());
    std::vector<std::string> names;
    size_t start = 0;
    while (start < s.size()) {
      size_t end = s.find('\n', start);
      if (end == std::string::npos) end = s.size();
      std::string line = s.substr(start, end - start);
      start = end + 1;
      size_t i = line.find_first_not_of(" \t");
      if (i == std::string::npos) continue;
      if (line.compare(i, 5, "Host ") != 0 && line.compare(i, 5, "host ") != 0) continue;
      std::string rest = line.substr(i + 5);
      size_t p = 0;
      while (p < rest.size()) {
        while (p < rest.size() && (rest[p] == ' ' || rest[p] == '\t')) ++p;
        size_t q = p;
        while (q < rest.size() && rest[q] != ' ' && rest[q] != '\t') ++q;
        std::string n = rest.substr(p, q - p);
        if (!n.empty() && n.find_first_of("*?!") == std::string::npos && n[0] != '#') names.push_back(n);
        p = q;
      }
    }
    for (auto& n : names) {
      int rc;
      std::string out = run_ssh_g({ssh, "-G", n}, rc);
      auto r = net::ssh_resolve(host, n, "", 0);
      if (rc != 0 || !r.ok()) {
        ++failures;
        std::printf("FAIL  %s: ssh -G exit %d, ssh_resolve %s\n", n.c_str(), rc, r.ok() ? "ok" : r.error().message.c_str());
        continue;
      }
      compare(n, *r, parse_g(out), c);
    }
    std::printf("%zu aliases, %d values compared, %d mismatches\n", names.size(), compared, failures);
    return failures ? 1 : 0;
  }

  auto base = hal::scratch_path("");
  if (!base.ok()) { std::printf("FAIL  no scratch directory\n"); return 1; }
  const std::string root = *base + ".d";
  host.make_dir(root);
  host.make_dir(root + "/inc");
  auto write = [&](const std::string& name, const std::string& body) {
    (void)host.write_file(root + "/" + name, ByteSpan((const uint8_t*)body.data(), body.size()));
    return root + "/" + name;
  };

  std::vector<Case> cases;
  write("alias.conf", "Host aar\n  HostName aar.example.com\n  User alice\n  Port 2222\n  IdentityFile ~/.ssh/aar_key\nHost *\n  User fallback\n");
  cases.push_back({"alias", "alias.conf", "aar", "", ""});
  cases.push_back({"alias, url user and port win", "alias.conf", "aar", "git", "2200"});
  write("wild.conf", "Host *.example.com\n  User wildcard-user\n  Port 2022\nHost api.example.com\n  Port 33\nHost *\n  User star\n  Port 44\n");
  cases.push_back({"first value wins", "wild.conf", "api.example.com", "", ""});
  cases.push_back({"wildcard", "wild.conf", "www.example.com", "", ""});
  cases.push_back({"catch-all", "wild.conf", "elsewhere.test", "", ""});
  cases.push_back({"case-insensitive host", "wild.conf", "API.Example.COM", "", ""});
  write("negate.conf", "Host * !bad.example.com\n  User good\n  Port 2100\n");
  cases.push_back({"negation, plain", "negate.conf", "fine.example.com", "", ""});
  cases.push_back({"negation, excluded", "negate.conf", "bad.example.com", "", ""});
  write("keys.conf", "Host keys.example.com\n  IdentityFile ~/.ssh/one\n  CertificateFile ~/.ssh/one-cert.pub\n  IdentityFile /abs/two\nHost *\n  IdentityFile ~/.ssh/three\n  IdentityFile ~/.ssh/one\n");
  cases.push_back({"IdentityFile accumulates", "keys.conf", "keys.example.com", "", ""});
  write("inc/10-a.conf", "Host included.example.com\n  Port 2200\n");
  write("inc/20-b.conf", "Host included.example.com\n  IdentityFile ~/.ssh/from-include\n  User from-include\n");
  write("inc/.hidden.conf", "Host included.example.com\n  User hidden\n");
  write("include.conf", "Include " + root + "/inc/*.conf\nInclude does-not-exist-anywhere.conf\nHost included.example.com\n  User late\n  StrictHostKeyChecking accept-new\n");
  cases.push_back({"Include glob", "include.conf", "included.example.com", "", ""});
  write("incblock.conf", "Host other.example.com\n  Include " + root + "/inc/*.conf\nHost *\n  Port 2900\n");
  cases.push_back({"Include inside an inactive block", "incblock.conf", "included.example.com", "", ""});
  write("match.conf", "Host alias.example.com\n  HostName real.example.com\nMatch host real.example.com\n  User by-host\n"
                      "Match originalhost alias.example.com\n  ConnectionAttempts 4\nMatch user bob\n  Port 2422\n"
                      "Match localuser " + c.account.name + "\n  ServerAliveCountMax 7\nMatch final\n  ServerAliveInterval 19\n"
                      "Match exec \"/bin/sh -c 'exit 0'\"\n  ConnectTimeout 33\nMatch exec \"/bin/sh -c 'exit 3'\"\n  CheckHostIP yes\n"
                      "Match !host nothing.example.com\n  HashKnownHosts yes\n");
  cases.push_back({"Match criteria", "match.conf", "alias.example.com", "", ""});
  cases.push_back({"Match user from the url", "match.conf", "alias.example.com", "bob", ""});
  write("matchall.conf", "Match all\n  User everyone\n");
  cases.push_back({"Match all", "matchall.conf", "x.example.com", "", ""});
  write("tokens.conf", "Host tok\n  HostName real.%h.example\n  User u-%h\n  IdentityFile ~/.ssh/%n-%r-%p\n  UserKnownHostsFile ~/kh-%h /abs/kh-%n\n  HostKeyAlias alias-%n\n");
  cases.push_back({"tokens", "tokens.conf", "tok", "", ""});
  cases.push_back({"tokens with a url user and port", "tokens.conf", "tok", "carol", "2401"});
  write("switch.conf", "Host switch.example.com\n  IdentitiesOnly yes\n  ProxyJump jump1.example.com,deployer@jump2.example.com:2200\n"
                       "  StrictHostKeyChecking no\n  GlobalKnownHostsFile ~/.ssh/global_known_hosts\n  HashKnownHosts yes\n  CheckHostIP yes\n"
                       "  ConnectTimeout 42\n  BatchMode yes\n  IdentityAgent ~/.ssh/agent.sock\n  HostKeyAlgorithms ssh-ed25519,rsa-sha2-512\n");
  cases.push_back({"switches", "switch.conf", "switch.example.com", "", ""});
  write("strict.conf", "Host yes.example.com\n  StrictHostKeyChecking yes\nHost ask.example.com\n  StrictHostKeyChecking ask\n"
                       "Host accept.example.com\n  StrictHostKeyChecking accept-new\nHost none.example.com\n  ProxyJump none\n  IdentityAgent none\n"
                       "  ConnectTimeout 1m30s\n");
  for (const char* h : {"yes.example.com", "ask.example.com", "accept.example.com", "none.example.com"}) cases.push_back({h, "strict.conf", h, "", ""});
  write("syntax.conf", "host=eq.example.com\n  user = spaced\n  Port=2333\n  IdentityFile \"~/.ssh/with space\"\n  # a comment\n\n  HostName eq.example.com # trailing\n");
  cases.push_back({"= and quotes and comments", "syntax.conf", "eq.example.com", "", ""});

  for (auto& cs : cases) {
    std::string fixture = root + "/" + cs.fixture;
    std::vector<std::string> argv = {ssh, "-G", "-F", fixture};
    if (!cs.user.empty()) { argv.push_back("-l"); argv.push_back(cs.user); }
    if (!cs.port.empty()) { argv.push_back("-p"); argv.push_back(cs.port); }
    argv.push_back(cs.host);
    int rc;
    std::string out = run_ssh_g(argv, rc);
    net::SshConfigFiles files;
    files.user_file = fixture;
    files.read_system = false;
    auto r = net::ssh_resolve(host, cs.host, cs.user, cs.port.empty() ? 0 : (uint16_t)std::atoi(cs.port.c_str()), files);
    if (rc != 0) {
      // ssh refused the config: so must the resolver
      if (r.ok()) { ++failures; std::printf("FAIL  %s: ssh refused it (%s) but ssh_resolve accepted it\n", cs.tag.c_str(), out.c_str()); }
      else std::printf("ok    %s: both refuse it\n", cs.tag.c_str());
      continue;
    }
    if (!r.ok()) { ++failures; std::printf("FAIL  %s: ssh_resolve: %s\n", cs.tag.c_str(), r.error().message.c_str()); continue; }
    int before = failures;
    compare(cs.tag, *r, parse_g(out), c);
    if (failures == before) std::printf("ok    %s\n", cs.tag.c_str());
  }

  // configurations ssh refuses, refused here too
  std::vector<std::pair<std::string, std::string>> bad = {
      {"unknown keyword", "Host *\n  NoSuchOption yes\n"},
      {"bad yes/no", "Host *\n  BatchMode maybe\n"},
      {"bad port", "Host *\n  Port 99999\n"},
      {"unknown algorithm", "Host *\n  HostKeyAlgorithms ssh-nonsense\n"},
      {"missing argument", "Host *\n  User\n"},
      {"unterminated quote", "Host *\n  IdentityFile \"~/.ssh/x\n"},
      {"bad Match attribute", "Match colour blue\n  User x\n"},
  };
  int n = 0;
  for (auto& [tag, body] : bad) {
    std::string f = write("bad" + std::to_string(n++) + ".conf", body);
    int rc;
    std::string out = run_ssh_g({ssh, "-G", "-F", f, "anyhost"}, rc);
    net::SshConfigFiles files;
    files.user_file = f;
    files.read_system = false;
    auto r = net::ssh_resolve(host, "anyhost", "", 0, files);
    bool both = rc != 0 && !r.ok();
    if (!both) ++failures;
    std::printf("%s  refused: %s (ssh exit %d; ssh_resolve: %s)\n", both ? "ok  " : "FAIL", tag.c_str(), rc,
                r.ok() ? "accepted" : r.error().message.c_str());
  }
  std::printf("%zu cases, %d values compared, %d mismatches\n%s\n", cases.size(), compared, failures,
              failures ? "FAILED" : "all ssh_config checks pass");
  return failures ? 1 : 0;
}
