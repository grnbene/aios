// log, show, diff, rev-list, rev-parse, merge-base: reading history.
#include <algorithm>
#include <cstring>
#include <cctype>
#include <functional>
#include <memory>
#include <unordered_set>
#include <regex>
#include <sstream>

#include "aios/hal/host.hpp"
#include "cli.hpp"
#include "color.hpp"
#include "hist_util.hpp"
#include "history_graph.hpp"

namespace aios_git {

using namespace aios;
using namespace aios::userland;

namespace {

std::string body_of(const std::string& message) {
  // everything after the subject paragraph (git's %b)
  size_t i = 0;
  while (i < message.size() && message[i] == '\n') ++i;
  size_t blank = message.find("\n\n", i);
  if (blank == std::string::npos) return "";
  size_t s = blank + 2;
  while (s < message.size() && message[s] == '\n') ++s;
  return message.substr(s);
}

std::string name_of(const std::string& ident) {
  size_t lt = ident.find(" <");
  return lt == std::string::npos ? ident : ident.substr(0, lt);
}
std::string email_of(const std::string& ident) {
  size_t lt = ident.find('<'), gt = ident.find('>');
  if (lt == std::string::npos || gt == std::string::npos) return "";
  return ident.substr(lt + 1, gt - lt - 1);
}

std::string sanitize_subject(const std::string& s) {
  // %f: a file-name friendly subject
  std::string out;
  bool dash = false;
  for (char ch : s) {
    bool ok = std::isalnum((unsigned char)ch) || ch == '.' || ch == '_';
    if (ok) {
      if (dash && !out.empty()) out += '-';
      dash = false;
      out += ch;
    } else {
      dash = true;
    }
  }
  while (!out.empty() && out.back() == '.') out.pop_back();
  while (!out.empty() && out[0] == '.') out.erase(0, 1);
  return out;
}

struct Decorations {
  std::map<std::string, std::vector<std::string>> by_oid;
  // what each name is, for its color: 'L' local branch, 'R' remote-tracking, 'T' tag, 'S' stash, 'H' a detached HEAD,
  // 'P' "HEAD -> branch"
  std::map<std::string, std::vector<char>> kinds;
};

// log-tree.c's format_decorations: " (" + names + ")" with each name in its kind's color (color.decorate.<slot> over
// git's defaults) and the punctuation in the commit color. Plain when `color` is off.
std::string decorations_text(Ctx& c, const Decorations& d, const std::string& oid, bool color, const char* prefix = " (",
                             const char* suffix = ")") {
  auto it = d.by_oid.find(oid);
  if (it == d.by_oid.end()) return "";
  const auto& names = it->second;
  std::string out;
  if (!color) {
    for (size_t k = 0; k < names.size(); ++k) out += (k ? ", " : "") + names[k];
    return prefix + out + suffix;
  }
  static const std::map<char, std::pair<const char*, std::string>> defaults = {
      {'L', {"branch", "\033[1;32m"}}, {'R', {"remotebranch", "\033[1;31m"}}, {'T', {"tag", "\033[1;33m"}},
      {'S', {"stash", "\033[1;35m"}},  {'H', {"head", "\033[1;36m"}}};
  auto col = [&](char kind) -> std::string {
    auto d2 = defaults.find(kind);
    if (d2 == defaults.end()) return "";
    if (auto v = c.config.get(std::string("color.decorate.") + d2->second.first))
      if (auto parsed = parse_git_color(*v)) return *parsed;
    return d2->second.second;
  };
  const std::string commit = diff_palette(c).commit, reset = kColorReset;
  auto kinds = d.kinds.find(oid);
  const char* sep = prefix;
  for (size_t k = 0; k < names.size(); ++k) {
    const char kind = kinds != d.kinds.end() && k < kinds->second.size() ? kinds->second[k] : 'L';
    if (*sep) out += commit + sep + reset;
    const std::string& n = names[k];
    if (kind == 'P') {   // "HEAD -> branch": HEAD, the pointer in the commit color, the branch
      out += col('H') + "HEAD" + reset + commit + " -> " + reset + col('L') + n.substr(8) + reset;
    } else if (kind == 'T') {
      out += col('T') + "tag: " + reset + col('T') + n.substr(5) + reset;
    } else {
      out += col(kind) + n + reset;
    }
    sep = ", ";
  }
  if (*suffix) out += commit + suffix + reset;
  return out;
}

// The newest-added name is listed first: HEAD, then the refs from the last in name order to the first.
Decorations load_decorations(Ctx& c, bool full_names) {
  Decorations d;
  auto head = read_ref(c, "HEAD");
  std::string cur = current_branch(c);
  std::vector<std::pair<std::string, std::string>> refs = all_refs(c);
  auto add = [&](const std::string& oid, std::string name, char kind) {
    d.by_oid[oid].push_back(std::move(name));
    d.kinds[oid].push_back(kind);
  };
  if (head) {
    if (cur.empty()) add(*head, "HEAD", 'H');
    else add(*head, "HEAD -> " + (full_names ? "refs/heads/" + cur : cur), 'P');
  }
  // git lists a commit's names last ref first (each is put in front of the ones before it), HEAD's ahead of all
  for (auto it = refs.rbegin(); it != refs.rend(); ++it) {
    const std::string& name = it->first;
    std::string shown;
    std::string target = it->second;
    char kind;
    if (name.rfind("refs/heads/", 0) == 0) {
      if (name.substr(11) == cur) continue;
      shown = full_names ? name : name.substr(11);
      kind = 'L';
    } else if (name.rfind("refs/remotes/", 0) == 0) {
      shown = full_names ? name : name.substr(13);
      kind = 'R';
    } else if (name.rfind("refs/tags/", 0) == 0) {
      shown = "tag: " + (full_names ? name : name.substr(10));
      std::string p = peel_to(c, it->second, "commit");
      if (!p.empty()) target = p;
      kind = 'T';
    } else if (name == "refs/stash") {
      shown = "refs/stash";
      kind = 'S';
    } else {
      continue;
    }
    add(target, shown, kind);
  }
  return d;
}

struct Pretty {
  std::string kind = "medium";     // oneline short medium full fuller raw format tformat email
  std::string fmt;
  std::string date_mode = "default";
  bool abbrev_commit = false;
  int abbrev = 0;               // --abbrev=<n>, 0 for the repository's own length
  bool decorate = false;
  bool decorate_full = false;
  char term = '\n';             // what ends an entry: a newline, or NUL with -z
  std::string mark;             // <, >, = or + before a commit's id (--left-right, --cherry-mark)
  int expand_tabs = -1;         // --expand-tabs[=n] / --no-expand-tabs; -1: the format's own (8 for medium/full/fuller)
  std::string after_id;         // parents and children after the id (--parents, --children)
};

std::string short_id(Ctx& c, const Pretty& pr, const std::string& oid) { return c.abbrev(oid, pr.abbrev); }

std::vector<std::string> utf8_chars(const std::string& s) {
  std::vector<std::string> out;
  for (size_t i = 0; i < s.size();) {
    size_t n = 1;
    unsigned char b = (unsigned char)s[i];
    if (b >= 0xF0) n = 4;
    else if (b >= 0xE0) n = 3;
    else if (b >= 0xC0) n = 2;
    out.push_back(s.substr(i, n));
    i += n;
  }
  return out;
}

std::string join_chars(const std::vector<std::string>& v, size_t from, size_t to) {
  std::string s;
  for (size_t i = from; i < to && i < v.size(); ++i) s += v[i];
  return s;
}

// The trailer block of a message (git's interpret-trailers rules): the last paragraph when it is made of
// "Token: value" lines, with continuation lines kept, or mostly so and holding a Signed-off-by / cherry-pick line.
std::vector<std::string> trailer_lines(const std::string& message) {
  std::vector<std::string> lines;
  std::string cur;
  for (char ch : message) { if (ch == '\n') { lines.push_back(cur); cur.clear(); } else cur += ch; }
  if (!cur.empty()) lines.push_back(cur);
  while (!lines.empty() && lines.back().find_first_not_of(" \t") == std::string::npos) lines.pop_back();
  size_t end = lines.size(), start = end;
  while (start > 0 && lines[start - 1].find_first_not_of(" \t") != std::string::npos) --start;
  if (start == 0) return {};
  auto is_trailer = [](const std::string& l) {
    size_t colon = l.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    for (size_t i = 0; i < colon; ++i) if (!std::isalnum((unsigned char)l[i]) && l[i] != '-') return false;
    return true;
  };
  int trailers = 0, others = 0;
  bool recognized = false;
  for (size_t i = start; i < end; ++i) {
    const std::string& l = lines[i];
    if (l.rfind("Signed-off-by: ", 0) == 0 || l.rfind("(cherry picked from commit ", 0) == 0) recognized = true;
    if (!l.empty() && (l[0] == ' ' || l[0] == '\t')) continue;
    if (is_trailer(l)) ++trailers;
    else ++others;
  }
  if (!trailers || (others && !(recognized && trailers * 3 >= others))) return {};
  return std::vector<std::string>(lines.begin() + (ptrdiff_t)start, lines.begin() + (ptrdiff_t)end);
}

// %xNN, %n and %% inside an option value.
std::string literal_escapes(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 1 < in.size()) {
      if (in[i + 1] == 'n') { out += '\n'; ++i; continue; }
      if (in[i + 1] == '%') { out += '%'; ++i; continue; }
      if (in[i + 1] == 'x' && i + 3 < in.size() + 0 && std::isxdigit((unsigned char)in[i + 2]) && std::isxdigit((unsigned char)in[i + 3])) {
        out += (char)std::stoi(in.substr(i + 2, 2), nullptr, 16);
        i += 3;
        continue;
      }
    }
    out += in[i];
  }
  return out;
}

// %(trailers) and its options: only, unfold, keyonly, valueonly, key=<k>, separator=<s>, key_value_separator=<s>.
bool expand_trailers(const std::string& message, const std::string& options, std::string& out) {
  bool only = false, unfold = false, key_only = false, value_only = false;
  std::string sep, kv_sep = ": ", key;
  bool has_sep = false;
  size_t at = 0;
  while (at < options.size()) {
    size_t comma = options.find(',', at);
    std::string opt = options.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
    at = comma == std::string::npos ? options.size() : comma + 1;
    if (opt == "only" || opt == "only=true") only = true;
    else if (opt == "only=false") only = false;
    else if (opt == "unfold") unfold = true;
    else if (opt == "keyonly") key_only = true;
    else if (opt == "valueonly") value_only = true;
    else if (opt.rfind("key=", 0) == 0) key = opt.substr(4);
    else if (opt.rfind("separator=", 0) == 0) { sep = literal_escapes(opt.substr(10)); has_sep = true; }
    else if (opt.rfind("key_value_separator=", 0) == 0) kv_sep = literal_escapes(opt.substr(20));
    else return false;
  }
  std::vector<std::string> block = trailer_lines(message);
  std::vector<std::string> items;
  for (size_t i = 0; i < block.size(); ++i) {
    std::string item = block[i];
    size_t colon = item.find(':');
    bool trailer = colon != std::string::npos && colon > 0;
    for (size_t k = 0; trailer && k < colon; ++k) if (!std::isalnum((unsigned char)item[k]) && item[k] != '-') trailer = false;
    bool continuation = !item.empty() && (item[0] == ' ' || item[0] == '\t');
    if (continuation) {
      if (items.empty()) continue;
      std::string extra = item;
      if (unfold) {
        size_t s = extra.find_first_not_of(" \t");
        extra = " " + (s == std::string::npos ? "" : extra.substr(s));
      } else {
        extra = "\n" + extra;
      }
      items.back() += extra;
      continue;
    }
    if (!trailer && only) continue;
    items.push_back(item);
  }
  std::string text;
  size_t shown = 0;
  for (auto& item : items) {
    std::string k, v;
    size_t colon = item.find(':');
    if (colon != std::string::npos) {
      k = item.substr(0, colon);
      size_t s = item.find_first_not_of(" \t", colon + 1);
      v = s == std::string::npos ? "" : item.substr(s);
    } else {
      v = item;
    }
    if (!key.empty()) {
      std::string lk = k, lwant = key;
      std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
      std::transform(lwant.begin(), lwant.end(), lwant.begin(), ::tolower);
      if (lk != lwant) continue;
    }
    std::string line;
    if (key_only) line = k;
    else if (value_only) line = v;
    else line = colon != std::string::npos ? k + kv_sep + v : item;
    if (has_sep) { if (shown) text += sep; text += line; }
    else text += line + "\n";
    ++shown;
  }
  out += text;
  return true;
}

