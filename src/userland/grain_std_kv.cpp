// kv_*: a key-value store for sessions, rate limits and short-lived tokens, without a cache server. One store per
// hosted app (shared by its sessions and workers, on any thread), held in memory and journaled the way ASQL keeps its
// tables: every change appended to <app data>/kv.journal through the hal host, replayed at start, the journal
// rewritten (temp file, rename) once it holds far more records than live keys. Values cross VMs as JSON text, so no
// grain object is shared between threads. Outside an app host (a plain `grain` script) the store is the process's,
// in memory only.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "aios/hal/host.hpp"
#include "aios/userland/grain.hpp"
#include "grain_builtins_internal.hpp"

namespace aios::userland::grain {

struct KvStore {
  struct Entry {
    std::string json;      // the value, as json_stringify writes it
    int64_t expires_ms = 0;  // wall clock; 0: never
  };
  static constexpr size_t kMaxKey = 1024, kMaxValue = 1u << 20, kMaxKeys = 1000000;

  std::mutex mu;
  std::unordered_map<std::string, Entry> map;
  hal::Host* host = nullptr;   // null: memory only
  std::string journal;
  uint64_t records = 0;        // records in the journal now
  uint64_t ops = 0;
  std::string problem;         // why the journal could not be read or written (the store then runs in memory)

  static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  }
  bool live(const Entry& e, int64_t now) const { return e.expires_ms == 0 || e.expires_ms > now; }

  // One record: an op letter, the key's and value's lengths, the expiry, a newline, then key and value bytes.
  static std::string record(char op, const std::string& key, const std::string& json, int64_t expires_ms) {
    return std::string(1, op) + " " + std::to_string(key.size()) + " " + std::to_string(json.size()) + " " + std::to_string(expires_ms) + "\n" +
           key + json + "\n";
  }
  void append(const std::string& rec) {
    if (!host || !problem.empty()) return;
    auto r = host->append_file_unsynced(journal, ByteSpan(reinterpret_cast<const uint8_t*>(rec.data()), rec.size()));
    if (!r.ok()) { problem = "kv journal " + journal + ": " + r.error().message; return; }
    ++records;
    if (records > 1000 && records > 3 * map.size()) compact();
  }
  void compact() {
    const int64_t now = now_ms();
    std::string all;
    size_t n = 0;
    for (const auto& [k, e] : map) if (live(e, now)) { all += record('S', k, e.json, e.expires_ms); ++n; }
    const std::string tmp = journal + ".tmp";
    auto w = host->write_file(tmp, ByteSpan(reinterpret_cast<const uint8_t*>(all.data()), all.size()));
    if (!w.ok()) { problem = "kv journal " + tmp + ": " + w.error().message; return; }
    auto mv = host->rename_file(tmp, journal);
    if (!mv.ok()) { problem = "kv journal " + journal + ": " + mv.error().message; return; }
    records = n;
  }
  void replay() {
    if (!host || !host->exists(journal)) return;
    auto d = host->read_file(journal);
    if (!d.ok()) { problem = "kv journal " + journal + ": " + d.error().message; return; }
    const std::string s(d->begin(), d->end());
    size_t at = 0;
    const int64_t now = now_ms();
    while (at < s.size()) {
      const size_t nl = s.find('\n', at);
      if (nl == std::string::npos) break;   // a record cut short by a crash: the rest is dropped
      char op = s[at];
      unsigned long long kl = 0, vl = 0;
      long long exp = 0;
      if (std::sscanf(s.c_str() + at + 1, " %llu %llu %lld", &kl, &vl, &exp) != 3 || nl + 1 + kl + vl + 1 > s.size()) break;
      std::string key = s.substr(nl + 1, kl), json = s.substr(nl + 1 + kl, vl);
      if (op == 'S') {
        if (exp == 0 || exp > now) map[key] = Entry{json, exp};
        else map.erase(key);
      } else if (op == 'D') {
        map.erase(key);
      }
      ++records;
      at = nl + 1 + kl + vl + 1;
    }
  }
  // Expired keys go when read, and in a sweep every so often so a write-only key does not stay forever.
  void tick(int64_t now) {
    if (++ops % 4096 != 0) return;
    for (auto it = map.begin(); it != map.end();) {
      if (!live(it->second, now)) it = map.erase(it);
      else ++it;
    }
  }
  Entry* find(const std::string& key, int64_t now) {
    auto it = map.find(key);
    if (it == map.end()) return nullptr;
    if (!live(it->second, now)) { map.erase(it); return nullptr; }
    return &it->second;
  }
  bool put(const std::string& key, std::string json, int64_t expires_ms, std::string& error) {
    if (!map.count(key) && map.size() >= kMaxKeys) { error = "the store holds its limit of " + std::to_string(kMaxKeys) + " keys"; return false; }
    append(record('S', key, json, expires_ms));
    map[key] = Entry{std::move(json), expires_ms};
    return true;
  }
  bool erase(const std::string& key) {
    if (!map.erase(key)) return false;
    append(record('D', key, "", 0));
    return true;
  }
};

