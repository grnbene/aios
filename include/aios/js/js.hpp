#pragma once
// A JavaScript engine for this tree: ECMAScript 5.1 with the ES2015+ the web depends on (let/const, arrow
// functions, classes, template literals, destructuring, spread, iterators, generators, async/await, Promise,
// Symbol, Map/Set, typed arrays, Proxy/Reflect). A bytecode VM over a garbage-collected heap.
//
// Built for the same constraints as the rest of the kernel: no C++ exceptions (errors are a pending
// exception on the Context and a `false`/Exception return), no host libm assumptions, no threads. The host
// (the browser pane, a test runner) binds its own objects through the native-function and host-object API
// below.
//
// Conventions. A function returning bool succeeds or has left an exception pending in the Context
// (`ctx.exception()`); one returning Value returns Value::Exception() for the same. Nothing in the engine
// throws a C++ exception.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aios::js {

struct Cell;
struct JsString;
struct Symbol;
struct BigInt;
struct Object;
struct Function;
struct Env;
struct FunctionCode;
class Context;
class Heap;

// ---------------------------------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------------------------------

enum class Tag : uint8_t {
  Undefined,
  Null,
  Boolean,
  Number,
  String,
  Symbol,
  BigInt,
  Object,
  Empty,       // an array hole, an uninitialised `let` (TDZ): never visible to a script
  Exception,   // the sentinel a native returns after setting a pending exception
};

struct Value {
  Tag tag = Tag::Undefined;
  union {
    double num;
    bool boolean;
    Cell* cell;
  };
  Value() : num(0) {}

  static Value Undefined() { return Value(); }
  static Value Null() { Value v; v.tag = Tag::Null; return v; }
  static Value Bool(bool b) { Value v; v.tag = Tag::Boolean; v.boolean = b; return v; }
  static Value Number(double d) { Value v; v.tag = Tag::Number; v.num = d; return v; }
  static Value String(JsString* s);
  static Value Sym(Symbol* s);
  static Value Big(BigInt* b);
  static Value Obj(Object* o);
  static Value Empty() { Value v; v.tag = Tag::Empty; return v; }
  static Value Exception() { Value v; v.tag = Tag::Exception; return v; }

  bool is_undefined() const { return tag == Tag::Undefined; }
  bool is_null() const { return tag == Tag::Null; }
  bool is_nullish() const { return tag == Tag::Undefined || tag == Tag::Null; }
  bool is_bool() const { return tag == Tag::Boolean; }
  bool is_number() const { return tag == Tag::Number; }
  bool is_string() const { return tag == Tag::String; }
  bool is_symbol() const { return tag == Tag::Symbol; }
  bool is_bigint() const { return tag == Tag::BigInt; }
  bool is_object() const { return tag == Tag::Object; }
  bool is_empty() const { return tag == Tag::Empty; }
  bool is_exception() const { return tag == Tag::Exception; }
  bool is_cell() const { return tag == Tag::String || tag == Tag::Symbol || tag == Tag::BigInt || tag == Tag::Object; }

  JsString* as_string() const { return reinterpret_cast<JsString*>(cell); }
  Symbol* as_symbol() const { return reinterpret_cast<Symbol*>(cell); }
  BigInt* as_bigint() const { return reinterpret_cast<BigInt*>(cell); }
  Object* as_object() const { return reinterpret_cast<Object*>(cell); }
  bool is_callable() const;
};

// ---------------------------------------------------------------------------------------------------
// The heap
// ---------------------------------------------------------------------------------------------------

enum class CellKind : uint8_t { String, Symbol, BigInt, Object, Env, Code };

