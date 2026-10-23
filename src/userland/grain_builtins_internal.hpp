#pragma once
// The argument checks every grain native starts with, shared by the portable
// standard library (grain_builtins.cpp) and the natives that only exist inside
// aios (grain_aios.cpp). A failed check fills `error` with what was wanted and
// what came; the native then returns nil and the VM raises it at the call.
#include <memory>
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

// A number or an exact decimal: for the functions that take either (abs, min, max, round, ...).
inline bool want_numeric(const Value& v, std::string& error, const char* who) {
  if (v.is_number() || v.type == Type::Decimal) return true;
  error = std::string(who) + "() wants a number, got a " + type_name(v.type);
  return false;
}

// A pane drawn and driven by native code instead of grain callbacks (a face
// pane, grain_face.cpp). It still belongs to the VM that made it, and the
// closures it calls are that VM's.
class PaneDriver {
 public:
  virtual ~PaneDriver() = default;
  // Draw the whole pane into `argb` (width x height, row-major ARGB).
  virtual bool draw(Vm& vm, std::vector<uint32_t>& argb, int width, int height, std::string& error) = 0;
  // An input event map, as a grain pane's on_event receives it.
  virtual bool event(Vm& vm, const Value& event, std::string& error) = 0;
};
// Registers a driven pane for `vm` and returns its pane map (as create_pane does).
Value register_driven_pane(Vm& vm, const std::string& title, int width, int height, std::shared_ptr<PaneDriver> driver);
// The driver of one of `vm`'s panes (a pane map, handle or id), or null.
std::shared_ptr<PaneDriver> pane_driver(const Vm& vm, const Value& pane);

}  // namespace aios::userland::grain
