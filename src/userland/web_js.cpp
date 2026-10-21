// The page's JavaScript: the engine in src/js bound to the document the layout engine walks.
//
// The engine itself (parser, bytecode compiler, VM, garbage collector, the ES builtins) is src/js. The DOM, events,
// network surface, storage and canvas are src/userland/web_js/*.cpp. This file is what makes them a page: it installs
// the globals (window, location, history, navigator, timers, performance, crypto, aos), runs a document's scripts in
// order with each one isolated from the others' failures, and carries the pane's frames, keys and clicks into the
// page's heap through the JsRuntime interface.
//
// No exceptions and no RTTI: this is bare-metal CORE code.
#include <cstdio>
#include <cstdlib>
#include "aios/userland/web_js.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "web_js/page.hpp"

namespace aios::userland {

namespace {
double (*g_wall_ms)() = nullptr;
// The instant a page with no bound clock believes it is.
constexpr double kNoClockMs = 1757433600000.0;   // 2026-09-09T12:00:00Z
}  // namespace

void js_set_clock(double (*wall_ms)()) { g_wall_ms = wall_ms; }

namespace webjs {

using js::PropKey;
using js::Tracer;

// ---------------------------------------------------------------------------------------------------
// the Page
// ---------------------------------------------------------------------------------------------------

Page::Page() : ctx_owner(std::make_unique<Context>()), ctx(*ctx_owner) {}
Page::~Page() = default;

namespace {

double wall_ms(Page& P) {
  if (P.has_host && P.host.now_ms) return P.host.now_ms();
  return g_wall_ms ? g_wall_ms() : kNoClockMs;
}

// Time as the page measures it: milliseconds since it began. With a real clock bound this is the wall clock, so a busy
// wait on performance.now() ends; without one, page time is only what the pane's frames have added.
double page_now(Page& P) { return P.has_clock ? std::max(0.0, wall_ms(P) - P.wall_origin) : P.now_ms; }

Value str(Page& P, const std::string& s) { return Value::String(P.ctx.new_string_utf8(s)); }
Value ascii(Page& P, const char* s) { return Value::String(P.ctx.atom_ascii(s)); }

void def_value(Page& P, Object* o, const char* name, Value v, uint8_t attrs = js::kDefaultAttrs) { P.ctx.define_data(o, P.ctx.atom_utf8(name), v, attrs); }

// ---- faults -----------------------------------------------------------------------------------

// Take the pending exception, record it as the page's last fault, and tell the page's own `error` handlers.
void fault(Page& P, const char* where) {
  Context& c = P.ctx;
  if (c.step_budget_exceeded) {
    c.clear_exception();
    c.step_budget_exceeded = false;
    P.last_fault = std::string(where) + ": this script ran too long and was stopped (over " + std::to_string(Page::kStepBudget) + " steps)";
    if (P.dispatch_error.empty()) P.dispatch_error = P.last_fault;
    P.log.push_back("Uncaught " + P.last_fault);
    return;
  }
  if (!c.has_exception()) return;
  Value ex = c.take_exception();
  Pin pin(P);
  pin.add(ex);
  const std::string msg = c.describe_exception(ex);
  P.last_fault = std::string(where) + ": " + msg;
  if (P.dispatch_error.empty()) P.dispatch_error = P.last_fault;
  P.log.push_back("Uncaught " + msg);
  if (std::getenv("AIOS_JS_ERRORS")) std::fprintf(stderr, "[js error] %s: %s\n", where, msg.c_str());
  static bool in_error_event = false;
  if (in_error_event || !P.window_obj) return;
  in_error_event = true;
  Object* ev = make_event(P, "error", "ErrorEvent", false, true);
  pin.add(Value::Obj(ev));
  js::JsString* head = nullptr;
  Value message = str(P, "Uncaught " + msg);
  if (c.to_string(ex, &head)) message = str(P, "Uncaught " + head->utf8());
  else c.clear_exception();
  c.put(ev, c.atom_ascii("message"), message, false);
  c.put(ev, c.atom_ascii("error"), ex, false);
  dispatch_event_object(P, P.window_obj, P.window_obj, ev);
  c.clear_exception();
  in_error_event = false;
}

// Run one callback the host owns (a timer, a frame, an event listener) with a fresh budget and its microtasks.
bool run_callback(Page& P, const Value& fn, const Value& self, std::vector<Value> args, const char* where) {
  P.new_budget();
  Pin pin(P);
  pin.add(fn);
  pin.add(self);
  for (const Value& v : args) pin.add(v);
  Value r;
  bool ok = P.call(fn, self, std::move(args), &r);
  if (!ok) fault(P, where);
  if (!P.ctx.run_jobs()) fault(P, "promise job");
  return ok;
}

// ---- timers -----------------------------------------------------------------------------------

constexpr size_t kMaxTimers = 100000;

Value make_string_handler(Page& P, const std::string& code) {
  Page* pp = &P;
  return Value::Obj(P.ctx.new_host_function("", [pp, code](Context&, CallArgs&) -> Value {
    Value r;
    if (!pp->ctx.eval_script(code, "timer string", &r)) return Value::Exception();
    return r;
  }, 0));
}

Value set_timer(Page& P, CallArgs& a, bool repeat) {
  PendingTimer t;
  if (a[0].is_callable()) t.fn = a[0];
  else if (a[0].is_undefined() && a.argc == 0) {
    P.ctx.throw_type_error(std::string("Failed to execute '") + (repeat ? "setInterval" : "setTimeout") + "' on 'Window': 1 argument required, but only 0 present.");
    return Value::Exception();
  } else t.fn = make_string_handler(P, P.text(a[0]));
  double d = P.number(a[1]);
  if (!(d > 0)) d = 0;
  if (d > 2147483647.0) d = 2147483647.0;
  for (int i = 2; i < a.argc; ++i) t.args.push_back(a.argv[i]);
  if (P.deferred.size() + P.clocked.size() >= kMaxTimers) {
    P.ctx.throw_range_error("too many pending timers");
    return Value::Exception();
  }
  t.id = P.next_timer_id++;
  t.repeat = repeat;
  const int id = t.id;
  if (!repeat && d <= 0) {
    P.deferred.push_back(std::move(t));
  } else {
    t.every_ms = repeat ? std::max(d, 1.0) : 0;
    t.due_ms = P.now_ms + std::max(d, repeat ? 1.0 : 0.0);
    P.clocked.push_back(std::move(t));
  }
  return Value::Number(id);
}

void clear_timer(Page& P, int id) {
  if (id <= 0) return;
  auto drop = [id](std::vector<PendingTimer>& v) {
    for (size_t i = 0; i < v.size(); ++i)
      if (v[i].id == id) { v.erase(v.begin() + static_cast<long>(i)); return; }
  };
  drop(P.deferred);
  drop(P.clocked);
  drop(P.raf);
  for (PendingTimer& t : P.inflight)
    if (t.id == id) t.id = 0;   // already taken for this batch: the loop skips it
}

// Run the zero-delay timers and queued tasks. Only what was queued when the drain began runs in a pass: a callback that
// queues another waits for the next one, so `function f(){ setTimeout(f, 0) }` is a page that keeps working, not a hang.
void drain(Page& P, int max_passes) {
  for (int pass = 0; pass < max_passes && !P.deferred.empty(); ++pass) {
    const size_t base = P.inflight.size();
    for (PendingTimer& t : P.deferred) P.inflight.push_back(std::move(t));
    P.deferred.clear();
    const size_t n = P.inflight.size();
    for (size_t i = base; i < n; ++i) {
      if (P.inflight[i].id == 0) continue;
      Value fn = P.inflight[i].fn;
      std::vector<Value> args = P.inflight[i].args;
      run_callback(P, fn, Value::Undefined(), std::move(args), "timer");
    }
    P.inflight.resize(base);
  }
}

void run_animation_frames(Page& P) {
  if (P.raf.empty()) return;
  const size_t base = P.inflight.size();
  for (PendingTimer& t : P.raf) P.inflight.push_back(std::move(t));
  P.raf.clear();
  const size_t n = P.inflight.size();
  const double stamp = page_now(P);
  for (size_t i = base; i < n; ++i) {
    if (P.inflight[i].id == 0) continue;
    Value fn = P.inflight[i].fn;
    run_callback(P, fn, Value::Undefined(), {Value::Number(stamp)}, "animation frame");
  }
  P.inflight.resize(base);
}

// ---- JSON, for aos ----------------------------------------------------------------------------

bool json_call(Page& P, const char* method, const Value& arg, Value* out) {
  Value json, fn;
  Context& c = P.ctx;
  if (!c.get(Value::Obj(c.global()), c.atom_ascii("JSON"), &json) || !c.get(json, c.atom_utf8(method), &fn)) return false;
  return c.call(fn, json, {arg}, out);
}

std::string json_of(Page& P, const Value& v) {
  Value out;
  if (!json_call(P, "stringify", v, &out)) { P.ctx.clear_exception(); return "null"; }
  if (!out.is_string()) return "null";
  return out.as_string()->utf8();
}

Value json_parse(Page& P, const std::string& text) {
  Value out;
  if (!json_call(P, "parse", str(P, text), &out)) { P.ctx.clear_exception(); return Value::Null(); }
  return out;
}

// ---- aos --------------------------------------------------------------------------------------

uint32_t color_of(Page& P, const Value& v) {
  if (v.is_number()) {
    const uint64_t n = static_cast<uint64_t>(v.num);
    return n <= 0xFFFFFFull ? static_cast<uint32_t>(0xFF000000ull | n) : static_cast<uint32_t>(n);
  }
  std::string s = P.text(v);
  for (auto& ch : s)
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
  if (!s.empty() && s[0] == '#') {
    auto nib = [](char ch) -> uint32_t {
      if (ch >= '0' && ch <= '9') return static_cast<uint32_t>(ch - '0');
      if (ch >= 'a' && ch <= 'f') return static_cast<uint32_t>(ch - 'a' + 10);
      return 0;
    };
    if (s.size() == 4) return 0xFF000000u | (nib(s[1]) * 17u << 16) | (nib(s[2]) * 17u << 8) | (nib(s[3]) * 17u);
    uint32_t rgb = 0;
    for (size_t i = 1; i < s.size() && i < 7; ++i) rgb = (rgb << 4) | nib(s[i]);
    if (s.size() == 9) return (((nib(s[7]) << 4) | nib(s[8])) << 24) | rgb;
    return 0xFF000000u | rgb;
  }
  struct Named { const char* n; uint32_t c; };
  static const Named named[] = {
      {"white", 0xFFFFFFFF}, {"black", 0xFF000000}, {"red", 0xFFE53935}, {"green", 0xFF43A047}, {"blue", 0xFF1E88E5}, {"yellow", 0xFFFDD835}, {"orange", 0xFFFB8C00},
      {"purple", 0xFF8E24AA}, {"cyan", 0xFF00ACC1}, {"magenta", 0xFFD81B60}, {"gray", 0xFF9E9E9E}, {"grey", 0xFF9E9E9E}, {"silver", 0xFFBDBDBD}, {"navy", 0xFF1A237E},
      {"teal", 0xFF00897B}, {"lime", 0xFFC0CA33}, {"pink", 0xFFF06292}, {"brown", 0xFF6D4C41}, {"transparent", 0x00000000},
  };
  for (const auto& e : named)
    if (s == e.n) return e.c;
  return 0xFFFFFFFFu;
}

void paint_push(Page& P, PaintOp op) {
  if (op.op == PaintOp::Clear) P.paint.clear();
  if (P.paint.size() < 20000) P.paint.push_back(std::move(op));
}

void install_aos(Page& P) {
  Page* pp = &P;
  Context& c = P.ctx;
  Object* aos = c.new_object();
  Object* paint = c.new_object();
  auto rect_like = [pp](PaintOp::Kind kind) {
    return [pp, kind](Context&, CallArgs& a) -> Value {
      PaintOp op;
      op.op = kind;
      op.a = pp->number(a[0]); op.b = pp->number(a[1]); op.c = pp->number(a[2]); op.d = pp->number(a[3]);
      op.color = a.argc > 4 ? color_of(*pp, a[4]) : 0xFFFFFFFFu;
      paint_push(*pp, op);
      return Value::Undefined();
    };
  };
  def_method(P, paint, "clear", 1, [pp](Context&, CallArgs& a) -> Value {
    PaintOp op;
    op.op = PaintOp::Clear;
    op.color = a.argc ? color_of(*pp, a[0]) : 0xFF101010u;
    paint_push(*pp, op);
    return Value::Undefined();
  });
  def_method(P, paint, "rect", 5, rect_like(PaintOp::Rect));
  def_method(P, paint, "fillRect", 5, rect_like(PaintOp::Rect));
  def_method(P, paint, "outline", 5, rect_like(PaintOp::Outline));
  def_method(P, paint, "strokeRect", 5, rect_like(PaintOp::Outline));
  def_method(P, paint, "line", 5, rect_like(PaintOp::Line));
  def_method(P, paint, "text", 5, [pp](Context&, CallArgs& a) -> Value {
    PaintOp op;
    op.op = PaintOp::Text;
    op.a = pp->number(a[0]); op.b = pp->number(a[1]);
    op.text = pp->text(a[2]);
    op.color = a.argc > 3 ? color_of(*pp, a[3]) : 0xFFFFFFFFu;
    op.c = a.argc > 4 ? pp->number(a[4]) : 15;
    paint_push(*pp, op);
    return Value::Undefined();
  });
  def_method(P, paint, "circle", 4, [pp](Context&, CallArgs& a) -> Value {
    PaintOp op;
    op.op = PaintOp::Circle;
    op.a = pp->number(a[0]); op.b = pp->number(a[1]); op.c = pp->number(a[2]);
    op.color = a.argc > 3 ? color_of(*pp, a[3]) : 0xFFFFFFFFu;
    paint_push(*pp, op);
    return Value::Undefined();
  });
  def_value(P, aos, "paint", Value::Obj(paint));

  Object* store = c.new_object();
  def_method(P, store, "put", 2, [pp](Context&, CallArgs& a) -> Value {
    const std::string kind = pp->text(a[0]);
    if (kind.empty()) {
      pp->ctx.throw_type_error("aos.store.put(kind, object): a record needs a kind");
      return Value::Exception();
    }
    const std::string json = a[1].is_object() ? json_of(*pp, a[1]) : json_of(*pp, Value::String(pp->ctx.new_string_utf8(pp->text(a[1]))));
    if (!pp->has_host || !pp->host.store_put) return Value::String(pp->ctx.atoms.empty);
    return str(*pp, pp->host.store_put(kind, json));
  });
  def_method(P, store, "list", 1, [pp](Context&, CallArgs& a) -> Value {
    std::string json = pp->has_host && pp->host.store_list ? pp->host.store_list(pp->text(a[0])) : std::string("[]");
    if (json.empty()) json = "[]";
    Value v = json_parse(*pp, json);
    bool is_arr = false;
    pp->ctx.is_array(v, &is_arr);
    return is_arr ? v : make_array(*pp, {});
  });
  def_method(P, store, "get", 1, [pp](Context&, CallArgs& a) -> Value {
    const std::string json = pp->has_host && pp->host.store_get ? pp->host.store_get(pp->text(a[0])) : std::string();
    return json.empty() ? Value::Null() : json_parse(*pp, json);
  });
  def_method(P, store, "similar", 2, [pp](Context&, CallArgs& a) -> Value {
    int k = a.argc > 1 ? static_cast<int>(pp->number(a[1])) : 5;
    k = std::clamp(k, 1, 50);
    std::string json = pp->has_host && pp->host.store_similar ? pp->host.store_similar(pp->text(a[0]), k) : std::string("[]");
    if (json.empty()) json = "[]";
    Value v = json_parse(*pp, json);
    bool is_arr = false;
    pp->ctx.is_array(v, &is_arr);
    return is_arr ? v : make_array(*pp, {});
  });
  def_value(P, aos, "store", Value::Obj(store));

  def_method(P, aos, "size", 0, [pp](Context& cc, CallArgs&) -> Value {
    Object* o = cc.new_object();
    def_value(*pp, o, "w", Value::Number(pp->view_w));
    def_value(*pp, o, "h", Value::Number(pp->view_h));
    def_value(*pp, o, "width", Value::Number(pp->view_w));
    def_value(*pp, o, "height", Value::Number(pp->view_h));
    return Value::Obj(o);
  });
  def_accessor(P, aos, "width", [pp](Context&, CallArgs&) -> Value { return Value::Number(pp->view_w); });
  def_accessor(P, aos, "height", [pp](Context&, CallArgs&) -> Value { return Value::Number(pp->view_h); });
  def_method(P, aos, "now", 0, [pp](Context&, CallArgs&) -> Value { return Value::Number(std::floor(wall_ms(*pp))); });
  def_method(P, aos, "elapsed", 0, [pp](Context&, CallArgs&) -> Value { return Value::Number(pp->now_ms); });
  def_method(P, aos, "onFrame", 1, [pp](Context&, CallArgs& a) -> Value {
    if (a[0].is_callable() && pp->frame_fns.size() < 64) pp->frame_fns.push_back(a[0]);
    return Value::Undefined();
  });
  def_method(P, aos, "onKey", 1, [pp](Context&, CallArgs& a) -> Value {
    if (a[0].is_callable() && pp->key_fns.size() < 64) pp->key_fns.push_back(a[0]);
    return Value::Undefined();
  });
  def_method(P, aos, "say", 1, [pp](Context&, CallArgs& a) -> Value {
    std::string s;
    for (int i = 0; i < a.argc; ++i) {
      if (i) s += " ";
      s += pp->text(a.argv[i]);
    }
    pp->log.push_back(s);
    if (pp->has_host && pp->host.say) pp->host.say(s);
    return Value::Undefined();
  });
  def_method(P, aos, "organ", 2, [pp](Context&, CallArgs& a) -> Value {
    const std::string name = pp->text(a[0]);
    const std::string request = a[1].is_object() ? json_of(*pp, a[1]) : pp->text(a[1]);
    if (!pp->has_host || !pp->host.organ) return Value::String(pp->ctx.atoms.empty);
    return str(*pp, pp->host.organ(name, request));
  });
  P.keys_held = c.new_object();
  def_value(P, aos, "keys", Value::Obj(P.keys_held));
  def_value(P, aos, "bound", Value::Bool(P.has_host));
  def_value(P, c.global(), "aos", Value::Obj(aos), js::kHiddenAttrs);
}

// ---- location, history ------------------------------------------------------------------------

struct UrlParts {
  std::string scheme;    // "https:"
  std::string host;      // "example.com"
  std::string port;
  std::string path;
  std::string query;     // "?a=b" or ""
  std::string hash;      // "#x" or ""
  bool authority = false;
};

UrlParts split_url(const std::string& url) {
  UrlParts u;
  std::string rest = url;
  const size_t h = rest.find('#');
  if (h != std::string::npos) { u.hash = rest.substr(h); rest.resize(h); }
  const size_t q = rest.find('?');
  if (q != std::string::npos) { u.query = rest.substr(q); rest.resize(q); }
  const size_t colon = rest.find(':');
  if (colon != std::string::npos && colon > 0) {
    bool scheme_ok = true;
    for (size_t i = 0; i < colon; ++i)
      if (!std::isalnum(static_cast<unsigned char>(rest[i])) && rest[i] != '+' && rest[i] != '-' && rest[i] != '.') scheme_ok = false;
    if (scheme_ok) { u.scheme = rest.substr(0, colon + 1); rest = rest.substr(colon + 1); }
  }
  if (rest.compare(0, 2, "//") == 0) {
    u.authority = true;
    const size_t slash = rest.find('/', 2);
    std::string auth = rest.substr(2, slash == std::string::npos ? std::string::npos : slash - 2);
    u.path = slash == std::string::npos ? "/" : rest.substr(slash);
    const size_t at = auth.rfind('@');
    if (at != std::string::npos) auth = auth.substr(at + 1);
    const size_t pc = auth.rfind(':');
    if (pc != std::string::npos && auth.find(']', pc) == std::string::npos) { u.port = auth.substr(pc + 1); auth.resize(pc); }
    u.host = auth;
  } else {
    u.path = rest;
  }
  return u;
}

std::string origin_of(const UrlParts& u) {
  if (!u.authority || (u.scheme != "http:" && u.scheme != "https:")) return "null";
  const bool dflt = (u.scheme == "http:" && u.port == "80") || (u.scheme == "https:" && u.port == "443");
  return u.scheme + "//" + u.host + (u.port.empty() || dflt ? "" : ":" + u.port);
}

void request_navigation(Page& P, const std::string& url, const char* how) {
  // An address the pane can fetch is handed to it, to follow once the handler
  // is done. Anything else (`javascript:`, `about:`) has nowhere to go.
  if (url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0) {
    P.nav_url = url;
    return;
  }
  P.last_fault = std::string("navigation: ") + how + "(" + url + ") was asked for by the page; this pane does not follow navigation started by script";
  if (P.dispatch_error.empty()) P.dispatch_error = P.last_fault;
  P.log.push_back(P.last_fault);
}

void fire_deferred_event(Page& P, const char* type, const char* cls, std::function<void(Object*)> fill) {
  Page* pp = &P;
  PendingTimer t;
  t.id = P.next_timer_id++;
  std::string ty = type, cl = cls;
  t.fn = Value::Obj(P.ctx.new_host_function("", [pp, ty, cl, fill](Context&, CallArgs&) -> Value {
    Object* ev = make_event(*pp, ty, cl.c_str(), false, false);
    Pin pin(*pp);
    pin.add(Value::Obj(ev));
    if (fill) fill(ev);
    dispatch_event_object(*pp, pp->window_obj, pp->window_obj, ev);
    return Value::Undefined();
  }, 0));
  P.deferred.push_back(std::move(t));
}

// Move to a new same-document address, tell the page.
void set_location(Page& P, const std::string& url) {
  const std::string old = P.location_url;
  P.location_url = url;
  const UrlParts a = split_url(old), b = split_url(url);
  if (a.hash != b.hash && a.path == b.path && a.query == b.query && a.host == b.host) {
    fire_deferred_event(P, "hashchange", "HashChangeEvent", [pp = &P, old, url](Object* ev) {
      pp->ctx.put(ev, pp->ctx.atom_ascii("oldURL"), str(*pp, old), false);
      pp->ctx.put(ev, pp->ctx.atom_ascii("newURL"), str(*pp, url), false);
    });
  }
}

void history_go(Page& P, int delta) {
  const int target = P.hist_idx + delta;
  if (delta == 0 || target < 0 || target >= static_cast<int>(P.hist_urls.size())) return;
  P.hist_idx = target;
  set_location(P, P.hist_urls[static_cast<size_t>(target)]);
  Value st = P.hist_states[static_cast<size_t>(target)];
  fire_deferred_event(P, "popstate", "PopStateEvent", [pp = &P, st](Object* ev) { pp->ctx.put(ev, pp->ctx.atom_ascii("state"), st, false); });
}

void install_location_history(Page& P) {
  Page* pp = &P;
  Context& c = P.ctx;
  P.hist_urls.assign(1, P.location_url);
  P.hist_states.assign(1, Value::Null());
  P.hist_idx = 0;

  Object* lp = make_proto(P, c.intr.object_proto, "Location");
  P.location_obj = c.new_object(lp);
  auto part = [pp, lp](const char* name, std::function<std::string(const UrlParts&)> get, std::function<std::string(const UrlParts&, const std::string&)> set = nullptr) {
    def_accessor(*pp, lp, name, [pp, get](Context&, CallArgs&) -> Value { return str(*pp, get(split_url(pp->location_url))); },
                 set ? HostFn([pp, set](Context&, CallArgs& a) -> Value {
                   request_navigation(*pp, set(split_url(pp->location_url), pp->text(a[0])), "location assignment");
                   return Value::Undefined();
                 })
                     : HostFn(nullptr));
  };
  def_accessor(P, lp, "href", [pp](Context&, CallArgs&) -> Value { return str(*pp, pp->location_url); },
               [pp](Context&, CallArgs& a) -> Value {
                 request_navigation(*pp, resolve(*pp, pp->text(a[0])), "location.href");
                 return Value::Undefined();
               });
  part("protocol", [](const UrlParts& u) { return u.scheme; });
  part("host", [](const UrlParts& u) { return u.host + (u.port.empty() ? "" : ":" + u.port); });
  part("hostname", [](const UrlParts& u) { return u.host; });
  part("port", [](const UrlParts& u) { return u.port; });
  part("pathname", [](const UrlParts& u) { return u.path; });
  part("search", [](const UrlParts& u) { return u.query; });
  part("origin", [](const UrlParts& u) { return origin_of(u); });
  def_accessor(P, lp, "hash", [pp](Context&, CallArgs&) -> Value {
    const std::string h = split_url(pp->location_url).hash;
    return str(*pp, h == "#" ? std::string() : h);
  }, [pp](Context&, CallArgs& a) -> Value {
    std::string h = pp->text(a[0]);
    if (!h.empty() && h[0] != '#') h = "#" + h;
    const size_t cut = pp->location_url.find('#');
    set_location(*pp, (cut == std::string::npos ? pp->location_url : pp->location_url.substr(0, cut)) + h);
    pp->hist_urls[static_cast<size_t>(pp->hist_idx)] = pp->location_url;
    return Value::Undefined();
  });
  def_method(P, lp, "assign", 1, [pp](Context&, CallArgs& a) -> Value { request_navigation(*pp, resolve(*pp, pp->text(a[0])), "location.assign"); return Value::Undefined(); });
  def_method(P, lp, "replace", 1, [pp](Context&, CallArgs& a) -> Value { request_navigation(*pp, resolve(*pp, pp->text(a[0])), "location.replace"); return Value::Undefined(); });
  def_method(P, lp, "reload", 0, [pp](Context&, CallArgs&) -> Value { request_navigation(*pp, pp->location_url, "location.reload"); return Value::Undefined(); });
  def_method(P, lp, "toString", 0, [pp](Context&, CallArgs&) -> Value { return str(*pp, pp->location_url); });

  Object* g = c.global();
  def_accessor(P, g, "location", [pp](Context&, CallArgs&) -> Value { return Value::Obj(pp->location_obj); },
               [pp](Context&, CallArgs& a) -> Value { request_navigation(*pp, resolve(*pp, pp->text(a[0])), "location"); return Value::Undefined(); });
  def_accessor(P, P.document_proto, "location", [pp](Context&, CallArgs&) -> Value { return Value::Obj(pp->location_obj); },
               [pp](Context&, CallArgs& a) -> Value { request_navigation(*pp, resolve(*pp, pp->text(a[0])), "document.location"); return Value::Undefined(); });

  Object* hp = make_proto(P, c.intr.object_proto, "History");
  Object* history = c.new_object(hp);
  def_accessor(P, hp, "length", [pp](Context&, CallArgs&) -> Value { return Value::Number(static_cast<double>(pp->hist_urls.size())); });
  def_accessor(P, hp, "state", [pp](Context&, CallArgs&) -> Value { return pp->hist_states[static_cast<size_t>(pp->hist_idx)]; });
  def_accessor(P, hp, "scrollRestoration", [](Context& cc, CallArgs&) -> Value { return Value::String(cc.atom_ascii("auto")); }, [](Context&, CallArgs&) -> Value { return Value::Undefined(); });
  auto push = [pp](bool replace) {
    return [pp, replace](Context& cc, CallArgs& a) -> Value {
      std::string url = pp->location_url;
      if (a.argc > 2 && !a[2].is_nullish()) {
        url = resolve(*pp, pp->text(a[2]));
        const UrlParts to = split_url(url), from = split_url(pp->location_url);
        // file: documents have an opaque origin, but may still move within their own file
        const bool same_file = to.scheme == "file:" && from.scheme == "file:" && to.path == from.path;
        if (!same_file && (origin_of(to) != origin_of(from) || origin_of(to) == "null")) {
          Object* e = cc.new_error(js::ErrorKind::Error, "A history state object with URL '" + url + "' cannot be created in a document with origin '" + origin_of(split_url(pp->location_url)) + "'.");
          cc.define_data(e, cc.atoms.name, Value::String(cc.atom_ascii("SecurityError")), js::kHiddenAttrs);
          return cc.throw_value(Value::Obj(e));
        }
      }
      if (replace) {
        pp->hist_urls[static_cast<size_t>(pp->hist_idx)] = url;
        pp->hist_states[static_cast<size_t>(pp->hist_idx)] = a[0];
      } else {
        pp->hist_urls.resize(static_cast<size_t>(pp->hist_idx) + 1);
        pp->hist_states.resize(static_cast<size_t>(pp->hist_idx) + 1);
        pp->hist_urls.push_back(url);
        pp->hist_states.push_back(a[0]);
        pp->hist_idx = static_cast<int>(pp->hist_urls.size()) - 1;
      }
      pp->location_url = url;
      return Value::Undefined();
    };
  };
  def_method(P, hp, "pushState", 3, push(false));
  def_method(P, hp, "replaceState", 3, push(true));
  def_method(P, hp, "back", 0, [pp](Context&, CallArgs&) -> Value { history_go(*pp, -1); return Value::Undefined(); });
  def_method(P, hp, "forward", 0, [pp](Context&, CallArgs&) -> Value { history_go(*pp, 1); return Value::Undefined(); });
  def_method(P, hp, "go", 1, [pp](Context&, CallArgs& a) -> Value { history_go(*pp, static_cast<int>(pp->number(a[0]))); return Value::Undefined(); });
  def_value(P, g, "history", Value::Obj(history), js::kHiddenAttrs);
}

// ---- navigator, screen, window ----------------------------------------------------------------

void install_navigator_window(Page& P) {
  Page* pp = &P;
  Context& c = P.ctx;
  Object* g = c.global();

  Object* np = make_proto(P, c.intr.object_proto, "Navigator");
  Object* nav = c.new_object(np);
  auto nstr = [&](const char* name, const std::string& v) { def_accessor(P, np, name, [pp, v](Context&, CallArgs&) -> Value { return str(*pp, v); }); };
  nstr("userAgent", "AIOS/1.0 (this machine's own web engine)");
  nstr("appName", "Netscape");
  nstr("appVersion", "5.0 (AIOS)");
  nstr("platform", "aios");
  nstr("product", "Gecko");
  nstr("vendor", "");
  nstr("language", "en-US");
  def_accessor(P, np, "languages", [pp](Context&, CallArgs&) -> Value { return make_array(*pp, {ascii(*pp, "en-US"), ascii(*pp, "en")}); });
  def_accessor(P, np, "onLine", [](Context&, CallArgs&) -> Value { return Value::Bool(true); });
  def_accessor(P, np, "cookieEnabled", [](Context&, CallArgs&) -> Value { return Value::Bool(true); });
  def_accessor(P, np, "maxTouchPoints", [](Context&, CallArgs&) -> Value { return Value::Number(0); });
  def_accessor(P, np, "hardwareConcurrency", [pp](Context&, CallArgs&) -> Value {
    const std::string n = pp->has_host && pp->host.machine ? pp->host.machine("cpus") : std::string();
    const int v = n.empty() ? 1 : std::atoi(n.c_str());
    return Value::Number(v > 0 ? v : 1);
  });
  def_method(P, np, "sendBeacon", 2, [pp](Context&, CallArgs& a) -> Value {
    if (!pp->fetch) return Value::Bool(false);
    web::FetchOptions opt;
    opt.method = "POST";
    opt.body = a[1].is_undefined() ? std::string() : pp->text(a[1]);
    opt.headers.emplace_back("Content-Type", "text/plain;charset=UTF-8");
    const std::string url = resolve(*pp, pp->text(a[0]));
    auto got = pp->fetch(url, opt);
    pp->net.push_back("POST " + url + (got.ok() ? " -> " + std::to_string(got->status) : " failed"));
    return Value::Bool(got.ok() && got->status < 400);
  });
  def_value(P, g, "navigator", Value::Obj(nav), js::kHiddenAttrs);

  Object* sp = make_proto(P, c.intr.object_proto, "Screen");
  Object* screen = c.new_object(sp);
  for (const char* n : {"width", "availWidth"})
    def_accessor(P, sp, n, [pp](Context&, CallArgs&) -> Value { return Value::Number(pp->view_w); });
  for (const char* n : {"height", "availHeight"})
    def_accessor(P, sp, n, [pp](Context&, CallArgs&) -> Value { return Value::Number(pp->view_h); });
  for (const char* n : {"colorDepth", "pixelDepth"})
    def_accessor(P, sp, n, [](Context&, CallArgs&) -> Value { return Value::Number(24); });
  def_value(P, g, "screen", Value::Obj(screen), js::kHiddenAttrs);

  auto vnum = [&](const char* name, std::function<double()> f) {
    def_accessor(P, g, name, [f](Context&, CallArgs&) -> Value { return Value::Number(f()); }, [pp, name](Context&, CallArgs& a) -> Value {
      pp->ctx.define_data(pp->ctx.global(), pp->ctx.atom_utf8(name), a[0]);
      return Value::Undefined();
    });
  };
  vnum("innerWidth", [pp] { return pp->view_w; });
  vnum("innerHeight", [pp] { return pp->view_h; });
  vnum("outerWidth", [pp] { return pp->view_w; });
  vnum("outerHeight", [pp] { return pp->view_h; });
  vnum("devicePixelRatio", [] { return 1.0; });
  for (const char* n : {"scrollX", "scrollY", "pageXOffset", "pageYOffset", "screenX", "screenY", "screenLeft", "screenTop"}) vnum(n, [] { return 0.0; });

  for (const char* n : {"scrollTo", "scrollBy", "scroll", "focus", "blur", "stop", "print"})
    def_method(P, g, n, 0, [](Context&, CallArgs&) -> Value { return Value::Undefined(); });
  def_method(P, g, "alert", 1, [pp](Context&, CallArgs& a) -> Value {
    pp->log.push_back("alert: " + (a.argc ? pp->text(a[0]) : std::string()));
    return Value::Undefined();
  });
  def_method(P, g, "confirm", 1, [pp](Context&, CallArgs& a) -> Value {
    pp->log.push_back("confirm (answered no, there is no dialog): " + (a.argc ? pp->text(a[0]) : std::string()));
    return Value::Bool(false);
  });
  def_method(P, g, "prompt", 1, [pp](Context&, CallArgs& a) -> Value {
    pp->log.push_back("prompt (cancelled, there is no dialog): " + (a.argc ? pp->text(a[0]) : std::string()));
    return Value::Null();
  });
  def_method(P, g, "open", 1, [pp](Context&, CallArgs& a) -> Value {
    request_navigation(*pp, resolve(*pp, a.argc ? pp->text(a[0]) : std::string()), "window.open");
    return Value::Null();
  });
  def_method(P, g, "close", 0, [](Context&, CallArgs&) -> Value { return Value::Undefined(); });
  def_method(P, g, "getComputedStyle", 1, [pp](Context& cc, CallArgs& a) -> Value {
    if (!a[0].is_object() || a[0].as_object()->cls != js::Class::Host || static_cast<HostObject*>(a[0].as_object())->host_kind != HostKind::Node) {
      cc.throw_type_error("Failed to execute 'getComputedStyle' on 'Window': parameter 1 is not of type 'Element'.");
      return Value::Exception();
    }
    // There is no per-element cascade result to read back: this is the element's own style attribute.
    Value st;
    if (!cc.get(a[0], cc.atom_ascii("style"), &st)) return Value::Exception();
    (void)pp;
    return st;
  });

  // timers
  def_method(P, g, "setTimeout", 2, [pp](Context&, CallArgs& a) -> Value { return set_timer(*pp, a, false); });
  def_method(P, g, "setInterval", 2, [pp](Context&, CallArgs& a) -> Value { return set_timer(*pp, a, true); });
  auto clear = [pp](Context&, CallArgs& a) -> Value {
    const double d = pp->number(a[0]);
    if (std::isfinite(d)) clear_timer(*pp, static_cast<int>(d));
    return Value::Undefined();
  };
  def_method(P, g, "clearTimeout", 1, clear);
  def_method(P, g, "clearInterval", 1, clear);
  def_method(P, g, "requestAnimationFrame", 1, [pp](Context& cc, CallArgs& a) -> Value {
    if (!a[0].is_callable()) {
      cc.throw_type_error("Failed to execute 'requestAnimationFrame' on 'Window': The callback provided as parameter 1 is not a function.");
      return Value::Exception();
    }
    if (pp->raf.size() >= kMaxTimers) {
      cc.throw_range_error("too many pending animation frames");
      return Value::Exception();
    }
    PendingTimer t;
    t.id = pp->next_timer_id++;
    t.fn = a[0];
    pp->raf.push_back(std::move(t));
    return Value::Number(pp->raf.back().id);
  });
  def_method(P, g, "cancelAnimationFrame", 1, clear);

  // performance
  Object* perf = make_proto(P, c.intr.object_proto, "Performance");
  Object* perf_obj = c.new_object(perf);
  def_method(P, perf, "now", 0, [pp](Context&, CallArgs&) -> Value { return Value::Number(page_now(*pp)); });
  def_accessor(P, perf, "timeOrigin", [pp](Context&, CallArgs&) -> Value { return Value::Number(pp->wall_origin); });
  def_method(P, perf, "mark", 1, [pp](Context&, CallArgs&) -> Value { (void)pp; return Value::Undefined(); });
  def_method(P, perf, "measure", 1, [](Context&, CallArgs&) -> Value { return Value::Undefined(); });
  for (const char* n : {"clearMarks", "clearMeasures", "clearResourceTimings"})
    def_method(P, perf, n, 0, [](Context&, CallArgs&) -> Value { return Value::Undefined(); });
  for (const char* n : {"getEntries", "getEntriesByName", "getEntriesByType"})
    def_method(P, perf, n, 0, [pp](Context&, CallArgs&) -> Value { return make_array(*pp, {}); });
  def_value(P, g, "performance", Value::Obj(perf_obj), js::kHiddenAttrs);

  // crypto: randomness comes from the machine or not at all
  Object* cp = make_proto(P, c.intr.object_proto, "Crypto");
  Object* crypto = c.new_object(cp);
  auto fill = [pp](uint8_t* out, size_t n) -> bool {
    if (!pp->has_host || !pp->host.random_bytes) return false;
    pp->host.random_bytes(out, n);
    return true;
  };
  def_method(P, cp, "getRandomValues", 1, [pp, fill](Context& cc, CallArgs& a) -> Value {
    if (!a[0].is_object()) {
      cc.throw_type_error("Failed to execute 'getRandomValues' on 'Crypto': parameter 1 is not of type 'ArrayBufferView'.");
      return Value::Exception();
    }
    Value len_v, bpe_v;
    cc.get(a[0], cc.atom_ascii("length"), &len_v);
    cc.get(a[0], cc.atom_ascii("BYTES_PER_ELEMENT"), &bpe_v);
    const double len = pp->number(len_v), bpe = pp->number(bpe_v);
    if (!(len >= 0) || !(bpe >= 1) || bpe > 4) {
      cc.throw_type_error("The data argument must be an integer-type TypedArray.");
      return Value::Exception();
    }
    if (len * bpe > 65536) {
      Object* e = cc.new_error(js::ErrorKind::Error, "The ArrayBufferView's byte length (" + std::to_string(static_cast<long>(len * bpe)) + ") exceeds the number of bytes of entropy available via this API (65536).");
      cc.define_data(e, cc.atoms.name, Value::String(cc.atom_ascii("QuotaExceededError")), js::kHiddenAttrs);
      return cc.throw_value(Value::Obj(e));
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(len * bpe));
    if (!fill(bytes.data(), bytes.size())) {
      cc.throw_error(js::ErrorKind::Error, "crypto.getRandomValues: this page was given no entropy source by the machine");
      return Value::Exception();
    }
    for (size_t i = 0; i < static_cast<size_t>(len); ++i) {
      uint32_t v = 0;
      for (size_t k = 0; k < static_cast<size_t>(bpe); ++k) v |= static_cast<uint32_t>(bytes[i * static_cast<size_t>(bpe) + k]) << (8 * k);
      if (!cc.put_v(a[0], cc.index_atom(static_cast<uint32_t>(i)), Value::Number(v), true)) return Value::Exception();
    }
    return a[0];
  });
  def_method(P, cp, "randomUUID", 0, [pp, fill](Context& cc, CallArgs&) -> Value {
    uint8_t b[16];
    if (!fill(b, 16)) {
      cc.throw_error(js::ErrorKind::Error, "crypto.randomUUID: this page was given no entropy source by the machine");
      return Value::Exception();
    }
    b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return str(*pp, buf);
  });
  def_value(P, g, "crypto", Value::Obj(crypto), js::kHiddenAttrs);
}

// ---- the rest of the platform surface, written in JavaScript ----------------------------------
//
// These are small enough, and so much a matter of ordinary language semantics, that they are clearer as script than as
// host calls: structuredClone, matchMedia, MessageChannel, window.postMessage, idle callbacks and the observer
// constructors. The observers are constructed but never deliver: nothing in this engine measures an element's box or
// watches a subtree, and a callback that fired with invented numbers would be worse than one that does not fire.
#include "web_js/prelude_lazy.inc"

const char kPrelude[] = R"JS((function (window) {
  'use strict';
  function def(name, value) { Object.defineProperty(window, name, { value: value, writable: true, configurable: true, enumerable: false }); }
  function dataError(msg) { var e = new Error(msg); e.name = 'DataCloneError'; e.code = 25; return e; }

