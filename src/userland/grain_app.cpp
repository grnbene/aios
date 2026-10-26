// grain apps (grain_app.hpp): bundles, manifests, a store in a directory, the host that runs apps in this process,
// and the control link. Everything goes through the host layer (hal::Host for files, time and randomness;
// hal::net for sockets; hal::Thread), so it runs wherever grain does.
#include "aios/userland/grain_app.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "aios/core/base64.hpp"
#include "aios/core/bytes.hpp"
#include "aios/core/json.hpp"
#include "aios/crypto/aes_gcm.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/net/http_stream.hpp"
#include "aios/node/secure.hpp"
#include "aios/userland/grain_webui.hpp"

namespace aios::userland::grain::apps {

namespace {

constexpr char kBundleMagic[] = "GRAINAPP1";
constexpr uint8_t kControlFrame = 1;

std::string hex_of(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string o;
  for (size_t i = 0; i < n; ++i) { o += d[p[i] >> 4]; o += d[p[i] & 15]; }
  return o;
}

bool valid_name(const std::string& n) {
  if (n.empty() || n.size() > 64) return false;
  for (char c : n) if (!std::isalnum((unsigned char)c) && c != '-' && c != '_') return false;
  return true;
}

std::string join(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  return a.back() == '/' ? a + b : a + "/" + b;
}

// make_dir is one level; this is every level of `path`
void make_dirs(hal::Host& h, const std::string& path) {
  std::string at;
  size_t i = 0;
  while (i <= path.size()) {
    const size_t s = path.find('/', i);
    const std::string part = path.substr(i, s == std::string::npos ? std::string::npos : s - i);
    at += (at.empty() && path[0] == '/' ? "/" : (at.empty() ? "" : "/")) + part;
    if (!part.empty() && !h.exists(at)) (void)h.make_dir(at);
    if (s == std::string::npos) break;
    i = s + 1;
  }
}

std::string as_text(const ByteBuffer& b) { return std::string(b.begin(), b.end()); }
ByteSpan span(const std::string& s) { return ByteSpan(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

// "2026-10-24 09:30:00" (UTC) from wall nanoseconds, without the C library's non-reentrant gmtime
std::string utc_time(uint64_t ns) {
  int64_t secs = (int64_t)(ns / 1000000000ull);
  int64_t days = secs / 86400, rem = secs % 86400;
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int64_t doe = days - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const int64_t m = mp + (mp < 10 ? 3 : -9);
  if (m <= 2) ++y;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", (int)y, (int)m, (int)d, (int)(rem / 3600), (int)(rem / 60 % 60), (int)(rem % 60));
  return buf;
}

// ---- grain values, read as config ----
const Value* field(const Value& m, const char* key) {
  if (m.type != Type::Map || !m.obj) return nullptr;
  return static_cast<MapObj*>(m.obj.get())->find(key);
}
std::string text_field(const Value& m, const char* key, const std::string& def) {
  const Value* v = field(m, key);
  return v && v->type == Type::Str ? v->str() : def;
}
int64_t int_field(const Value& m, const char* key, int64_t def) {
  const Value* v = field(m, key);
  return v && (v->type == Type::Int || v->type == Type::Real) ? v->as_int() : def;
}

}  // namespace

// ---- bundles ----

void Bundle::seal_id() {
  std::string all;
  for (const auto& [p, d] : files) { all += p; all.push_back('\0'); all += std::to_string(d.size()); all.push_back('\0'); all += d; }
  const auto h = crypto::sha256(span(all));
  id = hex_of(h.data(), 8);
}

std::string Bundle::encode() const {
  ByteBuffer out;
  Writer w(out);
  w.raw(ByteSpan(reinterpret_cast<const uint8_t*>(kBundleMagic), sizeof kBundleMagic - 1));
  w.str(name);
  w.u32((uint32_t)files.size());
  for (const auto& [p, d] : files) { w.str(p); w.str(d); }
  return as_text(out);
}

Result<Bundle> Bundle::decode(const std::string& bytes) {
  const size_t ml = sizeof kBundleMagic - 1;
  if (bytes.size() < ml || bytes.compare(0, ml, kBundleMagic) != 0) return Result<Bundle>(Status::Invalid, "not a grain app bundle");
  Reader r(span(bytes).subspan(ml));
  Bundle b;
  b.name = r.str();
  const uint32_t n = r.u32();
  for (uint32_t i = 0; i < n && r.ok(); ++i) { std::string p = r.str(); b.files[p] = r.str(); }
  if (!r.ok() || r.remaining() != 0) return Result<Bundle>(Status::Corrupt, "a damaged app bundle");
  b.seal_id();
  return b;
}

Result<Bundle> pack_dir(hal::Host& host, const std::string& dir) {
  Bundle b;
  std::vector<std::string> todo{""};
  while (!todo.empty()) {
    const std::string rel = todo.back();
    todo.pop_back();
    auto names = host.list_dir(rel.empty() ? dir : join(dir, rel));
    if (!names.ok()) return Result<Bundle>(names.error());
    for (const auto& n : *names) {
      if (n.empty() || n[0] == '.') continue;   // hidden: .git, .grain (a local run's state), editor files
      const std::string r = rel.empty() ? n : rel + "/" + n;
      auto st = host.stat(join(dir, r));
      if (!st.ok()) continue;
      if (st->is_dir) { todo.push_back(r); continue; }
      auto data = host.read_file(join(dir, r));
      if (!data.ok()) return Result<Bundle>(data.error());
      b.files[r] = as_text(*data);
    }
  }
  if (!b.files.count("app.gr")) return Result<Bundle>(Status::NotFound, dir + " has no app.gr: it is what makes a directory an app");
  b.seal_id();
  auto m = read_manifest(b);
  if (!m.ok()) return Result<Bundle>(m.error());
  b.name = m->name;
  return b;
}

Result<Manifest> read_manifest(const Bundle& b) {
  auto it = b.files.find("app.gr");
  if (it == b.files.end()) return Result<Manifest>(Status::NotFound, "the app has no app.gr");
  auto compiled = compile_source(it->second, "app.gr");
  if (!compiled.ok()) {
    std::string e = "app.gr does not compile";
    for (const auto& d : compiled.errors) e += "; line " + std::to_string(d.line) + ": " + d.message;
    return Result<Manifest>(Status::Invalid, e);
  }
  Vm vm;
  install_core_builtins(vm);
  vm.files = nullptr;
  vm.step_limit = 2000000;
  vm.packed_source = [&b](const std::string& path, std::string& source) {
    auto f = b.files.find(path);
    if (f == b.files.end()) return false;
    source = f->second;
    return true;
  };
  std::vector<std::string> printed;
  vm.sink = &printed;
  auto r = vm.run(compiled.script);
  if (!r.ok) return Result<Manifest>(Status::Invalid, "app.gr: line " + std::to_string(r.line) + ": " + r.error);
  Value* a = vm.find_global("app");
  if (!a || a->type != Type::Map) return Result<Manifest>(Status::Invalid, "app.gr must set `app` to a map: let app = {\"name\": ..., \"main\": ...}");
  Manifest m;
  m.name = text_field(*a, "name", "");
  if (!valid_name(m.name)) return Result<Manifest>(Status::Invalid, "app.gr: \"name\" is letters, digits, - and _ (got \"" + m.name + "\")");
  m.main = text_field(*a, "main", m.main);
  if (!b.files.count(m.main)) return Result<Manifest>(Status::NotFound, "app.gr names \"" + m.main + "\" as its main, and the app has no such file");
  m.port = (int)int_field(*a, "port", 0);
  m.kind = text_field(*a, "kind", m.port > 0 ? "web" : "worker");
  if (m.kind != "web" && m.kind != "worker") return Result<Manifest>(Status::Invalid, "app.gr: \"kind\" is \"web\" or \"worker\"");
  if (m.kind == "web" && (m.port <= 0 || m.port > 65535)) return Result<Manifest>(Status::Invalid, "app.gr: a web app needs a \"port\"");
  m.listen = text_field(*a, "listen", m.listen);
  if (m.listen != "local" && m.listen != "all") return Result<Manifest>(Status::Invalid, "app.gr: \"listen\" is \"local\" or \"all\"");
  m.restart = text_field(*a, "restart", m.restart);
  if (m.restart != "always" && m.restart != "on-failure" && m.restart != "never") return Result<Manifest>(Status::Invalid, "app.gr: \"restart\" is \"always\", \"on-failure\" or \"never\"");
  m.check = text_field(*a, "check", "");
  if (!m.check.empty() && !b.files.count(m.check)) return Result<Manifest>(Status::NotFound, "app.gr names \"" + m.check + "\" as its check, and the app has no such file");
  m.deployed = text_field(*a, "deployed", "");
  if (!m.deployed.empty() && !b.files.count(m.deployed)) return Result<Manifest>(Status::NotFound, "app.gr names \"" + m.deployed + "\" as its deployed hook, and the app has no such file");
  m.description = text_field(*a, "description", "");
  m.max_steps = (uint64_t)int_field(*a, "max_steps", (int64_t)m.max_steps);
  m.max_body = (size_t)int_field(*a, "max_body", (int64_t)m.max_body);
  if (const Value* s = field(*a, "settings"); s && s->type == Type::Map) {
    for (const auto& [k, v] : static_cast<MapObj*>(s->obj.get())->entries) m.settings.push_back({k, v.to_text()});
  }
  auto names = [&](const char* key, std::vector<std::string>& out) {
    if (const Value* s = field(*a, key); s && s->type == Type::List)
      for (const auto& v : s->list_items()) if (v.type == Type::Str) out.push_back(v.str());
  };
  names("secrets", m.secrets);
  names("requires", m.needs);
  if (const Value* h = field(*a, "health"); h && h->type == Type::Map) {
    m.health_path = text_field(*h, "path", m.health_path);
    m.health_timeout_s = (int)int_field(*h, "timeout", m.health_timeout_s);
    if (const Value* ok = field(*h, "ok"); ok && ok->type == Type::List)
      for (const auto& v : ok->list_items()) if (v.type == Type::Int) m.health_ok.push_back((int)v.i);
  }
  return m;
}

Result<void> compile_all(const Bundle& b) {
  std::string errs;
  for (const auto& [p, src] : b.files) {
    if (p.size() < 3 || p.compare(p.size() - 3, 3, ".gr") != 0) continue;
    auto c = compile_source(src, p);
    if (!c.ok()) for (const auto& d : c.errors) errs += (errs.empty() ? "" : "; ") + p + ":" + std::to_string(d.line) + ": " + d.message;
  }
  if (!errs.empty()) return Result<void>(Status::Invalid, errs);
  return Ok();
}

// ---- the store in a directory ----

namespace {

class DirStore final : public Store {
 public:
  DirStore(hal::Host& h, std::string home) : h_(h), home_(std::move(home)) { make_dirs(h_, join(home_, "apps")); }

  Result<void> put_version(const Bundle& b) override {
    if (!valid_name(b.name)) return Result<void>(Status::Invalid, "a bundle without a valid app name");
    const std::string dir = join(app_dir(b.name), "versions");
    make_dirs(h_, dir);
    const std::string path = join(dir, b.id + ".app");
    if (h_.exists(path)) return Ok();
    const std::string bytes = b.encode();
    return h_.write_file(path, span(bytes));
  }
  Result<Bundle> get_version(const std::string& app, const std::string& id) override {
    auto d = h_.read_file(join(join(app_dir(app), "versions"), id + ".app"));
    if (!d.ok()) return Result<Bundle>(Status::NotFound, app + " has no version " + id);
    return Bundle::decode(as_text(*d));
  }
  std::vector<std::string> versions(const std::string& app) override {
    std::vector<std::string> out;
    auto names = h_.list_dir(join(app_dir(app), "versions"));
    if (names.ok()) for (const auto& n : *names) if (n.size() > 4 && n.compare(n.size() - 4, 4, ".app") == 0) out.push_back(n.substr(0, n.size() - 4));
    return out;
  }
  std::vector<std::string> apps() override {
    std::vector<std::string> out;
    auto names = h_.list_dir(join(home_, "apps"));
    if (names.ok()) for (const auto& n : *names) if (valid_name(n) && h_.exists(join(join(join(home_, "apps"), n), "record.json"))) out.push_back(n);
    std::sort(out.begin(), out.end());
    return out;
  }
  AppRecord record(const std::string& app) override {
    AppRecord r;
    auto d = h_.read_file(join(app_dir(app), "record.json"));
    if (!d.ok()) return r;
    auto j = json::parse(as_text(*d));
    if (!j.ok()) return r;
    r.current = (*j)["current"].str();
    r.running = (*j)["running"].boolean_or(false);
    for (const auto& e : (*j)["history"].items()) r.history.push_back({e[0].str(), (uint64_t)e[1].i64()});
    return r;
  }
  Result<void> put_record(const std::string& app, const AppRecord& r) override {
    make_dirs(h_, app_dir(app));
    json::Value j = json::Value::object();
    j.set("current", json::Value::string(r.current));
    j.set("running", json::Value::boolean(r.running));
    json::Value hist = json::Value::array();
    const size_t from = r.history.size() > 50 ? r.history.size() - 50 : 0;
    for (size_t i = from; i < r.history.size(); ++i) {
      json::Value e = json::Value::array();
      e.push(json::Value::string(r.history[i].first));
      e.push(json::Value::integer((int64_t)r.history[i].second));
      hist.push(e);
    }
    j.set("history", hist);
    const std::string text = j.dump();
    return h_.write_file(join(app_dir(app), "record.json"), span(text));
  }
  std::map<std::string, std::string> settings(const std::string& app, bool secret) override {
    std::map<std::string, std::string> out;
    std::string text;
    if (secret) {
      auto d = h_.read_file(join(app_dir(app), "secrets.sealed"));
      if (!d.ok() || d->size() < 12) return out;
      crypto::GcmNonce nonce;
      std::memcpy(nonce.data(), d->data(), 12);
      ByteBuffer plain;
      if (!crypto::aes256_gcm_open(machine_key(), nonce, span(app), ByteSpan(d->data() + 12, d->size() - 12), plain)) return out;
      text = as_text(plain);
    } else {
      auto d = h_.read_file(join(app_dir(app), "settings.json"));
      if (!d.ok()) return out;
      text = as_text(*d);
    }
    auto j = json::parse(text);
    if (j.ok()) for (const auto& [k, v] : j->members()) out[k] = v.str();
    return out;
  }
  Result<void> put_settings(const std::string& app, const std::map<std::string, std::string>& values, bool secret) override {
    make_dirs(h_, app_dir(app));
    json::Value j = json::Value::object();
    for (const auto& [k, v] : values) j.set(k, json::Value::string(v));
    const std::string text = j.dump();
    if (!secret) return h_.write_file(join(app_dir(app), "settings.json"), span(text));
    crypto::GcmNonce nonce;
    h_.random(MutableByteSpan(nonce.data(), nonce.size()));
    ByteBuffer sealed = crypto::aes256_gcm_seal(machine_key(), nonce, span(app), span(text));
    ByteBuffer out(nonce.begin(), nonce.end());
    out.insert(out.end(), sealed.begin(), sealed.end());
    const std::string path = join(app_dir(app), "secrets.sealed");
    auto w = h_.write_file(path, out);
    if (w.ok()) (void)h_.set_access(path, hal::Host::FileAccess::Private);
    return w;
  }
  void append_log(const std::string& app, const std::string& line) override {
    std::lock_guard<std::mutex> lk(log_mu_);
    const std::string path = join(app_dir(app), "log");
    auto st = h_.stat(path);
    if (st.ok() && st->size > (8u << 20)) (void)h_.rename_file(path, path + ".1");   // one generation kept
    const std::string text = line + "\n";
    (void)h_.append_file_unsynced(path, span(text));
  }
  std::vector<std::string> read_log(const std::string& app, size_t last_n) override {
    std::vector<std::string> lines;
    for (const char* f : {"log.1", "log"}) {
      auto d = h_.read_file(join(app_dir(app), f));
      if (!d.ok()) continue;
      size_t at = 0;
      const std::string t = as_text(*d);
      while (at < t.size()) {
        size_t nl = t.find('\n', at);
        if (nl == std::string::npos) nl = t.size();
        lines.push_back(t.substr(at, nl - at));
        at = nl + 1;
      }
    }
    if (lines.size() > last_n) lines.erase(lines.begin(), lines.end() - (long)last_n);
    return lines;
  }
  std::string data_dir(const std::string& app) override {
    const std::string d = join(app_dir(app), "data");
    make_dirs(h_, d);
    return d;
  }
  Result<void> remove_app(const std::string& app) override {
    // The record goes (the app is no longer listed); the versions, settings and data stay on disk for an operator
    // to look at or delete: nothing here deletes what an app wrote.
    const std::string path = join(app_dir(app), "record.json");
    if (!h_.exists(path)) return Result<void>(Status::NotFound, "no app " + app);
    return h_.rename_file(path, path + ".removed");
  }

 private:
  hal::Host& h_;
  std::string home_;
  std::mutex log_mu_, key_mu_;
  crypto::Aes256Key key_{};
  bool have_key_ = false;

  std::string app_dir(const std::string& app) const { return join(join(home_, "apps"), app); }
  const crypto::Aes256Key& machine_key() {
    std::lock_guard<std::mutex> lk(key_mu_);
    if (have_key_) return key_;
    const std::string path = join(home_, "machine.key");
    auto d = h_.read_file(path);
    if (d.ok() && d->size() == key_.size()) {
      std::memcpy(key_.data(), d->data(), key_.size());
    } else {
      h_.random(MutableByteSpan(key_.data(), key_.size()));
      (void)h_.write_file(path, ByteSpan(key_.data(), key_.size()));
      (void)h_.set_access(path, hal::Host::FileAccess::Private);
    }
    have_key_ = true;
    return key_;
  }
};

// What a hosted app's file builtins see: its own files (read-only), then its data directory for relative paths
// (where what it writes goes); an absolute path is the machine's.
class AppFiles final : public Files {
 public:
  AppFiles(hal::Host& h, const Bundle& b, std::string data) : h_(h), b_(b), data_(std::move(data)) {}
  bool read(const std::string& path, std::string& out, std::string& error) override {
    auto f = b_.files.find(path);
    if (f != b_.files.end()) { out = f->second; return true; }
    auto d = h_.read_file(where(path));
    if (!d.ok()) { error = d.error().message; return false; }
    out = as_text(*d);
    return true;
  }
  bool write(const std::string& path, const std::string& data, bool append, std::string& error) override {
    if (data_.empty() && path[0] != '/') { error = "this app has no data directory"; return false; }
    auto r = append ? h_.append_file(where(path), span(data)) : h_.write_file(where(path), span(data));
    if (!r.ok()) { error = r.error().message; return false; }
    return true;
  }
  bool list(const std::string& path, std::vector<FileEntry>& out, std::string& error) override {
    auto names = h_.list_dir(where(path));
    if (!names.ok()) { error = names.error().message; return false; }
    for (const auto& n : *names) {
      FileEntry e;
      e.name = n;
      auto st = h_.stat(join(where(path), n));
      e.dir = st.ok() && st->is_dir;
      e.size = st.ok() ? (int64_t)st->size : 0;
      out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& c) { return a.name < c.name; });
    return true;
  }
  bool remove(const std::string& path, std::string& error) override {
    auto r = h_.remove(where(path));
    if (!r.ok()) { error = r.error().message; return false; }
    return true;
  }
  bool exists(const std::string& path) override { return b_.files.count(path) || h_.exists(where(path)); }
  bool make_dir(const std::string& path, bool parents, std::string& error) override {
    if (parents) { make_dirs(h_, where(path)); return true; }
    auto r = h_.make_dir(where(path));
    if (!r.ok()) { error = r.error().message; return false; }
    return true;
  }
  bool remove_dir(const std::string& path, std::string& error) override { return remove(path, error); }
  bool is_dir(const std::string& path) override { auto st = h_.stat(where(path)); return st.ok() && st->is_dir; }
  bool absolute(const std::string& path, std::string& out, std::string&) override { out = where(path); return true; }

 private:
  hal::Host& h_;
  const Bundle& b_;
  std::string data_;
  std::string where(const std::string& path) const { return !path.empty() && path[0] == '/' ? path : join(data_, path); }
};

}  // namespace

std::unique_ptr<Store> make_dir_store(hal::Host& host, const std::string& home) { return std::make_unique<DirStore>(host, home); }

Result<std::string> host_token(hal::Host& host, const std::string& home) {
  make_dirs(host, home);
  const std::string path = join(home, "host.key");
  auto d = host.read_file(path);
  if (d.ok() && d->size() >= 32) {
    std::string t = as_text(*d);
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
    return t;
  }
  uint8_t raw[24];
  host.random(MutableByteSpan(raw, sizeof raw));
  const std::string t = hex_of(raw, sizeof raw);
  auto w = host.write_file(path, span(t + "\n"));
  if (!w.ok()) return Result<std::string>(w.error());
  (void)host.set_access(path, hal::Host::FileAccess::Private);
  return t;
}

Result<std::string> inline_imports(const std::string& path, const std::function<bool(const std::string&, std::string&)>& read) {
  std::vector<std::string> seen;
  std::function<Result<std::string>(const std::string&)> expand = [&](const std::string& p) -> Result<std::string> {
    std::string src;
    if (!read(p, src)) return Result<std::string>(Status::NotFound, "no file " + p + " in the app");
    const size_t slash = p.rfind('/');
    const std::string dir = slash == std::string::npos ? "" : p.substr(0, slash + 1);
    std::string out;
    size_t at = 0;
    while (at <= src.size()) {
      size_t nl = src.find('\n', at);
      if (nl == std::string::npos) nl = src.size();
      const std::string line = src.substr(at, nl - at);
      // import "x" / import("x"), alone on its line
      size_t i = line.find_first_not_of(" \t");
      std::string target;
      if (i != std::string::npos && line.compare(i, 6, "import") == 0) {
        size_t q1 = line.find('"', i + 6), q2 = q1 == std::string::npos ? q1 : line.find('"', q1 + 1);
        if (q2 != std::string::npos) {
          bool clean = true;
          for (size_t k = i + 6; k < q1; ++k) if (line[k] != ' ' && line[k] != '(' && line[k] != '\t') clean = false;
          const size_t after = line.find_first_not_of(" \t)", q2 + 1);
          if (after != std::string::npos && line[after] != '#' && !(line[after] == '/' && after + 1 < line.size() && line[after + 1] == '/')) clean = false;
          if (clean) target = line.substr(q1 + 1, q2 - q1 - 1);
        }
      }
      if (!target.empty()) {
        // relative to the importing file; "a/../b" folded
        std::vector<std::string> parts;
        std::string joined = (target[0] == '/' ? target.substr(1) : dir + target), cur;
        for (size_t k = 0; k <= joined.size(); ++k) {
          if (k == joined.size() || joined[k] == '/') {
            if (cur == "..") { if (!parts.empty()) parts.pop_back(); }
            else if (!cur.empty() && cur != ".") parts.push_back(cur);
            cur.clear();
          } else cur += joined[k];
        }
        std::string norm;
        for (const auto& x : parts) norm += (norm.empty() ? "" : "/") + x;
        if (std::find(seen.begin(), seen.end(), norm) == seen.end()) {
          seen.push_back(norm);
          auto sub = expand(norm);
          if (!sub.ok()) return sub;
          out += "# ---- " + norm + " (imported)\n" + *sub + "\n# ---- end of " + norm + "\n";
        }
      } else {
        out += line;
        if (nl < src.size()) out += "\n";
      }
      if (nl >= src.size()) break;
      at = nl + 1;
    }
    return out;
  };
  seen.push_back(path);
  return expand(path);
}

namespace {
// app_program(path): one of the app's files with its imports put in place (inline_imports)
Value bi_app_program(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::Str) { error = "app_program(path): the path of one of the app's files"; return Value::nil(); }
  if (!vm.packed_source) { error = "app_program(): only an app's VMs have the app's files"; return Value::nil(); }
  auto r = inline_imports(args[0].str(), vm.packed_source);
  if (!r.ok()) { error = "app_program(): " + r.error().message; return Value::nil(); }
  return Value::string(*r);
}
}  // namespace

// ---- the host ----

enum class State { Starting, Running, Exited, Crashed, Failed, Stopped };
const char* state_name(State s) {
  switch (s) {
    case State::Starting: return "starting";
    case State::Running: return "running";
    case State::Exited: return "exited";
    case State::Crashed: return "crashed";
    case State::Failed: return "failed";
    case State::Stopped: return "stopped";
  }
  return "?";
}

struct Host::Instance {
  std::string app, version;
  Manifest m;
  Bundle bundle;
  std::shared_ptr<VmSettings> settings = std::make_shared<VmSettings>();
  std::shared_ptr<std::atomic<bool>> stop = std::make_shared<std::atomic<bool>>(false);
  std::unique_ptr<AppFiles> files;
  std::function<void(Vm&)> setup;          // every VM of this instance
  std::shared_ptr<WebUi> ui;
  std::unique_ptr<hal::Listener> listener;
  hal::Thread accept, worker;
  std::atomic<bool> worker_done{false};
  std::atomic<Vm*> vm{nullptr};
  int port = 0;                            // where it listens now (the manifest's, or a spare one while it is tried)
  std::atomic<bool> accept_stop{false};    // ends the accept loop alone (moving to another port), not the app
  std::atomic<int> state{(int)State::Starting};
  std::mutex mu;
  std::string last_error;
  uint64_t since_ns = 0;
  std::atomic<uint64_t> requests{0}, failures{0};
  // the watcher's restart bookkeeping
  uint64_t next_try_ns = 0, restarts = 0;
  std::vector<uint64_t> recent;            // when it was restarted, the last few minutes
  int backoff_s = 1;

