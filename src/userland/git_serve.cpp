// The serving side of git's protocol (stateless as smart HTTP runs it): what `git http-backend`,
// `git upload-pack --stateless-rpc` and `git receive-pack --stateless-rpc` answer. A real git client on
// the other end clones from, fetches from and pushes to a repository this machine holds. upload-pack speaks
// v0 (multi_ack_detailed) and, to a client that asks with Git-Protocol: version=2, v2 (ls-refs and fetch);
// receive-pack speaks v0 only, as git's does (push has no v2).
#include <cstdlib>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "aios/userland/git.hpp"
#include "aios/userland/git_merge.hpp"
#include "aios/userland/git_remote.hpp"

namespace aios::userland {

namespace {

const char* kZero = "0000000000000000000000000000000000000000";

void pkt(std::string& out, const std::string& line) {
  static const char* d = "0123456789abcdef";
  size_t n = line.size() + 4;
  out += d[(n >> 12) & 15];
  out += d[(n >> 8) & 15];
  out += d[(n >> 4) & 15];
  out += d[n & 15];
  out += line;
}

// Every pkt-line in a request: data lines as they are, flushes as "" with `flush` set, and v2's delimiter
// ("0001", between a command's capabilities and its arguments) as "" with `delim` set.
struct Line { std::string text; bool flush = false; bool delim = false; };
Result<std::vector<Line>> read_lines(const std::string& in, size_t& pos, bool stop_after_first_flush) {
  std::vector<Line> out;
  while (pos + 4 <= in.size()) {
    size_t n = 0;
    for (int i = 0; i < 4; ++i) {
      char c = in[pos + i];
      int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (v < 0) return Result<std::vector<Line>>(Status::Corrupt, "not a pkt-line");
      n = n * 16 + (size_t)v;
    }
    if (n == 0) {
      pos += 4;
      out.push_back(Line{"", true});
      if (stop_after_first_flush) return out;
      continue;
    }
    if (n == 1) {
      pos += 4;
      out.push_back(Line{"", false, true});
      continue;
    }
    if (n < 4 || pos + n > in.size()) return Result<std::vector<Line>>(Status::Corrupt, "truncated pkt-line");
    std::string t = in.substr(pos + 4, n - 4);
    if (!t.empty() && t.back() == '\n') t.pop_back();
    out.push_back(Line{t, false});
    pos += n;
  }
  return out;
}

std::string peeled_commit(const Git& repo, const std::string& oid) {
  std::string at = oid;
  for (int i = 0; i < 16; ++i) {
    auto o = repo.read_object(at);
    if (!o.ok() || o->type != "tag") return at;
    std::string body((const char*)o->payload.data(), o->payload.size());
    if (body.rfind("object ", 0) != 0 || body.size() < 47) return at;
    at = body.substr(7, 40);
  }
  return at;
}

bool config_true(const Git& repo, const std::string& key, bool def) {
  auto v = repo.get_config(key);
  if (!v.ok()) return def;
  std::string s = *v;
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s == "true" || s == "yes" || s == "on" || s == "1";
}

// Whether a Git-Protocol header (or GIT_PROTOCOL) asks for v2: colon-separated keys, "version=2" among them.
bool asks_v2(const std::string& git_protocol) {
  for (size_t at = 0; at <= git_protocol.size();) {
    size_t colon = git_protocol.find(':', at);
    if (colon == std::string::npos) colon = git_protocol.size();
    if (git_protocol.compare(at, colon - at, "version=2") == 0) return true;
    at = colon + 1;
  }
  return false;
}

// The tips of the repositories this one borrows objects from (objects/info/alternates), which receive-pack
// advertises as ".have" the way git's does: a pushing client then leaves out everything they reach. A fork whose
// alternates point at its project's main was otherwise sent main's newer history again on a push after merging
// main, and wrote it loose a second time though main already held it.
std::vector<std::string> alternate_tips(const Git& repo) {
  hal::Host& h = repo.host();
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  auto is_oid = [](const std::string& s) { return s.size() == 40 && s.find_first_not_of("0123456789abcdef") == std::string::npos; };
  auto add = [&](const std::string& oid) { if (is_oid(oid) && seen.insert(oid).second) out.push_back(oid); };
  for (const auto& objects : repo.alternate_object_dirs()) {
    std::string dir = objects;
    while (!dir.empty() && dir.back() == '/') dir.pop_back();
    if (dir.size() < 8 || dir.compare(dir.size() - 8, 8, "/objects") != 0) continue;   // a bare store, not a repository's
    dir.resize(dir.size() - 8);
    if (auto f = h.read_file(dir + "/packed-refs"); f.ok()) {
      std::string text((const char*)f->data(), f->size());
      for (size_t at = 0; at < text.size();) {
        size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        if (nl - at > 41 && text[at + 40] == ' ') add(text.substr(at, 40));   // "#" and "^<peeled>" lines fail this
        at = nl + 1;
      }
    }
    std::vector<std::pair<std::string, int>> todo{{dir + "/refs", 0}};
    while (!todo.empty()) {
      auto [path, depth] = todo.back();
      todo.pop_back();
      auto names = h.list_dir(path);
      if (!names.ok()) continue;
      for (auto& n : *names) {
        if (n == "." || n == "..") continue;
        std::string p = path + "/" + n;
        auto st = h.stat(p);
        if (!st.ok()) continue;
        if (st->is_dir) { if (depth < 16) todo.push_back({p, depth + 1}); continue; }
        auto f = h.read_file(p);
        if (f.ok() && f->size() >= 40) add(std::string((const char*)f->data(), 40));   // a symbolic ref ("ref: ...") is not hex
      }
    }
  }
  return out;
}

// Where a shallow request's history stops, as upload-pack works it out: with "deepen N", the commits N deep from
// the wants (from the client's shallow commits, with deepen-relative) are where the client's history stops
// ("shallow", even a root commit), and any commit the client had as shallow that is now above that line is
// "unshallow". Without a depth the client's shallow commits only bound what the pack holds.
struct ShallowPlan {
  bool active = false;                       // a depth, or a client that is shallow already
  std::vector<std::string> shallow, unshallow;
  std::unordered_set<std::string> cut;       // commits whose parents are not sent: the new shallow line
};

ShallowPlan plan_shallow(const Git& repo, const std::vector<std::string>& wants,
                         const std::vector<std::string>& client_shallow, int depth, bool relative) {
  ShallowPlan sp;
  sp.active = depth > 0 || !client_shallow.empty();
  if (!sp.active) return sp;
  const std::unordered_set<std::string> client_shallow_set(client_shallow.begin(), client_shallow.end());
  std::unordered_set<std::string> above;   // reached within the depth, short of the line
  if (depth > 0) {
    std::unordered_map<std::string, int> best;
    std::vector<std::string> level;
    const int limit = relative ? depth + 1 : depth;
    for (auto& from : relative ? client_shallow : wants) {
      std::string c = peeled_commit(repo, from);
      if (!best.count(c)) { best[c] = 1; level.push_back(c); }
    }
    for (int d = 1; !level.empty(); ++d) {   // breadth first: each commit at its least depth
      std::vector<std::string> next;
      for (auto& c : level) {
        if (d >= limit) { sp.cut.insert(c); continue; }
        above.insert(c);
        auto cm = repo.read_commit(c);
        if (!cm.ok()) continue;
        for (auto& p : cm->parents) if (!best.count(p)) { best[p] = d + 1; next.push_back(p); }
      }
      level = std::move(next);
    }
    for (auto& c : sp.cut) if (!client_shallow_set.count(c)) sp.shallow.push_back(c);
    std::sort(sp.shallow.begin(), sp.shallow.end());
  }
  for (auto& s : client_shallow) {
    if (above.count(s)) sp.unshallow.push_back(s);
    else sp.cut.insert(s);
  }
  return sp;
}

// What the pack holds: everything the wants reach that the common commits do not, or, for a shallow client, a
// shallow walk: commits from the wants down to the shallow line, not the client's own (from its haves down to its
// shallow commits), and the trees and blobs they need beyond what it has. The walk goes on through the client's
// commits, to reach the parents of the ones it unshallows.
Result<std::vector<std::string>> objects_to_send(const Git& repo, const std::vector<std::string>& wants,
                                                 const std::vector<std::string>& common,
                                                 const std::vector<std::string>& client_shallow, const ShallowPlan& sp) {
  using R = Result<std::vector<std::string>>;
  if (!sp.active) return git_objects_between(repo, wants, common);
  const std::unordered_set<std::string> client_shallow_set(client_shallow.begin(), client_shallow.end());
  auto walk = [&](const std::vector<std::string>& from, const std::unordered_set<std::string>& stop_at,
                  const std::unordered_set<std::string>& skip) {
    std::vector<std::string> order;
    std::unordered_set<std::string> seen;
    std::vector<std::string> todo;
    for (auto& f : from) todo.push_back(peeled_commit(repo, f));
    while (!todo.empty()) {
      std::string c = todo.back();
      todo.pop_back();
      if (!seen.insert(c).second) continue;
      auto cm = repo.read_commit(c);
      if (!cm.ok()) continue;
      if (!skip.count(c)) order.push_back(c);
      if (stop_at.count(c)) continue;
      for (auto& p : cm->parents) todo.push_back(p);
    }
    return order;
  };
  // Every tree and blob under these commits' trees not already in `seen`, added to it and to `order`.
  auto trees_under = [&](const std::vector<std::string>& commits, std::unordered_set<std::string>& seen,
                         std::vector<std::string>* order) -> Result<void> {
    std::vector<std::string> todo;
    for (auto& c : commits) if (auto cm = repo.read_commit(c); cm.ok()) todo.push_back(cm->tree);
    while (!todo.empty()) {
      std::string tr = todo.back();
      todo.pop_back();
      if (!seen.insert(tr).second) continue;
      if (order) order->push_back(tr);
      auto ents = repo.read_tree(tr);
      if (!ents.ok()) return ents.error();
      for (auto& e : *ents) {
        if (e.mode == 0160000) continue;
        if (e.is_dir()) todo.push_back(e.oid);
        else if (seen.insert(e.oid).second && order) order->push_back(e.oid);
      }
    }
    return Ok();
  };
  std::vector<std::string> objects;
  const auto have_commits = walk(common, client_shallow_set, {});
  const std::unordered_set<std::string> have_set(have_commits.begin(), have_commits.end());
  const auto send_commits = walk(wants, sp.cut, have_set);
  std::unordered_set<std::string> seen;
  std::vector<std::string> tree_objs;
  if (auto s = trees_under(have_commits, seen, nullptr); !s.ok()) return R(s.error());
  if (auto s = trees_under(send_commits, seen, &tree_objs); !s.ok()) return R(s.error());
  for (auto& wnt : wants) if (repo.read_object(wnt).ok() && peeled_commit(repo, wnt) != wnt) objects.push_back(wnt);
  objects.insert(objects.end(), send_commits.begin(), send_commits.end());
  objects.insert(objects.end(), tree_objs.begin(), tree_objs.end());
  return objects;
}

// The pack in side-band frames on band 1, framed straight into the answer, sized once: a frame built as a string
// and then copied in was three copies of the pack. No flush: the caller ends the answer.
void frame_pack(std::string& out, const ByteBuffer& pack, size_t band_max) {
  out.reserve(out.size() + pack.size() + (pack.size() / band_max + 1) * 5 + 8);
  static const char* hexd = "0123456789abcdef";
  for (size_t off = 0; off < pack.size(); off += band_max) {
    size_t n = std::min(band_max, pack.size() - off);
    size_t len = n + 5;
    out += hexd[(len >> 12) & 15]; out += hexd[(len >> 8) & 15]; out += hexd[(len >> 4) & 15]; out += hexd[len & 15];
    out += '\1';
    out.append((const char*)pack.data() + off, n);
  }
}

// include-tag: the annotated tags whose target the pack already carries go in too, so a fetch has them without a
// second request for them. A tag ref whose object sits whole in a pack as something other than a tag is passed over
// without being read: every fetch asks for this, and most tags have long been the client's.
void add_followed_tags(const Git& repo, std::vector<std::string>& objects) {
  auto refs = repo.refs();
  if (!refs.ok()) return;
  std::unordered_set<std::string> sent(objects.begin(), objects.end());
  for (auto& r : *refs) {
    if (r.name.rfind("refs/tags/", 0) != 0 || sent.count(r.oid)) continue;
    if (auto e = repo.packed_entry(r.oid); e.ok() && e->type >= 1 && e->type <= 3) continue;
    std::vector<std::string> chain;
    std::string at = r.oid;
    for (int i = 0; i < 16; ++i) {
      auto o = repo.read_object(at);
      if (!o.ok() || o->type != "tag") break;
      std::string body((const char*)o->payload.data(), o->payload.size());
      if (body.rfind("object ", 0) != 0 || body.size() < 47) { chain.clear(); break; }
      chain.push_back(at);
      at = body.substr(7, 40);
    }
    if (chain.empty() || !sent.count(at)) continue;
    for (auto& t : chain) if (sent.insert(t).second) objects.push_back(t);
  }
}

// ---- protocol v2 (gitprotocol-v2): a capability advertisement with no refs, then one command per request ----

// A partial clone's object filter (git's --filter): "blob:none" leaves every blob out, "blob:limit=<n>[kmg]" the
// blobs bigger than n bytes. A blob the client named in a want is always sent (that is how it fetches one it lacks).
// Returns false for a filter this side does not know.
bool apply_filter(const Git& repo, const std::string& spec, const std::vector<std::string>& wants, std::vector<std::string>& objects) {
  return git_filter_objects(repo, spec, wants, objects);
}
}  // namespace

bool git_filter_objects(const Git& repo, const std::string& spec, const std::vector<std::string>& wants, std::vector<std::string>& objects) {
  if (spec.empty()) return true;
  uint64_t limit = 0;
  bool none = spec == "blob:none";
  if (!none) {
    if (spec.rfind("blob:limit=", 0) != 0) return false;
    std::string n = spec.substr(11);
    if (n.empty()) return false;
    uint64_t mul = 1;
    char last = (char)std::tolower((unsigned char)n.back());
    if (last == 'k') mul = 1024; else if (last == 'm') mul = 1024 * 1024; else if (last == 'g') mul = 1024ull * 1024 * 1024;
    if (mul != 1) n.pop_back();
    for (char ch : n) if (ch < '0' || ch > '9') return false;
    limit = std::strtoull(n.c_str(), nullptr, 10) * mul;
  }
  std::unordered_set<std::string> keep(wants.begin(), wants.end()), blobs;
  std::unordered_set<std::string> in(objects.begin(), objects.end());
  // which of the objects are blobs: what the trees being sent say they are
  for (auto& o : objects) {
    auto t = repo.read_tree(o);
    if (!t.ok()) continue;
    for (auto& e : *t) if (!e.is_dir() && e.mode != 0160000 && in.count(e.oid)) blobs.insert(e.oid);
  }
  std::vector<std::string> out;
  for (auto& o : objects) {
    if (!blobs.count(o) || keep.count(o)) { out.push_back(o); continue; }
    if (none) continue;
    auto b = repo.read_object(o);
    if (b.ok() && b->payload.size() <= limit) out.push_back(o);
  }
  objects.swap(out);
  return true;
}

namespace {

std::string v2_advertisement() {
  std::string out;
  pkt(out, "version 2\n");
  pkt(out, "agent=git/aios\n");
  pkt(out, "ls-refs=unborn\n");
  pkt(out, "fetch=shallow filter\n");   // deepen, deepen-relative, the client's shallow lines, filter; not deepen-since/-not
  pkt(out, "object-format=sha1\n");
  out += "0000";
  return out;
}

std::string v2_error(const std::string& why) {
  std::string out;
  pkt(out, "ERR " + why + "\n");
  return out;
}

// ls-refs: "<oid> <name>[ symref-target:<ref>][ peeled:<oid>]" for each ref under one of the ref-prefixes (all of
// them without any), HEAD first, and with "unborn" a HEAD on a branch with no commits yet as "unborn HEAD".
std::string v2_ls_refs(const Git& repo, const std::vector<Line>& args) {
  bool symrefs = false, peel = false, unborn = false;
  std::vector<std::string> prefixes;
  for (auto& a : args) {
    if (a.flush || a.delim) continue;
    if (a.text == "symrefs") symrefs = true;
    else if (a.text == "peel") peel = true;
    else if (a.text == "unborn") unborn = true;
    else if (a.text.rfind("ref-prefix ", 0) == 0) prefixes.push_back(a.text.substr(11));
  }
  auto listed = [&](const std::string& name) {
    if (prefixes.empty()) return true;
    for (auto& p : prefixes) if (name.rfind(p, 0) == 0) return true;
    return false;
  };
  std::string out;
  const std::string head_ref = repo.head_ref();   // empty when HEAD is detached
  if (listed("HEAD")) {
    const std::string target = symrefs && !head_ref.empty() ? " symref-target:" + head_ref : std::string();
    if (auto head = repo.head_oid(); head.ok()) pkt(out, *head + " HEAD" + target + "\n");
    else if (unborn && !head_ref.empty()) pkt(out, "unborn HEAD" + target + "\n");
  }
  if (auto refs = repo.refs(); refs.ok())
    for (auto& r : *refs) {
      if (!listed(r.name)) continue;
      std::string line = r.oid + " " + r.name;
      // Tags only, as the v0 advertisement peels: reading every branch's commit to learn it is not a tag cost a
      // read per ref for nothing.
      if (peel && r.name.rfind("refs/tags/", 0) == 0)
        if (std::string p = peeled_commit(repo, r.oid); p != r.oid) line += " peeled:" + p;
      pkt(out, line + "\n");
    }
  out += "0000";
  return out;
}

// fetch: wants, haves and (eventually) "done", answered in sections. Until "done" the haves are acknowledged
// ("acknowledgments": NAK, or an ACK for each in common); once one is in common this side is "ready" and the pack
// follows in the same answer, as with multi_ack_detailed's "ready" in v0. Then shallow-info (for a shallow request)
// and the packfile, always in side-band-64k frames.
Result<std::string> v2_fetch(Git& repo, const std::vector<Line>& args) {
  std::vector<std::string> wants, haves, client_shallow;
  std::string filter;
  int depth = 0;
  bool relative = false, done = false, include_tag = false;
  auto oid_arg = [](const std::string& t, size_t at, std::string& oid) {
    if (t.size() < at + 40) return false;
    oid = t.substr(at, 40);
    return true;
  };
  for (auto& a : args) {
    if (a.flush || a.delim) continue;
    const std::string& t = a.text;
    std::string oid;
    if (t.rfind("want ", 0) == 0) {
      if (!oid_arg(t, 5, oid)) return v2_error("upload-pack: malformed want");
      wants.push_back(oid);
    } else if (t.rfind("have ", 0) == 0) {
      if (!oid_arg(t, 5, oid)) return v2_error("upload-pack: malformed have");
      haves.push_back(oid);
    } else if (t.rfind("shallow ", 0) == 0) {
      if (!oid_arg(t, 8, oid)) return v2_error("upload-pack: malformed shallow");
      client_shallow.push_back(oid);
    } else if (t.rfind("deepen ", 0) == 0) {
      depth = std::atoi(t.c_str() + 7);
    } else if (t == "deepen-relative") {
      relative = true;
    } else if (t == "include-tag") {
      include_tag = true;
    } else if (t == "done") {
      done = true;
    } else if (t.rfind("filter ", 0) == 0) {
      filter = t.substr(7);
      while (!filter.empty() && filter.back() == '\n') filter.pop_back();
    } else if (t.rfind("deepen-since ", 0) == 0 || t.rfind("deepen-not ", 0) == 0 ||
               t.rfind("want-ref ", 0) == 0 || t.rfind("packfile-uris ", 0) == 0) {
      // Not advertised, so a client should not send them; one that does is told, not sent a pack that ignores them.
      return v2_error("upload-pack: '" + t.substr(0, t.find(' ')) + "' is not supported");
    }
    // thin-pack, ofs-delta, no-progress, sideband-all, wait-for-done: the pack is whole objects with no progress,
    // which every one of them allows.
  }
  if (wants.empty()) return v2_error("upload-pack: fetch with no wants");
  for (auto& wnt : wants)
    if (!repo.read_object(wnt).ok()) return v2_error("upload-pack: not our ref " + wnt);

  std::vector<std::string> common;
  for (auto& h : haves)
    if (repo.read_object(h).ok()) common.push_back(h);
  std::string out;
  if (!done) {
    pkt(out, "acknowledgments\n");
    if (common.empty()) pkt(out, "NAK\n");
    for (auto& h : common) pkt(out, "ACK " + h + "\n");
    if (common.empty()) {   // not ready: the client sends more haves, or "done"
      out += "0000";
      return out;
    }
    pkt(out, "ready\n");
    out += "0001";
  }
  const ShallowPlan sp = plan_shallow(repo, wants, client_shallow, depth, relative);
  auto objects = objects_to_send(repo, wants, common, client_shallow, sp);
  if (!objects.ok()) return Result<std::string>(objects.error());
  if (include_tag) add_followed_tags(repo, *objects);
  if (!apply_filter(repo, filter, wants, *objects)) return v2_error("upload-pack: unknown filter '" + filter + "'");
  uint64_t counted = 0;
  auto pack = git_build_pack(repo, *objects, counted);
  if (!pack.ok()) return Result<std::string>(pack.error());
  if (sp.active) {
    pkt(out, "shallow-info\n");
    for (auto& s : sp.shallow) pkt(out, "shallow " + s + "\n");
    for (auto& s : sp.unshallow) pkt(out, "unshallow " + s + "\n");
    out += "0001";
  }
  pkt(out, "packfile\n");
  frame_pack(out, *pack, 65515);
  out += "0000";
  return out;
}

// One v2 request: "command=<name>", its capabilities (agent, object-format), a delimiter, its arguments, a flush.
Result<std::string> serve_v2(Git& repo, const std::vector<Line>& lines) {
  const std::string command = lines[0].text.substr(8);
  size_t at = 1;
  for (; at < lines.size() && !lines[at].delim && !lines[at].flush; ++at) {
    const std::string& cap = lines[at].text;
    if (cap.rfind("object-format=", 0) == 0 && cap != "object-format=sha1")
      return v2_error("upload-pack: this repository's objects are sha1, not " + cap.substr(14));
  }
  std::vector<Line> args;
  if (at < lines.size() && lines[at].delim)
    for (++at; at < lines.size() && !lines[at].flush; ++at) args.push_back(lines[at]);
  if (command == "ls-refs") return v2_ls_refs(repo, args);
  if (command == "fetch") return v2_fetch(repo, args);
  return v2_error("upload-pack: unknown command '" + command + "'");
}

}  // namespace

std::string git_advertise_refs(const Git& repo, const std::string& service, bool http_banner, const std::string& git_protocol) {
  // v2 advertises what it can do and no refs (ls-refs lists them), and git http-backend leaves the "# service="
  // banner out of it; receive-pack has no v2 and answers v0 whatever was asked, as git's does.
  if (service == "git-upload-pack" && asks_v2(git_protocol)) return v2_advertisement();
  std::string out;
  if (http_banner) {
    pkt(out, "# service=" + service + "\n");
    out += "0000";
  }
  std::vector<std::pair<std::string, std::string>> lines;   // name, oid
  auto head = repo.head_oid();
  std::string head_ref = repo.head_ref();
  auto refs = repo.refs();
  if (head.ok() && service == "git-upload-pack") lines.push_back({"HEAD", *head});
  if (refs.ok()) for (auto& r : *refs) lines.push_back({r.name, r.oid});
  if (service == "git-receive-pack") {
    std::unordered_set<std::string> own;
    for (auto& [name, oid] : lines) own.insert(oid);
    for (auto& tip : alternate_tips(repo)) if (!own.count(tip)) lines.push_back({".have", tip});
  }
  // No no-thin (receive-pack): a client may send a thin pack, its deltas leaning on objects already here, which is
  // smaller to send. index-pack appends those bases to it and keeps it as one pack (git's --fix-thin).
  std::string caps = service == "git-upload-pack"
                         ? "multi_ack_detailed side-band-64k ofs-delta shallow deepen-relative allow-tip-sha1-in-want allow-reachable-sha1-in-want no-progress filter"
                         : "report-status delete-refs side-band-64k ofs-delta quiet";
  if (service == "git-upload-pack" && !head_ref.empty()) caps += " symref=HEAD:" + head_ref;
  caps += " agent=git/aios";
  if (lines.empty()) {
    pkt(out, std::string(kZero) + " capabilities^{}" + std::string(1, '\0') + caps + "\n");
  } else {
    bool first = true;
    for (auto& [name, oid] : lines) {
      pkt(out, oid + " " + name + (first ? std::string(1, '\0') + caps : std::string()) + "\n");
      first = false;
      if (service == "git-upload-pack" && name.rfind("refs/tags/", 0) == 0) {
        std::string p = peeled_commit(repo, oid);
        if (p != oid) pkt(out, p + " " + name + "^{}\n");
      }
    }
  }
  out += "0000";
  return out;
}

Result<std::string> git_serve_upload_pack(Git& repo, const std::string& in) {
  size_t pos = 0;
  auto lines = read_lines(in, pos, false);
  if (!lines.ok()) return Result<std::string>(lines.error());
  // A v2 request names its command first; a v0 one never starts that way (wants, or a lone flush).
  if (!lines->empty() && !(*lines)[0].flush && (*lines)[0].text.rfind("command=", 0) == 0) return serve_v2(repo, *lines);
  // The wants (and shallow, deepen) up to the first flush; after it the negotiation, kept in order because each
  // have is answered as it is met and each flush ends a round. A stateless request holds one round (or "done"); the
  // live session hands over the whole conversation so far, and writes only the answer past what it already wrote.
  std::vector<std::string> wants, client_shallow;
  std::vector<const Line*> negotiation;
  std::string caps;
  int depth = 0;
  bool in_wants = true;
  std::string filter;
  for (auto& l : *lines) {
    if (!in_wants) { negotiation.push_back(&l); continue; }
    if (l.flush) { in_wants = false; continue; }
    const std::string& t = l.text;
    if (t.rfind("want ", 0) == 0) {
      std::string oid = t.substr(5, 40);
      if (t.size() > 45) caps += " " + t.substr(46);
      wants.push_back(oid);
    } else if (t.rfind("shallow ", 0) == 0) {
      client_shallow.push_back(t.substr(8, 40));
    } else if (t.rfind("deepen ", 0) == 0) {
      depth = std::atoi(t.c_str() + 7);
    } else if (t.rfind("filter ", 0) == 0) {
      filter = t.substr(7);
      while (!filter.empty() && filter.back() == '\n') filter.pop_back();
    }
  }
  const bool sideband = caps.find("side-band-64k") != std::string::npos || caps.find("side-band") != std::string::npos;
  const size_t band_max = caps.find("side-band-64k") != std::string::npos ? 65515 : 995;
  const bool relative = (" " + caps + " ").find(" deepen-relative ") != std::string::npos;
  for (auto& wnt : wants)
    if (!repo.read_object(wnt).ok()) return Result<std::string>(Status::NotFound, "upload-pack: not our ref " + wnt);

  std::string out;
  // The shallow section is sent, flush-ended, on every request that has a depth.
  const ShallowPlan sp = plan_shallow(repo, wants, client_shallow, depth, relative);
  if (depth > 0) {
    for (auto& s : sp.shallow) pkt(out, "shallow " + s);
    for (auto& s : sp.unshallow) pkt(out, "unshallow " + s);
    out += "0000";
  }

  // The answers are git's get_common_commits, line for line, since the client's negotiator counts on each of them:
  // a have this side holds is "ACK <oid> common" (multi_ack_detailed), "ACK <oid> continue" (multi_ack), or, without
  // either, a plain ACK for the first object in common only. A have this side lacks is answered only once every want
  // already reaches what the client has (ok_to_give_up): "ACK <oid> ready", or "continue". A round's flush adds
  // "ACK <last common> ready" when that round met only commons and the pack could be made from here, then NAK (always
  // with multi_ack; without it only when nothing is in common). "done" is answered by one final ACK of the last common
  // have, or NAK when there is none; the commons were acknowledged as they came and are not repeated.
  const std::string padded = " " + caps + " ";
  const int multi_ack = padded.find(" multi_ack_detailed ") != std::string::npos ? 2
                        : padded.find(" multi_ack ") != std::string::npos   ? 1
                                                                            : 0;
  std::unordered_set<std::string> they_have;   // git's THEY_HAVE: each have this side holds, and a commit's parents
  size_t have_obj = 0;                         // the haves recorded as such (a commit flagged as a parent first is not)
  uint64_t oldest_have = 0;                    // the oldest committer time among the commit haves
  std::vector<std::string> common;             // what the pack leaves out: every have this side holds
  std::string last_hex;
  bool got_common = false, got_other = false, done = false;
  // ok_to_give_up: every want is a commit the client has, or reaches one through commits no older than its oldest
  // have (older history is not walked, as git's can_all_from_reach_with_flag does not), or is no commit at all, which
  // history cannot settle. Kept until `they_have` grows, since it is asked of every unknown have.
  size_t give_up_at = SIZE_MAX;
  bool give_up_cached = false;
  auto ok_to_give_up = [&]() {
    if (have_obj == 0) return false;
    if (give_up_at == they_have.size()) return give_up_cached;
    give_up_at = they_have.size();
    give_up_cached = false;
    for (auto& wnt : wants) {
      std::string c = peeled_commit(repo, wnt);
      if (they_have.count(c)) continue;
      auto cm = repo.read_commit(c);
      if (!cm.ok()) continue;
      bool found = false;
      std::unordered_set<std::string> seen{c};
      std::vector<GitCommit> todo{std::move(*cm)};
      while (!found && !todo.empty()) {
        GitCommit x = std::move(todo.back());
        todo.pop_back();
        for (auto& p : x.parents) {
          if (they_have.count(p)) { found = true; break; }
          if (!seen.insert(p).second) continue;
          auto pc = repo.read_commit(p);
          if (pc.ok() && pc->committer_time >= oldest_have) todo.push_back(std::move(*pc));
        }
      }
      if (!found) return false;
    }
    give_up_cached = true;
    return true;
  };
  for (const Line* l : negotiation) {
    if (l->flush) {
      if (multi_ack == 2 && got_common && !got_other && ok_to_give_up()) pkt(out, "ACK " + last_hex + " ready\n");
      if (have_obj == 0 || multi_ack) pkt(out, "NAK\n");
      got_common = got_other = false;
      continue;
    }
    const std::string& t = l->text;
    if (t == "done") { done = true; break; }
    if (t.rfind("have ", 0) != 0) continue;
    const std::string oid = t.substr(5, 40);
    auto o = repo.read_object(oid);
    if (!o.ok()) {
      got_other = true;
      if (multi_ack && ok_to_give_up()) pkt(out, "ACK " + oid + (multi_ack == 2 ? " ready\n" : " continue\n"));
      continue;
    }
    got_common = true;
    last_hex = oid;
    // git's do_got_oid: a commit already flagged (a parent of an earlier have) is acknowledged but not recorded.
    bool record = !they_have.count(oid);
    if (o->type == "commit" && record) {
      auto cm = repo.read_commit(oid);
      if (cm.ok()) {
        if (!oldest_have || cm->committer_time < oldest_have) oldest_have = cm->committer_time;
        for (auto& p : cm->parents) they_have.insert(p);
      }
    }
    if (record) {
      they_have.insert(oid);
      ++have_obj;
    }
    common.push_back(oid);
    if (multi_ack == 2) pkt(out, "ACK " + oid + " common\n");
    else if (multi_ack) pkt(out, "ACK " + oid + " continue\n");
    else if (have_obj == 1) pkt(out, "ACK " + oid + "\n");
  }
  if (!done) return out;   // the end of a round (or of the wants): the client says more
  if (have_obj == 0) pkt(out, "NAK\n");
  else if (multi_ack) pkt(out, "ACK " + last_hex + "\n");

  auto objects = objects_to_send(repo, wants, common, client_shallow, sp);
  if (!objects.ok()) return Result<std::string>(objects.error());
  if (!apply_filter(repo, filter, wants, *objects)) return Result<std::string>(Status::Invalid, "upload-pack: unknown filter '" + filter + "'");
  uint64_t counted = 0;
  auto pack = git_build_pack(repo, *objects, counted);
  if (!pack.ok()) return Result<std::string>(pack.error());
  if (!sideband) {
    out.append((const char*)pack->data(), pack->size());
    return out;
  }
  frame_pack(out, *pack, band_max);
  out += "0000";
  return out;
}

Result<std::string> git_serve_receive_pack(Git& repo, const std::string& in, std::vector<ReceivedRef>* applied,
                                           const GitPreReceive& pre_receive) {
  size_t pos = 0;
  auto lines = read_lines(in, pos, true);
  if (!lines.ok()) return Result<std::string>(lines.error());
  struct Cmd { std::string old_oid, new_oid, ref, result; };
  std::vector<Cmd> cmds;
  std::string caps;
  for (auto& l : *lines) {
    if (l.flush) break;
    std::string t = l.text;
    size_t nul = t.find('\0');
    if (nul != std::string::npos) { caps = t.substr(nul + 1); t = t.substr(0, nul); }
    if (t.size() < 83) return Result<std::string>(Status::Corrupt, "receive-pack: malformed command " + t);
    cmds.push_back(Cmd{t.substr(0, 40), t.substr(41, 40), t.substr(82), ""});
  }
  const bool sideband = caps.find("side-band-64k") != std::string::npos;
  std::string unpack = "ok";
  bool needs_pack = false;
  for (auto& c : cmds) if (c.new_oid != kZero) needs_pack = true;
  if (needs_pack && pos < in.size()) {
    auto r = git_index_pack(repo, ByteSpan((const uint8_t*)in.data() + pos, in.size() - pos));
    if (!r.ok()) unpack = r.error().str();
  }
  const bool deny_ff = config_true(repo, "receive.denynonfastforwards", false);
  const bool deny_deletes = config_true(repo, "receive.denydeletes", false);
  std::string deny_current = "refuse";
  if (auto v = repo.get_config("receive.denycurrentbranch"); v.ok()) deny_current = *v;
  // the pre-receive hook sees what would happen (git runs it after unpacking, before any ref moves)
  std::string declined;
  if (pre_receive && unpack == "ok") {
    std::vector<ReceivedRef> proposed;
    for (auto& c : cmds) proposed.push_back(ReceivedRef{c.old_oid, c.new_oid, c.ref});
    declined = pre_receive(proposed);
  }
  for (auto& c : cmds) {
    if (unpack != "ok") { c.result = "unpacker error"; continue; }
    if (!declined.empty()) { c.result = "pre-receive hook declined"; continue; }
    if (!Git::valid_ref_name(c.ref) || c.ref.rfind("refs/", 0) != 0) { c.result = "funny refname"; continue; }
    auto cur = repo.ref_oid(c.ref);
    std::string now = cur.ok() ? *cur : std::string(kZero);
    if (now != c.old_oid) { c.result = "stale info"; continue; }
    if (!repo.bare() && c.ref == repo.head_ref() && deny_current != "ignore" && deny_current != "warn" &&
        deny_current != "false") {
      c.result = "branch is currently checked out";
      continue;
    }
    if (c.new_oid == kZero) {
      if (deny_deletes) { c.result = "deletion prohibited"; continue; }
      auto r = repo.delete_ref(c.ref);
      c.result = r.ok() ? "" : r.error().str();
      continue;
    }
    if (!repo.read_object(c.new_oid).ok()) { c.result = "missing necessary objects"; continue; }
    if (deny_ff && c.old_oid != kZero) {
      auto anc = git_is_ancestor(repo, c.old_oid, c.new_oid);
      if (!anc.ok() || !*anc) { c.result = "non-fast-forward"; continue; }
    }
    auto r = repo.update_ref(c.ref, c.new_oid);
    c.result = r.ok() ? "" : r.error().str();
  }
  if (applied) for (auto& c : cmds) if (c.result.empty()) applied->push_back(ReceivedRef{c.old_oid, c.new_oid, c.ref});
  std::string report;
  pkt(report, "unpack " + unpack + "\n");
  for (auto& c : cmds) pkt(report, c.result.empty() ? "ok " + c.ref + "\n" : "ng " + c.ref + " " + c.result + "\n");
  report += "0000";
  if (caps.find("report-status") == std::string::npos) return std::string();
  if (!sideband) return report;
  std::string out;
  // what the hook said, on the progress band: the client prints it as "remote: ..."
  for (size_t off = 0; off < declined.size(); off += 65515) pkt(out, std::string(1, '\2') + declined.substr(off, 65515));
  for (size_t off = 0; off < report.size(); off += 65515) {
    size_t n = std::min<size_t>(65515, report.size() - off);
    pkt(out, std::string(1, '\1') + report.substr(off, n));
  }
  out += "0000";
  return out;
}

}  // namespace aios::userland