  def('structuredClone', function structuredClone(value) {
    if (arguments.length === 0) throw new TypeError("Failed to execute 'structuredClone' on 'Window': 1 argument required, but only 0 present.");
    var seen = new Map();
    function clone(v) {
      if (v === null || (typeof v !== 'object' && typeof v !== 'function')) {
        if (typeof v === 'symbol') throw dataError(String(v) + ' could not be cloned.');
        return v;
      }
      if (typeof v === 'function') throw dataError(String(v) + ' could not be cloned.');
      if (seen.has(v)) return seen.get(v);
      var out, tag = Object.prototype.toString.call(v);
      if (v instanceof Date) { out = new Date(v.getTime()); seen.set(v, out); return out; }
      if (v instanceof RegExp) { out = new RegExp(v.source, v.flags); seen.set(v, out); return out; }
      if (v instanceof Map) { out = new Map(); seen.set(v, out); v.forEach(function (val, key) { out.set(clone(key), clone(val)); }); return out; }
      if (v instanceof Set) { out = new Set(); seen.set(v, out); v.forEach(function (val) { out.add(clone(val)); }); return out; }
      if (v instanceof Error) {
        var C = { EvalError: EvalError, RangeError: RangeError, ReferenceError: ReferenceError, SyntaxError: SyntaxError, TypeError: TypeError, URIError: URIError }[v.name] || Error;
        out = new C(v.message); seen.set(v, out);
        if (v.stack !== undefined) out.stack = v.stack;
        if ('cause' in v) out.cause = clone(v.cause);
        return out;
      }
      if (v instanceof ArrayBuffer) { out = v.slice(0); seen.set(v, out); return out; }
      if (ArrayBuffer.isView(v)) {
        var buf = clone(v.buffer);
        out = v instanceof DataView ? new DataView(buf, v.byteOffset, v.byteLength) : new v.constructor(buf, v.byteOffset, v.length);
        seen.set(v, out); return out;
      }
      if (typeof Blob !== 'undefined' && v instanceof Blob) { seen.set(v, v); return v; }
      if (Array.isArray(v)) {
        out = new Array(v.length); seen.set(v, out);
        for (var i = 0; i < v.length; i++) if (i in v) out[i] = clone(v[i]);
        return out;
      }
      // a class instance clones to a plain object of its own enumerable data, as the algorithm says
      if (tag !== '[object Object]' && tag !== '[object Arguments]') throw dataError('#<' + (v.constructor && v.constructor.name || 'Object') + '> could not be cloned.');
      out = {}; seen.set(v, out);
      Object.keys(v).forEach(function (k) { out[k] = clone(v[k]); });
      return out;
    }
    return clone(value);
  });

