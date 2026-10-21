#pragma once
// The page's JavaScript heap and everything bound into it: the DOM, events, timers, the network surface, storage,
// canvas and audio. One Page per JsRuntime. The files in this directory each install one slice of the global object:
//
//   dom.cpp     nodes, documents, collections, style, classList, dataset, events
//   net.cpp     fetch, XMLHttpRequest, URL, Headers, Request, Response, Blob, FormData, AbortController, storage
//   canvas.cpp  CanvasRenderingContext2D, ImageData, Image, Audio, AudioContext
//   ../web_js.cpp  the globals (window, location, navigator, timers, aos), running a page's scripts, dispatching input
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "aios/js/context.hpp"
#include "aios/userland/web.hpp"
#include "aios/userland/web_js.hpp"

namespace aios::userland::webjs {

using js::CallArgs;
using js::Context;
using js::Function;
using js::Object;
using js::Value;
using web::Node;

class Page;

// What a host object is, so a native can check `this` without RTTI.
enum class HostKind : uint8_t { Node, Document, ClassList, Style, Dataset, Storage, Event, Canvas2D, CanvasCtx, Gradient, Image, Headers, Response, Request, Blob, FormData, Url, SearchParams, Abort, Xhr, Target };

struct HostObject : Object {
  HostObject(HostKind k) : Object(js::Class::Host), host_kind(k) {}
  HostKind host_kind;
};

// A node of the page's tree, as a script sees it. The Node itself is owned by the tree (or by Page::orphans).
struct NodeObject final : HostObject {
  NodeObject(Node* n) : HostObject(HostKind::Node), dom(n) {}
  Node* dom;
  Object* class_list = nullptr;
  Object* style = nullptr;
  Object* dataset = nullptr;
  void trace(js::Tracer& t) override;
};

// `document` and `window` are event targets that are not nodes.
struct TargetObject final : HostObject {
  TargetObject() : HostObject(HostKind::Target) {}
};

struct Listener {
  const void* target = nullptr;   // a Node*, or the Object* of document / window / any other EventTarget
  std::string type;
  Value fn;                       // a function, or an object with handleEvent
  bool capture = false, once = false, passive = false;
  Value signal;                   // an AbortSignal that removes the listener when it aborts
  int id = 0;
  bool is_property = false;       // installed by `el.onclick = fn`
};

struct PendingTimer {
  int id = 0;
  Value fn;
  std::vector<Value> args;
  double every_ms = 0, due_ms = 0;
  bool repeat = false;
};

class Page {
 public:
  Page();
  ~Page();
  Page(const Page&) = delete;
  Page& operator=(const Page&) = delete;

  std::unique_ptr<Context> ctx_owner;
  Context& ctx;
  web::Document* doc = nullptr;
  HostBindings host;
  bool has_host = false;
  web::Fetch fetch;
  std::string base;
  int view_w = 800, view_h = 600;

  // ---- reporting ----
  std::vector<std::string> log;      // console output, line by line
  std::vector<std::string> net;      // requests the page made
  std::string last_fault;            // the last thing a handler, timer or frame did wrong
  std::string dispatch_error;        // the first failure of the last dispatch
  int dom_writes = 0;
  int listener_count = 0;

  // ---- limits ----
  static constexpr uint64_t kStepBudget = 200000000;
  void new_budget() { ctx.steps = 0; ctx.step_budget = kStepBudget; ctx.step_budget_exceeded = false; }

  // ---- DOM wrappers ----
  std::unordered_map<Node*, NodeObject*> wrappers;
  std::vector<web::NodePtr> orphans;     // nodes a script made or removed and still holds
  Object* node_protos_ = nullptr;         // unused slot kept for layout stability
  NodeObject* wrap(Node* n);
  Value node_value(Node* n) { return n ? Value::Obj(wrap(n)) : Value::Null(); }
  void forget_subtree(Node* n);
  Node* take_orphan(Node* n);            // releases n from the orphan list (ownership moves to the tree)
  void adopt_orphan(web::NodePtr n);
  Object* document_obj = nullptr;
  Object* window_obj = nullptr;          // the global object

  // ---- prototypes ----
  Object* event_target_proto = nullptr;
  Object* node_proto = nullptr;
  Object* element_proto = nullptr;
  Object* html_element_proto = nullptr;
  Object* text_proto = nullptr;
  Object* document_proto = nullptr;
  Object* event_proto = nullptr;
  Object* node_list_proto = nullptr;
  std::unordered_map<std::string, Object*> tag_protos;   // by tag: input, textarea, canvas, ...
  Object* classlist_proto = nullptr;
  Object* style_proto = nullptr;
  Object* storage_proto = nullptr;

  // ---- events ----
  std::vector<Listener> listeners;
  int next_listener_id = 1;
  std::unordered_map<std::string, Object*> event_protos;   // by constructor name: Event, MouseEvent, ...
  bool prevented = false;                // preventDefault() on the event being dispatched from the pane
  int dispatch_depth = 0;
  // Values the host holds across calls into script (a snapshot of listeners, a batch of timers): the collector marks them.
  std::vector<Value> pinned;

  // ---- time ----
  std::vector<Value> frame_fns, key_fns;
  std::vector<PendingTimer> raf;
  std::vector<PendingTimer> deferred;    // zero-delay timers and queued tasks
  std::vector<PendingTimer> clocked;
  int next_timer_id = 1;
  double now_ms = 0;

