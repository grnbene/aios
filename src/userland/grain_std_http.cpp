// grain's HTTP client, over aios's own (channel/http_client: hal::net and the
// in-tree TLS 1.2/1.3 with certificate verification against a trust store).
//
//   http_get(url [, options])            -> response
//   http_post(url, body [, options])     -> response
//   http_request(options)                -> response; options.url and options.method
//
// options: method ("GET"), headers (a map), body (a string), timeout_ms (20000),
// follow_redirects (true: up to 5 hops; 303, and 301/302 after a POST, go on as
// a GET; 307/308 repeat the method and body). A User-Agent of "grain" is sent
// unless the headers name one.
// A response is {status, reason, ok (status 200-299), headers, body}; headers
// are keyed by lower-case name, repeated ones joined with ", ". An HTTP error
// status is a response like any other; failing to get one at all (no such
// host, refused, a certificate that does not verify, a timeout) is an error,
// which pcall can catch.
//
// Not part of install_core_builtins: the network is a capability its embedder
// grants, as with sockets. https verifies against the trust store under the
// root given to set_http_trust_root (its ssl/ca-certificates.crt when there is
// one, else the system's: /etc/ssl, or on Windows the system store exported
// there).
#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <vector>

#include "aios/channel/http_client.hpp"
#include "aios/hal/host.hpp"
#include "aios/userland/grain.hpp"
#include "grain_builtins_internal.hpp"

