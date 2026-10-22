// The DOM a script sees: node wrappers over the SAME tree the layout engine walks, so a script that appends a node
// appends the thing that gets drawn. Properties live on prototypes (EventTarget > Node > Element > HTMLElement, ...)
// the way a browser's do; the collections, classList, style and dataset are real objects bound to their element.
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "page.hpp"

namespace aios::userland::webjs {

using js::PropKey;
using js::Tracer;

// ---------------------------------------------------------------------------------------------------
// binding helpers
// ---------------------------------------------------------------------------------------------------

Function* def_method(Page& P, Object* target, const char* name, int length, HostFn fn) {
  Function* f = P.ctx.new_host_function(name, std::move(fn), length);
  P.ctx.define_data(target, P.ctx.atom_utf8(name), Value::Obj(f), js::kHiddenAttrs | js::kEnumerable);
  return f;
}

void def_accessor(Page& P, Object* target, const char* name, HostFn getter, HostFn setter) {
  Function* g = P.ctx.new_host_function(std::string("get ") + name, std::move(getter), 0);
  // These accessors live on HTMLElement.prototype for every tag, though a real browser puts each on its own tag's prototype
  // (content on <template>, options on <select>). A custom element's constructor that assigns `this.content = x` would
  // throw in strict mode on a getter-only accessor, so a getter-only one here shadows itself with a plain own property.
  if (!setter && target == P.html_element_proto) {
    setter = [key = std::string(name)](Context& c, CallArgs& a) -> Value {
      if (a.this_value.is_object()) c.define_data(a.this_value.as_object(), c.atom_utf8(key), a[0]);
      return Value::Undefined();
    };
  }
  Function* s = setter ? P.ctx.new_host_function(std::string("set ") + name, std::move(setter), 1) : nullptr;
  P.ctx.define_accessor(target, P.ctx.atom_utf8(name), g, s, js::kConfigurable | js::kEnumerable);
}

Object* make_proto(Page& P, Object* parent, const char* tag_name) {
  Object* p = P.ctx.new_object(parent);
  if (tag_name) P.ctx.define_data(p, P.ctx.sym.to_string_tag, js::Value::String(P.ctx.atom_utf8(tag_name)), js::kConfigurable);
  return p;
}

Function* def_ctor(Page& P, const char* name, int length, Object* proto, HostFn fn) {
  Function* f = P.ctx.new_host_function(name, std::move(fn), length, true);
  P.ctx.define_data(f, P.ctx.atoms.prototype, Value::Obj(proto), 0);
  P.ctx.define_data(proto, P.ctx.atoms.constructor, Value::Obj(f), js::kHiddenAttrs);
  P.ctx.define_data(P.ctx.global(), P.ctx.atom_utf8(name), Value::Obj(f), js::kHiddenAttrs);
  return f;
}

Value make_array(Page& P, const std::vector<Value>& v) { return Value::Obj(P.ctx.new_array_from(v)); }

std::string arg_text(Page& P, CallArgs& a, int i) {
  if (i >= a.argc) return "undefined";
  return P.text(a.argv[i]);
}

std::string Page::text(const Value& v) {
  js::JsString* s;
  if (!ctx.to_string(v, &s)) {
    ctx.clear_exception();
    return std::string();
  }
  return s->utf8();
}

double Page::number(const Value& v) {
  double d;
  if (!ctx.to_number(v, &d)) {
    ctx.clear_exception();
    return NAN;
  }
  return d;
}

void Page::note_fault(const char* where) {
  if (!ctx.has_exception()) return;
  std::string msg = ctx.describe_exception(ctx.take_exception());
  last_fault = std::string(where) + ": " + msg;
  if (dispatch_error.empty()) dispatch_error = last_fault;
  log.push_back("Uncaught " + msg);
  if (std::getenv("AIOS_JS_ERRORS")) std::fprintf(stderr, "[js-error] %s: %s\n", where, msg.c_str());
}

bool Page::call(const Value& fn, const Value& self, std::vector<Value> args, Value* out) {
  Value r;
  const bool ok = ctx.call(fn, self, args.data(), static_cast<int>(args.size()), &r);
  if (out) *out = r;
  return ok;
}

void Page::add_timer(std::vector<PendingTimer>& list, PendingTimer t, size_t cap) {
  if (list.size() < cap) list.push_back(std::move(t));
}

void NodeObject::trace(Tracer& t) {
  Object::trace(t);
  t.mark(class_list);
  t.mark(style);
  t.mark(dataset);
}

static Value throw_dom(Page& P, const char* name, const std::string& msg) {
  Object* e = P.ctx.new_error(js::ErrorKind::Error, msg);
  P.ctx.define_data(e, P.ctx.atoms.name, Value::String(P.ctx.atom_utf8(name)), js::kHiddenAttrs);
  return P.ctx.throw_value(Value::Obj(e));
}

NodeObject* this_node(Page& P, CallArgs& a) {
  if (a.this_value.is_object() && a.this_value.as_object()->cls == js::Class::Host && static_cast<HostObject*>(a.this_value.as_object())->host_kind == HostKind::Node)
    return static_cast<NodeObject*>(a.this_value.as_object());
  // `document` is a Node too: the Node members (parentNode, childNodes, firstChild, ...) answer for the tree's root.
  if (a.this_value.is_object() && a.this_value.as_object() == P.document_obj && P.doc && P.doc->dom) return P.wrap(P.doc->dom.get());
  P.ctx.throw_type_error("Illegal invocation");
  return nullptr;
}

HostObject* this_host(Page& P, CallArgs& a, HostKind kind, const char* what) {
  if (a.this_value.is_object() && a.this_value.as_object()->cls == js::Class::Host && static_cast<HostObject*>(a.this_value.as_object())->host_kind == kind)
    return static_cast<HostObject*>(a.this_value.as_object());
  P.ctx.throw_type_error(std::string("Illegal invocation: this is not a ") + what);
  return nullptr;
}

static NodeObject* node_arg(const Value& v) {
  if (v.is_object() && v.as_object()->cls == js::Class::Host && static_cast<HostObject*>(v.as_object())->host_kind == HostKind::Node)
    return static_cast<NodeObject*>(v.as_object());
  return nullptr;
}

// ---------------------------------------------------------------------------------------------------
// wrappers and ownership
// ---------------------------------------------------------------------------------------------------

NodeObject* Page::wrap(Node* n) {
  if (!n) return nullptr;
  auto it = wrappers.find(n);
  if (it != wrappers.end()) return it->second;
  NodeObject* o = ctx.heap.make<NodeObject>(n);
  Object* proto = html_element_proto;
  if (n->kind == Node::Kind::Text) proto = text_proto;
  else if (n->kind == Node::Kind::Document) proto = node_proto;
  else {
    auto tp = tag_protos.find(n->tag);
    if (tp != tag_protos.end()) proto = tp->second;
  }
  o->proto = proto;
  wrappers[n] = o;
  return o;
}

void Page::forget_subtree(Node* n) {
  if (!n) return;
  for (auto& c : n->children) forget_subtree(c.get());
  auto it = wrappers.find(n);
  if (it != wrappers.end()) {
    it->second->dom = nullptr;
    wrappers.erase(it);
  }
  if (focused == n) focused = nullptr;
  for (size_t i = listeners.size(); i > 0; --i)
    if (listeners[i - 1].target == n) listeners.erase(listeners.begin() + static_cast<long>(i - 1));
}

Node* Page::take_orphan(Node* n) {
  for (size_t i = 0; i < orphans.size(); ++i) {
    if (orphans[i].get() == n) {
      orphans[i].release();
      orphans.erase(orphans.begin() + static_cast<long>(i));
      return n;
    }
  }
  return nullptr;
}

void Page::adopt_orphan(web::NodePtr n) { orphans.push_back(std::move(n)); }

// MutationObserver support: once the prelude hands over its recorder, every tree/attribute/text change below is reported to it
// as (type, target, attributeName, oldValue, addedNodes, removedNodes, previousSibling, nextSibling). Matching observers and
// delivering at microtask time is the recorder's job; here we only say what changed.
static bool mo_on(Page& P) { return P.mo_hook.is_object() && !P.mo_busy; }

static Node* sib_of(Node* parent, size_t i, int dir) {
  if (!parent) return nullptr;
  long j = static_cast<long>(i) + dir;
  return j >= 0 && j < static_cast<long>(parent->children.size()) ? parent->children[static_cast<size_t>(j)].get() : nullptr;
}

static void mo_note(Page& P, const char* type, Node* target, const char* attr, const std::string* old, const std::vector<Node*>& added,
                    const std::vector<Node*>& removed, Node* prev, Node* next) {
  if (!mo_on(P) || !target) return;
  P.mo_busy = true;
  auto nodes = [&](const std::vector<Node*>& v) {
    std::vector<Value> out;
    for (Node* n : v) out.push_back(P.node_value(n));
    return make_array(P, out);
  };
  Value ty = Value::String(P.ctx.new_string_utf8(type));
  Value at = attr ? Value::String(P.ctx.new_string_utf8(attr)) : Value::Null();
  Value ol = old ? Value::String(P.ctx.new_string_utf8(*old)) : Value::Null();
  std::vector<Value> args{ty, P.node_value(target), at, ol, nodes(added), nodes(removed), P.node_value(prev), P.node_value(next)};
  P.mo_busy = false;
  Pin pin(P);
  for (const Value& v : args) pin.add(v);
  P.call(P.mo_hook, Value::Undefined(), args);
}

// Detach `kid` from wherever it lives and return ownership. Null if it was in neither a tree nor the orphan list.
static web::NodePtr detach(Page& P, Node* kid) {
  if (kid->parent) {
    Node* old = kid->parent;
    for (size_t i = 0; i < old->children.size(); ++i) {
      if (old->children[i].get() == kid) {
        web::NodePtr owned = std::move(old->children[i]);
        old->children.erase(old->children.begin() + static_cast<long>(i));
        kid->parent = nullptr;
        mo_note(P, "childList", old, nullptr, nullptr, {}, {kid}, sib_of(old, i, -1), i < old->children.size() ? old->children[i].get() : nullptr);
        return owned;
      }
    }
  }
  if (P.take_orphan(kid)) return web::NodePtr(kid);
  return nullptr;
}

static bool is_fragment(const Node& n) { return n.kind == Node::Kind::Element && n.tag == "#document-fragment"; }

static bool is_inclusive_ancestor(const Node* anc, const Node* n) {
  for (; n; n = n->parent)
    if (n == anc) return true;
  return false;
}

// Insert `kid` (or, for a fragment, its children) into `parent` before `before` (null: append).
static bool insert_node(Page& P, Node* parent, Node* kid, Node* before) {
  if (is_inclusive_ancestor(kid, parent)) {
    throw_dom(P, "HierarchyRequestError", "The new child element contains the parent.");
    return false;
  }
  if (before && before->parent != parent) {
    throw_dom(P, "NotFoundError", "The node before which the new node is to be inserted is not a child of this node.");
    return false;
  }
  std::vector<web::NodePtr> moving;
  if (is_fragment(*kid)) {
    for (auto& c : kid->children) { c->parent = nullptr; moving.push_back(std::move(c)); }
    kid->children.clear();
  } else {
    web::NodePtr owned = detach(P, kid);
    if (!owned) owned = web::NodePtr(kid);   // never owned (a node taken from somewhere unexpected): adopt
    moving.push_back(std::move(owned));
  }
  size_t at = parent->children.size();
  if (before) {
    for (size_t i = 0; i < parent->children.size(); ++i)
      if (parent->children[i].get() == before) { at = i; break; }
  }
  const size_t first = at;
  std::vector<Node*> added;
  for (auto& m : moving) {
    m->parent = parent;
    added.push_back(m.get());
    parent->children.insert(parent->children.begin() + static_cast<long>(at++), std::move(m));
  }
  P.write();
  if (!added.empty()) mo_note(P, "childList", parent, nullptr, nullptr, added, {}, sib_of(parent, first, -1), sib_of(parent, at - 1, 1));
  return true;
}

static void remove_node(Page& P, Node* n) {
  web::NodePtr owned = detach(P, n);
  if (owned) P.adopt_orphan(std::move(owned));
  P.write();
}

static web::NodePtr clone_node(const Node& n, bool deep) {
  auto c = std::make_unique<Node>();
  c->kind = n.kind;
  c->tag = n.tag;
  c->text = n.text;
  c->attrs = n.attrs;
  if (deep) {
    for (const auto& k : n.children) {
      web::NodePtr kc = clone_node(*k, true);
      kc->parent = c.get();
      c->children.push_back(std::move(kc));
    }
  }
  return c;
}

static void set_attr(Page& P, Node* n, const std::string& name, const std::string& value) {
  for (auto& at : n->attrs)
    if (at.name == name) { const std::string old = at.value; at.value = value; P.write(); mo_note(P, "attributes", n, name.c_str(), &old, {}, {}, nullptr, nullptr); return; }
  web::Attr a;
  a.name = name;
  a.value = value;
  n->attrs.push_back(a);
  P.write();
  mo_note(P, "attributes", n, name.c_str(), nullptr, {}, {}, nullptr, nullptr);
}

static void remove_attr(Page& P, Node* n, const std::string& name) {
  for (size_t i = 0; i < n->attrs.size(); ++i)
    if (n->attrs[i].name == name) { const std::string old = n->attrs[i].value; n->attrs.erase(n->attrs.begin() + static_cast<long>(i)); P.write(); mo_note(P, "attributes", n, name.c_str(), &old, {}, {}, nullptr, nullptr); return; }
}

static std::string lower(std::string s) {
  for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  return s;
}

static std::string upper(std::string s) {
  for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
  return s;
}

static void collect_text(const Node& n, std::string& out) {
  if (n.kind == Node::Kind::Text) { out += n.text; return; }
  for (const auto& c : n.children) collect_text(*c, out);
}

// Empty `n`. Children a MutationObserver may be told about stay alive as orphans instead of being destroyed.
static void clear_children(Page& P, Node* n) {
  std::vector<Node*> gone;
  const bool keep = mo_on(P);
  for (auto& c : n->children) {
    if (keep) { gone.push_back(c.get()); c->parent = nullptr; P.adopt_orphan(std::move(c)); }
    else P.forget_subtree(c.get());
  }
  n->children.clear();
  if (!gone.empty()) mo_note(P, "childList", n, nullptr, nullptr, {}, gone, nullptr, nullptr);
}

static void set_text_content(Page& P, Node* n, const std::string& t) {
  if (!n) return;
  if (n->kind == Node::Kind::Text) { const std::string old = n->text; n->text = t; P.write(); mo_note(P, "characterData", n, nullptr, &old, {}, {}, nullptr, nullptr); return; }
  clear_children(P, n);
  if (!t.empty()) {
    auto tn = std::make_unique<Node>();
    tn->kind = Node::Kind::Text;
    tn->text = t;
    tn->parent = n;
    Node* rawt = tn.get();
    n->children.push_back(std::move(tn));
    mo_note(P, "childList", n, nullptr, nullptr, {rawt}, {}, nullptr, nullptr);
  }
  P.write();
}

static Node* document_root(Page& P) { return P.doc && P.doc->dom ? P.doc->dom.get() : nullptr; }

Node* find_by_id(Node* n, const std::string& id) {
  if (!n) return nullptr;
  if (n->kind == Node::Kind::Element && n->attr("id") == id) return n;
  for (auto& c : n->children)
    if (Node* f = find_by_id(c.get(), id)) return f;
  return nullptr;
}

namespace {
Value node_list(Page& P, const std::vector<Node*>& nodes);
}

static void query_walk(Node* n, const web::CompiledSelectors& sel, std::vector<Node*>& out, bool include_self, size_t limit) {
  if (!n || (limit && out.size() >= limit)) return;
  if (include_self && n->kind == Node::Kind::Element && web::compiled_selectors_match(*n, sel)) out.push_back(n);
  for (auto& c : n->children) query_walk(c.get(), sel, out, true, limit);
}

void query_all(Node* n, const std::string& sel, std::vector<Node*>& out, bool include_self) {
  if (sel.empty()) return;
  const web::CompiledSelectors compiled = web::compile_selectors(sel);
  query_walk(n, compiled, out, include_self, 0);
}

// The selector as script gave it, or a SyntaxError DOMException and false.
static bool compile_or_throw(Page& P, const std::string& sel, web::CompiledSelectors* out) {
  *out = web::compile_selectors(sel);
  if (out->valid) return true;
  throw_dom(P, "SyntaxError", "'" + sel + "' is not a valid selector.");
  return false;
}

// querySelector / querySelectorAll on a tree, with the error and the early exit a browser has.
static Value select_in(Page& P, Node* root, const std::string& sel, bool include_self, bool all) {
  web::CompiledSelectors compiled;
  if (!compile_or_throw(P, sel, &compiled)) return Value::Exception();
  std::vector<Node*> hits;
  query_walk(root, compiled, hits, include_self, all ? 0 : 1);
  if (all) return node_list(P, hits);
  return hits.empty() ? Value::Null() : P.node_value(hits[0]);
}

static void by_tag(Node* n, const std::string& tag, std::vector<Node*>& out) {
  if (!n) return;
  if (n->kind == Node::Kind::Element && (tag == "*" || n->tag == tag)) out.push_back(n);
  for (auto& c : n->children) by_tag(c.get(), tag, out);
}

static bool has_class_word(const Node& n, const std::string& want) {
  const std::string cls = n.attr("class");
  size_t at = 0;
  while (at < cls.size()) {
    while (at < cls.size() && std::isspace(static_cast<unsigned char>(cls[at]))) ++at;
    const size_t b = at;
    while (at < cls.size() && !std::isspace(static_cast<unsigned char>(cls[at]))) ++at;
    if (at > b && cls.compare(b, at - b, want) == 0) return true;
  }
  return false;
}

static void by_class(Node* n, const std::vector<std::string>& words, std::vector<Node*>& out) {
  if (!n) return;
  if (n->kind == Node::Kind::Element) {
    bool all = !words.empty();
    for (const auto& w : words) if (!has_class_word(*n, w)) { all = false; break; }
    if (all) out.push_back(n);
  }
  for (auto& c : n->children) by_class(c.get(), words, out);
}

static std::vector<std::string> class_words(const Node& n) {
  std::vector<std::string> out;
  const std::string cls = n.attr("class");
  size_t at = 0;
  while (at < cls.size()) {
    while (at < cls.size() && std::isspace(static_cast<unsigned char>(cls[at]))) ++at;
    const size_t b = at;
    while (at < cls.size() && !std::isspace(static_cast<unsigned char>(cls[at]))) ++at;
    if (at > b) out.push_back(cls.substr(b, at - b));
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------
// markup out and in
// ---------------------------------------------------------------------------------------------------

static bool is_void_tag(const std::string& t) {
  static const char* const v[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"};
  for (const char* s : v) if (t == s) return true;
  return false;
}

static void escape_into(std::string& out, const std::string& s, bool attr) {
  for (char c : s) {
    if (c == '&') out += "&amp;";
    else if (c == '<' && !attr) out += "&lt;";
    else if (c == '>' && !attr) out += "&gt;";
    else if (c == '"' && attr) out += "&quot;";
    else out += c;
  }
}

static void serialize_into(std::string& out, const Node& n, bool outer) {
  if (n.kind == Node::Kind::Text) {
    const Node* p = n.parent;
    if (p && (p->tag == "script" || p->tag == "style")) out += n.text;
    else escape_into(out, n.text, false);
    return;
  }
  if (n.kind == Node::Kind::Element && outer && !is_fragment(n)) {
    out += "<" + n.tag;
    for (const auto& a : n.attrs) {
      out += " " + a.name + "=\"";
      escape_into(out, a.value, true);
      out += "\"";
    }
    out += ">";
    if (is_void_tag(n.tag)) return;
  }
  for (const auto& c : n.children) serialize_into(out, *c, true);
  if (n.kind == Node::Kind::Element && outer && !is_fragment(n)) out += "</" + n.tag + ">";
}

std::string serialize_html(const Node& n, bool outer) {
  std::string out;
  serialize_into(out, n, outer);
  return out;
}

// Scripts parsed out of a string never run (innerHTML, insertAdjacentHTML, a template's content): only the page's own
// markup and elements made with createElement do.
static void mark_inert(Page& P, Node* n) {
  if (n->is("script")) P.inert_scripts.insert(n);
  for (auto& c : n->children) mark_inert(P, c.get());
}

// Parse a fragment with the page's own parser and move its top-level nodes under `into`.
static void parse_fragment_into(Page& P, Node* into, const std::string& html, Node* before = nullptr) {
  web::NodePtr frag = web::parse_html(aios::ByteSpan(reinterpret_cast<const uint8_t*>(html.data()), html.size()));
  if (!frag) return;
  Node* src = frag->first("body");
  if (!src) src = frag.get();
  size_t at = into->children.size();
  if (before)
    for (size_t i = 0; i < into->children.size(); ++i)
      if (into->children[i].get() == before) { at = i; break; }
  const size_t first = at;
  std::vector<Node*> added;
  for (auto& c : src->children) {
    Node* raw = c.release();
    raw->parent = into;
    mark_inert(P, raw);
    added.push_back(raw);
    into->children.insert(into->children.begin() + static_cast<long>(at++), web::NodePtr(raw));
  }
  src->children.clear();
  P.write();
  if (!added.empty()) mo_note(P, "childList", into, nullptr, nullptr, added, {}, sib_of(into, first, -1), sib_of(into, at - 1, 1));
}

// ---------------------------------------------------------------------------------------------------
// style declarations
// ---------------------------------------------------------------------------------------------------

struct Decl {
  std::string name, value;
  bool important = false;
};

static std::vector<Decl> parse_style(const std::string& text) {
  std::vector<Decl> out;
  size_t at = 0;
  const size_t n = text.size();
  while (at < n) {
    // a declaration ends at ';' outside parentheses and quotes
    size_t i = at;
    int depth = 0;
    char quote = 0;
    for (; i < n; ++i) {
      char c = text[i];
      if (quote) { if (c == '\\') ++i; else if (c == quote) quote = 0; continue; }
      if (c == '"' || c == '\'') quote = c;
      else if (c == '(') ++depth;
      else if (c == ')') { if (depth) --depth; }
      else if (c == ';' && depth == 0) break;
    }
    const std::string one = text.substr(at, i - at);
    at = i + 1;
    const size_t colon = one.find(':');
    if (colon == std::string::npos) continue;
    Decl d;
    for (char c : one.substr(0, colon)) if (!std::isspace(static_cast<unsigned char>(c))) d.name += d.name.compare(0, 2, "--") == 0 || (d.name.empty() && c == '-') ? c : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (d.name.empty()) continue;
    std::string val = one.substr(colon + 1);
    size_t b = 0, e = val.size();
    while (b < e && std::isspace(static_cast<unsigned char>(val[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(val[e - 1]))) --e;
    val = val.substr(b, e - b);
    const size_t bang = val.rfind('!');
    if (bang != std::string::npos) {
      std::string tail = lower(val.substr(bang + 1));
      tail.erase(std::remove_if(tail.begin(), tail.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); }), tail.end());
      if (tail == "important") {
        d.important = true;
        val = val.substr(0, bang);
        while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back()))) val.pop_back();
      }
    }
    d.value = val;
    out.push_back(d);
  }
  return out;
}

static std::string serialize_style(const std::vector<Decl>& decls) {
  std::string out;
  for (const Decl& d : decls) {
    if (!out.empty()) out += " ";
    out += d.name + ": " + d.value + (d.important ? " !important" : "") + ";";
  }
  return out;
}

// camelCase / vendor-prefixed script name -> CSS property name.
static std::string css_name(const std::string& js) {
  if (js == "cssFloat" || js == "float") return "float";
  if (js.compare(0, 2, "--") == 0) return js;
  std::string out;
  for (size_t i = 0; i < js.size(); ++i) {
    char c = js[i];
    if (c >= 'A' && c <= 'Z') {
      out += '-';
      out += static_cast<char>(c + 32);
    } else {
      out += c;
    }
  }
  if (out.compare(0, 7, "webkit-") == 0 || out.compare(0, 4, "moz-") == 0 || out.compare(0, 3, "ms-") == 0) out = "-" + out;
  return out;
}

static std::string js_css_name(const std::string& css) {
  std::string out;
  bool up = false;
  for (size_t i = 0; i < css.size(); ++i) {
    if (css[i] == '-' && i > 0) { up = true; continue; }
    out += up && css[i] >= 'a' && css[i] <= 'z' ? static_cast<char>(css[i] - 32) : css[i];
    up = false;
  }
  return out;
}

struct StyleObject final : HostObject {
  StyleObject() : HostObject(HostKind::Style) { exotic_props = true; exotic_get = true; }
  NodeObject* owner = nullptr;
  Page* page = nullptr;
  void trace(Tracer& t) override { Object::trace(t); t.mark(owner); }

