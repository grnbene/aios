// git journal: every saved version of every file, as the fsmonitor daemon kept them (see fsmonitor.cpp): the change
// layer's snapshots wait for edits to settle, the journal does not.
//   git journal [-n N] [<path>...]         the saved versions, newest first: their number, when, the blob, the path
//   git journal show <n>                   what version <n> held
//   git journal restore <n> [--to <path>]  put version <n> back in the work tree (the next snapshot records it)
#include <ctime>
#include <set>

#include "cli.hpp"

namespace aios_git {

namespace {
struct Saved { size_t n = 0; long long ms = 0; std::string blob, path; };

std::vector<Saved> read_journal(Ctx& c) {
  std::vector<Saved> out;
  auto f = c.host().read_file(c.repo->git_dir() + "/aios-journal");
  if (!f.ok()) return out;
  std::string text((const char*)f->data(), f->size());
  size_t at = 0, n = 0;
  while (at < text.size()) {
    size_t nl = text.find('\n', at);
    if (nl == std::string::npos) break;   // a line still being written
    std::string line = text.substr(at, nl - at);
    at = nl + 1;
    size_t a = line.find(' '), b = a == std::string::npos ? a : line.find(' ', a + 1);
    if (b == std::string::npos) continue;
    out.push_back(Saved{++n, std::atoll(line.substr(0, a).c_str()), line.substr(a + 1, b - a - 1), line.substr(b + 1)});
  }
  return out;
}

const Saved& entry(Ctx& c, const std::vector<Saved>& all, const std::string& spec) {
  size_t n = (size_t)std::atoll(spec.c_str());
  if (n == 0 || n > all.size()) die(c, "no saved version #" + spec + " (git journal lists them)");
  return all[n - 1];
}
}  // namespace

int cmd_journal(Ctx& c) {
  c.open_repo();
  auto all = read_journal(c);
  std::string sub = c.args.size() > 1 ? c.args[1] : "";
  if (sub == "show" || sub == "restore") {
    if (c.args.size() < 3) usage(c, "usage: git journal " + sub + " <n>");
    const Saved& s = entry(c, all, c.args[2]);
    auto o = c.repo->read_object(s.blob);
    if (!o.ok()) die(c, "the blob of #" + c.args[2] + " is gone: " + o.error().message);
    if (sub == "show") {
      c.out.write((const char*)o->payload.data(), (std::streamsize)o->payload.size());
      return 0;
    }
    std::string to = s.path;
    for (size_t i = 3; i + 1 < c.args.size(); ++i) if (c.args[i] == "--to") to = c.args[i + 1];
    std::string full = c.top + "/" + to;
    for (size_t slash = full.find('/', c.top.size() + 1); slash != std::string::npos; slash = full.find('/', slash + 1))
      (void)c.host().make_dir(full.substr(0, slash));
    if (auto w = c.host().write_file(full, aios::ByteSpan(o->payload.data(), o->payload.size())); !w.ok()) die(c, w.error().message);
    c.out << "restored " << to << " to #" << s.n << " (" << s.path << ", " << c.abbrev(s.blob) << ")\n";
    return 0;
  }
  size_t limit = 50;
  std::set<std::string> only;
  for (size_t i = 1; i < c.args.size(); ++i) {
    if (c.args[i] == "-n" && i + 1 < c.args.size()) limit = (size_t)std::atoll(c.args[++i].c_str());
    else if (c.args[i] != "--") only.insert(c.args[i]);
  }
  if (all.empty()) c.out << "No saved versions (the fsmonitor daemon keeps them in the change layer: git fsmonitor--daemon start).\n";
  size_t shown = 0;
  for (auto it = all.rbegin(); it != all.rend() && shown < limit; ++it) {
    if (!only.empty() && !only.count(it->path)) continue;
    time_t t = (time_t)(it->ms / 1000);
    char when[32];
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::gmtime(&t));
    c.out << "#" << it->n << "\t" << when << "." << (it->ms % 1000) / 100 << "\t" << c.abbrev(it->blob) << "\t" << it->path << "\n";
    ++shown;
  }
  return 0;
}

}  // namespace aios_git