namespace aios::userland::grain {

namespace {

std::mutex g_root_mu;
std::string g_trust_root;

std::string trust_root() {
  std::lock_guard<std::mutex> lk(g_root_mu);
  return g_trust_root;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

Value response_value(const channel::HttpResponse& r) {
  auto m = std::make_shared<MapObj>();
  m->put("status", Value::integer(r.status));
  m->put("reason", Value::string(r.reason));
  m->put("ok", Value::boolean(r.status >= 200 && r.status <= 299));
  auto h = std::make_shared<MapObj>();
  for (const auto& [name, value] : r.headers) {
    const std::string k = lower(name);
    if (Value* have = h->find(k)) *have = Value::string(have->str() + ", " + value);
    else h->put(k, Value::string(value));
  }
  m->put("headers", Value::object(Type::Map, h));
  m->put("body", Value::string(std::string(reinterpret_cast<const char*>(r.body.data()), r.body.size())));
  return Value::object(Type::Map, m);
}

// Fills `req` from an options map; the url and method may already be set.
bool apply_options(const Value& opt, channel::HttpRequest& req, std::string& url, bool& follow, const char* who, std::string& error) {
  if (opt.type == Type::Nil) return true;
  if (opt.type != Type::Map) { error = std::string(who) + "() wants its options as a map, got a " + type_name(opt.type); return false; }
  auto* m = static_cast<MapObj*>(opt.obj.get());
  if (Value* v = m->find("url")) {
    if (!want_str(*v, error, who)) return false;
    url = v->str();
  }
  if (Value* v = m->find("method")) {
    if (!want_str(*v, error, who)) return false;
    req.method = v->str();
    for (char& c : req.method) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  if (Value* v = m->find("headers")) {
    if (v->type != Type::Map) { error = std::string(who) + "(): headers is a map of name to value"; return false; }
    for (const auto& [name, value] : static_cast<MapObj*>(v->obj.get())->entries) req.headers.emplace_back(name, value.to_text());
  }
  if (Value* v = m->find("body")) {
    if (!want_str(*v, error, who)) return false;
    req.body.assign(v->str().begin(), v->str().end());
  }
  if (Value* v = m->find("follow_redirects")) follow = v->truthy();
  if (Value* v = m->find("timeout_ms")) {
    if (!want_number(*v, error, who)) return false;
    if (v->as_int() <= 0) { error = std::string(who) + "(): timeout_ms must be positive"; return false; }
    req.timeout_ms = static_cast<int>(std::min<int64_t>(v->as_int(), 24LL * 3600 * 1000));
  }
  return true;
}

// Where a Location header points, from the URL that answered with it.
std::string resolve(const std::string& base, const std::string& loc) {
  if (loc.find("://") != std::string::npos) return loc;
  const size_t scheme = base.find("://");
  const size_t path_at = scheme == std::string::npos ? std::string::npos : base.find('/', scheme + 3);
  const std::string origin = path_at == std::string::npos ? base : base.substr(0, path_at);
  if (loc.rfind("//", 0) == 0) return base.substr(0, scheme + 1) + loc;
  if (!loc.empty() && loc[0] == '/') return origin + loc;
  std::string dir = path_at == std::string::npos ? "/" : base.substr(path_at);
  if (const size_t q = dir.find_first_of("?#"); q != std::string::npos) dir.resize(q);
  dir.resize(dir.rfind('/') + 1);
  return origin + dir + loc;
}

Value send(channel::HttpRequest& req, std::string url, bool follow, const char* who, std::string& error) {
  if (url.empty()) { error = std::string(who) + "() needs a url"; return Value::nil(); }
  const bool has_agent = std::any_of(req.headers.begin(), req.headers.end(), [](const auto& h) { return lower(h.first) == "user-agent"; });
  if (!has_agent) req.headers.emplace_back("User-Agent", "grain");
  constexpr int kMaxHops = 5;
  for (int hop = 0;; ++hop) {
    auto ep = channel::parse_endpoint(url);
    if (!ep.ok()) { error = std::string(who) + "(\"" + url + "\"): " + ep.error().message; return Value::nil(); }
    req.endpoint = *ep;
    req.host = req.endpoint.secure ? &hal::process_host() : nullptr;
    req.kernel_root = req.endpoint.secure ? trust_root() : std::string();
    auto r = channel::http_send(req);
    if (!r.ok()) { error = std::string(who) + "(\"" + url + "\"): " + r.error().message; return Value::nil(); }
    const int st = r->status;
    const std::string loc = r->header("Location");
    if (!follow || loc.empty() || (st != 301 && st != 302 && st != 303 && st != 307 && st != 308)) return response_value(*r);
    if (hop + 1 > kMaxHops) { error = std::string(who) + "(\"" + url + "\"): more than " + std::to_string(kMaxHops) + " redirects"; return Value::nil(); }
    if (st == 303 || ((st == 301 || st == 302) && req.method == "POST")) {
      req.method = "GET";
      req.body.clear();
    }
    url = resolve(url, loc);
  }
}

Value bi_http_get(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "http_get")) return Value::nil();
  channel::HttpRequest req;
  std::string url = args[0].str();
  bool follow = true;
  if (!apply_options(args.size() > 1 ? args[1] : Value::nil(), req, url, follow, "http_get", error)) return Value::nil();
  req.method = "GET";
  return send(req, url, follow, "http_get", error);
}

Value bi_http_post(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "http_post") || !want_str(args[1], error, "http_post")) return Value::nil();
  channel::HttpRequest req;
  std::string url = args[0].str();
  bool follow = true;
  if (!apply_options(args.size() > 2 ? args[2] : Value::nil(), req, url, follow, "http_post", error)) return Value::nil();
  req.method = "POST";
  req.body.assign(args[1].str().begin(), args[1].str().end());
  return send(req, url, follow, "http_post", error);
}

Value bi_http_request(Vm&, std::vector<Value>& args, std::string& error) {
  channel::HttpRequest req;
  std::string url;
  bool follow = true;
  if (!apply_options(args[0], req, url, follow, "http_request", error)) return Value::nil();
  return send(req, url, follow, "http_request", error);
}

}  // namespace

std::string grain_http_trust_root() { return trust_root(); }
void install_events_builtins(Vm& vm);   // grain_std_events.cpp

void set_http_trust_root(const std::string& root) {
  std::lock_guard<std::mutex> lk(g_root_mu);
  g_trust_root = root;
}

void install_http_builtins(Vm& vm) {
  vm.define_native("http_get", 1, 2, bi_http_get);
  vm.define_native("http_post", 2, 3, bi_http_post);
  vm.define_native("http_request", 1, 1, bi_http_request);
  install_events_builtins(vm);   // server-sent events: the same capability, a stream instead of one answer
}

}  // namespace aios::userland::grain
