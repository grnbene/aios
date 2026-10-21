// The ref-filter machinery of for-each-ref, branch and tag: %(atom) formats, --sort keys and --contains/--merged/--points-at.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <sstream>

#include "aios/hal/host.hpp"
#include "cli.hpp"
#include "color.hpp"

namespace aios_git {

using namespace aios;
using namespace aios::userland;

namespace {

struct Person {
  std::string name, email;     // email with its angle brackets
  int64_t time = 0;
  std::string tz = "+0000";
  bool present = false;
};

Person person_of(const std::string& line, uint64_t time, const std::string& tz) {
  Person p;
  p.present = !line.empty();
  size_t lt = line.find(" <");
  if (lt == std::string::npos) { p.name = line; return p; }
  p.name = line.substr(0, lt);
  p.email = line.substr(lt + 1);
  p.time = (int64_t)time;
  p.tz = tz;
  return p;
}

struct Obj {
  std::string oid, type;
  size_t size = 0;
  int level = 0;                         // 0: the id only, 1: type and size (from the headers), 2: everything
  std::string peeled;                    // for a tag: the object at the end of the chain
  std::string tagged;                    // for a tag: the object it names directly
  Person author, committer, tagger;
  std::string tree;
  std::vector<std::string> parents;
  std::string message;                   // exactly as stored, after the headers
};

struct MessageParts {
  std::string subject, body, contents, signature, lines_source;
};

size_t signature_start(const std::string& m, size_t from) {
  size_t at = from;
  while (at < m.size()) {
    size_t eol = m.find('\n', at);
    std::string line = m.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
    if (line == "-----BEGIN PGP SIGNATURE-----" || line == "-----BEGIN PGP MESSAGE-----" || line == "-----BEGIN SSH SIGNATURE-----" ||
        line == "-----BEGIN SIGNED MESSAGE-----")
      return at;
    if (eol == std::string::npos) break;
    at = eol + 1;
  }
  return m.size();
}

MessageParts split_message(const std::string& m) {
  MessageParts out;
  out.contents = m;
  size_t pos = 0;
  while (pos < m.size() && m[pos] == '\n') ++pos;
  size_t sig = signature_start(m, pos);
  out.signature = m.substr(sig);
  size_t q = pos;
  while (q < sig) {
    size_t eol = m.find('\n', q);
    if (eol == std::string::npos || eol > sig) { q = sig; break; }
    if (eol == q) break;
    q = eol + 1;
  }
  out.subject = subject_of(m.substr(pos, q - pos));
  size_t body = q;
  while (body < sig && m[body] == '\n') ++body;
  out.body = m.substr(body, sig - body);
  out.lines_source = m.substr(pos, sig - pos);
  return out;
}

// git's versioncmp: digit runs compare as numbers, the rest byte by byte.
int version_compare(const std::string& a, const std::string& b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (std::isdigit((unsigned char)a[i]) && std::isdigit((unsigned char)b[j])) {
      size_t ie = i, je = j;
      while (ie < a.size() && std::isdigit((unsigned char)a[ie])) ++ie;
      while (je < b.size() && std::isdigit((unsigned char)b[je])) ++je;
      size_t is = i, js = j;
      while (is + 1 < ie && a[is] == '0') ++is;
      while (js + 1 < je && b[js] == '0') ++js;
      if (ie - is != je - js) return (ie - is) < (je - js) ? -1 : 1;
      int r = a.compare(is, ie - is, b, js, je - js);
      if (r) return r < 0 ? -1 : 1;
      i = ie;
      j = je;
      continue;
    }
    if (a[i] != b[j]) return (unsigned char)a[i] < (unsigned char)b[j] ? -1 : 1;
    ++i;
    ++j;
  }
  if (i < a.size()) return 1;
  if (j < b.size()) return -1;
  return 0;
}

bool ref_exists_name(Ctx& c, const std::string& name) {
  if (name.rfind("refs/", 0) == 0) return read_ref(c, name).has_value();
  return name == "HEAD" || name == "FETCH_HEAD" || name == "ORIG_HEAD" || name == "MERGE_HEAD" ? read_ref(c, name).has_value() : false;
}

std::string head_text(Ctx& c) {
  auto r = c.host().read_file(c.repo->git_dir() + "/HEAD");
  if (!r.ok()) return "";
  std::string s((const char*)r->data(), r->size());
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

std::string trim_ws(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) ++a;
  while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
  return s.substr(a, b - a);
}

std::string quote_value(const std::string& v, RefQuote q) {
  std::string out;
  switch (q) {
    case RefQuote::None: return v;
    case RefQuote::Shell:
      out = "'";
      for (char ch : v) { if (ch == '\'') out += "'\\''"; else out += ch; }
      return out + "'";
    case RefQuote::Perl:
      out = "'";
      for (char ch : v) { if (ch == '\'' || ch == '\\') out += '\\'; out += ch; }
      return out + "'";
    case RefQuote::Python:
      out = "'";
      for (char ch : v) { if (ch == '\'' || ch == '\\') out += '\\'; if (ch == '\n') out += "\\n"; else out += ch; }
      return out + "'";
    case RefQuote::Tcl:
      out = "\"";
      for (char ch : v) {
        if (ch == '"' || ch == '\\' || ch == '[' || ch == ']' || ch == '$') out += '\\';
        if (ch == '\n') out += "\\n"; else out += ch;
      }
      return out + "\"";
  }
  return v;
}

int display_width(const std::string& s) {
  int w = 0;
  for (unsigned char ch : s) if ((ch & 0xC0) != 0x80) ++w;
  return w;
}

// strftime for the specifiers a --date=format:... normally holds
std::string strftime_text(Ctx& c, int64_t t, const std::string& tz, const std::string& fmt) {
  std::string iso = format_date(t, tz, "iso");     // YYYY-MM-DD HH:MM:SS +ZZZZ
  std::string y = iso.substr(0, 4), mo = iso.substr(5, 2), d = iso.substr(8, 2), h = iso.substr(11, 2), mi = iso.substr(14, 2),
              s = iso.substr(17, 2);
  std::string def = format_date(t, tz, "default");   // Www Mmm D HH:MM:SS YYYY +ZZZZ
  std::string wd = def.substr(0, 3), mon = def.substr(4, 3);
  static const char* kLongDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  static const char* kShortDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char* kLongMonths[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
  static const char* kShortMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int wday = 0, month = std::atoi(mo.c_str()) - 1;
  for (int i = 0; i < 7; ++i) if (wd == kShortDays[i]) wday = i;
  std::string out;
  for (size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%' || i + 1 >= fmt.size()) { out += fmt[i]; continue; }
    char k = fmt[++i];
    switch (k) {
      case 'Y': out += y; break;
      case 'y': out += y.substr(2); break;
      case 'm': out += mo; break;
      case 'd': out += d; break;
      case 'e': out += d[0] == '0' ? " " + d.substr(1) : d; break;
      case 'H': out += h; break;
      case 'M': out += mi; break;
      case 'S': out += s; break;
      case 'I': { int hh = std::atoi(h.c_str()) % 12; char b[4]; std::snprintf(b, sizeof b, "%02d", hh ? hh : 12); out += b; break; }
      case 'p': out += std::atoi(h.c_str()) >= 12 ? "PM" : "AM"; break;
      case 'a': out += kShortDays[wday]; break;
      case 'A': out += kLongDays[wday]; break;
      case 'b': case 'h': out += kShortMonths[month]; break;
      case 'B': out += kLongMonths[month]; break;
      case 'z': out += tz; break;
      case 's': out += std::to_string(t); break;
      case 'F': out += y + "-" + mo + "-" + d; break;
      case 'T': out += h + ":" + mi + ":" + s; break;
      case 'R': out += h + ":" + mi; break;
      case 'D': out += mo + "/" + d + "/" + y.substr(2); break;
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case '%': out += '%'; break;
      default: unsupported(c, std::string("the date format specifier %") + k);
    }
  }
  return out;
}

std::string date_text(Ctx& c, const Person& p, const std::string& mode) {
  if (!p.present) return "";
  if (mode.rfind("format:", 0) == 0) return strftime_text(c, p.time, p.tz, mode.substr(7));
  if (mode.empty() || mode == "default") return format_date(p.time, p.tz, "default");
  static const std::set<std::string> known = {"iso", "iso8601", "iso-strict", "iso8601-strict", "short", "unix", "raw", "rfc", "rfc2822", "relative",
                                              "default-local", "iso-local", "iso-strict-local", "short-local", "rfc-local", "relative-local",
                                              "unix-local", "raw-local", "iso8601-local", "iso8601-strict-local", "rfc2822-local"};
  if (!known.count(mode)) unsupported(c, "the date format " + mode);
  std::string text = format_date(p.time, p.tz, mode);
  // a local default date leaves the zone off
  if (mode == "default-local" && text.size() > 6) text.resize(text.size() - 6);
  return text;
}

struct Node {
  enum Kind { Literal, Atom, If, Then, Else, End, Align, Color } kind = Literal;
  std::string text;                      // literal text, or the atom's name
  std::vector<std::string> mods;         // atom modifiers
  bool deref = false;                    // *atom
  std::string arg;                       // %(if:equals=...) / %(align:...) argument text
};

bool is_date_atom(const std::string& n) {
  return n == "authordate" || n == "committerdate" || n == "creatordate" || n == "taggerdate";
}

}  // namespace

struct RefFormat::Impl {
  Ctx& c;
  RefQuote quote;
  std::vector<Node> nodes;
  std::map<std::string, Obj> objects;
  Obj scratch;                                       // load()'s answer when only the id is wanted
  bool reset_at_eol = false;                         // the last %(color:...) is not reset
  std::map<std::string, std::string> worktrees;      // ref -> path where it is checked out
  bool worktrees_read = false;
  std::map<std::string, std::string> shorts;
  std::unique_ptr<std::unordered_set<std::string>> ref_names;