// The placeholder whose letter is at fmt[i] (just after the '%'); on return i is its last character.
// False when it is not a placeholder git knows (it is then written out as it was).
bool expand_one(Ctx& c, const std::string& fmt, size_t& i, const GitCommit& cm, const Pretty& pr, Decorations* deco,
                std::string& out, bool& auto_color, size_t written = 0) {
  char n = fmt[i];
  auto two = [&](char x) { return i + 1 < fmt.size() && fmt[i + 1] == x; };
  switch (n) {
    case '%': out += '%'; return true;
    case 'n': out += '\n'; return true;
    case 'H':
    case 'h': {
      const std::string id = n == 'H' ? cm.oid : short_id(c, pr, cm.oid);
      if (auto_color) out += diff_palette(c).commit + id + kColorReset;
      else out += id;
      return true;
    }
    case 'm': out += pr.mark.size() > 1 ? pr.mark.substr(0, 1) : (pr.mark.empty() ? ">" : pr.mark); return true;
    case 'T': out += cm.tree; return true;
    case 't': out += short_id(c, pr, cm.tree); return true;
    case 'P': for (size_t k = 0; k < cm.parents.size(); ++k) out += (k ? " " : "") + cm.parents[k]; return true;
    case 'p': for (size_t k = 0; k < cm.parents.size(); ++k) out += (k ? " " : "") + short_id(c, pr, cm.parents[k]); return true;
    case 's': out += subject_of(cm.message); return true;
    case 'f': out += sanitize_subject(subject_of(cm.message)); return true;
    case 'b': out += body_of(cm.message); return true;
    case 'B': out += cm.message; return true;
    case 'e': return true;
    case 'd':
    case 'D': {
      if (!deco) return true;
      out += n == 'd' ? decorations_text(c, *deco, cm.oid, auto_color) : decorations_text(c, *deco, cm.oid, auto_color, "", "");
      return true;
    }
    case 'x': {
      if (i + 2 < fmt.size() && std::isxdigit((unsigned char)fmt[i + 1]) && std::isxdigit((unsigned char)fmt[i + 2])) {
        out += (char)std::stoi(fmt.substr(i + 1, 2), nullptr, 16);
        i += 2;
        return true;
      }
      return false;
    }
    case 'a':
    case 'c': {
      if (i + 1 >= fmt.size()) return false;
      char k = fmt[i + 1];
      const std::string& who = n == 'a' ? cm.author : cm.committer;
      int64_t t = (int64_t)(n == 'a' ? cm.author_time : cm.committer_time);
      const std::string& tz = n == 'a' ? cm.author_tz : cm.committer_tz;
      switch (k) {
        case 'n': case 'N': out += name_of(who); break;
        case 'e': case 'E': out += email_of(who); break;
        case 'l': case 'L': { std::string e = email_of(who); out += e.substr(0, e.find('@')); break; }
        case 'd': out += format_date(t, tz, pr.date_mode); break;
        case 'D': out += format_date(t, tz, "rfc"); break;
        case 'r': out += format_date(t, tz, "relative"); break;
        case 't': out += std::to_string(t); break;
        case 'i': out += format_date(t, tz, "iso"); break;
        case 'I': out += format_date(t, tz, "iso-strict"); break;
        case 's': out += format_date(t, tz, "short"); break;
        case 'h': unsupported(c, std::string("the placeholder %") + n + "h");
        default: return false;
      }
      ++i;
      return true;
    }
    case 'C': {
      // pretty.c: %C(auto) colors what follows as log would; %C(<color>) and %C(always,<color>) write the color
      // (the plain one only when colors are on); %Cred %Cgreen %Cblue %Creset are the old spellings
      if (fmt.compare(i + 1, 6, "(auto)") == 0) {
        auto_color = c.color;
        if (auto_color && written + out.size() > 0) out += kColorReset;   // pretty.c: when anything is written already
        i += 6;
        return true;
      }
      if (two('(')) {
        size_t e = fmt.find(')', i);
        if (e == std::string::npos) return false;
        std::string spec = fmt.substr(i + 2, e - i - 2);
        bool on = c.color;
        if (spec.rfind("auto,", 0) == 0) spec = spec.substr(5);
        else if (spec.rfind("always,", 0) == 0) { spec = spec.substr(7); on = true; }
        i = e;
        auto_color = false;
        if (!on) return true;
        auto col = parse_git_color(spec);
        if (!col) die(c, "unable to parse --pretty format");
        out += *col;
        return true;
      }
      static const std::pair<const char*, const char*> basic[] = {
          {"red", "\033[31m"}, {"green", "\033[32m"}, {"blue", "\033[34m"}, {"reset", "\033[m"}};
      for (auto& [name, code] : basic)
        if (fmt.compare(i + 1, std::strlen(name), name) == 0) {
          i += std::strlen(name);
          auto_color = false;
          if (c.color) out += code;
          return true;
        }
      return false;
    }
    case 'w': unsupported(c, "the placeholder %w(...)");
    case 'S': unsupported(c, "the placeholder %S");
    case 'N': {
      auto note_ref = c.config.get("core.notesref").value_or("refs/notes/commits");
      auto note = c.repo->read_note(note_ref, cm.oid);
      if (note.ok()) out += *note;
      return true;
    }
    case 'G': {
      if (i + 1 >= fmt.size()) return false;
      char k = fmt[i + 1];
      if (std::string("?GSKFPT").find(k) == std::string::npos) return false;
      if (!cm.header("gpgsig").empty() || !cm.header("gpgsig-sha256").empty()) unsupported(c, "the signature placeholders on a signed commit");
      if (k == '?') out += 'N';
      ++i;
      return true;
    }
    case 'g': {
      if (i + 1 >= fmt.size() || std::string("DdnNeEs").find(fmt[i + 1]) == std::string::npos) return false;
      ++i;
      return true;
    }
    case '(': {
      size_t e = fmt.find(')', i);
      if (e == std::string::npos) return false;
      std::string name = fmt.substr(i + 1, e - i - 1);
      if (name == "trailers" || name.rfind("trailers:", 0) == 0) {
        if (!expand_trailers(cm.message, name.size() > 9 ? name.substr(9) : "", out)) unsupported(c, "the option of %(" + name + ")");
        i = e;
        return true;
      }
      if (name.rfind("describe", 0) == 0) unsupported(c, "the placeholder %(describe)");
      return false;
    }
    default: return false;
  }
}

struct Padding {
  int width = 0;
  bool to_column = false;
  char align = '<';       // '<' text at the left, '>' at the right, '=' centered
  char trunc = 0;         // 'r', 'l', 'm' or none
};

bool parse_padding(const std::string& fmt, size_t& i, Padding& p) {
  // fmt[i] is '<' or '>'
  size_t j = i;
  if (fmt[j] == '<') p.align = '<';
  else p.align = '>';
  ++j;
  if (j < fmt.size() && fmt[j] == '<' && p.align == '>') { p.align = '='; ++j; }
  else if (j < fmt.size() && fmt[j] == '>' && p.align == '>') return false;
  if (j < fmt.size() && fmt[j] == '|') { p.to_column = true; ++j; }
  if (j >= fmt.size() || fmt[j] != '(') return false;
  size_t e = fmt.find(')', j);
  if (e == std::string::npos) return false;
  std::string spec = fmt.substr(j + 1, e - j - 1);
  size_t comma = spec.find(',');
  std::string num = spec.substr(0, comma);
  if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos) return false;
  p.width = std::atoi(num.c_str());
  if (comma != std::string::npos) {
    std::string t = spec.substr(comma + 1);
    if (t == "trunc") p.trunc = 'r';
    else if (t == "ltrunc") p.trunc = 'l';
    else if (t == "mtrunc") p.trunc = 'm';
    else if (t != "") return false;
  }
  i = e;
  return true;
}

std::string pad_text(const std::string& text, int width, const Padding& p) {
  std::vector<std::string> ch = utf8_chars(text);
  int len = (int)ch.size();
  if (len > width) {
    if (!p.trunc || width < 2) return text;
    int keep = width - 2;
    if (p.trunc == 'r') return join_chars(ch, 0, (size_t)keep) + "..";
    if (p.trunc == 'l') return ".." + join_chars(ch, (size_t)(len - keep), (size_t)len);
    int at = width / 2 - 1;
    return join_chars(ch, 0, (size_t)at) + ".." + join_chars(ch, (size_t)(at + len - keep), (size_t)len);
  }
  int fill = width - len;
  int left = p.align == '>' ? fill : p.align == '=' ? fill / 2 : 0;
  return std::string((size_t)left, ' ') + text + std::string((size_t)(fill - left), ' ');
}

std::string expand_format(Ctx& c, const std::string& fmt, const GitCommit& cm, const Pretty& pr, Decorations* deco) {
  std::string out;
  bool auto_color = false;   // set by %C(auto), cleared by any other color
  Padding pending;
  bool padded = false;
  for (size_t i = 0; i < fmt.size(); ++i) {
    char ch = fmt[i];
    if (ch != '%' || i + 1 >= fmt.size()) { out += ch; continue; }
    size_t at = i;
    ++i;
    if (fmt[i] == '<' || fmt[i] == '>') {
      Padding p;
      if (parse_padding(fmt, i, p)) { pending = p; padded = true; continue; }
      out += '%';
      i = at;
      continue;
    }
    char magic = 0;
    if ((fmt[i] == '+' || fmt[i] == '-' || fmt[i] == ' ') && i + 1 < fmt.size()) { magic = fmt[i]; ++i; }
    std::string piece;
    size_t last = i;
    if (!expand_one(c, fmt, last, cm, pr, deco, piece, auto_color, out.size())) {
      out += '%';
      i = at;
      continue;
    }
    i = last;
    if (padded) {
      int width = pending.width;
      if (pending.to_column) {
        size_t nl = out.rfind('\n');
        std::string line = nl == std::string::npos ? out : out.substr(nl + 1);
        width -= (int)utf8_chars(line).size();
      }
      piece = pad_text(piece, width, pending);
      padded = false;
    }
    if (magic == '+' && !piece.empty()) out += '\n';
    else if (magic == ' ' && !piece.empty()) out += ' ';
    else if (magic == '-' && piece.empty()) while (!out.empty() && out.back() == '\n') out.pop_back();
    out += piece;
  }
  return out;
}

namespace {
// pp_utf8_width: the columns of [b, e) as git's utf8_width counts them (wide East Asian characters two, combining
// marks none), or -1 when it holds a control character or is not UTF-8 - git then stops aligning the line.
int pp_utf8_width(const char* b, const char* e) {
  int w = 0;
  while (b < e) {
    const unsigned char ch = (unsigned char)*b;
    uint32_t cp;
    int len;
    if (ch < 0x80) { cp = ch; len = 1; }
    else if ((ch & 0xe0) == 0xc0) { cp = ch & 0x1fu; len = 2; }
    else if ((ch & 0xf0) == 0xe0) { cp = ch & 0x0fu; len = 3; }
    else if ((ch & 0xf8) == 0xf0) { cp = ch & 0x07u; len = 4; }
    else return -1;
    if (e - b < len) return -1;
    for (int k = 1; k < len; ++k) {
      if ((b[k] & 0xc0) != 0x80) return -1;
      cp = (cp << 6) | ((unsigned char)b[k] & 0x3fu);
    }
    b += len;
    if (cp == 0) continue;
    if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) return -1;
    const bool zero = (cp >= 0x300 && cp <= 0x36f) || (cp >= 0x483 && cp <= 0x489) || (cp >= 0x591 && cp <= 0x5bd) ||
                      (cp >= 0x610 && cp <= 0x61a) || (cp >= 0x64b && cp <= 0x65f) || (cp >= 0x200b && cp <= 0x200f) ||
                      (cp >= 0x20d0 && cp <= 0x20f0) || (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xfe20 && cp <= 0xfe2f) || cp == 0xfeff;
    if (zero) continue;
    const bool wide = (cp >= 0x1100 && cp <= 0x115f) || cp == 0x2329 || cp == 0x232a || (cp >= 0x2e80 && cp <= 0xa4cf && cp != 0x303f) ||
                      (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe30 && cp <= 0xfe6f) ||
                      (cp >= 0xff00 && cp <= 0xff60) || (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x1f300 && cp <= 0x1f64f) ||
                      (cp >= 0x1f900 && cp <= 0x1f9ff) || (cp >= 0x20000 && cp <= 0x3fffd);
    w += wide ? 2 : 1;
  }
  return w;
}

