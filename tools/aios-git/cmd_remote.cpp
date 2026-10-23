// clone, fetch, pull, push, ls-remote, remote, bundle, http-backend, upload-pack, receive-pack:
// moving history between repositories (network, another repository on this machine, or a bundle file).
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdio>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <filesystem>
#include <functional>
#include <sstream>
#include <system_error>

#include "aios/hal/host.hpp"
#include "aios/hal/process.hpp"
#include "cli_rewrite.hpp"

namespace aios_git {

using namespace aios;
using namespace aios::userland;

namespace {

const std::string kZero(40, '0');

// ---------------------------------------------------------------------------
// where the other side is
// ---------------------------------------------------------------------------
struct Bundle {
  std::vector<std::pair<std::string, std::string>> prereqs;   // oid, comment
  std::vector<std::pair<std::string, std::string>> refs;      // oid, ref
  std::string pack;
};

bool read_bundle(Ctx& c, const std::string& path, Bundle& b, std::string* err) {
  auto r = path == "-" ? Result<ByteBuffer>(Status::Invalid, "") : c.host().read_file(path);
  std::string data;
  if (path == "-") data = read_all(c.in);
  else if (r.ok()) data.assign((const char*)r->data(), r->size());
  else { if (err) *err = "could not open '" + path + "'"; return false; }
  size_t pos = data.find('\n');
  if (pos == std::string::npos) { if (err) *err = "'" + path + "' does not look like a v2 or v3 bundle file"; return false; }
  std::string sig = data.substr(0, pos);
  if (sig != "# v2 git bundle" && sig != "# v3 git bundle") { if (err) *err = "'" + path + "' does not look like a v2 or v3 bundle file"; return false; }
  ++pos;
  while (pos < data.size()) {
    size_t nl = data.find('\n', pos);
    if (nl == std::string::npos) { if (err) *err = "unterminated bundle header"; return false; }
    std::string line = data.substr(pos, nl - pos);
    pos = nl + 1;
    if (line.empty()) break;
    if (line[0] == '@') continue;   // v3 capabilities
    if (line[0] == '-') {
      std::string oid = line.substr(1, 40);
      b.prereqs.push_back({oid, line.size() > 42 ? line.substr(42) : ""});
      continue;
    }
    if (line.size() < 42) { if (err) *err = "unrecognized header: " + line; return false; }
    b.refs.push_back({line.substr(0, 40), line.substr(41)});
  }
  b.pack = data.substr(pos);
  return true;
}

bool is_url(const std::string& s) { return s.find("://") != std::string::npos; }
bool is_scp(const std::string& s) {
  size_t colon = s.find(':');
  if (colon == std::string::npos || colon == 1) return false;   // C:/...
  size_t slash = s.find('/');
  return slash == std::string::npos || colon < slash;
}
std::string strip_file_scheme(const std::string& s) {
  if (s.rfind("file://", 0) != 0) return s;
  std::string p = s.substr(7);
  if (p.size() > 2 && p[0] == '/' && p[2] == ':') p = p.substr(1);   // file:///C:/x
  return p;
}

// A local repository opened as the other side.
std::unique_ptr<Git> open_local(Ctx& c, const std::string& path) {
  auto g = std::make_unique<Git>(*c.kernel);
  auto& h = c.host();
  std::string p = c.abs_path(path);
  if (h.exists(p + "/.git")) {
    if (g->open(p).ok()) return g;
  }
  if (h.exists(p + "/HEAD") && h.exists(p + "/objects")) {
    if (g->open_dirs(p, "").ok()) return g;
  }
  return nullptr;
}

struct Side {
  enum Kind { Local, BundleFile, Network } kind = Network;
  std::string url;          // what the user named (for messages)
  std::string location;     // local path, bundle path, or URL
  std::unique_ptr<Git> local;
  Bundle bundle;
  std::vector<GitRemoteRef> refs;   // what it has, with peeled tags as "name^{}"
  std::string head_ref;             // what HEAD names, when known
  GitRemoteOptions opt;
};

std::string display_url(const std::string& url) {
  // credentials never appear in output
  size_t scheme = url.find("://");
  if (scheme == std::string::npos) return url;
  size_t at = url.find('@', scheme + 3), slash = url.find('/', scheme + 3);
  if (at != std::string::npos && (slash == std::string::npos || at < slash)) return url.substr(0, scheme + 3) + url.substr(at + 1);
  return url;
}

Side open_side(Ctx& c, const std::string& url, bool must_exist = true) {
  Side s;
  s.url = url;
  std::string loc = strip_file_scheme(url);
  if (!is_url(loc) && !is_scp(loc)) {
    std::string abs = c.abs_path(loc);
    auto st = c.host().stat(abs);
    if (st.ok() && !st->is_dir) {
      std::string err;
      if (!read_bundle(c, abs, s.bundle, &err)) die(c, err);
      s.kind = Side::BundleFile;
      s.location = abs;
      for (auto& [oid, ref] : s.bundle.refs) s.refs.push_back(GitRemoteRef{ref, oid});
      return s;
    }
    s.kind = Side::Local;
    s.location = abs;
    s.local = open_local(c, abs);
    if (!s.local) {
      if (!must_exist) return s;
      c.err << "fatal: '" << url << "' does not appear to be a git repository\n"
               "fatal: Could not read from remote repository.\n\n"
               "Please make sure you have the correct access rights\nand the repository exists.\n";
      throw Exit{128};
    }
    if (auto h = s.local->head_oid(); h.ok()) s.refs.push_back(GitRemoteRef{"HEAD", *h});
    s.head_ref = s.local->head_ref();
    auto rs = s.local->refs();
    if (rs.ok()) {
      for (auto& r : *rs) {
        s.refs.push_back(GitRemoteRef{r.name, r.oid});
        if (r.name.rfind("refs/tags/", 0) == 0) {
          auto o = s.local->read_object(r.oid);
          if (o.ok() && o->type == "tag") {
            std::string body((const char*)o->payload.data(), o->payload.size());
            if (body.rfind("object ", 0) == 0) s.refs.push_back(GitRemoteRef{r.name + "^{}", body.substr(7, 40)});
          }
        }
      }
    }
    return s;
  }
  s.kind = Side::Network;
  s.location = url;
  s.opt.http_headers = http_headers_for(c, url);
  // ssh's own warnings (a host key newly recorded, a key file it could not load) reach the terminal as git shows them
  s.opt.ssh_notice = [&c](const std::string& m) { c.err << m << "\n"; };
  return s;
}

void network_ls(Ctx& c, Side& s) {
  GitRemote remote(*c.kernel);
  auto r = remote.ls_remote(s.location, s.opt);
  if (!r.ok()) {
    c.err << "fatal: unable to access '" << display_url(s.url) << "': " << r.error().message << "\n";
    throw Exit{128};
  }
  s.refs = r->refs;
  if (!r->default_branch.empty()) s.head_ref = r->default_branch;
}

// "main" on the remote side: which advertised ref it is (git's rules: refs/<x>, refs/tags/<x>, refs/heads/<x>, ...)
std::string match_remote_ref(const std::vector<GitRemoteRef>& refs, const std::string& name) {
  const std::vector<std::string> forms = {name, "refs/" + name, "refs/tags/" + name, "refs/heads/" + name, "refs/remotes/" + name,
                                          "refs/remotes/" + name + "/HEAD"};
  for (auto& f : forms)
    for (auto& r : refs) if (r.name == f) return r.name;
  return "";
}
std::string oid_of(const std::vector<GitRemoteRef>& refs, const std::string& name) {
  for (auto& r : refs) if (r.name == name) return r.oid;
  return "";
}

// ---------------------------------------------------------------------------
// refspecs
// ---------------------------------------------------------------------------
struct Refspec { std::string src, dst; bool force = false; bool pattern = false; };
Refspec parse_refspec(const std::string& s) {
  Refspec r;
  std::string t = s;
  if (!t.empty() && t[0] == '+') { r.force = true; t = t.substr(1); }
  size_t colon = t.find(':');
  if (colon == std::string::npos) r.src = t;
  else { r.src = t.substr(0, colon); r.dst = t.substr(colon + 1); }
  r.pattern = r.src.find('*') != std::string::npos;
  return r;
}
bool match_pattern(const std::string& pat, const std::string& name, std::string& mid) {
  size_t star = pat.find('*');
  if (star == std::string::npos) return false;
  std::string pre = pat.substr(0, star), post = pat.substr(star + 1);
  if (name.size() < pre.size() + post.size() || name.rfind(pre, 0) != 0 || name.compare(name.size() - post.size(), post.size(), post) != 0) return false;
  mid = name.substr(pre.size(), name.size() - pre.size() - post.size());
  return true;
}
std::string apply_pattern(const std::string& pat, const std::string& mid) {
  size_t star = pat.find('*');
  return star == std::string::npos ? pat : pat.substr(0, star) + mid + pat.substr(star + 1);
}

// What a fetch lands: remote ref -> local ref ("" = FETCH_HEAD only), forced or not.
struct Landing { std::string remote_ref, oid, local_ref; bool force = false; bool for_merge = false; };

// ---------------------------------------------------------------------------
// moving objects
// ---------------------------------------------------------------------------
// A local clone of a whole history takes the other repository's object store as files, as git does: a hard link where
// the file system allows (nothing copied, nothing unpacked: a 100 MB pack costs nothing), a byte copy where it does not
// or with --no-hardlinks. Packs and their indexes, and every loose object. A source that borrows from other stores
// (objects/info/alternates) has theirs taken the same way, so the clone stands on its own. false when it cannot: then
// objects are moved one by one instead.
bool take_object_store(const std::string& from, const std::string& to, bool links) {
  // The store's shape is known: objects/xx/<38 hex> and objects/pack/*. Each
  // directory is read once and each file linked by name; a recursive walk
  // with a relative path computed per file (4,000 canonicalisations) was a
  // fifth of a local clone.
  namespace fs = std::filesystem;
  std::error_code ec;
  auto take_dir = [&](const std::string& sub) -> bool {
    const std::string src_dir = from + "/" + sub;
    fs::directory_iterator it(src_dir, ec);
    if (ec) { ec.clear(); return true; }   // no such directory on the source
    const std::string dst_dir = to + "/" + sub;
    fs::create_directories(dst_dir, ec);
    if (ec) return false;
    for (fs::directory_iterator end; it != end; it.increment(ec)) {
      if (ec) return false;
      if (!it->is_regular_file(ec)) continue;
      const std::string name = it->path().filename().string();
      const std::string dest = dst_dir + "/" + name;
      if (links) {
        fs::create_hard_link(it->path(), dest, ec);
        if (!ec) continue;
        if (ec == std::errc::file_exists) { ec.clear(); continue; }
        ec.clear();
      }
      if (fs::exists(dest, ec)) continue;
      fs::copy_file(it->path(), dest, ec);
      if (ec) return false;
    }
    return true;
  };
  fs::directory_iterator top(from, ec);
  if (ec) return false;
  for (fs::directory_iterator end; top != end; top.increment(ec)) {
    if (ec) return false;
    if (!top->is_directory(ec)) continue;
    const std::string name = top->path().filename().string();
    if (name == "info") continue;   // the source's own notes about itself
    if (!take_dir(name)) return false;
  }
  return true;
}

// Brings `wants` into the local repository from the other side.
// `exact`: the objects named and nothing else, whatever this side already has (a partial clone's lazy fetch: the
// blobs it lacks are reachable from commits it has, so nothing may be left out for being "had").
void fetch_objects(Ctx& c, Side& s, const std::vector<std::string>& wants, int depth, const std::string& filter = "",
                   bool exact = false) {
  std::vector<std::string> missing;
  for (auto& w : wants) if (!w.empty() && !c.repo->has_object(w)) missing.push_back(w);
  if (missing.empty()) return;
  if (s.kind == Side::Local && exact) {
    auto n = git_copy_objects(*s.local, *c.repo, missing);
    if (!n.ok()) die(c, "could not fetch from the promisor remote: " + n.error().message);
    return;
  }
  if (s.kind == Side::Local) {
    if (depth > 0) unsupported(c, "--depth with a local repository");
    std::vector<std::string> stop;
    for (auto& [n, o] : all_refs(c)) if (s.local->has_object(o)) stop.push_back(o);
    if (auto h = read_ref(c, "HEAD")) if (s.local->has_object(*h)) stop.push_back(*h);
    auto objs = git_objects_between(*s.local, missing, stop);
    if (!objs.ok()) die(c, "could not read from the other repository: " + objs.error().message);
    if (!filter.empty() && !git_filter_objects(*s.local, filter, missing, *objs)) die(c, "invalid filter-spec '" + filter + "'");
    auto n = git_copy_objects(*s.local, *c.repo, *objs);
    if (!n.ok()) die(c, "could not copy objects: " + n.error().message);
    return;
  }
  if (s.kind == Side::BundleFile) {
    for (auto& [oid, comment] : s.bundle.prereqs)
      if (!c.repo->has_object(oid)) {
        c.err << "error: Repository lacks these prerequisite commits:\nerror: " << oid << " \n";
        throw Exit{1};
      }
    if (!s.bundle.pack.empty()) {
      auto r = git_index_pack(*c.repo, ByteSpan((const uint8_t*)s.bundle.pack.data(), s.bundle.pack.size()));
      if (!r.ok()) die(c, "index-pack failed: " + r.error().message);
    }
    return;
  }
  GitRemote remote(*c.kernel);
  GitRemoteOptions o = s.opt;
  o.depth = depth;
  o.filter = filter;
  o.no_haves = exact;
  o.select = [&](const std::vector<GitRemoteRef>&) { return missing; };
  auto r = remote.fetch(*c.repo, s.location, o);
  if (!r.ok()) {
    c.err << "fatal: unable to access '" << display_url(s.url) << "': " << r.error().message << "\n";
    throw Exit{128};
  }
}

}  // namespace

// A partial clone's objects come from its promisor remote when something reads one that is not here.
void install_promisor(Ctx& c) {
  auto remote = c.config.get("extensions.partialclone");
  if (!remote || remote->empty() || !c.repo) return;
  std::string name = *remote;
  c.repo->set_missing_object_hook([&c, name](const std::vector<std::string>& oids) -> bool {
    std::string url = remote_url(c, name);
    if (url.empty()) return false;
    Side s = open_side(c, url);
    fetch_objects(c, s, oids, 0, "", true);
    return true;
  });
}

namespace {

// ---------------------------------------------------------------------------
// fetch
// ---------------------------------------------------------------------------
struct FetchResult { bool ok = true; std::vector<Landing> landed; };

std::string ref_desc(const std::string& ref) {
  if (ref.rfind("refs/heads/", 0) == 0) return "branch '" + ref.substr(11) + "'";
  if (ref.rfind("refs/tags/", 0) == 0) return "tag '" + ref.substr(10) + "'";
  if (ref == "HEAD") return "";
  return "'" + ref + "'";
}

FetchResult do_fetch(Ctx& c, const std::string& remote_arg, const std::vector<std::string>& refspec_args, bool quiet, bool force_all,
                     int depth, const std::string& tags_mode, bool prune, bool write_fetch_head, bool update_head_ok,
                     bool deepen_relative = false) {
  bool named = false;
  std::string url = remote_url(c, remote_arg, &named);
  if (!named && !c.config.get("remote." + remote_arg + ".url") && remote_arg.find_first_of("/\\:.") == std::string::npos && !c.host().exists(c.abs_path(remote_arg)))
    die(c, "'" + remote_arg + "' does not appear to be a git repository");
  Side s = open_side(c, url);
  if (s.kind == Side::Network) {
    // one conversation: the advertisement arrives in the fetch, and the refspecs are matched against it
  }
  std::vector<Refspec> specs;
  bool from_config = false;
  for (auto& a : refspec_args) specs.push_back(parse_refspec(a));
  if (specs.empty()) {
    if (named) {
      for (auto& f : c.config.get_all("remote." + remote_arg + ".fetch")) specs.push_back(parse_refspec(f));
      from_config = true;
    }
    if (specs.empty()) specs.push_back(parse_refspec("HEAD"));
  }
  std::vector<Refspec> configured;
  if (named) for (auto& f : c.config.get_all("remote." + remote_arg + ".fetch")) configured.push_back(parse_refspec(f));
  std::string tags = tags_mode;
  if (tags.empty() && named) {
    auto t = c.config.get("remote." + remote_arg + ".tagopt");
    if (t && *t == "--no-tags") tags = "no";
    else if (t && *t == "--tags") tags = "all";
  }
  if (tags.empty()) tags = (from_config || named) ? "follow" : "no";

  FetchResult res;
  std::vector<Landing> plan;
  auto plan_from = [&](const std::vector<GitRemoteRef>& refs) {
    plan.clear();
    std::set<std::string> taken;
    for (size_t si = 0; si < specs.size(); ++si) {
      auto& sp = specs[si];
      if (sp.pattern) {
        for (auto& r : refs) {
          if (r.name.size() > 3 && r.name.compare(r.name.size() - 3, 3, "^{}") == 0) continue;
          std::string mid;
          if (!match_pattern(sp.src, r.name, mid)) continue;
          plan.push_back(Landing{r.name, r.oid, sp.dst.empty() ? "" : apply_pattern(sp.dst, mid), sp.force || force_all, false});
        }
        continue;
      }
      std::string src = sp.src;
      bool is_oid = src.size() == 40 && std::all_of(src.begin(), src.end(), [](char ch) { return std::isxdigit((unsigned char)ch); });
      std::string name = is_oid ? src : match_remote_ref(refs, src.empty() ? "HEAD" : src);
      if (name.empty()) {
        c.err << "fatal: couldn't find remote ref " << src << "\n";
        throw Exit{128};
      }
      std::string oid = is_oid ? src : oid_of(refs, name);
      std::string dst = sp.dst;
      if (!dst.empty() && dst.rfind("refs/", 0) != 0) {
        if (name.rfind("refs/heads/", 0) == 0) dst = "refs/heads/" + dst;
        else if (name.rfind("refs/tags/", 0) == 0) dst = "refs/tags/" + dst;
        else dst = "refs/heads/" + dst;
      }
      Landing l{name, oid, dst, sp.force || force_all, !from_config};
      plan.push_back(l);
      // git also updates the configured remote-tracking ref for a ref fetched by name
      if (dst.empty() && !from_config)
        for (auto& cf : configured) {
          std::string mid;
          if (cf.pattern && match_pattern(cf.src, name, mid)) { plan.push_back(Landing{name, oid, apply_pattern(cf.dst, mid), cf.force, false}); break; }
          if (!cf.pattern && cf.src == name && !cf.dst.empty()) { plan.push_back(Landing{name, oid, cf.dst, cf.force, false}); break; }
        }
    }
    // tags: all of them, or those pointing at something being fetched or already here
    if (tags != "no") {
      std::set<std::string> fetched;
      for (auto& l : plan) fetched.insert(l.oid);
      for (auto& r : refs) {
        if (r.name.rfind("refs/tags/", 0) != 0 || (r.name.size() > 3 && r.name.compare(r.name.size() - 3, 3, "^{}") == 0)) continue;
        bool want = tags == "all";
        // following: a tag this repository already has by that name is left alone (find_non_local_tags)
        if (!want && read_ref(c, r.name)) continue;
        if (!want) {
          std::string peeled = oid_of(refs, r.name + "^{}");
          std::string target = peeled.empty() ? r.oid : peeled;
          want = fetched.count(target) || c.repo->has_object(target);
        }
        if (!want) continue;
        bool dup = false;
        for (auto& l : plan) if (l.local_ref == r.name) dup = true;
        if (!dup) plan.push_back(Landing{r.name, r.oid, r.name, false, false});
      }
    }
    // a for-merge entry, when the configured refspecs were used: the branch's own upstream
    if (from_config) {
      std::string b = current_branch(c);
      auto merge = c.config.get("branch." + b + ".merge");
      auto remote = c.config.get("branch." + b + ".remote");
      for (auto& l : plan) if (merge && remote && *remote == remote_arg && l.remote_ref == *merge) l.for_merge = true;
    }
    (void)taken;
  };
  std::vector<std::string> wants;
  if (s.kind == Side::Network) {
    GitRemote remote(*c.kernel);
    GitRemoteOptions o = s.opt;
    o.depth = depth;
    o.deepen_relative = deepen_relative;
    o.include_tag = tags == "follow";
    std::vector<GitRemoteRef> adv;
    o.select = [&](const std::vector<GitRemoteRef>& refs) {
      adv = refs;
      plan_from(refs);
      std::vector<std::string> w;
      for (auto& l : plan) w.push_back(l.oid);
      return w;
    };
    auto r = remote.fetch(*c.repo, s.location, o);
    if (!r.ok()) {
      if (r.status() == Status::NotFound && !adv.empty()) {}
      c.err << "fatal: unable to access '" << display_url(url) << "': " << r.error().message << "\n";
      throw Exit{128};
    }
    s.refs = adv;
    if (!r->default_branch.empty()) s.head_ref = r->default_branch;
  } else {
    plan_from(s.refs);
    for (auto& l : plan) wants.push_back(l.oid);
    fetch_objects(c, s, wants, depth);
  }
  // backfill_tags: a tag not here whose object came with the fetch (include-tag) and whose target is now here too
  if (tags == "follow") {
    std::set<std::string> planned;
    for (auto& l : plan) planned.insert(l.local_ref);
    std::vector<std::string> tag_wants;
    for (auto& r : s.refs) {
      if (r.name.rfind("refs/tags/", 0) != 0 || (r.name.size() > 3 && r.name.compare(r.name.size() - 3, 3, "^{}") == 0)) continue;
      if (planned.count(r.name) || read_ref(c, r.name)) continue;
      std::string peeled = oid_of(s.refs, r.name + "^{}");
      const std::string target = peeled.empty() ? r.oid : peeled;
      if (!c.repo->has_object(target)) continue;
      if (!c.repo->has_object(r.oid)) { tag_wants.push_back(r.oid); if (s.kind == Side::Network) continue; }
      plan.push_back(Landing{r.name, r.oid, r.name, false, false});
    }
    if (s.kind != Side::Network && !tag_wants.empty()) fetch_objects(c, s, tag_wants, 0);
  }
  // land the refs
  std::ostringstream report;
  std::string fetch_head;
  // git shows the url without credentials, trailing slashes or ".git", in its report and in FETCH_HEAD
  std::string shown_url = display_url(url);
  while (shown_url.size() > 1 && shown_url.back() == '/') shown_url.pop_back();
  if (shown_url.size() > 4 && shown_url.compare(shown_url.size() - 4, 4, ".git") == 0) shown_url.resize(shown_url.size() - 4);
  struct ReportLine { std::string flag, summary, from, to, note; };
  std::vector<ReportLine> lines;
  auto line = [&](const std::string& flag, const std::string& summary, const std::string& from, const std::string& to, const std::string& note) {
    lines.push_back(ReportLine{flag, summary, from, to, note});
  };
  auto short_of = [](const std::string& r) {
    for (const char* pre : {"refs/heads/", "refs/tags/", "refs/remotes/"})
      if (r.rfind(pre, 0) == 0) return r.substr(std::strlen(pre));
    return r;
  };
  std::string cur_ref = c.repo->head_ref();
  // FETCH_HEAD: for-merge entries first
  std::vector<Landing> ordered = plan;
  std::stable_sort(ordered.begin(), ordered.end(), [](const Landing& a, const Landing& b) { return a.for_merge && !b.for_merge; });
  std::set<std::string> written_fh;
  for (auto& l : ordered) {
    if (l.local_ref.rfind("refs/tags/", 0) == 0 && l.remote_ref == l.local_ref && !l.for_merge && tags != "all") {}
    std::string key = l.remote_ref + " " + l.oid;
    if (written_fh.count(key)) continue;
    if (!l.local_ref.empty() && !l.for_merge && l.local_ref.rfind("refs/tags/", 0) == 0 && from_config) {}
    written_fh.insert(key);
    std::string desc = ref_desc(l.remote_ref);
    fetch_head += l.oid + "\t" + (l.for_merge ? "" : "not-for-merge") + "\t" + (desc.empty() ? "" : desc + " of ") + shown_url + "\n";
  }
  // store_updated_refs: one pass per FETCH_HEAD status, the refs to be merged first, and the report in that order too
  for (auto& l : ordered) {
    if (l.local_ref.empty()) {
      line("*", l.remote_ref == "HEAD" ? "branch" : l.remote_ref.rfind("refs/tags/", 0) == 0 ? "tag" : "branch",
           l.remote_ref == "HEAD" ? "HEAD" : short_of(l.remote_ref), "FETCH_HEAD", "");
      continue;
    }
    auto cur = read_ref(c, l.local_ref);
    if (cur && *cur == l.oid) continue;
    if (l.local_ref == cur_ref && !c.repo->bare() && !update_head_ok) {
      c.err << "fatal: refusing to fetch into branch '" << l.local_ref << "' checked out at '" << c.top << "'\n";
      throw Exit{128};
    }
    bool is_tag = l.local_ref.rfind("refs/tags/", 0) == 0;
    if (!cur) {
      update_ref_checked(c, l.local_ref, l.oid, std::nullopt, "fetch: storing head");
      line("*", is_tag ? "[new tag]" : l.local_ref.rfind("refs/heads/", 0) == 0 || l.local_ref.rfind("refs/remotes/", 0) == 0 ? "[new branch]" : "[new ref]",
           short_of(l.remote_ref), short_of(l.local_ref), "");
      continue;
    }
    if (is_tag && !l.force && !force_all) {
      line("!", "[rejected]", short_of(l.remote_ref), short_of(l.local_ref), "  (would clobber existing tag)");
      res.ok = false;
      continue;
    }
    std::string oc = peel_to(c, *cur, "commit"), nc = peel_to(c, l.oid, "commit");
    bool ff = !oc.empty() && !nc.empty() && is_ancestor(c, oc, nc);
    if (ff) {
      update_ref_checked(c, l.local_ref, l.oid, std::nullopt, "fetch: fast-forward");
      line(" ", c.abbrev(*cur) + ".." + c.abbrev(l.oid), short_of(l.remote_ref), short_of(l.local_ref), "");
    } else if (l.force) {
      update_ref_checked(c, l.local_ref, l.oid, std::nullopt, "fetch: forced-update");
      line("+", c.abbrev(*cur) + "..." + c.abbrev(l.oid), short_of(l.remote_ref), short_of(l.local_ref), "  (forced update)");
    } else {
      line("!", "[rejected]", short_of(l.remote_ref), short_of(l.local_ref), "  (non-fast-forward)");
      res.ok = false;
    }
  }
  if (prune && named) {
    std::set<std::string> keep;
    for (auto& l : plan) if (!l.local_ref.empty()) keep.insert(l.local_ref);
    for (auto& [ref, oid] : all_refs(c)) {
      bool in_scope = false;
      for (auto& sp : configured) {
        std::string mid;
        if (sp.pattern && match_pattern(sp.dst, ref, mid)) in_scope = true;
      }
      if (!in_scope || keep.count(ref)) continue;
      // a symbolic ref (origin/HEAD) is not one the remote has or lacks: prune leaves it
      if (auto t = c.host().read_file(c.repo->common_dir() + "/" + ref); t.ok() && t->size() > 5 && std::memcmp(t->data(), "ref: ", 5) == 0)
        continue;
      (void)c.repo->delete_ref(ref);
      line("-", "[deleted]", "(none)", short_of(ref), "");
    }
  }
  // the remote's HEAD, remembered once (git's remote.<name>.followRemoteHEAD "create")
  if (named && from_config && s.head_ref.rfind("refs/heads/", 0) == 0) {
    std::string sym = c.repo->common_dir() + "/refs/remotes/" + remote_arg + "/HEAD";
    std::string tracking;
    for (auto& cf : configured) {
      std::string mid;
      if (cf.pattern && match_pattern(cf.src, s.head_ref, mid)) { tracking = apply_pattern(cf.dst, mid); break; }
    }
    if (!tracking.empty() && read_ref(c, tracking) && !c.host().exists(sym) &&
        c.config.get("remote." + remote_arg + ".followremotehead").value_or("never") != "never") {
      std::string body = "ref: " + tracking + "\n";
      (void)c.host().make_dir(dirname_of(sym));
      c.host().write_file(sym, ByteSpan((const uint8_t*)body.data(), body.size()));
    }
  }
  if (!lines.empty()) {
    size_t width = 10;
    for (auto& l : lines) width = std::max(width, l.from.size());
    // transport_summary_width: room for "<old>..<new>" at the longest abbreviation of what was fetched
    size_t abbrev = 0;
    for (auto& l : plan) abbrev = std::max(abbrev, c.abbrev(l.oid).size());
    const size_t sum_width = abbrev ? 2 * abbrev + 3 : 17;
    report << "From " << shown_url << "\n";
    for (auto& l : lines) {
      std::string sum = l.summary;
      if (sum.size() < sum_width) sum += std::string(sum_width - sum.size(), ' ');
      std::string from = l.from + std::string(width - l.from.size(), ' ');
      report << " " << l.flag << " " << sum << " " << from << " -> " << l.to << l.note << "\n";
    }
  }
  if (write_fetch_head) c.write_git_file("FETCH_HEAD", fetch_head);
  if (!quiet) c.err << report.str();
  res.landed = plan;
  return res;
}

// ---------------------------------------------------------------------------
// push
// ---------------------------------------------------------------------------
struct PushLine { std::string flag, summary, from, to, note; bool ok = true; };

void print_push(Ctx& c, const std::string& url, const std::vector<PushLine>& lines, bool quiet, bool all_up_to_date,
                size_t sum_width = 17) {
  bool failed = false;
  for (auto& l : lines) if (!l.ok) failed = true;
  if (all_up_to_date && !failed) { c.err << "Everything up-to-date\n"; return; }
  if (quiet && !failed) return;
  c.err << "To " << display_url(url) << "\n";
  for (auto& l : lines) {
    if (quiet && l.ok) continue;
    if (l.flag == "=") continue;
    std::string sum = l.summary;
    if (sum.size() < sum_width) sum += std::string(sum_width - sum.size(), ' ');
    c.err << " " << l.flag << " " << sum << " " << l.from << (l.to.empty() ? "" : " -> " + l.to) << l.note << "\n";
  }
}

}  // namespace

// ---------------------------------------------------------------------------
int cmd_fetch(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'q', "quiet"}, {'v', "verbose"}, {'f', "force"}, {'t', "tags"}, {'n', ""}, {0, "no-tags"},
                                         {'p', "prune"}, {'P', "prune-tags"}, {0, "depth", Arg::Required}, {0, "all"}, {0, "dry-run"},
                                         {0, "progress"}, {0, "no-progress"}, {0, "update-head-ok"}, {'u', ""}, {0, "unshallow"},
                                         {0, "multiple"}, {0, "recurse-submodules", Arg::Optional}, {0, "filter", Arg::Required},
                                         {0, "write-fetch-head"}, {0, "atomic"}, {'k', "keep"}, {0, "set-upstream"}, {0, "append"},
                                         {0, "deepen", Arg::Required}, {0, "shallow-since", Arg::Required}, {0, "refetch"},
                                         {0, "porcelain"}, {0, "negotiation-tip", Arg::Required}, {'j', "jobs", Arg::Required},
                                         {'4', "ipv4"}, {'6', "ipv6"}, {0, "show-forced-updates"}, {0, "auto-maintenance"}, {0, "auto-gc"}});
  c.open_repo(false);
  if (p.has("dry-run") || p.has("filter") || p.has("atomic") || p.has("set-upstream") || p.has("append") ||
      p.has("shallow-since") || p.has("refetch") || p.has("porcelain") || p.has("negotiation-tip") || p.has("prune-tags"))
    unsupported(c, "this fetch option");
  bool quiet = p.has("quiet");
  int depth = p.has("depth") ? std::atoi(p.get("depth").c_str()) : 0;
  if (p.has("depth") && depth <= 0) die(c, "depth " + p.get("depth") + " is not a positive number");
  bool relative = false;
  if (p.has("deepen")) {
    if (p.has("depth")) die(c, "options '--deepen' and '--depth' cannot be used together");
    depth = std::atoi(p.get("deepen").c_str());
    if (depth <= 0) die(c, "negative depth in --deepen is not supported");
    relative = true;
  }
  if (p.has("unshallow")) {
    if (p.has("depth") || p.has("deepen")) die(c, "options '--depth' and '--unshallow' cannot be used together");
    if (!c.host().exists(c.repo->common_dir() + "/shallow")) die(c, "--unshallow on a complete repository does not make sense");
    depth = 0x7fffffff;   // what git sends: the whole history
  }
  std::string tags = p.has("tags") ? "all" : (p.has("no-tags") || p.has("n")) ? "no" : "";
  bool prune = p.has("prune") || c.config.get_bool("fetch.prune", false);
  bool write_fh = !p.negated.count("write-fetch-head");
  if (p.has("all") || p.has("multiple")) {
    std::vector<std::string> remotes;
    if (p.has("all")) {
      std::set<std::string> seen;
      for (auto& e : c.config.entries())
        if (e.key.rfind("remote.", 0) == 0 && e.key.size() > 11 && e.key.compare(e.key.size() - 4, 4, ".url") == 0) {
          std::string r = e.key.substr(7, e.key.size() - 11);
          if (seen.insert(r).second) remotes.push_back(r);
        }
    } else {
      remotes = p.pos;
    }
    bool ok = true;
    for (auto& r : remotes) {
      if (!quiet) c.out << "Fetching " << r << "\n";
      ok = do_fetch(c, r, {}, quiet, p.has("force"), depth, tags, prune, write_fh, p.has("update-head-ok"), relative).ok && ok;
    }
    return ok ? 0 : 1;
  }
  std::string remote = p.pos.empty() ? "" : p.pos[0];
  if (remote.empty()) {
    std::string b = current_branch(c);
    remote = b.empty() ? "" : c.config.get("branch." + b + ".remote").value_or("");
    if (remote.empty()) remote = "origin";
    if (!c.config.get("remote." + remote + ".url")) die(c, "No remote repository specified.  Please, specify either a URL or a\nremote name from which new revisions should be fetched.");
  }
  std::vector<std::string> specs(p.pos.begin() + (p.pos.empty() ? 0 : 1), p.pos.end());
  return do_fetch(c, remote, specs, quiet, p.has("force"), depth, tags, prune, write_fh, p.has("update-head-ok"), relative).ok ? 0 : 1;
}

