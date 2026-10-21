#include "aios/channel/agent_control_serve.hpp"
#include "aios/channel/rpc_value.hpp"
#include "aios/crypto/hmac_sha256.hpp"
#include "aios/crypto/sha256.hpp"
#include "aios/capability/authority.hpp"
#include "aios/intelligence/intelligence.hpp"
#include "aios/intelligence/qwen_mind.hpp"
#include "aios/hal/net.hpp"
#include "aios/hal/sync.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>

namespace aios::channel {
namespace {

constexpr size_t kMaxFrame = 4u << 20;

bool read_frame(hal::Connection& c, std::string& out, int timeout_ms) {
  uint8_t hdr[4];
  if (!c.recv_all(MutableByteSpan(hdr, 4), timeout_ms)) return false;
  const uint32_t n = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
  if (n == 0 || n > kMaxFrame) return false;
  out.resize(n);
  return c.recv_all(MutableByteSpan(reinterpret_cast<uint8_t*>(out.data()), n), timeout_ms);
}

bool write_frame(hal::Connection& c, const std::string& body) {
  std::string f;
  f.reserve(4 + body.size());
  const uint32_t n = (uint32_t)body.size();
  f.push_back((char)(n >> 24)); f.push_back((char)(n >> 16)); f.push_back((char)(n >> 8)); f.push_back((char)n);
  f += body;
  return c.send_all(span_of(f));
}

RpcValue ok_answer(const std::string& op, const std::string& id) {
  RpcValue r = RpcValue::object();
  r.set("ok", RpcValue::boolean(true));
  r.set("op", RpcValue::text(op));
  if (!id.empty()) r.set("id", RpcValue::text(id));
  return r;
}

RpcValue error_answer(const std::string& op, const std::string& id, const std::string& why) {
  RpcValue r = RpcValue::object();
  r.set("ok", RpcValue::boolean(false));
  r.set("op", RpcValue::text(op));
  if (!id.empty()) r.set("id", RpcValue::text(id));
  r.set("error", RpcValue::text(why));
  return r;
}

std::string to_hex(const uint8_t* data, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s; s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) { s.push_back(d[data[i] >> 4]); s.push_back(d[data[i] & 0xF]); }
  return s;
}

// Constant-time compare -- an auth check that short-circuits on the first
// mismatched byte leaks how much of the token was right via timing.
bool constant_time_eq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
  return diff == 0;
}

// The canonical string a caller signs: the request object rendered with the
// "auth" field itself omitted, in the same field order it was constructed
// in. The caller builds its request, renders+signs everything except "auth",
// then adds "auth" as the last field before sending -- so re-removing it
// here reproduces exactly what was signed.
std::string canonical_without_auth(const RpcValue& req) {
  RpcValue copy = RpcValue::object();
  for (const auto& [k, v] : req.fields()) {
    if (k == "auth") continue;
    copy.set(k, v);
  }
  return copy.render();
}

// Replay protection. A signed request must carry "ts" (unix seconds) and a
// "nonce" (any unique string, 8-128 chars); both sit inside the HMAC. The
// server accepts a ts within +/- the window and each nonce once inside it, so
// a captured request cannot be sent again: a replay inside the window repeats
// a nonce, one after it carries a stale ts.
struct ReplayGuard {
  hal::Mutex mu;
  std::map<std::string, uint64_t> seen;   // nonce -> ts it arrived with
  // Empty string: fresh. Otherwise why it was refused.
  std::string admit(const std::string& nonce, uint64_t ts, uint64_t now_s, uint64_t window_s) {
    if (ts + window_s < now_s || ts > now_s + window_s)
      return "stale or future ts (" + std::to_string(ts) + " vs server " + std::to_string(now_s) + ", window " + std::to_string(window_s) + "s)";
    hal::LockGuard g(mu);
    for (auto it = seen.begin(); it != seen.end();) {
      if (it->second + window_s < now_s) it = seen.erase(it); else ++it;
    }
    if (seen.count(nonce)) return "replayed nonce";
    seen[nonce] = ts;
    return {};
  }
};