// strbuf_add_tabexpand: each tab to the next multiple of `width` columns, until a part git cannot measure.
void add_tabexpand(std::string& out, const char* line, size_t len, int width) {
  const char* end = line + len;
  while (const char* tab = (const char*)std::memchr(line, '\t', (size_t)(end - line))) {
    const int w = pp_utf8_width(line, tab);
    if (w < 0) break;
    out.append(line, (size_t)(tab - line));
    out.append((size_t)(width - (w % width)), ' ');
    line = tab + 1;
  }
  out.append(line, (size_t)(end - line));
}
}  // namespace

// tabs: expand tabs to this width (git's medium, full and fuller formats use 8), 0 to keep them
std::string indent_message(const std::string& message, const char* ind, int tabs = 0) {
  std::string out;
  std::vector<std::string> lines;
  std::string cur;
  for (char ch : message) { if (ch == '\n') { lines.push_back(cur); cur.clear(); } else cur += ch; }
  if (!cur.empty()) lines.push_back(cur);
  // leading blank lines are dropped, trailing ones too
  size_t a = 0, b = lines.size();
  // blank as git's is_blank_line has it: nothing but git's isspace (a lone "\r" too, a form feed not)
  while (a < b && lines[a].find_first_not_of(" \t\r") == std::string::npos) ++a;
  while (b > a && lines[b - 1].find_first_not_of(" \t\r") == std::string::npos) --b;
  // each line without its trailing white space, as git's pp_remainder prints it (is_blank_line trims it)
  for (size_t i = a; i < b; ++i) {
    size_t e = lines[i].size();
    while (e > 0 && git_isspace((unsigned char)lines[i][e - 1])) --e;
    out += ind;
    if (tabs > 0) add_tabexpand(out, lines[i].data(), e, tabs);
    else out.append(lines[i], 0, e);
    out += '\n';
  }
  return out;
}

void print_header(Ctx& c, const GitCommit& cm, const Pretty& pr, Decorations* deco, std::ostream& out, bool& first,
                  bool with_diff) {
  auto deco_str = [&]() -> std::string {
    if (!pr.decorate || !deco) return "";
    return decorations_text(c, *deco, cm.oid, c.color);
  };
  // show_log: the commit color over "commit ", the mark, the id and its parents, reset before the decorations
  const std::string commit_c = c.color ? diff_palette(c).commit : "", reset_c = c.color ? kColorReset : "";
  std::string id = pr.abbrev_commit ? short_id(c, pr, cm.oid) : cm.oid;
  if (pr.kind == "format" || pr.kind == "tformat") {
    std::string s = expand_format(c, pr.fmt, cm, pr, deco);
    if (pr.kind == "format") {
      if (!first) out << pr.term;
      out << s;
    } else if (!(s.empty() && with_diff)) {
      out << s << pr.term;   // an empty --format= with a diff after it prints no line of its own
    }
    first = false;
    return;
  }
  if (pr.kind == "oneline") {
    out << commit_c << pr.mark << id << pr.after_id << reset_c << deco_str() << " " << subject_of(cm.message) << pr.term;
    first = false;
    return;
  }
  if (!first) out << pr.term;
  first = false;
  out << commit_c << "commit " << pr.mark << id << pr.after_id << reset_c << deco_str() << "\n";
  if (cm.parents.size() > 1 && pr.kind != "raw") {
    out << "Merge:";
    for (auto& p : cm.parents) out << " " << short_id(c, pr, p);
    out << "\n";
  }
  if (pr.kind == "raw") {
    out << "tree " << cm.tree << "\n";
    for (auto& p : cm.parents) out << "parent " << p << "\n";
    out << "author " << cm.author << " " << cm.author_time << " " << cm.author_tz << "\n";
    out << "committer " << cm.committer << " " << cm.committer_time << " " << cm.committer_tz << "\n";
    // every header after committer as stored (gpgsig, mergetag, encoding...), continuation lines folded back
    for (auto& [k, v] : cm.extra_headers) {
      std::string folded = v;
      for (size_t at = folded.find('\n'); at != std::string::npos; at = folded.find('\n', at + 2)) folded.insert(at + 1, " ");
      out << k << " " << folded << "\n";
    }
    std::string raw_body = indent_message(cm.message, "    ", pr.expand_tabs > 0 ? pr.expand_tabs : 0);
    if (!raw_body.empty()) out << "\n" << raw_body;
    return;
  }
  out << (pr.kind == "fuller" ? "Author:     " : "Author: ") << cm.author << "\n";
  if (pr.kind == "medium") out << "Date:   " << format_date((int64_t)cm.author_time, cm.author_tz, pr.date_mode) << "\n";
  if (pr.kind == "full" || pr.kind == "fuller") {
    if (pr.kind == "fuller") {
      out << "AuthorDate: " << format_date((int64_t)cm.author_time, cm.author_tz, pr.date_mode) << "\n";
      out << "Commit:     " << cm.committer << "\n";
      out << "CommitDate: " << format_date((int64_t)cm.committer_time, cm.committer_tz, pr.date_mode) << "\n";
    } else {
      out << "Commit: " << cm.committer << "\n";
    }
  }
  std::string title_paragraph = cm.message.substr(0, cm.message.find("\n\n"));
  const int tabs = pr.expand_tabs >= 0 ? pr.expand_tabs : (pr.kind == "medium" || pr.kind == "full" || pr.kind == "fuller") ? 8 : 0;
  std::string body = indent_message(pr.kind == "short" ? title_paragraph : cm.message, "    ", tabs);
  if (!body.empty()) out << "\n" << body;
}

// Shared by log and show: the options that choose how a commit is printed.
std::vector<std::string> take_pretty(Ctx& c, const std::vector<std::string>& args, Pretty& pr) {
  std::vector<std::string> rest;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--") { for (; i < args.size(); ++i) rest.push_back(args[i]); break; }
    std::function<void(const std::string&, bool)> set_fmt = [&](const std::string& v, bool is_format_opt) {
      if (v.rfind("format:", 0) == 0) { pr.kind = "format"; pr.fmt = v.substr(7); }
      else if (v.rfind("tformat:", 0) == 0) { pr.kind = "tformat"; pr.fmt = v.substr(8); }
      else if (v == "oneline" || v == "short" || v == "medium" || v == "full" || v == "fuller" || v == "raw") pr.kind = v;
      else if (v == "email" || v == "mboxrd" || v == "reference") unsupported(c, "--pretty=" + v);
      else if (auto alias = c.config.get("pretty." + v)) set_fmt(*alias, is_format_opt);
      else if (v.find('%') != std::string::npos || is_format_opt) { pr.kind = "tformat"; pr.fmt = v; }
      else fail(c, "invalid --pretty format: " + v, 128);
    };
    if (a == "--oneline") { pr.kind = "oneline"; pr.abbrev_commit = true; }
    else if (a.rfind("--pretty=", 0) == 0) set_fmt(a.substr(9), false);
    else if (a == "--pretty") pr.kind = "medium";
    else if (a.rfind("--format=", 0) == 0) set_fmt(a.substr(9), true);
    else if (a == "--format" && i + 1 < args.size()) set_fmt(args[++i], true);
    else if (a == "--abbrev-commit") pr.abbrev_commit = true;
    else if (a.rfind("--abbrev=", 0) == 0) {
      int n = std::atoi(a.c_str() + 9);
      pr.abbrev = std::max(4, std::min(40, n));
    }
    else if (a == "--no-abbrev-commit") pr.abbrev_commit = false;
    else if (a == "--expand-tabs") pr.expand_tabs = 8;
    else if (a.rfind("--expand-tabs=", 0) == 0) pr.expand_tabs = std::atoi(a.c_str() + 14);
    else if (a == "--no-expand-tabs") pr.expand_tabs = 0;
    else if (a.rfind("--date=", 0) == 0) {
      pr.date_mode = a.substr(7);
      if (pr.date_mode == "local") pr.date_mode = "default-local";
      if (pr.date_mode.rfind("format:", 0) == 0 || pr.date_mode.rfind("human", 0) == 0) unsupported(c, "--date=" + pr.date_mode);
    } else if (a == "--decorate" || a == "--decorate=short" || a == "--decorate=auto") { pr.decorate = true; pr.decorate_full = false; }
    else if (a == "--no-decorate" || a == "--decorate=no") pr.decorate = false;
    else if (a == "--decorate=full") { pr.decorate = true; pr.decorate_full = true; }
    else rest.push_back(a);
  }
  return rest;
}

// a "-<n>" or -n <n> count, --max-count
bool take_count(const std::vector<std::string>& args, size_t& i, long long& count) {
  const std::string& a = args[i];
  if (a.size() > 1 && a[0] == '-' && std::isdigit((unsigned char)a[1])) { count = std::atoll(a.c_str() + 1); return true; }
  if (a == "-n" && i + 1 < args.size()) { count = std::atoll(args[++i].c_str()); return true; }
  if (a.rfind("-n", 0) == 0 && a.size() > 2 && std::isdigit((unsigned char)a[2])) { count = std::atoll(a.c_str() + 2); return true; }
  if (a.rfind("--max-count=", 0) == 0) { count = std::atoll(a.c_str() + 12); return true; }
  if (a == "--max-count" && i + 1 < args.size()) { count = std::atoll(args[++i].c_str()); return true; }
  return false;
}

// Options of history walks that only some callers print: what to show beside each commit and which commits a diff picks.
struct HistoryFlags {
  bool parents = false, children = false, left_right = false, graph = false, follow = false;
  bool left_only = false, right_only = false;
  bool has_s = false, has_g = false, pickaxe_regex = false;
  std::string pick_s, pick_g;
};

