// shortlog, count-objects, format-patch, submodule status: the commands that report on a repository without
// changing it. (blame is cmd_blame.cpp's.)
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>

#include "aios/hal/host.hpp"
#include "cli.hpp"

namespace aios_git {

using aios::ByteSpan;
using aios::userland::git_local_tz;
using aios::userland::git_split_lines;

namespace {

std::string trimmed(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string name_of(const std::string& ident) {
  size_t lt = ident.find(" <");
  return lt == std::string::npos ? ident : ident.substr(0, lt);
}
std::string mail_of(const std::string& ident) {
  size_t lt = ident.find('<');
  return lt == std::string::npos ? "<>" : ident.substr(lt);
}

}  // namespace

// ---------------------------------------------------------------------------
// shortlog
// ---------------------------------------------------------------------------
int cmd_shortlog(Ctx& c) {
  std::vector<std::string> args(c.args.begin() + 1, c.args.end());
  bool summary = false, numbered = false, email = false;
  std::vector<std::string> rest;
  for (auto& a : args) {
    if (a == "-s" || a == "--summary") summary = true;
    else if (a == "-n" || a == "--numbered") numbered = true;
    else if (a == "-e" || a == "--email") email = true;
    else if (a.size() > 1 && a[0] == '-' && a[1] != '-' && a.find_first_not_of("sne", 1) == std::string::npos) {
      summary |= a.find('s') != std::string::npos;
      numbered |= a.find('n') != std::string::npos;
      email |= a.find('e') != std::string::npos;
    } else if (a == "-c" || a == "--committer" || a.rfind("--group", 0) == 0 || a.rfind("-w", 0) == 0 || a == "--format")
      unsupported(c, "shortlog " + a);
    else rest.push_back(a);
  }
  c.open_repo(false);
  WalkOptions w;
  // the revision walk's own options (shortlog takes setup_revisions'): the common ones
  {
    std::vector<std::string> kept;
    auto num = [&](const std::string& v, const std::string& opt) {
      char* end = nullptr;
      const long n = std::strtol(v.c_str(), &end, 10);
      if (v.empty() || *end) die(c, "'" + v + "': not an integer" + (opt.empty() ? "" : " for " + opt));
      return n;
    };
    auto date = [&](const std::string& v) {
      int64_t t = 0;
      if (!parse_approxidate(v, t)) unsupported(c, "the date '" + v + "'");
      return t;
    };
    for (size_t i = 0; i < rest.size(); ++i) {
      const std::string& a = rest[i];
      if (a == "--") { kept.insert(kept.end(), rest.begin() + (long)i, rest.end()); break; }
      if (a.size() > 1 && a[0] == '-' && std::all_of(a.begin() + 1, a.end(), [](char ch) { return std::isdigit((unsigned char)ch); }))
        w.max_count = num(a.substr(1), "");
      else if (a.rfind("--max-count=", 0) == 0) w.max_count = num(a.substr(12), "--max-count");
      else if (a.rfind("--skip=", 0) == 0) w.skip = num(a.substr(7), "--skip");
      else if (a.rfind("--since=", 0) == 0 || a.rfind("--after=", 0) == 0) w.since = date(a.substr(8));
      else if (a.rfind("--until=", 0) == 0) w.until = date(a.substr(8));
      else if (a.rfind("--before=", 0) == 0) w.until = date(a.substr(9));
      else if (a.rfind("--author=", 0) == 0) w.authors.push_back(a.substr(9));
      else if (a.rfind("--grep=", 0) == 0) w.greps.push_back(a.substr(7));
      else if (a == "--no-merges") w.max_parents = 1;
      else if (a == "--merges") w.min_parents = 2;
      else if (a == "--first-parent") w.first_parent = true;
      else kept.push_back(a);
    }
    rest = std::move(kept);
  }
  std::vector<std::string> paths;
  auto dd = std::find(rest.begin(), rest.end(), "--");
  if (dd != rest.end()) { paths.assign(dd + 1, rest.end()); rest.erase(dd, rest.end()); }
  auto left = parse_revisions(c, rest, w, true);
  if (!left.empty()) usage(c, "unrecognized argument: " + left[0]);
  w.paths = make_pathspec(c, paths);
  std::map<std::string, std::vector<std::string>> by;   // author -> subjects, newest first
  for (auto& cm : walk(c, w)) {
    std::string who = email ? name_of(cm.author) + " " + mail_of(cm.author) : name_of(cm.author);
    by[who].push_back(subject_of(cm.message));
  }
  std::vector<std::pair<std::string, std::vector<std::string>>> rows(by.begin(), by.end());
  if (numbered)
    std::stable_sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.second.size() > b.second.size(); });
  for (auto& [who, subjects] : rows) {
    if (summary) {
      char buf[16];
      std::snprintf(buf, sizeof buf, "%6zu", subjects.size());
      c.out << buf << "\t" << who << "\n";
      continue;
    }
    c.out << who << " (" << subjects.size() << "):\n";
    for (auto it = subjects.rbegin(); it != subjects.rend(); ++it) c.out << "      " << *it << "\n";
    c.out << "\n";
  }
  return 0;
}

