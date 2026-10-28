// Array, and the array iterator.
#include <algorithm>
#include <cmath>

#include "builtins.hpp"

namespace aios::js {

namespace {

struct ArrayIteratorObject final : Object {
  ArrayIteratorObject() : Object(Class::ArrayIterator) {}
  Object* target = nullptr;
  uint64_t index = 0;
  int kind = 1;   // 0 keys, 1 values, 2 entries
  bool done = false;
  void trace(Tracer& t) override { Object::trace(t); t.mark(target); }
};

bool callable_arg(Context& ctx, const Value& f) {
  if (f.is_callable()) return true;
  ctx.throw_type_error(vm_describe(ctx, f) + " is not a function");
  return false;
}

// Hand a computed list to the array a species constructor produced: a fresh plain array takes it directly.
bool deliver(Context& ctx, Object* result, std::vector<Value>& vals, uint64_t length) {
  if (result->cls == Class::Array) {
    ArrayObject* arr = static_cast<ArrayObject*>(result);
    if (!arr->slow && arr->elems.empty() && arr->extensible && arr->length_writable && arr->proto == ctx.intr.array_proto) {
      arr->elems = std::move(vals);
      arr->length_ = static_cast<uint32_t>(length);
      return true;
    }
  }
  for (size_t i = 0; i < vals.size(); ++i)
    if (!vals[i].is_empty() && !create_idx(ctx, result, i, vals[i])) return false;
  return set_length(ctx, result, length);
}

bool this_array_like(Context& ctx, const Value& v, Object** o, uint64_t* len) {
  if (!ctx.to_object(v, o)) return false;
  return ctx.length_of(*o, len);
}

// ---- constructor and statics ----

Value array_ctor(Context& ctx, CallArgs& a) {
  Value nt = a.is_construct() ? a.new_target : Value::Obj(a.callee);
  Object* proto;
  JS_TRY(ctx.prototype_from_ctor(nt, ctx.intr.array_proto, &proto));
  ArrayObject* arr = ctx.new_array(0);
  arr->proto = proto;
  if (a.argc == 1) {
    if (a[0].is_number()) {
      double d = a[0].num;
      if (d < 0 || d != std::floor(d) || d > 4294967295.0) {
        ctx.throw_range_error("Invalid array length");
        return Value::Exception();
      }
      arr->length_ = static_cast<uint32_t>(d);
      return Value::Obj(arr);
    }
    arr->elems.push_back(a[0]);
    arr->length_ = 1;
    return Value::Obj(arr);
  }
  for (int i = 0; i < a.argc; ++i) arr->elems.push_back(a.argv[i]);
  arr->length_ = static_cast<uint32_t>(a.argc);
  return Value::Obj(arr);
}

Value array_is_array(Context& ctx, CallArgs& a) {
  bool r;
  JS_TRY(ctx.is_array(a[0], &r));
  return Value::Bool(r);
}

Value array_of(Context& ctx, CallArgs& a) {
  Object* result;
  if (a.this_value.is_object() && a.this_value.as_object()->is_constructor && a.this_value.as_object() != ctx.intr.array_ctor) {
    Value n = Value::Number(a.argc);
    Value r;
    JS_TRY(ctx.construct(a.this_value, &n, 1, a.this_value, &r));
    result = r.as_object();
  } else {
    result = ctx.new_array(0);
  }
  for (int i = 0; i < a.argc; ++i) JS_TRY(create_idx(ctx, result, static_cast<uint64_t>(i), a.argv[i]));
  JS_TRY(set_length(ctx, result, static_cast<uint64_t>(a.argc)));
  return Value::Obj(result);
}

Value array_from(Context& ctx, CallArgs& a) {
  Value items = a[0], mapfn = a[1], this_arg = a[2];
  const bool mapping = !mapfn.is_undefined();
  if (mapping && !callable_arg(ctx, mapfn)) return Value::Exception();
  const bool custom = a.this_value.is_object() && a.this_value.as_object()->is_constructor && a.this_value.as_object() != ctx.intr.array_ctor;
  Value using_iter;
  JS_TRY(ctx.get_method(items, ctx.sym.iterator, &using_iter));
  if (!using_iter.is_undefined()) {
    Object* result;
    if (custom) {
      Value r;
      JS_TRY(ctx.construct(a.this_value, nullptr, 0, a.this_value, &r));
      result = r.as_object();
    } else {
      result = ctx.new_array(0);
    }
    uint64_t k = 0;
    JS_TRY(ctx.iterate(items, [&](Value v) {
      Value mapped = v;
      if (mapping && !ctx.call(mapfn, this_arg, {v, Value::Number(static_cast<double>(k))}, &mapped)) return false;
      if (!create_idx(ctx, result, k, mapped)) return false;
      ++k;
      return true;
    }));
    JS_TRY(set_length(ctx, result, k));
    return Value::Obj(result);
  }
  Object* src;
  JS_TRY(ctx.to_object(items, &src));
  uint64_t len;
  JS_TRY(ctx.length_of(src, &len));
  Object* result;
  if (custom) {
    Value n = Value::Number(static_cast<double>(len)), r;
    JS_TRY(ctx.construct(a.this_value, &n, 1, a.this_value, &r));
    result = r.as_object();
  } else {
    result = ctx.new_array(0);
  }
  for (uint64_t k = 0; k < len; ++k) {
    Value v;
    JS_TRY(get_idx(ctx, src, k, &v));
    if (mapping) {
      Value m;
      JS_TRY(ctx.call(mapfn, this_arg, {v, Value::Number(static_cast<double>(k))}, &m));
      v = m;
    }
    JS_TRY(create_idx(ctx, result, k, v));
  }
  JS_TRY(set_length(ctx, result, len));
  return Value::Obj(result);
}

Value array_species(Context&, CallArgs& a) { return a.this_value; }

// ---- mutators ----

Value array_push(Context& ctx, CallArgs& a) {
  Object* o;
  JS_TRY(ctx.to_object(a.this_value, &o));
  if (o->cls == Class::Array && is_dense_array(o) && o->extensible && static_cast<ArrayObject*>(o)->length_writable && !ctx.intr.array_proto->has_index_props) {
    ArrayObject* arr = static_cast<ArrayObject*>(o);
    if (static_cast<uint64_t>(arr->length_) + static_cast<uint64_t>(a.argc) > 4294967295ull) {
      ctx.throw_range_error("Invalid array length");
      return Value::Exception();
    }
    for (int i = 0; i < a.argc; ++i) arr->elems.push_back(a.argv[i]);
    arr->length_ += static_cast<uint32_t>(a.argc);
    return Value::Number(arr->length_);
  }
  uint64_t len;
  JS_TRY(ctx.length_of(o, &len));
  if (len + static_cast<uint64_t>(a.argc) > 9007199254740991ull) {
    ctx.throw_type_error("Pushing " + std::to_string(a.argc) + " elements on an array-like of length " + std::to_string(len) + " is disallowed, as the total surpasses 2**53-1");
    return Value::Exception();
  }
  for (int i = 0; i < a.argc; ++i) JS_TRY(set_idx(ctx, o, len + static_cast<uint64_t>(i), a.argv[i]));
  JS_TRY(set_length(ctx, o, len + static_cast<uint64_t>(a.argc)));
  return Value::Number(static_cast<double>(len + static_cast<uint64_t>(a.argc)));
}

Value array_pop(Context& ctx, CallArgs& a) {
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  if (len == 0) {
    JS_TRY(set_length(ctx, o, 0));
    return Value::Undefined();
  }
  if (is_dense_array(o) && static_cast<ArrayObject*>(o)->length_writable && !static_cast<ArrayObject*>(o)->elems.back().is_empty()) {
    ArrayObject* arr = static_cast<ArrayObject*>(o);
    Value v = arr->elems.back();
    arr->elems.pop_back();
    --arr->length_;
    return v;
  }
  Value v;
  JS_TRY(get_idx(ctx, o, len - 1, &v));
  JS_TRY(delete_idx(ctx, o, len - 1));
  JS_TRY(set_length(ctx, o, len - 1));
  return v;
}

Value array_shift(Context& ctx, CallArgs& a) {
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  if (len == 0) {
    JS_TRY(set_length(ctx, o, 0));
    return Value::Undefined();
  }
  if (is_dense_array(o) && static_cast<ArrayObject*>(o)->length_writable && !static_cast<ArrayObject*>(o)->elems.front().is_empty()) {
    ArrayObject* arr = static_cast<ArrayObject*>(o);
    bool holes = false;
    for (const Value& v : arr->elems) if (v.is_empty()) { holes = true; break; }
    if (!holes) {
      Value first = arr->elems.front();
      arr->elems.erase(arr->elems.begin());
      --arr->length_;
      return first;
    }
  }
  Value first;
  JS_TRY(get_idx(ctx, o, 0, &first));
  for (uint64_t k = 1; k < len; ++k) {
    bool has;
    JS_TRY(has_idx(ctx, o, k, &has));
    if (has) {
      Value v;
      JS_TRY(get_idx(ctx, o, k, &v));
      JS_TRY(set_idx(ctx, o, k - 1, v));
    } else {
      JS_TRY(delete_idx(ctx, o, k - 1));
    }
  }
  JS_TRY(delete_idx(ctx, o, len - 1));
  JS_TRY(set_length(ctx, o, len - 1));
  return first;
}

Value array_unshift(Context& ctx, CallArgs& a) {
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  const uint64_t n = static_cast<uint64_t>(a.argc);
  if (n > 0) {
    if (len + n > 9007199254740991ull) {
      ctx.throw_type_error("Invalid array length");
      return Value::Exception();
    }
    if (is_dense_array(o) && o->extensible && static_cast<ArrayObject*>(o)->length_writable && !ctx.intr.array_proto->has_index_props) {
      ArrayObject* arr = static_cast<ArrayObject*>(o);
      bool holes = false;
      for (const Value& v : arr->elems) if (v.is_empty()) { holes = true; break; }
      if (!holes) {
        arr->elems.insert(arr->elems.begin(), a.argv, a.argv + a.argc);
        arr->length_ += static_cast<uint32_t>(n);
        return Value::Number(arr->length_);
      }
    }
    for (uint64_t k = len; k > 0; --k) {
      bool has;
      JS_TRY(has_idx(ctx, o, k - 1, &has));
      if (has) {
        Value v;
        JS_TRY(get_idx(ctx, o, k - 1, &v));
        JS_TRY(set_idx(ctx, o, k - 1 + n, v));
      } else {
        JS_TRY(delete_idx(ctx, o, k - 1 + n));
      }
    }
    for (uint64_t j = 0; j < n; ++j) JS_TRY(set_idx(ctx, o, j, a.argv[j]));
  }
  JS_TRY(set_length(ctx, o, len + n));
  return Value::Number(static_cast<double>(len + n));
}

Value array_splice(Context& ctx, CallArgs& a) {
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  uint64_t start;
  JS_TRY(relative_index(ctx, a[0], len, 0, &start));
  uint64_t delete_count;
  if (a.argc == 0) delete_count = 0;
  else if (a.argc == 1) delete_count = len - start;
  else {
    double dc;
    JS_TRY(ctx.to_integer(a[1], &dc));
    dc = std::min(std::max(dc, 0.0), static_cast<double>(len - start));
    delete_count = static_cast<uint64_t>(dc);
  }
  const uint64_t item_count = a.argc > 2 ? static_cast<uint64_t>(a.argc - 2) : 0;
  if (len + item_count - delete_count > 9007199254740991ull) {
    ctx.throw_type_error("Invalid array length");
    return Value::Exception();
  }
  Object* removed;
  JS_TRY(array_species_create(ctx, o, delete_count, &removed));
  // dense fast path
  if (is_dense_array(o) && o->extensible && static_cast<ArrayObject*>(o)->length_writable && !ctx.intr.array_proto->has_index_props) {
    ArrayObject* arr = static_cast<ArrayObject*>(o);
    std::vector<Value> out(arr->elems.begin() + static_cast<long>(start), arr->elems.begin() + static_cast<long>(start + delete_count));
    arr->elems.erase(arr->elems.begin() + static_cast<long>(start), arr->elems.begin() + static_cast<long>(start + delete_count));
    if (item_count) arr->elems.insert(arr->elems.begin() + static_cast<long>(start), a.argv + 2, a.argv + a.argc);
    arr->length_ = static_cast<uint32_t>(arr->elems.size());
    JS_TRY(deliver(ctx, removed, out, delete_count));
    return Value::Obj(removed);
  }
  for (uint64_t k = 0; k < delete_count; ++k) {
    bool has;
    JS_TRY(has_idx(ctx, o, start + k, &has));
    if (has) {
      Value v;
      JS_TRY(get_idx(ctx, o, start + k, &v));
      JS_TRY(create_idx(ctx, removed, k, v));
    }
  }
  JS_TRY(set_length(ctx, removed, delete_count));
  if (item_count < delete_count) {
    for (uint64_t k = start; k < len - delete_count; ++k) {
      bool has;
      JS_TRY(has_idx(ctx, o, k + delete_count, &has));
      if (has) {
        Value v;
        JS_TRY(get_idx(ctx, o, k + delete_count, &v));
        JS_TRY(set_idx(ctx, o, k + item_count, v));
      } else {
        JS_TRY(delete_idx(ctx, o, k + item_count));
      }
    }
    for (uint64_t k = len; k > len - delete_count + item_count; --k) JS_TRY(delete_idx(ctx, o, k - 1));
  } else if (item_count > delete_count) {
    for (uint64_t k = len - delete_count; k > start; --k) {
      bool has;
      JS_TRY(has_idx(ctx, o, k + delete_count - 1, &has));
      if (has) {
        Value v;
        JS_TRY(get_idx(ctx, o, k + delete_count - 1, &v));
        JS_TRY(set_idx(ctx, o, k + item_count - 1, v));
      } else {
        JS_TRY(delete_idx(ctx, o, k + item_count - 1));
      }
    }
  }
  for (uint64_t i = 0; i < item_count; ++i) JS_TRY(set_idx(ctx, o, start + i, a.argv[2 + i]));
  JS_TRY(set_length(ctx, o, len - delete_count + item_count));
  return Value::Obj(removed);
}

Value array_reverse(Context& ctx, CallArgs& a) {
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  if (is_dense_array(o)) {
    ArrayObject* arr = static_cast<ArrayObject*>(o);
    bool holes = false;
    for (const Value& v : arr->elems) if (v.is_empty()) { holes = true; break; }
    if (!holes) {
      std::reverse(arr->elems.begin(), arr->elems.end());
      return Value::Obj(o);
    }
  }
  for (uint64_t lower = 0; lower < len / 2; ++lower) {
    const uint64_t upper = len - lower - 1;
    bool lower_exists, upper_exists;
    Value lv, uv;
    JS_TRY(has_idx(ctx, o, lower, &lower_exists));
    if (lower_exists) JS_TRY(get_idx(ctx, o, lower, &lv));
    JS_TRY(has_idx(ctx, o, upper, &upper_exists));
    if (upper_exists) JS_TRY(get_idx(ctx, o, upper, &uv));
    if (lower_exists && upper_exists) {
      JS_TRY(set_idx(ctx, o, lower, uv));
      JS_TRY(set_idx(ctx, o, upper, lv));
    } else if (upper_exists) {
      JS_TRY(set_idx(ctx, o, lower, uv));
      JS_TRY(delete_idx(ctx, o, upper));
    } else if (lower_exists) {
      JS_TRY(delete_idx(ctx, o, lower));
      JS_TRY(set_idx(ctx, o, upper, lv));
    }
  }
  return Value::Obj(o);
}

Value array_fill(Context& ctx, CallArgs& a) {
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  uint64_t k, end;
  JS_TRY(relative_index(ctx, a[1], len, 0, &k));
  JS_TRY(relative_index(ctx, a[2], len, len, &end));
  for (; k < end; ++k) JS_TRY(set_idx(ctx, o, k, a[0]));
  return Value::Obj(o);
}

Value array_copy_within(Context& ctx, CallArgs& a) {
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  uint64_t to, from, fin;
  JS_TRY(relative_index(ctx, a[0], len, 0, &to));
  JS_TRY(relative_index(ctx, a[1], len, 0, &from));
  JS_TRY(relative_index(ctx, a[2], len, len, &fin));
  if (fin <= from) return Value::Obj(o);
  uint64_t count = std::min(fin - from, len - to);
  int64_t dir = 1;
  if (from < to && to < from + count) {
    dir = -1;
    from += count - 1;
    to += count - 1;
  }
  while (count > 0) {
    bool has;
    JS_TRY(has_idx(ctx, o, from, &has));
    if (has) {
      Value v;
      JS_TRY(get_idx(ctx, o, from, &v));
      JS_TRY(set_idx(ctx, o, to, v));
    } else {
      JS_TRY(delete_idx(ctx, o, to));
    }
    from = static_cast<uint64_t>(static_cast<int64_t>(from) + dir);
    to = static_cast<uint64_t>(static_cast<int64_t>(to) + dir);
    --count;
  }
  return Value::Obj(o);
}

// A stable merge sort over a list, with a comparator that can fail (a script's compare function may throw).
struct Entry { Value v; JsString* key; };

bool merge_sort(Context& ctx, std::vector<Entry>& items, const Value& cmp) {
  std::vector<Entry> tmp(items.size());
  std::vector<JsString*> unused;
  const size_t n = items.size();
  bool failed = false;
  auto less_equal = [&](const Entry& x, const Entry& y) {
    if (!cmp.is_undefined()) {
      Value r;
      if (!ctx.call(cmp, Value::Undefined(), {x.v, y.v}, &r)) { failed = true; return true; }
      double d;
      if (!ctx.to_number(r, &d)) { failed = true; return true; }
      return !(d > 0);
    }
    return !(y.key->str() < x.key->str());
  };
  for (size_t width = 1; width < n && !failed; width *= 2) {
    for (size_t lo = 0; lo < n && !failed; lo += 2 * width) {
      const size_t mid = std::min(lo + width, n), hi = std::min(lo + 2 * width, n);
      size_t i = lo, j = mid, k = lo;
      while (i < mid && j < hi) tmp[k++] = less_equal(items[i], items[j]) ? items[i++] : items[j++];
      while (i < mid) tmp[k++] = items[i++];
      while (j < hi) tmp[k++] = items[j++];
    }
    if (!failed) items.swap(tmp);
  }
  return !failed;
}

// Collect the values at the existing indices, sorted (undefined last). `holes` is the number of missing indices.
bool sort_indexed(Context& ctx, Object* o, uint64_t len, const Value& cmp, std::vector<Value>* sorted, uint64_t* holes) {
  std::vector<Entry> items;
  std::vector<Value> undefs;
  for (uint64_t k = 0; k < len; ++k) {
    bool has;
    if (!has_idx(ctx, o, k, &has)) return false;
    if (!has) continue;
    Value v;
    if (!get_idx(ctx, o, k, &v)) return false;
    if (v.is_undefined()) undefs.push_back(v);
    else items.push_back({v, nullptr});
  }
  *holes = len - items.size() - undefs.size();
  if (cmp.is_undefined()) {
    for (Entry& e : items) {
      if (!ctx.to_string(e.v, &e.key)) return false;
    }
  }
  if (!merge_sort(ctx, items, cmp)) return false;
  for (Entry& e : items) sorted->push_back(e.v);
  for (Value& u : undefs) sorted->push_back(u);
  return true;
}

Value array_sort(Context& ctx, CallArgs& a) {
  if (!a[0].is_undefined() && !a[0].is_callable()) {
    ctx.throw_type_error("The comparison function must be either a function or undefined");
    return Value::Exception();
  }
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  std::vector<Value> sorted;
  uint64_t holes = 0;
  JS_TRY(sort_indexed(ctx, o, len, a[0], &sorted, &holes));
  uint64_t k = 0;
  for (; k < sorted.size(); ++k) JS_TRY(set_idx(ctx, o, k, sorted[k]));
  for (uint64_t i = 0; i < holes; ++i, ++k) {
    bool has;
    JS_TRY(has_idx(ctx, o, k, &has));
    if (has) JS_TRY(delete_idx(ctx, o, k));
  }
  return Value::Obj(o);
}

Value array_to_sorted(Context& ctx, CallArgs& a) {
  if (!a[0].is_undefined() && !a[0].is_callable()) {
    ctx.throw_type_error("The comparison function must be either a function or undefined");
    return Value::Exception();
  }
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  std::vector<Entry> items;
  for (uint64_t k = 0; k < len; ++k) {
    Value v;
    JS_TRY(get_idx(ctx, o, k, &v));
    items.push_back({v, nullptr});
  }
  std::vector<Entry> defined, undef;
  for (Entry& e : items) (e.v.is_undefined() ? undef : defined).push_back(e);
  if (a[0].is_undefined()) for (Entry& e : defined) JS_TRY(ctx.to_string(e.v, &e.key));
  JS_TRY(merge_sort(ctx, defined, a[0]));
  std::vector<Value> out;
  for (Entry& e : defined) out.push_back(e.v);
  for (Entry& e : undef) out.push_back(e.v);
  return Value::Obj(ctx.new_array_from(out));
}

// ---- accessors ----

Value array_concat(Context& ctx, CallArgs& a) {
  Object* o;
  JS_TRY(ctx.to_object(a.this_value, &o));
  Object* result;
  JS_TRY(array_species_create(ctx, o, 0, &result));
  std::vector<Value> out;
  uint64_t n = 0;
  bool direct = result->cls == Class::Array && static_cast<ArrayObject*>(result)->elems.empty() && result->proto == ctx.intr.array_proto;
  auto add = [&](const Value& v) -> bool {
    if (direct) { out.push_back(v); ++n; return true; }
    if (!v.is_empty() && !create_idx(ctx, result, n, v)) return false;
    ++n;
    return true;
  };
  for (int i = -1; i < a.argc; ++i) {
    Value item = i < 0 ? Value::Obj(o) : a.argv[i];
    bool spreadable = false;
    if (item.is_object()) {
      Value s;
      JS_TRY(item.as_object()->get(ctx, ctx.sym.is_concat_spreadable, item, &s));
      if (!s.is_undefined()) spreadable = Context::to_boolean(s);
      else JS_TRY(ctx.is_array(item, &spreadable));
    }
    if (!spreadable) {
      JS_TRY(add(item));
      continue;
    }
    Object* e = item.as_object();
    uint64_t len;
    JS_TRY(ctx.length_of(e, &len));
    if (n + len > 9007199254740991ull) {
      ctx.throw_type_error("Invalid array length");
      return Value::Exception();
    }
    if (is_dense_array(e)) {
      ArrayObject* ea = static_cast<ArrayObject*>(e);
      bool holes = false;
      for (const Value& v : ea->elems) if (v.is_empty()) { holes = true; break; }
      if (!holes && direct) {
        out.insert(out.end(), ea->elems.begin(), ea->elems.end());
        n += len;
        continue;
      }
    }
    for (uint64_t k = 0; k < len; ++k) {
      bool has;
      JS_TRY(has_idx(ctx, e, k, &has));
      if (has) {
        Value v;
        JS_TRY(get_idx(ctx, e, k, &v));
        JS_TRY(add(v));
      } else if (direct) {
        out.push_back(Value::Empty());
        ++n;
      } else {
        ++n;
      }
    }
  }
  if (direct) JS_TRY(deliver(ctx, result, out, n));
  else JS_TRY(set_length(ctx, result, n));
  return Value::Obj(result);
}

std::vector<Object*>& join_stack() {
  static std::vector<Object*> s;
  return s;
}

Value array_join(Context& ctx, CallArgs& a) {
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  std::u16string sep = u",";
  if (!a[0].is_undefined()) {
    JsString* s;
    JS_TRY(ctx.to_string(a[0], &s));
    sep = s->str();
  }
  auto& stack = join_stack();
  if (std::find(stack.begin(), stack.end(), o) != stack.end()) return Value::String(ctx.atoms.empty);
  stack.push_back(o);
  std::u16string out;
  bool ok = true;
  for (uint64_t k = 0; k < len && ok; ++k) {
    if (k) out += sep;
    Value v;
    if (!get_idx(ctx, o, k, &v)) { ok = false; break; }
    if (v.is_nullish()) continue;
    JsString* s;
    if (!ctx.to_string(v, &s)) { ok = false; break; }
    out += s->str();
  }
  stack.pop_back();
  if (!ok) return Value::Exception();
  return StrV(ctx, std::move(out));
}

Value array_to_string(Context& ctx, CallArgs& a) {
  Object* o;
  JS_TRY(ctx.to_object(a.this_value, &o));
  Value join;
  JS_TRY(o->get(ctx, ctx.atoms.join, Value::Obj(o), &join));
  if (join.is_callable()) {
    Value r;
    JS_TRY(ctx.call(join, Value::Obj(o), nullptr, 0, &r));
    return r;
  }
  JsString* s;
  JS_TRY(ordinary_to_string_tag(ctx, Value::Obj(o), &s));
  return Value::String(s);
}

Value array_to_locale_string(Context& ctx, CallArgs& a) {
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  auto& stack = join_stack();
  if (std::find(stack.begin(), stack.end(), o) != stack.end()) return Value::String(ctx.atoms.empty);
  stack.push_back(o);
  std::u16string out;
  bool ok = true;
  for (uint64_t k = 0; k < len && ok; ++k) {
    if (k) out += u",";
    Value v;
    if (!get_idx(ctx, o, k, &v)) { ok = false; break; }
    if (v.is_nullish()) continue;
    Value f, r;
    JsString* s;
    if (!ctx.get_v(v, ctx.atom_ascii("toLocaleString"), &f) || !ctx.call(f, v, nullptr, 0, &r) || !ctx.to_string(r, &s)) { ok = false; break; }
    out += s->str();
  }
  stack.pop_back();
  if (!ok) return Value::Exception();
  return StrV(ctx, std::move(out));
}

Value array_slice(Context& ctx, CallArgs& a) {
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  uint64_t k, fin;
  JS_TRY(relative_index(ctx, a[0], len, 0, &k));
  JS_TRY(relative_index(ctx, a[1], len, len, &fin));
  const uint64_t count = fin > k ? fin - k : 0;
  Object* result;
  JS_TRY(array_species_create(ctx, o, count, &result));
  std::vector<Value> out;
  if (is_dense_array(o)) {
    ArrayObject* arr = static_cast<ArrayObject*>(o);
    out.assign(arr->elems.begin() + static_cast<long>(k), arr->elems.begin() + static_cast<long>(k + count));
  } else {
    for (uint64_t i = 0; i < count; ++i) {
      bool has;
      JS_TRY(has_idx(ctx, o, k + i, &has));
      if (has) {
        Value v;
        JS_TRY(get_idx(ctx, o, k + i, &v));
        out.push_back(v);
      } else {
        out.push_back(Value::Empty());
      }
    }
  }
  JS_TRY(deliver(ctx, result, out, count));
  return Value::Obj(result);
}

Value index_of_impl(Context& ctx, CallArgs& a, bool last) {
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  if (len == 0) return Value::Number(-1);
  if (!last) {
    uint64_t k;
    JS_TRY(relative_index(ctx, a[1], len, 0, &k));
    for (; k < len; ++k) {
      bool has;
      JS_TRY(has_idx(ctx, o, k, &has));
      if (!has) continue;
      Value v;
      JS_TRY(get_idx(ctx, o, k, &v));
      if (Context::strict_equals(v, a[0])) return Value::Number(static_cast<double>(k));
    }
    return Value::Number(-1);
  }
  double from = static_cast<double>(len) - 1;
  if (a.argc > 1) {
    double n;
    JS_TRY(ctx.to_integer(a[1], &n));
    from = n >= 0 ? std::min(n, static_cast<double>(len) - 1) : static_cast<double>(len) + n;
  }
  for (double kd = from; kd >= 0; --kd) {
    const uint64_t k = static_cast<uint64_t>(kd);
    bool has;
    JS_TRY(has_idx(ctx, o, k, &has));
    if (!has) continue;
    Value v;
    JS_TRY(get_idx(ctx, o, k, &v));
    if (Context::strict_equals(v, a[0])) return Value::Number(static_cast<double>(k));
  }
  return Value::Number(-1);
}

Value array_index_of(Context& ctx, CallArgs& a) { return index_of_impl(ctx, a, false); }
Value array_last_index_of(Context& ctx, CallArgs& a) { return index_of_impl(ctx, a, true); }

Value array_includes(Context& ctx, CallArgs& a) {
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  if (len == 0) return Value::Bool(false);
  uint64_t k;
  JS_TRY(relative_index(ctx, a[1], len, 0, &k));
  for (; k < len; ++k) {
    Value v;
    JS_TRY(get_idx(ctx, o, k, &v));
    if (ctx.same_value_zero(v, a[0])) return Value::Bool(true);
  }
  return Value::Bool(false);
}

Value array_at(Context& ctx, CallArgs& a) {
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  double rel;
  JS_TRY(ctx.to_integer(a[0], &rel));
  double k = rel >= 0 ? rel : static_cast<double>(len) + rel;
  if (k < 0 || k >= static_cast<double>(len)) return Value::Undefined();
  Value v;
  JS_TRY(get_idx(ctx, o, static_cast<uint64_t>(k), &v));
  return v;
}

// ---- iteration with callbacks ----

enum class Cb { ForEach, Map, Filter, Some, Every, Find, FindIndex, FindLast, FindLastIndex };

Value callback_impl(Context& ctx, CallArgs& a, Cb kind) {
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  if (!callable_arg(ctx, a[0])) return Value::Exception();
  const Value fn = a[0], this_arg = a[1];
  Object* result = nullptr;
  std::vector<Value> out;
  if (kind == Cb::Map) JS_TRY(array_species_create(ctx, o, len, &result));
  if (kind == Cb::Filter) JS_TRY(array_species_create(ctx, o, 0, &result));
  const bool find_kind = kind == Cb::Find || kind == Cb::FindIndex || kind == Cb::FindLast || kind == Cb::FindLastIndex;
  const bool reverse = kind == Cb::FindLast || kind == Cb::FindLastIndex;
  if (kind == Cb::Map) out.assign(static_cast<size_t>(len), Value::Empty());
  for (uint64_t n = 0; n < len; ++n) {
    const uint64_t k = reverse ? len - 1 - n : n;
    Value v;
    if (!find_kind) {
      bool has;
      JS_TRY(has_idx(ctx, o, k, &has));
      if (!has) continue;
    }
    JS_TRY(get_idx(ctx, o, k, &v));
    Value r;
    JS_TRY(ctx.call(fn, this_arg, {v, Value::Number(static_cast<double>(k)), Value::Obj(o)}, &r));
    switch (kind) {
      case Cb::ForEach: break;
      case Cb::Map: out[static_cast<size_t>(k)] = r; break;
      case Cb::Filter: if (Context::to_boolean(r)) out.push_back(v); break;
      case Cb::Some: if (Context::to_boolean(r)) return Value::Bool(true); break;
      case Cb::Every: if (!Context::to_boolean(r)) return Value::Bool(false); break;
      case Cb::Find: case Cb::FindLast: if (Context::to_boolean(r)) return v; break;
      case Cb::FindIndex: case Cb::FindLastIndex: if (Context::to_boolean(r)) return Value::Number(static_cast<double>(k)); break;
    }
  }
  switch (kind) {
    case Cb::Map: JS_TRY(deliver(ctx, result, out, len)); return Value::Obj(result);
    case Cb::Filter: {
      const uint64_t count = out.size();
      JS_TRY(deliver(ctx, result, out, count));
      return Value::Obj(result);
    }
    case Cb::Some: return Value::Bool(false);
    case Cb::Every: return Value::Bool(true);
    case Cb::FindIndex: case Cb::FindLastIndex: return Value::Number(-1);
    default: return Value::Undefined();
  }
}

Value array_for_each(Context& ctx, CallArgs& a) { return callback_impl(ctx, a, Cb::ForEach); }
Value array_map(Context& ctx, CallArgs& a) { return callback_impl(ctx, a, Cb::Map); }
Value array_filter(Context& ctx, CallArgs& a) { return callback_impl(ctx, a, Cb::Filter); }
Value array_some(Context& ctx, CallArgs& a) { return callback_impl(ctx, a, Cb::Some); }
Value array_every(Context& ctx, CallArgs& a) { return callback_impl(ctx, a, Cb::Every); }
Value array_find(Context& ctx, CallArgs& a) { return callback_impl(ctx, a, Cb::Find); }
Value array_find_index(Context& ctx, CallArgs& a) { return callback_impl(ctx, a, Cb::FindIndex); }
Value array_find_last(Context& ctx, CallArgs& a) { return callback_impl(ctx, a, Cb::FindLast); }
Value array_find_last_index(Context& ctx, CallArgs& a) { return callback_impl(ctx, a, Cb::FindLastIndex); }

Value reduce_impl(Context& ctx, CallArgs& a, bool right) {
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  if (!callable_arg(ctx, a[0])) return Value::Exception();
  uint64_t n = 0;
  Value acc;
  bool have = a.argc >= 2;
  if (have) acc = a[1];
  for (; n < len; ++n) {
    const uint64_t k = right ? len - 1 - n : n;
    bool has;
    JS_TRY(has_idx(ctx, o, k, &has));
    if (!has) continue;
    Value v;
    JS_TRY(get_idx(ctx, o, k, &v));
    if (!have) { acc = v; have = true; continue; }
    Value r;
    JS_TRY(ctx.call(a[0], Value::Undefined(), {acc, v, Value::Number(static_cast<double>(k)), Value::Obj(o)}, &r));
    acc = r;
  }
  if (!have) {
    ctx.throw_type_error("Reduce of empty array with no initial value");
    return Value::Exception();
  }
  return acc;
}

Value array_reduce(Context& ctx, CallArgs& a) { return reduce_impl(ctx, a, false); }
Value array_reduce_right(Context& ctx, CallArgs& a) { return reduce_impl(ctx, a, true); }

bool flatten(Context& ctx, Object* target, std::vector<Value>& out, Object* source, uint64_t source_len, double depth, const Value& mapper, const Value& this_arg) {
  (void)target;
  for (uint64_t k = 0; k < source_len; ++k) {
    bool has;
    if (!has_idx(ctx, source, k, &has)) return false;
    if (!has) continue;
    Value el;
    if (!get_idx(ctx, source, k, &el)) return false;
    if (!mapper.is_undefined()) {
      Value m;
      if (!ctx.call(mapper, this_arg, {el, Value::Number(static_cast<double>(k)), Value::Obj(source)}, &m)) return false;
      el = m;
    }
    bool is_arr = false;
    if (depth > 0 && !ctx.is_array(el, &is_arr)) return false;
    if (depth > 0 && is_arr) {
      uint64_t l;
      if (!ctx.length_of(el.as_object(), &l)) return false;
      if (!flatten(ctx, target, out, el.as_object(), l, depth - 1, Value::Undefined(), Value::Undefined())) return false;
    } else {
      out.push_back(el);
    }
  }
  return true;
}

Value array_flat(Context& ctx, CallArgs& a) {
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  double depth = 1;
  if (!a[0].is_undefined()) {
    JS_TRY(ctx.to_integer(a[0], &depth));
    if (depth < 0) depth = 0;
  }
  Object* result;
  JS_TRY(array_species_create(ctx, o, 0, &result));
  std::vector<Value> out;
  JS_TRY(flatten(ctx, result, out, o, len, depth, Value::Undefined(), Value::Undefined()));
  const uint64_t n = out.size();
  JS_TRY(deliver(ctx, result, out, n));
  return Value::Obj(result);
}

Value array_flat_map(Context& ctx, CallArgs& a) {
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  if (!callable_arg(ctx, a[0])) return Value::Exception();
  Object* result;
  JS_TRY(array_species_create(ctx, o, 0, &result));
  std::vector<Value> out;
  JS_TRY(flatten(ctx, result, out, o, len, 1, a[0], a[1]));
  const uint64_t n = out.size();
  JS_TRY(deliver(ctx, result, out, n));
  return Value::Obj(result);
}

Value array_to_reversed(Context& ctx, CallArgs& a) {
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  std::vector<Value> out;
  for (uint64_t k = 0; k < len; ++k) {
    Value v;
    JS_TRY(get_idx(ctx, o, len - 1 - k, &v));
    out.push_back(v);
  }
  return Value::Obj(ctx.new_array_from(out));
}

Value array_to_spliced(Context& ctx, CallArgs& a) {
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  uint64_t start;
  JS_TRY(relative_index(ctx, a[0], len, 0, &start));
  uint64_t skip;
  if (a.argc == 0) skip = 0;
  else if (a.argc == 1) skip = len - start;
  else {
    double dc;
    JS_TRY(ctx.to_integer(a[1], &dc));
    skip = static_cast<uint64_t>(std::min(std::max(dc, 0.0), static_cast<double>(len - start)));
  }
  std::vector<Value> out;
  for (uint64_t k = 0; k < start; ++k) { Value v; JS_TRY(get_idx(ctx, o, k, &v)); out.push_back(v); }
  for (int i = 2; i < a.argc; ++i) out.push_back(a.argv[i]);
  for (uint64_t k = start + skip; k < len; ++k) { Value v; JS_TRY(get_idx(ctx, o, k, &v)); out.push_back(v); }
  return Value::Obj(ctx.new_array_from(out));
}

Value array_with(Context& ctx, CallArgs& a) {
  Object* o;
  uint64_t len;
  JS_TRY(this_array_like(ctx, a.this_value, &o, &len));
  double rel;
  JS_TRY(ctx.to_integer(a[0], &rel));
  double idx = rel >= 0 ? rel : static_cast<double>(len) + rel;
  if (idx < 0 || idx >= static_cast<double>(len)) {
    ctx.throw_range_error("Invalid index : " + std::to_string(static_cast<long long>(rel)));
    return Value::Exception();
  }
  std::vector<Value> out;
  for (uint64_t k = 0; k < len; ++k) {
    Value v = a[1];
    if (k != static_cast<uint64_t>(idx)) JS_TRY(get_idx(ctx, o, k, &v));
    out.push_back(v);
  }
  return Value::Obj(ctx.new_array_from(out));
}

// ---- iterators ----

Value make_array_iterator(Context& ctx, const Value& this_value, int kind) {
  Object* o;
  JS_TRY(ctx.to_object(this_value, &o));
  ArrayIteratorObject* it = ctx.heap.make<ArrayIteratorObject>();
  it->proto = ctx.intr.array_iterator_proto;
  it->target = o;
  it->kind = kind;
  return Value::Obj(it);
}

Value array_values(Context& ctx, CallArgs& a) { return make_array_iterator(ctx, a.this_value, 1); }
Value array_keys(Context& ctx, CallArgs& a) { return make_array_iterator(ctx, a.this_value, 0); }
Value array_entries(Context& ctx, CallArgs& a) { return make_array_iterator(ctx, a.this_value, 2); }

Value array_iterator_next(Context& ctx, CallArgs& a) {
  if (!a.this_value.is_object() || a.this_value.as_object()->cls != Class::ArrayIterator) {
    ctx.throw_type_error("next method called on incompatible receiver " + vm_describe(ctx, a.this_value));
    return Value::Exception();
  }
  ArrayIteratorObject* it = static_cast<ArrayIteratorObject*>(a.this_value.as_object());
  if (it->done) return Value::Obj(ctx.create_iter_result(Value::Undefined(), true));
  uint64_t len;
  JS_TRY(ctx.length_of(it->target, &len));
  if (it->index >= len) {
    it->done = true;
    return Value::Obj(ctx.create_iter_result(Value::Undefined(), true));
  }
  const uint64_t i = it->index++;
  if (it->kind == 0) return Value::Obj(ctx.create_iter_result(Value::Number(static_cast<double>(i)), false));
  Value v;
  JS_TRY(get_idx(ctx, it->target, i, &v));
  if (it->kind == 1) return Value::Obj(ctx.create_iter_result(v, false));
  return Value::Obj(ctx.create_iter_result(Value::Obj(ctx.new_array_from({Value::Number(static_cast<double>(i)), v})), false));
}

}  // namespace

void setup_array(Context& ctx) {
  Object* proto = ctx.intr.array_proto;
  Function* ctor = new_ctor(ctx, "Array", array_ctor, 1, proto);
  ctx.intr.array_ctor = ctor;
  ctx.define_method(ctor, "from", array_from, 1);
  ctx.define_method(ctor, "isArray", array_is_array, 1);
  ctx.define_method(ctor, "of", array_of, 0);
  ctx.define_getter(ctor, ctx.sym.species, "[Symbol.species]", array_species);

  struct M { const char* name; NativeFn fn; int len; } methods[] = {
      {"at", array_at, 1}, {"concat", array_concat, 1}, {"copyWithin", array_copy_within, 2}, {"entries", array_entries, 0},
      {"every", array_every, 1}, {"fill", array_fill, 1}, {"filter", array_filter, 1}, {"find", array_find, 1},
      {"findIndex", array_find_index, 1}, {"findLast", array_find_last, 1}, {"findLastIndex", array_find_last_index, 1},
      {"flat", array_flat, 0}, {"flatMap", array_flat_map, 1}, {"forEach", array_for_each, 1}, {"includes", array_includes, 1},
      {"indexOf", array_index_of, 1}, {"join", array_join, 1}, {"keys", array_keys, 0}, {"lastIndexOf", array_last_index_of, 1},
      {"map", array_map, 1}, {"pop", array_pop, 0}, {"push", array_push, 1}, {"reduce", array_reduce, 1},
      {"reduceRight", array_reduce_right, 1}, {"reverse", array_reverse, 0}, {"shift", array_shift, 0}, {"slice", array_slice, 2},
      {"some", array_some, 1}, {"sort", array_sort, 1}, {"splice", array_splice, 2}, {"toLocaleString", array_to_locale_string, 0},
      {"toReversed", array_to_reversed, 0}, {"toSorted", array_to_sorted, 1}, {"toSpliced", array_to_spliced, 2},
      {"toString", array_to_string, 0}, {"unshift", array_unshift, 1}, {"with", array_with, 2}};
  for (const M& m : methods) ctx.define_method(proto, m.name, m.fn, m.len);
  Function* values = ctx.define_method(proto, "values", array_values, 0);
  ctx.define_data(proto, ctx.sym.iterator, Value::Obj(values), kHiddenAttrs);
  Object* unscopables = ctx.new_object(nullptr);
  for (const char* n : {"at", "copyWithin", "entries", "fill", "find", "findIndex", "findLast", "findLastIndex", "flat", "flatMap", "includes", "keys", "toReversed", "toSorted", "toSpliced", "values"})
    ctx.define_data(unscopables, ctx.atom_ascii(n), Value::Bool(true));
  ctx.define_data(proto, ctx.sym.unscopables, Value::Obj(unscopables), kConfigurable);
}

// %IteratorPrototype%, the array iterator and the helpers that hang off them.
Value iterator_proto_iterator(Context&, CallArgs& a) { return a.this_value; }

void setup_iterators(Context& ctx) {
  Object* ip = ctx.new_object();
  ctx.intr.iterator_proto = ip;
  ctx.define_method(ip, ctx.sym.iterator, "[Symbol.iterator]", iterator_proto_iterator, 0);
  Object* aip = ctx.new_object(ip);
  ctx.intr.array_iterator_proto = aip;
  ctx.define_method(aip, "next", array_iterator_next, 0);
  ctx.define_data(aip, ctx.sym.to_string_tag, Str(ctx, "Array Iterator"), kConfigurable);
  Object* aitp = ctx.new_object();
  ctx.intr.async_iterator_proto = aitp;
  ctx.define_method(aitp, ctx.sym.async_iterator, "[Symbol.asyncIterator]", iterator_proto_iterator, 0);
}

}  // namespace aios::js