  // CSS.escape is the CSSOM algorithm. CSS.supports answers whether a declaration or condition PARSES (a property name
  // and a non-empty, balanced value; any value for a custom property): this engine has no registry of the properties its
  // cascade implements, so it cannot say more, and a page that feature-detects a layout mode must not read a true as "laid out".
  def('CSS', (function () {
    function escape(value) {
      var s = String(value), n = s.length, out = '', first = s.charCodeAt(0);
      for (var i = 0; i < n; i++) {
        var c = s.charCodeAt(i);
        if (c === 0) { out += '�'; continue; }
        if ((c >= 1 && c <= 0x1f) || c === 0x7f || (i === 0 && c >= 48 && c <= 57) || (i === 1 && c >= 48 && c <= 57 && first === 45)) { out += '\\' + c.toString(16) + ' '; continue; }
        if (i === 0 && n === 1 && c === 45) { out += '\\' + s.charAt(i); continue; }
        if (c >= 0x80 || c === 45 || c === 95 || (c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122)) { out += s.charAt(i); continue; }
        out += '\\' + s.charAt(i);
      }
      return out;
    }
    function balanced(v) {
      var stack = [], q = null;
      for (var i = 0; i < v.length; i++) {
        var ch = v.charAt(i);
        if (ch === '\\') { i++; continue; }
        if (q) { if (ch === q) q = null; continue; }
        if (ch === '"' || ch === "'") q = ch;
        else if (ch === '(' || ch === '[' || ch === '{') stack.push(ch === '(' ? ')' : ch === '[' ? ']' : '}');
        else if (ch === ')' || ch === ']' || ch === '}') { if (stack.pop() !== ch) return false; }
        else if (ch === ';' && !stack.length) return false;
      }
      return !q && !stack.length;
    }
    function declares(prop, value) {
      prop = String(prop).trim(); value = String(value).trim();
      if (!/^(--[^\s]+|-?[a-zA-Z_][a-zA-Z0-9_-]*)$/.test(prop)) return false;
      if (prop.indexOf('--') === 0) return balanced(value);
      return value.length > 0 && balanced(value);
    }
    function condition(text) {
      text = text.trim();
      var m = /^not\s+(.*)$/i.exec(text);
      if (m) return !term(m[1]);
      var parts = [], depth = 0, start = 0, op = null, q = null;
      for (var i = 0; i < text.length; i++) {
        var ch = text.charAt(i);
        if (q) { if (ch === q) q = null; continue; }
        if (ch === '"' || ch === "'") q = ch;
        else if (ch === '(') depth++;
        else if (ch === ')') depth--;
        else if (depth === 0 && /\s/.test(ch)) {
          var rest = text.slice(i);
          var w = /^\s+(and|or)\s+/i.exec(rest);
          if (w) { var o = w[1].toLowerCase(); if (op && op !== o) return false; op = o; parts.push(text.slice(start, i)); i += w[0].length - 1; start = i + 1; }
        }
      }
      parts.push(text.slice(start));
      if (!op) return term(parts[0]);
      for (var k = 0; k < parts.length; k++) {
        var r = term(parts[k]);
        if (op === 'and' && !r) return false;
        if (op === 'or' && r) return true;
      }
      return op === 'and';
    }
    function term(t) {
      t = t.trim();
      var sel = /^selector\((.*)\)$/is.exec(t);
      if (sel) { try { document.createDocumentFragment().querySelector(sel[1]); return true; } catch (e) { return false; } }
      if (t.charAt(0) !== '(' || t.charAt(t.length - 1) !== ')' || !balanced(t)) return false;
      var inner = t.slice(1, -1).trim();
      var colon = inner.indexOf(':');
      if (colon > 0 && /^(--[^\s]+|-?[a-zA-Z_][a-zA-Z0-9_-]*)\s*$/.test(inner.slice(0, colon))) return declares(inner.slice(0, colon), inner.slice(colon + 1));
      return condition(inner);
    }
    function supports(a, b) {
      if (arguments.length === 0) throw new TypeError("Failed to execute 'supports' on 'CSS': 1 argument required, but only 0 present.");
      if (arguments.length >= 2) return declares(a, b);
      return condition(String(a));
    }
    var CSS = {};
    Object.defineProperty(CSS, 'escape', { value: escape, writable: true, configurable: true, enumerable: true });
    Object.defineProperty(CSS, 'supports', { value: supports, writable: true, configurable: true, enumerable: true });
    Object.defineProperty(CSS, Symbol.toStringTag, { value: 'CSS', configurable: true });
    return CSS;
  })());

