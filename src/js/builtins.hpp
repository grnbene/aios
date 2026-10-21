#pragma once
// Helpers shared by the builtin library files.
#include <string>
#include <vector>

#include "bigint.hpp"
#include "internal.hpp"
#include "vm.hpp"

namespace aios::js {

#define NATIVE(name) static Value name(Context& ctx, CallArgs& a)

inline Value Num(double d) { return Value::Number(d); }
inline Value Boo(bool b) { return Value::Bool(b); }
inline Value Str(Context& ctx, std::string_view s) { return Value::String(ctx.atom_utf8(s)); }
inline Value StrV(Context& ctx, std::u16string s) { return Value::String(ctx.new_string(std::move(s))); }
inline Value ObjV(Object* o) { return Value::Obj(o); }

// Property descriptors <-> script objects.
bool to_prop_desc(Context& ctx, const Value& v, PropDesc* out);
Value from_prop_desc(Context& ctx, const PropDesc& d);

// Array-likes: the generic Get/Set/Has/Delete by index, with a fast path for dense arrays.
bool get_idx(Context& ctx, Object* o, uint64_t i, Value* out);
bool set_idx(Context& ctx, Object* o, uint64_t i, const Value& v);      // Set(o, i, v, true)
bool has_idx(Context& ctx, Object* o, uint64_t i, bool* out);
bool delete_idx(Context& ctx, Object* o, uint64_t i);                   // DeletePropertyOrThrow
bool create_idx(Context& ctx, Object* o, uint64_t i, const Value& v);   // CreateDataPropertyOrThrow
bool set_length(Context& ctx, Object* o, uint64_t len);                 // Set(o, "length", len, true)
bool create_list_from_array_like(Context& ctx, const Value& v, std::vector<Value>* out);
bool is_dense_array(Object* o);

// Relative index arguments (slice, splice, ...): clamp ToIntegerOrInfinity(v) into [0, len].
bool relative_index(Context& ctx, const Value& v, uint64_t len, uint64_t default_value, uint64_t* out);

std::string key_to_display(PropKey k);
bool ordinary_to_string_tag(Context& ctx, const Value& v, JsString** out);   // Object.prototype.toString

// Species-aware array creation.
bool array_species_create(Context& ctx, Object* original, uint64_t length, Object** out);

// GetSubstitution (String.prototype.replace and RegExp's @@replace): expands $-patterns in `replacement`.
bool get_substitution(Context& ctx, JsString* matched, JsString* str, size_t position, const std::vector<Value>& captures, const Value& named, JsString* replacement, std::u16string* out);
bool is_regexp(Context& ctx, const Value& v, bool* out);

// Date formatting with Intl.DateTimeFormat-style options (en-US); defaults: 1 date, 2 time, 3 both when no component is named.
std::string date_format_locale(Context& ctx, double t, const Value& options, int defaults, bool* ok);

// The names of the typed-array element types, in intrinsic order.
constexpr int kTypedArrayKinds = 11;

}  // namespace aios::js
