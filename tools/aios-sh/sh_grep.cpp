// grep, and the POSIX-regex translation grep and sed share. POSIX basic and extended expressions are
// rewritten into std::regex's ECMAScript grammar (which also gives \d \w \s \b, as GNU grep's do);
// a fixed string, or a pattern with no special characters at all, is matched by plain search.
#include "sh.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <functional>
#include <regex>
#include <string_view>

namespace aiossh {

std::string posix_regex_to_ecma(const std::string& re, bool extended) {
  std::string o;
  bool at_start = true;   // a '*' here is literal in a basic expression
  for (size_t i = 0; i < re.size(); ++i) {
    char c = re[i];
    if (c == '[') {
      // a bracket expression: copied, with '\' (literal in POSIX) escaped and a leading ']' kept literal
      size_t j = i + 1;
      std::string b = "[";
      if (j < re.size() && re[j] == '^') { b += '^'; ++j; }
      if (j < re.size() && re[j] == ']') { b += "\\]"; ++j; }
      bool closed = false;
      for (; j < re.size(); ++j) {
        if (re[j] == '[' && j + 1 < re.size() && (re[j + 1] == ':' || re[j + 1] == '.' || re[j + 1] == '=')) {
          size_t e = re.find(std::string(1, re[j + 1]) + "]", j + 2);
          if (e != std::string::npos) { b += re.substr(j, e + 2 - j); j = e + 1; continue; }
        }
        if (re[j] == ']') { closed = true; break; }
        if (re[j] == '\\') { b += "\\\\"; continue; }
        b += re[j];
      }
      if (!closed) { o += "\\["; at_start = false; continue; }
      o += b + "]";
      i = j;
      at_start = false;
      continue;
    }
    if (c == '\\' && i + 1 < re.size()) {
      char n = re[++i];
      if (!extended && std::strchr("(){}|+?", n)) { o += n; at_start = n == '(' || n == '|'; continue; }
      if (n == '<' || n == '>') { o += "\\b"; continue; }
      if (n == '`') { o += "^"; continue; }
      if (n == '\'') { o += "$"; continue; }
      if (std::strchr("wWsSbBdD", n)) { o += '\\'; o += n; at_start = false; continue; }
      if (std::isdigit(static_cast<unsigned char>(n))) { o += '\\'; o += n; at_start = false; continue; }
      if (n == 'n') { o += "\\n"; continue; }
      if (n == 't') { o += "\\t"; continue; }
      // any other escaped char is that char, literally
      if (std::strchr(".[]{}()*+?^$|\\/", n)) { if (n == '/') o += '/'; else { o += '\\'; o += n; } }
      else o += n;
      at_start = false;
      continue;
    }
    if (!extended && std::strchr("(){}|+?", c)) { o += '\\'; o += c; at_start = false; continue; }
    if (c == '*' && at_start) { o += "\\*"; at_start = false; continue; }
    if (extended && c == '{' && (i + 1 >= re.size() || !std::isdigit(static_cast<unsigned char>(re[i + 1])))) { o += "\\{"; at_start = false; continue; }
    if (c == '/') { o += '/'; at_start = false; continue; }
    o += c;
    at_start = c == '^' || (extended && (c == '(' || c == '|'));
  }
  return o;
}

namespace {

bool plain_text(const std::string& s) {
  for (char c : s) if (std::strchr(".[]{}()*+?^$|\\", c)) return false;
  return true;
}

std::string lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

struct Matcher {
  std::vector<std::string> fixed;       // plain search, one per pattern
  std::vector<std::regex> res;
  bool icase = false, whole_word = false, whole_line = false, use_regex = false;

  static bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

