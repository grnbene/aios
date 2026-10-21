#pragma once
// The engine's runtime: heap, atoms, realm, the value stack and the helpers (conversions, property access,
// calls) the VM and the builtins are written against. See js.hpp for the value and object model.

#include <deque>
#include <unordered_set>

#include "aios/js/js.hpp"

namespace aios::js {

class Heap {
 public:
  Heap() = default;
  ~Heap();
  Heap(const Heap&) = delete;
  Heap& operator=(const Heap&) = delete;

  template <class T, class... A>
  T* make(A&&... a) {
    T* c = new T(std::forward<A>(a)...);
    link(c);
    return c;
  }
  void link(Cell* c);
  // Atoms: interned strings. `intern` returns the one JsString for these code units.
  JsString* intern(std::u16string_view s);
  JsString* intern_utf8(std::string_view s);

  // Mark everything reachable from the roots the caller marked, free the rest. Atoms nobody references are dropped.
  void sweep();
  size_t bytes() const { return bytes_; }
  size_t threshold() const { return threshold_; }
  bool should_collect() const { return stress_ || bytes_ >= threshold_; }
  void set_stress(bool on) { stress_ = on; }   // collect at every safepoint (finds missing roots)
  void set_threshold_after_collect();
  void note_bytes(long delta) { bytes_ = static_cast<size_t>(static_cast<long>(bytes_) + delta); }
  size_t cell_count() const { return count_; }
  // Cells with weak references register here so the collector can run their weak passes.
  void add_weak(Cell* c) { weak_cells.insert(c); }
  void remove_weak(Cell* c) { weak_cells.erase(c); }
  std::unordered_set<Cell*> weak_cells;
  static uint32_t hash_units(const char16_t* p, size_t n);

 private:
  friend class Context;
  Cell* head_ = nullptr;
  size_t bytes_ = 0;
  size_t threshold_ = 8u << 20;
  bool stress_ = false;
  size_t count_ = 0;
  std::unordered_map<uint64_t, std::vector<JsString*>> atoms_;   // by content hash
};

// A rooted value for host code that holds a Value across calls into the engine.
class Persistent {
 public:
  Persistent() = default;
  Persistent(Context* ctx, Value v);
  Persistent(const Persistent&) = delete;
  Persistent& operator=(const Persistent&) = delete;
  Persistent(Persistent&& o) noexcept;
  Persistent& operator=(Persistent&& o) noexcept;
  ~Persistent();
  Value get() const { return v_; }
  void reset();
  explicit operator bool() const { return ctx_ != nullptr; }
 private:
  Context* ctx_ = nullptr;
  Value v_;
  size_t slot_ = 0;
};

// A temporary root for values a native holds while it calls back into script.
class Rooted {
 public:
  Rooted(Context& ctx, Value v);
  Rooted(const Rooted&) = delete;
  Rooted& operator=(const Rooted&) = delete;
  ~Rooted();
  Value& operator*() { return *p_; }
  Value* operator->() { return p_; }
 private:
  Context& ctx_;
  Value* p_;
};

enum class Hint : uint8_t { Default, Number, String };

class Context {
 public:
  Context();
  ~Context();
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  Heap heap;
  CommonAtoms atoms;
  WellKnownSymbols sym;
  Intrinsics intr;

  // ---- pending exception ----
  Value exception() const { return exception_; }
  void set_exception(Value v) { exception_ = v; exception_set_ = true; }
  void clear_exception() { exception_ = Value::Undefined(); exception_set_ = false; }
  bool has_exception() const { return exception_set_; }
  Value throw_value(Value v) { exception_ = v; exception_set_ = true; return Value::Exception(); }
  Value throw_error(ErrorKind k, std::string_view msg);
  Value throw_type_error(std::string_view msg) { return throw_error(ErrorKind::TypeError, msg); }
  Value throw_range_error(std::string_view msg) { return throw_error(ErrorKind::RangeError, msg); }
  Value throw_reference_error(std::string_view msg) { return throw_error(ErrorKind::ReferenceError, msg); }
  Value throw_syntax_error(std::string_view msg) { return throw_error(ErrorKind::SyntaxError, msg); }
  Value take_exception() { Value v = exception_; exception_ = Value::Undefined(); exception_set_ = false; return v; }
  // Text for an exception value ("TypeError: x is not a function" plus the stack when it has one), for hosts.
  std::string describe_exception(Value ex);
  // A value as a developer console shows it: strings quoted inside structures, objects expanded to a few levels.
  std::string inspect(const Value& v);

  // ---- strings ----
  JsString* new_string(std::u16string s);
  JsString* new_string_utf8(std::string_view s);
  JsString* new_string_ascii(std::string_view s);
  JsString* concat(JsString* a, JsString* b);
  JsString* substring(JsString* s, uint32_t from, uint32_t to);
  JsString* atom(std::u16string_view s) { return heap.intern(s); }
  JsString* atom_utf8(std::string_view s) { return heap.intern_utf8(s); }
  JsString* atom_ascii(std::string_view s);
  JsString* to_atom(JsString* s);
  JsString* index_atom(uint32_t i);
  JsString* number_to_string(double d, int radix = 10);
  JsString* empty_string() { return atoms.empty; }
  Symbol* new_symbol(JsString* description);
  BigInt* new_bigint(BigNum n);