// Walk options common to log and rev-list. Returns arguments left over (revisions and paths).
std::vector<std::string> take_walk_options(Ctx& c, const std::vector<std::string>& args, WalkOptions& w, HistoryFlags& hf) {
  std::vector<std::string> rest;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    long long n;
    if (a == "--") { for (; i < args.size(); ++i) rest.push_back(args[i]); break; }
    if (take_count(args, i, n)) { w.max_count = n; continue; }
    if (a.rfind("--skip=", 0) == 0) { w.skip = std::atoll(a.c_str() + 7); continue; }
    if (a == "--first-parent") { w.first_parent = true; continue; }
    if (a == "--no-merges") { w.max_parents = 1; continue; }
    if (a == "--merges") { w.min_parents = 2; continue; }
    if (a.rfind("--max-parents=", 0) == 0) { w.max_parents = std::atoi(a.c_str() + 14); continue; }
    if (a.rfind("--min-parents=", 0) == 0) { w.min_parents = std::atoi(a.c_str() + 14); continue; }
    if (a == "--reverse") { w.reverse = true; continue; }
    if (a == "--topo-order") { w.topo = true; w.date_order = w.author_date_order = false; continue; }
    if (a == "--date-order") { w.date_order = true; w.topo = w.author_date_order = false; continue; }
    if (a == "--author-date-order") { w.author_date_order = true; w.topo = w.date_order = false; continue; }
    if (a == "--parents") { hf.parents = true; w.rewrite_parents = true; continue; }
    if (a == "--children") { hf.children = true; w.want_children = true; continue; }
    if (a == "--left-right") { hf.left_right = true; continue; }
    if (a == "--left-only") { hf.left_only = true; hf.right_only = false; continue; }
    if (a == "--right-only") { hf.right_only = true; hf.left_only = false; continue; }
    if (a == "--cherry-pick") { w.cherry_pick = true; continue; }
    if (a == "--cherry-mark") { w.cherry_mark = true; continue; }
    if (a == "--graph") { hf.graph = true; w.rewrite_parents = true; continue; }
    if (a == "--follow") { hf.follow = true; continue; }
    if (a == "--pickaxe-regex") { hf.pickaxe_regex = true; continue; }
    if (a == "--pickaxe-all") continue;
    if (a.rfind("-S", 0) == 0 && a != "-S") { hf.has_s = true; hf.pick_s = a.substr(2); continue; }
    if (a == "-S" && i + 1 < args.size()) { hf.has_s = true; hf.pick_s = args[++i]; continue; }
    if (a.rfind("-G", 0) == 0 && a != "-G") { hf.has_g = true; hf.pick_g = a.substr(2); continue; }
    if (a == "-G" && i + 1 < args.size()) { hf.has_g = true; hf.pick_g = args[++i]; continue; }
    if (a.rfind("--author=", 0) == 0) { w.authors.push_back(a.substr(9)); continue; }
    if (a == "--author" && i + 1 < args.size()) { w.authors.push_back(args[++i]); continue; }
    if (a.rfind("--grep=", 0) == 0) { w.greps.push_back(a.substr(7)); continue; }
    if (a == "--grep" && i + 1 < args.size()) { w.greps.push_back(args[++i]); continue; }
    if (a == "-i" || a == "--regexp-ignore-case") { w.grep_icase = true; continue; }
    if (a == "--all-match") { w.all_match = true; continue; }
    if (a == "--invert-grep") { w.invert_grep = true; continue; }
    if (a.rfind("--since=", 0) == 0 || a.rfind("--after=", 0) == 0 || a.rfind("--until=", 0) == 0 || a.rfind("--before=", 0) == 0) {
      std::string v = a.substr(a.find('=') + 1);
      int64_t t;
      if (!parse_approxidate(v, t)) unsupported(c, "the date '" + v + "'");
      if (a[2] == 's' || a[2] == 'a') w.since = t;
      else w.until = t;
      continue;
    }
    if (a == "--boundary" || a == "--simplify-by-decoration" || a == "--ancestry-path" || a == "-g" || a == "--walk-reflogs" ||
        a == "--full-history" || a == "--simplify-merges" || a == "--objects" || a == "--bisect" || a == "-L" || a.rfind("-L", 0) == 0 ||
        a == "--source" || a == "--exclude")
      unsupported(c, "the history option " + a);
    if (a == "-S" || a == "-G") usage(c, std::string("switch `") + a[1] + "' requires a value");
    rest.push_back(a);
  }
  if (w.rewrite_parents && w.want_children) die(c, "options '--parents' and '--children' cannot be used together");
  if (hf.graph && w.reverse) die(c, "options '--reverse' and '--graph' cannot be used together");
  if (hf.graph && !w.date_order && !w.author_date_order) w.topo = true;
  return rest;
}

// Splits revisions from paths: everything after "--" is a path; before it, a revision unless it is not
// one and names a file.
void split_revs_paths(Ctx& c, const std::vector<std::string>& in, std::vector<std::string>& revs, std::vector<std::string>& paths) {
  bool after = false;
  for (auto& a : in) {
    if (after) { paths.push_back(a); continue; }
    if (a == "--") { after = true; continue; }
    if (!a.empty() && a[0] == '-' && a != "-" && a.rfind("--", 0) != 0 && a.size() > 1 && !std::isdigit((unsigned char)a[1]))
      usage(c, "unknown option `" + a + "'");
    if (!a.empty() && a.rfind("--", 0) == 0 && a != "--all" && a != "--branches" && a != "--tags" && a != "--remotes" && a != "--not")
      usage(c, "unrecognized argument: " + a);
    revs.push_back(a);
  }
}

void check_paths_or_revs(Ctx& c, std::vector<std::string>& revs, std::vector<std::string>& paths, bool dashdash_seen) {
  // anything in revs that does not resolve must be a path that exists (or git refuses)
  std::vector<std::string> keep;
  for (auto& r : revs) {
    if (r == "--all" || r == "--branches" || r == "--tags" || r == "--remotes" || r == "--not") { keep.push_back(r); continue; }
    std::string probe = r;
    if (probe.size() > 1 && probe[0] == '^') probe = probe.substr(1);
    size_t dd = probe.find("..");
    bool ok = false;
    if (dd != std::string::npos) {
      std::string l = probe.substr(0, dd), rr = probe.substr(dd + (probe.compare(dd, 3, "...") == 0 ? 3 : 2));
      ok = resolve(c, l.empty() ? "HEAD" : l).has_value() && resolve(c, rr.empty() ? "HEAD" : rr).has_value();
    } else {
      ok = resolve(c, probe).has_value();
    }
    if (ok) { keep.push_back(r); continue; }
    if (!dashdash_seen && c.host().exists(c.abs_path(r))) { paths.push_back(r); continue; }
    if (r.size() > 1 && r[0] == '^') die(c, "bad revision '" + r + "'");
    must_resolve(c, r);   // dies with git's message
  }
  revs = keep;
}

size_t count_occurrences(const std::string& hay, const std::string& needle, bool is_regex, const std::regex& re) {
  size_t n = 0;
  if (is_regex) {
    for (auto it = std::sregex_iterator(hay.begin(), hay.end(), re); it != std::sregex_iterator(); ++it) {
      ++n;
      if (it->length() == 0) break;
    }
    return n;
  }
  if (needle.empty()) return 0;
  for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) ++n;
  return n;
}

// -S<string> keeps a commit when a file holds the string a different number of times before and after it, and
// -G<regex> when a changed line matches. Merges show no diff in log, so neither keeps one.
bool pickaxe_keeps(Ctx& c, const GitCommit& cm, const HistoryFlags& hf, const Pathspec& ps, bool icase) {
  if (cm.parents.size() > 1) return false;
  std::string from = cm.parents.empty() ? "" : read_commit(c, cm.parents[0]).tree;
  DiffOptions o;
  auto pairs = diff_trees(c, from, cm.tree, ps, o);
  auto flags = std::regex::extended | (icase ? std::regex::icase : std::regex::extended);
  std::regex re;
  try {
    if (hf.has_g) re = std::regex(hf.pick_g, flags);
    else if (hf.pickaxe_regex) re = std::regex(hf.pick_s, flags);
  } catch (const std::regex_error&) {
    die(c, "invalid regex: " + (hf.has_g ? hf.pick_g : hf.pick_s));
  }
  for (auto& p : pairs) {
    std::string ta = pair_text(c, p, false), tb = pair_text(c, p, true);
    if (hf.has_s) {
      if (count_occurrences(ta, hf.pick_s, hf.pickaxe_regex, re) != count_occurrences(tb, hf.pick_s, hf.pickaxe_regex, re)) return true;
      continue;
    }
    if (is_binary(ta) || is_binary(tb)) continue;
    std::string h = git_unified_hunks(ta, tb, 0);
    size_t i = 0;
    while (i < h.size()) {
      size_t nl = h.find('\n', i);
      if (nl == std::string::npos) nl = h.size();
      if (nl > i && (h[i] == '+' || h[i] == '-')) {
        std::string line = h.substr(i + 1, nl - i - 1);
        if (std::regex_search(line, re)) return true;
      }
      i = nl + 1;
    }
  }
  return false;
}

// What the graph and the marks of A...B need from a commit: its mark, with and without the space that follows it.
std::string revision_mark(const WalkOptions& w, const HistoryFlags& hf, const std::string& oid) {
  if (w.patchsame.count(oid)) return "=";
  if (w.symmetric && w.left.count(oid)) return "<";
  if (hf.left_right) return ">";
  if (hf.graph) return "*";
  if (w.cherry_mark) return "+";
  return "";
}

}  // namespace

// ---------------------------------------------------------------------------
std::string reflog_user_format(const std::string& arg) {
  for (const char* pre : {"--format=", "--pretty=format:", "--pretty=tformat:"})
    if (arg.rfind(pre, 0) == 0 && arg.size() > std::strlen(pre)) return arg.substr(std::strlen(pre));
  return "";
}

std::string format_reflog_entry(Ctx& c, const std::string& fmt, const std::string& oid, const std::string& gd,
                                const std::string& gs) {
  // the selector placeholders are filled here (a '%' in them doubled so the commit pass leaves it), the rest as log does
  std::string pre;
  auto lit = [&](const std::string& s) { for (char ch : s) { pre += ch; if (ch == '%') pre += '%'; } };
  for (size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] == '%' && i + 1 < fmt.size() && fmt[i + 1] == '%') { pre += "%%"; ++i; continue; }
    if (fmt[i] == '%' && i + 2 < fmt.size() && fmt[i + 1] == 'g') {
      char k = fmt[i + 2];
      if (k == 'd' || k == 'D') { lit(gd); i += 2; continue; }
      if (k == 's') { lit(gs); i += 2; continue; }
    }
    pre += fmt[i];
  }
  Pretty pr;
  pr.kind = "tformat";
  return expand_format(c, pre, read_commit(c, oid), pr, nullptr);
}

// ---------------------------------------------------------------------------
// log.decorate: "auto", git's default, names branches and tags on commits when the output is a terminal.
bool default_decorate(Ctx& c) {
  auto v = c.config.get("log.decorate");
  if (!v || *v == "auto") return c.terminal;
  return *v == "short" || *v == "true" || *v == "yes" || *v == "on" || *v == "1";
}