namespace {

std::mutex g_stores_mu;
std::map<std::string, std::weak_ptr<KvStore>> g_stores;   // by journal path: an app restarted keeps its one store

std::shared_ptr<KvStore> process_store() {
  static std::shared_ptr<KvStore> s = std::make_shared<KvStore>();
  return s;
}

KvStore& store_of(Vm& vm) { return vm.kv ? *vm.kv : *process_store(); }

bool key_arg(const Value& v, std::string& key, std::string& error, const char* who) {
  if (!want_str(v, error, who)) return false;
  key = v.str();
  if (key.empty() || key.size() > KvStore::kMaxKey) { error = std::string(who) + "(): a key is 1 to 1024 bytes"; return false; }
  return true;
}

// Seconds to an absolute expiry; 0 / absent: none.
bool ttl_arg(std::vector<Value>& args, size_t at, int64_t& expires_ms, std::string& error, const char* who) {
  expires_ms = 0;
  if (args.size() <= at || args[at].type == Type::Nil) return true;
  if (!want_number(args[at], error, who)) return false;
  const double s = args[at].type == Type::Real ? args[at].d : static_cast<double>(args[at].as_int());
  if (s <= 0) { error = std::string(who) + "(): ttl is a positive number of seconds"; return false; }
  expires_ms = KvStore::now_ms() + static_cast<int64_t>(s * 1000);
  return true;
}

bool encodable(const Value& v) {
  switch (v.type) {
    case Type::Nil: case Type::Bool: case Type::Int: case Type::Real: case Type::Str: return true;
    case Type::List: for (const Value& x : v.list_items()) if (!encodable(x)) return false; return true;
    case Type::Map: for (const auto& [k, x] : static_cast<MapObj*>(v.obj.get())->entries) if (!encodable(x)) return false; return true;
    default: return false;
  }
}

Value decode(const std::string& json) {
  std::string err;
  return grain_json_parse(json, err);
}

// kv_get(key [, default])
Value bi_kv_get(Vm& vm, std::vector<Value>& args, std::string& error) {
  std::string key;
  if (!key_arg(args[0], key, error, "kv_get")) return Value::nil();
  KvStore& s = store_of(vm);
  std::string json;
  {
    std::lock_guard<std::mutex> lk(s.mu);
    KvStore::Entry* e = s.find(key, KvStore::now_ms());
    if (!e) return args.size() > 1 ? args[1] : Value::nil();
    json = e->json;
  }
  return decode(json);
}

// kv_set(key, value [, ttl_seconds]); a nil value deletes.
Value bi_kv_set(Vm& vm, std::vector<Value>& args, std::string& error) {
  std::string key;
  if (!key_arg(args[0], key, error, "kv_set")) return Value::nil();
  int64_t exp = 0;
  if (!ttl_arg(args, 2, exp, error, "kv_set")) return Value::nil();
  KvStore& s = store_of(vm);
  if (args[1].type == Type::Nil) { std::lock_guard<std::mutex> lk(s.mu); s.erase(key); return Value::nil(); }
  if (!encodable(args[1])) { error = "kv_set(): a value is a string, number, bool, list or map (bytes: base64_encode them)"; return Value::nil(); }
  std::string json = grain_json_stringify(args[1]);
  if (json.size() > KvStore::kMaxValue) { error = "kv_set(): a value is at most 1 MB as JSON"; return Value::nil(); }
  std::lock_guard<std::mutex> lk(s.mu);
  s.tick(KvStore::now_ms());
  if (!s.put(key, std::move(json), exp, error)) { error = "kv_set(): " + error; return Value::nil(); }
  return Value::nil();
}

// kv_del(key): whether it was there.
Value bi_kv_del(Vm& vm, std::vector<Value>& args, std::string& error) {
  std::string key;
  if (!key_arg(args[0], key, error, "kv_del")) return Value::nil();
  KvStore& s = store_of(vm);
  std::lock_guard<std::mutex> lk(s.mu);
  const bool there = s.find(key, KvStore::now_ms()) != nullptr;
  if (there) s.erase(key);
  return Value::boolean(there);
}

// kv_incr(key [, by=1 [, ttl_seconds]]): the new value, atomically. A missing key starts at 0 (and takes the ttl);
// an existing one keeps its expiry.
Value bi_kv_incr(Vm& vm, std::vector<Value>& args, std::string& error) {
  std::string key;
  if (!key_arg(args[0], key, error, "kv_incr")) return Value::nil();
  int64_t by = 1;
  if (args.size() > 1 && args[1].type != Type::Nil) {
    if (args[1].type != Type::Int) { error = "kv_incr(): by is a whole number"; return Value::nil(); }
    by = args[1].i;
  }
  int64_t exp = 0;
  if (!ttl_arg(args, 2, exp, error, "kv_incr")) return Value::nil();
  KvStore& s = store_of(vm);
  std::lock_guard<std::mutex> lk(s.mu);
  const int64_t now = KvStore::now_ms();
  s.tick(now);
  int64_t n = 0;
  if (KvStore::Entry* e = s.find(key, now)) {
    const Value v = decode(e->json);
    if (v.type != Type::Int) { error = "kv_incr(): " + key + " holds " + e->json + ", not a whole number"; return Value::nil(); }
    n = v.i;
    exp = e->expires_ms;
  }
  n += by;
  if (!s.put(key, std::to_string(n), exp, error)) { error = "kv_incr(): " + error; return Value::nil(); }
  return Value::integer(n);
}

// kv_expire(key, ttl_seconds): whether the key was there to expire.
Value bi_kv_expire(Vm& vm, std::vector<Value>& args, std::string& error) {
  std::string key;
  if (!key_arg(args[0], key, error, "kv_expire")) return Value::nil();
  int64_t exp = 0;
  if (!ttl_arg(args, 1, exp, error, "kv_expire")) return Value::nil();
  if (exp == 0) { error = "kv_expire(): give a ttl in seconds"; return Value::nil(); }
  KvStore& s = store_of(vm);
  std::lock_guard<std::mutex> lk(s.mu);
  KvStore::Entry* e = s.find(key, KvStore::now_ms());
  if (!e) return Value::boolean(false);
  std::string json = e->json;
  return Value::boolean(s.put(key, std::move(json), exp, error));
}

// kv_ttl(key): seconds left, -1 when it never expires, nil when it is not there.
Value bi_kv_ttl(Vm& vm, std::vector<Value>& args, std::string& error) {
  std::string key;
  if (!key_arg(args[0], key, error, "kv_ttl")) return Value::nil();
  KvStore& s = store_of(vm);
  std::lock_guard<std::mutex> lk(s.mu);
  const int64_t now = KvStore::now_ms();
  KvStore::Entry* e = s.find(key, now);
  if (!e) return Value::nil();
  if (e->expires_ms == 0) return Value::integer(-1);
  return Value::real(static_cast<double>(e->expires_ms - now) / 1000.0);
}

// kv_keys([prefix]): the live keys, sorted.
Value bi_kv_keys(Vm& vm, std::vector<Value>& args, std::string& error) {
  std::string prefix;
  if (!args.empty() && args[0].type != Type::Nil) {
    if (!want_str(args[0], error, "kv_keys")) return Value::nil();
    prefix = args[0].str();
  }
  KvStore& s = store_of(vm);
  std::vector<std::string> keys;
  {
    std::lock_guard<std::mutex> lk(s.mu);
    const int64_t now = KvStore::now_ms();
    for (const auto& [k, e] : s.map) if (s.live(e, now) && k.compare(0, prefix.size(), prefix) == 0) keys.push_back(k);
  }
  std::sort(keys.begin(), keys.end());
  std::vector<Value> out;
  out.reserve(keys.size());
  for (auto& k : keys) out.push_back(Value::string(std::move(k)));
  return Value::list(std::move(out));
}

// kv_mget(keys): their values in order, nil for the missing.
Value bi_kv_mget(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::List) { error = "kv_mget() wants a list of keys"; return Value::nil(); }
  std::vector<std::string> keys;
  for (const Value& v : args[0].list_items()) {
    std::string k;
    if (!key_arg(v, k, error, "kv_mget")) return Value::nil();
    keys.push_back(std::move(k));
  }
  KvStore& s = store_of(vm);
  std::vector<std::string> found(keys.size());
  std::vector<bool> have(keys.size(), false);
  {
    std::lock_guard<std::mutex> lk(s.mu);
    const int64_t now = KvStore::now_ms();
    for (size_t i = 0; i < keys.size(); ++i)
      if (KvStore::Entry* e = s.find(keys[i], now)) { found[i] = e->json; have[i] = true; }
  }
  std::vector<Value> out;
  out.reserve(keys.size());
  for (size_t i = 0; i < keys.size(); ++i) out.push_back(have[i] ? decode(found[i]) : Value::nil());
  return Value::list(std::move(out));
}

}  // namespace

std::shared_ptr<KvStore> open_kv_store(hal::Host& host, const std::string& journal_path, std::string* problem) {
  std::lock_guard<std::mutex> lk(g_stores_mu);
  if (auto have = g_stores[journal_path].lock()) return have;
  auto s = std::make_shared<KvStore>();
  s->host = &host;
  s->journal = journal_path;
  s->replay();
  if (s->problem.empty() && s->records > 3 * s->map.size() + 64) s->compact();   // start from a short journal
  if (problem) *problem = s->problem;
  g_stores[journal_path] = s;
  return s;
}

void install_kv_builtins(Vm& vm) {
  vm.define_native("kv_get", 1, 2, bi_kv_get);
  vm.define_native("kv_set", 2, 3, bi_kv_set);
  vm.define_native("kv_del", 1, 1, bi_kv_del);
  vm.define_native("kv_incr", 1, 3, bi_kv_incr);
  vm.define_native("kv_expire", 2, 2, bi_kv_expire);
  vm.define_native("kv_ttl", 1, 1, bi_kv_ttl);
  vm.define_native("kv_keys", 0, 1, bi_kv_keys);
  vm.define_native("kv_mget", 1, 1, bi_kv_mget);
}

}  // namespace aios::userland::grain
