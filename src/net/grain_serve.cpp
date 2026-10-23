// A web site written in grain.
//
//   fn handle(req) {
//     if req.path == "/hello" { return text("hello " + (req.query.name ?? "world")) }
//     if req.method == "POST" { return json({"you_sent": req.form}) }
//     return {"status": 404, "body": "no such page"}
//   }
//
// A script with no handle() is a page: what it prints is the response. One that builds a ui.pane
// is a web app (see grain_webui.hpp).
//
// The script is compiled once per version of the file and run for each request
// in a VM of its own: there is no state between requests except what it keeps
// in files, so two requests can never see each other's variables and a crash in
// one cannot affect another. (Compiled scripts are pooled; one is used by one
// request at a time. Editing the file takes effect on the next request.)
//
// `req` is a map: method, path, target, query_string, query (a map), headers
// (a map, names in lower case), body (bytes), text (the body as a string),
// form (a map, when the body is application/x-www-form-urlencoded), ip, host,
// https. A body larger than the vhost's grain_max_body, or chunked, is not read
// up front: body, text and form are nil and streamed is true, and the script
// reads it with body_read(n), which returns up to n bytes and an empty bytes at
// the end. body_read works for small bodies too. `handle` returns a string (200, text/html), bytes (200,
// application/octet-stream), nil (204), or a response map {status, headers,
// body}, where body is a string, bytes, or a list or map (sent as JSON). The
// helpers text(s), json(value), redirect(url) build the common ones, each with
// an optional status.
//
// What the script may use is the standard library and the HTTP client, with
// file access confined to the site's root: no sockets, no processes, no
// windows, no FFI, no plugins. Native functions are available only when the
// process turned them on (grain::set_native_policy), and then only the safe
// kind. A request is held to `max_steps` instructions.
#include "aios/net/grain_serve.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>   // std::lock_guard
#include "aios/hal/sync.hpp"

#include "aios/userland/grain.hpp"

namespace aios::net {

namespace {

namespace g = aios::userland::grain;

// All file access of a handler, kept under the site root.
class RootedFiles final : public g::Files {
 public:
  RootedFiles(hal::Host& host, std::string root) : host_(host), root_(std::move(root)) {
    while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
  }

  bool read(const std::string& path, std::string& out, std::string& error) override {
    std::string full;
    if (!resolve(path, full, error)) return false;
    auto r = host_.read_file(full);
    if (!r.ok()) { error = "cannot read " + path; return false; }
    out.assign(r->begin(), r->end());
    return true;
  }
  bool write(const std::string& path, const std::string& data, bool append, std::string& error) override {
    std::string full;
    if (!resolve(path, full, error)) return false;
    const aios::ByteSpan span(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    auto r = append ? host_.append_file(full, span) : host_.write_file(full, span);
    if (!r.ok()) { error = "cannot write " + path; return false; }
    return true;
  }
  bool list(const std::string& path, std::vector<g::FileEntry>& out, std::string& error) override {
    std::string full;
    if (!resolve(path.empty() ? "." : path, full, error)) return false;
    auto names = host_.list_dir(full);
    if (!names.ok()) { error = "cannot list " + path; return false; }
    for (const auto& n : *names) {
      g::FileEntry e;
      e.name = n;
      auto st = host_.stat(full + "/" + n);
      if (st.ok()) { e.size = (int64_t)st->size; e.dir = st->is_dir; }
      out.push_back(e);
    }
    return true;
  }
  bool remove(const std::string& path, std::string& error) override {
    std::string full;
    if (!resolve(path, full, error)) return false;
    auto r = host_.remove(full);
    if (!r.ok()) { error = "cannot remove " + path; return false; }
    return true;
  }
  bool exists(const std::string& path) override {
    std::string full, error;
    return resolve(path, full, error) && host_.exists(full);
  }
  bool make_dir(const std::string& path, bool parents, std::string& error) override {
    std::string full;
    if (!resolve(path, full, error)) return false;
    if (parents) {   // each missing level, outermost first
      std::string at = root_;
      size_t i = 0;
      while (i < path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        if (j > i) {
          at += "/" + path.substr(i, j - i);
          if (!host_.exists(at) && !host_.make_dir(at).ok()) { error = "cannot make " + path; return false; }
        }
        i = j + 1;
      }
      return true;
    }
    if (!host_.make_dir(full).ok()) { error = "cannot make " + path; return false; }
    return true;
  }
  bool remove_dir(const std::string& path, std::string& error) override { return remove(path, error); }
  bool is_dir(const std::string& path) override {
    std::string full, error;
    if (!resolve(path, full, error)) return false;
    auto st = host_.stat(full);
    return st.ok() && st->is_dir;
  }

 private:
  hal::Host& host_;
  std::string root_;

  // A relative path inside the root; no "..", no absolute paths, no NULs.
  bool resolve(const std::string& path, std::string& full, std::string& error) {
    if (path.find('\0') != std::string::npos) { error = "a path cannot hold a NUL"; return false; }
    if (!path.empty() && (path[0] == '/' || path[0] == '\\')) { error = "paths are relative to the site's root: " + path; return false; }
    for (size_t i = 0; i <= path.size();) {
      size_t j = path.find_first_of("/\\", i);
      if (j == std::string::npos) j = path.size();
      if (path.compare(i, j - i, "..") == 0) { error = "a path cannot leave the site's root: " + path; return false; }
      i = j + 1;
    }
    full = path.empty() || path == "." ? root_ : root_ + "/" + path;
    return true;
  }
};

std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string url_decode(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') out += ' ';
    else if (s[i] == '%' && i + 2 < s.size() + 0 && hex_value(s[i + 1]) >= 0 && hex_value(s[i + 2]) >= 0) {
      out += (char)(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2]));
      i += 2;
    } else out += s[i];
  }
  return out;
}

