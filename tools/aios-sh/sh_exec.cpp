// aios-sh's executor: expansion (parameters, command substitution, arithmetic, field splitting, globbing),
// lists, pipelines (one thread per stage, OS pipes between them), compound commands, redirections, the
// builtins that change the shell itself, and host programs.
#include "sh.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <functional>
#include <regex>
#include <stdexcept>
#include <thread>

namespace aiossh {

// A write to a stream whose reader has gone (`yes | head`): the stage stops, as SIGPIPE would stop it.
struct BrokenPipe {};

// An expansion error a non-interactive shell does not survive (set -u on an unbound name, ${x?}): bash exits
// 127 for the first and 1 for the second, and so does this.
struct FatalExpansion : std::runtime_error { int code; FatalExpansion(const std::string& m, int c) : std::runtime_error(m), code(c) {} };

// ---------------------------------------------------------------------------------------------------
// State and I/O
// ---------------------------------------------------------------------------------------------------

std::string State::get(const std::string& name) const {
  if (name == "PWD") {
    std::string p = cwd;
#ifdef _WIN32
    for (auto& c : p) if (c == '\\') c = '/';
#endif
    return p;
  }
  auto it = vars.find(name);
  return it == vars.end() ? std::string() : it->second.value;
}
bool State::has(const std::string& name) const { return name == "PWD" || vars.count(name) > 0; }
void State::set(const std::string& name, const std::string& value) {
  auto& v = vars[name];
  if (v.readonly) throw std::runtime_error(name + ": readonly variable");
  if (!value.empty() && value[0] == kArrayMark) {
    std::vector<std::string> el;
    for (size_t i = 1; i < value.size();) {
      size_t e = value.find(kArraySep, i + 1);
      if (e == std::string::npos) e = value.size();
      el.push_back(value.substr(i + 1, e - i - 1));
      i = e;
    }
    if (assoc.count(name)) {
      auto& m = assoc[name];
      m.clear();
      for (const auto& e : el) {   // [key]=value
        size_t close = e.find("]=");
        if (!e.empty() && e[0] == '[' && close != std::string::npos) m[e.substr(1, close - 1)] = e.substr(close + 2);
      }
      v.value.clear();
      return;
    }
    std::map<long long, std::string> arr;
    long long next = 0;
    for (const auto& e : el) {
      size_t close = e.find("]=");
      long long idx;
      if (!e.empty() && e[0] == '[' && close != std::string::npos && parse_int(e.substr(1, close - 1), idx) && idx >= 0) {
        arr[idx] = e.substr(close + 2);
        next = idx + 1;
      } else {
        arr[next++] = e;
      }
    }
    arrays[name] = arr;
    auto z = arr.find(0);
    v.value = z == arr.end() ? std::string() : z->second;
    return;
  }
  v.value = value;
  auto a = arrays.find(name);
  if (a != arrays.end()) a->second[0] = value;
}
namespace {
// a negative index counts back from one past the highest index
bool array_index(const std::map<long long, std::string>& a, const std::string& key, long long& idx) {
  if (!parse_int(key, idx)) idx = 0;
  if (idx < 0) idx += a.empty() ? 0 : a.rbegin()->first + 1;
  return idx >= 0;
}
}  // namespace
bool State::get_element(const std::string& name, const std::string& key, std::string& out) const {
  auto m = assoc.find(name);
  if (m != assoc.end()) {
    auto it = m->second.find(key);
    if (it == m->second.end()) return false;
    out = it->second;
    return true;
  }
  auto a = arrays.find(name);
  if (a == arrays.end()) {
    long long idx = 0;
    if (!parse_int(key, idx) || (idx != 0 && idx != -1) || !has(name)) return false;
    out = get(name);
    return true;
  }
  long long idx;
  if (!array_index(a->second, key, idx)) return false;
  auto it = a->second.find(idx);
  if (it == a->second.end()) return false;
  out = it->second;
  return true;
}
void State::set_element(const std::string& name, const std::string& key, const std::string& value) {
  auto& v = vars[name];
  if (v.readonly) throw std::runtime_error(name + ": readonly variable");
  auto m = assoc.find(name);
  if (m != assoc.end()) { m->second[key] = value; return; }
  auto a = arrays.find(name);
  if (a == arrays.end()) {
    a = arrays.emplace(name, std::map<long long, std::string>{}).first;
    if (!v.value.empty()) a->second[0] = v.value;
  }
  long long idx;
  if (!array_index(a->second, key, idx)) throw std::runtime_error(name + "[" + key + "]: bad array subscript");
  a->second[idx] = value;
  auto z = a->second.find(0);
  v.value = z == a->second.end() ? std::string() : z->second;
}
void State::unset_element(const std::string& name, const std::string& key) {
  auto m = assoc.find(name);
  if (m != assoc.end()) { m->second.erase(key); return; }
  auto a = arrays.find(name);
  if (a == arrays.end()) return;
  long long idx;
  if (array_index(a->second, key, idx)) a->second.erase(idx);
  if (idx == 0) vars[name].value.clear();
}
std::vector<std::string> State::elements(const std::string& name) const {
  std::vector<std::string> out;
  auto m = assoc.find(name);
  if (m != assoc.end()) { for (const auto& [k, v] : m->second) out.push_back(v); return out; }
  auto a = arrays.find(name);
  if (a != arrays.end()) { for (const auto& [k, v] : a->second) out.push_back(v); return out; }
  if (has(name)) out.push_back(get(name));
  return out;
}
std::vector<std::string> State::keys(const std::string& name) const {
  std::vector<std::string> out;
  auto m = assoc.find(name);
  if (m != assoc.end()) { for (const auto& [k, v] : m->second) out.push_back(k); return out; }
  auto a = arrays.find(name);
  if (a != arrays.end()) { for (const auto& [k, v] : a->second) out.push_back(std::to_string(k)); return out; }
  if (has(name)) out.push_back("0");
  return out;
}std::vector<std::string> State::env_list() const {
  std::vector<std::string> out;
  for (const auto& [k, v] : vars) if (v.exported) out.push_back(k + "=" + v.value);
  std::string pwd = get("PWD");
  out.push_back("PWD=" + pwd);
  return out;
}

void Io::write(const std::string& s) const {
  if (!os::write_all(out, s.data(), s.size())) throw BrokenPipe{};
}
void Io::ewrite(const std::string& s) const { os::write_all(err, s.data(), s.size()); }
std::string Io::read_all() const {
  std::string s;
  char buf[1 << 16];
  long n;
  while ((n = os::read_fd(in, buf, sizeof buf)) > 0) s.append(buf, static_cast<size_t>(n));
  return s;
}

bool LineReader::next(std::string& line, bool* had_newline) {
  for (;;) {
    size_t nl = buf_.find('\n', pos_);
    if (nl != std::string::npos) {
      line = buf_.substr(pos_, nl - pos_);
      pos_ = nl + 1;
      if (had_newline) *had_newline = true;
      return true;
    }
    if (eof_) {
      if (pos_ >= buf_.size()) return false;
      line = buf_.substr(pos_);
      pos_ = buf_.size();
      if (had_newline) *had_newline = false;
      return true;
    }
    if (pos_ > 0) { buf_.erase(0, pos_); pos_ = 0; }
    char tmp[1 << 16];
    long n = os::read_fd(fd_, tmp, sizeof tmp);
    if (n <= 0) eof_ = true;
    else buf_.append(tmp, static_cast<size_t>(n));
  }
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) s += (i ? sep : "") + v[i];
  return s;
}
std::vector<std::string> split_lines(const std::string& s) {
  std::vector<std::string> out;
  size_t a = 0;
  while (a < s.size()) {
    size_t b = s.find('\n', a);
    if (b == std::string::npos) { out.push_back(s.substr(a)); break; }
    out.push_back(s.substr(a, b - a));
    a = b + 1;
  }
  return out;
}
bool parse_int(const std::string& s, long long& out) {
  if (s.empty()) return false;
  size_t i = 0;
  bool neg = false;
  if (s[0] == '-' || s[0] == '+') { neg = s[0] == '-'; i = 1; }
  if (i >= s.size()) return false;
  long long v = 0;
  for (; i < s.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    v = v * 10 + (s[i] - '0');
  }
  out = neg ? -v : v;
  return true;
}

void Shell::error(const Io& io, const std::string& msg) {
  std::lock_guard<std::mutex> l(io_mu);
  io.ewrite("aios-sh: " + msg + "\n");
}

// ---------------------------------------------------------------------------------------------------
// Globbing
// ---------------------------------------------------------------------------------------------------

bool has_glob_chars(const std::string& s) {
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\') { ++i; continue; }
    if (s[i] == '*' || s[i] == '?' || s[i] == '[') return true;
  }
  return false;
}

namespace {
bool match_class(const std::string& p, size_t& i, char c) {
  // p[i] is just past '['
  bool neg = false;
  if (i < p.size() && (p[i] == '!' || p[i] == '^')) { neg = true; ++i; }
  bool hit = false;
  bool first = true;
  while (i < p.size() && (p[i] != ']' || first)) {
    first = false;
    if (p[i] == '[' && i + 1 < p.size() && p[i + 1] == ':') {
      size_t e = p.find(":]", i + 2);
      if (e != std::string::npos) {
        std::string cls = p.substr(i + 2, e - i - 2);
        unsigned char u = static_cast<unsigned char>(c);
        if ((cls == "alpha" && std::isalpha(u)) || (cls == "digit" && std::isdigit(u)) || (cls == "alnum" && std::isalnum(u)) ||
            (cls == "space" && std::isspace(u)) || (cls == "upper" && std::isupper(u)) || (cls == "lower" && std::islower(u)) ||
            (cls == "punct" && std::ispunct(u)) || (cls == "xdigit" && std::isxdigit(u)))
          hit = true;
        i = e + 2;
        continue;
      }
    }
    char lo = p[i];
    if (lo == '\\' && i + 1 < p.size()) lo = p[++i];
    if (i + 2 < p.size() && p[i + 1] == '-' && p[i + 2] != ']') {
      char hi = p[i + 2];
      if (c >= lo && c <= hi) hit = true;
      i += 3;
      continue;
    }
    if (c == lo) hit = true;
    ++i;
  }
  if (i < p.size()) ++i;   // past ']'
  return hit != neg;
}
bool gmatch(const std::string& p, size_t pi, const std::string& s, size_t si) {
  while (pi < p.size()) {
    char c = p[pi];
    if (c == '*') {
      while (pi < p.size() && p[pi] == '*') ++pi;
      if (pi == p.size()) return true;
      for (size_t k = si; k <= s.size(); ++k) if (gmatch(p, pi, s, k)) return true;
      return false;
    }
    if (si >= s.size()) return false;
    if (c == '?') { ++pi; ++si; continue; }
    if (c == '[') {
      size_t close = pi + 1;
      if (close < p.size() && (p[close] == '!' || p[close] == '^')) ++close;
      if (close < p.size() && p[close] == ']') ++close;
      close = p.find(']', close);
      if (close != std::string::npos) {
        size_t k = pi + 1;
        if (!match_class(p, k, s[si])) return false;
        pi = k; ++si;
        continue;
      }
    }
    if (c == '\\' && pi + 1 < p.size()) { c = p[++pi]; }
    if (c != s[si]) return false;
    ++pi; ++si;
  }
  return si == s.size();
}
}  // namespace

bool glob_match(const std::string& pattern, const std::string& s) { return gmatch(pattern, 0, s, 0); }