  // ---- the machine ----
  std::vector<PaintOp> paint;
  Node* focus_request = nullptr;
  // What the script asked the pane to DO, once: submit a form (`form.submit()`,
  // `requestSubmit()`, a submit button's `click()`) or go to an address
  // (`location.assign`, `location.href = ...`). The script cannot fetch a page
  // itself; the pane owns the browser, so it takes these after the handler.
  Node* submit_form = nullptr;
  Node* submit_by = nullptr;
  std::string nav_url;
  Node* focused = nullptr;
  Object* keys_held = nullptr;
  bool storage_dirty = false;
  std::string cookie_jar;                // "name=value; name2=value2"
  std::string location_url;              // the page's address as script sees it (pushState moves it)
  std::string ready_state = "loading";   // loading, interactive, complete
  Node* current_script = nullptr;        // the <script> being run, for document.currentScript
  double wall_origin = 0;                // the wall clock when the page began, for performance.now()
  bool has_clock = false;                // a real clock was bound (else page time is the only time there is)
  uint64_t rng = 0x2545F4914F6CDD1Dull;  // Math.random: deterministic by design, so a frame can be reproduced
  std::unordered_set<const Node*> inert_scripts;   // <script> elements made from a string: never executed
  std::vector<PendingTimer> inflight;    // a batch of timers being run (so the collector still sees them)
  std::vector<std::string> hist_urls;    // same-document history: pushState / replaceState / back / forward
  std::vector<Value> hist_states;
  int hist_idx = 0;
  Object* location_obj = nullptr;
  struct CanvasHost;
  std::shared_ptr<CanvasHost> canvas_host;
  Object* local_storage = nullptr;

  // ---- helpers shared by the files ----
  // Report an exception left pending by script the page called: remembered as the last fault, and cleared.
  void note_fault(const char* where);
  // Call a script function from the host with a fresh budget not applied (the caller sets it). False if it threw.
  bool call(const Value& fn, const Value& self, std::vector<Value> args, Value* out = nullptr);
  void run_jobs() { ctx.run_jobs(); }
  std::string text(const Value& v);      // ToString, "" on failure
  double number(const Value& v);
  void write() { ++dom_writes; }
  void add_timer(std::vector<PendingTimer>& list, PendingTimer t, size_t cap);

  // Last in the struct so objects built before this field existed still see every other member where they expect it.
  Value mo_hook;                         // the prelude's MutationObserver recorder; undefined until a page makes an observer
  bool mo_busy = false;                  // inside the recorder: its own DOM reads/writes are not observed
};

// RAII pin for a value the host keeps in a C++ local across script calls.
struct Pin {
  Page& p;
  size_t n;
  Pin(Page& page) : p(page), n(page.pinned.size()) {}
  void add(const Value& v) { p.pinned.push_back(v); }
  ~Pin() { p.pinned.resize(n); }
};

// ---- binding helpers ----
using HostFn = js::HostFn;
Function* def_method(Page& P, Object* target, const char* name, int length, HostFn fn);
void def_accessor(Page& P, Object* target, const char* name, HostFn getter, HostFn setter = nullptr);
Function* def_ctor(Page& P, const char* name, int length, Object* proto, HostFn fn);   // installs on the global object
Object* make_proto(Page& P, Object* parent, const char* tag_name);
// `this` as a node / host object of a kind, or null with a TypeError pending.
NodeObject* this_node(Page& P, CallArgs& a);
HostObject* this_host(Page& P, CallArgs& a, HostKind kind, const char* what);
std::string arg_text(Page& P, CallArgs& a, int i);
Value make_array(Page& P, const std::vector<Value>& v);

// localStorage on the wire between the page and the machine: one item per line, key and value hex-encoded.
std::string storage_serialise(Page& P);
void storage_restore(Page& P, const std::string& text);

// ---- install functions ----
void install_dom(Page& P);
void install_events(Page& P);
void install_net(Page& P);
void install_storage(Page& P);
void install_canvas(Page& P);
void sync_canvases(Page& P);                  // copy drawn-on canvases into the document's inline images
void forget_canvas(Page& P, Node* n);
void trace_canvas(Page& P, js::Tracer& t);

// ---- shared entry points ----
// Dispatch an event object to a target and its ancestors the way a browser does (capture, target, bubble). Returns the
// number of handlers that ran.
struct EventPath;
int dispatch_event_object(Page& P, const void* target_key, Object* target_obj, Object* event);
Object* make_event(Page& P, const std::string& type, const char* ctor_name, bool bubbles, bool cancelable);
bool event_default_prevented(Object* event);
// All elements under `n` matching a selector (document order); excludes `n` itself unless `include_self`.
void query_all(Node* n, const std::string& sel, std::vector<Node*>& out, bool include_self);
Node* find_by_id(Node* n, const std::string& id);
std::string serialize_html(const Node& n, bool outer);
std::string resolve(Page& P, const std::string& url);
// Network objects other files make.
Value make_response_value(Page& P, int status, const std::string& status_text, const std::string& url, const std::string& body, const std::string& content_type, bool ok_transport);
Value resolved_promise(Page& P, const Value& v);
Value rejected_promise(Page& P, const Value& v);

}  // namespace aios::userland::webjs
