// aios-git's shared machinery: configuration, options, paths, identities, dates, refs.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <sstream>

#include "aios/crypto/sha1.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/process.hpp"
#include "aios/userland/git_sign.hpp"
#include "cli_rewrite.hpp"

namespace aios_git {

using namespace aios;
using namespace aios::userland;

namespace {
std::string lower(std::string s) {
  for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
  return s;
}
std::string trim_ws(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}
std::string bytes_str(const ByteBuffer& b) { return std::string((const char*)b.data(), b.size()); }
}  // namespace

// ---------------------------------------------------------------------------
// exits
// ---------------------------------------------------------------------------
void die(Ctx& c, const std::string& msg, int code) {
  c.err << "fatal: " << msg << "\n";
  throw Exit{code};
}
void fail(Ctx& c, const std::string& msg, int code) {
  c.err << "error: " << msg << "\n";
  throw Exit{code};
}
void usage(Ctx& c, const std::string& msg) {
  c.err << "error: " << msg << "\n";
  throw Exit{129};
}
void unsupported(Ctx& c, const std::string& what) {
  c.err << "fatal: aios-git does not implement " << what << "\n";
  throw Exit{128};
}

// ---------------------------------------------------------------------------
// configuration
// ---------------------------------------------------------------------------
std::string Config::canon(const std::string& key) {
  size_t first = key.find('.');
  size_t last = key.rfind('.');
  if (first == std::string::npos) return lower(key);
  if (first == last) return lower(key);
  return lower(key.substr(0, first)) + key.substr(first, last - first) + "." + lower(key.substr(last + 1));
}

bool Config::parse_bool(const std::string& v, bool has_value, bool* ok) {
  if (ok) *ok = true;
  if (!has_value) return true;
  std::string s = lower(trim_ws(v));
  if (s == "true" || s == "yes" || s == "on" || s == "1") return true;
  if (s == "false" || s == "no" || s == "off" || s == "0" || s.empty()) return false;
  char* end = nullptr;
  long n = std::strtol(s.c_str(), &end, 10);
  if (end && *end == 0) return n != 0;
  if (ok) *ok = false;
  return false;
}

void Config::add(const std::string& key, const std::string& value, bool has_value, const std::string& scope, const std::string& origin) {
  entries_.push_back(ConfigEntry{canon(key), value, has_value, scope, origin});
}

void Config::add_text(const std::string& text, const std::string& scope, const std::string& origin) {
  std::string section;   // canonical "section" or "section.Sub"
  size_t i = 0;
  const size_t n = text.size();
  auto skip_line = [&]() { while (i < n && text[i] != '\n') ++i; if (i < n) ++i; };
  while (i < n) {
    while (i < n && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i;
    if (i >= n) break;
    char ch = text[i];
    if (ch == '#' || ch == ';') { skip_line(); continue; }
    if (ch == '[') {
      ++i;
      std::string name;
      while (i < n && text[i] != ']' && text[i] != '"' && text[i] != ' ' && text[i] != '\t' && text[i] != '\n') name += text[i++];
      while (i < n && (text[i] == ' ' || text[i] == '\t')) ++i;
      if (i < n && text[i] == '"') {
        ++i;
        std::string sub;
        while (i < n && text[i] != '"' && text[i] != '\n') {
          if (text[i] == '\\' && i + 1 < n) { sub += text[i + 1]; i += 2; continue; }
          sub += text[i++];
        }
        if (i < n && text[i] == '"') ++i;
        section = lower(name) + "." + sub;
      } else {
        // [section] or the old [section.subsection] (whose subsection folds case)
        section = lower(name);
      }
      while (i < n && text[i] != ']' && text[i] != '\n') ++i;
      if (i < n && text[i] == ']') ++i;
      continue;
    }
    // name [= value]
    std::string name;
    while (i < n && (std::isalnum((unsigned char)text[i]) || text[i] == '-')) name += text[i++];
    while (i < n && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (name.empty()) { skip_line(); continue; }
    bool has_value = false;
    std::string value;
    if (i < n && text[i] == '=') {
      has_value = true;
      ++i;
      while (i < n && (text[i] == ' ' || text[i] == '\t')) ++i;
      bool quoted = false;
      std::string pending_ws;
      while (i < n) {
        char v = text[i];
        if (v == '\n') { break; }
        if (v == '\r' && i + 1 < n && text[i + 1] == '\n') { ++i; continue; }
        if (!quoted && (v == '#' || v == ';')) { while (i < n && text[i] != '\n') ++i; break; }
        if (v == '"') { quoted = !quoted; ++i; continue; }
        if (v == '\\' && i + 1 < n) {
          char e = text[i + 1];
          i += 2;
          if (e == '\n') continue;                         // continuation
          if (e == '\r' && i < n && text[i] == '\n') { ++i; continue; }
          value += pending_ws;
          pending_ws.clear();
          value += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'b' ? '\b' : e;
          continue;
        }
        if (!quoted && (v == ' ' || v == '\t')) { pending_ws += v; ++i; continue; }
        value += pending_ws;
        pending_ws.clear();
        value += v;
        ++i;
      }
    }
    while (i < n && text[i] != '\n') ++i;
    if (i < n) ++i;
    if (section.empty()) continue;
    entries_.push_back(ConfigEntry{section + "." + lower(name), value, has_value, scope, origin});
  }
}

std::optional<std::string> Config::get(const std::string& key) const {
  std::string k = canon(key);
  for (size_t i = entries_.size(); i-- > 0;)
    if (entries_[i].key == k) return entries_[i].has_value ? entries_[i].value : std::string("true");
  return std::nullopt;
}
std::vector<std::string> Config::get_all(const std::string& key) const {
  std::string k = canon(key);
  std::vector<std::string> out;
  for (auto& e : entries_) if (e.key == k) out.push_back(e.has_value ? e.value : std::string("true"));
  return out;
}
bool Config::get_bool(const std::string& key, bool def) const {
  std::string k = canon(key);
  for (size_t i = entries_.size(); i-- > 0;)
    if (entries_[i].key == k) return parse_bool(entries_[i].value, entries_[i].has_value);
  return def;
}
long long Config::get_int(const std::string& key, long long def) const {
  auto v = get(key);
  if (!v) return def;
  char* end = nullptr;
  long long n = std::strtoll(v->c_str(), &end, 10);
  if (end && (*end == 'k' || *end == 'K')) n *= 1024;
  else if (end && (*end == 'm' || *end == 'M')) n *= 1024 * 1024;
  else if (end && (*end == 'g' || *end == 'G')) n *= 1024 * 1024 * 1024;
  return n;
}

namespace {
std::string config_quote(const std::string& v) {
  bool need = v.empty() ? false : (v.front() == ' ' || v.back() == ' ' || v.front() == '\t' || v.back() == '\t');
  std::string out;
  for (char ch : v) {
    if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
    else if (ch == '\n') out += "\\n";
    else if (ch == '\t') out += "\\t";
    else if (ch == '\b') out += "\\b";
    else { if (ch == '#' || ch == ';') need = true; out += ch; }
  }
  return need ? "\"" + out + "\"" : out;
}
std::string section_header(const std::string& sec, const std::string& sub, bool has_sub) {
  if (!has_sub) return "[" + sec + "]";
  std::string esc;
  for (char ch : sub) { if (ch == '"' || ch == '\\') esc += '\\'; esc += ch; }
  return "[" + sec + " \"" + esc + "\"]";
}
// Which canonical section a header line names, or "" when the line is not a header.
std::string header_section(const std::string& line) {
  Config c;
  std::string t = trim_ws(line);
  if (t.empty() || t[0] != '[') return "";
  c.add_text(t + "\nx = 1\n");
  if (c.entries().empty()) return "";
  std::string k = c.entries()[0].key;
  return k.substr(0, k.size() - 2);
}
std::string line_key(const std::string& line) {
  std::string t = trim_ws(line);
  if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') return "";
  std::string name;
  for (char ch : t) { if (std::isalnum((unsigned char)ch) || ch == '-') name += ch; else break; }
  return lower(name);
}
}  // namespace

bool edit_config_file(hal::Host& host, const std::string& path, const std::string& key, const std::string& value,
                      ConfigEdit how, std::string* error) {
  std::string canon_key = Config::canon(key);
  size_t first = key.find('.'), last = key.rfind('.');
  if (first == std::string::npos || last == key.size() - 1 || first == 0) {
    if (error) *error = "key does not contain a section: " + key;
    return false;
  }
  std::string sec = lower(key.substr(0, first));
  bool has_sub = first != last;
  std::string sub = has_sub ? key.substr(first + 1, last - first - 1) : "";
  std::string var = key.substr(last + 1);
  std::string want_section = has_sub ? sec + "." + sub : sec;
  std::string want_var = lower(var);

  std::string text;
  if (auto r = host.read_file(path); r.ok()) text = bytes_str(*r);
  std::vector<std::string> lines;
  {
    std::string cur;
    for (char ch : text) { if (ch == '\n') { lines.push_back(cur); cur.clear(); } else cur += ch; }
    if (!cur.empty()) lines.push_back(cur);
  }
  std::string cur_section;
  long last_in_section = -1, last_match = -1;
  std::vector<size_t> matches;
  for (size_t i = 0; i < lines.size(); ++i) {
    std::string hs = header_section(lines[i]);
    if (!hs.empty()) { cur_section = hs; if (hs == want_section) last_in_section = (long)i; continue; }
    if (cur_section == want_section) {
      last_in_section = (long)i;
      if (line_key(lines[i]) == want_var) { matches.push_back(i); last_match = (long)i; }
    }
  }
  std::string new_line = "\t" + var + " = " + config_quote(value);
  bool found = !matches.empty();
  if (how == ConfigEdit::Unset || how == ConfigEdit::UnsetAll) {
    if (!found) return false;
    if (how == ConfigEdit::Unset && matches.size() > 1) {
      if (error) *error = "key has multiple values";
      return false;
    }
    for (size_t k = matches.size(); k-- > 0;) lines.erase(lines.begin() + (long)matches[k]);
  } else if (how == ConfigEdit::Set && found) {
    if (matches.size() > 1) {
      if (error) *error = "cannot overwrite multiple values with a single value\n       Use a regexp, --add or --replace-all to change " + key + ".";
      return false;
    }
    lines[(size_t)last_match] = new_line;
  } else if (how == ConfigEdit::ReplaceAll && found) {
    lines[matches[0]] = new_line;
    for (size_t k = matches.size(); k-- > 1;) lines.erase(lines.begin() + (long)matches[k]);
  } else if (last_in_section >= 0) {
    lines.insert(lines.begin() + last_in_section + 1, new_line);
  } else {
    lines.push_back(section_header(sec, sub, has_sub));
    lines.push_back(new_line);
  }
  std::string outtext;
  for (auto& l : lines) outtext += l + "\n";
  auto w = host.write_file(path, ByteSpan((const uint8_t*)outtext.data(), outtext.size()));
  if (!w.ok()) {
    if (error) *error = "could not write config file " + path + ": " + w.error().message;
    return false;
  }
  return true;
}

void remove_config_section(hal::Host& host, const std::string& path, const std::string& section_key) {
  auto r = host.read_file(path);
  if (!r.ok()) return;
  std::string text = bytes_str(*r), out, cur;
  std::vector<std::string> lines;
  for (char ch : text) { if (ch == '\n') { lines.push_back(cur); cur.clear(); } else cur += ch; }
  if (!cur.empty()) lines.push_back(cur);
  bool skipping = false;
  for (auto& l : lines) {
    std::string hs = header_section(l);
    if (!hs.empty()) skipping = hs == section_key;
    if (!skipping) out += l + "\n";
  }
  host.write_file(path, ByteSpan((const uint8_t*)out.data(), out.size()));
}

void rename_config_section(hal::Host& host, const std::string& path, const std::string& from, const std::string& to) {
  auto r = host.read_file(path);
  if (!r.ok()) return;
  std::string text = bytes_str(*r), out, cur;
  std::vector<std::string> lines;
  for (char ch : text) { if (ch == '\n') { lines.push_back(cur); cur.clear(); } else cur += ch; }
  if (!cur.empty()) lines.push_back(cur);
  size_t dot = to.find('.');
  for (auto& l : lines) {
    if (header_section(l) == from)
      l = dot == std::string::npos ? "[" + to + "]" : section_header(to.substr(0, dot), to.substr(dot + 1), true);
    out += l + "\n";
  }
  host.write_file(path, ByteSpan((const uint8_t*)out.data(), out.size()));
}

// ---------------------------------------------------------------------------
// options
// ---------------------------------------------------------------------------
std::string Parsed::get(const std::string& k, const std::string& def) const {
  if (!values.count(k) || values.at(k).empty() || negated.count(k)) return def;
  return values.at(k).back();
}

Parsed parse_options(Ctx& c, const std::vector<std::string>& args, size_t start, const std::vector<OptSpec>& spec,
                     bool stop_at_first_positional) {
  Parsed p;
  auto key_of = [](const OptSpec& s) { return s.long_name.empty() ? std::string(1, s.short_name) : s.long_name; };
  bool only_pos = false;
  for (size_t i = start; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (only_pos) { p.pos.push_back(a); continue; }
    if (a == "--") {
      p.dashdash = true;
      for (size_t j = i + 1; j < args.size(); ++j) p.after_dashdash.push_back(args[j]);
      break;
    }
    if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
      std::string name = a.substr(2), val;
      bool has_eq = false;
      size_t eq = name.find('=');
      if (eq != std::string::npos) { val = name.substr(eq + 1); name = name.substr(0, eq); has_eq = true; }
      const OptSpec* hit = nullptr;
      bool neg = false;
      for (auto& s : spec) if (s.long_name == name) hit = &s;
      if (!hit && name.rfind("no-", 0) == 0)
        for (auto& s : spec) if (s.long_name == name.substr(3) && s.negatable) { hit = &s; neg = true; }
      if (!hit) {
        // A unique prefix of a long option, as git accepts (--cach for --cached).
        const OptSpec* pre = nullptr;
        int n = 0;
        for (auto& s : spec)
          if (!s.long_name.empty() && s.long_name.rfind(name, 0) == 0) { pre = &s; ++n; }
        if (n == 1) hit = pre;
      }
      if (!hit) usage(c, "unknown option `" + name + "'");
      std::string k = key_of(*hit);
      if (neg) {
        if (has_eq) usage(c, "option `no-" + name + "' takes no value");
        p.negated.insert(k);
        p.values[k];
        continue;
      }
      p.negated.erase(k);
      if (hit->arg == Arg::None) {
        if (has_eq) usage(c, "option `" + name + "' takes no value");
        p.values[k].push_back("");
      } else if (hit->arg == Arg::Optional) {
        p.values[k].push_back(has_eq ? val : "");
      } else {
        if (!has_eq) {
          if (i + 1 >= args.size()) usage(c, "option `" + name + "' requires a value");
          val = args[++i];
        }
        p.values[k].push_back(val);
      }
      continue;
    }
    if (a.size() > 1 && a[0] == '-' && a != "-") {
      // -abc: flags in a row; one taking a value takes the rest of the word, or the next argument.
      // A bare number (-5) is a count where the spec has a "count" option.
      if (std::isdigit((unsigned char)a[1])) {
        bool counted = false;
        for (auto& s : spec)
          if (s.long_name == "max-count") { p.values["max-count"].push_back(a.substr(1)); counted = true; }
        if (counted) continue;
      }
      for (size_t j = 1; j < a.size(); ++j) {
        const OptSpec* hit = nullptr;
        for (auto& s : spec) if (s.short_name == a[j]) hit = &s;
        if (!hit) usage(c, std::string("unknown switch `") + a[j] + "'");
        std::string k = key_of(*hit);
        p.negated.erase(k);
        if (hit->arg == Arg::None) { p.values[k].push_back(""); continue; }
        std::string rest = a.substr(j + 1);
        if (hit->arg == Arg::Optional) { p.values[k].push_back(rest); break; }
        if (rest.empty()) {
          if (i + 1 >= args.size()) usage(c, std::string("switch `") + a[j] + "' requires a value");
          rest = args[++i];
        }
        p.values[k].push_back(rest);
        break;
      }
      continue;
    }
    p.pos.push_back(a);
    if (stop_at_first_positional) only_pos = true;
  }
  return p;
}

// ---------------------------------------------------------------------------
// paths
// ---------------------------------------------------------------------------
std::string slashes(std::string p) {
  for (auto& ch : p) if (ch == '\\') ch = '/';
  return p;
}
bool is_absolute(const std::string& p) {
  return (!p.empty() && p[0] == '/') || (p.size() > 1 && p[1] == ':' && std::isalpha((unsigned char)p[0]));
}
std::string normalize_path(const std::string& in) {
  std::string p = slashes(in);
  std::string root;
  if (p.size() > 1 && p[1] == ':') { root = p.substr(0, 2) + "/"; p = p.substr(2); }
  else if (!p.empty() && p[0] == '/') root = "/";
  if (!root.empty() && root.size() == 3 && !p.empty() && p[0] == '/') p = p.substr(1);
  else if (root == "/") p = p.substr(1);
  std::vector<std::string> parts;
  size_t i = 0;
  while (i <= p.size()) {
    size_t j = p.find('/', i);
    if (j == std::string::npos) j = p.size();
    std::string seg = p.substr(i, j - i);
    if (seg == "..") {
      if (!parts.empty() && parts.back() != "..") parts.pop_back();
      else if (root.empty()) parts.push_back("..");
    } else if (!seg.empty() && seg != ".") {
      parts.push_back(seg);
    }
    i = j + 1;
  }
  std::string out = root;
  for (size_t k = 0; k < parts.size(); ++k) out += (k ? "/" : "") + parts[k];
  if (out.empty()) return ".";
  if (out.size() > 1 && out.back() == '/' && !(out.size() == 3 && out[1] == ':')) out.pop_back();
  return out;
}
std::string join(const std::string& a, const std::string& b) {
  if (b.empty() || b == ".") return a;
  if (is_absolute(b)) return b;
  if (a.empty() || a == ".") return b;
  return a.back() == '/' ? a + b : a + "/" + b;
}
std::string dirname_of(const std::string& p) {
  size_t s = p.rfind('/');
  return s == std::string::npos ? "" : p.substr(0, s);
}
std::string basename_of(const std::string& p) {
  size_t s = p.rfind('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

std::string quote_path(const std::string& p, bool quote_high, bool quote_space) {
  bool need = false;
  for (unsigned char ch : p)
    if (ch < 0x20 || ch == 0x7f || ch == '"' || ch == '\\' || (quote_high && ch >= 0x80) || (quote_space && ch == ' ')) { need = true; break; }
  if (!need) return p;
  std::string out = "\"";
  for (unsigned char ch : p) {
    switch (ch) {
      case '\a': out += "\\a"; break;
      case '\b': out += "\\b"; break;
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\v': out += "\\v"; break;
      case '\f': out += "\\f"; break;
      case '\r': out += "\\r"; break;
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      default:
        if (ch < 0x20 || ch == 0x7f || (quote_high && ch >= 0x80)) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\%03o", ch);
          out += buf;
        } else {
          out += (char)ch;
        }
    }
  }
  return out + "\"";
}

namespace {
// git's wildmatch. Without `pathname` (a plain pathspec glob) '*' matches '/' too, as fnmatch without
// FNM_PATHNAME does. With it (":(glob)") '*', '?' and '[...]' stay within one folder, and a "**" that is a
// whole path component crosses folders: "**/x" is x at any depth, "a/**" everything inside a, "a/**/b" b
// zero or more folders below a. A "**" inside a component ("a**b") is a plain '*'.
bool wildmatch_at(const char* base, const char* p, const char* t, bool icase, bool pathname) {
  auto fold = [&](char ch) { return icase ? (char)std::tolower((unsigned char)ch) : ch; };
  while (*p) {
    if (*p == '*') {
      const char* q = p;
      while (*q == '*') ++q;
      if (pathname && q - p >= 2 && (p == base || p[-1] == '/') && (*q == '/' || !*q)) {
        if (!*q) return true;   // trailing "/**": everything below
        // "**/": the rest may start at t or after any '/' in it
        for (const char* s = t;;) {
          if (wildmatch_at(base, q + 1, s, icase, pathname)) return true;
          s = std::strchr(s, '/');
          if (!s) return false;
          ++s;
        }
      }
      p = q;
      if (!*p) return !pathname || !std::strchr(t, '/');
      for (;; ++t) {
        if (wildmatch_at(base, p, t, icase, pathname)) return true;
        if (!*t || (pathname && *t == '/')) return false;
      }
    }
    if (!*t) return false;
    if (*p == '?') {
      if (pathname && *t == '/') return false;
      ++p; ++t;
      continue;
    }
    if (*p == '[') {
      const char* q = p + 1;
      bool neg = *q == '!' || *q == '^';
      if (neg) ++q;
      bool hit = false;
      bool first = true;
      const char tc = fold(*t);
      while (*q && (*q != ']' || first)) {
        first = false;
        if (q[1] == '-' && q[2] && q[2] != ']') {
          if ((*t >= q[0] && *t <= q[2]) || (icase && tc >= fold(q[0]) && tc <= fold(q[2]))) hit = true;
          q += 3;
        } else {
          if (fold(*q) == tc) hit = true;
          ++q;
        }
      }
      if (*q != ']') { if (fold('[') != tc) return false; ++p; ++t; continue; }   // no closing ']': a literal '['
      if (hit == neg || (pathname && *t == '/')) return false;
      p = q + 1;
      ++t;
      continue;
    }
    if (*p == '\\' && p[1]) ++p;
    if (fold(*p) != fold(*t)) return false;
    ++p;
    ++t;
  }
  return !*t;
}
bool has_glob(const std::string& s) { return s.find_first_of("*?[") != std::string::npos; }
}  // namespace

bool wildmatch(const std::string& pat, const std::string& path, bool icase, bool pathname) {
  return wildmatch_at(pat.c_str(), pat.c_str(), path.c_str(), icase, pathname);
}

namespace {
// One pathspec item against a path: the path itself, anything under it, or a glob. As in git, the literal
// comparisons come first, so "a*" also names a file or folder literally called "a*".
bool item_matches(const std::string& it, unsigned magic, const std::string& path, bool icase) {
  if (it.empty()) return true;
  icase = icase || (magic & Pathspec::kIcase);
  auto eqs = [&](const std::string& a, const std::string& b) {
    if (!icase) return a == b;
    if (a.size() != b.size()) return false;
    for (size_t k = 0; k < a.size(); ++k) if (std::tolower((unsigned char)a[k]) != std::tolower((unsigned char)b[k])) return false;
    return true;
  };
  if (eqs(path, it)) return true;
  if (path.size() > it.size() && path[it.size()] == '/' && eqs(path.substr(0, it.size()), it)) return true;
  if (magic & Pathspec::kLiteral) return false;
  return has_glob(it) && wildmatch(it, path, icase, (magic & Pathspec::kGlob) != 0);
}
unsigned magic_at(const std::vector<unsigned>& m, size_t i) { return i < m.size() ? m[i] : 0; }
}  // namespace

bool Pathspec::is_wild(size_t i) const {
  return i < items.size() && !(magic_at(magic, i) & kLiteral) && has_glob(items[i]);
}
int Pathspec::matched_index(const std::string& path) const {
  for (size_t i = 0; i < excludes.size(); ++i) if (item_matches(excludes[i], magic_at(exclude_magic, i), path, icase)) return -1;
  for (size_t i = 0; i < items.size(); ++i) if (item_matches(items[i], magic_at(magic, i), path, icase)) return (int)i;
  return -1;
}
bool Pathspec::match(const std::string& path) const {
  if (items.empty()) {   // nothing named: everything, less what is excluded
    for (size_t i = 0; i < excludes.size(); ++i) if (item_matches(excludes[i], magic_at(exclude_magic, i), path, icase)) return false;
    return true;
  }
  return matched_index(path) >= 0;
}
bool Pathspec::may_match_below(const std::string& dir) const {
  if (items.empty()) return true;
  for (size_t i = 0; i < items.size(); ++i) {
    const std::string& it = items[i];
    if (it.empty()) return true;
    // an ":(icase)" item could name the folder in another case: do not prune on it
    if (magic_at(magic, i) & kIcase) return true;
    if (is_wild(i)) {
      // the literal part before the first glob character must agree with the directory
      std::string lit = it.substr(0, it.find_first_of("*?["));
      if (lit.size() <= dir.size() + 1 ? (dir + "/").rfind(lit, 0) == 0 || lit.rfind(dir + "/", 0) == 0 : lit.rfind(dir + "/", 0) == 0)
        return true;
      continue;
    }
    if (it == dir || it.rfind(dir + "/", 0) == 0 || dir.rfind(it + "/", 0) == 0) return true;
  }
  return false;
}

Pathspec make_pathspec(Ctx& c, const std::vector<std::string>& user_paths) {
  Pathspec ps;
  ps.icase = c.repo && c.repo->settings().ignorecase;
  // git's environment switches: GIT_LITERAL_PATHSPECS turns magic off altogether, the others set a default
  // that an item's own ":(literal)" / ":(glob)" overrides.
  auto env_on = [&](const char* k) { std::string v = c.getenv(k); return !v.empty() && v != "0" && v != "false"; };
  const bool env_literal = env_on("GIT_LITERAL_PATHSPECS");
  const bool env_glob = env_on("GIT_GLOB_PATHSPECS"), env_noglob = env_on("GIT_NOGLOB_PATHSPECS");
  if (env_glob && env_noglob) die(c, "global 'glob' and 'noglob' pathspec settings are incompatible");
  const bool env_icase = env_on("GIT_ICASE_PATHSPECS");
  for (auto& raw : user_paths) {
    std::string u = slashes(raw);
    unsigned magic = 0;
    bool exclude = false, top = false;
    if (env_literal) {
      magic |= Pathspec::kLiteral;
    } else if (u.size() > 1 && u[0] == ':' && u[1] == '(') {
      // long magic: ":(word,word,...)path"
      size_t close = u.find(')');
      if (close == std::string::npos) die(c, "Missing ')' at the end of pathspec magic in '" + raw + "'");
      std::string words = u.substr(2, close - 2);
      for (size_t b = 0; b <= words.size();) {
        size_t e = words.find(',', b);
        if (e == std::string::npos) e = words.size();
        std::string w = words.substr(b, e - b);
        b = e + 1;
        if (w.empty()) continue;
        if (w == "top") top = true;
        else if (w == "exclude") exclude = true;
        else if (w == "glob") magic |= Pathspec::kGlob;
        else if (w == "icase") magic |= Pathspec::kIcase;
        else if (w == "literal") magic |= Pathspec::kLiteral;
        else if (w.rfind("prefix:", 0) == 0) continue;   // git's internal hand-off between processes
        else if (w.rfind("attr:", 0) == 0) die(c, "attr pathspec magic is not supported: '" + raw + "'");
        else die(c, "Invalid pathspec magic '" + w + "' in '" + raw + "'");
      }
      u = u.substr(close + 1);
    } else if (!u.empty() && u[0] == ':') {
      // short magic: ":/", ":!", ":^" in any combination, optionally ended by a second ':' (":!:path")
      size_t k = 1;
      for (; k < u.size(); ++k) {
        if (u[k] == '/') top = true;
        else if (u[k] == '!' || u[k] == '^') exclude = true;
        else { if (u[k] == ':') ++k; break; }
      }
      u = u.substr(k);
    }
    if ((magic & Pathspec::kGlob) && (magic & Pathspec::kLiteral)) die(c, raw + ": 'literal' and 'glob' are incompatible");
    if (!(magic & (Pathspec::kGlob | Pathspec::kLiteral))) {
      if (env_glob) magic |= Pathspec::kGlob;
      else if (env_noglob) magic |= Pathspec::kLiteral;
    }
    if (env_icase) magic |= Pathspec::kIcase;
    std::string rp;
    if (top) {
      rp = normalize_path(u);
    } else if (exclude && !(magic & (Pathspec::kGlob | Pathspec::kLiteral)) && u.find('/') == std::string::npos && has_glob(u)) {
      // A plain exclusion with no folder in it ("*.min.*") is matched anywhere in the tree, as git's globs
      // are (a '*' crosses '/'), so it is not anchored to the current folder.
      rp = u;
    } else {
      rp = c.to_repo_path(u);
    }
    if (exclude) {
      if (u.empty()) continue;
      if (rp == "..") continue;   // outside the repository: nothing here to exclude
      ps.excludes.push_back(rp == "." ? "" : rp);
      ps.exclude_magic.push_back(magic);
      continue;
    }
    if (rp == "..") die(c, raw + ": '" + raw + "' is outside repository at '" + c.top + "'");
    ps.items.push_back(rp == "." ? "" : rp);
    ps.raw.push_back(raw);
    ps.magic.push_back(magic);
  }
  return ps;
}

std::string Ctx::to_repo_path(const std::string& user_path) const {
  std::string u = slashes(user_path);
  std::string full = is_absolute(u) ? normalize_path(u) : normalize_path(join(cwd, u));
  if (top.empty()) return normalize_path(u);
  auto eq_ci = [&](const std::string& a, const std::string& b) {
#ifdef _WIN32
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
#else
    return a == b;
#endif
  };
  if (eq_ci(full, top)) return ".";
  if (full.size() > top.size() && full[top.size()] == '/' && eq_ci(full.substr(0, top.size()), top))
    return full.substr(top.size() + 1);
  return "..";   // outside
}

std::string Ctx::to_display_path(const std::string& repo_path) const {
  if (prefix.empty()) return repo_path;
  if (repo_path.rfind(prefix, 0) == 0) return repo_path.size() == prefix.size() ? "./" : repo_path.substr(prefix.size());
  // ../ for every level of prefix not shared
  std::string pre = prefix, rp = repo_path, ups;
  while (!pre.empty()) {
    size_t s = pre.find_last_of('/', pre.size() - 2);
    std::string parent = s == std::string::npos ? "" : pre.substr(0, s + 1);
    ups += "../";
    pre = parent;
    if (rp.rfind(pre, 0) == 0) return ups + rp.substr(pre.size());
  }
  return ups + rp;
}

std::string Ctx::abs_path(const std::string& p) const {
  std::string u = slashes(p);
  return is_absolute(u) ? normalize_path(u) : normalize_path(join(cwd, u));
}

// ---------------------------------------------------------------------------
// the invocation context
// ---------------------------------------------------------------------------
std::string Ctx::getenv(const std::string& k) const {
  auto it = env.find(k);
  if (it != env.end()) return it->second;
#ifdef _WIN32
  // Windows environment names do not care about case.
  for (auto& [n, v] : env) {
    if (n.size() != k.size()) continue;
    bool same = true;
    for (size_t i = 0; i < n.size(); ++i) if (std::toupper((unsigned char)n[i]) != std::toupper((unsigned char)k[i])) { same = false; break; }
    if (same) return v;
  }
#endif
  return "";
}

bool Ctx::hasenv(const std::string& k) const {
  if (env.count(k)) return true;
#ifdef _WIN32
  for (auto& [n, v] : env) {
    if (n.size() != k.size()) continue;
    bool same = true;
    for (size_t i = 0; i < n.size(); ++i) if (std::toupper((unsigned char)n[i]) != std::toupper((unsigned char)k[i])) { same = false; break; }
    if (same) return true;
  }
#endif
  return false;
}

hal::Host& Ctx::host() { return kernel->host(); }

std::string Ctx::config_origin(const std::string& path) const {
  if (!top.empty() && path.rfind(top + "/.git/", 0) == 0 && path.find('/', top.size() + 6) == std::string::npos)
    return "file:" + path.substr(top.size() + 1);
  return "file:" + path;
}

void Ctx::load_config() {
  config.clear();
  auto& h = host();
  std::string home = slashes(getenv("HOME"));
  if (home.empty()) home = slashes(getenv("USERPROFILE"));
  // where an include.path / includeIf path is read from: "~/" is the home, a relative one sits beside the file naming it
  auto include_path = [&](const std::string& value, const std::string& from) {
    std::string inc = slashes(value);
    if (inc.rfind("~/", 0) == 0) return home + inc.substr(1);
    return is_absolute(inc) ? inc : join(dirname_of(from), inc);
  };
  // [includeIf "gitdir:<pattern>"], "gitdir/i:" and "onbranch:<pattern>" (the repository must be known)
  auto condition_holds = [&](const std::string& cond, const std::string& from) {
    auto pattern_of = [&](std::string pat, bool path_like) {
      if (path_like) {
        if (pat.rfind("~/", 0) == 0) pat = home + pat.substr(1);
        else if (pat.rfind("./", 0) == 0) pat = join(dirname_of(from), pat.substr(2));
        else if (!is_absolute(pat)) pat = "**/" + pat;
      }
      if (!pat.empty() && pat.back() == '/') pat += "**";
      return pat;
    };
    if (!repo) return false;
    bool icase = cond.rfind("gitdir/i:", 0) == 0;
    if (icase || cond.rfind("gitdir:", 0) == 0) {
      std::string dir = slashes(repo->git_dir());
      return wildmatch(pattern_of(cond.substr(icase ? 9 : 7), true), dir, icase, true);
    }
    if (cond.rfind("onbranch:", 0) == 0) {
      std::string branch = repo->branch();
      return !branch.empty() && wildmatch(pattern_of(cond.substr(9), false), branch, false, true);
    }
    return false;
  };
  std::function<void(const std::string&, int, const std::string&)> add_file = [&](const std::string& path, int depth, const std::string& scope) {
    auto r = h.read_file(path);
    if (!r.ok()) return;
    Config one;
    one.add_text(bytes_str(*r), scope, config_origin(path));
    for (auto& e : one.entries()) {
      config.add(e.key, e.value, e.has_value, scope, e.origin);
      if (depth >= 10) continue;
      if (e.key == "include.path") add_file(include_path(e.value, path), depth + 1, scope);
      else if (e.key.rfind("includeif.", 0) == 0 && e.key.size() > 15 && e.key.compare(e.key.size() - 5, 5, ".path") == 0 &&
               condition_holds(e.key.substr(10, e.key.size() - 15), path))
        add_file(include_path(e.value, path), depth + 1, scope);
    }
  };
  if (getenv("GIT_CONFIG_NOSYSTEM").empty()) {
    std::string sys = hasenv("GIT_CONFIG_SYSTEM") ? slashes(getenv("GIT_CONFIG_SYSTEM")) : "/etc/gitconfig";
    if (!sys.empty() && sys != "/dev/null") add_file(sys, 0, "system");
  }
  std::string g = slashes(getenv("GIT_CONFIG_GLOBAL"));
  if (!g.empty()) {
    global_config_path = g;
    if (g != "/dev/null") add_file(g, 0, "global");
  } else {
    std::string xdg = slashes(getenv("XDG_CONFIG_HOME"));
    if (xdg.empty() && !home.empty()) xdg = home + "/.config";
    if (!xdg.empty()) add_file(xdg + "/git/config", 0, "global");
    if (!home.empty()) {
      global_config_path = home + "/.gitconfig";
      add_file(global_config_path, 0, "global");
    }
  }
  if (repo) {
    add_file(repo->common_dir() + "/config", 0, "local");
    if (config.get_bool("extensions.worktreeconfig", false)) add_file(repo->git_dir() + "/config.worktree", 0, "worktree");
  }
  long long n = std::atoll(getenv("GIT_CONFIG_COUNT").c_str());
  for (long long i = 0; i < n; ++i) {
    std::string k = getenv("GIT_CONFIG_KEY_" + std::to_string(i));
    if (!k.empty()) config.add(k, getenv("GIT_CONFIG_VALUE_" + std::to_string(i)), true, "command", "command line:");
  }
  for (auto& [k, v] : cli_config) {
    if (v == "\x01") config.add(k, "", false, "command", "command line:");
    else config.add(k, v, true, "command", "command line:");
  }
}

void Ctx::apply_repo_settings() {
  if (!repo) return;
  Git::Settings s;
#ifdef _WIN32
  s.filemode = config.get_bool("core.filemode", false);
  s.symlinks = config.get_bool("core.symlinks", false);
  s.ignorecase = config.get_bool("core.ignorecase", true);
#else
  s.filemode = config.get_bool("core.filemode", true);
  s.symlinks = config.get_bool("core.symlinks", true);
#ifdef __APPLE__
  s.ignorecase = config.get_bool("core.ignorecase", true);
#else
  s.ignorecase = config.get_bool("core.ignorecase", false);
#endif
#endif
  if (auto x = config.get("core.excludesfile")) {
    std::string p = slashes(*x);
    if (p.rfind("~/", 0) == 0) {
      std::string home = slashes(getenv("HOME"));
      if (home.empty()) home = slashes(getenv("USERPROFILE"));
      p = home + p.substr(1);
    }
    s.excludes_file = p;
  } else {
    std::string xdg = slashes(getenv("XDG_CONFIG_HOME"));
    std::string home = slashes(getenv("HOME"));
    if (home.empty()) home = slashes(getenv("USERPROFILE"));
    if (xdg.empty() && !home.empty()) xdg = home + "/.config";
    if (!xdg.empty()) s.excludes_file = xdg + "/git/ignore";
  }
  if (auto v = config.get("core.fsync")) { s.fsync_refs = Git::fsync_covers_refs(*v); s.fsync_index = Git::fsync_covers_index(*v); }
  repo->set_settings(s);
}

namespace {
bool is_git_dir(hal::Host& h, const std::string& d) {
  return h.exists(d + "/HEAD") && h.exists(d + "/objects") && (h.exists(d + "/refs") || h.exists(d + "/commondir"));
}
}  // namespace

bool Ctx::try_open_repo() {
  if (repo) return true;
  auto& h = host();
  std::string gitdir = opt_git_dir.empty() ? slashes(getenv("GIT_DIR")) : opt_git_dir;
  std::string worktree = opt_work_tree.empty() ? slashes(getenv("GIT_WORK_TREE")) : opt_work_tree;
  std::string index_file = slashes(getenv("GIT_INDEX_FILE"));
  auto r = std::make_unique<Git>(*kernel);
  if (!index_file.empty()) r->set_index_file(abs_path(index_file));
  if (!gitdir.empty()) {
    gitdir = abs_path(gitdir);
    if (h.stat(gitdir).ok() && !h.stat(gitdir)->is_dir) {
      // a gitdir: file given as --git-dir
      auto f = h.read_file(gitdir);
      std::string s = f.ok() ? trim_ws(bytes_str(*f)) : "";
      if (s.rfind("gitdir:", 0) == 0) {
        std::string t = slashes(trim_ws(s.substr(7)));
        gitdir = is_absolute(t) ? normalize_path(t) : normalize_path(join(dirname_of(gitdir), t));
      }
    }
    if (!is_git_dir(h, gitdir)) die(*this, "not a git repository: '" + (opt_git_dir_given.empty() ? gitdir : opt_git_dir_given) + "'");
    // Which work tree: --work-tree, else core.worktree, else (not bare) the current directory.
    Config local;
    if (auto f = h.read_file(gitdir + "/config"); f.ok()) local.add_text(bytes_str(*f));
    bool bare = local.get_bool("core.bare", false);
    std::string wt;
    if (!worktree.empty()) wt = abs_path(worktree);
    else if (auto cw = local.get("core.worktree")) wt = is_absolute(slashes(*cw)) ? normalize_path(slashes(*cw)) : normalize_path(join(gitdir, slashes(*cw)));
    else if (!bare) wt = cwd;
    auto o = r->open_dirs(gitdir, wt);
    if (!o.ok()) die(*this, o.error().message);
    repo = std::move(r);
    top = wt;
  } else {
    std::string cur = cwd;
    std::string ceiling = slashes(getenv("GIT_CEILING_DIRECTORIES"));
    for (;;) {
      std::string dotgit = join(cur, ".git");
      if (h.exists(dotgit + "/HEAD") || (h.stat(dotgit).ok() && !h.stat(dotgit)->is_dir)) {
        auto o = r->open(cur);
        if (!o.ok()) die(*this, o.error().message);
        // core.worktree in a separate git dir moves the work tree
        repo = std::move(r);
        top = cur;
        break;
      }
      if (is_git_dir(h, cur)) {
        // a bare repository (or a git dir entered directly)
        auto o = r->open_dirs(cur, "");
        if (!o.ok()) die(*this, o.error().message);
        repo = std::move(r);
        top = "";
        break;
      }
      std::string parent = dirname_of(cur);
      if (parent.empty() || parent == cur || cur.size() <= 3) {
        if (cur.size() > 1 && cur != "/" && !(cur.size() == 3 && cur[1] == ':')) {
          // one more step to the root itself
          std::string root = cur[1] == ':' ? cur.substr(0, 2) + "/" : "/";
          if (root != cur) { cur = root; continue; }
        }
        return false;
      }
      if (!ceiling.empty() && (parent == ceiling)) return false;
      cur = parent.size() == 2 && parent[1] == ':' ? parent + "/" : parent;
    }
  }
  if (!top.empty()) {
    if (top.size() > 3 && top.back() == '/') top.pop_back();
    std::string rp = to_repo_path(cwd);
    prefix = (rp == "." || rp == "..") ? "" : rp + "/";
  }
  load_config();
  apply_repo_settings();
  install_promisor(*this);
  fsmonitor_prepare(*this);
  return true;
}

void Ctx::open_repo(bool need_work_tree) {
  if (!try_open_repo()) die(*this, "not a git repository (or any of the parent directories): .git");
  if (need_work_tree && top.empty()) die(*this, "this operation must be run in a work tree");
}

void Ctx::write_git_file(const std::string& name, const std::string& content) {
  std::string p = repo->git_dir() + "/" + name;
  auto r = host().write_file(p, ByteSpan((const uint8_t*)content.data(), content.size()));
  if (!r.ok()) die(*this, "could not write " + p + ": " + r.error().message);
}
std::optional<std::string> Ctx::read_git_file(const std::string& name) {
  auto r = host().read_file(repo->git_dir() + "/" + name);
  if (!r.ok()) return std::nullopt;
  return bytes_str(*r);
}
void Ctx::remove_git_file(const std::string& name) {
  std::string p = repo->git_dir() + "/" + name;
  if (host().exists(p)) (void)host().remove(p);
}

void default_branch_hint(Ctx& c) {
  if (!c.config.get_bool("advice.defaultbranchname", true)) return;
  // git 2.43's words (an empty hint line keeps its space)
  c.err << "hint: Using 'master' as the name for the initial branch. This default branch name\n"
           "hint: is subject to change. To configure the initial branch name to use in all\n"
           "hint: of your new repositories, which will suppress this warning, call:\n"
           "hint: \n"
           "hint: \tgit config --global init.defaultBranch <name>\n"
           "hint: \n"
           "hint: Names commonly chosen instead of 'master' are 'main', 'trunk' and\n"
           "hint: 'development'. The just-created branch can be renamed via this command:\n"
           "hint: \n"
           "hint: \tgit branch -m <name>\n";
}

// ---------------------------------------------------------------------------
// identities and dates
// ---------------------------------------------------------------------------
int64_t now_seconds() { return (int64_t)std::time(nullptr); }

namespace {
// days from 1970-01-01 to y-m-d (proleptic Gregorian)
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}
void civil_from_days(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int64_t)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : -9);
  y += m <= 2;
}
int tz_minutes(const std::string& tz) {
  if (tz.size() < 5) return 0;
  int sign = tz[0] == '-' ? -1 : 1;
  int hh = (tz[1] - '0') * 10 + (tz[2] - '0');
  int mm = (tz[3] - '0') * 10 + (tz[4] - '0');
  return sign * (hh * 60 + mm);
}
const char* kDays[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
}  // namespace

std::string format_date(int64_t t, const std::string& tz_in, const std::string& mode_in) {
  std::string mode = mode_in;
  std::string tz = tz_in.size() >= 5 ? tz_in : "+0000";
  bool local = false;
  if (mode.size() > 6 && mode.compare(mode.size() - 6, 6, "-local") == 0) { mode = mode.substr(0, mode.size() - 6); local = true; }
  if (local) tz = git_local_tz((uint64_t)t);
  if (mode == "unix") return std::to_string(t);
  if (mode == "raw") return std::to_string(t) + " " + tz;
  if (mode == "relative") {
    int64_t diff = now_seconds() - t;
    auto plural = [](int64_t n, const char* unit) { return std::to_string(n) + " " + unit + (n == 1 ? "" : "s") + " ago"; };
    if (diff < 0) return "in the future";
    if (diff < 90) return plural(diff, "second");
    diff = (diff + 30) / 60;
    if (diff < 90) return plural(diff, "minute");
    diff = (diff + 30) / 60;
    if (diff < 36) return plural(diff, "hour");
    diff = (diff + 12) / 24;
    if (diff < 14) return plural(diff, "day");
    if (diff < 70) return plural((diff + 3) / 7, "week");
    if (diff < 365) return plural((diff + 15) / 30, "month");
    if (diff < 1825) {
      int64_t totalmonths = (diff * 12 * 2 + 365) / (365 * 2);
      int64_t years = totalmonths / 12, months = totalmonths % 12;
      if (months) return std::to_string(years) + " year" + (years == 1 ? "" : "s") + ", " + std::to_string(months) + " month" + (months == 1 ? "" : "s") + " ago";
      return plural(years, "year");
    }
    return plural((diff + 183) / 365, "year");
  }
  int64_t local_t = t + (int64_t)tz_minutes(tz) * 60;
  int64_t days = local_t >= 0 ? local_t / 86400 : -((-local_t + 86399) / 86400);
  int64_t secs = local_t - days * 86400;
  int64_t y; unsigned m, d;
  civil_from_days(days, y, m, d);
  int hh = (int)(secs / 3600), mi = (int)(secs / 60 % 60), ss = (int)(secs % 60);
  int wd = (int)(((days % 7) + 7) % 7);
  char buf[96];
  if (mode == "iso" || mode == "iso8601") {
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02u %02d:%02d:%02d %s", (long long)y, m, d, hh, mi, ss, tz.c_str());
  } else if (mode == "iso-strict" || mode == "iso8601-strict") {
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02uT%02d:%02d:%02d%c%c%c:%c%c", (long long)y, m, d, hh, mi, ss, tz[0], tz[1], tz[2], tz[3], tz[4]);
  } else if (mode == "short") {
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02u", (long long)y, m, d);
  } else if (mode == "rfc" || mode == "rfc2822") {
    std::snprintf(buf, sizeof buf, "%s, %u %s %lld %02d:%02d:%02d %s", kDays[wd], d, kMonths[m - 1], (long long)y, hh, mi, ss, tz.c_str());
  } else {
    std::snprintf(buf, sizeof buf, "%s %s %u %02d:%02d:%02d %lld%s%s", kDays[wd], kMonths[m - 1], d, hh, mi, ss, (long long)y,
                  local ? "" : " ", local ? "" : tz.c_str());
  }
  return buf;
}

bool parse_date(const std::string& in, int64_t& t, std::string& tz) {
  std::string s = trim_ws(in);
  if (s.empty()) return false;
  if (s[0] == '@') s = s.substr(1);
  auto parse_tz = [](const std::string& z, std::string& out) -> bool {
    std::string x = z;
    if (x == "Z" || x == "z" || x == "UTC" || x == "GMT") { out = "+0000"; return true; }
    if (x.size() == 6 && x[3] == ':') x = x.substr(0, 3) + x.substr(4);
    if (x.size() == 3 && (x[0] == '+' || x[0] == '-')) x += "00";
    if (x.size() != 5 || (x[0] != '+' && x[0] != '-')) return false;
    for (size_t i = 1; i < 5; ++i) if (!std::isdigit((unsigned char)x[i])) return false;
    out = x;
    return true;
  };
  // "<seconds> <tz>" or "<seconds>"
  {
    size_t i = 0;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
    if (i > 0 && (i == s.size() || s[i] == ' ')) {
      std::string rest = trim_ws(s.substr(i));
      std::string z = "+0000";
      if (rest.empty() || parse_tz(rest, z)) {
        t = std::atoll(s.substr(0, i).c_str());
        tz = rest.empty() ? git_local_tz((uint64_t)t) : z;
        return true;
      }
    }
  }
  // ISO 8601 / "YYYY-MM-DD HH:MM:SS [tz]"
  int Y, M, D, h = 0, mi = 0, sec = 0;
  char sep = 0;
  int consumed = 0;
  if (std::sscanf(s.c_str(), "%4d-%2d-%2d%n", &Y, &M, &D, &consumed) == 3) {
    std::string rest = s.substr((size_t)consumed);
    if (!rest.empty() && (rest[0] == 'T' || rest[0] == ' ')) {
      sep = rest[0];
      int c2 = 0;
      if (std::sscanf(rest.c_str() + 1, "%2d:%2d:%2d%n", &h, &mi, &sec, &c2) >= 2) {
        rest = rest.substr(1 + (size_t)c2);
        if (!rest.empty() && rest[0] == '.') { size_t k = 1; while (k < rest.size() && std::isdigit((unsigned char)rest[k])) ++k; rest = rest.substr(k); }
      } else {
        return false;
      }
    }
    (void)sep;
    rest = trim_ws(rest);
    std::string z;
    if (rest.empty()) {
      int64_t guess = days_from_civil(Y, (unsigned)M, (unsigned)D) * 86400 + h * 3600 + mi * 60 + sec;
      z = git_local_tz((uint64_t)guess);
    } else if (!parse_tz(rest, z)) {
      return false;
    }
    t = days_from_civil(Y, (unsigned)M, (unsigned)D) * 86400 + h * 3600 + mi * 60 + sec - (int64_t)tz_minutes(z) * 60;
    tz = z;
    return true;
  }
  // RFC 2822: "Thu, 07 Apr 2005 22:13:13 +0200"
  {
    std::string x = s;
    size_t comma = x.find(',');
    if (comma != std::string::npos) x = trim_ws(x.substr(comma + 1));
    char mon[8] = {0};
    char zone[16] = {0};
    if (std::sscanf(x.c_str(), "%d %3s %d %d:%d:%d %15s", &D, mon, &Y, &h, &mi, &sec, zone) >= 6) {
      int mm = -1;
      for (int k = 0; k < 12; ++k) if (strncmp(kMonths[k], mon, 3) == 0) mm = k + 1;
      if (mm < 0) return false;
      std::string z = zone[0] ? std::string(zone) : "+0000";
      if (!parse_tz(z, z)) return false;
      t = days_from_civil(Y, (unsigned)mm, (unsigned)D) * 86400 + h * 3600 + mi * 60 + sec - (int64_t)tz_minutes(z) * 60;
      tz = z;
      return true;
    }
  }
  return false;
}

bool parse_approxidate(const std::string& in, int64_t& t) {
  std::string tz;
  if (parse_date(in, t, tz)) return true;
  // git's approxidate, the relative part: "<n> <unit>[s] [<n> <unit>...] [ago]" with '.', ' ' or '_' between
  // words, months and years counted on the calendar; "now", "yesterday".
  std::vector<std::string> words;
  std::string w;
  for (char ch : in + " ") {
    if (ch == '.' || ch == ' ' || ch == '_') { if (!w.empty()) words.push_back(w); w.clear(); }
    else w += (char)std::tolower((unsigned char)ch);
  }
  if (words.empty()) return false;
  int64_t now = now_seconds(), secs = 0;
  long months = 0;
  for (size_t i = 0; i < words.size(); ++i) {
    const std::string& x = words[i];
    if (x == "ago" || x == "now") continue;
    if (x == "yesterday") { secs += 86400; continue; }
    if (x.empty() || !std::isdigit((unsigned char)x[0]) || i + 1 >= words.size()) return false;
    for (char ch : x) if (!std::isdigit((unsigned char)ch)) return false;
    long long n = std::atoll(x.c_str());
    std::string u = words[++i];
    if (u.size() > 1 && u.back() == 's') u.pop_back();
    if (u == "second" || u == "sec") secs += n;
    else if (u == "minute" || u == "min") secs += n * 60;
    else if (u == "hour") secs += n * 3600;
    else if (u == "day") secs += n * 86400;
    else if (u == "week") secs += n * 7 * 86400;
    else if (u == "month") months += (long)n;
    else if (u == "year") months += (long)n * 12;
    else return false;
  }
  t = now - secs;
  if (months) {
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400, y;
    unsigned m, d;
    civil_from_days(days, y, m, d);
    int64_t mm = y * 12 + (m - 1) - months;
    y = mm >= 0 ? mm / 12 : (mm - 11) / 12;
    m = (unsigned)(mm - y * 12) + 1;
    t = days_from_civil(y, m, d) * 86400 + (t - days * 86400);
  }
  return true;
}

std::string Ctx::ident(bool committer, bool strict) {
  const char* who = committer ? "COMMITTER" : "AUTHOR";
  std::string name = getenv(std::string("GIT_") + who + "_NAME");
  std::string email = getenv(std::string("GIT_") + who + "_EMAIL");
  // set but empty is a choice git honours (and refuses), not a reason to fall back to config
  bool name_set = hasenv(std::string("GIT_") + who + "_NAME"), email_set = hasenv(std::string("GIT_") + who + "_EMAIL");
  if (name.empty() && !name_set) {
    auto v = config.get(committer ? "committer.name" : "author.name");
    if (!v) v = config.get("user.name");
    if (v) name = *v;
  }
  if (email.empty() && !email_set) {
    auto v = config.get(committer ? "committer.email" : "author.email");
    if (!v) v = config.get("user.email");
    if (v) email = *v;
    else email = getenv("EMAIL");
  }
  if (!strict) {
    // git's default ident for a reflog: the login name, and login@host for the email.
    std::string login = getenv("USER");
    if (login.empty()) login = getenv("USERNAME");
    if (login.empty()) login = "unknown";
    if (name.empty()) name = login;
    if (email.empty()) {
      std::string host = getenv("HOSTNAME");
      email = login + "@" + (host.empty() ? std::string("localhost") : host);
    }
  }
  if (name.empty() && name_set) die(*this, "empty ident name (for <" + email + ">) not allowed");
  if (name.empty() || email.empty()) {
    err << "Author identity unknown\n\n*** Please tell me who you are.\n\nRun\n\n"
           "  git config --global user.email \"you@example.com\"\n"
           "  git config --global user.name \"Your Name\"\n\n"
           "to set your account's default identity.\nOmit --global to set the identity only in this repository.\n\n";
    die(*this, name.empty() ? "empty ident name not allowed" : "unable to auto-detect email address");
  }
  std::string date = getenv(std::string("GIT_") + who + "_DATE");
  int64_t t = now_seconds();
  std::string tz = git_local_tz((uint64_t)t);
  if (!date.empty() && !parse_date(date, t, tz)) die(*this, "invalid date format: " + date);
  // git strips characters that would break the header
  auto clean = [](std::string s) {
    std::string o;
    for (char ch : s) if (ch != '<' && ch != '>' && ch != '\n') o += ch;
    return trim_ws(o);
  };
  return clean(name) + " <" + clean(email) + "> " + std::to_string(t) + " " + tz;
}

// ---------------------------------------------------------------------------
// object names
// ---------------------------------------------------------------------------
int Ctx::default_abbrev() {
  if (abbrev_cache) return abbrev_cache;
  if (auto a = config.get("core.abbrev")) {
    if (*a != "auto") {
      int n = std::atoi(a->c_str());
      if (*a == "no") n = 40;
      if (n >= 4 && n <= 40) return abbrev_cache = n;
    }
  }
  // git's "auto": about 2^len objects expect a collision at 2^(len/2); counted from the packs.
  if (!repo) return abbrev_cache = 7;
  uint64_t count = 0;
  auto& h = host();
  std::string pd = repo->common_dir() + "/objects/pack";
  if (auto names = h.list_dir(pd); names.ok()) {
    for (auto& n : *names) {
      if (n.size() < 4 || n.compare(n.size() - 4, 4, ".idx") != 0) continue;
      auto idx = h.read_range(pd + "/" + n, 0, 8 + 256 * 4);
      if (!idx.ok() || idx->size() < 8 + 256 * 4) continue;
      const uint8_t* b = idx->data();
      size_t off = (b[0] == 0xff && b[1] == 't' && b[2] == 'O' && b[3] == 'c') ? 8 : 0;
      if (off + 1024 > idx->size()) continue;
      const uint8_t* last = b + off + 255 * 4;
      count += ((uint64_t)last[0] << 24) | ((uint64_t)last[1] << 16) | ((uint64_t)last[2] << 8) | last[3];
    }
  }
  int len = 7;
  if (count) {
    int msb = 0;
    for (uint64_t c2 = count; c2 > 1; c2 >>= 1) ++msb;
    int l = (msb + 1 + 1) / 2;
    if (l > len) len = l;
  }
  return abbrev_cache = len;
}

std::string Ctx::abbrev(const std::string& oid, int min_len) {
  if (oid.size() != 40) return oid;
  int len = min_len ? min_len : default_abbrev();
  if (len >= 40) return oid;
  if (!repo) return oid.substr(0, (size_t)len);
  return oid.substr(0, (size_t)repo->abbrev_len(oid, len));
}

// ---------------------------------------------------------------------------
// messages and commits
// ---------------------------------------------------------------------------
std::string cleanup_message(const std::string& msg, bool strip_comments) {
  std::vector<std::string> lines;
  std::string cur;
  for (char ch : msg) { if (ch == '\n') { lines.push_back(cur); cur.clear(); } else cur += ch; }
  if (!cur.empty()) lines.push_back(cur);
  std::vector<std::string> kept;
  for (auto& l : lines) {
    if (strip_comments && !l.empty() && l[0] == '#') continue;
    size_t e = l.find_last_not_of(" \t\r");
    kept.push_back(e == std::string::npos ? "" : l.substr(0, e + 1));
  }
  std::string out;
  bool pending_blank = false;
  for (auto& l : kept) {
    if (l.empty()) { if (!out.empty()) pending_blank = true; continue; }
    if (pending_blank) out += "\n";
    pending_blank = false;
    out += l + "\n";
  }
  return out;
}

std::string read_all(std::istream& in) {
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string subject_of(const std::string& message) {
  // The first paragraph, lines joined with spaces (git's %s).
  std::string out;
  size_t i = 0;
  while (i < message.size() && message[i] == '\n') ++i;
  while (i < message.size()) {
    size_t nl = message.find('\n', i);
    std::string line = message.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
    if (trim_ws(line).empty()) break;
    // git keeps a line's leading whitespace and drops its trailing whitespace
    size_t e = line.find_last_not_of(" \t\r\n");
    if (i > 0 && !out.empty()) out += " ";
    out += line.substr(0, e + 1);
    if (nl == std::string::npos) break;
    i = nl + 1;
  }
  return out;
}

std::string make_commit(Ctx& c, const std::string& tree, const std::vector<std::string>& parents,
                        const std::string& message, const std::string& author, const std::string& committer,
                        bool signable) {
  if (tree == empty_tree_oid() && !c.repo->has_object(tree)) {
    // a commit of the empty tree stores it, so the repository stays complete
    auto w = c.repo->write_object("tree", ByteSpan{});
    if (!w.ok()) die(c, w.error().message);
  }
  std::string body = "tree " + tree + "\n";
  for (auto& p : parents) body += "parent " + p + "\n";
  body += "author " + (author.empty() ? c.ident(false) : author) + "\n";
  body += "committer " + (committer.empty() ? c.ident(true) : committer) + "\n\n";
  body += message;
  if (signable) body = sign_commit_body(c, body);
  auto w = c.repo->write_object("commit", ByteSpan((const uint8_t*)body.data(), body.size()));
  if (!w.ok()) die(c, "failed to write commit object: " + w.error().message);
  return *w;
}

void take_sign_option(Ctx& c, const Parsed& p) {
  if (p.negated.count("gpg-sign") || p.has("no-gpg-sign")) { c.sign = 0; return; }
  if (p.has("gpg-sign")) { c.sign = 1; c.sign_key = p.get("gpg-sign"); }
}

std::string sign_commit_body(Ctx& c, const std::string& body) {
  bool want = c.sign == 1 || (c.sign == -1 && c.config.get_bool("commit.gpgsign", false));
  if (!want) return body;
  std::string format = c.config.get("gpg.format").value_or("openpgp");
  if (format != "ssh") die(c, "aios-git signs with SSH keys only: set gpg.format=ssh and user.signingkey (gpg.format is " + format + ")");
  std::string key = !c.sign_key.empty() ? c.sign_key : c.config.get("user.signingkey").value_or("");
  if (key.empty()) die(c, "user.signingkey needs to be set for ssh signing");
  const bool literal = key.rfind("key::", 0) == 0 || key.rfind("ssh-", 0) == 0;
  if (key.rfind("~/", 0) == 0) {
    std::string home = slashes(c.getenv("HOME"));
    if (home.empty()) home = slashes(c.getenv("USERPROFILE"));
    key = home + key.substr(1);
  }
  const std::string program = c.config.get("gpg.ssh.program").value_or("");
  // Signed here when the private key can be read and gpg.ssh.program is ssh-keygen's; otherwise as git does it:
  // `<program> -Y sign -n git -f <key> [-U] <file>` (ssh-keygen by default), which leaves <file>.sig. That way a key
  // held by an ssh-agent, or a signing program the machine is set up with, works as it does for git.
  // ssh-keygen (the default) with a private key file signs exactly as this does, so that is done here too
  const bool keygen = program.empty() || program == "ssh-keygen" ||
                      (program.size() > 11 && program.compare(program.size() - 11, 11, "/ssh-keygen") == 0);
  if (keygen && !literal) {
    std::string path = c.abs_path(key);
    // a public key file's private half sits beside it, as ssh-keygen -Y sign finds it
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".pub") == 0) path.resize(path.size() - 4);
    if (auto pem = c.host().read_file(path); pem.ok()) {
      auto id = aios::net::ssh_parse_private_key(ByteSpan(pem->data(), pem->size()));
      if (!id.ok()) die(c, "the signing key " + path + " is not usable: " + id.error().message);
      auto sig = aios::userland::sshsig_sign(*id, "git", body);
      if (!sig.ok()) die(c, "failed to sign the commit: " + sig.error().message);
      return aios::userland::git_add_signature(body, *sig);
    }
  }
  const std::string prog = program.empty() ? aios::hal::find_program({"ssh-keygen"}) : program;
  if (prog.empty()) die(c, "cannot read the signing key " + key + ", and there is no ssh-keygen to sign with it");
  const std::string dir = c.repo->git_dir();
  const std::string buffer = dir + "/SSH_SIGN_BUFFER", key_file = dir + "/SSH_SIGN_KEY", out = dir + "/SSH_SIGN_OUTPUT";
  auto cleanup = [&] { for (const auto& f : {buffer, buffer + ".sig", key_file, out}) (void)c.host().remove(f); };
  if (!c.host().write_file(buffer, ByteSpan((const uint8_t*)body.data(), body.size())).ok()) die(c, "cannot write " + buffer);
  std::string kf = c.abs_path(key);
  if (literal) {
    std::string line = key.rfind("key::", 0) == 0 ? key.substr(5) : key;
    line += "\n";
    if (!c.host().write_file(key_file, ByteSpan((const uint8_t*)line.data(), line.size())).ok()) { cleanup(); die(c, "cannot write " + key_file); }
    kf = key_file;
  }
  aios::hal::ProcessSpawn spec;
  spec.argv = {prog, "-Y", "sign", "-n", "git", "-f", kf};
  if (literal) spec.argv.push_back("-U");
  spec.argv.push_back(buffer);
  spec.output_path = out;
  auto p = aios::hal::spawn_process(spec);
  if (!p.ok()) { cleanup(); die(c, "could not run " + prog + " to sign the commit"); }
  auto st = aios::hal::wait_process(*p, 120000);
  aios::hal::release_process(*p);
  auto sig = c.host().read_file(buffer + ".sig");
  std::string said;
  if (auto f = c.host().read_file(out); f.ok()) said.assign((const char*)f->data(), f->size());
  cleanup();
  if (!st.ok() || !st->exited || st->exit_code != 0 || !sig.ok()) {
    c.err << said;
    die(c, "failed to sign the commit with " + prog);
  }
  return aios::userland::git_add_signature(body, std::string((const char*)sig->data(), sig->size()));
}

