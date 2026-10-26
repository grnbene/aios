// `grain app ...` (and aios's `app ...`): the commands that check, pack, run and deploy an app and ask a host about
// its apps (grain_app.hpp). One implementation for every embedder: the grain CLI and the aios console call it.
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

namespace aios::userland::grain::apps {

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
    if (t.token.empty()) { err = "no app host on this machine yet (" + home + "): start one (`grain host`; in aios, `serve`)"; return false; }
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
  auto r = call_control(h, t.address, t.port, t.token, req.dump());
  if (!r.ok()) { err = r.error().message; return false; }
  auto j = aios::json::parse(*r);
  if (!j.ok()) { err = "the host answered something that is not JSON"; return false; }
  reply = *j;
  return true;
}

void print_lines(std::ostream& out, const aios::json::Value& v) { for (const auto& l : v.items()) out << l.str() << "\n"; }

std::string pad(std::string s, size_t w) { if (s.size() < w) s.append(w - s.size(), ' '); return s + " "; }

std::string uptime(int64_t s) {
  if (s <= 0) return "-";
  if (s < 120) return std::to_string(s) + "s";
  if (s < 7200) return std::to_string(s / 60) + "m";
  if (s < 172800) return std::to_string(s / 3600) + "h";
  return std::to_string(s / 86400) + "d";
}

void print_status(std::ostream& out, const Target& t0, const aios::json::Value& r, bool header) {
  Target t = t0;
  if (t.name == "local" && !r["machine"].str().empty()) t.name = r["machine"].str();   // which machine "local" is
  if (header) out << pad("HOST", 12) << pad("APP", 18) << pad("STATE", 10) << pad("VERSION", 17) << pad("PORT", 6) << pad("UP", 6)
                        << pad("RESTARTS", 8) << pad("REQUESTS", 9) << "ERRORS\n";
  if (r["apps"].size() == 0) out << pad(t.name, 12) << "(no apps)\n";
  for (const auto& a : r["apps"].items()) {
    out << pad(t.name, 12) << pad(a["name"].str(), 18) << pad(a["state"].str(), 10) << pad(a["version"].str(), 17)
              << pad(a["port"].i64() ? std::to_string(a["port"].i64()) : "-", 6) << pad(uptime(a["uptime_s"].i64()), 6)
              << pad(std::to_string(a["restarts"].i64()), 8) << pad(std::to_string(a["requests"].i64()), 9) << a["failures"].i64() << "\n";
    if (!a["error"].str().empty() && a["state"].str() != "running") out << "    " << a["error"].str() << "\n";
  }
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

}  // namespace