  Impl(Ctx& ctx, RefQuote q) : c(ctx), quote(q) {}

  // What a format needs read of the object a ref names: nothing for the ref's own atoms and its id, the header for
  // its type and size, the whole object otherwise. `git tag -l` read and parsed all 1,000 of redis' tag objects to
  // print their names.
  static int atom_level(const Node& n) {
    if (n.kind != Node::Atom) return 0;
    const std::string& a = n.text;
    if (a == "refname" || a == "HEAD" || a == "upstream" || a == "push" || a == "symref" || a == "worktreepath") return 0;
    if (n.deref) return 2;
    if (a == "objectname") return 0;
    if (a == "objecttype" || a == "objectsize") return 1;
    return 2;
  }
  int need = -1;
  int needed_level() {
    if (need < 0) {
      need = 0;
      for (const Node& n : nodes) need = std::max(need, atom_level(n));
    }
    return need;
  }

  const Obj& load(const std::string& oid, int level = 2) {
    auto it = objects.find(oid);
    if (it != objects.end() && it->second.level >= level) return it->second;
    // Nothing to read: the id alone, without a cached entry per ref (a deref inside value() loads at level 2).
    if (level == 0 && it == objects.end()) { scratch.oid = oid; return scratch; }
    if (it == objects.end()) it = objects.try_emplace(oid).first;
    if (level == 0) {
      it->second.oid = oid;
      return it->second;
    }
    if (level == 1) {
      Obj& o = it->second;
      o.oid = oid;
      o.level = 1;
      if (auto info = c.repo->object_info(oid); info.ok()) { o.type = info->first; o.size = (size_t)info->second; }
      return o;
    }
    Obj& o = it->second;
    o = Obj{};
    o.oid = oid;
    o.level = 2;
    auto r = c.repo->read_object(oid);
    if (!r.ok()) { o.type = ""; return o; }
    o.type = r->type;
    o.size = r->payload.size();
    std::string text((const char*)r->payload.data(), r->payload.size());
    if (o.type == "commit") {
      GitCommit cm = read_commit(c, oid);
      o.tree = cm.tree;
      o.parents = cm.parents;
      o.author = person_of(cm.author, cm.author_time, cm.author_tz);
      o.committer = person_of(cm.committer, cm.committer_time, cm.committer_tz);
      o.message = cm.raw_message;
    } else if (o.type == "tag") {
      size_t split = text.find("\n\n");
      std::string head = text.substr(0, split == std::string::npos ? text.size() : split);
      o.message = split == std::string::npos ? "" : text.substr(split + 2);
      std::istringstream hs(head);
      std::string line;
      while (std::getline(hs, line)) {
        if (line.rfind("object ", 0) == 0) o.tagged = line.substr(7);
        else if (line.rfind("tagger ", 0) == 0) {
          std::string rest = line.substr(7);
          size_t gt = rest.rfind('>');
          std::string who = gt == std::string::npos ? rest : rest.substr(0, gt + 1);
          uint64_t t = 0;
          std::string tz = "+0000";
          if (gt != std::string::npos) {
            std::istringstream ts(rest.substr(gt + 1));
            ts >> t >> tz;
          }
          o.tagger = person_of(who, t, tz);
        }
      }
      std::string cur = o.tagged;
      for (int i = 0; i < 32 && !cur.empty(); ++i) {
        const Obj& x = load(cur);
        if (x.type != "tag" || x.tagged.empty()) { o.peeled = cur; break; }
        cur = x.tagged;
      }
      if (o.peeled.empty()) o.peeled = cur;
    } else {
      o.message = "";
    }
    return o;
  }