  // Each match in line: [start, end). Stops after the first unless all.
  void matches(const std::string& line, std::vector<std::pair<size_t, size_t>>& out, bool all) const {
    if (!use_regex) {
      std::string hay = icase ? lower(line) : line;
      for (const auto& f : fixed) {
        size_t from = 0;
        while (from <= hay.size()) {
          size_t at = f.empty() ? from : hay.find(f, from);
          if (at == std::string::npos) break;
          size_t end = at + f.size();
          bool ok = true;
          if (whole_line) ok = at == 0 && end == hay.size();
          if (ok && whole_word) ok = (at == 0 || !word_char(hay[at - 1])) && (end >= hay.size() || !word_char(hay[end]));
          if (ok) { out.push_back({at, end}); if (!all) return; }
          from = (f.empty() ? at + 1 : at + 1);
          if (f.empty() && !all) return;
        }
      }
      return;
    }
    for (const auto& re : res) {
      auto begin = std::sregex_iterator(line.begin(), line.end(), re);
      for (auto it = begin; it != std::sregex_iterator(); ++it) {
        size_t at = static_cast<size_t>(it->position()), len = static_cast<size_t>(it->length());
        out.push_back({at, at + len});
        if (!all) return;
        if (len == 0 && at >= line.size()) break;
      }
    }
  }
  bool any(const std::string& line) const { std::vector<std::pair<size_t, size_t>> m; matches(line, m, false); return !m.empty(); }
};

}  // namespace

int util_grep(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  bool extended = argv[0] == "egrep", fixed = argv[0] == "fgrep", perl = false;
  bool icase = false, invert = false, number = false, count = false, list = false, list_not = false, recursive = false;
  bool only = false, quiet = false, no_messages = false, word = false, line_x = false, with_name = false, no_name = false;
  bool skip_binary = false, text = false, null_after_name = false;
  long long max_count = -1, after = 0, before = 0;
  std::vector<std::string> patterns, files, includes, excludes, exclude_dirs;
  bool patterns_given = false;
  size_t i = 1;
  auto take_val = [&](const std::string& a, size_t& k, const std::string& name) -> std::string {
    if (a.size() > name.size() && a.compare(0, name.size(), name) == 0) return a.substr(name.size());
    return k + 1 < argv.size() ? argv[++k] : std::string();
  };
  for (; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (a == "--") { ++i; break; }
    if (a.size() < 2 || a[0] != '-') { if (!patterns_given) { patterns.push_back(a); patterns_given = true; } else files.push_back(a); continue; }
    if (a.rfind("--", 0) == 0) {
      size_t eq = a.find('=');
      std::string name = a.substr(2, eq == std::string::npos ? std::string::npos : eq - 2);
      std::string val = eq == std::string::npos ? "" : a.substr(eq + 1);
      auto value = [&]() { return eq != std::string::npos ? val : (i + 1 < argv.size() ? argv[++i] : std::string()); };
      if (name == "include") includes.push_back(value());
      else if (name == "exclude") excludes.push_back(value());
      else if (name == "exclude-dir") exclude_dirs.push_back(value());
      else if (name == "ignore-case") icase = true;
      else if (name == "invert-match") invert = true;
      else if (name == "line-number") number = true;
      else if (name == "count") count = true;
      else if (name == "files-with-matches") list = true;
      else if (name == "files-without-match") list_not = true;
      else if (name == "recursive" || name == "dereference-recursive") recursive = true;
      else if (name == "only-matching") only = true;
      else if (name == "quiet" || name == "silent") quiet = true;
      else if (name == "no-messages") no_messages = true;
      else if (name == "word-regexp") word = true;
      else if (name == "line-regexp") line_x = true;
      else if (name == "extended-regexp") extended = true;
      else if (name == "fixed-strings") fixed = true;
      else if (name == "perl-regexp") perl = true;
      else if (name == "with-filename") with_name = true;
      else if (name == "no-filename") no_name = true;
      else if (name == "max-count") parse_int(value(), max_count);
      else if (name == "regexp") { patterns.push_back(value()); patterns_given = true; }
      else if (name == "after-context") parse_int(value(), after);
      else if (name == "before-context") parse_int(value(), before);
      else if (name == "context") { parse_int(value(), after); before = after; }
      else if (name == "binary-files") { if (value() == "without-match") skip_binary = true; }
      else if (name == "text") text = true;
      else if (name == "null") null_after_name = true;
      // --color, --colour, --line-buffered, --label: accepted
      continue;
    }
    for (size_t k = 1; k < a.size(); ++k) {
      char c = a[k];
      switch (c) {
        case 'i': case 'y': icase = true; break;
        case 'v': invert = true; break;
        case 'n': number = true; break;
        case 'c': count = true; break;
        case 'l': list = true; break;
        case 'L': list_not = true; break;
        case 'r': case 'R': recursive = true; break;
        case 'o': only = true; break;
        case 'q': quiet = true; break;
        case 's': no_messages = true; break;
        case 'w': word = true; break;
        case 'x': line_x = true; break;
        case 'E': extended = true; break;
        case 'F': fixed = true; break;
        case 'G': extended = false; break;
        case 'P': perl = true; break;
        case 'H': with_name = true; break;
        case 'h': no_name = true; break;
        case 'I': skip_binary = true; break;
        case 'a': text = true; break;
        case 'Z': null_after_name = true; break;
        case 'U': case 'b': case 'u': break;
        case 'e': case 'f': case 'm': case 'A': case 'B': case 'C': {
          std::string rest = a.substr(k + 1);
          std::string v = !rest.empty() ? rest : (i + 1 < argv.size() ? argv[++i] : std::string());
          if (c == 'e') { patterns.push_back(v); patterns_given = true; }
          else if (c == 'f') { std::string d; os::read_whole(host_path(st.cwd, v), d, nullptr); for (auto& l : split_lines(d)) { if (!l.empty() && l.back() == '\r') l.pop_back(); patterns.push_back(l); } patterns_given = true; }
          else if (c == 'm') parse_int(v, max_count);
          else if (c == 'A') parse_int(v, after);
          else if (c == 'B') parse_int(v, before);
          else { parse_int(v, after); before = after; }
          k = a.size();
          break;
        }
        default:
          if (std::isdigit(static_cast<unsigned char>(c))) { long long n = 0; parse_int(a.substr(k), n); after = before = n; k = a.size(); break; }
          io.ewrite("grep: invalid option -- '" + std::string(1, c) + "'\n");
          return 2;
      }
    }
    (void)take_val;
  }
  for (; i < argv.size(); ++i) { if (!patterns_given) { patterns.push_back(argv[i]); patterns_given = true; } else files.push_back(argv[i]); }
  if (!patterns_given) { io.ewrite("Usage: grep [OPTION]... PATTERNS [FILE]...\n"); return 2; }
  // one pattern argument may hold several, one per line
  std::vector<std::string> pats;
  for (const auto& p : patterns) { auto ls = split_lines(p); if (ls.empty()) pats.push_back(""); for (auto& l : ls) pats.push_back(l); }

  Matcher m;
  m.icase = icase;
  m.whole_word = word;
  m.whole_line = line_x;
  bool all_plain = fixed;
  if (!fixed && !perl) { all_plain = true; for (const auto& p : pats) all_plain = all_plain && plain_text(p); }
  if (all_plain) {
    for (const auto& p : pats) m.fixed.push_back(icase ? lower(p) : p);
  } else {
    m.use_regex = true;
    try {
      for (const auto& p : pats) {
        std::string e = perl ? p : posix_regex_to_ecma(p, extended);
        if (word) e = "\\b(?:" + e + ")\\b";
        if (line_x) e = "^(?:" + e + ")$";
        auto flags = std::regex::ECMAScript | std::regex::optimize;
        if (icase) flags |= std::regex::icase;
        m.res.emplace_back(e, flags);
      }
    } catch (const std::regex_error& e) {
      io.ewrite(std::string("grep: invalid regular expression: ") + e.what() + "\n");
      return 2;
    }
  }

  // with no operand, -r searches "." and names files without a "./" (as GNU grep does); an explicit "." keeps it
  bool implicit_dot = files.empty() && recursive;
  if (files.empty()) files.push_back(recursive ? "." : "-");
  bool show_name = !no_name && (with_name || recursive || files.size() > 1);
  int status = 1;
  bool error = false;
  bool stop_all = false;

  // Input is read a block at a time and searched as it comes: `git log | grep x | head -1` and `grep -q`/-m/-l
  // stop reading once they have their answer (the producer then sees a closed pipe), and a big input is never
  // held whole. Matched lines go out when a block is used up, so a slow producer's hits are not held back.
  struct Lines {
    explicit Lines(Fd fd) : f(fd) {}
    Fd f;
    std::string buf;
    size_t pos = 0;
    bool eof = false;
    std::function<void()> before_read;
    bool fill() {
      if (eof) return false;
      if (before_read) before_read();
      char b[1 << 16];
      long n = os::read_fd(f, b, sizeof b);
      if (n <= 0) { eof = true; return false; }
      buf.erase(0, pos);
      pos = 0;
      buf.append(b, static_cast<size_t>(n));
      return true;
    }
    // the first n bytes, read ahead without consuming them (the binary check looks at the first 8000)
    std::string_view peek(size_t n) { while (buf.size() - pos < n && fill()) {} return std::string_view(buf).substr(pos, n); }
    bool next(std::string& line) {
      size_t from = pos;
      for (;;) {
        size_t nl = buf.find('\n', from);
        if (nl != std::string::npos) { line.assign(buf, pos, nl - pos); pos = nl + 1; return true; }
        from = buf.size() - pos;   // fill() moves the unread part to the front: search on from its end
        if (!fill()) {
          if (pos >= buf.size()) return false;
          line.assign(buf, pos, std::string::npos);
          pos = buf.size();
          return true;
        }
      }
    }
  };

  auto search = [&](const std::string& name, Lines& in) {
    if (stop_all) return;
    std::string_view head8k = in.peek(8000);
    bool binary = !text && std::memchr(head8k.data(), 0, head8k.size()) != nullptr;
    if (binary && skip_binary) return;
    std::string prefix_name = show_name ? name : "";
    std::string sep_name = null_after_name ? std::string(1, '\0') : ":";
    long long found = 0;
    std::string out;
    bool listing = count || list || list_not || quiet || binary;
    if (!listing) in.before_read = [&]() { if (!out.empty()) { io.write(out); out.clear(); } };
    std::deque<std::string> recent;   // the last `before` lines, for before-context
    long long last_printed = -1, pending_after = 0;
    bool any_printed = false;
    std::string line;
    for (long long ln = 0; in.next(line); ++ln) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      std::vector<std::pair<size_t, size_t>> hits;
      m.matches(line, hits, only);
      bool hit = !hits.empty();
      if (invert) hit = !hit;
      if (!hit) {
        if (pending_after > 0 && !listing) {
          out += (show_name ? prefix_name + "-" : "") + (number ? std::to_string(ln + 1) + "-" : "") + line + "\n";
          last_printed = ln;
          --pending_after;
        }
        if (before > 0) { recent.push_back(line); if (static_cast<long long>(recent.size()) > before) recent.pop_front(); }
        continue;
      }
      ++found;
      if (quiet) { stop_all = true; status = 0; return; }
      if (list || list_not) break;
      if (count || binary) { if (max_count >= 0 && found >= max_count) break; continue; }
      // before-context
      if (before > 0) {
        long long from = std::max<long long>(ln - static_cast<long long>(recent.size()), last_printed + 1);
        if (any_printed && from > last_printed + 1) out += "--\n";
        for (long long k = from; k < ln; ++k)
          out += (show_name ? prefix_name + "-" : "") + (number ? std::to_string(k + 1) + "-" : "") +
                 recent[static_cast<size_t>(k - (ln - static_cast<long long>(recent.size())))] + "\n";
      } else if (after > 0 && any_printed && ln > last_printed + 1) {
        out += "--\n";
      }
      std::string head = (show_name ? prefix_name + sep_name : "") + (number ? std::to_string(ln + 1) + ":" : "");
      if (only && !invert) for (auto& h : hits) { if (h.second > h.first) out += head + line.substr(h.first, h.second - h.first) + "\n"; }
      else out += head + line + "\n";
      any_printed = true;
      last_printed = ln;
      pending_after = after;
      if (before > 0) { recent.push_back(line); if (static_cast<long long>(recent.size()) > before) recent.pop_front(); }
      if (out.size() > (1 << 16)) { io.write(out); out.clear(); }
      if (max_count >= 0 && found >= max_count) break;
    }
    in.before_read = nullptr;
    if (list) { if (found) { io.write(name + (null_after_name ? std::string(1, '\0') : "\n")); } }
    else if (list_not) { if (!found) io.write(name + "\n"); }
    else if (count) io.write((show_name ? name + sep_name : "") + std::to_string(found) + "\n");
    else if (binary && found) io.write("Binary file " + (name == "-" ? std::string("(standard input)") : name) + " matches\n");
    else io.write(out);
    if (found) status = 0;
  };

