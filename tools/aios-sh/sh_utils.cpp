// aios-sh's utilities: the commands a shell script expects to find, written here (grep, sed and awk have
// files of their own). Each takes argv, the shell's state (for cwd and variables) and three streams; paths
// are resolved against the shell's cwd, never the process's.
#include "sh.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <set>
#include <sstream>

#include "aios/userland/git_merge.hpp"
#include "../aios-git/git_cli.hpp"

namespace aiossh {

int util_grep(Shell&, State&, const std::vector<std::string>&, const Io&);   // sh_grep.cpp
int util_sed(Shell&, State&, const std::vector<std::string>&, const Io&);    // sh_sed.cpp
int util_awk(Shell&, State&, const std::vector<std::string>&, const Io&);    // sh_awk.cpp
int u_sha_md5(Shell&, State&, const std::vector<std::string>&, const Io&);   // sh_hash.cpp

namespace {

void err(const Io& io, const std::string& who, const std::string& msg) { io.ewrite(who + ": " + msg + "\n"); }

// Short options: "-abc" is -a -b -c; "--" ends them; "-" alone is an operand. `with_value` names the
// letters that take a value (attached, "-n5", or the next argument).
struct Opts {
  std::map<char, std::string> v;     // letter -> value ("" for a flag)
  std::map<std::string, std::string> longs;   // --name or --name=value
  std::vector<std::string> args;
  bool bad = false;
  std::string bad_opt;
  bool has(char c) const { return v.count(c) > 0; }
  std::string get(char c, const std::string& d = "") const { auto it = v.find(c); return it == v.end() ? d : it->second; }
};
Opts parse_opts(const std::vector<std::string>& argv, const std::string& flags, const std::string& with_value,
                bool stop_at_first_operand = false, bool numeric_minus = false) {
  Opts o;
  size_t i = 1;
  for (; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (a == "--") { ++i; break; }
    if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
      size_t eq = a.find('=');
      o.longs[a.substr(2, eq == std::string::npos ? std::string::npos : eq - 2)] = eq == std::string::npos ? "" : a.substr(eq + 1);
      continue;
    }
    if (a.size() < 2 || a[0] != '-') { if (stop_at_first_operand) break; o.args.push_back(a); continue; }
    if (numeric_minus && std::isdigit(static_cast<unsigned char>(a[1]))) { o.v['n'] = a.substr(1); continue; }   // head -5
    for (size_t k = 1; k < a.size(); ++k) {
      char c = a[k];
      if (with_value.find(c) != std::string::npos) {
        std::string val = a.substr(k + 1);
        if (val.empty() && i + 1 < argv.size()) val = argv[++i];
        o.v[c] = val;
        break;
      }
      if (flags.find(c) == std::string::npos) { o.bad = true; o.bad_opt = std::string(1, c); }
      o.v[c] = "";
    }
  }
  for (; i < argv.size(); ++i) o.args.push_back(argv[i]);
  return o;
}

// The contents of each operand ("-" or none: stdin), in order. Calls fn(name, data); returns 1 if any failed.
int each_input(State& st, const std::vector<std::string>& files, const Io& io, const std::string& who,
               const std::function<void(const std::string&, const std::string&)>& fn) {
  int status = 0;
  if (files.empty()) { fn("-", io.read_all()); return 0; }
  for (const auto& f : files) {
    if (f == "-") { fn(f, io.read_all()); continue; }
    std::string p = host_path(st.cwd, f);
    if (os::is_dir(p)) { err(io, who, f + ": Is a directory"); status = 1; continue; }
    std::string data, e;
    if (!os::read_whole(p, data, &e)) { err(io, who, f + ": " + e); status = 1; continue; }
    fn(f, data);
  }
  return status;
}

// Each operand ("-" or none: stdin) as an open stream, in order: fn(name, fd) reads what it needs from it a block
// or a line at a time, so a utility never holds a whole input and stops reading when its reader goes. A file is
// closed again after fn, or when fn throws. Returns 1 if any could not be opened.
int each_stream(State& st, const std::vector<std::string>& files, const Io& io, const std::string& who,
                const std::function<void(const std::string&, Fd)>& fn) {
  int status = 0;
  if (files.empty()) { fn("-", io.in); return 0; }
  for (const auto& f : files) {
    if (f == "-") { fn(f, io.in); continue; }
    std::string p = host_path(st.cwd, f), e;
    if (os::is_dir(p)) { err(io, who, f + ": Is a directory"); status = 1; continue; }
    FdCloser fd(os::open_file(p, os::Open::Read, &e));
    if (fd.f == kNoFd) { err(io, who, f + ": " + e); status = 1; continue; }
    fn(f, fd.f);
  }
  return status;
}

// Output gathered into blocks, since a write per line is a system call per line. flush() is called by the
// utility when it is done (a destructor must not write: a failed write throws).
struct BufferedOut {
  const Io& io;
  std::string buf;
  void put(const std::string& s) { buf += s; if (buf.size() >= (1u << 16)) flush(); }
  void flush() { if (buf.empty()) return; std::string b; b.swap(buf); io.write(b); }
};

// Each line of a stream to fn(line, had_newline). What is gathered in `out` is flushed before every read that
// may wait, so lines from a slow producer come out as they arrive rather than at the end.
void each_line(Fd f, BufferedOut& out, const std::function<void(const std::string&, bool)>& fn) {
  LineReader r(f);
  std::string line;
  bool nl = false;
  for (;;) {
    if (!r.ready()) out.flush();
    if (!r.next(line, &nl)) break;
    fn(line, nl);
  }
}

std::string escape_c(const std::string& s, bool* stop = nullptr) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
    char c = s[++i];
    switch (c) {
      case 'n': o += '\n'; break;
      case 't': o += '\t'; break;
      case 'r': o += '\r'; break;
      case 'a': o += '\a'; break;
      case 'b': o += '\b'; break;
      case 'f': o += '\f'; break;
      case 'v': o += '\v'; break;
      case 'e': o += '\x1b'; break;
      case '\\': o += '\\'; break;
      case 'c': if (stop) *stop = true; return o;
      case '0': {
        int v = 0, n = 0;
        while (n < 3 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7') { v = v * 8 + (s[++i] - '0'); ++n; }
        o += static_cast<char>(v);
        break;
      }
      case 'x': {
        int v = 0, n = 0;
        while (n < 2 && i + 1 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1]))) { char h = s[++i]; v = v * 16 + (std::isdigit(static_cast<unsigned char>(h)) ? h - '0' : (std::tolower(h) - 'a' + 10)); ++n; }
        o += static_cast<char>(v);
        break;
      }
      default: o += '\\'; o += c;
    }
  }
  return o;
}

// ---------------------------------------------------------------------------------------------------

int u_true(Shell&, State&, const std::vector<std::string>&, const Io&) { return 0; }
int u_false(Shell&, State&, const std::vector<std::string>&, const Io&) { return 1; }

int u_echo(Shell&, State&, const std::vector<std::string>& argv, const Io& io) {
  bool newline = true, escapes = false;
  size_t i = 1;
  for (; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (a.size() < 2 || a[0] != '-' || a.find_first_not_of("neE", 1) != std::string::npos) break;
    for (size_t k = 1; k < a.size(); ++k) { if (a[k] == 'n') newline = false; else if (a[k] == 'e') escapes = true; else escapes = false; }
  }
  std::string out;
  bool stop = false;
  for (size_t k = i; k < argv.size() && !stop; ++k) out += (k > i ? " " : "") + (escapes ? escape_c(argv[k], &stop) : argv[k]);
  if (newline && !stop) out += '\n';
  io.write(out);
  return 0;
}

int u_printf(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  if (argv.size() < 2) { err(io, "printf", "usage: printf format [arguments]"); return 2; }
  size_t first = 1;
  std::string var;
  if (argv[1] == "-v" && argv.size() > 3) { var = argv[2]; first = 3; }
  const std::string fmt = argv[first];
  std::vector<std::string> args(argv.begin() + static_cast<long>(first) + 1, argv.end());
  std::string out;
  size_t ai = 0;
  int status = 0;
  bool stop = false;
  do {
    for (size_t i = 0; i < fmt.size() && !stop; ++i) {
      char c = fmt[i];
      if (c == '\\') {
        size_t j = i + 1;
        std::string esc = "\\";
        if (j < fmt.size()) { esc += fmt[j]; if (fmt[j] == '0' || fmt[j] == 'x') while (j + 1 < fmt.size() && esc.size() < 5 && std::isxdigit(static_cast<unsigned char>(fmt[j + 1]))) esc += fmt[++j]; }
        out += escape_c(esc, &stop);
        i = j;
        continue;
      }
      if (c != '%') { out += c; continue; }
      if (i + 1 < fmt.size() && fmt[i + 1] == '%') { out += '%'; ++i; continue; }
      size_t j = i + 1;
      std::string spec = "%";
      while (j < fmt.size() && std::strchr("-+ #0", fmt[j])) spec += fmt[j++];
      while (j < fmt.size() && (std::isdigit(static_cast<unsigned char>(fmt[j])) || fmt[j] == '*')) {
        if (fmt[j] == '*') { spec += ai < args.size() ? args[ai++] : "0"; ++j; } else spec += fmt[j++];
      }
      if (j < fmt.size() && fmt[j] == '.') { spec += fmt[j++]; while (j < fmt.size() && (std::isdigit(static_cast<unsigned char>(fmt[j])) || fmt[j] == '*')) { if (fmt[j] == '*') { spec += ai < args.size() ? args[ai++] : "0"; ++j; } else spec += fmt[j++]; } }
      while (j < fmt.size() && std::strchr("hlLqjzt", fmt[j])) ++j;
      if (j >= fmt.size()) { out += spec; break; }
      char conv = fmt[j];
      i = j;
      std::string arg = ai < args.size() ? args[ai++] : "";
      // Sized to what the conversion produces: an argument (an agent's base64-encoded command) or a width
      // can be any length.
      auto put = [&out](const std::string& f, auto v) {
        int n = std::snprintf(nullptr, 0, f.c_str(), v);
        if (n <= 0) return;
        std::string b(static_cast<size_t>(n) + 1, '\0');
        std::snprintf(b.data(), b.size(), f.c_str(), v);
        out.append(b.data(), static_cast<size_t>(n));
      };
      if (conv == 's') put(spec + "s", arg.c_str());
      else if (conv == 'b') { out += escape_c(arg, &stop); }
      else if (conv == 'c') { out += arg.empty() ? std::string() : arg.substr(0, 1); }
      else if (conv == 'q') { out += "'" + arg + "'"; }
      else if (std::strchr("diouxXc", conv)) {
        long long v = 0;
        if (!arg.empty() && (arg[0] == '\'' || arg[0] == '"')) v = arg.size() > 1 ? static_cast<unsigned char>(arg[1]) : 0;
        else if (!arg.empty()) { char* e = nullptr; v = std::strtoll(arg.c_str(), &e, 0); if (e && *e) { err(io, "printf", arg + ": invalid number"); status = 1; } }
        put(spec + "ll" + conv, v);
      } else if (std::strchr("feEgGaA", conv)) {
        double v = arg.empty() ? 0.0 : std::strtod(arg.c_str(), nullptr);
        put(spec + conv, v);
      } else { out += spec; out += conv; }
    }
  } while (ai < args.size() && ai > 0 && !stop);
  if (!var.empty()) { st.set(var, out); return status; }
  io.write(out);
  return status;
}