  State st() const { return (State)state.load(); }
  void fail(State s, const std::string& why) {
    std::lock_guard<std::mutex> lk(mu);
    last_error = why;
    state = (int)s;
  }
  std::string error() { std::lock_guard<std::mutex> lk(mu); return last_error; }
};

namespace {

// A client's address without its port ("1.2.3.4:5678" -> "1.2.3.4", "[::1]:80" -> "::1")
std::string peer_address(const std::string& p) {
  if (!p.empty() && p[0] == '[') { const size_t e = p.find(']'); return e == std::string::npos ? p : p.substr(1, e - 1); }
  const size_t c = p.rfind(':');
  return c != std::string::npos && p.find(':') == c ? p.substr(0, c) : p;
}

const char* reason_of(int s) {
  switch (s) {
    case 200: return "OK"; case 201: return "Created"; case 204: return "No Content"; case 301: return "Moved Permanently";
    case 302: return "Found"; case 304: return "Not Modified"; case 400: return "Bad Request"; case 401: return "Unauthorized";
    case 403: return "Forbidden"; case 404: return "Not Found"; case 405: return "Method Not Allowed"; case 413: return "Payload Too Large";
    case 429: return "Too Many Requests"; case 503: return "Service Unavailable";
    default: return s < 400 ? "OK" : "Error";
  }
}

bool send_text(hal::Connection& c, const std::string& s) { return c.send_all(span(s)); }

// One HTTP request on a connection, to the app's WebUi, the way `grain serve` answers it.
void answer(std::shared_ptr<hal::Connection> conn, std::shared_ptr<WebUi> ui, Host::Instance* inst, size_t max_body) {
  (void)inst;
  net::ConnReader reader(*conn);
  auto head = net::read_request_head(reader, 30000);
  if (!head.ok()) { conn->close(); return; }
  if (head->content_length > (int64_t)max_body) {
    send_text(*conn, "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    conn->close();
    return;
  }
  WebUi::Request req;
  req.method = head->method;
  const std::string& target = head->target;
  const size_t q = target.find('?');
  req.path = target.substr(0, q);
  req.query = q == std::string::npos ? "" : target.substr(q + 1);
  for (const auto& hd : head->headers) {
    std::string name = hd.name;
    for (char& c : name) c = (char)std::tolower((unsigned char)c);
    std::string value = hd.value;
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
    if (name == "cookie") req.cookie = value;
    else if (name == "x-grain-ui") req.xhr = value == "1";
    else if (name == "accept") {
      req.tree = value.find("application/x-grain-ui") != std::string::npos;
      req.stream = value.find("text/event-stream") != std::string::npos;
    }
    req.headers.push_back({name, value});
  }
  if (head->content_length > 0 || head->chunked) {
    net::RequestBodyReader body(reader, *head);
    while (!body.done()) {
      auto chunk = body.next(1 << 16, 30000);
      if (!chunk.ok()) { conn->close(); return; }
      req.body.append(chunk->begin(), chunk->end());
      if (req.body.size() > max_body) {
        send_text(*conn, "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        conn->close();
        return;
      }
      if (chunk->empty()) break;
    }
  }
  req.remote = peer_address(conn->peer());
  if (inst) inst->requests++;
  if (req.stream && req.method == "GET") {
    WebUi::Stream stream;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string frame, error;
    if (ui->open_stream(req, stream, headers, frame, error)) {
      std::string out = "HTTP/1.1 200 OK\r\n";
      for (const auto& h : headers) out += h.first + ": " + h.second + "\r\n";
      out += "Connection: close\r\n\r\n" + frame;
      bool alive = send_text(*conn, out);
      while (alive && !(inst && inst->stop->load()) && ui->next_frame(stream, 15000, frame))
        alive = send_text(*conn, frame.empty() ? std::string(": keepalive\n\n") : frame);
      ui->close_stream(stream);
      conn->close();
      return;
    }
  }

  bool headers_sent = false;
  auto on_headers = [&](int status, const std::vector<std::pair<std::string, std::string>>& headers) -> bool {
    if (headers_sent) return true;
    headers_sent = true;
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + reason_of(status) + "\r\n";
    for (const auto& h : headers) out += h.first + ": " + h.second + "\r\n";
    out += "Connection: close\r\n\r\n";
    return send_text(*conn, out);
  };
  auto on_chunk = [&](const std::string& chunk) -> bool {
    if (!headers_sent) {
      on_headers(200, {{"Content-Type", "text/event-stream; charset=utf-8"}, {"Cache-Control", "no-cache"}});
    }
    return send_text(*conn, chunk);
  };

  WebUi::Response res;
  if (req.path == "/favicon.ico") res.status = 204;
  else res = ui->handle(req, on_headers, on_chunk);

  if (headers_sent) {
    conn->close();
    return;
  }

  if (inst && res.status >= 500) inst->failures++;
  std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " + reason_of(res.status) + "\r\n";
  for (const auto& h : res.headers) out += h.first + ": " + h.second + "\r\n";
  out += "Content-Length: " + std::to_string(res.body.size()) + "\r\nConnection: close\r\n\r\n";
  if (req.method != "HEAD") out += res.body;
  send_text(*conn, out);
  conn->close();
}

// GET http://127.0.0.1:<port><path>: its status, or 0 when nothing answered
int http_status(int port, const std::string& path, int timeout_ms) {
  auto c = hal::net_connect("127.0.0.1", (uint16_t)port, timeout_ms);
  if (!c.ok()) return 0;
  auto conn = std::move(*c);
  if (!send_text(*conn, "GET " + path + " HTTP/1.0\r\nHost: 127.0.0.1\r\nUser-Agent: grain-app-health\r\n\r\n")) return 0;
  std::string got;
  uint8_t buf[512];
  while (got.find("\r\n") == std::string::npos && got.size() < 4096) {
    const long n = conn->recv_some(MutableByteSpan(buf, sizeof buf), timeout_ms);
    if (n <= 0) break;
    got.append(reinterpret_cast<const char*>(buf), (size_t)n);
  }
  conn->close();
  const size_t sp = got.find(' ');
  return sp == std::string::npos ? 0 : std::atoi(got.c_str() + sp + 1);
}

}  // namespace

Host::Host(hal::Host& host, Store& store, Options opt) : host_(host), store_(store), opt_(std::move(opt)) {
  if (std::find(opt_.capabilities.begin(), opt_.capabilities.end(), "grain") == opt_.capabilities.end()) opt_.capabilities.push_back("grain");
}

Host::~Host() { shutdown(); }

void Host::log(const std::string& app, const std::string& line) { store_.append_log(app, utc_time(host_.wall_ns()) + "  " + line); }

void Host::start_all() {
  {
    std::lock_guard<std::mutex> ops(ops_mu_);
    for (const auto& app : store_.apps()) {
      const AppRecord r = store_.record(app);
      if (!r.running || r.current.empty()) continue;
      auto s = launch_locked(app, r.current);
      if (!s.ok()) log(app, "[host] did not start: " + s.error().message);
    }
  }
  if (!watcher_.joinable()) watcher_ = hal::Thread([this] { watch(); });
}

void Host::shutdown() {
  if (stopping_.exchange(true)) return;
  if (watcher_.joinable()) watcher_.join();
  std::map<std::string, std::shared_ptr<Instance>> all;
  {
    std::lock_guard<std::mutex> lk(mu_);
    all.swap(running_);
  }
  for (auto& [n, in] : all) halt(in);
}

void Host::start_accepting(const std::shared_ptr<Instance>& in) {
  Instance* raw = in.get();
  std::shared_ptr<WebUi> ui = in->ui;
  const size_t max_body = in->m.max_body;
  in->accept = hal::Thread([raw, ui, max_body] {
    while (!raw->stop->load() && !raw->accept_stop.load()) {
      auto c = raw->listener->accept(100);   // (short: a hand-over waits for this loop to notice)
      if (!c.ok()) continue;
      std::shared_ptr<hal::Connection> conn(std::move(*c));
      hal::Thread t([conn, ui, raw, max_body] { answer(conn, ui, raw, max_body); });
      if (t.joinable()) t.detach(); else conn->close();
    }
  });
}

// The version before gives the new one its listening socket: what connects meanwhile waits in the socket's queue
// and is answered by the new one - no gap in which the port refuses.
void Host::hand_over(const std::shared_ptr<Instance>& from, const std::shared_ptr<Instance>& to) {
  from->accept_stop = true;
  if (from->accept.joinable()) from->accept.join();
  to->accept_stop = true;
  if (to->listener) to->listener->close();
  if (to->accept.joinable()) to->accept.join();
  to->accept_stop = false;
  to->listener = std::move(from->listener);
  to->port = from->port;
  start_accepting(to);
}

// A version tried on a spare port takes the app's own: its listener moves, its sessions and state stay.
Result<void> Host::rebind(const std::shared_ptr<Instance>& in, int port) {
  in->accept_stop = true;
  if (in->listener) in->listener->close();
  if (in->accept.joinable()) in->accept.join();
  in->accept_stop = false;
  Result<std::unique_ptr<hal::Listener>> l = Result<std::unique_ptr<hal::Listener>>(Status::Busy, "");
  for (int i = 0; i < 40; ++i) {   // the version before has just let it go
    l = hal::net_listen((uint16_t)port, in->m.listen == "local");
    if (l.ok()) break;
    hal::sleep_ms(50);
  }
  if (!l.ok()) return Result<void>(l.error());
  in->listener = std::move(*l);
  in->port = port;
  start_accepting(in);
  return Ok();
}

Result<void> Host::launch_locked(const std::string& app, const std::string& version, int port_override,
                                 std::shared_ptr<Instance>* made, bool keep) {
  auto b = store_.get_version(app, version);
  if (!b.ok()) return Result<void>(b.error());
  auto m = read_manifest(*b);
  if (!m.ok()) return Result<void>(m.error());
  for (const auto& need : m->needs)
    if (std::find(opt_.capabilities.begin(), opt_.capabilities.end(), need) == opt_.capabilities.end())
      return Result<void>(Status::Unsupported, app + " requires \"" + need + "\", which this host does not have");
  auto in = std::make_shared<Instance>();
  in->app = app;
  in->version = version;
  in->m = *m;
  in->bundle = std::move(*b);
  {   // the manifest's settings, then this machine's, then its secrets
    std::lock_guard<std::mutex> lk(in->settings->mu);
    for (const auto& [k, v] : in->m.settings) in->settings->values[k] = v;
    for (const auto& [k, v] : store_.settings(app, false)) in->settings->values[k] = v;
    for (const auto& [k, v] : store_.settings(app, true)) in->settings->values[k] = v;
    in->settings->values["GRAIN_APP"] = app;
    in->settings->values["GRAIN_APP_VERSION"] = version;
  }
  for (const auto& s : in->m.secrets)
    if (!in->settings->values.count(s)) log(app, "[host] warning: secret " + s + " is not set on this machine (`grain app secret " + app + " " + s + "`)");
  in->files = std::make_unique<AppFiles>(host_, in->bundle, store_.data_dir(app));
  Instance* raw = in.get();
  auto setup_vm = opt_.setup_vm;
  in->setup = [raw, setup_vm, this, app](Vm& vm) {
    if (setup_vm) setup_vm(vm);
    vm.files = raw->files.get();
    vm.base_dir = "";
    vm.packed_source = [raw](const std::string& path, std::string& source) {
      auto f = raw->bundle.files.find(path);
      if (f == raw->bundle.files.end()) return false;
      source = f->second;
      return true;
    };
    vm.settings = raw->settings;
    vm.stop_flag = raw->stop;
    vm.define_native("app_program", 1, 1, bi_app_program);
    vm.on_print = [this, app](const std::string& line) { log(app, line); };
  };
  in->since_ns = host_.wall_ns();
  log(app, "[host] starting version " + version + " (" + in->m.kind + (in->m.kind == "web" ? ", port " + std::to_string(in->m.port) : "") + ")");
  if (in->m.kind == "web") {
    WebUi::Host wh;
    wh.origin = app + ":" + in->m.main;
    wh.cookie_name = "grain_" + app;
    wh.max_steps = in->m.max_steps;
    const std::string main = in->m.main;
    wh.load_source = [raw, main, version](std::string& source, std::string& v, std::string&) { source = raw->bundle.files.at(main); v = version; return true; };
    wh.current_version = [version](std::string& v, std::string&) { v = version; return true; };
    wh.setup = [raw](Vm& vm) -> std::shared_ptr<void> { raw->setup(vm); return nullptr; };
    in->ui = std::make_shared<WebUi>(std::move(wh));
    in->ui->log = [this, app](const std::string& msg) { log(app, "[web] " + msg); };
    const int want = port_override >= 0 ? port_override : in->m.port;
    auto l = hal::net_listen((uint16_t)want, port_override == 0 ? true : in->m.listen == "local");
    if (!l.ok()) {
      in->fail(State::Failed, "cannot listen on port " + std::to_string(want) + ": " + l.error().message);
      log(app, "[host] " + in->error());
      if (made) *made = in;
      if (keep) { std::lock_guard<std::mutex> lk(mu_); running_[app] = in; }
      return Result<void>(Status::Busy, in->error());
    }
    in->listener = std::move(*l);
    in->port = (int)in->listener->port();
    in->state = (int)State::Running;
    start_accepting(in);
  } else {
    in->state = (int)State::Running;
    in->worker = hal::Thread([raw, this, app] {
      auto compiled = compile_source(raw->bundle.files.at(raw->m.main), raw->m.main);
      if (!compiled.ok()) {
        std::string e = raw->m.main + " does not compile";
        for (const auto& d : compiled.errors) e += "; line " + std::to_string(d.line) + ": " + d.message;
        raw->fail(State::Crashed, e);
        log(app, "[host] " + e);
        return;
      }
      Vm vm;
      raw->setup(vm);
      vm.step_limit = 0;
      raw->vm = &vm;
      auto r = vm.run(compiled.script);
      raw->vm = nullptr;
      raw->worker_done.store(true);
      if (raw->stop->load()) { raw->state = (int)State::Stopped; return; }
      if (r.ok) { raw->state = (int)State::Exited; log(app, "[host] " + raw->m.main + " finished"); return; }
      std::string e = raw->m.main + ":" + std::to_string(r.line) + ": " + r.error;
      for (const auto& t : r.trace) e += "\n  from " + t;
      raw->fail(State::Crashed, e);
      log(app, "[host] crashed: " + e);
    });
  }
  if (made) *made = in;
  if (keep) { std::lock_guard<std::mutex> lk(mu_); running_[app] = in; }
  return Ok();
}

void Host::halt(const std::shared_ptr<Instance>& in) {
  if (!in) return;
  in->stop->store(true);
  if (in->listener) in->listener->close();
  if (in->accept.joinable()) in->accept.join();
  if (in->worker.joinable()) {
    for (int i = 0; i < 30 && !in->worker_done.load(); ++i) {
      aios::hal::sleep_ms(100);
    }
  }
  if (!in->worker_done.load()) {
    if (Vm* vm = in->vm.load()) vm->step_limit = vm->steps + 1;   // a loop that never sleeps ends at its next instruction
  }
  if (in->worker.joinable()) in->worker.join();
  in->ui.reset();
  if (in->st() != State::Failed && in->st() != State::Crashed) in->state = (int)State::Stopped;
  log(in->app, "[host] stopped version " + in->version);
}

Result<void> Host::wait_healthy(const std::shared_ptr<Instance>& in, int timeout_s, std::string& why) {
  const uint64_t until = host_.monotonic_ns() + (uint64_t)timeout_s * 1000000000ull;
  int last = 0;
  uint64_t bad_since = 0;   // when it began answering, and answering wrong: five seconds of that is an answer
  while (host_.monotonic_ns() < until) {
    const State s = in->st();
    if (s == State::Failed || s == State::Crashed) { why = in->error(); return Result<void>(Status::Invalid, why); }
    if (in->m.kind == "worker") {
      if (s == State::Exited) return Ok();   // a worker that did its job and finished
      if (host_.monotonic_ns() + (uint64_t)(timeout_s - 2) * 1000000000ull >= until) return Ok();   // up for 2 s
    } else {
      last = http_status(in->port, in->m.health_path, 5000);
      const bool ok = last > 0 && (in->m.health_ok.empty() ? last < 500
                                   : std::find(in->m.health_ok.begin(), in->m.health_ok.end(), last) != in->m.health_ok.end());
      if (ok) return Ok();
      if (last > 0) {
        if (!bad_since) bad_since = host_.monotonic_ns();
        else if (host_.monotonic_ns() - bad_since > 5000000000ull) break;
      }
    }
    hal::sleep_ms(250);
  }
  why = in->m.kind == "web" ? "GET " + in->m.health_path + " on port " + std::to_string(in->port) + " answered " +
                                (last ? std::to_string(last) + " for 5 s" : std::string("nothing for ") + std::to_string(timeout_s) + " s")
                            : "did not stay up";
  return Result<void>(Status::Invalid, why);
}

Result<void> Host::run_check(const std::shared_ptr<Instance>& in, std::string& why) {
  if (in->m.check.empty()) return Ok();
  return run_hook(in, in->m.check, "check", why);
}

// file's fn_name(app), in a VM of the instance: true (or, for a deployed hook, nothing) is success
Result<void> Host::run_hook(const std::shared_ptr<Instance>& in, const std::string& file, const char* fn_name, std::string& why,
                            std::vector<std::string>* said) {
  auto compiled = compile_source(in->bundle.files.at(file), file);
  if (!compiled.ok()) { why = file + " does not compile"; return Result<void>(Status::Invalid, why); }
  Vm vm;
  in->setup(vm);
  vm.step_limit = 0;
  if (said) {
    auto to_log = vm.on_print;
    vm.on_print = [to_log, said](const std::string& line) { said->push_back(line); if (to_log) to_log(line); };
  }
  auto r = vm.run(compiled.script);
  if (!r.ok) { why = file + ":" + std::to_string(r.line) + ": " + r.error; return Result<void>(Status::Invalid, why); }
  Value* fn = vm.find_global(fn_name);
  if (!fn || fn->type != Type::Fn) { why = file + " must define " + fn_name + "(app)"; return Result<void>(Status::Invalid, why); }
  auto info = std::make_shared<MapObj>();
  info->put("name", Value::string(in->app));
  info->put("version", Value::string(in->version));
  info->put("port", Value::integer(in->port));
  info->put("url", Value::string(in->port ? "http://127.0.0.1:" + std::to_string(in->port) : ""));
  std::string err;
  Value out = vm.call(*fn, {Value::object(Type::Map, info)}, err);
  const std::string who = std::string(fn_name) + "()";
  if (!err.empty()) { why = who + ": " + err; return Result<void>(Status::Invalid, why); }
  if (out.type == Type::Bool && out.b) return Ok();
  if (out.type == Type::List && out.list_items().empty()) return Ok();
  if (out.type == Type::Nil && std::string(fn_name) != "check") return Ok();
  why = who + ": " + (out.type == Type::Nil ? std::string("returned nothing (true means it passed)") : out.to_text());
  return Result<void>(Status::Invalid, why);
}

Result<std::string> Host::deploy(const Bundle& b, std::vector<std::string>& report) {
  auto m = read_manifest(b);
  if (!m.ok()) return Result<std::string>(m.error());
  if (auto c = compile_all(b); !c.ok()) return Result<std::string>(c.error());
  for (const auto& need : m->needs)
    if (std::find(opt_.capabilities.begin(), opt_.capabilities.end(), need) == opt_.capabilities.end())
      return Result<std::string>(Status::Unsupported, m->name + " requires \"" + need + "\", which this host does not have (it has: " +
                                 [&] { std::string s; for (const auto& c : opt_.capabilities) s += (s.empty() ? "" : ", ") + c; return s; }() + ")");
  std::lock_guard<std::mutex> ops(ops_mu_);
  Bundle named = b;
  named.name = m->name;
  if (auto p = store_.put_version(named); !p.ok()) return Result<std::string>(p.error());
  AppRecord rec = store_.record(m->name);
  const std::string prev = rec.current;
  const bool was_running = rec.running;
  report.push_back(m->name + ": version " + b.id + (prev.empty() ? " (first deploy)" : prev != b.id ? " (was " + prev + ")"
                                                   : was_running ? " (the version already running, deployed again)" : " (again)"));
  std::shared_ptr<Instance> old;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (auto it = running_.find(m->name); it != running_.end()) old = it->second;
  }
  // A web app with a version answering: the new one is tried beside it, on a spare port - started, health-asked
  // and checked there while the old one goes on serving - and takes the port only once it passed. A version
  // that fails is gone before a single client saw it.
  if (old && old->st() == State::Running && old->m.kind == "web" && m->kind == "web" && old->m.port == m->port) {
    log(m->name, "[host] trying version " + b.id + " beside " + old->version);
    std::shared_ptr<Instance> trial;
    std::string why;
    auto launched = launch_locked(m->name, b.id, 0, &trial, false);
    bool ok = launched.ok() && trial;
    if (!ok) why = launched.ok() ? "it did not start" : launched.error().message;
    if (ok) { ok = wait_healthy(trial, m->health_timeout_s, why).ok(); if (ok) report.push_back("  started beside " + old->version + " (port " + std::to_string(trial->port) + "); healthy"); }
    if (ok) { ok = run_check(trial, why).ok(); if (ok && !m->check.empty()) report.push_back("  " + m->check + ": passed"); }
    if (!ok) {
      report.push_back("  FAILED: " + why);
      log(m->name, "[host] version " + b.id + " failed beside " + old->version + ": " + why);
      for (const auto& l : store_.read_log(m->name, 8)) report.push_back("    | " + l);
      if (trial) halt(trial);
      report.push_back("  " + old->version + " kept: it never stopped serving");
      return Result<std::string>(Status::Invalid, m->name + " version " + b.id + " failed: " + why);
    }
    // the switch: the old one lets the port go, the new one takes it
    {
      std::lock_guard<std::mutex> lk(mu_);
      running_.erase(m->name);
    }
    hand_over(old, trial);
    halt(old);
    {
      std::lock_guard<std::mutex> lk(mu_);
      running_[m->name] = trial;
    }
    rec.current = b.id;
    rec.running = true;
    rec.history.push_back({b.id, host_.wall_ns()});
    (void)store_.put_record(m->name, rec);
    report.push_back("  serving on port " + std::to_string(m->port));
    log(m->name, "[host] version " + b.id + " took port " + std::to_string(m->port) + " from " + old->version);
    if (!m->deployed.empty()) {
      std::vector<std::string> said;
      ok = run_hook(trial, m->deployed, "deployed", why, &said).ok();
      for (const auto& l : said) report.push_back("    " + l);
      if (ok) report.push_back("  " + m->deployed + ": done");
    }
    if (ok) { report.push_back("  running"); log(m->name, "[host] version " + b.id + " is running"); return b.id; }
    // its hook failed: the version before comes back (it was running a moment ago), its hook run again
    report.push_back("  FAILED: " + why);
    log(m->name, "[host] version " + b.id + " failed: " + why);
    {
      std::lock_guard<std::mutex> lk(mu_);
      running_.erase(m->name);
    }
    halt(trial);
    rec.current = prev;
    rec.history.push_back({prev, host_.wall_ns()});
    (void)store_.put_record(m->name, rec);
    std::shared_ptr<Instance> pin;
    auto back = launch_locked(m->name, prev, -1, &pin);
    std::string pwhy;
    bool healthy = back.ok() && pin && wait_healthy(pin, m->health_timeout_s, pwhy).ok();
    if (healthy && prev != b.id && !pin->m.deployed.empty()) {
      std::vector<std::string> said;
      healthy = run_hook(pin, pin->m.deployed, "deployed", pwhy, &said).ok();
      for (const auto& l : said) report.push_back("    " + l);
      if (healthy) report.push_back("  " + pin->m.deployed + " of " + prev + ": done again");
    }
    report.push_back(healthy ? "  rolled back to " + prev + ": running" : "  rolled back to " + prev + ", which is not healthy either: " + pwhy);
    return Result<std::string>(Status::Invalid, m->name + " version " + b.id + " failed: " + why);
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    running_.erase(m->name);
  }
  if (old) halt(old);
  rec.current = b.id;
  rec.running = true;
  rec.history.push_back({b.id, host_.wall_ns()});
  (void)store_.put_record(m->name, rec);
  log(m->name, "[host] deploying version " + b.id);
  std::string why;
  auto launched = launch_locked(m->name, b.id);
  std::shared_ptr<Instance> in;
  { std::lock_guard<std::mutex> lk(mu_); in = running_[m->name]; }
  bool ok = launched.ok() && in;
  if (!ok) why = launched.ok() ? "it did not start" : launched.error().message;
  if (ok) { auto h = wait_healthy(in, m->health_timeout_s, why); ok = h.ok(); if (ok) report.push_back("  started; healthy"); }
  if (ok) { auto c = run_check(in, why); ok = c.ok(); if (ok && !m->check.empty()) report.push_back("  " + m->check + ": passed"); }
  if (ok && !m->deployed.empty()) {
    std::vector<std::string> said;
    auto d = run_hook(in, m->deployed, "deployed", why, &said);
    ok = d.ok();
    for (const auto& l : said) report.push_back("    " + l);   // what the hook said, in the deploy's report
    if (ok) report.push_back("  " + m->deployed + ": done");
  }
  if (ok) { report.push_back("  running"); log(m->name, "[host] version " + b.id + " is running"); return b.id; }
  report.push_back("  FAILED: " + why);
  log(m->name, "[host] version " + b.id + " failed: " + why);
  for (const auto& l : store_.read_log(m->name, 8)) report.push_back("    | " + l);
  {
    std::lock_guard<std::mutex> lk(mu_);
    running_.erase(m->name);
  }
  if (in) halt(in);
  if (!prev.empty() && prev == b.id && was_running) {
    // the version that was running, deployed again, failed this time (its hook, say): it is started again as it
    // was - what its hook set up before is still there, since a failed step leaves things as they were
    std::string pwhy;
    auto back = launch_locked(m->name, prev);
    std::shared_ptr<Instance> pin;
    { std::lock_guard<std::mutex> lk(mu_); pin = running_[m->name]; }
    const bool healthy = back.ok() && pin && wait_healthy(pin, m->health_timeout_s, pwhy).ok();
    report.push_back(healthy ? "  " + prev + " running again, as before" : "  " + prev + " started again, and is not healthy: " + (back.ok() ? pwhy : back.error().message));
    log(m->name, "[host] " + prev + " started again after the failed redeploy");
  } else if (!prev.empty() && prev != b.id) {
    rec.current = prev;
    rec.history.push_back({prev, host_.wall_ns()});
    (void)store_.put_record(m->name, rec);
    std::string pwhy;
    auto back = launch_locked(m->name, prev);
    std::shared_ptr<Instance> pin;
    { std::lock_guard<std::mutex> lk(mu_); pin = running_[m->name]; }
    bool healthy = back.ok() && pin && wait_healthy(pin, m->health_timeout_s, pwhy).ok();
    if (healthy && !pin->m.deployed.empty()) {   // what the version before set up when it was deployed, set up again
      std::string hwhy;
      std::vector<std::string> said;
      const bool hook_ok = run_hook(pin, pin->m.deployed, "deployed", hwhy, &said).ok();
      for (const auto& l : said) report.push_back("    " + l);
      if (!hook_ok) { healthy = false; pwhy = hwhy; }
      else report.push_back("  " + pin->m.deployed + " of " + prev + ": done again");
    }
    report.push_back(healthy ? "  rolled back to " + prev + ": running" : "  rolled back to " + prev + ", which is not healthy either: " + (back.ok() ? pwhy : back.error().message));
    log(m->name, "[host] rolled back to " + prev);
  } else {
    rec.running = false;
    (void)store_.put_record(m->name, rec);
    report.push_back("  nothing to roll back to: the app is stopped");
  }
  return Result<std::string>(Status::Invalid, m->name + " version " + b.id + " failed: " + why);
}

Result<void> Host::start(const std::string& app) {
  std::lock_guard<std::mutex> ops(ops_mu_);
  AppRecord rec = store_.record(app);
  if (rec.current.empty()) return Result<void>(Status::NotFound, "no app " + app + " on this machine (deploy it first)");
  rec.running = true;
  (void)store_.put_record(app, rec);
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = running_.find(app);
    if (it != running_.end() && it->second->st() == State::Running) return Ok();
    if (it != running_.end()) running_.erase(it);
  }
  return launch_locked(app, rec.current);
}

Result<void> Host::stop(const std::string& app) {
  std::lock_guard<std::mutex> ops(ops_mu_);
  AppRecord rec = store_.record(app);
  if (rec.current.empty()) return Result<void>(Status::NotFound, "no app " + app + " on this machine");
  rec.running = false;
  (void)store_.put_record(app, rec);
  std::shared_ptr<Instance> in;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = running_.find(app);
    if (it != running_.end()) { in = it->second; running_.erase(it); }
  }
  halt(in);
  return Ok();
}