// a=1&b=two -> {a: "1", b: "two"}; a repeated name keeps its last value
g::Value parse_form(const std::string& s) {
  auto m = std::make_shared<g::MapObj>();
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find('&', i);
    if (j == std::string::npos) j = s.size();
    const std::string pair = s.substr(i, j - i);
    if (!pair.empty()) {
      const size_t eq = pair.find('=');
      const std::string key = url_decode(eq == std::string::npos ? pair : pair.substr(0, eq));
      const std::string val = eq == std::string::npos ? "" : url_decode(pair.substr(eq + 1));
      if (!key.empty()) m->put(key, g::Value::string(val));
    }
    i = j + 1;
  }
  return g::Value::object(g::Type::Map, m);
}

// ---- the helpers a handler builds responses with ------------------------------------

g::Value response(int64_t status, const std::string& content_type, g::Value body) {
  auto m = std::make_shared<g::MapObj>();
  auto h = std::make_shared<g::MapObj>();
  if (!content_type.empty()) h->put("content-type", g::Value::string(content_type));
  m->put("status", g::Value::integer(status));
  m->put("headers", g::Value::object(g::Type::Map, h));
  m->put("body", std::move(body));
  return g::Value::object(g::Type::Map, m);
}

int64_t status_arg(std::vector<g::Value>& args, size_t at, int64_t dflt, std::string& error, const char* who) {
  if (args.size() <= at) return dflt;
  if (args[at].type != g::Type::Int || args[at].i < 100 || args[at].i > 599) { error = std::string(who) + "(): a status is a whole number from 100 to 599"; return dflt; }
  return args[at].i;
}

g::Value bi_text(g::Vm&, std::vector<g::Value>& args, std::string& error) {
  const int64_t status = status_arg(args, 1, 200, error, "text");
  if (!error.empty()) return g::Value::nil();
  return response(status, "text/plain; charset=utf-8", g::Value::string(args[0].type == g::Type::Str ? args[0].str() : args[0].to_text()));
}

g::Value bi_json(g::Vm& vm, std::vector<g::Value>& args, std::string& error) {
  const int64_t status = status_arg(args, 1, 200, error, "json");
  if (!error.empty()) return g::Value::nil();
  g::Value* stringify = vm.find_global("json_stringify");
  if (!stringify) { error = "json(): this VM has no json_stringify"; return g::Value::nil(); }
  g::Value text = vm.call(*stringify, {args[0]}, error);
  if (!error.empty()) return g::Value::nil();
  return response(status, "application/json", std::move(text));
}

g::Value bi_redirect(g::Vm&, std::vector<g::Value>& args, std::string& error) {
  if (args[0].type != g::Type::Str) { error = "redirect() wants a URL"; return g::Value::nil(); }
  const int64_t status = status_arg(args, 1, 302, error, "redirect");
  if (!error.empty()) return g::Value::nil();
  g::Value r = response(status, "", g::Value::string(""));
  static_cast<g::MapObj*>(static_cast<g::MapObj*>(r.obj.get())->find("headers")->obj.get())->put("location", args[0]);
  return r;
}

// The request being served on this thread, for body_read().
thread_local const GrainRequest* t_request = nullptr;
thread_local int t_body_error = 0;

// body_read(n?) -> bytes: up to n (default and most 65536) of the request body; empty at its end
g::Value bi_body_read(g::Vm&, std::vector<g::Value>& args, std::string& error) {
  int64_t n = 65536;
  if (!args.empty()) {
    if (args[0].type != g::Type::Int || args[0].i < 1) { error = "body_read() wants a number of bytes (at least 1)"; return g::Value::nil(); }
    n = std::min<int64_t>(args[0].i, 1 << 20);
  }
  std::vector<uint8_t> chunk;
  if (!t_request || !t_request->read_body) return g::Value::bytes({});
  const int rc = t_request->read_body((size_t)n, chunk);
  if (rc != 0) {
    t_body_error = rc;
    error = rc == 2 ? "the request body is larger than this site allows" : "the request body could not be read";
    return g::Value::nil();
  }
  return g::Value::bytes(std::move(chunk));
}