int cmd_log(Ctx& c) {
  c.open_repo(false);
  std::vector<std::string> args(c.args.begin() + 1, c.args.end());
  Pretty pr;
  pr.decorate = default_decorate(c);
  if (auto d = c.config.get("log.date")) pr.date_mode = *d;
  args = take_pretty(c, args, pr);
  DiffOptions dopt;
  bool any_diff = false;
  args = take_diff_options(c, args, dopt, &any_diff);
  if (dopt.z) pr.term = '\0';
  WalkOptions w;
  HistoryFlags hf;
  args = take_walk_options(c, args, w, hf);
  // the diff options come first and take -S/-G/--pickaxe-regex with them; the walk is what keeps or drops commits by them
  if (dopt.has_pickaxe_s) { hf.has_s = true; hf.pick_s = dopt.pickaxe_s; }
  if (!dopt.pickaxe_g.empty()) { hf.has_g = true; hf.pick_g = dopt.pickaxe_g; }
  if (dopt.pickaxe_regex) hf.pickaxe_regex = true;
  std::vector<std::string> extra;
  for (auto& a : args) {
    if (a == "--no-walk") unsupported(c, "log --no-walk");
    if (a == "--stdin") unsupported(c, "log --stdin");
    if (a == "--name-only") { any_diff = true; dopt.format = DiffOptions::NameOnly; continue; }
    if (a == "--no-color" || a == "--no-mailmap" || a == "--mailmap" || a == "--no-show-signature" || a == "--color=never") continue;
    extra.push_back(a);
  }
  if (hf.graph && (hf.has_s || hf.has_g || hf.follow)) unsupported(c, "log --graph with --follow, -S or -G");
  std::vector<std::string> revs, paths;
  bool dd = std::find(extra.begin(), extra.end(), "--") != extra.end();
  split_revs_paths(c, extra, revs, paths);
  check_paths_or_revs(c, revs, paths, dd);
  if (hf.follow && paths.size() != 1) die(c, "--follow requires exactly one pathspec");
  auto rest = parse_revisions(c, revs, w, true);
  if (!rest.empty()) usage(c, "unrecognized argument: " + rest[0]);
  if (w.tips.empty()) {
    auto head = read_ref(c, "HEAD");
    if (!head) {
      std::string b = current_branch(c);
      die(c, "your current branch '" + b + "' does not have any commits yet");
    }
  }
  // --follow walks everything and keeps the commits that change the one path as it was named then, so the
  // path is not a walk filter; every other path filter simplifies the history.
  Pathspec filter_paths = make_pathspec(c, paths);
  if (!hf.follow) w.paths = filter_paths;
  std::string followed = hf.follow ? filter_paths.items[0] : "";
  std::map<std::string, std::vector<DiffPair>> follow_pairs;
  if (hf.has_s || hf.has_g || hf.follow || hf.left_only || hf.right_only) {
    w.post_filter = [&](const GitCommit& cm) {
      if (hf.left_only && !w.left.count(cm.oid)) return false;
      if (hf.right_only && w.left.count(cm.oid)) return false;
      if (hf.follow) {
        if (cm.parents.size() > 1) return false;
        std::string from = cm.parents.empty() ? "" : read_commit(c, cm.parents[0]).tree;
        Snapshot a = snapshot_tree(c, from), b = snapshot_tree(c, cm.tree);
        Pathspec one;
        one.items = {followed};
        DiffOptions o = dopt;
        o.renames = false;
        auto pairs = diff_snapshots(c, a, b, one, o);
        if (pairs.size() == 1 && pairs[0].status == 'A') {
          DiffOptions r = dopt;
          r.renames = true;
          for (auto& p : diff_snapshots(c, a, b, Pathspec{}, r)) {
            if ((p.status == 'R') && p.new_path == followed) { pairs = {p}; followed = p.old_path; break; }
          }
        }
        if (pairs.empty()) return false;
        follow_pairs[cm.oid] = pairs;
        return true;
      }
      if ((hf.has_s || hf.has_g) && !pickaxe_keeps(c, cm, hf, filter_paths, w.grep_icase)) return false;
      return true;
    };
  }
  Decorations deco;
  bool need_deco = pr.decorate || (pr.kind == "format" || pr.kind == "tformat");
  if (need_deco) deco = load_decorations(c, pr.decorate_full);
  bool first = true, missing_newline_prev = false;
  std::ostream& out = c.out;
  const bool user_fmt = pr.kind == "format" || pr.kind == "tformat";
  const bool terminator_style = pr.kind == "oneline" || pr.kind == "tformat";
  std::unique_ptr<HistoryGraph> graph;
  if (hf.graph) graph = std::make_unique<HistoryGraph>([&](const std::string& oid) { return w.interesting && w.interesting(oid); }, w.first_parent);
  if (graph && c.color) {
    // graph.c's column colors: log.graphColors (each one git's color syntax, the bad ones left out), else the twelve
    std::vector<std::string> cols;
    if (auto v = c.config.get("log.graphcolors")) {
      size_t at = 0;
      while (at <= v->size()) {
        size_t comma = v->find(',', at);
        if (comma == std::string::npos) comma = v->size();
        if (auto col = parse_git_color(v->substr(at, comma - at))) cols.push_back(*col);
        else
          c.err << "error: invalid color value: " << v->substr(at, comma - at) << "\nwarning: ignored invalid color '"
                << v->substr(at, comma - at) << "' in log.graphColors\n";
        at = comma + 1;
      }
    } else {
      cols = {"\033[31m", "\033[32m", "\033[33m", "\033[34m", "\033[35m", "\033[36m",
              "\033[1;31m", "\033[1;32m", "\033[1;33m", "\033[1;34m", "\033[1;35m", "\033[1;36m"};
    }
    cols.push_back(kColorReset);
    graph->set_colors(std::move(cols));
  }
  auto abbrev_id = [&](const std::string& oid) { return pr.abbrev_commit ? short_id(c, pr, oid) : oid; };
  // each commit is printed as the walk reaches it, so a reader that goes away stops the walk
  if (c.hide_merges) {
    // whatchanged: a commit with no diff to show (a merge, an empty commit) is not shown and, as cmd_log_walk puts
    // max_count back for it, not counted
    auto before = w.post_filter;
    w.post_filter = [&, before](const GitCommit& cm) {
      if (before && !before(cm)) return false;
      if (cm.parents.size() > 1) return false;
      std::string from = cm.parents.empty() ? "" : read_commit(c, cm.parents[0]).tree;
      DiffOptions q = dopt;
      q.renames = false;
      return !diff_trees(c, from, cm.tree, w.paths.empty() ? Pathspec{} : w.paths, q).empty();
    };
  }
  walk_each(c, w, [&](const GitCommit& cm) {
    std::string mark = revision_mark(w, hf, cm.oid);
    pr.mark = (mark.empty() || hf.graph) ? "" : mark + " ";
    pr.after_id.clear();
    if (hf.parents) for (auto& p : cm.parents) pr.after_id += " " + abbrev_id(p);
    if (hf.children) {
      auto ch = w.children.find(cm.oid);
      if (ch != w.children.end()) for (auto& k : ch->second) pr.after_id += " " + abbrev_id(k);
    }
    std::string diff_text;
    std::vector<DiffPair> pairs;
    // a merge shows no diff in log unless -m/--cc (not supported) - or under --first-parent, which diffs it against
    // its first parent as git does (diff_merges_default_to_first_parent); a root diffs against nothing
    if (any_diff && dopt.format != DiffOptions::Quiet && (cm.parents.size() <= 1 || w.first_parent)) {
      std::string from = cm.parents.empty() ? "" : read_commit(c, cm.parents[0]).tree;
      auto fp = follow_pairs.find(cm.oid);
      if (fp != follow_pairs.end()) pairs = fp->second;
      else pairs = diff_trees(c, from, cm.tree, w.paths.empty() ? Pathspec{} : w.paths, dopt);
    }
    // the diff's text, once the graph (if any) has placed the commit: a stat is laid out for the room the graph leaves
    auto render_diff = [&]() {
      if (pairs.empty()) return;
      DiffOptions o = dopt;
      if (graph) o.stat_prefix_bytes = (int)graph->padding_prefix_bytes();
      std::ostringstream ds;
      if (pr.kind != "oneline" && !(user_fmt && pr.fmt.empty())) ds << (diff_stat_and_patch(o) ? "---\n" : "\n");
      print_diff(c, pairs, o, ds);
      diff_text = ds.str();
    };
    // whatchanged (always_show_header off): a commit with no diff to show - a merge, an empty commit - has no entry
    if (c.hide_merges && pairs.empty()) return;
    if (!graph) {
      render_diff();
      print_header(c, cm, pr, need_deco ? &deco : nullptr, out, first, any_diff);
      out << diff_text;
      return;
    }
    graph->update(cm.oid, cm.parents, mark.empty() ? '*' : mark[0]);
    render_diff();
    std::string deco_part;
    if (pr.decorate && need_deco) deco_part = decorations_text(c, deco, cm.oid, c.color);
    std::string head, msg;
    if (user_fmt) {
      msg = expand_format(c, pr.fmt, cm, pr, need_deco ? &deco : nullptr);
    } else if (pr.kind == "oneline") {
      if (c.color) head = diff_palette(c).commit + abbrev_id(cm.oid) + pr.after_id + kColorReset + deco_part + " ";
      else head = abbrev_id(cm.oid) + pr.after_id + deco_part + " ";
      msg = subject_of(cm.message);
    } else {
      std::ostringstream hs;
      bool f = true;
      print_header(c, cm, pr, need_deco ? &deco : nullptr, hs, f, any_diff);
      std::string all = hs.str();
      size_t nl = all.find('\n');
      head = all.substr(0, nl + 1);
      msg = all.substr(nl + 1);
    }
    bool missing_newline = msg.empty() || msg.back() != '\n';
    if (!terminator_style && !first) {
      if (pr.term == '\n' && !missing_newline_prev) out << graph->padding_line();
      out << pr.term;
    }
    first = false;
    out << graph->show_commit();
    out << head;
    if (!user_fmt && pr.kind != "oneline") out << graph->show_oneline();
    out << graph->show_commit_msg(msg);
    missing_newline_prev = missing_newline;
    if (terminator_style && !(user_fmt && pr.fmt.empty())) {
      if (!missing_newline) out << graph->padding_line();
      out << pr.term;
    }
    size_t at = 0;
    while (at < diff_text.size()) {
      size_t nl = diff_text.find('\n', at);
      size_t end = nl == std::string::npos ? diff_text.size() : nl + 1;
      out << graph->padding_line();
      out.write(diff_text.data() + at, (std::streamsize)(end - at));
      at = end;
    }
  });
  return 0;
}

// ---------------------------------------------------------------------------
// git whatchanged: log with the raw diff of each commit (unless a format is asked for), merges left out unless -m,
// -c or --cc gives them a diff
int cmd_whatchanged(Ctx& c) {
  std::vector<std::string> args(c.args.begin() + 1, c.args.end());
  bool fmt = false, merges = false;
  for (auto& a : args) {
    if (a == "--") break;
    if (a == "-p" || a == "-u" || a == "--patch" || a == "--stat" || a.rfind("--stat=", 0) == 0 || a == "--numstat" ||
        a == "--shortstat" || a == "--name-only" || a == "--name-status" || a == "--raw" || a == "--summary" ||
        a == "-s" || a == "--no-patch" || a.rfind("--dirstat", 0) == 0 || a.rfind("-U", 0) == 0)
      fmt = true;
    if (a == "-m" || a == "-c" || a == "--cc") merges = true;
  }
  std::vector<std::string> next{"log"};
  if (!fmt) next.push_back("--raw");
  next.insert(next.end(), args.begin(), args.end());
  c.hide_merges = true;   // commits without a diff are walked (and counted by -n, as git counts them) but not shown
  (void)merges;
  c.args = next;
  return cmd_log(c);
}

