#include "pager.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <set>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace aios_git {

namespace {

// git's default colors
constexpr const char* kReset = "\033[m";
constexpr const char* kCommit = "\033[33m";        // yellow
constexpr const char* kMeta = "\033[1m";           // bold: diff --git, index, ---, +++
constexpr const char* kFrag = "\033[36m";          // cyan: @@ ... @@
constexpr const char* kOld = "\033[31m";           // red: -line
constexpr const char* kNew = "\033[32m";           // green: +line
constexpr const char* kHead = "\033[1;36m";        // decoration: HEAD
constexpr const char* kBranch = "\033[1;32m";      //             a local branch
constexpr const char* kRemote = "\033[1;31m";      //             a remote-tracking branch
constexpr const char* kTag = "\033[1;33m";         //             a tag

bool starts(const std::string& s, size_t at, const char* p) { return s.compare(at, std::char_traits<char>::length(p), p) == 0; }

bool hex_run(const std::string& s, size_t at, size_t* end) {
  size_t i = at;
  while (i < s.size() && std::isxdigit((unsigned char)s[i])) ++i;
  *end = i;
  return i - at >= 7;
}

// Colors whole lines as git does for log, show and diff, and hands them on. With `stop_on_closed` (a pager is
// reading) a write the other end refused - the pager quit - ends the command quietly, as git's SIGPIPE does, but
// through the command's own unwinding so nothing it holds is left behind.
class LineFilter : public std::streambuf {
 public:
  enum class Mode { Plain, Log, Diff };
  LineFilter(Ctx& c, std::streambuf* dest, Mode mode, bool color, bool graph, bool oneline, bool custom_format,
             bool stop_on_closed)
      : c_(c), dest_(dest), mode_(mode), color_(color), graph_(graph), oneline_(oneline), custom_(custom_format),
        stop_(stop_on_closed) {}

  ~LineFilter() override {
    try { flush_partial(); } catch (...) {}
  }

 protected:
  int_type overflow(int_type ch) override {
    if (traits_type::eq_int_type(ch, traits_type::eof())) return traits_type::not_eof(ch);
    line_ += traits_type::to_char_type(ch);
    if (ch == '\n') emit_line();
    return ch;
  }
  std::streamsize xsputn(const char* s, std::streamsize n) override {
    for (std::streamsize i = 0; i < n; ++i) {
      line_ += s[i];
      if (s[i] == '\n') emit_line();
    }
    return n;
  }
  int sync() override {
    flush_partial();
    if (dest_->pubsync() == 0) return 0;
    if (stop_) throw Exit{0};
    return -1;
  }

 private:
  void put(const std::string& s) {
    if (s.empty()) return;
    if (dest_->sputn(s.data(), (std::streamsize)s.size()) != (std::streamsize)s.size() && stop_) throw Exit{0};
  }
  void flush_partial() {
    if (line_.empty()) return;
    std::string l;
    l.swap(line_);
    put(color_ && mode_ != Mode::Plain ? colored(l) : l);
  }
  void emit_line() {
    std::string l;
    l.swap(line_);
    put(color_ && mode_ != Mode::Plain ? colored(l) : l);
  }

  // " (HEAD -> main, tag: v1, origin/main)" in color
  std::string decorations(const std::string& d) {
    // d starts with " (" and ends with ")"
    std::string inner = d.substr(2, d.size() - 3);
    std::string out = std::string(" ") + kCommit + "(" + kReset;
    size_t i = 0;
    bool first = true;
    while (i <= inner.size()) {
      size_t j = inner.find(", ", i);
      if (j == std::string::npos) j = inner.size();
      std::string name = inner.substr(i, j - i);
      if (!first) out += std::string(kCommit) + ", " + kReset;
      first = false;
      if (starts(name, 0, "HEAD -> ")) {
        out += std::string(kHead) + "HEAD" + kReset + kHead + " -> " + kReset + kBranch + name.substr(8) + kReset;
      } else if (name == "HEAD") {
        out += std::string(kHead) + name + kReset;
      } else if (starts(name, 0, "tag: ")) {
        out += std::string(kTag) + name + kReset;
      } else {
        const size_t slash = name.find('/');
        const bool remote = slash != std::string::npos && c_.config.get("remote." + name.substr(0, slash) + ".url").has_value();
        out += std::string(remote ? kRemote : kBranch) + name + kReset;
      }
      if (j >= inner.size()) break;
      i = j + 2;
    }
    return out + kCommit + ")" + kReset;
  }

  // the rest of a commit or oneline line after the id: decorations, then plain text
  std::string after_id(const std::string& rest) {
    if (starts(rest, 0, " (")) {
      size_t close = rest.find(')');
      // a decoration list closes before the subject; a subject that merely starts with "(" has no close here or
      // is the subject itself - only " (...)" right after the id is taken
      if (close != std::string::npos) return decorations(rest.substr(0, close + 1)) + rest.substr(close + 1);
    }
    return rest;
  }