// ---------------------------------------------------------------------------
// count-objects
// ---------------------------------------------------------------------------
int cmd_count_objects(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'v', "verbose"}, {'H', "human-readable"}});
  c.open_repo(false);
  if (p.has("human-readable")) unsupported(c, "count-objects -H");
  auto& h = c.host();
  const std::string objects = c.repo->common_dir() + "/objects";
  uint64_t loose = 0, loose_bytes = 0, in_pack = 0, packs = 0, pack_bytes = 0, garbage = 0, garbage_bytes = 0, prunable = 0;
  std::set<std::string> loose_ids;
  for (int d = 0; d < 256; ++d) {
    char dir[3];
    std::snprintf(dir, sizeof dir, "%02x", d);
    auto ents = h.list_dir(objects + "/" + dir);
    if (!ents.ok()) continue;
    for (auto& e : *ents) {
      auto st = h.stat(objects + "/" + dir + "/" + e);
      if (!st.ok() || st->is_dir) continue;
      bool hex = e.size() == 38 && std::all_of(e.begin(), e.end(), [](char ch) { return std::isxdigit((unsigned char)ch); });
      // a loose object counts what it takes on disk (git's on_disk_bytes: st_blocks * 512)
      if (hex) { ++loose; loose_bytes += st->disk_bytes ? st->disk_bytes : st->size; loose_ids.insert(dir + e); }
      else { ++garbage; garbage_bytes += st->size; }
    }
  }
  auto ents = h.list_dir(objects + "/pack");
  if (ents.ok()) {
    std::set<std::string> names(ents->begin(), ents->end());
    for (auto& e : names) {
      auto st = h.stat(objects + "/pack/" + e);
      if (!st.ok() || st->is_dir) continue;
      auto ends = [&](const char* s) { size_t n = std::strlen(s); return e.size() > n && e.compare(e.size() - n, n, s) == 0; };
      std::string stem = e.substr(0, e.rfind('.'));
      if (ends(".pack")) {
        if (!names.count(stem + ".idx")) { ++garbage; garbage_bytes += st->size; continue; }
        ++packs;
        pack_bytes += st->size;
      } else if (ends(".idx")) {
        if (!names.count(stem + ".pack")) { ++garbage; garbage_bytes += st->size; continue; }
        pack_bytes += st->size;
        // version 2 index: "\377tOc", version, 256 fan-out counts (the last is the total), then the sorted ids.
        // Mapped, not read: the count is in the header, and a loose id is looked up by binary search (the whole
        // 3 MB index was read and every id in it spelled out in hex).
        auto view = h.view_file(objects + "/pack/" + e);
        if (!view.ok() || (*view)->size < 8 + 256 * 4) continue;
        const uint8_t* b = (*view)->data;
        const size_t size = (*view)->size;
        bool v2 = b[0] == 0xff && b[1] == 't' && b[2] == 'O' && b[3] == 'c';
        size_t fan = v2 ? 8 : 0;
        auto be32 = [&](size_t at) { return (uint32_t)b[at] << 24 | (uint32_t)b[at + 1] << 16 | (uint32_t)b[at + 2] << 8 | b[at + 3]; };
        uint32_t n = be32(fan + 255 * 4);
        in_pack += n;
        const size_t stride = v2 ? 20 : 24, first = v2 ? fan + 256 * 4 : 256 * 4 + 4;
        if (first + (size_t)n * stride > size) continue;
        for (auto& id : loose_ids) {
          uint8_t want[20];
          bool ok = id.size() == 40;
          for (size_t k = 0; ok && k < 20; ++k) {
            auto nib = [](char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1; };
            int hi = nib(id[2 * k]), lo = nib(id[2 * k + 1]);
            if (hi < 0 || lo < 0) ok = false;
            else want[k] = (uint8_t)(hi << 4 | lo);
          }
          if (!ok) continue;
          uint32_t lo = want[0] ? be32(fan + (size_t)(want[0] - 1) * 4) : 0, hi = be32(fan + (size_t)want[0] * 4);
          while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            int cmp = std::memcmp(b + first + (size_t)mid * stride, want, 20);
            if (cmp == 0) { ++prunable; break; }
            if (cmp < 0) lo = mid + 1; else hi = mid;
          }
        }
      } else if (!(ends(".keep") || ends(".bitmap") || ends(".rev") || ends(".promisor") || ends(".mtimes"))) {
        ++garbage;
        garbage_bytes += st->size;
      }
    }
  }
  if (!p.has("verbose")) {
    c.out << loose << " objects, " << loose_bytes / 1024 << " kilobytes\n";
    return 0;
  }
  c.out << "count: " << loose << "\nsize: " << loose_bytes / 1024 << "\nin-pack: " << in_pack << "\npacks: " << packs
        << "\nsize-pack: " << pack_bytes / 1024 << "\nprune-packable: " << prunable << "\ngarbage: " << garbage
        << "\nsize-garbage: " << garbage_bytes / 1024 << "\n";
  return 0;
}

