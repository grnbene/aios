// The garbage-collected heap, strings, atoms and the pieces of the object model that do not need a Context.
#include <algorithm>
#include <cstring>

#include "aios/js/context.hpp"

namespace aios::js {

// --- Value ---------------------------------------------------------------------------------------

Value Value::String(JsString* s) { Value v; v.tag = Tag::String; v.cell = reinterpret_cast<Cell*>(s); return v; }
Value Value::Sym(Symbol* s) { Value v; v.tag = Tag::Symbol; v.cell = reinterpret_cast<Cell*>(s); return v; }
Value Value::Big(BigInt* b) { Value v; v.tag = Tag::BigInt; v.cell = reinterpret_cast<Cell*>(b); return v; }
Value Value::Obj(Object* o) { Value v; v.tag = Tag::Object; v.cell = reinterpret_cast<Cell*>(o); return v; }

// --- Tracer --------------------------------------------------------------------------------------

void Tracer::drain() {
  while (!stack_.empty()) {
    Cell* c = stack_.back();
    stack_.pop_back();
    c->trace(*this);
  }
}

// --- strings ---------------------------------------------------------------------------------------

void JsString::flatten() const {
  if (!left) return;
  // Iterative: a rope built by a long `s += x` loop is a left-leaning chain thousands deep.
  std::u16string out;
  out.reserve(length);
  std::vector<const JsString*> stack;
  stack.push_back(this);
  while (!stack.empty()) {
    const JsString* s = stack.back();
    stack.pop_back();
    if (s->left) {
      stack.push_back(s->right);
      stack.push_back(s->left);
    } else {
      out += s->flat;
    }
  }
  flat = std::move(out);
  left = nullptr;
  right = nullptr;
}

uint32_t Heap::hash_units(const char16_t* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

uint32_t JsString::hash() const {
  if (!hashed) {
    const std::u16string& s = str();
    hash_ = Heap::hash_units(s.data(), s.size());
    hashed = true;
  }
  return hash_;
}

int64_t JsString::array_index() const {
  if (index_cache != -2) return index_cache;
  const std::u16string& s = str();
  int64_t r = -1;
  if (!s.empty() && s.size() <= 10) {
    if (s.size() == 1 && s[0] == u'0') r = 0;
    else if (s[0] >= u'1' && s[0] <= u'9') {
      uint64_t v = 0;
      bool ok = true;
      for (char16_t c : s) {
        if (c < u'0' || c > u'9') { ok = false; break; }
        v = v * 10 + static_cast<uint64_t>(c - u'0');
      }
      if (ok && v < 4294967295ull) r = static_cast<int64_t>(v);
    }
  }
  index_cache = r;
  return r;
}

std::string JsString::utf8() const {
  const std::u16string& s = str();
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    uint32_t c = s[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
      c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      ++i;
    } else if (c >= 0xD800 && c < 0xE000) {
      c = 0xFFFD;   // a lone surrogate cannot be UTF-8
    }
    if (c < 0x80) out += static_cast<char>(c);
    else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { out += static_cast<char>(0xE0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
    else { out += static_cast<char>(0xF0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
  }
  return out;
}

void JsString::trace(Tracer& t) {
  if (left) { t.mark(left); t.mark(right); }
}

void Symbol::trace(Tracer& t) { t.mark(description); }

void Env::trace(Tracer& t) {
  t.mark(parent);
  for (const Value& v : slots) t.mark(v);
}

void Object::trace(Tracer& t) {
  t.mark(proto);
  for (const Prop& p : props.all()) {
    t.mark(p.key);
    t.mark(p.value);
    t.mark(p.getter);
    t.mark(p.setter);
  }
  for (const Value& v : elems) t.mark(v);
}

void Function::trace(Tracer& t) {
  Object::trace(t);
  t.mark(reinterpret_cast<Cell*>(code));
  t.mark(env);
  t.mark(this_val);
  t.mark(home_object);
  t.mark(new_target_lexical);
  t.mark(fields_owner);
  t.mark(fields);
  for (const Value& v : slots) t.mark(v);
}

// --- the property table ---------------------------------------------------------------------------

namespace {
constexpr size_t kIndexThreshold = 12;
}

Prop* PropertyTable::find(PropKey k) {
  if (props_.size() <= kIndexThreshold) {
    for (Prop& p : props_)
      if (p.key == k) return &p;
    return nullptr;
  }
  if (index_.empty()) reindex();
  auto it = index_.find(k);
  return it == index_.end() ? nullptr : &props_[it->second];
}

void PropertyTable::reindex() {
  index_.clear();
  index_.reserve(props_.size() * 2);
  for (uint32_t i = 0; i < props_.size(); ++i) index_[props_[i].key] = i;
}

Prop& PropertyTable::add(PropKey k) {
  props_.emplace_back();
  props_.back().key = k;
  if (props_.size() > kIndexThreshold) {
    if (index_.empty()) reindex();
    else index_[k] = static_cast<uint32_t>(props_.size() - 1);
  }
  return props_.back();
}

bool PropertyTable::erase(PropKey k) {
  for (size_t i = 0; i < props_.size(); ++i) {
    if (props_[i].key == k) {
      props_.erase(props_.begin() + static_cast<long>(i));
      if (props_.size() > kIndexThreshold) reindex(); else index_.clear();
      return true;
    }
  }
  return false;
}

// --- the heap --------------------------------------------------------------------------------------

Heap::~Heap() {
  Cell* c = head_;
  while (c) {
    Cell* n = c->gc_next;
    delete c;
    c = n;
  }
}

void Heap::link(Cell* c) {
  c->gc_next = head_;
  head_ = c;
  ++count_;
  bytes_ += 64;
}

JsString* Heap::intern(std::u16string_view s) {
  const uint32_t h = hash_units(s.data(), s.size());
  auto& bucket = atoms_[h];
  for (JsString* a : bucket)
    if (a->str() == s) return a;
  JsString* a = make<JsString>();
  a->flat.assign(s.data(), s.size());
  a->length = static_cast<uint32_t>(s.size());
  a->is_atom = true;
  a->hash_ = h;
  a->hashed = true;
  bucket.push_back(a);
  return a;
}

JsString* Heap::intern_utf8(std::string_view s) {
  std::u16string u;
  u.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = 0xFFFD;
    size_t n = 1;
    if (c < 0x80) cp = c;
    else if ((c & 0xE0) == 0xC0 && i + 1 < s.size() && (s[i + 1] & 0xC0) == 0x80) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); n = 2; if (cp < 0x80) cp = 0xFFFD; }
    else if ((c & 0xF0) == 0xE0 && i + 2 < s.size() && (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80) {
      cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); n = 3;
      if (cp < 0x800 || (cp >= 0xD800 && cp < 0xE000)) cp = 0xFFFD;
    } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size() && (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80 && (s[i + 3] & 0xC0) == 0x80) {
      cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); n = 4;
      if (cp < 0x10000 || cp > 0x10FFFF) cp = 0xFFFD;
    }
    if (cp >= 0x10000) { cp -= 0x10000; u += static_cast<char16_t>(0xD800 + (cp >> 10)); u += static_cast<char16_t>(0xDC00 + (cp & 0x3FF)); }
    else u += static_cast<char16_t>(cp);
    i += n;
  }
  return intern(u);
}

void Heap::set_threshold_after_collect() {
  threshold_ = std::max<size_t>(8u << 20, bytes_ * 2);
}

// Sweep: the caller has already marked every root and drained the tracer.
void Heap::sweep() {
  // Atoms are weak: drop the unmarked ones from the table before their cells are freed.
  for (auto it = atoms_.begin(); it != atoms_.end();) {
    auto& v = it->second;
    v.erase(std::remove_if(v.begin(), v.end(), [](JsString* a) { return !a->marked; }), v.end());
    if (v.empty()) it = atoms_.erase(it); else ++it;
  }
  Cell** link = &head_;
  size_t live_bytes = 0, live = 0;
  while (*link) {
    Cell* c = *link;
    if (c->marked) {
      c->marked = false;
      live_bytes += 64 + c->extra_bytes();
      ++live;
      link = &c->gc_next;
    } else {
      *link = c->gc_next;
      delete c;
    }
  }
  bytes_ = live_bytes;
  count_ = live;
  set_threshold_after_collect();
}

// --- persistent handles ---------------------------------------------------------------------------

Persistent::Persistent(Context* ctx, Value v) : ctx_(ctx) {
  slot_ = ctx->push_persistent(v);
  v_ = v;
}

Persistent::Persistent(Persistent&& o) noexcept : ctx_(o.ctx_), v_(o.v_), slot_(o.slot_) { o.ctx_ = nullptr; }

Persistent& Persistent::operator=(Persistent&& o) noexcept {
  if (this != &o) {
    reset();
    ctx_ = o.ctx_; v_ = o.v_; slot_ = o.slot_;
    o.ctx_ = nullptr;
  }
  return *this;
}

Persistent::~Persistent() { reset(); }

void Persistent::reset() {
  if (ctx_) { ctx_->release_persistent(slot_); ctx_ = nullptr; }
}

Rooted::Rooted(Context& ctx, Value v) : ctx_(ctx) {
  p_ = new Value(v);
  ctx.temp_roots.push_back(p_);
}

Rooted::~Rooted() {
  auto& r = ctx_.temp_roots;
  for (size_t i = r.size(); i-- > 0;) {
    if (r[i] == p_) { r.erase(r.begin() + static_cast<long>(i)); break; }
  }
  delete p_;
}

}  // namespace aios::js