int cmd_show(Ctx& c) {
  c.open_repo(false);
  std::vector<std::string> args(c.args.begin() + 1, c.args.end());
  Pretty pr;
  pr.decorate = default_decorate(c);
  if (auto d = c.config.get("log.date")) pr.date_mode = *d;
  args = take_pretty(c, args, pr);
  DiffOptions dopt;
  bool any_fmt = false;
  args = take_diff_options(c, args, dopt, &any_fmt);
  std::vector<std::string> objs, paths;
  bool after = false;
  for (auto& a : args) {
    if (after) { paths.push_back(a); continue; }
    if (a == "--") { after = true; continue; }
    if (a == "-q" || a == "--quiet") { dopt.format = DiffOptions::Quiet; continue; }
    if (a == "--no-color" || a == "--color=never") continue;
    if (a.size() > 1 && a[0] == '-') usage(c, "unknown option `" + a.substr(a[1] == '-' ? 2 : 1) + "'");
    objs.push_back(a);
  }
  if (objs.empty()) {
    if (!read_ref(c, "HEAD")) die(c, "your current branch '" + current_branch(c) + "' does not have any commits yet");
    objs.push_back("HEAD");
  }
  Pathspec ps = make_pathspec(c, paths);
  bool first = true;
  Decorations deco;
  bool need_deco = pr.decorate || pr.kind == "format" || pr.kind == "tformat";
  if (need_deco) deco = load_decorations(c, pr.decorate_full);
  for (auto& spec : objs) {
    std::string oid = must_resolve(c, spec);
    std::string type = type_of(c, oid);
    if (type == "blob") {
      std::string t = blob_text(c, oid);
      c.out.write(t.data(), (std::streamsize)t.size());
      continue;
    }
    if (type == "tree") {
      c.out << "tree " << spec << "\n\n";
      auto ents = c.repo->read_tree(oid);
      if (ents.ok()) for (auto& e : *ents) c.out << e.name << (e.is_dir() ? "/" : "") << "\n";
      continue;
    }
    if (type == "tag") {
      std::string body = blob_text(c, oid);
      std::string name, tagger, msg;
      size_t hdr_end = body.find("\n\n");
      std::istringstream ss(body.substr(0, hdr_end));
      std::string line;
      std::string obj;
      while (std::getline(ss, line)) {
        if (line.rfind("tag ", 0) == 0) name = line.substr(4);
        else if (line.rfind("tagger ", 0) == 0) tagger = line.substr(7);
        else if (line.rfind("object ", 0) == 0) obj = line.substr(7);
      }
      msg = hdr_end == std::string::npos ? "" : body.substr(hdr_end + 2);
      c.out << "tag " << name << "\n";
      if (!tagger.empty()) {
        size_t gt = tagger.rfind('>');
        std::string who = tagger.substr(0, gt + 1);
        std::string rest = gt + 2 <= tagger.size() ? tagger.substr(gt + 2) : "";
        int64_t t = std::atoll(rest.c_str());
        std::string tz = rest.find(' ') != std::string::npos ? rest.substr(rest.find(' ') + 1) : "+0000";
        c.out << "Tagger: " << who << "\nDate:   " << format_date(t, tz, pr.date_mode) << "\n";
      }
      c.out << "\n" << msg;
      if (!msg.empty() && msg.back() != '\n') c.out << "\n";
      c.out << "\n";
      oid = peel_to(c, oid, "commit");
      if (oid.empty()) continue;
    }
    GitCommit cm = read_commit(c, oid);
    // with paths, a commit that changed none of them is not shown at all (TREESAME to a parent: git prunes it)
    if (!ps.empty()) {
      DiffOptions q;
      q.renames = false;
      auto same = [&](const std::string& from) { return diff_trees(c, from, cm.tree, ps, q).empty(); };
      bool treesame = false;
      if (cm.parents.empty()) treesame = same("");
      else for (auto& par : cm.parents) if (same(read_commit(c, par).tree)) { treesame = true; break; }
      if (treesame) continue;
    }
    std::vector<DiffPair> shown_pairs;
    bool have_pairs = false;
    if ((!dopt.pickaxe_s.empty() || dopt.has_pickaxe_s || !dopt.pickaxe_g.empty() || !dopt.filter.empty()) && dopt.format != DiffOptions::Quiet && cm.parents.size() <= 1) {
      std::string from = cm.parents.empty() ? "" : read_commit(c, cm.parents[0]).tree;
      shown_pairs = diff_trees(c, from, cm.tree, ps, dopt);
      have_pairs = true;
      if (shown_pairs.empty()) continue;
    }
    print_header(c, cm, pr, need_deco ? &deco : nullptr, c.out, first, true);
    if (dopt.format == DiffOptions::Quiet) continue;
    bool user_fmt = pr.kind == "format" || pr.kind == "tformat";
    if (cm.parents.size() > 1 && dopt.format == DiffOptions::Patch && dopt.extra.empty()) {
      // git's combined diff (--cc) of a merge is not produced here; for a clean merge it is empty
      if (pr.kind != "oneline" && !user_fmt) c.out << "\n";
      continue;
    }
    std::string from = cm.parents.empty() ? "" : read_commit(c, cm.parents[0]).tree;
    auto pairs = have_pairs ? shown_pairs : diff_trees(c, from, cm.tree, ps, dopt);
    if (pairs.empty()) continue;
    if (pr.kind != "oneline" && !(user_fmt && pr.fmt.empty())) c.out << (diff_stat_and_patch(dopt) ? "---\n" : "\n");
    print_diff(c, pairs, dopt, c.out);
  }
  return 0;
}

// ---------------------------------------------------------------------------
int cmd_diff(Ctx& c) {
  std::vector<std::string> args(c.args.begin() + 1, c.args.end());
  DiffOptions o;
  if (auto r = std::find(args.begin(), args.end(), "--no-index"); r != args.end()) {
    args.erase(r);
    args = take_diff_options(c, args, o);
    std::vector<std::string> files;
    bool quiet = false;
    for (auto& a : args) {
      if (a == "--") continue;
      if (a == "--quiet") { quiet = true; continue; }
      if (a == "--exit-code") continue;
      files.push_back(a);
    }
    if (files.size() != 2) usage(c, "usage: git diff --no-index [<options>] <path> <path>");
    struct Side { bool ok = false; std::string text; uint32_t mode = 0100644; };
    auto read_side = [&](const std::string& f) {
      Side s;
      if (f == "/dev/null") return s;
      auto st = c.host().stat(c.abs_path(f));
      if (!st.ok()) die(c, "Could not access '" + f + "'");
      if (st->is_dir && !st->is_link) die(c, "aios-git does not implement a directory in diff --no-index");
      s.ok = true;
      s.mode = st->is_link ? 0120000 : st->is_exec ? 0100755 : 0100644;
      if (st->is_link) {
        auto t = c.host().read_symlink(c.abs_path(f));
        s.text = t.ok() ? *t : "";
      } else {
        auto rf = c.host().read_file(c.abs_path(f));
        if (!rf.ok()) die(c, "Could not access '" + f + "'");
        s.text.assign((const char*)rf->data(), rf->size());
      }
      return s;
    };
    Side sa = read_side(files[0]), sb = read_side(files[1]);
    if (!sa.ok && !sb.ok) die(c, "Could not access '" + files[0] + "'");
    if (sa.ok && sb.ok && sa.text == sb.text && sa.mode == sb.mode) return 0;
    if (quiet) return 1;
    DiffPair p;
    p.old_path = slashes(files[0]);
    p.new_path = slashes(files[1]);
    if (!sa.ok) p.old_path = p.new_path;
    if (!sb.ok) p.new_path = p.old_path;
    p.a = FileState{hash_blob(sa.text), sa.mode, sa.ok};
    p.b = FileState{hash_blob(sb.text), sb.mode, sb.ok};
    p.status = !sa.ok ? 'A' : !sb.ok ? 'D' : 'M';
    p.a_text = sa.text;
    p.b_text = sb.text;
    if (o.format == DiffOptions::Quiet) return 1;
    print_diff(c, {p}, o, c.out);
    return 1;
  }
  c.open_repo(false);
  args = take_diff_options(c, args, o);
  bool cached = false, quiet = false, exit_code = false, merge_base = false;
  std::vector<std::string> rest;
  bool dd = false;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (dd) { rest.push_back(a); continue; }
    if (a == "--") { dd = true; rest.push_back(a); continue; }
    if (a == "--cached" || a == "--staged") cached = true;
    else if (a == "--quiet") { quiet = true; exit_code = true; }
    else if (a == "--exit-code") exit_code = true;
    else if (a == "--merge-base") merge_base = true;
    else if (a.size() > 1 && a[0] == '-') usage(c, "invalid option: " + a);
    else rest.push_back(a);
  }
  if (quiet) o.format = DiffOptions::Quiet, o.extra.clear();
  std::vector<std::string> revs, paths;
  split_revs_paths(c, rest, revs, paths);
  // A name that is not a revision but is a file is a path; git asks for "--" when it is both.
  {
    std::vector<std::string> keep;
    for (auto& r : revs) {
      std::string probe = r;
      size_t d3 = probe.find("..."), d2 = probe.find("..");
      bool ok;
      if (d3 != std::string::npos) ok = resolve(c, probe.substr(0, d3).empty() ? "HEAD" : probe.substr(0, d3)).has_value() && resolve(c, probe.substr(d3 + 3).empty() ? "HEAD" : probe.substr(d3 + 3)).has_value();
      else if (d2 != std::string::npos) ok = resolve(c, probe.substr(0, d2).empty() ? "HEAD" : probe.substr(0, d2)).has_value() && resolve(c, probe.substr(d2 + 2).empty() ? "HEAD" : probe.substr(d2 + 2)).has_value();
      else ok = resolve(c, probe).has_value();
      if (ok && keep.size() < 2 && paths.empty()) { keep.push_back(r); continue; }
      if (!c.top.empty() && c.host().exists(c.abs_path(r))) { paths.push_back(r); continue; }
      if (ok) { keep.push_back(r); continue; }
      must_resolve(c, r);
    }
    revs = keep;
  }
  Pathspec ps = make_pathspec(c, paths);
  Snapshot a, b;
  std::map<std::string, std::string> texts;
  bool worktree_side = false;
  std::vector<DiffPair> unmerged_pairs;
  Snapshot old_side_of_unmerged;   // `diff --cached`: what the compared commit has at a path now unmerged
  auto tree_of_rev = [&](const std::string& r) {
    std::string t = peel_to(c, must_resolve(c, r), "tree");
    if (t.empty()) die(c, r + ": not a tree-ish");
    return t;
  };
  // two trees: only the subtrees that differ are read (whole trees for -C -C, which looks among unchanged files)
  auto two_trees = [&](const std::string& ta, const std::string& tb) {
    if (o.copies_harder) { a = snapshot_tree(c, ta); b = snapshot_tree(c, tb); }
    else snapshot_tree_pair(c, ta, tb, a, b);
  };
  if (revs.size() == 1 && (revs[0].find("...") != std::string::npos || revs[0].find("..") != std::string::npos)) {
    std::string r = revs[0];
    size_t d3 = r.find("...");
    if (d3 != std::string::npos) {
      std::string l = r.substr(0, d3), rr = r.substr(d3 + 3);
      std::string lc = commit_of(c, l.empty() ? "HEAD" : l), rc = commit_of(c, rr.empty() ? "HEAD" : rr);
      auto bases = merge_bases(c, lc, rc);
      if (bases.empty()) die(c, l + "..." + rr + ": no merge base");
      two_trees(read_commit(c, bases[0]).tree, read_commit(c, rc).tree);
    } else {
      size_t d2 = r.find("..");
      std::string l = r.substr(0, d2), rr = r.substr(d2 + 2);
      two_trees(tree_of_rev(l.empty() ? "HEAD" : l), tree_of_rev(rr.empty() ? "HEAD" : rr));
    }
  } else if (revs.size() >= 2) {
    if (revs.size() > 2) unsupported(c, "diff of more than two revisions (combined diff)");
    if (merge_base) {
      auto bases = merge_bases(c, commit_of(c, revs[0]), commit_of(c, revs[1]));
      if (bases.empty()) die(c, "no merge base found");
      two_trees(read_commit(c, bases[0]).tree, tree_of_rev(revs[1]));
    } else {
      two_trees(tree_of_rev(revs[0]), tree_of_rev(revs[1]));
    }
  } else {
    if (c.top.empty()) die(c, "this operation must be run in a work tree");
    std::string base_rev = revs.empty() ? "" : revs[0];
    if (cached) {
      std::string rev = base_rev.empty() ? "HEAD" : base_rev;
      if (merge_base) {
        auto bases = merge_bases(c, commit_of(c, rev), commit_of(c, "HEAD"));
        if (bases.empty()) die(c, "no merge base found");
        rev = bases[0];
      }
      auto head = read_ref(c, "HEAD");
      a = (rev == "HEAD" && !head) ? Snapshot{} : snapshot_tree(c, tree_of_rev(rev));
      b = snapshot_index(c);
      if (o.ita_visible) for (auto& e : c.repo->index()) if (e.intent_to_add) b[e.path] = FileState{e.oid, e.mode, true};
      old_side_of_unmerged = a;
      for (auto& u : c.repo->unmerged()) a.erase(u.path);
    } else if (!base_rev.empty()) {
      std::string rev = base_rev;
      if (merge_base) {
        auto bases = merge_bases(c, commit_of(c, rev), commit_of(c, "HEAD"));
        if (bases.empty()) die(c, "no merge base found");
        rev = bases[0];
      }
      a = snapshot_tree(c, tree_of_rev(rev));
      b = snapshot_worktree(c, false, &texts);
      worktree_side = true;
    } else {
      a = snapshot_index(c);
      if (o.ita_visible) for (auto& e : c.repo->index()) if (e.intent_to_add) a[e.path] = FileState{e.oid, e.mode, true};
      b = snapshot_worktree(c, false, &texts);
      worktree_side = true;
      for (auto& u : c.repo->unmerged()) {
        a.erase(u.path);
        b.erase(u.path);
      }
    }
    std::set<std::string> seen;
    for (auto& u : c.repo->unmerged()) {
      if (!seen.insert(u.path).second || !ps.match(u.path)) continue;
      DiffPair p;
      p.status = 'U';
      p.old_path = p.new_path = u.path;
      // the work tree's file, as far as its mode goes, stands for the result
      if (auto st = c.host().stat(join(c.top, u.path)); worktree_side && st.ok() && !st->is_dir)
        p.b = FileState{std::string(40, '0'), st->is_link ? 0120000u : st->is_exec ? 0100755u : 0100644u, true};
      if (auto it = old_side_of_unmerged.find(u.path); it != old_side_of_unmerged.end()) p.a = it->second;
      unmerged_pairs.push_back(p);
      // against the work tree git also compares the file with "ours" (stage 2)
      if (worktree_side && o.format != DiffOptions::Patch) {
        for (auto& st2 : c.repo->unmerged()) {
          if (st2.path != u.path || st2.stage != 2) continue;
          bool ok = false;
          std::string t = read_worktree_file(c, u.path, &ok);
          if (ok && hash_blob(t) != st2.oid) {
            DiffPair m;
            m.status = 'M';
            m.old_path = m.new_path = u.path;
            m.a = FileState{st2.oid, st2.mode, true};
            m.b = FileState{hash_blob(t), st2.mode, true};
            m.b_text = t;
            unmerged_pairs.push_back(m);
          }
        }
      }
    }
  }
  auto pairs = diff_snapshots(c, a, b, ps, o, worktree_side ? &texts : nullptr);
  bool has_upper = false;
  for (char ch : o.filter) if (std::isupper((unsigned char)ch)) has_upper = true;
  auto passes = [&](char st) {
    if (o.filter.empty()) return true;
    if (o.filter.find((char)std::tolower((unsigned char)st)) != std::string::npos) return false;
    return !has_upper || o.filter.find(st) != std::string::npos;
  };
  if (!unmerged_pairs.empty()) {
    for (auto& u : unmerged_pairs) if (passes(u.status)) pairs.push_back(u);
    std::stable_sort(pairs.begin(), pairs.end(), [](const DiffPair& x, const DiffPair& y) { return x.new_path < y.new_path; });
  }
  bool any = print_diff(c, pairs, o, c.out);
  if (c.diff_check_failed) return 2;
  if (exit_code) return any ? 1 : 0;
  return 0;
}