// ---------------------------------------------------------------------------
// format-patch
// ---------------------------------------------------------------------------
namespace {

bool needs_rfc2047(const std::string& s) {
  for (unsigned char ch : s) if (ch >= 0x80) return true;
  return false;
}
// RFC 2047 "Q" encoding of a header value, as git writes a non-ASCII subject or name: a space is "=20" (not '_',
// which some readers leave as it is), a name keeps only letters, digits and !*+-/ as they are, and an encoded word
// longer than 76 columns (counting `used`, what the header line holds before it) goes on in a new one.
std::string rfc2047(const std::string& s, bool address, size_t used) {
  static const char* hx = "0123456789ABCDEF";
  std::string o = "=?UTF-8?q?";
  size_t line = used + o.size();
  for (unsigned char ch : s) {
    bool special = ch >= 0x80 || !std::isprint(ch) || ch == ' ' || ch == '=' || ch == '?' || ch == '_' ||
                   (address && !(std::isalnum(ch) || ch == '!' || ch == '*' || ch == '+' || ch == '-' || ch == '/'));
    if (line + 2 + (special ? 3 : 1) > 76) { o += "?=\n =?UTF-8?q?"; line = 11; }
    if (special) { o += '='; o += hx[ch >> 4]; o += hx[ch & 15]; line += 3; }
    else { o += (char)ch; ++line; }
  }
  return o + "?=";
}

// A plain header value folded as git folds a subject: words go on while the line stays within 78 columns, then
// on a new line that starts with a space.
std::string folded(const std::string& s, size_t used) {
  std::string o;
  size_t line = used;
  std::istringstream ws(s);
  std::string w;
  bool first = true;
  while (ws >> w) {
    if (!first && line + 1 + w.size() > 78) { o += "\n "; line = 1; }
    else if (!first) { o += ' '; ++line; }
    o += w;
    line += w.size();
    first = false;
  }
  return o;
}

// git's file name for a patch: the subject with each run of other characters made one '-', at most 52 of them
std::string sanitized_subject(const std::string& subject) {
  std::string o;
  bool dash = false;
  for (unsigned char ch : subject) {
    if (std::isalnum(ch) || ch == '.' || ch == '_') {
      if (dash && !o.empty()) o += '-';
      dash = false;
      o += (char)ch;
    } else {
      dash = true;
    }
  }
  while (!o.empty() && o[0] == '.') o.erase(0, 1);
  if (o.size() > 52) o.resize(52);
  while (!o.empty() && (o.back() == '.' || o.back() == '-')) o.pop_back();
  return o;
}

}  // namespace