  void read_worktrees() {
    if (worktrees_read) return;
    worktrees_read = true;
    auto& h = c.host();
    auto head_of = [&](const std::string& headfile, const std::string& path) {
      auto r = h.read_file(headfile);
      if (!r.ok()) return;
      std::string s((const char*)r->data(), r->size());
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
      if (s.rfind("ref: ", 0) == 0) worktrees.emplace(s.substr(5), path);
    };
    std::string common = c.repo->common_dir();
    if (!c.repo->bare()) {
      std::string main = common.size() > 5 && common.compare(common.size() - 5, 5, "/.git") == 0 ? common.substr(0, common.size() - 5) : dirname_of(common);
      head_of(common + "/HEAD", main);
    }
    auto names = h.list_dir(common + "/worktrees");
    if (!names.ok()) return;
    for (auto& n : *names) {
      if (n == "." || n == "..") continue;
      auto g = h.read_file(common + "/worktrees/" + n + "/gitdir");
      if (!g.ok()) continue;
      std::string gd((const char*)g->data(), g->size());
      while (!gd.empty() && (gd.back() == '\n' || gd.back() == '\r')) gd.pop_back();
      head_of(common + "/worktrees/" + n + "/HEAD", dirname_of(gd));
    }
  }

  std::string short_ref(const std::string& ref) {
    auto it = shorts.find(ref);
    if (it != shorts.end()) return it->second;
    if (!ref_names) {
      // Every ref, once: the clash probes test names against it rather than opening five loose paths per ref.
      ref_names = std::make_unique<std::unordered_set<std::string>>();
      for (auto& [r, o] : all_refs(c)) ref_names->insert(r);
    }
    return shorts[ref] = unambiguous_ref_name(c, ref, ref_names.get());
  }

  void parse(const std::string& fmt) {
    std::string lit;
    auto flush = [&] {
      if (lit.empty()) return;
      Node n;
      n.text = lit;
      nodes.push_back(n);
      lit.clear();
    };
    for (size_t i = 0; i < fmt.size(); ++i) {
      if (fmt[i] == '%' && i + 1 < fmt.size() && fmt[i + 1] == '%') { lit += '%'; ++i; continue; }
      if (fmt[i] == '%' && i + 2 < fmt.size() && std::isxdigit((unsigned char)fmt[i + 1]) && std::isxdigit((unsigned char)fmt[i + 2])) {
        lit += (char)std::stoi(fmt.substr(i + 1, 2), nullptr, 16);
        i += 2;
        continue;
      }
      if (fmt[i] == '%' && i + 1 < fmt.size() && fmt[i + 1] == '(') {
        size_t e = fmt.find(')', i + 2);
        if (e == std::string::npos) die(c, "malformed format string " + fmt.substr(i));
        flush();
        std::string body = fmt.substr(i + 2, e - i - 2);
        i = e;
        Node n;
        n.kind = Node::Atom;
        if (!body.empty() && body[0] == '*') { n.deref = true; body = body.substr(1); }
        size_t colon = body.find(':');
        n.text = body.substr(0, colon);
        std::string args = colon == std::string::npos ? "" : body.substr(colon + 1);
        if (n.text == "if") { n.kind = Node::If; n.arg = args; }
        else if (n.text == "then") n.kind = Node::Then;
        else if (n.text == "else") n.kind = Node::Else;
        else if (n.text == "end") n.kind = Node::End;
        else if (n.text == "align") { n.kind = Node::Align; n.arg = args; }
        else if (n.text == "color") {
          // ref-filter's color_atom_parser: the color checked even when colors are off
          n.kind = Node::Color;
          if (colon == std::string::npos) die(c, "expected format: %(color:<color>)");
          auto col = parse_git_color(args);
          if (!col) {
            c.err << "error: invalid color value: " << args << "\n";
            die(c, "unrecognized color: %(color:" + args + ")");
          }
          n.arg = c.color ? *col : "";
          reset_at_eol = args != "reset";
        }
        else if (colon != std::string::npos) {
          if (is_date_atom(n.text) && args.rfind("format:", 0) == 0) n.mods.push_back(args);
          else {
            std::istringstream as(args);
            std::string m;
            while (std::getline(as, m, ',')) n.mods.push_back(m);
          }
        }
        if (n.kind == Node::Atom) validate(n);
        nodes.push_back(n);
        continue;
      }
      lit += fmt[i];
    }
    flush();
  }