  std::vector<Decl> decls() const { return owner && owner->dom ? parse_style(owner->dom->attr("style")) : std::vector<Decl>(); }
  void store(const std::vector<Decl>& d) {
    if (!owner || !owner->dom) return;
    if (d.empty()) remove_attr(*page, owner->dom, "style");
    else set_attr(*page, owner->dom, "style", serialize_style(d));
  }
  static bool is_own_name(Context& ctx, PropKey k, Object* self) {
    // methods and fixed properties live on the prototype (and `constructor`, `toString`, ...): answer those ordinarily
    bool has = false;
    self->Object::has_property(ctx, k, &has);
    return has;
  }
  std::string get_value(const std::string& name) const {
    std::string found;
    for (const Decl& d : decls()) if (d.name == name) found = d.value;
    return found;
  }
  void set_value(const std::string& name, const std::string& value, bool important = false) {
    std::vector<Decl> d = decls();
    for (size_t i = 0; i < d.size(); ++i)
      if (d[i].name == name) { d.erase(d.begin() + static_cast<long>(i)); --i; }
    if (!value.empty()) d.push_back({name, value, important});
    store(d);
  }

  bool get(Context& ctx, PropKey k, const Value& receiver, Value* out) override {
    if (k->kind != js::CellKind::String || is_own_name(ctx, k, this)) return Object::get(ctx, k, receiver, out);
    const std::string name = static_cast<js::JsString*>(k)->utf8();
    if (!name.empty() && std::isdigit(static_cast<unsigned char>(name[0]))) {
      const long i = std::strtol(name.c_str(), nullptr, 10);
      auto d = decls();
      *out = i >= 0 && static_cast<size_t>(i) < d.size() ? Value::String(ctx.atom_utf8(d[static_cast<size_t>(i)].name)) : Value::Undefined();
      return true;
    }
    *out = Value::String(ctx.new_string_utf8(get_value(css_name(name))));
    return true;
  }
  bool set(Context& ctx, PropKey k, const Value& v, const Value& receiver, bool* ok) override {
    if (k->kind != js::CellKind::String || is_own_name(ctx, k, this)) return Object::set(ctx, k, v, receiver, ok);
    const std::string name = static_cast<js::JsString*>(k)->utf8();
    std::string val = v.is_nullish() ? std::string() : page->text(v);
    set_value(css_name(name), val);
    *ok = true;
    return true;
  }
  bool has_property(Context& ctx, PropKey k, bool* out) override {
    if (k->kind == js::CellKind::String && !is_own_name(ctx, k, this)) { *out = true; return true; }
    return Object::has_property(ctx, k, out);
  }
  bool delete_property(Context& ctx, PropKey k, bool* ok) override {
    if (k->kind == js::CellKind::String && !is_own_name(ctx, k, this)) {
      set_value(css_name(static_cast<js::JsString*>(k)->utf8()), "");
      *ok = true;
      return true;
    }
    return Object::delete_property(ctx, k, ok);
  }
  bool own_keys(Context& ctx, std::vector<PropKey>* out) override {
    for (const Decl& d : decls()) out->push_back(ctx.atom_utf8(js_css_name(d.name)));
    return Object::own_keys(ctx, out);
  }
};

static StyleObject* this_style(Page& P, CallArgs& a) { return static_cast<StyleObject*>(this_host(P, a, HostKind::Style, "CSSStyleDeclaration")); }

// ---------------------------------------------------------------------------------------------------
// classList and dataset
// ---------------------------------------------------------------------------------------------------

struct ClassListObject final : HostObject {
  ClassListObject() : HostObject(HostKind::ClassList) {}
  NodeObject* owner = nullptr;
  void trace(Tracer& t) override { Object::trace(t); t.mark(owner); }
};

struct DatasetObject final : HostObject {
  DatasetObject() : HostObject(HostKind::Dataset) { exotic_props = true; exotic_get = true; }
  NodeObject* owner = nullptr;
  Page* page = nullptr;
  void trace(Tracer& t) override { Object::trace(t); t.mark(owner); }