int run_app_command(hal::Host& h, const std::vector<std::string>& args, const AppCommandEnv& env, std::ostream& out, std::ostream& errs) {
  Parsed p = parse(h, args);
  if (!env.home.empty() && p.home == default_home(h)) p.home = env.home;
  if (p.words.empty() || p.words[0] == "help" || p.words[0] == "--help") { app_usage(p.words.empty() ? errs : out); return p.words.empty() ? kUsage : kOk; }
  const std::string sub = p.words[0];
  auto arg = [&](size_t i) { return i < p.words.size() ? p.words[i] : std::string(); };

  if (sub == "check" || sub == "pack") {
    auto b = pack_dir(h, arg(1).empty() ? "." : arg(1));
    if (!b.ok()) { errs << "grain app: " << b.error().message << "\n"; return kFailed; }
    auto m = read_manifest(*b);
    if (auto c = compile_all(*b); !c.ok()) { errs << "grain app: " << c.error().message << "\n"; return kFailed; }
    out << m->name << ": " << b->files.size() << " file(s), version " << b->id << ", " << m->kind
              << (m->port ? " on port " + std::to_string(m->port) : std::string()) << "; everything compiles\n";
    if (sub == "pack") {
      const std::string file = m->name + "-" + b->id + ".app";
      if (auto w = h.write_file(file, span_of(b->encode())); !w.ok()) { errs << w.error().message << "\n"; return kFailed; }
      out << "wrote " << file << "\n";
    }
    return kOk;
  }

  if (sub == "run") {   // this machine, this process, a private store in the app's own .grain
    const std::string dir = arg(1).empty() ? "." : arg(1);
    auto b = pack_dir(h, dir);
    if (!b.ok()) { errs << "grain app: " << b.error().message << "\n"; return kFailed; }
    auto store = make_dir_store(h, dir + "/.grain");
    Host apps(h, *store, {env.setup_vm, env.capabilities, "local"});
    std::vector<std::string> report;
    auto r = apps.deploy(*b, report);
    for (const auto& l : report) out << l << "\n";
    if (!r.ok()) return kFailed;
    size_t shown = store->read_log(b->name, 100000).size();
    for (;;) {   // the log as it grows
      aios::hal::sleep_ms(500);
      auto lines = store->read_log(b->name, 100000);
      for (size_t i = shown; i < lines.size(); ++i) out << lines[i] << "\n";
      if (lines.size() < shown) shown = 0; else shown = lines.size();
      out << std::flush;
    }
  }

  if (sub == "hosts") {
    auto d = h.read_file(p.home + "/hosts.json");
    out << "local  127.0.0.1:" << (read_trim(h, p.home + "/host.port").empty() ? std::to_string(kDefaultPort) : read_trim(h, p.home + "/host.port")) << "\n";
    if (d.ok()) if (auto j = aios::json::parse(text_of(*d)); j.ok())
      for (const auto& [n, e] : j->members()) out << n << "  " << e["address"].str() << ":" << e["port"].i64(kDefaultPort) << "\n";
    return kOk;
  }
  if (sub == "add-host") {
    if (arg(1).empty() || arg(2).empty() || p.key.empty()) { errs << "grain app add-host NAME ADDRESS[:PORT] key=<key|@file>\n"; return kUsage; }
    aios::json::Value all = aios::json::Value::object();
    if (auto d = h.read_file(p.home + "/hosts.json"); d.ok()) if (auto j = aios::json::parse(text_of(*d)); j.ok()) all = *j;
    aios::json::Value e = aios::json::Value::object();
    const std::string addr = arg(2);
    const size_t colon = addr.rfind(':');
    e.set("address", aios::json::Value::string(colon == std::string::npos ? addr : addr.substr(0, colon)));
    e.set("port", aios::json::Value::integer(colon == std::string::npos ? kDefaultPort : std::atoi(addr.c_str() + colon + 1)));
    if (p.key[0] == '@') e.set("key_file", aios::json::Value::string(p.key.substr(1))); else e.set("key", aios::json::Value::string(p.key));
    all.set(arg(1), e);
    (void)h.make_dir(p.home);
    const std::string text = all.dump();
    if (auto w = h.write_file(p.home + "/hosts.json", span_of(text)); !w.ok()) { errs << w.error().message << "\n"; return kFailed; }
    (void)h.set_access(p.home + "/hosts.json", aios::hal::Host::FileAccess::Private);
    out << "host " << arg(1) << " is " << e["address"].str() << ":" << e["port"].i64() << "\n";
    return kOk;
  }

  // Everything else is a request to one host (or, for status, every known host).
  std::vector<std::string> wanted;
  if (p.all) {
    wanted.push_back("local");
    if (auto d = h.read_file(p.home + "/hosts.json"); d.ok()) if (auto j = aios::json::parse(text_of(*d)); j.ok()) for (const auto& [n, e] : j->members()) wanted.push_back(n);
  } else {
    wanted.push_back(p.at);
  }
  aios::json::Value req = aios::json::Value::object();
  req.set("op", aios::json::Value::string(sub));
  if (sub == "deploy") {
    // @bundle:<base64>: a bundle packed elsewhere (aios sends one across the fleet this way), else a directory here
    const bool packed = arg(1).rfind("@bundle:", 0) == 0;
    Result<Bundle> b = Result<Bundle>(Status::Invalid, "");
    if (packed) {
      auto raw = base64_decode(arg(1).substr(8));
      b = raw.ok() ? Bundle::decode(std::string(raw->begin(), raw->end())) : Result<Bundle>(Status::Invalid, "the bundle is not base64");
    } else {
      b = pack_dir(h, arg(1).empty() ? "." : arg(1));
    }
    if (!b.ok()) { errs << "grain app: " << b.error().message << "\n"; return kFailed; }
    if (auto c = compile_all(*b); !c.ok()) { errs << "grain app: " << c.error().message << "\n"; return kFailed; }
    const std::string enc = b->encode();
    req.set("bundle", aios::json::Value::string(aios::base64_encode(span_of(enc))));
  } else if (sub == "set" || sub == "unset" || sub == "secret") {
    req.set("app", aios::json::Value::string(arg(1)));
    aios::json::Value values = aios::json::Value::object(), unset = aios::json::Value::array();
    if (sub == "set") {
      for (size_t i = 2; i < p.words.size(); ++i) {
        const size_t eq = p.words[i].find('=');
        if (eq == std::string::npos) { errs << "grain app set APP KEY=VALUE...\n"; return kUsage; }
        values.set(p.words[i].substr(0, eq), aios::json::Value::string(p.words[i].substr(eq + 1)));
      }
    } else if (sub == "unset") {
      req.set("op", aios::json::Value::string("set"));
      for (size_t i = 2; i < p.words.size(); ++i) unset.push(aios::json::Value::string(p.words[i]));
    } else {
      if (arg(2).empty()) { errs << "grain app secret APP NAME   (the value is read from stdin)\n"; return kUsage; }
      std::string value;
      std::getline(std::cin, value);
      while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
      values.set(arg(2), aios::json::Value::string(value));
    }
    req.set("values", values);
    req.set("unset", unset);
  } else {
    req.set("app", aios::json::Value::string(arg(1)));
    req.set("n", aios::json::Value::integer(p.n));
  }
  int rc = kOk;
  bool first = true;
  for (const auto& w : wanted) {
    Target t;
    std::string err;
    if (!resolve(h, p.home, w, p.key, t, err)) { errs << "grain app: " << err << "\n"; rc = kFailed; continue; }
    size_t seen = 0;
    do {
      aios::json::Value reply;
      if (!call(h, t, req, reply, err)) { errs << "grain app: " << t.name << ": " << err << "\n"; rc = kFailed; break; }
      if (sub == "status") {
        aios::json::Value only = reply;
        if (!arg(1).empty()) {
          aios::json::Value keep = aios::json::Value::array();
          for (const auto& a : reply["apps"].items()) if (a["name"].str() == arg(1)) keep.push(a);
          only.set("apps", keep);
        }
        print_status(out, t, only, first);
      } else if (sub == "logs") {
        const auto& lines = reply["lines"].items();
        size_t from = 0;
        if (seen) { from = lines.size(); for (size_t i = lines.size(); i-- > 0;) { if (lines[i].str() == req["last"].str()) { from = i + 1; break; } } }
        for (size_t i = from; i < lines.size(); ++i) out << lines[i].str() << "\n";
        if (!lines.empty()) req.set("last", aios::json::Value::string(lines.back().str()));
        out << std::flush;
      } else if (sub == "versions") {
        out << "current " << reply["current"].str() << "\n";
        for (const auto& e : reply["history"].items()) out << "  " << e["at"].str() << "  " << e["version"].str() << "\n";
      } else if (sub == "settings" || sub == "secrets") {
        print_lines(out, reply["names"]);
      } else {
        print_lines(out, reply["report"]);
      }
      if (!reply["ok"].boolean_or(false)) { errs << "grain app: " << t.name << ": " << reply["error"].str() << "\n"; rc = kFailed; break; }
      if (sub != "status" && sub != "logs" && sub != "versions" && sub != "settings" && sub != "secrets" && reply["report"].size() == 0)
        out << t.name << ": " << sub << " " << arg(1) << ": done\n";
      ++seen;
      if (sub == "logs" && p.follow) { req.set("n", aios::json::Value::integer(500)); aios::hal::sleep_ms(1000); }
    } while (sub == "logs" && p.follow);
    first = false;
  }
  return rc;
}

}  // namespace aios::userland::grain::apps