// test / [ : POSIX expressions with ! -a -o and parentheses
struct Test {
  State& st;
  std::vector<std::string> a;
  size_t i = 0;
  bool error = false;
  bool unary(const std::string& op, const std::string& x) {
    std::string p = host_path(st.cwd, x);
    if (op == "-e" || op == "-a") return os::exists(p);
    if (op == "-f") return os::is_file(p);
    if (op == "-d") return os::is_dir(p);
    if (op == "-s") return os::is_file(p) && os::file_size(p) > 0;
    if (op == "-r" || op == "-w") return os::exists(p);
    if (op == "-x") return os::is_dir(p) || os::is_executable(p) || (os::is_file(p) && !os::find_program(x, "", st.cwd).empty());
    if (op == "-L" || op == "-h") return os::is_symlink(p);
    if (op == "-z") return x.empty();
    if (op == "-n") return !x.empty();
    if (op == "-t") return false;
    error = true;
    return false;
  }
  bool binary(const std::string& l, const std::string& op, const std::string& r) {
    if (op == "=" || op == "==") return l == r;
    if (op == "!=") return l != r;
    if (op == "<") return l < r;
    if (op == ">") return l > r;
    if (op == "-nt") return os::mtime(host_path(st.cwd, l)) > os::mtime(host_path(st.cwd, r));
    if (op == "-ot") return os::mtime(host_path(st.cwd, l)) < os::mtime(host_path(st.cwd, r));
    long long x = 0, y = 0;
    if (!parse_int(l, x) || !parse_int(r, y)) { error = true; return false; }
    if (op == "-eq") return x == y; if (op == "-ne") return x != y; if (op == "-lt") return x < y;
    if (op == "-le") return x <= y; if (op == "-gt") return x > y; if (op == "-ge") return x >= y;
    error = true;
    return false;
  }
  static bool is_binop(const std::string& s) {
    for (const char* o : {"=", "==", "!=", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt", "-ge", "-nt", "-ot"}) if (s == o) return true;
    return false;
  }
  static bool is_unop(const std::string& s) {
    for (const char* o : {"-e", "-f", "-d", "-s", "-r", "-w", "-x", "-L", "-h", "-z", "-n", "-t", "-a"}) if (s == o) return true;
    return false;
  }
  bool primary() {
    if (i >= a.size()) { error = true; return false; }
    if (a[i] == "!") { ++i; return !primary(); }
    if (a[i] == "(") {
      ++i;
      bool v = or_expr();
      if (i < a.size() && a[i] == ")") ++i; else error = true;
      return v;
    }
    if (i + 2 < a.size() + 0 && i + 1 < a.size() && is_binop(a[i + 1]) && i + 2 < a.size()) { bool v = binary(a[i], a[i + 1], a[i + 2]); i += 3; return v; }
    if (is_unop(a[i]) && i + 1 < a.size()) { bool v = unary(a[i], a[i + 1]); i += 2; return v; }
    return !a[i++].empty();
  }
  bool and_expr() { bool v = primary(); while (i < a.size() && a[i] == "-a") { ++i; bool r = primary(); v = v && r; } return v; }
  bool or_expr() { bool v = and_expr(); while (i < a.size() && a[i] == "-o") { ++i; bool r = and_expr(); v = v || r; } return v; }
};
int u_test(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  std::vector<std::string> a(argv.begin() + 1, argv.end());
  if (argv[0] == "[") {
    if (a.empty() || a.back() != "]") { err(io, "[", "missing ']'"); return 2; }
    a.pop_back();
  }
  if (a.empty()) return 1;
  Test t{st, a};
  // the POSIX shortcuts by argument count, which keep "-n" and "!" usable as operands
  bool v;
  if (a.size() == 1) v = !a[0].empty();
  else if (a.size() == 2 && a[0] == "!") v = a[1].empty();
  else if (a.size() == 2 && Test::is_unop(a[0])) v = t.unary(a[0], a[1]);
  else if (a.size() == 3 && Test::is_binop(a[1])) v = t.binary(a[0], a[1], a[2]);
  else { v = t.or_expr(); if (t.i != a.size()) t.error = true; }
  if (t.error) { err(io, argv[0], "bad expression"); return 2; }
  return v ? 0 : 1;
}

int u_pwd(Shell&, State& st, const std::vector<std::string>&, const Io& io) { io.write(st.get("PWD") + "\n"); return 0; }

int u_cat(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "nAbsuvEeT", "");
  bool number = o.has('n');
  long long ln = 0;
  if (!number) {
    // copied a block at a time, files too: `cat big | head` holds one block, and stops when head does
    std::vector<std::string> names = o.args.empty() ? std::vector<std::string>{"-"} : o.args;
    int status = 0;
    char buf[1 << 16];
    for (const auto& name : names) {
      Fd f = io.in;
      if (name != "-") {
        std::string p = host_path(st.cwd, name), e;
        if (os::is_dir(p)) { err(io, "cat", name + ": Is a directory"); status = 1; continue; }
        f = os::open_file(p, os::Open::Read, &e);
        if (f == kNoFd) { err(io, "cat", name + ": " + e); status = 1; continue; }
      }
      long n;
      try {
        while ((n = os::read_fd(f, buf, sizeof buf)) > 0) io.write(std::string(buf, static_cast<size_t>(n)));
      } catch (...) { if (f != io.in) os::close_fd(f); throw; }
      if (f != io.in) os::close_fd(f);
    }
    return status;
  }
  // -n: numbered a line at a time as the lines arrive, the count running on across files
  BufferedOut out{io, {}};
  int status = each_stream(st, o.args, io, "cat", [&](const std::string&, Fd f) {
    each_line(f, out, [&](const std::string& l, bool) { char b[16]; std::snprintf(b, sizeof b, "%6lld\t", ++ln); out.put(b + l + "\n"); });
  });
  out.flush();
  return status;
}

int u_head_tail(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  bool tail = argv[0] == "tail";
  Opts o = parse_opts(argv, "qvfF", "nc", false, true);
  std::string nspec = o.get('n', "10");
  bool from_start = tail && !nspec.empty() && nspec[0] == '+';
  bool all_but = !tail && !nspec.empty() && nspec[0] == '-';
  long long n = 10;
  if (!parse_int(nspec[0] == '+' || nspec[0] == '-' ? nspec.substr(1) : nspec, n)) { err(io, argv[0], "invalid number of lines: " + nspec); return 1; }
  bool bytes = o.has('c');
  // -c N, and as with lines: head -c -N is all but the last N bytes, tail -c +N starts at byte N.
  std::string cspec = o.get('c', "0");
  bool c_from_start = tail && !cspec.empty() && cspec[0] == '+';
  bool c_all_but = !tail && !cspec.empty() && cspec[0] == '-';
  long long cn = 0;
  if (bytes && !parse_int(c_from_start || c_all_but ? cspec.substr(1) : cspec, cn)) { err(io, argv[0], "invalid number of bytes: " + cspec); return 1; }
  bool headers = o.args.size() > 1 && !o.has('q');
  int status = 0;
  bool first_header = true;
  auto header = [&](BufferedOut& out, const std::string& name) {
    if (headers) out.put((first_header ? "" : "\n") + std::string("==> ") + name + " <==\n");
    first_header = false;
  };
  if (!tail && !bytes && !all_but) {
    // head on a stream or a file: stop reading once enough lines are out (a producer upstream then sees a closed pipe)
    BufferedOut out{io, {}};
    status = each_stream(st, o.args, io, argv[0], [&](const std::string& name, Fd f) {
      header(out, name);
      LineReader r(f);
      std::string line;
      bool nl = false;
      for (long long k = 0; k < n; ++k) {
        if (!r.ready()) out.flush();
        if (!r.next(line, &nl)) break;
        out.put(line + (nl ? "\n" : ""));
      }
    });
    out.flush();
    return status;
  }
  if (tail && !bytes) {
    BufferedOut out{io, {}};
    status = each_stream(st, o.args, io, argv[0], [&](const std::string& name, Fd f) {
      header(out, name);
      if (from_start) {
        // tail -n +N: the first N-1 lines skipped, the rest passed on as they arrive
        long long skip = n > 0 ? n - 1 : 0;
        each_line(f, out, [&](const std::string& l, bool nl) { if (skip > 0) --skip; else out.put(l + (nl ? "\n" : "")); });
        return;
      }
      // tail -n N: the input read once with only its last N lines kept, in a ring that drops the oldest
      std::deque<std::string> ring;
      LineReader r(f);
      std::string line;
      bool nl = false;
      while (r.next(line, &nl)) {
        if (n <= 0) continue;
        if (static_cast<long long>(ring.size()) == n) ring.pop_front();
        ring.push_back(line + (nl ? "\n" : ""));
      }
      for (const auto& l : ring) out.put(l);
    });
    out.flush();
    return status;
  }
  if (!bytes) {
    // head -n -N: all but the last N lines. A line is passed on once N newer ones have arrived behind it, so
    // only N lines are ever held.
    BufferedOut out{io, {}};
    status = each_stream(st, o.args, io, argv[0], [&](const std::string& name, Fd f) {
      header(out, name);
      std::deque<std::string> held;
      each_line(f, out, [&](const std::string& l, bool nl) {
        held.push_back(l + (nl ? "\n" : ""));
        if (static_cast<long long>(held.size()) > n) { out.put(held.front()); held.pop_front(); }
      });
    });
    out.flush();
    return status;
  }
  // The byte forms read a block at a time. What is gathered is written before each read, since a read may wait.
  BufferedOut out{io, {}};
  size_t c = static_cast<size_t>(std::max<long long>(cn, 0));
  std::vector<char> buf(1 << 16);
  status = each_stream(st, o.args, io, argv[0], [&](const std::string& name, Fd f) {
    header(out, name);
    if (!tail && !c_all_but) {
      // head -c N: only as many bytes as asked for are read (head -c 10 /dev/zero ends)
      size_t left = c;
      while (left > 0) {
        out.flush();
        long k = os::read_fd(f, buf.data(), std::min(left, buf.size()));
        if (k <= 0) break;
        out.put(std::string(buf.data(), static_cast<size_t>(k)));
        left -= static_cast<size_t>(k);
      }
      return;
    }
    // tail -c +N skips N-1 bytes and passes on the rest. head -c -N holds back the last N bytes seen and passes on
    // what falls out of them; tail -c N keeps them and writes them at the end. The held bytes are `held` from
    // `from` on: its front is dropped only once it is the larger part, so each byte is copied a bounded number of
    // times however small the reads are.
    size_t skip = c_from_start ? (c > 0 ? c - 1 : 0) : 0;
    std::string held;
    size_t from = 0;
    for (;;) {
      out.flush();
      long k = os::read_fd(f, buf.data(), buf.size());
      if (k <= 0) break;
      const char* p = buf.data();
      size_t len = static_cast<size_t>(k);
      if (c_from_start) {
        size_t s = std::min(skip, len);
        skip -= s;
        out.put(std::string(p + s, len - s));
        continue;
      }
      held.append(p, len);
      if (held.size() - from > c) {
        size_t extra = held.size() - from - c;
        if (!tail) out.put(held.substr(from, extra));
        from += extra;
      }
      if (from > held.size() / 2) { held.erase(0, from); from = 0; }
    }
    if (tail && !c_from_start) out.put(held.substr(from));
  });
  out.flush();
  return status;
}

int u_wc(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "lwcmL", "");
  bool l = o.has('l'), w = o.has('w'), c = o.has('c') || o.has('m');
  if (!l && !w && !c) l = w = c = true;
  struct Row { long long a, b, d; std::string name; };
  std::vector<Row> rows;
  long long tl = 0, tw = 0, tc = 0;
  // GNU's padding: one count of one input is not padded; otherwise every number is as wide as the digits of
  // the inputs' total size (7 when an input is not a regular file, standard input included)
  bool all_regular = !o.args.empty();
  uint64_t total_size = 0;
  for (const auto& f : o.args) {
    std::string p = host_path(st.cwd, f);
    if (f == "-" || !os::is_file(p)) all_regular = false; else total_size += os::file_size(p);
  }
  // counted a block at a time: a word that spans two blocks is still one word
  int status = each_stream(st, o.args, io, "wc", [&](const std::string& name, Fd f) {
    long long a = 0, b = 0, size = 0;
    bool in_word = false;
    char buf[1 << 16];
    long k;
    while ((k = os::read_fd(f, buf, sizeof buf)) > 0) {
      for (long i = 0; i < k; ++i) {
        char ch = buf[i];
        if (ch == '\n') ++a;
        bool sp = std::isspace(static_cast<unsigned char>(ch)) != 0;
        if (!sp && !in_word) ++b;
        in_word = !sp;
      }
      size += k;
    }
    tl += a; tw += b; tc += size;
    rows.push_back({a, b, size, name == "-" ? "" : name});
  });
  int counts = (l ? 1 : 0) + (w ? 1 : 0) + (c ? 1 : 0);
  size_t inputs = o.args.empty() ? 1 : o.args.size();
  int width = 1;
  if (!(counts == 1 && inputs == 1)) {
    if (!all_regular) width = 7;
    else for (uint64_t t = total_size; t >= 10; t /= 10) ++width;
  }
  if (rows.size() > 1) rows.push_back({tl, tw, tc, "total"});
  std::string out;
  for (const auto& r : rows) {
    std::vector<long long> v;
    if (l) v.push_back(r.a);
    if (w) v.push_back(r.b);
    if (c) v.push_back(r.d);
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
      char buf[48];
      std::snprintf(buf, sizeof buf, "%s%*lld", i ? " " : "", width, v[i]);
      s += buf;
    }
    if (!r.name.empty()) s += " " + r.name;
    out += s + "\n";
  }
  io.write(out);
  return status;
}