  static std::string attr_name(const std::string& key) {
    std::string a = "data-";
    for (char c : key) {
      if (c >= 'A' && c <= 'Z') { a += '-'; a += static_cast<char>(c + 32); }
      else a += c;
    }
    return a;
  }
  static std::string key_name(const std::string& attr) {
    std::string key;
    bool up = false;
    for (size_t i = 5; i < attr.size(); ++i) {
      if (attr[i] == '-') { up = true; continue; }
      key += up && attr[i] >= 'a' && attr[i] <= 'z' ? static_cast<char>(attr[i] - 32) : attr[i];
      up = false;
    }
    return key;
  }
  bool get(Context& ctx, PropKey k, const Value& receiver, Value* out) override {
    if (k->kind != js::CellKind::String) return Object::get(ctx, k, receiver, out);
    if (!owner || !owner->dom) { *out = Value::Undefined(); return true; }
    const std::string an = attr_name(static_cast<js::JsString*>(k)->utf8());
    if (!owner->dom->has_attr(an)) return Object::get(ctx, k, receiver, out);
    *out = Value::String(ctx.new_string_utf8(owner->dom->attr(an)));
    return true;
  }
  bool set(Context& ctx, PropKey k, const Value& v, const Value& receiver, bool* ok) override {
    if (k->kind != js::CellKind::String) return Object::set(ctx, k, v, receiver, ok);
    if (owner && owner->dom) set_attr(*page, owner->dom, attr_name(static_cast<js::JsString*>(k)->utf8()), page->text(v));
    *ok = true;
    return true;
  }
  bool has_property(Context& ctx, PropKey k, bool* out) override {
    if (k->kind == js::CellKind::String && owner && owner->dom && owner->dom->has_attr(attr_name(static_cast<js::JsString*>(k)->utf8()))) { *out = true; return true; }
    return Object::has_property(ctx, k, out);
  }
  bool get_own_property(Context& ctx, PropKey k, js::PropDesc* out, bool* found) override {
    if (k->kind == js::CellKind::String && owner && owner->dom) {
      const std::string an = attr_name(static_cast<js::JsString*>(k)->utf8());
      if (owner->dom->has_attr(an)) {
        *out = js::PropDesc::Data(Value::String(ctx.new_string_utf8(owner->dom->attr(an))), js::kDefaultAttrs);
        *found = true;
        return true;
      }
    }
    return Object::get_own_property(ctx, k, out, found);
  }
  bool define_own_property(Context& ctx, PropKey k, const js::PropDesc& d, bool* ok) override {
    if (k->kind == js::CellKind::String && d.has_value && owner && owner->dom) {
      set_attr(*page, owner->dom, attr_name(static_cast<js::JsString*>(k)->utf8()), page->text(d.value));
      *ok = true;
      return true;
    }
    return Object::define_own_property(ctx, k, d, ok);
  }
  bool delete_property(Context& ctx, PropKey k, bool* ok) override {
    if (k->kind == js::CellKind::String && owner && owner->dom) {
      remove_attr(*page, owner->dom, attr_name(static_cast<js::JsString*>(k)->utf8()));
      *ok = true;
      return true;
    }
    return Object::delete_property(ctx, k, ok);
  }
  bool own_keys(Context& ctx, std::vector<PropKey>* out) override {
    if (owner && owner->dom)
      for (const auto& at : owner->dom->attrs)
        if (at.name.compare(0, 5, "data-") == 0) out->push_back(ctx.atom_utf8(key_name(at.name)));
    return Object::own_keys(ctx, out);
  }
};

static void set_class_attr(Page& P, Node* n, const std::vector<std::string>& words) {
  std::string joined;
  for (const auto& w : words) { if (!joined.empty()) joined += " "; joined += w; }
  set_attr(P, n, "class", joined);
}

static bool valid_token(Page& P, const std::string& t) {
  if (t.empty()) { P.ctx.throw_syntax_error("The token provided must not be empty."); return false; }
  for (char c : t)
    if (std::isspace(static_cast<unsigned char>(c))) {
      throw_dom(P, "InvalidCharacterError", "The token provided ('" + t + "') contains HTML space characters, which are not valid in tokens.");
      return false;
    }
  return true;
}

// ---------------------------------------------------------------------------------------------------
// attribute reflection
// ---------------------------------------------------------------------------------------------------

namespace {

enum class Reflect { String, Bool, Int, UrlString };
struct Reflected { const char* prop; const char* attr; Reflect kind; };

const Reflected kReflected[] = {
    {"id", "id", Reflect::String}, {"title", "title", Reflect::String}, {"lang", "lang", Reflect::String}, {"dir", "dir", Reflect::String},
    {"className", "class", Reflect::String}, {"name", "name", Reflect::String}, {"type", "type", Reflect::String}, {"placeholder", "placeholder", Reflect::String},
    {"alt", "alt", Reflect::String}, {"rel", "rel", Reflect::String}, {"target", "target", Reflect::String}, {"action", "action", Reflect::String},
    {"method", "method", Reflect::String}, {"htmlFor", "for", Reflect::String}, {"role", "role", Reflect::String}, {"accept", "accept", Reflect::String},
    {"autocomplete", "autocomplete", Reflect::String}, {"pattern", "pattern", Reflect::String}, {"min", "min", Reflect::String}, {"max", "max", Reflect::String},
    {"step", "step", Reflect::String}, {"wrap", "wrap", Reflect::String}, {"media", "media", Reflect::String}, {"crossOrigin", "crossorigin", Reflect::String},
    {"integrity", "integrity", Reflect::String}, {"slot", "slot", Reflect::String}, {"htmlContent", "content", Reflect::String}, {"label", "label", Reflect::String},
    {"hreflang", "hreflang", Reflect::String}, {"download", "download", Reflect::String}, {"enctype", "enctype", Reflect::String}, {"inputMode", "inputmode", Reflect::String},
    {"loading", "loading", Reflect::String}, {"decoding", "decoding", Reflect::String}, {"sizes", "sizes", Reflect::String}, {"srcset", "srcset", Reflect::String},
    {"href", "href", Reflect::UrlString}, {"src", "src", Reflect::UrlString},
    {"disabled", "disabled", Reflect::Bool}, {"checked", "checked", Reflect::Bool}, {"defaultChecked", "checked", Reflect::Bool}, {"selected", "selected", Reflect::Bool}, {"readOnly", "readonly", Reflect::Bool},
    {"required", "required", Reflect::Bool}, {"hidden", "hidden", Reflect::Bool}, {"multiple", "multiple", Reflect::Bool}, {"autofocus", "autofocus", Reflect::Bool},
    {"open", "open", Reflect::Bool}, {"async", "async", Reflect::Bool}, {"defer", "defer", Reflect::Bool}, {"noValidate", "novalidate", Reflect::Bool},
    {"draggable", "draggable", Reflect::Bool}, {"spellcheck", "spellcheck", Reflect::Bool}, {"isMap", "ismap", Reflect::Bool}, {"controls", "controls", Reflect::Bool},
    {"loop", "loop", Reflect::Bool}, {"muted", "muted", Reflect::Bool}, {"autoplay", "autoplay", Reflect::Bool}, {"contentEditable", "contenteditable", Reflect::String},
    {"tabIndex", "tabindex", Reflect::Int}, {"maxLength", "maxlength", Reflect::Int}, {"minLength", "minlength", Reflect::Int}, {"size", "size", Reflect::Int},
    {"cols", "cols", Reflect::Int}, {"rows", "rows", Reflect::Int}, {"colSpan", "colspan", Reflect::Int}, {"rowSpan", "rowspan", Reflect::Int},
};

// A custom element (a tag with a dash) has only the global reflected attributes; `this.target`, `this.name`, `this.label`,
// `this.open`, `this.size`... on one are the component's own fields, which a browser keeps as plain properties.
bool custom_plain_prop(const Node* n, const char* prop) {
  if (!n || n->tag.find('-') == std::string::npos) return false;
  for (const char* g : {"id", "title", "lang", "dir", "className", "slot", "hidden", "draggable", "spellcheck", "contentEditable", "tabIndex", "role", "inputMode", "autofocus"})
    if (std::strcmp(prop, g) == 0) return false;
  return true;
}

bool tag_is_field(const std::string& t) { return t == "input" || t == "textarea" || t == "select" || t == "option" || t == "button"; }

}  // namespace

// ---------------------------------------------------------------------------------------------------
// the node classes
// ---------------------------------------------------------------------------------------------------

namespace {

// Collections: arrays whose prototype adds NodeList's helpers.
Value node_list(Page& P, const std::vector<Node*>& nodes) {
  std::vector<Value> v;
  v.reserve(nodes.size());
  for (Node* n : nodes) v.push_back(P.node_value(n));
  js::ArrayObject* arr = P.ctx.new_array_from(v);
  arr->proto = P.node_list_proto;
  return Value::Obj(arr);
}

std::vector<Node*> children_of(Node* n, bool elements_only) {
  std::vector<Node*> out;
  for (auto& c : n->children)
    if (!elements_only || c->kind == Node::Kind::Element) out.push_back(c.get());
  return out;
}

Node* sibling(Node* n, int dir, bool elements_only) {
  if (!n || !n->parent) return nullptr;
  auto& sib = n->parent->children;
  for (size_t i = 0; i < sib.size(); ++i) {
    if (sib[i].get() != n) continue;
    long j = static_cast<long>(i) + dir;
    while (j >= 0 && static_cast<size_t>(j) < sib.size()) {
      if (!elements_only || sib[static_cast<size_t>(j)]->kind == Node::Kind::Element) return sib[static_cast<size_t>(j)].get();
      j += dir;
    }
    return nullptr;
  }
  return nullptr;
}

// The nodes a `append(...)` style call stands for: nodes as they are, anything else as text.
bool nodes_from_args(Page& P, CallArgs& a, int from, std::vector<web::NodePtr>* owned_text, std::vector<Node*>* out) {
  for (int i = from; i < a.argc; ++i) {
    if (NodeObject* no = node_arg(a.argv[i])) {
      if (no->dom) out->push_back(no->dom);
      continue;
    }
    auto tn = std::make_unique<Node>();
    tn->kind = Node::Kind::Text;
    tn->text = P.text(a.argv[i]);
    out->push_back(tn.get());
    owned_text->push_back(std::move(tn));
  }
  return true;
}

void install_node_class(Page& P) {
  Context& ctx = P.ctx;
  Page* pp = &P;
  Object* np = P.node_proto;

  def_method(P, np, "appendChild", 1, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    NodeObject* kid = node_arg(a[0]);
    if (!kid || !kid->dom) {
      pp->ctx.throw_type_error("Failed to execute 'appendChild' on 'Node': parameter 1 is not of type 'Node'.");
      return Value::Exception();
    }
    if (!self->dom) return a[0];
    if (!insert_node(*pp, self->dom, kid->dom, nullptr)) return Value::Exception();
    return a[0];
  });
  def_method(P, np, "insertBefore", 2, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    NodeObject* kid = node_arg(a[0]);
    if (!kid || !kid->dom) {
      pp->ctx.throw_type_error("Failed to execute 'insertBefore' on 'Node': parameter 1 is not of type 'Node'.");
      return Value::Exception();
    }
    NodeObject* ref = node_arg(a[1]);
    if (!self->dom) return a[0];
    if (!insert_node(*pp, self->dom, kid->dom, ref ? ref->dom : nullptr)) return Value::Exception();
    return a[0];
  });
  def_method(P, np, "removeChild", 1, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    NodeObject* kid = node_arg(a[0]);
    if (!kid || !kid->dom || !self->dom || kid->dom->parent != self->dom) {
      return throw_dom(*pp, "NotFoundError", "Failed to execute 'removeChild' on 'Node': The node to be removed is not a child of this node.");
    }
    remove_node(*pp, kid->dom);
    return a[0];
  });
  def_method(P, np, "replaceChild", 2, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    NodeObject* fresh = node_arg(a[0]);
    NodeObject* old = node_arg(a[1]);
    if (!fresh || !old || !fresh->dom || !old->dom || !self->dom || old->dom->parent != self->dom)
      return throw_dom(*pp, "NotFoundError", "Failed to execute 'replaceChild' on 'Node': The node to be replaced is not a child of this node.");
    Node* next = sibling(old->dom, 1, false);
    remove_node(*pp, old->dom);
    if (!insert_node(*pp, self->dom, fresh->dom, next)) return Value::Exception();
    return a[1];
  });
  def_method(P, np, "cloneNode", 0, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    if (!self->dom) return Value::Null();
    web::NodePtr c = clone_node(*self->dom, Context::to_boolean(a[0]));
    Node* raw = c.get();
    pp->adopt_orphan(std::move(c));
    return pp->node_value(raw);
  });
  def_method(P, np, "contains", 1, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    NodeObject* other = node_arg(a[0]);
    if (!self->dom || !other || !other->dom) return Value::Bool(false);
    return Value::Bool(is_inclusive_ancestor(self->dom, other->dom));
  });
  def_method(P, np, "hasChildNodes", 0, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return Value::Bool(self->dom && !self->dom->children.empty());
  });
  def_method(P, np, "getRootNode", 0, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    Node* n = self->dom;
    while (n && n->parent) n = n->parent;
    return pp->node_value(n);
  });
  def_method(P, np, "isSameNode", 1, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return Value::Bool(a[0].is_object() && a[0].as_object() == self);
  });
  def_method(P, np, "isEqualNode", 1, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    NodeObject* other = node_arg(a[0]);
    if (!self->dom || !other || !other->dom) return Value::Bool(false);
    return Value::Bool(serialize_html(*self->dom, true) == serialize_html(*other->dom, true));
  });
  def_method(P, np, "compareDocumentPosition", 1, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    NodeObject* other = node_arg(a[0]);
    if (!self->dom || !other || !other->dom) return Value::Number(1);
    if (self->dom == other->dom) return Value::Number(0);
    if (is_inclusive_ancestor(self->dom, other->dom)) return Value::Number(4 | 16);   // following + contained by
    if (is_inclusive_ancestor(other->dom, self->dom)) return Value::Number(2 | 8);    // preceding + contains
    // document order: walk the root once
    Node* root = self->dom;
    while (root->parent) root = root->parent;
    int order = 0;
    bool seen_self = false, seen_other = false;
    std::vector<Node*> stack{root};
    // iterative pre-order
    std::vector<Node*> walk;
    while (!stack.empty()) {
      Node* n = stack.back();
      stack.pop_back();
      walk.push_back(n);
      for (size_t i = n->children.size(); i > 0; --i) stack.push_back(n->children[i - 1].get());
    }
    for (Node* n : walk) {
      if (n == self->dom) { seen_self = true; if (!seen_other) order = 4; }
      if (n == other->dom) { seen_other = true; if (!seen_self) order = 2; }
    }
    return Value::Number(order ? order : 1);
  });
  def_method(P, np, "normalize", 0, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return Value::Undefined();
  });

  auto get_str = [pp](const char* name, std::function<std::string(Node&)> f) {
    def_accessor(*pp, pp->node_proto, name, [pp, f](Context& c, CallArgs& a) -> Value {
      NodeObject* self = this_node(*pp, a);
      if (!self) return Value::Exception();
      if (!self->dom) return Value::String(c.atoms.empty);
      return Value::String(c.new_string_utf8(f(*self->dom)));
    });
  };
  get_str("nodeName", [](Node& n) { return n.kind == Node::Kind::Text ? std::string("#text") : n.kind == Node::Kind::Document ? std::string("#document") : upper(n.tag); });
  get_str("baseURI", [pp](Node&) { return pp->doc ? pp->doc->base : std::string(); });
  def_accessor(P, np, "nodeType", [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    if (!self->dom) return Value::Number(0);
    return Value::Number(self->dom->kind == Node::Kind::Text ? 3 : self->dom->kind == Node::Kind::Document ? 9 : is_fragment(*self->dom) ? 11 : 1);
  });
  def_accessor(P, np, "nodeValue", [pp](Context& c, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    if (!self->dom || self->dom->kind != Node::Kind::Text) return Value::Null();
    return Value::String(c.new_string_utf8(self->dom->text));
  }, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    if (self->dom && self->dom->kind == Node::Kind::Text) set_text_content(*pp, self->dom, pp->text(a[0]));
    return Value::Undefined();
  });
  def_accessor(P, np, "textContent", [pp](Context& c, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    if (!self->dom) return Value::String(c.atoms.empty);
    std::string t;
    collect_text(*self->dom, t);
    return Value::String(c.new_string_utf8(t));
  }, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    set_text_content(*pp, self->dom, a[0].is_nullish() ? std::string() : pp->text(a[0]));
    return Value::Undefined();
  });
  def_accessor(P, np, "parentNode", [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    Node* p = self->dom ? self->dom->parent : nullptr;
    if (p && p->kind == Node::Kind::Document) return Value::Obj(pp->document_obj);
    return pp->node_value(p);
  });
  def_accessor(P, np, "parentElement", [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    Node* p = self->dom ? self->dom->parent : nullptr;
    if (!p || p->kind != Node::Kind::Element) return Value::Null();
    return pp->node_value(p);
  });
  def_accessor(P, np, "childNodes", [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return node_list(*pp, self->dom ? children_of(self->dom, false) : std::vector<Node*>());
  });
  def_accessor(P, np, "firstChild", [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return self->dom && !self->dom->children.empty() ? pp->node_value(self->dom->children.front().get()) : Value::Null();
  });
  def_accessor(P, np, "lastChild", [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return self->dom && !self->dom->children.empty() ? pp->node_value(self->dom->children.back().get()) : Value::Null();
  });
  def_accessor(P, np, "nextSibling", [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return pp->node_value(sibling(self->dom, 1, false));
  });
  def_accessor(P, np, "previousSibling", [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return pp->node_value(sibling(self->dom, -1, false));
  });
  def_accessor(P, np, "isConnected", [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    Node* root = document_root(*pp);
    return Value::Bool(root && self->dom && is_inclusive_ancestor(root, self->dom));
  });
  def_accessor(P, np, "ownerDocument", [pp](Context&, CallArgs&) -> Value { return Value::Obj(pp->document_obj); });
  (void)ctx;
}