Result<void> Host::restart(const std::string& app) {
  if (auto s = stop(app); !s.ok()) return s;
  return start(app);
}

Result<std::string> Host::rollback(const std::string& app, std::vector<std::string>& report) {
  AppRecord rec = store_.record(app);
  if (rec.current.empty()) return Result<std::string>(Status::NotFound, "no app " + app + " on this machine");
  std::string prev;
  for (auto it = rec.history.rbegin(); it != rec.history.rend(); ++it) if (it->first != rec.current) { prev = it->first; break; }
  if (prev.empty()) return Result<std::string>(Status::NotFound, app + " has no earlier version on this machine");
  auto b = store_.get_version(app, prev);
  if (!b.ok()) return Result<std::string>(b.error());
  b->name = app;
  report.push_back(app + ": back to " + prev);
  return deploy(*b, report);
}

Result<void> Host::remove(const std::string& app) {
  if (auto s = stop(app); !s.ok()) return s;
  return store_.remove_app(app);
}

std::vector<AppStatus> Host::status() {
  std::vector<AppStatus> out;
  for (const auto& app : store_.apps()) {
    const AppRecord rec = store_.record(app);
    AppStatus s;
    s.name = app;
    s.version = rec.current;
    std::shared_ptr<Instance> in;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto it = running_.find(app);
      if (it != running_.end()) in = it->second;
    }
    if (in) {
      s.state = state_name(in->st());
      s.version = in->version;
      s.last_error = in->error();
      s.port = in->port ? in->port : in->m.port;
      s.kind = in->m.kind;
      s.since_ns = in->since_ns;
      s.restarts = in->restarts;
      s.requests = in->requests.load();
      s.failures = in->failures.load();
    } else {
      s.state = rec.running ? "starting" : "stopped";
    }
    out.push_back(s);
  }
  return out;
}