int u_mkdir(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "pv", "m");
  int status = 0;
  for (const auto& a : o.args) {
    std::string p = host_path(st.cwd, a);
    if (o.has('p')) {
      // each missing ancestor, top down
      std::vector<std::string> chain;
      std::string q = p;
      while (!q.empty() && !os::is_dir(q)) {
        chain.push_back(q);
        size_t cut = q.find_last_of("/\\");
        if (cut == std::string::npos || cut == 0 || (cut == 2 && q[1] == ':')) break;
        q = q.substr(0, cut);
      }
      for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        std::string e;
        if (!os::is_dir(*it) && !os::make_dir(*it, &e) && !os::is_dir(*it)) { err(io, "mkdir", a + ": " + e); status = 1; break; }
      }
      continue;
    }
    std::string e;
    if (os::exists(p)) { err(io, "mkdir", "cannot create directory '" + a + "': File exists"); status = 1; continue; }
    if (!os::make_dir(p, &e)) { err(io, "mkdir", "cannot create directory '" + a + "': " + e); status = 1; }
  }
  return status;
}

bool remove_tree(const std::string& p, std::string* e) {
  if (os::is_dir(p)) {
    std::vector<os::DirEntry> es;
    os::list_dir(p, es);
    for (const auto& d : es) {
      std::string c = p + "/" + d.name;
      if (d.dir && !d.link) { if (!remove_tree(c, e)) return false; }
      else if (d.dir && d.link) { if (!os::remove_dir(c, e)) return false; }
      else if (!os::remove_file(c, e)) return false;
    }
    return os::remove_dir(p, e);
  }
  return os::remove_file(p, e);
}

int u_rm(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "rRfidv", "");
  bool rec = o.has('r') || o.has('R'), force = o.has('f');
  int status = 0;
  if (o.args.empty() && !force) { err(io, "rm", "missing operand"); return 1; }
  for (const auto& a : o.args) {
    std::string p = host_path(st.cwd, a);
    if (!os::exists(p)) { if (!force) { err(io, "rm", "cannot remove '" + a + "': No such file or directory"); status = 1; } continue; }
    std::string e;
    if (os::is_dir(p)) {
      if (!rec && !o.has('d')) { err(io, "rm", "cannot remove '" + a + "': Is a directory"); status = 1; continue; }
      if (!(rec ? remove_tree(p, &e) : os::remove_dir(p, &e))) { err(io, "rm", "cannot remove '" + a + "': " + e); status = 1; }
      continue;
    }
    if (!os::remove_file(p, &e)) { err(io, "rm", "cannot remove '" + a + "': " + e); status = 1; }
  }
  return status;
}

int u_rmdir(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "pv", "");
  int status = 0;
  for (const auto& a : o.args) { std::string e; if (!os::remove_dir(host_path(st.cwd, a), &e)) { err(io, "rmdir", "failed to remove '" + a + "': " + e); status = 1; } }
  return status;
}

std::string base_name(std::string p) {
  while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
  size_t cut = p.find_last_of("/\\");
  return cut == std::string::npos ? p : p.substr(cut + 1);
}

bool copy_tree(const std::string& from, const std::string& to, std::string* e) {
  if (!os::is_dir(from)) return os::copy_file(from, to, e);
  if (!os::is_dir(to) && !os::make_dir(to, e)) return false;
  std::vector<os::DirEntry> es;
  os::list_dir(from, es);
  for (const auto& d : es) if (!copy_tree(from + "/" + d.name, to + "/" + d.name, e)) return false;
  return true;
}

int u_cp_mv(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  bool mv = argv[0] == "mv";
  Opts o = parse_opts(argv, "rRafinpvuTL", "t");
  std::vector<std::string> srcs = o.args;
  std::string dest;
  if (o.has('t')) dest = o.get('t');
  else { if (srcs.size() < 2) { err(io, argv[0], "missing destination file operand"); return 1; } dest = srcs.back(); srcs.pop_back(); }
  std::string dp = host_path(st.cwd, dest);
  bool into = os::is_dir(dp);
  if (srcs.size() > 1 && !into) { err(io, argv[0], "target '" + dest + "' is not a directory"); return 1; }
  int status = 0;
  for (const auto& s : srcs) {
    std::string sp = host_path(st.cwd, s);
    if (!os::exists(sp)) { err(io, argv[0], "cannot stat '" + s + "': No such file or directory"); status = 1; continue; }
    std::string target = into ? dp + "/" + base_name(s) : dp;
    if (o.has('n') && os::exists(target)) continue;
    std::string e;
    if (mv) {
      if (os::is_dir(target) && !into) { err(io, "mv", "cannot overwrite directory '" + dest + "'"); status = 1; continue; }
      if (!os::rename_path(sp, target, &e)) {
        // across volumes, or a directory MoveFileEx will not move: copy, then remove
        if (!(copy_tree(sp, target, &e) && remove_tree(sp, &e))) { err(io, "mv", "cannot move '" + s + "': " + e); status = 1; }
      }
      continue;
    }
    if (os::is_dir(sp) && !(o.has('r') || o.has('R') || o.has('a'))) { err(io, "cp", "-r not specified; omitting directory '" + s + "'"); status = 1; continue; }
    if (!copy_tree(sp, target, &e)) { err(io, "cp", "cannot copy '" + s + "': " + e); status = 1; }
  }
  return status;
}

int u_touch(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "acm", "dtr");
  int status = 0;
  for (const auto& a : o.args) {
    std::string p = host_path(st.cwd, a), e;
    if (o.has('c') && !os::exists(p)) continue;
    if (!os::touch(p, &e)) { err(io, "touch", "cannot touch '" + a + "': " + e); status = 1; }
  }
  return status;
}

int u_ln(Shell& sh, State& st, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "sfnvT", "");
  if (o.args.size() < 2) { err(io, "ln", "missing operand"); return 1; }
#ifdef _WIN32
  // No symbolic links an unprivileged Windows process may make: a copy stands in.
  std::vector<std::string> a = {"cp", "-r", o.args[0], o.args[1]};
  if (o.has('f')) { std::string t = host_path(st.cwd, o.args[1]); std::string e; if (os::is_file(t)) os::remove_file(t, &e); }
  return u_cp_mv(sh, st, a, io);
#else
  (void)sh;
  // ln [-s] [-f] [-n] TARGET LINK: a symbolic link holds TARGET as written (it is resolved where the link lives); a hard
  // link names TARGET's file. LINK an existing directory (followed unless -n or -T) means a link inside it.
  const std::string& target = o.args[0];
  std::string link = host_path(st.cwd, o.args[1]);
  if (os::is_dir(link) && !o.has('T') && !(o.has('n') && os::is_symlink(link))) link += "/" + base_name(target);
  if (o.has('f') && (os::is_symlink(link) || os::is_file(link))) { std::string e; os::remove_file(link, &e); }
  std::string e;
  if (!os::make_link(o.has('s') ? target : host_path(st.cwd, target), link, o.has('s'), &e)) {
    err(io, "ln", "failed to create " + std::string(o.has('s') ? "symbolic " : "hard ") + "link '" + o.args[1] + "': " + e);
    return 1;
  }
  return 0;
#endif
}

int u_noop(Shell&, State&, const std::vector<std::string>&, const Io&) { return 0; }   // chmod, chown, sync

int u_basename(Shell&, State&, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "az", "s");
  if (o.args.empty()) { err(io, "basename", "missing operand"); return 1; }
  std::vector<std::string> names = o.has('a') || o.has('s') ? o.args : std::vector<std::string>{o.args[0]};
  std::string suffix = o.has('s') ? o.get('s') : (!o.has('a') && o.args.size() > 1 ? o.args[1] : "");
  for (const auto& n : names) {
    std::string b = base_name(n);
    if (!suffix.empty() && b.size() > suffix.size() && b.compare(b.size() - suffix.size(), suffix.size(), suffix) == 0) b.resize(b.size() - suffix.size());
    io.write(b + "\n");
  }
  return 0;
}

int u_dirname(Shell&, State&, const std::vector<std::string>& argv, const Io& io) {
  for (size_t i = 1; i < argv.size(); ++i) {
    std::string p = argv[i];
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    size_t cut = p.find_last_of("/\\");
    std::string d = cut == std::string::npos ? "." : cut == 0 ? p.substr(0, 1) : p.substr(0, cut);
    if (d.size() == 2 && d[1] == ':') d += '/';
    io.write(d + "\n");
  }
  return argv.size() > 1 ? 0 : 1;
}

std::string shown(const std::string& host) {
  std::string s = host;
#ifdef _WIN32
  for (auto& c : s) if (c == '\\') c = '/';
#endif
  return s;
}

int u_realpath(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "emsqzLP", "");
  int status = 0;
  for (const auto& a : o.args) {
    std::string p = host_path(st.cwd, a);
    if (o.has('e') && !os::exists(p)) { err(io, argv[0], a + ": No such file or directory"); status = 1; continue; }
    io.write(shown(o.has('s') ? p : os::resolved(p)) + "\n");
  }
  return status;
}