// ---- Element ----

void install_element_class(Page& P) {
  Page* pp = &P;
  Object* ep = P.element_proto;

  def_accessor(P, ep, "tagName", [pp](Context& c, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return self->dom ? Value::String(c.new_string_utf8(upper(self->dom->tag))) : Value::Undefined();
  });
  def_accessor(P, ep, "localName", [pp](Context& c, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return self->dom ? Value::String(c.new_string_utf8(self->dom->tag)) : Value::Undefined();
  });
  def_accessor(P, ep, "namespaceURI", [pp](Context& c, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return Value::String(c.atom_utf8(self->dom && self->dom->tag == "svg" ? "http://www.w3.org/2000/svg" : "http://www.w3.org/1999/xhtml"));
  });
  def_accessor(P, ep, "children", [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return node_list(*pp, self->dom ? children_of(self->dom, true) : std::vector<Node*>());
  });
  auto elem_get = [pp](const char* name, std::function<Node*(Node*)> f) {
    def_accessor(*pp, pp->element_proto, name, [pp, f](Context&, CallArgs& a) -> Value {
      NodeObject* self = this_node(*pp, a);
      if (!self) return Value::Exception();
      return self->dom ? pp->node_value(f(self->dom)) : Value::Null();
    });
  };
  elem_get("firstElementChild", [](Node* n) -> Node* { for (auto& c : n->children) if (c->kind == Node::Kind::Element) return c.get(); return nullptr; });
  elem_get("lastElementChild", [](Node* n) -> Node* { for (size_t i = n->children.size(); i > 0; --i) if (n->children[i - 1]->kind == Node::Kind::Element) return n->children[i - 1].get(); return nullptr; });
  elem_get("nextElementSibling", [](Node* n) { return sibling(n, 1, true); });
  elem_get("previousElementSibling", [](Node* n) { return sibling(n, -1, true); });
  def_accessor(P, ep, "childElementCount", [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return Value::Number(self->dom ? static_cast<double>(children_of(self->dom, true).size()) : 0);
  });

  // attributes
  def_method(P, ep, "getAttribute", 1, [pp](Context& c, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    const std::string name = lower(arg_text(*pp, a, 0));
    if (!self->dom || !self->dom->has_attr(name)) return Value::Null();
    return Value::String(c.new_string_utf8(self->dom->attr(name)));
  });
  def_method(P, ep, "setAttribute", 2, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    if (self->dom) set_attr(*pp, self->dom, lower(arg_text(*pp, a, 0)), arg_text(*pp, a, 1));
    return Value::Undefined();
  });
  def_method(P, ep, "hasAttribute", 1, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return Value::Bool(self->dom && self->dom->has_attr(lower(arg_text(*pp, a, 0))));
  });
  def_method(P, ep, "removeAttribute", 1, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    if (self->dom) remove_attr(*pp, self->dom, lower(arg_text(*pp, a, 0)));
    return Value::Undefined();
  });
  def_method(P, ep, "toggleAttribute", 1, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self || !self->dom) return Value::Bool(false);
    const std::string name = lower(arg_text(*pp, a, 0));
    const bool has = self->dom->has_attr(name);
    const bool want = a.argc > 1 ? Context::to_boolean(a[1]) : !has;
    if (want && !has) set_attr(*pp, self->dom, name, "");
    else if (!want && has) remove_attr(*pp, self->dom, name);
    return Value::Bool(want);
  });
  def_method(P, ep, "getAttributeNames", 0, [pp](Context& c, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    std::vector<Value> out;
    if (self->dom) for (const auto& at : self->dom->attrs) out.push_back(Value::String(c.atom_utf8(at.name)));
    return make_array(*pp, out);
  });
  def_method(P, ep, "hasAttributes", 0, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return Value::Bool(self->dom && !self->dom->attrs.empty());
  });
  def_accessor(P, ep, "attributes", [pp](Context& c, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    std::vector<Value> out;
    if (self->dom)
      for (const auto& at : self->dom->attrs) {
        Object* o = c.new_object();
        c.define_data(o, c.atoms.name, Value::String(c.atom_utf8(at.name)));
        c.define_data(o, c.atom_ascii("nodeName"), Value::String(c.atom_utf8(at.name)));
        c.define_data(o, c.atoms.value, Value::String(c.new_string_utf8(at.value)));
        c.define_data(o, c.atom_ascii("nodeValue"), Value::String(c.new_string_utf8(at.value)));
        out.push_back(Value::Obj(o));
      }
    js::ArrayObject* arr = c.new_array_from(out);
    for (const Value& v : out) {
      Value nm;
      c.get(v, c.atoms.name, &nm);
      c.define_data(arr, c.to_atom(nm.as_string()), v, js::kHiddenAttrs);
    }
    c.define_method(arr, "getNamedItem", [](Context& cc, CallArgs& aa) -> Value {
      Value r;
      js::JsString* s;
      if (!cc.to_string(aa[0], &s)) return Value::Exception();
      if (!cc.get(aa.this_value, cc.to_atom(s), &r)) return Value::Exception();
      return r.is_undefined() ? Value::Null() : r;
    }, 1);
    return Value::Obj(arr);
  });

  // selection
  def_method(P, ep, "matches", 1, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    web::CompiledSelectors sel;
    if (!compile_or_throw(*pp, arg_text(*pp, a, 0), &sel)) return Value::Exception();
    return Value::Bool(self->dom && self->dom->kind == Node::Kind::Element && web::compiled_selectors_match(*self->dom, sel));
  });
  def_method(P, ep, "webkitMatchesSelector", 1, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    web::CompiledSelectors sel;
    if (!compile_or_throw(*pp, arg_text(*pp, a, 0), &sel)) return Value::Exception();
    return Value::Bool(self->dom && self->dom->kind == Node::Kind::Element && web::compiled_selectors_match(*self->dom, sel));
  });
  def_method(P, ep, "closest", 1, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    web::CompiledSelectors sel;
    if (!compile_or_throw(*pp, arg_text(*pp, a, 0), &sel)) return Value::Exception();
    for (Node* n = self->dom; n; n = n->parent)
      if (n->kind == Node::Kind::Element && web::compiled_selectors_match(*n, sel)) return pp->node_value(n);
    return Value::Null();
  });
  def_method(P, ep, "querySelector", 1, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return select_in(*pp, self->dom, arg_text(*pp, a, 0), false, false);
  });
  def_method(P, ep, "querySelectorAll", 1, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return select_in(*pp, self->dom, arg_text(*pp, a, 0), false, true);
  });
  def_method(P, ep, "getElementsByTagName", 1, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    std::vector<Node*> hits;
    if (self->dom) for (auto& c : self->dom->children) by_tag(c.get(), lower(arg_text(*pp, a, 0)), hits);
    return node_list(*pp, hits);
  });
  def_method(P, ep, "getElementsByClassName", 1, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    std::vector<std::string> words;
    std::string t = arg_text(*pp, a, 0), w;
    for (char ch : t + " ") { if (std::isspace(static_cast<unsigned char>(ch))) { if (!w.empty()) words.push_back(w); w.clear(); } else w += ch; }
    std::vector<Node*> hits;
    if (self->dom) for (auto& c : self->dom->children) by_class(c.get(), words, hits);
    return node_list(*pp, hits);
  });

  // mutation
  def_method(P, ep, "remove", 0, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    if (self->dom && self->dom->parent) remove_node(*pp, self->dom);
    return Value::Undefined();
  });
  auto multi = [pp](const char* name, int mode) {
    // 0 append, 1 prepend, 2 before, 3 after, 4 replaceWith
    def_method(*pp, pp->element_proto, name, 0, [pp, mode](Context&, CallArgs& a) -> Value {
      NodeObject* self = this_node(*pp, a);
      if (!self) return Value::Exception();
      if (!self->dom) return Value::Undefined();
      std::vector<web::NodePtr> texts;
      std::vector<Node*> nodes;
      nodes_from_args(*pp, a, 0, &texts, &nodes);
      Node* parent = mode >= 2 ? self->dom->parent : self->dom;
      if (!parent) return Value::Undefined();
      Node* anchor = nullptr;
      if (mode == 0) anchor = nullptr;
      else if (mode == 1) anchor = parent->children.empty() ? nullptr : parent->children.front().get();
      else if (mode == 2) anchor = self->dom;
      else anchor = sibling(self->dom, 1, false);
      if (mode == 4) {
        anchor = sibling(self->dom, 1, false);
        for (Node* n : nodes) if (n == anchor) anchor = sibling(anchor, 1, false);
      }
      for (auto& t : texts) {   // text nodes made here are owned by the orphan list until inserted
        Node* raw = t.get();
        pp->adopt_orphan(std::move(t));
        (void)raw;
      }
      for (Node* n : nodes) {
        if (n == anchor) anchor = sibling(anchor, 1, false);
        if (!insert_node(*pp, parent, n, mode == 3 || mode == 4 ? anchor : anchor)) return Value::Exception();
      }
      if (mode == 4 && self->dom->parent) remove_node(*pp, self->dom);
      return Value::Undefined();
    });
  };
  multi("append", 0);
  multi("prepend", 1);
  multi("before", 2);
  multi("after", 3);
  multi("replaceWith", 4);
  def_method(P, ep, "replaceChildren", 0, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self || !self->dom) return Value::Undefined();
    clear_children(*pp, self->dom);
    std::vector<web::NodePtr> texts;
    std::vector<Node*> nodes;
    nodes_from_args(*pp, a, 0, &texts, &nodes);
    for (auto& t : texts) pp->adopt_orphan(std::move(t));
    for (Node* n : nodes) if (!insert_node(*pp, self->dom, n, nullptr)) return Value::Exception();
    pp->write();
    return Value::Undefined();
  });
  def_method(P, ep, "insertAdjacentHTML", 2, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self || !self->dom) return Value::Undefined();
    const std::string where = lower(arg_text(*pp, a, 0)), html = arg_text(*pp, a, 1);
    Node* n = self->dom;
    if (where == "beforeend") parse_fragment_into(*pp, n, html);
    else if (where == "afterbegin") parse_fragment_into(*pp, n, html, n->children.empty() ? nullptr : n->children.front().get());
    else if (where == "beforebegin" && n->parent) parse_fragment_into(*pp, n->parent, html, n);
    else if (where == "afterend" && n->parent) parse_fragment_into(*pp, n->parent, html, sibling(n, 1, false));
    else return throw_dom(*pp, "SyntaxError", "The value provided ('" + where + "') is not one of 'beforebegin', 'afterbegin', 'beforeend', or 'afterend'.");
    return Value::Undefined();
  });
  def_method(P, ep, "insertAdjacentElement", 2, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    NodeObject* kid = node_arg(a[1]);
    if (!self || !self->dom || !kid || !kid->dom) return Value::Null();
    const std::string where = lower(arg_text(*pp, a, 0));
    Node* n = self->dom;
    bool ok = true;
    if (where == "beforeend") ok = insert_node(*pp, n, kid->dom, nullptr);
    else if (where == "afterbegin") ok = insert_node(*pp, n, kid->dom, n->children.empty() ? nullptr : n->children.front().get());
    else if (where == "beforebegin" && n->parent) ok = insert_node(*pp, n->parent, kid->dom, n);
    else if (where == "afterend" && n->parent) ok = insert_node(*pp, n->parent, kid->dom, sibling(n, 1, false));
    else return Value::Null();
    return ok ? a[1] : Value::Exception();
  });
  def_method(P, ep, "insertAdjacentText", 2, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self || !self->dom) return Value::Undefined();
    auto tn = std::make_unique<Node>();
    tn->kind = Node::Kind::Text;
    tn->text = arg_text(*pp, a, 1);
    Node* raw = tn.get();
    pp->adopt_orphan(std::move(tn));
    const std::string where = lower(arg_text(*pp, a, 0));
    Node* n = self->dom;
    if (where == "beforeend") insert_node(*pp, n, raw, nullptr);
    else if (where == "afterbegin") insert_node(*pp, n, raw, n->children.empty() ? nullptr : n->children.front().get());
    else if (where == "beforebegin" && n->parent) insert_node(*pp, n->parent, raw, n);
    else if (where == "afterend" && n->parent) insert_node(*pp, n->parent, raw, sibling(n, 1, false));
    return Value::Undefined();
  });

  // markup
  def_accessor(P, ep, "innerHTML", [pp](Context& c, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return self->dom ? Value::String(c.new_string_utf8(serialize_html(*self->dom, false))) : Value::String(c.atoms.empty);
  }, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    if (!self->dom) return Value::Undefined();
    clear_children(*pp, self->dom);
    if (!a[0].is_nullish()) parse_fragment_into(*pp, self->dom, pp->text(a[0]));
    pp->write();
    return Value::Undefined();
  });
  def_accessor(P, ep, "outerHTML", [pp](Context& c, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return self->dom ? Value::String(c.new_string_utf8(serialize_html(*self->dom, true))) : Value::String(c.atoms.empty);
  }, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self || !self->dom || !self->dom->parent) return Value::Undefined();
    Node* parent = self->dom->parent;
    Node* next = sibling(self->dom, 1, false);
    parse_fragment_into(*pp, parent, pp->text(a[0]), next);
    remove_node(*pp, self->dom);
    return Value::Undefined();
  });
  def_accessor(P, ep, "innerText", [pp](Context& c, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    std::string t;
    if (self->dom) collect_text(*self->dom, t);
    return Value::String(c.new_string_utf8(t));
  }, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    set_text_content(*pp, self->dom, a[0].is_nullish() ? std::string() : pp->text(a[0]));
    return Value::Undefined();
  });
}