// Empty: authorized. Otherwise the reason it is not.
std::string auth_check(Kernel& k, const RpcValue& req, const AgentControlOptions& o, ReplayGuard& guard) {
  if (o.auth_secret.empty()) return {};  // no auth configured -- dev/loopback only, see agent-control.cpp
  if (!req.has("auth")) return "unauthorized";
  const std::string canon = canonical_without_auth(req);
  auto mac = crypto::hmac_sha256(ByteSpan((const uint8_t*)o.auth_secret.data(), o.auth_secret.size()),
                                  ByteSpan((const uint8_t*)canon.data(), canon.size()));
  const std::string expect = to_hex(mac.data(), mac.size());
  if (!constant_time_eq(expect, req["auth"].as_text())) return "unauthorized";
  if (!req.has("ts") || !req.has("nonce")) return "unauthorized: signed requests must carry \"ts\" (unix seconds) and \"nonce\"";
  const std::string nonce = req["nonce"].as_text();
  if (nonce.size() < 8 || nonce.size() > 128) return "unauthorized: nonce must be 8-128 characters";
  const uint64_t ts = req["ts"].as_u64(0);
  const uint64_t now_s = k.host().wall_ns() / 1000000000ull;
  std::string why = guard.admit(nonce, ts, now_s, o.replay_window_s);
  if (!why.empty()) return "unauthorized: " + why;
  return {};
}

Result<EntityID> parse_entity(const RpcValue& req, const std::string& field) {
  if (!req.has(field)) return Result<EntityID>(Status::Invalid, "missing '" + field + "'");
  Id128 raw;
  if (!Id128::parse(req[field].as_text(), raw)) return Result<EntityID>(Status::Invalid, "'" + field + "' is not a valid id");
  return EntityID{raw};
}

Result<CapabilityID> parse_cap(const RpcValue& req, const std::string& field) {
  if (!req.has(field)) return Result<CapabilityID>(Status::Invalid, "missing '" + field + "'");
  Id128 raw;
  if (!Id128::parse(req[field].as_text(), raw)) return Result<CapabilityID>(Status::Invalid, "'" + field + "' is not a valid id");
  return CapabilityID{raw};
}

RpcValue render_cycle_report(Kernel& k, const CycleReport& r) {
  RpcValue v = RpcValue::object();
  v.set("entity", RpcValue::text(r.entity.str()));
  v.set("intelligence", RpcValue::text(r.intelligence));
  v.set("context", RpcValue::text(r.context.str()));
  v.set("perceived", RpcValue::text(r.perceived.str()));
  v.set("result", RpcValue::text(r.result.str()));
  // The whole point of a cycle is what the mind actually said or produced --
  // a caller with only ids and a "computed" bool cannot tell a real answer
  // from a refusal from an empty string. Read the result state's own payload
  // back, the same way any other Surface caller would.
  auto result_state = k.states().copy(r.result);
  if (result_state.ok()) {
    v.set("answer", RpcValue::text(std::string(result_state->payload.begin(), result_state->payload.end())));
  }
  v.set("graph", RpcValue::text(r.graph.str()));
  v.set("computed", RpcValue::boolean(r.computed));
  if (r.computed) {
    RpcValue ex = RpcValue::object();
    ex.set("operations_run", RpcValue::number(r.execution.operations_run));
    ex.set("elapsed_ns", RpcValue::number(r.execution.elapsed_ns));
    ex.set("complete", RpcValue::boolean(r.execution.complete));
    v.set("execution", std::move(ex));
  }
  v.set("published", RpcValue::boolean(r.published));
  if (r.published) v.set("published_sequence", RpcValue::number(r.published_sequence));
  v.set("elapsed_ns", RpcValue::number(r.elapsed_ns));
  return v;
}

RpcValue render_event(const AuthorityEvent& e) {
  RpcValue v = RpcValue::object();
  v.set("kind", RpcValue::text(authority_event_name(e.kind)));
  v.set("ns", RpcValue::number(e.ns));
  v.set("actor", RpcValue::text(e.actor.str()));
  v.set("subject", RpcValue::text(e.subject.str()));
  v.set("resource", RpcValue::text(e.resource.str()));
  v.set("rights", RpcValue::text(rights_str(e.rights)));
  if (!e.capability.is_null()) v.set("capability", RpcValue::text(e.capability.str()));
  if (!e.note.empty()) v.set("note", RpcValue::text(e.note));
  return v;
}

// Every agent this service has ever created via agent.create, from any
// caller -- this process is the only spawner in it, so this is a complete,
// live "what's running" list without touching Kernel at all. Populated on
// agent.create, pruned on agent.terminate (and defensively on agent.list, in
// case an entity somehow vanished another way).
//
// It also remembers how each entity's mind was made. A rewind puts an entity back to an earlier image and the kernel
// drops its mind binding (a mind is not part of the image), so the service binds a fresh one the same way afterwards:
// the same external command with the same environment, or the same endpoint and model.
using MindMaker = std::function<Result<std::unique_ptr<Intelligence>>()>;
struct AgentRegistry {
  mutable hal::Mutex mu;
  std::vector<EntityID> ids;
  std::map<std::string, MindMaker> makers;
  void add(const EntityID& id) { hal::LockGuard g(mu); ids.push_back(id); }
  void remove(const EntityID& id) { hal::LockGuard g(mu); ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end()); makers.erase(id.str()); }
  void remember_mind(const EntityID& id, MindMaker m) { hal::LockGuard g(mu); makers[id.str()] = std::move(m); }
  MindMaker mind_maker(const EntityID& id) const { hal::LockGuard g(mu); auto it = makers.find(id.str()); return it == makers.end() ? MindMaker() : it->second; }
  std::vector<EntityID> snapshot() const { hal::LockGuard g(mu); return ids; }
};