int u_readlink(Shell& sh, State& st, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "femnqsvz", "");
  if (o.has('f') || o.has('e') || o.has('m')) return u_realpath(sh, st, argv, io);
  int status = 0;
  for (const auto& a : o.args) {
    std::string t;
    if (!os::read_link(host_path(st.cwd, a), &t)) { status = 1; continue; }   // not a link: nothing printed, as readlink
    io.write(t + (o.has('n') ? "" : "\n"));
  }
  return status;
}

int u_env(Shell& sh, State& st, const std::vector<std::string>& argv, const Io& io) {
  State copy = st;
  size_t i = 1;
  for (; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (a == "-i") { for (auto& [k, v] : copy.vars) v.exported = false; continue; }
    if (a == "-u" && i + 1 < argv.size()) { copy.vars.erase(argv[++i]); continue; }
    size_t eq = a.find('=');
    if (eq != std::string::npos && eq > 0) { copy.set(a.substr(0, eq), a.substr(eq + 1)); copy.vars[a.substr(0, eq)].exported = true; continue; }
    break;
  }
  if (i >= argv.size()) {
    for (const auto& kv : copy.env_list()) io.write(kv + "\n");
    return 0;
  }
  return run_utility_by_name(sh, copy, std::vector<std::string>(argv.begin() + static_cast<long>(i), argv.end()), io);
}

int u_printenv(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  if (argv.size() == 1) { for (const auto& kv : st.env_list()) io.write(kv + "\n"); return 0; }
  int status = 0;
  for (size_t i = 1; i < argv.size(); ++i) { if (st.has(argv[i])) io.write(st.get(argv[i]) + "\n"); else status = 1; }
  return status;
}

int u_sleep(Shell&, State&, const std::vector<std::string>& argv, const Io& io) {
  double total = 0;
  for (size_t i = 1; i < argv.size(); ++i) {
    std::string a = argv[i];
    double mul = 1;
    if (!a.empty() && std::strchr("smhd", a.back())) { char u = a.back(); a.pop_back(); mul = u == 'm' ? 60 : u == 'h' ? 3600 : u == 'd' ? 86400 : 1; }
    char* e = nullptr;
    double v = std::strtod(a.c_str(), &e);
    if (!e || *e) { err(io, "sleep", "invalid time interval '" + argv[i] + "'"); return 1; }
    total += v * mul;
  }
  os::sleep_ms(static_cast<long>(total * 1000));
  return 0;
}

int u_which(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  int status = 0;
  for (size_t i = 1; i < argv.size(); ++i) {
    if (argv[i] == "-a" || argv[i] == "-s") continue;
    if (argv[i] == "git") {
      std::string p = host_git(st);
      io.write(p.empty() ? "git: aios-sh builtin\n" : shown(p) + "\n");
      continue;
    }
    if (utilities().count(argv[i])) { io.write(argv[i] + ": aios-sh builtin\n"); continue; }
    std::string p = os::find_program(argv[i], st.get("PATH"), st.cwd);
    if (p.empty()) { status = 1; continue; }
    io.write(shown(p) + "\n");
  }
  return status;
}

int u_seq(Shell&, State&, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "w", "sf");
  std::vector<double> n;
  for (const auto& a : o.args) n.push_back(std::strtod(a.c_str(), nullptr));
  double first = 1, step = 1, last = 1;
  if (n.size() == 1) last = n[0];
  else if (n.size() == 2) { first = n[0]; last = n[1]; }
  else if (n.size() >= 3) { first = n[0]; step = n[1]; last = n[2]; }
  else { err(io, "seq", "missing operand"); return 1; }
  if (step == 0) return 1;
  std::string sep = o.has('s') ? o.get('s') : "\n";
  bool integral = true;
  for (const auto& a : o.args) if (a.find('.') != std::string::npos) integral = false;
  std::string out;
  bool firstv = true;
  for (double v = first; step > 0 ? v <= last + 1e-9 : v >= last - 1e-9; v += step) {
    if (!firstv) out += sep;
    firstv = false;
    char buf[64];
    if (integral) std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(std::llround(v)));
    else std::snprintf(buf, sizeof buf, "%g", v);
    out += buf;
    if (out.size() > (1 << 16)) { io.write(out); out.clear(); }
  }
  if (!firstv) out += "\n";
  io.write(out);
  return 0;
}

int u_date(Shell&, State&, const std::vector<std::string>& argv, const Io& io) {
  std::string fmt = "%a %b %e %H:%M:%S %Z %Y";
  bool utc = false;
  std::time_t t = std::time(nullptr);
  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (a == "-u" || a == "--utc") utc = true;
    else if (a.size() > 1 && a[0] == '+') fmt = a.substr(1);
    else if (a == "-I" || a == "--iso-8601") fmt = "%Y-%m-%d";
    else if ((a == "-d" || a == "--date") && i + 1 < argv.size()) { std::string d = argv[++i]; if (!d.empty() && d[0] == '@') t = static_cast<std::time_t>(std::strtoll(d.c_str() + 1, nullptr, 10)); }
  }
  // %s (seconds since the epoch) and %N (nanoseconds) by hand; strftime knows neither everywhere
  std::string f2;
  for (size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] == '%' && i + 1 < fmt.size() && fmt[i + 1] == 's') { f2 += std::to_string(static_cast<long long>(t)); ++i; continue; }
    if (fmt[i] == '%' && i + 1 < fmt.size() && fmt[i + 1] == 'N') {
      auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count() % 1000000000;
      char b[16]; std::snprintf(b, sizeof b, "%09lld", static_cast<long long>(ns)); f2 += b; ++i; continue;
    }
    f2 += fmt[i];
  }
  std::tm tm{};
#ifdef _WIN32
  if (utc) gmtime_s(&tm, &t); else localtime_s(&tm, &t);
#else
  if (utc) gmtime_r(&t, &tm); else localtime_r(&t, &tm);
#endif
  char buf[512];
  size_t n = std::strftime(buf, sizeof buf, f2.c_str(), &tm);
  io.write(std::string(buf, n) + "\n");
  return 0;
}

int u_tee(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "ai", "");
  std::vector<Fd> outs;
  int status = 0;
  for (const auto& f : o.args) {
    std::string e;
    Fd d = os::open_file(host_path(st.cwd, f), o.has('a') ? os::Open::Append : os::Open::Write, &e);
    if (d == kNoFd) { err(io, "tee", f + ": " + e); status = 1; continue; }
    outs.push_back(d);
  }
  char buf[1 << 16];
  long n;
  while ((n = os::read_fd(io.in, buf, sizeof buf)) > 0) {
    for (Fd d : outs) os::write_all(d, buf, static_cast<size_t>(n));
    io.write(std::string(buf, static_cast<size_t>(n)));
  }
  for (Fd d : outs) os::close_fd(d);
  return status;
}

int u_yes(Shell&, State&, const std::vector<std::string>& argv, const Io& io) {
  std::string line = argv.size() > 1 ? join(std::vector<std::string>(argv.begin() + 1, argv.end()), " ") : "y";
  std::string chunk;
  while (chunk.size() < 8192) chunk += line + "\n";
  for (;;) io.write(chunk);   // ends with BrokenPipe when the reader goes
}

int u_sort(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "nrufbdhVsMRg", "ktTo");
  std::vector<std::string> lines;
  int status = each_input(st, o.args, io, "sort", [&](const std::string&, const std::string& d) {
    for (auto& l : split_lines(d)) { if (!l.empty() && l.back() == '\r') l.pop_back(); lines.push_back(l); }
  });
  std::string sep = o.get('t');
  int key = 0, key_end = 0;
  if (o.has('k')) {
    std::string k = o.get('k');
    key = std::atoi(k.c_str());
    size_t comma = k.find(',');
    key_end = comma == std::string::npos ? 0 : std::atoi(k.c_str() + comma + 1);
    for (char c : k) { if (c == 'n') o.v['n'] = ""; if (c == 'r') o.v['r'] = ""; }
  }
  auto field = [&](const std::string& l) -> std::string {
    if (!key) return l;
    std::vector<std::string> f;
    if (sep.empty()) { std::istringstream in(l); std::string w; while (in >> w) f.push_back(w); }
    else { size_t a = 0; for (;;) { size_t b = l.find(sep[0], a); f.push_back(l.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; } }
    if (key > static_cast<int>(f.size())) return "";
    if (!key_end || key_end == key) return f[static_cast<size_t>(key - 1)];
    std::string s;
    for (int i = key; i <= std::min<int>(key_end, static_cast<int>(f.size())); ++i) s += (i > key ? (sep.empty() ? " " : sep) : "") + f[static_cast<size_t>(i - 1)];
    return s;
  };
  bool numeric = o.has('n') || o.has('g') || o.has('h'), fold = o.has('f'), version = o.has('V');
  auto num = [](const std::string& s) { const char* p = s.c_str(); while (*p == ' ' || *p == '\t') ++p; double v = std::strtod(p, nullptr); size_t L = std::strlen(p); if (L && std::strchr("KMGT", p[L - 1])) v *= p[L - 1] == 'K' ? 1e3 : p[L - 1] == 'M' ? 1e6 : p[L - 1] == 'G' ? 1e9 : 1e12; return v; };
  auto vers = [](const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
      if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
        size_t i2 = i, j2 = j;
        while (i2 < a.size() && std::isdigit(static_cast<unsigned char>(a[i2]))) ++i2;
        while (j2 < b.size() && std::isdigit(static_cast<unsigned char>(b[j2]))) ++j2;
        long long x = std::stoll(a.substr(i, i2 - i)), y = std::stoll(b.substr(j, j2 - j));
        if (x != y) return x < y ? -1 : 1;
        i = i2; j = j2;
        continue;
      }
      if (a[i] != b[j]) return a[i] < b[j] ? -1 : 1;
      ++i; ++j;
    }
    return a.size() - i < b.size() - j ? -1 : a.size() - i > b.size() - j ? 1 : 0;
  };
  auto cmp = [&](const std::string& a, const std::string& b) -> int {
    std::string x = field(a), y = field(b);
    if (numeric) { double p = num(x), q = num(y); if (p != q) return p < q ? -1 : 1; return 0; }
    if (version) return vers(x, y);
    if (fold) { std::string p = x, q = y; for (auto& c : p) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); for (auto& c : q) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return p < q ? -1 : p > q ? 1 : 0; }
    return x < y ? -1 : x > y ? 1 : 0;
  };
  bool rev = o.has('r');
  std::stable_sort(lines.begin(), lines.end(), [&](const std::string& a, const std::string& b) {
    int c = cmp(a, b);
    if (c == 0 && (numeric || key)) c = a < b ? -1 : a > b ? 1 : 0;   // a last-resort whole-line compare
    return rev ? c > 0 : c < 0;
  });
  if (o.has('u')) lines.erase(std::unique(lines.begin(), lines.end(), [&](const std::string& a, const std::string& b) { return cmp(a, b) == 0; }), lines.end());
  std::string out;
  for (const auto& l : lines) out += l + "\n";
  if (o.has('o')) { std::string e; if (!os::write_whole(host_path(st.cwd, o.get('o')), out, false, &e)) { err(io, "sort", e); return 2; } return status; }
  io.write(out);
  return status;
}