// Everything the collector owns. `trace` marks the cells this one points at.
struct Cell {
  Cell* gc_next = nullptr;
  CellKind kind;
  bool marked = false;
  explicit Cell(CellKind k) : kind(k) {}
  Cell(const Cell&) = delete;
  Cell& operator=(const Cell&) = delete;
  virtual ~Cell() = default;
  virtual void trace(class Tracer& t) = 0;
  // Bytes this cell holds beyond its own size, for the allocation accounting that decides when to collect.
  virtual size_t extra_bytes() const { return 0; }
  // Weak references (WeakMap, WeakRef, ...): after marking, `trace_weak` marks what an ephemeron keeps alive when its key
  // survived (true if it marked something new); once marking is complete `sweep_weak` drops what did not survive.
  virtual bool trace_weak(class Tracer&) { return false; }
  virtual void sweep_weak(Context&) {}
};

class Tracer {
 public:
  void mark(Cell* c) {
    if (c && !c->marked) { c->marked = true; stack_.push_back(c); }
  }
  void mark(const Value& v) {
    if (v.is_cell()) mark(v.cell);
  }
  void drain();   // trace everything reachable from what has been marked
 private:
  std::vector<Cell*> stack_;
};

// ---------------------------------------------------------------------------------------------------
// Strings and symbols
// ---------------------------------------------------------------------------------------------------

// A JavaScript string: a sequence of UTF-16 code units. Concatenation builds a rope that is flattened the first
// time the text is needed, so `s += x` in a loop is linear.
struct JsString final : Cell {
  JsString() : Cell(CellKind::String) {}
  uint32_t length = 0;
  mutable std::u16string flat;
  mutable JsString* left = nullptr;    // a rope: left + right, no flat text until flatten()
  mutable JsString* right = nullptr;
  mutable uint32_t hash_ = 0;
  mutable bool hashed = false;
  bool is_atom = false;                // interned: equal atoms are the same pointer
  mutable int64_t index_cache = -2;    // -2 unknown, -1 not an array index, else the index

  const std::u16string& str() const { if (left) flatten(); return flat; }
  void flatten() const;
  uint32_t hash() const;
  // The array index this string spells (canonical decimal, < 2^32 - 1), or -1.
  int64_t array_index() const;
  std::string utf8() const;
  void trace(Tracer& t) override;
  size_t extra_bytes() const override { return flat.capacity() * 2; }
};

struct Symbol final : Cell {
  Symbol() : Cell(CellKind::Symbol) {}
  JsString* description = nullptr;     // may be null: Symbol()
  bool registered = false;             // Symbol.for
  bool is_private = false;             // a class's #name: never listed, never visible to a Proxy
  void trace(Tracer& t) override;
};

// An arbitrary-precision integer: a sign and a little-endian magnitude of 32-bit limbs with no leading zero limb.
// Zero is the empty magnitude and is never negative. The arithmetic lives in bigint.hpp.
struct BigNum {
  bool neg = false;
  std::vector<uint32_t> mag;
  bool is_zero() const { return mag.empty(); }
};

struct BigInt final : Cell {
  BigInt() : Cell(CellKind::BigInt) {}
  explicit BigInt(BigNum v) : Cell(CellKind::BigInt), n(std::move(v)) {}
  BigNum n;
  void trace(Tracer&) override {}
  size_t extra_bytes() const override { return n.mag.capacity() * sizeof(uint32_t); }
};

// A property key is an atom (an interned string) or a symbol: one pointer, compared by identity.
using PropKey = Cell*;

// ---------------------------------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------------------------------

enum Attr : uint8_t {
  kWritable = 1,
  kEnumerable = 2,
  kConfigurable = 4,
  kAccessor = 8,
  kDefaultAttrs = kWritable | kEnumerable | kConfigurable,
  kHiddenAttrs = kWritable | kConfigurable,   // builtin methods: not enumerable
};

struct Prop {
  PropKey key = nullptr;
  Value value;                         // a data property's value
  Object* getter = nullptr;            // accessor properties
  Object* setter = nullptr;
  uint8_t attrs = kDefaultAttrs;
  bool is_accessor() const { return attrs & kAccessor; }
};

