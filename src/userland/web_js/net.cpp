// The network and data surface of a page: fetch, XMLHttpRequest, URL, URLSearchParams, Headers, Request, Response,
// Blob, File, FormData, AbortController, and Web Storage.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "aios/core/base64.hpp"
#include "page.hpp"

namespace aios::userland::webjs {

using js::PropKey;
using js::Tracer;

namespace {

// ---------------------------------------------------------------------------------------------------
// text helpers
// ---------------------------------------------------------------------------------------------------

std::string lower(std::string s) {
  for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  return s;
}

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

std::string pct_encode(const std::string& s, bool form) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '*' || c == '-' || c == '.' || c == '_') out += static_cast<char>(c);
    else if (form && c == ' ') out += '+';
    else if (!form && (c == '~' || c == '!' || c == '\'' || c == '(' || c == ')')) out += static_cast<char>(c);
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string pct_decode(const std::string& s, bool form) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() + 0 && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) {
      out += static_cast<char>(hexv(s[i + 1]) * 16 + hexv(s[i + 2]));
      i += 2;
    } else if (form && s[i] == '+') {
      out += ' ';
    } else {
      out += s[i];
    }
  }
  return out;
}

Value str_val(Page& P, const std::string& s) { return Value::String(P.ctx.new_string_utf8(s)); }

Value promise_static(Page& P, const char* fn, const Value& v) {
  Value ctor, f, r;
  if (!P.ctx.get(Value::Obj(P.ctx.global()), P.ctx.atom_ascii("Promise"), &ctor) || !P.ctx.get(ctor, P.ctx.atom_utf8(fn), &f) || !P.ctx.call(f, ctor, {v}, &r)) {
    P.ctx.clear_exception();
    return Value::Undefined();
  }
  return r;
}

Value make_dom_exception(Page& P, const char* name, const std::string& msg) {
  Object* e = P.ctx.new_error(js::ErrorKind::Error, msg);
  P.ctx.define_data(e, P.ctx.atoms.name, Value::String(P.ctx.atom_utf8(name)), js::kHiddenAttrs);
  return Value::Obj(e);
}

Value type_error_value(Page& P, const std::string& msg) { return Value::Obj(P.ctx.new_error(js::ErrorKind::TypeError, msg)); }

}  // namespace

Value resolved_promise(Page& P, const Value& v) { return promise_static(P, "resolve", v); }
Value rejected_promise(Page& P, const Value& v) { return promise_static(P, "reject", v); }

namespace {

// ---------------------------------------------------------------------------------------------------
// Headers
// ---------------------------------------------------------------------------------------------------

struct HeadersObject final : HostObject {
  HeadersObject() : HostObject(HostKind::Headers) {}
  std::vector<std::pair<std::string, std::string>> items;   // names lower-cased