std::vector<std::string> Host::logs(const std::string& app, size_t last_n) { return store_.read_log(app, last_n); }

Result<void> Host::set_settings(const std::string& app, const std::map<std::string, std::string>& values, bool secret,
                                const std::vector<std::string>& unset) {
  if (!valid_name(app)) return Result<void>(Status::Invalid, "not an app name: " + app);
  auto cur = store_.settings(app, secret);
  for (const auto& [k, v] : values) cur[k] = v;
  for (const auto& k : unset) cur.erase(k);
  auto r = store_.put_settings(app, cur, secret);
  if (r.ok()) log(app, std::string("[host] ") + (secret ? "secrets" : "settings") + " changed: restart to use them");
  return r;
}

std::vector<std::string> Host::settings_names(const std::string& app, bool secret) {
  std::vector<std::string> out;
  for (const auto& [k, v] : store_.settings(app, secret)) out.push_back(secret ? k : k + "=" + v);
  return out;
}

void Host::watch() {
  while (!stopping_.load()) {
    for (int i = 0; i < 10 && !stopping_.load(); ++i) hal::sleep_ms(100);
    if (stopping_.load()) break;
    std::unique_lock<std::mutex> ops(ops_mu_, std::try_to_lock);
    if (!ops.owns_lock()) continue;   // a deploy or a stop is under way: it decides
    for (const auto& app : store_.apps()) {
      const AppRecord rec = store_.record(app);
      if (!rec.running || rec.current.empty()) continue;
      std::shared_ptr<Instance> in;
      {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = running_.find(app);
        if (it != running_.end()) in = it->second;
      }
      const State s = in ? in->st() : State::Stopped;
      if (s == State::Running || s == State::Starting) continue;
      const std::string policy = in ? in->m.restart : "always";
      if (s == State::Exited && policy != "always") continue;
      if (policy == "never" && in) continue;
      const uint64_t now = host_.monotonic_ns();
      if (in && now < in->next_try_ns) continue;
      // at most ten restarts in five minutes; then it waits for an operator
      std::vector<uint64_t> recent = in ? in->recent : std::vector<uint64_t>{};
      recent.erase(std::remove_if(recent.begin(), recent.end(), [&](uint64_t t) { return now - t > 300000000000ull; }), recent.end());
      if (recent.size() >= 10) {
        if (s != State::Failed) { in->fail(State::Failed, "restarted 10 times in 5 minutes: " + in->error()); log(app, "[host] gave up restarting: `grain app start` or a deploy tries again"); }
        continue;
      }
      const uint64_t restarts = in ? in->restarts + 1 : 0;
      const int backoff = in ? std::min(in->backoff_s * 2, 30) : 1;
      if (in) { halt(in); std::lock_guard<std::mutex> lk(mu_); running_.erase(app); }
      log(app, "[host] restarting (" + std::string(state_name(s)) + ")");
      (void)launch_locked(app, rec.current);
      std::lock_guard<std::mutex> lk(mu_);
      if (auto it = running_.find(app); it != running_.end()) {
        it->second->restarts = restarts;
        it->second->backoff_s = backoff;
        recent.push_back(now);
        it->second->recent = recent;
        it->second->next_try_ns = now + (uint64_t)backoff * 1000000000ull;
      }
    }
  }
}