  void validate(const Node& n) {
    static const std::set<std::string> plain = {
        "refname", "objecttype", "objectsize", "objectname", "tree", "parent", "numparent", "author", "authorname", "authoremail",
        "authordate", "committer", "committername", "committeremail", "committerdate", "tag", "tagger", "taggername", "taggeremail",
        "taggerdate", "creator", "creatordate", "subject", "body", "contents", "upstream", "push", "HEAD", "symref", "worktreepath"};
    static const std::set<std::string> unsupported_atoms = {"trailers", "describe", "ahead-behind", "is-base", "rest", "deltabase", "signature"};
    if (unsupported_atoms.count(n.text)) unsupported(c, "the --format atom %(" + n.text + ")");
    if (!plain.count(n.text)) die(c, "unknown field name: " + n.text);
    if (n.deref && (n.text == "HEAD" || n.text == "upstream" || n.text == "push" || n.text == "symref" || n.text == "worktreepath" || n.text == "refname"))
      die(c, "unknown field name: *" + n.text);
  }

  std::string refname_value(const RefRow& row, const std::vector<std::string>& mods) {
    if (row.detached) return head_description(c);
    std::string name = row.ref;
    if (mods.empty()) return name;
    if (mods.size() > 1) die(c, "unrecognized %(refname) argument: " + mods[1]);
    const std::string& m = mods[0];
    if (m == "short") return short_ref(name);
    auto num = [&](const std::string& v) -> long {
      char* end = nullptr;
      long n = std::strtol(v.c_str(), &end, 10);
      if (!end || *end || v.empty()) die(c, "positive value expected refname:" + m);
      return n;
    };
    auto parts = [&](const std::string& s) {
      std::vector<std::string> v;
      size_t at = 0;
      for (;;) {
        size_t sl = s.find('/', at);
        if (sl == std::string::npos) { v.push_back(s.substr(at)); break; }
        v.push_back(s.substr(at, sl - at));
        at = sl + 1;
      }
      return v;
    };
    auto join_parts = [&](const std::vector<std::string>& v, size_t a, size_t b) {
      std::string out;
      for (size_t i = a; i < b; ++i) { if (i > a) out += '/'; out += v[i]; }
      return out;
    };
    if (m == "dir") { size_t sl = name.rfind('/'); return sl == std::string::npos ? "" : name.substr(0, sl); }
    if (m == "base") { size_t sl = name.rfind('/'); return sl == std::string::npos ? name : name.substr(sl + 1); }
    if (m.rfind("lstrip=", 0) == 0 || m.rfind("strip=", 0) == 0) {
      long n = num(m.substr(m.find('=') + 1));
      if (n >= 0) {   // what follows the n-th '/', with no split and join
        size_t at = 0;
        for (long i = 0; i < n; ++i) {
          size_t sl = name.find('/', at);
          if (sl == std::string::npos) return "";
          at = sl + 1;
        }
        return name.substr(at);
      }
    }
    auto v = parts(name);
    if (m.rfind("lstrip=", 0) == 0 || m.rfind("strip=", 0) == 0) {
      long n = num(m.substr(m.find('=') + 1));
      if (n < 0) n = std::max<long>(0, (long)v.size() + n);
      if ((size_t)n >= v.size()) return "";
      return join_parts(v, (size_t)n, v.size());
    }
    if (m.rfind("rstrip=", 0) == 0) {
      long n = num(m.substr(7));
      if (n < 0) n = std::max<long>(0, (long)v.size() + n);
      if ((size_t)n >= v.size()) return "";
      return join_parts(v, 0, v.size() - (size_t)n);
    }
    die(c, "unrecognized %(refname) argument: " + m);
  }

  struct Tracking { std::string ref; bool configured = false, exists = false; long ahead = 0, behind = 0; std::string remote, merge; };

  Tracking tracking_of(const std::string& branch, bool push) {
    Tracking t;
    auto remote = c.config.get("branch." + branch + ".remote");
    auto merge = c.config.get("branch." + branch + ".merge");
    if (!push) {
      if (!remote || !merge) return t;
      t.configured = true;
      t.ref = upstream_of(c, branch);
      t.remote = *remote;
      t.merge = *merge;
    } else {
      std::string rem = c.config.get("branch." + branch + ".pushremote").value_or(c.config.get("remote.pushdefault").value_or(remote ? *remote : ""));
      if (rem.empty()) return t;
      std::string mode = c.config.get("push.default").value_or("simple");
      t.configured = true;
      t.remote = rem;
      if (mode == "nothing") return t;
      std::string dest = branch;
      if (mode == "upstream") {
        if (!remote || !merge) return t;
        dest = *merge;
        if (dest.rfind("refs/heads/", 0) == 0) dest = dest.substr(11);
      } else if (mode == "simple" && remote && merge && rem == *remote) {
        std::string m = *merge;
        if (m.rfind("refs/heads/", 0) == 0) m = m.substr(11);
        if (m != branch) return t;
      }
      t.merge = "refs/heads/" + dest;
      t.ref = "";
      for (auto& spec : c.config.get_all("remote." + rem + ".fetch")) {
        std::string s = !spec.empty() && spec[0] == '+' ? spec.substr(1) : spec;
        size_t colon = s.find(':');
        if (colon == std::string::npos) continue;
        std::string src = s.substr(0, colon), dst = s.substr(colon + 1);
        size_t star = src.find('*');
        std::string full = t.merge;
        if (star == std::string::npos) { if (src == full) { t.ref = dst; break; } continue; }
        std::string pre = src.substr(0, star), post = src.substr(star + 1);
        if (full.rfind(pre, 0) == 0 && full.size() >= pre.size() + post.size() &&
            full.compare(full.size() - post.size(), post.size(), post) == 0) {
          std::string mid = full.substr(pre.size(), full.size() - pre.size() - post.size());
          size_t dstar = dst.find('*');
          t.ref = dstar == std::string::npos ? dst : dst.substr(0, dstar) + mid + dst.substr(dstar + 1);
          break;
        }
      }
      if (t.ref.empty() && rem == ".") t.ref = t.merge;
      if (t.ref.empty()) return t;
    }
    if (t.ref.empty()) return t;
    auto theirs = read_ref(c, t.ref);
    auto mine = read_ref(c, "refs/heads/" + branch);
    t.exists = theirs.has_value();
    if (theirs && mine) {
      WalkOptions a, b;
      a.tips = {*mine}; a.hide = {*theirs};
      b.tips = {*theirs}; b.hide = {*mine};
      t.ahead = (long)walk(c, a).size();
      t.behind = (long)walk(c, b).size();
    }
    return t;
  }