int cmd_format_patch(Ctx& c) {
  std::vector<std::string> args(c.args.begin() + 1, c.args.end());
  bool to_stdout = false, numbered = false;
  long long count = -1;
  std::string out_dir;
  std::vector<std::string> revs;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--stdout") to_stdout = true;
    else if (a == "-n" || a == "--numbered") numbered = true;
    else if (a == "-o" && i + 1 < args.size()) out_dir = args[++i];
    else if (a.rfind("--output-directory=", 0) == 0) out_dir = a.substr(19);
    else if (a.size() > 1 && a[0] == '-' && std::all_of(a.begin() + 1, a.end(), [](char ch) { return std::isdigit((unsigned char)ch); }))
      count = std::atoll(a.c_str() + 1);
    else if (a.rfind("--max-count=", 0) == 0) count = std::atoll(a.c_str() + 12);
    else if (a == "--no-stat" || a == "-p" || a == "--cover-letter" || a.rfind("--subject-prefix", 0) == 0 ||
             a == "--root" || a.rfind("--signoff", 0) == 0 || a == "-s" || a == "-k" || a.rfind("--in-reply-to", 0) == 0 ||
             a.rfind("--thread", 0) == 0 || a.rfind("--to", 0) == 0 || a.rfind("--cc", 0) == 0 || a == "--attach" || a == "--inline")
      unsupported(c, "format-patch " + a);
    else if (a.size() > 1 && a[0] == '-') usage(c, "unknown option `" + a + "'");
    else revs.push_back(a);
  }
  c.open_repo(false);
  WalkOptions w;
  if (revs.size() == 1 && count < 0 && revs[0].find("..") == std::string::npos) {
    // "<since>": the commits on HEAD that are not in <since>
    w.tips = {commit_of(c, "HEAD")};
    w.hide = {commit_of(c, revs[0])};
  } else {
    if (revs.empty()) revs.push_back("HEAD");
    auto left = parse_revisions(c, revs, w, true);
    if (!left.empty()) usage(c, "unrecognized argument: " + left[0]);
  }
  if (count >= 0) w.max_count = count;
  w.max_parents = 1;   // git skips merges
  auto list = walk(c, w);
  std::reverse(list.begin(), list.end());   // oldest first, as they apply
  const size_t total = list.size();
  for (size_t k = 0; k < total; ++k) {
    const GitCommit& cm = list[k];
    std::ostringstream m;
    if (to_stdout && k > 0) m << "\n";   // git's separator between patches on one stream
    m << "From " << cm.oid << " Mon Sep 17 00:00:00 2001\n";
    std::string name = name_of(cm.author);
    m << "From: " << (needs_rfc2047(name) ? rfc2047(name, true, 6) : name) << " " << mail_of(cm.author) << "\n";
    m << "Date: " << format_date((int64_t)cm.author_time, cm.author_tz, "rfc") << "\n";
    std::string subject = subject_of(cm.message);
    std::string head = "Subject: " + (numbered || total > 1 ? "[PATCH " + std::to_string(k + 1) + "/" + std::to_string(total) + "] "
                                                               : std::string("[PATCH] "));
    m << head << (needs_rfc2047(subject) ? rfc2047(subject, false, head.size()) : folded(subject, head.size())) << "\n";
    std::string body;
    size_t nl = cm.message.find("\n\n");
    if (nl != std::string::npos) body = cm.message.substr(nl + 2);
    if (needs_rfc2047(cm.message) || needs_rfc2047(cm.author))
      m << "MIME-Version: 1.0\nContent-Type: text/plain; charset=UTF-8\nContent-Transfer-Encoding: 8bit\n";
    m << "\n";
    if (!body.empty()) m << body << (body.back() == '\n' ? "" : "\n");
    m << "---\n";
    std::string from = cm.parents.empty() ? "" : read_commit(c, cm.parents[0]).tree;
    auto pairs = diff_trees(c, from, cm.tree, Pathspec{}, DiffOptions{});
    DiffOptions st;
    st.format = DiffOptions::Stat;
    st.extra = {DiffOptions::Summary};
    st.stat_width = 72;
    print_diff(c, pairs, st, m);
    m << "\n";
    print_diff(c, pairs, DiffOptions{}, m);
    m << "-- \n2.45.0\n\n";
    if (to_stdout) { c.out << m.str(); continue; }
    char num[8];
    std::snprintf(num, sizeof num, "%04zu", k + 1);
    std::string file = std::string(num) + "-" + sanitized_subject(subject) + ".patch";
    std::string dir = out_dir.empty() ? c.cwd : c.abs_path(out_dir);
    (void)c.host().make_dir(dir);
    std::string text = m.str();
    auto r = c.host().write_file(join(dir, file), aios::ByteSpan((const uint8_t*)text.data(), text.size()));
    if (!r.ok()) die(c, "could not open '" + join(dir, file) + "' for writing: " + r.error().message);
    c.out << (out_dir.empty() ? file : join(out_dir, file)) << "\n";
  }
  return 0;
}