// A provider, model or effort handed to an external mind lands in its environment: only what a model name is made of.
bool safe_mind_arg(const std::string& s) {
  if (s.empty() || s.size() > 128) return false;
  for (unsigned char c : s) if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == ':' || c == '/')) return false;
  return true;
}

// The address of a model's API, as handed to an external mind: http(s)://host[:port][/path], URL characters only.
bool safe_mind_host(const std::string& s) {
  if (s.size() < 8 || s.size() > 256) return false;
  if (s.rfind("http://", 0) != 0 && s.rfind("https://", 0) != 0) return false;
  for (unsigned char c : s) if (!(std::isalnum(c) || std::strchr("-._~:/?#[]@!$&()*+,;=%", c))) return false;
  return true;
}

// "[NAME=value ...] program [args ...]": the assignments in front become the environment, the rest the command.
// Words split on spaces; single or double quotes keep a word (with its spaces) together; there are no escapes.
bool split_mind_command(const std::string& cmd, std::vector<std::string>& env, std::vector<std::string>& argv, std::string& why) {
  std::vector<std::string> words;
  std::string cur;
  bool in_word = false;
  char quote = 0;
  for (char c : cmd) {
    if (quote) { if (c == quote) quote = 0; else cur.push_back(c); continue; }
    if (c == '"' || c == '\'') { quote = c; in_word = true; continue; }
    if (std::isspace((unsigned char)c)) { if (in_word) { words.push_back(cur); cur.clear(); in_word = false; } continue; }
    cur.push_back(c); in_word = true;
  }
  if (quote) { why = "an unclosed quote"; return false; }
  if (in_word) words.push_back(cur);
  size_t i = 0;
  for (; i < words.size(); ++i) {
    const std::string& w = words[i];
    const size_t eq = w.find('=');
    if (eq == std::string::npos || eq == 0 || std::isdigit((unsigned char)w[0])) break;
    bool name_ok = true;
    for (size_t j = 0; j < eq; ++j) if (!(std::isalnum((unsigned char)w[j]) || w[j] == '_')) { name_ok = false; break; }
    if (!name_ok) break;
    env.push_back(w);
  }
  argv.assign(words.begin() + (long)i, words.end());
  if (argv.empty()) { why = "no program to run"; return false; }
  return true;
}

// A resource a request names. Either "resource_id" (an id, or "*"), as before, or "resource": a name such as
// "tree:/repo", "path:/repo/a.txt", "cmd:shell" or "git:origin", which the service turns into an id of its own
// (a hash of its kind and name), so a grant and a later cap.exercise of the same name meet, and a different name
// is never covered by it. "*" is the whole kind. With neither: the whole kind, as before.
Id128 named_resource(ResourceKind kind, const std::string& name) {
  std::string material = resource_kind_name(kind);
  material.push_back('\0');
  material += name;
  const auto h = crypto::sha256(span_of(material));
  auto be64 = [&h](size_t at) { uint64_t v = 0; for (size_t i = 0; i < 8; ++i) v = (v << 8) | h[at + i]; return v; };
  Id128 id; id.hi = be64(0); id.lo = be64(8);
  if (id.is_null()) id.lo = 1;
  return id;
}
bool request_resource(const RpcValue& req, ResourceKind kind, Id128& id, std::string& label, std::string& why) {
  const bool has_id = req.has("resource_id") && req["resource_id"].as_text() != "*";
  const bool has_name = req.has("resource") && req["resource"].as_text() != "*";
  if (has_id && has_name) { why = "give 'resource_id' or 'resource', not both"; return false; }
  label = resource_kind_name(kind);
  if (has_id) {
    if (!Id128::parse(req["resource_id"].as_text(), id)) { why = "'resource_id' is not a valid id or '*'"; return false; }
    label += ":" + id.short_str();
  } else if (has_name) {
    const std::string name = req["resource"].as_text();
    if (name.empty() || name.size() > 1024) { why = "'resource' must be 1 to 1024 characters"; return false; }
    id = named_resource(kind, name);
    label += ":" + name;
  } else {
    label += ":*";
  }
  return true;
}