// A property descriptor as the spec uses it: each field may be absent.
struct PropDesc {
  Value value;
  Object* getter = nullptr;
  Object* setter = nullptr;
  bool has_value = false, has_get = false, has_set = false;
  bool has_writable = false, has_enumerable = false, has_configurable = false;
  bool writable = false, enumerable = false, configurable = false;
  bool is_accessor() const { return has_get || has_set; }
  bool is_data() const { return has_value || has_writable; }
  static PropDesc Data(Value v, uint8_t attrs = kDefaultAttrs) {
    PropDesc d;
    d.value = v; d.has_value = true;
    d.writable = attrs & kWritable; d.has_writable = true;
    d.enumerable = attrs & kEnumerable; d.has_enumerable = true;
    d.configurable = attrs & kConfigurable; d.has_configurable = true;
    return d;
  }
};

enum class Class : uint8_t {
  Object, Array, Function, Error, Boolean, Number, String, Symbol, Date, RegExp, Map, Set, WeakMap, WeakSet,
  WeakRef, Promise, ArrayBuffer, TypedArray, DataView, Arguments, Generator, AsyncGenerator, Proxy, Iterator,
  ArrayIterator, StringIterator, MapIterator, SetIterator, RegExpStringIterator,
  Math, JSON, Reflect, Global, Host, FinalizationRegistry, TextDecoder, BigInt,
};

// The insertion-ordered own properties of an object, with a hash index once there are enough of them to need one.
class PropertyTable {
 public:
  Prop* find(PropKey k);
  const Prop* find(PropKey k) const { return const_cast<PropertyTable*>(this)->find(k); }
  Prop& add(PropKey k);                 // the key must not be present
  bool erase(PropKey k);
  size_t size() const { return props_.size(); }
  std::vector<Prop>& all() { return props_; }
  const std::vector<Prop>& all() const { return props_; }
 private:
  std::vector<Prop> props_;
  std::unordered_map<PropKey, uint32_t> index_;
  void reindex();
};

struct Object : Cell {
  explicit Object(Class c = Class::Object) : Cell(CellKind::Object), cls(c) {}
  Object* proto = nullptr;
  Class cls;
  bool extensible = true;
  bool callable = false;                // a function, or a proxy of one
  bool is_constructor = false;
  bool exotic_props = false;            // overrides get_own_property/define_own_property/delete_property/own_keys
  bool exotic_get = false;              // overrides get/set/has_property (a Proxy, a host object with named hooks)
  bool has_index_props = false;         // some index-like key lives here (a hint that lets array stores skip the prototype walk)
  PropertyTable props;
  // Array-like exotic objects keep their integer-indexed values here (Array, arguments).
  std::vector<Value> elems;

  void trace(Tracer& t) override;
  size_t extra_bytes() const override { return props.size() * sizeof(Prop) + elems.capacity() * sizeof(Value); }

  // ---- the spec's internal methods. Ordinary objects use these defaults; exotic ones override. ----
  virtual bool get_prototype_of(Context& ctx, Object** out);
  virtual bool set_prototype_of(Context& ctx, Object* p, bool* ok);
  virtual bool is_extensible_obj(Context& ctx, bool* out);
  virtual bool prevent_extensions(Context& ctx, bool* ok);
  virtual bool get_own_property(Context& ctx, PropKey k, PropDesc* out, bool* found);
  virtual bool define_own_property(Context& ctx, PropKey k, const PropDesc& d, bool* ok);
  virtual bool has_property(Context& ctx, PropKey k, bool* out);
  virtual bool get(Context& ctx, PropKey k, const Value& receiver, Value* out);
  virtual bool set(Context& ctx, PropKey k, const Value& v, const Value& receiver, bool* ok);
  virtual bool delete_property(Context& ctx, PropKey k, bool* ok);
  // Own keys in spec order: integer indices ascending, then strings in creation order, then symbols.
  virtual bool own_keys(Context& ctx, std::vector<PropKey>* out);
  // [[Call]] and [[Construct]]: a function (or a proxy of one) implements these.
  virtual bool call_internal(Context& ctx, const Value& this_v, const Value* argv, int argc, Value* out);
  virtual bool construct_internal(Context& ctx, const Value* argv, int argc, const Value& new_target, Value* out);
};