std::vector<std::string> glob_expand(const std::string& cwd, const std::string& pattern) {
  // Split into segments on '/' (and '\' on Windows when not escaping a glob char); expand segment by segment.
  std::vector<std::string> segs;
  std::string cur;
  std::string prefix;   // a leading "/" or "C:/" kept as typed
  size_t i = 0;
#ifdef _WIN32
  if (pattern.size() >= 2 && pattern[1] == ':') { prefix = pattern.substr(0, 2); i = 2; if (i < pattern.size() && (pattern[i] == '/' || pattern[i] == '\\')) { prefix += pattern[i]; ++i; } }
  else
#endif
  if (!pattern.empty() && pattern[0] == '/') { prefix = "/"; i = 1; }
  for (; i < pattern.size(); ++i) {
    char c = pattern[i];
    if (c == '\\' && i + 1 < pattern.size() && std::strchr("*?[]\\", pattern[i + 1])) { cur += c; cur += pattern[++i]; continue; }
    if (c == '/'
#ifdef _WIN32
        || c == '\\'
#endif
    ) { if (!cur.empty()) segs.push_back(cur); cur.clear(); continue; }
    cur += c;
  }
  if (!cur.empty()) segs.push_back(cur);
  auto unescape = [](const std::string& s) { std::string o; for (size_t k = 0; k < s.size(); ++k) { if (s[k] == '\\' && k + 1 < s.size()) ++k; o += s[k]; } return o; };
  std::vector<std::string> paths = {prefix};   // as the user will see them
  for (size_t si = 0; si < segs.size(); ++si) {
    const std::string& seg = segs[si];
    bool last = si + 1 == segs.size();
    std::vector<std::string> next;
    for (const auto& base : paths) {
      std::string shown_base = base;
      if (!shown_base.empty() && shown_base.back() != '/' && shown_base.back() != '\\') shown_base += '/';
      if (!has_glob_chars(seg)) {
        std::string cand = shown_base + unescape(seg);
        if (last || os::is_dir(host_path(cwd, cand))) next.push_back(cand);
        continue;
      }
      std::vector<os::DirEntry> ents;
      os::list_dir(host_path(cwd, base.empty() ? "." : base), ents);
      std::sort(ents.begin(), ents.end(), [](const os::DirEntry& a, const os::DirEntry& b) { return a.name < b.name; });
      for (const auto& e : ents) {
        if (e.name[0] == '.' && seg[0] != '.') continue;
        if (!glob_match(seg, e.name)) continue;
        if (!last && !e.dir) continue;
        next.push_back(shown_base + e.name);
      }
    }
    paths.swap(next);
    if (paths.empty()) break;
  }
  // the existence of a last literal segment was not checked: drop what is not there
  std::vector<std::string> out;
  for (const auto& p : paths) if (!p.empty() && os::exists(host_path(cwd, p))) out.push_back(p);
  return out;
}

// ---------------------------------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------------------------------

namespace {
class Arith {
 public:
  Arith(Shell& sh, State& st, const Io& io, const std::string& e) : sh_(sh), st_(st), io_(io), e_(e) {}
  long long run() {
    long long v = comma();
    skip();
    if (p_ < e_.size()) throw std::runtime_error("arithmetic syntax error near '" + e_.substr(p_) + "'");
    return v;
  }

 private:
  Shell& sh_; State& st_; const Io& io_; std::string e_; size_t p_ = 0;
  void skip() { while (p_ < e_.size() && std::isspace(static_cast<unsigned char>(e_[p_]))) ++p_; }
  bool eat(const char* op) {
    skip();
    size_t n = std::strlen(op);
    if (e_.compare(p_, n, op) == 0) {
      // don't take "<" out of "<=", "<<", etc.
      if (n == 1 && p_ + 1 < e_.size()) {
        char a = e_[p_], b = e_[p_ + 1];
        if ((a == '<' || a == '>' || a == '=' || a == '!') && b == '=') return false;
        if ((a == '<' && b == '<') || (a == '>' && b == '>') || (a == '&' && b == '&') || (a == '|' && b == '|')) return false;
        if ((a == '+' && b == '+') || (a == '-' && b == '-')) return false;
        if (std::strchr("+-*/%&|^", a) && b == '=') return false;
      }
      p_ += n;
      return true;
    }
    return false;
  }
  // name, or name[key] for an array element (the key already evaluated)
  long long var_value(const std::string& ref) {
    std::string v;
    size_t lb = ref.find('[');
    if (lb != std::string::npos) { if (!st_.get_element(ref.substr(0, lb), ref.substr(lb + 1, ref.size() - lb - 2), v)) v.clear(); }
    else v = st_.get(ref);
    if (v.empty()) return 0;
    long long out = 0;
    if (parse_int(v, out)) return out;
    return Arith(sh_, st_, io_, v).run();
  }
  void set_var(const std::string& ref, long long value) {
    size_t lb = ref.find('[');
    if (lb != std::string::npos) st_.set_element(ref.substr(0, lb), ref.substr(lb + 1, ref.size() - lb - 2), std::to_string(value));
    else st_.set(ref, std::to_string(value));
  }
  std::string ident() {
    skip();
    size_t a = p_;
    while (p_ < e_.size() && (std::isalnum(static_cast<unsigned char>(e_[p_])) || e_[p_] == '_')) ++p_;
    std::string name = e_.substr(a, p_ - a);
    if (p_ < e_.size() && e_[p_] == '[') {
      int depth = 0;
      size_t close = p_;
      for (; close < e_.size(); ++close) { if (e_[close] == '[') ++depth; else if (e_[close] == ']' && --depth == 0) break; }
      std::string sub = e_.substr(p_ + 1, close - p_ - 1);
      p_ = close < e_.size() ? close + 1 : close;
      std::string key = st_.assoc.count(name) ? sub : std::to_string(Arith(sh_, st_, io_, sub).run());
      return name + "[" + key + "]";
    }
    return name;
  }
  long long comma() { long long v = assign(); while (eat(",")) v = assign(); return v; }
  long long assign() {
    skip();
    size_t save = p_;
    if (p_ < e_.size() && (std::isalpha(static_cast<unsigned char>(e_[p_])) || e_[p_] == '_')) {
      std::string name = ident();
      skip();
      static const char* ops[] = {"+=", "-=", "*=", "/=", "%=", "<<=", ">>=", "&=", "|=", "^="};
      for (const char* op : ops) {
        if (e_.compare(p_, std::strlen(op), op) == 0) {
          p_ += std::strlen(op);
          long long r = assign(), l = var_value(name), v = 0;
          std::string o = op;
          if (o == "+=") v = l + r; else if (o == "-=") v = l - r; else if (o == "*=") v = l * r;
          else if (o == "/=") { if (!r) throw std::runtime_error("division by zero"); v = l / r; }
          else if (o == "%=") { if (!r) throw std::runtime_error("division by zero"); v = l % r; }
          else if (o == "<<=") v = l << r; else if (o == ">>=") v = l >> r; else if (o == "&=") v = l & r;
          else if (o == "|=") v = l | r; else v = l ^ r;
          set_var(name, v);
          return v;
        }
      }
      if (p_ < e_.size() && e_[p_] == '=' && (p_ + 1 >= e_.size() || e_[p_ + 1] != '=')) {
        ++p_;
        long long v = assign();
        set_var(name, v);
        return v;
      }
    }
    p_ = save;
    return ternary();
  }
  long long ternary() {
    long long c = lor();
    if (eat("?")) {
      long long a = assign();
      if (!eat(":")) throw std::runtime_error("expected ':' in ?:");
      long long b = assign();
      return c ? a : b;
    }
    return c;
  }
  long long lor() { long long v = land(); while (eat("||")) { long long r = land(); v = (v || r) ? 1 : 0; } return v; }
  long long land() { long long v = bor(); while (eat("&&")) { long long r = bor(); v = (v && r) ? 1 : 0; } return v; }
  long long bor() { long long v = bxor(); while (eat("|")) v |= bxor(); return v; }
  long long bxor() { long long v = band(); while (eat("^")) v ^= band(); return v; }
  long long band() { long long v = eq(); while (eat("&")) v &= eq(); return v; }
  long long eq() {
    long long v = rel();
    for (;;) {
      if (eat("==")) v = v == rel(); else if (eat("!=")) v = v != rel(); else return v;
    }
  }
  long long rel() {
    long long v = shift();
    for (;;) {
      if (eat("<=")) v = v <= shift(); else if (eat(">=")) v = v >= shift();
      else if (eat("<")) v = v < shift(); else if (eat(">")) v = v > shift(); else return v;
    }
  }
  long long shift() {
    long long v = add();
    for (;;) { if (eat("<<")) v <<= add(); else if (eat(">>")) v >>= add(); else return v; }
  }
  long long add() {
    long long v = mul();
    for (;;) { if (eat("+")) v += mul(); else if (eat("-")) v -= mul(); else return v; }
  }
  long long mul() {
    long long v = power();
    for (;;) {
      if (eat("*")) v *= power();
      else if (eat("/")) { long long r = power(); if (!r) throw std::runtime_error("division by zero"); v /= r; }
      else if (eat("%")) { long long r = power(); if (!r) throw std::runtime_error("division by zero"); v %= r; }
      else return v;
    }
  }
  long long power() {
    long long b = unary();
    skip();
    if (e_.compare(p_, 2, "**") == 0) { p_ += 2; long long ex = power(); long long r = 1; while (ex-- > 0) r *= b; return r; }
    return b;
  }
  long long unary() {
    skip();
    if (e_.compare(p_, 2, "++") == 0 || e_.compare(p_, 2, "--") == 0) {
      bool inc = e_[p_] == '+';
      p_ += 2;
      std::string name = ident();
      long long v = var_value(name) + (inc ? 1 : -1);
      set_var(name, v);
      return v;
    }
    if (eat("!")) return !unary();
    if (eat("~")) return ~unary();
    if (eat("-")) return -unary();
    if (eat("+")) return unary();
    return primary();
  }
  long long primary() {
    skip();
    if (p_ >= e_.size()) throw std::runtime_error("arithmetic: expected an operand");
    if (e_[p_] == '(') {
      ++p_;
      long long v = comma();
      if (!eat(")")) throw std::runtime_error("arithmetic: expected ')'");
      return v;
    }
    if (e_[p_] == '$') {
      ++p_;
      if (p_ < e_.size() && e_[p_] == '{') { size_t e = e_.find('}', p_); std::string n = e_.substr(p_ + 1, e - p_ - 1); p_ = e + 1; return var_value(n); }
      return var_value(ident());
    }
    if (std::isdigit(static_cast<unsigned char>(e_[p_]))) {
      size_t a = p_;
      while (p_ < e_.size() && std::isalnum(static_cast<unsigned char>(e_[p_]))) ++p_;
      std::string num = e_.substr(a, p_ - a);
      if (num.size() > 1 && num[0] == '0' && (num[1] == 'x' || num[1] == 'X')) return std::stoll(num.substr(2), nullptr, 16);
      if (num.size() > 1 && num[0] == '0') return std::stoll(num, nullptr, 8);
      size_t hash = num.find('#');
      return std::stoll(num, nullptr, 10) + (hash == std::string::npos ? 0 : 0);
    }
    if (std::isalpha(static_cast<unsigned char>(e_[p_])) || e_[p_] == '_') {
      std::string name = ident();
      skip();
      if (e_.compare(p_, 2, "++") == 0 || e_.compare(p_, 2, "--") == 0) {
        bool inc = e_[p_] == '+';
        p_ += 2;
        long long v = var_value(name);
        set_var(name, v + (inc ? 1 : -1));
        return v;
      }
      return var_value(name);
    }
    throw std::runtime_error("arithmetic syntax error near '" + e_.substr(p_) + "'");
  }
};
}  // namespace

long long Shell::arith(State& st, const std::string& expr, const Io& io) {
  // $x, ${x} and $(...) inside are expanded first, as POSIX does.
  std::string text = expr;
  if (text.find('$') != std::string::npos || text.find('`') != std::string::npos) text = expand_word_nosplit(st, parse_word_text(text), io);
  return Arith(*this, st, io, text).run();
}

// ---------------------------------------------------------------------------------------------------
// Expansion
// ---------------------------------------------------------------------------------------------------

namespace {

// Text being built for one or more fields. Each character carries whether it came quoted (no split, no glob)
// and whether it came from an expansion (split on IFS when unquoted).
struct Fields {
  struct F { std::string text; std::string quoted; std::string from_exp; bool exists = false; };
  std::vector<F> f = {F{}};
  void add(const std::string& s, bool quoted, bool from_exp) {
    f.back().text += s;
    f.back().quoted.append(s.size(), quoted ? 1 : 0);
    f.back().from_exp.append(s.size(), from_exp ? 1 : 0);
    if (quoted || !s.empty()) f.back().exists = true;
  }
  void mark_exists() { f.back().exists = true; }
  void new_field() { f.push_back(F{}); }
};

}  // namespace

