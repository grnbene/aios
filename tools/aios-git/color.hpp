// Colors as git writes them: its color names parsed to escape sequences (color.c), the diff palette with
// color.diff.<slot> over git's defaults, and the whitespace rules core.whitespace sets (ws.c).
#pragma once
#include <optional>
#include <string>

namespace aios_git {

struct Ctx;

// git's color_parse_mem: "[reset] [fg [bg]] [attr]..." to the escape sequence, "" for no color at all,
// nullopt for a value git refuses.
std::optional<std::string> parse_git_color(const std::string& value);

constexpr const char* kColorReset = "\033[m";

// diff.c's diff_colors, each slot overridable by color.diff.<slot>.
struct DiffPalette {
  std::string reset = kColorReset, context, meta = "\033[1m", frag = "\033[36m", old_ = "\033[31m", new_ = "\033[32m",
              commit = "\033[33m", whitespace = "\033[41m", func;
  // --color-moved: a moved line, the alternate block, and their dimmed forms
  std::string old_moved = "\033[1;35m", old_moved_alt = "\033[1;34m", old_moved_dim = "\033[2m", old_moved_alt_dim = "\033[2;3m",
              new_moved = "\033[1;36m", new_moved_alt = "\033[1;33m", new_moved_dim = "\033[2m", new_moved_alt_dim = "\033[2;3m";
};
const DiffPalette& diff_palette(Ctx& c);

// ws.c's rule bits (only the ones the diff paints by), and diff.c's error-highlight bits.
enum : unsigned {
  kWsBlankAtEol = 1u << 6,
  kWsSpaceBeforeTab = 1u << 7,
  kWsIndentWithNonTab = 1u << 8,
  kWsCrAtEol = 1u << 9,
  kWsBlankAtEof = 1u << 10,
  kWsTabInIndent = 1u << 11,
  kWsTabWidthMask = 0x3f,
};
enum : unsigned { kWsehNew = 1, kWsehContext = 2, kWsehOld = 4 };
unsigned whitespace_rule(Ctx& c);         // core.whitespace over git's default (blank-at-eol, blank-at-eof, space-before-tab)
unsigned ws_error_highlight(Ctx& c);      // diff.wsErrorHighlight, "new" by default

// color.c's decision for a command: its --color[=<when>] (`cli`: unset when not given, "" for a bare --color) or
// --no-color, else color.<cmd_key>, else color.ui; "auto" (git's default) is on when stdout is a terminal.
bool want_color(Ctx& c, const std::optional<std::string>& cli, bool no_color, const char* cmd_key);

// git's own isspace (git-compat-util.h): space, tab, LF and CR only - not the C library's form feed or vertical tab.
inline bool git_isspace(unsigned char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; }

// ws_check_emit: one line's text (after its sign, with its newline) painted in `set`, whitespace errors in `ws`.
void ws_check_emit(std::string& out, const char* line, size_t len, unsigned rule, const std::string& set,
                   const std::string& reset, const std::string& ws);

}  // namespace aios_git