int u_uniq(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "cdui", "fs");
  std::vector<std::string> in = o.args.empty() ? std::vector<std::string>{} : std::vector<std::string>{o.args[0]};
  auto key = [&](std::string s) { if (o.has('i')) for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
  // An output operand is written as the lines are decided, like standard output.
  Io out_io = io;
  FdCloser out_file;
  if (o.args.size() > 1) {
    std::string e;
    out_file.f = os::open_file(host_path(st.cwd, o.args[1]), os::Open::Write, &e);
    if (out_file.f == kNoFd) { err(io, "uniq", o.args[1] + ": " + e); return 1; }
    out_io.out = out_file.f;
  }
  BufferedOut out{out_io, {}};
  // Only adjacent lines are compared, so one line and its count are all that is held while the input streams by.
  std::string prev, prev_key;
  size_t count = 0;
  auto emit = [&]() {
    if (!count || (o.has('d') && count < 2) || (o.has('u') && count != 1)) return;
    if (o.has('c')) { char b[16]; std::snprintf(b, sizeof b, "%7zu ", count); out.put(b); }
    out.put(prev + "\n");
  };
  int status = each_stream(st, in, io, "uniq", [&](const std::string&, Fd f) {
    each_line(f, out, [&](const std::string& l, bool) {
      std::string k = key(l);
      if (count && k == prev_key) { ++count; return; }
      emit();
      prev = l;
      prev_key = std::move(k);
      count = 1;
    });
  });
  emit();
  out.flush();
  return status;
}

std::string tr_set(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '[' && i + 1 < s.size() && s[i + 1] == ':') {
      size_t e = s.find(":]", i + 2);
      if (e != std::string::npos) {
        std::string cls = s.substr(i + 2, e - i - 2);
        for (int c = 0; c < 256; ++c) {
          unsigned char u = static_cast<unsigned char>(c);
          if ((cls == "lower" && std::islower(u)) || (cls == "upper" && std::isupper(u)) || (cls == "digit" && std::isdigit(u)) ||
              (cls == "alpha" && std::isalpha(u)) || (cls == "alnum" && std::isalnum(u)) || (cls == "space" && std::isspace(u)) ||
              (cls == "punct" && std::ispunct(u)) || (cls == "blank" && (u == ' ' || u == '\t')) || (cls == "cntrl" && std::iscntrl(u)) ||
              (cls == "xdigit" && std::isxdigit(u)) || (cls == "print" && std::isprint(u)) || (cls == "graph" && std::isgraph(u)))
            out += static_cast<char>(c);
        }
        i = e + 1;
        continue;
      }
    }
    char c = s[i];
    if (c == '\\' && i + 1 < s.size()) {
      char n = s[++i];
      c = n == 'n' ? '\n' : n == 't' ? '\t' : n == 'r' ? '\r' : n == '\\' ? '\\' : n == '0' ? '\0' : n;
    }
    if (i + 2 < s.size() && s[i + 1] == '-') {
      char hi = s[i + 2];
      for (int k = static_cast<unsigned char>(c); k <= static_cast<unsigned char>(hi); ++k) out += static_cast<char>(k);
      i += 2;
      continue;
    }
    out += c;
  }
  return out;
}

int u_tr(Shell&, State&, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "dscC", "");
  if (o.args.empty()) { err(io, "tr", "missing operand"); return 1; }
  std::string a = tr_set(o.args[0]), b = o.args.size() > 1 ? tr_set(o.args[1]) : "";
  bool comp = o.has('c') || o.has('C');
  bool in_a[256] = {};
  for (char c : a) in_a[static_cast<unsigned char>(c)] = true;
  if (comp) for (bool& x : in_a) x = !x;
  char map[256];
  for (int i = 0; i < 256; ++i) map[i] = static_cast<char>(i);
  if (!o.has('d') && !b.empty()) {
    if (comp) { for (int i = 0; i < 256; ++i) if (in_a[i]) map[i] = b.back(); }
    else for (size_t i = 0; i < a.size(); ++i) map[static_cast<unsigned char>(a[i])] = b[std::min(i, b.size() - 1)];
  }
  bool squeeze_set[256] = {};
  if (o.has('s')) {
    const std::string& sq = (o.has('d') || b.empty()) ? (o.has('d') ? b : a) : b;
    for (char c : sq) squeeze_set[static_cast<unsigned char>(c)] = true;
    if (comp && b.empty()) for (int i = 0; i < 256; ++i) squeeze_set[i] = in_a[i];
  }
  char buf[1 << 16];
  long n;
  int last = -1;
  while ((n = os::read_fd(io.in, buf, sizeof buf)) > 0) {
    std::string out;
    for (long i = 0; i < n; ++i) {
      unsigned char c = static_cast<unsigned char>(buf[i]);
      if (o.has('d') && in_a[c]) continue;
      char m = map[c];
      if (o.has('s') && squeeze_set[static_cast<unsigned char>(m)] && last == static_cast<unsigned char>(m)) continue;
      out += m;
      last = static_cast<unsigned char>(m);
    }
    io.write(out);
  }
  return 0;
}

int u_cut(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "sn", "dfcb");
  std::string delim = o.has('d') ? o.get('d').substr(0, 1) : "\t";
  if (o.has('d') && o.get('d').empty()) delim = " ";
  std::string list = o.has('f') ? o.get('f') : o.has('c') ? o.get('c') : o.get('b');
  if (list.empty()) { err(io, "cut", "you must specify a list of bytes, characters, or fields"); return 1; }
  std::vector<std::pair<long, long>> ranges;
  for (size_t a = 0; a <= list.size();) {
    size_t b = list.find(',', a);
    if (b == std::string::npos) b = list.size();
    std::string r = list.substr(a, b - a);
    size_t dash = r.find('-');
    long lo = 1, hi = 1L << 30;
    if (dash == std::string::npos) lo = hi = std::atol(r.c_str());
    else { if (dash > 0) lo = std::atol(r.substr(0, dash).c_str()); if (dash + 1 < r.size()) hi = std::atol(r.substr(dash + 1).c_str()); }
    ranges.push_back({lo, hi});
    a = b + 1;
  }
  auto want = [&](long i) { for (auto& r : ranges) if (i >= r.first && i <= r.second) return true; return false; };
  BufferedOut out{io, {}};
  int status = each_stream(st, o.args, io, "cut", [&](const std::string&, Fd fd) {
    each_line(fd, out, [&](const std::string& whole, bool) {
      std::string line = whole;
      bool cr = !line.empty() && line.back() == '\r';
      if (cr) line.pop_back();
      if (o.has('f')) {
        if (line.find(delim[0]) == std::string::npos) { if (!o.has('s')) out.put(line + "\n"); return; }
        std::vector<std::string> f;
        size_t a = 0;
        for (;;) { size_t b = line.find(delim[0], a); f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
        std::string row;
        bool first = true;
        for (size_t i = 0; i < f.size(); ++i) if (want(static_cast<long>(i + 1))) { row += (first ? "" : delim) + f[i]; first = false; }
        out.put(row + "\n");
      } else {
        std::string row;
        for (size_t i = 0; i < line.size(); ++i) if (want(static_cast<long>(i + 1))) row += line[i];
        out.put(row + "\n");
      }
    });
  });
  out.flush();
  return status;
}

int u_tac(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  std::vector<std::string> files(argv.begin() + 1, argv.end());
  return each_input(st, files, io, "tac", [&](const std::string&, const std::string& d) {
    auto ls = split_lines(d);
    std::string out;
    for (auto it = ls.rbegin(); it != ls.rend(); ++it) out += *it + "\n";
    io.write(out);
  });
}

int u_rev(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  std::vector<std::string> files(argv.begin() + 1, argv.end());
  BufferedOut out{io, {}};
  int status = each_stream(st, files, io, "rev", [&](const std::string&, Fd f) {
    each_line(f, out, [&](const std::string& line, bool) { out.put(std::string(line.rbegin(), line.rend()) + "\n"); });
  });
  out.flush();
  return status;
}

int u_nl(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "", "bswv");
  long long n = 0;
  bool all = o.get('b') == "a";
  BufferedOut out{io, {}};
  int status = each_stream(st, o.args, io, "nl", [&](const std::string&, Fd f) {
    each_line(f, out, [&](const std::string& l, bool) {
      if (l.empty() && !all) { out.put("\n"); return; }
      char b[16]; std::snprintf(b, sizeof b, "%6lld\t", ++n);
      out.put(b + l + "\n");
    });
  });
  out.flush();
  return status;
}

int u_expr(Shell&, State&, const std::vector<std::string>& argv, const Io& io) {
  std::vector<std::string> a(argv.begin() + 1, argv.end());
  auto is_num = [](const std::string& s, long long& v) { return parse_int(s, v); };
  std::string r;
  if (a.size() == 1) r = a[0];
  else if (a.size() == 3) {
    long long x, y;
    const std::string &l = a[0], &op = a[1], &rr = a[2];
    bool nums = is_num(l, x) && is_num(rr, y);
    if (op == "+" && nums) r = std::to_string(x + y);
    else if (op == "-" && nums) r = std::to_string(x - y);
    else if (op == "*" && nums) r = std::to_string(x * y);
    else if (op == "/" && nums && y) r = std::to_string(x / y);
    else if (op == "%" && nums && y) r = std::to_string(x % y);
    else if (op == "=") r = l == rr ? "1" : "0";
    else if (op == "!=") r = l != rr ? "1" : "0";
    else if (op == "<") r = (nums ? x < y : l < rr) ? "1" : "0";
    else if (op == ">") r = (nums ? x > y : l > rr) ? "1" : "0";
    else if (op == "<=") r = (nums ? x <= y : l <= rr) ? "1" : "0";
    else if (op == ">=") r = (nums ? x >= y : l >= rr) ? "1" : "0";
    else if (op == ":") { size_t k = 0; while (k < l.size() && k < rr.size() && (rr[k] == '.' || rr[k] == l[k])) ++k; r = std::to_string(rr == ".*" ? l.size() : k); }
    else { err(io, "expr", "syntax error"); return 2; }
  } else if (a.size() == 2 && a[0] == "length") r = std::to_string(a[1].size());
  else { err(io, "expr", "syntax error"); return 2; }
  io.write(r + "\n");
  return (r.empty() || r == "0") ? 1 : 0;
}

int u_whoami(Shell&, State& st, const std::vector<std::string>&, const Io& io) {
  std::string u = st.get("USER");
  if (u.empty()) u = st.get("USERNAME");
  io.write((u.empty() ? "user" : u) + "\n");
  return 0;
}

int u_uname(Shell&, State&, const std::vector<std::string>& argv, const Io& io) {
#ifdef _WIN32
  std::string s = "MINGW64_NT-10.0", m = "x86_64";   // what Git Bash says, which scripts already know how to read
#else
  std::string s = "Linux", m = "x86_64";
#endif
  bool all = argv.size() > 1 && argv[1] == "-a";
  if (argv.size() > 1 && argv[1] == "-m") { io.write(m + "\n"); return 0; }
  io.write(all ? s + " aios " + m + "\n" : s + "\n");
  return 0;
}

int u_hostname(Shell&, State& st, const std::vector<std::string>&, const Io& io) {
  std::string h = st.get("COMPUTERNAME");
  if (h.empty()) h = st.get("HOSTNAME");
  io.write((h.empty() ? "localhost" : h) + "\n");
  return 0;
}

