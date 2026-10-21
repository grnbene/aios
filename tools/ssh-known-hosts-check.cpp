// known_hosts against OpenSSH's ssh-keygen. Which lines name a host (plain and
// comma lists, [host]:port, * ? and ! patterns, |1| hashes) is ssh-keygen -F's
// answer; from the lines it reports, OpenSSH's rule gives the state (a matching
// @revoked line with this key: revoked; a plain line with it: known; a plain line
// with another key of its type: changed; else unknown), and ssh_check_host_key
// must give the same state and deciding line. ssh_hash_host_name must reproduce
// the hashes ssh-keygen -H writes, ssh_add_known_host must write lines ssh-keygen
// -F finds (file 0600, a new directory 0700), and ssh_accept_host_key must give
// each StrictHostKeyChecking outcome. Nothing outside a fresh temp directory is
// read or written.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "aios/core/base64.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/process.hpp"
#include "aios/net/ssh.hpp"
#include "aios/net/ssh_known_hosts.hpp"

using namespace aios;
using net::SshHostKeyState;

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
std::string run(const std::vector<std::string>& argv, int* rc = nullptr) {
  auto r = hal::run_tool(argv, 60000);
  if (rc) *rc = r.ok() ? r->exit_code : 127;
  return r.ok() ? r->output : std::string();
}
const char* word(SshHostKeyState s) {
  switch (s) {
    case SshHostKeyState::Known: return "Known";
    case SshHostKeyState::Changed: return "Changed";
    case SshHostKeyState::Revoked: return "Revoked";
    case SshHostKeyState::Unknown: return "Unknown";
  }
  return "?";
}

struct Key {
  std::string type, b64;
  ByteBuffer blob;
};
Key make_key(const std::string& dir, const std::string& name, const std::string& type) {
  std::string path = dir + "/" + name;
  run({"ssh-keygen", "-q", "-t", type, "-N", "", "-f", path});
  std::string pub = read_text(path + ".pub");
  Key k;
  size_t sp = pub.find(' ');
  k.type = pub.substr(0, sp);
  size_t sp2 = pub.find(' ', sp + 1);
  k.b64 = pub.substr(sp + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp - 1);
  while (!k.b64.empty() && (k.b64.back() == '\n' || k.b64.back() == '\r')) k.b64.pop_back();
  auto b = base64_decode(k.b64);
  if (b.ok()) k.blob = *b;
  return k;
}

// The lines ssh-keygen -F says name `name` in `file`: (line number, marker, type, base64).
struct Hit { int line; std::string marker, type, b64; };
std::vector<Hit> keygen_find(const std::string& file, const std::string& name) {
  std::vector<Hit> out;
  std::string text = run({"ssh-keygen", "-F", name, "-f", file});
  std::istringstream in(text);
  std::string l;
  int pending = 0;
  while (std::getline(in, l)) {
    size_t at = l.find(" found: line ");
    if (l.rfind("# Host ", 0) == 0 && at != std::string::npos) { pending = std::atoi(l.c_str() + at + 13); continue; }
    if (!pending || l.empty()) continue;
    std::istringstream w(l);
    Hit h{pending, "", "", ""};
    std::string first;
    w >> first;
    if (!first.empty() && first[0] == '@') { h.marker = first; w >> first; }
    w >> h.type >> h.b64;
    out.push_back(h);
    pending = 0;
  }
  return out;
}

// OpenSSH's decision from the lines that name the host, files in order.
struct Want { SshHostKeyState state = SshHostKeyState::Unknown; std::string file; int line = 0; };
Want oracle(const std::vector<std::string>& files, const std::string& name, const Key& k) {
  Want w;
  Want known, changed;
  for (auto& f : files) {
    for (auto& h : keygen_find(f, name)) {
      if (h.marker == "@revoked") {
        if (h.b64 == k.b64) { w.state = SshHostKeyState::Revoked; w.file = f; w.line = h.line; return w; }
        continue;
      }
      if (h.marker == "@cert-authority" || h.type != k.type) continue;
      if (h.b64 == k.b64) { if (known.file.empty()) { known = {SshHostKeyState::Known, f, h.line}; } }
      else if (changed.file.empty()) changed = {SshHostKeyState::Changed, f, h.line};
    }
  }
  if (!known.file.empty()) return known;
  if (!changed.file.empty()) return changed;
  return w;
}

void expect_state(const std::string& tag, const std::vector<std::string>& files, const std::string& name, const Key& k) {
  Want want = oracle(files, name, k);
  auto got = net::ssh_check_host_key(hal::process_host(), files, name, k.type, ByteSpan(k.blob));
  bool same = got.state == want.state && (want.state == SshHostKeyState::Unknown || (got.file == want.file && got.line == want.line));
  check(same, tag + ": " + word(got.state) + (got.line ? " (line " + std::to_string(got.line) + ")" : "") + ", OpenSSH: " +
                  word(want.state) + (want.line ? " (line " + std::to_string(want.line) + ")" : ""));
}

}  // namespace