// ---------------------------------------------------------------------------
int cmd_rev_list(Ctx& c) {
  c.open_repo(false);
  std::vector<std::string> args(c.args.begin() + 1, c.args.end());
  // --objects and its company: the trees and blobs of the listed commits, after them (list-objects.c)
  bool objects = false, objects_edge = false, in_commit_order = false, object_names = true;
  {
    std::vector<std::string> kept;
    bool after_dd = false;
    for (auto& a : args) {
      if (!after_dd && a == "--") after_dd = true;
      if (!after_dd && a == "--objects") { objects = true; continue; }
      if (!after_dd && (a == "--objects-edge" || a == "--objects-edge-aggressive")) { objects = objects_edge = true; continue; }
      if (!after_dd && a == "--in-commit-order") { in_commit_order = true; continue; }
      if (!after_dd && a == "--no-object-names") { object_names = false; continue; }
      if (!after_dd && a == "--object-names") { object_names = true; continue; }
      kept.push_back(a);
    }
    args = std::move(kept);
  }
  WalkOptions w;
  HistoryFlags hf;
  args = take_walk_options(c, args, w, hf);
  if (hf.graph || hf.follow || hf.has_s || hf.has_g) unsupported(c, "rev-list --graph, --follow, -S and -G");
  bool count = false, header = false, timestamp = false;
  std::vector<std::string> extra;
  Pretty pr;
  bool want_pretty = false;
  for (auto& a : args) {
    if (a == "--count") { count = true; continue; }
    if (a == "--header") { header = true; continue; }
    if (a == "--timestamp") { timestamp = true; continue; }
    if (a == "-z") { pr.term = '\0'; continue; }
    if (a.rfind("--abbrev=", 0) == 0) { pr.abbrev = std::max(4, std::min(40, std::atoi(a.c_str() + 9))); continue; }
    if (a.rfind("--pretty", 0) == 0 || a.rfind("--format", 0) == 0 || a == "--oneline") {
      std::vector<std::string> one{a};
      take_pretty(c, one, pr);
      want_pretty = true;
      continue;
    }
    if (a == "--abbrev-commit") { pr.abbrev_commit = true; continue; }
    if (a == "--quiet") continue;
    extra.push_back(a);
  }
  std::vector<std::string> revs, paths;
  bool dd = std::find(extra.begin(), extra.end(), "--") != extra.end();
  split_revs_paths(c, extra, revs, paths);
  check_paths_or_revs(c, revs, paths, dd);
  auto rest = parse_revisions(c, revs, w, false);
  if (!rest.empty()) usage(c, "unrecognized argument: " + rest[0]);
  if (w.tips.empty() && revs.empty()) usage(c, "usage: git rev-list [<options>] <commit>... [--] [<path>...]");
  w.paths = make_pathspec(c, paths);
  if (hf.left_only || hf.right_only) {
    w.post_filter = [&](const GitCommit& cm) { return hf.left_only ? w.left.count(cm.oid) > 0 : w.left.count(cm.oid) == 0; };
  }
  if (objects && !w.paths.empty()) unsupported(c, "rev-list --objects with paths");
  long long limit = w.max_count;
  if (count && !objects) w.max_count = -1;
  w.ids_only = count || (!want_pretty && !header);
  std::unordered_set<std::string> hidden;
  if (objects) w.hidden_out = &hidden;
  // The objects pass: tags named on the command line (each of a chain, by its own name), then each listed commit's
  // tree, walked depth first in entry order; a tree or blob that an edge parent's tree holds is left out
  // (mark_edges_uninteresting), and nothing is listed twice.
  std::vector<std::pair<std::string, std::string>> pending;   // (oid, name or path)
  std::unordered_set<std::string> seen, dull;
  auto tree_entries = [&](const std::string& oid) {
    auto t = c.repo->read_tree(oid);
    return t.ok() ? *t : std::vector<GitTreeEntry>{};
  };
  std::function<void(const std::string&)> mark_dull = [&](const std::string& tree) {
    if (tree.empty() || !dull.insert(tree).second) return;
    for (auto& e : tree_entries(tree)) {
      if (e.is_dir()) mark_dull(e.oid);
      else if (e.mode != 0160000) dull.insert(e.oid);
    }
  };
  long long object_count = 0;
  auto show_object = [&](const std::string& oid, const std::string& name) {
    ++object_count;
    if (count) return;
    if (!object_names) { c.out << oid << "\n"; return; }
    c.out << oid << " " << name.substr(0, name.find('\n')) << "\n";
  };
  std::function<void(const std::string&, std::string&, const std::string&)> process_tree =
      [&](const std::string& tree, std::string& base, const std::string& name) {
        if (dull.count(tree) || !seen.insert(tree).second) return;
        const size_t baselen = base.size();
        base += name;
        show_object(tree, base);
        if (!base.empty()) base += '/';
        for (auto& e : tree_entries(tree)) {
          if (e.is_dir()) process_tree(e.oid, base, e.name);
          else if (e.mode == 0160000) continue;   // a submodule's commit is not in this repository
          else if (!dull.count(e.oid) && seen.insert(e.oid).second) show_object(e.oid, base + e.name);
        }
        base.resize(baselen);
      };
  auto traverse_pending = [&]() {
    for (auto& [oid, name] : pending) {
      if (dull.count(oid) || seen.count(oid)) continue;
      const std::string type = type_of(c, oid);
      std::string base;
      if (type == "tag") { seen.insert(oid); show_object(oid, name); }
      else if (type == "tree") process_tree(oid, base, name);
      else if (type == "blob" && seen.insert(oid).second) show_object(oid, name);
    }
    pending.clear();
  };
  if (objects) {
    // the named tags, as handle_commit queues them: positive revisions only
    bool negate = false;
    auto queue_named = [&](const std::string& oid) {
      std::string cur = oid;
      for (int guard = 0; guard < 16 && type_of(c, cur) == "tag"; ++guard) {
        TagInfo ti;
        if (!read_tag_object(c, cur, ti) || ti.target.empty()) break;
        pending.push_back({cur, ti.name});
        cur = ti.target;
      }
      const std::string t = type_of(c, cur);
      if (t == "tree" || t == "blob") pending.push_back({cur, ""});
    };
    auto queue_refs = [&](const char* prefix) {
      for (auto& [ref, oid] : all_refs(c)) if (ref.rfind(prefix, 0) == 0) queue_named(oid);
    };
    for (auto& r : revs) {
      if (r == "--not") { negate = !negate; continue; }
      if (negate) continue;
      if (r == "--all") { queue_refs("refs/"); if (auto h = read_ref(c, "HEAD")) queue_named(*h); continue; }
      if (r == "--tags") { queue_refs("refs/tags/"); continue; }
      if (r == "--branches") { queue_refs("refs/heads/"); continue; }
      if (r == "--remotes") { queue_refs("refs/remotes/"); continue; }
      if (r.empty() || r[0] == '-' || r[0] == '^') continue;
      std::string spec = r;
      if (size_t d3 = spec.find("..."); d3 != std::string::npos) {
        if (d3 > 0) if (auto o = resolve(c, spec.substr(0, d3))) queue_named(*o);
        spec = spec.substr(d3 + 3);
      } else if (size_t d2 = spec.find(".."); d2 != std::string::npos) {
        spec = spec.substr(d2 + 2);
      }
      if (spec.empty()) spec = "HEAD";
      if (auto o = resolve(c, spec)) queue_named(*o);
    }
  }
  if (objects) {
    std::vector<GitCommit> listed;
    walk_each(c, w, [&](const GitCommit& cm) { listed.push_back(cm); });
    std::unordered_set<std::string> shown_edges;
    for (auto& cm : listed)
      for (auto& p : cm.parents) {
        if (!hidden.count(p)) continue;
        if (auto pc = c.repo->read_commit(p); pc.ok()) mark_dull(pc->tree);
        if (objects_edge && !count && shown_edges.insert(p).second) c.out << "-" << p << "\n";
      }
    std::vector<std::string> trees;
    long long commits = 0;
    for (auto& cm : listed) {
      ++commits;
      pending.push_back({cm.tree, ""});
      if (!count) c.out << (pr.abbrev_commit ? short_id(c, pr, cm.oid) : cm.oid) << "\n";
      if (in_commit_order) traverse_pending();
    }
    traverse_pending();
    if (count) c.out << commits + object_count << "\n";
    return 0;
  }
  if (count) {
    long long n = 0, right = 0, same = 0;
    walk_each(c, w, [&](const GitCommit& cm) {
      if (w.patchsame.count(cm.oid)) ++same;
      else if (w.symmetric && w.left.count(cm.oid)) ++n;
      else if (hf.left_right) ++right;
      else ++n;
    });
    if (limit >= 0 && n > limit) n = limit;
    if (hf.left_right) {
      c.out << n << "\t" << right;
      if (w.cherry_mark) c.out << "\t" << same;
      c.out << "\n";
    } else {
      c.out << n + same << "\n";
    }
    return 0;
  }
  bool first = true;
  // each commit is printed as the walk reaches it, so a reader that goes away stops the walk
  walk_each(c, w, [&](const GitCommit& cm) {
    std::string mark = revision_mark(w, hf, cm.oid);
    std::string id = (pr.abbrev_commit ? short_id(c, pr, cm.oid) : cm.oid);
    std::string tail;
    if (hf.parents) for (auto& p : cm.parents) tail += " " + p;
    if (hf.children) {
      auto ch = w.children.find(cm.oid);
      if (ch != w.children.end()) for (auto& k : ch->second) tail += " " + k;
    }
    if (header) {
      c.out << (timestamp ? std::to_string(cm.committer_time) + " " : std::string()) << mark << id << tail << "\n";
      c.out << "tree " << cm.tree << "\n";
      for (auto& p : cm.parents) c.out << "parent " << p << "\n";
      c.out << "author " << cm.author << " " << cm.author_time << " " << cm.author_tz << "\n";
      c.out << "committer " << cm.committer << " " << cm.committer_time << " " << cm.committer_tz << "\n";
      for (auto& [k, v] : cm.extra_headers) {
        std::string folded = v;
        for (size_t at = folded.find('\n'); at != std::string::npos; at = folded.find('\n', at + 2)) folded.insert(at + 1, " ");
        c.out << k << " " << folded << "\n";
      }
      std::string body = indent_message(cm.message, "    ");
      if (!body.empty()) c.out << "\n" << body;
      c.out << '\0';
      return;
    }
    if (want_pretty) {
      if (pr.kind == "oneline") { c.out << (timestamp ? std::to_string(cm.committer_time) + " " : std::string()) << mark << id << tail << " " << subject_of(cm.message) << "\n"; return; }
      c.out << "commit " << mark << id << tail << "\n";
      pr.mark.clear();
      pr.after_id.clear();
      if (pr.kind == "format" || pr.kind == "tformat") c.out << expand_format(c, pr.fmt, cm, pr, nullptr) << pr.term;
      else print_header(c, cm, pr, nullptr, c.out, first, false);
      return;
    }
    c.out << (timestamp ? std::to_string(cm.committer_time) + " " : std::string()) << mark << id << tail << "\n";
  });
  return 0;
}