int u_mktemp(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "dqut", "p");
  std::string tmpl = o.args.empty() ? "tmp.XXXXXXXXXX" : o.args[0];
  std::string dir = o.has('p') ? host_path(st.cwd, o.get('p')) : os::temp_dir();
  if (tmpl.find('/') != std::string::npos || tmpl.find('\\') != std::string::npos) dir.clear();
  static const char* abc = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  static unsigned long long seed = static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ static_cast<unsigned long long>(os::self_pid());
  for (int attempt = 0; attempt < 100; ++attempt) {
    std::string name = tmpl;
    for (auto& c : name) if (c == 'X') { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; c = abc[(seed >> 33) % 62]; }
    std::string p = dir.empty() ? host_path(st.cwd, name) : host_path(dir, name);
    if (os::exists(p)) continue;
    std::string e;
    bool ok = o.has('d') ? os::make_dir(p, &e) : os::write_whole(p, "", false, &e);
    if (!ok) { err(io, "mktemp", e); return 1; }
    io.write(shown(p) + "\n");
    return 0;
  }
  err(io, "mktemp", "could not create a unique name");
  return 1;
}

int u_ls(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "laAhRtrSF1d", "");
  bool longf = o.has('l'), all = o.has('a'), almost = o.has('A'), rec = o.has('R'), by_time = o.has('t'), rev = o.has('r'), by_size = o.has('S');
  std::vector<std::string> targets = o.args.empty() ? std::vector<std::string>{"."} : o.args;
  int status = 0;
  bool headers = targets.size() > 1 || rec;
  std::function<void(const std::string&, bool)> list = [&](const std::string& t, bool first) {
    std::string p = host_path(st.cwd, t);
    if (!os::exists(p)) { err(io, "ls", "cannot access '" + t + "': No such file or directory"); status = 2; return; }
    auto line = [&](const std::string& name, const std::string& full, bool dir) {
      std::string disp = name + (o.has('F') && dir ? "/" : "");
      if (!longf) return disp + "\n";
      char b[128];
      std::time_t mt = static_cast<std::time_t>(os::mtime(full));
      std::tm tm{};
#ifdef _WIN32
      localtime_s(&tm, &mt);
#else
      localtime_r(&mt, &tm);
#endif
      char ts[32];
      std::strftime(ts, sizeof ts, "%b %e %H:%M", &tm);
      std::string to;
      bool link = os::read_link(full, &to);
      std::snprintf(b, sizeof b, "%s 1 user user %8llu %s ", link ? "lrwxr-xr-x" : dir ? "drwxr-xr-x" : "-rw-r--r--", static_cast<unsigned long long>(link ? to.size() : dir ? 0 : os::file_size(full)), ts);
      return std::string(b) + disp + (link ? " -> " + to : "") + "\n";
    };
    // -l names a link given on the command line, not the folder it points at (as ls without -H or -L)
    if (!os::is_dir(p) || o.has('d') || (longf && os::is_symlink(p) && t.back() != '/')) { io.write(line(t, p, os::is_dir(p))); return; }
    std::vector<os::DirEntry> es;
    os::list_dir(p, es);
    if (all) { es.push_back({".", true, false}); es.push_back({"..", true, false}); }
    std::vector<os::DirEntry> shown_e;
    for (auto& e : es) if (all || almost || e.name[0] != '.') shown_e.push_back(e);
    std::sort(shown_e.begin(), shown_e.end(), [&](const os::DirEntry& a, const os::DirEntry& b) {
      if (by_time) { auto x = os::mtime(p + "/" + a.name), y = os::mtime(p + "/" + b.name); if (x != y) return x > y; }
      if (by_size) { auto x = os::file_size(p + "/" + a.name), y = os::file_size(p + "/" + b.name); if (x != y) return x > y; }
      return a.name < b.name;
    });
    if (rev) std::reverse(shown_e.begin(), shown_e.end());
    std::string out;
    if (headers) out += (first ? "" : "\n") + t + ":\n";
    if (longf) out += "total " + std::to_string(shown_e.size()) + "\n";
    for (auto& e : shown_e) out += line(e.name, p + "/" + e.name, e.dir);
    io.write(out);
    if (rec) for (auto& e : shown_e) if (e.dir && e.name != "." && e.name != ".." && !e.link) list(t + "/" + e.name, false);
  };
  bool first = true;
  for (const auto& t : targets) { list(t, first); first = false; }
  return status;
}

// find: paths, then an expression of -name -iname -path -type -maxdepth -mindepth -newer -empty -size
// -not/! -a -o ( ) -print -print0 -exec ... {} ; / + -delete -prune
struct FindExpr {
  enum Kind { True, Name, IName, Path, IPath, Type, Empty, Newer, Size, Not, And, Or, Print, Print0, Exec, ExecPlus, Delete, Prune, Regex } kind = True;
  std::string arg;
  std::vector<std::string> cmd;
  std::shared_ptr<FindExpr> a, b;
  long long n = 0;
  int cmp = 0;   // for -size: -1 less, 0 equal, 1 more
};