  // document.fonts: a FontFaceSet. This engine has no @font-face and no font loading, so nothing ever needs loading: load()
  // resolves with the (empty) list of faces it loaded, check() is true, ready is already resolved and status is 'loaded'.
  // Faces a page add()s are kept so has/size/iteration answer truthfully; the loading events never fire because nothing loads.
  (function () {
    var faces = new Set(), listeners = {};
    var fonts = {
      get size() { return faces.size; },
      get status() { return 'loaded'; },
      add: function (f) { faces.add(f); return fonts; },
      delete: function (f) { return faces['delete'](f); },
      has: function (f) { return faces.has(f); },
      clear: function () { faces.clear(); },
      forEach: function (cb, thisArg) { faces.forEach(function (f) { cb.call(thisArg, f, f, fonts); }); },
      values: function () { return faces.values(); },
      keys: function () { return faces.values(); },
      entries: function () { return faces.entries(); },
      load: function (font) {
        if (arguments.length === 0) return Promise.reject(new TypeError("Failed to execute 'load' on 'FontFaceSet': 1 argument required, but only 0 present."));
        return Promise.resolve([]);
      },
      check: function (font) {
        if (arguments.length === 0) throw new TypeError("Failed to execute 'check' on 'FontFaceSet': 1 argument required, but only 0 present.");
        return true;
      },
      addEventListener: function (type, fn) { (listeners[type] = listeners[type] || []).push(fn); },
      removeEventListener: function (type, fn) { var l = listeners[type]; if (l) { var i = l.indexOf(fn); if (i >= 0) l.splice(i, 1); } },
      dispatchEvent: function (ev) { (listeners[ev.type] || []).slice().forEach(function (fn) { fn.call(fonts, ev); }); return true; }
    };
    fonts.ready = Promise.resolve(fonts);
    fonts[Symbol.iterator] = fonts.values;
    Object.defineProperty(fonts, Symbol.toStringTag, { value: 'FontFaceSet', configurable: true });
    Object.defineProperty(window.document, 'fonts', { get: function () { return fonts; }, configurable: true, enumerable: true });
  })();

  var mediaFeature = {
    'width': function () { return window.innerWidth; }, 'height': function () { return window.innerHeight; },
    'device-width': function () { return window.screen.width; }, 'device-height': function () { return window.screen.height; },
    'resolution': function () { return 1; }, 'color': function () { return 8; }
  };
  var mediaEnum = {
    'prefers-color-scheme': 'dark', 'prefers-reduced-motion': 'no-preference', 'prefers-contrast': 'no-preference', 'hover': 'hover', 'pointer': 'fine',
    'any-hover': 'hover', 'any-pointer': 'fine', 'orientation': 'landscape', 'display-mode': 'browser', 'forced-colors': 'none', 'prefers-reduced-transparency': 'no-preference'
  };
  function toPx(s) {
    var m = /^(-?[\d.]+)(px|em|rem|dppx)?$/.exec(s.trim());
    if (!m) return NaN;
    return (m[2] === 'em' || m[2] === 'rem') ? parseFloat(m[1]) * 16 : parseFloat(m[1]);
  }
  function evalCondition(cond) {
    cond = cond.trim();
    var m = /^\(\s*([a-z-]+)\s*(?::\s*([^)]+))?\)$/.exec(cond);
    if (!m) return false;
    var name = m[1], val = m[2];
    var range = /^(min|max)-(.*)$/.exec(name);
    if (range && mediaFeature[range[2]]) {
      var want = toPx(val || ''), have = mediaFeature[range[2]]();
      return range[1] === 'min' ? have >= want : have <= want;
    }
    if (mediaFeature[name]) return val === undefined ? mediaFeature[name]() > 0 : mediaFeature[name]() === toPx(val);
    if (name in mediaEnum) return val === undefined ? mediaEnum[name] !== 'none' && mediaEnum[name] !== 'no-preference' : val.trim() === mediaEnum[name];
    return false;
  }
  function evalQuery(q) {
    q = q.trim().toLowerCase();
    if (q === '' || q === 'all') return true;
    var neg = false;
    if (q.indexOf('not ') === 0) { neg = true; q = q.slice(4); }
    if (q.indexOf('only ') === 0) q = q.slice(5);
    var parts = q.split(/\s+and\s+/), ok = true;
    for (var i = 0; i < parts.length; i++) {
      var p = parts[i].trim();
      if (p === 'screen' || p === 'all') continue;
      if (p === 'print' || p === 'speech') { ok = false; continue; }
      if (!evalCondition(p)) ok = false;
    }
    return neg ? !ok : ok;
  }
  class MediaQueryList extends EventTarget {
    constructor(media) {
      super();
      Object.defineProperty(this, 'media', { value: media, enumerable: true });
      Object.defineProperty(this, 'onchange', { value: null, writable: true, enumerable: true });
    }
    get matches() { return media_matches(this.media); }
    addListener(cb) { if (cb) this.addEventListener('change', cb); }
    removeListener(cb) { if (cb) this.removeEventListener('change', cb); }
  }
  function media_matches(media) {
    var list = String(media).split(',');
    for (var i = 0; i < list.length; i++) if (evalQuery(list[i])) return true;
    return false;
  }
  def('MediaQueryList', MediaQueryList);
  def('matchMedia', function matchMedia(q) {
    if (arguments.length === 0) throw new TypeError("Failed to execute 'matchMedia' on 'Window': 1 argument required, but only 0 present.");
    return new MediaQueryList(String(q));
  });

  class MessagePort extends EventTarget {
    constructor() { super(); this._other = null; this._onmessage = null; this._closed = false; }
    get onmessage() { return this._onmessage; }
    set onmessage(fn) {
      if (this._onmessage) this.removeEventListener('message', this._onmessage);
      this._onmessage = typeof fn === 'function' ? fn : null;
      if (this._onmessage) this.addEventListener('message', this._onmessage);
    }
    postMessage(data) {
      var other = this._other;
      if (!other || this._closed) return;
      var copy = window.structuredClone(data);
      setTimeout(function () { if (!other._closed) other.dispatchEvent(new MessageEvent('message', { data: copy })); }, 0);
    }
    start() {}
    close() { this._closed = true; }
  }
  function MessageChannel() { this.port1 = new MessagePort(); this.port2 = new MessagePort(); this.port1._other = this.port2; this.port2._other = this.port1; }
  def('MessagePort', MessagePort);
  def('MessageChannel', MessageChannel);

  def('postMessage', function postMessage(message, targetOrigin) {
    if (arguments.length === 0) throw new TypeError("Failed to execute 'postMessage' on 'Window': 1 argument required, but only 0 present.");
    var copy = window.structuredClone(message);
    var origin = window.location.origin;
    if (targetOrigin !== undefined && targetOrigin !== '*' && targetOrigin !== '/' && targetOrigin !== origin) return;
    setTimeout(function () { window.dispatchEvent(new MessageEvent('message', { data: copy, origin: origin, source: window })); }, 0);
  });

  def('requestIdleCallback', function requestIdleCallback(cb) {
    if (typeof cb !== 'function') throw new TypeError("Failed to execute 'requestIdleCallback' on 'Window': The callback provided as parameter 1 is not a function.");
    var start = performance.now();
    return setTimeout(function () { cb({ didTimeout: false, timeRemaining: function () { return Math.max(0, 50 - (performance.now() - start)); } }); }, 1);
  });
  def('cancelIdleCallback', function cancelIdleCallback(id) { clearTimeout(id); });

  function observer(name, extra) {
    var C = function (callback) {
      if (typeof callback !== 'function') throw new TypeError("Failed to construct '" + name + "': The callback provided as parameter 1 is not a function.");
      this._callback = callback; this._targets = [];
    };
    C.prototype.observe = function (target) { if (this._targets.indexOf(target) < 0) this._targets.push(target); };
    C.prototype.unobserve = function (target) { var i = this._targets.indexOf(target); if (i >= 0) this._targets.splice(i, 1); };
    C.prototype.disconnect = function () { this._targets = []; };
    C.prototype.takeRecords = function () { return []; };
    if (extra) extra(C);
    Object.defineProperty(C, 'name', { value: name });
    def(name, C);
  }
  // ---- DOM change reporting: the C++ DOM calls onMutation for every tree/attribute/text change once a page asks for it
  // (a MutationObserver or a custom element definition), so a page with neither pays nothing. ----
  var hookReg = window.__aiosMO; delete window.__aiosMO;
  var hookOn = false, moObservers = [], moQueued = false, ceDefs = new Map(), ceNames = new Map(), ceUpgraded = new WeakSet(), ceWaiting = new Map(), ceDepth = 0;
  function ensureHook() { if (!hookOn && hookReg) { hookOn = true; hookReg(onMutation); } }
  function later(e) { setTimeout(function () { throw e; }, 0); }
  function domErr(msg, name) { return typeof DOMException === 'function' ? new DOMException(msg, name) : new Error(msg); }