// Whether `v` is callable / a constructor.
inline bool Value::is_callable() const { return is_object() && as_object()->callable; }

// The call a native function receives.
struct CallArgs {
  Value this_value;
  const Value* argv = nullptr;
  int argc = 0;
  Value new_target;                     // undefined for a plain call
  Function* callee = nullptr;
  Value operator[](int i) const { return i < argc ? argv[i] : Value::Undefined(); }
  bool is_construct() const { return !new_target.is_undefined(); }
};

using NativeFn = Value (*)(Context& ctx, CallArgs& args);
using HostFn = std::function<Value(Context& ctx, CallArgs& args)>;

struct Function : Object {
  enum class Kind : uint8_t { Closure, Native, Host, Bound };
  Function() : Object(Class::Function) { callable = true; }
  Kind kind = Kind::Native;
  // Closure
  FunctionCode* code = nullptr;
  Env* env = nullptr;
  Value this_val;                       // an arrow function's lexical this; a bound function's this
  Object* home_object = nullptr;        // for `super`
  Value new_target_lexical;             // an arrow function's lexical new.target
  Object* fields_owner = nullptr;       // an arrow function: the class constructor whose fields super() initialises
  Object* fields = nullptr;             // a class constructor: the array of instance field and private method entries
  // Native
  NativeFn native = nullptr;
  HostFn host;
  std::vector<Value> slots;             // data a native keeps (a bound function's target and arguments; a Promise's resolve state)
  bool gc_safe = false;                 // a native that roots what it holds and may let the collector run
  // class constructors
  bool is_class = false;
  bool is_derived = false;
  void trace(Tracer& t) override;
  size_t extra_bytes() const override { return Object::extra_bytes() + slots.capacity() * sizeof(Value); }
  bool call_internal(Context& ctx, const Value& this_v, const Value* argv, int argc, Value* out) override;
  bool construct_internal(Context& ctx, const Value* argv, int argc, const Value& new_target, Value* out) override;
};

// An ordinary array. Dense by construction; a very sparse array falls back to named properties.
struct ArrayObject : Object {
  uint32_t length_ = 0;                 // may exceed elems.size() for a sparse tail
  bool length_writable = true;
  bool slow = false;                    // indices live in `props` (a frozen, sparse or accessor-bearing array)
  ArrayObject() : Object(Class::Array) { exotic_props = true; }
  bool get_own_property(Context& ctx, PropKey k, PropDesc* out, bool* found) override;
  bool define_own_property(Context& ctx, PropKey k, const PropDesc& d, bool* ok) override;
  bool delete_property(Context& ctx, PropKey k, bool* ok) override;
  bool own_keys(Context& ctx, std::vector<PropKey>* out) override;
  bool set(Context& ctx, PropKey k, const Value& v, const Value& receiver, bool* ok) override;
  bool get(Context& ctx, PropKey k, const Value& receiver, Value* out) override;
  bool has_property(Context& ctx, PropKey k, bool* out) override;
};

// A lexical environment record: the variables of a scope that an inner function captured.
struct Env final : Cell {
  Env() : Cell(CellKind::Env) {}
  Env* parent = nullptr;
  std::vector<Value> slots;
  void trace(Tracer& t) override;
  size_t extra_bytes() const override { return slots.capacity() * sizeof(Value); }
};

// ---------------------------------------------------------------------------------------------------
// Atoms the engine itself refers to
// ---------------------------------------------------------------------------------------------------

