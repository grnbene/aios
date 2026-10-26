// `grain host` and `grain app`: apps hosted by grain on its own (aios/userland/grain_app.hpp). A host keeps its
// apps in a directory and answers on grain's encrypted control link; `grain app` talks to it - this machine's, or
// one named in ~/.grain/hosts.json - and needs nothing else on either end.
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "aios/core/base64.hpp"
#include "aios/core/json.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/sync.hpp"
#include "aios/userland/grain_app.hpp"
#include "tools.hpp"

#if defined(__linux__)
#include <unistd.h>
#endif

namespace grain_cli {

namespace g = aios::userland::grain;
namespace ga = aios::userland::grain::apps;

namespace {

constexpr int kOk = 0, kUsage = 64, kFailed = 1;
constexpr uint16_t kDefaultPort = 7480;

std::string text_of(const aios::ByteBuffer& b) { return std::string(b.begin(), b.end()); }
aios::ByteSpan span_of(const std::string& s) { return aios::ByteSpan(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

// Where this machine's host keeps its apps: GRAIN_HOME, else ~/.grain (%LOCALAPPDATA%\grain on Windows).
std::string default_home(aios::hal::Host& h) {
  std::string v = h.env("GRAIN_HOME");
  if (!v.empty()) return v;
#if defined(_WIN32)
  v = h.env("LOCALAPPDATA");
  if (!v.empty()) return v + "\\grain";
#endif
  v = h.env("HOME");
  return (v.empty() ? std::string(".") : v) + "/.grain";
}

struct Target {
  std::string name, address, token;
  uint16_t port = kDefaultPort;
};

std::string read_trim(aios::hal::Host& h, const std::string& path) {
  auto d = h.read_file(path);
  if (!d.ok()) return "";
  std::string t = text_of(*d);
  while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
  return t;
}

// "local" (this machine's host), a name from <home>/hosts.json, or host:port with key=...
bool resolve(aios::hal::Host& h, const std::string& home, const std::string& want, const std::string& key_arg, Target& t, std::string& err) {
  if (want.empty() || want == "local") {
    t.name = "local";
    t.address = "127.0.0.1";
    const std::string p = read_trim(h, home + "/host.port");
    if (!p.empty()) t.port = (uint16_t)std::atoi(p.c_str());
    t.token = read_trim(h, home + "/host.key");
    if (t.token.empty()) { err = "no host on this machine yet (" + home + "): start one with `grain host`"; return false; }
    return true;
  }
  auto d = h.read_file(home + "/hosts.json");
  if (d.ok()) {
    auto j = aios::json::parse(text_of(*d));
    if (j.ok() && j->has(want)) {
      const auto& e = (*j)[want];
      t.name = want;
      t.address = e["address"].str();
      t.port = (uint16_t)e["port"].i64(kDefaultPort);
      t.token = e["key"].str();
      if (t.token.empty() && !e["key_file"].str().empty()) t.token = read_trim(h, e["key_file"].str());
      if (t.token.empty()) { err = "hosts.json has no key for " + want; return false; }
      return true;
    }
  }
  const size_t colon = want.rfind(':');
  t.name = want;
  t.address = colon == std::string::npos ? want : want.substr(0, colon);
  if (colon != std::string::npos) t.port = (uint16_t)std::atoi(want.c_str() + colon + 1);
  t.token = key_arg.size() > 1 && key_arg[0] == '@' ? read_trim(h, key_arg.substr(1)) : key_arg;
  if (t.token.empty()) { err = "no host called " + want + " in " + home + "/hosts.json, and no key=<key|@file> for it"; return false; }
  return true;
}

bool call(aios::hal::Host& h, const Target& t, const aios::json::Value& req, aios::json::Value& reply, std::string& err) {
  auto r = ga::call_control(h, t.address, t.port, t.token, req.dump());
  if (!r.ok()) { err = r.error().message; return false; }
  auto j = aios::json::parse(*r);
  if (!j.ok()) { err = "the host answered something that is not JSON"; return false; }
  reply = *j;
  return true;
}

void print_lines(const aios::json::Value& v) { for (const auto& l : v.items()) std::cout << l.str() << "\n"; }

std::string pad(std::string s, size_t w) { if (s.size() < w) s.append(w - s.size(), ' '); return s + " "; }

std::string uptime(int64_t s) {
  if (s <= 0) return "-";
  if (s < 120) return std::to_string(s) + "s";
  if (s < 7200) return std::to_string(s / 60) + "m";
  if (s < 172800) return std::to_string(s / 3600) + "h";
  return std::to_string(s / 86400) + "d";
}

void print_status(const Target& t, const aios::json::Value& r, bool header) {
  if (header) std::cout << pad("HOST", 12) << pad("APP", 18) << pad("STATE", 10) << pad("VERSION", 17) << pad("PORT", 6) << pad("UP", 6)
                        << pad("RESTARTS", 8) << pad("REQUESTS", 9) << "ERRORS\n";
  if (r["apps"].size() == 0) std::cout << pad(t.name, 12) << "(no apps)\n";
  for (const auto& a : r["apps"].items()) {
    std::cout << pad(t.name, 12) << pad(a["name"].str(), 18) << pad(a["state"].str(), 10) << pad(a["version"].str(), 17)
              << pad(a["port"].i64() ? std::to_string(a["port"].i64()) : "-", 6) << pad(uptime(a["uptime_s"].i64()), 6)
              << pad(std::to_string(a["restarts"].i64()), 8) << pad(std::to_string(a["requests"].i64()), 9) << a["failures"].i64() << "\n";
    if (!a["error"].str().empty() && a["state"].str() != "running") std::cout << "    " << a["error"].str() << "\n";
  }
}

void host_usage(std::ostream& o) {
  o << "grain host [--home DIR] [--port N] [--all-ips]   run this machine's app host (in the foreground)\n"
       "grain host install [--home DIR] [--port N] [--all-ips]   start it with the machine from now on\n"
       "grain host key [--home DIR]                       the key `grain app ... at <this machine>` needs\n";
}

void app_usage(std::ostream& o) {
  o << "grain app check <dir>                  the manifest and every file compile\n"
       "grain app run <dir>                    run it here, in the foreground, with its log on the screen\n"
       "grain app deploy <dir> [at HOST]       a new version: kept, started, health-asked, checked; rolled back if it fails\n"
       "grain app status [APP] [at HOST|--all] what runs, which version, since when, restarts, requests\n"
       "grain app start|stop|restart|remove APP [at HOST]\n"
       "grain app rollback APP [at HOST]       the version before this one\n"
       "grain app versions APP [at HOST]       the versions it has run, newest last\n"
       "grain app logs APP [-n N] [-f] [at HOST]\n"
       "grain app set APP KEY=VALUE... | unset APP KEY... [at HOST]   this machine's settings for it\n"
       "grain app secret APP NAME [at HOST]    reads the value from stdin; kept sealed on that machine only\n"
       "grain app settings|secrets APP [at HOST]\n"
       "grain app hosts                        the hosts `at NAME` knows (<home>/hosts.json)\n"
       "grain app add-host NAME ADDRESS[:PORT] key=<key|@file>\n"
       "HOST is `local` (default), a name from hosts.json, or ADDRESS:PORT with key=<key|@file>.\n";
}

struct Parsed {
  std::vector<std::string> words;
  std::string home, at, key;
  bool all = false, follow = false, all_ips = false;
  int n = 100;
  uint16_t port = kDefaultPort;
};

Parsed parse(aios::hal::Host& h, const std::vector<std::string>& a) {
  Parsed p;
  p.home = default_home(h);
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] == "--home" && i + 1 < a.size()) p.home = a[++i];
    else if ((a[i] == "at" || a[i] == "on" || a[i] == "to") && i + 1 < a.size()) p.at = a[++i];
    else if (a[i].rfind("key=", 0) == 0) p.key = a[i].substr(4);
    else if (a[i] == "--all" || a[i] == "--all-hosts") p.all = true;
    else if (a[i] == "--all-ips") p.all_ips = true;
    else if (a[i] == "-f" || a[i] == "--follow") p.follow = true;
    else if (a[i] == "-n" && i + 1 < a.size()) p.n = std::atoi(a[++i].c_str());
    else if (a[i] == "--port" && i + 1 < a.size()) p.port = (uint16_t)std::atoi(a[++i].c_str());
    else p.words.push_back(a[i]);
  }
  return p;
}

// The VM setup an app's VMs get here: everything `grain run` gives a program.
void setup_vm(g::Vm& vm) { prepare_vm(vm, {}); }

std::vector<std::string> host_caps() { return {"grain", "http", "db", "files", "process"}; }

int run_host(aios::hal::Host& h, const Parsed& p) {
  auto token = ga::host_token(h, p.home);
  if (!token.ok()) { std::cerr << "grain host: " << token.error().message << "\n"; return kFailed; }
  auto store = ga::make_dir_store(h, p.home);
  ga::Host apps(h, *store, {setup_vm, host_caps(), h.env("HOSTNAME")});
  ga::ControlServer control(h, apps, *token);
  auto port = control.listen(p.port, !p.all_ips);
  if (!port.ok()) { std::cerr << "grain host: cannot listen on port " << p.port << ": " << port.error().message << "\n"; return kFailed; }
  const std::string ps = std::to_string(*port);
  (void)h.write_file(p.home + "/host.port", span_of(ps + "\n"));
  apps.start_all();
  std::cout << "grain host: " << store->apps().size() << " app(s) in " << p.home << "; control on " << (p.all_ips ? "all IPs" : "127.0.0.1")
            << " port " << *port << " (key: " << p.home << "/host.key)\n" << std::flush;
  for (;;) aios::hal::sleep_ms(60000);
}

// Starting the host with the machine: the one thing that is the operating system's to do.
int install_host(aios::hal::Host& h, const Parsed& p) {
  std::string self = h.env("GRAIN_SELF");
#if defined(__linux__)
  if (self.empty()) { char buf[4096]; const long n = readlink("/proc/self/exe", buf, sizeof buf - 1); if (n > 0) self.assign(buf, (size_t)n); }
  const bool root = h.env("USER") == "root" || h.env("HOME") == "/root";
  const std::string args = " host --home " + p.home + " --port " + std::to_string(p.port) + (p.all_ips ? " --all-ips" : "");
  // The binary is copied next to the apps, so replacing grain later does not change what the running host is.
  const std::string bin = p.home + "/bin/grain";
  auto self_bytes = h.read_file(self);
  if (!self_bytes.ok()) { std::cerr << "grain host install: cannot read " << self << "\n"; return kFailed; }
  (void)h.make_dir(p.home);
  (void)h.make_dir(p.home + "/bin");
  if (auto w = h.write_file(bin + ".new", *self_bytes); !w.ok()) { std::cerr << "grain host install: " << w.error().message << "\n"; return kFailed; }
  (void)h.set_access(bin + ".new", aios::hal::Host::FileAccess::Executable);
  (void)h.rename_file(bin + ".new", bin);
  const std::string unit = "[Unit]\nDescription=grain app host (" + p.home + ")\nAfter=network.target\n\n[Service]\nExecStart=" + bin + args +
                           "\nRestart=always\nRestartSec=2\n\n[Install]\nWantedBy=" + (root ? "multi-user.target" : "default.target") + "\n";
  const std::string dir = root ? "/etc/systemd/system" : h.env("HOME") + "/.config/systemd/user";
  if (!root) { (void)h.make_dir(h.env("HOME") + "/.config"); (void)h.make_dir(h.env("HOME") + "/.config/systemd"); (void)h.make_dir(dir); }
  if (auto w = h.write_file(dir + "/grain-host.service", span_of(unit)); !w.ok()) { std::cerr << "grain host install: " << w.error().message << "\n"; return kFailed; }
  const std::string ctl = root ? "systemctl" : "systemctl --user";
  const int rc = std::system((ctl + " daemon-reload && " + ctl + " enable --now grain-host.service").c_str());
  std::cout << "grain host: installed as " << dir << "/grain-host.service (" << bin << ")" << (rc == 0 ? ", running" : ", but systemctl failed") << "\n";
  return rc == 0 ? kOk : kFailed;
#elif defined(__APPLE__)
  (void)self;
  const std::string bin = p.home + "/bin/grain";
  std::cout << "grain host install: on macOS, a LaunchAgent that runs `" << bin << " host --home " << p.home
            << "`; not written yet - run `grain host` from launchd or a terminal for now\n";
  return kFailed;
#else
  (void)self;
  std::cout << "grain host install: not available on this system yet - run `grain host` as a service by hand\n";
  return kFailed;
#endif
}

}  // namespace

int host_command(const std::vector<std::string>& args) {
  auto& h = aios::hal::process_host();
  Parsed p = parse(h, args);
  if (!p.words.empty() && (p.words[0] == "help" || p.words[0] == "--help")) { host_usage(std::cout); return kOk; }
  if (!p.words.empty() && p.words[0] == "install") return install_host(h, p);
  if (!p.words.empty() && p.words[0] == "key") {
    auto t = ga::host_token(h, p.home);
    if (!t.ok()) { std::cerr << t.error().message << "\n"; return kFailed; }
    std::cout << *t << "\n";
    return kOk;
  }
  if (!p.words.empty()) { host_usage(std::cerr); return kUsage; }
  return run_host(h, p);
}

int app_command(const std::vector<std::string>& args) {
  ga::AppCommandEnv env;
  env.setup_vm = setup_vm;
  env.capabilities = host_caps();
  return ga::run_app_command(aios::hal::process_host(), args, env, std::cout, std::cerr);
}

}  // namespace grain_cli