  function MutationObserver(callback) {
    if (!new.target) throw new TypeError("Failed to construct 'MutationObserver': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    if (typeof callback !== 'function') throw new TypeError("Failed to construct 'MutationObserver': The callback provided as parameter 1 is not a function.");
    Object.defineProperty(this, '_cb', { value: callback });
    Object.defineProperty(this, '_regs', { value: [] });
    Object.defineProperty(this, '_records', { value: [], writable: true });
  }
  MutationObserver.prototype.observe = function observe(target, opts) {
    if (!target || typeof target.nodeType !== 'number') throw new TypeError("Failed to execute 'observe' on 'MutationObserver': parameter 1 is not of type 'Node'.");
    var o = Object.assign({}, opts || {});
    if (o.attributes === undefined && (o.attributeOldValue !== undefined || o.attributeFilter !== undefined)) o.attributes = true;
    if (o.characterData === undefined && o.characterDataOldValue !== undefined) o.characterData = true;
    if (!o.childList && !o.attributes && !o.characterData)
      throw new TypeError("Failed to execute 'observe' on 'MutationObserver': The options object must set at least one of 'attributes', 'characterData', or 'childList' to true.");
    ensureHook();
    for (var i = 0; i < this._regs.length; i++) if (this._regs[i].target === target) { this._regs[i].opts = o; return; }
    this._regs.push({ target: target, opts: o });
    if (moObservers.indexOf(this) < 0) moObservers.push(this);
  };
  MutationObserver.prototype.disconnect = function disconnect() {
    this._regs.length = 0; this._records = [];
    var i = moObservers.indexOf(this); if (i >= 0) moObservers.splice(i, 1);
  };
  MutationObserver.prototype.takeRecords = function takeRecords() { var r = this._records; this._records = []; return r; };
  def('MutationObserver', MutationObserver);
  function MutationRecord() { throw new TypeError('Illegal constructor'); }
  def('MutationRecord', MutationRecord);

  function deliverMO() {
    moQueued = false;
    var obs = moObservers.slice();
    for (var i = 0; i < obs.length; i++) {
      var o = obs[i];
      if (!o._records.length) continue;
      var recs = o._records; o._records = [];
      try { o._cb.call(o, recs, o); } catch (e) { later(e); }
    }
  }

  function onMutation(type, target, attr, old, added, removed, prev, next) {
    if (ceDefs.size && ceDepth < 40) { ceDepth++; try { ceReact(type, target, attr, old, added, removed); } finally { ceDepth--; } }
    var hit = false;
    for (var i = 0; i < moObservers.length; i++) {
      var o = moObservers[i];
      for (var j = 0; j < o._regs.length; j++) {
        var r = o._regs[j], op = r.opts;
        if (r.target !== target && !(op.subtree && r.target.contains(target))) continue;
        if (type === 'attributes') {
          if (!op.attributes) continue;
          if (op.attributeFilter && op.attributeFilter.indexOf(attr) < 0) continue;
        } else if (type === 'characterData') { if (!op.characterData) continue; }
        else if (!op.childList) continue;
        var rec = Object.create(MutationRecord.prototype);
        rec.type = type; rec.target = target; rec.addedNodes = added; rec.removedNodes = removed; rec.previousSibling = prev; rec.nextSibling = next;
        rec.attributeName = type === 'attributes' ? attr : null; rec.attributeNamespace = null;
        rec.oldValue = (type === 'attributes' && op.attributeOldValue) || (type === 'characterData' && op.characterDataOldValue) ? old : null;
        o._records.push(rec); hit = true;
        break;   // one record per observer per change, however many of its registrations cover the target
      }
    }
    if (hit && !moQueued) { moQueued = true; queueMicrotask(deliverMO); }
  }

  // ---- DocumentFragment, ShadowRoot (an unrendered fragment: the layout engine has no flat tree to draw it from) ----
  var fragProto = Object.getPrototypeOf(document.createDocumentFragment());
  function DocumentFragment() { if (!new.target) throw new TypeError("Failed to construct 'DocumentFragment': Please use the 'new' operator, this DOM object constructor cannot be called as a function."); return document.createDocumentFragment(); }
  DocumentFragment.prototype = fragProto;
  def('DocumentFragment', DocumentFragment);
  function ShadowRoot() { throw new TypeError('Illegal constructor'); }
  ShadowRoot.prototype = Object.create(fragProto, { constructor: { value: ShadowRoot, writable: true, configurable: true } });
  def('ShadowRoot', ShadowRoot);
  var shadows = new WeakMap();
  Object.defineProperty(window.Element.prototype, 'attachShadow', { configurable: true, writable: true, value: function attachShadow(init) {
    if (!init || (init.mode !== 'open' && init.mode !== 'closed')) throw new TypeError("Failed to execute 'attachShadow' on 'Element': The provided value '" + (init && init.mode) + "' is not a valid enum value of type ShadowRootMode.");
    if (shadows.has(this)) throw domErr("Failed to execute 'attachShadow' on 'Element': Shadow root cannot be created on a host which already hosts a shadow tree.", 'NotSupportedError');
    var sr = document.createDocumentFragment();
    Object.setPrototypeOf(sr, ShadowRoot.prototype);
    Object.defineProperties(sr, {
      host: { value: this }, mode: { value: init.mode }, delegatesFocus: { value: !!init.delegatesFocus },
      slotAssignment: { value: init.slotAssignment || 'named' }, activeElement: { value: null }, adoptedStyleSheets: { value: [], writable: true }, styleSheets: { value: [] }
    });
    shadows.set(this, sr);
    return sr;
  } });
  Object.defineProperty(window.Element.prototype, 'shadowRoot', { configurable: true, get: function () { var sr = shadows.get(this); return sr && sr.mode === 'open' ? sr : null; } });

  // ---- custom elements ----
  var HTMLElementNative = window.HTMLElement, ceUpgrading = null, origCreate = document.createElement;
  function HTMLElement() {
    if (ceUpgrading) { var el = ceUpgrading; ceUpgrading = null; return el; }
    var name = new.target && ceNames.get(new.target);
    if (!name) throw new TypeError('Illegal constructor');
    var made = origCreate.call(document, name);
    Object.setPrototypeOf(made, new.target.prototype);
    ceUpgraded.add(made);
    return made;
  }
  HTMLElement.prototype = HTMLElementNative.prototype;
  Object.defineProperty(HTMLElementNative.prototype, 'constructor', { value: HTMLElement, writable: true, configurable: true });
  Object.setPrototypeOf(HTMLElement, Object.getPrototypeOf(HTMLElementNative));
  def('HTMLElement', HTMLElement);

  function ceCall(el, name, args) {
    var fn = el[name];
    if (typeof fn === 'function') { try { fn.apply(el, args); } catch (e) { later(e); } }
  }
  function ceUpgrade(el, d) {
    if (ceUpgraded.has(el)) return;
    ceUpgraded.add(el);
    Object.setPrototypeOf(el, d.ctor.prototype);
    ceUpgrading = el;
    try { new d.ctor(); } catch (e) { later(e); } finally { ceUpgrading = null; }
    var obs = d.ctor.observedAttributes;
    if (obs && el.hasAttributes && el.hasAttributes())
      for (var i = 0; i < obs.length; i++) if (el.hasAttribute(obs[i])) ceCall(el, 'attributeChangedCallback', [obs[i], null, el.getAttribute(obs[i])]);
    if (document.contains(el)) ceCall(el, 'connectedCallback', []);
  }
  function ceWalk(root, fn) {
    if (root.nodeType !== 1) return;
    fn(root);
    var all = root.querySelectorAll('*');
    for (var i = 0; i < all.length; i++) fn(all[i]);
  }
  function ceReact(type, target, attr, old, added, removed) {
    if (type === 'attributes') {
      if (ceUpgraded.has(target)) {
        var d = ceDefs.get(target.localName), obs = d && d.ctor.observedAttributes;
        if (obs && obs.indexOf(attr) >= 0) ceCall(target, 'attributeChangedCallback', [attr, old, target.getAttribute(attr)]);
      }
      return;
    }
    if (type !== 'childList') return;
    var inDoc = document.contains(target);
    for (var i = 0; i < added.length; i++) {
      if (!inDoc) continue;
      ceWalk(added[i], function (el) {
        var d = ceDefs.get(el.localName);
        if (!d) return;
        if (!ceUpgraded.has(el)) ceUpgrade(el, d); else ceCall(el, 'connectedCallback', []);
      });
    }
    for (var k = 0; k < removed.length; k++) {
      ceWalk(removed[k], function (el) { if (ceUpgraded.has(el) && !document.contains(el)) ceCall(el, 'disconnectedCallback', []); });
    }
  }
  var customElements = {
    define: function define(name, ctor, opts) {
      if (typeof ctor !== 'function') throw new TypeError("Failed to execute 'define' on 'CustomElementRegistry': The callback provided as parameter 2 is not a constructor.");
      if (!/^[a-z][a-z0-9._-]*-[a-z0-9._-]*$/.test(name)) throw domErr("Failed to execute 'define' on 'CustomElementRegistry': \"" + name + "\" is not a valid custom element name", 'SyntaxError');
      if (ceDefs.has(name)) throw domErr("Failed to execute 'define' on 'CustomElementRegistry': the name \"" + name + "\" has already been used with this registry", 'NotSupportedError');
      if (ceNames.has(ctor)) throw domErr("Failed to execute 'define' on 'CustomElementRegistry': this constructor has already been used with this registry", 'NotSupportedError');
      var d = { name: name, ctor: ctor };
      ceDefs.set(name, d); ceNames.set(ctor, name);
      ensureHook();
      var found = document.querySelectorAll(name);
      for (var i = 0; i < found.length; i++) ceUpgrade(found[i], d);
      var w = ceWaiting.get(name); if (w) { ceWaiting.delete(name); w.forEach(function (res) { res(ctor); }); }
    },
    get: function get(name) { var d = ceDefs.get(name); return d ? d.ctor : undefined; },
    getName: function getName(ctor) { return ceNames.get(ctor) || null; },
    whenDefined: function whenDefined(name) {
      var d = ceDefs.get(name);
      if (d) return Promise.resolve(d.ctor);
      return new Promise(function (res) { var l = ceWaiting.get(name); if (!l) ceWaiting.set(name, l = []); l.push(res); });
    },
    upgrade: function upgrade(root) { ceWalk(root, function (el) { var d = ceDefs.get(el.localName); if (d) ceUpgrade(el, d); }); }
  };
  function CustomElementRegistry() { throw new TypeError('Illegal constructor'); }
  Object.setPrototypeOf(customElements, CustomElementRegistry.prototype);
  def('CustomElementRegistry', CustomElementRegistry);
  def('customElements', customElements);
  document.createElement = function createElement(name, opts) {
    var el = origCreate.apply(document, arguments);
    if (ceDefs.size) { var d = ceDefs.get(el.localName); if (d) ceUpgrade(el, d); }
    return el;
  };

  // ---- ResizeObserver / IntersectionObserver. This engine exposes no per-element boxes to script, so there is nothing to
  // watch: each observed target gets the one initial notification a browser sends (sizes as the element reports them,
  // intersecting, since a page that lazy-loads on intersection must still get its content). ----
  function boxObserver(name, entryFor, extra) {
    var C = function (callback, options) {
      if (!new.target) throw new TypeError("Failed to construct '" + name + "': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
      if (typeof callback !== 'function') throw new TypeError("Failed to construct '" + name + "': The callback provided as parameter 1 is not a function.");
      Object.defineProperty(this, '_cb', { value: callback });
      Object.defineProperty(this, '_targets', { value: [] });
      Object.defineProperty(this, '_pending', { value: [] });
      if (extra) extra(this, options || {});
    };
    C.prototype.observe = function observe(target) {
      if (!target || typeof target.nodeType !== 'number') throw new TypeError("Failed to execute 'observe' on '" + name + "': parameter 1 is not of type 'Element'.");
      if (this._targets.indexOf(target) >= 0) return;
      this._targets.push(target); this._pending.push(target);
      var self = this;
      if (this._pending.length === 1) setTimeout(function () {
        var list = self._pending.splice(0).filter(function (t) { return self._targets.indexOf(t) >= 0; });
        if (!list.length) return;
        var entries = list.map(entryFor);
        try { self._cb.call(self, entries, self); } catch (e) { later(e); }
      }, 0);
    };
    C.prototype.unobserve = function unobserve(target) {
      var i = this._targets.indexOf(target); if (i >= 0) this._targets.splice(i, 1);
    };
    C.prototype.disconnect = function disconnect() { this._targets.length = 0; this._pending.length = 0; };
    C.prototype.takeRecords = function takeRecords() { return []; };
    Object.defineProperty(C, 'name', { value: name });
    def(name, C);
    return C;
  }
  function rectOf(el) {
    var r = el.getBoundingClientRect();
    return r;
  }
  boxObserver('ResizeObserver', function (t) {
    var w = t.clientWidth || 0, h = t.clientHeight || 0;
    var rect = { x: 0, y: 0, width: w, height: h, top: 0, left: 0, right: w, bottom: h };
    var size = [{ inlineSize: w, blockSize: h }];
    return { target: t, contentRect: rect, borderBoxSize: size, contentBoxSize: size, devicePixelContentBoxSize: size };
  });
  boxObserver('IntersectionObserver', function (t) {
    var r = rectOf(t);
    return { target: t, isIntersecting: true, intersectionRatio: 1, time: performance.now(), boundingClientRect: r, intersectionRect: r, rootBounds: null, isVisible: true };
  }, function (self, o) {
    var th = o.threshold === undefined ? [0] : Array.isArray(o.threshold) ? o.threshold : [o.threshold];
    Object.defineProperties(self, { root: { value: o.root || null }, rootMargin: { value: o.rootMargin || '0px' }, thresholds: { value: th } });
  });

  // ---- Range and Selection (positions and text; no layout, so no boxes) ----
  function nodeLen(n) { return n.nodeType === 3 || n.nodeType === 8 ? n.data.length : n.childNodes.length; }
  function idxOf(n) { return Array.prototype.indexOf.call(n.parentNode.childNodes, n); }
  function bpCmp(a, ao, b, bo) {
    if (a === b) return ao < bo ? -1 : ao > bo ? 1 : 0;
    var p = a.compareDocumentPosition(b);
    if (p & 4) {
      if (p & 16) { var c = b; while (c.parentNode !== a) c = c.parentNode; return idxOf(c) < ao ? 1 : -1; }
      return -1;
    }
    return -bpCmp(b, bo, a, ao);
  }
  function Range() {
    if (!new.target) throw new TypeError("Failed to construct 'Range': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    Object.defineProperty(this, '_s', { value: [document, 0], writable: true });
    Object.defineProperty(this, '_e', { value: [document, 0], writable: true });
  }
  var RP = Range.prototype;
  function chkOffset(n, o) {
    if (o < 0 || o > nodeLen(n)) throw domErr("The offset " + o + " is larger than the node's length (" + nodeLen(n) + ").", 'IndexSizeError');
  }
  function defGet(name, fn) { Object.defineProperty(RP, name, { get: fn, configurable: true, enumerable: true }); }
  defGet('startContainer', function () { return this._s[0]; });
  defGet('startOffset', function () { return this._s[1]; });
  defGet('endContainer', function () { return this._e[0]; });
  defGet('endOffset', function () { return this._e[1]; });
  defGet('collapsed', function () { return this._s[0] === this._e[0] && this._s[1] === this._e[1]; });
  defGet('commonAncestorContainer', function () {
    var a = this._s[0];
    while (a && !a.contains(this._e[0])) a = a.parentNode;
    return a;
  });
  RP.setStart = function setStart(n, o) { o = o >>> 0; chkOffset(n, o); this._s = [n, o]; if (bpCmp(n, o, this._e[0], this._e[1]) > 0) this._e = [n, o]; };
  RP.setEnd = function setEnd(n, o) { o = o >>> 0; chkOffset(n, o); this._e = [n, o]; if (bpCmp(this._s[0], this._s[1], n, o) > 0) this._s = [n, o]; };
  RP.setStartBefore = function (n) { this.setStart(n.parentNode, idxOf(n)); };
  RP.setStartAfter = function (n) { this.setStart(n.parentNode, idxOf(n) + 1); };
  RP.setEndBefore = function (n) { this.setEnd(n.parentNode, idxOf(n)); };
  RP.setEndAfter = function (n) { this.setEnd(n.parentNode, idxOf(n) + 1); };
  RP.selectNode = function selectNode(n) { var p = n.parentNode, i = idxOf(n); this._s = [p, i]; this._e = [p, i + 1]; };
  RP.selectNodeContents = function selectNodeContents(n) { this._s = [n, 0]; this._e = [n, nodeLen(n)]; };
  RP.collapse = function collapse(toStart) { if (toStart) this._e = this._s.slice(); else this._s = this._e.slice(); };
  RP.cloneRange = function cloneRange() { var r = new Range(); r._s = this._s.slice(); r._e = this._e.slice(); return r; };
  RP.detach = function detach() {};
  RP.compareBoundaryPoints = function compareBoundaryPoints(how, other) {
    var a = (how === 0 || how === 3) ? this._s : this._e, b = (how === 0 || how === 2) ? other._s : other._e;
    if (how === 1) { a = this._e; b = other._s; } else if (how === 3) { a = this._s; b = other._e; }
    return bpCmp(a[0], a[1], b[0], b[1]);
  };
  RP.isPointInRange = function (n, o) { return bpCmp(n, o, this._s[0], this._s[1]) >= 0 && bpCmp(n, o, this._e[0], this._e[1]) <= 0; };
  RP.intersectsNode = function (n) {
    var p = n.parentNode; if (!p) return true;
    var i = idxOf(n);
    return bpCmp(p, i, this._e[0], this._e[1]) < 0 && bpCmp(p, i + 1, this._s[0], this._s[1]) > 0;
  };
  function textPieces(r, fn) {
    var root = r.commonAncestorContainer;
    (function walk(n) {
      if (n.nodeType === 3) {
        var s = n === r._s[0] ? r._s[1] : 0, e = n === r._e[0] ? r._e[1] : n.data.length;
        if (bpCmp(n, n.data.length, r._s[0], r._s[1]) > 0 || n === r._s[0])
          if (bpCmp(n, 0, r._e[0], r._e[1]) < 0 || n === r._e[0]) fn(n, s, e);
        return;
      }
      var k = n.childNodes;
      for (var i = 0; i < k.length; i++) walk(k[i]);
    })(root);
  }
  RP.toString = function toString() { var out = ''; textPieces(this, function (n, s, e) { if (e > s) out += n.data.substring(s, e); }); return out; };
  RP.deleteContents = function deleteContents() {
    var self = this, edits = [];
    textPieces(this, function (n, s, e) { edits.push([n, s, e]); });
    edits.forEach(function (x) {
      var n = x[0];
      if (x[1] === 0 && x[2] === n.data.length && n !== self._s[0] && n !== self._e[0]) { if (n.parentNode) n.parentNode.removeChild(n); }
      else n.data = n.data.substring(0, x[1]) + n.data.substring(x[2]);
    });
    this.collapse(true);
  };
  RP.insertNode = function insertNode(node) {
    var sc = this._s[0], so = this._s[1];
    if (sc.nodeType === 3) {
      var tail = sc.splitText ? sc.splitText(so) : null;
      sc.parentNode.insertBefore(node, tail);
    } else sc.insertBefore(node, sc.childNodes[so] || null);
  };
  RP.createContextualFragment = function createContextualFragment(html) {
    var t = document.createElement('template'); t.innerHTML = html; return t.content;
  };
  RP.cloneContents = function cloneContents() {
    var f = document.createDocumentFragment(), t = this.toString();
    if (t) f.appendChild(document.createTextNode(t));
    return f;
  };
  RP.getBoundingClientRect = function () { return { x: 0, y: 0, width: 0, height: 0, top: 0, left: 0, right: 0, bottom: 0 }; };
  RP.getClientRects = function () { return []; };
  Object.defineProperties(Range, { START_TO_START: { value: 0 }, START_TO_END: { value: 1 }, END_TO_END: { value: 2 }, END_TO_START: { value: 3 } });
  def('Range', Range);
  def('AbstractRange', Range);
  document.createRange = function createRange() { return new Range(); };

  function Selection() { throw new TypeError('Illegal constructor'); }
  var sel = Object.create(Selection.prototype), selRange = null;
  function selGet(name, fn) { Object.defineProperty(Selection.prototype, name, { get: fn, configurable: true, enumerable: true }); }
  selGet('rangeCount', function () { return selRange ? 1 : 0; });
  selGet('anchorNode', function () { return selRange ? selRange._s[0] : null; });
  selGet('anchorOffset', function () { return selRange ? selRange._s[1] : 0; });
  selGet('focusNode', function () { return selRange ? selRange._e[0] : null; });
  selGet('focusOffset', function () { return selRange ? selRange._e[1] : 0; });
  selGet('isCollapsed', function () { return !selRange || selRange.collapsed; });
  selGet('type', function () { return !selRange ? 'None' : selRange.collapsed ? 'Caret' : 'Range'; });
  selGet('direction', function () { return 'none'; });
  var SP = Selection.prototype;
  SP.getRangeAt = function getRangeAt(i) { if (!selRange || i !== 0) throw domErr('The index is not in the allowed range.', 'IndexSizeError'); return selRange; };
  SP.addRange = function addRange(r) { if (!selRange) selRange = r; };
  SP.removeRange = function removeRange(r) { if (selRange === r) selRange = null; };
  SP.removeAllRanges = SP.empty = function removeAllRanges() { selRange = null; };
  SP.collapse = SP.setPosition = function collapse(n, o) {
    if (n === null) { selRange = null; return; }
    var r = new Range(); r.setStart(n, o || 0); r.setEnd(n, o || 0); selRange = r;
  };
  SP.collapseToStart = function () { if (selRange) selRange.collapse(true); };
  SP.collapseToEnd = function () { if (selRange) selRange.collapse(false); };
  SP.selectAllChildren = function selectAllChildren(n) { var r = new Range(); r.selectNodeContents(n); selRange = r; };
  SP.setBaseAndExtent = function (an, ao, fn, fo) {
    var r = new Range();
    if (bpCmp(an, ao, fn, fo) <= 0) { r.setStart(an, ao); r.setEnd(fn, fo); } else { r.setStart(fn, fo); r.setEnd(an, ao); }
    selRange = r;
  };
  SP.extend = function extend(n, o) { if (selRange) { if (bpCmp(n, o, selRange._s[0], selRange._s[1]) < 0) selRange.setStart(n, o); else selRange.setEnd(n, o); } };
  SP.containsNode = function containsNode(n, partial) {
    if (!selRange) return false;
    return partial ? selRange.intersectsNode(n) : selRange.isPointInRange(n, 0) && selRange.isPointInRange(n, nodeLen(n));
  };
  SP.deleteFromDocument = function () { if (selRange) selRange.deleteContents(); };
  SP.toString = function toString() { return selRange ? selRange.toString() : ''; };
  def('Selection', Selection);
  def('getSelection', function getSelection() { return sel; });
  document.getSelection = window.getSelection;

  // CSSStyleSheet over a <style> element: styled-components/emotion in "speedy" mode find element.sheet (or the entry of
  // document.styleSheets whose ownerNode is the element) and insertRule into it. Every change writes the rules back as the
  // element's text, so the cascade sees them. Rules are kept as text: cssText/selectorText only.
  (function () {
    var sheets = new WeakMap();
    function splitRules(text) {
      var out = [], depth = 0, start = 0, q = null;
      text = String(text).replace(/\/\*[\s\S]*?\*\//g, '');
      for (var i = 0; i < text.length; i++) {
        var ch = text[i];
        if (q) { if (ch === '\\') i++; else if (ch === q) q = null; continue; }
        if (ch === '"' || ch === "'") q = ch;
        else if (ch === '{') depth++;
        else if (ch === '}') { if (depth > 0 && --depth === 0) { out.push(text.slice(start, i + 1).trim()); start = i + 1; } }
        else if (ch === ';' && depth === 0) { var at = text.slice(start, i + 1).trim(); if (at) out.push(at); start = i + 1; }
      }
      return out;
    }
    function CSSRule(text, sheet) { this.cssText = text; this.parentStyleSheet = sheet; }
    Object.defineProperty(CSSRule.prototype, 'selectorText', { get: function () { var i = this.cssText.indexOf('{'); return i < 0 ? '' : this.cssText.slice(0, i).trim(); }, configurable: true });
    function CSSStyleSheet(init) { this.cssRules = []; this.ownerNode = null; this.disabled = false; this.type = 'text/css'; this.href = null; this.media = { mediaText: '' }; }
    var SSP = CSSStyleSheet.prototype;
    Object.defineProperty(SSP, 'rules', { get: function () { return this.cssRules; }, configurable: true });
    function sync(sh) {
      var n = sh.ownerNode;
      if (n) { sh._w = true; n.textContent = sh.cssRules.map(function (r) { return r.cssText; }).join('\n'); sh._w = false; }
    }
    SSP.insertRule = function insertRule(rule, index) {
      index = index === undefined ? 0 : index >>> 0;
      if (index > this.cssRules.length) throw new DOMException("Failed to execute 'insertRule' on 'CSSStyleSheet': The index provided (" + index + ") is larger than the maximum index (" + this.cssRules.length + ").", 'IndexSizeError');
      this.cssRules.splice(index, 0, new CSSRule(String(rule), this));
      sync(this);
      return index;
    };
    SSP.deleteRule = function deleteRule(index) {
      index >>>= 0;
      if (index >= this.cssRules.length) throw new DOMException("Failed to execute 'deleteRule' on 'CSSStyleSheet': The index provided (" + index + ") is larger than the maximum index (" + this.cssRules.length + ").", 'IndexSizeError');
      this.cssRules.splice(index, 1);
      sync(this);
    };
    SSP.replaceSync = function replaceSync(text) { var sh = this; sh.cssRules = splitRules(text).map(function (t) { return new CSSRule(t, sh); }); sync(sh); };
    SSP.replace = function replace(text) { this.replaceSync(text); return Promise.resolve(this); };
    def('CSSStyleSheet', CSSStyleSheet);
    def('CSSRule', CSSRule);
    function sheetOf(el) {
      var sh = sheets.get(el);
      if (!sh) {
        sh = new CSSStyleSheet(); sh.ownerNode = el; sheets.set(el, sh);
        sh.cssRules = splitRules(el.textContent).map(function (t) { return new CSSRule(t, sh); });
      }
      return sh;
    }
    var HS = window.HTMLStyleElement && window.HTMLStyleElement.prototype;
    if (HS) Object.defineProperty(HS, 'sheet', { get: function () { return this.isConnected ? sheetOf(this) : null; }, configurable: true, enumerable: true });
    Object.defineProperty(Object.getPrototypeOf(document), 'styleSheets', { get: function () {
      var out = [], els = document.querySelectorAll('style');
      for (var i = 0; i < els.length; i++) out.push(sheetOf(els[i]));
      out.item = function (i) { return out[i] || null; };
      return out;
    }, configurable: true, enumerable: true });
  })();

  // HTMLMediaElement: the base the <audio>/<video> prototypes share. There is no decoder, so playback state is just
  // remembered, but feature probes (instanceof HTMLMediaElement, el.play().catch, canPlayType) must not throw.
  (function () {
    function HTMLMediaElement() { throw new TypeError('Illegal constructor'); }
    var MP = HTMLMediaElement.prototype = Object.create(HTMLElement.prototype, { constructor: { value: HTMLMediaElement, writable: true, configurable: true } });
    var st = new WeakMap();
    function S(o) { var v = st.get(o); if (!v) st.set(o, v = { paused: true, currentTime: 0 }); return v; }
    function ro(name, fn) { Object.defineProperty(MP, name, { get: fn, configurable: true, enumerable: true }); }
    ro('paused', function () { return S(this).paused; });
    ro('ended', function () { return false; });
    ro('duration', function () { return NaN; });
    ro('readyState', function () { return 0; });
    ro('networkState', function () { return 0; });
    ro('buffered', function () { return { length: 0, start: function () { return 0; }, end: function () { return 0; } }; });
    ro('seekable', function () { return { length: 0 }; });
    ro('error', function () { return null; });
    Object.defineProperty(MP, 'currentTime', { get: function () { return S(this).currentTime; }, set: function (v) { S(this).currentTime = +v || 0; }, configurable: true, enumerable: true });
    MP.play = function play() { S(this).paused = false; return Promise.resolve(); };
    MP.pause = function pause() { S(this).paused = true; };
    MP.load = function load() { S(this).paused = true; };
    MP.canPlayType = function canPlayType() { return ''; };
    def('HTMLMediaElement', HTMLMediaElement);
    ['HTMLAudioElement', 'HTMLVideoElement'].forEach(function (n) {
      var C = window[n];
      if (C && C.prototype && Object.getPrototypeOf(C.prototype) === HTMLElement.prototype) Object.setPrototypeOf(C.prototype, MP);
    });
  })();
})(globalThis);)JS";

}  // namespace

// ---------------------------------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------------------------------

namespace {

void mark_page(Page& P, Tracer& t) {
  for (auto& kv : P.wrappers) t.mark(kv.second);
  for (const Listener& l : P.listeners) { t.mark(l.fn); t.mark(l.signal); }
  for (const Value& v : P.pinned) t.mark(v);
  t.mark(P.mo_hook);
  for (const Value& v : P.frame_fns) t.mark(v);
  for (const Value& v : P.key_fns) t.mark(v);
  for (const Value& v : P.hist_states) t.mark(v);
  for (auto* list : {&P.raf, &P.deferred, &P.clocked, &P.inflight})
    for (const PendingTimer& pt : *list) { t.mark(pt.fn); for (const Value& v : pt.args) t.mark(v); }
  for (auto& kv : P.event_protos) t.mark(kv.second);
  for (auto& kv : P.tag_protos) t.mark(kv.second);
  for (Object* o : {P.event_target_proto, P.node_proto, P.element_proto, P.html_element_proto, P.text_proto, P.document_proto, P.event_proto, P.node_list_proto,
                    P.classlist_proto, P.style_proto, P.storage_proto, P.document_obj, P.window_obj, P.keys_held, P.local_storage, P.location_obj})
    t.mark(o);
  trace_canvas(P, t);
}

void install_globals(Page& P) {
  Page* pp = &P;
  Context& c = P.ctx;

  P.has_clock = (P.has_host && P.host.now_ms) || g_wall_ms != nullptr;
  P.wall_origin = wall_ms(P);
  P.location_url = P.doc ? P.doc->url : std::string("about:blank");

  c.now_ms = [pp]() { return wall_ms(*pp); };
  // Deterministic on purpose: a page that draws differently on every load is a page nobody can compare. Anything that
  // needs real randomness (crypto) asks the machine for it.
  c.random_double = [pp]() {
    pp->rng ^= pp->rng << 13; pp->rng ^= pp->rng >> 7; pp->rng ^= pp->rng << 17;
    return static_cast<double>(pp->rng >> 11) / 9007199254740992.0;
  };
  c.print = [pp](const std::string& text) {
    if (pp->log.size() >= 100000) return;
    pp->log.push_back(!text.empty() && text.back() == '\n' ? text.substr(0, text.size() - 1) : text);
  };
  c.on_unhandled_rejection = [pp](Value promise, Value reason) {
    Pin pin(*pp);
    pin.add(promise);
    pin.add(reason);
    Object* ev = make_event(*pp, "unhandledrejection", "Event", false, true);
    pin.add(Value::Obj(ev));
    pp->ctx.put(ev, pp->ctx.atom_ascii("promise"), promise, false);
    pp->ctx.put(ev, pp->ctx.atom_ascii("reason"), reason, false);
    dispatch_event_object(*pp, pp->window_obj, pp->window_obj, ev);
    if (event_default_prevented(ev)) return;
    const std::string msg = pp->ctx.describe_exception(reason);
    pp->last_fault = "unhandled promise rejection: " + msg;
    if (pp->dispatch_error.empty()) pp->dispatch_error = pp->last_fault;
    pp->log.push_back("Uncaught (in promise) " + msg);
  };
  c.add_root_tracer([pp](Tracer& t) { mark_page(*pp, t); });

  Object* g = c.global();
  P.window_obj = g;
  install_dom(P);
  g->proto = P.event_target_proto;   // the window is an event target
  install_events(P);
  install_net(P);
  install_storage(P);
  install_canvas(P);
  // The prelude's MutationObserver/custom-element recorder hands itself to the DOM here (see mo_note in dom.cpp).
  c.define_data(g, c.atom_ascii("__aiosMO"), Value::Obj(c.new_host_function("__aiosMO", [pp](Context&, CallArgs& a) -> Value {
    pp->mo_hook = a[0];
    return Value::Undefined();
  }, 1)), js::kHiddenAttrs);

  for (const char* alias : {"window", "self", "top", "parent", "frames"}) def_value(P, g, alias, Value::Obj(g), js::kHiddenAttrs);
  def_value(P, g, "isSecureContext", Value::Bool(false), js::kHiddenAttrs);
  def_value(P, g, "name", Value::String(c.atoms.empty), js::kHiddenAttrs);

  TargetObject* doc = c.heap.make<TargetObject>();
  doc->proto = P.document_proto;
  P.document_obj = doc;
  def_value(P, g, "document", Value::Obj(doc), js::kHiddenAttrs);

  install_location_history(P);
  install_navigator_window(P);
  install_aos(P);

  if (P.has_host && P.host.storage_load) storage_restore(P, P.host.storage_load());

  Value r;
  if (!c.eval_script(kPrelude, "aios:prelude", &r)) {
    fault(P, "platform prelude");
  }
  // The rarely-touched platform objects load on first use (see kLazyStub); the stub reaches their source through this.
  c.define_data(c.global(), c.atom_ascii("__aiosLazySource"),
                Value::Obj(c.new_host_function("", [pp](Context& cc, CallArgs&) -> Value { return Value::String(cc.new_string_utf8(kLazyChunk)); }, 0)), js::kConfigurable);
  if (!c.eval_script(kLazyStub, "aios:prelude-lazy", &r)) fault(P, "platform prelude");
  P.new_budget();
}

// ---- scripts ----------------------------------------------------------------------------------

struct ScriptItem {
  Node* node = nullptr;
  std::string code;
  std::string url;
  bool is_module = false;
  bool nomodule = false;
  bool classic = true;   // a type this engine runs at all; otherwise it is a data block
};

void collect_scripts(Node& n, std::vector<ScriptItem>& out) {
  if (n.is("script")) {
    ScriptItem s;
    s.node = &n;
    std::string type = n.attr("type");
    for (char& ch : type) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const size_t semi = type.find(';');
    if (semi != std::string::npos) type.resize(semi);
    while (!type.empty() && type.back() == ' ') type.pop_back();
    if (type == "module") s.is_module = true;
    else if (!type.empty() && type != "text/javascript" && type != "application/javascript" && type != "text/ecmascript" && type != "application/ecmascript" &&
             type != "application/x-javascript" && type != "text/jscript")
      s.classic = false;
    s.nomodule = n.has_attr("nomodule");
    s.url = n.has_attr("src") ? n.attr("src") : std::string();
    if (s.url.empty())
      for (const auto& ch : n.children)
        if (ch->kind == Node::Kind::Text) s.code += ch->text;
    out.push_back(std::move(s));
    return;
  }
  for (const auto& ch : n.children) collect_scripts(*ch, out);
}

void fire_script_event(Page& P, Node* script, const char* type) {
  Object* ev = make_event(P, type, "Event", false, false);
  Pin pin(P);
  pin.add(Value::Obj(ev));
  NodeObject* w = P.wrap(script);
  pin.add(Value::Obj(w));
  P.new_budget();
  dispatch_event_object(P, script, w, ev);
  if (!P.ctx.run_jobs()) fault(P, "promise job");
}

void run_body_onload(Page& P) {
  if (!P.doc || !P.doc->dom) return;
  Node* body = P.doc->dom->first("body");
  if (!body || !body->has_attr("onload")) return;
  Value fn;
  P.new_budget();
  if (!P.ctx.make_function("event", body->attr("onload"), &fn)) { fault(P, "body onload"); return; }
  run_callback(P, fn, Value::Obj(P.window_obj), {}, "body onload");
}

void dispatch_simple(Page& P, Object* target, const char* type, bool bubbles) {
  Object* ev = make_event(P, type, "Event", bubbles, false);
  Pin pin(P);
  pin.add(Value::Obj(ev));
  P.new_budget();
  dispatch_event_object(P, target, target, ev);
  if (!P.ctx.run_jobs()) fault(P, "promise job");
}

}  // namespace

}  // namespace webjs

// ---------------------------------------------------------------------------------------------------
// JsRuntime
// ---------------------------------------------------------------------------------------------------

struct JsRuntime::Impl {
  webjs::Page page;
  bool installed = false;
  std::unordered_set<const web::Node*> done;   // scripts already run

