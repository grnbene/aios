// sed: the stream editor, as GNU sed's command set goes for what scripts use. Addresses (n, $, /re/, n~m,
// a,b, a,+n, !), s with & \1-\9 \n \U \L \E and the g p i N w flags, y, d D p P n N g G h H x q Q = a i c r w
// l z, labels with b t T, { } blocks; -n -e -f -i[SUF] -E/-r -s -z.
#include "sh.hpp"

#include <cstring>
#include <deque>
#include <functional>
#include <regex>

namespace aiossh {

std::string posix_regex_to_ecma(const std::string& re, bool extended);   // sh_grep.cpp

namespace {

struct Addr {
  enum Kind { None, Line, Last, Regex, Step, Plus } kind = None;
  long long n = 0, step = 0;
  std::shared_ptr<std::regex> re;
};

struct Cmd {
  Addr a1, a2;
  bool negate = false;
  char name = 0;
  // s
  std::shared_ptr<std::regex> re;
  std::string replacement;
  bool global = false, print = false;
  long long nth = 1;
  std::string wfile;
  // y
  std::string from, to;
  // text for a i c, file for r w, label for b t T :
  std::string text;
  int exit_code = 0;
  size_t block_end = 0;     // '{': index of its '}'
  // range state
  bool in_range = false;
  long long range_end = 0;
};

struct Error { std::string msg; };

class Script {
 public:
  std::vector<Cmd> cmds;
  bool extended = false;
  std::shared_ptr<std::regex> last_re;

  void parse(const std::string& text) {
    s_ = text;
    p_ = 0;
    std::vector<size_t> open;
    for (;;) {
      skip_ws_and_seps();
      if (p_ >= s_.size()) break;
      if (s_[p_] == '}') {
        if (open.empty()) throw Error{"unexpected '}'"};
        cmds[open.back()].block_end = cmds.size();
        Cmd c; c.name = '}';
        cmds.push_back(c);
        open.pop_back();
        ++p_;
        continue;
      }
      Cmd c;
      c.a1 = address();
      if (c.a1.kind != Addr::None) {
        skip_spaces();
        if (cur() == ',') { ++p_; skip_spaces(); c.a2 = address(); if (c.a2.kind == Addr::None) throw Error{"unexpected ','"}; }
      }
      skip_spaces();
      while (cur() == '!') { c.negate = !c.negate; ++p_; skip_spaces(); }
      if (p_ >= s_.size()) throw Error{"missing command"};
      c.name = s_[p_++];
      command_args(c);
      if (c.name == '{') open.push_back(cmds.size());
      cmds.push_back(c);
    }
    if (!open.empty()) throw Error{"unmatched '{'"};
  }