// What an agent's cycles did, as the caller recorded them (cycle.record): the reply, what came back, the verdict, the
// checkpoint taken at the start of the cycle (what agent.rewind goes back to) and the work tree. Kept per entity and
// appended to a file under the kernel's root, so the audit trail and a rewind survive this service restarting.
class CycleLog {
 public:
  explicit CycleLog(std::string path) : path_(std::move(path)) { load(); }
  void add(const std::string& entity, RpcValue rec) {
    hal::LockGuard g(mu_);
    append_to_file(entity, rec);
    by_entity_[entity].push_back(std::move(rec));
  }
  std::vector<RpcValue> of(const std::string& entity, bool one, uint64_t cycle) const {
    hal::LockGuard g(mu_);
    std::vector<RpcValue> out;
    auto it = by_entity_.find(entity);
    if (it == by_entity_.end()) return out;
    for (const auto& r : it->second) if (!one || r["cycle"].as_u64() == cycle) out.push_back(r);
    return out;
  }
  // The checkpoint taken at the start of `cycle` (the latest record of it that has one), "" when none was.
  std::string checkpoint_at(const std::string& entity, uint64_t cycle) const {
    std::string cp;
    for (const auto& r : of(entity, true, cycle)) if (r.has("checkpoint") && !r["checkpoint"].as_text().empty()) cp = r["checkpoint"].as_text();
    return cp;
  }

 private:
  void load() {
    std::ifstream f(path_);
    std::string line;
    while (std::getline(f, line)) {
      if (line.empty()) continue;
      auto v = rpc_parse(line);
      if (!v.ok() || v->kind() != RpcValue::Kind::Object || !v->has("entity")) continue;  // a torn last line is skipped
      by_entity_[(*v)["entity"].as_text()].push_back(*v);
    }
  }
  void append_to_file(const std::string& entity, const RpcValue& rec) {
    RpcValue line = rec;
    line.set("entity", RpcValue::text(entity));
    std::ofstream f(path_, std::ios::app);
    if (f) f << line.render() << "\n";
  }
  std::string path_;
  mutable hal::Mutex mu_;
  std::map<std::string, std::vector<RpcValue>> by_entity_;
};