  void ensure_installed() {
    if (installed) return;
    installed = true;
    webjs::install_globals(page);
  }
};

JsRuntime::JsRuntime() : impl_(std::make_unique<Impl>()) {}
JsRuntime::~JsRuntime() = default;

void JsRuntime::set_host(HostBindings h, int view_w, int view_h) {
  impl_->page.host = std::move(h);
  impl_->page.has_host = true;
  impl_->page.view_w = view_w > 0 ? view_w : 800;
  impl_->page.view_h = view_h > 0 ? view_h : 600;
}

void JsRuntime::set_view(int view_w, int view_h) {
  if (view_w > 0) impl_->page.view_w = view_w;
  if (view_h > 0) impl_->page.view_h = view_h;
}

ScriptRun JsRuntime::run(web::Document& doc, const web::Fetch& fetch, const std::string& base) {
  using namespace webjs;
  ScriptRun r;
  if (!doc.dom) return r;
  Impl& m = *impl_;
  Page& P = m.page;

  {
    std::vector<ScriptItem> first;
    collect_scripts(*doc.dom, first);
    if (first.empty()) return r;
  }

  P.doc = &doc;
  P.fetch = fetch;
  P.base = base.empty() ? doc.base : base;
  m.ensure_installed();

  auto note = [&](const std::string& msg) {
    if (r.error.empty()) r.error = msg;
    else r.error += " | " + msg;
    r.failed++;
  };

  bool has_nomodule = false;
  {
    std::vector<ScriptItem> all;
    collect_scripts(*doc.dom, all);
    for (const ScriptItem& s : all) has_nomodule = has_nomodule || s.nomodule;
  }

  // Scripts run in document order, and a script may add more (a loader appending <script src>): look again after each.
  for (;;) {
    std::vector<ScriptItem> items;
    collect_scripts(*doc.dom, items);
    ScriptItem* next = nullptr;
    for (ScriptItem& s : items) {
      if (m.done.count(s.node) || P.inert_scripts.count(s.node)) continue;
      next = &s;
      break;
    }
    if (!next) break;
    ScriptItem item = *next;
    m.done.insert(item.node);
    if (!item.classic) continue;   // a JSON island, a template: data, not code
    if (item.is_module) {
      P.log.push_back("module script skipped: this engine has no ES modules" + (item.url.empty() ? std::string() : " (" + item.url + ")"));
      if (!has_nomodule) note((item.url.empty() ? std::string("inline module script") : item.url) + ": ES modules are not supported");
      continue;
    }

    std::string src = item.code, where = "inline script " + std::to_string(r.scripts + 1);
    if (!item.url.empty()) {
      const std::string abs = resolve(P, item.url);
      where = abs;
      if (!fetch) {
        note("this page loads " + item.url + " and nothing was given to fetch it with");
        fire_script_event(P, item.node, "error");
        continue;
      }
      auto got = fetch(abs, {});
      if (!got.ok() || got->status >= 400) {
        note("could not fetch " + abs + ": " + (got.ok() ? "HTTP " + std::to_string(got->status) : got.error().message));
        fire_script_event(P, item.node, "error");
        continue;
      }
      src = got->body;
      if (src.empty()) { fire_script_event(P, item.node, "load"); continue; }
    }
    if (src.empty()) continue;

    P.new_budget();
    P.current_script = item.node;
    P.dispatch_error.clear();
    Value result;
    const bool ok = P.ctx.eval_script(src, where, &result);
    P.current_script = nullptr;
    r.scripts++;
    if (!ok) {
      fault(P, where.c_str());
      note(where + ": " + P.last_fault.substr(P.last_fault.find(": ") == std::string::npos ? 0 : P.last_fault.find(": ") + 2));
    } else if (!P.ctx.run_jobs()) {
      fault(P, where.c_str());
    }
    if (!item.url.empty() && ok) fire_script_event(P, item.node, "load");
  }

  // The document is parsed and its scripts have run.
  P.ready_state = "interactive";
  dispatch_simple(P, P.document_obj, "readystatechange", false);
  dispatch_simple(P, P.document_obj, "DOMContentLoaded", true);
  drain(P, 50);
  P.ready_state = "complete";
  dispatch_simple(P, P.document_obj, "readystatechange", false);
  dispatch_simple(P, P.window_obj, "load", false);
  run_body_onload(P);
  // One animation frame, so a page that draws from requestAnimationFrame has drawn once before it is shown; the rest come
  // from the pane's ticks. Whatever it queued with a zero delay runs too.
  run_animation_frames(P);
  drain(P, 50);
  sync_canvases(P);

  r.statements = 0;   // the bytecode engine does not count source statements
  r.dom_writes = P.dom_writes;
  r.listeners = static_cast<int>(P.listeners.size());
  r.log = P.log;
  return r;
}

const std::string& JsRuntime::last_error() const { return impl_->page.dispatch_error; }
const std::vector<std::string>& JsRuntime::network() const { return impl_->page.net; }
int JsRuntime::listener_count() const { return static_cast<int>(impl_->page.listeners.size()); }
const std::vector<PaintOp>& JsRuntime::paint_ops() const { return impl_->page.paint; }
bool JsRuntime::storage_dirty() const { return impl_->page.storage_dirty; }

bool JsRuntime::take_submit_request(web::Node*& form, web::Node*& submitter) {
  webjs::Page& P = impl_->page;
  form = P.submit_form;
  submitter = P.submit_by;
  P.submit_form = nullptr;
  P.submit_by = nullptr;
  return form != nullptr;
}

std::string JsRuntime::take_navigation_request() {
  std::string u = std::move(impl_->page.nav_url);
  impl_->page.nav_url.clear();
  return u;
}

web::Node* JsRuntime::take_focus_request() {
  web::Node* n = impl_->page.focus_request;
  impl_->page.focus_request = nullptr;
  return n;
}

std::string JsRuntime::take_fault() {
  std::string f = impl_->page.last_fault;
  impl_->page.last_fault.clear();
  return f;
}

void JsRuntime::flush_storage() {
  webjs::Page& P = impl_->page;
  if (!P.storage_dirty) return;
  P.storage_dirty = false;
  if (P.has_host && P.host.storage_save) P.host.storage_save(webjs::storage_serialise(P));
}

bool JsRuntime::live() const {
  const webjs::Page& P = impl_->page;
  return !P.frame_fns.empty() || !P.clocked.empty() || !P.raf.empty() || !P.deferred.empty();
}

bool JsRuntime::wants_keys() const {
  const webjs::Page& P = impl_->page;
  if (!P.key_fns.empty()) return true;
  for (const webjs::Listener& l : P.listeners)
    if (l.type == "keydown" || l.type == "keyup" || l.type == "keypress") return true;
  return false;
}

bool JsRuntime::has_handler(web::Node* target, const std::string& type) const {
  const webjs::Page& P = impl_->page;
  auto listens = [&](const void* key) {
    for (const webjs::Listener& l : P.listeners)
      if (l.target == key && l.type == type) return true;
    return false;
  };
  for (web::Node* n = target; n; n = n->parent) {
    if (listens(n)) return true;
    if (n->kind == web::Node::Kind::Element && n->has_attr("on" + type)) return true;
  }
  return listens(P.document_obj) || listens(P.window_obj);
}

int JsRuntime::tick(double dt_ms, int* dom_writes) {
  using namespace webjs;
  Page& P = impl_->page;
  if (!impl_->installed) { if (dom_writes) *dom_writes = 0; return 0; }
  const int before = P.dom_writes;
  int ran = 0;
  if (!std::isfinite(dt_ms) || dt_ms < 0) dt_ms = 0;
  if (dt_ms > 250) dt_ms = 250;   // a stall is not a leap: physics that steps by seconds explodes
  P.now_ms += dt_ms;

  {
    std::vector<Value> batch = P.frame_fns;
    Pin pin(P);
    for (const Value& v : batch) pin.add(v);
    for (const Value& fn : batch) {
      run_callback(P, fn, Value::Undefined(), {Value::Number(dt_ms), Value::Number(P.now_ms)}, "frame handler");
      ++ran;
    }
  }
  {
    const size_t n = P.raf.size();
    run_animation_frames(P);
    ran += static_cast<int>(n);
  }
  // Timers that have come due. An interval that fell behind runs once and is rescheduled from now, not once per missed period.
  {
    std::vector<int> due;
    for (const PendingTimer& t : P.clocked)
      if (t.due_ms <= P.now_ms) due.push_back(t.id);
    for (int id : due) {
      size_t at = P.clocked.size();
      for (size_t i = 0; i < P.clocked.size(); ++i)
        if (P.clocked[i].id == id) { at = i; break; }
      if (at == P.clocked.size()) continue;   // cleared by an earlier callback
      PendingTimer t = P.clocked[at];
      if (t.repeat) P.clocked[at].due_ms = P.now_ms + t.every_ms;
      else P.clocked.erase(P.clocked.begin() + static_cast<long>(at));
      run_callback(P, t.fn, Value::Undefined(), t.args, "timer");
      ++ran;
    }
  }
  drain(P, 1);
  sync_canvases(P);
  if (dom_writes) *dom_writes = P.dom_writes - before;
  return ran;
}

int JsRuntime::key(const std::string& name, bool down, int* dom_writes) {
  using namespace webjs;
  Page& P = impl_->page;
  if (!impl_->installed) { if (dom_writes) *dom_writes = 0; return 0; }
  const int before = P.dom_writes;
  int ran = 0;
  if (P.keys_held) {
    Context& c = P.ctx;
    if (down) {
      c.put(P.keys_held, c.atom_utf8(name), Value::Bool(true), false);
    } else {
      bool ok;
      c.delete_prop(P.keys_held, c.atom_utf8(name), &ok);
    }
  }
  {
    Context& c = P.ctx;
    Object* ev = c.new_object();
    Pin pin(P);
    pin.add(Value::Obj(ev));
    c.define_data(ev, c.atom_ascii("type"), str(P, down ? "keydown" : "keyup"));
    c.define_data(ev, c.atom_ascii("key"), str(P, name));
    c.define_data(ev, c.atom_ascii("down"), Value::Bool(down));
    std::vector<Value> batch = P.key_fns;
    for (const Value& v : batch) pin.add(v);
    for (const Value& fn : batch) {
      run_callback(P, fn, Value::Undefined(), {Value::Obj(ev)}, "aos.onKey handler");
      ++ran;
    }
  }
  // The document's own keydown/keyup listeners: a page that binds `document.addEventListener('keydown', ...)` is every
  // game ever written.
  EventInit init;
  init.key = name;
  int wrote = 0;
  ran += dispatch(nullptr, down ? "keydown" : "keyup", &wrote, &init, nullptr);
  sync_canvases(P);
  if (dom_writes) *dom_writes = P.dom_writes - before;
  return ran;
}

int JsRuntime::dispatch(web::Node* target, const std::string& type, int* dom_writes) {
  return dispatch(target, type, dom_writes, nullptr, nullptr);
}

int JsRuntime::dispatch(web::Node* target, const std::string& type, int* dom_writes, const EventInit* init, bool* prevented) {
  using namespace webjs;
  Page& P = impl_->page;
  if (!impl_->installed) {
    if (dom_writes) *dom_writes = 0;
    if (prevented) *prevented = false;
    return 0;
  }
  Context& c = P.ctx;
  const int before = P.dom_writes;
  P.dispatch_error.clear();
  P.new_budget();

  const char* cls = "Event";
  bool cancelable = true;
  if (init) cls = "KeyboardEvent";
  else if (type == "click" || type == "dblclick" || type == "mousedown" || type == "mouseup" || type == "mousemove" || type == "mouseover" || type == "mouseout" || type == "contextmenu") cls = "MouseEvent";
  else if (type == "pointerdown" || type == "pointerup" || type == "pointermove") cls = "PointerEvent";
  else if (type == "wheel") cls = "WheelEvent";
  else if (type == "input") cls = "InputEvent";
  else if (type == "focus" || type == "blur" || type == "focusin" || type == "focusout") { cls = "FocusEvent"; cancelable = false; }
  else if (type == "change") cancelable = false;
  const bool bubbles = type != "focus" && type != "blur" && type != "mouseenter" && type != "mouseleave";

  Object* ev = make_event(P, type, cls, bubbles, cancelable);
  Pin pin(P);
  pin.add(Value::Obj(ev));
  if (init) {
    int code = 0;
    if (init->key == "Enter") code = 13;
    else if (init->key == "Escape") code = 27;
    else if (init->key == "Backspace") code = 8;
    else if (init->key == "Tab") code = 9;
    else if (init->key == "ArrowLeft") code = 37;
    else if (init->key == "ArrowUp") code = 38;
    else if (init->key == "ArrowRight") code = 39;
    else if (init->key == "ArrowDown") code = 40;
    else if (init->key == " ") code = 32;
    else if (init->key.size() == 1) code = static_cast<int>(static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(init->key[0]))));
    auto set = [&](const char* k, Value v) { c.put(ev, c.atom_utf8(k), v, false); };
    set("key", str(P, init->key));
    set("code", str(P, init->key));
    set("shiftKey", Value::Bool(init->shift));
    set("ctrlKey", Value::Bool(init->ctrl));
    set("altKey", Value::Bool(init->alt));
    set("metaKey", Value::Bool(init->meta));
    set("keyCode", Value::Number(code));
    set("which", Value::Number(code));
  }
  Object* target_obj = target ? P.wrap(target) : P.document_obj;
  const void* key = target ? static_cast<const void*>(target) : static_cast<const void*>(P.document_obj);
  pin.add(Value::Obj(target_obj));
  const int ran = dispatch_event_object(P, key, target_obj, ev);
  if (!c.run_jobs()) fault(P, "promise job");
  drain(P, 1);
  sync_canvases(P);
  if (prevented) *prevented = event_default_prevented(ev);
  if (dom_writes) *dom_writes = P.dom_writes - before;
  return ran;
}

