// The ordinary object internal methods (ECMA-262 10.1) and the Array exotic object.
#include <algorithm>

#include "aios/js/context.hpp"

namespace aios::js {

namespace {

inline bool is_string_key(PropKey k) { return k->kind == CellKind::String; }

// The array index a key spells, or -1.
inline int64_t key_index(PropKey k) {
  if (!is_string_key(k)) return -1;
  return static_cast<JsString*>(k)->array_index();
}

PropDesc desc_of(const Prop& p) {
  PropDesc d;
  d.has_enumerable = d.has_configurable = true;
  d.enumerable = p.attrs & kEnumerable;
  d.configurable = p.attrs & kConfigurable;
  if (p.is_accessor()) {
    d.has_get = d.has_set = true;
    d.getter = p.getter;
    d.setter = p.setter;
  } else {
    d.has_value = d.has_writable = true;
    d.value = p.value;
    d.writable = p.attrs & kWritable;
  }
  return d;
}

// ValidateAndApplyPropertyDescriptor for a property stored in `o`'s table.
bool validate_and_apply(Context& ctx, Object* o, PropKey k, bool extensible, const PropDesc& d, Prop* cur) {
  if (!cur) {
    if (!extensible) return false;
    Prop& p = o->props.add(k);
    uint8_t attrs = 0;
    if (d.has_enumerable && d.enumerable) attrs |= kEnumerable;
    if (d.has_configurable && d.configurable) attrs |= kConfigurable;
    if (d.is_accessor()) {
      attrs |= kAccessor;
      p.getter = d.has_get ? d.getter : nullptr;
      p.setter = d.has_set ? d.setter : nullptr;
    } else {
      if (d.has_writable && d.writable) attrs |= kWritable;
      p.value = d.has_value ? d.value : Value::Undefined();
    }
    p.attrs = attrs;
    if (key_index(k) >= 0) o->has_index_props = true;
    return true;
  }
  const bool cur_configurable = cur->attrs & kConfigurable;
  if (!cur_configurable) {
    if (d.has_configurable && d.configurable) return false;
    if (d.has_enumerable && d.enumerable != static_cast<bool>(cur->attrs & kEnumerable)) return false;
    const bool kind_change = (d.is_accessor() && !cur->is_accessor()) || (d.is_data() && cur->is_accessor());
    if (kind_change) return false;
    if (cur->is_accessor()) {
      if (d.has_get && d.getter != cur->getter) return false;
      if (d.has_set && d.setter != cur->setter) return false;
    } else if (!(cur->attrs & kWritable)) {
      if (d.has_writable && d.writable) return false;
      if (d.has_value && !ctx.same_value(d.value, cur->value)) return false;
    }
  }
  // apply
  if (d.is_accessor() && !cur->is_accessor()) {
    cur->attrs = static_cast<uint8_t>((cur->attrs & (kEnumerable | kConfigurable)) | kAccessor);
    cur->value = Value::Undefined();
    cur->getter = cur->setter = nullptr;
  } else if (d.is_data() && cur->is_accessor()) {
    cur->attrs = static_cast<uint8_t>(cur->attrs & (kEnumerable | kConfigurable));
    cur->getter = cur->setter = nullptr;
    cur->value = Value::Undefined();
  }
  if (d.has_value) cur->value = d.value;
  if (d.has_get) cur->getter = d.getter;
  if (d.has_set) cur->setter = d.setter;
  auto set_bit = [&](bool has, bool v, uint8_t bit) {
    if (has) cur->attrs = static_cast<uint8_t>(v ? (cur->attrs | bit) : (cur->attrs & ~bit));
  };
  set_bit(d.has_writable, d.writable, kWritable);
  set_bit(d.has_enumerable, d.enumerable, kEnumerable);
  set_bit(d.has_configurable, d.configurable, kConfigurable);
  return true;
}

}  // namespace

// --- ordinary internal methods ---------------------------------------------------------------------

bool Object::get_prototype_of(Context&, Object** out) { *out = proto; return true; }

bool Object::set_prototype_of(Context&, Object* p, bool* ok) {
  if (p == proto) { *ok = true; return true; }
  if (!extensible) { *ok = false; return true; }
  for (Object* q = p; q; q = q->proto) {
    if (q == this) { *ok = false; return true; }   // a cycle
    if (q->cls == Class::Proxy) break;
  }
  proto = p;
  *ok = true;
  return true;
}

bool Object::is_extensible_obj(Context&, bool* out) { *out = extensible; return true; }

bool Object::prevent_extensions(Context&, bool* ok) { extensible = false; *ok = true; return true; }

bool Object::get_own_property(Context&, PropKey k, PropDesc* out, bool* found) {
  Prop* p = props.find(k);
  if (!p) { *found = false; return true; }
  *found = true;
  *out = desc_of(*p);
  return true;
}

bool Object::define_own_property(Context& ctx, PropKey k, const PropDesc& d, bool* ok) {
  Prop* cur = props.find(k);
  *ok = validate_and_apply(ctx, this, k, extensible, d, cur);
  return true;
}

bool Object::has_property(Context& ctx, PropKey k, bool* out) {
  for (Object* o = this; o;) {
    if (o != this && o->exotic_get) return o->has_property(ctx, k, out);
    if (o->exotic_props) {
      PropDesc d;
      bool found = false;
      if (!o->get_own_property(ctx, k, &d, &found)) return false;
      if (found) { *out = true; return true; }
    } else if (o->props.find(k)) {
      *out = true;
      return true;
    }
    Object* next = nullptr;
    if (!o->get_prototype_of(ctx, &next)) return false;
    o = next;
  }
  *out = false;
  return true;
}

bool Object::get(Context& ctx, PropKey k, const Value& receiver, Value* out) {
  for (Object* o = this; o;) {
    if (o != this && o->exotic_get) return o->get(ctx, k, receiver, out);
    if (o->exotic_props) {
      PropDesc d;
      bool found = false;
      if (!o->get_own_property(ctx, k, &d, &found)) return false;
      if (found) {
        if (d.is_accessor()) {
          if (!d.getter) { *out = Value::Undefined(); return true; }
          return ctx.call(Value::Obj(d.getter), receiver, nullptr, 0, out);
        }
        *out = d.value;
        return true;
      }
    } else if (Prop* p = o->props.find(k)) {
      if (p->is_accessor()) {
        if (!p->getter) { *out = Value::Undefined(); return true; }
        return ctx.call(Value::Obj(p->getter), receiver, nullptr, 0, out);
      }
      *out = p->value;
      return true;
    }
    o = o->proto;
  }
  *out = Value::Undefined();
  return true;
}

bool Object::set(Context& ctx, PropKey k, const Value& v, const Value& receiver, bool* ok) {
  // OrdinarySet: find the property, own first, then up the chain.
  PropDesc own;
  bool found = false;
  if (exotic_props) {
    if (!get_own_property(ctx, k, &own, &found)) return false;
  } else if (Prop* p = props.find(k)) {
    found = true;
    own = desc_of(*p);
  }
  if (!found) {
    Object* parent = nullptr;
    if (!get_prototype_of(ctx, &parent)) return false;
    if (parent) return parent->set(ctx, k, v, receiver, ok);
    own = PropDesc::Data(Value::Undefined());
  }
  if (own.is_data()) {
    if (!own.writable) { *ok = false; return true; }
    if (!receiver.is_object()) { *ok = false; return true; }
    Object* r = receiver.as_object();
    // Fast path: the receiver is this object and the property is an ordinary own data property.
    if (r == this && !exotic_props) {
      if (Prop* p = props.find(k)) {
        if (!p->is_accessor()) { p->value = v; *ok = true; return true; }
      }
    }
    PropDesc existing;
    bool have = false;
    if (!r->get_own_property(ctx, k, &existing, &have)) return false;
    if (have) {
      if (existing.is_accessor()) { *ok = false; return true; }
      if (!existing.writable) { *ok = false; return true; }
      PropDesc vd;
      vd.value = v;
      vd.has_value = true;
      return r->define_own_property(ctx, k, vd, ok);
    }
    return r->define_own_property(ctx, k, PropDesc::Data(v), ok);
  }
  if (!own.setter) { *ok = false; return true; }
  Value tmp;
  if (!ctx.call(Value::Obj(own.setter), receiver, &v, 1, &tmp)) return false;
  *ok = true;
  return true;
}

bool Object::delete_property(Context& ctx, PropKey k, bool* ok) {
  Prop* p = props.find(k);
  if (!p) { *ok = true; return true; }
  if (!(p->attrs & kConfigurable)) { *ok = false; return true; }
  props.erase(k);
  *ok = true;
  (void)ctx;
  return true;
}

bool Object::own_keys(Context& ctx, std::vector<PropKey>* out) {
  std::vector<std::pair<uint32_t, PropKey>> idx;
  std::vector<PropKey> strs, syms;
  for (const Prop& p : props.all()) {
    if (p.key->kind == CellKind::Symbol) {
      if (!static_cast<Symbol*>(p.key)->is_private) syms.push_back(p.key);
      continue;
    }
    int64_t i = key_index(p.key);
    if (i >= 0) idx.emplace_back(static_cast<uint32_t>(i), p.key);
    else strs.push_back(p.key);
  }
  std::sort(idx.begin(), idx.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (auto& e : idx) out->push_back(e.second);
  for (PropKey k : strs) out->push_back(k);
  for (PropKey k : syms) out->push_back(k);
  (void)ctx;
  return true;
}

// --- Array -------------------------------------------------------------------------------------------

namespace {

constexpr uint32_t kMaxDenseGap = 1u << 16;

bool is_plain_default(const PropDesc& d, bool creating) {
  if (d.is_accessor()) return false;
  if (creating) {
    return d.has_writable && d.writable && d.has_enumerable && d.enumerable && d.has_configurable && d.configurable;
  }
  if (d.has_writable && !d.writable) return false;
  if (d.has_enumerable && !d.enumerable) return false;
  if (d.has_configurable && !d.configurable) return false;
  return true;
}

// Move every element out of the dense vector and into the property table.
void go_slow(Context& ctx, ArrayObject* a) {
  if (a->slow) return;
  for (size_t i = 0; i < a->elems.size(); ++i) {
    if (a->elems[i].is_empty()) continue;
    Prop& p = a->props.add(ctx.index_atom(static_cast<uint32_t>(i)));
    p.value = a->elems[i];
    p.attrs = kDefaultAttrs;
  }
  a->elems.clear();
  a->elems.shrink_to_fit();
  a->slow = true;
  a->has_index_props = true;
}

// ArraySetLength.
bool array_set_length(Context& ctx, ArrayObject* a, const PropDesc& d, bool* ok) {
  if (!d.has_value) {
    // only attribute changes
    if (d.is_accessor() || (d.has_configurable && d.configurable) || (d.has_enumerable && d.enumerable)) { *ok = false; return true; }
    if (d.has_writable && d.writable && !a->length_writable) { *ok = false; return true; }
    if (d.has_writable && !d.writable) a->length_writable = false;
    *ok = true;
    return true;
  }
  uint32_t new_len;
  {
    double n;
    if (!ctx.to_number(d.value, &n)) return false;
    double u;
    if (!ctx.to_number(d.value, &u)) return false;
    uint32_t nl = static_cast<uint32_t>(Context::to_uint32_d(n));
    if (static_cast<double>(nl) != u) {
      ctx.throw_range_error("Invalid array length");
      return false;
    }
    new_len = nl;
  }
  if (d.is_accessor() || (d.has_configurable && d.configurable) || (d.has_enumerable && d.enumerable)) { *ok = false; return true; }
  if (new_len == a->length_) {
    if (d.has_writable && !d.writable) a->length_writable = false;
    else if (d.has_writable && d.writable && !a->length_writable) { *ok = false; return true; }
    *ok = true;
    return true;
  }
  if (!a->length_writable) { *ok = false; return true; }
  bool final_ok = true;
  if (new_len < a->length_) {
    if (!a->slow) {
      if (a->elems.size() > new_len) a->elems.resize(new_len);
      a->length_ = new_len;
    } else {
      // delete from the end, stopping at a non-configurable element
      std::vector<std::pair<uint32_t, PropKey>> idx;
      for (const Prop& p : a->props.all()) {
        int64_t i = key_index(p.key);
        if (i >= static_cast<int64_t>(new_len)) idx.emplace_back(static_cast<uint32_t>(i), p.key);
      }
      std::sort(idx.begin(), idx.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
      uint32_t reached = new_len;
      for (auto& e : idx) {
        Prop* p = a->props.find(e.second);
        if (p && !(p->attrs & kConfigurable)) { reached = e.first + 1; final_ok = false; break; }
        a->props.erase(e.second);
      }
      a->length_ = reached;
    }
  } else {
    a->length_ = new_len;
    if (!a->slow && new_len > a->elems.size() + kMaxDenseGap) go_slow(ctx, a);
  }
  if (d.has_writable && !d.writable) a->length_writable = false;
  *ok = final_ok;
  return true;
}

}  // namespace

bool ArrayObject::get_own_property(Context& ctx, PropKey k, PropDesc* out, bool* found) {
  if (k == ctx.atoms.length) {
    *found = true;
    *out = PropDesc::Data(Value::Number(static_cast<double>(length_)), length_writable ? kWritable : 0);
    return true;
  }
  if (!slow) {
    int64_t i = key_index(k);
    if (i >= 0) {
      if (static_cast<uint64_t>(i) < elems.size() && !elems[static_cast<size_t>(i)].is_empty()) {
        *found = true;
        *out = PropDesc::Data(elems[static_cast<size_t>(i)]);
      } else {
        *found = false;
      }
      return true;
    }
  }
  return Object::get_own_property(ctx, k, out, found);
}

bool ArrayObject::define_own_property(Context& ctx, PropKey k, const PropDesc& d, bool* ok) {
  if (k == ctx.atoms.length) return array_set_length(ctx, this, d, ok);
  int64_t i = key_index(k);
  if (i < 0) return Object::define_own_property(ctx, k, d, ok);
  const uint32_t idx = static_cast<uint32_t>(i);
  if (idx >= length_ && !length_writable) { *ok = false; return true; }
  if (!slow) {
    const bool exists = idx < elems.size() && !elems[idx].is_empty();
    if (exists && is_plain_default(d, false)) {
      if (d.has_value) elems[idx] = d.value;
      *ok = true;
      return true;
    }
    if (!exists && extensible && is_plain_default(d, true) && idx <= elems.size() + kMaxDenseGap) {
      if (idx >= elems.size()) elems.resize(static_cast<size_t>(idx) + 1, Value::Empty());
      elems[idx] = d.has_value ? d.value : Value::Undefined();
      if (idx >= length_) length_ = idx + 1;
      has_index_props = true;
      *ok = true;
      return true;
    }
    if (!exists && !extensible) { *ok = false; return true; }
    go_slow(ctx, this);
  }
  bool r = true;
  if (!Object::define_own_property(ctx, k, d, &r)) return false;
  if (r && idx >= length_) length_ = idx + 1;
  *ok = r;
  return true;
}

bool ArrayObject::delete_property(Context& ctx, PropKey k, bool* ok) {
  if (k == ctx.atoms.length) { *ok = false; return true; }
  if (!slow) {
    int64_t i = key_index(k);
    if (i >= 0) {
      if (static_cast<uint64_t>(i) < elems.size()) elems[static_cast<size_t>(i)] = Value::Empty();
      *ok = true;
      return true;
    }
  }
  return Object::delete_property(ctx, k, ok);
}

bool ArrayObject::own_keys(Context& ctx, std::vector<PropKey>* out) {
  if (!slow) {
    for (size_t i = 0; i < elems.size(); ++i)
      if (!elems[i].is_empty()) out->push_back(ctx.index_atom(static_cast<uint32_t>(i)));
    out->push_back(ctx.atoms.length);
    std::vector<PropKey> strs, syms;
    for (const Prop& p : props.all()) (p.key->kind == CellKind::Symbol ? syms : strs).push_back(p.key);
    for (PropKey k : strs) out->push_back(k);
    for (PropKey k : syms) out->push_back(k);
    return true;
  }
  std::vector<PropKey> rest;
  if (!Object::own_keys(ctx, &rest)) return false;
  // indices come first in `rest`; insert "length" after them
  size_t pos = 0;
  while (pos < rest.size() && key_index(rest[pos]) >= 0) ++pos;
  out->insert(out->end(), rest.begin(), rest.begin() + static_cast<long>(pos));
  out->push_back(ctx.atoms.length);
  out->insert(out->end(), rest.begin() + static_cast<long>(pos), rest.end());
  return true;
}

bool ArrayObject::get(Context& ctx, PropKey k, const Value& receiver, Value* out) {
  if (!slow) {
    int64_t i = key_index(k);
    if (i >= 0 && static_cast<uint64_t>(i) < elems.size() && !elems[static_cast<size_t>(i)].is_empty()) {
      *out = elems[static_cast<size_t>(i)];
      return true;
    }
    if (k == ctx.atoms.length) { *out = Value::Number(static_cast<double>(length_)); return true; }
  }
  return Object::get(ctx, k, receiver, out);
}

bool ArrayObject::has_property(Context& ctx, PropKey k, bool* out) {
  if (!slow) {
    int64_t i = key_index(k);
    if (i >= 0 && static_cast<uint64_t>(i) < elems.size() && !elems[static_cast<size_t>(i)].is_empty()) { *out = true; return true; }
    if (k == ctx.atoms.length) { *out = true; return true; }
  }
  return Object::has_property(ctx, k, out);
}

bool ArrayObject::set(Context& ctx, PropKey k, const Value& v, const Value& receiver, bool* ok) {
  // Fast path: an element store on this array, with nothing on the prototype chain that could intercept it.
  if (!slow && receiver.is_object() && receiver.as_object() == this) {
    int64_t i = key_index(k);
    if (i >= 0) {
      const size_t idx = static_cast<size_t>(i);
      if (idx < elems.size() && !elems[idx].is_empty()) { elems[idx] = v; *ok = true; return true; }
      bool chain_clear = true;
      for (Object* p = proto; p; p = p->proto)
        if (p->has_index_props || p->exotic_get || p->cls == Class::Proxy) { chain_clear = false; break; }
      if (chain_clear && extensible && length_writable && idx <= elems.size() + kMaxDenseGap) {
        if (idx >= elems.size()) elems.resize(idx + 1, Value::Empty());
        elems[idx] = v;
        if (idx >= length_) length_ = static_cast<uint32_t>(idx + 1);
        has_index_props = true;
        *ok = true;
        return true;
      }
    }
  }
  return Object::set(ctx, k, v, receiver, ok);
}

}  // namespace aios::js