int cmd_pull(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'q', "quiet"}, {'v', "verbose"}, {0, "ff-only"}, {0, "ff"}, {0, "no-ff"}, {'r', "rebase", Arg::Optional},
                                         {0, "no-edit"}, {0, "edit"}, {0, "commit"}, {0, "no-commit"}, {0, "squash"}, {0, "depth", Arg::Required},
                                         {0, "tags"}, {0, "no-tags"}, {'f', "force"}, {0, "prune"}, {0, "progress"}, {0, "no-progress"},
                                         {0, "allow-unrelated-histories"}, {0, "autostash"}, {'X', "strategy-option", Arg::Required},
                                         {'s', "strategy", Arg::Required}, {0, "stat"}, {'n', ""}, {0, "verify"}, {0, "no-verify"},
                                         {0, "recurse-submodules", Arg::Optional}, {0, "set-upstream"}});
  c.open_repo();
  bool rebase = p.has("rebase") && p.get("rebase") != "false";
  if (!p.has("rebase") && !p.negated.count("rebase")) {
    std::string b = current_branch(c);
    auto br = c.config.get("branch." + b + ".rebase");
    auto pr = br ? br : c.config.get("pull.rebase");
    if (pr && Config::parse_bool(*pr, true)) rebase = true;
  }
  std::string rebase_mode = p.has("rebase") ? p.get("rebase") : "";
  if (rebase && rebase_mode.empty()) {
    std::string cb = current_branch(c);
    auto br = c.config.get("branch." + cb + ".rebase");
    rebase_mode = br ? *br : c.config.get("pull.rebase").value_or("true");
  }
  if (rebase_mode == "true" || rebase_mode == "yes" || rebase_mode == "on" || rebase_mode == "1") rebase_mode = "";
  if (rebase && rebase_mode != "" && rebase_mode != "merges" && rebase_mode != "interactive" && rebase_mode != "i")
    unsupported(c, "pull --rebase=" + rebase_mode);
  if (p.has("squash") || p.has("set-upstream") || (p.has("autostash") && !rebase)) unsupported(c, "pull --autostash (without --rebase)/--squash/--set-upstream");
  if (rebase && !p.has("autostash") && !c.config.get_bool("rebase.autostash", false)) {
    if (!rebase_require_clean(c, "pull with rebase", "please commit or stash them.")) return 128;
  }
  std::string b = current_branch(c);
  std::string remote = p.pos.empty() ? (b.empty() ? "" : c.config.get("branch." + b + ".remote").value_or("")) : p.pos[0];
  if (remote.empty()) {
    if (b.empty()) die(c, "You are not currently on a branch.");
    c.err << "There is no tracking information for the current branch.\nPlease specify which branch you want to merge with.\n";
    return 1;
  }
  std::vector<std::string> specs(p.pos.begin() + (p.pos.empty() ? 0 : 1), p.pos.end());
  std::string tags = p.has("tags") ? "all" : p.has("no-tags") ? "no" : "";
  auto fr = do_fetch(c, remote, specs, p.has("quiet"), p.has("force"), p.has("depth") ? std::atoi(p.get("depth").c_str()) : 0, tags,
                     p.has("prune"), true, false);
  if (!fr.ok) return 1;
  auto fh = c.read_git_file("FETCH_HEAD");
  bool any_merge = fh && fh->find("\t\t") != std::string::npos;
  if (!any_merge) {
    if (specs.empty()) {
      c.err << "Your configuration specifies to merge with the ref '" << c.config.get("branch." + b + ".merge").value_or("")
            << "'\nfrom the remote, but no such ref was fetched.\n";
      return 1;
    }
  }
  std::string theirs = fh->substr(0, 40);
  auto head = read_ref(c, "HEAD");
  if (rebase) {
    std::string tracking;
    if (specs.empty()) tracking = upstream_of(c, b);
    else if (specs[0].find(':') == std::string::npos) tracking = "refs/remotes/" + remote + "/" + specs[0];
    std::string fork = tracking.empty() || !head ? std::string() : rebase_fork_point(c, tracking, *head);
    std::vector<std::string> rargs;
    if (p.has("quiet")) rargs.push_back("-q");
    if (p.has("verbose")) rargs.push_back("-v");
    if (p.has("autostash")) rargs.push_back("--autostash");
    if (rebase_mode == "merges") rargs.push_back("--rebase-merges");
    if (rebase_mode == "interactive" || rebase_mode == "i") rargs.push_back("-i");
    if (p.has("strategy-option")) { rargs.push_back("-X"); rargs.push_back(p.get("strategy-option")); }
    if (p.has("strategy")) { rargs.push_back("-s"); rargs.push_back(p.get("strategy")); }
    if (!fork.empty()) { rargs.push_back("--onto"); rargs.push_back(theirs); rargs.push_back(fork); }
    else rargs.push_back(theirs);
    std::string action = "pull";
    for (size_t i = 1; i < c.args.size(); ++i) action += " " + c.args[i];
    return rebase_for_pull(c, rargs, action);
  }
  bool ff_possible = !head || is_ancestor(c, *head, theirs) || is_ancestor(c, theirs, *head);
  bool ff_only = p.has("ff-only");
  if (!ff_only && !p.has("ff") && !p.has("no-ff") && !p.negated.count("rebase") && !p.has("rebase")) {
    auto pff = c.config.get("pull.ff");
    if (pff && *pff == "only") ff_only = true;
    else if (!pff && !c.config.get("pull.rebase") && !ff_possible) {
      c.err << "hint: You have divergent branches and need to specify how to reconcile them.\n"
               "hint: You can do so by running one of the following commands sometime before\n"
               "hint: your next pull:\n"
               "hint: \n"
               "hint:   git config pull.rebase false  # merge\n"
               "hint:   git config pull.rebase true   # rebase\n"
               "hint:   git config pull.ff only       # fast-forward only\n"
               "hint: \n"
               "hint: You can replace \"git config\" with \"git config --global\" to set a default\n"
               "hint: preference for all repositories. You can also pass --rebase, --no-rebase,\n"
               "hint: or --ff-only on the command line to override the configured default per\n"
               "hint: invocation.\n"
               "fatal: Need to specify how to reconcile divergent branches.\n";
      return 128;
    }
  }
  std::vector<std::string> margs{"merge"};
  if (ff_only) margs.push_back("--ff-only");
  if (p.has("no-ff")) margs.push_back("--no-ff");
  if (p.has("ff")) margs.push_back("--ff");
  if (p.has("quiet")) margs.push_back("-q");
  if (p.has("no-commit")) margs.push_back("--no-commit");
  if (p.has("allow-unrelated-histories")) margs.push_back("--allow-unrelated-histories");
  if (p.has("strategy-option")) { margs.push_back("-X"); margs.push_back(p.get("strategy-option")); }
  margs.push_back("FETCH_HEAD");
  c.args = margs;
  return cmd_merge(c);
}