// ---- HTMLElement: reflected attributes, classList, style, dataset, geometry, focus ----

Object* make_class_list(Page& P, NodeObject* owner) {
  ClassListObject* o = P.ctx.heap.make<ClassListObject>();
  o->proto = P.classlist_proto;
  o->owner = owner;
  return o;
}

ClassListObject* this_class_list(Page& P, CallArgs& a) {
  return static_cast<ClassListObject*>(this_host(P, a, HostKind::ClassList, "DOMTokenList"));
}

void install_class_list(Page& P) {
  Page* pp = &P;
  Object* cp = P.classlist_proto;
  auto words_of = [](ClassListObject* c) { return c->owner && c->owner->dom ? class_words(*c->owner->dom) : std::vector<std::string>(); };
  def_method(P, cp, "add", 0, [pp, words_of](Context&, CallArgs& a) -> Value {
    ClassListObject* c = this_class_list(*pp, a);
    if (!c) return Value::Exception();
    if (!c->owner || !c->owner->dom) return Value::Undefined();
    auto words = words_of(c);
    for (int i = 0; i < a.argc; ++i) {
      const std::string w = pp->text(a.argv[i]);
      if (!valid_token(*pp, w)) return Value::Exception();
      if (std::find(words.begin(), words.end(), w) == words.end()) words.push_back(w);
    }
    set_class_attr(*pp, c->owner->dom, words);
    return Value::Undefined();
  });
  def_method(P, cp, "remove", 0, [pp, words_of](Context&, CallArgs& a) -> Value {
    ClassListObject* c = this_class_list(*pp, a);
    if (!c) return Value::Exception();
    if (!c->owner || !c->owner->dom) return Value::Undefined();
    auto words = words_of(c);
    for (int i = 0; i < a.argc; ++i) {
      const std::string w = pp->text(a.argv[i]);
      if (!valid_token(*pp, w)) return Value::Exception();
      words.erase(std::remove(words.begin(), words.end(), w), words.end());
    }
    set_class_attr(*pp, c->owner->dom, words);
    return Value::Undefined();
  });
  def_method(P, cp, "contains", 1, [pp, words_of](Context&, CallArgs& a) -> Value {
    ClassListObject* c = this_class_list(*pp, a);
    if (!c) return Value::Exception();
    const std::string w = arg_text(*pp, a, 0);
    auto words = words_of(c);
    return Value::Bool(std::find(words.begin(), words.end(), w) != words.end());
  });
  def_method(P, cp, "toggle", 1, [pp, words_of](Context&, CallArgs& a) -> Value {
    ClassListObject* c = this_class_list(*pp, a);
    if (!c) return Value::Exception();
    const std::string w = arg_text(*pp, a, 0);
    if (!valid_token(*pp, w)) return Value::Exception();
    if (!c->owner || !c->owner->dom) return Value::Bool(false);
    auto words = words_of(c);
    const bool has = std::find(words.begin(), words.end(), w) != words.end();
    const bool want = a.argc > 1 && !a[1].is_undefined() ? Context::to_boolean(a[1]) : !has;
    if (want && !has) words.push_back(w);
    else if (!want && has) words.erase(std::remove(words.begin(), words.end(), w), words.end());
    set_class_attr(*pp, c->owner->dom, words);
    return Value::Bool(want);
  });
  def_method(P, cp, "replace", 2, [pp, words_of](Context&, CallArgs& a) -> Value {
    ClassListObject* c = this_class_list(*pp, a);
    if (!c) return Value::Exception();
    const std::string from = arg_text(*pp, a, 0), to = arg_text(*pp, a, 1);
    if (!valid_token(*pp, from) || !valid_token(*pp, to)) return Value::Exception();
    if (!c->owner || !c->owner->dom) return Value::Bool(false);
    auto words = words_of(c);
    auto it = std::find(words.begin(), words.end(), from);
    if (it == words.end()) return Value::Bool(false);
    *it = to;
    // drop a duplicate of the replacement
    bool seen = false;
    for (size_t i = 0; i < words.size(); ++i)
      if (words[i] == to) { if (seen) { words.erase(words.begin() + static_cast<long>(i)); --i; } seen = true; }
    set_class_attr(*pp, c->owner->dom, words);
    return Value::Bool(true);
  });
  def_method(P, cp, "item", 1, [pp, words_of](Context& cx, CallArgs& a) -> Value {
    ClassListObject* c = this_class_list(*pp, a);
    if (!c) return Value::Exception();
    auto words = words_of(c);
    double i = pp->number(a[0]);
    if (i < 0 || i >= static_cast<double>(words.size())) return Value::Null();
    return Value::String(cx.new_string_utf8(words[static_cast<size_t>(i)]));
  });
  def_method(P, cp, "supports", 1, [](Context&, CallArgs&) -> Value { return Value::Bool(true); });
  def_method(P, cp, "toString", 0, [pp](Context& cx, CallArgs& a) -> Value {
    ClassListObject* c = this_class_list(*pp, a);
    if (!c) return Value::Exception();
    return Value::String(cx.new_string_utf8(c->owner && c->owner->dom ? c->owner->dom->attr("class") : std::string()));
  });
  def_accessor(P, cp, "length", [pp, words_of](Context&, CallArgs& a) -> Value {
    ClassListObject* c = this_class_list(*pp, a);
    if (!c) return Value::Exception();
    return Value::Number(static_cast<double>(words_of(c).size()));
  });
  def_accessor(P, cp, "value", [pp](Context& cx, CallArgs& a) -> Value {
    ClassListObject* c = this_class_list(*pp, a);
    if (!c) return Value::Exception();
    return Value::String(cx.new_string_utf8(c->owner && c->owner->dom ? c->owner->dom->attr("class") : std::string()));
  }, [pp](Context&, CallArgs& a) -> Value {
    ClassListObject* c = this_class_list(*pp, a);
    if (!c) return Value::Exception();
    if (c->owner && c->owner->dom) set_attr(*pp, c->owner->dom, "class", pp->text(a[0]));
    return Value::Undefined();
  });
  // iteration over a snapshot
  auto snapshot = [pp, words_of](CallArgs& a, int kind, Context& cx) -> Value {
    ClassListObject* c = this_class_list(*pp, a);
    if (!c) return Value::Exception();
    std::vector<Value> items;
    auto words = words_of(c);
    for (size_t i = 0; i < words.size(); ++i) {
      Value w = Value::String(cx.new_string_utf8(words[i]));
      if (kind == 0) items.push_back(Value::Number(static_cast<double>(i)));
      else if (kind == 1) items.push_back(w);
      else items.push_back(make_array(*pp, {Value::Number(static_cast<double>(i)), w}));
    }
    Value arr = make_array(*pp, items), fn, r;
    if (!cx.get(arr, cx.atom_ascii(kind == 0 ? "keys" : kind == 1 ? "values" : "entries"), &fn)) return Value::Exception();
    if (!cx.call(fn, arr, nullptr, 0, &r)) return Value::Exception();
    return r;
  };
  Function* values = def_method(P, cp, "values", 0, [snapshot](Context& cx, CallArgs& a) -> Value { return snapshot(a, 1, cx); });
  def_method(P, cp, "keys", 0, [snapshot](Context& cx, CallArgs& a) -> Value { return snapshot(a, 0, cx); });
  def_method(P, cp, "entries", 0, [snapshot](Context& cx, CallArgs& a) -> Value { return snapshot(a, 2, cx); });
  P.ctx.define_data(cp, P.ctx.sym.iterator, Value::Obj(values), js::kHiddenAttrs);
  def_method(P, cp, "forEach", 1, [pp, words_of](Context& cx, CallArgs& a) -> Value {
    ClassListObject* c = this_class_list(*pp, a);
    if (!c) return Value::Exception();
    auto words = words_of(c);
    for (size_t i = 0; i < words.size(); ++i) {
      Value ignored;
      if (!cx.call(a[0], a[1], {Value::String(cx.new_string_utf8(words[i])), Value::Number(static_cast<double>(i)), a.this_value}, &ignored)) return Value::Exception();
    }
    return Value::Undefined();
  });
}