std::string hash_blob(const std::string& content) {
  std::string header = "blob " + std::to_string(content.size());
  std::string all = header;
  all.push_back('\0');
  all += content;
  auto sum = crypto::sha1(ByteSpan((const uint8_t*)all.data(), all.size()));
  static const char* hx = "0123456789abcdef";
  std::string out;
  for (uint8_t b : sum) { out += hx[b >> 4]; out += hx[b & 15]; }
  return out;
}

std::string empty_tree_oid() { return "4b825dc642cb6eb9a060e54bf8d69288fbee4904"; }

std::string write_tree_of(Ctx& c, const std::vector<GitIndexEntry>& entries) {
  if (entries.empty()) {
    auto w = c.repo->write_object("tree", ByteSpan{});
    if (!w.ok()) die(c, w.error().message);
    return *w;
  }
  auto w = c.repo->write_tree_from_entries(entries);
  if (!w.ok()) die(c, w.error().message);
  return *w;
}

void append_reflog(Ctx& c, const std::string& ref, const std::string& old_oid, const std::string& new_oid,
                   const std::string& msg) {
  if (!c.repo) return;
  bool logall = c.config.get_bool("core.logallrefupdates", !c.repo->bare());
  bool always = ref == "HEAD" || ref == "refs/stash" || ref.rfind("refs/heads/", 0) == 0;
  if (!logall && ref != "refs/stash") return;
  if (!always && ref.rfind("refs/remotes/", 0) != 0 && ref.rfind("refs/notes/", 0) != 0) return;
  std::string base = (ref == "HEAD" ? c.repo->git_dir() : c.repo->common_dir()) + "/logs/" + ref;
  auto& h = c.host();
  std::string dir = dirname_of(base);
  (void)h.make_dir(dir);
  std::string who = c.ident(true, false);   // a reflog line never fails the command for want of a name
  std::string line = (old_oid.empty() ? std::string(40, '0') : old_oid) + " " + (new_oid.empty() ? std::string(40, '0') : new_oid) +
                     " " + who + "\t" + msg + "\n";
  if (c.repo->settings().fsync_refs) (void)h.append_file(base, ByteSpan((const uint8_t*)line.data(), line.size()));
  else (void)h.append_file_unsynced(base, ByteSpan((const uint8_t*)line.data(), line.size()));
}