// ---------------------------------------------------------------------------
int cmd_clone(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'q', "quiet"}, {'v', "verbose"}, {0, "bare"}, {0, "mirror"}, {'n', "no-checkout"},
                                         {0, "checkout"}, {0, "depth", Arg::Required}, {'b', "branch", Arg::Required},
                                         {0, "single-branch"}, {0, "no-tags"}, {0, "tags"}, {'o', "origin", Arg::Required},
                                         {'c', "config", Arg::Required}, {0, "progress"}, {0, "no-progress"}, {'l', "local"},
                                         {0, "no-hardlinks"}, {'s', "shared"}, {0, "reference", Arg::Required}, {0, "dissociate"},
                                         {0, "recurse-submodules", Arg::Optional}, {0, "shallow-submodules"}, {0, "filter", Arg::Required},
                                         {0, "separate-git-dir", Arg::Required}, {'j', "jobs", Arg::Required}, {0, "template", Arg::Required},
                                         {0, "sparse"}, {0, "reject-shallow"}, {0, "also-filter-submodules"}, {0, "remote-submodules"},
                                         {'u', "upload-pack", Arg::Required}, {0, "bundle-uri", Arg::Required}, {0, "ref-format", Arg::Required},
                                         {0, "revision", Arg::Required}, {0, "shallow-since", Arg::Required}, {0, "shallow-exclude", Arg::Required}});
  if (p.has("mirror") || p.has("reference") || p.has("separate-git-dir") || p.has("upload-pack") ||
      p.has("bundle-uri") || p.has("revision") || p.has("shallow-since") || p.has("shallow-exclude") || p.has("recurse-submodules"))
    unsupported(c, "this clone option");
  if (p.pos.empty()) die(c, "You must specify a repository to clone.", 129);
  if (p.pos.size() > 2) die(c, "Too many arguments.", 129);
  std::string url = p.pos[0];
  bool bare = p.has("bare");
  bool quiet = p.has("quiet");
  std::string origin = p.get("origin", "origin");
  std::string dir;
  if (p.pos.size() > 1) dir = p.pos[1];
  else {
    std::string base = slashes(strip_file_scheme(url));
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.size() > 5 && base.compare(base.size() - 5, 5, "/.git") == 0) base = base.substr(0, base.size() - 5);
    size_t s = base.find_last_of("/:");
    base = s == std::string::npos ? base : base.substr(s + 1);
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".git") == 0) base = base.substr(0, base.size() - 4);
    if (base.size() > 7 && base.compare(base.size() - 7, 7, ".bundle") == 0) base = base.substr(0, base.size() - 7);
    if (base.empty()) die(c, "No directory name could be guessed.\nPlease specify a directory on the command line");
    dir = bare ? base + ".git" : base;
  }
  std::string target = c.abs_path(dir);
  auto& h = c.host();
  bool existed = h.exists(target);
  if (existed) {
    auto names = h.list_dir(target);
    bool empty = names.ok();
    if (names.ok()) for (auto& n : *names) if (n != "." && n != "..") empty = false;
    if (!empty) die(c, "destination path '" + dir + "' already exists and is not an empty directory.");
  }
  c.load_config();
  // the source first: a missing repository leaves nothing behind
  std::string resolved_url = url;
  {
    std::string loc = strip_file_scheme(url);
    if (!is_url(loc) && !is_scp(loc) && !h.exists(c.abs_path(loc))) die(c, "repository '" + url + "' does not exist");
  }
  Side s = open_side(c, resolved_url);
  if (!quiet) c.err << (bare ? "Cloning into bare repository '" : "Cloning into '") << dir << "'...\n";
  auto cleanup = [&]() {
    std::function<void(const std::string&)> rm = [&](const std::string& p2) {
      auto st = h.stat(p2);
      if (!st.ok()) return;
      if (st->is_dir && !st->is_link) {
        auto ns = h.list_dir(p2);
        if (ns.ok()) for (auto& n : *ns) if (n != "." && n != "..") rm(p2 + "/" + n);
      }
      (void)h.remove(p2);
    };
    if (!existed) rm(target);
    else {
      auto ns = h.list_dir(target);
      if (ns.ok()) for (auto& n : *ns) if (n != "." && n != "..") rm(target + "/" + n);
    }
  };
  try {
    Git g(*c.kernel);
    std::string init_branch = c.config.get("init.defaultbranch").value_or("master");
    // a bundle names no HEAD branch before the repository is made, so git falls back (and says so)
    if (s.kind == Side::BundleFile && !c.config.get("init.defaultbranch")) default_branch_hint(c);
    auto r = g.init(target, init_branch, bare);
    if (!r.ok()) die(c, "could not create work tree dir '" + dir + "': " + r.error().message);
    c.repo = std::make_unique<Git>(*c.kernel);
    if (bare) { if (!c.repo->open_dirs(target, "").ok()) die(c, "could not open the new repository"); c.top = ""; }
    else { if (!c.repo->open(target).ok()) die(c, "could not open the new repository"); c.top = target; }
    std::string cfg = c.repo->common_dir() + "/config";
    std::string err;
    // the url as given (a relative local path becomes absolute, as git records it)
    // a local source is recorded as an absolute path, joined as given ("<cwd>/." for ".")
    std::string recorded = url;
    if (s.kind != Side::Network && !is_url(url)) recorded = is_absolute(slashes(url)) ? slashes(url) : c.cwd + "/" + slashes(url);
    for (auto& kv : p.all("config")) {
      size_t eq = kv.find('=');
      if (eq == std::string::npos) die(c, "error: --config expects key=value");
      edit_config_file(h, cfg, kv.substr(0, eq), kv.substr(eq + 1), ConfigEdit::Add, &err);
    }
    edit_config_file(h, cfg, "remote." + origin + ".url", recorded, ConfigEdit::Set, &err);
    bool single = p.has("single-branch") || p.has("depth");
    if (p.negated.count("single-branch")) single = false;
    // in git's order: url, tagopt, fetch
    if (p.has("no-tags")) edit_config_file(h, cfg, "remote." + origin + ".tagopt", "--no-tags", ConfigEdit::Set, &err);
    if (!bare) {
      if (!single) edit_config_file(h, cfg, "remote." + origin + ".fetch", "+refs/heads/*:refs/remotes/" + origin + "/*", ConfigEdit::Set, &err);
    }
    c.load_config();
    c.apply_repo_settings();
    // --shared: borrow a local source's objects (objects/info/alternates) instead of copying them
    if (p.has("shared") && s.kind == Side::Local && s.local)
      if (auto ar = c.repo->add_alternate(s.local->common_dir() + "/objects"); !ar.ok()) die(c, "could not record the alternate: " + ar.error().message);
    // what is there
    std::vector<GitRemoteRef> adv;
    std::string default_branch;
    std::string want_branch = p.get("branch");
    int depth = p.has("depth") ? std::atoi(p.get("depth").c_str()) : 0;
    std::vector<std::string> wants;
    auto choose = [&](const std::vector<GitRemoteRef>& refs) {
      adv = refs;
      std::vector<std::string> w;
      std::string head_oid = oid_of(refs, "HEAD");
      if (!s.head_ref.empty()) default_branch = s.head_ref;
      if (default_branch.empty())
        for (auto& r2 : refs) if (r2.name.rfind("refs/heads/", 0) == 0 && r2.oid == head_oid) { default_branch = r2.name; if (r2.name == "refs/heads/main" || r2.name == "refs/heads/master") break; }
      std::string only;
      if (single) {
        if (!want_branch.empty()) only = match_remote_ref(refs, want_branch);
        else only = default_branch;
      }
      for (auto& r2 : refs) {
        bool head = r2.name.rfind("refs/heads/", 0) == 0;
        bool tag = r2.name.rfind("refs/tags/", 0) == 0 && r2.name.find("^{}") == std::string::npos;
        if (head && (only.empty() || r2.name == only)) w.push_back(r2.oid);
        else if (tag && !p.has("no-tags") && (!single || bare || p.has("tags"))) w.push_back(r2.oid);
        else if (!single && !head && !tag && bare && r2.name != "HEAD" && r2.name.find("^{}") == std::string::npos && false) w.push_back(r2.oid);
      }
      if (!only.empty() && only.rfind("refs/tags/", 0) == 0) w.push_back(oid_of(refs, only));
      return w;
    };
    const std::string filter = p.get("filter");
    if (!filter.empty()) {
      // a partial clone: the blobs stay at the remote until something here needs one (git's promisor remote)
      if (filter != "blob:none" && filter.rfind("blob:limit=", 0) != 0) die(c, "invalid filter-spec '" + filter + "'");
      edit_config_file(h, cfg, "core.repositoryformatversion", "1", ConfigEdit::Set, &err);
      edit_config_file(h, cfg, "remote." + origin + ".promisor", "true", ConfigEdit::Set, &err);
      edit_config_file(h, cfg, "remote." + origin + ".partialclonefilter", filter, ConfigEdit::Set, &err);
      edit_config_file(h, cfg, "extensions.partialclone", origin, ConfigEdit::Set, &err);
    }
    if (s.kind == Side::Network) {
      GitRemote remote(*c.kernel);
      GitRemoteOptions o = s.opt;
      o.depth = depth;
      o.filter = filter;
      std::string headsym;
      o.select = [&](const std::vector<GitRemoteRef>& refs) { return choose(refs); };
      auto fr = remote.fetch(*c.repo, s.location, o);
      if (!fr.ok()) {
        // (an empty repository is not an error here: the advertisement had nothing to ask for)
        c.err << "fatal: unable to access '" << display_url(url) << "': " << fr.error().message << "\n";
        throw Exit{128};
      } else if (!fr->default_branch.empty() && fr->default_branch.rfind("refs/heads/", 0) == 0) {
        default_branch = fr->default_branch;
      }
      if (adv.empty() && fr.ok()) adv = fr->refs;
    } else {
      wants = choose(s.refs);
      // --shared borrows the source's objects (the alternate recorded above): nothing is copied, which is the point of
      // it. It used to record the alternate and then copy the whole store as well.
      const bool borrowed = p.has("shared") && s.kind == Side::Local && s.local;
      bool taken = borrowed || (s.kind == Side::Local && depth == 0 && s.local && filter.empty() &&
                                take_object_store(s.local->common_dir() + "/objects", c.repo->common_dir() + "/objects", !p.has("no-hardlinks")));
      if (taken && !borrowed)
        for (const auto& alt : s.local->alternate_object_dirs())
          if (!take_object_store(alt, c.repo->common_dir() + "/objects", !p.has("no-hardlinks"))) { taken = false; break; }
      if (taken) c.repo->rescan_packs();
      if (!taken) fetch_objects(c, s, wants, depth, filter);
    }
    bool empty_remote = true;
    for (auto& r2 : adv) if (r2.name != "HEAD") empty_remote = false;
    if (s.kind == Side::BundleFile && default_branch.empty()) {
      for (auto& r2 : adv) if (r2.name == "HEAD") {
        for (auto& r3 : adv) if (r3.name.rfind("refs/heads/", 0) == 0 && r3.oid == r2.oid) { default_branch = r3.name; break; }
      }
    }
    if (empty_remote && !adv.empty() && oid_of(adv, "HEAD").size() == 40) empty_remote = false;
    if (empty_remote) {
      c.err << "warning: You appear to have cloned an empty repository.\n";
      std::string b = default_branch.empty() ? "refs/heads/" + init_branch : default_branch;
      (void)c.repo->set_head(b);
      if (!bare) {
        edit_config_file(h, cfg, "branch." + b.substr(11) + ".remote", origin, ConfigEdit::Set, &err);
        edit_config_file(h, cfg, "branch." + b.substr(11) + ".merge", b, ConfigEdit::Set, &err);
      }
      return 0;
    }
    // refs: all in packed-refs, one file, as git's clone writes them (a loose file, renamed into place and synced,
    // for each of redis' 500 tags and branches was most of a local clone)
    {
      // git's update_remote_refs: the mapped refs packed (with --single-branch only the branch cloned; tags only
      // without it, and not with --no-tags), then with --single-branch the tags whose objects came along written one
      // by one (write_followtags)
      const std::string chosen = want_branch.empty() ? default_branch : match_remote_ref(adv, want_branch);
      const bool one_branch = single && !bare;
      std::vector<std::pair<std::string, std::string>> packed, follow;
      for (auto& r2 : adv) {
        if (r2.name == "HEAD" || r2.name.find("^{}") != std::string::npos) continue;
        if (!c.repo->has_object(r2.oid)) continue;   // not fetched (single branch, tags off)
        if (r2.name.rfind("refs/heads/", 0) == 0) {
          if (one_branch && r2.name != chosen) continue;
          packed.push_back({bare ? r2.name : "refs/remotes/" + origin + "/" + r2.name.substr(11), r2.oid});
        } else if (r2.name.rfind("refs/tags/", 0) == 0) {
          if (p.has("no-tags")) continue;
          (one_branch && !p.has("tags") ? follow : packed).push_back({r2.name, r2.oid});
        }
      }
      for (auto& [name, oid] : follow) (void)c.repo->update_ref(name, oid);
      if (!packed.empty()) {
        auto w = c.repo->write_packed_refs(packed);
        if (!w.ok()) die(c, "could not write packed-refs: " + w.error().message);
      }
    }
    std::string branch_ref = want_branch.empty() ? default_branch : match_remote_ref(adv, want_branch);
    if (!want_branch.empty() && branch_ref.empty()) {
      c.err << "warning: Could not find remote branch " << want_branch << " to clone.\nfatal: Remote branch " << want_branch << " not found in upstream " << origin << "\n";
      throw Exit{128};
    }
    if (single && !bare && !branch_ref.empty() && branch_ref.rfind("refs/heads/", 0) == 0)
      edit_config_file(h, cfg, "remote." + origin + ".fetch", "+" + branch_ref + ":refs/remotes/" + origin + "/" + branch_ref.substr(11), ConfigEdit::Set, &err);
    std::string head_oid = branch_ref.empty() ? oid_of(adv, "HEAD") : oid_of(adv, branch_ref);
    if (branch_ref.rfind("refs/tags/", 0) == 0) head_oid = peel_to(c, head_oid, "commit");
    // refs/remotes/<origin>/HEAD names the remote's HEAD branch only when that branch was fetched (guess_remote_head
    // over the mapped refs): not after --single-branch -b of another one
    const bool head_fetched = !single || branch_ref == default_branch;
    if (!bare && !default_branch.empty() && head_fetched) {
      std::string body = "ref: refs/remotes/" + origin + "/" + default_branch.substr(11) + "\n";
      (void)h.make_dir(c.repo->common_dir() + "/refs/remotes/" + origin);
      h.write_file(c.repo->common_dir() + "/refs/remotes/" + origin + "/HEAD", ByteSpan((const uint8_t*)body.data(), body.size()));
    }
    if (branch_ref.rfind("refs/heads/", 0) == 0) {
      std::string b = branch_ref.substr(11);
      if (!bare) {
        (void)c.repo->update_ref(branch_ref, head_oid);
        edit_config_file(h, cfg, "branch." + b + ".remote", origin, ConfigEdit::Set, &err);
        edit_config_file(h, cfg, "branch." + b + ".merge", branch_ref, ConfigEdit::Set, &err);
      }
      (void)c.repo->set_head(branch_ref);
    } else if (!head_oid.empty()) {
      (void)c.repo->set_head_detached(head_oid);
    }
    c.load_config();
    if (!bare && !p.has("no-checkout") && !head_oid.empty() && branch_ref.rfind("refs/heads/", 0) != 0) detached_advice(c, head_oid);
    if (!bare && head_oid.empty()) c.err << "warning: remote HEAD refers to nonexistent ref, unable to checkout\n";
    if (!bare && p.has("sparse")) {
      // only the top level's files, until git sparse-checkout set/add names more
      edit_config_file(h, cfg, "core.sparseCheckout", "true", ConfigEdit::Set, &err);
      edit_config_file(h, cfg, "core.sparseCheckoutCone", "true", ConfigEdit::Set, &err);
      (void)h.make_dir(c.repo->git_dir() + "/info");
      std::string pats = "/*\n!/*/\n";
      (void)h.write_file(c.repo->git_dir() + "/info/sparse-checkout", ByteSpan((const uint8_t*)pats.data(), pats.size()));
      c.repo->reload_sparse();
    }
    install_promisor(c);
    if (!bare && !p.has("no-checkout") && !head_oid.empty()) {
      auto co = c.repo->checkout_tree(read_commit(c, head_oid).tree, true);
      if (!co.ok()) die(c, "unable to checkout working tree: " + co.error().message);
    }
    if (!bare) {
      const std::string why = "clone: from " + display_url(url);
      if (branch_ref.rfind("refs/heads/", 0) == 0) append_reflog(c, branch_ref, "", head_oid, why);
      if (!default_branch.empty() && (!single || branch_ref == default_branch))
        append_reflog(c, "refs/remotes/" + origin + "/HEAD", "", oid_of(adv, default_branch), why);
      append_reflog(c, "HEAD", "", head_oid, why);
    }
    return 0;
  } catch (Exit& e) {
    cleanup();
    throw;
  }
}