  std::string get(const std::string& name) const {
    std::string out;
    bool any = false;
    for (const auto& kv : items)
      if (kv.first == name) { out += (any ? ", " : "") + kv.second; any = true; }
    return out;
  }
  bool has(const std::string& name) const {
    for (const auto& kv : items) if (kv.first == name) return true;
    return false;
  }
  void set(const std::string& name, const std::string& value) {
    bool placed = false;
    for (size_t i = 0; i < items.size(); ++i) {
      if (items[i].first != name) continue;
      if (!placed) { items[i].second = value; placed = true; }
      else { items.erase(items.begin() + static_cast<long>(i)); --i; }
    }
    if (!placed) items.emplace_back(name, value);
  }
  void append(const std::string& name, const std::string& value) { items.emplace_back(name, value); }
};

HeadersObject* as_headers(const Value& v) {
  if (v.is_object() && v.as_object()->cls == js::Class::Host && static_cast<HostObject*>(v.as_object())->host_kind == HostKind::Headers) return static_cast<HeadersObject*>(v.as_object());
  return nullptr;
}

HeadersObject* this_headers(Page& P, CallArgs& a) { return static_cast<HeadersObject*>(this_host(P, a, HostKind::Headers, "Headers")); }

Object* g_headers_proto = nullptr;

HeadersObject* new_headers(Page& P) {
  HeadersObject* h = P.ctx.heap.make<HeadersObject>();
  h->proto = g_headers_proto;
  return h;
}

// Fill from a Headers, a plain object, or an array of pairs.
bool fill_headers(Page& P, HeadersObject* h, const Value& init) {
  if (init.is_nullish()) return true;
  if (HeadersObject* src = as_headers(init)) {
    for (const auto& kv : src->items) h->append(kv.first, kv.second);
    return true;
  }
  if (!init.is_object()) return true;
  Value iter;
  if (!P.ctx.get_method(init, P.ctx.sym.iterator, &iter)) return false;
  if (!iter.is_undefined()) {
    return P.ctx.iterate(init, [&](Value pair) {
      std::vector<Value> kv;
      if (!P.ctx.iterable_to_list(pair, &kv) || kv.size() != 2) {
        if (!P.ctx.has_exception()) P.ctx.throw_type_error("Failed to construct 'Headers': Invalid value");
        return false;
      }
      h->append(lower(trim(P.text(kv[0]))), trim(P.text(kv[1])));
      return true;
    });
  }
  std::vector<js::PropKey> keys;
  Object* o = init.as_object();
  if (!o->own_keys(P.ctx, &keys)) return false;
  for (PropKey k : keys) {
    if (k->kind != js::CellKind::String) continue;
    js::PropDesc d;
    bool found;
    if (!o->get_own_property(P.ctx, k, &d, &found) || !found || !d.enumerable) continue;
    Value v;
    if (!o->get(P.ctx, k, init, &v)) return false;
    h->append(lower(trim(static_cast<js::JsString*>(k)->utf8())), trim(P.text(v)));
  }
  return true;
}

void install_headers(Page& P) {
  Page* pp = &P;
  g_headers_proto = make_proto(P, P.ctx.intr.object_proto, "Headers");
  Object* hp = g_headers_proto;
  def_ctor(P, "Headers", 0, hp, [pp](Context&, CallArgs& a) -> Value {
    if (!a.is_construct()) {
      pp->ctx.throw_type_error("Failed to construct 'Headers': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
      return Value::Exception();
    }
    HeadersObject* h = new_headers(*pp);
    if (!fill_headers(*pp, h, a[0])) return Value::Exception();
    return Value::Obj(h);
  });
  def_method(P, hp, "get", 1, [pp](Context&, CallArgs& a) -> Value {
    HeadersObject* h = this_headers(*pp, a);
    if (!h) return Value::Exception();
    const std::string name = lower(arg_text(*pp, a, 0));
    return h->has(name) ? str_val(*pp, h->get(name)) : Value::Null();
  });
  def_method(P, hp, "has", 1, [pp](Context&, CallArgs& a) -> Value {
    HeadersObject* h = this_headers(*pp, a);
    if (!h) return Value::Exception();
    return Value::Bool(h->has(lower(arg_text(*pp, a, 0))));
  });
  def_method(P, hp, "set", 2, [pp](Context&, CallArgs& a) -> Value {
    HeadersObject* h = this_headers(*pp, a);
    if (!h) return Value::Exception();
    h->set(lower(arg_text(*pp, a, 0)), trim(arg_text(*pp, a, 1)));
    return Value::Undefined();
  });
  def_method(P, hp, "append", 2, [pp](Context&, CallArgs& a) -> Value {
    HeadersObject* h = this_headers(*pp, a);
    if (!h) return Value::Exception();
    h->append(lower(arg_text(*pp, a, 0)), trim(arg_text(*pp, a, 1)));
    return Value::Undefined();
  });
  def_method(P, hp, "delete", 1, [pp](Context&, CallArgs& a) -> Value {
    HeadersObject* h = this_headers(*pp, a);
    if (!h) return Value::Exception();
    const std::string name = lower(arg_text(*pp, a, 0));
    h->items.erase(std::remove_if(h->items.begin(), h->items.end(), [&](const auto& kv) { return kv.first == name; }), h->items.end());
    return Value::Undefined();
  });
  def_method(P, hp, "getSetCookie", 0, [pp](Context&, CallArgs& a) -> Value {
    HeadersObject* h = this_headers(*pp, a);
    if (!h) return Value::Exception();
    std::vector<Value> out;
    for (const auto& kv : h->items) if (kv.first == "set-cookie") out.push_back(str_val(*pp, kv.second));
    return make_array(*pp, out);
  });
  // iteration: sorted by name, duplicates combined, as the spec has it
  auto sorted = [pp](HeadersObject* h) {
    std::vector<std::pair<std::string, std::string>> out;
    std::vector<std::string> names;
    for (const auto& kv : h->items) if (std::find(names.begin(), names.end(), kv.first) == names.end()) names.push_back(kv.first);
    std::sort(names.begin(), names.end());
    for (const auto& n : names) out.emplace_back(n, h->get(n));
    return out;
  };
  auto iter = [pp, sorted](CallArgs& a, int kind) -> Value {
    HeadersObject* h = this_headers(*pp, a);
    if (!h) return Value::Exception();
    std::vector<Value> items;
    for (const auto& kv : sorted(h)) {
      if (kind == 0) items.push_back(str_val(*pp, kv.first));
      else if (kind == 1) items.push_back(str_val(*pp, kv.second));
      else items.push_back(make_array(*pp, {str_val(*pp, kv.first), str_val(*pp, kv.second)}));
    }
    Value arr = make_array(*pp, items), fn, r;
    if (!pp->ctx.get(arr, pp->ctx.atom_ascii("values"), &fn) || !pp->ctx.call(fn, arr, nullptr, 0, &r)) return Value::Exception();
    return r;
  };
  def_method(P, hp, "keys", 0, [iter](Context&, CallArgs& a) -> Value { return iter(a, 0); });
  def_method(P, hp, "values", 0, [iter](Context&, CallArgs& a) -> Value { return iter(a, 1); });
  Function* entries = def_method(P, hp, "entries", 0, [iter](Context&, CallArgs& a) -> Value { return iter(a, 2); });
  P.ctx.define_data(hp, P.ctx.sym.iterator, Value::Obj(entries), js::kHiddenAttrs);
  def_method(P, hp, "forEach", 1, [pp, sorted](Context& c, CallArgs& a) -> Value {
    HeadersObject* h = this_headers(*pp, a);
    if (!h) return Value::Exception();
    for (const auto& kv : sorted(h)) {
      Value ignored;
      if (!c.call(a[0], a[1], {str_val(*pp, kv.second), str_val(*pp, kv.first), a.this_value}, &ignored)) return Value::Exception();
    }
    return Value::Undefined();
  });
}

// ---------------------------------------------------------------------------------------------------
// Blob, File, FormData
// ---------------------------------------------------------------------------------------------------

struct BlobObject final : HostObject {
  BlobObject() : HostObject(HostKind::Blob) {}
  std::string data, type;
  std::string name;            // a File
  double last_modified = 0;
  bool is_file = false;
};

BlobObject* as_blob(const Value& v) {
  if (v.is_object() && v.as_object()->cls == js::Class::Host && static_cast<HostObject*>(v.as_object())->host_kind == HostKind::Blob) return static_cast<BlobObject*>(v.as_object());
  return nullptr;
}

Object* g_blob_proto = nullptr;
Object* g_file_proto = nullptr;
std::map<std::string, std::string> g_blob_urls;      // "blob:..." -> data, per process (a page's object URLs)
std::map<std::string, std::string> g_blob_types;
int g_blob_counter = 0;

BlobObject* new_blob(Page& P, const std::string& data, const std::string& type) {
  BlobObject* b = P.ctx.heap.make<BlobObject>();
  b->proto = g_blob_proto;
  b->data = data;
  b->type = type;
  return b;
}

// The bytes of one part of a Blob: a string, another Blob, or binary data.
bool part_bytes(Page& P, const Value& v, std::string* out) {
  if (BlobObject* b = as_blob(v)) { *out += b->data; return true; }
  std::string bytes;
  if (P.ctx.bytes_of(v, &bytes)) { *out += bytes; return true; }
  js::JsString* s;
  if (!P.ctx.to_string(v, &s)) return false;
  *out += s->utf8();
  return true;
}

bool blob_parts(Page& P, const Value& parts, std::string* out) {
  if (parts.is_undefined()) return true;
  if (!parts.is_object()) {
    P.ctx.throw_type_error("Failed to construct 'Blob': The provided value cannot be converted to a sequence.");
    return false;
  }
  return P.ctx.iterate(parts, [&](Value v) { return part_bytes(P, v, out); });
}

BlobObject* this_blob(Page& P, CallArgs& a) { return static_cast<BlobObject*>(this_host(P, a, HostKind::Blob, "Blob")); }

Value blob_arraybuffer_value(Page& P, const std::string& data) {
  return Value::Obj(P.ctx.new_array_buffer(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
}

void install_blob(Page& P) {
  Page* pp = &P;
  g_blob_proto = make_proto(P, P.ctx.intr.object_proto, "Blob");
  g_file_proto = make_proto(P, g_blob_proto, "File");
  Object* bp = g_blob_proto;
  def_ctor(P, "Blob", 0, bp, [pp](Context&, CallArgs& a) -> Value {
    if (!a.is_construct()) {
      pp->ctx.throw_type_error("Failed to construct 'Blob': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
      return Value::Exception();
    }
    std::string data;
    if (!blob_parts(*pp, a[0], &data)) return Value::Exception();
    std::string type;
    if (a[1].is_object()) {
      Value t;
      if (!pp->ctx.get(a[1], pp->ctx.atom_ascii("type"), &t)) return Value::Exception();
      if (!t.is_undefined()) type = lower(pp->text(t));
    }
    return Value::Obj(new_blob(*pp, data, type));
  });
  def_ctor(P, "File", 2, g_file_proto, [pp](Context&, CallArgs& a) -> Value {
    if (!a.is_construct() || a.argc < 2) {
      pp->ctx.throw_type_error("Failed to construct 'File': 2 arguments required.");
      return Value::Exception();
    }
    std::string data;
    if (!blob_parts(*pp, a[0], &data)) return Value::Exception();
    BlobObject* b = new_blob(*pp, data, "");
    b->proto = g_file_proto;
    b->is_file = true;
    b->name = pp->text(a[1]);
    b->last_modified = pp->now_ms;
    if (a[2].is_object()) {
      Value t, lm;
      pp->ctx.get(a[2], pp->ctx.atom_ascii("type"), &t);
      pp->ctx.get(a[2], pp->ctx.atom_ascii("lastModified"), &lm);
      if (!t.is_undefined()) b->type = lower(pp->text(t));
      if (lm.is_number()) b->last_modified = lm.num;
    }
    return Value::Obj(b);
  });
  def_accessor(P, bp, "size", [pp](Context&, CallArgs& a) -> Value {
    BlobObject* b = this_blob(*pp, a);
    if (!b) return Value::Exception();
    return Value::Number(static_cast<double>(b->data.size()));
  });
  def_accessor(P, bp, "type", [pp](Context&, CallArgs& a) -> Value {
    BlobObject* b = this_blob(*pp, a);
    if (!b) return Value::Exception();
    return str_val(*pp, b->type);
  });
  def_accessor(P, g_file_proto, "name", [pp](Context&, CallArgs& a) -> Value {
    BlobObject* b = this_blob(*pp, a);
    if (!b) return Value::Exception();
    return str_val(*pp, b->name);
  });
  def_accessor(P, g_file_proto, "lastModified", [pp](Context&, CallArgs& a) -> Value {
    BlobObject* b = this_blob(*pp, a);
    if (!b) return Value::Exception();
    return Value::Number(b->last_modified);
  });
  def_method(P, bp, "text", 0, [pp](Context&, CallArgs& a) -> Value {
    BlobObject* b = this_blob(*pp, a);
    if (!b) return Value::Exception();
    return resolved_promise(*pp, str_val(*pp, b->data));
  });
  def_method(P, bp, "arrayBuffer", 0, [pp](Context&, CallArgs& a) -> Value {
    BlobObject* b = this_blob(*pp, a);
    if (!b) return Value::Exception();
    return resolved_promise(*pp, blob_arraybuffer_value(*pp, b->data));
  });
  def_method(P, bp, "bytes", 0, [pp](Context&, CallArgs& a) -> Value {
    BlobObject* b = this_blob(*pp, a);
    if (!b) return Value::Exception();
    return resolved_promise(*pp, Value::Obj(pp->ctx.new_uint8_array(reinterpret_cast<const uint8_t*>(b->data.data()), b->data.size())));
  });
  def_method(P, bp, "slice", 0, [pp](Context&, CallArgs& a) -> Value {
    BlobObject* b = this_blob(*pp, a);
    if (!b) return Value::Exception();
    const double len = static_cast<double>(b->data.size());
    double s = a[0].is_undefined() ? 0 : pp->number(a[0]), e = a[1].is_undefined() ? len : pp->number(a[1]);
    if (s < 0) s = std::max(0.0, len + s);
    if (e < 0) e = std::max(0.0, len + e);
    s = std::min(s, len);
    e = std::min(e, len);
    return Value::Obj(new_blob(*pp, e > s ? b->data.substr(static_cast<size_t>(s), static_cast<size_t>(e - s)) : std::string(), a[2].is_undefined() ? b->type : lower(pp->text(a[2]))));
  });
}

struct FormDataObject final : HostObject {
  FormDataObject() : HostObject(HostKind::FormData) {}
  std::vector<std::pair<std::string, Value>> items;
  void trace(Tracer& t) override {
    Object::trace(t);
    for (auto& kv : items) t.mark(kv.second);
  }
};

Object* g_formdata_proto = nullptr;

FormDataObject* this_form_data(Page& P, CallArgs& a) { return static_cast<FormDataObject*>(this_host(P, a, HostKind::FormData, "FormData")); }

// A form value: strings stay strings, Blobs become Files (with the given file name).
Value form_value(Page& P, const Value& v, const Value& filename) {
  if (BlobObject* b = as_blob(v)) {
    if (b->is_file && filename.is_undefined()) return v;
    BlobObject* f = new_blob(P, b->data, b->type);
    f->proto = g_file_proto;
    f->is_file = true;
    f->name = filename.is_undefined() ? "blob" : P.text(filename);
    f->last_modified = P.now_ms;
    return Value::Obj(f);
  }
  return str_val(P, P.text(v));
}

void collect_form_fields(Page& P, Node* form, FormDataObject* fd) {
  std::vector<Node*> fields;
  query_all(form, "input, select, textarea", fields, false);
  for (Node* n : fields) {
    const std::string name = n->attr("name");
    if (name.empty() || n->has_attr("disabled")) continue;
    if (n->tag == "input") {
      const std::string type = lower(n->attr("type"));
      if ((type == "checkbox" || type == "radio") && !n->has_attr("checked")) continue;
      if (type == "submit" || type == "button" || type == "reset" || type == "image" || type == "file") continue;
      fd->items.emplace_back(name, str_val(P, n->has_attr("value") ? n->attr("value") : (type == "checkbox" || type == "radio" ? std::string("on") : std::string())));
    } else if (n->tag == "textarea") {
      fd->items.emplace_back(name, str_val(P, n->has_attr("value") ? n->attr("value") : n->inner_text()));
    } else {
      std::vector<Node*> opts;
      query_all(n, "option", opts, false);
      Node* sel = nullptr;
      for (Node* o : opts) if (o->has_attr("selected")) { sel = o; break; }
      if (!sel && !opts.empty()) sel = opts.front();
      if (sel) fd->items.emplace_back(name, str_val(P, sel->has_attr("value") ? sel->attr("value") : sel->inner_text()));
    }
  }
}

void install_form_data(Page& P) {
  Page* pp = &P;
  g_formdata_proto = make_proto(P, P.ctx.intr.object_proto, "FormData");
  Object* fp = g_formdata_proto;
  def_ctor(P, "FormData", 0, fp, [pp](Context&, CallArgs& a) -> Value {
    if (!a.is_construct()) {
      pp->ctx.throw_type_error("Failed to construct 'FormData': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
      return Value::Exception();
    }
    FormDataObject* fd = pp->ctx.heap.make<FormDataObject>();
    fd->proto = g_formdata_proto;
    if (a[0].is_object() && a[0].as_object()->cls == js::Class::Host && static_cast<HostObject*>(a[0].as_object())->host_kind == HostKind::Node) {
      NodeObject* n = static_cast<NodeObject*>(a[0].as_object());
      if (n->dom) collect_form_fields(*pp, n->dom, fd);
    }
    return Value::Obj(fd);
  });
  def_method(P, fp, "append", 2, [pp](Context&, CallArgs& a) -> Value {
    FormDataObject* f = this_form_data(*pp, a);
    if (!f) return Value::Exception();
    f->items.emplace_back(arg_text(*pp, a, 0), form_value(*pp, a[1], a[2]));
    return Value::Undefined();
  });
  def_method(P, fp, "set", 2, [pp](Context&, CallArgs& a) -> Value {
    FormDataObject* f = this_form_data(*pp, a);
    if (!f) return Value::Exception();
    const std::string name = arg_text(*pp, a, 0);
    Value v = form_value(*pp, a[1], a[2]);
    bool placed = false;
    for (size_t i = 0; i < f->items.size(); ++i) {
      if (f->items[i].first != name) continue;
      if (!placed) { f->items[i].second = v; placed = true; }
      else { f->items.erase(f->items.begin() + static_cast<long>(i)); --i; }
    }
    if (!placed) f->items.emplace_back(name, v);
    return Value::Undefined();
  });
  def_method(P, fp, "get", 1, [pp](Context&, CallArgs& a) -> Value {
    FormDataObject* f = this_form_data(*pp, a);
    if (!f) return Value::Exception();
    const std::string name = arg_text(*pp, a, 0);
    for (auto& kv : f->items) if (kv.first == name) return kv.second;
    return Value::Null();
  });
  def_method(P, fp, "getAll", 1, [pp](Context&, CallArgs& a) -> Value {
    FormDataObject* f = this_form_data(*pp, a);
    if (!f) return Value::Exception();
    const std::string name = arg_text(*pp, a, 0);
    std::vector<Value> out;
    for (auto& kv : f->items) if (kv.first == name) out.push_back(kv.second);
    return make_array(*pp, out);
  });
  def_method(P, fp, "has", 1, [pp](Context&, CallArgs& a) -> Value {
    FormDataObject* f = this_form_data(*pp, a);
    if (!f) return Value::Exception();
    const std::string name = arg_text(*pp, a, 0);
    for (auto& kv : f->items) if (kv.first == name) return Value::Bool(true);
    return Value::Bool(false);
  });
  def_method(P, fp, "delete", 1, [pp](Context&, CallArgs& a) -> Value {
    FormDataObject* f = this_form_data(*pp, a);
    if (!f) return Value::Exception();
    const std::string name = arg_text(*pp, a, 0);
    f->items.erase(std::remove_if(f->items.begin(), f->items.end(), [&](const auto& kv) { return kv.first == name; }), f->items.end());
    return Value::Undefined();
  });
  auto iter = [pp](CallArgs& a, int kind) -> Value {
    FormDataObject* f = this_form_data(*pp, a);
    if (!f) return Value::Exception();
    std::vector<Value> items;
    for (auto& kv : f->items) {
      if (kind == 0) items.push_back(str_val(*pp, kv.first));
      else if (kind == 1) items.push_back(kv.second);
      else items.push_back(make_array(*pp, {str_val(*pp, kv.first), kv.second}));
    }
    Value arr = make_array(*pp, items), fn, r;
    if (!pp->ctx.get(arr, pp->ctx.atom_ascii("values"), &fn) || !pp->ctx.call(fn, arr, nullptr, 0, &r)) return Value::Exception();
    return r;
  };
  def_method(P, fp, "keys", 0, [iter](Context&, CallArgs& a) -> Value { return iter(a, 0); });
  def_method(P, fp, "values", 0, [iter](Context&, CallArgs& a) -> Value { return iter(a, 1); });
  Function* entries = def_method(P, fp, "entries", 0, [iter](Context&, CallArgs& a) -> Value { return iter(a, 2); });
  P.ctx.define_data(fp, P.ctx.sym.iterator, Value::Obj(entries), js::kHiddenAttrs);
  def_method(P, fp, "forEach", 1, [pp](Context& c, CallArgs& a) -> Value {
    FormDataObject* f = this_form_data(*pp, a);
    if (!f) return Value::Exception();
    auto items = f->items;
    for (auto& kv : items) {
      Value ignored;
      if (!c.call(a[0], a[1], {kv.second, str_val(*pp, kv.first), a.this_value}, &ignored)) return Value::Exception();
    }
    return Value::Undefined();
  });
}

// ---------------------------------------------------------------------------------------------------
// URL and URLSearchParams
// ---------------------------------------------------------------------------------------------------

struct UrlObject;

struct SearchParamsObject final : HostObject {
  SearchParamsObject() : HostObject(HostKind::SearchParams) {}
  std::vector<std::pair<std::string, std::string>> items;
  UrlObject* owner = nullptr;
  void trace(Tracer& t) override;
};

struct Parts {
  std::string scheme, user, pass, host, port, path, query, fragment;
  bool has_authority = false;
  bool has_query = false, has_fragment = false;
};

bool is_special(const std::string& scheme) {
  return scheme == "http" || scheme == "https" || scheme == "ws" || scheme == "wss" || scheme == "ftp" || scheme == "file";
}

std::string default_port(const std::string& scheme) {
  if (scheme == "http" || scheme == "ws") return "80";
  if (scheme == "https" || scheme == "wss") return "443";
  if (scheme == "ftp") return "21";
  return "";
}

std::string normalize_path(const std::string& path) {
  std::vector<std::string> segs;
  size_t i = 0;
  const bool abs = !path.empty() && path[0] == '/';
  while (i <= path.size()) {
    size_t j = path.find('/', i);
    if (j == std::string::npos) j = path.size();
    std::string seg = path.substr(i, j - i);
    const bool last = j == path.size();
    if (i == 0 && abs) { i = 1; continue; }
    if (seg == "..") { if (!segs.empty()) segs.pop_back(); if (last) segs.push_back(""); }
    else if (seg == ".") { if (last) segs.push_back(""); }
    else segs.push_back(seg);
    i = j + 1;
  }
  std::string out = abs ? "/" : "";
  for (size_t k = 0; k < segs.size(); ++k) out += (k ? "/" : "") + segs[k];
  return out;
}

bool parse_parts(const std::string& in, Parts* p) {
  std::string s = trim(in);
  const size_t colon = s.find(':');
  if (colon == std::string::npos || colon == 0 || !std::isalpha(static_cast<unsigned char>(s[0]))) return false;
  for (size_t i = 0; i < colon; ++i) {
    char c = s[i];
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.')) return false;
  }
  p->scheme = lower(s.substr(0, colon));
  std::string rest = s.substr(colon + 1);
  const size_t hash = rest.find('#');
  if (hash != std::string::npos) { p->fragment = rest.substr(hash + 1); p->has_fragment = true; rest = rest.substr(0, hash); }
  const size_t q = rest.find('?');
  if (q != std::string::npos) { p->query = rest.substr(q + 1); p->has_query = true; rest = rest.substr(0, q); }
  if (rest.compare(0, 2, "//") == 0) {
    p->has_authority = true;
    rest = rest.substr(2);
    const size_t slash = rest.find('/');
    std::string auth = rest.substr(0, slash);
    p->path = slash == std::string::npos ? std::string() : rest.substr(slash);
    const size_t at = auth.rfind('@');
    if (at != std::string::npos) {
      std::string ui = auth.substr(0, at);
      auth = auth.substr(at + 1);
      const size_t c2 = ui.find(':');
      p->user = ui.substr(0, c2);
      if (c2 != std::string::npos) p->pass = ui.substr(c2 + 1);
    }
    size_t pc = std::string::npos;
    if (!auth.empty() && auth[0] == '[') {
      const size_t rb = auth.find(']');
      if (rb != std::string::npos && rb + 1 < auth.size() && auth[rb + 1] == ':') pc = rb + 1;
    } else {
      pc = auth.rfind(':');
    }
    if (pc != std::string::npos) {
      p->port = auth.substr(pc + 1);
      auth = auth.substr(0, pc);
      for (char c : p->port) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    p->host = is_special(p->scheme) ? lower(auth) : auth;
    if (is_special(p->scheme) && p->scheme != "file" && p->host.empty()) return false;
    if (p->port == default_port(p->scheme)) p->port.clear();
    if (is_special(p->scheme)) {
      if (p->path.empty()) p->path = "/";
      p->path = normalize_path(p->path);
    }
  } else {
    p->path = rest;
  }
  return true;
}

std::string host_of(const Parts& p) { return p.host + (p.port.empty() ? "" : ":" + p.port); }

std::string origin_of(const Parts& p) {
  if (!is_special(p.scheme) || p.scheme == "file") return "null";
  return p.scheme + "://" + host_of(p);
}

std::string href_of(const Parts& p) {
  std::string out = p.scheme + ":";
  if (p.has_authority) {
    out += "//";
    if (!p.user.empty() || !p.pass.empty()) out += p.user + (p.pass.empty() ? "" : ":" + p.pass) + "@";
    out += host_of(p);
  }
  out += p.path;
  if (p.has_query) out += "?" + p.query;
  if (p.has_fragment) out += "#" + p.fragment;
  return out;
}

struct UrlObject final : HostObject {
  UrlObject() : HostObject(HostKind::Url) {}
  Parts parts;
  SearchParamsObject* params = nullptr;
  void trace(Tracer& t) override { Object::trace(t); t.mark(params); }
};

void SearchParamsObject::trace(Tracer& t) {
  Object::trace(t);
  t.mark(owner);
}

Object* g_url_proto = nullptr;
Object* g_params_proto = nullptr;

std::string serialize_params(const std::vector<std::pair<std::string, std::string>>& items) {
  std::string out;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) out += "&";
    out += pct_encode(items[i].first, true) + "=" + pct_encode(items[i].second, true);
  }
  return out;
}

void parse_params(const std::string& q0, std::vector<std::pair<std::string, std::string>>* out) {
  std::string q = q0;
  if (!q.empty() && q[0] == '?') q = q.substr(1);
  size_t at = 0;
  while (at <= q.size() && !q.empty()) {
    size_t amp = q.find('&', at);
    if (amp == std::string::npos) amp = q.size();
    const std::string pair = q.substr(at, amp - at);
    if (!pair.empty()) {
      const size_t eq = pair.find('=');
      out->emplace_back(pct_decode(pair.substr(0, eq), true), eq == std::string::npos ? std::string() : pct_decode(pair.substr(eq + 1), true));
    }
    if (amp >= q.size()) break;
    at = amp + 1;
  }
}

void params_changed(SearchParamsObject* sp) {
  if (!sp->owner) return;
  const std::string q = serialize_params(sp->items);
  sp->owner->parts.has_query = !q.empty();
  sp->owner->parts.query = q;
}

SearchParamsObject* this_params(Page& P, CallArgs& a) { return static_cast<SearchParamsObject*>(this_host(P, a, HostKind::SearchParams, "URLSearchParams")); }
UrlObject* this_url(Page& P, CallArgs& a) { return static_cast<UrlObject*>(this_host(P, a, HostKind::Url, "URL")); }

SearchParamsObject* new_params(Page& P, UrlObject* owner) {
  SearchParamsObject* sp = P.ctx.heap.make<SearchParamsObject>();
  sp->proto = g_params_proto;
  sp->owner = owner;
  return sp;
}

void install_url(Page& P) {
  Page* pp = &P;
  g_params_proto = make_proto(P, P.ctx.intr.object_proto, "URLSearchParams");
  g_url_proto = make_proto(P, P.ctx.intr.object_proto, "URL");
  Object* sp = g_params_proto;
  def_ctor(P, "URLSearchParams", 0, sp, [pp](Context&, CallArgs& a) -> Value {
    if (!a.is_construct()) {
      pp->ctx.throw_type_error("Failed to construct 'URLSearchParams': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
      return Value::Exception();
    }
    SearchParamsObject* s = new_params(*pp, nullptr);
    const Value init = a[0];
    if (init.is_object() && init.as_object()->cls == js::Class::Host && static_cast<HostObject*>(init.as_object())->host_kind == HostKind::SearchParams) {
      s->items = static_cast<SearchParamsObject*>(init.as_object())->items;
    } else if (init.is_object()) {
      Value iter;
      if (!pp->ctx.get_method(init, pp->ctx.sym.iterator, &iter)) return Value::Exception();
      if (!iter.is_undefined()) {
        if (!pp->ctx.iterate(init, [&](Value pair) {
              std::vector<Value> kv;
              if (!pp->ctx.iterable_to_list(pair, &kv) || kv.size() != 2) {
                if (!pp->ctx.has_exception()) pp->ctx.throw_type_error("Failed to construct 'URLSearchParams': Each query pair must be an iterable [name, value] tuple");
                return false;
              }
              s->items.emplace_back(pp->text(kv[0]), pp->text(kv[1]));
              return true;
            }))
          return Value::Exception();
      } else {
        std::vector<PropKey> keys;
        if (!init.as_object()->own_keys(pp->ctx, &keys)) return Value::Exception();
        for (PropKey k : keys) {
          if (k->kind != js::CellKind::String) continue;
          Value v;
          if (!init.as_object()->get(pp->ctx, k, init, &v)) return Value::Exception();
          s->items.emplace_back(static_cast<js::JsString*>(k)->utf8(), pp->text(v));
        }
      }
    } else if (!init.is_undefined()) {
      parse_params(pp->text(init), &s->items);
    }
    return Value::Obj(s);
  });
  def_method(P, sp, "get", 1, [pp](Context&, CallArgs& a) -> Value {
    SearchParamsObject* s = this_params(*pp, a);
    if (!s) return Value::Exception();
    const std::string n = arg_text(*pp, a, 0);
    for (auto& kv : s->items) if (kv.first == n) return str_val(*pp, kv.second);
    return Value::Null();
  });
  def_method(P, sp, "getAll", 1, [pp](Context&, CallArgs& a) -> Value {
    SearchParamsObject* s = this_params(*pp, a);
    if (!s) return Value::Exception();
    const std::string n = arg_text(*pp, a, 0);
    std::vector<Value> out;
    for (auto& kv : s->items) if (kv.first == n) out.push_back(str_val(*pp, kv.second));
    return make_array(*pp, out);
  });
  def_method(P, sp, "has", 1, [pp](Context&, CallArgs& a) -> Value {
    SearchParamsObject* s = this_params(*pp, a);
    if (!s) return Value::Exception();
    const std::string n = arg_text(*pp, a, 0);
    for (auto& kv : s->items) if (kv.first == n && (a.argc < 2 || kv.second == pp->text(a[1]))) return Value::Bool(true);
    return Value::Bool(false);
  });
  def_method(P, sp, "append", 2, [pp](Context&, CallArgs& a) -> Value {
    SearchParamsObject* s = this_params(*pp, a);
    if (!s) return Value::Exception();
    s->items.emplace_back(arg_text(*pp, a, 0), arg_text(*pp, a, 1));
    params_changed(s);
    return Value::Undefined();
  });
  def_method(P, sp, "set", 2, [pp](Context&, CallArgs& a) -> Value {
    SearchParamsObject* s = this_params(*pp, a);
    if (!s) return Value::Exception();
    const std::string n = arg_text(*pp, a, 0), v = arg_text(*pp, a, 1);
    bool placed = false;
    for (size_t i = 0; i < s->items.size(); ++i) {
      if (s->items[i].first != n) continue;
      if (!placed) { s->items[i].second = v; placed = true; }
      else { s->items.erase(s->items.begin() + static_cast<long>(i)); --i; }
    }
    if (!placed) s->items.emplace_back(n, v);
    params_changed(s);
    return Value::Undefined();
  });
  def_method(P, sp, "delete", 1, [pp](Context&, CallArgs& a) -> Value {
    SearchParamsObject* s = this_params(*pp, a);
    if (!s) return Value::Exception();
    const std::string n = arg_text(*pp, a, 0);
    s->items.erase(std::remove_if(s->items.begin(), s->items.end(), [&](const auto& kv) { return kv.first == n && (a.argc < 2 || kv.second == pp->text(a[1])); }), s->items.end());
    params_changed(s);
    return Value::Undefined();
  });
  def_method(P, sp, "sort", 0, [pp](Context&, CallArgs& a) -> Value {
    SearchParamsObject* s = this_params(*pp, a);
    if (!s) return Value::Exception();
    std::stable_sort(s->items.begin(), s->items.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    params_changed(s);
    return Value::Undefined();
  });
  def_method(P, sp, "toString", 0, [pp](Context&, CallArgs& a) -> Value {
    SearchParamsObject* s = this_params(*pp, a);
    if (!s) return Value::Exception();
    return str_val(*pp, serialize_params(s->items));
  });
  def_accessor(P, sp, "size", [pp](Context&, CallArgs& a) -> Value {
    SearchParamsObject* s = this_params(*pp, a);
    if (!s) return Value::Exception();
    return Value::Number(static_cast<double>(s->items.size()));
  });
  auto iter = [pp](CallArgs& a, int kind) -> Value {
    SearchParamsObject* s = this_params(*pp, a);
    if (!s) return Value::Exception();
    std::vector<Value> items;
    for (auto& kv : s->items) {
      if (kind == 0) items.push_back(str_val(*pp, kv.first));
      else if (kind == 1) items.push_back(str_val(*pp, kv.second));
      else items.push_back(make_array(*pp, {str_val(*pp, kv.first), str_val(*pp, kv.second)}));
    }
    Value arr = make_array(*pp, items), fn, r;
    if (!pp->ctx.get(arr, pp->ctx.atom_ascii("values"), &fn) || !pp->ctx.call(fn, arr, nullptr, 0, &r)) return Value::Exception();
    return r;
  };
  def_method(P, sp, "keys", 0, [iter](Context&, CallArgs& a) -> Value { return iter(a, 0); });
  def_method(P, sp, "values", 0, [iter](Context&, CallArgs& a) -> Value { return iter(a, 1); });
  Function* entries = def_method(P, sp, "entries", 0, [iter](Context&, CallArgs& a) -> Value { return iter(a, 2); });
  P.ctx.define_data(sp, P.ctx.sym.iterator, Value::Obj(entries), js::kHiddenAttrs);
  def_method(P, sp, "forEach", 1, [pp](Context& c, CallArgs& a) -> Value {
    SearchParamsObject* s = this_params(*pp, a);
    if (!s) return Value::Exception();
    auto items = s->items;
    for (auto& kv : items) {
      Value ignored;
      if (!c.call(a[0], a[1], {str_val(*pp, kv.second), str_val(*pp, kv.first), a.this_value}, &ignored)) return Value::Exception();
    }
    return Value::Undefined();
  });

  // URL
  auto make_url = [pp](const std::string& input, const Value& base_v, Parts* out) -> bool {
    std::string abs = input;
    if (!base_v.is_undefined()) {
      const std::string base = pp->text(base_v);
      Parts probe;
      if (!parse_parts(input, &probe)) abs = web::resolve_url(base, input);
    }
    if (!parse_parts(abs, out)) {
      pp->ctx.throw_type_error("Failed to construct 'URL': Invalid URL");
      return false;
    }
    return true;
  };
  def_ctor(P, "URL", 1, g_url_proto, [pp, make_url](Context&, CallArgs& a) -> Value {
    if (!a.is_construct()) {
      pp->ctx.throw_type_error("Failed to construct 'URL': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
      return Value::Exception();
    }
    if (a.argc < 1) {
      pp->ctx.throw_type_error("Failed to construct 'URL': 1 argument required, but only 0 present.");
      return Value::Exception();
    }
    UrlObject* u = pp->ctx.heap.make<UrlObject>();
    u->proto = g_url_proto;
    if (!make_url(pp->text(a[0]), a[1], &u->parts)) return Value::Exception();
    u->params = new_params(*pp, u);
    parse_params(u->parts.query, &u->params->items);
    return Value::Obj(u);
  });
  Value url_ctor;
  P.ctx.get(Value::Obj(P.ctx.global()), P.ctx.atom_ascii("URL"), &url_ctor);
  P.ctx.define_method(url_ctor.as_object(), "canParse", [](Context& c, CallArgs& a) -> Value {
    js::JsString* s;
    if (!c.to_string(a[0], &s)) return Value::Exception();
    Parts p;
    if (parse_parts(s->utf8(), &p)) return Value::Bool(true);
    if (a.argc > 1 && !a[1].is_undefined()) {
      js::JsString* b;
      if (!c.to_string(a[1], &b)) return Value::Exception();
      return Value::Bool(parse_parts(web::resolve_url(b->utf8(), s->utf8()), &p));
    }
    return Value::Bool(false);
  }, 1);
  def_method(P, url_ctor.as_object(), "createObjectURL", 1, [pp](Context&, CallArgs& a) -> Value {
    BlobObject* b = as_blob(a[0]);
    if (!b) {
      pp->ctx.throw_type_error("Failed to execute 'createObjectURL' on 'URL': Overload resolution failed.");
      return Value::Exception();
    }
    const std::string url = "blob:aios/" + std::to_string(++g_blob_counter);
    g_blob_urls[url] = b->data;
    g_blob_types[url] = b->type;
    return str_val(*pp, url);
  });
  def_method(P, url_ctor.as_object(), "revokeObjectURL", 1, [pp](Context&, CallArgs& a) -> Value {
    g_blob_urls.erase(arg_text(*pp, a, 0));
    return Value::Undefined();
  });
  Object* up = g_url_proto;
  auto url_str = [pp](const char* name, std::function<std::string(UrlObject*)> get, std::function<void(UrlObject*, const std::string&)> set) {
    HostFn setter = nullptr;
    if (set) setter = [pp, set](Context&, CallArgs& a) -> Value {
      UrlObject* u = this_url(*pp, a);
      if (!u) return Value::Exception();
      set(u, pp->text(a[0]));
      return Value::Undefined();
    };
    def_accessor(*pp, g_url_proto, name, [pp, get](Context&, CallArgs& a) -> Value {
      UrlObject* u = this_url(*pp, a);
      if (!u) return Value::Exception();
      return str_val(*pp, get(u));
    }, setter);
  };
  url_str("href", [](UrlObject* u) { return href_of(u->parts); }, [pp](UrlObject* u, const std::string& v) {
    Parts p;
    if (!parse_parts(v, &p)) { pp->ctx.throw_type_error("Failed to set the 'href' property on 'URL': Invalid URL"); return; }
    u->parts = p;
    u->params->items.clear();
    parse_params(p.query, &u->params->items);
  });
  url_str("origin", [](UrlObject* u) { return origin_of(u->parts); }, nullptr);
  url_str("protocol", [](UrlObject* u) { return u->parts.scheme + ":"; }, [](UrlObject* u, const std::string& v) { u->parts.scheme = lower(v.substr(0, v.find(':'))); });
  url_str("username", [](UrlObject* u) { return u->parts.user; }, [](UrlObject* u, const std::string& v) { u->parts.user = v; });
  url_str("password", [](UrlObject* u) { return u->parts.pass; }, [](UrlObject* u, const std::string& v) { u->parts.pass = v; });
  url_str("host", [](UrlObject* u) { return host_of(u->parts); }, [](UrlObject* u, const std::string& v) {
    const size_t c = v.rfind(':');
    if (c == std::string::npos) u->parts.host = lower(v);
    else { u->parts.host = lower(v.substr(0, c)); u->parts.port = v.substr(c + 1); if (u->parts.port == default_port(u->parts.scheme)) u->parts.port.clear(); }
  });
  url_str("hostname", [](UrlObject* u) { return u->parts.host; }, [](UrlObject* u, const std::string& v) { u->parts.host = lower(v); });
  url_str("port", [](UrlObject* u) { return u->parts.port; }, [](UrlObject* u, const std::string& v) { u->parts.port = v == default_port(u->parts.scheme) ? std::string() : v; });
  url_str("pathname", [](UrlObject* u) { return u->parts.path; }, [](UrlObject* u, const std::string& v) { u->parts.path = (v.empty() || v[0] == '/') ? v : "/" + v; });
  url_str("search", [](UrlObject* u) { return u->parts.has_query && !u->parts.query.empty() ? "?" + u->parts.query : std::string(); }, [](UrlObject* u, const std::string& v) {
    std::string q = v;
    if (!q.empty() && q[0] == '?') q = q.substr(1);
    u->parts.query = q;
    u->parts.has_query = !q.empty();
    u->params->items.clear();
    parse_params(q, &u->params->items);
  });
  url_str("hash", [](UrlObject* u) { return u->parts.has_fragment && !u->parts.fragment.empty() ? "#" + u->parts.fragment : std::string(); }, [](UrlObject* u, const std::string& v) {
    std::string f = v;
    if (!f.empty() && f[0] == '#') f = f.substr(1);
    u->parts.fragment = f;
    u->parts.has_fragment = !f.empty();
  });
  def_accessor(P, up, "searchParams", [pp](Context&, CallArgs& a) -> Value {
    UrlObject* u = this_url(*pp, a);
    if (!u) return Value::Exception();
    return Value::Obj(u->params);
  });
  def_method(P, up, "toString", 0, [pp](Context&, CallArgs& a) -> Value {
    UrlObject* u = this_url(*pp, a);
    if (!u) return Value::Exception();
    return str_val(*pp, href_of(u->parts));
  });
  def_method(P, up, "toJSON", 0, [pp](Context&, CallArgs& a) -> Value {
    UrlObject* u = this_url(*pp, a);
    if (!u) return Value::Exception();
    return str_val(*pp, href_of(u->parts));
  });
}

// ---------------------------------------------------------------------------------------------------
// AbortController and AbortSignal
// ---------------------------------------------------------------------------------------------------

struct AbortSignalObject final : HostObject {
  AbortSignalObject() : HostObject(HostKind::Abort) {}
  bool aborted = false;
  Value reason;
  void trace(Tracer& t) override { Object::trace(t); t.mark(reason); }
};

Object* g_signal_proto = nullptr;
Object* g_controller_proto = nullptr;

AbortSignalObject* as_signal(const Value& v) {
  if (v.is_object() && v.as_object()->cls == js::Class::Host && static_cast<HostObject*>(v.as_object())->host_kind == HostKind::Abort) return static_cast<AbortSignalObject*>(v.as_object());
  return nullptr;
}

void abort_signal(Page& P, AbortSignalObject* s, const Value& reason) {
  if (s->aborted) return;
  s->aborted = true;
  s->reason = reason.is_undefined() ? make_dom_exception(P, "AbortError", "signal is aborted without reason") : reason;
  Object* ev = make_event(P, "abort", "Event", false, false);
  dispatch_event_object(P, s, s, ev);
}

AbortSignalObject* new_signal(Page& P) {
  AbortSignalObject* s = P.ctx.heap.make<AbortSignalObject>();
  s->proto = g_signal_proto;
  return s;
}

void install_abort(Page& P) {
  Page* pp = &P;
  g_signal_proto = make_proto(P, P.event_target_proto, "AbortSignal");
  g_controller_proto = make_proto(P, P.ctx.intr.object_proto, "AbortController");
  Function* sig_ctor = def_ctor(P, "AbortSignal", 0, g_signal_proto, [](Context& c, CallArgs&) -> Value {
    c.throw_type_error("Illegal constructor");
    return Value::Exception();
  });
  def_accessor(P, g_signal_proto, "aborted", [pp](Context&, CallArgs& a) -> Value {
    AbortSignalObject* s = as_signal(a.this_value);
    if (!s) { pp->ctx.throw_type_error("Illegal invocation"); return Value::Exception(); }
    return Value::Bool(s->aborted);
  });
  def_accessor(P, g_signal_proto, "reason", [pp](Context&, CallArgs& a) -> Value {
    AbortSignalObject* s = as_signal(a.this_value);
    if (!s) { pp->ctx.throw_type_error("Illegal invocation"); return Value::Exception(); }
    return s->reason;
  });
  def_method(P, g_signal_proto, "throwIfAborted", 0, [pp](Context&, CallArgs& a) -> Value {
    AbortSignalObject* s = as_signal(a.this_value);
    if (!s) { pp->ctx.throw_type_error("Illegal invocation"); return Value::Exception(); }
    if (s->aborted) return pp->ctx.throw_value(s->reason);
    return Value::Undefined();
  });
  def_method(P, sig_ctor, "abort", 0, [pp](Context&, CallArgs& a) -> Value {
    AbortSignalObject* s = new_signal(*pp);
    abort_signal(*pp, s, a[0]);
    return Value::Obj(s);
  });
  def_method(P, sig_ctor, "timeout", 1, [pp](Context&, CallArgs& a) -> Value {
    AbortSignalObject* s = new_signal(*pp);
    PendingTimer t;
    t.id = pp->next_timer_id++;
    t.every_ms = pp->number(a[0]);
    t.due_ms = pp->now_ms + t.every_ms;
    t.fn = Value::Obj(pp->ctx.new_host_function("", [pp, s](Context&, CallArgs&) -> Value {
      abort_signal(*pp, s, make_dom_exception(*pp, "TimeoutError", "signal timed out"));
      return Value::Undefined();
    }, 0));
    pp->add_timer(pp->clocked, t, 2000);
    return Value::Obj(s);
  });
  def_method(P, sig_ctor, "any", 1, [pp](Context&, CallArgs& a) -> Value {
    AbortSignalObject* s = new_signal(*pp);
    std::vector<Value> list;
    if (!pp->ctx.iterable_to_list(a[0], &list)) return Value::Exception();
    for (const Value& v : list) {
      AbortSignalObject* in = as_signal(v);
      if (in && in->aborted) { abort_signal(*pp, s, in->reason); return Value::Obj(s); }
    }
    for (const Value& v : list) {
      if (!as_signal(v)) continue;
      Function* f = pp->ctx.new_host_function("", [pp, s, v](Context&, CallArgs&) -> Value {
        AbortSignalObject* in = as_signal(v);
        abort_signal(*pp, s, in ? in->reason : Value::Undefined());
        return Value::Undefined();
      }, 0);
      Value add, ignored;
      Object* opts = pp->ctx.new_object();
      pp->ctx.define_data(opts, pp->ctx.atom_ascii("once"), Value::Bool(true));
      if (pp->ctx.get(v, pp->ctx.atom_ascii("addEventListener"), &add)) pp->ctx.call(add, v, {Value::String(pp->ctx.atom_ascii("abort")), Value::Obj(f), Value::Obj(opts)}, &ignored);
    }
    return Value::Obj(s);
  });
  def_ctor(P, "AbortController", 0, g_controller_proto, [pp](Context&, CallArgs& a) -> Value {
    if (!a.is_construct()) {
      pp->ctx.throw_type_error("Failed to construct 'AbortController': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
      return Value::Exception();
    }
    Object* o = pp->ctx.new_object(g_controller_proto);
    AbortSignalObject* s = new_signal(*pp);
    pp->ctx.define_data(o, pp->ctx.atom_ascii("signal"), Value::Obj(s), js::kEnumerable | js::kConfigurable);
    return Value::Obj(o);
  });
  def_method(P, g_controller_proto, "abort", 0, [pp](Context&, CallArgs& a) -> Value {
    if (!a.this_value.is_object()) return Value::Undefined();
    Value sig;
    if (!pp->ctx.get(a.this_value, pp->ctx.atom_ascii("signal"), &sig)) return Value::Exception();
    if (AbortSignalObject* s = as_signal(sig)) abort_signal(*pp, s, a[0]);
    return Value::Undefined();
  });
}

// ---------------------------------------------------------------------------------------------------
// bodies, Request, Response, fetch
// ---------------------------------------------------------------------------------------------------

struct ResponseObject final : HostObject {
  ResponseObject() : HostObject(HostKind::Response) {}
  int status = 200;
  std::string status_text = "OK", url, body, type = "default";
  HeadersObject* headers = nullptr;
  bool body_used = false, redirected = false;
  Object* stream = nullptr;
  void trace(Tracer& t) override {
    Object::trace(t);
    t.mark(headers);
    t.mark(stream);
  }
};

struct RequestObject final : HostObject {
  RequestObject() : HostObject(HostKind::Request) {}
  std::string url, method = "GET", body, mode = "cors", credentials = "same-origin", cache = "default", redirect = "follow", referrer = "about:client";
  bool has_body = false, body_used = false;
  HeadersObject* headers = nullptr;
  Value signal;
  void trace(Tracer& t) override {
    Object::trace(t);
    t.mark(headers);
    t.mark(signal);
  }
};

Object* g_response_proto = nullptr;
Object* g_request_proto = nullptr;
Object* g_stream_proto = nullptr;

ResponseObject* as_response(const Value& v) {
  if (v.is_object() && v.as_object()->cls == js::Class::Host && static_cast<HostObject*>(v.as_object())->host_kind == HostKind::Response) return static_cast<ResponseObject*>(v.as_object());
  return nullptr;
}
RequestObject* as_request(const Value& v) {
  if (v.is_object() && v.as_object()->cls == js::Class::Host && static_cast<HostObject*>(v.as_object())->host_kind == HostKind::Request) return static_cast<RequestObject*>(v.as_object());
  return nullptr;
}
ResponseObject* this_response(Page& P, CallArgs& a) { return static_cast<ResponseObject*>(this_host(P, a, HostKind::Response, "Response")); }
RequestObject* this_request(Page& P, CallArgs& a) { return static_cast<RequestObject*>(this_host(P, a, HostKind::Request, "Request")); }

const char* status_text_for(int s) {
  switch (s) {
    case 200: return "OK"; case 201: return "Created"; case 202: return "Accepted"; case 204: return "No Content"; case 206: return "Partial Content";
    case 301: return "Moved Permanently"; case 302: return "Found"; case 303: return "See Other"; case 304: return "Not Modified"; case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect"; case 400: return "Bad Request"; case 401: return "Unauthorized"; case 403: return "Forbidden"; case 404: return "Not Found";
    case 405: return "Method Not Allowed"; case 409: return "Conflict"; case 410: return "Gone"; case 413: return "Payload Too Large"; case 418: return "I'm a teapot";
    case 422: return "Unprocessable Entity"; case 429: return "Too Many Requests"; case 500: return "Internal Server Error"; case 501: return "Not Implemented";
    case 502: return "Bad Gateway"; case 503: return "Service Unavailable"; case 504: return "Gateway Timeout"; default: return "";
  }
}

ResponseObject* new_response(Page& P, int status, const std::string& status_text, const std::string& url, const std::string& body, const std::string& content_type) {
  ResponseObject* r = P.ctx.heap.make<ResponseObject>();
  r->proto = g_response_proto;
  r->status = status;
  r->status_text = status_text;
  r->url = url;
  r->body = body;
  r->headers = new_headers(P);
  if (!content_type.empty()) r->headers->set("content-type", content_type);
  r->headers->set("content-length", std::to_string(body.size()));
  return r;
}

// A body init becomes bytes and a content type.
bool encode_body(Page& P, const Value& body, std::string* data, std::string* content_type) {
  if (body.is_nullish()) return true;
  if (BlobObject* b = as_blob(body)) { *data = b->data; *content_type = b->type; return true; }
  if (body.is_object() && body.as_object()->cls == js::Class::Host) {
    HostObject* h = static_cast<HostObject*>(body.as_object());
    if (h->host_kind == HostKind::SearchParams) {
      *data = serialize_params(static_cast<SearchParamsObject*>(h)->items);
      *content_type = "application/x-www-form-urlencoded;charset=UTF-8";
      return true;
    }
    if (h->host_kind == HostKind::FormData) {
      const std::string boundary = "----aiosformboundary" + std::to_string(g_blob_counter++ + 1000);
      std::string out;
      for (auto& kv : static_cast<FormDataObject*>(h)->items) {
        out += "--" + boundary + "\r\n";
        if (BlobObject* f = as_blob(kv.second)) {
          out += "Content-Disposition: form-data; name=\"" + kv.first + "\"; filename=\"" + (f->is_file ? f->name : "blob") + "\"\r\n";
          out += "Content-Type: " + (f->type.empty() ? std::string("application/octet-stream") : f->type) + "\r\n\r\n" + f->data + "\r\n";
        } else {
          out += "Content-Disposition: form-data; name=\"" + kv.first + "\"\r\n\r\n" + P.text(kv.second) + "\r\n";
        }
      }
      out += "--" + boundary + "--\r\n";
      *data = out;
      *content_type = "multipart/form-data; boundary=" + boundary;
      return true;
    }
  }
  std::string bytes;
  if (P.ctx.bytes_of(body, &bytes)) { *data = bytes; return true; }
  js::JsString* s;
  if (!P.ctx.to_string(body, &s)) return false;
  *data = s->utf8();
  *content_type = "text/plain;charset=UTF-8";
  return true;
}

Value text_value(Page& P, const std::string& bytes) { return str_val(P, bytes); }

Value body_json(Page& P, const std::string& body, bool* ok) {
  Value json, parse, r;
  *ok = false;
  if (!P.ctx.get(Value::Obj(P.ctx.global()), P.ctx.atom_ascii("JSON"), &json) || !P.ctx.get(json, P.ctx.atom_ascii("parse"), &parse)) return Value::Undefined();
  if (!P.ctx.call(parse, json, {str_val(P, body)}, &r)) return Value::Undefined();
  *ok = true;
  return r;
}

// Promise-returning body readers shared by Request and Response.
Value read_body(Page& P, const std::string& body, const std::string& type, int how, bool* used, Context& c) {
  (void)c;
  if (*used) return rejected_promise(P, type_error_value(P, "Body has already been consumed."));
  *used = true;
  switch (how) {
    case 0: return resolved_promise(P, text_value(P, body));
    case 1: {
      bool ok;
      Value v = body_json(P, body, &ok);
      if (!ok) {
        Value ex = P.ctx.take_exception();
        return rejected_promise(P, ex);
      }
      return resolved_promise(P, v);
    }
    case 2: return resolved_promise(P, blob_arraybuffer_value(P, body));
    case 3: return resolved_promise(P, Value::Obj(new_blob(P, body, type)));
    default: return resolved_promise(P, Value::Obj(P.ctx.new_uint8_array(reinterpret_cast<const uint8_t*>(body.data()), body.size())));
  }
}

void install_body_methods(Page& P, Object* proto, bool is_response) {
  Page* pp = &P;
  const char* names[] = {"text", "json", "arrayBuffer", "blob", "bytes"};
  for (int how = 0; how < 5; ++how) {
    def_method(P, proto, names[how], 0, [pp, how, is_response](Context& c, CallArgs& a) -> Value {
      if (is_response) {
        ResponseObject* r = this_response(*pp, a);
        if (!r) return Value::Exception();
        return read_body(*pp, r->body, r->headers->get("content-type"), how, &r->body_used, c);
      }
      RequestObject* r = this_request(*pp, a);
      if (!r) return Value::Exception();
      return read_body(*pp, r->body, r->headers->get("content-type"), how, &r->body_used, c);
    });
  }
}

// ReadableStream: only what Response.body needs -- one chunk, then done.
struct StreamObject final : HostObject {
  StreamObject() : HostObject(HostKind::Target) {}
  std::string body;
  bool read = false, locked = false;
};

void install_response(Page& P) {
  Page* pp = &P;
  g_response_proto = make_proto(P, P.ctx.intr.object_proto, "Response");
  g_stream_proto = make_proto(P, P.ctx.intr.object_proto, "ReadableStream");
  Object* rp = g_response_proto;
  Function* ctor = def_ctor(P, "Response", 0, rp, [pp](Context&, CallArgs& a) -> Value {
    if (!a.is_construct()) {
      pp->ctx.throw_type_error("Failed to construct 'Response': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
      return Value::Exception();
    }
    std::string data, ct;
    if (!encode_body(*pp, a[0], &data, &ct)) return Value::Exception();
    int status = 200;
    std::string status_text;
    Value init = a[1];
    if (init.is_object()) {
      Value v;
      if (!pp->ctx.get(init, pp->ctx.atom_ascii("status"), &v)) return Value::Exception();
      if (!v.is_undefined()) status = static_cast<int>(pp->number(v));
      if (!pp->ctx.get(init, pp->ctx.atom_ascii("statusText"), &v)) return Value::Exception();
      if (!v.is_undefined()) status_text = pp->text(v);
    }
    if (status < 200 || status > 599) {
      pp->ctx.throw_range_error("Failed to construct 'Response': The status provided (" + std::to_string(status) + ") is outside the range [200, 599].");
      return Value::Exception();
    }
    ResponseObject* r = new_response(*pp, status, status_text, "", data, ct);
    if (init.is_object()) {
      Value h;
      if (!pp->ctx.get(init, pp->ctx.atom_ascii("headers"), &h)) return Value::Exception();
      HeadersObject* extra = new_headers(*pp);
      if (!fill_headers(*pp, extra, h)) return Value::Exception();
      for (auto& kv : extra->items) r->headers->set(kv.first, kv.second);
    }
    return Value::Obj(r);
  });
  def_method(P, ctor, "json", 1, [pp](Context&, CallArgs& a) -> Value {
    Value json, str, r;
    if (!pp->ctx.get(Value::Obj(pp->ctx.global()), pp->ctx.atom_ascii("JSON"), &json) || !pp->ctx.get(json, pp->ctx.atom_ascii("stringify"), &str) || !pp->ctx.call(str, json, {a[0]}, &r)) return Value::Exception();
    ResponseObject* resp = new_response(*pp, 200, "OK", "", r.is_string() ? r.as_string()->utf8() : "", "application/json");
    resp->type = "default";
    return Value::Obj(resp);
  });
  def_method(P, ctor, "error", 0, [pp](Context&, CallArgs&) -> Value {
    ResponseObject* r = new_response(*pp, 0, "", "", "", "");
    r->type = "error";
    return Value::Obj(r);
  });
  def_method(P, ctor, "redirect", 1, [pp](Context&, CallArgs& a) -> Value {
    ResponseObject* r = new_response(*pp, a.argc > 1 ? static_cast<int>(pp->number(a[1])) : 302, "Found", "", "", "");
    r->headers->set("location", arg_text(*pp, a, 0));
    return Value::Obj(r);
  });
  install_body_methods(P, rp, true);
  def_method(P, rp, "clone", 0, [pp](Context&, CallArgs& a) -> Value {
    ResponseObject* r = this_response(*pp, a);
    if (!r) return Value::Exception();
    if (r->body_used) {
      pp->ctx.throw_type_error("Failed to execute 'clone' on 'Response': Response body is already used");
      return Value::Exception();
    }
    ResponseObject* c = new_response(*pp, r->status, r->status_text, r->url, r->body, "");
    c->headers->items = r->headers->items;
    c->type = r->type;
    c->redirected = r->redirected;
    return Value::Obj(c);
  });
  def_accessor(P, rp, "status", [pp](Context&, CallArgs& a) -> Value { ResponseObject* r = this_response(*pp, a); return r ? Value::Number(r->status) : Value::Exception(); });
  def_accessor(P, rp, "ok", [pp](Context&, CallArgs& a) -> Value { ResponseObject* r = this_response(*pp, a); return r ? Value::Bool(r->status >= 200 && r->status < 300) : Value::Exception(); });
  def_accessor(P, rp, "statusText", [pp](Context&, CallArgs& a) -> Value { ResponseObject* r = this_response(*pp, a); return r ? str_val(*pp, r->status_text) : Value::Exception(); });
  def_accessor(P, rp, "url", [pp](Context&, CallArgs& a) -> Value { ResponseObject* r = this_response(*pp, a); return r ? str_val(*pp, r->url) : Value::Exception(); });
  def_accessor(P, rp, "type", [pp](Context&, CallArgs& a) -> Value { ResponseObject* r = this_response(*pp, a); return r ? str_val(*pp, r->type) : Value::Exception(); });
  def_accessor(P, rp, "redirected", [pp](Context&, CallArgs& a) -> Value { ResponseObject* r = this_response(*pp, a); return r ? Value::Bool(r->redirected) : Value::Exception(); });
  def_accessor(P, rp, "bodyUsed", [pp](Context&, CallArgs& a) -> Value { ResponseObject* r = this_response(*pp, a); return r ? Value::Bool(r->body_used) : Value::Exception(); });
  def_accessor(P, rp, "headers", [pp](Context&, CallArgs& a) -> Value { ResponseObject* r = this_response(*pp, a); return r ? Value::Obj(r->headers) : Value::Exception(); });
  def_accessor(P, rp, "body", [pp](Context&, CallArgs& a) -> Value {
    ResponseObject* r = this_response(*pp, a);
    if (!r) return Value::Exception();
    if (r->body.empty() && r->status == 204) return Value::Null();
    if (!r->stream) {
      StreamObject* s = pp->ctx.heap.make<StreamObject>();
      s->proto = g_stream_proto;
      s->body = r->body;
      r->stream = s;
    }
    return Value::Obj(r->stream);
  });

  // ReadableStream (the read side Response.body hands out)
  def_accessor(P, g_stream_proto, "locked", [](Context&, CallArgs& a) -> Value {
    return Value::Bool(a.this_value.is_object() && static_cast<StreamObject*>(a.this_value.as_object())->locked);
  });
  def_method(P, g_stream_proto, "getReader", 0, [pp](Context&, CallArgs& a) -> Value {
    if (!a.this_value.is_object() || a.this_value.as_object()->cls != js::Class::Host || static_cast<HostObject*>(a.this_value.as_object())->host_kind != HostKind::Target) {
      pp->ctx.throw_type_error("Illegal invocation");
      return Value::Exception();
    }
    StreamObject* s = static_cast<StreamObject*>(a.this_value.as_object());
    if (s->locked) {
      pp->ctx.throw_type_error("ReadableStream is locked");
      return Value::Exception();
    }
    s->locked = true;
    Object* reader = pp->ctx.new_object();
    Value stream_v = a.this_value;
    pp->ctx.define_data(reader, pp->ctx.atom_ascii("__stream"), stream_v, js::kHiddenAttrs);
    def_method(*pp, reader, "read", 0, [pp, stream_v](Context& c, CallArgs&) -> Value {
      StreamObject* st = static_cast<StreamObject*>(stream_v.as_object());
      Object* res = c.new_object();
      if (!st->read && !st->body.empty()) {
        st->read = true;
        c.define_data(res, c.atoms.value, Value::Obj(c.new_uint8_array(reinterpret_cast<const uint8_t*>(st->body.data()), st->body.size())));
        c.define_data(res, c.atoms.done, Value::Bool(false));
      } else {
        st->read = true;
        c.define_data(res, c.atoms.value, Value::Undefined());
        c.define_data(res, c.atoms.done, Value::Bool(true));
      }
      return resolved_promise(*pp, Value::Obj(res));
    });
    def_method(*pp, reader, "cancel", 0, [pp, stream_v](Context&, CallArgs&) -> Value {
      static_cast<StreamObject*>(stream_v.as_object())->read = true;
      return resolved_promise(*pp, Value::Undefined());
    });
    def_method(*pp, reader, "releaseLock", 0, [stream_v](Context&, CallArgs&) -> Value {
      static_cast<StreamObject*>(stream_v.as_object())->locked = false;
      return Value::Undefined();
    });
    pp->ctx.define_data(reader, pp->ctx.atom_ascii("closed"), resolved_promise(*pp, Value::Undefined()));
    return Value::Obj(reader);
  });
}

void install_request(Page& P) {
  Page* pp = &P;
  g_request_proto = make_proto(P, P.ctx.intr.object_proto, "Request");
  Object* qp = g_request_proto;
  auto build = [pp](const Value& input, const Value& init, RequestObject* r) -> bool {
    if (RequestObject* src = as_request(input)) {
      r->url = src->url;
      r->method = src->method;
      r->body = src->body;
      r->has_body = src->has_body;
      r->headers = new_headers(*pp);
      r->headers->items = src->headers->items;
      r->signal = src->signal;
      r->mode = src->mode;
      r->credentials = src->credentials;
    } else {
      r->url = resolve(*pp, pp->text(input));
      r->headers = new_headers(*pp);
    }
    if (init.is_object()) {
      auto field = [&](const char* n, Value* out) { return pp->ctx.get(init, pp->ctx.atom_ascii(n), out); };
      Value v;
      if (!field("method", &v)) return false;
      if (!v.is_undefined()) { r->method = pp->text(v); for (char& ch : r->method) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); }
      if (!field("headers", &v)) return false;
      if (!v.is_undefined()) { r->headers = new_headers(*pp); if (!fill_headers(*pp, r->headers, v)) return false; }
      if (!field("body", &v)) return false;
      if (!v.is_nullish()) {
        if (r->method == "GET" || r->method == "HEAD") {
          pp->ctx.throw_type_error("Failed to construct 'Request': Request with GET/HEAD method cannot have body.");
          return false;
        }
        std::string ct;
        if (!encode_body(*pp, v, &r->body, &ct)) return false;
        r->has_body = true;
        if (!ct.empty() && !r->headers->has("content-type")) r->headers->set("content-type", ct);
      }
      if (!field("signal", &v)) return false;
      if (!v.is_undefined()) r->signal = v;
      if (!field("mode", &v)) return false;
      if (!v.is_undefined()) r->mode = pp->text(v);
      if (!field("credentials", &v)) return false;
      if (!v.is_undefined()) r->credentials = pp->text(v);
    }
    return true;
  };
  def_ctor(P, "Request", 1, qp, [pp, build](Context&, CallArgs& a) -> Value {
    if (!a.is_construct()) {
      pp->ctx.throw_type_error("Failed to construct 'Request': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
      return Value::Exception();
    }
    if (a.argc < 1) {
      pp->ctx.throw_type_error("Failed to construct 'Request': 1 argument required, but only 0 present.");
      return Value::Exception();
    }
    RequestObject* r = pp->ctx.heap.make<RequestObject>();
    r->proto = g_request_proto;
    if (!build(a[0], a[1], r)) return Value::Exception();
    return Value::Obj(r);
  });
  install_body_methods(P, qp, false);
  def_method(P, qp, "clone", 0, [pp](Context&, CallArgs& a) -> Value {
    RequestObject* r = this_request(*pp, a);
    if (!r) return Value::Exception();
    RequestObject* c = pp->ctx.heap.make<RequestObject>();
    c->proto = g_request_proto;
    c->url = r->url; c->method = r->method; c->body = r->body; c->has_body = r->has_body; c->signal = r->signal;
    c->mode = r->mode; c->credentials = r->credentials;
    c->headers = new_headers(*pp);
    c->headers->items = r->headers->items;
    return Value::Obj(c);
  });
  auto req_str = [pp](const char* name, std::function<std::string(RequestObject*)> f) {
    def_accessor(*pp, g_request_proto, name, [pp, f](Context&, CallArgs& a) -> Value {
      RequestObject* r = this_request(*pp, a);
      if (!r) return Value::Exception();
      return str_val(*pp, f(r));
    });
  };
  req_str("url", [](RequestObject* r) { return r->url; });
  req_str("method", [](RequestObject* r) { return r->method; });
  req_str("mode", [](RequestObject* r) { return r->mode; });
  req_str("credentials", [](RequestObject* r) { return r->credentials; });
  req_str("cache", [](RequestObject* r) { return r->cache; });
  req_str("redirect", [](RequestObject* r) { return r->redirect; });
  req_str("referrer", [](RequestObject* r) { return r->referrer; });
  def_accessor(P, qp, "headers", [pp](Context&, CallArgs& a) -> Value { RequestObject* r = this_request(*pp, a); return r ? Value::Obj(r->headers) : Value::Exception(); });
  def_accessor(P, qp, "signal", [pp](Context&, CallArgs& a) -> Value {
    RequestObject* r = this_request(*pp, a);
    if (!r) return Value::Exception();
    if (r->signal.is_undefined()) r->signal = Value::Obj(new_signal(*pp));
    return r->signal;
  });
  def_accessor(P, qp, "bodyUsed", [pp](Context&, CallArgs& a) -> Value { RequestObject* r = this_request(*pp, a); return r ? Value::Bool(r->body_used) : Value::Exception(); });
}

// data: URLs
bool decode_data_url(const std::string& url, std::string* body, std::string* type) {
  const size_t comma = url.find(',');
  if (comma == std::string::npos) return false;
  std::string meta = url.substr(5, comma - 5);
  const std::string payload = url.substr(comma + 1);
  bool b64 = false;
  const size_t semi = meta.rfind(';');
  if (semi != std::string::npos && lower(meta.substr(semi + 1)) == "base64") { b64 = true; meta = meta.substr(0, semi); }
  *type = meta.empty() ? "text/plain;charset=US-ASCII" : meta;
  if (b64) {
    std::string clean;
    for (char c : pct_decode(payload, false)) if (!std::isspace(static_cast<unsigned char>(c))) clean += c;
    auto decoded = aios::base64_decode(clean);
    if (!decoded.ok()) return false;
    body->assign(decoded->begin(), decoded->end());
    return true;
  }
  *body = pct_decode(payload, false);
  return true;
}

// The whole of a fetch: returns a settled promise.
Value do_fetch(Page& P, const Value& input, const Value& init) {
  // build a Request from the arguments
  Value request_ctor;
  if (!P.ctx.get(Value::Obj(P.ctx.global()), P.ctx.atom_ascii("Request"), &request_ctor)) return rejected_promise(P, P.ctx.take_exception());
  Value args[2] = {input, init};
  Value req_v;
  if (!P.ctx.construct(request_ctor, args, 2, request_ctor, &req_v)) return rejected_promise(P, P.ctx.take_exception());
  RequestObject* r = as_request(req_v);
  if (AbortSignalObject* sig = as_signal(r->signal); sig && sig->aborted) return rejected_promise(P, sig->reason);
  const std::string abs = r->url;
  web::FetchOptions opt;
  opt.method = r->method;
  opt.body = r->body;
  for (auto& kv : r->headers->items) opt.headers.push_back(kv);
  if (!opt.body.empty() && !r->headers->has("content-type")) {
    const char c = opt.body[0];
    opt.headers.push_back({"Content-Type", c == '{' || c == '[' ? "application/json" : "text/plain;charset=UTF-8"});
  }
  std::string line = opt.method + " " + abs;
  auto remember = [&](const std::string& tail) {
    P.net.push_back(line + tail);
    if (P.net.size() > 200) P.net.erase(P.net.begin());
  };
  if (abs.compare(0, 5, "data:") == 0) {
    std::string body, type;
    if (!decode_data_url(abs, &body, &type)) {
      remember("  -> bad data url");
      return rejected_promise(P, type_error_value(P, "Failed to fetch"));
    }
    remember("  -> 200, " + std::to_string(body.size()) + " bytes");
    return resolved_promise(P, Value::Obj(new_response(P, 200, "OK", abs, body, type)));
  }
  if (abs.compare(0, 5, "blob:") == 0) {
    auto it = g_blob_urls.find(abs);
    if (it == g_blob_urls.end()) {
      remember("  -> not found");
      return rejected_promise(P, type_error_value(P, "Failed to fetch"));
    }
    remember("  -> 200, " + std::to_string(it->second.size()) + " bytes");
    return resolved_promise(P, Value::Obj(new_response(P, 200, "OK", abs, it->second, g_blob_types[abs])));
  }
  if (!P.fetch) {
    remember("  -> no transport");
    return rejected_promise(P, type_error_value(P, "Failed to fetch"));
  }
  auto got = P.fetch(abs, opt);
  if (!got.ok()) {
    remember("  -> " + got.error().message);
    return rejected_promise(P, type_error_value(P, "Failed to fetch"));
  }
  const int status = got->status > 0 ? got->status : 200;
  remember("  -> " + std::to_string(status) + ", " + std::to_string(got->body.size()) + " bytes");
  ResponseObject* resp = new_response(P, status, status_text_for(status), got->final_url.empty() ? abs : got->final_url, std::string(got->body.begin(), got->body.end()), got->content_type);
  resp->type = "basic";
  resp->redirected = !got->final_url.empty() && got->final_url != abs;
  return resolved_promise(P, Value::Obj(resp));
}

void install_fetch(Page& P) {
  Page* pp = &P;
  Function* f = P.ctx.new_host_function("fetch", [pp](Context&, CallArgs& a) -> Value {
    if (a.argc < 1) return rejected_promise(*pp, type_error_value(*pp, "Failed to execute 'fetch' on 'Window': 1 argument required, but only 0 present."));
    return do_fetch(*pp, a[0], a[1]);
  }, 1);
  P.ctx.define_data(P.ctx.global(), P.ctx.atom_ascii("fetch"), Value::Obj(f), js::kHiddenAttrs);
}

// ---------------------------------------------------------------------------------------------------
// XMLHttpRequest
// ---------------------------------------------------------------------------------------------------

struct XhrObject final : HostObject {
  XhrObject() : HostObject(HostKind::Xhr) {}
  int ready = 0, status = 0;
  std::string method = "GET", url, response_text, response_url, status_text, response_type, mime;
  std::vector<std::pair<std::string, std::string>> req_headers;
  HeadersObject* resp_headers = nullptr;
  bool async = true, sent = false, aborted = false;
  double timeout = 0;
  Value response;
  void trace(Tracer& t) override { Object::trace(t); t.mark(resp_headers); t.mark(response); }
};

Object* g_xhr_proto = nullptr;

XhrObject* this_xhr(Page& P, CallArgs& a) { return static_cast<XhrObject*>(this_host(P, a, HostKind::Xhr, "XMLHttpRequest")); }

void xhr_event(Page& P, XhrObject* x, const char* type) {
  Object* ev = make_event(P, type, std::strcmp(type, "readystatechange") == 0 ? "Event" : "ProgressEvent", false, false);
  dispatch_event_object(P, x, x, ev);
}

void xhr_state(Page& P, XhrObject* x, int state) {
  x->ready = state;
  xhr_event(P, x, "readystatechange");
}

void xhr_finish(Page& P, XhrObject* x, bool ok) {
  if (x->aborted) return;
  xhr_state(P, x, 2);
  xhr_state(P, x, 3);
  if (ok) {
    xhr_state(P, x, 4);
    xhr_event(P, x, "load");
  } else {
    xhr_state(P, x, 4);
    xhr_event(P, x, "error");
  }
  xhr_event(P, x, "loadend");
}

void install_xhr(Page& P) {
  Page* pp = &P;
  g_xhr_proto = make_proto(P, P.event_target_proto, "XMLHttpRequest");
  Object* xp = g_xhr_proto;
  Function* ctor = def_ctor(P, "XMLHttpRequest", 0, xp, [pp](Context&, CallArgs& a) -> Value {
    if (!a.is_construct()) {
      pp->ctx.throw_type_error("Failed to construct 'XMLHttpRequest': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
      return Value::Exception();
    }
    XhrObject* x = pp->ctx.heap.make<XhrObject>();
    x->proto = g_xhr_proto;
    x->resp_headers = new_headers(*pp);
    return Value::Obj(x);
  });
  const char* states[] = {"UNSENT", "OPENED", "HEADERS_RECEIVED", "LOADING", "DONE"};
  for (int i = 0; i < 5; ++i) {
    P.ctx.define_data(ctor, P.ctx.atom_ascii(states[i]), Value::Number(i), 0);
    P.ctx.define_data(xp, P.ctx.atom_ascii(states[i]), Value::Number(i), 0);
  }
  def_method(P, xp, "open", 2, [pp](Context&, CallArgs& a) -> Value {
    XhrObject* x = this_xhr(*pp, a);
    if (!x) return Value::Exception();
    x->method = arg_text(*pp, a, 0);
    for (char& c : x->method) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    x->url = resolve(*pp, arg_text(*pp, a, 1));
    x->async = a.argc < 3 || Context::to_boolean(a[2]);
    x->req_headers.clear();
    x->sent = false;
    x->aborted = false;
    xhr_state(*pp, x, 1);
    return Value::Undefined();
  });
  def_method(P, xp, "setRequestHeader", 2, [pp](Context&, CallArgs& a) -> Value {
    XhrObject* x = this_xhr(*pp, a);
    if (!x) return Value::Exception();
    if (x->ready != 1 || x->sent) return Value::Undefined();
    x->req_headers.emplace_back(arg_text(*pp, a, 0), arg_text(*pp, a, 1));
    return Value::Undefined();
  });
  def_method(P, xp, "overrideMimeType", 1, [pp](Context&, CallArgs& a) -> Value {
    XhrObject* x = this_xhr(*pp, a);
    if (!x) return Value::Exception();
    x->mime = arg_text(*pp, a, 0);
    return Value::Undefined();
  });
  def_method(P, xp, "send", 0, [pp](Context&, CallArgs& a) -> Value {
    XhrObject* x = this_xhr(*pp, a);
    if (!x) return Value::Exception();
    if (x->ready != 1 || x->sent) {
      Object* e = pp->ctx.new_error(js::ErrorKind::Error, "Failed to execute 'send' on 'XMLHttpRequest': The object's state must be OPENED.");
      pp->ctx.define_data(e, pp->ctx.atoms.name, Value::String(pp->ctx.atom_ascii("InvalidStateError")), js::kHiddenAttrs);
      return pp->ctx.throw_value(Value::Obj(e));
    }
    x->sent = true;
    std::string body, ct;
    if (!a[0].is_nullish() && !encode_body(*pp, a[0], &body, &ct)) return Value::Exception();
    web::FetchOptions opt;
    opt.method = x->method;
    opt.body = body;
    bool has_ct = false;
    for (auto& kv : x->req_headers) { opt.headers.push_back(kv); if (lower(kv.first) == "content-type") has_ct = true; }
    if (!body.empty() && !has_ct && !ct.empty()) opt.headers.push_back({"Content-Type", ct});
    bool ok = false;
    std::string line = x->method + " " + x->url;
    if (x->url.compare(0, 5, "data:") == 0) {
      std::string data, type;
      ok = decode_data_url(x->url, &data, &type);
      x->status = ok ? 200 : 0;
      x->response_text = data;
      x->response_url = x->url;
      x->resp_headers->set("content-type", type);
    } else if (pp->fetch) {
      auto got = pp->fetch(x->url, opt);
      if (got.ok()) {
        ok = true;
        x->status = got->status > 0 ? got->status : 200;
        x->response_text.assign(got->body.begin(), got->body.end());
        x->response_url = got->final_url.empty() ? x->url : got->final_url;
        if (!got->content_type.empty()) x->resp_headers->set("content-type", got->content_type);
        x->resp_headers->set("content-length", std::to_string(got->body.size()));
        line += "  -> " + std::to_string(x->status) + ", " + std::to_string(got->body.size()) + " bytes";
      } else {
        line += "  -> " + got.error().message;
      }
    } else {
      line += "  -> no transport";
    }
    pp->net.push_back(line);
    if (pp->net.size() > 200) pp->net.erase(pp->net.begin());
    x->status_text = status_text_for(x->status);
    // the response in the requested shape
    const std::string rt = x->response_type;
    if (rt == "json") {
      bool jok;
      x->response = body_json(*pp, x->response_text, &jok);
      if (!jok) { pp->ctx.clear_exception(); x->response = Value::Null(); }
    } else if (rt == "arraybuffer") {
      x->response = blob_arraybuffer_value(*pp, x->response_text);
    } else if (rt == "blob") {
      x->response = Value::Obj(new_blob(*pp, x->response_text, x->resp_headers->get("content-type")));
    } else {
      x->response = str_val(*pp, x->response_text);
    }
    if (x->async) {
      // events arrive after the current task, so a handler assigned after send() still hears them
      PendingTimer t;
      t.id = pp->next_timer_id++;
      t.fn = Value::Obj(pp->ctx.new_host_function("", [pp, x, ok](Context&, CallArgs&) -> Value {
        xhr_finish(*pp, x, ok);
        return Value::Undefined();
      }, 0));
      pp->add_timer(pp->deferred, t, 2000);
    } else {
      xhr_finish(*pp, x, ok);
    }
    return Value::Undefined();
  });
  def_method(P, xp, "abort", 0, [pp](Context&, CallArgs& a) -> Value {
    XhrObject* x = this_xhr(*pp, a);
    if (!x) return Value::Exception();
    if (x->ready == 0 || x->ready == 4) return Value::Undefined();
    x->aborted = true;
    x->ready = 4;
    xhr_event(*pp, x, "abort");
    xhr_event(*pp, x, "loadend");
    x->ready = 0;
    return Value::Undefined();
  });
  def_method(P, xp, "getResponseHeader", 1, [pp](Context&, CallArgs& a) -> Value {
    XhrObject* x = this_xhr(*pp, a);
    if (!x) return Value::Exception();
    const std::string n = lower(arg_text(*pp, a, 0));
    return x->ready >= 2 && x->resp_headers->has(n) ? str_val(*pp, x->resp_headers->get(n)) : Value::Null();
  });
  def_method(P, xp, "getAllResponseHeaders", 0, [pp](Context&, CallArgs& a) -> Value {
    XhrObject* x = this_xhr(*pp, a);
    if (!x) return Value::Exception();
    std::string out;
    if (x->ready >= 2) for (auto& kv : x->resp_headers->items) out += kv.first + ": " + kv.second + "\r\n";
    return str_val(*pp, out);
  });
  auto xget = [pp](const char* name, std::function<Value(XhrObject*)> f) {
    def_accessor(*pp, g_xhr_proto, name, [pp, f](Context&, CallArgs& a) -> Value {
      XhrObject* x = this_xhr(*pp, a);
      if (!x) return Value::Exception();
      return f(x);
    });
  };
  xget("readyState", [](XhrObject* x) { return Value::Number(x->ready); });
  xget("status", [](XhrObject* x) { return Value::Number(x->ready >= 2 ? x->status : 0); });
  xget("statusText", [pp](XhrObject* x) { return str_val(*pp, x->ready >= 2 ? x->status_text : std::string()); });
  xget("responseText", [pp](XhrObject* x) { return str_val(*pp, x->ready == 4 ? x->response_text : std::string()); });
  xget("responseURL", [pp](XhrObject* x) { return str_val(*pp, x->response_url); });
  xget("response", [pp](XhrObject* x) { return x->ready == 4 ? x->response : str_val(*pp, std::string()); });
  xget("responseXML", [](XhrObject*) { return Value::Null(); });
  xget("upload", [pp](XhrObject*) { return Value::Obj(pp->ctx.new_object(pp->event_target_proto)); });
  def_accessor(P, xp, "responseType", [pp](Context&, CallArgs& a) -> Value { XhrObject* x = this_xhr(*pp, a); return x ? str_val(*pp, x->response_type) : Value::Exception(); },
               [pp](Context&, CallArgs& a) -> Value { XhrObject* x = this_xhr(*pp, a); if (x) x->response_type = pp->text(a[0]); return Value::Undefined(); });
  def_accessor(P, xp, "timeout", [pp](Context&, CallArgs& a) -> Value { XhrObject* x = this_xhr(*pp, a); return x ? Value::Number(x->timeout) : Value::Exception(); },
               [pp](Context&, CallArgs& a) -> Value { XhrObject* x = this_xhr(*pp, a); if (x) x->timeout = pp->number(a[0]); return Value::Undefined(); });
  def_accessor(P, xp, "withCredentials", [](Context&, CallArgs&) -> Value { return Value::Bool(false); }, [](Context&, CallArgs&) -> Value { return Value::Undefined(); });
}

}  // namespace

Value make_response_value(Page& P, int status, const std::string& status_text, const std::string& url, const std::string& body, const std::string& content_type, bool) {
  return Value::Obj(new_response(P, status, status_text, url, body, content_type));
}

void install_net(Page& P) {
  install_headers(P);
  install_blob(P);
  install_form_data(P);
  install_url(P);
  install_abort(P);
  install_response(P);
  install_request(P);
  install_fetch(P);
  install_xhr(P);
}

// ---------------------------------------------------------------------------------------------------
// Web Storage
// ---------------------------------------------------------------------------------------------------

namespace {

struct StorageObject final : HostObject {
  StorageObject() : HostObject(HostKind::Storage) { exotic_props = true; exotic_get = true; }
  std::vector<std::pair<std::string, std::string>> items;
  Page* page = nullptr;
  bool persistent = false;

  void dirty() { if (persistent) page->storage_dirty = true; }
  std::string* find(const std::string& k) {
    for (auto& kv : items) if (kv.first == k) return &kv.second;
    return nullptr;
  }
  static bool method_name(Context& ctx, Object* self, PropKey k) {
    bool has = false;
    self->Object::has_property(ctx, k, &has);
    return has;
  }
  bool get(Context& ctx, PropKey k, const Value& receiver, Value* out) override {
    if (k->kind != js::CellKind::String || method_name(ctx, this, k)) return Object::get(ctx, k, receiver, out);
    std::string* v = find(static_cast<js::JsString*>(k)->utf8());
    *out = v ? Value::String(ctx.new_string_utf8(*v)) : Value::Undefined();
    return true;
  }
  bool set(Context& ctx, PropKey k, const Value& v, const Value& receiver, bool* ok) override {
    if (k->kind != js::CellKind::String || method_name(ctx, this, k)) return Object::set(ctx, k, v, receiver, ok);
    const std::string key = static_cast<js::JsString*>(k)->utf8(), val = page->text(v);
    if (std::string* cur = find(key)) *cur = val;
    else items.emplace_back(key, val);
    dirty();
    *ok = true;
    return true;
  }
  bool has_property(Context& ctx, PropKey k, bool* out) override {
    if (k->kind == js::CellKind::String && find(static_cast<js::JsString*>(k)->utf8())) { *out = true; return true; }
    return Object::has_property(ctx, k, out);
  }
  bool get_own_property(Context& ctx, PropKey k, js::PropDesc* out, bool* found) override {
    if (k->kind == js::CellKind::String)
      if (std::string* v = find(static_cast<js::JsString*>(k)->utf8())) {
        *out = js::PropDesc::Data(Value::String(ctx.new_string_utf8(*v)), js::kDefaultAttrs);
        *found = true;
        return true;
      }
    return Object::get_own_property(ctx, k, out, found);
  }
  bool define_own_property(Context& ctx, PropKey k, const js::PropDesc& d, bool* ok) override {
    if (k->kind == js::CellKind::String && d.has_value) {
      const std::string key = static_cast<js::JsString*>(k)->utf8(), val = page->text(d.value);
      if (std::string* cur = find(key)) *cur = val;
      else items.emplace_back(key, val);
      dirty();
      *ok = true;
      return true;
    }
    return Object::define_own_property(ctx, k, d, ok);
  }
  bool delete_property(Context& ctx, PropKey k, bool* ok) override {
    if (k->kind == js::CellKind::String) {
      const std::string key = static_cast<js::JsString*>(k)->utf8();
      for (size_t i = 0; i < items.size(); ++i)
        if (items[i].first == key) { items.erase(items.begin() + static_cast<long>(i)); dirty(); break; }
      *ok = true;
      return true;
    }
    return Object::delete_property(ctx, k, ok);
  }
  bool own_keys(Context& ctx, std::vector<PropKey>* out) override {
    for (auto& kv : items) out->push_back(ctx.atom_utf8(kv.first));
    return Object::own_keys(ctx, out);
  }
};

StorageObject* this_storage(Page& P, CallArgs& a) { return static_cast<StorageObject*>(this_host(P, a, HostKind::Storage, "Storage")); }

std::string hex_of(const std::string& s) {
  static const char* d = "0123456789abcdef";
  std::string o;
  for (unsigned char c : s) { o += d[c >> 4]; o += d[c & 15]; }
  return o;
}

std::string unhex(const std::string& h) {
  std::string o;
  for (size_t i = 0; i + 1 < h.size(); i += 2) o += static_cast<char>((hexv(h[i]) << 4) | hexv(h[i + 1]));
  return o;
}

}  // namespace

std::string storage_serialise(Page& P) {
  std::string out;
  if (!P.local_storage) return out;
  StorageObject* s = static_cast<StorageObject*>(P.local_storage);
  for (auto& kv : s->items) out += hex_of(kv.first) + " " + hex_of(kv.second) + "\n";
  return out;
}

void storage_restore(Page& P, const std::string& text) {
  if (!P.local_storage) return;
  StorageObject* s = static_cast<StorageObject*>(P.local_storage);
  size_t at = 0;
  while (at < text.size()) {
    size_t nl = text.find('\n', at);
    if (nl == std::string::npos) nl = text.size();
    const std::string line = text.substr(at, nl - at);
    at = nl + 1;
    const size_t sp = line.find(' ');
    if (sp == std::string::npos) continue;
    s->items.emplace_back(unhex(line.substr(0, sp)), unhex(line.substr(sp + 1)));
  }
}

void install_storage(Page& P) {
  Page* pp = &P;
  P.storage_proto = make_proto(P, P.ctx.intr.object_proto, "Storage");
  Object* sp = P.storage_proto;
  def_ctor(P, "Storage", 0, sp, [](Context& c, CallArgs&) -> Value {
    c.throw_type_error("Illegal constructor");
    return Value::Exception();
  });
  def_method(P, sp, "getItem", 1, [pp](Context&, CallArgs& a) -> Value {
    StorageObject* s = this_storage(*pp, a);
    if (!s) return Value::Exception();
    std::string* v = s->find(arg_text(*pp, a, 0));
    return v ? str_val(*pp, *v) : Value::Null();
  });
  def_method(P, sp, "setItem", 2, [pp](Context&, CallArgs& a) -> Value {
    StorageObject* s = this_storage(*pp, a);
    if (!s) return Value::Exception();
    const std::string k = arg_text(*pp, a, 0), v = arg_text(*pp, a, 1);
    if (std::string* cur = s->find(k)) *cur = v;
    else s->items.emplace_back(k, v);
    s->dirty();
    return Value::Undefined();
  });
  def_method(P, sp, "removeItem", 1, [pp](Context&, CallArgs& a) -> Value {
    StorageObject* s = this_storage(*pp, a);
    if (!s) return Value::Exception();
    const std::string k = arg_text(*pp, a, 0);
    for (size_t i = 0; i < s->items.size(); ++i)
      if (s->items[i].first == k) { s->items.erase(s->items.begin() + static_cast<long>(i)); s->dirty(); break; }
    return Value::Undefined();
  });
  def_method(P, sp, "clear", 0, [pp](Context&, CallArgs& a) -> Value {
    StorageObject* s = this_storage(*pp, a);
    if (!s) return Value::Exception();
    s->items.clear();
    s->dirty();
    return Value::Undefined();
  });
  def_method(P, sp, "key", 1, [pp](Context&, CallArgs& a) -> Value {
    StorageObject* s = this_storage(*pp, a);
    if (!s) return Value::Exception();
    const double i = pp->number(a[0]);
    return i >= 0 && i < static_cast<double>(s->items.size()) ? str_val(*pp, s->items[static_cast<size_t>(i)].first) : Value::Null();
  });
  def_accessor(P, sp, "length", [pp](Context&, CallArgs& a) -> Value {
    StorageObject* s = this_storage(*pp, a);
    if (!s) return Value::Exception();
    return Value::Number(static_cast<double>(s->items.size()));
  });
  for (const char* which : {"localStorage", "sessionStorage"}) {
    StorageObject* s = P.ctx.heap.make<StorageObject>();
    s->proto = P.storage_proto;
    s->page = pp;
    s->persistent = std::strcmp(which, "localStorage") == 0;
    if (s->persistent) P.local_storage = s;
    P.ctx.define_data(P.ctx.global(), P.ctx.atom_ascii(which), Value::Obj(s), js::kHiddenAttrs);
  }
}

}  // namespace aios::userland::webjs