std::optional<std::string> read_ref(Ctx& c, const std::string& full) {
  if (full == "FETCH_HEAD" || full == "MERGE_HEAD" || full == "ORIG_HEAD" || full == "CHERRY_PICK_HEAD" ||
      full == "REVERT_HEAD" || full == "REBASE_HEAD" || full == "AUTO_MERGE") {
    auto t = c.read_git_file(full);
    if (!t || t->size() < 40) return std::nullopt;
    return t->substr(0, 40);
  }
  if (full == "HEAD") {
    auto h = c.repo->head_oid();
    if (!h.ok()) return std::nullopt;
    return *h;
  }
  auto r = c.repo->ref_oid(full);
  if (!r.ok()) return std::nullopt;
  return *r;
}

void update_ref_checked(Ctx& c, const std::string& ref_in, const std::string& new_oid, const std::optional<std::string>& old_oid,
                        const std::string& reflog_msg) {
  std::string ref = ref_in;
  if (ref == "HEAD" && !c.repo->head_ref().empty()) ref = c.repo->head_ref();
  auto cur = ref == "HEAD" ? read_ref(c, "HEAD") : read_ref(c, ref);
  if (old_oid) {
    if (old_oid->empty() || *old_oid == std::string(40, '0')) {
      if (cur) die(c, "cannot lock ref '" + ref + "': reference already exists");
    } else if (!cur) {
      die(c, "cannot lock ref '" + ref + "': reference is missing but expected " + *old_oid);
    } else if (*cur != *old_oid) {
      die(c, "cannot lock ref '" + ref + "': is at " + *cur + " but expected " + *old_oid);
    }
  }
  if (ref != "HEAD" && !Git::valid_ref_name(ref)) die(c, "refusing to update ref with bad name '" + ref + "'");
  // a symbolic HEAD names itself and the branch it stands for in the transaction, as git's hook sees it
  std::vector<RefUpdate> updates;
  if (ref_in == "HEAD" && ref != "HEAD") updates.push_back({"HEAD", cur.value_or(""), new_oid});
  updates.push_back({ref, cur.value_or(""), new_oid});
  with_ref_transaction(c, updates, [&] {
    if (ref == "HEAD") {
      auto r = c.repo->set_head_detached(new_oid);
      if (!r.ok()) die(c, r.error().message);
    } else {
      // with an old value, checked and written under the ref's lock: no other writer slips in between
      auto r = old_oid ? c.repo->compare_and_swap_ref(ref, *old_oid == std::string(40, '0') ? std::string() : *old_oid, new_oid)
                       : c.repo->update_ref(ref, new_oid);
      if (!r.ok()) die(c, "cannot lock ref '" + ref + "': " + r.error().message);
    }
    if (!reflog_msg.empty()) {
      append_reflog(c, ref, cur.value_or(""), new_oid, reflog_msg);
      if (ref != "HEAD" && (ref_in == "HEAD" || ref == c.repo->head_ref())) append_reflog(c, "HEAD", cur.value_or(""), new_oid, reflog_msg);
    }
  });
}