 private:
  std::string s_;
  size_t p_ = 0;
  char cur() const { return p_ < s_.size() ? s_[p_] : '\0'; }
  void skip_spaces() { while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t')) ++p_; }
  void skip_ws_and_seps() { while (p_ < s_.size() && (std::isspace(static_cast<unsigned char>(s_[p_])) || s_[p_] == ';')) ++p_; }

  std::shared_ptr<std::regex> compile(const std::string& re, bool icase) {
    if (re.empty()) {
      if (!last_re) throw Error{"no previous regular expression"};
      return last_re;
    }
    try {
      auto flags = std::regex::ECMAScript;
      if (icase) flags |= std::regex::icase;
      auto r = std::make_shared<std::regex>(posix_regex_to_ecma(re, extended), flags);
      last_re = r;
      return r;
    } catch (const std::regex_error& e) {
      throw Error{std::string("invalid regular expression '") + re + "': " + e.what()};
    }
  }

  // Text up to an unescaped delimiter; "\delim" becomes delim, "\n" inside a regex stays (newline match).
  std::string delimited(char d, bool regex_part) {
    std::string out;
    while (p_ < s_.size() && s_[p_] != d) {
      if (s_[p_] == '\\' && p_ + 1 < s_.size()) {
        if (s_[p_ + 1] == d) { out += d; p_ += 2; continue; }
        if (s_[p_ + 1] == 'n' && regex_part) { out += "\\n"; p_ += 2; continue; }
        out += s_[p_];
        out += s_[p_ + 1];
        p_ += 2;
        continue;
      }
      if (s_[p_] == '[' && regex_part) {
        // a bracket expression may hold the delimiter
        size_t e = p_ + 1;
        if (e < s_.size() && s_[e] == '^') ++e;
        if (e < s_.size() && s_[e] == ']') ++e;
        e = s_.find(']', e);
        if (e != std::string::npos) { out += s_.substr(p_, e + 1 - p_); p_ = e + 1; continue; }
      }
      out += s_[p_++];
    }
    if (p_ >= s_.size()) throw Error{std::string("unterminated '") + d + "' expression"};
    ++p_;
    return out;
  }

  Addr address() {
    Addr a;
    char c = cur();
    if (std::isdigit(static_cast<unsigned char>(c))) {
      long long n = 0;
      while (std::isdigit(static_cast<unsigned char>(cur()))) n = n * 10 + (s_[p_++] - '0');
      if (cur() == '~') {
        ++p_;
        long long st = 0;
        while (std::isdigit(static_cast<unsigned char>(cur()))) st = st * 10 + (s_[p_++] - '0');
        a.kind = Addr::Step; a.n = n; a.step = st;
        return a;
      }
      a.kind = Addr::Line; a.n = n;
      return a;
    }
    if (c == '+' && p_ + 1 < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p_ + 1]))) {
      ++p_;
      long long n = 0;
      while (std::isdigit(static_cast<unsigned char>(cur()))) n = n * 10 + (s_[p_++] - '0');
      a.kind = Addr::Plus; a.n = n;
      return a;
    }
    if (c == '$') { ++p_; a.kind = Addr::Last; return a; }
    if (c == '/' || c == '\\') {
      char d = '/';
      if (c == '\\') { ++p_; d = cur(); }
      ++p_;
      std::string re = delimited(d, true);
      bool icase = false;
      while (cur() == 'I' || cur() == 'M') { if (cur() == 'I') icase = true; ++p_; }
      a.kind = Addr::Regex;
      a.re = compile(re, icase);
      return a;
    }
    return a;
  }

  // text for a/i/c: "a\" newline text (lines ending in \ continue), or GNU's one-liner "a text"
  std::string text_arg() {
    skip_spaces();
    std::string out;
    if (cur() == '\\') {
      ++p_;
      if (cur() == '\n') ++p_;
      else skip_spaces();
    }
    while (p_ < s_.size()) {
      char c = s_[p_];
      if (c == '\\' && p_ + 1 < s_.size()) {
        char n = s_[p_ + 1];
        if (n == '\n') { out += '\n'; p_ += 2; continue; }
        out += n == 't' ? '\t' : n;
        p_ += 2;
        continue;
      }
      if (c == '\n') { ++p_; break; }
      out += c;
      ++p_;
    }
    return out;
  }

  std::string rest_of_line() {
    skip_spaces();
    size_t e = p_;
    while (e < s_.size() && s_[e] != '\n' && s_[e] != ';' && s_[e] != '}') ++e;
    std::string out = s_.substr(p_, e - p_);
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
    p_ = e;
    return out;
  }