// ---------------------------------------------------------------------------
namespace {
bool stderr_is_terminal() {
#ifdef _WIN32
  return ::_isatty(2) != 0;
#else
  return ::isatty(2) != 0;
#endif
}
// git's sizes: "412 bytes", "1.20 KiB", "3.45 MiB"
std::string human_bytes(double b) {
  const char* units[] = {"KiB", "MiB", "GiB", "TiB"};
  char buf[64];
  if (b < 1024) { std::snprintf(buf, sizeof buf, "%.0f bytes", b); return buf; }
  int u = -1;
  while (b >= 1024 && u < 3) { b /= 1024; ++u; }
  std::snprintf(buf, sizeof buf, "%.2f %s", b, units[u < 0 ? 0 : u]);
  return buf;
}
}  // namespace

int cmd_push(Ctx& c) {
  static const bool trace = std::getenv("AIOS_GIT_TRACE") != nullptr;
  const auto push_t0 = std::chrono::steady_clock::now();
  auto push_ms = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - push_t0).count(); };
  auto p = parse_options(c, c.args, 1, {{'q', "quiet"}, {'v', "verbose"}, {'f', "force"}, {0, "force-with-lease", Arg::Optional},
                                         {0, "force-if-includes"}, {0, "no-verify"}, {0, "verify"}, {'u', "set-upstream"}, {'d', "delete"},
                                         {0, "tags"}, {0, "all"}, {0, "branches"}, {0, "mirror"}, {'n', "dry-run"}, {0, "porcelain"},
                                         {0, "progress"}, {0, "no-progress"}, {0, "atomic"}, {'o', "push-option", Arg::Required},
                                         {0, "follow-tags"}, {0, "prune"}, {0, "recurse-submodules", Arg::Optional}, {0, "thin"},
                                         {0, "signed", Arg::Optional}, {0, "repo", Arg::Required}, {0, "receive-pack", Arg::Required},
                                         {'4', "ipv4"}, {'6', "ipv6"}});
  c.open_repo(false);
  if (p.has("mirror") || p.has("porcelain") || p.has("atomic") || p.has("push-option") || p.has("follow-tags") || p.has("prune") ||
      p.has("signed") || p.has("receive-pack") || p.has("force-if-includes"))
    unsupported(c, "this push option");
  bool quiet = p.has("quiet"), dry = p.has("dry-run");
  std::string remote = p.pos.empty() ? "" : p.pos[0];
  std::string cur = current_branch(c);
  if (remote.empty()) {
    remote = cur.empty() ? "" : c.config.get("branch." + cur + ".pushremote").value_or(c.config.get("remote.pushdefault").value_or(c.config.get("branch." + cur + ".remote").value_or("")));
    if (remote.empty()) {
      if (c.config.get("remote.origin.url")) remote = "origin";
      else die(c, "No configured push destination.\nEither specify the URL from the command-line or configure a remote repository using\n\n    git remote add <name> <url>\n\nand then push using the remote name\n\n    git push <name>\n");
    }
  }
  bool named = false;
  std::string url = remote_url(c, remote, &named);
  if (named) if (auto pu = c.config.get("remote." + remote + ".pushurl")) url = *pu;
  std::vector<std::string> spec_args(p.pos.begin() + (p.pos.empty() ? 0 : 1), p.pos.end());
  if (p.has("tags")) for (auto& [ref, oid] : all_refs(c)) if (ref.rfind("refs/tags/", 0) == 0) spec_args.push_back(ref + ":" + ref);
  if (p.has("all") || p.has("branches")) for (auto& [ref, oid] : all_refs(c)) if (ref.rfind("refs/heads/", 0) == 0) spec_args.push_back(ref + ":" + ref);
  if (p.has("delete")) {
    for (auto& s : spec_args) if (s.find(':') == std::string::npos) s = ":" + s;
  }
  if (spec_args.empty()) {
    if (named) for (auto& ps : c.config.get_all("remote." + remote + ".push")) spec_args.push_back(ps);
  }
  if (spec_args.empty()) {
    if (cur.empty()) die(c, "You are not currently on a branch.\nTo push the history leading to the current (detached HEAD)\nstate now, use\n\n    git push " + remote + " HEAD:<name-of-remote-branch>\n");
    std::string mode = c.config.get("push.default").value_or("simple");
    std::string up_remote = c.config.get("branch." + cur + ".remote").value_or("");
    std::string up_merge = c.config.get("branch." + cur + ".merge").value_or("");
    if (mode == "nothing") die(c, "You didn't specify any refspecs to push, and push.default is \"nothing\".");
    if ((mode == "simple" || mode == "upstream") && up_remote == remote && !up_merge.empty()) {
      if (mode == "simple" && up_merge != "refs/heads/" + cur)
        die(c, "The upstream branch of your current branch does not match\nthe name of your current branch.  To push to the upstream branch\non the remote, use\n\n    git push " + remote + " HEAD:" + up_merge.substr(11) + "\n\nTo push to the branch of the same name on the remote, use\n\n    git push " + remote + " HEAD\n");
      spec_args.push_back("refs/heads/" + cur + ":" + up_merge);
    } else if (mode == "upstream" && up_merge.empty()) {
      die(c, "The current branch " + cur + " has no upstream branch.");
    } else if ((mode == "simple" && up_remote.empty() && p.pos.empty())) {
      c.err << "fatal: The current branch " << cur << " has no upstream branch.\nTo push the current branch and set the remote as upstream, use\n\n    git push --set-upstream "
            << remote << " " << cur << "\n\nTo have this happen automatically for branches without a tracking\nupstream, see 'push.autoSetupRemote' in 'git help config'.\n\n";
      if (!c.config.get_bool("push.autosetupremote", false)) throw Exit{128};
      spec_args.push_back("refs/heads/" + cur + ":refs/heads/" + cur);
    } else if (mode == "matching") {
      unsupported(c, "push.default=matching");
    } else {
      spec_args.push_back("refs/heads/" + cur + ":refs/heads/" + cur);
    }
  }
  Side s = open_side(c, url);
  if (s.kind == Side::BundleFile) die(c, "cannot push to a bundle file");
  // The remote's refs are asked for only when a refspec needs them (a short destination name, a short delete)
  // or for a dry run: a plain push learns them from receive-pack's own advertisement, on the one connection
  // the push makes, as git does. Asking first cost a second SSH handshake on every push.
  bool have_refs = s.kind != Side::Network;
  std::vector<std::string> advertised;   // the refs' ids a network receive-pack advertised
  auto remote_refs = [&]() -> const std::vector<GitRemoteRef>& {
    if (!have_refs) { network_ls(c, s); have_refs = true; }
    return s.refs;
  };
  if (dry) remote_refs();
  // leases
  std::map<std::string, std::string> leases;   // ref -> expected ("" = use remote-tracking)
  bool lease_all = false;
  for (auto& v : p.all("force-with-lease")) {
    if (v.empty()) { lease_all = true; continue; }
    size_t colon = v.find(':');
    if (colon == std::string::npos) leases[v] = "";
    else leases[v.substr(0, colon)] = v.substr(colon + 1);
  }
  struct Update { GitPushUpdate up; std::string src_name, dst_name, src_ref; };
  std::vector<Update> ups;
  for (auto& sa : spec_args) {
    Refspec rs = parse_refspec(sa);
    if (rs.pattern) unsupported(c, "push refspecs with patterns");
    Update u;
    u.up.force = rs.force || p.has("force");
    std::string src = rs.src, dst = rs.dst;
    if (src.empty()) {
      // delete
      if (dst.empty()) die(c, "invalid refspec '" + sa + "'");
      if (dst.rfind("refs/", 0) != 0) {
        std::string m = match_remote_ref(remote_refs(), dst);
        dst = m.empty() ? "refs/heads/" + dst : m;
      }
      u.up.ref = dst;
      u.src_name = "(delete)";
      u.dst_name = dst.rfind("refs/heads/", 0) == 0 ? dst.substr(11) : dst.rfind("refs/tags/", 0) == 0 ? dst.substr(10) : dst;
      ups.push_back(u);
      continue;
    }
    std::string src_full;
    if (src == "HEAD" || src == "@") src_full = c.repo->head_ref();
    else src_full = dwim_ref(c, src);
    std::string oid;
    if (!src_full.empty()) { auto o = read_ref(c, src_full); if (o) oid = *o; }
    if (oid.empty()) {
      auto o = resolve(c, src);
      if (!o) die(c, "src refspec " + src + " does not match any");
      oid = *o;
    }
    if (dst.empty()) {
      if (src_full.empty()) die(c, "The destination you provided is not a full refname (i.e.,\nstarting with \"refs/\"). We tried to guess what you meant by:\n\n- Looking for a ref that matches '" + src + "' on the remote side.\n");
      dst = src_full;
    } else if (dst.rfind("refs/", 0) != 0) {
      std::string m = match_remote_ref(remote_refs(), dst);
      if (!m.empty()) dst = m;
      else if (src_full.rfind("refs/heads/", 0) == 0 || (src_full.empty() && type_of(c, oid) == "commit")) dst = "refs/heads/" + dst;
      else if (src_full.rfind("refs/tags/", 0) == 0) dst = "refs/tags/" + dst;
      else die(c, "The destination you provided is not a full refname (i.e.,\nstarting with \"refs/\").");
    }
    u.up.new_oid = oid;
    u.up.ref = dst;
    u.src_ref = src_full;
    auto short_of = [](const std::string& r) {
      for (const char* pre : {"refs/heads/", "refs/tags/"})
        if (r.rfind(pre, 0) == 0) return r.substr(std::strlen(pre));
      return r;
    };
    u.src_name = (src == "HEAD" || src == "@") ? "HEAD" : src_full.empty() ? src : short_of(src_full);
    u.dst_name = short_of(dst);
    auto lease = leases.find(dst);
    if (lease == leases.end()) {
      std::string short_dst = short_of(dst);
      lease = leases.find(short_dst);
    }
    if (lease != leases.end() || lease_all) {
      u.up.has_lease = true;
      std::string expect = lease != leases.end() ? lease->second : "";
      if (expect.empty()) {
        std::string tracking = named && dst.rfind("refs/heads/", 0) == 0 ? "refs/remotes/" + remote + "/" + dst.substr(11) : "";
        auto t = tracking.empty() ? std::nullopt : read_ref(c, tracking);
        expect = t ? *t : "";
      } else {
        auto o = resolve(c, expect);
        if (!o) usage(c, "cannot parse expected object name '" + expect + "'");
        expect = *o;
      }
      u.up.expected_old = expect;
      u.up.force = true;
    }
    ups.push_back(u);
  }
  std::vector<GitPushUpdate> raw;
  for (auto& u : ups) raw.push_back(u.up);
  if (!p.has("no-verify") && !find_hook(c, "pre-push", false).empty()) {
    // the hook is told what is about to be sent: <local ref> <local oid> <remote ref> <remote oid>
    std::map<std::string, std::string> theirs;
    for (auto& r2 : s.refs) theirs[r2.name] = r2.oid;
    std::string lines;
    for (size_t i = 0; i < raw.size(); ++i) {
      auto it = theirs.find(raw[i].ref);
      lines += (raw[i].new_oid.empty() ? std::string("(delete)") : ups[i].src_ref.empty() ? ups[i].src_name : ups[i].src_ref) + " " +
               (raw[i].new_oid.empty() ? kZero : raw[i].new_oid) + " " + raw[i].ref + " " + (it == theirs.end() ? kZero : it->second) + "\n";
    }
    HookOptions ho;
    ho.input = lines;
    ho.capture_stdout = true;
    HookResult hr = run_hook(c, "pre-push", {named ? remote : url, url}, ho);
    c.out << hr.out;
    if (hr.ran && hr.code != 0) {
      c.err << "error: failed to push some refs to '" << display_url(url) << "'\n";
      return 1;
    }
  }
  if (s.kind == Side::Local) {
    // our objects into theirs, then their refs, with receive-pack's rules
    Git& dst = *s.local;
    if (trace) std::fprintf(stderr, "[git] push: remote open, %zu refs listed at %.1f ms\n", s.refs.size(), push_ms());
    std::map<std::string, std::string> theirs;
    for (auto& r2 : s.refs) theirs[r2.name] = r2.oid;
    Config dcfg;
    if (auto f = c.host().read_file(dst.common_dir() + "/config"); f.ok()) dcfg.add_text(std::string((const char*)f->data(), f->size()));
    std::string deny_current = dcfg.get("receive.denycurrentbranch").value_or("refuse");
    bool deny_ff = dcfg.get_bool("receive.denynonfastforwards", false);
    bool deny_delete = dcfg.get_bool("receive.denydeletes", false);
    std::vector<std::string> tips;
    for (auto& up : raw) {
      auto it = theirs.find(up.ref);
      up.remote_old = it == theirs.end() ? kZero : it->second;
      if (up.has_lease && up.remote_old != (up.expected_old.empty() ? kZero : up.expected_old)) { up.status = "stale info"; continue; }
      if (up.new_oid.empty()) {
        if (up.remote_old == kZero) { up.status = "remote ref does not exist"; continue; }
        continue;
      }
      if (up.new_oid == up.remote_old) { up.status = "up to date"; up.ok = true; continue; }
      if (up.remote_old != kZero && !up.force) {
        if (up.ref.rfind("refs/tags/", 0) == 0) { up.status = "already exists"; continue; }
        if (!c.repo->has_object(up.remote_old)) { up.status = "fetch first"; continue; }
        std::string pc = peel_to(c, up.new_oid, "commit");
        if (pc.empty() || !is_ancestor(c, up.remote_old, pc)) { up.status = "non-fast-forward"; continue; }
      }
      tips.push_back(up.new_oid);
    }
    if (!dry) {
      std::vector<std::string> stop;
      for (auto& [n, o] : theirs) if (c.repo->has_object(o)) stop.push_back(o);
      if (trace) std::fprintf(stderr, "[git] push: %zu stop tips checked at %.1f ms\n", stop.size(), push_ms());
      auto objs = git_objects_between(*c.repo, tips, stop);
      if (!objs.ok()) die(c, objs.error().message);
      auto n = git_copy_objects(*c.repo, dst, *objs);
      if (!n.ok()) die(c, "could not write objects to the other repository: " + n.error().message);
      if (trace) std::fprintf(stderr, "[git] push: %llu object(s) copied at %.1f ms\n", (unsigned long long)*n, push_ms());
    }
    for (auto& up : raw) {
      if (!up.status.empty()) continue;
      if (!dst.bare() && up.ref == dst.head_ref() && deny_current != "ignore" && deny_current != "warn" && deny_current != "false" &&
          deny_current != "updateInstead") {
        up.status = "branch is currently checked out";
        up.ok = false;
        up.remote_old = up.remote_old.empty() ? kZero : up.remote_old;
        up.new_oid = up.new_oid;
        // mark as a remote rejection
        up.status = "remote:" + up.status;
        continue;
      }
      if (up.new_oid.empty()) {
        if (deny_delete) { up.status = "remote:deletion prohibited"; continue; }
        if (!dry) (void)dst.delete_ref(up.ref);
        up.ok = true;
        up.status = "ok";
        continue;
      }
      if (deny_ff && up.remote_old != kZero) {
        auto anc = git_is_ancestor(dst, up.remote_old, up.new_oid);
        if (!anc.ok() || !*anc) { up.status = "remote:non-fast-forward"; continue; }
      }
      if (!dry) {
        auto r = dst.update_ref(up.ref, up.new_oid);
        if (!r.ok()) { up.status = "remote:" + r.error().message; continue; }
      }
      up.ok = true;
      up.status = "ok";
    }
    if (trace) std::fprintf(stderr, "[git] push: refs written at %.1f ms\n", push_ms());
  } else if (!dry) {
    GitRemote gr(*c.kernel);
    GitRemoteOptions po = s.opt;
    po.quiet = quiet || p.has("no-progress") || !(p.has("progress") || stderr_is_terminal());
    auto r = gr.push_updates(*c.repo, url, raw, po);
    if (!r.ok()) {
      c.err << "fatal: unable to access '" << display_url(url) << "': " << r.error().message << "\n";
      return 128;
    }
    advertised = r->advertised;
    // what git says about the pack on a terminal (or with --progress): how many objects, how many bytes
    const bool progress = !quiet && !p.has("no-progress") && (p.has("progress") || stderr_is_terminal());
    if (progress && r->objects > 0) {
      const std::string n = std::to_string(r->objects);
      c.err << "Enumerating objects: " << n << ", done.\n"
            << "Counting objects: 100% (" << n << "/" << n << "), done.\n"
            << "Writing objects: 100% (" << n << "/" << n << "), " << human_bytes((double)r->pack_bytes) << ", done.\n"
            << "Total " << n << " (delta 0), reused 0 (delta 0), pack-reused 0\n";
    }
    // what the remote said (its hooks): "remote: " before each line, as git prints it
    if (!r->remote_messages.empty()) {
      std::string m = r->remote_messages;
      if (m.back() != '\n') m += '\n';
      for (size_t at = 0, nl; (nl = m.find('\n', at)) != std::string::npos; at = nl + 1) c.err << "remote: " << m.substr(at, nl - at) << "\n";
    }
  } else {
    // dry run: what would happen, from the advertisement
    std::map<std::string, std::string> theirs;
    for (auto& r2 : s.refs) theirs[r2.name] = r2.oid;
    for (auto& up : raw) {
      auto it = theirs.find(up.ref);
      up.remote_old = it == theirs.end() ? kZero : it->second;
      up.ok = true;
      up.status = up.new_oid == up.remote_old ? "up to date" : "ok";
    }
  }
  // report
  std::vector<PushLine> lines;
  bool all_up = true;
  bool failed = false;
  for (size_t i = 0; i < raw.size(); ++i) {
    auto& up = raw[i];
    auto& u = ups[i];
    PushLine l;
    std::string from = u.src_name, to = u.dst_name;
    if (up.status == "up to date") { l.flag = "="; l.summary = "[up to date]"; l.from = from; l.to = to; lines.push_back(l); continue; }
    all_up = false;
    if (!up.ok) {
      failed = true;
      l.ok = false;
      l.flag = "!";
      if (up.status.rfind("remote:", 0) == 0) { l.summary = "[remote rejected]"; l.note = " (" + up.status.substr(7) + ")"; }
      else if (up.status == "stale info" || up.status == "non-fast-forward" || up.status == "fetch first" || up.status == "already exists") { l.summary = "[rejected]"; l.note = " (" + up.status + ")"; }
      else if (up.status == "remote ref does not exist") { l.summary = "[rejected]"; l.note = " (remote ref does not exist)"; }
      else { l.summary = "[remote rejected]"; l.note = " (" + up.status + ")"; }
      l.from = up.new_oid.empty() ? to : from;
      l.to = up.new_oid.empty() ? "" : to;
      lines.push_back(l);
      continue;
    }
    if (up.new_oid.empty()) { l.flag = "-"; l.summary = "[deleted]"; l.from = to; lines.push_back(l); }
    else if (up.remote_old.empty() || up.remote_old == kZero) {
      l.flag = "*";
      l.summary = up.ref.rfind("refs/tags/", 0) == 0 ? "[new tag]" : up.ref.rfind("refs/heads/", 0) == 0 ? "[new branch]" : "[new reference]";
      l.from = from;
      l.to = to;
      lines.push_back(l);
    } else {
      std::string pc = peel_to(c, up.new_oid, "commit");
      bool ff = c.repo->has_object(up.remote_old) && !pc.empty() && is_ancestor(c, up.remote_old, pc);
      l.flag = ff ? " " : "+";
      l.summary = c.abbrev(up.remote_old) + (ff ? ".." : "...") + c.abbrev(up.new_oid);
      l.from = from;
      l.to = to;
      if (!ff) l.note = " (forced update)";
      lines.push_back(l);
    }
    // remote-tracking refs follow a successful push to a named remote
    if (!dry && named && up.ref.rfind("refs/heads/", 0) == 0) {
      std::string tracking;
      for (auto& f : c.config.get_all("remote." + remote + ".fetch")) {
        Refspec rs = parse_refspec(f);
        std::string mid;
        if (rs.pattern && match_pattern(rs.src, up.ref, mid)) { tracking = apply_pattern(rs.dst, mid); break; }
        if (!rs.pattern && rs.src == up.ref) { tracking = rs.dst; break; }
      }
      if (!tracking.empty()) {
        if (up.new_oid.empty()) (void)c.repo->delete_ref(tracking);
        else (void)c.repo->update_ref(tracking, up.new_oid);
      }
    }
    if (!dry && p.has("set-upstream") && u.src_ref.rfind("refs/heads/", 0) == 0 && up.ref.rfind("refs/heads/", 0) == 0 && !up.new_oid.empty()) {
      std::string b = u.src_ref.substr(11);
      std::string err;
      edit_config_file(c.host(), c.repo->common_dir() + "/config", "branch." + b + ".remote", remote, ConfigEdit::Set, &err);
      edit_config_file(c.host(), c.repo->common_dir() + "/config", "branch." + b + ".merge", up.ref, ConfigEdit::Set, &err);
      if (!quiet) c.out << "branch '" << b << "' set up to track '" << remote << "/" << up.ref.substr(11) << "'.\n";
    }
  }
  // transport_summary_width over every ref the remote advertised (its current value, abbreviated here)
  size_t abbrev = 0;
  for (auto& r2 : s.refs) {
    if (r2.oid.empty() || r2.oid == kZero || (r2.name.size() > 3 && r2.name.compare(r2.name.size() - 3, 3, "^{}") == 0)) continue;
    abbrev = std::max(abbrev, c.abbrev(r2.oid).size());
  }
  for (auto& oid : advertised) if (oid.size() == 40 && oid != kZero) abbrev = std::max(abbrev, c.abbrev(oid).size());
  print_push(c, url, lines, quiet, all_up, abbrev ? 2 * abbrev + 3 : 17);
  if (failed) {
    c.err << "error: failed to push some refs to '" << display_url(url) << "'\n";
    bool nonff = false, fetch_first = false;
    for (auto& up : raw) { if (up.status == "non-fast-forward") nonff = true; if (up.status == "fetch first") fetch_first = true; }
    if (fetch_first)
      c.err << "hint: Updates were rejected because the remote contains work that you do not\n"
               "hint: have locally. This is usually caused by another repository pushing to\n"
               "hint: the same ref. If you want to integrate the remote changes, use\n"
               "hint: 'git pull' before pushing again.\n"
               "hint: See the 'Note about fast-forwards' in 'git push --help' for details.\n";
    else if (nonff)
      c.err << "hint: Updates were rejected because the tip of your current branch is behind\n"
               "hint: its remote counterpart. If you want to integrate the remote changes,\n"
               "hint: use 'git pull' before pushing again.\n"
               "hint: See the 'Note about fast-forwards' in 'git push --help' for details.\n";
    return 1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
int cmd_ls_remote(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'h', "heads"}, {'b', "branches"}, {'t', "tags"}, {0, "refs"}, {'q', "quiet"},
                                         {0, "exit-code"}, {0, "symref"}, {0, "get-url"}, {0, "sort", Arg::Required},
                                         {'u', "upload-pack", Arg::Required}, {'o', "server-option", Arg::Required}});
  if (p.has("sort") || p.has("upload-pack") || p.has("server-option")) unsupported(c, "ls-remote --sort/--upload-pack/--server-option");
  c.try_open_repo();
  if (!c.repo) c.load_config();
  std::string remote = p.pos.empty() ? "origin" : p.pos[0];
  if (p.pos.empty() && c.repo) {
    std::string b = current_branch(c);
    remote = c.config.get("branch." + b + ".remote").value_or("origin");
  }
  std::string url = c.repo ? remote_url(c, remote) : remote;
  if (!c.repo) {
    if (auto u = c.config.get("remote." + remote + ".url")) url = *u;
  }
  if (p.has("get-url")) { c.out << url << "\n"; return 0; }
  // a local repository needs no repository of our own; open_side uses c.repo only for nothing here
  Side s = open_side(c, url);
  if (s.kind == Side::Network) network_ls(c, s);
  std::vector<std::string> patterns(p.pos.begin() + (p.pos.empty() ? 0 : 1), p.pos.end());
  int shown = 0;
  bool heads = p.has("heads") || p.has("branches"), tags = p.has("tags");
  if (p.has("symref") && !s.head_ref.empty() && !heads && !tags) c.out << "ref: " << s.head_ref << "\tHEAD\n";
  for (auto& r : s.refs) {
    if (heads || tags) {
      bool ok = (heads && r.name.rfind("refs/heads/", 0) == 0) || (tags && r.name.rfind("refs/tags/", 0) == 0);
      if (!ok) continue;
    }
    if (p.has("refs") && (r.name == "HEAD" || r.name.find("^{}") != std::string::npos)) continue;
    if (!patterns.empty()) {
      bool any = false;
      for (auto& pat : patterns) {
        std::string tail = "/" + pat;
        if (r.name == pat || (r.name.size() >= tail.size() && r.name.compare(r.name.size() - tail.size(), tail.size(), tail) == 0)) any = true;
        else if (pat.find_first_of("*?[") != std::string::npos && wildmatch("*" + tail, r.name, false, false)) any = true;   // tail_match: wildmatch without WM_PATHNAME
      }
      if (!any) continue;
    }
    c.out << r.oid << "\t" << r.name << "\n";
    ++shown;
  }
  if (p.has("exit-code") && !shown) return 2;
  return 0;
}