std::string Host::control(const std::string& request_json) {
  json::Value out = json::Value::object();
  auto fail = [&](const std::string& e) { out.set("ok", json::Value::boolean(false)); out.set("error", json::Value::string(e)); return out.dump(); };
  auto req = json::parse(request_json);
  if (!req.ok()) return fail("not JSON: " + req.error().message);
  const std::string op = (*req)["op"].str(), app = (*req)["app"].str();
  auto lines = [](const std::vector<std::string>& v) { json::Value a = json::Value::array(); for (const auto& s : v) a.push(json::Value::string(s)); return a; };
  out.set("ok", json::Value::boolean(true));
  out.set("machine", json::Value::string(opt_.machine));
  if (op == "status") {
    json::Value arr = json::Value::array();
    for (const auto& s : status()) {
      json::Value o = json::Value::object();
      o.set("name", json::Value::string(s.name));
      o.set("state", json::Value::string(s.state));
      o.set("version", json::Value::string(s.version));
      o.set("kind", json::Value::string(s.kind));
      o.set("port", json::Value::integer(s.port));
      o.set("since", json::Value::string(s.since_ns ? utc_time(s.since_ns) : ""));
      o.set("uptime_s", json::Value::integer(s.since_ns ? (int64_t)((host_.wall_ns() - s.since_ns) / 1000000000ull) : 0));
      o.set("restarts", json::Value::integer((int64_t)s.restarts));
      o.set("requests", json::Value::integer((int64_t)s.requests));
      o.set("failures", json::Value::integer((int64_t)s.failures));
      o.set("error", json::Value::string(s.last_error));
      arr.push(o);
    }
    out.set("apps", arr);
    json::Value caps = json::Value::array();
    for (const auto& c : opt_.capabilities) caps.push(json::Value::string(c));
    out.set("capabilities", caps);
    return out.dump();
  }
  if (op == "deploy") {
    auto raw = base64_decode((*req)["bundle"].str());
    if (!raw.ok()) return fail("the bundle is not base64");
    auto b = Bundle::decode(as_text(*raw));
    if (!b.ok()) return fail(b.error().message);
    std::vector<std::string> report;
    auto r = deploy(*b, report);
    out.set("report", lines(report));
    if (!r.ok()) { out.set("ok", json::Value::boolean(false)); out.set("error", json::Value::string(r.error().message)); return out.dump(); }
    out.set("version", json::Value::string(*r));
    return out.dump();
  }
  if (op == "rollback") {
    std::vector<std::string> report;
    auto r = rollback(app, report);
    out.set("report", lines(report));
    if (!r.ok()) { out.set("ok", json::Value::boolean(false)); out.set("error", json::Value::string(r.error().message)); }
    return out.dump();
  }
  Result<void> r = Ok();
  if (op == "start") r = start(app);
  else if (op == "stop") r = stop(app);
  else if (op == "restart") r = restart(app);
  else if (op == "remove") r = remove(app);
  else if (op == "logs") { out.set("lines", lines(logs(app, (size_t)std::max<int64_t>(1, (*req)["n"].i64(100))))); return out.dump(); }
  else if (op == "versions") {
    const AppRecord rec = store_.record(app);
    json::Value hist = json::Value::array();
    for (const auto& [id, at] : rec.history) {
      json::Value e = json::Value::object();
      e.set("version", json::Value::string(id));
      e.set("at", json::Value::string(utc_time(at)));
      hist.push(e);
    }
    out.set("current", json::Value::string(rec.current));
    out.set("history", hist);
    return out.dump();
  } else if (op == "set" || op == "secret") {
    std::map<std::string, std::string> values;
    for (const auto& [k, v] : (*req)["values"].members()) values[k] = v.str();
    std::vector<std::string> unset;
    for (const auto& k : (*req)["unset"].items()) unset.push_back(k.str());
    r = set_settings(app, values, op == "secret", unset);
  } else if (op == "settings" || op == "secrets") {
    out.set("names", lines(settings_names(app, op == "secrets")));
    return out.dump();
  } else return fail("unknown request \"" + op + "\" (status, deploy, start, stop, restart, rollback, remove, logs, versions, set, secret, settings, secrets)");
  if (!r.ok()) return fail(r.error().message);
  return out.dump();
}

