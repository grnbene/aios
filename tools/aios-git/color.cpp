#include "color.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "cli.hpp"

namespace aios_git {

namespace {

bool match_word(const char* w, size_t len, const char* match) {
  return std::strlen(match) == len && strncasecmp(w, match, len) == 0;
}

// color.c's struct color: what one word names, and how it is written after "\033[".
struct Color {
  enum Type { Unspecified, Normal, Ansi, C256, Rgb } type = Unspecified;
  int value = 0;
  unsigned char r = 0, g = 0, b = 0;
};

int hexval(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

bool hex_byte(const char* in, unsigned char* out) {
  const int hi = hexval(in[0]), lo = hexval(in[1]);
  if (hi < 0 || lo < 0) return false;
  *out = (unsigned char)(hi << 4 | lo);
  return true;
}

bool parse_ansi(Color& out, const char* name, size_t len) {
  static const char* const names[] = {"black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"};
  int offset = 30;
  if (match_word(name, len, "default")) { out.type = Color::Ansi; out.value = 9 + offset; return true; }
  if (len >= 6 && strncasecmp(name, "bright", 6) == 0) { offset = 90; name += 6; len -= 6; }
  for (int i = 0; i < 8; ++i)
    if (match_word(name, len, names[i])) { out.type = Color::Ansi; out.value = i + offset; return true; }
  return false;
}

bool parse_color(Color& out, const char* name, size_t len) {
  if (match_word(name, len, "normal")) { out.type = Color::Normal; return true; }
  if (len == 7 && name[0] == '#' && hex_byte(name + 1, &out.r) && hex_byte(name + 3, &out.g) && hex_byte(name + 5, &out.b)) {
    out.type = Color::Rgb;
    return true;
  }
  if (parse_ansi(out, name, len)) return true;
  std::string word(name, len);
  char* end = nullptr;
  const long v = std::strtol(word.c_str(), &end, 10);
  if (end && (size_t)(end - word.c_str()) == len && len > 0) {
    if (v < -1) return false;
    if (v < 0) { out.type = Color::Normal; return true; }
    if (v < 8) { out.type = Color::Ansi; out.value = (int)v + 30; return true; }
    if (v < 16) { out.type = Color::Ansi; out.value = (int)v - 8 + 90; return true; }
    if (v < 256) { out.type = Color::C256; out.value = (int)v; return true; }
  }
  return false;
}

int parse_attr(const char* name, size_t len) {
  static const struct { const char* name; int val, neg; } attrs[] = {
      {"bold", 1, 22}, {"dim", 2, 22}, {"italic", 3, 23}, {"ul", 4, 24}, {"blink", 5, 25}, {"reverse", 7, 27}, {"strike", 9, 29}};
  bool negate = false;
  if (len >= 2 && name[0] == 'n' && name[1] == 'o') {
    name += 2;
    len -= 2;
    if (len && *name == '-') { ++name; --len; }
    negate = true;
  }
  for (auto& a : attrs)
    if (std::strlen(a.name) == len && std::memcmp(a.name, name, len) == 0) return negate ? a.neg : a.val;
  return -1;
}

bool empty_color(const Color& c) { return c.type <= Color::Normal; }

std::string output(const Color& c, bool background) {
  const int off = background ? 10 : 0;
  switch (c.type) {
    case Color::Ansi: return std::to_string(c.value + off);
    case Color::C256: return std::to_string(38 + off) + ";5;" + std::to_string(c.value);
    case Color::Rgb:
      return std::to_string(38 + off) + ";2;" + std::to_string(c.r) + ";" + std::to_string(c.g) + ";" + std::to_string(c.b);
    default: return "";
  }
}

}  // namespace

std::optional<std::string> parse_git_color(const std::string& value) {
  const char* p = value.c_str();
  size_t len = value.size();
  while (len && std::isspace((unsigned char)*p)) { ++p; --len; }
  if (!len) return std::string();
  bool reset = false;
  unsigned long attr = 0;
  Color fg, bg;
  while (len) {
    const char* word = p;
    size_t wl = 0;
    while (wl < len && !std::isspace((unsigned char)word[wl])) ++wl;
    len -= wl;
    p = word + wl;
    while (len && std::isspace((unsigned char)*p)) { ++p; --len; }
    if (match_word(word, wl, "reset")) { reset = true; continue; }
    Color c;
    if (parse_color(c, word, wl)) {
      if (fg.type == Color::Unspecified) { fg = c; continue; }
      if (bg.type == Color::Unspecified) { bg = c; continue; }
      return std::nullopt;
    }
    const int a = parse_attr(word, wl);
    if (a < 0) return std::nullopt;
    attr |= 1ul << a;
  }
  if (!reset && !attr && empty_color(fg) && empty_color(bg)) return std::string();
  std::string out = "\033[";
  int sep = reset ? 1 : 0;
  for (int i = 0; attr; ++i) {
    if (!(attr & (1ul << i))) continue;
    attr &= ~(1ul << i);
    if (sep++) out += ';';
    out += std::to_string(i);
  }
  if (!empty_color(fg)) { if (sep++) out += ';'; out += output(fg, false); }
  if (!empty_color(bg)) { if (sep++) out += ';'; out += output(bg, true); }
  return out + "m";
}

namespace {
// git_config_colorbool: 0 never, 1 always, 2 auto, -1 not a color setting
int colorbool(std::string v, bool as_config) {
  for (auto& ch : v) ch = (char)std::tolower((unsigned char)ch);
  if (v == "never") return 0;
  if (v == "always") return 1;
  if (v == "auto") return 2;
  if (!as_config) return -1;
  if (v.empty() || v == "true" || v == "yes" || v == "on" || v == "1") return 2;
  if (v == "false" || v == "no" || v == "off" || v == "0") return 0;
  return -1;
}
bool stdout_tty() {
#ifdef _WIN32
  return false;
#else
  return ::isatty(1) != 0;
#endif
}
}  // namespace

bool want_color(Ctx& c, const std::optional<std::string>& cli, bool no_color, const char* cmd_key) {
  int v = 2;
  if (no_color) {
    v = 0;
  } else if (cli) {
    v = cli->empty() ? 1 : colorbool(*cli, false);
    if (v < 0) die(c, "option `color' expects \"always\", \"auto\", or \"never\"");
  } else if (auto cfg = c.config.get(std::string("color.") + cmd_key)) {
    v = colorbool(*cfg, true);
  } else if (auto ui = c.config.get("color.ui")) {
    v = colorbool(*ui, true);
  }
  if (v == 2) {
    const std::string term = c.getenv("TERM");
    return stdout_tty() && term != "dumb";
  }
  return v == 1;
}

const DiffPalette& diff_palette(Ctx& c) {
  static std::unique_ptr<DiffPalette> cached;
  if (cached) return *cached;
  cached = std::make_unique<DiffPalette>();
  DiffPalette& p = *cached;
  struct Slot { const char* name; std::string* dst; } slots[] = {
      {"context", &p.context}, {"plain", &p.context}, {"meta", &p.meta}, {"frag", &p.frag}, {"func", &p.func},
      {"old", &p.old_}, {"new", &p.new_}, {"commit", &p.commit}, {"whitespace", &p.whitespace},
      {"oldmoved", &p.old_moved}, {"oldmovedalternative", &p.old_moved_alt}, {"oldmoveddimmed", &p.old_moved_dim},
      {"oldmovedalternativedimmed", &p.old_moved_alt_dim}, {"newmoved", &p.new_moved}, {"newmovedalternative", &p.new_moved_alt},
      {"newmoveddimmed", &p.new_moved_dim}, {"newmovedalternativedimmed", &p.new_moved_alt_dim}};
  for (auto& s : slots)
    if (auto v = c.config.get(std::string("color.diff.") + s.name))
      if (auto col = parse_git_color(*v)) *s.dst = *col;
  return p;
}

unsigned whitespace_rule(Ctx& c) {
  unsigned rule = kWsBlankAtEol | kWsBlankAtEof | kWsSpaceBeforeTab | 8;
  auto v = c.config.get("core.whitespace");
  if (!v) return rule;
  static const struct { const char* name; unsigned bits; } names[] = {
      {"trailing-space", kWsBlankAtEol | kWsBlankAtEof}, {"space-before-tab", kWsSpaceBeforeTab},
      {"indent-with-non-tab", kWsIndentWithNonTab},      {"cr-at-eol", kWsCrAtEol},
      {"blank-at-eol", kWsBlankAtEol},                   {"blank-at-eof", kWsBlankAtEof},
      {"tab-in-indent", kWsTabInIndent}};
  const std::string& s = *v;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::strchr(", \t\n\r", s[i])) ++i;
    size_t ep = s.find(',', i);
    if (ep == std::string::npos) ep = s.size();
    size_t start = i, len = ep - i;
    bool neg = false;
    if (len && s[start] == '-') { neg = true; ++start; --len; }
    if (!len) break;
    for (auto& n : names)
      if (std::strncmp(n.name, s.c_str() + start, len) == 0) { rule = neg ? rule & ~n.bits : rule | n.bits; break; }
    if (s.compare(start, 9, "tabwidth=") == 0) {
      const unsigned w = (unsigned)std::atoi(s.c_str() + start + 9);
      if (w > 0 && w < 64) rule = (rule & ~(unsigned)kWsTabWidthMask) | w;
    }
    i = ep;
  }
  return rule;
}