  void command_args(Cmd& c) {
    switch (c.name) {
      case 's': {
        char d = cur();
        if (!d || d == '\n' || d == '\\') throw Error{"unterminated 's' command"};
        ++p_;
        std::string re = delimited(d, true);
        c.replacement = delimited(d, false);
        bool icase = false;
        for (;;) {
          char f = cur();
          if (f == 'g') { c.global = true; ++p_; }
          else if (f == 'p') { c.print = true; ++p_; }
          else if (f == 'i' || f == 'I') { icase = true; ++p_; }
          else if (f == 'm' || f == 'M' || f == 'e') ++p_;
          else if (std::isdigit(static_cast<unsigned char>(f))) { long long n = 0; while (std::isdigit(static_cast<unsigned char>(cur()))) n = n * 10 + (s_[p_++] - '0'); c.nth = n; }
          else if (f == 'w') { ++p_; c.wfile = rest_of_line(); break; }
          else break;
        }
        c.re = compile(re, icase);
        break;
      }
      case 'y': {
        char d = cur();
        ++p_;
        auto unesc = [](const std::string& s) { std::string o; for (size_t i = 0; i < s.size(); ++i) { if (s[i] == '\\' && i + 1 < s.size()) { ++i; o += s[i] == 'n' ? '\n' : s[i] == 't' ? '\t' : s[i]; } else o += s[i]; } return o; };
        c.from = unesc(delimited(d, false));
        c.to = unesc(delimited(d, false));
        if (c.from.size() != c.to.size()) throw Error{"strings for 'y' command are different lengths"};
        break;
      }
      case 'a': case 'i': case 'c': c.text = text_arg(); break;
      case 'r': case 'R': case 'w': case 'W': c.text = rest_of_line(); break;
      case 'b': case 't': case 'T': c.text = rest_of_line(); break;
      case ':': { c.text = rest_of_line(); if (c.text.empty()) throw Error{"\":\" lacks a label"}; break; }
      case 'q': case 'Q': { skip_spaces(); long long n = 0; bool any = false; while (std::isdigit(static_cast<unsigned char>(cur()))) { n = n * 10 + (s_[p_++] - '0'); any = true; } if (any) c.exit_code = static_cast<int>(n); break; }
      case 'l': case 'L': { skip_spaces(); while (std::isdigit(static_cast<unsigned char>(cur()))) ++p_; break; }
      case '{': case '}': case '=': case 'd': case 'D': case 'g': case 'G': case 'h': case 'H': case 'n': case 'N': case 'p': case 'P':
      case 'x': case 'z': case 'F': break;
      case '#': while (p_ < s_.size() && s_[p_] != '\n') ++p_; break;
      default: throw Error{std::string("unknown command: '") + c.name + "'"};
    }
  }
};

std::string apply_replacement(const std::string& rep, const std::smatch& m) {
  std::string out;
  enum { None, Upper, Lower } mode = None;
  bool one_upper = false, one_lower = false;
  auto put = [&](const std::string& s) {
    for (char ch : s) {
      if (one_upper) { ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); one_upper = false; }
      else if (one_lower) { ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); one_lower = false; }
      else if (mode == Upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      else if (mode == Lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      out += ch;
    }
  };
  for (size_t i = 0; i < rep.size(); ++i) {
    char c = rep[i];
    if (c == '&') { put(m.str(0)); continue; }
    if (c == '\\' && i + 1 < rep.size()) {
      char n = rep[++i];
      if (std::isdigit(static_cast<unsigned char>(n))) { size_t g = static_cast<size_t>(n - '0'); if (g < m.size()) put(m.str(g)); continue; }
      if (n == 'n') { out += '\n'; continue; }
      if (n == 't') { out += '\t'; continue; }
      if (n == 'U') { mode = Upper; continue; }
      if (n == 'L') { mode = Lower; continue; }
      if (n == 'E') { mode = None; continue; }
      if (n == 'u') { one_upper = true; continue; }
      if (n == 'l') { one_lower = true; continue; }
      put(std::string(1, n));
      continue;
    }
    put(std::string(1, c));
  }
  return out;
}

// The input lines, read only as the script asks for them: across the files in turn (one file with -s), so a
// script handles a slow producer's lines as they arrive. `$` is answered by reading one line ahead, and only when
// an address asks it.
class Input {
 public:
  Input(State& st, const Io& io, std::vector<std::string> files, char sep, int& status, std::function<void()> before_read)
      : st_(st), io_(io), files_(std::move(files)), sep_(sep), status_(status), before_read_(std::move(before_read)) {}
  Input(const Input&) = delete;
  Input& operator=(const Input&) = delete;
  ~Input() { close(); }