  std::string remote_atom(const RefRow& row, const Node& n, bool push) {
    if (row.ref.rfind("refs/heads/", 0) != 0) return "";
    std::string branch = row.ref.substr(11);
    Tracking t = tracking_of(branch, push);
    if (!t.configured) return "";
    bool nobracket = false;
    std::string opt;
    for (auto& m : n.mods) {
      if (m == "nobracket") nobracket = true;
      else if (opt.empty()) opt = m;
      else die(c, "unrecognized %(" + n.text + ") argument: " + m);
    }
    if (opt == "remotename") return t.remote;
    if (opt == "remoteref") return t.merge;
    if (t.ref.empty()) return "";
    if (opt.empty()) return t.ref;
    if (opt == "short") return short_ref(t.ref);
    if (opt == "track") {
      if (!t.exists) return nobracket ? "gone" : "[gone]";
      std::string s;
      if (t.ahead && t.behind) s = "ahead " + std::to_string(t.ahead) + ", behind " + std::to_string(t.behind);
      else if (t.ahead) s = "ahead " + std::to_string(t.ahead);
      else if (t.behind) s = "behind " + std::to_string(t.behind);
      else return "";
      return nobracket ? s : "[" + s + "]";
    }
    if (opt == "trackshort") {
      if (!t.exists) return "";
      if (!t.ahead && !t.behind) return "=";
      if (!t.ahead) return "<";
      if (!t.behind) return ">";
      return "<>";
    }
    die(c, "unrecognized %(" + n.text + ") argument: " + opt);
  }

  std::string person_atom(const Person& p, const std::string& part, const std::vector<std::string>& mods) {
    if (!p.present) return "";
    if (part == "name") return p.name;
    if (part == "email") {
      std::string e = p.email;
      for (auto& m : mods) {
        if (m == "trim") { if (e.size() >= 2) e = e.substr(1, e.size() - 2); }
        else if (m == "localpart") { if (e.size() >= 2) e = e.substr(1, e.size() - 2); size_t at = e.find('@'); if (at != std::string::npos) e = e.substr(0, at); }
        else die(c, "unrecognized email option: " + m);
      }
      return e;
    }
    if (part == "date") return date_text(c, p, mods.empty() ? "" : mods[0]);
    return p.name + " " + p.email + " " + std::to_string(p.time) + " " + p.tz;
  }

  const Person& who(const Obj& o, const std::string& kind) {
    static const Person none;
    if (kind == "author") return o.type == "commit" ? o.author : none;
    if (kind == "committer") return o.type == "commit" ? o.committer : none;
    if (kind == "tagger") return o.type == "tag" ? o.tagger : none;
    return o.type == "commit" ? o.committer : o.type == "tag" ? o.tagger : none;
  }