  std::string colored(const std::string& raw) {
    if (!raw.empty() && raw[0] == '\033') return raw;   // the command colored it itself (a patch, a stat)
    std::string nl;
    std::string l = raw;
    if (!l.empty() && l.back() == '\n') { l.pop_back(); nl = "\n"; }
    std::string prefix;
    if (graph_) {
      size_t k = 0;
      while (k < l.size() && (l[k] == '*' || l[k] == '|' || l[k] == '/' || l[k] == '\\' || l[k] == ' ' || l[k] == '_')) ++k;
      // a diff line keeps its own first character: "| +added" is graph "| " then "+added"
      if (k > 0 && k < l.size() && (l[k - 1] == ' ')) { prefix = l.substr(0, k); l = l.substr(k); }
      else if (k == l.size()) return raw;
    }
    std::string body;
    if (mode_ == Mode::Log && !custom_ && starts(l, 0, "commit ")) {
      size_t e = 0;
      hex_run(l, 7, &e);
      in_hunk_ = in_header_ = false;
      body = std::string(kCommit) + l.substr(0, e) + kReset + after_id(l.substr(e));
    } else if (mode_ == Mode::Log && !custom_ && oneline_ && !in_hunk_ && !in_header_ && [&] { size_t e; return hex_run(l, 0, &e) && (e == l.size() || l[e] == ' '); }()) {
      size_t e = 0;
      hex_run(l, 0, &e);
      body = std::string(kCommit) + l.substr(0, e) + kReset + after_id(l.substr(e));
    } else if (starts(l, 0, "diff --git ") || starts(l, 0, "diff --cc ") || starts(l, 0, "diff --combined ")) {
      in_hunk_ = false;
      in_header_ = true;
      body = std::string(kMeta) + l + kReset;
    } else if (starts(l, 0, "@@")) {
      size_t close = l.find("@@", l.find_first_not_of('@'));
      size_t end = close == std::string::npos ? l.size() : l.find_first_not_of('@', close);
      if (end == std::string::npos) end = l.size();
      in_hunk_ = true;
      in_header_ = false;
      body = std::string(kFrag) + l.substr(0, end) + kReset + l.substr(end);
    } else if (in_hunk_ && !l.empty() && (l[0] == '+' || l[0] == '-' || l[0] == ' ' || l[0] == '\\')) {
      body = l[0] == '+' ? std::string(kNew) + l + kReset : l[0] == '-' ? std::string(kOld) + l + kReset : l;
    } else if (in_header_) {
      body = std::string(kMeta) + l + kReset;
    } else {
      in_hunk_ = false;
      body = l;
    }
    return prefix + body + nl;
  }

  Ctx& c_;
  std::streambuf* dest_;
  Mode mode_;
  bool color_, graph_, oneline_, custom_, stop_;
  bool in_hunk_ = false, in_header_ = false;
  std::string line_;
};

bool stdout_is_terminal() {
#ifdef _WIN32
  return _isatty(_fileno(stdout)) != 0;
#else
  return ::isatty(1) != 0;
#endif
}

// "always" / "true" / "auto" / "never" / "false" (or a boolean) to on, off, or auto (-1).
int color_setting(const std::string& v) {
  if (v == "always" || v == "true" || v == "yes" || v == "on" || v == "1") return 1;
  if (v == "never" || v == "false" || v == "no" || v == "off" || v == "0") return 0;
  return -1;
}

}  // namespace