int main() {
  // no controlling terminal: the "ask" cases must find nobody to ask, wherever this runs
  ::setsid();
  auto base = hal::scratch_path("");
  if (!base.ok()) { std::printf("FAIL  no scratch directory\n"); return 1; }
  const std::string tmp = *base + ".d";
  hal::process_host().make_dir(tmp);
  hal::Host& host = hal::process_host();

  Key ed = make_key(tmp, "ed", "ed25519"), ed2 = make_key(tmp, "ed2", "ed25519"), rsa = make_key(tmp, "rsa", "rsa"),
      ec = make_key(tmp, "ec", "ecdsa");
  auto line = [](const std::string& names, const Key& k) { return names + " " + k.type + " " + k.b64 + "\n"; };

  // plain names, comma lists, comments, blanks, junk, case
  std::string f1 = tmp + "/mixed";
  write_text(f1, "# a comment\n\n   \n" + line("host.example.org", ed) + line("other.example.org,Host.Example.ORG", rsa) +
                     "this is not a known_hosts line\n# " + line("other.example.org", ed));
  for (auto& [tag, name, key] : std::vector<std::tuple<std::string, std::string, Key>>{
           {"plain name", "host.example.org", ed}, {"comma list", "host.example.org", rsa}, {"comma list, other name", "other.example.org", rsa},
           {"another ed25519 key: changed", "host.example.org", ed2}, {"a name no line has", "nowhere.example.org", ed},
           {"a commented-out line is no line", "other.example.org", ed}, {"case-insensitive", "HOST.example.org", ed}})
    expect_state(tag, {f1}, name, key);

  // [host]:port
  std::string f2 = tmp + "/port";
  write_text(f2, line(net::ssh_known_hosts_name("host.example.org", 2222), ed));
  check(net::ssh_known_hosts_name("host.example.org", 2222) == "[host.example.org]:2222" && net::ssh_known_hosts_name("h", 22) == "h",
        "ssh_known_hosts_name writes [host]:port off port 22");
  expect_state("[host]:port", {f2}, "[host.example.org]:2222", ed);
  expect_state("the plain name is another record", {f2}, "host.example.org", ed);

  // wildcards and negation
  std::string f3 = tmp + "/wild";
  write_text(f3, line("*.example.org", ed) + line("!bad.example.org,*.example.org", rsa) + line("?x*", ec));
  for (auto& [tag, name, key] : std::vector<std::tuple<std::string, std::string, Key>>{
           {"wildcard", "a.example.org", ed}, {"negated name", "bad.example.org", rsa}, {"negation only rules out its own line", "bad.example.org", ed},
           {"? and *", "axis", ec}, {"? needs a character", "x", ec}})
    expect_state(tag, {f3}, name, key);

  // hashed, as ssh-keygen -H writes them
  std::string f4 = tmp + "/hashed";
  write_text(f4, line("host.example.org", ed) + line("[host.example.org]:2222", rsa) + line("Mixed.Case.org", ec));
  std::string plain4 = read_text(f4);
  run({"ssh-keygen", "-H", "-f", f4});
  for (auto& [tag, name, key] : std::vector<std::tuple<std::string, std::string, Key>>{
           {"hashed", "host.example.org", ed}, {"hashed [host]:port", "[host.example.org]:2222", rsa}, {"hashed, another name", "elsewhere.org", ed},
           {"hashed, changed key", "host.example.org", ed2}, {"hashed, lowercased name", "mixed.case.org", ec}})
    expect_state(tag, {f4}, name, key);
  {
    // every hash ssh-keygen wrote, reproduced from its salt
    std::istringstream in(read_text(f4));
    std::string l;
    std::vector<std::string> names = {"host.example.org", "[host.example.org]:2222", "Mixed.Case.org"};
    int n = 0;
    while (std::getline(in, l)) {
      if (l.rfind("|1|", 0) != 0) continue;
      std::string field = l.substr(0, l.find(' '));
      size_t bar = field.find('|', 3);
      auto salt = base64_decode(field.substr(3, bar - 3));
      check(salt.ok() && net::ssh_hash_host_name(names[(size_t)n], ByteSpan(*salt)) == field,
            "ssh_hash_host_name reproduces ssh-keygen -H for " + names[(size_t)n]);
      ++n;
    }
    check(n == 3, "ssh-keygen -H hashed all three lines");
  }

  // markers
  std::string f5 = tmp + "/markers";
  write_text(f5, "@revoked " + line("host.example.org", ed) + "@cert-authority " + line("host.example.org", rsa));
  expect_state("@revoked", {f5}, "host.example.org", ed);
  expect_state("@cert-authority is no host key", {f5}, "host.example.org", rsa);
  std::string f6 = tmp + "/revoked-and-plain";
  write_text(f6, line("host.example.org", ed) + "@revoked " + line("host.example.org", ed));
  expect_state("revoked anywhere beats known", {f6}, "host.example.org", ed);

  // several files: user then global
  std::string fu = tmp + "/user", fg = tmp + "/global";
  write_text(fu, line("host.example.org", ed2));
  write_text(fg, line("host.example.org", ed) + line("host.example.org", rsa));
  expect_state("known in the global file", {fu, fg}, "host.example.org", ed);
  expect_state("rsa only in the global file", {fu, fg}, "host.example.org", rsa);
  auto algs = net::ssh_known_host_algorithms(host, {fu, fg}, "host.example.org");
  check(algs.size() == 2 && algs[0] == "ssh-ed25519" && algs[1] == "ssh-rsa", "ssh_known_host_algorithms: ed25519 then rsa, in file order");
  check(net::ssh_known_host_algorithms(host, {tmp + "/missing"}, "host.example.org").empty(), "a missing file names nothing");

  // writing
  auto added = [&](const std::string& tag, const std::string& file, const std::string& name, const Key& k, bool hashed) {
    auto r = net::ssh_add_known_host(host, file, name, k.type, ByteSpan(k.blob), hashed);
    auto hits = keygen_find(file, name);
    bool found = false;
    for (auto& h : hits) if (h.b64 == k.b64) found = true;
    check(r.ok() && found, tag + ": ssh-keygen -F finds what was added" + (r.ok() ? std::string() : ": " + r.error().message));
    if (hashed) check(read_text(file).rfind("|1|", 0) == 0, tag + ": written hashed");
  };
  added("add plain", tmp + "/add-plain", "host.example.org", ed, false);
  added("add again, beside it", tmp + "/add-plain", "host.example.org", rsa, false);
  write_text(tmp + "/add-no-newline", "old.example.org " + ed.type + " " + ed.b64);
  added("add after a last line with no newline", tmp + "/add-no-newline", "host.example.org", rsa, false);
  added("add [host]:port", tmp + "/add-port", "[host.example.org]:2222", rsa, false);
  added("add hashed, in a new directory", tmp + "/deep/add-hashed", "host.example.org", ec, true);
  struct stat fs{}, ds{};
  check(::stat((tmp + "/deep/add-hashed").c_str(), &fs) == 0 && (fs.st_mode & 0777) == 0600, "a new known_hosts file is 0600");
  check(::stat((tmp + "/deep").c_str(), &ds) == 0 && (ds.st_mode & 0777) == 0700, "a new directory for it is 0700");

  // policy
  auto policy = [&](const std::string& strict, bool batch, const std::string& file) {
    net::SshHostKeyPolicy p;
    p.user_files = {file};
    p.strict = strict;
    p.batch = batch;
    return p;
  };
  auto accept = [&](const net::SshHostKeyPolicy& p, const std::string& name, const Key& k, std::vector<std::string>& notes) {
    notes.clear();
    return net::ssh_accept_host_key(host, p, name, name, k.type, ByteSpan(k.blob), &notes);
  };
  std::vector<std::string> notes;
  std::string pf = tmp + "/policy";
  write_text(pf, line("known.example.org", ed));
  {
    auto r = accept(policy("ask", false, pf), "known.example.org", ed, notes);
    check(r.ok() && notes.empty(), "a known key: Ok, nothing said");
  }
  for (const char* strict : {"accept-new", "off", "no"}) {
    std::string name = std::string("new-") + strict + ".example.org";
    auto r = accept(policy(strict, false, pf), name, ed, notes);
    bool recorded = false;
    for (auto& h : keygen_find(pf, name)) if (h.b64 == ed.b64) recorded = true;
    check(r.ok() && recorded && notes.size() == 1 && notes[0] == "Warning: Permanently added '" + name + "' (ED25519) to the list of known hosts.",
          std::string(strict) + ": an unknown host is recorded, with OpenSSH's warning");
  }
  {
    auto r = accept(policy("yes", false, pf), "never.example.org", ed, notes);
    check(!r.ok() && r.error().status == Status::Denied && keygen_find(pf, "never.example.org").empty(), "yes: an unknown host is refused, nothing written");
  }
  {
    auto r = accept(policy("ask", true, pf), "batch.example.org", ed, notes);
    check(!r.ok() && keygen_find(pf, "batch.example.org").empty(), "ask in BatchMode: refused, nothing written");
  }
  {
    // this check runs with no controlling terminal: nobody to ask means no
    auto r = accept(policy("ask", false, pf), "asked.example.org", ed, notes);
    check(!r.ok() && keygen_find(pf, "asked.example.org").empty(), "ask with no terminal: refused, nothing written");
  }
  for (const char* strict : {"ask", "accept-new", "off", "yes"}) {
    auto r = accept(policy(strict, false, pf), "known.example.org", ed2, notes);
    check(!r.ok() && r.error().message.find("REMOTE HOST IDENTIFICATION HAS CHANGED") != std::string::npos &&
              r.error().message.find(pf + ":1") != std::string::npos,
          std::string(strict) + ": a changed key is refused, naming the offending line");
  }
  {
    auto r = accept(policy("off", false, f6), "host.example.org", ed, notes);
    check(!r.ok() && r.error().message.find("REVOKED") != std::string::npos, "a revoked key is refused even with checking off");
  }
  std::printf("%s\n", failures ? "FAILED" : "all known_hosts checks pass");
  return failures ? 1 : 0;
}