// ---- the control link ----

ControlServer::ControlServer(hal::Host& host, Host& apps, std::string token) : host_(host), apps_(apps), token_(std::move(token)) {}
ControlServer::~ControlServer() { close(); }

Result<uint16_t> ControlServer::listen(uint16_t port, bool loopback_only) {
  auto l = hal::net_listen(port, loopback_only);
  if (!l.ok()) return Result<uint16_t>(l.error());
  listener_ = std::move(*l);
  const uint16_t bound = listener_->port();
  accept_ = hal::Thread([this] {
    while (!stop_.load()) {
      auto c = listener_->accept(500);
      if (!c.ok()) continue;
      std::shared_ptr<hal::Connection> conn(std::move(*c));
      hal::Thread t([this, conn] {
        auto plain = make_plain_link(conn);
        Id128 me, peer;
        host_.random(MutableByteSpan(reinterpret_cast<uint8_t*>(&me), sizeof me));
        ByteBuffer payload;
        auto key = secure_handshake_responder(*plain, token_, host_, me, peer, payload, {});
        if (!key.ok()) { conn->close(); return; }
        auto link = make_secure_link(conn, *key, false);
        uint8_t type = 0;
        ByteBuffer body;
        while (!stop_.load() && link->recv(type, body, 600000)) {
          if (type != kControlFrame) break;
          const std::string reply = apps_.control(as_text(body));
          if (!link->send(kControlFrame, span(reply))) break;
        }
        conn->close();
      });
      if (t.joinable()) t.detach(); else conn->close();
    }
  });
  return bound;
}

