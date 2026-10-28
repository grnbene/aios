// The registry of running grain runtimes (runtime_registry.hpp): each process's record, written by itself, read
// by `grain ps`.
#include "aios/userland/runtime_registry.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "aios/userland/grain.hpp"
#include "grain_builtins_internal.hpp"

#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace aios::userland::runtime {

namespace {

std::string json_str(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
  }
  return o + "\"";
}

int64_t unix_now() { return (int64_t)std::time(nullptr); }

#if !defined(_WIN32)
// When the process started, in clock ticks since boot (/proc/<pid>/stat field 22): with the pid, which process it
// is - a pid can be given to another process once this one is gone. "" where there is no /proc.
std::string proc_start(long pid) {
  FILE* f = std::fopen(("/proc/" + std::to_string(pid) + "/stat").c_str(), "r");
  if (!f) return "";
  char buf[2048];
  const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
  std::fclose(f);
  buf[n] = 0;
  const char* p = std::strrchr(buf, ')');   // the name may hold spaces and parentheses; the fields follow the last ')'
  if (!p) return "";
  int field = 2;
  for (++p; *p && field < 22; ++p)
    if (*p == ' ') ++field;
  std::string out;
  while (*p >= '0' && *p <= '9') out += *p++;
  return out;
}

bool alive(long pid, const std::string& start) {
  if (pid <= 0) return false;
  if (::kill((pid_t)pid, 0) != 0 && errno != EPERM) return false;
  if (start.empty()) return true;
  const std::string now = proc_start(pid);
  return now.empty() || now == start;
}
#endif

struct Keeper {
  std::function<std::string()> fill;
  int seconds = 3;
  std::chrono::steady_clock::time_point next{};
};

struct State {
  std::mutex mu;             // the record's fields
  std::mutex fill_mu;        // held while a keeper's fill runs, so stop_keeping can wait for it
  bool announced = false;
  std::string kind, what, root, started_text;
  int64_t started = 0;
  std::map<int, std::string> ports;
  std::map<std::string, std::string> fields;   // key -> JSON
  std::map<std::string, Keeper> keepers;
  std::thread thread;
  std::atomic<bool> stop{false};
  std::string path;
};

State& state() {
  static State* s = new State();   // never destroyed: the keeper thread may outlive static destruction order
  return *s;
}

void write_locked(State& s) {
#if !defined(_WIN32)
  if (!s.announced) return;
  const std::string dir = registry_dir();
  if (dir.empty()) return;
  const long pid = (long)::getpid();
  s.path = dir + "/" + std::to_string(pid) + ".json";
  char cwd[4096];
  const std::string here = ::getcwd(cwd, sizeof cwd) ? std::string(cwd) : std::string();
  std::string j = "{\"pid\":" + std::to_string(pid) + ",\"proc_start\":" + json_str(s.started_text) + ",\"kind\":" + json_str(s.kind) +
                  ",\"what\":" + json_str(s.what) + ",\"root\":" + json_str(s.root) + ",\"cwd\":" + json_str(here) +
                  ",\"started\":" + std::to_string(s.started) + ",\"updated\":" + std::to_string(unix_now()) + ",\"ports\":[";
  bool first = true;
  for (const auto& [port, label] : s.ports) {
    if (!first) j += ",";
    first = false;
    j += "{\"port\":" + std::to_string(port) + ",\"for\":" + json_str(label) + "}";
  }
  j += "]";
  for (const auto& [k, v] : s.fields) j += "," + json_str(k) + ":" + (v.empty() ? std::string("null") : v);
  j += "}\n";
  const std::string tmp = s.path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return;
  size_t off = 0;
  while (off < j.size()) {
    const ssize_t w = ::write(fd, j.data() + off, j.size() - off);
    if (w <= 0) break;
    off += (size_t)w;
  }
  ::close(fd);
  if (off == j.size()) ::rename(tmp.c_str(), s.path.c_str());
  else ::unlink(tmp.c_str());
#else
  (void)s;
#endif
}

void at_exit() { withdraw(); }

void ensure_thread(State& s) {
  if (s.thread.joinable()) return;
  s.thread = std::thread([&s] {
    while (!s.stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      if (s.stop.load()) break;
      std::lock_guard<std::mutex> fl(s.fill_mu);
      std::vector<std::pair<std::string, std::function<std::string()>>> due;
      {
        std::lock_guard<std::mutex> lk(s.mu);
        const auto now = std::chrono::steady_clock::now();
        for (auto& [k, kp] : s.keepers)
          if (now >= kp.next) { due.push_back({k, kp.fill}); kp.next = now + std::chrono::seconds(kp.seconds); }
      }
      if (due.empty()) continue;
      std::map<std::string, std::string> got;
      for (auto& [k, fill] : due) {
        try { got[k] = fill(); } catch (...) { got[k] = "null"; }
      }
      std::lock_guard<std::mutex> lk(s.mu);
      for (auto& [k, v] : got)
        if (s.keepers.count(k)) s.fields[k] = v;
      write_locked(s);
    }
  });
  s.thread.detach();   // never joined: it holds no resources a normal exit needs, and withdraw() stops its writes
}