// ---------------------------------------------------------------------------
int cmd_remote(Ctx& c) {
  std::vector<std::string> args(c.args.begin() + 1, c.args.end());
  bool verbose = false;
  while (!args.empty() && (args[0] == "-v" || args[0] == "--verbose")) { verbose = true; args.erase(args.begin()); }
  c.open_repo(false);
  std::string cfg = c.repo->common_dir() + "/config";
  auto remotes = [&]() {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (auto& e : c.config.entries()) {
      if (e.key.rfind("remote.", 0) != 0) continue;
      size_t last = e.key.rfind('.');
      if (last <= 7) continue;
      std::string r = e.key.substr(7, last - 7);
      std::string var = e.key.substr(last + 1);
      if ((var == "url" || var == "fetch" || var == "pushurl") && seen.insert(r).second) out.push_back(r);
    }
    std::sort(out.begin(), out.end());
    return out;
  };
  if (args.empty()) {
    for (auto& r : remotes()) {
      if (!verbose) { c.out << r << "\n"; continue; }
      std::string url = c.config.get("remote." + r + ".url").value_or("");
      auto pushes = c.config.get_all("remote." + r + ".pushurl");
      c.out << r << "\t" << url << " (fetch)\n";
      if (pushes.empty()) c.out << r << "\t" << url << " (push)\n";
      for (auto& pu : pushes) c.out << r << "\t" << pu << " (push)\n";
    }
    return 0;
  }
  std::string sub = args[0];
  std::vector<std::string> sargs{"remote"};
  sargs.insert(sargs.end(), args.begin() + 1, args.end());
  std::string err;
  auto exists = [&](const std::string& r) { auto all = remotes(); return std::find(all.begin(), all.end(), r) != all.end(); };
  if (sub == "add") {
    auto p = parse_options(c, sargs, 1, {{'f', "fetch"}, {0, "tags"}, {'t', "track", Arg::Required}, {'m', "master", Arg::Required},
                                          {0, "mirror", Arg::Optional}});
    if (p.has("mirror")) unsupported(c, "remote add --mirror");
    if (p.pos.size() != 2) usage(c, "usage: git remote add [<options>] <name> <url>");
    std::string name = p.pos[0], url = p.pos[1];
    if (exists(name)) { c.err << "error: remote " << name << " already exists.\n"; return 3; }
    if (!Git::valid_ref_name("refs/remotes/" + name + "/x")) die(c, "'" + name + "' is not a valid remote name");
    edit_config_file(c.host(), cfg, "remote." + name + ".url", url, ConfigEdit::Set, &err);
    auto tracks = p.all("track");
    if (tracks.empty()) edit_config_file(c.host(), cfg, "remote." + name + ".fetch", "+refs/heads/*:refs/remotes/" + name + "/*", ConfigEdit::Add, &err);
    for (auto& t : tracks) edit_config_file(c.host(), cfg, "remote." + name + ".fetch", "+refs/heads/" + t + ":refs/remotes/" + name + "/" + t, ConfigEdit::Add, &err);
    if (p.negated.count("tags")) edit_config_file(c.host(), cfg, "remote." + name + ".tagopt", "--no-tags", ConfigEdit::Set, &err);
    else if (p.has("tags")) edit_config_file(c.host(), cfg, "remote." + name + ".tagopt", "--tags", ConfigEdit::Set, &err);
    c.load_config();
    if (p.has("master")) {
      std::string body = "ref: refs/remotes/" + name + "/" + p.get("master") + "\n";
      (void)c.host().make_dir(c.repo->common_dir() + "/refs/remotes/" + name);
      c.host().write_file(c.repo->common_dir() + "/refs/remotes/" + name + "/HEAD", ByteSpan((const uint8_t*)body.data(), body.size()));
    }
    if (p.has("fetch")) {
      c.args = {"fetch", name};
      return cmd_fetch(c);
    }
    return 0;
  }
  if (sub == "remove" || sub == "rm") {
    if (sargs.size() != 2) usage(c, "usage: git remote remove <name>");
    std::string name = sargs[1];
    if (!exists(name)) { c.err << "error: No such remote: '" << name << "'\n"; return 2; }
    remove_config_section(c.host(), cfg, "remote." + name);
    for (auto& [ref, oid] : all_refs(c)) if (ref.rfind("refs/remotes/" + name + "/", 0) == 0) (void)c.repo->delete_ref(ref);
    (void)c.host().remove(c.repo->common_dir() + "/refs/remotes/" + name + "/HEAD");
    // branches that tracked it stop doing so
    for (auto& e : c.config.entries()) {
      if (e.key.rfind("branch.", 0) == 0 && e.key.size() > 14 && e.key.compare(e.key.size() - 7, 7, ".remote") == 0 && e.value == name) {
        std::string b = e.key.substr(7, e.key.size() - 14);
        edit_config_file(c.host(), cfg, "branch." + b + ".remote", "", ConfigEdit::UnsetAll, &err);
        edit_config_file(c.host(), cfg, "branch." + b + ".merge", "", ConfigEdit::UnsetAll, &err);
      }
    }
    return 0;
  }
  if (sub == "rename") {
    if (sargs.size() != 3) usage(c, "usage: git remote rename <old> <new>");
    std::string from = sargs[1], to = sargs[2];
    if (!exists(from)) { c.err << "error: No such remote: '" << from << "'\n"; return 2; }
    if (exists(to)) { c.err << "error: remote " << to << " already exists.\n"; return 3; }
    auto fetches = c.config.get_all("remote." + from + ".fetch");
    rename_config_section(c.host(), cfg, "remote." + from, "remote." + to);
    edit_config_file(c.host(), cfg, "remote." + to + ".fetch", "", ConfigEdit::UnsetAll, &err);
    for (auto f : fetches) {
      size_t at = f.find("refs/remotes/" + from + "/");
      if (at != std::string::npos) f.replace(at, 13 + from.size(), "refs/remotes/" + to);
      edit_config_file(c.host(), cfg, "remote." + to + ".fetch", f, ConfigEdit::Add, &err);
    }
    for (auto& [ref, oid] : all_refs(c)) {
      if (ref.rfind("refs/remotes/" + from + "/", 0) != 0) continue;
      (void)c.repo->update_ref("refs/remotes/" + to + "/" + ref.substr(14 + from.size()), oid);
      (void)c.repo->delete_ref(ref);
    }
    c.load_config();
    for (auto& e : c.config.entries())
      if (e.key.rfind("branch.", 0) == 0 && e.key.size() > 14 && e.key.compare(e.key.size() - 7, 7, ".remote") == 0 && e.value == from)
        edit_config_file(c.host(), cfg, e.key, to, ConfigEdit::Set, &err);
    return 0;
  }
  if (sub == "get-url") {
    auto p = parse_options(c, sargs, 1, {{0, "push"}, {0, "all"}});
    if (p.pos.size() != 1) usage(c, "usage: git remote get-url [--push] [--all] <name>");
    std::string name = p.pos[0];
    if (!exists(name)) { c.err << "error: No such remote '" << name << "'\n"; return 2; }
    std::vector<std::string> urls = p.has("push") ? c.config.get_all("remote." + name + ".pushurl") : std::vector<std::string>{};
    if (urls.empty()) urls = c.config.get_all("remote." + name + ".url");
    if (p.has("all")) for (auto& u : urls) c.out << u << "\n";
    else if (!urls.empty()) c.out << remote_url(c, name) << "\n";
    return 0;
  }
  if (sub == "set-url") {
    auto p = parse_options(c, sargs, 1, {{0, "push"}, {0, "add"}, {0, "delete"}});
    if (p.pos.size() < 2) usage(c, "usage: git remote set-url [--push] <name> <newurl> [<oldurl>]");
    std::string name = p.pos[0], url = p.pos[1];
    if (!exists(name)) { c.err << "error: No such remote '" << name << "'\n"; return 2; }
    std::string key = "remote." + name + (p.has("push") ? ".pushurl" : ".url");
    if (p.has("delete")) { edit_config_file(c.host(), cfg, key, "", ConfigEdit::UnsetAll, &err); return 0; }
    edit_config_file(c.host(), cfg, key, url, p.has("add") ? ConfigEdit::Add : ConfigEdit::ReplaceAll, &err);
    return 0;
  }
  if (sub == "set-head") {
    auto p = parse_options(c, sargs, 1, {{'a', "auto"}, {'d', "delete"}});
    if (p.pos.empty()) usage(c, "usage: git remote set-head <name> (-a | --auto | -d | --delete | <branch>)");
    std::string name = p.pos[0];
    std::string path = c.repo->common_dir() + "/refs/remotes/" + name + "/HEAD";
    if (p.has("delete")) { (void)c.host().remove(path); return 0; }
    std::string branch;
    if (p.has("auto")) {
      Side s = open_side(c, remote_url(c, name));
      if (s.kind == Side::Network) network_ls(c, s);
      if (s.head_ref.rfind("refs/heads/", 0) != 0) die(c, "Cannot determine remote HEAD");
      branch = s.head_ref.substr(11);
    } else {
      if (p.pos.size() < 2) usage(c, "usage: git remote set-head <name> <branch>");
      branch = p.pos[1];
      if (!read_ref(c, "refs/remotes/" + name + "/" + branch)) die(c, "Not a valid ref: refs/remotes/" + name + "/" + branch);
    }
    std::string body = "ref: refs/remotes/" + name + "/" + branch + "\n";
    (void)c.host().make_dir(dirname_of(path));
    c.host().write_file(path, ByteSpan((const uint8_t*)body.data(), body.size()));
    c.out << name << "/HEAD set to " << branch << "\n";
    return 0;
  }
  if (sub == "prune") {
    for (size_t i = 1; i < sargs.size(); ++i) {
      if (sargs[i][0] == '-') continue;
      c.args = {"fetch", "--prune", sargs[i]};
      int rc = cmd_fetch(c);
      if (rc) return rc;
    }
    return 0;
  }
  if (sub == "show" || sub == "update" || sub == "set-branches") unsupported(c, "remote " + sub);
  usage(c, "unknown subcommand: " + sub);
}