#define AIOS_JS_ATOMS(X)                                                                                         \
  X(length) X(prototype) X(constructor) X(name) X(message) X(stack) X(toString) X(valueOf) X(toJSON)              \
  X(value) X(writable) X(enumerable) X(configurable) X(get) X(set) X(done) X(next) X(return_) X(throw_)          \
  X(callee) X(caller) X(arguments) X(index) X(input) X(lastIndex) X(raw) X(then) X(resolve) X(reject)            \
  X(proto__) X(cause) X(errors) X(source) X(flags) X(global) X(ignoreCase) X(multiline) X(sticky) X(unicode)    \
  X(dotAll) X(hasIndices) X(groups) X(default_) X(undefined) X(null) X(true_) X(false_) X(NaN) X(Infinity)      \
  X(eval) X(Symbol) X(Object) X(Array) X(Function) X(Error) X(empty) X(async) X(await) X(of) X(let) X(static_)  \
  X(yield) X(from) X(as) X(target) X(meta) X(anonymous) X(lastIndexOf) X(add) X(has) X(size) X(keys) X(values)  \
  X(entries) X(join) X(apply) X(call) X(bind) X(byteLength) X(buffer) X(byteOffset) X(species) X(description)

struct CommonAtoms {
#define X(n) JsString* n = nullptr;
  AIOS_JS_ATOMS(X)
#undef X
};

// The well-known symbols.
struct WellKnownSymbols {
  Symbol* iterator = nullptr;
  Symbol* async_iterator = nullptr;
  Symbol* has_instance = nullptr;
  Symbol* to_primitive = nullptr;
  Symbol* to_string_tag = nullptr;
  Symbol* species = nullptr;
  Symbol* is_concat_spreadable = nullptr;
  Symbol* unscopables = nullptr;
  Symbol* match = nullptr;
  Symbol* match_all = nullptr;
  Symbol* replace = nullptr;
  Symbol* search = nullptr;
  Symbol* split = nullptr;
};

enum class ErrorKind : uint8_t { Error, TypeError, RangeError, ReferenceError, SyntaxError, EvalError, URIError, AggregateError };

// The prototypes and constructors every realm has.
struct Intrinsics {
  Object* object_proto = nullptr;
  Object* function_proto = nullptr;
  Object* array_proto = nullptr;
  Object* array_ctor = nullptr;
  Object* string_proto = nullptr;
  Object* number_proto = nullptr;
  Object* boolean_proto = nullptr;
  Object* symbol_proto = nullptr;
  Object* bigint_proto = nullptr;
  Object* error_proto = nullptr;
  Object* error_protos[8] = {};
  Object* iterator_proto = nullptr;
  Object* array_iterator_proto = nullptr;
  Object* string_iterator_proto = nullptr;
  Object* map_iterator_proto = nullptr;
  Object* set_iterator_proto = nullptr;
  Object* regexp_string_iterator_proto = nullptr;
  Object* generator_proto = nullptr;
  Object* generator_function_proto = nullptr;
  Object* async_function_proto = nullptr;
  Object* async_generator_proto = nullptr;
  Object* async_generator_function_proto = nullptr;
  Object* async_iterator_proto = nullptr;
  Object* async_from_sync_iterator_proto = nullptr;
  Object* promise_proto = nullptr;
  Object* promise_ctor = nullptr;
  Object* regexp_proto = nullptr;
  Object* regexp_ctor = nullptr;
  Object* regexp_exec = nullptr;
  Object* date_proto = nullptr;
  Object* map_proto = nullptr;
  Object* set_proto = nullptr;
  Object* weakmap_proto = nullptr;
  Object* weakset_proto = nullptr;
  Object* weakref_proto = nullptr;
  Object* finreg_proto = nullptr;
  Object* arraybuffer_proto = nullptr;
  Object* arraybuffer_ctor = nullptr;
  Object* typedarray_proto = nullptr;
  Object* typedarray_protos[12] = {};
  Object* typedarray_ctors[12] = {};
  Object* dataview_proto = nullptr;
  Object* proxy_ctor = nullptr;
  Object* throw_type_error = nullptr;
  Object* global = nullptr;
};

}  // namespace aios::js