Value rect_object(Page& P) {
  Object* r = P.ctx.new_object();
  for (const char* k : {"x", "y", "top", "left", "right", "bottom", "width", "height"}) P.ctx.define_data(r, P.ctx.atom_ascii(k), Value::Number(0));
  return Value::Obj(r);
}

// The first <option> under a <select>, descendants in document order.
void collect_options(Node* n, std::vector<Node*>& out) {
  for (auto& c : n->children) {
    if (c->kind != Node::Kind::Element) continue;
    if (c->tag == "option") out.push_back(c.get());
    else if (c->tag == "optgroup") collect_options(c.get(), out);
  }
}

std::string option_value(Node* o) { return o->has_attr("value") ? o->attr("value") : o->inner_text(); }

Node* selected_option(Node* sel) {
  std::vector<Node*> opts;
  collect_options(sel, opts);
  for (Node* o : opts) if (o->has_attr("selected")) return o;
  return opts.empty() ? nullptr : opts.front();
}

void install_html_element_class(Page& P) {
  Page* pp = &P;
  Object* hp = P.html_element_proto;

  // reflected attributes, installed once on HTMLElement.prototype
  for (const Reflected& r : kReflected) {
    const Reflected rr = r;
    def_accessor(P, hp, rr.prop, [pp, rr](Context& c, CallArgs& a) -> Value {
      NodeObject* self = this_node(*pp, a);
      if (!self) return Value::Exception();
      Node* n = self->dom;
      if (!n) return rr.kind == Reflect::Bool ? Value::Bool(false) : Value::String(c.atoms.empty);
      if (custom_plain_prop(n, rr.prop)) return Value::Undefined();
      switch (rr.kind) {
        case Reflect::Bool: return Value::Bool(n->has_attr(rr.attr));
        case Reflect::Int: return Value::Number(n->has_attr(rr.attr) ? std::atoi(n->attr(rr.attr).c_str()) : (std::strcmp(rr.attr, "tabindex") == 0 ? -1 : 0));
        case Reflect::UrlString: {
          const std::string v = n->attr(rr.attr);
          return Value::String(c.new_string_utf8(v.empty() ? v : resolve(*pp, v)));
        }
        default: return Value::String(c.new_string_utf8(n->attr(rr.attr)));
      }
    }, [pp, rr](Context& c, CallArgs& a) -> Value {
      NodeObject* self = this_node(*pp, a);
      if (!self) return Value::Exception();
      Node* n = self->dom;
      if (!n) return Value::Undefined();
      if (custom_plain_prop(n, rr.prop)) { c.define_data(self, c.atom_utf8(rr.prop), a[0]); return Value::Undefined(); }
      if (rr.kind == Reflect::Bool) {
        if (Context::to_boolean(a[0])) { if (!n->has_attr(rr.attr)) set_attr(*pp, n, rr.attr, ""); }
        else remove_attr(*pp, n, rr.attr);
      } else {
        set_attr(*pp, n, rr.attr, pp->text(a[0]));
      }
      return Value::Undefined();
    });
  }

  def_accessor(P, hp, "classList", [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    if (!self->class_list) self->class_list = make_class_list(*pp, self);
    return Value::Obj(self->class_list);
  }, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    if (self->dom) set_attr(*pp, self->dom, "class", pp->text(a[0]));
    return Value::Undefined();
  });
  def_accessor(P, hp, "style", [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    if (!self->style) {
      StyleObject* s = pp->ctx.heap.make<StyleObject>();
      s->proto = pp->style_proto;
      s->owner = self;
      s->page = pp;
      self->style = s;
    }
    return Value::Obj(self->style);
  }, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self || !self->dom) return Value::Undefined();
    if (a[0].is_nullish()) remove_attr(*pp, self->dom, "style");
    else set_attr(*pp, self->dom, "style", pp->text(a[0]));
    return Value::Undefined();
  });
  def_accessor(P, hp, "dataset", [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    if (!self->dataset) {
      DatasetObject* d = pp->ctx.heap.make<DatasetObject>();
      d->proto = pp->ctx.intr.object_proto;
      d->owner = self;
      d->page = pp;
      self->dataset = d;
    }
    return Value::Obj(self->dataset);
  });

  // geometry: this engine lays a page out for a viewport and exposes no per-element boxes to script
  def_method(P, hp, "getBoundingClientRect", 0, [pp](Context&, CallArgs&) -> Value { return rect_object(*pp); });
  def_method(P, hp, "getClientRects", 0, [pp](Context&, CallArgs&) -> Value { return make_array(*pp, {}); });
  for (const char* g : {"offsetWidth", "offsetHeight", "offsetTop", "offsetLeft", "clientWidth", "clientHeight", "clientTop", "clientLeft", "scrollWidth", "scrollHeight"})
    def_accessor(P, hp, g, [](Context&, CallArgs&) -> Value { return Value::Number(0); });
  def_accessor(P, hp, "offsetParent", [](Context&, CallArgs&) -> Value { return Value::Null(); });
  for (const char* g : {"scrollTop", "scrollLeft"})
    def_accessor(P, hp, g, [](Context&, CallArgs&) -> Value { return Value::Number(0); }, [](Context&, CallArgs&) -> Value { return Value::Undefined(); });
  for (const char* m : {"scrollIntoView", "scrollTo", "scrollBy", "scroll", "blur", "select", "setSelectionRange", "setCustomValidity", "showPopover", "hidePopover", "requestFullscreen"})
    def_method(P, hp, m, 0, [](Context&, CallArgs&) -> Value { return Value::Undefined(); });
  def_method(P, hp, "checkValidity", 0, [](Context&, CallArgs&) -> Value { return Value::Bool(true); });
  def_method(P, hp, "reportValidity", 0, [](Context&, CallArgs&) -> Value { return Value::Bool(true); });
  def_method(P, hp, "getAnimations", 0, [pp](Context&, CallArgs&) -> Value { return make_array(*pp, {}); });
  def_accessor(P, hp, "isContentEditable", [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return Value::Bool(self->dom && self->dom->attr("contenteditable") != "" && self->dom->attr("contenteditable") != "false");
  });

  // focus is a request to the pane, which owns the keyboard
  def_method(P, hp, "focus", 0, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    if (self->dom) { pp->focus_request = self->dom; pp->focused = self->dom; }
    return Value::Undefined();
  });
  def_method(P, hp, "click", 0, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    if (!self->dom) return Value::Undefined();
    Object* ev = make_event(*pp, "click", "MouseEvent", true, true);
    dispatch_event_object(*pp, self->dom, self, ev);
    // The default action of a click: a submit button sends its form, a link
    // goes to its address -- unless a handler said no.
    if (!event_default_prevented(ev)) {
      if (web::is_submit_control(self->dom)) {
        if (web::Node* form = web::enclosing_form(self->dom)) {
          Object* sev = make_event(*pp, "submit", "Event", true, true);
          NodeObject* fobj = static_cast<NodeObject*>(pp->node_value(form).as_object());
          dispatch_event_object(*pp, form, fobj, sev);
          if (!event_default_prevented(sev)) { pp->submit_form = form; pp->submit_by = self->dom; }
        }
      } else if (self->dom->tag == "a" && self->dom->has_attr("href")) {
        const std::string url = resolve(*pp, self->dom->attr("href"));
        if (url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0) pp->nav_url = url;
      }
    }
    return Value::Undefined();
  });
  def_accessor(P, hp, "form", [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return pp->node_value(self->dom ? web::enclosing_form(self->dom) : nullptr);
  }, [pp](Context& c, CallArgs& a) -> Value {
    // `form` is a getter on form controls only; on a custom element or a div a framework assigns it as a plain property.
    NodeObject* self = this_node(*pp, a);
    if (self && self->dom && !tag_is_field(self->dom->tag) && self->dom->tag != "button" && self->dom->tag != "label") c.define_data(self, c.atom_ascii("form"), a[0]);
    return Value::Undefined();
  });

  // form fields: value, select, options
  auto field = [](Node* n) { return n && tag_is_field(n->tag); };
  def_accessor(P, hp, "value", [pp, field](Context& c, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    Node* n = self->dom;
    if (!n) return Value::String(c.atoms.empty);
    if (n->tag == "select") {
      Node* o = selected_option(n);
      return Value::String(c.new_string_utf8(o ? option_value(o) : std::string()));
    }
    if (n->tag == "option") return Value::String(c.new_string_utf8(option_value(n)));
    if (n->tag == "textarea" && !n->has_attr("value")) return Value::String(c.new_string_utf8(n->inner_text()));
    if (field(n) || n->tag == "li" || n->tag == "progress" || n->tag == "meter" || n->tag == "param" || n->tag == "data") return Value::String(c.new_string_utf8(n->attr("value")));
    // any other element: a plain expando, which the generic property machinery owns
    Value v;
    if (!c.get(self, c.atom_ascii("__value"), &v)) return Value::Exception();
    return v;
  }, [pp, field](Context& c, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    Node* n = self->dom;
    if (!n) return Value::Undefined();
    if (n->tag == "select") {
      const std::string want = pp->text(a[0]);
      std::vector<Node*> opts;
      collect_options(n, opts);
      for (Node* o : opts) {
        if (option_value(o) == want) { if (!o->has_attr("selected")) set_attr(*pp, o, "selected", ""); }
        else remove_attr(*pp, o, "selected");
      }
      return Value::Undefined();
    }
    if (field(n)) {
      const std::string text = a[0].is_nullish() ? std::string() : pp->text(a[0]);
      set_attr(*pp, n, "value", text);
      if (n->tag == "textarea") set_text_content(*pp, n, text);
      return Value::Undefined();
    }
    c.define_data(self, c.atom_ascii("value"), a[0]);   // a plain property on a non-field element
    return Value::Undefined();
  });
  def_accessor(P, hp, "defaultValue", [pp](Context& c, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return Value::String(c.new_string_utf8(self->dom ? (self->dom->tag == "textarea" ? self->dom->inner_text() : self->dom->attr("value")) : std::string()));
  }, [pp](Context&, CallArgs& a) -> Value {
    // React assigns node.defaultValue in strict mode while it syncs an input: a getter-only accessor throws there.
    NodeObject* self = this_node(*pp, a);
    if (!self || !self->dom) return Value::Undefined();
    const std::string text = a[0].is_nullish() ? std::string() : pp->text(a[0]);
    if (self->dom->tag == "textarea") set_text_content(*pp, self->dom, text);
    else set_attr(*pp, self->dom, "value", text);
    return Value::Undefined();
  });
  def_accessor(P, hp, "selectedIndex", [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    if (!self->dom || self->dom->tag != "select") return Value::Number(-1);
    std::vector<Node*> opts;
    collect_options(self->dom, opts);
    Node* sel = selected_option(self->dom);
    for (size_t i = 0; i < opts.size(); ++i) if (opts[i] == sel) return Value::Number(static_cast<double>(i));
    return Value::Number(-1);
  }, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self || !self->dom || self->dom->tag != "select") return Value::Undefined();
    std::vector<Node*> opts;
    collect_options(self->dom, opts);
    const double want = pp->number(a[0]);
    for (size_t i = 0; i < opts.size(); ++i) {
      if (static_cast<double>(i) == want) { if (!opts[i]->has_attr("selected")) set_attr(*pp, opts[i], "selected", ""); }
      else remove_attr(*pp, opts[i], "selected");
    }
    return Value::Undefined();
  });
  def_accessor(P, hp, "options", [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    std::vector<Node*> opts;
    if (self->dom && self->dom->tag == "select") collect_options(self->dom, opts);
    return node_list(*pp, opts);
  });
  def_accessor(P, hp, "elements", [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    std::vector<Node*> hits;
    if (self->dom) query_all(self->dom, "input, select, textarea, button, fieldset", hits, false);
    return node_list(*pp, hits);
  });
  def_accessor(P, hp, "text", [pp](Context& c, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    std::string t;
    if (self->dom) collect_text(*self->dom, t);
    return Value::String(c.new_string_utf8(t));
  }, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (self && self->dom) set_text_content(*pp, self->dom, pp->text(a[0]));
    return Value::Undefined();
  });
  // `form.submit()` sends the form without asking: no `submit` event, no
  // validation. The pane does the sending once the handler returns.
  def_method(P, hp, "submit", 0, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (self && self->dom && self->dom->tag == "form") { pp->submit_form = self->dom; pp->submit_by = nullptr; }
    return Value::Undefined();
  });
  // `requestSubmit()` is a submit button's click without the button: the
  // `submit` event fires, and the form goes unless a handler cancelled it.
  def_method(P, hp, "requestSubmit", 0, [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (self && self->dom) {
      Object* ev = make_event(*pp, "submit", "Event", true, true);
      dispatch_event_object(*pp, self->dom, self, ev);
      if (!event_default_prevented(ev) && self->dom->tag == "form") {
        pp->submit_form = self->dom;
        pp->submit_by = nullptr;
        if (a.argc && a[0].is_object() && a[0].as_object()->cls == js::Class::Host &&
            static_cast<HostObject*>(a[0].as_object())->host_kind == HostKind::Node)
          pp->submit_by = static_cast<NodeObject*>(a[0].as_object())->dom;
      }
    }
    return Value::Undefined();
  });
  def_method(P, hp, "reset", 0, [](Context&, CallArgs&) -> Value { return Value::Undefined(); });

  // <template>: its content is a fragment of its children
  def_accessor(P, hp, "content", [pp](Context&, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self) return Value::Exception();
    return Value::Obj(self);   // children of a template are already inert here; the element stands in for its fragment
  });

  // anchors and images
  def_accessor(P, hp, "origin", [pp](Context& c, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self || !self->dom) return Value::String(c.atoms.empty);
    const std::string abs = resolve(*pp, self->dom->attr("href"));
    const size_t sch = abs.find("://");
    if (sch == std::string::npos) return Value::String(c.atoms.empty);
    const size_t end = abs.find('/', sch + 3);
    return Value::String(c.new_string_utf8(abs.substr(0, end == std::string::npos ? abs.size() : end)));
  });
  for (const char* g : {"naturalWidth", "naturalHeight"})
    def_accessor(P, hp, g, [pp](Context&, CallArgs& a) -> Value {
      NodeObject* self = this_node(*pp, a);
      if (!self) return Value::Exception();
      return Value::Number(self->dom ? std::atoi(self->dom->attr("width").c_str()) : 0);
    });
  def_accessor(P, hp, "complete", [](Context&, CallArgs&) -> Value { return Value::Bool(true); });
  def_accessor(P, hp, "currentSrc", [pp](Context& c, CallArgs& a) -> Value {
    NodeObject* self = this_node(*pp, a);
    if (!self || !self->dom) return Value::String(c.atoms.empty);
    return Value::String(c.new_string_utf8(resolve(*pp, self->dom->attr("src"))));
  });
  for (const char* g : {"width", "height"})
    def_accessor(P, hp, g, [pp, g](Context&, CallArgs& a) -> Value {
      NodeObject* self = this_node(*pp, a);
      if (!self) return Value::Exception();
      if (!self->dom) return Value::Number(0);
      const std::string v = self->dom->attr(g);
      if (v.empty()) return Value::Number(self->dom->tag == "canvas" ? (std::string(g) == "width" ? 300 : 150) : 0);
      return Value::Number(std::atof(v.c_str()));
    }, [pp, g](Context&, CallArgs& a) -> Value {
      NodeObject* self = this_node(*pp, a);
      if (!self || !self->dom) return Value::Undefined();
      set_attr(*pp, self->dom, g, pp->ctx.number_to_string(std::trunc(pp->number(a[0])))->utf8());
      return Value::Undefined();
    });
}

}  // namespace

