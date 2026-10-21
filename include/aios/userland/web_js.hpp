#pragma once
// A JavaScript engine for this machine's web engine.
//
// Without one, a page that assembles itself in script rendered its skeleton and
// nothing else: the structure and the text that happened to be in the markup,
// no content, no behaviour. Every application interface written in the last
// fifteen years is that page.
//
// This is a real interpreter — lexer, parser, evaluator, closures — bound to
// the SAME DOM the layout engine walks, so a script that builds nodes builds
// the thing that gets drawn. It is not a complete ES: what it covers is listed
// against `js features`, and it says what it met and could not do rather than
// failing silently, because a script that half-ran is the worst of the three
// possible outcomes.
//
// No exceptions: this file is in the bare-metal CORE, which is built
// -fno-exceptions. Errors come back in the report.
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "aios/userland/web.hpp"

namespace aios::userland {

// Runs every inline <script> in the document, in order, against doc's own DOM.
// Does NOT lay out — layout() is idempotent and the caller owns the metrics.
// `fetch` and `base` let an external <script src> be brought in the way the
// stylesheets already are. Pass an empty fetch to run inline scripts only —
// every real application interface is an external bundle, so without this the
// engine would only ever run demo pages.
ScriptRun run_document_scripts(web::Document& doc, const web::Fetch& fetch = {},
                               const std::string& base = {});

// A page's JavaScript, kept alive for as long as the page is.
//
// run_document_scripts above builds an interpreter, runs the scripts and throws
// the heap away — which is fine for a page that only draws itself once, and
// useless for one with handlers: by the time a person clicks, the closure that
// was registered no longer exists. A Browser owns one of these per page so the
// heap, the listeners and the DOM wrappers all outlive the load.
// What a dispatched keyboard event carries.
struct EventInit {
  std::string key;
  bool shift = false;
  bool ctrl = false;
  bool alt = false;
  bool meta = false;
};

class JsRuntime;
std::string js_eval_to_text(const std::string& source, std::string& error);

class JsRuntime {
 public:
  JsRuntime();
  ~JsRuntime();
  JsRuntime(const JsRuntime&) = delete;
  JsRuntime& operator=(const JsRuntime&) = delete;

  // Bind the machine before run(): `aos` is built from these at install time.
  void set_host(HostBindings h, int view_w, int view_h);
  void set_view(int view_w, int view_h);

  // Run every script in the document. Same contract as the free function.
  ScriptRun run(web::Document& doc, const web::Fetch& fetch, const std::string& base);


  // One frame of the page's own time: aos.onFrame handlers, due timers and
  // intervals, requestAnimationFrame callbacks. Returns how many ran.
  int tick(double dt_ms, int* dom_writes);
  // Whether the page has anything that wants frames: a frame handler, a
  // pending timer, an interval, or an animation frame request.
  bool live() const;
  // A key, pressed or released, to aos.onKey and the document's keydown/keyup.
  int key(const std::string& name, bool down, int* dom_writes);
  // Whether anything on the page listens for keys at all — so a pane knows
  // whether to hand the keyboard over or keep it for scrolling.
  bool wants_keys() const;

  // What aos.paint drew, in order. Cleared by aos.paint.clear().
  const std::vector<PaintOp>& paint_ops() const;
  // An element the script asked to focus (`el.focus()`), once. Null when none.
  web::Node* take_focus_request();
  // A form the script asked to submit, once. False when there is none.
  bool take_submit_request(web::Node*& form, web::Node*& submitter);
  // An address the script asked to go to, once. Empty when none.
  std::string take_navigation_request();

  // The last error a handler, timer or frame raised since this was last asked,
  // or "". The page keeps running; this is for whoever is looking at it.
  std::string take_fault();

  // localStorage changed since it was last saved through the host.
  bool storage_dirty() const;
  void flush_storage();

  // Deliver an event to a node and then to its ancestors, the way bubbling
  // works. Returns how many handlers ran; *dom_writes gets how many DOM
  // mutations they made, so the caller knows whether to lay out again.
  int dispatch(web::Node* target, const std::string& type, int* dom_writes);

  // The same, with a keyboard event's own fields, and with the answer to
  // "did a handler call preventDefault()" -- which is how a pane knows whether
  // the page has taken the keystroke or it still has to type it into the box.
  int dispatch(web::Node* target, const std::string& type, int* dom_writes,
               const EventInit* init, bool* prevented);

  // Evaluate source IN THIS PAGE'S heap, with its DOM and everything its
  // scripts defined. The only way to ask a running application a question.
  std::string eval(const std::string& source, std::string& error, int* dom_writes = nullptr);

  // Every request the page's script has made: method, url, and what came
  // back. An interface that looks inert is usually one whose request failed.
  const std::vector<std::string>& network() const;

  // What the last dispatch's handlers failed on, or empty. A handler that
  // throws is caught so the others still run -- this is the only way the error
  // reaches a person.
  const std::string& last_error() const;

  // Whether anything would run for this event on this node or its ancestors.
  // A pane has to know BEFORE it dispatches: a click with a handler belongs to
  // the page, one without it may be a link to follow.
  bool has_handler(web::Node* target, const std::string& type) const;

  // How many handlers are registered, so a pane can say whether a page is
  // interactive at all.
  int listener_count() const;

 private:
  friend std::string js_eval_to_text(const std::string& source, std::string& error);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Evaluate one expression or program and return its value as text. For `js`
// on the console, and for proving the evaluator without a page.
std::string js_eval_to_text(const std::string& source, std::string& error);

// Where a page's clock comes from. Set once by whatever owns the substrate --
// the desktop does it from the kernel's wall clock -- so `Date.now()` and
// `new Date()` are the real instant. Unset, the engine uses a fixed one and
// says so, rather than pretending on a machine with no clock.
void js_set_clock(double (*wall_ms)());

// What this engine supports, in one place, so the answer is not folklore.
const char* js_supported_features();

}  // namespace aios::userland