  // A line given up front: -i reads its file whole (CRLF is decided over all of it), then runs the script here.
  void push(std::string line, bool nl) { ahead_.emplace_back(std::move(line), nl); }
  bool next(std::string& line, bool& nl) {
    if (ahead_.empty() && !read_one()) return false;
    line = std::move(ahead_.front().first);
    nl = ahead_.front().second;
    ahead_.pop_front();
    return true;
  }
  // Whether the line just taken is the last one.
  bool last() { return ahead_.empty() && !read_one(); }
  // D: what is left of the pattern space starts the next cycle as if it had been read.
  void unread(std::string line, bool nl) { ahead_.emplace_front(std::move(line), nl); }

 private:
  State& st_;
  const Io& io_;
  std::vector<std::string> files_;
  size_t next_file_ = 0;
  char sep_;
  int& status_;
  std::function<void()> before_read_;
  std::deque<std::pair<std::string, bool>> ahead_;
  Fd fd_ = kNoFd;
  bool mine_ = false, eof_ = false;
  std::string buf_;
  size_t pos_ = 0;

  // One more line onto ahead_ from the sources; false when all are used up. A file's last line without a
  // separator stays one: the next file's first line does not join it.
  bool read_one() {
    for (;;) {
      if (fd_ == kNoFd && !open_next()) return false;
      size_t e = buf_.find(sep_, pos_);
      if (e != std::string::npos) { ahead_.emplace_back(buf_.substr(pos_, e - pos_), true); pos_ = e + 1; return true; }
      if (eof_) {
        bool rest = pos_ < buf_.size();
        if (rest) ahead_.emplace_back(buf_.substr(pos_), false);
        close();
        if (rest) return true;
        continue;
      }
      buf_.erase(0, pos_);
      pos_ = 0;
      // what the script has written so far goes out first, since this read may wait on a slow producer
      if (before_read_) before_read_();
      char b[1 << 16];
      long k = os::read_fd(fd_, b, sizeof b);
      if (k <= 0) eof_ = true;
      else buf_.append(b, static_cast<size_t>(k));
    }
  }
  bool open_next() {
    while (next_file_ < files_.size()) {
      const std::string& f = files_[next_file_++];
      if (f == "-") {
        fd_ = io_.in;
        mine_ = false;
      } else {
        std::string p = host_path(st_.cwd, f), e;
        if (os::is_dir(p)) { io_.ewrite("sed: couldn't read " + f + ": Is a directory\n"); status_ = 2; continue; }
        fd_ = os::open_file(p, os::Open::Read, &e);
        if (fd_ == kNoFd) { io_.ewrite("sed: can't read " + f + ": " + e + "\n"); status_ = 2; continue; }
        mine_ = true;
      }
      buf_.clear();
      pos_ = 0;
      eof_ = false;
      return true;
    }
    return false;
  }
  void close() {
    if (fd_ != kNoFd && mine_) os::close_fd(fd_);
    fd_ = kNoFd;
  }
};

struct Run {
  Script& sc;
  const Io& io;
  State& st;
  bool quiet;
  std::string out;
  std::map<std::string, Fd> wfiles;
  int exit_code = 0;
  bool quit = false;
  bool stream = false;   // out goes to io as it grows; with -i it is the file's new contents, kept whole

  void emit(const std::string& s) { out += s; if (out.size() > (1 << 16)) flush(); }
  void flush() { if (stream && !out.empty()) { io.write(out); out.clear(); } }
  void wfile(const std::string& name, const std::string& line) {
    if (name == "/dev/stdout") { emit(line + "\n"); return; }
    auto it = wfiles.find(name);
    if (it == wfiles.end()) it = wfiles.emplace(name, os::open_file(host_path(st.cwd, name), os::Open::Write, nullptr)).first;
    std::string l = line + "\n";
    os::write_all(it->second, l.data(), l.size());
  }