std::string ago(int64_t secs) {
  if (secs < 0) secs = 0;
  if (secs < 60) return std::to_string(secs) + "s";
  if (secs < 3600) return std::to_string(secs / 60) + "m";
  if (secs < 86400) return std::to_string(secs / 3600) + "h";
  return std::to_string(secs / 86400) + "d";
}

using grain::Value;
using grain::Type;
using grain::MapObj;

std::string text_of(const Value* v) { return v && v->type != Type::Nil ? (v->type == Type::Str ? v->str() : v->to_text()) : std::string(); }

const Value* get(const Value& m, const char* k) {
  if (m.type != Type::Map) return nullptr;
  return static_cast<MapObj*>(m.obj.get())->find(k);
}

std::string pad_to(std::string s, size_t n) {
  if (s.size() > n) return s.substr(0, n - 1) + "~";
  s.resize(n, ' ');
  return s;
}

}  // namespace

std::string registry_dir() {
#if defined(_WIN32)
  return "";
#else
  static std::string cached;
  static std::once_flag once;
  std::call_once(once, [] {
    std::string base;
    if (const char* x = std::getenv("XDG_RUNTIME_DIR"); x && *x) {
      struct stat st;
      if (::stat(x, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == ::getuid()) base = std::string(x) + "/grain";
    }
    if (base.empty()) base = "/tmp/grain-" + std::to_string((long)::getuid());
    ::mkdir(base.c_str(), 0700);
    struct stat st;
    // only a directory of this user's own, closed to everyone else (a /tmp name someone else made first is not ours)
    if (::lstat(base.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::getuid() || (st.st_mode & 077) != 0) return;
    cached = base;
  });
  return cached;
#endif
}

void announce(const std::string& kind, const std::string& what, const std::string& root) {
  State& s = state();
  std::lock_guard<std::mutex> lk(s.mu);
  if (!s.announced) {
    s.started = unix_now();
#if !defined(_WIN32)
    s.started_text = proc_start((long)::getpid());
#endif
    std::atexit(at_exit);
  }
  s.announced = true;
  s.kind = kind;
  s.what = what;
  if (!root.empty() || s.root.empty()) s.root = root;
  write_locked(s);
}

void add_port(int port, const std::string& label) {
  State& s = state();
  std::lock_guard<std::mutex> lk(s.mu);
  s.ports[port] = label;
  write_locked(s);
}

void forget_port(int port) {
  State& s = state();
  std::lock_guard<std::mutex> lk(s.mu);
  s.ports.erase(port);
  write_locked(s);
}

void keep_current(const std::string& key, std::function<std::string()> fill, int seconds) {
  State& s = state();
  std::string first;
  try { first = fill(); } catch (...) { first = "null"; }
  {
    std::lock_guard<std::mutex> lk(s.mu);
    Keeper kp;
    kp.fill = std::move(fill);
    kp.seconds = std::max(1, seconds);
    kp.next = std::chrono::steady_clock::now() + std::chrono::seconds(kp.seconds);
    s.keepers[key] = std::move(kp);
    s.fields[key] = first;
    write_locked(s);
    ensure_thread(s);
  }
}

void stop_keeping(const std::string& key) {
  State& s = state();
  std::lock_guard<std::mutex> fl(s.fill_mu);   // a fill in progress finishes first: what it reads may be going away
  std::lock_guard<std::mutex> lk(s.mu);
  s.keepers.erase(key);
}

void withdraw() {
#if !defined(_WIN32)
  State& s = state();
  std::lock_guard<std::mutex> lk(s.mu);
  if (!s.announced) return;
  s.announced = false;
  s.stop = true;
  s.keepers.clear();
  if (!s.path.empty()) ::unlink(s.path.c_str());
#endif
}

std::vector<std::string> app_host_homes() {
  std::vector<std::string> out;
#if !defined(_WIN32)
  // the records, as ps_text reads them: its JSON form keeps this to one reader
  std::string err;
  Value all = grain::grain_json_parse(ps_text(true), err);
  const Value* regs = err.empty() ? get(all, "registered") : nullptr;
  if (regs && regs->type == Type::List)
    for (const Value& r : regs->list_items())
      if (text_of(get(r, "kind")) == "app host" && !text_of(get(r, "root")).empty()) out.push_back(text_of(get(r, "root")));
#endif
  return out;
}

std::string ps_text(bool json) {
#if defined(_WIN32)
  (void)json;
  return "grain ps: not on this system yet\n";
#else
  const std::string dir = registry_dir();
  std::vector<Value> recs;
  std::set<long> known;
  if (!dir.empty()) {
    if (DIR* d = ::opendir(dir.c_str())) {
      while (dirent* e = ::readdir(d)) {
        std::string name = e->d_name;
        if (name.size() < 6 || name.compare(name.size() - 5, 5, ".json") != 0) continue;
        const std::string path = dir + "/" + name;
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) continue;
        std::string text;
        char buf[8192];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
        std::string err;
        Value v = grain::grain_json_parse(text, err);
        if (!err.empty() || v.type != Type::Map) continue;
        const long pid = std::strtol(text_of(get(v, "pid")).c_str(), nullptr, 10);
        if (!alive(pid, text_of(get(v, "proc_start")))) { ::unlink(path.c_str()); continue; }   // gone: its record goes too
        known.insert(pid);
        recs.push_back(v);
      }
      ::closedir(d);
    }
  }
  std::sort(recs.begin(), recs.end(), [](const Value& a, const Value& b) { return text_of(get(a, "started")) < text_of(get(b, "started")); });

  // grain / aios processes of this user that wrote no record, with the ports they listen on
  struct Other { long pid; std::string cmd; int64_t up; std::vector<int> ports; };
  std::vector<Other> others;
  std::map<std::string, int> listening;   // socket inode -> port
  for (const char* t : {"/proc/net/tcp", "/proc/net/tcp6"}) {
    FILE* f = std::fopen(t, "r");
    if (!f) continue;
    char line[512];
    if (!std::fgets(line, sizeof line, f)) { std::fclose(f); continue; }   // the header line
    while (std::fgets(line, sizeof line, f)) {
      char local[128], rem[128], st[8];
      unsigned long inode = 0;
      int uid = 0;
      if (std::sscanf(line, "%*d: %127s %127s %7s %*s %*s %*s %d %*d %lu", local, rem, st, &uid, &inode) != 5) continue;
      if (std::string(st) != "0A") continue;   // LISTEN
      const char* colon = std::strrchr(local, ':');
      if (colon) listening[std::to_string(inode)] = (int)std::strtol(colon + 1, nullptr, 16);
    }
    std::fclose(f);
  }
  const long self = (long)::getpid();
  long uptime_ticks_now = 0;
  {
    FILE* f = std::fopen("/proc/uptime", "r");
    double up = 0;
    if (f) { if (std::fscanf(f, "%lf", &up) != 1) up = 0; std::fclose(f); }
    uptime_ticks_now = (long)(up * (double)::sysconf(_SC_CLK_TCK));
  }
  if (DIR* d = ::opendir("/proc")) {
    while (dirent* e = ::readdir(d)) {
      const long pid = std::strtol(e->d_name, nullptr, 10);
      if (pid <= 0 || pid == self || known.count(pid)) continue;
      struct stat st;
      const std::string base = std::string("/proc/") + e->d_name;
      if (::stat(base.c_str(), &st) != 0 || st.st_uid != ::getuid()) continue;
      FILE* f = std::fopen((base + "/cmdline").c_str(), "r");
      if (!f) continue;
      std::string cmd;
      char buf[4096];
      size_t n;
      while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) cmd.append(buf, n);
      std::fclose(f);
      if (cmd.empty()) continue;
      std::string exe = cmd.substr(0, cmd.find('\0'));
      const std::string prog = exe.substr(exe.rfind('/') == std::string::npos ? 0 : exe.rfind('/') + 1);
      if (prog.rfind("grain", 0) != 0 && prog.rfind("aios", 0) != 0) continue;
      std::replace(cmd.begin(), cmd.end(), '\0', ' ');
      while (!cmd.empty() && cmd.back() == ' ') cmd.pop_back();
      Other o{pid, cmd, 0, {}};
      const std::string started = proc_start(pid);
      if (!started.empty() && uptime_ticks_now > 0) o.up = (uptime_ticks_now - std::strtol(started.c_str(), nullptr, 10)) / ::sysconf(_SC_CLK_TCK);
      if (DIR* fd = ::opendir((base + "/fd").c_str())) {
        while (dirent* fe = ::readdir(fd)) {
          char link[128];
          const ssize_t l = ::readlink((base + "/fd/" + fe->d_name).c_str(), link, sizeof link - 1);
          if (l <= 0) continue;
          link[l] = 0;
          std::string s(link);
          if (s.rfind("socket:[", 0) != 0) continue;
          auto it = listening.find(s.substr(8, s.size() - 9));
          if (it != listening.end()) o.ports.push_back(it->second);
        }
        ::closedir(fd);
      }
      std::sort(o.ports.begin(), o.ports.end());
      o.ports.erase(std::unique(o.ports.begin(), o.ports.end()), o.ports.end());
      others.push_back(std::move(o));
    }
    ::closedir(d);
  }

  if (json) {
    std::string out = "{\"registered\":[";
    for (size_t i = 0; i < recs.size(); ++i) out += (i ? "," : "") + grain::grain_json_stringify(recs[i]);
    out += "],\"unregistered\":[";
    for (size_t i = 0; i < others.size(); ++i) {
      std::string ports;
      for (size_t k = 0; k < others[i].ports.size(); ++k) ports += (k ? "," : "") + std::to_string(others[i].ports[k]);
      out += (i ? "," : "") + std::string("{\"pid\":") + std::to_string(others[i].pid) + ",\"command\":" + json_str(others[i].cmd) +
             ",\"up_seconds\":" + std::to_string(others[i].up) + ",\"ports\":[" + ports + "]}";
    }
    return out + "]}\n";
  }

  std::ostringstream o;
  const int64_t now = unix_now();
  o << pad_to("PID", 9) << pad_to("KIND", 13) << pad_to("UP", 6) << pad_to("PORTS", 30) << "WHAT\n";
  if (recs.empty()) o << "(no grain runtime has registered on this machine" << (dir.empty() ? ": no registry directory" : "") << ")\n";
  for (const Value& r : recs) {
    std::string ports;
    if (const Value* ps = get(r, "ports"); ps && ps->type == Type::List)
      for (const Value& p : ps->list_items()) ports += (ports.empty() ? "" : ", ") + text_of(get(p, "port")) + " " + text_of(get(p, "for"));
    const int64_t started = std::strtoll(text_of(get(r, "started")).c_str(), nullptr, 10);
    std::string what = text_of(get(r, "what"));
    const std::string root = text_of(get(r, "root"));
    if (!root.empty()) what += "  [" + root + "]";
    o << pad_to(text_of(get(r, "pid")), 9) << pad_to(text_of(get(r, "kind")), 13) << pad_to(ago(now - started), 6)
      << pad_to(ports.empty() ? "-" : ports, 30) << what << "\n";
    const int64_t updated = std::strtoll(text_of(get(r, "updated")).c_str(), nullptr, 10);
    if (const Value* apps = get(r, "apps"); apps && apps->type == Type::List) {
      for (const Value& a : apps->list_items()) {
        std::string line = "    app   " + pad_to(text_of(get(a, "name")), 22) + pad_to(text_of(get(a, "state")), 10) +
                           pad_to(text_of(get(a, "version")).substr(0, 8), 10);
        const std::string port = text_of(get(a, "port"));
        line += pad_to(port.empty() || port == "0" ? "" : ":" + port, 8);
        line += text_of(get(a, "requests")) + " req, " + text_of(get(a, "failures")) + " failed";
        if (const std::string err = text_of(get(a, "error")); !err.empty()) line += "  (" + err.substr(0, 60) + ")";
        o << line << "\n";
      }
    }
    if (const Value* th = get(r, "things"); th && th->type == Type::List) {
      for (const Value& t : th->list_items()) {
        o << "    thing " << pad_to(text_of(get(t, "thing")), 22) << pad_to(text_of(get(t, "state")), 10)
          << pad_to(text_of(get(t, "things")) + " rows", 12) << "version " << text_of(get(t, "version")) << "\n";
      }
    }
    if ((get(r, "apps") || get(r, "things")) && now - updated > 10) o << "    (as of " << ago(now - updated) << " ago)\n";
  }
  if (!others.empty()) {
    o << "\nnot registered (an older binary, or started some other way):\n";
    for (const auto& x : others) {
      std::string ports;
      for (int p : x.ports) ports += (ports.empty() ? "" : ", ") + std::to_string(p);
      o << pad_to(std::to_string(x.pid), 9) << pad_to("?", 13) << pad_to(ago(x.up), 6) << pad_to(ports.empty() ? "-" : ports, 30)
        << (x.cmd.size() > 110 ? x.cmd.substr(0, 109) + "~" : x.cmd) << "\n";
    }
  }
  return o.str();
#endif
}

}  // namespace aios::userland::runtime