std::string resolve(Page& P, const std::string& url) {
  const std::string& base = !P.base.empty() ? P.base : (P.doc ? P.doc->base : std::string());
  return base.empty() ? url : web::resolve_url(base, url);
}

// ---------------------------------------------------------------------------------------------------
// the Document
// ---------------------------------------------------------------------------------------------------

namespace {

Node* document_body(Page& P) {
  Node* root = document_root(P);
  if (!root) return nullptr;
  if (Node* b = root->first("body")) return b;
  return nullptr;
}

std::string cookie_get(Page& P) { return P.cookie_jar; }

void cookie_set(Page& P, const std::string& line) {
  // "name=value; Path=/; Max-Age=..." : the first pair is the cookie; attributes other than expiry are not modelled
  const size_t semi = line.find(';');
  const std::string pair = line.substr(0, semi);
  const size_t eq = pair.find('=');
  std::string name = eq == std::string::npos ? std::string() : pair.substr(0, eq);
  std::string value = eq == std::string::npos ? pair : pair.substr(eq + 1);
  auto trim = [](std::string& s) { while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin()); while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back(); };
  trim(name);
  trim(value);
  bool expire = false;
  if (semi != std::string::npos) {
    const std::string attrs = lower(line.substr(semi));
    if (attrs.find("max-age=0") != std::string::npos || attrs.find("max-age=-") != std::string::npos || attrs.find("expires=thu, 01 jan 1970") != std::string::npos) expire = true;
  }
  std::vector<std::pair<std::string, std::string>> items;
  size_t at = 0;
  const std::string& jar = P.cookie_jar;
  while (at < jar.size()) {
    size_t sc = jar.find("; ", at);
    const std::string one = jar.substr(at, sc == std::string::npos ? std::string::npos : sc - at);
    const size_t e = one.find('=');
    items.emplace_back(e == std::string::npos ? std::string() : one.substr(0, e), e == std::string::npos ? one : one.substr(e + 1));
    if (sc == std::string::npos) break;
    at = sc + 2;
  }
  bool found = false;
  for (size_t i = 0; i < items.size(); ++i) {
    if (items[i].first != name) continue;
    found = true;
    if (expire) items.erase(items.begin() + static_cast<long>(i));
    else items[i].second = value;
    break;
  }
  if (!found && !expire) items.emplace_back(name, value);
  std::string out;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) out += "; ";
    out += items[i].first.empty() ? items[i].second : items[i].first + "=" + items[i].second;
  }
  P.cookie_jar = out;
}

NodeObject* make_element(Page& P, const std::string& tag) {
  auto n = std::make_unique<Node>();
  n->kind = Node::Kind::Element;
  n->tag = lower(tag);
  Node* raw = n.get();
  P.adopt_orphan(std::move(n));
  return P.wrap(raw);
}

void install_document_class(Page& P) {
  Page* pp = &P;
  Object* dp = P.document_proto;

  def_method(P, dp, "getElementById", 1, [pp](Context&, CallArgs& a) -> Value {
    return pp->node_value(find_by_id(document_root(*pp), arg_text(*pp, a, 0)));
  });
  def_method(P, dp, "querySelector", 1, [pp](Context&, CallArgs& a) -> Value { return select_in(*pp, document_root(*pp), arg_text(*pp, a, 0), true, false); });
  def_method(P, dp, "querySelectorAll", 1, [pp](Context&, CallArgs& a) -> Value { return select_in(*pp, document_root(*pp), arg_text(*pp, a, 0), true, true); });
  def_method(P, dp, "getElementsByTagName", 1, [pp](Context&, CallArgs& a) -> Value {
    std::vector<Node*> hits;
    by_tag(document_root(*pp), lower(arg_text(*pp, a, 0)), hits);
    return node_list(*pp, hits);
  });
  def_method(P, dp, "getElementsByClassName", 1, [pp](Context&, CallArgs& a) -> Value {
    std::vector<std::string> words;
    std::string t = arg_text(*pp, a, 0), w;
    for (char ch : t + " ") { if (std::isspace(static_cast<unsigned char>(ch))) { if (!w.empty()) words.push_back(w); w.clear(); } else w += ch; }
    std::vector<Node*> hits;
    by_class(document_root(*pp), words, hits);
    return node_list(*pp, hits);
  });
  def_method(P, dp, "getElementsByName", 1, [pp](Context&, CallArgs& a) -> Value {
    std::vector<Node*> all, hits;
    by_tag(document_root(*pp), "*", all);
    const std::string want = arg_text(*pp, a, 0);
    for (Node* n : all) if (n->attr("name") == want) hits.push_back(n);
    return node_list(*pp, hits);
  });
  def_method(P, dp, "createElement", 1, [pp](Context&, CallArgs& a) -> Value {
    return Value::Obj(make_element(*pp, arg_text(*pp, a, 0)));
  });
  def_method(P, dp, "createElementNS", 2, [pp](Context&, CallArgs& a) -> Value {
    return Value::Obj(make_element(*pp, arg_text(*pp, a, 1)));
  });
  def_method(P, dp, "createTextNode", 1, [pp](Context&, CallArgs& a) -> Value {
    auto n = std::make_unique<Node>();
    n->kind = Node::Kind::Text;
    n->text = arg_text(*pp, a, 0);
    Node* raw = n.get();
    pp->adopt_orphan(std::move(n));
    return Value::Obj(pp->wrap(raw));
  });
  def_method(P, dp, "createComment", 1, [pp](Context&, CallArgs&) -> Value {
    // comments are not kept in this tree: an empty text node stands in so insertion points still work
    auto n = std::make_unique<Node>();
    n->kind = Node::Kind::Text;
    Node* raw = n.get();
    pp->adopt_orphan(std::move(n));
    return Value::Obj(pp->wrap(raw));
  });
  def_method(P, dp, "createDocumentFragment", 0, [pp](Context&, CallArgs&) -> Value {
    auto n = std::make_unique<Node>();
    n->kind = Node::Kind::Element;
    n->tag = "#document-fragment";
    Node* raw = n.get();
    pp->adopt_orphan(std::move(n));
    return Value::Obj(pp->wrap(raw));
  });
  def_method(P, dp, "createEvent", 1, [pp](Context&, CallArgs& a) -> Value {
    std::string kind = arg_text(*pp, a, 0);
    const char* ctor = "Event";
    const std::string k = lower(kind);
    if (k == "customevent") ctor = "CustomEvent";
    else if (k == "keyboardevent") ctor = "KeyboardEvent";
    else if (k == "mouseevent" || k == "mouseevents") ctor = "MouseEvent";
    else if (k == "uievent" || k == "uievents") ctor = "UIEvent";
    return Value::Obj(make_event(*pp, "", ctor, false, false));
  });
  def_method(P, dp, "hasFocus", 0, [](Context&, CallArgs&) -> Value { return Value::Bool(true); });
  def_method(P, dp, "elementFromPoint", 2, [](Context&, CallArgs&) -> Value { return Value::Null(); });
  def_method(P, dp, "execCommand", 1, [](Context&, CallArgs&) -> Value { return Value::Bool(false); });
  def_method(P, dp, "write", 1, [pp](Context&, CallArgs& a) -> Value {
    Node* body = document_body(*pp);
    if (body) { std::string html; for (int i = 0; i < a.argc; ++i) html += pp->text(a.argv[i]); parse_fragment_into(*pp, body, html); }
    return Value::Undefined();
  });
  def_method(P, dp, "open", 0, [pp](Context&, CallArgs&) -> Value { return Value::Obj(pp->document_obj); });
  def_method(P, dp, "close", 0, [](Context&, CallArgs&) -> Value { return Value::Undefined(); });

  auto doc_node = [pp](const char* name, std::function<Node*(Node*)> f) {
    def_accessor(*pp, pp->document_proto, name, [pp, f](Context&, CallArgs&) -> Value {
      Node* root = document_root(*pp);
      return root ? pp->node_value(f(root)) : Value::Null();
    });
  };
  doc_node("body", [](Node* r) { return r->first("body"); });
  doc_node("head", [](Node* r) { return r->first("head"); });
  doc_node("documentElement", [](Node* r) { Node* h = r->first("html"); return h ? h : r; });
  doc_node("scrollingElement", [](Node* r) { Node* h = r->first("html"); return h ? h : r; });
  def_accessor(P, dp, "activeElement", [pp](Context&, CallArgs&) -> Value {
    if (pp->focused) return pp->node_value(pp->focused);
    Node* b = document_body(*pp);
    return b ? pp->node_value(b) : Value::Null();
  });
  for (const char* coll : {"forms", "images", "links", "scripts"}) {
    const std::string c = coll;
    def_accessor(P, dp, coll, [pp, c](Context&, CallArgs&) -> Value {
      std::vector<Node*> hits;
      const char* sel = c == "forms" ? "form" : c == "images" ? "img" : c == "links" ? "a[href], area[href]" : "script";
      query_all(document_root(*pp), sel, hits, true);
      return node_list(*pp, hits);
    });
  }
  def_accessor(P, dp, "styleSheets", [pp](Context&, CallArgs&) -> Value { return make_array(*pp, {}); });
  def_accessor(P, dp, "title", [pp](Context& c, CallArgs&) -> Value {
    std::string title = pp->doc ? pp->doc->title : std::string();
    if (title.empty() && pp->doc && pp->doc->dom)
      if (Node* t = pp->doc->dom->first("title")) {
        bool space = false;
        for (char ch : t->inner_text()) {
          if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') { space = true; continue; }
          if (space && !title.empty()) title += ' ';
          space = false;
          title += ch;
        }
      }
    return Value::String(c.new_string_utf8(title));
  }, [pp](Context&, CallArgs& a) -> Value {
    if (!pp->doc || !pp->doc->dom) return Value::Undefined();
    const std::string t = pp->text(a[0]);
    Node* title = pp->doc->dom->first("title");
    if (!title) {
      Node* head = pp->doc->dom->first("head");
      if (!head) head = pp->doc->dom.get();
      auto tn = std::make_unique<Node>();
      tn->kind = Node::Kind::Element;
      tn->tag = "title";
      tn->parent = head;
      title = tn.get();
      head->children.push_back(std::move(tn));
    }
    set_text_content(*pp, title, t);
    pp->doc->title = t;
    return Value::Undefined();
  });
  def_accessor(P, dp, "cookie", [pp](Context& c, CallArgs&) -> Value { return Value::String(c.new_string_utf8(cookie_get(*pp))); },
               [pp](Context&, CallArgs& a) -> Value { cookie_set(*pp, pp->text(a[0])); return Value::Undefined(); });
  auto doc_str = [pp](const char* name, std::function<std::string()> f) {
    def_accessor(*pp, pp->document_proto, name, [f](Context& c, CallArgs&) -> Value { return Value::String(c.new_string_utf8(f())); });
  };
  doc_str("URL", [pp]() { return pp->location_url; });
  doc_str("documentURI", [pp]() { return pp->location_url; });
  doc_str("baseURI", [pp]() { return pp->doc ? pp->doc->base : std::string(); });
  doc_str("readyState", [pp]() { return pp->ready_state; });
  doc_str("characterSet", []() { return std::string("UTF-8"); });
  doc_str("charset", []() { return std::string("UTF-8"); });
  doc_str("contentType", []() { return std::string("text/html"); });
  doc_str("compatMode", []() { return std::string("CSS1Compat"); });
  doc_str("visibilityState", []() { return std::string("visible"); });
  doc_str("referrer", []() { return std::string(); });
  doc_str("domain", [pp]() {
    const std::string u = pp->location_url;
    const size_t s = u.find("://");
    if (s == std::string::npos) return std::string();
    const size_t e = u.find_first_of("/:?#", s + 3);
    return u.substr(s + 3, e == std::string::npos ? std::string::npos : e - s - 3);
  });
  def_accessor(P, dp, "hidden", [](Context&, CallArgs&) -> Value { return Value::Bool(false); });
  def_accessor(P, dp, "currentScript", [pp](Context&, CallArgs&) -> Value { return pp->node_value(pp->current_script); });
  def_accessor(P, dp, "defaultView", [pp](Context&, CallArgs&) -> Value { return Value::Obj(pp->window_obj); });
  def_accessor(P, dp, "nodeType", [](Context&, CallArgs&) -> Value { return Value::Number(9); });
  def_accessor(P, dp, "nodeName", [](Context& c, CallArgs&) -> Value { return Value::String(c.atom_utf8("#document")); });
}

}  // namespace