void ControlServer::close() {
  if (stop_.exchange(true)) return;
  if (listener_) listener_->close();
  if (accept_.joinable()) accept_.join();
}

Result<std::string> call_control(hal::Host& host, const std::string& address, uint16_t port, const std::string& token,
                                 const std::string& request_json, int timeout_ms) {
  auto c = hal::net_connect(address, port, 10000);
  if (!c.ok()) return Result<std::string>(Status::IoError, "cannot reach the app host at " + address + ":" + std::to_string(port) + ": " + c.error().message);
  std::shared_ptr<hal::Connection> conn(std::move(*c));
  auto plain = make_plain_link(conn);
  Id128 me, peer;
  host.random(MutableByteSpan(reinterpret_cast<uint8_t*>(&me), sizeof me));
  ByteBuffer payload;
  auto key = secure_handshake_initiator(*plain, token, host, me, peer, payload, {});
  if (!key.ok()) return Result<std::string>(Status::Denied, "the app host at " + address + ":" + std::to_string(port) + " refused the key: " + key.error().message);
  auto link = make_secure_link(conn, *key, true);
  if (!link->send(kControlFrame, span(request_json))) return Result<std::string>(Status::IoError, "the request was not sent");
  uint8_t type = 0;
  ByteBuffer reply;
  if (!link->recv(type, reply, timeout_ms) || type != kControlFrame) return Result<std::string>(Status::IoError, "no answer from the app host");
  conn->close();
  return as_text(reply);
}

}  // namespace aios::userland::grain::apps