// One request, one response, on the thread that accepted the connection.
// Every mutating branch below calls straight through Surface -- never
// Kernel::grant -- so the structural guarantee holds: a caller can only ever
// end up with authority `svc`'s own resident identity already had to give.
RpcValue handle(Kernel& k, Surface& svc, const AgentControlOptions& o, AgentRegistry& registry, CycleLog& cycles,
                ReplayGuard& guard, const RpcValue& req) {
  const std::string op = req.has("op") ? req["op"].as_text() : std::string();
  const std::string id = req.has("id") ? req["id"].as_text() : std::string();

  if (std::string why = auth_check(k, req, o, guard); !why.empty()) {
    return error_answer(op, id, why);
  }

  if (op == "svc.ping") {
    return ok_answer(op, id);
  }

  if (op == "agent.create") {
    const std::string label = req.has("label") ? req["label"].as_text() : "agent";
    EntityID parent;
    if (req.has("parent")) {
      auto p = parse_entity(req, "parent");
      if (!p.ok()) return error_answer(op, id, p.error().message);
      parent = *p;
    }
    auto r = svc.spawn(label, parent);
    if (!r.ok()) return error_answer(op, id, r.error().message);
    registry.add(*r);
    RpcValue out = ok_answer(op, id);
    out.set("entity", RpcValue::text(r->str()));
    return out;
  }

  if (op == "agent.bind_mind") {
    auto entity = parse_entity(req, "entity");
    if (!entity.ok()) return error_answer(op, id, entity.error().message);
    const std::string kind = req.has("kind") ? req["kind"].as_text() : "qwen";
    if (kind == "external") {
      const std::string name = req.has("mind") ? req["mind"].as_text() : std::string();
      auto known = o.external_minds.find(name);
      if (known == o.external_minds.end()) {
        std::string names;
        for (const auto& m : o.external_minds) names += (names.empty() ? "" : ", ") + m.first;
        return error_answer(op, id, "unknown external mind '" + name + "' (this service was started with: " + (names.empty() ? "none" : names) + ")");
      }
      std::vector<std::string> env, argv;
      std::string why;
      if (!split_mind_command(known->second, env, argv, why)) return error_answer(op, id, "external mind '" + name + "' has an unusable command: " + why);
      static const struct { const char* field; const char* var; } kMindEnv[] = {
        {"provider", "AIOS_MODEL_PROVIDER"}, {"model", "AIOS_MODEL"}, {"effort", "AIOS_MIND_EFFORT"}};
      if (req.has("host") && !req["host"].as_text().empty()) {
        const std::string h = req["host"].as_text();
        if (!safe_mind_host(h)) return error_answer(op, id, "'host' must be an http:// or https:// address (URL characters only, up to 256)");
        env.push_back("AIOS_MODEL_HOST=" + h);
      }
      for (const auto& m : kMindEnv) {
        if (!req.has(m.field)) continue;
        const std::string v = req[m.field].as_text();
        if (v.empty()) continue;
        if (!safe_mind_arg(v)) return error_answer(op, id, std::string("'") + m.field + "' may only hold letters, digits and . - _ : / (up to 128 characters)");
        env.push_back(std::string(m.var) + "=" + v);
      }
      auto mind = spawn_external_intelligence(argv, env);
      if (!mind.ok()) return error_answer(op, id, "external mind '" + name + "' did not start: " + mind.error().message);
      auto r = svc.bind_mind(*entity, std::move(*mind));
      if (!r.ok()) return error_answer(op, id, r.error().message);
      registry.remember_mind(*entity, [argv, env]() { return spawn_external_intelligence(argv, env); });
      return ok_answer(op, id);
    }
    if (kind != "qwen") return error_answer(op, id, "unsupported mind kind '" + kind + "' (qwen or external)");

    // No url: the mind reads AIOS_MIND_URL (QwenIntelligence), so no model server is built in.
    const std::string url = req.has("url") ? req["url"].as_text() : std::string();
    const std::string model = req.has("model") ? req["model"].as_text() : std::string("spark");
    std::string cred;
    if (req.has("credential_ref") && req["credential_ref"].as_text() == "file") {
      cred = intelligence::network_mind_credential(k.host(), k.config().root);
    } else if (req.has("credential")) {
      // Accepted, never echoed or logged -- the caller's responsibility to
      // avoid sending one over an unencrypted link; this service does not
      // write it anywhere beyond this in-memory move.
      cred = req["credential"].as_text();
    }
    MindMaker make_qwen = [url, model, cred]() -> Result<std::unique_ptr<Intelligence>> {
      auto m = intelligence::make_qwen_intelligence(url, model);
      if (!cred.empty()) intelligence::set_qwen_credential(*m, cred);
      return std::unique_ptr<Intelligence>(std::move(m));
    };
    auto mind = make_qwen();
    if (!mind.ok()) return error_answer(op, id, mind.error().message);
    auto r = svc.bind_mind(*entity, std::move(*mind));
    if (!r.ok()) return error_answer(op, id, r.error().message);
    registry.remember_mind(*entity, std::move(make_qwen));
    return ok_answer(op, id);
  }

  if (op == "agent.send") {
    auto entity = parse_entity(req, "entity");
    if (!entity.ok()) return error_answer(op, id, entity.error().message);
    if (!req.has("input")) return error_answer(op, id, "missing 'input'");
    const MindBinding* mb = k.mind(*entity);
    if (!mb) return error_answer(op, id, "entity has no bound intelligence");
    // The entity writes its own context state -- act_as, not the service's
    // own authority, so this only ever spends rights the entity itself holds.
    auto scoped = svc.act_as(*entity);
    if (!scoped.ok()) return error_answer(op, id, scoped.error().message);
    const std::string& text = req["input"].as_text();
    auto r = scoped->set_state(mb->context, ByteSpan(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
    if (!r.ok()) return error_answer(op, id, r.error().message);
    return ok_answer(op, id);
  }

  if (op == "agent.terminate") {
    auto entity = parse_entity(req, "entity");
    if (!entity.ok()) return error_answer(op, id, entity.error().message);
    auto r = svc.terminate(*entity);
    if (!r.ok()) return error_answer(op, id, r.error().message);
    registry.remove(*entity);
    return ok_answer(op, id);
  }

  if (op == "agent.list") {
    RpcValue list = RpcValue::list();
    for (const auto& eid : registry.snapshot()) {
      if (!k.entities().exists(eid)) { registry.remove(eid); continue; }
      auto ent = k.entities().copy(eid);
      if (!ent.ok()) continue;
      RpcValue v = RpcValue::object();
      v.set("entity", RpcValue::text(eid.str()));
      v.set("label", RpcValue::text(ent->label));
      if (const MindBinding* mb = k.mind(eid)) {
        v.set("intelligence", RpcValue::text(mb->mind ? mb->mind->name() : ""));
        v.set("cycles", RpcValue::number(mb->cycles));
      }
      list.push(std::move(v));
    }
    RpcValue out = ok_answer(op, id);
    out.set("agents", std::move(list));
    return out;
  }

  if (op == "cap.grant") {
    auto to = parse_entity(req, "to");
    if (!to.ok()) return error_answer(op, id, to.error().message);
    ResourceKind kind;
    if (!req.has("kind") || !parse_resource_kind(req["kind"].as_text(), kind)) {
      return error_answer(op, id, "unknown resource kind (memory|compute|channel|storage|entity|network|state|kernel)");
    }
    Id128 resource_id;  // default null = wildcard over the whole kind
    std::string label, why;
    if (!request_resource(req, kind, resource_id, label, why)) return error_answer(op, id, why);
    uint16_t rights = 0;
    if (!req.has("rights") || !parse_rights(req["rights"].as_text(), rights)) {
      return error_answer(op, id, "'rights' missing or unparseable (csv of read,write,execute,grant,destroy,publish,consume,allocate,schedule,checkpoint)");
    }
    const uint64_t ttl_ns = req.has("ttl_s") ? req["ttl_s"].as_u64() * 1000000000ull : 0;
    const uint64_t scope = req.has("scope") ? req["scope"].as_u64() : 0;

    auto r = svc.grant(*to, ResourceRef{kind, resource_id}, rights, ttl_ns, scope);
    if (!r.ok()) return error_answer(op, id, r.error().message);
    RpcValue out = ok_answer(op, id);
    out.set("capability", RpcValue::text(r->str()));
    return out;
  }

  // The entity uses authority it holds: allowed or refused by what its own capabilities cover, and either way on
  // the ledger. The answer is {"ok":true,"allowed":bool,"error":"..."?}: a refusal is an answer, not a failure.
  if (op == "cap.exercise") {
    auto entity = parse_entity(req, "entity");
    if (!entity.ok()) return error_answer(op, id, entity.error().message);
    ResourceKind kind;
    if (!req.has("kind") || !parse_resource_kind(req["kind"].as_text(), kind)) return error_answer(op, id, "unknown resource kind");
    Id128 resource_id;
    std::string label, why;
    if (!request_resource(req, kind, resource_id, label, why)) return error_answer(op, id, why);
    uint16_t rights = 0;
    if (!req.has("rights") || !parse_rights(req["rights"].as_text(), rights)) return error_answer(op, id, "'rights' missing or unparseable");
    std::string action = req.has("action") ? req["action"].as_text() : std::string();
    if (action.size() > 300) action.resize(300);
    auto scoped = svc.act_as(*entity);
    if (!scoped.ok()) return error_answer(op, id, scoped.error().message);
    auto r = scoped->exercise(ResourceRef{kind, resource_id}, rights, label + (action.empty() ? "" : ": " + action));
    RpcValue out = ok_answer(op, id);
    out.set("allowed", RpcValue::boolean(r.ok()));
    if (!r.ok()) out.set("error", RpcValue::text(r.status() == Status::Denied
        ? "entity " + entity->short_str() + " holds no " + rights_str(rights) + " capability over " + label : r.error().message));
    return out;
  }

  // An image of the entity as it is now, which agent.rewind can put it back to.
  if (op == "agent.checkpoint") {
    auto entity = parse_entity(req, "entity");
    if (!entity.ok()) return error_answer(op, id, entity.error().message);
    auto cp = svc.checkpoint_of(*entity);
    if (!cp.ok()) return error_answer(op, id, cp.error().message);
    RpcValue out = ok_answer(op, id);
    out.set("checkpoint", RpcValue::text(*cp));
    return out;
  }

  // Back to a checkpoint of ITSELF: either explicit "checkpoint" path or recorded "cycle".
  // The entity keeps its identity and gets the contents it had then.
  if (op == "agent.rewind") {
    auto entity = parse_entity(req, "entity");
    if (!entity.ok()) return error_answer(op, id, entity.error().message);
    std::string cp;
    if (req.has("checkpoint")) {
      cp = req["checkpoint"].as_text();
    } else if (req.has("cycle") && req["cycle"].as_u64() >= 1) {
      const uint64_t cycle = req["cycle"].as_u64();
      cp = cycles.checkpoint_at(entity->str(), cycle);
      if (cp.empty()) return error_answer(op, id, "no checkpoint was recorded at the start of cycle " + std::to_string(cycle));
    } else {
      return error_answer(op, id, "agent.rewind needs 'checkpoint' path or 'cycle' (>= 1)");
    }
    const std::string mine = k.config().root + "/checkpoints/" + entity->str() + "/";
    if (cp.rfind(mine, 0) != 0 || cp.find("..") != std::string::npos) {
      return error_answer(op, id, "agent.rewind takes a checkpoint of this entity (from agent.checkpoint)");
    }
    auto rw = svc.rewind(cp);
    if (!rw.ok()) return error_answer(op, id, rw.error().message);
    if (*rw != *entity) return error_answer(op, id, "the checkpoint belongs to another entity");
    // The kernel dropped the entity's mind with its old contents: bind a fresh one the way the first was made.
    if (auto make = registry.mind_maker(*entity)) {
      auto mind = make();
      if (!mind.ok()) return error_answer(op, id, "rewound, but its mind could not be started again: " + mind.error().message);
      auto bound = svc.bind_mind(*entity, std::move(*mind));
      if (!bound.ok()) return error_answer(op, id, "rewound, but its mind could not be bound again: " + bound.error().message);
    }
    RpcValue out = ok_answer(op, id);
    out.set("entity", RpcValue::text(rw->str()));
    return out;
  }

  // One cycle's record, kept for the audit trail and for rewinding.
  if (op == "cycle.record") {
    auto entity = parse_entity(req, "entity");
    if (!entity.ok()) return error_answer(op, id, entity.error().message);
    if (!k.entities().exists(*entity)) return error_answer(op, id, "no such entity");
    if (!req.has("cycle") || req["cycle"].as_u64() < 1) return error_answer(op, id, "missing 'cycle' (>= 1)");
    RpcValue rec = RpcValue::object();
    rec.set("cycle", RpcValue::number(req["cycle"].as_u64()));
    rec.set("ns", RpcValue::number((uint64_t)k.now()));
    for (const char* f : {"action", "target", "verdict", "reason", "reply", "observation", "checkpoint", "tree"}) {
      if (!req.has(f)) continue;
      std::string v = req[f].as_text();
      if (v.size() > (64u << 10)) v.resize(64u << 10);
      rec.set(f, RpcValue::text(v));
    }
    if (rec.has("checkpoint")) {
      const std::string cp = rec["checkpoint"].as_text();
      const std::string mine = k.config().root + "/checkpoints/" + entity->str() + "/";
      if (!cp.empty() && (cp.rfind(mine, 0) != 0 || cp.find("..") != std::string::npos)) {
        return error_answer(op, id, "'checkpoint' must be one of this entity's (from agent.checkpoint)");
      }
    }
    cycles.add(entity->str(), std::move(rec));
    return ok_answer(op, id);
  }

  // The recorded cycles of an entity (one cycle with "cycle"), also after the entity has ended.
  if (op == "cycle.records") {
    auto entity = parse_entity(req, "entity");
    if (!entity.ok()) return error_answer(op, id, entity.error().message);
    RpcValue list = RpcValue::list();
    for (auto& r : cycles.of(entity->str(), req.has("cycle"), req.has("cycle") ? req["cycle"].as_u64() : 0)) list.push(std::move(r));
    RpcValue out = ok_answer(op, id);
    out.set("cycles", std::move(list));
    return out;
  }

  if (op == "cap.request") {
    auto entity = parse_entity(req, "entity");
    if (!entity.ok()) return error_answer(op, id, entity.error().message);
    ResourceKind kind;
    if (!req.has("kind") || !parse_resource_kind(req["kind"].as_text(), kind)) {
      return error_answer(op, id, "unknown resource kind");
    }
    Id128 resource_id;
    std::string label, why;
    if (!request_resource(req, kind, resource_id, label, why)) return error_answer(op, id, why);
    uint16_t rights = 0;
    if (!req.has("rights") || !parse_rights(req["rights"].as_text(), rights)) {
      return error_answer(op, id, "'rights' missing or unparseable");
    }
    const uint64_t ttl_ns = req.has("ttl_s") ? req["ttl_s"].as_u64() * 1000000000ull : 0;

    // The agent requests from its OWN held authority, not the service
    // granting on its behalf -- a different, correct semantic.
    auto scoped = svc.act_as(*entity);
    if (!scoped.ok()) return error_answer(op, id, scoped.error().message);
    auto r = scoped->request(ResourceRef{kind, resource_id}, rights, ttl_ns);
    if (!r.ok()) return error_answer(op, id, r.error().message);
    RpcValue out = ok_answer(op, id);
    out.set("capability", RpcValue::text(r->str()));
    return out;
  }

  if (op == "cap.revoke") {
    auto cap = parse_cap(req, "capability");
    if (!cap.ok()) return error_answer(op, id, cap.error().message);
    auto r = svc.revoke(*cap);
    if (!r.ok()) return error_answer(op, id, r.error().message);
    return ok_answer(op, id);
  }

  if (op == "cycle.run") {
    auto entity = parse_entity(req, "entity");
    if (!entity.ok()) return error_answer(op, id, entity.error().message);
    auto r = svc.cycle(*entity);
    if (!r.ok()) return error_answer(op, id, r.error().message);
    RpcValue out = ok_answer(op, id);
    out.set("report", render_cycle_report(k, *r));
    return out;
  }

  if (op == "cycle.run_n") {
    auto entity = parse_entity(req, "entity");
    if (!entity.ok()) return error_answer(op, id, entity.error().message);
    int n = req.has("n") ? (int)req["n"].as_u64(1) : 1;
    n = std::max(1, std::min(n, o.max_cycle_run_n));
    RpcValue reports = RpcValue::list();
    for (int i = 0; i < n; ++i) {
      auto r = svc.cycle(*entity);
      if (!r.ok()) {
        RpcValue out = error_answer(op, id, r.error().message);
        out.set("reports", reports);
        out.set("completed", RpcValue::number((uint64_t)i));
        return out;
      }
      reports.push(render_cycle_report(k, *r));
    }
    RpcValue out = ok_answer(op, id);
    out.set("reports", std::move(reports));
    return out;
  }

  if (op == "ledger.events") {
    auto entity = parse_entity(req, "entity");
    if (!entity.ok()) return error_answer(op, id, entity.error().message);
    const auto& all_events = k.authority().events();
    // AuthorityLedger::events() is append-only in stable order (confirmed:
    // record_event only ever push_back's, nothing erases). A caller can just
    // remember the count it saw last time and pass it back as `since` to get
    // only what's new -- no sequence field needed on the ledger itself.
    uint64_t since = req.has("since") ? req["since"].as_u64(0) : 0;
    if (since > all_events.size()) since = 0;  // stale/reset cursor: start over rather than error
    RpcValue events = RpcValue::list();
    for (size_t i = since; i < all_events.size(); ++i) {
      const auto& e = all_events[i];
      if (e.actor == *entity || e.subject == *entity) events.push(render_event(e));
    }
    RpcValue out = ok_answer(op, id);
    out.set("events", std::move(events));
    out.set("cursor", RpcValue::number((uint64_t)all_events.size()));
    return out;
  }

  if (op == "entity.status") {
    auto entity = parse_entity(req, "entity");
    if (!entity.ok()) return error_answer(op, id, entity.error().message);
    if (!k.entities().exists(*entity)) return error_answer(op, id, "entity not found");
    auto ent = k.entities().copy(*entity);
    if (!ent.ok()) return error_answer(op, id, ent.error().message);
    RpcValue out = ok_answer(op, id);
    out.set("label", RpcValue::text(ent->label));
    out.set("capabilities", RpcValue::number((uint64_t)ent->capabilities.size()));
    if (const MindBinding* mb = k.mind(*entity)) {
      RpcValue mind = RpcValue::object();
      mind.set("intelligence", RpcValue::text(mb->mind ? mb->mind->name() : ""));
      mind.set("cycles", RpcValue::number(mb->cycles));
      out.set("mind", std::move(mind));
    }
    return out;
  }

  return error_answer(op, id, "unknown op");
}

}  // namespace

Result<void> agent_control_serve(Kernel& k, Surface& svc, const AgentControlOptions& o,
                                 std::function<void(const std::string&)> log,
                                 std::atomic<bool>* stop) {
  auto listener = hal::net_listen(o.port, o.loopback_only);
  if (!listener.ok()) return listener.error();

  log(std::string("agent-control listening on ") + (o.loopback_only ? "127.0.0.1" : "0.0.0.0") + ":" +
      std::to_string((*listener)->port()) + (o.auth_secret.empty() ? " (NO AUTH -- loopback dev mode)" : " auth=required"));

  struct Handler {
    std::atomic<bool> finished{false};
    hal::Thread thread;
  };
  std::deque<std::unique_ptr<Handler>> handlers;
  AgentRegistry registry;
  CycleLog cycles(k.config().root + "/cycle-records.jsonl");
  ReplayGuard guard;

  while (!(stop && stop->load())) {
    for (auto it = handlers.begin(); it != handlers.end();) {
      if ((*it)->finished.load()) { it = handlers.erase(it); } else { ++it; }
    }

    auto conn = (*listener)->accept(200);
    if (!conn.ok()) { if (conn.error().status == Status::Busy) continue; log("accept: " + conn.error().str()); hal::sleep_ms(50); continue; }
    std::shared_ptr<hal::Connection> c(std::move(*conn));

    auto h = std::make_unique<Handler>();
    Handler* hp = h.get();
    hp->thread = hal::Thread([&k, &svc, &o, &registry, &cycles, &guard, c, hp]() {
      std::string body;
      if (read_frame(*c, body, o.frame_timeout_ms)) {
        auto parsed = rpc_parse(body);
        if (parsed.ok() && parsed->kind() == RpcValue::Kind::Object) {
          RpcValue resp = handle(k, svc, o, registry, cycles, guard, *parsed);
          write_frame(*c, resp.render());
        } else {
          write_frame(*c, error_answer("", "", "request is not a JSON object").render());
        }
      }
      c->close();
      hp->finished.store(true);
    });
    handlers.push_back(std::move(h));
  }

  // Remaining handlers (in-flight or just-finished) join as `handlers`
  // destructs below -- deque destruction runs each unique_ptr<Handler>'s
  // destructor, which joins its Thread.
  return Ok();
}

}  // namespace aios::channel