  // ---- objects ----
  Object* new_object(Object* proto);
  Object* new_object() { return new_object(intr.object_proto); }
  ArrayObject* new_array(uint32_t length = 0);
  ArrayObject* new_array_from(const std::vector<Value>& v);
  Function* new_native(std::string_view name, NativeFn fn, int length, bool is_ctor = false);
  Function* new_host_function(std::string_view name, HostFn fn, int length, bool is_ctor = false);
  Object* new_error(ErrorKind k, std::string_view msg);

  // ---- property helpers (all may run script: false means an exception is pending) ----
  bool get(Value obj, PropKey k, Value* out);
  bool get(Object* obj, PropKey k, Value* out) { return obj->get(*this, k, Value::Obj(obj), out); }
  bool get_v(Value base, PropKey k, Value* out);              // base may be a primitive
  bool get_index(Value base, uint32_t i, Value* out);
  bool put(Object* obj, PropKey k, Value v, bool throw_on_fail = true);   // [[Set]] with the object as its own receiver
  bool put_v(Value base, PropKey k, Value v, bool strict);
  bool create_data_property(Object* o, PropKey k, Value v, bool* ok = nullptr);
  bool define_data(Object* o, PropKey k, Value v, uint8_t attrs = kDefaultAttrs);       // ordinary, always succeeds
  bool define_accessor(Object* o, PropKey k, Object* getter, Object* setter, uint8_t attrs = kConfigurable);
  void define_value(Object* o, std::string_view name, Value v, uint8_t attrs = kHiddenAttrs);
  Function* define_method(Object* o, std::string_view name, NativeFn fn, int length, uint8_t attrs = kHiddenAttrs);
  Function* define_method(Object* o, Symbol* sym, std::string_view name, NativeFn fn, int length, uint8_t attrs = kHiddenAttrs);
  void define_getter(Object* o, std::string_view name, NativeFn fn);
  void define_getter(Object* o, Symbol* sym, std::string_view name, NativeFn fn);
  bool has_property(Object* o, PropKey k, bool* out) { return o->has_property(*this, k, out); }
  bool has_own_property(Object* o, PropKey k, bool* out);
  bool delete_prop(Object* o, PropKey k, bool* ok) { return o->delete_property(*this, k, ok); }
  bool to_property_key(Value v, PropKey* out);
  bool property_key_from_index(uint64_t i, PropKey* out);
  // For hosts: a key from a C string, and the common get/set by name.
  PropKey key(std::string_view ascii_or_utf8);
  bool get_named(Value obj, std::string_view name, Value* out);
  bool set_named(Object* obj, std::string_view name, Value v);
  bool length_of(Object* o, uint64_t* out);                   // ToLength(Get(o, "length"))

  // ---- conversions ----
  bool to_primitive(Value v, Hint hint, Value* out);
  bool to_number(Value v, double* out);
  // ToNumeric: a Number or a BigInt value (an object goes through ToPrimitive first).
  bool to_numeric(Value v, Value* out);
  // ToBigInt: booleans and strings convert, a Number or undefined/null/Symbol is a TypeError, a bad string a SyntaxError.
  bool to_bigint(Value v, BigInt** out);
  // The operand of a BigInt/Number mixing error.
  Value throw_mixed_bigint();
  bool to_string(Value v, JsString** out);
  bool to_object(Value v, Object** out);
  bool to_integer(Value v, double* out);                      // ToIntegerOrInfinity
  bool to_length(Value v, uint64_t* out);
  bool to_int32(Value v, int32_t* out);
  bool to_uint32(Value v, uint32_t* out);
  static bool to_boolean(const Value& v);
  static double to_int32_d(double d);
  static double to_uint32_d(double d);
  bool to_index(Value v, uint64_t* out);
  bool same_value(const Value& a, const Value& b);
  bool same_value_zero(const Value& a, const Value& b);
  static bool strict_equals(const Value& a, const Value& b);
  bool loose_equals(const Value& a, const Value& b, bool* out);
  const char* type_of(const Value& v);
  static double string_to_number(const std::u16string& s);

  // ---- calls ----
  bool call(Value fn, Value this_v, const Value* argv, int argc, Value* out);
  bool call(Value fn, Value this_v, std::initializer_list<Value> args, Value* out) {
    return call(fn, this_v, args.begin(), static_cast<int>(args.size()), out);
  }
  bool construct(Value fn, const Value* argv, int argc, Value new_target, Value* out);
  bool call_method(Value obj, PropKey name, const Value* argv, int argc, Value* out);
  bool instance_of(Value v, Value target, bool* out);
  bool ordinary_has_instance(Value ctor, Value v, bool* out);
  bool is_array(Value v, bool* out);
  bool species_constructor(Object* o, Value default_ctor, Value* out);
  // The prototype for an object a constructor makes, honouring new.target.
  bool prototype_from_ctor(Value new_target, Object* fallback, Object** out);
  bool get_method(Value v, PropKey k, Value* out);            // undefined/null -> undefined; non-callable -> TypeError

