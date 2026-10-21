// Map, Set, WeakMap, WeakSet, WeakRef and FinalizationRegistry.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>

#include "builtins.hpp"

namespace aios::js {

namespace {

// ---------------------------------------------------------------------------------------------------
// the insertion-ordered table under Map and Set
// ---------------------------------------------------------------------------------------------------

struct Table {
  struct Entry {
    Value key, value;
    bool live = true;
  };
  std::vector<Entry> entries;
  size_t live_count = 0;
  int active_iterators = 0;
  std::unordered_multimap<size_t, uint32_t> index;

  static size_t hash_of(const Value& v) {
    switch (v.tag) {
      case Tag::Undefined: return 1;
      case Tag::Null: return 2;
      case Tag::Boolean: return v.boolean ? 3 : 4;
      case Tag::Number: {
        double d = v.num == 0 ? 0.0 : v.num;
        if (std::isnan(d)) return 5;
        uint64_t bits;
        std::memcpy(&bits, &d, 8);
        return std::hash<uint64_t>()(bits);
      }
      case Tag::String: return v.as_string()->hash() * 31 + 7;
      case Tag::BigInt: return big::hash(v.as_bigint()->n);
      default: return std::hash<const void*>()(v.cell);
    }
  }

  static bool equal(const Value& a, const Value& b) {
    if (a.tag != b.tag) return false;
    switch (a.tag) {
      case Tag::Undefined: case Tag::Null: return true;
      case Tag::Boolean: return a.boolean == b.boolean;
      case Tag::Number: return a.num == b.num || (std::isnan(a.num) && std::isnan(b.num));
      case Tag::String: return a.as_string() == b.as_string() || a.as_string()->str() == b.as_string()->str();
      case Tag::BigInt: return a.cell == b.cell || big::cmp(a.as_bigint()->n, b.as_bigint()->n) == 0;
      default: return a.cell == b.cell;
    }
  }

  int64_t find(const Value& k) const {
    auto range = index.equal_range(hash_of(k));
    for (auto it = range.first; it != range.second; ++it)
      if (entries[it->second].live && equal(entries[it->second].key, k)) return it->second;
    return -1;
  }

  void set(Value k, const Value& v) {
    if (k.is_number() && k.num == 0) k = Value::Number(0);
    int64_t i = find(k);
    if (i >= 0) { entries[static_cast<size_t>(i)].value = v; return; }
    entries.push_back({k, v, true});
    index.emplace(hash_of(k), static_cast<uint32_t>(entries.size() - 1));
    ++live_count;
  }

  bool remove(const Value& k) {
    int64_t i = find(k);
    if (i < 0) return false;
    Entry& e = entries[static_cast<size_t>(i)];
    auto range = index.equal_range(hash_of(k));
    for (auto it = range.first; it != range.second; ++it)
      if (it->second == static_cast<uint32_t>(i)) { index.erase(it); break; }
    e.live = false;
    e.key = Value::Undefined();
    e.value = Value::Undefined();
    --live_count;
    maybe_compact();
    return true;
  }

  void clear() {
    index.clear();
    if (active_iterators == 0) {
      entries.clear();
    } else {
      for (Entry& e : entries) { e.live = false; e.key = e.value = Value::Undefined(); }
    }
    live_count = 0;
  }

