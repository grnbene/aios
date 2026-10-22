// Events: the Event hierarchy, EventTarget, and dispatch with capture, target and bubble phases.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "page.hpp"

namespace aios::userland::webjs {

using js::PropKey;
using js::Tracer;

namespace {

struct EventObject final : HostObject {
  EventObject() : HostObject(HostKind::Event) {}
  bool bubbles = false, cancelable = false, composed = false;
  bool stop_prop = false, stop_imm = false, canceled = false, dispatching = false, in_passive = false;
  bool trusted = false;
  int phase = 0;
  Object* target = nullptr;
  Object* current = nullptr;
  void trace(Tracer& t) override {
    Object::trace(t);
    t.mark(target);
    t.mark(current);
  }
};

EventObject* as_event(const Value& v) {
  if (v.is_object() && v.as_object()->cls == js::Class::Host && static_cast<HostObject*>(v.as_object())->host_kind == HostKind::Event) return static_cast<EventObject*>(v.as_object());
  return nullptr;
}

EventObject* this_event(Page& P, CallArgs& a) {
  EventObject* e = as_event(a.this_value);
  if (!e) P.ctx.throw_type_error("Illegal invocation");
  return e;
}

struct Field {
  const char* name;
  int kind;   // 0 string "", 1 number 0, 2 bool false, 3 null, 4 undefined
};

struct EventClass {
  const char* name;
  const char* parent;
  std::vector<Field> fields;
};

const EventClass kClasses[] = {
    {"Event", nullptr, {}},
    {"CustomEvent", "Event", {{"detail", 3}}},
    {"UIEvent", "Event", {{"view", 3}, {"detail", 1}}},
    {"MouseEvent", "UIEvent", {{"screenX", 1}, {"screenY", 1}, {"clientX", 1}, {"clientY", 1}, {"pageX", 1}, {"pageY", 1}, {"offsetX", 1}, {"offsetY", 1}, {"movementX", 1}, {"movementY", 1},
                               {"button", 1}, {"buttons", 1}, {"ctrlKey", 2}, {"shiftKey", 2}, {"altKey", 2}, {"metaKey", 2}, {"relatedTarget", 3}}},
    {"PointerEvent", "MouseEvent", {{"pointerId", 1}, {"pointerType", 0}, {"isPrimary", 2}, {"width", 1}, {"height", 1}, {"pressure", 1}}},
    {"WheelEvent", "MouseEvent", {{"deltaX", 1}, {"deltaY", 1}, {"deltaZ", 1}, {"deltaMode", 1}}},
    {"KeyboardEvent", "UIEvent", {{"key", 0}, {"code", 0}, {"location", 1}, {"repeat", 2}, {"isComposing", 2}, {"ctrlKey", 2}, {"shiftKey", 2}, {"altKey", 2}, {"metaKey", 2},
                                  {"keyCode", 1}, {"charCode", 1}, {"which", 1}}},
    {"FocusEvent", "UIEvent", {{"relatedTarget", 3}}},
    {"InputEvent", "UIEvent", {{"data", 3}, {"inputType", 0}, {"isComposing", 2}}},
    {"MessageEvent", "Event", {{"data", 3}, {"origin", 0}, {"lastEventId", 0}, {"source", 3}}},
    {"ErrorEvent", "Event", {{"message", 0}, {"filename", 0}, {"lineno", 1}, {"colno", 1}, {"error", 3}}},
    {"ProgressEvent", "Event", {{"lengthComputable", 2}, {"loaded", 1}, {"total", 1}}},
    {"HashChangeEvent", "Event", {{"oldURL", 0}, {"newURL", 0}}},
    {"PopStateEvent", "Event", {{"state", 3}}},
    {"StorageEvent", "Event", {{"key", 3}, {"oldValue", 3}, {"newValue", 3}, {"url", 0}, {"storageArea", 3}}},
    {"TransitionEvent", "Event", {{"propertyName", 0}, {"elapsedTime", 1}, {"pseudoElement", 0}}},
    {"AnimationEvent", "Event", {{"animationName", 0}, {"elapsedTime", 1}, {"pseudoElement", 0}}},
};

const EventClass* find_class(const std::string& name) {
  for (const EventClass& c : kClasses) if (name == c.name) return &c;
  return nullptr;
}

void collect_fields(const EventClass* c, std::vector<Field>* out) {
  if (c->parent) collect_fields(find_class(c->parent), out);
  for (const Field& f : c->fields) out->push_back(f);
}

Value field_default(Page& P, const Field& f) {
  switch (f.kind) {
    case 0: return Value::String(P.ctx.atoms.empty);
    case 1: return Value::Number(0);
    case 2: return Value::Bool(false);
    case 3: return Value::Null();
    default: return Value::Undefined();
  }
}

Object* class_proto(Page& P, const std::string& name) {
  auto it = P.event_protos.find(name);
  return it == P.event_protos.end() ? P.event_proto : it->second;
}

EventObject* new_event_object(Page& P, const std::string& type, const std::string& cls, bool bubbles, bool cancelable) {
  EventObject* e = P.ctx.heap.make<EventObject>();
  e->proto = class_proto(P, cls);
  e->bubbles = bubbles;
  e->cancelable = cancelable;
  Context& c = P.ctx;
  c.define_data(e, c.atom_ascii("type"), Value::String(c.new_string_utf8(type)), js::kEnumerable | js::kConfigurable);
  c.define_data(e, c.atom_ascii("timeStamp"), Value::Number(P.now_ms), js::kEnumerable | js::kConfigurable);
  if (const EventClass* ec = find_class(cls)) {
    std::vector<Field> fields;
    collect_fields(ec, &fields);
    for (const Field& f : fields) c.define_data(e, c.atom_ascii(f.name), field_default(P, f), js::kEnumerable | js::kConfigurable | js::kWritable);
  }
  return e;
}

// Event constructors take (type, init): bubbles / cancelable / composed, then the class's own fields.
Value construct_event(Page& P, CallArgs& a, const std::string& cls) {
  if (!a.is_construct()) {
    P.ctx.throw_type_error("Failed to construct '" + cls + "': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    return Value::Exception();
  }
  if (a.argc < 1) {
    P.ctx.throw_type_error("Failed to construct '" + cls + "': 1 argument required, but only 0 present.");
    return Value::Exception();
  }
  const std::string type = P.text(a[0]);
  Object* proto;
  if (!P.ctx.prototype_from_ctor(a.new_target, class_proto(P, cls), &proto)) return Value::Exception();
  EventObject* e = new_event_object(P, type, cls, false, false);
  e->proto = proto;
  if (a[1].is_object()) {
    Object* init = a[1].as_object();
    auto flag = [&](const char* n, bool* out) {
      Value v;
      if (!init->get(P.ctx, P.ctx.atom_ascii(n), a[1], &v)) return false;
      *out = Context::to_boolean(v);
      return true;
    };
    if (!flag("bubbles", &e->bubbles) || !flag("cancelable", &e->cancelable) || !flag("composed", &e->composed)) return Value::Exception();
    if (const EventClass* ec = find_class(cls)) {
      std::vector<Field> fields;
      collect_fields(ec, &fields);
      for (const Field& f : fields) {
        Value v;
        if (!init->get(P.ctx, P.ctx.atom_ascii(f.name), a[1], &v)) return Value::Exception();
        if (!v.is_undefined()) P.ctx.define_data(e, P.ctx.atom_ascii(f.name), v, js::kEnumerable | js::kConfigurable | js::kWritable);
      }
    }
  }
  return Value::Obj(e);
}

// ---- listeners ----

const void* key_of(const Value& v) {
  if (!v.is_object()) return nullptr;
  Object* o = v.as_object();
  if (o->cls == js::Class::Host && static_cast<HostObject*>(o)->host_kind == HostKind::Node) {
    NodeObject* n = static_cast<NodeObject*>(o);
    return n->dom ? static_cast<const void*>(n->dom) : nullptr;
  }
  return o;
}

bool parse_options(Page& P, const Value& opt, bool* capture, bool* once, bool* passive, Value* signal) {
  *capture = *once = *passive = false;
  if (opt.is_bool()) { *capture = opt.boolean; return true; }
  if (!opt.is_object()) return true;
  Value v;
  Object* o = opt.as_object();
  if (!o->get(P.ctx, P.ctx.atom_ascii("capture"), opt, &v)) return false;
  *capture = Context::to_boolean(v);
  if (!o->get(P.ctx, P.ctx.atom_ascii("once"), opt, &v)) return false;
  *once = Context::to_boolean(v);
  if (!o->get(P.ctx, P.ctx.atom_ascii("passive"), opt, &v)) return false;
  *passive = Context::to_boolean(v);
  if (!o->get(P.ctx, P.ctx.atom_ascii("signal"), opt, signal)) return false;
  return true;
}

void remove_listener_id(Page& P, int id) {
  for (size_t i = 0; i < P.listeners.size(); ++i)
    if (P.listeners[i].id == id) { P.listeners.erase(P.listeners.begin() + static_cast<long>(i)); return; }
}

Value add_event_listener(Page& P, CallArgs& a) {
  const void* key = key_of(a.this_value);
  if (!key) {
    if (a.this_value.is_undefined() || a.this_value.is_null()) key = P.window_obj;   // a bare addEventListener(): the global
    else return Value::Undefined();
  }
  if (a.argc < 2) {
    P.ctx.throw_type_error("Failed to execute 'addEventListener' on 'EventTarget': 2 arguments required, but only " + std::to_string(a.argc) + " present.");
    return Value::Exception();
  }
  const Value fn = a[1];
  if (fn.is_nullish()) return Value::Undefined();
  if (!fn.is_object()) {
    P.ctx.throw_type_error("Failed to execute 'addEventListener' on 'EventTarget': The callback provided as parameter 2 is not an object.");
    return Value::Exception();
  }
  Listener l;
  l.target = key;
  l.type = P.text(a[0]);
  l.fn = fn;
  if (!parse_options(P, a[2], &l.capture, &l.once, &l.passive, &l.signal)) return Value::Exception();
  for (const Listener& e : P.listeners)
    if (e.target == key && e.type == l.type && e.capture == l.capture && !e.is_property && e.fn.is_object() && e.fn.as_object() == fn.as_object()) return Value::Undefined();   // already registered
  if (l.signal.is_object()) {
    Value aborted;
    P.ctx.get(l.signal, P.ctx.atom_ascii("aborted"), &aborted);
    if (Context::to_boolean(aborted)) return Value::Undefined();
  }
  l.id = P.next_listener_id++;
  P.listeners.push_back(l);
  P.listener_count++;
  if (l.signal.is_object()) {
    // an abort removes the listener
    const int id = l.id;
    Page* pp = &P;
    Function* remover = P.ctx.new_host_function("", [pp, id](Context&, CallArgs&) -> Value {
      remove_listener_id(*pp, id);
      return Value::Undefined();
    }, 0);
    Value add, ignored;
    Object* opts = P.ctx.new_object();
    P.ctx.define_data(opts, P.ctx.atom_ascii("once"), Value::Bool(true));
    if (P.ctx.get(l.signal, P.ctx.atom_ascii("addEventListener"), &add) && add.is_callable())
      P.ctx.call(add, l.signal, {Value::String(P.ctx.atom_ascii("abort")), Value::Obj(remover), Value::Obj(opts)}, &ignored);
    P.ctx.clear_exception();
  }
  return Value::Undefined();
}

Value remove_event_listener(Page& P, CallArgs& a) {
  const void* key = key_of(a.this_value);
  if (!key) key = P.window_obj;
  if (a.argc < 2 || !a[1].is_object()) return Value::Undefined();
  bool capture, once, passive;
  Value signal;
  if (!parse_options(P, a[2], &capture, &once, &passive, &signal)) return Value::Exception();
  const std::string type = P.text(a[0]);
  for (size_t i = 0; i < P.listeners.size(); ++i) {
    Listener& e = P.listeners[i];
    if (e.target == key && e.type == type && e.capture == capture && !e.is_property && e.fn.is_object() && e.fn.as_object() == a[1].as_object()) {
      P.listeners.erase(P.listeners.begin() + static_cast<long>(i));
      break;
    }
  }
  return Value::Undefined();
}

// ---- dispatch ----

struct PathEntry {
  const void* key;
  Object* obj;
  Node* node;
};

// Run one target's listeners for the phase. Returns how many ran.
int invoke_listeners(Page& P, EventObject* ev, const PathEntry& at, const std::string& type, int phase) {
  // phase: 1 capture, 2 at target (capture then bubble listeners), 3 bubble
  std::vector<Listener> snapshot;
  for (const Listener& l : P.listeners) {
    if (l.target != at.key || l.type != type) continue;
    if (phase == 1 && !l.capture) continue;
    if (phase == 3 && l.capture) continue;
    snapshot.push_back(l);
  }
  if (phase == 2) {
    std::stable_sort(snapshot.begin(), snapshot.end(), [](const Listener& x, const Listener& y) { return x.capture && !y.capture; });
  }
  int ran = 0;
  Pin pin(P);
  for (const Listener& l : snapshot) pin.add(l.fn);
  ev->current = at.obj;
  for (const Listener& l : snapshot) {
    bool still = false;
    for (const Listener& e : P.listeners) if (e.id == l.id) { still = true; break; }
    if (!still) continue;
    if (l.once) remove_listener_id(P, l.id);
    ev->in_passive = l.passive;
    Value self = at.obj ? Value::Obj(at.obj) : Value::Undefined();
    Value result;
    bool ok;
    if (l.fn.is_callable()) {
      ok = P.call(l.fn, self, {Value::Obj(ev)}, &result);
    } else {
      Value handle;
      ok = P.ctx.get(l.fn, P.ctx.atom_ascii("handleEvent"), &handle);
      if (ok && handle.is_callable()) ok = P.call(handle, l.fn, {Value::Obj(ev)}, &result);
    }
    ev->in_passive = false;
    ++ran;
    if (!ok) {
      P.note_fault("event handler");
    } else if (l.is_property && result.is_bool() && !result.boolean && ev->cancelable && !l.passive) {
      ev->canceled = true;
    }
    if (ev->stop_imm) break;
  }
  return ran;
}

// An inline handler: <button onclick="go()">.
int invoke_inline(Page& P, EventObject* ev, const PathEntry& at, const std::string& type) {
  if (!at.node || at.node->kind != Node::Kind::Element) return 0;
  const std::string name = "on" + type;
  if (!at.node->has_attr(name)) return 0;
  Value fn;
  if (!P.ctx.make_function("event", at.node->attr(name), &fn)) {
    P.note_fault("inline handler");
    return 0;
  }
  Pin pin(P);
  pin.add(fn);
  Value result;
  ev->current = at.obj;
  if (!P.call(fn, at.obj ? Value::Obj(at.obj) : Value::Undefined(), {Value::Obj(ev)}, &result)) {
    P.note_fault("inline handler");
  } else if (result.is_bool() && !result.boolean && ev->cancelable) {
    ev->canceled = true;
  }
  return 1;
}

}  // namespace

bool event_default_prevented(Object* event) {
  EventObject* e = as_event(Value::Obj(event));
  return e && e->canceled;
}

Object* make_event(Page& P, const std::string& type, const char* ctor_name, bool bubbles, bool cancelable) {
  return new_event_object(P, type, ctor_name, bubbles, cancelable);
}

int dispatch_event_object(Page& P, const void* target_key, Object* target_obj, Object* event) {
  EventObject* ev = as_event(Value::Obj(event));
  if (!ev) return 0;
  js::JsString* ts = nullptr;
  {
    Value tv;
    P.ctx.get(Value::Obj(event), P.ctx.atom_ascii("type"), &tv);
    if (!P.ctx.to_string(tv, &ts)) { P.ctx.clear_exception(); return 0; }
  }
  const std::string type = ts->utf8();
  std::vector<PathEntry> path;
  if (target_obj && target_obj->cls == js::Class::Host && static_cast<HostObject*>(target_obj)->host_kind == HostKind::Node) {
    Node* n = static_cast<Node*>(const_cast<void*>(target_key));
    for (; n; n = n->parent) path.push_back({n, P.wrap(n), n});
    if (P.document_obj) path.push_back({P.document_obj, P.document_obj, nullptr});
    path.push_back({P.window_obj, P.window_obj, nullptr});
  } else if (target_obj == P.document_obj) {
    path.push_back({P.document_obj, P.document_obj, nullptr});
    path.push_back({P.window_obj, P.window_obj, nullptr});
  } else {
    path.push_back({target_key, target_obj, nullptr});
  }
  Pin pin(P);
  pin.add(Value::Obj(event));
  for (const PathEntry& e : path) if (e.obj) pin.add(Value::Obj(e.obj));
  ev->dispatching = true;
  ev->stop_prop = ev->stop_imm = false;
  ev->target = target_obj;
  int ran = 0;
  ev->phase = 1;
  for (size_t i = path.size(); i-- > 1 && !ev->stop_prop;) ran += invoke_listeners(P, ev, path[i], type, 1);
  if (!ev->stop_prop) {
    ev->phase = 2;
    ran += invoke_inline(P, ev, path[0], type);
    if (!ev->stop_imm) ran += invoke_listeners(P, ev, path[0], type, 2);
  }
  if (ev->bubbles) {
    ev->phase = 3;
    for (size_t i = 1; i < path.size() && !ev->stop_prop; ++i) {
      ran += invoke_inline(P, ev, path[i], type);
      if (ev->stop_imm || ev->stop_prop) break;
      ran += invoke_listeners(P, ev, path[i], type, 3);
    }
  }
  ev->phase = 0;
  ev->current = nullptr;
  ev->dispatching = false;
  return ran;
}

// ---------------------------------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------------------------------

namespace {

const char* const kHandlerNames[] = {
    "click", "dblclick", "mousedown", "mouseup", "mousemove", "mouseover", "mouseout", "mouseenter", "mouseleave", "contextmenu", "wheel", "keydown", "keyup", "keypress",
    "input", "change", "submit", "reset", "focus", "blur", "focusin", "focusout", "load", "error", "abort", "scroll", "resize", "select", "toggle", "touchstart",
    "touchend", "touchmove", "touchcancel", "pointerdown", "pointerup", "pointermove", "pointerover", "pointerout", "pointerenter", "pointerleave", "pointercancel",
    "dragstart", "drag", "dragend", "dragenter", "dragover", "dragleave", "drop", "animationstart", "animationend", "animationiteration", "transitionend", "popstate",
    "hashchange", "message", "beforeunload", "unload", "visibilitychange", "readystatechange", "online", "offline", "storage", "copy", "cut", "paste", "invalid",
    "canplay", "ended", "play", "pause", "playing", "timeupdate", "loadeddata", "loadedmetadata", "progress", "timeout", "loadstart", "loadend"};

}  // namespace

void install_events(Page& P) {
  Context& ctx = P.ctx;
  Page* pp = &P;
  // EventTarget
  def_method(P, P.event_target_proto, "addEventListener", 2, [pp](Context&, CallArgs& a) -> Value { return add_event_listener(*pp, a); });
  def_method(P, P.event_target_proto, "removeEventListener", 2, [pp](Context&, CallArgs& a) -> Value { return remove_event_listener(*pp, a); });
  def_method(P, P.event_target_proto, "dispatchEvent", 1, [pp](Context&, CallArgs& a) -> Value {
    EventObject* ev = as_event(a[0]);
    if (!ev) {
      pp->ctx.throw_type_error("Failed to execute 'dispatchEvent' on 'EventTarget': parameter 1 is not of type 'Event'.");
      return Value::Exception();
    }
    if (ev->dispatching) {
      Object* e = pp->ctx.new_error(js::ErrorKind::Error, "The event is already being dispatched.");
      pp->ctx.define_data(e, pp->ctx.atoms.name, Value::String(pp->ctx.atom_ascii("InvalidStateError")), js::kHiddenAttrs);
      return pp->ctx.throw_value(Value::Obj(e));
    }
    const void* key = key_of(a.this_value);
    Object* target = a.this_value.is_object() ? a.this_value.as_object() : pp->window_obj;
    if (!key) key = target;
    dispatch_event_object(*pp, key, target, ev);
    pp->run_jobs();
    return Value::Bool(!ev->canceled);
  });
  Function* et = def_ctor(P, "EventTarget", 0, P.event_target_proto, [pp](Context&, CallArgs& a) -> Value {
    if (!a.is_construct()) {
      pp->ctx.throw_type_error("Failed to construct 'EventTarget': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
      return Value::Exception();
    }
    Object* proto;
    if (!pp->ctx.prototype_from_ctor(a.new_target, pp->event_target_proto, &proto)) return Value::Exception();
    TargetObject* t = pp->ctx.heap.make<TargetObject>();
    t->proto = proto;
    return Value::Obj(t);
  });
  (void)et;
  // on<event> properties, as listeners registered at first assignment
  for (const char* n : kHandlerNames) {
    const std::string type = n;
    def_accessor(P, P.event_target_proto, (std::string("on") + n).c_str(), [pp, type](Context&, CallArgs& a) -> Value {
      const void* key = key_of(a.this_value);
      if (!key) key = pp->window_obj;
      for (const Listener& l : pp->listeners)
        if (l.is_property && l.target == key && l.type == type) return l.fn;
      return Value::Null();
    }, [pp, type](Context&, CallArgs& a) -> Value {
      const void* key = key_of(a.this_value);
      if (!key) key = pp->window_obj;
      for (size_t i = 0; i < pp->listeners.size(); ++i) {
        Listener& l = pp->listeners[i];
        if (!(l.is_property && l.target == key && l.type == type)) continue;
        if (a[0].is_callable()) l.fn = a[0];
        else pp->listeners.erase(pp->listeners.begin() + static_cast<long>(i));
        return Value::Undefined();
      }
      if (a[0].is_callable()) {
        Listener l;
        l.target = key;
        l.type = type;
        l.fn = a[0];
        l.is_property = true;
        l.id = pp->next_listener_id++;
        pp->listeners.push_back(l);
        pp->listener_count++;
      }
      return Value::Undefined();
    });
  }

  // Event and its subclasses
  P.event_proto = make_proto(P, ctx.intr.object_proto, "Event");
  P.event_protos["Event"] = P.event_proto;
  def_method(P, P.event_proto, "preventDefault", 0, [pp](Context&, CallArgs& a) -> Value {
    EventObject* e = this_event(*pp, a);
    if (!e) return Value::Exception();
    if (e->cancelable && !e->in_passive) e->canceled = true;
    return Value::Undefined();
  });
  def_method(P, P.event_proto, "stopPropagation", 0, [pp](Context&, CallArgs& a) -> Value {
    EventObject* e = this_event(*pp, a);
    if (!e) return Value::Exception();
    e->stop_prop = true;
    return Value::Undefined();
  });
  def_method(P, P.event_proto, "stopImmediatePropagation", 0, [pp](Context&, CallArgs& a) -> Value {
    EventObject* e = this_event(*pp, a);
    if (!e) return Value::Exception();
    e->stop_prop = e->stop_imm = true;
    return Value::Undefined();
  });
  def_method(P, P.event_proto, "composedPath", 0, [pp](Context&, CallArgs& a) -> Value {
    EventObject* e = this_event(*pp, a);
    if (!e) return Value::Exception();
    std::vector<Value> out;
    if (e->dispatching && e->target) {
      if (e->target->cls == js::Class::Host && static_cast<HostObject*>(e->target)->host_kind == HostKind::Node) {
        for (Node* n = static_cast<NodeObject*>(e->target)->dom; n; n = n->parent) out.push_back(pp->node_value(n));
        out.push_back(Value::Obj(pp->document_obj));
        out.push_back(Value::Obj(pp->window_obj));
      } else {
        out.push_back(Value::Obj(e->target));
      }
    }
    return make_array(*pp, out);
  });
  def_method(P, P.event_proto, "initEvent", 3, [pp](Context&, CallArgs& a) -> Value {
    EventObject* e = this_event(*pp, a);
    if (!e) return Value::Exception();
    pp->ctx.define_data(e, pp->ctx.atom_ascii("type"), Value::String(pp->ctx.new_string_utf8(pp->text(a[0]))), js::kEnumerable | js::kConfigurable);
    e->bubbles = Context::to_boolean(a[1]);
    e->cancelable = Context::to_boolean(a[2]);
    return Value::Undefined();
  });
  auto ev_get = [pp](const char* name, std::function<Value(EventObject*, Context&)> f) {
    def_accessor(*pp, pp->event_proto, name, [pp, f](Context& c, CallArgs& a) -> Value {
      EventObject* e = this_event(*pp, a);
      if (!e) return Value::Exception();
      return f(e, c);
    });
  };
  ev_get("bubbles", [](EventObject* e, Context&) { return Value::Bool(e->bubbles); });
  ev_get("cancelable", [](EventObject* e, Context&) { return Value::Bool(e->cancelable); });
  ev_get("composed", [](EventObject* e, Context&) { return Value::Bool(e->composed); });
  ev_get("defaultPrevented", [](EventObject* e, Context&) { return Value::Bool(e->canceled); });
  ev_get("eventPhase", [](EventObject* e, Context&) { return Value::Number(e->phase); });
  ev_get("isTrusted", [](EventObject* e, Context&) { return Value::Bool(e->trusted); });
  ev_get("target", [](EventObject* e, Context&) { return e->target ? Value::Obj(e->target) : Value::Null(); });
  ev_get("srcElement", [](EventObject* e, Context&) { return e->target ? Value::Obj(e->target) : Value::Null(); });
  ev_get("currentTarget", [](EventObject* e, Context&) { return e->current ? Value::Obj(e->current) : Value::Null(); });
  ev_get("returnValue", [](EventObject* e, Context&) { return Value::Bool(!e->canceled); });
  ev_get("cancelBubble", [](EventObject* e, Context&) { return Value::Bool(e->stop_prop); });
  for (const EventClass& c : kClasses) {
    Object* proto = c.parent ? make_proto(P, class_proto(P, c.parent), c.name) : P.event_proto;
    if (c.parent) P.event_protos[c.name] = proto;
    const std::string cls = c.name;
    Function* f = def_ctor(P, c.name, 1, proto, [pp, cls](Context&, CallArgs& a) -> Value { return construct_event(*pp, a, cls); });
    if (!c.parent) {
      for (int i = 0; i < 4; ++i) {
        static const char* const names[] = {"NONE", "CAPTURING_PHASE", "AT_TARGET", "BUBBLING_PHASE"};
        ctx.define_data(f, ctx.atom_ascii(names[i]), Value::Number(i), 0);
      }
    }
  }
}

}  // namespace aios::userland::webjs