std::string Shell::command_substitution(State& st, const std::string& text, const Io& io) {
  Fd r, w;
  if (!os::make_pipe(r, w)) { error(io, "cannot make a pipe"); return {}; }
  State sub = st;
  Io sio{io.in, w, io.err};
  int status = 0;
  std::thread t([&]() {
    try { status = run_text(sub, text, sio); }
    catch (const ExitSignal& e) { status = e.status; }
    catch (const BrokenPipe&) { status = 141; }
    catch (...) { status = 2; }
    os::close_fd(w);
  });
  std::string out;
  char buf[1 << 16];
  long n;
  while ((n = os::read_fd(r, buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
  t.join();
  os::close_fd(r);
  st.last_status = status;
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

namespace {

std::string param_value(Shell& sh, State& st, const std::string& name, bool* set) {
  *set = true;
  if (name == "?") return std::to_string(st.last_status);
  if (name == "$") return std::to_string(os::self_pid());
  if (name == "!") return st.last_bg_pid ? std::to_string(st.last_bg_pid) : std::string();
  if (name == "#") return std::to_string(st.params.size());
  if (name == "0") return st.arg0;
  if (name == "-") {
    std::string f;
    if (st.opt_errexit) f += 'e';
    if (st.opt_nounset) f += 'u';
    if (st.opt_xtrace) f += 'x';
    if (st.opt_noglob) f += 'f';
    return f;
  }
  if (name == "@" || name == "*") return join(st.params, " ");
  long long idx;
  if (parse_int(name, idx) && idx > 0) {
    if (static_cast<size_t>(idx) <= st.params.size()) return st.params[static_cast<size_t>(idx - 1)];
    *set = false;
    return {};
  }
  if (name == "RANDOM") { static unsigned seed = static_cast<unsigned>(os::now_seconds() ^ os::self_pid()); seed = seed * 1103515245 + 12345; return std::to_string((seed >> 16) % 32768); }
  if (name == "SECONDS") { static int64_t start = os::now_seconds(); return std::to_string(os::now_seconds() - start); }
  (void)sh;
  *set = st.has(name);
  return st.get(name);
}

// The longest/shortest prefix/suffix of v matching pat.
std::string strip(const std::string& v, const std::string& pat, bool suffix, bool longest) {
  if (!suffix) {
    if (longest) { for (size_t k = v.size() + 1; k-- > 0;) if (glob_match(pat, v.substr(0, k))) return v.substr(k); }
    else { for (size_t k = 0; k <= v.size(); ++k) if (glob_match(pat, v.substr(0, k))) return v.substr(k); }
  } else {
    if (longest) { for (size_t k = 0; k <= v.size(); ++k) if (glob_match(pat, v.substr(k))) return v.substr(0, k); }
    else { for (size_t k = v.size() + 1; k-- > 0;) if (glob_match(pat, v.substr(k))) return v.substr(0, k); }
  }
  return v;
}

}  // namespace

namespace {

size_t utf8_length(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
  return n;
}

// ${name[sub]rest}: the name, the subscript when there is one, and the operator after them
struct ParamRef { std::string name, sub, rest; bool has_sub = false; };
ParamRef split_param(const std::string& body) {
  ParamRef r;
  size_t k = 0;
  if (k < body.size() && (std::isalpha(static_cast<unsigned char>(body[0])) || body[0] == '_')) {
    while (k < body.size() && (std::isalnum(static_cast<unsigned char>(body[k])) || body[k] == '_')) ++k;
  } else if (k < body.size()) {
    if (std::isdigit(static_cast<unsigned char>(body[0]))) { while (k < body.size() && std::isdigit(static_cast<unsigned char>(body[k]))) ++k; }
    else k = 1;
  }
  r.name = body.substr(0, k);
  if (k < body.size() && body[k] == '[' && k > 0 && (std::isalpha(static_cast<unsigned char>(body[0])) || body[0] == '_')) {
    int depth = 0;
    for (size_t i = k; i < body.size(); ++i) {
      if (body[i] == '[') ++depth;
      else if (body[i] == ']' && --depth == 0) { r.sub = body.substr(k + 1, i - k - 1); r.has_sub = true; k = i + 1; break; }
    }
  }
  r.rest = body.substr(k);
  return r;
}

std::string subscript_key(Shell& sh, State& st, const std::string& name, const std::string& sub, const Io& io) {
  if (st.assoc.count(name)) return sh.expand_word_nosplit(st, parse_word_text(sub), io);
  return std::to_string(sh.arith(st, sub, io));
}

bool is_slice(const std::string& rest) { return rest.size() > 1 && rest[0] == ':' && !std::strchr("-=?+", rest[1]); }

std::vector<std::string> slice(Shell& sh, State& st, const std::vector<std::string>& v, const std::string& rest, const Io& io) {
  std::string spec = rest.substr(1);
  size_t c = spec.find(':');
  long long n = static_cast<long long>(v.size());
  long long off = sh.arith(st, spec.substr(0, c), io);
  long long len = c == std::string::npos ? n : sh.arith(st, spec.substr(c + 1), io);
  if (off < 0) off += n;
  if (off < 0 || off > n) return {};
  if (len < 0) len = n - off + len;
  if (len < 0) len = 0;
  long long end = std::min(n, off + len);
  return std::vector<std::string>(v.begin() + off, v.begin() + end);
}

std::string apply_ops(Shell& sh, State& st, const std::string& name, std::string v, bool set, const std::string& rest, const Io& io);

}  // namespace

// Expands one ${...} body (or a plain name) to its value.
static std::string expand_param(Shell& sh, State& st, const WordPart& part, const Io& io) {
  const std::string& body = part.text;
  if (!part.braced) {
    bool set;
    std::string v = param_value(sh, st, body, &set);
    if (!set && st.opt_nounset && body != "@" && body != "*") throw FatalExpansion(body + ": unbound variable", 127);
    return v;
  }
  if (body.size() > 1 && body[0] == '!') {
    ParamRef r = split_param(body.substr(1));
    if (r.has_sub && (r.sub == "@" || r.sub == "*")) return join(st.keys(r.name), " ");   // ${!a[@]}
    if (r.rest == "*" || r.rest == "@") {   // ${!prefix*}: the names starting with prefix
      std::vector<std::string> names;
      for (const auto& [k, var] : st.vars) if (k.compare(0, r.name.size(), r.name) == 0) names.push_back(k);
      return join(names, " ");
    }
    bool set;
    std::string target = param_value(sh, st, r.name, &set);   // ${!name}: the variable name holds
    ParamRef t = split_param(target);
    std::string v;
    if (t.has_sub) set = st.get_element(t.name, subscript_key(sh, st, t.name, t.sub, io), v);
    else v = param_value(sh, st, target, &set);
    return apply_ops(sh, st, target, v, set, r.rest, io);
  }
  if (body.size() > 1 && body[0] == '#' && body != "#") {   // ${#name}, ${#a[@]}, ${#a[i]}
    ParamRef r = split_param(body.substr(1));
    if (r.has_sub && (r.sub == "@" || r.sub == "*")) return std::to_string(st.elements(r.name).size());
    if (r.name == "@" || r.name == "*") return std::to_string(st.params.size());
    std::string v;
    if (r.has_sub) { if (!st.get_element(r.name, subscript_key(sh, st, r.name, r.sub, io), v)) v.clear(); }
    else { bool set; v = param_value(sh, st, r.name, &set); }
    return std::to_string(utf8_length(v));
  }
  ParamRef r = split_param(body);
  bool set = false;
  std::string v;
  if (r.has_sub && (r.sub == "@" || r.sub == "*")) {
    auto el = st.elements(r.name);
    if (is_slice(r.rest)) return join(slice(sh, st, el, r.rest, io), " ");
    v = join(el, " ");
    set = !el.empty();
  } else if (r.has_sub) {
    set = st.get_element(r.name, subscript_key(sh, st, r.name, r.sub, io), v);
    if (!set && st.opt_nounset && r.rest.empty()) throw FatalExpansion(r.name + "[" + r.sub + "]: unbound variable", 127);
    if (!set) v.clear();
  } else {
    if ((r.name == "@" || r.name == "*") && is_slice(r.rest)) {
      std::vector<std::string> all{st.arg0};
      all.insert(all.end(), st.params.begin(), st.params.end());
      return join(slice(sh, st, all, r.rest, io), " ");
    }
    v = param_value(sh, st, r.name, &set);
    if (r.rest.empty() && !set && st.opt_nounset && r.name != "@" && r.name != "*") throw FatalExpansion(r.name + ": unbound variable", 127);
  }
  return apply_ops(sh, st, r.has_sub ? r.name + "[" + r.sub + "]" : r.name, v, set, r.rest, io);
}

namespace {
std::string apply_ops(Shell& sh, State& st, const std::string& name, std::string v, bool set, const std::string& rest, const Io& io) {
  const std::string body = name + rest;
  auto word = [&](const std::string& w) { return sh.expand_word_nosplit(st, parse_word_text(w), io); };
  if (rest.empty()) {
    if (!set && st.opt_nounset && name != "@" && name != "*") throw FatalExpansion(name + ": unbound variable", 127);
    return v;
  }
  bool colon = rest[0] == ':' && rest.size() > 1 && std::strchr("-=?+", rest[1]);
  std::string op = colon ? rest.substr(0, 2) : rest.substr(0, 1);
  std::string arg = rest.substr(op.size());
  bool missing = colon ? (!set || v.empty()) : !set;
  if (op == "-" || op == ":-") return missing ? word(arg) : v;
  if (op == "=" || op == ":=") {
    if (missing) {
      v = word(arg);
      size_t lb = name.find('[');
      if (lb == std::string::npos) st.set(name, v);
      else st.set_element(name.substr(0, lb), subscript_key(sh, st, name.substr(0, lb), name.substr(lb + 1, name.size() - lb - 2), io), v);
    }
    return v;
  }
  if (op == "?" || op == ":?") {
    if (missing) throw FatalExpansion(name + ": " + (arg.empty() ? std::string("parameter null or not set") : word(arg)), 1);
    return v;
  }
  if (op == "+" || op == ":+") return missing ? std::string() : word(arg);
  if (rest[0] == '#' || rest[0] == '%') {
    bool longest = rest.size() > 1 && rest[1] == rest[0];
    std::string pat = word(rest.substr(longest ? 2 : 1));
    return strip(v, pat, rest[0] == '%', longest);
  }
  if (rest[0] == '/') {
    bool all = rest.size() > 1 && rest[1] == '/';
    std::string r = rest.substr(all ? 2 : 1);
    size_t slash = r.find('/');
    std::string pat = word(r.substr(0, slash)), rep = slash == std::string::npos ? "" : word(r.substr(slash + 1));
    bool anchor_start = !pat.empty() && pat[0] == '#', anchor_end = !pat.empty() && pat[0] == '%';
    if (anchor_start || anchor_end) pat = pat.substr(1);
    std::string out;
    size_t i = 0;
    bool done = false;
    while (i <= v.size()) {
      size_t best = std::string::npos;
      if (!done && (!anchor_start || i == 0)) {
        for (size_t len = v.size() - i + 1; len-- > 0;) {
          if (anchor_end && i + len != v.size()) continue;
          if (glob_match(pat, v.substr(i, len)) && (len > 0 || pat.empty())) { best = len; break; }
        }
      }
      if (best != std::string::npos && best > 0) {
        out += rep;
        i += best;
        if (!all) done = true;
        continue;
      }
      if (i < v.size()) out += v[i];
      ++i;
    }
    return out;
  }
  if (rest[0] == ':') {   // ${v:offset:length}
    std::string spec = rest.substr(1);
    size_t c = spec.find(':');
    long long off = sh.arith(st, spec.substr(0, c), io);
    long long len = c == std::string::npos ? static_cast<long long>(v.size()) : sh.arith(st, spec.substr(c + 1), io);
    if (off < 0) off += static_cast<long long>(v.size());
    if (off < 0) off = 0;
    if (static_cast<size_t>(off) > v.size()) return {};
    if (len < 0) len = static_cast<long long>(v.size()) - off + len;
    if (len < 0) len = 0;
    return v.substr(static_cast<size_t>(off), static_cast<size_t>(len));
  }
  if (rest == "^^" || rest == ",," || rest == "^" || rest == ",") {
    std::string o = v;
    bool up = rest[0] == '^';
    for (size_t i = 0; i < o.size(); ++i) {
      o[i] = static_cast<char>(up ? std::toupper(static_cast<unsigned char>(o[i])) : std::tolower(static_cast<unsigned char>(o[i])));
      if (rest.size() == 1) break;
    }
    return o;
  }
  throw std::runtime_error("${" + body + "}: bad substitution");
}
}  // namespace

namespace {

// $@, $*, ${@:n}, ${a[@]}, ${a[*]}, ${!a[@]}, ${a[@]:n:m}, ${a[@]/x/y}...: a list of fields rather than one
// value. star: the [*] / $* form, which joins inside double quotes.
bool list_param(Shell& sh, State& st, const WordPart& part, const Io& io, std::vector<std::string>& list, bool& star) {
  if (!part.braced) {
    if (part.text != "@" && part.text != "*") return false;
    list = st.params;
    star = part.text == "*";
    return true;
  }
  const std::string& body = part.text;
  if (body.size() > 1 && body[0] == '!') {
    ParamRef r = split_param(body.substr(1));
    if (!r.has_sub || (r.sub != "@" && r.sub != "*") || !r.rest.empty()) return false;
    list = st.keys(r.name);
    star = r.sub == "*";
    return true;
  }
  if (!body.empty() && body[0] == '#') return false;
  ParamRef r = split_param(body);
  std::vector<std::string> base;
  if ((r.name == "@" || r.name == "*") && !r.has_sub) {
    star = r.name == "*";
    if (is_slice(r.rest)) {
      std::vector<std::string> all{st.arg0};
      all.insert(all.end(), st.params.begin(), st.params.end());
      list = slice(sh, st, all, r.rest, io);
      return true;
    }
    base = st.params;
  } else if (r.has_sub && (r.sub == "@" || r.sub == "*")) {
    star = r.sub == "*";
    base = st.elements(r.name);
    if (is_slice(r.rest)) { list = slice(sh, st, base, r.rest, io); return true; }
  } else {
    return false;
  }
  if (r.rest.empty()) { list = base; return true; }
  if (std::strchr("#%/^,", r.rest[0])) {   // per element
    for (const auto& e : base) list.push_back(apply_ops(sh, st, r.name, e, true, r.rest, io));
    return true;
  }
  return false;   // ${a[@]:-x} and the like: one value
}

// The elements of a=( ... ): its words, expanded as a command's are (split, globbed, brace-expanded).
std::vector<std::string> array_elements(Shell& sh, State& st, const std::string& body, const Io& io) {
  std::shared_ptr<Node> tree;
  try { tree = parse(": " + body); } catch (const ParseError& e) { throw std::runtime_error("syntax error in array: " + e.message); }
  std::vector<Word> words;
  std::function<void(const Node&)> gather = [&](const Node& n) {
    if (n.kind == Node::Simple) { for (const auto& w : n.words) words.push_back(w); return; }
    for (const auto& k : n.kids) if (k) gather(*k);
  };
  gather(*tree);
  if (!words.empty()) words.erase(words.begin());   // the ':'
  return sh.expand_words(st, words, io);
}

// <( cmd ): cmd's output, in a file whose path is the word. >( cmd ) is not supported.
std::string process_substitution(Shell& sh, State& st, const std::string& text, bool output, const Io& io) {
  if (output) throw std::runtime_error(">(...): output process substitution is not supported");
  static std::atomic<int> counter{0};
  std::string path = host_path(st.cwd, "/tmp") + "/aios-sh-" + std::to_string(os::self_pid()) + "-" + std::to_string(++counter);
#ifdef _WIN32
  for (auto& c : path) if (c == '\\') c = '/';
#endif
  std::string err;
  Fd f = os::open_file(path, os::Open::Write, &err);
  if (f == kNoFd) throw std::runtime_error("<(...): " + path + ": " + err);
  {
    std::lock_guard<std::mutex> l(sh.temp_mu);
    sh.temp_files.push_back(path);
  }
  State sub = st;
  Io sio{io.in, f, io.err};
  try { sh.run_text(sub, text, sio); }
  catch (const ExitSignal&) {}
  catch (const BrokenPipe&) {}
  os::close_fd(f);
  return path;
}

void expand_parts(Shell& sh, State& st, const std::vector<WordPart>& parts, bool in_dquote, Fields& out, const Io& io) {
  for (const auto& part : parts) {
    switch (part.kind) {
      case WordPart::Literal: out.add(part.text, in_dquote, false); break;
      case WordPart::SingleQuoted: out.add(part.text, true, false); out.mark_exists(); break;
      case WordPart::DoubleQuoted: {
        // "$@" / "${a[@]}" with nothing in it is no field at all, not an empty one
        std::vector<std::string> list;
        bool star = false;
        if (part.inner.size() == 1 && part.inner[0].kind == WordPart::Param && list_param(sh, st, part.inner[0], io, list, star) && !star && list.empty()) break;
        out.mark_exists();
        expand_parts(sh, st, part.inner, true, out, io);
        break;
      }
      case WordPart::Tilde: {
        std::string home = part.text.empty() ? st.get("HOME") : std::string();
        if (part.text.empty() && home.empty()) home = os::home_dir();
#ifdef _WIN32
        for (auto& c : home) if (c == '\\') c = '/';
#endif
        out.add(part.text.empty() ? home : "~" + part.text, true, false);
        break;
      }
      case WordPart::Param: {
        std::vector<std::string> list;
        bool star = false;
        if (list_param(sh, st, part, io, list, star)) {
          if (star && in_dquote) {   // "$*", "${a[*]}": one field, joined by IFS's first character
            std::string ifs = st.has("IFS") ? st.get("IFS") : std::string(" ");
            out.add(join(list, ifs.empty() ? std::string() : ifs.substr(0, 1)), true, true);
            break;
          }
          // "$@", "${a[@]}": one field per element
          for (size_t i = 0; i < list.size(); ++i) {
            if (i) out.new_field();
            out.add(list[i], in_dquote, true);
            if (in_dquote) out.mark_exists();
          }
          break;
        }
        out.add(expand_param(sh, st, part, io), in_dquote, true);
        break;
      }
      case WordPart::CommandSub: out.add(sh.command_substitution(st, part.text, io), in_dquote, true); break;
      case WordPart::Arith: out.add(std::to_string(sh.arith(st, part.text, io)), in_dquote, true); break;
      case WordPart::ArrayLit: {
        std::string v(1, kArrayMark);
        for (const auto& e : array_elements(sh, st, part.text, io)) { v += kArraySep; v += e; }
        out.add(v, true, false);
        break;
      }
      case WordPart::ProcSub: out.add(process_substitution(sh, st, part.text, part.braced, io), true, false); out.mark_exists(); break;
    }
  }
}

}  // namespace

namespace {

// {a,b,c} and {1..5} / {a..e} / {01..10..2}: alternatives for the text between the braces, or false when it is
// not a brace expression (no top-level comma, not a range).
bool brace_alternatives(const std::string& inner, std::vector<std::string>& alts) {
  std::vector<std::string> parts;
  int depth = 0;
  size_t start = 0;
  for (size_t i = 0; i < inner.size(); ++i) {
    if (inner[i] == '\\') { ++i; continue; }
    if (inner[i] == '{') ++depth;
    else if (inner[i] == '}') --depth;
    else if (inner[i] == ',' && depth == 0) { parts.push_back(inner.substr(start, i - start)); start = i + 1; }
  }
  if (!parts.empty()) { parts.push_back(inner.substr(start)); alts = parts; return true; }
  // a range
  size_t dd = inner.find("..");
  if (dd == std::string::npos) return false;
  std::string a = inner.substr(0, dd), rest = inner.substr(dd + 2), b = rest, step_s;
  size_t dd2 = rest.find("..");
  if (dd2 != std::string::npos) { b = rest.substr(0, dd2); step_s = rest.substr(dd2 + 2); }
  long long x, y, step = 1;
  if (!step_s.empty() && (!parse_int(step_s, step) || step == 0)) return false;
  if (step < 0) step = -step;
  if (parse_int(a, x) && parse_int(b, y)) {
    size_t width = 0;
    auto padded = [](const std::string& s) { std::string d = s[0] == '-' ? s.substr(1) : s; return d.size() > 1 && d[0] == '0'; };
    if (padded(a) || padded(b)) width = std::max(a.size(), b.size());
    for (long long v = x; x <= y ? v <= y : v >= y; v += x <= y ? step : -step) {
      std::string s = std::to_string(v < 0 ? -v : v);
      size_t w = width && v < 0 ? width - 1 : width;
      while (s.size() < w) s = "0" + s;
      alts.push_back((v < 0 ? "-" : "") + s);
    }
    return true;
  }
  if (a.size() == 1 && b.size() == 1 && std::isalpha(static_cast<unsigned char>(a[0])) && std::isalpha(static_cast<unsigned char>(b[0]))) {
    int ca = a[0], cb = b[0];
    for (int c = ca; ca <= cb ? c <= cb : c >= cb; c += ca <= cb ? static_cast<int>(step) : -static_cast<int>(step)) alts.push_back(std::string(1, static_cast<char>(c)));
    return true;
  }
  return false;
}

// Brace expansion, on the word's unquoted literal text (bash does it before every other expansion). The word is
// flattened to atoms: one per unquoted literal character, one per other part ($x, "...", ...), so braces and
// commas may surround expansions: {$a,b}.
struct Atom { char c = 0; const WordPart* part = nullptr; };

Word atoms_to_word(const std::vector<Atom>& atoms) {
  Word w;
  for (const auto& a : atoms) {
    if (a.part) { w.parts.push_back(*a.part); continue; }
    if (w.parts.empty() || w.parts.back().kind != WordPart::Literal || w.parts.back().braced) {
      WordPart lit;
      lit.kind = WordPart::Literal;
      w.parts.push_back(lit);
    }
    w.parts.back().text += a.c;
  }
  return w;
}

void brace_expand_atoms(const std::vector<Atom>& atoms, std::vector<Word>& out, int depth) {
  if (depth < 64) {
    for (size_t i = 0; i < atoms.size(); ++i) {
      if (atoms[i].part) continue;
      if (atoms[i].c == '\\') { ++i; continue; }
      if (atoms[i].c != '{') continue;
      // the matching close, and the top-level commas
      int d = 0;
      size_t close = std::string::npos;
      std::vector<size_t> commas;
      for (size_t j = i; j < atoms.size(); ++j) {
        if (atoms[j].part) continue;
        char c = atoms[j].c;
        if (c == '\\') { ++j; continue; }
        if (c == '{') ++d;
        else if (c == '}' && --d == 0) { close = j; break; }
        else if (c == ',' && d == 1) commas.push_back(j);
      }
      if (close == std::string::npos) break;
      std::vector<std::vector<Atom>> alts;
      if (!commas.empty()) {
        size_t from = i + 1;
        commas.push_back(close);
        for (size_t c : commas) { alts.emplace_back(atoms.begin() + static_cast<long>(from), atoms.begin() + static_cast<long>(c)); from = c + 1; }
      } else {
        // a range: only plain characters inside
        std::string inner;
        bool plain = true;
        for (size_t j = i + 1; j < close; ++j) { if (atoms[j].part) { plain = false; break; } inner += atoms[j].c; }
        std::vector<std::string> seq;
        if (!plain || !brace_alternatives(inner, seq)) continue;
        for (const auto& s : seq) { std::vector<Atom> v; for (char c : s) { Atom a; a.c = c; v.push_back(a); } alts.push_back(v); }
      }
      for (const auto& alt : alts) {
        std::vector<Atom> nw(atoms.begin(), atoms.begin() + static_cast<long>(i));
        nw.insert(nw.end(), alt.begin(), alt.end());
        nw.insert(nw.end(), atoms.begin() + static_cast<long>(close) + 1, atoms.end());
        brace_expand_atoms(nw, out, depth + 1);
      }
      return;
    }
  }
  out.push_back(atoms_to_word(atoms));
}

void brace_expand(const Word& w, std::vector<Word>& out) {
  bool any = false;
  for (const auto& p : w.parts) if (p.kind == WordPart::Literal && p.text.find('{') != std::string::npos) any = true;
  if (!any) { out.push_back(w); return; }
  std::vector<Atom> atoms;
  for (const auto& p : w.parts) {
    if (p.kind != WordPart::Literal) { Atom a; a.part = &p; atoms.push_back(a); continue; }
    for (char c : p.text) { Atom a; a.c = c; atoms.push_back(a); }
  }
  brace_expand_atoms(atoms, out, 0);
}
}  // namespace

std::vector<std::string> Shell::expand_words(State& st, const std::vector<Word>& words_in, const Io& io) {
  std::vector<std::string> result;
  std::string ifs = st.has("IFS") ? st.get("IFS") : std::string(" \t\n");
  std::vector<Word> words;
  for (const auto& w : words_in) brace_expand(w, words);
  for (const auto& w : words) {
    Fields fs;
    expand_parts(*this, st, w.parts, false, fs, io);
    for (auto& f : fs.f) {
      // field splitting: unquoted characters that came from an expansion and are in IFS
      std::vector<Fields::F> pieces(1);
      bool any_content = f.exists;
      for (size_t i = 0; i < f.text.size(); ++i) {
        char c = f.text[i];
        if (!f.quoted[i] && f.from_exp[i] && ifs.find(c) != std::string::npos) {
          if (!pieces.back().text.empty() || pieces.back().exists) pieces.emplace_back();
          continue;
        }
        pieces.back().text += c;
        pieces.back().quoted += f.quoted[i];
        pieces.back().from_exp += f.from_exp[i];
        if (f.quoted[i]) pieces.back().exists = true;
      }
      // a word that was only quotes ("" or "$empty") is still one, empty, argument
      if (f.text.empty() && f.exists) { pieces.clear(); pieces.emplace_back(); pieces.back().exists = true; }
      for (auto& p : pieces) {
        if (p.text.empty() && !p.exists) continue;
        // globbing: unquoted * ? [ ; quoted glob characters are escaped so they match themselves
        bool globby = false;
        std::string pat;
        for (size_t i = 0; i < p.text.size(); ++i) {
          char c = p.text[i];
          bool special = c == '*' || c == '?' || c == '[' || c == ']' || c == '\\';
          if (p.quoted[i] && special) { pat += '\\'; pat += c; continue; }
          if (!p.quoted[i] && (c == '*' || c == '?' || c == '[')) globby = true;
#ifdef _WIN32
          if (!p.quoted[i] && c == '\\') { pat += '/'; continue; }   // a Windows path separator, not an escape
#endif
          pat += c;
        }
        if (globby && !st.opt_noglob) {
          auto hits = glob_expand(st.cwd, pat);
          if (!hits.empty()) { for (auto& h : hits) result.push_back(h); continue; }
        }
        result.push_back(p.text);
      }
      (void)any_content;
    }
  }
  return result;
}

std::string Shell::expand_word_nosplit(State& st, const Word& w, const Io& io) {
  Fields fs;
  expand_parts(*this, st, w.parts, false, fs, io);
  std::string s;
  for (size_t i = 0; i < fs.f.size(); ++i) s += (i ? " " : "") + fs.f[i].text;
  return s;
}

// ---------------------------------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------------------------------

namespace {

// Streams opened for one command's redirections, closed when it ends.
struct Opened {
  std::vector<Fd> fds;
  std::vector<std::thread> feeders;   // here-document writers
  ~Opened() {
    for (auto& t : feeders) t.join();
    for (Fd f : fds) os::close_fd(f);
  }
};

int depth_condition = 0;   // per thread below
thread_local int t_condition = 0;   // > 0 while running an if/while condition or the left of && / ||: no errexit

bool apply_redirects(Shell& sh, State& st, const std::vector<Redirect>& rs, Io& io, Opened& opened, const Io& outer) {
  for (const auto& r : rs) {
    Fd* slot = r.fd == 0 ? &io.in : r.fd == 1 ? &io.out : r.fd == 2 ? &io.err : nullptr;
    if (r.kind == Redirect::HereDoc || r.kind == Redirect::HereString) {
      std::string body = r.heredoc;
      if (r.kind == Redirect::HereString) body = sh.expand_word_nosplit(st, r.target, outer) + "\n";
      else if (r.heredoc_expand) body = sh.expand_word_nosplit(st, parse_word_text(body), outer);
      Fd rd, wr;
      if (!os::make_pipe(rd, wr)) { sh.error(outer, "cannot make a pipe"); return false; }
      opened.fds.push_back(rd);
      opened.feeders.emplace_back([wr, body]() { os::write_all(wr, body.data(), body.size()); os::close_fd(wr); });
      if (slot) *slot = rd;
      continue;
    }
    std::string target = sh.expand_word_nosplit(st, r.target, outer);
    if (r.kind == Redirect::DupOut || r.kind == Redirect::DupIn) {
      if (target == "-") { if (slot) *slot = kNoFd; continue; }
      long long n = -1;
      parse_int(target, n);
      Fd src = n == 0 ? io.in : n == 1 ? io.out : n == 2 ? io.err : kNoFd;
      if (n < 0 || n > 2) { sh.error(outer, target + ": bad file descriptor"); return false; }
      if (slot) *slot = src;
      continue;
    }
    std::string path = host_path(st.cwd, target);
    std::string err;
    os::Open how = r.kind == Redirect::In ? os::Open::Read
                 : (r.kind == Redirect::Append || r.kind == Redirect::AppendOutErr) ? os::Open::Append
                 : r.kind == Redirect::ReadWrite ? os::Open::ReadWrite : os::Open::Write;
    if (how != os::Open::Read && os::is_dir(path)) { sh.error(outer, target + ": Is a directory"); return false; }
    Fd f = os::open_file(path, how, &err);
    if (f == kNoFd) { sh.error(outer, target + ": " + err); return false; }
    opened.fds.push_back(f);
    if (r.kind == Redirect::OutErr || r.kind == Redirect::AppendOutErr) { io.out = f; io.err = f; continue; }
    if (slot) *slot = f;
  }
  return true;
}

void xtrace(Shell& sh, State& st, const Io& io, const std::vector<std::string>& argv) {
  if (!st.opt_xtrace) return;
  std::string line = "+";
  for (const auto& a : argv) {
    bool q = a.empty() || a.find_first_of(" \t\n'\"$`\\") != std::string::npos;
    line += " " + (q ? "'" + a + "'" : a);
  }
  std::lock_guard<std::mutex> l(sh.io_mu);
  io.ewrite(line + "\n");
}

}  // namespace

int run_external(Shell& sh, State& st, const std::vector<std::string>& argv, const Io& io,
                 const std::vector<std::pair<std::string, std::string>>& env_extra);
int run_builtin(Shell& sh, State& st, const std::vector<std::string>& argv, const Io& io, bool* found);

int Shell::run_text(State& st, const std::string& text, const Io& io) {
  std::shared_ptr<Node> tree;
  try {
    tree = parse(text);
  } catch (const ParseError& e) {
    error(io, "line " + std::to_string(e.line) + ": syntax error: " + e.message);
    st.last_status = 2;
    return 2;
  }
  if (st.opt_noexec) return 0;   // parsed, which is all -n asks for
  return run(st, *tree, io);
}

namespace {
int run_function(Shell& sh, State& st, const Node& body, const std::vector<std::string>& argv, const Io& io) {
  std::vector<std::string> saved = st.params;
  st.params.assign(argv.begin() + 1, argv.end());
  st.locals.emplace_back();
  int status = 0;
  try {
    status = sh.run(st, body, io);
  } catch (const ReturnSignal& r) {
    status = r.status;
  } catch (...) {
    for (auto& [k, v] : st.locals.back()) { st.arrays.erase(k); st.assoc.erase(k); if (v.value == "\x01unset") st.vars.erase(k); else st.vars[k] = v; }
    st.locals.pop_back();
    st.params = saved;
    throw;
  }
  for (auto& [k, v] : st.locals.back()) { st.arrays.erase(k); st.assoc.erase(k); if (v.value == "\x01unset") st.vars.erase(k); else st.vars[k] = v; }
  st.locals.pop_back();
  st.params = saved;
  return status;
}
}  // namespace

std::string posix_regex_to_ecma(const std::string& re, bool extended);   // sh_grep.cpp

namespace {

// [[ ... ]]: no splitting or globbing of words; == != match patterns, =~ a regex (BASH_REMATCH); && || ! ( ).
class CondEval {
 public:
  CondEval(Shell& sh, State& st, const Io& io, const Node& n) : sh_(sh), st_(st), io_(io), n_(n) {}
  int run() {
    if (n_.words.empty()) return 2;
    bool v = or_(true);
    if (i_ < n_.words.size()) throw std::runtime_error("[[: syntax error near '" + text(i_) + "'");
    return v ? 0 : 1;
  }

 private:
  Shell& sh_; State& st_; const Io& io_; const Node& n_; size_t i_ = 0;
  bool is_op(size_t k) const { return k < n_.words.size() && n_.ops[k] == "op"; }
  std::string text(size_t k) const {   // a word's source text when it is plain (operators, -f, ==)
    if (k >= n_.words.size()) return {};
    const auto& w = n_.words[k];
    if (w.parts.size() != 1 || w.parts[0].kind != WordPart::Literal) return {};
    return w.parts[0].text;
  }
  bool op_is(size_t k, const char* s) const { return is_op(k) && text(k) == s; }
  bool word_is(size_t k, const char* s) const { return k < n_.words.size() && !is_op(k) && text(k) == s; }
  std::string value(size_t k) { return sh_.expand_word_nosplit(st_, n_.words[k], io_); }
  std::string pattern(size_t k) {   // quoted parts match themselves
    Fields fs;
    expand_parts(sh_, st_, n_.words[k].parts, false, fs, io_);
    std::string pat;
    for (const auto& f : fs.f)
      for (size_t c = 0; c < f.text.size(); ++c) {
        if (f.quoted[c] && std::strchr("*?[]\\", f.text[c])) pat += '\\';
        pat += f.text[c];
      }
    return pat;
  }
  bool or_(bool live) {
    bool v = and_(live);
    while (op_is(i_, "||")) { ++i_; bool r = and_(live && !v); v = v || r; }
    return v;
  }
  bool and_(bool live) {
    bool v = not_(live);
    while (op_is(i_, "&&")) { ++i_; bool r = not_(live && v); v = v && r; }
    return v;
  }
  bool not_(bool live) {
    if (word_is(i_, "!") && i_ + 1 < n_.words.size()) { ++i_; return !not_(live); }
    return primary(live);
  }
  bool primary(bool live) {
    if (i_ >= n_.words.size()) throw std::runtime_error("[[: unexpected end of expression");
    if (op_is(i_, "(")) {
      ++i_;
      bool v = or_(live);
      if (!op_is(i_, ")")) throw std::runtime_error("[[: expected ')'");
      ++i_;
      return v;
    }
    static const char* unary = "efdrwxsLhzna-pSbcgukOGNtv";
    std::string t = text(i_);
    if (!is_op(i_) && t.size() == 2 && t[0] == '-' && std::strchr(unary, t[1]) && i_ + 1 < n_.words.size() && !is_op(i_ + 1) && !is_binary(i_ + 1)) {
      ++i_;
      size_t arg = i_++;
      if (!live) return false;
      std::string v = value(arg);
      if (t == "-z") return v.empty();
      if (t == "-n") return !v.empty();
      if (t == "-v") {
        ParamRef r = split_param(v);
        if (r.has_sub) { std::string x; return st_.get_element(r.name, subscript_key(sh_, st_, r.name, r.sub, io_), x); }
        return st_.has(v) || st_.arrays.count(v) || st_.assoc.count(v);
      }
      if (t == "-a") t = "-e";
      return test({"test", t, v}) == 0;
    }
    size_t a = i_++;
    if (is_binary(i_)) {
      std::string op = text(i_++);
      if (i_ >= n_.words.size()) throw std::runtime_error("[[: expected an operand after " + op);
      if (op == "=~") {
        std::string re = regex_text();
        if (!live) return false;
        std::string lhs = value(a);
        std::regex rx;
        try { rx = std::regex(posix_regex_to_ecma(re, true), std::regex::ECMAScript); }
        catch (const std::regex_error&) { return false; }   // bash: status 2; treated as no match
        std::smatch m;
        bool hit = std::regex_search(lhs, m, rx);
        std::vector<std::string> groups;
        if (hit) for (size_t g = 0; g < m.size(); ++g) groups.push_back(m[g].matched ? m[g].str() : std::string());
        auto& br = st_.arrays["BASH_REMATCH"];
        br.clear();
        for (size_t g = 0; g < groups.size(); ++g) br[static_cast<long long>(g)] = groups[g];
        st_.vars["BASH_REMATCH"].value = groups.empty() ? std::string() : groups[0];
        return hit;
      }
      size_t b = i_++;
      if (!live) return false;
      if (op == "==" || op == "=") return glob_match(pattern(b), value(a));
      if (op == "!=") return !glob_match(pattern(b), value(a));
      if (op == "<") return value(a) < value(b);
      if (op == ">") return value(a) > value(b);
      if (op == "-eq" || op == "-ne" || op == "-lt" || op == "-le" || op == "-gt" || op == "-ge") {
        long long x = sh_.arith(st_, value(a), io_), y = sh_.arith(st_, value(b), io_);
        if (op == "-eq") return x == y; if (op == "-ne") return x != y; if (op == "-lt") return x < y;
        if (op == "-le") return x <= y; if (op == "-gt") return x > y; return x >= y;
      }
      return test({"test", value(a), op, value(b)}) == 0;   // -nt -ot -ef
    }
    if (!live) return false;
    return !value(a).empty();
  }
  bool is_binary(size_t k) const {
    if (k >= n_.words.size()) return false;
    std::string t = text(k);
    if (is_op(k)) return t == "<" || t == ">";
    for (const char* b : {"==", "=", "!=", "=~", "-eq", "-ne", "-lt", "-le", "-gt", "-ge", "-nt", "-ot", "-ef"}) if (t == b) return true;
    return false;
  }
  // The regex after =~: its words and operator tokens up to a top-level && / || or the closing ), quoted text
  // matching itself.
  std::string regex_text() {
    std::string re;
    int depth = 0;
    while (i_ < n_.words.size()) {
      if (is_op(i_)) {
        std::string t = text(i_);
        if ((t == "&&" || t == "||") && depth == 0) break;
        if (t == ")" && depth == 0) break;
        if (t == "(") ++depth;
        if (t == ")") --depth;
        re += t;
        ++i_;
        continue;
      }
      Fields fs;
      expand_parts(sh_, st_, n_.words[i_].parts, false, fs, io_);
      for (const auto& f : fs.f)
        for (size_t c = 0; c < f.text.size(); ++c) {
          if (f.quoted[c] && std::strchr("\\^$.|?*+()[]{}", f.text[c])) re += '\\';
          re += f.text[c];
        }
      ++i_;
    }
    return re;
  }
  int test(const std::vector<std::string>& argv) {
    auto& u = utilities();
    auto it = u.find("test");
    return it == u.end() ? 2 : it->second(sh_, st_, argv, io_);
  }
};

// NAME=value, NAME+=value, NAME[sub]=value, NAME+=(...) for a simple command's prefix.
void assign(Shell& sh, State& st, std::string name, const std::string& value, const Io& io) {
  bool append = !name.empty() && name.back() == '+';
  if (append) name.pop_back();
  size_t lb = name.find('[');
  if (lb != std::string::npos) {
    std::string base = name.substr(0, lb);
    std::string key = subscript_key(sh, st, base, name.substr(lb + 1, name.size() - lb - 2), io);
    std::string old;
    if (append) st.get_element(base, key, old);
    st.set_element(base, key, old + value);
    return;
  }
  if (!append) { st.set(name, value); return; }
  if (!value.empty() && value[0] == kArrayMark) {   // a+=(x y)
    std::string merged(1, kArrayMark);
    if (st.assoc.count(name)) {
      for (const auto& [k, v] : st.assoc[name]) { merged += kArraySep; merged += "[" + k + "]=" + v; }
    } else {
      for (const auto& e : st.elements(name)) { merged += kArraySep; merged += e; }
    }
    merged += value.substr(1);
    st.set(name, merged);
    return;
  }
  st.set(name, st.get(name) + value);
}

}  // namespace

int Shell::run(State& st, const Node& n, const Io& outer) {
  Io io = outer;
  Opened opened;
  int status = 0;
  try {
    switch (n.kind) {
      case Node::List: {
        for (size_t i = 0; i < n.kids.size(); ++i) {
          const Node& kid = *n.kids[i];
          if (n.ops[i] == "&") {
            // Background: its own copy of the shell, on its own thread; the script does not wait for it.
            auto copy = std::make_shared<State>(st);
            auto tree = n.kids[i];
            Io bio{os::dup_fd(io.in), os::dup_fd(io.out), os::dup_fd(io.err)};
            std::thread([this, copy, tree, bio]() {
              try { run(*copy, *tree, bio); } catch (...) {}
              os::close_fd(bio.in); os::close_fd(bio.out); os::close_fd(bio.err);
            }).detach();
            st.last_bg_pid = os::self_pid() + static_cast<int>(i) + 1;
            status = 0;
            continue;
          }
          status = run(st, kid, io);
          st.last_status = status;
          if (status != 0 && st.opt_errexit && t_condition == 0 && kid.kind != Node::AndOr && kid.kind != Node::Not) throw ExitSignal{status};
        }
        break;
      }
      case Node::AndOr: {
        ++t_condition;
        try { status = run(st, *n.kids[0], io); } catch (...) { --t_condition; throw; }
        --t_condition;
        st.last_status = status;
        for (size_t i = 1; i < n.kids.size(); ++i) {
          bool go = n.ops[i - 1] == "&&" ? status == 0 : status != 0;
          if (!go) continue;
          bool last = i + 1 == n.kids.size();
          if (!last) ++t_condition;
          try { status = run(st, *n.kids[i], io); } catch (...) { if (!last) --t_condition; throw; }
          if (!last) --t_condition;
          st.last_status = status;
        }
        if (status != 0 && st.opt_errexit && t_condition == 0) {
          // POSIX: errexit applies to an and-or list only when its last command ran and failed
          bool last_ran = true;
          (void)last_ran;
        }
        break;
      }
      case Node::Not: {
        ++t_condition;
        try { status = run(st, *n.kids[0], io); } catch (...) { --t_condition; throw; }
        --t_condition;
        status = status == 0 ? 1 : 0;
        break;
      }
      case Node::Pipeline: {
        size_t k = n.kids.size();
        std::vector<Fd> rd(k, kNoFd), wr(k, kNoFd);
        for (size_t i = 0; i + 1 < k; ++i)
          if (!os::make_pipe(rd[i], wr[i])) { error(io, "cannot make a pipe"); return 1; }
        std::vector<int> statuses(k, 0);
        std::vector<std::thread> threads;
        for (size_t i = 0; i < k; ++i) {
          Io sio{i == 0 ? io.in : rd[i - 1], i + 1 == k ? io.out : wr[i], io.err};
          if (i + 1 < k && i < n.ops.size() && n.ops[i] == "|&") sio.err = wr[i];
          threads.emplace_back([this, &st, &n, &statuses, i, k, sio, &rd, &wr]() {
            State copy = st;
            int s = 0;
            try { s = run(copy, *n.kids[i], sio); }
            catch (const ExitSignal& e) { s = e.status; }
            catch (const BrokenPipe&) { s = 141; }
            catch (const ReturnSignal& r) { s = r.status; }
            catch (const std::exception& e) { error(sio, e.what()); s = 1; }
            statuses[i] = s;
            // this stage is done with its ends: the next sees end of input, the previous a closed reader
            if (i + 1 < k) os::close_fd(wr[i]);
            if (i > 0) os::close_fd(rd[i - 1]);
          });
        }
        for (auto& t : threads) t.join();
        status = statuses.back();
        if (st.opt_pipefail) for (size_t i = k; i-- > 0;) if (statuses[i] != 0) { status = statuses[i]; break; }
        break;
      }
      case Node::Subshell: {
        if (!apply_redirects(*this, st, n.redirects, io, opened, outer)) return 1;
        State copy = st;
        try { status = run(copy, *n.kids[0], io); }
        catch (const ExitSignal& e) { status = e.status; }
        break;
      }
      case Node::Group: {
        if (!apply_redirects(*this, st, n.redirects, io, opened, outer)) return 1;
        status = run(st, *n.kids[0], io);
        break;
      }
      case Node::If: {
        if (!apply_redirects(*this, st, n.redirects, io, opened, outer)) return 1;
        status = 0;
        size_t i = 0;
        bool taken = false;
        for (; i + 1 < n.kids.size(); i += 2) {
          ++t_condition;
          int c;
          try { c = run(st, *n.kids[i], io); } catch (...) { --t_condition; throw; }
          --t_condition;
          if (c == 0) { status = run(st, *n.kids[i + 1], io); taken = true; break; }
        }
        if (!taken && n.kids.size() % 2 == 1) status = run(st, *n.kids.back(), io);
        break;
      }
      case Node::While:
      case Node::Until: {
        if (!apply_redirects(*this, st, n.redirects, io, opened, outer)) return 1;
        status = 0;
        for (;;) {
          ++t_condition;
          int c;
          try { c = run(st, *n.kids[0], io); } catch (...) { --t_condition; throw; }
          --t_condition;
          if ((c == 0) != (n.kind == Node::While)) break;
          try { status = run(st, *n.kids[1], io); }
          catch (LoopSignal& l) {
            if (l.levels > 1) { --l.levels; throw; }
            if (l.is_break) break;
          }
        }
        break;
      }
      case Node::ArithCmd: {
        if (!apply_redirects(*this, st, n.redirects, io, opened, outer)) return 1;
        status = arith(st, n.name, io) != 0 ? 0 : 1;
        break;
      }
      case Node::Cond: {
        if (!apply_redirects(*this, st, n.redirects, io, opened, outer)) return 1;
        status = CondEval(*this, st, io, n).run();
        break;
      }
      case Node::For: {
        if (!apply_redirects(*this, st, n.redirects, io, opened, outer)) return 1;
        if (n.name == "((") {
          status = 0;
          auto expr = [&](const std::string& e, long long dflt) {
            bool blank = e.find_first_not_of(" \t\n") == std::string::npos;
            return blank ? dflt : arith(st, e, io);
          };
          for (expr(n.ops[0], 0); expr(n.ops[1], 1) != 0; expr(n.ops[2], 0)) {
            try { status = run(st, *n.kids[0], io); }
            catch (LoopSignal& l) {
              if (l.levels > 1) { --l.levels; throw; }
              if (l.is_break) break;
            }
          }
          break;
        }
        std::vector<std::string> items = n.has_in ? expand_words(st, n.items, io) : st.params;
        status = 0;
        for (const auto& it : items) {
          st.set(n.name, it);
          try { status = run(st, *n.kids[0], io); }
          catch (LoopSignal& l) {
            if (l.levels > 1) { --l.levels; throw; }
            if (l.is_break) break;
          }
        }
        break;
      }
      case Node::Case: {
        if (!apply_redirects(*this, st, n.redirects, io, opened, outer)) return 1;
        std::string subject = expand_word_nosplit(st, n.words[0], io);
        status = 0;
        for (size_t i = 0; i < n.case_patterns.size(); ++i) {
          bool hit = false;
          for (const auto& pw : n.case_patterns[i]) {
            // the pattern: expanded, with quoted parts matching literally
            Fields fs;
            expand_parts(*this, st, pw.parts, false, fs, io);
            std::string pat;
            for (const auto& f : fs.f)
              for (size_t c = 0; c < f.text.size(); ++c) {
                if (f.quoted[c] && std::strchr("*?[]\\", f.text[c])) pat += '\\';
                pat += f.text[c];
              }
            if (glob_match(pat, subject)) { hit = true; break; }
          }
          if (hit) { status = run(st, *n.kids[i], io); break; }
        }
        break;
      }
      case Node::FuncDef: {
        st.functions[n.name] = n.kids[0];
        status = 0;
        break;
      }
      case Node::Simple: {
        // assignments' values and the words are expanded first; redirections then apply
        std::vector<std::pair<std::string, std::string>> assigns;
        for (const auto& [name, w] : n.assigns) {
          std::string v = expand_word_nosplit(st, w, io);
          if (!w.parts.empty() && w.parts[0].kind == WordPart::Tilde) {}
          assigns.push_back({name, v});
        }
        std::vector<std::string> argv = expand_words(st, n.words, io);
        if (!apply_redirects(*this, st, n.redirects, io, opened, outer)) { status = 1; st.last_status = 1; return 1; }
        if (argv.empty()) {
          for (auto& [k, v] : assigns) assign(*this, st, k, v, io);
          status = st.last_status;   // a command substitution's status, or 0
          if (n.words.empty()) status = assigns.empty() ? 0 : st.last_status;
          break;
        }
        xtrace(*this, st, io, argv);
        // a function
        auto fit = st.functions.find(argv[0]);
        if (fit != st.functions.end()) {
          std::map<std::string, Var> saved;
          for (auto& [k, v] : assigns) { saved[k] = st.vars.count(k) ? st.vars[k] : Var{"\x01unset"}; st.vars[k].value = v; st.vars[k].exported = true; }
          auto body = fit->second;
          status = run_function(*this, st, *body, argv, io);
          for (auto& [k, v] : saved) { if (v.value == "\x01unset") st.vars.erase(k); else st.vars[k] = v; }
          break;
        }
        bool found = false;
        // builtins that change the shell, then the utilities, with the prefix assignments in effect for them
        std::map<std::string, Var> saved;
        for (auto& [k, v] : assigns) { saved[k] = st.vars.count(k) ? st.vars[k] : Var{"\x01unset"}; st.vars[k].value = v; st.vars[k].exported = true; }
        status = run_builtin(*this, st, argv, io, &found);
        if (!found) {
          auto& utils = utilities();
          auto uit = utils.find(argv[0]);
          if (uit != utils.end()) { status = uit->second(*this, st, argv, io); found = true; }
        }
        if (!found) status = run_external(*this, st, argv, io, {});
        for (auto& [k, v] : saved) { if (v.value == "\x01unset") st.vars.erase(k); else st.vars[k] = v; }
        break;
      }
    }
  } catch (const FatalExpansion& e) {
    error(io, e.what());
    throw ExitSignal{e.code};
  } catch (const std::runtime_error& e) {
    error(io, e.what());
    status = 1;
    if (!st.opt_errexit) { st.last_status = status; return status; }
    throw ExitSignal{status};
  }
  st.last_status = status;
  return status;
}

// ---------------------------------------------------------------------------------------------------
// Host programs
// ---------------------------------------------------------------------------------------------------

int run_external(Shell& sh, State& st, const std::vector<std::string>& argv_in, const Io& io,
                 const std::vector<std::pair<std::string, std::string>>&) {
  std::vector<std::string> argv = argv_in;
  std::string path = os::find_program(argv[0], st.get("PATH"), st.cwd);
  if (path.empty()) {
    sh.error(io, argv[0] + ": command not found");
    return 127;
  }
  // A script: its #! line says how to run it; a shell script (or one with no #!) runs in this shell.
  std::string head;
  {
    Fd f = os::open_file(path, os::Open::Read, nullptr);
    if (f != kNoFd) { char buf[256]; long n = os::read_fd(f, buf, sizeof buf); if (n > 0) head.assign(buf, static_cast<size_t>(n)); os::close_fd(f); }
  }
  std::string lowpath = path;
  for (auto& c : lowpath) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  bool native = false;
#ifdef _WIN32
  for (const char* e : {".exe", ".com", ".cmd", ".bat", ".ps1"}) if (lowpath.size() > 4 && lowpath.compare(lowpath.size() - 4, 4, e) == 0) native = true;
#else
  native = head.rfind("#!", 0) != 0 && head.rfind("\x7f" "ELF", 0) == 0;
  if (!native && head.rfind("#!", 0) != 0 && os::is_executable(path) && head.find('\0') != std::string::npos) native = true;
#endif
  if (!native) {
    std::string interp, interp_arg;
    if (head.rfind("#!", 0) == 0) {
      std::string line = head.substr(2, head.find('\n') == std::string::npos ? std::string::npos : head.find('\n') - 2);
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      size_t a = line.find_first_not_of(' ');
      line = a == std::string::npos ? "" : line.substr(a);
      size_t sp = line.find(' ');
      interp = line.substr(0, sp);
      interp_arg = sp == std::string::npos ? "" : line.substr(sp + 1);
      std::string base = interp.substr(interp.find_last_of('/') + 1);
      if (base == "env") { size_t s2 = interp_arg.find(' '); interp = interp_arg.substr(0, s2); interp_arg = s2 == std::string::npos ? "" : interp_arg.substr(s2 + 1); base = interp; }
      if (base == "sh" || base == "bash" || base == "dash" || base == "zsh" || base == "ksh") interp.clear();
      else interp = base;
    }
    if (interp.empty()) {
      // run it here, as a subshell: $0 is the script, $1... its arguments
      std::string text;
      os::read_whole(path, text, nullptr);
      State sub = st;
      sub.arg0 = argv[0];
      sub.params.assign(argv.begin() + 1, argv.end());
      try { return sh.run_text(sub, text, io); } catch (const ExitSignal& e) { return e.status; }
    }
    std::vector<std::string> a2 = {interp};
    if (!interp_arg.empty()) a2.push_back(interp_arg);
    a2.push_back(path);
    a2.insert(a2.end(), argv.begin() + 1, argv.end());
    std::string ip = os::find_program(interp, st.get("PATH"), st.cwd);
    if (ip.empty()) { sh.error(io, argv[0] + ": " + interp + ": not found (from its #! line)"); return 127; }
    a2[0] = ip;
    argv = a2;
    path = ip;
  } else {
    argv[0] = path;
  }
  os::Proc p;
  std::string why;
  if (!os::spawn(argv, st.cwd, st.env_list(), io.in, io.out, io.err, p, &why)) {
    sh.error(io, argv_in[0] + ": " + why);
    return 126;
  }
  return os::wait(p);
}

// ---------------------------------------------------------------------------------------------------
// Builtins that act on the shell itself
// ---------------------------------------------------------------------------------------------------

namespace {

bool valid_name(const std::string& s) {
  if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
  for (char c : s) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
  return true;
}

int set_options(State& st, const std::vector<std::string>& argv, size_t start, const Io& io, bool* positional_set) {
  size_t i = start;
  for (; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (a == "--") { ++i; break; }
    if (a.size() < 2 || (a[0] != '-' && a[0] != '+')) break;
    bool on = a[0] == '-';
    if (a.substr(1) == "o") {
      if (i + 1 >= argv.size()) {
        io.write(std::string("errexit\t") + (st.opt_errexit ? "on" : "off") + "\nnounset\t" + (st.opt_nounset ? "on" : "off") +
                 "\npipefail\t" + (st.opt_pipefail ? "on" : "off") + "\nxtrace\t" + (st.opt_xtrace ? "on" : "off") + "\n");
        continue;
      }
      std::string o = argv[++i];
      if (o == "errexit") st.opt_errexit = on; else if (o == "nounset") st.opt_nounset = on; else if (o == "xtrace") st.opt_xtrace = on;
      else if (o == "pipefail") st.opt_pipefail = on; else if (o == "noglob") st.opt_noglob = on;
      // others (posix, noclobber, emacs, vi, ...) are accepted and have no effect here
      continue;
    }
    for (size_t k = 1; k < a.size(); ++k) {
      char c = a[k];
      if (c == 'e') st.opt_errexit = on; else if (c == 'u') st.opt_nounset = on; else if (c == 'x') st.opt_xtrace = on;
      else if (c == 'f') st.opt_noglob = on;
      else if (c == 'o' && i + 1 < argv.size()) {   // -euo pipefail: o takes the next word
        std::string o = argv[++i];
        if (o == "errexit") st.opt_errexit = on; else if (o == "nounset") st.opt_nounset = on; else if (o == "xtrace") st.opt_xtrace = on;
        else if (o == "pipefail") st.opt_pipefail = on; else if (o == "noglob") st.opt_noglob = on;
      }
      // -v, -h, -m, -C, -a, -b, -n: accepted
    }
  }
  if (i < argv.size() || (start < argv.size() && i > start && argv[i - 1] == "--")) {
    st.params.assign(argv.begin() + static_cast<long>(i), argv.end());
    if (positional_set) *positional_set = true;
  }
  return 0;
}

}  // namespace

int shell_main_like(Shell& sh, State& st, const std::vector<std::string>& argv, const Io& io);   // main.cpp

int run_builtin(Shell& sh, State& st, const std::vector<std::string>& argv, const Io& io, bool* found) {
  *found = true;
  const std::string& c = argv[0];
  if (c == "cd") {
    std::string dest = argv.size() > 1 ? argv[1] : st.get("HOME");
    if (argv.size() > 1 && argv[1] == "-") { dest = st.get("OLDPWD"); }
    if (dest.empty()) dest = os::home_dir();
    std::string target = host_path(st.cwd, dest);
    if (!os::is_dir(target)) { sh.error(io, "cd: " + dest + ": No such file or directory"); return 1; }
    st.set("OLDPWD", st.get("PWD"));
    st.cwd = target;
    if (argv.size() > 1 && argv[1] == "-") io.write(st.get("PWD") + "\n");
    return 0;
  }
  if (c == "export" || c == "readonly") {
    bool ro = c == "readonly";
    if (argv.size() == 1 || (argv.size() == 2 && argv[1] == "-p")) {
      for (const auto& [k, v] : st.vars) if (ro ? v.readonly : v.exported) io.write((ro ? "readonly " : "export ") + k + "=\"" + v.value + "\"\n");
      return 0;
    }
    int status = 0;
    for (size_t i = 1; i < argv.size(); ++i) {
      if (argv[i] == "-p" || argv[i] == "-n" || argv[i] == "-f") continue;
      std::string a = argv[i];
      size_t eq = a.find('=');
      std::string name = a.substr(0, eq);
      if (!valid_name(name)) { sh.error(io, c + ": '" + a + "': not a valid identifier"); status = 1; continue; }
      if (eq != std::string::npos) st.set(name, a.substr(eq + 1));
      auto& v = st.vars[name];
      if (ro) v.readonly = true; else v.exported = true;
    }
    return status;
  }
  if (c == "unset") {
    for (size_t i = 1; i < argv.size(); ++i) {
      if (argv[i] == "-v") continue;
      if (argv[i] == "-f") { if (i + 1 < argv.size()) st.functions.erase(argv[++i]); continue; }
      ParamRef r = split_param(argv[i]);
      if (r.has_sub) {   // unset 'a[i]' / 'm[key]'
        st.unset_element(r.name, subscript_key(sh, st, r.name, r.sub, io));
        continue;
      }
      auto it = st.vars.find(argv[i]);
      if (it != st.vars.end() && it->second.readonly) { sh.error(io, "unset: " + argv[i] + ": readonly variable"); return 1; }
      st.vars.erase(argv[i]);
      st.arrays.erase(argv[i]);
      st.assoc.erase(argv[i]);
      st.functions.erase(argv[i]);
    }
    return 0;
  }
  if (c == "local" || c == "declare" || c == "typeset") {
    bool is_assoc = false, is_array = false, exported = false, ro = false, print = false;
    std::vector<std::string> names;
    for (size_t i = 1; i < argv.size(); ++i) {
      if (argv[i].size() > 1 && (argv[i][0] == '-' || argv[i][0] == '+')) {
        if (argv[i][0] == '-') for (char f : argv[i].substr(1)) { if (f == 'A') is_assoc = true; if (f == 'a') is_array = true; if (f == 'x') exported = true; if (f == 'r') ro = true; if (f == 'p') print = true; }
        continue;
      }
      names.push_back(argv[i]);
    }
    if (print) {
      int status = 0;
      for (const auto& nm : names) {
        if (st.assoc.count(nm)) { std::string s = "declare -A " + nm + "=("; for (const auto& [k, v] : st.assoc[nm]) s += "[" + k + "]=\"" + v + "\" "; io.write(s + ")\n"); }
        else if (st.arrays.count(nm)) { std::string s = "declare -a " + nm + "=("; bool first = true; for (const auto& [k, v] : st.arrays[nm]) { s += (first ? "" : " ") + std::string("[") + std::to_string(k) + "]=\"" + v + "\""; first = false; } io.write(s + ")\n"); }
        else if (st.vars.count(nm)) io.write("declare -- " + nm + "=\"" + st.vars[nm].value + "\"\n");
        else { sh.error(io, c + ": " + nm + ": not found"); status = 1; }
      }
      return status;
    }
    for (const auto& a : names) {
      size_t eq = a.find('=');
      std::string name = a.substr(0, eq);
      bool append = !name.empty() && name.back() == '+';
      if (append) name.pop_back();
      if (!st.locals.empty() && !st.locals.back().count(name)) {
        st.locals.back()[name] = st.vars.count(name) ? st.vars[name] : Var{"\x01unset"};
        st.arrays.erase(name);
        st.assoc.erase(name);
      }
      if (is_assoc && !st.assoc.count(name)) { st.assoc[name]; st.arrays.erase(name); }
      if (is_array && !st.arrays.count(name)) { auto& a = st.arrays[name]; if (st.vars.count(name) && !st.vars[name].value.empty()) a[0] = st.vars[name].value; }
      if (eq != std::string::npos) assign(sh, st, append ? name + "+" : name, a.substr(eq + 1), io);
      else if (!st.vars.count(name)) st.set(name, "");
      if (exported) st.vars[name].exported = true;
      if (ro) st.vars[name].readonly = true;
    }
    return 0;
  }
  if (c == "let") {
    long long v = 0;
    for (size_t i = 1; i < argv.size(); ++i) v = sh.arith(st, argv[i], io);
    return v != 0 ? 0 : 1;
  }
  if (c == "mapfile" || c == "readarray") {
    bool trim = false;
    std::string name = "MAPFILE";
    for (size_t i = 1; i < argv.size(); ++i) {
      if (argv[i] == "-t") trim = true;
      else if ((argv[i] == "-d" || argv[i] == "-n" || argv[i] == "-O" || argv[i] == "-s" || argv[i] == "-u" || argv[i] == "-C" || argv[i] == "-c") && i + 1 < argv.size()) ++i;
      else name = argv[i];
    }
    std::string all = io.read_all();
    std::vector<std::string> lines;
    size_t s = 0;
    while (s < all.size()) {
      size_t e = all.find('\n', s);
      if (e == std::string::npos) { lines.push_back(all.substr(s)); break; }
      lines.push_back(all.substr(s, trim ? e - s : e - s + 1));
      s = e + 1;
    }
    if (trim) for (auto& l : lines) if (!l.empty() && l.back() == '\r') l.pop_back();
    std::string v(1, kArrayMark);
    for (const auto& l : lines) { v += kArraySep; v += l; }
    st.assoc.erase(name);
    st.set(name, v);
    return 0;
  }
  if (c == "set") {
    if (argv.size() == 1) { for (const auto& [k, v] : st.vars) io.write(k + "='" + v.value + "'\n"); return 0; }
    return set_options(st, argv, 1, io, nullptr);
  }
  if (c == "shift") {
    long long n = 1;
    if (argv.size() > 1 && !parse_int(argv[1], n)) { sh.error(io, "shift: " + argv[1] + ": numeric argument required"); return 1; }
    if (n < 0 || static_cast<size_t>(n) > st.params.size()) return 1;
    st.params.erase(st.params.begin(), st.params.begin() + n);
    return 0;
  }
  if (c == "exit") {
    long long n = st.last_status;
    if (argv.size() > 1 && !parse_int(argv[1], n)) { sh.error(io, "exit: " + argv[1] + ": numeric argument required"); n = 2; }
    throw ExitSignal{static_cast<int>(n & 0xff)};
  }
  if (c == "return") {
    long long n = st.last_status;
    if (argv.size() > 1) parse_int(argv[1], n);
    throw ReturnSignal{static_cast<int>(n & 0xff)};
  }
  if (c == "break" || c == "continue") {
    long long n = 1;
    if (argv.size() > 1) parse_int(argv[1], n);
    throw LoopSignal{c == "break", static_cast<int>(n < 1 ? 1 : n)};
  }
  if (c == "eval") {
    std::vector<std::string> rest(argv.begin() + 1, argv.end());
    return sh.run_text(st, join(rest, " "), io);
  }
  if (c == "." || c == "source") {
    if (argv.size() < 2) { sh.error(io, c + ": filename argument required"); return 2; }
    std::string p = host_path(st.cwd, argv[1]);
    if (!os::is_file(p)) { std::string found_p = os::find_program(argv[1], st.get("PATH"), st.cwd); if (!found_p.empty()) p = found_p; }
    std::string text, err;
    if (!os::read_whole(p, text, &err)) { sh.error(io, argv[1] + ": " + err); return 1; }
    std::vector<std::string> saved = st.params;
    if (argv.size() > 2) st.params.assign(argv.begin() + 2, argv.end());
    int s = 0;
    try { s = sh.run_text(st, text, io); } catch (const ReturnSignal& r) { s = r.status; }
    if (argv.size() > 2) st.params = saved;
    return s;
  }
  if (c == "read") {
    bool raw = false;
    std::string prompt;
    std::vector<std::string> names;
    std::string delim = "\n";
    std::string array_name;
    for (size_t i = 1; i < argv.size(); ++i) {
      // -ra NAME and the like: flags may be bundled
      if (argv[i].size() > 2 && argv[i][0] == '-' && argv[i].find_first_not_of("rsea", 1) == std::string::npos) {
        if (argv[i].find('r') != std::string::npos) raw = true;
        if (argv[i].find('a') != std::string::npos && i + 1 < argv.size()) array_name = argv[++i];
        continue;
      }
      if (argv[i] == "-a" && i + 1 < argv.size()) { array_name = argv[++i]; continue; }
      if (argv[i] == "-r") raw = true;
      else if (argv[i] == "-p" && i + 1 < argv.size()) prompt = argv[++i];
      else if (argv[i] == "-d" && i + 1 < argv.size()) delim = argv[++i].empty() ? std::string(1, '\0') : argv[i].substr(0, 1);
      else if (argv[i] == "-s" || argv[i] == "-e") {}
      else if ((argv[i] == "-t" || argv[i] == "-n" || argv[i] == "-u") && i + 1 < argv.size()) ++i;
      else names.push_back(argv[i]);
    }
    if (names.empty()) names.push_back("REPLY");
    if (!prompt.empty()) io.ewrite(prompt);
    std::string line;
    bool got = false;
    char ch;
    while (os::read_fd(io.in, &ch, 1) == 1) {
      got = true;
      if (ch == delim[0]) break;
      if (!raw && ch == '\\') { char nx; if (os::read_fd(io.in, &nx, 1) == 1) { if (nx != '\n') line += nx; } continue; }
      line += ch;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string ifs = st.has("IFS") ? st.get("IFS") : " \t\n";
    if (!array_name.empty()) {
      // fields: IFS whitespace runs are one separator; other IFS characters each end a field
      std::vector<std::string> fields;
      size_t pos = 0;
      auto ws = [&](char ch) { return ifs.find(ch) != std::string::npos && std::isspace(static_cast<unsigned char>(ch)); };
      while (pos < line.size() && ws(line[pos])) ++pos;
      while (pos < line.size()) {
        size_t e = pos;
        while (e < line.size() && ifs.find(line[e]) == std::string::npos) ++e;
        fields.push_back(line.substr(pos, e - pos));
        pos = e;
        while (pos < line.size() && ws(line[pos])) ++pos;
        if (pos < line.size() && ifs.find(line[pos]) != std::string::npos) { ++pos; while (pos < line.size() && ws(line[pos])) ++pos; }
      }
      std::string v(1, kArrayMark);
      for (const auto& f : fields) { v += kArraySep; v += f; }
      st.assoc.erase(array_name);
      st.set(array_name, v);
      return got ? 0 : 1;
    }
    if (names.size() == 1 && names[0] == "REPLY") { st.set("REPLY", line); return got ? 0 : 1; }
    size_t pos = 0;
    for (size_t i = 0; i < names.size(); ++i) {
      while (pos < line.size() && ifs.find(line[pos]) != std::string::npos && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
      if (i + 1 == names.size()) {
        std::string restv = line.substr(std::min(pos, line.size()));
        while (!restv.empty() && ifs.find(restv.back()) != std::string::npos && std::isspace(static_cast<unsigned char>(restv.back()))) restv.pop_back();
        st.set(names[i], restv);
        break;
      }
      size_t e = pos;
      while (e < line.size() && ifs.find(line[e]) == std::string::npos) ++e;
      st.set(names[i], line.substr(std::min(pos, line.size()), e - std::min(pos, line.size())));
      pos = e < line.size() ? e + 1 : e;
    }
    return got ? 0 : 1;
  }
  if (c == "exec") {
    if (argv.size() == 1) return 0;   // redirections only: applied by the caller to this command (not persisted here)
    std::vector<std::string> rest(argv.begin() + 1, argv.end());
    bool f2 = false;
    int s = run_builtin(sh, st, rest, io, &f2);
    if (!f2) { auto& u = utilities(); auto it = u.find(rest[0]); s = it != u.end() ? it->second(sh, st, rest, io) : run_external(sh, st, rest, io, {}); }
    throw ExitSignal{s};
  }
  if (c == "command") {
    size_t i = 1;
    bool describe = false;
    while (i < argv.size() && argv[i].size() > 1 && argv[i][0] == '-') { if (argv[i] == "-v" || argv[i] == "-V") describe = true; ++i; }
    if (i >= argv.size()) return 0;
    std::vector<std::string> rest(argv.begin() + static_cast<long>(i), argv.end());
    if (describe) {
      const std::string& name = rest[0];
      static const char* bi[] = {"cd", "export", "readonly", "unset", "local", "set", "shift", "exit", "return", "break", "continue",
                                 "eval", ".", "source", "read", "exec", "command", "type", "wait", "trap", "umask", "hash", "alias", "getopts",
                                 "declare", "typeset", "let", "mapfile", "readarray"};
      for (const char* b : bi) if (name == b) { io.write(name + "\n"); return 0; }
      if (st.functions.count(name) || utilities().count(name)) { io.write(name + "\n"); return 0; }
      std::string p = os::find_program(name, st.get("PATH"), st.cwd);
      if (p.empty()) return 1;
      io.write(p + "\n");
      return 0;
    }
    bool f2 = false;
    int s = run_builtin(sh, st, rest, io, &f2);
    if (f2) return s;
    auto& u = utilities();
    auto it = u.find(rest[0]);
    if (it != u.end()) return it->second(sh, st, rest, io);
    return run_external(sh, st, rest, io, {});
  }
  if (c == "type") {
    int status = 0;
    for (size_t i = 1; i < argv.size(); ++i) {
      const std::string& name = argv[i];
      if (name.size() && name[0] == '-') continue;
      if (st.functions.count(name)) { io.write(name + " is a function\n"); continue; }
      if (utilities().count(name) && !(name == "git" && !host_git(st).empty())) { io.write(name + " is a shell builtin\n"); continue; }
      std::string p = os::find_program(name, st.get("PATH"), st.cwd);
      if (p.empty()) { sh.error(io, "type: " + name + ": not found"); status = 1; continue; }
      io.write(name + " is " + p + "\n");
    }
    return status;
  }
  if (c == "wait" || c == "trap" || c == "umask" || c == "hash" || c == "alias" || c == "unalias" || c == "ulimit" || c == "shopt" || c == "disown") return 0;
  if (c == "getopts") {
    // getopts optstring name [args]
    if (argv.size() < 3) return 2;
    const std::string& spec = argv[1];
    const std::string& var = argv[2];
    std::vector<std::string> args = argv.size() > 3 ? std::vector<std::string>(argv.begin() + 3, argv.end()) : st.params;
    long long ind = 1;
    parse_int(st.get("OPTIND"), ind);
    if (ind < 1) ind = 1;
    static thread_local size_t subpos = 1;
    if (static_cast<size_t>(ind) > args.size()) { st.set(var, "?"); return 1; }
    const std::string& a = args[static_cast<size_t>(ind - 1)];
    if (a.size() < 2 || a[0] != '-' || a == "--") { if (a == "--") st.set("OPTIND", std::to_string(ind + 1)); st.set(var, "?"); return 1; }
    if (subpos >= a.size()) subpos = 1;
    char o = a[subpos++];
    size_t at = spec.find(o);
    bool next_arg = subpos >= a.size();
    if (at == std::string::npos) { st.set(var, "?"); if (next_arg) { subpos = 1; st.set("OPTIND", std::to_string(ind + 1)); } return 0; }
    st.set(var, std::string(1, o));
    if (at + 1 < spec.size() && spec[at + 1] == ':') {
      if (!next_arg) { st.set("OPTARG", a.substr(subpos)); }
      else if (static_cast<size_t>(ind) < args.size()) { st.set("OPTARG", args[static_cast<size_t>(ind)]); ++ind; }
      subpos = 1;
      st.set("OPTIND", std::to_string(ind + 1));
      return 0;
    }
    if (next_arg) { subpos = 1; st.set("OPTIND", std::to_string(ind + 1)); }
    return 0;
  }
  if (c == "sh" || c == "bash" || c == "dash") return shell_main_like(sh, st, argv, io);
  *found = false;
  return 0;
}

int run_utility_by_name(Shell& sh, State& st, const std::vector<std::string>& argv, const Io& io) {
  bool found = false;
  int s = run_builtin(sh, st, argv, io, &found);
  if (found) return s;
  auto& u = utilities();
  auto it = u.find(argv[0]);
  if (it != u.end()) return it->second(sh, st, argv, io);
  return run_external(sh, st, argv, io, {});
}

}  // namespace aiossh