// ---------------------------------------------------------------------------
int cmd_bundle(Ctx& c) {
  if (c.args.size() < 2) usage(c, "usage: git bundle (create|verify|list-heads|unbundle) ...");
  std::string sub = c.args[1];
  if (sub == "create") {
    std::vector<std::string> args(c.args.begin() + 2, c.args.end());
    bool quiet = false;
    std::vector<std::string> rest;
    for (auto& a : args) {
      if (a == "-q" || a == "--quiet") { quiet = true; continue; }
      if (a == "--progress" || a == "--no-progress" || a == "--all-progress" || a == "--all-progress-implied") continue;
      if (a.rfind("--version=", 0) == 0) { if (a != "--version=2") unsupported(c, "bundle " + a); continue; }
      rest.push_back(a);
    }
    (void)quiet;
    if (rest.empty()) usage(c, "usage: git bundle create [<options>] <file> <git-rev-list args>");
    std::string file = rest[0];
    rest.erase(rest.begin());
    c.open_repo(false);
    // which refs go in, and what the receiver must already have
    std::vector<std::pair<std::string, std::string>> refs;   // oid, name
    std::vector<std::string> tips, hide;
    bool negate = false;
    auto add_ref = [&](const std::string& name, const std::string& oid) {
      for (auto& r : refs) if (r.second == name) return;
      refs.push_back({oid, name});
    };
    for (auto& a : rest) {
      if (a == "--not") { negate = !negate; continue; }
      if (a == "--all" || a == "--branches" || a == "--tags" || a == "--remotes") {
        std::string pre = a == "--all" ? "refs/" : a == "--branches" ? "refs/heads/" : a == "--tags" ? "refs/tags/" : "refs/remotes/";
        for (auto& [ref, oid] : all_refs(c)) {
          if (ref.rfind(pre, 0) != 0) continue;
          if (negate) hide.push_back(oid);
          else { tips.push_back(oid); add_ref(ref, oid); }
        }
        if (a == "--all" && !negate) if (auto h = read_ref(c, "HEAD")) { tips.push_back(*h); add_ref("HEAD", *h); }
        continue;
      }
      size_t dd = a.find("..");
      if (dd != std::string::npos) {
        std::string l = a.substr(0, dd), r = a.substr(dd + 2);
        hide.push_back(commit_of(c, l.empty() ? "HEAD" : l));
        std::string rname = r.empty() ? "HEAD" : r;
        std::string full = rname == "HEAD" ? "HEAD" : dwim_ref(c, rname);
        std::string oid = must_resolve(c, rname);
        tips.push_back(oid);
        if (!full.empty()) add_ref(full, oid);
        continue;
      }
      if (a.size() > 1 && a[0] == '^') { hide.push_back(commit_of(c, a.substr(1))); continue; }
      if (a.size() > 1 && a[0] == '-') unsupported(c, "bundle create option " + a);
      std::string oid = must_resolve(c, a);
      if (negate) { hide.push_back(peel_to(c, oid, "commit")); continue; }
      tips.push_back(oid);
      std::string full = a == "HEAD" ? "HEAD" : dwim_ref(c, a);
      if (!full.empty()) add_ref(full, oid);
    }
    if (refs.empty()) die(c, "Refusing to create empty bundle.");
    auto objs = git_objects_between(*c.repo, tips, hide);
    if (!objs.ok()) die(c, objs.error().message);
    uint64_t counted = 0;
    auto pack = git_build_pack(*c.repo, *objs, counted);
    if (!pack.ok()) die(c, pack.error().message);
    std::string out = "# v2 git bundle\n";
    // prerequisites: the excluded commits the included history builds on
    std::set<std::string> included(objs->begin(), objs->end());
    std::set<std::string> prereq;
    for (auto& o : *objs) {
      if (type_of(c, o) != "commit") continue;
      for (auto& par : read_commit(c, o).parents) if (!included.count(par)) prereq.insert(par);
    }
    for (auto& pr : prereq) out += "-" + pr + " " + subject_of(read_commit(c, pr).message) + "\n";
    for (auto& [oid, name] : refs) out += oid + " " + name + "\n";
    out += "\n";
    out.append((const char*)pack->data(), pack->size());
    if (file == "-") c.out.write(out.data(), (std::streamsize)out.size());
    else {
      auto w = c.host().write_file(c.abs_path(file), ByteSpan((const uint8_t*)out.data(), out.size()));
      if (!w.ok()) die(c, "cannot create '" + file + "': " + w.error().message);
    }
    return 0;
  }
  if (c.args.size() < 3) usage(c, "usage: git bundle " + sub + " <file>");
  std::string file = c.args[2];
  if (file.size() > 1 && file[0] == '-') { if (c.args.size() < 4) usage(c, "usage: git bundle " + sub + " <file>"); file = c.args[3]; }
  Bundle b;
  std::string err;
  if (!read_bundle(c, file == "-" ? "-" : c.abs_path(file), b, &err)) die(c, err);
  if (sub == "list-heads") {
    std::vector<std::string> only(c.args.begin() + 3, c.args.end());
    for (auto& [oid, ref] : b.refs) {
      if (!only.empty() && std::find(only.begin(), only.end(), ref) == only.end()) continue;
      c.out << oid << " " << ref << "\n";
    }
    return 0;
  }
  if (sub == "verify") {
    bool quiet = std::find(c.args.begin(), c.args.end(), "-q") != c.args.end() || std::find(c.args.begin(), c.args.end(), "--quiet") != c.args.end();
    c.open_repo(false);
    std::vector<std::string> lacking;
    for (auto& [oid, comment] : b.prereqs) if (!c.repo->has_object(oid)) lacking.push_back(oid + " ");
    if (!lacking.empty()) {
      c.err << "error: Repository lacks these prerequisite commits:\n";
      for (auto& l : lacking) c.err << "error: " << l << "\n";
      return 1;
    }
    if (!quiet) {
      c.out << "The bundle contains " << (b.refs.size() == 1 ? "this ref:\n" : "these " + std::to_string(b.refs.size()) + " refs:\n");
      for (auto& [oid, ref] : b.refs) c.out << oid << " " << ref << "\n";
      if (b.prereqs.empty()) c.out << "The bundle records a complete history.\n";
      else {
        c.out << "The bundle requires " << (b.prereqs.size() == 1 ? "this ref:\n" : "these " + std::to_string(b.prereqs.size()) + " refs:\n");
        for (auto& [oid, comment] : b.prereqs) c.out << oid << " \n";
      }
      c.out << "The bundle uses this hash algorithm: sha1\n";
    }
    c.err << file << " is okay\n";
    return 0;
  }
  if (sub == "unbundle") {
    c.open_repo(false);
    for (auto& [oid, comment] : b.prereqs)
      if (!c.repo->has_object(oid)) { c.err << "error: Repository lacks these prerequisite commits:\nerror: " << oid << " \n"; return 1; }
    if (!b.pack.empty()) {
      auto r = git_index_pack(*c.repo, ByteSpan((const uint8_t*)b.pack.data(), b.pack.size()));
      if (!r.ok()) die(c, "index-pack died: " + r.error().message);
    }
    for (auto& [oid, ref] : b.refs) c.out << oid << " " << ref << "\n";
    return 0;
  }
  usage(c, "unknown subcommand: " + sub);
}