  bool matches(Addr& a, const std::string& ps, long long ln, Input& in) {
    switch (a.kind) {
      case Addr::None: return true;
      case Addr::Line: return ln == a.n;
      case Addr::Last: return in.last();
      case Addr::Regex: return std::regex_search(ps, *a.re);
      case Addr::Step: return a.step > 0 ? (ln >= a.n && (ln - a.n) % a.step == 0) : ln == a.n;
      case Addr::Plus: return false;
    }
    return false;
  }
  bool selected(Cmd& c, const std::string& ps, long long ln, Input& in) {
    bool hit;
    if (c.a1.kind == Addr::None) hit = true;
    else if (c.a2.kind == Addr::None) hit = matches(c.a1, ps, ln, in);
    else if (c.in_range) {
      hit = true;
      bool end = c.a2.kind == Addr::Line ? ln >= c.a2.n : c.a2.kind == Addr::Plus ? ln >= c.range_end : c.a2.kind == Addr::Last ? in.last() : matches(c.a2, ps, ln, in);
      if (end) c.in_range = false;
    } else if (matches(c.a1, ps, ln, in)) {
      hit = true;
      if (c.a2.kind == Addr::Line && c.a2.n <= ln) c.in_range = false;
      else if (c.a2.kind == Addr::Plus) { c.range_end = ln + c.a2.n; c.in_range = c.a2.n > 0; }
      else c.in_range = true;
    } else hit = false;
    return c.negate ? !hit : hit;
  }