// ---------------------------------------------------------------------------
// submodule status
// ---------------------------------------------------------------------------
int cmd_submodule(Ctx& c) {
  std::vector<std::string> args(c.args.begin() + 1, c.args.end());
  std::string sub = args.empty() ? "status" : args[0];
  if (sub != "status") unsupported(c, "submodule " + sub);
  for (size_t i = 1; i < args.size(); ++i)
    if (args[i] != "--" && !args[i].empty() && args[i][0] == '-') unsupported(c, "submodule status " + args[i]);
  c.open_repo();
  auto& h = c.host();
  std::map<std::string, std::string> links;   // path -> recorded commit
  for (auto& e : c.repo->index()) if (e.mode == 0160000) links[e.path] = e.oid;
  std::set<std::string> unmerged;
  for (auto& u : c.repo->unmerged()) if (u.mode == 0160000) unmerged.insert(u.path);
  for (auto& u : unmerged) links.emplace(u, std::string(40, '0'));
  for (auto& [path, oid] : links) {
    if (unmerged.count(path)) { c.out << "U" << std::string(40, '0') << " " << path << "\n"; continue; }
    // the submodule's own HEAD, when it is checked out: ".git" a directory or a "gitdir: <path>" file
    std::string dotgit = join(c.top, path) + "/.git", gitdir;
    auto st = h.stat(dotgit);
    if (st.ok() && st->is_dir) gitdir = dotgit;
    else if (st.ok()) {
      auto f = h.read_file(dotgit);
      std::string t = f.ok() ? std::string(f->begin(), f->end()) : "";
      if (t.rfind("gitdir: ", 0) == 0) {
        gitdir = trimmed(t.substr(8));
        if (!is_absolute(gitdir)) gitdir = normalize_path(join(join(c.top, path), gitdir));
      }
    }
    std::string head, branch;
    if (!gitdir.empty()) {
      auto f = h.read_file(gitdir + "/HEAD");
      std::string t = f.ok() ? trimmed(std::string(f->begin(), f->end())) : "";
      if (t.rfind("ref: ", 0) == 0) {
        branch = t.substr(5);
        auto r = h.read_file(gitdir + "/" + branch);
        if (r.ok()) head = trimmed(std::string(r->begin(), r->end()));
        if (head.empty()) {
          auto packed = h.read_file(gitdir + "/packed-refs");
          if (packed.ok()) {
            std::istringstream ss(std::string(packed->begin(), packed->end()));
            std::string line;
            while (std::getline(ss, line))
              if (line.size() > 41 && line.compare(41, std::string::npos, branch) == 0) { head = line.substr(0, 40); break; }
          }
        }
      } else {
        head = t;
      }
    }
    if (head.empty()) { c.out << "-" << oid << " " << path << "\n"; continue; }
    std::string name = branch.rfind("refs/", 0) == 0 ? branch.substr(5) : c.abbrev(head);
    if (head == oid) c.out << " " << oid << " " << path << " (" << name << ")\n";
    else c.out << "+" << head << " " << path << " (" << name << ")\n";
  }
  return 0;
}

}  // namespace aios_git