// ---------------------------------------------------------------------------
namespace {
std::string gunzip(const std::string& in, bool& ok) {
  ok = false;
  if (in.size() < 18 || (uint8_t)in[0] != 0x1f || (uint8_t)in[1] != 0x8b || in[2] != 8) return "";
  uint8_t flags = (uint8_t)in[3];
  size_t pos = 10;
  if (flags & 4) { if (pos + 2 > in.size()) return ""; size_t xlen = (uint8_t)in[pos] | ((size_t)(uint8_t)in[pos + 1] << 8); pos += 2 + xlen; }
  if (flags & 8) { while (pos < in.size() && in[pos]) ++pos; ++pos; }
  if (flags & 16) { while (pos < in.size() && in[pos]) ++pos; ++pos; }
  if (flags & 2) pos += 2;
  if (pos >= in.size()) return "";
  size_t end = 0;
  auto r = inflate_raw(ByteSpan((const uint8_t*)in.data(), in.size()), pos, &end);
  if (!r.ok()) return "";
  ok = true;
  return std::string((const char*)r->data(), r->size());
}
void cgi_reply(Ctx& c, int status, const std::string& type, const std::string& body) {
  if (status != 200) c.out << "Status: " << status << (status == 403 ? " Forbidden" : status == 404 ? " Not Found" : status == 400 ? " Bad Request" : status == 405 ? " Method Not Allowed" : " Internal Server Error") << "\r\n";
  c.out << "Expires: Fri, 01 Jan 1980 00:00:00 GMT\r\nPragma: no-cache\r\nCache-Control: no-cache, max-age=0, must-revalidate\r\n";
  if (!type.empty()) c.out << "Content-Type: " << type << "\r\n";
  c.out << "\r\n";
  c.out.write(body.data(), (std::streamsize)body.size());
}
}  // namespace