  // One pass over the input, a line taken each cycle (and by n and N).
  void process(Input& in) {
    std::string hold, ps;
    bool nl = false;
    long long ln = 0;
    std::string append_queue;
    while (!quit && in.next(ps, nl)) {
      ++ln;
      bool deleted = false, restart = false, substituted = false;
      append_queue.clear();
      size_t pc = 0;
      while (pc < sc.cmds.size()) {
        Cmd& c = sc.cmds[pc];
        if (c.name == '}' || c.name == ':') { ++pc; continue; }
        if (!selected(c, ps, ln, in)) {
          pc = c.name == '{' ? c.block_end + 1 : pc + 1;
          continue;
        }
        switch (c.name) {
          case '{': break;
          case '=': emit(std::to_string(ln) + "\n"); break;
          case 'p': emit(ps + "\n"); break;
          case 'P': emit(ps.substr(0, ps.find('\n')) + "\n"); break;
          case 'd': deleted = true; break;
          case 'D': {
            size_t nlp = ps.find('\n');
            if (nlp == std::string::npos) { deleted = true; break; }
            ps = ps.substr(nlp + 1);
            restart = true;
            break;
          }
          case 'h': hold = ps; break;
          case 'H': hold += "\n" + ps; break;
          case 'g': ps = hold; break;
          case 'G': ps += "\n" + hold; break;
          case 'x': std::swap(ps, hold); break;
          case 'z': ps.clear(); break;
          case 'n':
            if (!quiet) emit(ps + (nl ? "\n" : ""));
            if (!in.next(ps, nl)) { quit = true; deleted = true; break; }
            ++ln;
            break;
          case 'N': {
            std::string more;
            if (!in.next(more, nl)) { if (!quiet) emit(ps + (nl ? "\n" : "")); deleted = true; quit = true; break; }
            ps += "\n" + more;
            ++ln;
            break;
          }
          case 'a': append_queue += c.text + "\n"; break;
          case 'i': emit(c.text + "\n"); break;
          case 'c':
            if (c.a2.kind == Addr::None || !c.in_range) emit(c.text + "\n");
            deleted = true;
            break;
          case 'r': { std::string d; if (os::read_whole(host_path(st.cwd, c.text), d, nullptr)) append_queue += d; break; }
          case 'w': wfile(c.text, ps); break;
          case 'y':
            for (auto& ch : ps) { size_t at = c.from.find(ch); if (at != std::string::npos) ch = c.to[at]; }
            break;
          case 'q': if (!quiet) emit(ps + (nl ? "\n" : "")); emit(append_queue); append_queue.clear(); exit_code = c.exit_code; quit = true; deleted = true; break;
          case 'Q': exit_code = c.exit_code; quit = true; deleted = true; break;
          case 'l': {
            std::string o;
            for (unsigned char ch : ps) {
              if (ch == '\\') o += "\\\\"; else if (ch == '\t') o += "\\t"; else if (ch == '\n') o += "\\n";
              else if (ch < 32 || ch >= 127) { char b[8]; std::snprintf(b, sizeof b, "\\%03o", ch); o += b; } else o += static_cast<char>(ch);
            }
            emit(o + "$\n");
            break;
          }
          case 'F': emit("-\n"); break;
          case 's': {
            std::string result;
            long long count = 0;
            bool did = false;
            auto begin = ps.cbegin();
            std::smatch mm;
            auto search_from = ps.cbegin();
            std::regex_constants::match_flag_type flags = std::regex_constants::match_default;
            while (std::regex_search(search_from, ps.cend(), mm, *c.re, flags)) {
              ++count;
              auto mstart = mm[0].first, mend = mm[0].second;
              bool replace_this = c.global ? count >= c.nth : count == c.nth;
              result.append(begin, mstart);
              if (replace_this) { result += apply_replacement(c.replacement, mm); did = true; }
              else result.append(mstart, mend);
              begin = mend;
              if (mstart == mend) {
                if (mend == ps.cend()) break;
                result += *mend;
                ++begin;
                search_from = begin;
              } else search_from = mend;
              flags = std::regex_constants::match_prev_avail;
              if (!c.global && count >= c.nth) break;
            }
            if (did) {
              result.append(begin, ps.cend());
              ps = result;
              substituted = true;
              if (c.print) emit(ps + "\n");
              if (!c.wfile.empty()) wfile(c.wfile, ps);
            }
            break;
          }
          case 'b': case 't': case 'T': {
            bool jump = c.name == 'b' || (c.name == 't' && substituted) || (c.name == 'T' && !substituted);
            if (c.name != 'b') substituted = false;
            if (!jump) break;
            if (c.text.empty()) { pc = sc.cmds.size(); continue; }
            size_t target = sc.cmds.size();
            for (size_t k = 0; k < sc.cmds.size(); ++k) if (sc.cmds[k].name == ':' && sc.cmds[k].text == c.text) target = k;
            if (target == sc.cmds.size()) throw Error{"can't find label for jump to '" + c.text + "'"};
            pc = target;
            continue;
          }
        }
        if (deleted || restart) break;
        ++pc;
      }
      if (restart && !deleted) {
        // D with a newline left: run the script again on what remains, without reading a new line
        in.unread(ps, nl);
        --ln;
        continue;
      }
      if (!deleted && !quiet) emit(ps + (nl ? "\n" : ""));
      emit(append_queue);
    }
  }
};

}  // namespace