// Compiled scripts, kept for reuse. A compiled script keeps a lookup cache that belongs to one VM at a
// time, so each is lent to one request and returned; the pool is keyed by the file's version.
struct ScriptPool {
  std::string version;
  std::vector<std::shared_ptr<g::FnObj>> idle;
};
aios::hal::Mutex g_pool_mutex;   // the host layer's lock: the core does not call pthreads itself
std::map<std::string, ScriptPool> g_pools;
constexpr size_t kPoolMax = 64;

std::shared_ptr<g::FnObj> borrow(const std::string& path, const std::string& version) {
  std::lock_guard<aios::hal::Mutex> lock(g_pool_mutex);
  ScriptPool& pool = g_pools[path];
  if (pool.version != version) { pool.version = version; pool.idle.clear(); }
  if (pool.idle.empty()) return nullptr;
  auto script = std::move(pool.idle.back());
  pool.idle.pop_back();
  return script;
}

void give_back(const std::string& path, const std::string& version, std::shared_ptr<g::FnObj> script) {
  std::lock_guard<aios::hal::Mutex> lock(g_pool_mutex);
  ScriptPool& pool = g_pools[path];
  if (pool.version == version && pool.idle.size() < kPoolMax) pool.idle.push_back(std::move(script));
}

bool header_ok(const std::string& s) { return s.find_first_of("\r\n") == std::string::npos && s.find('\0') == std::string::npos; }

}  // namespace

std::shared_ptr<aios::userland::grain::Files> make_rooted_files(hal::Host& host, const std::string& root) {
  return std::make_shared<RootedFiles>(host, root);
}