  std::string value(const Node& n, const RefRow& row, const Obj& direct) {
    const std::string& a = n.text;
    if (a == "refname") return refname_value(row, n.mods);
    if (a == "HEAD") return row.detached || row.ref == c.repo->head_ref() ? "*" : " ";
    if (a == "upstream") return remote_atom(row, n, false);
    if (a == "push") return remote_atom(row, n, true);
    if (a == "symref") {
      auto t = c.host().read_file(c.repo->common_dir() + "/" + row.ref);
      if (!t.ok()) return "";
      std::string s((const char*)t->data(), t->size());
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
      if (s.rfind("ref: ", 0) != 0) return "";
      std::string target = s.substr(5);
      if (n.mods.empty()) return target;
      if (n.mods[0] == "short") return short_ref(target);
      Node r = n;
      r.text = "refname";
      RefRow tr{target, ""};
      return refname_value(tr, n.mods);
    }
    if (a == "worktreepath") {
      read_worktrees();
      auto it = worktrees.find(row.ref);
      return it == worktrees.end() ? "" : it->second;
    }
    const Obj* o = &direct;
    if (n.deref) {
      if (direct.type != "tag") return "";
      o = &load(direct.tagged);
    }
    if (a == "objecttype") return o->type;
    if (a == "objectsize") {
      if (!n.mods.empty()) unsupported(c, "the --format atom %(objectsize:" + n.mods[0] + ")");
      return std::to_string(o->size);
    }
    if (a == "objectname" || a == "tree" || a == "parent") {
      std::string id = a == "objectname" ? o->oid : a == "tree" ? o->tree : "";
      if (a == "parent") {
        for (size_t i = 0; i < o->parents.size(); ++i) {
          if (i) id += ' ';
          id += o->parents[i];
        }
        if (n.mods.empty()) return id;
      }
      if (n.mods.empty() || id.empty()) return id;
      const std::string& m = n.mods[0];
      if (m == "short") return c.abbrev(id);
      if (m.rfind("short=", 0) == 0) {
        int len = std::atoi(m.c_str() + 6);
        return c.abbrev(id, std::max(len, 4));
      }
      die(c, "unrecognized %(" + a + ") argument: " + m);
    }
    if (a == "numparent") return o->type == "commit" ? std::to_string(o->parents.size()) : "";
    for (const char* kind : {"author", "committer", "tagger", "creator"}) {
      std::string k = kind;
      if (a == k) return person_atom(who(*o, k), "", n.mods);
      if (a == k + "name") return person_atom(who(*o, k), "name", n.mods);
      if (a == k + "email") return person_atom(who(*o, k), "email", n.mods);
      if (a == k + "date") return person_atom(who(*o, k), "date", n.mods);
    }
    if (a == "subject" || a == "body" || a == "contents") {
      if (o->type != "commit" && o->type != "tag") return "";
      MessageParts mp = split_message(o->message);
      if (a == "subject") return mp.subject;
      if (a == "body") return mp.body;
      if (n.mods.empty()) return o->message;
      const std::string& m = n.mods[0];
      if (m == "subject") return mp.subject;
      if (m == "body") return mp.body;
      if (m == "signature") return mp.signature;
      if (m.rfind("lines=", 0) == 0) {
        int lines = std::atoi(m.c_str() + 6);
        std::string out;
        const std::string& src = mp.lines_source;
        size_t sp = 0;
        for (int i = 0; i < lines && sp < src.size(); ++i) {
          if (i) out += "\n    ";
          size_t eol = src.find('\n', sp);
          out += src.substr(sp, eol == std::string::npos ? std::string::npos : eol - sp);
          if (eol == std::string::npos) break;
          sp = eol + 1;
        }
        return out;
      }
      if (m == "trailers" || m.rfind("trailers", 0) == 0) unsupported(c, "the --format atom %(contents:trailers)");
      die(c, "unrecognized %(contents) argument: " + m);
    }
    if (a == "tag") return o->type == "tag" ? tag_name(*o) : "";
    return "";
  }

  std::string tag_name(const Obj& o) {
    auto r = c.repo->read_object(o.oid);
    if (!r.ok()) return "";
    std::string text((const char*)r->payload.data(), r->payload.size());
    size_t at = text.find("\ntag ");
    if (text.rfind("tag ", 0) == 0) at = 0; else if (at != std::string::npos) at += 1; else return "";
    size_t eol = text.find('\n', at);
    return text.substr(at + 4, eol == std::string::npos ? std::string::npos : eol - at - 4);
  }

  struct Frame {
    enum Kind { Root, If, Then, Else, Align } kind = Root;
    std::string out, then_out;
    std::string cond;            // for If: the trimmed text between %(if) and %(then)
    bool condition = false;
    std::string arg;
  };

  std::string run(const RefRow& row) {
    const Obj& direct = load(row.oid, needed_level());
    std::vector<Frame> stack(1);
    for (const Node& n : nodes) {
      Frame& top = stack.back();
      switch (n.kind) {
        case Node::Literal: top.out += n.text; break;
        case Node::Atom: top.out += quote_value(value(n, row, direct), quote); break;
        case Node::Color: top.out += n.arg; break;
        case Node::If: { Frame f; f.kind = Frame::If; f.arg = n.arg; stack.push_back(f); break; }
        case Node::Align: { Frame f; f.kind = Frame::Align; f.arg = n.arg; stack.push_back(f); break; }
        case Node::Then: {
          if (top.kind != Frame::If) die(c, "format: %(then) atom used without an %(if) atom");
          std::string cond = trim_ws(top.out);
          top.cond = cond;
          const std::string& arg = top.arg;
          if (arg.empty()) top.condition = !cond.empty();
          else if (arg.rfind("equals=", 0) == 0) top.condition = cond == arg.substr(7);
          else if (arg.rfind("notequals=", 0) == 0) top.condition = cond != arg.substr(10);
          else die(c, "unrecognized %(if) argument: " + arg);
          top.kind = Frame::Then;
          top.out.clear();
          break;
        }
        case Node::Else: {
          if (top.kind != Frame::Then) die(c, "format: %(else) atom used without a %(then) atom");
          top.kind = Frame::Else;
          top.then_out = top.out;
          top.out.clear();
          break;
        }
        case Node::End: {
          if (stack.size() < 2) die(c, "format: %(end) atom used without corresponding atom");
          Frame f = stack.back();
          stack.pop_back();
          std::string res;
          if (f.kind == Frame::Then) res = f.condition ? f.out : "";
          else if (f.kind == Frame::Else) res = f.condition ? f.then_out : f.out;
          else if (f.kind == Frame::Align) res = aligned(f);
          else die(c, "format: %(end) atom used without corresponding atom");
          stack.back().out += res;
          break;
        }
      }
    }
    if (stack.size() != 1) die(c, "format: %(end) atom missing");
    // ref-filter: a format whose last %(color:...) is not reset gets one at the end of the line
    if (reset_at_eol && c.color) stack[0].out += kColorReset;
    return stack[0].out;
  }