unsigned ws_error_highlight(Ctx& c) {
  auto v = c.config.get("diff.wserrorhighlight");
  if (!v) return kWsehNew;
  unsigned val = 0;
  size_t i = 0;
  const std::string& s = *v;
  while (i < s.size()) {
    size_t e = s.find(',', i);
    if (e == std::string::npos) e = s.size();
    const std::string t = s.substr(i, e - i);
    if (t == "none") val = 0;
    else if (t == "default") val = kWsehNew;
    else if (t == "all") val = kWsehNew | kWsehOld | kWsehContext;
    else if (t == "new") val |= kWsehNew;
    else if (t == "old") val |= kWsehOld;
    else if (t == "context") val |= kWsehContext;
    else return kWsehNew;
    i = e + 1;
  }
  return val;
}

void ws_check_emit(std::string& out, const char* line, size_t n, unsigned rule, const std::string& set,
                   const std::string& reset, const std::string& ws) {
  long len = (long)n;
  bool nl = false, cr = false;
  if (len > 0 && line[len - 1] == '\n') { nl = true; --len; }
  if ((rule & kWsCrAtEol) && len > 0 && line[len - 1] == '\r') { cr = true; --len; }
  long trailing = -1;
  if (rule & kWsBlankAtEol)
    for (long i = len - 1; i >= 0; --i) {
      if (git_isspace((unsigned char)line[i])) trailing = i;
      else break;
    }
  if (trailing == -1) trailing = len;
  long written = 0, i = 0;
  for (i = 0; i < trailing; ++i) {
    if (line[i] == ' ') continue;
    if (line[i] != '\t') break;
    if ((rule & kWsSpaceBeforeTab) && written < i) {
      out += ws;
      out.append(line + written, (size_t)(i - written));
      out += reset;
      out += line[i];
    } else if (rule & kWsTabInIndent) {
      out.append(line + written, (size_t)(i - written));
      out += ws;
      out += line[i];
      out += reset;
    } else {
      out.append(line + written, (size_t)(i - written + 1));
    }
    written = i + 1;
  }
  if ((rule & kWsIndentWithNonTab) && i - written >= (long)(rule & kWsTabWidthMask)) {
    out += ws;
    out.append(line + written, (size_t)(i - written));
    out += reset;
    written = i;
  }
  if (trailing - written > 0) {
    out += set;
    out.append(line + written, (size_t)(trailing - written));
    out += reset;
  }
  if (trailing != len) {
    out += ws;
    out.append(line + trailing, (size_t)(len - trailing));
    out += reset;
  }
  if (cr) out += '\r';
  if (nl) out += '\n';
}

}  // namespace aios_git