bool grain_serve_request(const GrainSite& site, const GrainRequest& req, GrainResponse& out) {
  if (!site.host) { out.error = "no host"; return false; }
  const std::string app_path = !site.app.empty() && site.app[0] == '/' ? site.app : site.root + "/" + site.app;
  auto st = site.host->stat(app_path);
  if (!st.ok() || st->is_dir) { out.error = "cannot read the site's script " + app_path; return false; }
  const std::string version = std::to_string(st->mtime_ns) + ":" + std::to_string(st->size);

  std::shared_ptr<g::FnObj> script = borrow(app_path, version);
  if (!script) {
    auto src = site.host->read_file(app_path);
    if (!src.ok()) { out.error = "cannot read the site's script " + app_path; return false; }
    const std::string source(src->begin(), src->end());
    g::CompileResult compiled = g::compile_source(source, site.app, false);
    if (!compiled.ok()) {
      out.error = "the site's script does not compile:";
      for (const auto& d : compiled.errors) out.error += "\n  " + site.app + ":" + std::to_string(d.line) + ": " + d.message;
      return false;
    }
    script = compiled.script;
  }
  // Hand the script back for the next request however this one ends.
  struct Return {
    const std::string& path; const std::string& version; std::shared_ptr<g::FnObj>& script;
    ~Return() { give_back(path, version, std::move(script)); }
  } giver{app_path, version, script};

  RootedFiles files(*site.host, site.root);
  g::Vm vm;
  std::vector<std::string> printed;
  vm.sink = &printed;
  vm.files = &files;
  vm.step_limit = site.max_steps;
  g::install_core_builtins(vm);
  g::install_http_builtins(vm);
  g::install_db_builtins(vm);
  vm.define_native("text", 1, 2, bi_text);
  vm.define_native("json", 1, 2, bi_json);
  vm.define_native("redirect", 1, 2, bi_redirect);
  vm.define_native("body_read", 0, 1, bi_body_read);
  // A program written for a window also loads here (to be told it is a web app): no loop to run, no arguments.
  vm.define_native("run_panes", 0, 1, [](g::Vm&, std::vector<g::Value>&, std::string&) { return g::Value::nil(); });
  vm.define_global("args", g::Value::list({}));

  t_request = &req;
  t_body_error = 0;
  struct Clear { ~Clear() { t_request = nullptr; } } clear_request;

  auto run = vm.run(script);
  if (!run.ok) { out.error = "the site's script failed: " + run.error + " (line " + std::to_string(run.line) + ")"; return false; }
  g::Value* handle = vm.find_global("handle");
  if (!handle || handle->type != g::Type::Fn) {
    // A page script: what it prints is the page, as a PHP file's output is.
    g::WebPaneInfo pane;
    if (g::web_pane_first(vm, pane)) { out.ui_app = true; return true; }   // it builds a ui.pane: a web app
    std::string text;
    for (const auto& line : printed) text += line + "\n";
    out.status = 200;
    out.headers.push_back({"Content-Type", "text/html; charset=utf-8"});
    out.body.assign(text.begin(), text.end());
    return true;
  }

  // the request, as the script sees it
  auto r = std::make_shared<g::MapObj>();
  r->put("method", g::Value::string(req.method));
  r->put("path", g::Value::string(req.path));
  r->put("target", g::Value::string(req.target));
  r->put("query_string", g::Value::string(req.query));
  r->put("query", parse_form(req.query));
  auto headers = std::make_shared<g::MapObj>();
  std::string content_type;
  for (const auto& [name, value] : req.headers) {
    const std::string key = lower(name);
    if (key == "content-type") content_type = lower(value);
    if (g::Value* have = headers->find(key)) headers->put(key, g::Value::string(have->str() + ", " + value));
    else headers->put(key, g::Value::string(value));
  }
  r->put("headers", g::Value::object(g::Type::Map, headers));
  r->put("streamed", g::Value::boolean(req.body_streamed));
  if (req.body_streamed) {
    r->put("body", g::Value::nil());
    r->put("text", g::Value::nil());
    r->put("form", g::Value::nil());
  } else {
    const std::string body_text(req.body.begin(), req.body.end());
    r->put("body", g::Value::bytes(req.body));
    r->put("text", g::Value::string(body_text));
    r->put("form", content_type.rfind("application/x-www-form-urlencoded", 0) == 0 ? parse_form(body_text) : g::Value::object(g::Type::Map, std::make_shared<g::MapObj>()));
  }
  r->put("ip", g::Value::string(req.client_ip));
  r->put("host", g::Value::string(req.host));
  r->put("https", g::Value::boolean(req.https));

  std::string error;
  g::Value result = vm.call(*handle, {g::Value::object(g::Type::Map, r)}, error);
  if (!error.empty()) { out.error = "handle() failed: " + error; out.body_error = t_body_error; return false; }

  auto set_body = [&](const g::Value& body, bool& is_json) -> bool {
    is_json = false;
    if (body.type == g::Type::Nil) return true;
    if (body.type == g::Type::Str) { out.body.assign(body.str().begin(), body.str().end()); return true; }
    if (body.type == g::Type::Bytes) { out.body = body.bytes_data(); return true; }
    if (body.type == g::Type::Map || body.type == g::Type::List) {
      g::Value* stringify = vm.find_global("json_stringify");
      std::string e;
      g::Value text = stringify ? vm.call(*stringify, {body}, e) : g::Value::nil();
      if (!e.empty() || text.type != g::Type::Str) { out.error = "a response body could not be turned into JSON: " + e; return false; }
      out.body.assign(text.str().begin(), text.str().end());
      is_json = true;
      return true;
    }
    const std::string t = body.to_text();
    out.body.assign(t.begin(), t.end());
    return true;
  };

  bool is_json = false;
  switch (result.type) {
    case g::Type::Nil: out.status = 204; return true;
    case g::Type::Str:
      out.status = 200;
      out.headers.push_back({"Content-Type", "text/html; charset=utf-8"});
      return set_body(result, is_json);
    case g::Type::Bytes:
      out.status = 200;
      out.headers.push_back({"Content-Type", "application/octet-stream"});
      return set_body(result, is_json);
    case g::Type::Map: {
      auto* m = static_cast<g::MapObj*>(result.obj.get());
      if (g::Value* st = m->find("status")) {
        if (st->type != g::Type::Int || st->i < 100 || st->i > 599) { out.error = "a response's status is a whole number from 100 to 599"; return false; }
        out.status = (int)st->i;
      }
      bool has_type = false;
      if (g::Value* hv = m->find("headers")) {
        if (hv->type != g::Type::Map) { out.error = "a response's headers are a map"; return false; }
        for (auto& [k, v] : static_cast<g::MapObj*>(hv->obj.get())->entries) {
          const std::string val = v.type == g::Type::Str ? v.str() : v.to_text();
          if (!header_ok(k) || !header_ok(val) || k.empty()) continue;   // never let a header name or value carry a line break
          if (lower(k) == "content-type") has_type = true;
          const std::string low = lower(k);
          if (low == "content-length" || low == "transfer-encoding" || low == "connection") continue;   // the server frames the response
          out.headers.push_back({k, val});
        }
      }
      if (g::Value* body = m->find("body")) {
        if (!set_body(*body, is_json)) return false;
      }
      if (!has_type) out.headers.push_back({"Content-Type", is_json ? "application/json" : "text/html; charset=utf-8"});
      return true;
    }
    default:
      out.error = std::string("handle() returned a ") + g::type_name(result.type) + "; return a string, bytes, nil, or a response map";
      return false;
  }
}

}  // namespace aios::net