int util_sed(Shell&, State& st, const std::vector<std::string>& argv, const Io& io) {
  bool quiet = false, in_place = false, separate = false, extended = false, null_data = false;
  std::string suffix;
  std::string script;
  bool have_script = false;
  std::vector<std::string> files;
  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (a == "--") { for (++i; i < argv.size(); ++i) files.push_back(argv[i]); break; }
    if (a.rfind("--", 0) == 0) {
      if (a == "--quiet" || a == "--silent") quiet = true;
      else if (a.rfind("--in-place", 0) == 0) { in_place = true; size_t eq = a.find('='); if (eq != std::string::npos) suffix = a.substr(eq + 1); }
      else if (a == "--regexp-extended") extended = true;
      else if (a == "--separate") separate = true;
      else if (a == "--null-data") null_data = true;
      else if (a.rfind("--expression=", 0) == 0) { script += (have_script ? "\n" : "") + a.substr(13); have_script = true; }
      else if (a == "--expression" && i + 1 < argv.size()) { script += (have_script ? "\n" : "") + argv[++i]; have_script = true; }
      continue;
    }
    if (a.size() > 1 && a[0] == '-') {
      for (size_t k = 1; k < a.size(); ++k) {
        char c = a[k];
        if (c == 'n') quiet = true;
        else if (c == 'E' || c == 'r') extended = true;
        else if (c == 's') separate = true;
        else if (c == 'z') null_data = true;
        else if (c == 'u' || c == 'l') {}
        else if (c == 'i') { in_place = true; suffix = a.substr(k + 1); break; }
        else if (c == 'e' || c == 'f') {
          std::string v = k + 1 < a.size() ? a.substr(k + 1) : (i + 1 < argv.size() ? argv[++i] : std::string());
          if (c == 'f') { std::string d; os::read_whole(host_path(st.cwd, v), d, nullptr); v = d; }
          script += (have_script ? "\n" : "") + v;
          have_script = true;
          break;
        } else { io.ewrite("sed: invalid option -- '" + std::string(1, c) + "'\n"); return 1; }
      }
      continue;
    }
    if (!have_script) { script = a; have_script = true; continue; }
    files.push_back(a);
  }
  if (!have_script) { io.ewrite("Usage: sed [OPTION]... {script} [input-file]...\n"); return 1; }
  Script sc;
  sc.extended = extended;
  try {
    sc.parse(script);
  } catch (const Error& e) {
    io.ewrite("sed: -e expression #1: " + e.msg + "\n");
    return 1;
  }
  char sep = null_data ? '\0' : '\n';
  int status = 0;
  try {
    if (in_place) {
      if (files.empty()) { io.ewrite("sed: no input files\n"); return 1; }
      for (const auto& f : files) {
        std::string p = host_path(st.cwd, f), d, e;
        if (!os::read_whole(p, d, &e)) { io.ewrite("sed: can't read " + f + ": " + e + "\n"); status = 2; continue; }
        // a file written with CRLF keeps its line endings: the \r is held back from the script and put back
        bool crlf = d.find("\r\n") != std::string::npos;
        Script copy = sc;
        Run r{copy, io, st, quiet, {}, {}};
        Input in(st, io, {}, sep, status, nullptr);
        for (size_t a = 0; a < d.size();) {
          size_t b = d.find(sep, a);
          std::string line = d.substr(a, b == std::string::npos ? std::string::npos : b - a);
          if (crlf && !line.empty() && line.back() == '\r') line.pop_back();
          in.push(std::move(line), b != std::string::npos);
          a = b == std::string::npos ? d.size() : b + 1;
        }
        r.process(in);
        std::string out = r.out;
        if (crlf) { std::string o2; for (char c : out) { if (c == '\n') o2 += '\r'; o2 += c; } out = o2; }
        if (!suffix.empty()) { std::string backup = suffix.find('*') != std::string::npos ? suffix : f + suffix; os::write_whole(host_path(st.cwd, backup), d, false, &e); }
        if (!os::write_whole(p, out, false, &e)) { io.ewrite("sed: couldn't write " + f + ": " + e + "\n"); status = 4; }
        for (auto& [k, fd] : r.wfiles) os::close_fd(fd);
        if (r.quit && r.exit_code) status = r.exit_code;
      }
      return status;
    }
    // Without -i the output goes out as the input comes in; -s runs the script afresh on each file.
    auto run_one = [&](std::vector<std::string> names) {
      Run r{sc, io, st, quiet, {}, {}};
      r.stream = true;
      Input in(st, io, std::move(names), sep, status, [&r] { r.flush(); });
      r.process(in);
      r.flush();
      for (auto& [k, fd] : r.wfiles) os::close_fd(fd);
      if (r.quit && r.exit_code) status = r.exit_code;
    };
    if (files.empty()) files.push_back("-");
    if (separate) for (const auto& f : files) run_one({f});
    else run_one(files);
  } catch (const Error& e) {
    io.ewrite("sed: " + e.msg + "\n");
    return 1;
  } catch (const std::regex_error& e) {
    io.ewrite(std::string("sed: ") + e.what() + "\n");
    return 1;
  }
  return status;
}

}  // namespace aiossh