  std::string aligned(const Frame& f) {
    long width = 0;
    std::string pos = "left";
    std::istringstream as(f.arg);
    std::string part;
    while (std::getline(as, part, ',')) {
      if (part.rfind("width=", 0) == 0) width = std::atol(part.c_str() + 6);
      else if (part.rfind("position=", 0) == 0) pos = part.substr(9);
      else if (part == "left" || part == "right" || part == "middle") pos = part;
      else if (!part.empty() && std::isdigit((unsigned char)part[0])) width = std::atol(part.c_str());
      else die(c, "unrecognized %(align) argument: " + part);
    }
    int len = display_width(f.out);
    if (width <= len) return f.out;
    size_t pad = (size_t)(width - len);
    if (pos == "right") return std::string(pad, ' ') + f.out;
    if (pos == "middle") return std::string(pad / 2, ' ') + f.out + std::string(pad - pad / 2, ' ');
    return f.out + std::string(pad, ' ');
  }
};

RefFormat::RefFormat(Ctx& c, const std::string& format, RefQuote quote) : impl_(std::make_shared<Impl>(c, quote)) { impl_->parse(format); }
std::string RefFormat::expand(const RefRow& row) { return impl_->run(row); }

// git's shorten_unambiguous_ref: the shortest tail of the name that no earlier lookup rule would read as another ref.
std::string unambiguous_ref_name(Ctx& c, const std::string& ref, const std::unordered_set<std::string>* known) {
  static const std::vector<std::pair<std::string, std::string>> rules = {
      {"", ""}, {"refs/", ""}, {"refs/tags/", ""}, {"refs/heads/", ""}, {"refs/remotes/", ""}, {"refs/remotes/", "/HEAD"}};
  // refs/remotes/<name>/HEAD shortens to <name> (git 2.43); every rule is tried for a clash (warn_ambiguous_refs)
  for (size_t i = rules.size(); i-- > 1;) {
    const auto& [pre, post] = rules[i];
    if (ref.size() < pre.size() + post.size() || ref.rfind(pre, 0) != 0) continue;
    if (!post.empty() && ref.compare(ref.size() - post.size(), post.size(), post) != 0) continue;
    std::string shortened = ref.substr(pre.size(), ref.size() - pre.size() - post.size());
    bool ambiguous = false;
    for (size_t j = 0; j < rules.size() && !ambiguous; ++j) {
      if (j == i) continue;
      std::string probe = rules[j].first + shortened + rules[j].second;
      if (known && probe.rfind("refs/", 0) == 0 ? known->count(probe) > 0 : ref_exists_name(c, probe)) ambiguous = true;
    }
    if (!ambiguous) return shortened;
  }
  return ref;
}

// "(HEAD detached at abc1234)": what `branch` calls the detached HEAD.
std::string head_description(Ctx& c) {
  std::string git = c.repo->git_dir();
  auto& h = c.host();
  auto read_line = [&](const std::string& path) {
    auto r = h.read_file(path);
    if (!r.ok()) return std::string();
    std::string s((const char*)r->data(), r->size());
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
  };
  auto branch_name = [](std::string n) { return n.rfind("refs/heads/", 0) == 0 ? n.substr(11) : n; };
  for (const char* dir : {"/rebase-merge", "/rebase-apply"}) {
    if (!h.exists(git + dir)) continue;
    if (!h.exists(git + dir + "/head-name") && std::string(dir) == "/rebase-apply") continue;
    std::string onto = branch_name(read_line(git + dir + "/head-name"));
    if (!onto.empty()) return "(no branch, rebasing " + onto + ")";
  }
  auto head = read_ref(c, "HEAD");
  auto lr = h.read_file(git + "/logs/HEAD");
  std::string from, noid;
  if (lr.ok()) {
    std::string text((const char*)lr->data(), lr->size());
    size_t end = text.size();
    while (end > 0) {
      size_t start = text.rfind('\n', end - 1);
      std::string line = text.substr(start == std::string::npos ? 0 : start + 1, end - (start == std::string::npos ? 0 : start + 1));
      size_t tab = line.find('\t');
      std::string msg = tab == std::string::npos ? "" : line.substr(tab + 1);
      size_t m = msg.find("checkout: moving from ");
      if (m == 0) {
        size_t to = msg.rfind(" to ");
        if (to != std::string::npos) {
          from = msg.substr(to + 4);
          if (line.size() > 83) noid = line.substr(41, 40);
          break;
        }
      }
      if (start == std::string::npos) break;
      end = start;
    }
  }
  if (!from.empty() && !noid.empty() && head) {
    // a branch, tag or remote-tracking name stays as typed; anything else (HEAD~1, an id) is shown abbreviated
    std::string ref = from == "HEAD" ? "" : dwim_ref(c, from);
    std::string shown = !ref.empty() ? unambiguous_ref_name(c, ref) : c.abbrev(noid);
    return std::string(noid == *head ? "(HEAD detached at " : "(HEAD detached from ") + shown + ")";
  }
  return "(HEAD detached at " + (head ? c.abbrev(*head) : std::string("HEAD")) + ")";
}

namespace {

// One --sort key: [-][version:]atom[:mods]
struct SortKey {
  bool reverse = false, version = false;
  Node node;
  bool numeric = false;
};

SortKey parse_sort_key(Ctx& c, std::string spec) {
  SortKey k;
  if (!spec.empty() && spec[0] == '-') { k.reverse = true; spec = spec.substr(1); }
  if (spec.rfind("version:", 0) == 0) { k.version = true; spec = spec.substr(8); }
  else if (spec.rfind("v:", 0) == 0) { k.version = true; spec = spec.substr(2); }
  if (!spec.empty() && spec[0] == '*') { k.node.deref = true; spec = spec.substr(1); }
  size_t colon = spec.find(':');
  k.node.kind = Node::Atom;
  k.node.text = spec.substr(0, colon);
  if (colon != std::string::npos) {
    std::string args = spec.substr(colon + 1);
    if (is_date_atom(k.node.text) && args.rfind("format:", 0) == 0) k.node.mods.push_back(args);
    else {
      std::istringstream as(args);
      std::string m;
      while (std::getline(as, m, ',')) k.node.mods.push_back(m);
    }
  }
  k.numeric = is_date_atom(k.node.text) || k.node.text == "objectsize" || k.node.text == "numparent";
  return k;
}

}  // namespace

void sort_refs(Ctx& c, std::vector<RefRow>& rows, const std::vector<std::string>& keys, bool icase) {
  std::vector<SortKey> ks;
  for (auto& k : keys) ks.push_back(parse_sort_key(c, k));
  if (ks.empty()) ks.push_back(parse_sort_key(c, "refname"));
  // the last --sort named is the primary key
  std::reverse(ks.begin(), ks.end());
  RefFormat::Impl probe(c, RefQuote::None);
  for (auto& k : ks) probe.validate(k.node);
  struct Computed { std::vector<std::string> text; std::vector<int64_t> num; };
  // The default order, by name alone: the rows' own refnames, no object read and no copies.
  if (ks.size() == 1 && !icase && !ks[0].numeric && !ks[0].version && ks[0].node.kind == Node::Atom &&
      ks[0].node.text == "refname" && ks[0].node.mods.empty() && !ks[0].node.deref &&
      std::none_of(rows.begin(), rows.end(), [](const RefRow& r) { return r.detached; })) {
    const bool rev = ks[0].reverse;
    std::stable_sort(rows.begin(), rows.end(), [rev](const RefRow& a, const RefRow& b) { return rev ? b.ref < a.ref : a.ref < b.ref; });
    return;
  }
  std::vector<Computed> vals(rows.size());
  int level = 0;
  for (auto& k : ks) level = std::max(level, k.numeric ? 2 : RefFormat::Impl::atom_level(k.node));
  for (size_t r = 0; r < rows.size(); ++r) {
    const Obj& direct = probe.load(rows[r].oid, level);
    for (auto& k : ks) {
      std::string text;
      int64_t num = 0;
      Node n = k.node;
      if (k.numeric) {
        if (n.text == "objectsize" || n.text == "numparent") {
          text = probe.value(n, rows[r], direct);
          num = text.empty() ? 0 : std::atoll(text.c_str());
        } else {
          const Obj* o = &direct;
          if (n.deref) { if (direct.type == "tag") o = &probe.load(direct.tagged); else o = nullptr; }
          if (o) {
            const std::string kind = n.text.substr(0, n.text.size() - 4);
            const Person& p = probe.who(*o, kind);
            num = p.present ? p.time : 0;
          }
        }
      } else {
        text = probe.value(n, rows[r], direct);
        if (icase) std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
      }
      vals[r].text.push_back(text);
      vals[r].num.push_back(num);
    }
  }
  std::vector<size_t> order(rows.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    for (size_t k = 0; k < ks.size(); ++k) {
      int cmp;
      if (ks[k].numeric) cmp = vals[a].num[k] < vals[b].num[k] ? -1 : vals[a].num[k] > vals[b].num[k] ? 1 : 0;
      else if (ks[k].version) cmp = version_compare(vals[a].text[k], vals[b].text[k]);
      else cmp = vals[a].text[k].compare(vals[b].text[k]) < 0 ? -1 : vals[a].text[k] == vals[b].text[k] ? 0 : 1;
      if (cmp) return ks[k].reverse ? cmp > 0 : cmp < 0;
    }
    std::string ra = rows[a].ref, rb = rows[b].ref;
    if (icase) {
      std::transform(ra.begin(), ra.end(), ra.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
      std::transform(rb.begin(), rb.end(), rb.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
    }
    return ra < rb;
  });
  std::vector<RefRow> sorted;
  for (size_t i : order) sorted.push_back(rows[i]);
  rows = std::move(sorted);
}

void select_refs(Ctx& c, std::vector<RefRow>& rows, const RefSelect& sel) {
  if (sel.contains.empty() && sel.no_contains.empty() && sel.merged.empty() && sel.no_merged.empty() && sel.points_at.empty()) return;
  RefFormat::Impl probe(c, RefQuote::None);
  // the way each option reports a revision it cannot read
  auto commit_arg = [&](const std::string& spec, bool fatal) {
    auto oid = resolve(c, spec.empty() ? "HEAD" : spec);
    std::string cm = oid ? peel_to(c, *oid, "commit") : "";
    if (cm.empty()) {
      if (fatal) die(c, "malformed object name " + spec);
      usage(c, "malformed object name " + spec);
    }
    return cm;
  };
  std::vector<std::string> contains, no_contains, merged, no_merged, points;
  for (auto& s : sel.contains) contains.push_back(commit_arg(s, false));
  for (auto& s : sel.no_contains) no_contains.push_back(commit_arg(s, false));
  for (auto& s : sel.merged) merged.push_back(commit_arg(s, true));
  for (auto& s : sel.no_merged) no_merged.push_back(commit_arg(s, true));
  for (auto& s : sel.points_at) {
    auto oid = resolve(c, s);
    if (!oid) usage(c, "malformed object name '" + s + "'");
    points.push_back(*oid);
  }
  std::vector<RefRow> kept;
  for (auto& row : rows) {
    std::string cm = peel_to(c, row.oid, "commit");
    bool ok = true;
    // refs naming anything but a commit never satisfy a reachability test
    for (auto& w : contains) if (ok && (cm.empty() || !is_ancestor(c, w, cm))) ok = false;
    for (auto& w : no_contains) if (ok && (cm.empty() || is_ancestor(c, w, cm))) ok = false;
    if (ok && !merged.empty()) {
      bool any = false;
      for (auto& w : merged) if (!cm.empty() && is_ancestor(c, cm, w)) any = true;
      ok = any;
    }
    if (ok && !no_merged.empty()) {
      bool any = false;
      for (auto& w : no_merged) if (!cm.empty() && !is_ancestor(c, cm, w)) any = true;
      ok = any;
    }
    if (ok && !points.empty()) {
      bool any = false;
      const Obj& o = probe.load(row.oid);
      for (auto& w : points) if (row.oid == w || (o.type == "tag" && o.tagged == w)) any = true;
      ok = any;
    }
    if (ok) kept.push_back(row);
  }
  rows = std::move(kept);
}

}  // namespace aios_git