// ---------------------------------------------------------------------------
// remotes
// ---------------------------------------------------------------------------
std::string remote_url(Ctx& c, const std::string& r, bool* is_named) {
  if (is_named) *is_named = false;
  if (auto u = c.config.get("remote." + r + ".url")) {
    if (is_named) *is_named = true;
    std::string url = *u;
    // url.<base>.insteadOf
    std::string best_from, best_to;
    for (auto& e : c.config.entries()) {
      if (e.key.rfind("url.", 0) != 0 || e.key.size() < 14 || e.key.compare(e.key.size() - 10, 10, ".insteadof") != 0) continue;
      std::string base = e.key.substr(4, e.key.size() - 14);
      if (url.rfind(e.value, 0) == 0 && e.value.size() > best_from.size()) { best_from = e.value; best_to = base; }
    }
    if (!best_from.empty()) url = best_to + url.substr(best_from.size());
    return url;
  }
  return r;
}

std::vector<std::pair<std::string, std::string>> http_headers_for(Ctx& c, const std::string& url) {
  std::vector<std::pair<std::string, std::string>> out;
  // http.extraHeader, then the most specific http.<url>.extraHeader, in config order; an empty value
  // clears what came before (git's rule).
  std::vector<std::string> values;
  for (auto& e : c.config.entries()) {
    if (e.key == "http.extraheader") {
      if (e.value.empty()) values.clear();
      else values.push_back(e.value);
      continue;
    }
    if (e.key.rfind("http.", 0) == 0 && e.key.size() > 17 && e.key.compare(e.key.size() - 12, 12, ".extraheader") == 0) {
      std::string pattern = e.key.substr(5, e.key.size() - 17);
      std::string p = pattern;
      while (!p.empty() && p.back() == '/') p.pop_back();
      if (url.rfind(p, 0) == 0 && (url.size() == p.size() || url[p.size()] == '/' || p.back() == '/')) {
        if (e.value.empty()) values.clear();
        else values.push_back(e.value);
      }
    }
  }
  for (auto& v : values) {
    size_t colon = v.find(':');
    if (colon == std::string::npos) continue;
    out.push_back({trim_ws(v.substr(0, colon)), trim_ws(v.substr(colon + 1))});
  }
  return out;
}

}  // namespace aios_git