  std::function<void(const std::string&, bool)> visit = [&](const std::string& name, bool top) {
    if (stop_all) return;
    if (name == "-") { Lines in(io.in); search(show_name ? "(standard input)" : "-", in); return; }
    std::string p = host_path(st.cwd, name);
    if (os::is_dir(p)) {
      if (!recursive) { if (!no_messages) io.ewrite("grep: " + name + ": Is a directory\n"); return; }
      std::string base = name.substr(name.find_last_of("/\\") + 1);
      if (!top) for (const auto& x : exclude_dirs) if (glob_match(x, base)) return;
      std::vector<os::DirEntry> es;
      if (!os::list_dir(p, es)) { if (!no_messages) io.ewrite("grep: " + name + ": Permission denied\n"); error = true; return; }
      std::sort(es.begin(), es.end(), [](const os::DirEntry& a, const os::DirEntry& b) { return a.name < b.name; });
      for (const auto& e : es) {
        std::string child = (name == "." && top && implicit_dot ? std::string() : name + "/") + e.name;
        if (!e.dir) {
          bool ok = includes.empty();
          for (const auto& inc : includes) if (glob_match(inc, e.name)) ok = true;
          for (const auto& exc : excludes) if (glob_match(exc, e.name)) ok = false;
          if (!ok) continue;
        }
        if (e.link && e.dir) continue;
        visit(child, false);
      }
      return;
    }
    std::string e;
    Fd f = os::open_file(p, os::Open::Read, &e);
    if (f == kNoFd) { if (!no_messages) io.ewrite("grep: " + name + ": " + e + "\n"); error = true; return; }
    Lines in(f);
    try { search(name, in); } catch (...) { os::close_fd(f); throw; }
    os::close_fd(f);
  };
  try {
    for (const auto& f : files) visit(f, true);
  } catch (const std::regex_error& e) {
    io.ewrite(std::string("grep: ") + e.what() + "\n");
    return 2;
  }
  if (error && status != 0) return 2;
  if (error && !quiet) return 2;
  return status;
}

}  // namespace aiossh