Terminal::Terminal(Ctx& c, std::ostream& tout, std::ostream& real, bool real_is_stdout, const PagerOptions& opt)
    : tout_(tout) {
  if (c.args.empty()) return;
  const std::string cmd = c.args[0];
  const std::string sub = c.args.size() > 1 ? c.args[1] : "";
  const bool log_like = cmd == "log" || cmd == "show" || cmd == "whatchanged" || cmd == "reflog";
  const bool diff_like = cmd == "diff";
  const bool pageable = log_like || diff_like || cmd == "blame" || cmd == "shortlog" || cmd == "grep" ||
                        ((cmd == "change" || cmd == "op") && sub == "log");
  if (!pageable) return;

  if (!c.repo) c.try_open_repo();
  if (!c.repo) c.load_config();

  bool graph = false, oneline = false, custom = false;
  int color = -1;
  for (size_t i = 1; i < c.args.size(); ++i) {
    const std::string& a = c.args[i];
    if (a == "--") break;
    if (a == "--graph") graph = true;
    else if (a == "--oneline" || a == "--pretty=oneline" || a == "--format=oneline") oneline = true;
    else if (a.rfind("--format", 0) == 0 || a.rfind("--pretty=format:", 0) == 0 || a.rfind("--pretty=tformat:", 0) == 0) custom = true;
    else if (a == "--color") color = 1;
    else if (a.rfind("--color=", 0) == 0) color = color_setting(a.substr(8));
    else if (a == "--no-color") color = 0;
  }
  // The colors are this filter's to write: the command itself never sees --color (log and diff refused it).
  for (size_t i = 1; i < c.args.size();) {
    const std::string& a = c.args[i];
    if (a == "--") break;
    if (a == "--color" || a.rfind("--color=", 0) == 0 || a == "--no-color") c.args.erase(c.args.begin() + (long)i);
    else ++i;
  }

  // the pager: GIT_PAGER, core.pager, PAGER, less; "cat" or empty means none; pager.<command> can turn it off
  bool page = !opt.never && real_is_stdout && stdout_is_terminal();
  std::string pager;
  if (page) {
    if (auto pc = c.config.get("pager." + cmd)) {
      const int s = color_setting(*pc);
      if (s == 0) page = false;
      else if (s < 0) pager = *pc;
    }
    if (pager.empty()) pager = c.getenv("GIT_PAGER");
    if (pager.empty() && !c.hasenv("GIT_PAGER")) {
      if (auto cp = c.config.get("core.pager")) pager = *cp;
      else pager = c.getenv("PAGER");
      if (pager.empty() && !c.hasenv("PAGER")) pager = "less";
    }
    if (pager.empty() || pager == "cat") page = false;
  }
#ifdef _WIN32
  page = false;   // no pager process on Windows yet: the output goes straight to the console
#endif
  (void)opt.force;   // -p asks for the pager; it still needs a terminal, as git's does

  if (color < 0) {
    std::string v;
    // the command's own key (log and show color as diff does), then color.ui
    const char* key = diff_like || log_like ? "color.diff" : cmd == "grep" ? "color.grep" : "color.ui";
    if (auto cv = c.config.get(key)) v = *cv;
    else if (auto ui = c.config.get("color.ui")) v = *ui;
    color = v.empty() ? -1 : color_setting(v);
  }
  const bool on_terminal = (real_is_stdout && stdout_is_terminal()) || page;
#ifdef _WIN32
  const bool use_color = color == 1;   // a console only shows the escapes when asked to: --color or color.ui=always
#else
  const bool use_color = color == 1 || (color < 0 && on_terminal);
#endif
  c.terminal = on_terminal;
  c.color = use_color;

#ifndef _WIN32
  if (page) {
    int fds[2];
    if (::pipe(fds) == 0) {
      real.flush();
      std::fflush(stdout);
      const pid_t pid = ::fork();
      if (pid == 0) {
        ::dup2(fds[0], 0);
        ::close(fds[0]);
        ::close(fds[1]);
        if (!std::getenv("LESS")) ::setenv("LESS", "FRX", 1);   // quit when it fits, colors through, no screen clear
        if (!std::getenv("LV")) ::setenv("LV", "-c", 1);
        ::execl("/bin/sh", "sh", "-c", pager.c_str(), (char*)nullptr);
        ::_exit(127);
      }
      if (pid > 0) {
        ::dup2(fds[1], 1);
        ::signal(SIGPIPE, SIG_IGN);   // a pager that quits early is a refused write, handled by the filter
        pager_pid_ = pid;
      }
      ::close(fds[0]);
      ::close(fds[1]);
    }
  }
#endif

  if (!use_color && pager_pid_ < 0) return;   // nothing to change: the command writes straight through
  const auto mode = log_like ? LineFilter::Mode::Log : diff_like ? LineFilter::Mode::Diff : LineFilter::Mode::Plain;
  real_buf_ = tout_.rdbuf();
  filter_ = std::make_unique<LineFilter>(c, real.rdbuf(), mode, use_color, graph, oneline, custom, pager_pid_ >= 0);
  tout_.rdbuf(filter_.get());
  if (pager_pid_ >= 0) tout_.exceptions(std::ios::badbit);   // a refused write (Exit) leaves the command, not swallowed
}

Terminal::~Terminal() { finish(); }

void Terminal::finish() {
  tout_.exceptions(std::ios::goodbit);
  try { tout_.flush(); } catch (...) {}
  if (filter_) {
    tout_.rdbuf(real_buf_);   // the stream outlives this: never left pointing at the freed filter
    tout_.clear();
    filter_.reset();
  }
#ifndef _WIN32
  if (pager_pid_ >= 0) {
    std::cout.flush();
    std::fflush(stdout);
    // the pager reads to end of input: nothing may hold the pipe open
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) { ::dup2(devnull, 1); ::close(devnull); }
    else ::close(1);
    int status = 0;
    while (::waitpid((pid_t)pager_pid_, &status, 0) < 0 && errno == EINTR) {}
    pager_pid_ = -1;
  }
#endif
}

}  // namespace aios_git