// ---------------------------------------------------------------------------------------------------
// style declaration methods and the prototype chain
// ---------------------------------------------------------------------------------------------------

namespace {

void install_style_class(Page& P) {
  Page* pp = &P;
  Object* sp = P.style_proto;
  def_method(P, sp, "setProperty", 2, [pp](Context&, CallArgs& a) -> Value {
    StyleObject* s = this_style(*pp, a);
    if (!s) return Value::Exception();
    const std::string important = lower(a.argc > 2 ? pp->text(a[2]) : std::string());
    s->set_value(lower(arg_text(*pp, a, 0)) == arg_text(*pp, a, 0) ? arg_text(*pp, a, 0) : css_name(arg_text(*pp, a, 0)), a[1].is_nullish() ? std::string() : pp->text(a[1]), important == "important");
    return Value::Undefined();
  });
  def_method(P, sp, "getPropertyValue", 1, [pp](Context& c, CallArgs& a) -> Value {
    StyleObject* s = this_style(*pp, a);
    if (!s) return Value::Exception();
    return Value::String(c.new_string_utf8(s->get_value(arg_text(*pp, a, 0))));
  });
  def_method(P, sp, "getPropertyPriority", 1, [pp](Context& c, CallArgs& a) -> Value {
    StyleObject* s = this_style(*pp, a);
    if (!s) return Value::Exception();
    const std::string name = arg_text(*pp, a, 0);
    for (const Decl& d : s->decls()) if (d.name == name && d.important) return Value::String(c.atom_utf8("important"));
    return Value::String(c.atoms.empty);
  });
  def_method(P, sp, "removeProperty", 1, [pp](Context& c, CallArgs& a) -> Value {
    StyleObject* s = this_style(*pp, a);
    if (!s) return Value::Exception();
    const std::string name = arg_text(*pp, a, 0);
    const std::string old = s->get_value(name);
    s->set_value(name, "");
    return Value::String(c.new_string_utf8(old));
  });
  def_method(P, sp, "item", 1, [pp](Context& c, CallArgs& a) -> Value {
    StyleObject* s = this_style(*pp, a);
    if (!s) return Value::Exception();
    auto d = s->decls();
    double i = pp->number(a[0]);
    return i >= 0 && i < static_cast<double>(d.size()) ? Value::String(c.new_string_utf8(d[static_cast<size_t>(i)].name)) : Value::String(c.atoms.empty);
  });
  def_accessor(P, sp, "length", [pp](Context&, CallArgs& a) -> Value {
    StyleObject* s = this_style(*pp, a);
    if (!s) return Value::Exception();
    return Value::Number(static_cast<double>(s->decls().size()));
  });
  def_accessor(P, sp, "cssText", [pp](Context& c, CallArgs& a) -> Value {
    StyleObject* s = this_style(*pp, a);
    if (!s) return Value::Exception();
    return Value::String(c.new_string_utf8(serialize_style(s->decls())));
  }, [pp](Context&, CallArgs& a) -> Value {
    StyleObject* s = this_style(*pp, a);
    if (!s) return Value::Exception();
    s->store(parse_style(pp->text(a[0])));
    return Value::Undefined();
  });
}

}  // namespace

void install_dom(Page& P) {
  Context& ctx = P.ctx;
  // the chain: EventTarget > Node > Element > HTMLElement, and Text, Document beside Element
  P.event_target_proto = make_proto(P, ctx.intr.object_proto, "EventTarget");
  P.node_proto = make_proto(P, P.event_target_proto, "Node");
  P.element_proto = make_proto(P, P.node_proto, "Element");
  P.html_element_proto = make_proto(P, P.element_proto, "HTMLElement");
  P.text_proto = make_proto(P, P.node_proto, "Text");
  P.document_proto = make_proto(P, P.node_proto, "Document");
  P.classlist_proto = make_proto(P, ctx.intr.object_proto, "DOMTokenList");
  P.style_proto = make_proto(P, ctx.intr.object_proto, "CSSStyleDeclaration");
  P.node_list_proto = make_proto(P, ctx.intr.array_proto, "NodeList");
  def_method(P, P.node_list_proto, "item", 1, [pp = &P](Context& c, CallArgs& a) -> Value {
    if (!a.this_value.is_object()) return Value::Null();
    Value v;
    double i = pp->number(a[0]);
    if (i < 0 || !c.get_index(a.this_value, static_cast<uint32_t>(i), &v)) { c.clear_exception(); return Value::Null(); }
    return v.is_undefined() ? Value::Null() : v;
  });
  // tag-specific prototypes (so `input instanceof HTMLInputElement` and per-tag brand checks work)
  struct Tag { const char* ctor; const char* tags; };
  const Tag tags[] = {{"HTMLInputElement", "input"}, {"HTMLTextAreaElement", "textarea"}, {"HTMLSelectElement", "select"}, {"HTMLOptionElement", "option"},
                      {"HTMLButtonElement", "button"}, {"HTMLFormElement", "form"}, {"HTMLAnchorElement", "a"}, {"HTMLImageElement", "img"},
                      {"HTMLCanvasElement", "canvas"}, {"HTMLScriptElement", "script"}, {"HTMLStyleElement", "style"}, {"HTMLLinkElement", "link"},
                      {"HTMLDivElement", "div"}, {"HTMLSpanElement", "span"}, {"HTMLParagraphElement", "p"}, {"HTMLBodyElement", "body"},
                      {"HTMLHtmlElement", "html"}, {"HTMLHeadElement", "head"}, {"HTMLTableElement", "table"}, {"HTMLUListElement", "ul"},
                      {"HTMLLIElement", "li"}, {"HTMLLabelElement", "label"}, {"HTMLIFrameElement", "iframe"}, {"HTMLTemplateElement", "template"},
                      {"HTMLAudioElement", "audio"}, {"HTMLVideoElement", "video"}, {"HTMLDialogElement", "dialog"}, {"HTMLDetailsElement", "details"}, {"HTMLSlotElement", "slot"},
                      {"HTMLPreElement", "pre"}, {"HTMLOListElement", "ol"}, {"HTMLBRElement", "br"}, {"HTMLHRElement", "hr"}, {"HTMLMetaElement", "meta"}, {"HTMLTitleElement", "title"},
                      {"HTMLTableRowElement", "tr"}, {"HTMLFieldSetElement", "fieldset"}, {"HTMLProgressElement", "progress"}, {"HTMLSourceElement", "source"}, {"HTMLPictureElement", "picture"}};
  for (const Tag& t : tags) {
    Object* p = make_proto(P, P.html_element_proto, t.ctor);
    P.tag_protos[t.tags] = p;
    def_ctor(P, t.ctor, 0, p, [](Context& c, CallArgs&) -> Value {
      c.throw_type_error("Illegal constructor");
      return Value::Exception();
    });
  }
  auto illegal = [](Context& c, CallArgs&) -> Value {
    c.throw_type_error("Illegal constructor");
    return Value::Exception();
  };
  def_ctor(P, "Node", 0, P.node_proto, illegal);
  def_ctor(P, "Element", 0, P.element_proto, illegal);
  def_ctor(P, "HTMLElement", 0, P.html_element_proto, illegal);
  def_ctor(P, "Text", 1, P.text_proto, [pp = &P](Context&, CallArgs& a) -> Value {
    auto n = std::make_unique<Node>();
    n->kind = Node::Kind::Text;
    n->text = a.argc ? pp->text(a[0]) : std::string();
    Node* raw = n.get();
    pp->adopt_orphan(std::move(n));
    return Value::Obj(pp->wrap(raw));
  });
  def_ctor(P, "Document", 0, P.document_proto, illegal);
  def_ctor(P, "DOMTokenList", 0, P.classlist_proto, illegal);
  def_ctor(P, "CSSStyleDeclaration", 0, P.style_proto, illegal);
  def_ctor(P, "NodeList", 0, P.node_list_proto, illegal);
  {
    Value node_list_ctor;
    ctx.get(Value::Obj(ctx.global()), ctx.atom_ascii("NodeList"), &node_list_ctor);
    ctx.define_data(ctx.global(), ctx.atom_ascii("HTMLCollection"), node_list_ctor, js::kHiddenAttrs);
  }
  install_node_class(P);
  install_element_class(P);
  // Text nodes are CharacterData and ChildNode: the same members that work on any node
  for (const char* n : {"remove", "before", "after", "replaceWith"}) {
    Value m;
    ctx.get(Value::Obj(P.element_proto), ctx.atom_utf8(n), &m);
    ctx.define_data(P.text_proto, ctx.atom_utf8(n), m, js::kHiddenAttrs | js::kEnumerable);
  }
  {
    Page* pp = &P;
    Object* tp = P.text_proto;
    def_accessor(P, tp, "data", [pp](Context& c, CallArgs& a) -> Value {
      NodeObject* self = this_node(*pp, a);
      if (!self) return Value::Exception();
      return Value::String(c.new_string_utf8(self->dom ? self->dom->text : std::string()));
    }, [pp](Context&, CallArgs& a) -> Value {
      NodeObject* self = this_node(*pp, a);
      if (!self) return Value::Exception();
      if (self->dom) set_text_content(*pp, self->dom, pp->text(a[0]));
      return Value::Undefined();
    });
    def_accessor(P, tp, "wholeText", [pp](Context& c, CallArgs& a) -> Value {
      NodeObject* self = this_node(*pp, a);
      if (!self) return Value::Exception();
      return Value::String(c.new_string_utf8(self->dom ? self->dom->text : std::string()));
    });
    def_accessor(P, tp, "length", [pp](Context&, CallArgs& a) -> Value {
      NodeObject* self = this_node(*pp, a);
      if (!self) return Value::Exception();
      // UTF-16 code units, as the DOM counts them
      double n = 0;
      if (self->dom)
        for (size_t i = 0; i < self->dom->text.size(); ++i) {
          const unsigned char ch = static_cast<unsigned char>(self->dom->text[i]);
          if ((ch & 0xC0) != 0x80) n += ch >= 0xF0 ? 2 : 1;
        }
      return Value::Number(n);
    });
    def_accessor(P, tp, "nextElementSibling", [pp](Context&, CallArgs& a) -> Value {
      NodeObject* self = this_node(*pp, a);
      return self && self->dom ? pp->node_value(sibling(self->dom, 1, true)) : Value::Null();
    });
    def_accessor(P, tp, "previousElementSibling", [pp](Context&, CallArgs& a) -> Value {
      NodeObject* self = this_node(*pp, a);
      return self && self->dom ? pp->node_value(sibling(self->dom, -1, true)) : Value::Null();
    });
  }
  install_html_element_class(P);
  install_class_list(P);
  install_style_class(P);
  install_document_class(P);
}

}  // namespace aios::userland::webjs