std::string JsRuntime::eval(const std::string& source, std::string& error, int* dom_writes) {
  using namespace webjs;
  Impl& m = *impl_;
  m.ensure_installed();
  Page& P = m.page;
  const int before = P.dom_writes;
  error.clear();
  P.new_budget();
  Value result;
  std::string out;
  if (!P.ctx.eval_script(source, "eval", &result)) {
    if (P.ctx.step_budget_exceeded) {
      P.ctx.clear_exception();
      P.ctx.step_budget_exceeded = false;
      error = "this script ran too long and was stopped (over " + std::to_string(Page::kStepBudget) + " steps)";
    } else {
      error = P.ctx.describe_exception(P.ctx.take_exception());
    }
  } else {
    Pin pin(P);
    pin.add(result);
    out = result.is_string() ? result.as_string()->utf8() : P.ctx.inspect(result);
  }
  if (!P.ctx.run_jobs()) fault(P, "promise job");
  drain(P, 1);
  sync_canvases(P);
  if (dom_writes) *dom_writes = P.dom_writes - before;
  return out;
}

ScriptRun run_document_scripts(web::Document& doc, const web::Fetch& fetch, const std::string& base) {
  JsRuntime rt;
  return rt.run(doc, fetch, base);
}

std::string js_eval_to_text(const std::string& source, std::string& error) {
  JsRuntime rt;
  int writes = 0;
  std::string result = rt.eval(source, error, &writes);
  if (!error.empty()) return std::string();
  std::string out;
  for (const std::string& l : rt.impl_->page.log) out += l + "\n";
  // `js 1+1` is a question: the answer is the value. A script that only logged has said its piece.
  if (!result.empty() && result != "undefined") out += result;
  else if (out.empty()) out = result;
  return out;
}