int u_find(Shell& sh, State& st, const std::vector<std::string>& argv, const Io& io) {
  size_t i = 1;
  std::vector<std::string> roots;
  while (i < argv.size() && !argv[i].empty() && argv[i][0] != '-' && argv[i] != "!" && argv[i] != "(") roots.push_back(argv[i++]);
  if (roots.empty()) roots.push_back(".");
  int maxdepth = 1 << 30, mindepth = 0;
  bool has_action = false;
  std::vector<std::string> tok(argv.begin() + static_cast<long>(i), argv.end());
  size_t t = 0;
  std::function<std::shared_ptr<FindExpr>()> or_e, and_e, prim;
  auto mk = [](FindExpr::Kind k) { auto e = std::make_shared<FindExpr>(); e->kind = k; return e; };
  std::string parse_err;
  prim = [&]() -> std::shared_ptr<FindExpr> {
    if (t >= tok.size()) return mk(FindExpr::True);
    std::string a = tok[t++];
    if (a == "!" || a == "-not") { auto e = mk(FindExpr::Not); e->a = prim(); return e; }
    if (a == "(") { auto e = or_e(); if (t < tok.size() && tok[t] == ")") ++t; return e; }
    auto need = [&]() { if (t >= tok.size()) { parse_err = "missing argument to '" + a + "'"; return std::string(); } return tok[t++]; };
    if (a == "-name") { auto e = mk(FindExpr::Name); e->arg = need(); return e; }
    if (a == "-iname") { auto e = mk(FindExpr::IName); e->arg = need(); for (auto& c : e->arg) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return e; }
    if (a == "-path" || a == "-wholename") { auto e = mk(FindExpr::Path); e->arg = need(); return e; }
    if (a == "-ipath") { auto e = mk(FindExpr::IPath); e->arg = need(); for (auto& c : e->arg) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return e; }
    if (a == "-regex") { auto e = mk(FindExpr::Regex); e->arg = need(); return e; }
    if (a == "-type") { auto e = mk(FindExpr::Type); e->arg = need(); return e; }
    if (a == "-empty") return mk(FindExpr::Empty);
    if (a == "-newer") { auto e = mk(FindExpr::Newer); e->n = os::mtime(host_path(st.cwd, need())); return e; }
    if (a == "-size") {
      auto e = mk(FindExpr::Size);
      std::string s = need();
      if (!s.empty() && (s[0] == '+' || s[0] == '-')) { e->cmp = s[0] == '+' ? 1 : -1; s = s.substr(1); }
      long long mul = 512;
      if (!s.empty() && std::strchr("ckMGb", s.back())) { char u = s.back(); s.pop_back(); mul = u == 'c' ? 1 : u == 'k' ? 1024 : u == 'M' ? 1 << 20 : u == 'G' ? 1LL << 30 : 512; }
      e->n = std::atoll(s.c_str()) * mul;
      return e;
    }
    if (a == "-maxdepth") { maxdepth = std::atoi(need().c_str()); return mk(FindExpr::True); }
    if (a == "-mindepth") { mindepth = std::atoi(need().c_str()); return mk(FindExpr::True); }
    if (a == "-print") { has_action = true; return mk(FindExpr::Print); }
    if (a == "-print0") { has_action = true; return mk(FindExpr::Print0); }
    if (a == "-delete") { has_action = true; return mk(FindExpr::Delete); }
    if (a == "-prune") return mk(FindExpr::Prune);
    if (a == "-exec" || a == "-execdir" || a == "-ok") {
      has_action = true;
      auto e = mk(FindExpr::Exec);
      while (t < tok.size() && tok[t] != ";" && tok[t] != "+") e->cmd.push_back(tok[t++]);
      if (t < tok.size() && tok[t] == "+") e->kind = FindExpr::ExecPlus;
      ++t;
      return e;
    }
    if (a == "-true") return mk(FindExpr::True);
    if (a == "-false") { auto e = mk(FindExpr::Not); e->a = mk(FindExpr::True); return e; }
    if (a == "-follow" || a == "-xdev" || a == "-depth" || a == "-mount" || a == "-noleaf") return mk(FindExpr::True);
    if (a == "-mtime" || a == "-mmin" || a == "-user" || a == "-group" || a == "-perm" || a == "-newermt") { need(); return mk(FindExpr::True); }
    parse_err = "unknown predicate '" + a + "'";
    return mk(FindExpr::True);
  };
  and_e = [&]() {
    auto l = prim();
    while (t < tok.size() && tok[t] != "-o" && tok[t] != "-or" && tok[t] != ")") {
      if (tok[t] == "-a" || tok[t] == "-and") ++t;
      auto e = mk(FindExpr::And); e->a = l; e->b = prim(); l = e;
    }
    return l;
  };
  or_e = [&]() {
    auto l = and_e();
    while (t < tok.size() && (tok[t] == "-o" || tok[t] == "-or")) { ++t; auto e = mk(FindExpr::Or); e->a = l; e->b = and_e(); l = e; }
    return l;
  };
  auto expr = or_e();
  if (!parse_err.empty()) { err(io, "find", parse_err); return 1; }
  if (!has_action) { auto e = mk(FindExpr::And); e->a = expr; e->b = mk(FindExpr::Print); expr = e; }
  int status = 0;
  std::vector<std::vector<std::string>> plus_batches;
  std::map<FindExpr*, std::vector<std::string>> plus;
  std::function<bool(FindExpr&, const std::string&, const std::string&, bool, bool&)> eval =
      [&](FindExpr& e, const std::string& shown_path, const std::string& host, bool dir, bool& prune) -> bool {
    switch (e.kind) {
      case FindExpr::True: return true;
      case FindExpr::Name: return glob_match(e.arg, base_name(shown_path));
      case FindExpr::IName: { std::string b = base_name(shown_path); for (auto& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return glob_match(e.arg, b); }
      case FindExpr::Path: return glob_match(e.arg, shown_path);
      case FindExpr::IPath: { std::string b = shown_path; for (auto& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return glob_match(e.arg, b); }
      case FindExpr::Regex: return glob_match("*" + e.arg + "*", shown_path) || shown_path.find(e.arg) != std::string::npos;
      case FindExpr::Type: return e.arg == "d" ? dir : e.arg == "f" ? !dir : false;
      case FindExpr::Empty: { if (dir) { std::vector<os::DirEntry> es; os::list_dir(host, es); return es.empty(); } return os::file_size(host) == 0; }
      case FindExpr::Newer: return os::mtime(host) > e.n;
      case FindExpr::Size: { long long s = static_cast<long long>(os::file_size(host)); if (e.cmp > 0) return s > e.n; if (e.cmp < 0) return s < e.n; return s == e.n; }
      case FindExpr::Not: return !eval(*e.a, shown_path, host, dir, prune);
      case FindExpr::And: return eval(*e.a, shown_path, host, dir, prune) && eval(*e.b, shown_path, host, dir, prune);
      case FindExpr::Or: return eval(*e.a, shown_path, host, dir, prune) || eval(*e.b, shown_path, host, dir, prune);
      case FindExpr::Print: io.write(shown_path + "\n"); return true;
      case FindExpr::Print0: io.write(shown_path + std::string(1, '\0')); return true;
      case FindExpr::Delete: { std::string er; bool ok = dir ? os::remove_dir(host, &er) : os::remove_file(host, &er); if (!ok) { err(io, "find", shown_path + ": " + er); status = 1; } return ok; }
      case FindExpr::Prune: prune = true; return true;
      case FindExpr::Exec: {
        std::vector<std::string> c;
        for (auto x : e.cmd) { size_t at; while ((at = x.find("{}")) != std::string::npos) x.replace(at, 2, shown_path); c.push_back(x); }
        State copy = st;
        return run_utility_by_name(sh, copy, c, io) == 0;
      }
      case FindExpr::ExecPlus: plus[&e].push_back(shown_path); return true;
    }
    return false;
  };
  std::function<void(const std::string&, int)> walk = [&](const std::string& shown_path, int depth) {
    std::string host = host_path(st.cwd, shown_path);
    bool dir = os::is_dir(host);
    bool prune = false;
    if (depth >= mindepth) eval(*expr, shown_path, host, dir, prune);
    if (dir && !prune && depth < maxdepth) {
      std::vector<os::DirEntry> es;
      os::list_dir(host, es);
      std::sort(es.begin(), es.end(), [](const os::DirEntry& a, const os::DirEntry& b) { return a.name < b.name; });
      for (const auto& d : es) walk((shown_path == "/" ? "" : shown_path) + "/" + d.name, depth + 1);
    }
  };
  for (const auto& r : roots) {
    std::string root = r;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    if (!os::exists(host_path(st.cwd, root))) { err(io, "find", "'" + r + "': No such file or directory"); status = 1; continue; }
    walk(root, 0);
  }
  for (auto& [e, paths] : plus) {
    std::vector<std::string> c;
    for (auto& x : e->cmd) { if (x == "{}") c.insert(c.end(), paths.begin(), paths.end()); else c.push_back(x); }
    State copy = st;
    if (run_utility_by_name(sh, copy, c, io) != 0) status = 1;
  }
  return status;
}

int u_xargs(Shell& sh, State& st, const std::vector<std::string>& argv, const Io& io) {
  size_t i = 1;
  bool null_sep = false, no_run_empty = false;
  long long max_args = 0;
  std::string replace, delim;
  for (; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (a == "-0" || a == "--null") null_sep = true;
    else if (a == "-r" || a == "--no-run-if-empty") no_run_empty = true;
    else if (a == "-n" && i + 1 < argv.size()) parse_int(argv[++i], max_args);
    else if (a.rfind("-n", 0) == 0 && a.size() > 2) parse_int(a.substr(2), max_args);
    else if (a == "-I" && i + 1 < argv.size()) replace = argv[++i];
    else if (a.rfind("-I", 0) == 0 && a.size() > 2) replace = a.substr(2);
    else if (a == "-i") replace = "{}";
    else if (a == "-d" && i + 1 < argv.size()) delim = escape_c(argv[++i]);
    else if ((a == "-P" || a == "-L" || a == "-s") && i + 1 < argv.size()) ++i;
    else if (a.rfind("-P", 0) == 0 || a == "-t" || a == "-x" || a == "-p") {}
    else break;
  }
  std::vector<std::string> base(argv.begin() + static_cast<long>(i), argv.end());
  if (base.empty()) base.push_back("echo");
  std::string in = io.read_all();
  std::vector<std::string> items;
  if (null_sep || !delim.empty()) {
    char d = null_sep ? '\0' : delim[0];
    size_t a = 0;
    while (a < in.size()) { size_t b = in.find(d, a); if (b == std::string::npos) b = in.size(); if (b > a) items.push_back(in.substr(a, b - a)); a = b + 1; }
  } else if (!replace.empty()) {
    for (auto l : split_lines(in)) { if (!l.empty() && l.back() == '\r') l.pop_back(); if (!l.empty()) items.push_back(l); }
  } else {
    // blanks separate; quotes group
    std::string cur;
    bool have = false;
    for (size_t k = 0; k < in.size(); ++k) {
      char c = in[k];
      if (c == '\'' || c == '"') { size_t e = in.find(c, k + 1); if (e == std::string::npos) e = in.size(); cur += in.substr(k + 1, e - k - 1); k = e; have = true; continue; }
      if (c == '\\' && k + 1 < in.size()) { cur += in[++k]; have = true; continue; }
      if (std::isspace(static_cast<unsigned char>(c))) { if (have) items.push_back(cur); cur.clear(); have = false; continue; }
      cur += c; have = true;
    }
    if (have) items.push_back(cur);
  }
  if (items.empty() && no_run_empty) return 0;
  int status = 0;
  auto run = [&](const std::vector<std::string>& c) {
    State copy = st;
    int s = run_utility_by_name(sh, copy, c, io);
    if (s != 0) status = s == 127 ? 127 : 123;
  };
  if (!replace.empty()) {
    for (const auto& it : items) {
      std::vector<std::string> c;
      for (auto x : base) { size_t at; while ((at = x.find(replace)) != std::string::npos) x.replace(at, replace.size(), it); c.push_back(x); }
      run(c);
    }
    return status;
  }
  size_t per = max_args > 0 ? static_cast<size_t>(max_args) : (items.empty() ? 1 : items.size());
  if (items.empty()) { run(base); return status; }
  for (size_t k = 0; k < items.size(); k += per) {
    std::vector<std::string> c = base;
    for (size_t j = k; j < std::min(items.size(), k + per); ++j) c.push_back(items[j]);
    run(c);
  }
  return status;
}

int u_diff(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "uqsrNbwBiaE", "U");
  if (o.args.size() != 2) { err(io, "diff", "missing operand"); return 2; }
  int ctx = o.has('U') ? std::atoi(o.get('U').c_str()) : 3;
  auto load = [&](const std::string& f, std::string& out) {
    if (f == "-") { out = io.read_all(); return true; }
    std::string e;
    if (!os::read_whole(host_path(st.cwd, f), out, &e)) { if (o.has('N')) return true; err(io, "diff", f + ": " + e); return false; }
    return true;
  };
  std::string a, b;
  if (!load(o.args[0], a) || !load(o.args[1], b)) return 2;
  if (o.has('w') || o.has('b')) {
    auto squash = [&](std::string s) { std::string r; for (char c : s) { if (c == ' ' || c == '\t' || c == '\r') { if (o.has('w')) continue; if (!r.empty() && r.back() == ' ') continue; c = ' '; } r += c; } return r; };
    if (squash(a) == squash(b)) return 0;
  }
  if (a == b) return 0;
  if (o.has('q')) { io.write("Files " + o.args[0] + " and " + o.args[1] + " differ\n"); return 1; }
  if (o.has('u') || o.has('U')) {
    // the kernel's own diff (aios's git_unified_hunks: the same hunks git prints)
    std::string hunks = aios::userland::git_unified_hunks(a, b, ctx);
    io.write("--- " + o.args[0] + "\n+++ " + o.args[1] + "\n" + hunks);
    return 1;
  }
  // the "normal" format POSIX diff prints by default: NaM,N / N,McN,M / N,MdN with < and > lines
  auto la = aios::userland::git_split_lines(a), lb = aios::userland::git_split_lines(b);
  auto range = [](size_t from, size_t len) { return len <= 1 ? std::to_string(from + (len ? 1 : 0)) : std::to_string(from + 1) + "," + std::to_string(from + len); };
  auto line = [](char tag, std::string_view l) {
    std::string s = std::string(1, tag) + " " + std::string(l);
    if (s.empty() || s.back() != '\n') s += "\n\\ No newline at end of file\n";
    return s;
  };
  std::string out;
  for (const auto& e : aios::userland::git_diff_lines(la, lb)) {
    if (e.a_len == 0) out += std::to_string(e.a_start) + "a" + range(e.b_start, e.b_len) + "\n";
    else if (e.b_len == 0) out += range(e.a_start, e.a_len) + "d" + std::to_string(e.b_start) + "\n";
    else out += range(e.a_start, e.a_len) + "c" + range(e.b_start, e.b_len) + "\n";
    for (size_t k = 0; k < e.a_len; ++k) out += line('<', la[e.a_start + k]);
    if (e.a_len && e.b_len) out += "---\n";
    for (size_t k = 0; k < e.b_len; ++k) out += line('>', lb[e.b_start + k]);
  }
  io.write(out);
  return 1;
}

int u_cmp(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "sl", "");
  if (o.args.size() < 2) { err(io, "cmp", "missing operand"); return 2; }
  // "-" is standard input, as in POSIX cmp (`git show REV:f | cmp - f` was "No such file or directory").
  // Both are read a block at a time and compared as they come: a big blob is never held whole, and cmp
  // stops reading at the first difference (its producer then sees a closed pipe, as with the real cmp).
  if (o.args[0] == o.args[1] && o.args[0] == "-") return 0;
  struct Src {
    Fd f = kNoFd;
    bool mine = false;
    std::string buf;
    size_t pos = 0;
    bool more() {   // bytes left in buf, reading the next block when it is used up
      if (pos < buf.size()) return true;
      buf.resize(1 << 16);
      long n = os::read_fd(f, buf.data(), buf.size());
      buf.resize(n > 0 ? static_cast<size_t>(n) : 0);
      pos = 0;
      return n > 0;
    }
    ~Src() { if (mine) os::close_fd(f); }
  } src[2];
  for (int k = 0; k < 2; ++k) {
    if (o.args[k] == "-") { src[k].f = io.in; continue; }
    std::string e;
    src[k].f = os::open_file(host_path(st.cwd, o.args[k]), os::Open::Read, &e);
    if (src[k].f == kNoFd) { err(io, "cmp", e); return 2; }
    src[k].mine = true;
  }
  unsigned long long byte = 0, line = 1;
  for (;;) {
    bool ma = src[0].more(), mb = src[1].more();
    if (!ma && !mb) return 0;
    if (!ma || !mb) { if (!o.has('s')) io.write("cmp: EOF on " + (!ma ? o.args[0] : o.args[1]) + "\n"); return 1; }
    size_t n = std::min(src[0].buf.size() - src[0].pos, src[1].buf.size() - src[1].pos);
    const char* a = src[0].buf.data() + src[0].pos;
    const char* b = src[1].buf.data() + src[1].pos;
    if (std::memcmp(a, b, n) != 0) {
      size_t i = 0;
      while (a[i] == b[i]) { if (a[i] == '\n') ++line; ++i; }
      if (!o.has('s')) io.write(o.args[0] + " " + o.args[1] + " differ: byte " + std::to_string(byte + i + 1) + ", line " + std::to_string(line) + "\n");
      return 1;
    }
    if (!o.has('s')) line += static_cast<unsigned long long>(std::count(a, a + n, '\n'));
    byte += n;
    src[0].pos += n;
    src[1].pos += n;
  }
}