  // ---- iteration ----
  struct Iter { Value iterator; Value next; bool done = false; };
  bool get_iterator(Value iterable, Iter* out, bool async = false);
  bool iterator_step(Iter& it, bool* done, Value* value);     // false: exception
  bool iterator_close(Iter& it);                              // calls .return(), ignoring the result's value
  bool iterate(Value iterable, const std::function<bool(Value)>& fn);   // fn returns false to stop early (iterator is closed)
  Object* create_iter_result(Value v, bool done);
  bool iterable_to_list(Value iterable, std::vector<Value>* out);

  // ---- binary data, for hosts ----
  Object* new_array_buffer(const uint8_t* data, size_t length);
  Object* new_uint8_array(const uint8_t* data, size_t length);
  // The bytes an ArrayBuffer, typed array or DataView covers; false (nothing thrown) for anything else.
  bool bytes_of(const Value& v, std::string* out);

  // ---- running code ----
  // Compile and run a classic script in the global scope. On failure, *error holds the message and the
  // exception (a script's own, or a SyntaxError) is available from take_exception().
  bool eval_script(std::string_view utf8_source, std::string_view filename, Value* result);
  bool eval_script16(const std::u16string& source, std::string_view filename, Value* result);
  // A function built from a parameter list and a body, as `new Function(params, body)` would (an inline event handler).
  bool make_function(std::string_view params, std::string_view body, Value* out);
  Object* global() { return intr.global; }
  // A script's source locations for error messages and stack traces.
  void set_filename_for_errors(std::string_view f) { current_file_ = std::string(f); }

  // ---- jobs (promise reactions, async functions) ----
  void enqueue_job(std::function<void()> job);
  void enqueue_job_value(Value fn, std::vector<Value> args);     // a script job: fn(...args)
  // Run queued jobs until none are left. Returns false if a job left an unreported exception.
  bool run_jobs();
  bool has_jobs() const { return !jobs_.empty() || !value_jobs_.empty(); }
  // Hook for rejected promises nobody handled.
  std::function<void(Value promise, Value reason)> on_unhandled_rejection;

  // ---- garbage collection ----
  void gc();                                // collect now (only when no native frame is active)
  void maybe_gc();                          // collect if the heap has grown past its threshold and it is safe to
  void add_root_tracer(std::function<void(Tracer&)> t, void** handle_out = nullptr);
  size_t push_persistent(Value v);
  void release_persistent(size_t slot);
  Value& persistent_slot(size_t slot) { return persistents_[slot]; }
  std::vector<Value*> temp_roots;           // Rooted
  std::unordered_map<std::u16string, std::shared_ptr<void>> regexp_cache;   // compiled patterns by "flags/source" (regexp literals recreate their objects)
  std::vector<Value> kept_alive;            // WeakRef targets dereferenced during the current job
  std::vector<Value> pending_rejections;    // promises rejected with no handler yet; reported when the job queue drains
  int native_depth = 0;                     // natives currently on the C++ stack (collection waits for 0)

  // ---- host services ----
  std::function<double()> now_ms;           // wall clock, ms since the epoch
  std::function<double(double utc_ms)> tz_offset_minutes;   // minutes to add to a UTC instant to get local time, east positive (default: UTC)
  std::function<std::string(double utc_ms)> tz_name;        // the zone's long name at an instant ("Central European Standard Time")
  std::function<double()> random_double;
  std::function<void(const std::string&)> print;            // console output
  std::function<std::string(const std::string& url)> read_source;   // unused by the core

  // ---- limits ----
  size_t max_stack_values = 1u << 20;
  int max_call_depth = 4000;
  uint64_t step_budget = 0;                 // 0 = unlimited; counts backward jumps and calls
  uint64_t steps = 0;
  bool step_budget_exceeded = false;

  // internal
  struct VM;
  VM* vm = nullptr;
  std::string current_file_;
  std::unordered_map<JsString*, Value> global_lexicals;       // top-level let/const/class
  std::unordered_map<JsString*, bool> global_lexical_const;
  std::vector<Value> symbol_registry_keys;
  std::unordered_map<std::u16string, Symbol*> symbol_registry;
  std::vector<std::function<void(Tracer&)>> root_tracers;

 private:
  friend class Persistent;
  friend class Rooted;
  Value exception_;
  bool exception_set_ = false;
  std::vector<Value> persistents_;
  std::vector<size_t> free_persistents_;
  std::deque<std::function<void()>> jobs_;
  std::deque<std::pair<Value, std::vector<Value>>> value_jobs_;
  bool running_jobs_ = false;
  JsString* index_atoms_[1024] = {};
  void init_realm();
};

// Convenience used throughout the builtins.
#define JS_TRY(expr)                    \
  do {                                  \
    if (!(expr)) return Value::Exception(); \
  } while (0)

}  // namespace aios::js