  void maybe_compact() {
    if (active_iterators != 0 || entries.size() < 32 || entries.size() < live_count * 2) return;
    std::vector<Entry> kept;
    kept.reserve(live_count);
    for (Entry& e : entries) if (e.live) kept.push_back(e);
    entries.swap(kept);
    index.clear();
    for (size_t i = 0; i < entries.size(); ++i) index.emplace(hash_of(entries[i].key), static_cast<uint32_t>(i));
  }
};

struct CollectionObject final : Object {
  CollectionObject(Class c) : Object(c), table(std::make_shared<Table>()) {}
  std::shared_ptr<Table> table;
  void trace(Tracer& t) override {
    Object::trace(t);
    for (Table::Entry& e : table->entries) { t.mark(e.key); t.mark(e.value); }
  }
  size_t extra_bytes() const override { return Object::extra_bytes() + table->entries.capacity() * sizeof(Table::Entry); }
};

struct CollectionIterator final : Object {
  CollectionIterator(Class c) : Object(c) {}
  ~CollectionIterator() override { release(); }
  std::shared_ptr<Table> table;
  Object* owner = nullptr;
  size_t pos = 0;
  int kind = 0;   // 0 keys, 1 values, 2 entries
  void release() {
    if (table) { --table->active_iterators; table.reset(); }
    owner = nullptr;
  }
  void trace(Tracer& t) override {
    Object::trace(t);
    t.mark(owner);
  }
};

CollectionObject* this_collection(Context& ctx, const Value& v, Class cls, const char* method) {
  if (v.is_object() && v.as_object()->cls == cls) return static_cast<CollectionObject*>(v.as_object());
  ctx.throw_type_error(std::string("Method ") + (cls == Class::Map ? "Map" : "Set") + ".prototype." + method + " called on incompatible receiver " + vm_describe(ctx, v));
  return nullptr;
}

#define THIS_MAP(var, method)                                         \
  CollectionObject* var = this_collection(ctx, a.this_value, Class::Map, method); \
  if (!var) return Value::Exception()
#define THIS_SET(var, method)                                         \
  CollectionObject* var = this_collection(ctx, a.this_value, Class::Set, method); \
  if (!var) return Value::Exception()

Value make_iterator(Context& ctx, CollectionObject* c, int kind, Class iter_cls, Object* proto) {
  CollectionIterator* it = ctx.heap.make<CollectionIterator>(iter_cls);
  it->proto = proto;
  it->table = c->table;
  ++it->table->active_iterators;
  it->owner = c;
  it->kind = kind;
  return Value::Obj(it);
}

Value iterator_next(Context& ctx, CallArgs& a, Class iter_cls, bool is_map) {
  if (!a.this_value.is_object() || a.this_value.as_object()->cls != iter_cls) {
    ctx.throw_type_error("next method called on incompatible receiver " + vm_describe(ctx, a.this_value));
    return Value::Exception();
  }
  CollectionIterator* it = static_cast<CollectionIterator*>(a.this_value.as_object());
  if (!it->table) return Value::Obj(ctx.create_iter_result(Value::Undefined(), true));
  Table& t = *it->table;
  while (it->pos < t.entries.size() && !t.entries[it->pos].live) ++it->pos;
  if (it->pos >= t.entries.size()) {
    it->release();
    return Value::Obj(ctx.create_iter_result(Value::Undefined(), true));
  }
  const Table::Entry& e = t.entries[it->pos++];
  Value out;
  if (it->kind == 0) out = e.key;
  else if (it->kind == 1) out = is_map ? e.value : e.key;
  else out = Value::Obj(ctx.new_array_from({e.key, is_map ? e.value : e.key}));
  return Value::Obj(ctx.create_iter_result(out, false));
}

Value map_iterator_next(Context& ctx, CallArgs& a) { return iterator_next(ctx, a, Class::MapIterator, true); }
Value set_iterator_next(Context& ctx, CallArgs& a) { return iterator_next(ctx, a, Class::SetIterator, false); }

// ---- Map ----

Value map_ctor(Context& ctx, CallArgs& a) {
  if (!a.is_construct()) {
    ctx.throw_type_error("Constructor Map requires 'new'");
    return Value::Exception();
  }
  Object* proto;
  JS_TRY(ctx.prototype_from_ctor(a.new_target, ctx.intr.map_proto, &proto));
  CollectionObject* m = ctx.heap.make<CollectionObject>(Class::Map);
  m->proto = proto;
  if (a[0].is_nullish()) return Value::Obj(m);
  Value adder;
  JS_TRY(m->get(ctx, ctx.atoms.set, Value::Obj(m), &adder));
  if (!adder.is_callable()) {
    ctx.throw_type_error("'" + vm_describe(ctx, adder) + "' returned for property 'set' of object '#<Map>' is not a function");
    return Value::Exception();
  }
  JS_TRY(ctx.iterate(a[0], [&](Value item) {
    if (!item.is_object()) {
      ctx.throw_type_error("Iterator value " + vm_describe(ctx, item) + " is not an entry object");
      return false;
    }
    Value k, v, ignored;
    if (!get_idx(ctx, item.as_object(), 0, &k) || !get_idx(ctx, item.as_object(), 1, &v)) return false;
    return ctx.call(adder, Value::Obj(m), {k, v}, &ignored);
  }));
  return Value::Obj(m);
}

Value map_get(Context& ctx, CallArgs& a) {
  THIS_MAP(m, "get");
  int64_t i = m->table->find(a[0]);
  return i < 0 ? Value::Undefined() : m->table->entries[static_cast<size_t>(i)].value;
}

Value map_set(Context& ctx, CallArgs& a) {
  THIS_MAP(m, "set");
  m->table->set(a[0], a[1]);
  return a.this_value;
}

Value map_has(Context& ctx, CallArgs& a) {
  THIS_MAP(m, "has");
  return Value::Bool(m->table->find(a[0]) >= 0);
}

Value map_delete(Context& ctx, CallArgs& a) {
  THIS_MAP(m, "delete");
  return Value::Bool(m->table->remove(a[0]));
}

Value map_clear(Context& ctx, CallArgs& a) {
  THIS_MAP(m, "clear");
  m->table->clear();
  return Value::Undefined();
}

Value map_size(Context& ctx, CallArgs& a) {
  THIS_MAP(m, "size");
  return Value::Number(static_cast<double>(m->table->live_count));
}

Value map_for_each(Context& ctx, CallArgs& a) {
  THIS_MAP(m, "forEach");
  if (!a[0].is_callable()) {
    ctx.throw_type_error(vm_describe(ctx, a[0]) + " is not a function");
    return Value::Exception();
  }
  std::shared_ptr<Table> t = m->table;
  ++t->active_iterators;
  struct Guard { std::shared_ptr<Table> t; ~Guard() { --t->active_iterators; } } guard{t};
  for (size_t i = 0; i < t->entries.size(); ++i) {
    if (!t->entries[i].live) continue;
    Value k = t->entries[i].key, v = t->entries[i].value, ignored;
    JS_TRY(ctx.call(a[0], a[1], {v, k, a.this_value}, &ignored));
  }
  return Value::Undefined();
}

Value map_keys(Context& ctx, CallArgs& a) { THIS_MAP(m, "keys"); return make_iterator(ctx, m, 0, Class::MapIterator, ctx.intr.map_iterator_proto); }
Value map_values(Context& ctx, CallArgs& a) { THIS_MAP(m, "values"); return make_iterator(ctx, m, 1, Class::MapIterator, ctx.intr.map_iterator_proto); }
Value map_entries(Context& ctx, CallArgs& a) { THIS_MAP(m, "entries"); return make_iterator(ctx, m, 2, Class::MapIterator, ctx.intr.map_iterator_proto); }

Value map_group_by(Context& ctx, CallArgs& a) {
  if (!a[1].is_callable()) {
    ctx.throw_type_error(vm_describe(ctx, a[1]) + " is not a function");
    return Value::Exception();
  }
  CollectionObject* m = ctx.heap.make<CollectionObject>(Class::Map);
  m->proto = ctx.intr.map_proto;
  double idx = 0;
  JS_TRY(ctx.iterate(a[0], [&](Value v) {
    Value key;
    if (!ctx.call(a[1], Value::Undefined(), {v, Value::Number(idx++)}, &key)) return false;
    int64_t i = m->table->find(key);
    if (i < 0) {
      m->table->set(key, Value::Obj(ctx.new_array(0)));
      i = m->table->find(key);
    }
    ArrayObject* arr = static_cast<ArrayObject*>(m->table->entries[static_cast<size_t>(i)].value.as_object());
    arr->elems.push_back(v);
    ++arr->length_;
    return true;
  }));
  return Value::Obj(m);
}

Value species_getter(Context&, CallArgs& a) { return a.this_value; }

// ---- Set ----

Value set_ctor(Context& ctx, CallArgs& a) {
  if (!a.is_construct()) {
    ctx.throw_type_error("Constructor Set requires 'new'");
    return Value::Exception();
  }
  Object* proto;
  JS_TRY(ctx.prototype_from_ctor(a.new_target, ctx.intr.set_proto, &proto));
  CollectionObject* s = ctx.heap.make<CollectionObject>(Class::Set);
  s->proto = proto;
  if (a[0].is_nullish()) return Value::Obj(s);
  Value adder;
  JS_TRY(s->get(ctx, ctx.atoms.add, Value::Obj(s), &adder));
  if (!adder.is_callable()) {
    ctx.throw_type_error("'" + vm_describe(ctx, adder) + "' returned for property 'add' of object '#<Set>' is not a function");
    return Value::Exception();
  }
  JS_TRY(ctx.iterate(a[0], [&](Value item) {
    Value ignored;
    return ctx.call(adder, Value::Obj(s), {item}, &ignored);
  }));
  return Value::Obj(s);
}

Value set_add(Context& ctx, CallArgs& a) {
  THIS_SET(s, "add");
  if (s->table->find(a[0]) < 0) s->table->set(a[0], a[0]);
  return a.this_value;
}

Value set_has(Context& ctx, CallArgs& a) {
  THIS_SET(s, "has");
  return Value::Bool(s->table->find(a[0]) >= 0);
}

Value set_delete(Context& ctx, CallArgs& a) {
  THIS_SET(s, "delete");
  return Value::Bool(s->table->remove(a[0]));
}

Value set_clear(Context& ctx, CallArgs& a) {
  THIS_SET(s, "clear");
  s->table->clear();
  return Value::Undefined();
}

Value set_size(Context& ctx, CallArgs& a) {
  THIS_SET(s, "size");
  return Value::Number(static_cast<double>(s->table->live_count));
}

Value set_for_each(Context& ctx, CallArgs& a) {
  THIS_SET(s, "forEach");
  if (!a[0].is_callable()) {
    ctx.throw_type_error(vm_describe(ctx, a[0]) + " is not a function");
    return Value::Exception();
  }
  std::shared_ptr<Table> t = s->table;
  ++t->active_iterators;
  struct Guard { std::shared_ptr<Table> t; ~Guard() { --t->active_iterators; } } guard{t};
  for (size_t i = 0; i < t->entries.size(); ++i) {
    if (!t->entries[i].live) continue;
    Value k = t->entries[i].key, ignored;
    JS_TRY(ctx.call(a[0], a[1], {k, k, a.this_value}, &ignored));
  }
  return Value::Undefined();
}

Value set_values(Context& ctx, CallArgs& a) { THIS_SET(s, "values"); return make_iterator(ctx, s, 1, Class::SetIterator, ctx.intr.set_iterator_proto); }
Value set_entries(Context& ctx, CallArgs& a) { THIS_SET(s, "entries"); return make_iterator(ctx, s, 2, Class::SetIterator, ctx.intr.set_iterator_proto); }

// The "set-like" protocol of the ES2025 set methods: size, has and keys.
struct SetRecord {
  Value obj;
  double size = 0;
  Value has, keys;
};

bool get_set_record(Context& ctx, const Value& v, SetRecord* out) {
  if (!v.is_object()) {
    ctx.throw_type_error(vm_describe(ctx, v) + " is not an object");
    return false;
  }
  Value raw;
  if (!ctx.get(v, ctx.atoms.size, &raw)) return false;
  double n;
  if (!ctx.to_number(raw, &n)) return false;
  if (std::isnan(n)) {
    ctx.throw_type_error("The 'size' property must be a number");
    return false;
  }
  double sz;
  if (!ctx.to_integer(Value::Number(n), &sz)) return false;
  if (sz < 0) {
    ctx.throw_range_error("'" + ctx.number_to_string(sz)->utf8() + "' is an invalid size");
    return false;
  }
  Value has, keys;
  if (!ctx.get(v, ctx.atoms.has, &has)) return false;
  if (!has.is_callable()) { ctx.throw_type_error("The 'has' property must be a function"); return false; }
  if (!ctx.get(v, ctx.atoms.keys, &keys)) return false;
  if (!keys.is_callable()) { ctx.throw_type_error("The 'keys' property must be a function"); return false; }
  out->obj = v;
  out->size = sz;
  out->has = has;
  out->keys = keys;
  return true;
}

// Iterate other.keys(); fn returns false to stop (and the iterator is closed).
bool iterate_keys(Context& ctx, const SetRecord& r, const std::function<bool(Value)>& fn) {
  Value it;
  if (!ctx.call(r.keys, r.obj, nullptr, 0, &it)) return false;
  if (!it.is_object()) {
    ctx.throw_type_error(vm_describe(ctx, it) + " is not an object");
    return false;
  }
  Value next;
  if (!ctx.get(it, ctx.atoms.next, &next)) return false;
  for (;;) {
    Value res;
    if (!ctx.call(next, it, nullptr, 0, &res)) return false;
    if (!res.is_object()) {
      ctx.throw_type_error("Iterator result " + vm_describe(ctx, res) + " is not an object");
      return false;
    }
    Value done;
    if (!ctx.get(res, ctx.atoms.done, &done)) return false;
    if (Context::to_boolean(done)) return true;
    Value v;
    if (!ctx.get(res, ctx.atoms.value, &v)) return false;
    if (!fn(v)) {
      Value ret;
      if (!ctx.get_method(it, ctx.atoms.return_, &ret)) return false;
      if (!ret.is_undefined()) {
        Value ignored;
        if (!ctx.call(ret, it, nullptr, 0, &ignored)) return false;
      }
      return true;
    }
  }
}

CollectionObject* new_set(Context& ctx) {
  CollectionObject* s = ctx.heap.make<CollectionObject>(Class::Set);
  s->proto = ctx.intr.set_proto;
  return s;
}

std::vector<Value> live_keys(const CollectionObject* s) {
  std::vector<Value> out;
  for (const Table::Entry& e : s->table->entries) if (e.live) out.push_back(e.key);
  return out;
}

Value set_union(Context& ctx, CallArgs& a) {
  THIS_SET(s, "union");
  SetRecord other;
  JS_TRY(get_set_record(ctx, a[0], &other));
  CollectionObject* r = new_set(ctx);
  for (const Value& k : live_keys(s)) r->table->set(k, k);
  JS_TRY(iterate_keys(ctx, other, [&](Value v) { r->table->set(v, v); return true; }));
  return Value::Obj(r);
}

Value set_intersection(Context& ctx, CallArgs& a) {
  THIS_SET(s, "intersection");
  SetRecord other;
  JS_TRY(get_set_record(ctx, a[0], &other));
  CollectionObject* r = new_set(ctx);
  if (static_cast<double>(s->table->live_count) <= other.size) {
    for (const Value& k : live_keys(s)) {
      Value in;
      JS_TRY(ctx.call(other.has, other.obj, {k}, &in));
      if (Context::to_boolean(in)) r->table->set(k, k);
    }
  } else {
    JS_TRY(iterate_keys(ctx, other, [&](Value v) {
      if (s->table->find(v) >= 0) r->table->set(v, v);
      return true;
    }));
  }
  return Value::Obj(r);
}

Value set_difference(Context& ctx, CallArgs& a) {
  THIS_SET(s, "difference");
  SetRecord other;
  JS_TRY(get_set_record(ctx, a[0], &other));
  CollectionObject* r = new_set(ctx);
  for (const Value& k : live_keys(s)) r->table->set(k, k);
  if (static_cast<double>(s->table->live_count) <= other.size) {
    for (const Value& k : live_keys(s)) {
      Value in;
      JS_TRY(ctx.call(other.has, other.obj, {k}, &in));
      if (Context::to_boolean(in)) r->table->remove(k);
    }
  } else {
    JS_TRY(iterate_keys(ctx, other, [&](Value v) { r->table->remove(v); return true; }));
  }
  return Value::Obj(r);
}

Value set_symmetric_difference(Context& ctx, CallArgs& a) {
  THIS_SET(s, "symmetricDifference");
  SetRecord other;
  JS_TRY(get_set_record(ctx, a[0], &other));
  CollectionObject* r = new_set(ctx);
  for (const Value& k : live_keys(s)) r->table->set(k, k);
  JS_TRY(iterate_keys(ctx, other, [&](Value v) {
    if (s->table->find(v) >= 0) r->table->remove(v);
    else r->table->set(v, v);
    return true;
  }));
  return Value::Obj(r);
}

Value set_is_subset_of(Context& ctx, CallArgs& a) {
  THIS_SET(s, "isSubsetOf");
  SetRecord other;
  JS_TRY(get_set_record(ctx, a[0], &other));
  if (static_cast<double>(s->table->live_count) > other.size) return Value::Bool(false);
  for (const Value& k : live_keys(s)) {
    Value in;
    JS_TRY(ctx.call(other.has, other.obj, {k}, &in));
    if (!Context::to_boolean(in)) return Value::Bool(false);
  }
  return Value::Bool(true);
}

Value set_is_superset_of(Context& ctx, CallArgs& a) {
  THIS_SET(s, "isSupersetOf");
  SetRecord other;
  JS_TRY(get_set_record(ctx, a[0], &other));
  if (static_cast<double>(s->table->live_count) < other.size) return Value::Bool(false);
  bool result = true;
  JS_TRY(iterate_keys(ctx, other, [&](Value v) {
    if (s->table->find(v) < 0) { result = false; return false; }
    return true;
  }));
  return Value::Bool(result);
}

Value set_is_disjoint_from(Context& ctx, CallArgs& a) {
  THIS_SET(s, "isDisjointFrom");
  SetRecord other;
  JS_TRY(get_set_record(ctx, a[0], &other));
  bool result = true;
  if (static_cast<double>(s->table->live_count) <= other.size) {
    for (const Value& k : live_keys(s)) {
      Value in;
      JS_TRY(ctx.call(other.has, other.obj, {k}, &in));
      if (Context::to_boolean(in)) return Value::Bool(false);
    }
  } else {
    JS_TRY(iterate_keys(ctx, other, [&](Value v) {
      if (s->table->find(v) >= 0) { result = false; return false; }
      return true;
    }));
  }
  return Value::Bool(result);
}

// ---------------------------------------------------------------------------------------------------
// weak collections
// ---------------------------------------------------------------------------------------------------

bool can_be_held_weakly(const Value& v) {
  if (v.is_object()) return true;
  return v.is_symbol() && !v.as_symbol()->registered;
}

// Keys are weak, values are ephemerons: a value is kept alive only through its key.
struct WeakTable final : Object {
  WeakTable(Class c, Heap* h) : Object(c), heap(h) { h->add_weak(this); }
  ~WeakTable() override { heap->remove_weak(this); }
  Heap* heap;
  std::unordered_map<Cell*, Value> map;
  void trace(Tracer& t) override { Object::trace(t); }   // neither keys nor values are strong
  bool trace_weak(Tracer& t) override {
    bool marked_new = false;
    for (auto& kv : map) {
      if (kv.first->marked && kv.second.is_cell() && !kv.second.cell->marked) { t.mark(kv.second); marked_new = true; }
    }
    return marked_new;
  }
  void sweep_weak(Context&) override {
    for (auto it = map.begin(); it != map.end();) {
      if (!it->first->marked) it = map.erase(it);
      else ++it;
    }
  }
};

WeakTable* this_weak(Context& ctx, const Value& v, Class cls, const char* method) {
  if (v.is_object() && v.as_object()->cls == cls) return static_cast<WeakTable*>(v.as_object());
  ctx.throw_type_error(std::string("Method ") + (cls == Class::WeakMap ? "WeakMap" : "WeakSet") + ".prototype." + method + " called on incompatible receiver " + vm_describe(ctx, v));
  return nullptr;
}

Value weakmap_ctor(Context& ctx, CallArgs& a) {
  if (!a.is_construct()) {
    ctx.throw_type_error("Constructor WeakMap requires 'new'");
    return Value::Exception();
  }
  Object* proto;
  JS_TRY(ctx.prototype_from_ctor(a.new_target, ctx.intr.weakmap_proto, &proto));
  WeakTable* m = ctx.heap.make<WeakTable>(Class::WeakMap, &ctx.heap);
  m->proto = proto;
  if (a[0].is_nullish()) return Value::Obj(m);
  Value adder;
  JS_TRY(m->get(ctx, ctx.atoms.set, Value::Obj(m), &adder));
  if (!adder.is_callable()) {
    ctx.throw_type_error("'set' is not a function");
    return Value::Exception();
  }
  JS_TRY(ctx.iterate(a[0], [&](Value item) {
    if (!item.is_object()) {
      ctx.throw_type_error("Iterator value " + vm_describe(ctx, item) + " is not an entry object");
      return false;
    }
    Value k, v, ignored;
    if (!get_idx(ctx, item.as_object(), 0, &k) || !get_idx(ctx, item.as_object(), 1, &v)) return false;
    return ctx.call(adder, Value::Obj(m), {k, v}, &ignored);
  }));
  return Value::Obj(m);
}

Value weakmap_get(Context& ctx, CallArgs& a) {
  WeakTable* m = this_weak(ctx, a.this_value, Class::WeakMap, "get");
  if (!m) return Value::Exception();
  if (!can_be_held_weakly(a[0])) return Value::Undefined();
  auto it = m->map.find(a[0].cell);
  return it == m->map.end() ? Value::Undefined() : it->second;
}

Value weakmap_set(Context& ctx, CallArgs& a) {
  WeakTable* m = this_weak(ctx, a.this_value, Class::WeakMap, "set");
  if (!m) return Value::Exception();
  if (!can_be_held_weakly(a[0])) {
    ctx.throw_type_error("Invalid value used as weak map key");
    return Value::Exception();
  }
  m->map[a[0].cell] = a[1];
  return a.this_value;
}

Value weakmap_has(Context& ctx, CallArgs& a) {
  WeakTable* m = this_weak(ctx, a.this_value, Class::WeakMap, "has");
  if (!m) return Value::Exception();
  return Value::Bool(can_be_held_weakly(a[0]) && m->map.count(a[0].cell));
}

Value weakmap_delete(Context& ctx, CallArgs& a) {
  WeakTable* m = this_weak(ctx, a.this_value, Class::WeakMap, "delete");
  if (!m) return Value::Exception();
  return Value::Bool(can_be_held_weakly(a[0]) && m->map.erase(a[0].cell) > 0);
}

Value weakset_ctor(Context& ctx, CallArgs& a) {
  if (!a.is_construct()) {
    ctx.throw_type_error("Constructor WeakSet requires 'new'");
    return Value::Exception();
  }
  Object* proto;
  JS_TRY(ctx.prototype_from_ctor(a.new_target, ctx.intr.weakset_proto, &proto));
  WeakTable* s = ctx.heap.make<WeakTable>(Class::WeakSet, &ctx.heap);
  s->proto = proto;
  if (a[0].is_nullish()) return Value::Obj(s);
  Value adder;
  JS_TRY(s->get(ctx, ctx.atoms.add, Value::Obj(s), &adder));
  if (!adder.is_callable()) {
    ctx.throw_type_error("'add' is not a function");
    return Value::Exception();
  }
  JS_TRY(ctx.iterate(a[0], [&](Value item) {
    Value ignored;
    return ctx.call(adder, Value::Obj(s), {item}, &ignored);
  }));
  return Value::Obj(s);
}

Value weakset_add(Context& ctx, CallArgs& a) {
  WeakTable* s = this_weak(ctx, a.this_value, Class::WeakSet, "add");
  if (!s) return Value::Exception();
  if (!can_be_held_weakly(a[0])) {
    ctx.throw_type_error("Invalid value used in weak set");
    return Value::Exception();
  }
  s->map[a[0].cell] = Value::Bool(true);
  return a.this_value;
}

Value weakset_has(Context& ctx, CallArgs& a) {
  WeakTable* s = this_weak(ctx, a.this_value, Class::WeakSet, "has");
  if (!s) return Value::Exception();
  return Value::Bool(can_be_held_weakly(a[0]) && s->map.count(a[0].cell));
}

Value weakset_delete(Context& ctx, CallArgs& a) {
  WeakTable* s = this_weak(ctx, a.this_value, Class::WeakSet, "delete");
  if (!s) return Value::Exception();
  return Value::Bool(can_be_held_weakly(a[0]) && s->map.erase(a[0].cell) > 0);
}

// ---- WeakRef ----

struct WeakRefObject final : Object {
  WeakRefObject(Heap* h) : Object(Class::WeakRef), heap(h) { h->add_weak(this); }
  ~WeakRefObject() override { heap->remove_weak(this); }
  Heap* heap;
  Cell* target = nullptr;
  bool is_symbol = false;
  void sweep_weak(Context&) override {
    if (target && !target->marked) target = nullptr;
  }
};

Value weakref_ctor(Context& ctx, CallArgs& a) {
  if (!a.is_construct()) {
    ctx.throw_type_error("Constructor WeakRef requires 'new'");
    return Value::Exception();
  }
  if (!can_be_held_weakly(a[0])) {
    ctx.throw_type_error("WeakRef: invalid target");
    return Value::Exception();
  }
  Object* proto;
  JS_TRY(ctx.prototype_from_ctor(a.new_target, ctx.intr.weakref_proto, &proto));
  WeakRefObject* r = ctx.heap.make<WeakRefObject>(&ctx.heap);
  r->proto = proto;
  r->target = a[0].cell;
  r->is_symbol = a[0].is_symbol();
  ctx.kept_alive.push_back(a[0]);
  return Value::Obj(r);
}

Value weakref_deref(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object() || a.this_value.as_object()->cls != Class::WeakRef) {
    ctx.throw_type_error("Method WeakRef.prototype.deref called on incompatible receiver " + vm_describe(ctx, a.this_value));
    return Value::Exception();
  }
  WeakRefObject* r = static_cast<WeakRefObject*>(a.this_value.as_object());
  if (!r->target) return Value::Undefined();
  Value v = r->is_symbol ? Value::Sym(static_cast<Symbol*>(r->target)) : Value::Obj(static_cast<Object*>(r->target));
  ctx.kept_alive.push_back(v);
  return v;
}

// ---- FinalizationRegistry ----

struct FinalizationRegistryObject final : Object {
  FinalizationRegistryObject(Heap* h) : Object(Class::FinalizationRegistry), heap(h) { h->add_weak(this); }
  ~FinalizationRegistryObject() override { heap->remove_weak(this); }
  Heap* heap;
  Value cleanup;
  struct Cell_ { Cell* target; Value held; Cell* token; };
  std::vector<Cell_> cells;
  void trace(Tracer& t) override {
    Object::trace(t);
    t.mark(cleanup);
    for (Cell_& c : cells) t.mark(c.held);
  }
  void sweep_weak(Context& ctx) override {
    std::vector<Cell_> keep;
    for (Cell_& c : cells) {
      if (c.token && !c.token->marked) c.token = nullptr;
      if (c.target->marked) keep.push_back(c);
      else ctx.enqueue_job_value(cleanup, {c.held});
    }
    cells.swap(keep);
  }
};

Value finreg_ctor(Context& ctx, CallArgs& a) {
  if (!a.is_construct()) {
    ctx.throw_type_error("Constructor FinalizationRegistry requires 'new'");
    return Value::Exception();
  }
  if (!a[0].is_callable()) {
    ctx.throw_type_error("FinalizationRegistry: cleanup must be callable");
    return Value::Exception();
  }
  Object* proto;
  JS_TRY(ctx.prototype_from_ctor(a.new_target, ctx.intr.finreg_proto, &proto));
  FinalizationRegistryObject* r = ctx.heap.make<FinalizationRegistryObject>(&ctx.heap);
  r->proto = proto;
  r->cleanup = a[0];
  return Value::Obj(r);
}

FinalizationRegistryObject* this_finreg(Context& ctx, const Value& v) {
  if (v.is_object() && v.as_object()->cls == Class::FinalizationRegistry) return static_cast<FinalizationRegistryObject*>(v.as_object());
  ctx.throw_type_error("Method FinalizationRegistry.prototype method called on incompatible receiver " + vm_describe(ctx, v));
  return nullptr;
}

Value finreg_register(Context& ctx, CallArgs& a) {
  FinalizationRegistryObject* r = this_finreg(ctx, a.this_value);
  if (!r) return Value::Exception();
  if (!can_be_held_weakly(a[0])) {
    ctx.throw_type_error("FinalizationRegistry.prototype.register: invalid target");
    return Value::Exception();
  }
  if (ctx.same_value(a[0], a[1])) {
    ctx.throw_type_error("FinalizationRegistry.prototype.register: target and holdings must not be same");
    return Value::Exception();
  }
  if (!a[2].is_undefined() && !can_be_held_weakly(a[2])) {
    ctx.throw_type_error("FinalizationRegistry.prototype.register: invalid unregister token");
    return Value::Exception();
  }
  r->cells.push_back({a[0].cell, a[1], a[2].is_undefined() ? nullptr : a[2].cell});
  return Value::Undefined();
}

Value finreg_unregister(Context& ctx, CallArgs& a) {
  FinalizationRegistryObject* r = this_finreg(ctx, a.this_value);
  if (!r) return Value::Exception();
  if (!can_be_held_weakly(a[0])) {
    ctx.throw_type_error("Invalid unregisterToken ('" + vm_describe(ctx, a[0]) + "')");
    return Value::Exception();
  }
  bool removed = false;
  for (size_t i = r->cells.size(); i-- > 0;)
    if (r->cells[i].token == a[0].cell) { r->cells.erase(r->cells.begin() + static_cast<long>(i)); removed = true; }
  return Value::Bool(removed);
}

}  // namespace