int u_base64(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "di", "w");
  static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  BufferedOut out{io, {}};
  // Both ways run a block at a time, the inputs one after another as if one: what one block leaves over (bits of a
  // byte, or one or two bytes short of a group of three) carries into the next.
  if (o.has('d')) {
    int val[256];
    for (int& v : val) v = -1;
    for (int i = 0; i < 64; ++i) val[static_cast<unsigned char>(k[i])] = i;
    int acc = 0, bits = 0;
    int status = each_stream(st, o.args, io, "base64", [&](const std::string&, Fd f) {
      char buf[1 << 16];
      long n;
      while ((n = os::read_fd(f, buf, sizeof buf)) > 0) {
        std::string piece;
        for (long i = 0; i < n; ++i) {
          int v = val[static_cast<unsigned char>(buf[i])];
          if (v < 0) continue;
          acc = ((acc << 6) | v) & 0xffff;
          bits += 6;
          if (bits >= 8) { bits -= 8; piece += static_cast<char>((acc >> bits) & 0xff); }
        }
        out.put(piece);
      }
    });
    out.flush();
    return status;
  }
  const int wrap = o.has('w') ? std::atoi(o.get('w').c_str()) : 76;
  int col = 0;
  bool any = false;
  std::string piece;
  auto put_char = [&](char ch) {
    piece += ch;
    any = true;
    if (wrap > 0 && ++col == wrap) { piece += '\n'; col = 0; }
  };
  auto group = [&](const unsigned char* b, size_t len) {
    unsigned v = (static_cast<unsigned>(b[0]) << 16) | (len > 1 ? static_cast<unsigned>(b[1]) << 8 : 0u) | (len > 2 ? b[2] : 0u);
    put_char(k[v >> 18]);
    put_char(k[(v >> 12) & 63]);
    put_char(len > 1 ? k[(v >> 6) & 63] : '=');
    put_char(len > 2 ? k[v & 63] : '=');
  };
  unsigned char carry[3];
  size_t carried = 0;
  int status = each_stream(st, o.args, io, "base64", [&](const std::string&, Fd f) {
    char buf[1 << 16];
    long n;
    while ((n = os::read_fd(f, buf, sizeof buf)) > 0) {
      // whole groups of three are encoded now; one or two bytes left over wait for the next block
      for (long i = 0; i < n; ++i) {
        carry[carried++] = static_cast<unsigned char>(buf[i]);
        if (carried == 3) { group(carry, 3); carried = 0; }
      }
      out.put(piece);
      piece.clear();
    }
  });
  if (carried) group(carry, carried);
  // the last line ends with a newline; so does an empty encoding
  if (col > 0 || !any || wrap <= 0) piece += '\n';
  out.put(piece);
  out.flush();
  return status;
}

int u_du(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "shac", "d");
  std::vector<std::string> targets = o.args.empty() ? std::vector<std::string>{"."} : o.args;
  std::function<uint64_t(const std::string&)> size = [&](const std::string& p) -> uint64_t {
    if (!os::is_dir(p)) return os::file_size(p);
    uint64_t s = 0;
    std::vector<os::DirEntry> es;
    os::list_dir(p, es);
    for (auto& e : es) s += size(p + "/" + e.name);
    return s;
  };
  for (const auto& t : targets) {
    uint64_t s = size(host_path(st.cwd, t));
    std::string n;
    if (o.has('h')) { double v = static_cast<double>(s); const char* u = "BKMGT"; int k = 0; while (v >= 1024 && k < 4) { v /= 1024; ++k; } char b[32]; std::snprintf(b, sizeof b, k ? "%.1f%c" : "%.0f%c", v, u[k]); n = b; if (!k) n.pop_back(); }
    else n = std::to_string((s + 1023) / 1024);
    io.write(n + "\t" + t + "\n");
  }
  return 0;
}

int u_stat(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  Opts o = parse_opts(argv, "Lt", "cf");
  int status = 0;
  for (const auto& a : o.args) {
    std::string p = host_path(st.cwd, a);
    if (!os::exists(p)) { err(io, "stat", "cannot stat '" + a + "': No such file or directory"); status = 1; continue; }
    if (o.has('c')) {
      std::string f = o.get('c'), out;
      for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%' || i + 1 >= f.size()) { out += f[i]; continue; }
        char c = f[++i];
        if (c == 's') out += std::to_string(os::file_size(p));
        else if (c == 'n') out += a;
        else if (c == 'Y') out += std::to_string(os::mtime(p));
        else if (c == 'F') out += os::is_dir(p) ? "directory" : "regular file";
        else if (c == 'a') out += os::is_dir(p) ? "755" : "644";
        else { out += '%'; out += c; }
      }
      io.write(out + "\n");
      continue;
    }
    io.write("  File: " + a + "\n  Size: " + std::to_string(os::file_size(p)) + "\t" + (os::is_dir(p) ? "directory" : "regular file") + "\n");
  }
  return status;
}

int u_dos2unix(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  bool to_dos = argv[0] == "unix2dos";
  int status = 0;
  for (size_t i = 1; i < argv.size(); ++i) {
    if (argv[i][0] == '-') continue;
    std::string p = host_path(st.cwd, argv[i]), d, e;
    if (!os::read_whole(p, d, &e)) { err(io, argv[0], argv[i] + ": " + e); status = 1; continue; }
    std::string out;
    for (size_t k = 0; k < d.size(); ++k) {
      if (!to_dos && d[k] == '\r' && k + 1 < d.size() && d[k + 1] == '\n') continue;
      if (to_dos && d[k] == '\n' && (k == 0 || d[k - 1] != '\r')) out += '\r';
      out += d[k];
    }
    os::write_whole(p, out, false, &e);
  }
  return status;
}

int u_timeout(Shell& sh, State& st, const std::vector<std::string>& argv, const Io& io) {
  // runs the command; the limit itself is enforced by whatever runs this shell (the agent's own timeout)
  size_t i = 1;
  while (i < argv.size() && argv[i][0] == '-') { if ((argv[i] == "-s" || argv[i] == "-k") && i + 1 < argv.size()) ++i; ++i; }
  if (i + 1 >= argv.size()) { err(io, "timeout", "missing operand"); return 125; }
  std::vector<std::string> c(argv.begin() + static_cast<long>(i) + 1, argv.end());
  State copy = st;
  return run_utility_by_name(sh, copy, c, io);
}

// Thrown out of git's output stream once its reader has gone (`git log | head`): not a std::exception, so
// git's own `catch (const std::exception&)` lets it through and the walk stops, as SIGPIPE would stop it.
struct GitOutClosed {};

// A stream over one of the shell's descriptors, read or written only when git asks: `git status` in a
// pipeline must not wait for an input it never reads.
class FdStreamBuf : public std::streambuf {
 public:
  FdStreamBuf(Fd fd, bool reading) : fd_(fd), reading_(reading) {
    if (reading) setg(buf_, buf_, buf_);
    else setp(buf_, buf_ + sizeof buf_);
  }
  ~FdStreamBuf() override { if (!reading_ && !closed_) { try { sync(); } catch (const GitOutClosed&) {} } }
 protected:
  int_type underflow() override {
    if (fd_ == kNoFd || os::is_console(fd_)) return traits_type::eof();
    long n = os::read_fd(fd_, buf_, sizeof buf_);
    if (n <= 0) return traits_type::eof();
    setg(buf_, buf_, buf_ + n);
    return traits_type::to_int_type(buf_[0]);
  }
  int_type overflow(int_type ch) override {
    if (sync() != 0) return traits_type::eof();
    if (ch != traits_type::eof()) { *pptr() = (char)ch; pbump(1); }
    return traits_type::not_eof(ch);
  }
  int sync() override {
    if (reading_) return 0;
    size_t n = (size_t)(pptr() - pbase());
    if (closed_) return -1;
    if (n && fd_ != kNoFd && !os::write_all(fd_, pbase(), n)) { closed_ = true; throw GitOutClosed{}; }
    setp(buf_, buf_ + sizeof buf_);
    return 0;
  }
 private:
  Fd fd_;
  bool reading_;
  bool closed_ = false;
  char buf_[65536];
};

int u_git(Shell& sh, State& st, const std::vector<std::string>& argv, const Io& io) {
  if (argv[0] == "git") {
    std::string host = host_git(st);
    if (!host.empty()) {
      std::vector<std::string> a = argv;
      a[0] = host;
      return run_external(sh, st, a, io, {});
    }
  }
  FdStreamBuf in_buf(io.in, true), out_buf(io.out, false), err_buf(io.err, false);
  std::istream in_stream(&in_buf);
  std::ostream out_stream(&out_buf), err_stream(&err_buf);
  // git sees the shell's exported variables (GIT_INDEX_FILE=... git write-tree), not this process's
  std::map<std::string, std::string> env;
  for (auto& kv : st.env_list()) {
    size_t eq = kv.find('=');
    if (eq != std::string::npos) env[kv.substr(0, eq)] = kv.substr(eq + 1);
  }
  // an ostream swallows what its buffer throws unless badbit is in its exceptions(): then it rethrows it as is
  out_stream.exceptions(std::ios::badbit);
  int rc;
  try {
    rc = aios_git::run(argv, st.cwd, out_stream, err_stream, in_stream, &env);
    out_stream.flush();
  } catch (const GitOutClosed&) {
    rc = 141;
  }
  err_stream.flush();
  return rc;
}

}  // namespace

std::string host_git(const State& st) {
  std::string mode = st.get("AIOS_SH_GIT");
  if (mode == "builtin") return {};
  std::string p = os::find_program("git", st.get("PATH"), st.cwd);
  if (p.empty() && mode == "host") return "git";   // asked for the host's: run_external reports it missing
  return p;
}

const std::map<std::string, Utility>& utilities() {
  static const std::map<std::string, Utility> m = {
      {"true", u_true}, {":", u_true}, {"false", u_false}, {"echo", u_echo}, {"printf", u_printf}, {"test", u_test}, {"[", u_test},
      {"pwd", u_pwd}, {"cat", u_cat}, {"head", u_head_tail}, {"tail", u_head_tail}, {"wc", u_wc}, {"mkdir", u_mkdir},
      {"rm", u_rm}, {"rmdir", u_rmdir}, {"cp", u_cp_mv}, {"mv", u_cp_mv}, {"touch", u_touch}, {"ln", u_ln}, {"chmod", u_noop},
      {"chown", u_noop}, {"sync", u_noop}, {"basename", u_basename}, {"dirname", u_dirname}, {"realpath", u_realpath},
      {"readlink", u_readlink}, {"env", u_env}, {"printenv", u_printenv}, {"sleep", u_sleep}, {"which", u_which}, {"seq", u_seq},
      {"date", u_date}, {"tee", u_tee}, {"yes", u_yes}, {"sort", u_sort}, {"uniq", u_uniq}, {"tr", u_tr}, {"cut", u_cut},
      {"tac", u_tac}, {"rev", u_rev}, {"nl", u_nl}, {"expr", u_expr}, {"whoami", u_whoami}, {"uname", u_uname},
      {"hostname", u_hostname}, {"mktemp", u_mktemp}, {"ls", u_ls}, {"find", u_find}, {"xargs", u_xargs}, {"diff", u_diff},
      {"cmp", u_cmp}, {"base64", u_base64}, {"du", u_du}, {"stat", u_stat}, {"grep", util_grep}, {"egrep", util_grep},
      {"fgrep", util_grep}, {"sed", util_sed}, {"awk", util_awk}, {"gawk", util_awk}, {"sha256sum", u_sha_md5},
      {"sha1sum", u_sha_md5}, {"md5sum", u_sha_md5}, {"dos2unix", u_dos2unix}, {"unix2dos", u_dos2unix}, {"timeout", u_timeout},
      {"git", u_git}, {"aios-git", u_git},
      {"clear", u_noop}, {"stty", u_noop}, {"tput", u_noop}};
  return m;
}

}  // namespace aiossh