const char* js_supported_features() {
  return
    "Language: ES5 and ES2015 through ES2022 for classic scripts, run by a bytecode VM: let/const, classes (fields,\n"
    "  static, private names, accessors, extends/super), arrow functions, generators, async/await, async generators,\n"
    "  destructuring, spread/rest, default parameters, template literals (tagged too), optional chaining, ??, logical\n"
    "  assignment, **, labelled statements, getters/setters, Symbol, iterators, Proxy and Reflect, WeakRef,\n"
    "  FinalizationRegistry; toLocaleString and Date formatting are en-US only.\n"
    "Built-ins: Object, Function, Array, String, Number, Boolean, Math, JSON, Date (UTC plus the host's zone), RegExp\n"
    "  (named groups, lookbehind, sticky, unicode, \\p{...} by general category and script), Map, Set, WeakMap, WeakSet,\n"
    "  Promise (all, allSettled, any, race), BigInt (literals, every operator, asIntN/asUintN, BigInt64Array and\n"
    "  BigUint64Array, DataView get/setBigInt64), typed arrays, ArrayBuffer, DataView, TextEncoder/TextDecoder, atob/btoa,\n"
    "  structuredClone, queueMicrotask, console.\n"
    "NOT here: ES modules (import/export; <script type=module> is skipped and reported), direct eval (indirect\n"
    "  eval works), a mapped `arguments` object, the Intl object, SharedArrayBuffer/Atomics, WebAssembly, WebGL, Web\n"
    "  Workers, service workers, IndexedDB, WebSocket, WebRTC, Web Crypto subtle (crypto.getRandomValues and\n"
    "  randomUUID are real when the machine gave the page an entropy source), drag and drop,\n"
    "  CSS transitions/animations, @font-face (document.fonts exists: load() resolves with no faces, check() is true).\n"
    "DOM: documents, elements, text nodes with the real prototype chain (EventTarget > Node > Element > HTMLElement >\n"
    "  HTMLInputElement ...), querySelector/All with the full selector grammar the stylesheet engine has, children and\n"
    "  childNodes (plain arrays, not live), classList, dataset, style (parsed declarations, camelCase and kebab),\n"
    "  attributes, innerHTML/outerHTML/insertAdjacentHTML, forms (value, checked, selected, options, FormData),\n"
    "  focus(), click(), cookies (a per-page jar). Elements report zero for layout geometry (getBoundingClientRect,\n"
    "  offsetWidth ...): no box measurement is exposed to script. getComputedStyle returns the element's own style.\n"
    "Events: the full capture/target/bubble path through the node chain, document and window; on* properties and inline\n"
    "  handlers; Event, CustomEvent, UIEvent, MouseEvent, PointerEvent, WheelEvent, KeyboardEvent, FocusEvent, InputEvent,\n"
    "  MessageEvent, ErrorEvent, ProgressEvent, HashChangeEvent, PopStateEvent, StorageEvent; once/passive/signal\n"
    "  options; stopPropagation, stopImmediatePropagation, preventDefault. window 'error' and 'unhandledrejection'.\n"
    "  MutationObserver delivers real records at microtask time; ResizeObserver and IntersectionObserver deliver one\n"
    "  initial notification (there are no per-element boxes to watch). Custom elements (define, upgrade, connected/\n"
    "  disconnected/attributeChanged callbacks), attachShadow (an unrendered fragment), Range and Selection (positions,\n"
    "  text, no boxes).\n"
    "Network: fetch, XMLHttpRequest, Headers, Request, Response (text, json, arrayBuffer, blob, formData, one-chunk\n"
    "  body stream), URL, URLSearchParams, Blob, File, FormData, AbortController, data: and blob: URLs. Requests go\n"
    "  through the machine's own transport; a transport failure rejects fetch with TypeError.\n"
    "CSS: CSS.escape (the CSSOM algorithm) and CSS.supports, which says whether a declaration PARSES, not whether layout\n"
    "  implements it.\n"
    "Window: location (read; assigning it is reported, not followed), history (pushState, replaceState, back, forward,\n"
    "  popstate, hashchange within the document), navigator, screen, matchMedia, performance, MessageChannel,\n"
    "  postMessage, timers, requestAnimationFrame, requestIdleCallback, localStorage (kept by the machine across\n"
    "  reboots), sessionStorage, alert/confirm/prompt (logged; there is no dialog).\n"
    "Canvas: CanvasRenderingContext2D with transforms, paths (lines, arcs, ellipses, curves, arcTo, roundRect), fill with\n"
    "  nonzero/evenodd, stroke with width/cap/join/miter/dash, clip, linear and radial gradients, globalAlpha,\n"
    "  compositing (source-over, destination-out, copy), drawImage, getImageData/putImageData, toDataURL/toBlob (PNG).\n"
    "  Text is drawn in a built-in 5x7 bitmap font, so metrics and glyphs differ from a browser's.\n"
    "Media: Image decodes PNG, GIF and baseline JPEG (what the page decoder does); Audio and AudioContext oscillators play short tones; no media decoding.\n"
    "Scripts: inline and external (<script src> fetched in document order); a failing script is isolated from the ones\n"
    "  after it; scripts added to the document by script run; scripts parsed from a string (innerHTML) do not.\n"
    "A callback is stopped after two hundred million steps, so a runaway loop on a page cannot take the desktop with\n"
    "it; every frame, timer, handler and script gets a fresh budget.\n"
    "\n"
    "aos — the machine itself, from a page:\n"
    "  aos.store.put(kind, object) -> id     a record of this kind, as a kernel object\n"
    "                                        with identity, provenance and an embedding\n"
    "  aos.store.list(kind) -> [objects]     every record of that kind on this machine,\n"
    "                                        newest first, each with _id, _t (ms), _kind\n"
    "  aos.store.get(id) -> object | null\n"
    "  aos.store.similar(text, k) -> [objects]   semantic search over every record\n"
    "  aos.paint.clear(color)                straight onto the pane, over the document;\n"
    "  aos.paint.rect(x,y,w,h,color)         call clear at the top of every frame\n"
    "  aos.paint.outline(x,y,w,h,color)      colors: '#rrggbb', '#rgb', 0xRRGGBB, or a\n"
    "  aos.paint.text(x,y,text,color,px)     name (white black red green blue yellow...)\n"
    "  aos.paint.line(x1,y1,x2,y2,color)     coordinates are pane pixels, origin top-left\n"
    "  aos.paint.circle(cx,cy,r,color)\n"
    "  aos.size() -> {w,h}                   the pane, in pixels (also aos.width/height)\n"
    "  aos.onFrame(fn(dt_ms, now_ms))        every frame while shown — a game loop\n"
    "  aos.onKey(fn({type:'keydown'|'keyup', key}))   keys: 'ArrowLeft' 'ArrowRight'\n"
    "                                        'ArrowUp' 'ArrowDown' ' ' 'Enter' 'Escape' 'a'..'z'\n"
    "  aos.keys[key] === true                while a key is held\n"
    "  aos.now() -> ms since the epoch       the machine's clock — the SAME clock as a record's _t,\n"
    "                                        so aos.now() - r._t is how old a record is\n"
    "  aos.elapsed() -> ms                   since this surface loaded (what onFrame's now is)\n"
    "  aos.organ(name, request) -> text      call an organ this machine has grown\n"
    "  aos.say(text)                         to the machine's log\n";
}

}  // namespace aios::userland