void setup_collections(Context& ctx) {
  // Map
  Object* mp = ctx.new_object();
  ctx.intr.map_proto = mp;
  Function* mc = new_ctor(ctx, "Map", map_ctor, 0, mp);
  ctx.define_method(mc, "groupBy", map_group_by, 2);
  ctx.define_getter(mc, ctx.sym.species, "[Symbol.species]", species_getter);
  ctx.define_method(mp, "clear", map_clear, 0);
  ctx.define_method(mp, "delete", map_delete, 1);
  Function* entries = ctx.define_method(mp, "entries", map_entries, 0);
  ctx.define_method(mp, "forEach", map_for_each, 1);
  ctx.define_method(mp, "get", map_get, 1);
  ctx.define_method(mp, "has", map_has, 1);
  ctx.define_method(mp, "keys", map_keys, 0);
  ctx.define_method(mp, "set", map_set, 2);
  ctx.define_getter(mp, "size", map_size);
  ctx.define_method(mp, "values", map_values, 0);
  ctx.define_data(mp, ctx.sym.iterator, Value::Obj(entries), kHiddenAttrs);
  ctx.define_data(mp, ctx.sym.to_string_tag, Str(ctx, "Map"), kConfigurable);
  Object* mip = ctx.new_object(ctx.intr.iterator_proto);
  ctx.intr.map_iterator_proto = mip;
  ctx.define_method(mip, "next", map_iterator_next, 0);
  ctx.define_data(mip, ctx.sym.to_string_tag, Str(ctx, "Map Iterator"), kConfigurable);

  // Set
  Object* sp = ctx.new_object();
  ctx.intr.set_proto = sp;
  Function* sc = new_ctor(ctx, "Set", set_ctor, 0, sp);
  ctx.define_getter(sc, ctx.sym.species, "[Symbol.species]", species_getter);
  ctx.define_method(sp, "add", set_add, 1);
  ctx.define_method(sp, "clear", set_clear, 0);
  ctx.define_method(sp, "delete", set_delete, 1);
  ctx.define_method(sp, "difference", set_difference, 1);
  ctx.define_method(sp, "entries", set_entries, 0);
  ctx.define_method(sp, "forEach", set_for_each, 1);
  ctx.define_method(sp, "has", set_has, 1);
  ctx.define_method(sp, "intersection", set_intersection, 1);
  ctx.define_method(sp, "isDisjointFrom", set_is_disjoint_from, 1);
  ctx.define_method(sp, "isSubsetOf", set_is_subset_of, 1);
  ctx.define_method(sp, "isSupersetOf", set_is_superset_of, 1);
  ctx.define_getter(sp, "size", set_size);
  ctx.define_method(sp, "symmetricDifference", set_symmetric_difference, 1);
  ctx.define_method(sp, "union", set_union, 1);
  Function* values = ctx.define_method(sp, "values", set_values, 0);
  ctx.define_data(sp, ctx.atoms.keys, Value::Obj(values), kHiddenAttrs);
  ctx.define_data(sp, ctx.sym.iterator, Value::Obj(values), kHiddenAttrs);
  ctx.define_data(sp, ctx.sym.to_string_tag, Str(ctx, "Set"), kConfigurable);
  Object* sip = ctx.new_object(ctx.intr.iterator_proto);
  ctx.intr.set_iterator_proto = sip;
  ctx.define_method(sip, "next", set_iterator_next, 0);
  ctx.define_data(sip, ctx.sym.to_string_tag, Str(ctx, "Set Iterator"), kConfigurable);

  // WeakMap / WeakSet
  Object* wmp = ctx.new_object();
  ctx.intr.weakmap_proto = wmp;
  new_ctor(ctx, "WeakMap", weakmap_ctor, 0, wmp);
  ctx.define_method(wmp, "delete", weakmap_delete, 1);
  ctx.define_method(wmp, "get", weakmap_get, 1);
  ctx.define_method(wmp, "has", weakmap_has, 1);
  ctx.define_method(wmp, "set", weakmap_set, 2);
  ctx.define_data(wmp, ctx.sym.to_string_tag, Str(ctx, "WeakMap"), kConfigurable);
  Object* wsp = ctx.new_object();
  ctx.intr.weakset_proto = wsp;
  new_ctor(ctx, "WeakSet", weakset_ctor, 0, wsp);
  ctx.define_method(wsp, "add", weakset_add, 1);
  ctx.define_method(wsp, "delete", weakset_delete, 1);
  ctx.define_method(wsp, "has", weakset_has, 1);
  ctx.define_data(wsp, ctx.sym.to_string_tag, Str(ctx, "WeakSet"), kConfigurable);

  // WeakRef / FinalizationRegistry
  Object* wrp = ctx.new_object();
  ctx.intr.weakref_proto = wrp;
  new_ctor(ctx, "WeakRef", weakref_ctor, 1, wrp);
  ctx.define_method(wrp, "deref", weakref_deref, 0);
  ctx.define_data(wrp, ctx.sym.to_string_tag, Str(ctx, "WeakRef"), kConfigurable);
  Object* frp = ctx.new_object();
  ctx.intr.finreg_proto = frp;
  new_ctor(ctx, "FinalizationRegistry", finreg_ctor, 1, frp);
  ctx.define_method(frp, "register", finreg_register, 2);
  ctx.define_method(frp, "unregister", finreg_unregister, 1);
  ctx.define_data(frp, ctx.sym.to_string_tag, Str(ctx, "FinalizationRegistry"), kConfigurable);
}

}  // namespace aios::js
