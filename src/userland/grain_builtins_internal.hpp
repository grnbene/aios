#pragma once
// The argument checks every grain native starts with, shared by the portable
// standard library (grain_builtins.cpp) and the natives that only exist inside
// aios (grain_aios.cpp). A failed check fills `error` with what was wanted and
// what came; the native then returns nil and the VM raises it at the call.
#include <string>
#include <vector>

#include "aios/userland/grain.hpp"

namespace aios::userland::grain {

[[maybe_unused]] inline Value arg_or(std::vector<Value>& a, size_t i) { return i < a.size() ? a[i] : Value::nil(); }

inline bool want_list(const Value& v, std::string& error, const char* who) {
  if (v.type == Type::List) return true;
  error = std::string(who) + "() wants a list, got a " + type_name(v.type);
  return false;
}

inline bool want_str(const Value& v, std::string& error, const char* who) {
  if (v.type == Type::Str) return true;
  error = std::string(who) + "() wants a string, got a " + type_name(v.type);
  return false;
}

inline bool want_number(const Value& v, std::string& error, const char* who) {
  if (v.is_number()) return true;
  error = std::string(who) + "() wants a number, got a " + type_name(v.type);
  return false;
}

}  // namespace aios::userland::grain