// ---------------------------------------------------------------------------
int cmd_rev_parse(Ctx& c) {
  std::vector<std::string> args(c.args.begin() + 1, c.args.end());
  bool verify = false, quiet = false, short_mode = false, abbrev_ref = false, symbolic_full = false, symbolic = false;
  int short_len = 0;
  bool after_dd = false;
  // flags that need no repository
  for (auto& a : args) {
    if (a == "--local-env-vars") {
      for (const char* v : {"GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_CONFIG", "GIT_CONFIG_PARAMETERS", "GIT_CONFIG_COUNT",
                            "GIT_OBJECT_DIRECTORY", "GIT_DIR", "GIT_WORK_TREE", "GIT_IMPLICIT_WORK_TREE", "GIT_GRAFT_FILE",
                            "GIT_INDEX_FILE", "GIT_NO_REPLACE_OBJECTS", "GIT_REPLACE_REF_BASE", "GIT_PREFIX", "GIT_INTERNAL_SUPER_PREFIX",
                            "GIT_SHALLOW_FILE", "GIT_COMMON_DIR"})
        c.out << v << "\n";
      return 0;
    }
    if (a == "--sq-quote") unsupported(c, "rev-parse --sq-quote");
    if (a == "--parseopt") unsupported(c, "rev-parse --parseopt");
  }
  if (!c.try_open_repo()) {
    for (auto& a : args) {
      if (a == "--is-inside-work-tree" || a == "--is-inside-git-dir" || a == "--is-bare-repository" || a == "--git-dir" ||
          a == "--show-toplevel" || a == "--show-prefix" || a == "--show-cdup" || a == "--absolute-git-dir" ||
          a == "--git-common-dir" || a == "--is-shallow-repository" || a.rfind("--", 0) != 0)
        die(c, "not a git repository (or any of the parent directories): .git");
    }
    return 0;
  }
  std::vector<std::string> outputs;
  int revs_seen = 0;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (after_dd) { outputs.push_back(a); continue; }
    if (a == "--") { after_dd = true; if (!verify) outputs.push_back("--"); continue; }
    if (a == "--verify") { verify = true; continue; }
    if (a == "-q" || a == "--quiet") { quiet = true; continue; }
    if (a == "--short") { short_mode = true; short_len = 0; continue; }
    if (a.rfind("--short=", 0) == 0) { short_mode = true; short_len = std::atoi(a.c_str() + 8); if (short_len < 4) short_len = 4; continue; }
    if (a == "--abbrev-ref" || a.rfind("--abbrev-ref=", 0) == 0) { abbrev_ref = true; continue; }
    if (a == "--symbolic-full-name") { symbolic_full = true; continue; }
    if (a == "--symbolic") { symbolic = true; continue; }
    if (a == "--no-flags" || a == "--revs-only" || a == "--no-revs" || a == "--flags") continue;
    if (a == "--show-toplevel") {
      if (c.top.empty()) die(c, "this operation must be run in a work tree");
      outputs.push_back(c.top);
      continue;
    }
    if (a == "--show-prefix") { outputs.push_back(c.prefix); continue; }
    if (a == "--show-cdup") {
      std::string s;
      for (char ch : c.prefix) if (ch == '/') s += "../";
      outputs.push_back(s);
      continue;
    }
    if (a == "--git-dir") {
      std::string gd = c.repo->git_dir();
      if (!c.top.empty() && gd == c.top + "/.git" && c.cwd == c.top) outputs.push_back(".git");
      else if (gd == c.cwd) outputs.push_back(".");
      else outputs.push_back(gd);
      continue;
    }
    if (a == "--absolute-git-dir") { outputs.push_back(c.repo->git_dir()); continue; }
    if (a == "--git-common-dir") {
      std::string cd = c.repo->common_dir();
      if (!c.top.empty() && cd == c.top + "/.git" && c.cwd == c.top) outputs.push_back(".git");
      else outputs.push_back(cd);
      continue;
    }
    if (a == "--is-inside-work-tree") { outputs.push_back(c.top.empty() ? "false" : "true"); continue; }
    if (a == "--is-inside-git-dir") {
      std::string gd = c.repo->git_dir();
      outputs.push_back(c.cwd == gd || c.cwd.rfind(gd + "/", 0) == 0 ? "true" : "false");
      continue;
    }
    if (a == "--is-bare-repository") { outputs.push_back(c.repo->bare() ? "true" : "false"); continue; }
    if (a == "--is-shallow-repository") { outputs.push_back(c.host().exists(c.repo->common_dir() + "/shallow") ? "true" : "false"); continue; }
    if (a == "--show-object-format" || a == "--show-object-format=storage") { outputs.push_back("sha1"); continue; }
    if (a == "--git-path" && i + 1 < args.size()) {
      std::string rel = args[++i];
      bool common = rel.rfind("refs", 0) == 0 || rel.rfind("objects", 0) == 0 || rel == "config" || rel.rfind("hooks", 0) == 0 ||
                    rel == "packed-refs" || rel.rfind("info", 0) == 0;
      std::string base = common ? c.repo->common_dir() : c.repo->git_dir();
      if (!c.top.empty() && base == c.top + "/.git" && c.cwd == c.top) base = ".git";
      outputs.push_back(base + "/" + rel);
      continue;
    }
    if (a == "--default" && i + 1 < args.size()) { ++i; continue; }
    if (a == "--all" || a == "--branches" || a == "--tags" || a == "--remotes") {
      std::string pre = a == "--all" ? "refs/" : a == "--branches" ? "refs/heads/" : a == "--tags" ? "refs/tags/" : "refs/remotes/";
      for (auto& [n, o] : all_refs(c)) if (n.rfind(pre, 0) == 0) outputs.push_back(o);
      continue;
    }
    if (a.size() > 1 && a[0] == '-' && !verify) {
      // unknown flags are passed through, as rev-parse does
      outputs.push_back(a);
      continue;
    }
    // a revision (or range)
    ++revs_seen;
    if (verify && revs_seen > 1) {
      if (quiet) return 1;
      die(c, "Needed a single revision");
    }
    std::string spec = a;
    if (!verify) {
      size_t d3 = spec.find("..."), d2 = spec.find("..");
      if (d3 != std::string::npos || d2 != std::string::npos) {
        bool three = d3 != std::string::npos;
        size_t at = three ? d3 : d2;
        std::string l = spec.substr(0, at), r = spec.substr(at + (three ? 3 : 2));
        auto lo = resolve(c, l.empty() ? "HEAD" : l), ro = resolve(c, r.empty() ? "HEAD" : r);
        if (lo && ro) {
          outputs.push_back(*ro);
          if (three) {
            outputs.push_back(*lo);
            for (auto& b : merge_bases(c, peel_to(c, *lo, "commit"), peel_to(c, *ro, "commit"))) outputs.push_back("^" + b);
          } else {
            outputs.push_back("^" + *lo);
          }
          continue;
        }
      }
      if (spec.size() > 1 && spec[0] == '^') {
        if (auto o = resolve(c, spec.substr(1))) { outputs.push_back("^" + *o); continue; }
      }
    }
    std::string why;
    auto o = resolve(c, spec, &why);
    if (!o) {
      if (verify) {
        if (quiet) return 1;
        die(c, "Needed a single revision");
      }
      // not a revision: a path that exists is echoed back, anything else is an error
      if (c.host().exists(c.abs_path(spec))) { outputs.push_back(spec); continue; }
      c.out << spec << "\n";
      c.err << "fatal: ambiguous argument '" << spec << "': unknown revision or path not in the working tree.\n"
               "Use '--' to separate paths from revisions, like this:\n'git <command> [<revision>...] -- [<file>...]'\n";
      return 128;
    }
    if (abbrev_ref || symbolic_full || symbolic) {
      // the name, not the id
      std::string base = spec;
      std::string full;
      if (spec == "HEAD" || spec == "@") {
        full = c.repo->head_ref();
        if (full.empty()) { outputs.push_back("HEAD"); continue; }
      } else if (spec.find("@{u") != std::string::npos || spec.find("@{upstream}") != std::string::npos || spec.find("@{push}") != std::string::npos) {
        std::string b = spec.substr(0, spec.find("@{"));
        if (b.empty() || b == "HEAD") b = current_branch(c);
        else { std::string f = dwim_ref(c, b); b = f.rfind("refs/heads/", 0) == 0 ? f.substr(11) : b; }
        full = upstream_of(c, b);
        if (full.empty()) die(c, "no upstream configured for branch '" + b + "'");
      } else {
        full = dwim_ref(c, spec);
      }
      if (full.empty()) {
        if (symbolic) { outputs.push_back(spec); continue; }
        if (abbrev_ref) { outputs.push_back(spec); continue; }
        continue;   // --symbolic-full-name of a plain object id prints nothing
      }
      if (abbrev_ref) {
        std::string s = full;
        for (const char* pre : {"refs/heads/", "refs/tags/", "refs/remotes/", "refs/"})
          if (s.rfind(pre, 0) == 0) { s = s.substr(std::strlen(pre)); break; }
        outputs.push_back(s);
      } else if (symbolic_full) {
        outputs.push_back(full);
      } else {
        outputs.push_back(spec);
      }
      continue;
    }
    outputs.push_back(short_mode ? c.abbrev(*o, short_len) : *o);
  }
  if (verify && revs_seen == 0) {
    if (quiet) return 1;
    die(c, "Needed a single revision");
  }
  for (auto& o : outputs) c.out << o << "\n";
  return 0;
}

// ---------------------------------------------------------------------------
int cmd_merge_base(Ctx& c) {
  auto p = parse_options(c, c.args, 1, {{'a', "all"}, {0, "is-ancestor"}, {0, "octopus"}, {0, "fork-point"}, {0, "independent"}});
  c.open_repo(false);
  if (p.has("octopus") || p.has("fork-point") || p.has("independent")) unsupported(c, "merge-base --octopus/--fork-point/--independent");
  if (p.has("is-ancestor")) {
    if (p.pos.size() != 2) usage(c, "--is-ancestor takes exactly two commits");
    return is_ancestor(c, commit_of(c, p.pos[0]), commit_of(c, p.pos[1])) ? 0 : 1;
  }
  if (p.pos.size() < 2) usage(c, "usage: git merge-base [-a | --all] <commit> <commit>...");
  if (p.pos.size() > 2) unsupported(c, "merge-base of more than two commits");
  auto bases = merge_bases(c, commit_of(c, p.pos[0]), commit_of(c, p.pos[1]));
  if (bases.empty()) return 1;
  if (p.has("all")) for (auto& b : bases) c.out << b << "\n";
  else c.out << bases[0] << "\n";
  return 0;
}

}  // namespace aios_git