namespace {
// hooks/pre-receive, when the repository has one: "<old> <new> <ref>" lines on its standard input, in the repository;
// a non-zero exit declines the push, and what it printed goes back to the client.
GitPreReceive pre_receive_hook(Ctx& c, const Git& repo) {
  const std::string hook = repo.git_dir() + "/hooks/pre-receive";
  if (!c.host().exists(hook)) return nullptr;
  return [&c, &repo, hook](const std::vector<ReceivedRef>& proposed) -> std::string {
    const std::string sh = hal::find_program({"sh", "aios-sh"});
    if (sh.empty()) return "pre-receive: no shell to run the hook\n";
    std::string lines;
    for (const auto& r : proposed) lines += r.old_oid + " " + r.new_oid + " " + r.ref + "\n";
    const std::string input = repo.git_dir() + "/pre-receive.input", output = repo.git_dir() + "/pre-receive.output";
    (void)c.host().remove(output);
    if (!c.host().write_file(input, ByteSpan(reinterpret_cast<const uint8_t*>(lines.data()), lines.size())).ok())
      return "pre-receive: cannot hand the hook its input\n";
    hal::ProcessSpawn spec;
    spec.argv = {sh, "-c", "exec \"$0\" < \"$1\"", hook, input};
    spec.working_dir = repo.git_dir();
    spec.env = {{"GIT_DIR", "."}};
    spec.output_path = output;
    auto p = hal::spawn_process(spec);
    if (!p.ok()) { (void)c.host().remove(input); return "pre-receive: the hook did not start\n"; }
    auto st = hal::wait_process(*p, 120000);
    hal::release_process(*p);
    (void)c.host().remove(input);
    std::string said;
    if (auto f = c.host().read_file(output); f.ok()) said.assign(reinterpret_cast<const char*>(f->data()), f->size());
    (void)c.host().remove(output);
    const bool ok = st.ok() && st->exited && st->exit_code == 0;
    if (ok) return "";
    return said.empty() ? std::string("pre-receive hook declined the push\n") : said;
  };
}

// What a receiving repository does once a push is in: its hooks/post-receive runs with "<old> <new> <ref>" lines on its
// standard input (a file here: the hal process has no stdin), in the repository, before the client gets its answer.
void run_post_receive(Ctx& c, const Git& repo, const std::vector<ReceivedRef>& applied) {
  if (applied.empty()) return;
  const std::string hook = repo.git_dir() + "/hooks/post-receive";
  if (!c.host().exists(hook)) return;
  const std::string sh = hal::find_program({"sh", "aios-sh"});
  if (sh.empty()) return;
  std::string lines;
  for (const auto& r : applied) lines += r.old_oid + " " + r.new_oid + " " + r.ref + "\n";
  const std::string input = repo.git_dir() + "/post-receive.input";
  if (!c.host().write_file(input, ByteSpan(reinterpret_cast<const uint8_t*>(lines.data()), lines.size())).ok()) return;
  hal::ProcessSpawn spec;
  spec.argv = {sh, "-c", "exec \"$0\" < \"$1\"", hook, input};
  spec.working_dir = repo.git_dir();
  spec.env = {{"GIT_DIR", "."}};
  auto p = hal::spawn_process(spec);
  if (p.ok()) { hal::wait_process(*p, 60000); hal::release_process(*p); }
  c.host().remove(input);
}
}  // namespace

int cmd_http_backend(Ctx& c) {
  std::string root = slashes(c.getenv("GIT_PROJECT_ROOT"));
  std::string path_info = slashes(c.getenv("PATH_INFO"));
  std::string method = c.getenv("REQUEST_METHOD");
  std::string query = c.getenv("QUERY_STRING");
  c.load_config();
  if (path_info.empty()) { cgi_reply(c, 400, "text/plain", "No PATH_INFO\n"); return 0; }
  std::string service, repo_rel;
  const std::vector<std::string> tails = {"/info/refs", "/git-upload-pack", "/git-receive-pack"};
  std::string tail;
  for (auto& t : tails)
    if (path_info.size() > t.size() && path_info.compare(path_info.size() - t.size(), t.size(), t) == 0) { tail = t; repo_rel = path_info.substr(0, path_info.size() - t.size()); }
  if (tail.empty()) { cgi_reply(c, 404, "text/plain", "Not Found\n"); return 0; }
  if (repo_rel.find("..") != std::string::npos) { cgi_reply(c, 403, "text/plain", "Forbidden\n"); return 0; }
  std::string dir = root.empty() ? repo_rel : normalize_path(root + "/" + repo_rel);
  Git repo(*c.kernel);
  bool opened = false;
  if (c.host().exists(dir + "/HEAD") && c.host().exists(dir + "/objects")) opened = repo.open_dirs(dir, "").ok();
  else if (c.host().exists(dir + "/.git")) opened = repo.open(dir).ok();
  else if (c.host().exists(dir + ".git/HEAD")) opened = repo.open_dirs(dir + ".git", "").ok();
  if (!opened) { cgi_reply(c, 404, "text/plain", "Not Found\n"); return 0; }
  if (c.getenv("GIT_HTTP_EXPORT_ALL").empty() && !c.host().exists(repo.git_dir() + "/git-daemon-export-ok")) {
    cgi_reply(c, 404, "text/plain", "Not Found\n");
    return 0;
  }
  // receive-pack: allowed when http.receivepack says so, or (unset) for an authenticated user
  Config rcfg = c.config;
  if (auto f = c.host().read_file(repo.common_dir() + "/config"); f.ok()) {
    Config local;
    local.add_text(std::string((const char*)f->data(), f->size()));
    for (auto& e : local.entries()) rcfg.add(e.key, e.value, e.has_value);
  }
  long long n = std::atoll(c.getenv("GIT_CONFIG_COUNT").c_str());
  for (long long i = 0; i < n; ++i) rcfg.add(c.getenv("GIT_CONFIG_KEY_" + std::to_string(i)), c.getenv("GIT_CONFIG_VALUE_" + std::to_string(i)));
  auto allowed = [&](const std::string& svc) {
    if (svc == "git-upload-pack") return rcfg.get_bool("http.uploadpack", true);
    auto v = rcfg.get("http.receivepack");
    if (v) return Config::parse_bool(*v, true);
    return !c.getenv("REMOTE_USER").empty();
  };
  if (tail == "/info/refs") {
    size_t at = query.find("service=");
    if (at == std::string::npos) { cgi_reply(c, 403, "text/plain", "dumb HTTP is not served\n"); return 0; }
    service = query.substr(at + 8);
    size_t amp = service.find('&');
    if (amp != std::string::npos) service = service.substr(0, amp);
    if (service != "git-upload-pack" && service != "git-receive-pack") { cgi_reply(c, 403, "text/plain", "Unsupported service\n"); return 0; }
    if (!allowed(service)) { cgi_reply(c, 403, "text/plain", "Service not enabled: '" + service + "'\n"); return 0; }
    // The client's protocol (git http-backend hands it on as GIT_PROTOCOL, a web server as HTTP_GIT_PROTOCOL): version=2
    // gets the v2 capability advertisement instead of the v1 ref list.
    const std::string proto = !c.getenv("GIT_PROTOCOL").empty() ? c.getenv("GIT_PROTOCOL") : c.getenv("HTTP_GIT_PROTOCOL");
    cgi_reply(c, 200, "application/x-" + service + "-advertisement", git_advertise_refs(repo, service, true, proto));
    return 0;
  }
  if (method != "POST") { cgi_reply(c, 405, "text/plain", "Method Not Allowed\n"); return 0; }
  service = tail.substr(1);
  if (!allowed(service)) { cgi_reply(c, 403, "text/plain", "Service not enabled: '" + service + "'\n"); return 0; }
  std::string body = read_all(c.in);
  std::string enc = c.getenv("HTTP_CONTENT_ENCODING");
  if (enc == "gzip" || enc == "x-gzip") {
    bool ok = false;
    body = gunzip(body, ok);
    if (!ok) { cgi_reply(c, 400, "text/plain", "Bad gzip request body\n"); return 0; }
  }
  std::vector<ReceivedRef> applied;
  auto r = service == "git-upload-pack" ? git_serve_upload_pack(repo, body) : git_serve_receive_pack(repo, body, &applied, pre_receive_hook(c, repo));
  if (!r.ok()) { cgi_reply(c, 500, "text/plain", r.error().message + "\n"); return 0; }
  run_post_receive(c, repo, applied);
  cgi_reply(c, 200, "application/x-" + service + "-result", *r);
  return 0;
}

namespace {
// One pkt-line of a live session, kept as it came (`raw`, its length included) so that a whole request can be handed
// to the stateless server, and as text without its newline. False at the end of the input or on a malformed line.
struct LivePkt { std::string raw, text; bool flush = false; };
bool read_live_pkt(std::istream& in, LivePkt& p) {
  char len[4];
  if (!in.read(len, 4)) return false;
  size_t n = 0;
  for (char ch : len) {
    int v = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
    if (v < 0) return false;
    n = n * 16 + (size_t)v;
  }
  p.raw.assign(len, 4);
  p.text.clear();
  p.flush = n == 0;
  if (n == 0 || n == 1) return true;   // a flush, or v2's delimiter
  if (n < 4) return false;
  std::string body(n - 4, '\0');
  if (!in.read(body.data(), (std::streamsize)body.size())) return false;
  p.raw += body;
  if (!body.empty() && body.back() == '\n') body.pop_back();
  p.text = std::move(body);
  return true;
}

// upload-pack as ssh (and a git daemon) runs it: one conversation on standard input and output, not one request. Each
// request is gathered whole and answered by the code that answers smart HTTP's, and every answer is flushed before the
// next request is read, because the client waits for it. GIT_PROTOCOL in the environment (what ssh's SendEnv and git
// daemon hand on) with version=2 in it gets the v2 conversation: the capability advertisement, then one command per
// request until the client sends a flush on its own or hangs up. Otherwise v0: the wants, rounds of haves, the pack.
int serve_upload_pack_live(Ctx& c, Git& repo) {
  const std::string adv = git_advertise_refs(repo, "git-upload-pack", false, c.getenv("GIT_PROTOCOL"));
  c.out.write(adv.data(), (std::streamsize)adv.size());
  c.out.flush();
  // `written`: how much of the answer to the conversation so far is already out. The v0 conversation is handed over
  // whole each time and answered whole (the shallow section, every round's ACKs and NAKs, then the pack), because
  // git's answers hang on state from earlier rounds (what was in common, the last common have that "done" names).
  // The answer to a longer conversation begins with the answer to a shorter one, so only what is past it is new.
  size_t written = 0;
  auto answer = [&](const std::string& req) {
    auto r = git_serve_upload_pack(repo, req);
    if (!r.ok()) die(c, r.error().message);
    if (r->size() > written) c.out.write(r->data() + written, (std::streamsize)(r->size() - written));
    written = std::max(written, r->size());
    c.out.flush();
  };
  LivePkt p;
  if (adv.size() > 13 && adv.compare(4, 9, "version 2") == 0) {
    for (;;) {
      std::string req;
      bool command = false;
      for (;;) {
        if (!read_live_pkt(c.in, p)) return 0;   // the client hung up
        req += p.raw;
        if (p.flush) break;
        command = true;
      }
      if (!command) return 0;   // a flush on its own: no more commands
      written = 0;   // each v2 command is a request of its own
      answer(req);
    }
  }
  std::string conv;
  bool any = false;
  for (;;) {
    if (!read_live_pkt(c.in, p)) return 0;
    conv += p.raw;
    if (p.flush) break;
    any = true;
  }
  if (!any) return 0;   // no wants: the client only wanted the refs (ls-remote, or nothing to fetch)
  answer(conv);         // with a depth, the shallow section, which the client reads before it says what it has
  for (;;) {
    // Each round of haves ends with a flush and is answered at once; "done" is answered with the pack.
    bool done = false;
    for (;;) {
      if (!read_live_pkt(c.in, p)) return 0;
      conv += p.raw;
      if (p.flush) break;
      if (p.text == "done") { done = true; break; }
    }
    answer(conv);
    if (done) return 0;
  }
}

int serve_pack(Ctx& c, const std::string& service) {
  auto p = parse_options(c, c.args, 1, {{0, "stateless-rpc"}, {0, "advertise-refs"}, {0, "http-backend-info-refs"}, {0, "strict"},
                                         {0, "timeout", Arg::Required}, {0, "quiet"}});
  if (p.pos.size() != 1) usage(c, "usage: git " + service.substr(4) + " [<options>] <dir>");
  std::string dir = c.abs_path(p.pos[0]);
  Git repo(*c.kernel);
  bool ok = (c.host().exists(dir + "/HEAD") && repo.open_dirs(dir, "").ok()) || repo.open(dir).ok();
  if (!ok) die(c, "'" + p.pos[0] + "' does not appear to be a git repository");
  if (p.has("advertise-refs") || p.has("http-backend-info-refs")) {
    std::string a = git_advertise_refs(repo, service, false, c.getenv("GIT_PROTOCOL"));
    c.out.write(a.data(), (std::streamsize)a.size());
    return 0;
  }
  if (!p.has("stateless-rpc")) {
    if (service == "git-upload-pack") return serve_upload_pack_live(c, repo);
    unsupported(c, service.substr(4) + " without --stateless-rpc (a live two-way session)");
  }
  std::string in = read_all(c.in);
  std::vector<ReceivedRef> applied;
  auto r = service == "git-upload-pack" ? git_serve_upload_pack(repo, in) : git_serve_receive_pack(repo, in, &applied, pre_receive_hook(c, repo));
  if (!r.ok()) die(c, r.error().message);
  run_post_receive(c, repo, applied);
  c.out.write(r->data(), (std::streamsize)r->size());
  return 0;
}
}  // namespace

int cmd_upload_pack(Ctx& c) { return serve_pack(c, "git-upload-pack"); }
int cmd_receive_pack(Ctx& c) { return serve_pack(c, "git-receive-pack"); }

}  // namespace aios_git
