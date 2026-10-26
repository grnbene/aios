#pragma once
// grain — the machine's own general-purpose language.
//
// AOS had two languages and neither of them is this one. `face` describes a
// dream: it lays out and paints, it has `let` and `if` and `for`, and it has
// no functions you can call with arguments and no data structures that outlive
// a frame. A `.aos` script is a list of shell commands with a failure count.
// Nothing here could express `fib(30)`, which is why asking the machine for a
// Python interpreter got an answer about one rather than one.
//
// So: a real language, whole. Source text goes through four stages that are
// separate on purpose, because a single-pass parser-that-emits is the thing
// that cannot later be optimised, checked, or targeted at anything else:
//
//   lex      grain_lexer.cpp     text  -> tokens, each carrying its line
//   parse    grain_parser.cpp    tokens -> AST, a tree with no opinions
//   compile  grain_compiler.cpp  AST   -> Chunk, bytecode for a stack machine
//   run      grain_vm.cpp        Chunk -> values, on frames with upvalues
//
// The AST in the middle is what makes this a compiler rather than a tree
// walker with extra steps. `grain dis` prints what the compiler actually
// emitted, so the claim is checkable rather than asserted.
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "aios/core/ids.hpp"
#include "aios/userland/portable_int128.hpp"

namespace aios::hal { class Host; }

namespace aios::userland::grain {

// kv_* (grain_std_kv.cpp): a hosted app's key-value store, in memory and journaled at `journal_path`. One store per
// path in a process: an app restarted gets the same one. `problem` (when given) says why the journal could not be
// read; the store then runs in memory.
struct KvStore;
std::shared_ptr<KvStore> open_kv_store(hal::Host& host, const std::string& journal_path, std::string* problem = nullptr);

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------
// Int and Real are separate rather than one double. A language whose only
// number is a double cannot say 2^53 + 1, cannot index an array past that, and
// gives 7 / 2 == 3.5 where a byte offset was wanted. Arithmetic stays integral
// until something forces it not to.
class Obj;
struct Value;
using ObjRef = std::shared_ptr<Obj>;

enum class Type : uint8_t {
  Nil, Bool, Int, Real, Str, List, Map, Fn, Native, Extern,
  Id128, Capability, Channel, StateFrame, MemoryObject, Entity, BoundMethod, Bytes, Decimal
};

const char* type_name(Type t);

struct Value {
  Type type = Type::Nil;
  union {
    bool b;
    int64_t i;
    double d;
  };
  ObjRef obj;  // Str, List, Map, Fn, Native, Id128, Cap, Channel, Frame, Mem, Entity, BoundMethod

  Value() : type(Type::Nil), i(0) {}
  static Value nil() { return Value(); }
  static Value boolean(bool v) { Value x; x.type = Type::Bool; x.b = v; return x; }
  static Value integer(int64_t v) { Value x; x.type = Type::Int; x.i = v; return x; }
  static Value real(double v) { Value x; x.type = Type::Real; x.d = v; return x; }
  static Value object(Type t, ObjRef o) { Value x; x.type = t; x.i = 0; x.obj = std::move(o); return x; }
  static Value string(std::string s);
  static Value list(std::vector<Value> v);
  static Value bytes(std::vector<uint8_t> v);
  static Value id128(const aios::Id128& id);
  static Value id128(const std::string& hex);

  bool is_number() const { return type == Type::Int || type == Type::Real; }
  bool is_id128() const { return type == Type::Id128; }
  // Only nil and false are false. An empty string and 0 are values, and a
  // language that calls them false makes `if count` mean two different things
  // depending on whether count is a number or a list.
  bool truthy() const { return !(type == Type::Nil || (type == Type::Bool && !b)); }
  double as_double() const { return type == Type::Int ? (double)i : type == Type::Real ? d : 0.0; }
  int64_t as_int() const { return type == Type::Int ? i : type == Type::Real ? (int64_t)d : 0; }

  const aios::Id128& as_id128() const;
  const std::string& str() const;
  std::vector<Value>& list_items() const;
  std::vector<uint8_t>& bytes_data() const;   // a Bytes value's contents, mutable and shared
  std::string to_text() const;      // what `print` shows
  std::string repr() const;         // what a value inside a list shows
  bool equals(const Value& o) const;
  bool is_container() const;
};

// The heap side of a value. One base class with a kind rather than a class per
// kind: the VM only ever asks "which", and virtual dispatch across six leaves
// buys nothing here.
class Obj {
 public:
  explicit Obj(Type k) : kind(k) {}
  virtual ~Obj() = default;
  Type kind;
  // Fn values are either a bare FnObj or a ClosureObj. The VM asks which on
  // every call; a flag answers that without an RTTI walk (dynamic_pointer_cast
  // was a measurable share of every call, worst on the Pi's in-order core).
  bool is_closure = false;
  bool gc_tracked = false;   // already in a Vm's cycle-collector list (see Vm::track_container)
};

inline bool Value::is_container() const {
  if (!obj) return false;
  return type == Type::List || type == Type::Map || type == Type::Entity ||
         (type == Type::Fn && obj->is_closure);
}

class StrObj final : public Obj {
 public:
  explicit StrObj(std::string s) : Obj(Type::Str), text(std::move(s)) {}
  std::string text;
};

// A fixed-size run of mutable bytes: binary files, wire protocols, pixels. Like
// a list it is shared by reference, so a function that fills a buffer fills the
// caller's. Strings stay the way to hold text; bytes are for data that is not.
class BytesObj final : public Obj {
 public:
  explicit BytesObj(std::vector<uint8_t> v) : Obj(Type::Bytes), data(std::move(v)) {}
  std::vector<uint8_t> data;
};

// An exact decimal number: mantissa / 10^scale, held as a 128-bit integer (38 digits) and a scale from 0 to 30.
// Immutable, so it is shared between VMs like a string. 1.50 and 1.5 are the same number and compare equal; the
// scale only decides how it prints. See grain_std_decimal.cpp for the arithmetic and its rules.
class DecimalObj final : public Obj {
 public:
  DecimalObj(AIOS_INT128 m, int s) : Obj(Type::Decimal), mant(m), scale(s) {}
  AIOS_INT128 mant;
  int scale;
};

class ListObj final : public Obj {
 public:
  explicit ListObj(std::vector<Value> v) : Obj(Type::List), items(std::move(v)) {}
  std::vector<Value> items;
};

// Insertion-ordered. A map that reorders itself between runs makes every
// program that prints one non-reproducible, and reproducibility is worth more
// here than the constant factor.
//
// Most maps are records - {id, qty, price} - so up to kIndexFrom entries a
// lookup is a scan (length first, then bytes), and the hash index is only
// built when a map grows past that. Building 200k three-field records kept an
// unordered_map each (buckets, nodes, a second copy of every key) and hashed
// every field read: 3.5x slower than CPython at the same work.
class MapObj final : public Obj {
 public:
  static constexpr size_t kIndexFrom = 8;
  MapObj() : Obj(Type::Map) {}
  std::vector<std::pair<std::string, Value>> entries;
  // Past kIndexFrom entries, a lookup table: open addressing over positions in `entries` (slot = position + 1,
  // 0 = free, a power-of-two size, at most half full), with each entry's hash kept in `hashes` so growing the
  // table rehashes no strings and a probe compares a number before it compares text. No key is stored twice and
  // an insert allocates nothing of its own. Both are empty for a small map, which is scanned.
  std::vector<uint32_t> slots;
  std::vector<uint32_t> hashes;
  Value* find(const std::string& k);
  void put(const std::string& k, Value v);
  bool erase(const std::string& k);
  void clear_all() { entries.clear(); slots.clear(); hashes.clear(); }
 private:
  static uint32_t hash_of(const std::string& k);
  void rebuild_slots(size_t at_least);
};

// ---------------------------------------------------------------------------
// Substrate & Architecture Objects (AIOS Native)
// ---------------------------------------------------------------------------

class Id128Obj final : public Obj {
 public:
  explicit Id128Obj(aios::Id128 ident) : Obj(Type::Id128), id(ident) {}
  aios::Id128 id;
};

class CapObj final : public Obj {
 public:
  CapObj(aios::Id128 ident, std::string res, uint16_t rts, uint64_t exp = 0, aios::Id128 parent = {}, std::string hold = "")
      : Obj(Type::Capability), id(ident), resource(std::move(res)), rights(rts), expiry(exp), parent_id(parent), holder(std::move(hold)) {}
  aios::Id128 id;
  std::string resource;
  uint16_t rights = 0;
  uint64_t expiry = 0;
  aios::Id128 parent_id;
  std::string holder;

  bool can(uint16_t r) const { return (rights & r) == r; }
  bool has_expired(uint64_t current_gen) const { return expiry != 0 && current_gen >= expiry; }
  std::shared_ptr<CapObj> attenuate(uint16_t narrower_rights, const aios::Id128& new_id) const {
    if ((narrower_rights & ~rights) != 0) return nullptr;
    return std::make_shared<CapObj>(new_id, resource, narrower_rights, expiry, id, holder);
  }
};

class FrameObj final : public Obj {
 public:
  FrameObj(aios::Id128 ident, uint64_t gen, aios::Id128 pub, Value pay, uint64_t ts = 0)
      : Obj(Type::StateFrame), id(ident), generation(gen), publisher(pub), payload(std::move(pay)), timestamp(ts) {}
  aios::Id128 id;
  uint64_t generation = 0;
  aios::Id128 publisher;
  Value payload;
  uint64_t timestamp = 0;
};

class ChannelObj final : public Obj {
 public:
  ChannelObj(std::string nm, aios::Id128 ident, uint32_t cap = 256)
      : Obj(Type::Channel), name(std::move(nm)), id(ident), capacity(cap) {}
  std::string name;
  aios::Id128 id;
  uint32_t capacity = 256;
  uint64_t generation = 0;
  uint64_t head = 0;
  uint64_t tail = 0;
  uint64_t dropped = 0;
  std::vector<std::shared_ptr<FrameObj>> ring;

  bool publish(Value payload, aios::Id128 publisher = {}, aios::Id128 frame_id = {});
  std::shared_ptr<FrameObj> consume();
};

class MemObj final : public Obj {
 public:
  MemObj(aios::Id128 ident, size_t sz, std::string prov = "ram")
      : Obj(Type::MemoryObject), id(ident), size(sz), provider(std::move(prov)), buffer(sz, 0) {}
  aios::Id128 id;
  size_t size = 0;
  std::string provider;
  std::vector<uint8_t> buffer;
  uint64_t generation = 0;

  bool write(size_t offset, const std::string& data);
  std::string read(size_t offset, size_t len) const;
  bool migrate(const std::string& new_provider);
};

class EntityObj final : public Obj {
 public:
  EntityObj(std::string nm, aios::Id128 ident)
      : Obj(Type::Entity), name(std::move(nm)), id(ident) {}
  std::string name;
  aios::Id128 id;
  std::unordered_map<std::string, Value> fields;
  std::unordered_map<std::string, Value> handlers;

  Value get_field(const std::string& k) const;
  void set_field(const std::string& k, Value v);
};

class Vm;

class BoundMethodObj final : public Obj {
 public:
  using MethodFn = Value (*)(Vm& vm, BoundMethodObj& bm, std::vector<Value>& args, std::string& error);
  BoundMethodObj(Value rec, std::string nm, int min_a, int max_a, MethodFn f)
      : Obj(Type::BoundMethod), receiver(std::move(rec)), name(std::move(nm)),
        min_args(min_a), max_args(max_a), fn(f) {}
  Value receiver;
  std::string name;
  int min_args = 0;
  int max_args = -1;
  MethodFn fn = nullptr;
};

// ---------------------------------------------------------------------------
// Bytecode
// ---------------------------------------------------------------------------
enum class Op : uint8_t {
  Constant, Nil, True, False, Pop,
  GetLocal, SetLocal, GetGlobal, SetGlobal, DefineGlobal, GetUpvalue, SetUpvalue,
  Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual,
  Add, Subtract, Multiply, Divide, Modulo, Negate, Not,
  BitAnd, BitOr, BitXor, BitNot, Shl, Shr,
  Dup, Dup2,   // copy the top one / two values (compound assignment's target, evaluated once)
  Jump, JumpIfFalse, JumpIfTrue, Loop,
  Call, Closure, CloseUpvalue, Return,
  BuildList, BuildMap, IndexGet, IndexSet, FieldGet, OptFieldGet, FieldSet,
  IterPrep, IterNext,
  Halt,
  // Spread (`f(a, ...xs)`, `[a, ...xs]`): ListConcat n joins the n lists on the stack into one; CallList calls the
  // value under the list with the list's items as its arguments. Added last so the ops before keep their numbers.
  ListConcat, CallList
};

const char* op_name(Op op);

struct Chunk {
  std::vector<uint8_t> code;
  std::vector<Value> constants;
  std::vector<int> lines;   // one per byte of `code`, so a fault names a line
  std::vector<int> columns; // one per byte of `code`, so a fault names a column
  // GetGlobal/SetGlobal inline cache: constant index -> the global's slot in
  // the Vm that last ran this chunk (map nodes never move and globals are
  // never erased, so the pointer stays good). Saves hashing the name on every
  // reference - every recursive call to `fib` looked `fib` up by string.
  mutable std::vector<Value*> global_cache;
  mutable uint64_t global_cache_owner = 0;   // Vm serial, never an address: a freed Vm's address comes back
  size_t add_constant(const Value& v);
  void write(uint8_t byte, int line, int column = 0) {
    code.push_back(byte);
    lines.push_back(line);
    columns.push_back(column);
  }
};

// A compiled function. `upvalue_from_local` says, for each upvalue, whether it
// is captured out of the enclosing frame's locals or out of ITS upvalues --
// which is the whole of what makes a closure over a closure work.
class FnObj final : public Obj {
 public:
  FnObj() : Obj(Type::Fn) {}
  ~FnObj() override;
  std::string name = "<script>";
  int arity = 0;
  int local_slots = 0;
  Chunk chunk;
  // Where each named local lives, for the debugger: the stack slot, and the span of code (from, to) it is in scope for.
  struct DebugLocal { std::string name; int slot; uint32_t from; uint32_t to; };
  std::vector<DebugLocal> debug_locals;
  std::vector<bool> upvalue_from_local;
  std::vector<uint8_t> upvalue_index;
  // Native code for this function (grain_jit_arm64.cpp), made the first time
  // it is called on a machine that can run it. Null = interpret.
  void* jit_code = nullptr;
  bool jit_tried = false;
};

// A variable a closure captured. Open, it is still the enclosing call's stack
// slot (`slot`, in the VM running that call); closed, the value lives here.
class Upvalue {
 public:
  size_t slot = 0;
  Value closed;
  bool is_closed = false;
};

class ClosureObj final : public Obj {
 public:
  ClosureObj() : Obj(Type::Fn) { is_closure = true; }
  std::shared_ptr<FnObj> fn;
  std::vector<std::shared_ptr<Upvalue>> upvalues;
};

inline int callable_arity(const Value& f) {
  if (f.type != Type::Fn || !f.obj) return -1;
  const Obj* o = f.obj.get();
  if (o->is_closure) return static_cast<const ClosureObj*>(o)->fn ? static_cast<const ClosureObj*>(o)->fn->arity : -1;
  return static_cast<const FnObj*>(o)->arity;
}

// A handle to something the language itself has no words for -- a socket, a
// listener, a file. grain can hold it, pass it and let it go; only the native
// that made it can look inside. `kind` is what makes a native able to refuse a
// listener where a connection was meant, which an opaque pointer alone cannot.
class ExternObj final : public Obj {
 public:
  ExternObj(std::string k, std::shared_ptr<void> p) : Obj(Type::Extern), kind_name(std::move(k)), ptr(std::move(p)) { ++live; }
  ~ExternObj() override { --live; }
  std::string kind_name;
  std::shared_ptr<void> ptr;
  // How many exist, in any VM: a connection, socket, canvas or task is cleaned up properly only by being freed,
  // so Vm::retire_heap() leaves the heap alone to the operating system only when there are none.
  static inline std::atomic<int> live{0};
};

// A hosted app's settings (Vm::settings): shared by its VMs, which may run on several threads.
struct VmSettings {
  std::mutex mu;
  std::map<std::string, std::string> values;
};

class Vm;
using NativeFn = Value (*)(Vm& vm, std::vector<Value>& args, std::string& error);

class NativeObj final : public Obj {
 public:
  NativeObj(std::string n, int min_a, int max_a, NativeFn f)
      : Obj(Type::Native), name(std::move(n)), min_args(min_a), max_args(max_a), fn(f) {}
  std::string name;
  int min_args, max_args;   // max_args < 0 means variadic
  NativeFn fn;
};

// ---------------------------------------------------------------------------
// Front end
// ---------------------------------------------------------------------------
struct Diagnostic {
  int line = 0;
  int column = 0;
  std::string message;

  Diagnostic() = default;
  Diagnostic(int l, std::string msg) : line(l), column(0), message(std::move(msg)) {}
  Diagnostic(int l, int col, std::string msg) : line(l), column(col), message(std::move(msg)) {}

  std::string format_snippet(const std::string& source, const std::string& origin = "") const;
};

// Every stage reports the same way: a result, and the diagnostics that stopped
// it. Nothing throws across a stage boundary, because the REPL has to survive
// a bad line and ask for the next one.
// `native fn` compiles a typed subset of grain to machine code. It is off until
// the host turns it on: generated code is a thing a sandbox must opt in to.
// `unsafe` adds what can touch memory grain does not own: `ptr`, rd/wr through
// a ptr, `extern fn`, and casting bytes to a ptr. Without it a native function
// can only read and write the `bytes` it is given, bounds-checked.
struct NativePolicy {
  bool allow = false;
  bool unsafe = false;
};
void set_native_policy(NativePolicy policy);
NativePolicy native_policy();

struct CompileResult {
  std::shared_ptr<FnObj> script;   // null when it did not compile
  std::vector<Diagnostic> errors;
  bool ok() const { return script != nullptr && errors.empty(); }
};

// `keep_last_value` makes a trailing bare expression the script's result
// instead of discarding it. A file does not want that -- its last line is a
// statement like any other -- but a REPL that prints nothing for `1 + 1` is
// not a REPL.
CompileResult compile_source(const std::string& source, const std::string& origin,
                             bool keep_last_value = false);

// The disassembler is part of the language, not a debug afterthought: it is how
// anyone checks that the compiler emitted what the source said.
std::string disassemble(const FnObj& fn);

// ---------------------------------------------------------------------------
// Virtual machine
// ---------------------------------------------------------------------------
// Files, through whatever storage the machine running the VM actually has:
// ordinary paths on a hosted build, the SD card's FAT32 volume on the Pi. The
// language owns the builtins (argument checks, what a missing file means);
// the embedder owns only this. A Vm with no Files refuses the file builtins
// by name instead of pretending to have written anything.
struct FileEntry {
  std::string name;
  int64_t size = 0;
  bool dir = false;
};
struct Files {
  virtual ~Files() = default;
  virtual bool read(const std::string& path, std::string& out, std::string& error) = 0;
  virtual bool write(const std::string& path, const std::string& data, bool append, std::string& error) = 0;
  virtual bool list(const std::string& path, std::vector<FileEntry>& out, std::string& error) = 0;
  virtual bool remove(const std::string& path, std::string& error) = 0;
  virtual bool exists(const std::string& path) = 0;
  // Directories. Storage without them (or an embedder that has not said how)
  // refuses by name rather than pretending.
  virtual bool make_dir(const std::string& path, bool parents, std::string& error) {
    (void)path; (void)parents;
    error = "this machine's storage cannot make directories";
    return false;
  }
  virtual bool remove_dir(const std::string& path, std::string& error) {
    (void)path;
    error = "this machine's storage cannot remove directories";
    return false;
  }
  virtual bool is_dir(const std::string& path) { (void)path; return false; }
  // The path made absolute against the working directory; false where there is none.
  virtual bool absolute(const std::string& path, std::string& out, std::string& error) {
    (void)path; (void)out;
    error = "this machine's storage has no working directory";
    return false;
  }
};

// Cycles spent in the native call path's pieces (filled on the Pi only).
struct JitProfile {
  uint64_t slow_calls = 0;                    // calls that went through Vm::jit_call (natives, first calls, ...)
  uint64_t ret = 0, ret_n = 0;                // jit_return
  uint64_t get_global = 0, get_global_n = 0;  // jit_get_global
  uint64_t step = 0, step_n = 0;              // step_one (includes nested work)
  uint64_t step_by_op[64] = {};               // which instructions fell back to the interpreter
};
extern JitProfile g_jit_profile;

class Vm {
 public:
  Vm();
  ~Vm();

  // Runs a compiled script to completion. `output` collects everything the
  // program printed, so a caller that is not a terminal -- the desktop, a
  // test, a dream -- gets the text rather than losing it to stdout.
  struct RunResult {
    bool ok = false;
    Value value;                  // what the script's last expression produced
    std::string error;
    int line = 0;
    int column = 0;
    std::vector<std::string> trace;   // innermost call first
  };
  RunResult run(const std::shared_ptr<FnObj>& script);
  Value call(const Value& callee, const std::vector<Value>& args, std::string& error);

  void define_native(const std::string& name, int min_args, int max_args, NativeFn fn);
  void define_global(const std::string& name, Value v);
  Value* find_global(const std::string& name);
  // Every global's name, sorted (what an editor offers to complete).
  std::vector<std::string> global_names() const;
  // This VM's number, unique for the life of the process (never an address, which a later VM can reuse).
  uint64_t serial() const;
  // Drops every global not named in `keep`: a sandbox, since what a program
  // cannot name it cannot call. Only before the first run - the global inline
  // cache assumes globals are never erased once a chunk has looked one up.
  void retain_globals(const std::vector<std::string>& keep);

  // Where `print` goes. Empty by default, which means stdout.
  std::vector<std::string>* sink = nullptr;
  void emit(const std::string& line);

  // Structured logging configuration:
  // log_level: 0 = debug, 1 = info, 2 = warn, 3 = error
  int log_level = 1;
  // log_format: 0 = text, 1 = json
  int log_format = 0;

  // Where read_file/write_file/... go. Not owned; null = no file storage.
  Files* files = nullptr;

  // What a hosted app (grain_app.cpp) gives the VMs it starts - its sessions, its workers - and nothing else does:
  // its own settings, which getenv()/setenv() use before the process's environment (apps sharing a process each
  // see their own), where its print() goes (its log), and the flag its host raises to stop it (sleep() returns at
  // once). Null: the process's environment, `sink`/stdout, nothing to stop.
  std::shared_ptr<VmSettings> settings;
  std::shared_ptr<KvStore> kv;   // the app's kv_* store; null: the process's, in memory
  std::function<void(const std::string&)> on_print;
  std::shared_ptr<std::atomic<bool>> stop_flag;

  // import "lib.gr" (grain_builtins.cpp): relative paths resolve against base_dir - the directory of the program
  // that is running, set by whatever starts it; empty means the working directory. `imported` is what this VM has
  // loaded, so a file imported twice (or by a cycle) runs once. packed_source, if set, is asked first: a program
  // built with `grain build` carries its modules inside itself.
  std::string base_dir;
  std::vector<std::string> imported;
  std::function<bool(const std::string& path, std::string& source)> packed_source;

  // A program that loops forever must not take the console with it.
  uint64_t step_limit = 0;   // 0 = no limit
  uint64_t steps = 0;

  // Data that changes on its own. `every(seconds, fn)` and `after(seconds, fn)` register here; whatever
  // runs the program - a window's frame loop, the web server, `wait(seconds)` - calls run_timers() often
  // and fires what is due. A view that reads the data shows the change without being told.
  struct Timer {
    uint64_t id = 0;
    double due_ms = 0;
    double period_ms = 0;   // 0: once
    Value fn;
  };
  std::vector<Timer> timers;
  uint64_t next_timer_id = 1;

  // Set on a VM that runs a parallel_map() lane or a spawn()ed task (grain_std_parallel.cpp). A
  // worker has no loop of its own to fire timers or show panes, so every()/after()/live()/events()
  // and run_panes() refuse there instead of registering something that would never run.
  bool worker = false;
  // spawn()'s tasks this VM started and has not awaited; dropping it (with the VM) waits for them,
  // so no task is still running grain when the program that started it is gone.
  std::shared_ptr<void> tasks;

  // For a program that has finished and whose process is about to end. Waits for the tasks it started, as
  // dropping the VM would; then, if nothing it holds needs closing properly (a database connection says goodbye
  // when freed, a socket closes), leaves the heap for the operating system to take back instead of freeing it
  // object by object - a program with a few hundred thousand records spent a fifth of its run doing that. Returns
  // false, having freed nothing, when something did need freeing. The VM must not run again after it returns true.
  bool retire_heap();

  // Compile functions to native code where the machine can run it (AArch64:
  // the Pi's kernel, or any arm64 host; x86-64 hosts). Off = interpret
  // everything, which is also the reference the native code is checked against.
  bool jit_enabled = true;

  // type()'s answers, one string per type name, made on first use (see bi_type).
  std::vector<Value> type_names;

  // Execute exactly one instruction - the one at `ip` in the current frame -
  // with the interpreter's own semantics. The native code hands every
  // uncommon instruction here, which is what keeps the two identical.
  bool step_one(size_t ip);
  // The native code's Call (instruction at `ip` in the current frame) and
  // Return; see grain_jit_arm64.cpp.
  bool jit_call(size_t ip);
  void jit_return();
  bool jit_get_global(size_t ip);

  // What a captured variable holds now: its stack slot in this VM while open, its own copy once
  // closed. How a closure's state is read when it is copied to a worker.
  Value upvalue_value(const Upvalue& uv) const;

  // Cycle collection: finds unreachable cycles among tracked containers and breaks them
  size_t collect_cycles();
  void track_container(const std::shared_ptr<Obj>& obj);

  // Debugger support
  struct DebugState {
    bool enabled = false;
    bool step_line = false;
    bool step_over = false;
    size_t step_over_depth = 0;
    std::string current_file;
    std::vector<int> frame_line;   // per call depth: the line that frame was last seen on (a line is entered once per visit)
    std::unordered_set<int> breakpoints;
    std::function<void(Vm& vm, const std::string& file, int line)> on_break;
  };
  DebugState debug_state;
  size_t frame_depth() const;
  std::vector<std::string> stack_trace() const;
  std::vector<std::pair<std::string, Value>> current_locals() const;

 private:
  struct Frame;
  struct State;
  std::unique_ptr<State> st_;
  bool execute_loop(size_t target_frame_depth, bool single_step = false);
  bool run_native(void* code, size_t base);
  bool invoke(const Value& callee, uint8_t argc, int line, int column = 0);
  void close_upvalues_from(size_t from);
};

// The portable standard library: print/len/str/num/type/push/keys/range/...,
// strings, math, JSON, files (through Vm::files), the canvas and its widgets.
// Everything a grain built without aios has (grain_builtins.cpp).
void install_core_builtins(Vm& vm);

// Parts of install_core_builtins, one file each:
void install_hash_builtins(Vm& vm);    // sha256, sha1, sha512, sha384, md5, keccak256, hmac_sha256, random_bytes (grain_std_hash.cpp)
void install_regex_builtins(Vm& vm);   // regex_test/match/find_all/replace/split (grain_std_regex.cpp)
// Fires the VM's due timers. True when at least one ran (the program's data may have changed); an
// error in a timer's function goes to `error` and that timer is dropped. `until_next_ms` (if given)
// is how long until the next one is due, or -1 when there are none.
bool run_timers(Vm& vm, std::string& error, double* until_next_ms = nullptr);
double wall_ms();   // the clock timers use: milliseconds, monotonic
void install_time_builtins(Vm& vm);    // now, date_parts/format/parse/make (grain_std_time.cpp)
void install_text_builtins(Vm& vm);    // format, sort_by, group_by, uuid (grain_std_text.cpp)
void install_bytes_builtins(Vm& vm);   // bytes, bytes_get/set, pack/unpack, bytes_* (grain_std_bytes.cpp)
void install_path_builtins(Vm& vm);    // path_join, basename, dirname, extname, mkdir, ... (grain_std_path.cpp)
// parallel_map, spawn, await, task_done: grain on every core, each worker a VM of its own (grain_std_parallel.cpp).
void install_parallel_builtins(Vm& vm);
void install_decimal_builtins(Vm& vm);   // decimal(), dec_div(), scale() (grain_std_decimal.cpp)

// The bridges that make this a language ON aios rather than beside it: the
// sidecar's C ABI, the SAT solver, the MariaDB wire client and ASQL
// (grain_aios.cpp).
void install_aios_builtins(Vm& vm);

// Both: what every VM inside aios gets.
void install_builtins(Vm& vm);

// udp_* / tcp_* over hal::net (grain_sockets.cpp). Separate from
// install_builtins() so a sandboxed VM (the sidecar's aios_grain_run) has no
// network unless its embedder hands it one; `aios grain` and the Pi's
// console do.
void install_socket_builtins(Vm& vm);

// http_get / http_post / http_request over hal::net and TLS (grain_std_http.cpp).
// Like sockets, granted by the embedder rather than part of every VM. https
// verifies against <root>/ssl/ca-certificates.crt when present, else the
// system's store; the root is process-wide and set once by the embedder.
void install_http_builtins(Vm& vm);
// db_connect, db_query, db_close and live(): the MySQL/MariaDB/ASQL wire protocol, and query results that stay
// current (grain_std_db.cpp). Like http, the network is a capability the embedder grants.
void install_db_builtins(Vm& vm);
void set_http_trust_root(const std::string& root);

// Windowing & Canvas Pane Registration API (register_pane, pane_render, pane_dispatch_event, ...)
void install_window_builtins(Vm& vm);
// Face widgets as grain values, shown in a pane (`ui.column`, `ui.button`, ..., `ui.pane`;
// grain_face.cpp). Part of the window builtins.
void install_face_builtins(Vm& vm);

// Where panes are shown. The embedder supplies it - real windows in the
// standalone grain (SDL2), the desktop inside aios - and run_panes() hands it
// the VM; with none, run_panes() says so instead of returning as if shown.
struct DisplayRun {
  int64_t max_frames = 0;           // 0: until every pane is closed
  // Input played to the windows before the user's, in order (scripted UI, and
  // how the display is tested without a person at it). Each is an event map as
  // a pane receives it: {type: "mouse_down", pane, x, y, button}, ...
  std::vector<Value> script;
};
class Display {
 public:
  virtual ~Display() = default;
  virtual bool run(Vm& vm, const DisplayRun& run, std::string& error) = 0;
};
void set_display(Display* d);   // not owned
Display* display();

// What a Display uses of the panes a program registered (grain_builtins.cpp).
// Panes belong to the VM that registered them: each call sees only `vm`'s.
struct PaneInfo {
  uint64_t id = 0;
  std::string title;
  int x = 0, y = 0, width = 0, height = 0;
};
std::vector<PaneInfo> pane_list(const Vm& vm);
// Runs the pane's draw callback, then copies its canvas out (ARGB, row-major).
bool pane_frame(Vm& vm, uint64_t id, std::vector<uint32_t>& argb, int& width, int& height, std::string& error);
// Delivers an event map to the pane's callbacks (event, then click or key).
bool pane_event(Vm& vm, uint64_t id, const Value& event, std::string& error);
// The window was resized: the pane's canvas follows, then it gets a "resize" event.
bool pane_resized(Vm& vm, uint64_t id, int width, int height, std::string& error);
// The window was closed: the pane gets a "close" event and is unregistered.
bool pane_closed(Vm& vm, uint64_t id, std::string& error);
// Unregisters every pane of `vm`, with no callbacks: for an embedder that is
// done with the VM (its program ended, or its app was closed).
void drop_panes(const Vm& vm);

// A ui.pane shown in a browser instead of a window (grain_face.cpp renders it, grain_webui.cpp
// serves it). The program is the same one a desktop window runs: its `view` is called, its
// widgets become HTML, and a click or Enter in the browser calls the same grain function.
struct WebPaneInfo {
  uint64_t id = 0;
  std::string title;
  int width = 0, height = 0;
  int refresh = 0;           // seconds between redraws the page asks for (the pane's "refresh" option); 0 = none
  bool shared = false;       // the pane's "shared" option: one copy of the program for every visitor, so they all see the same live data
  std::string css;           // custom CSS stylesheet injected into <style>
  std::string theme;         // preset theme (e.g. "cyber", "nord", "matrix", "sunset", "glass")
};
// The first ui.pane the program made, if it made one.
bool web_pane_first(const Vm& vm, WebPaneInfo& out);
// Calls the pane's view and returns the widgets as an HTML fragment. `version` changes on every
// render: an event names the version it was shown, so one from an old page is ignored.
bool web_pane_render(Vm& vm, uint64_t id, std::string& html, uint64_t& version, std::string& error);
// The same widgets as data rather than HTML, for a native client (`grain open`): a JSON object
// {"widget", "opts", "label", "id", "act", "enter", "value", "data", "children"} per widget, where "act"
// and "enter" name the action to send back for a button or a field's Enter. `version` as above.
bool web_pane_render_tree(Vm& vm, uint64_t id, std::string& json, uint64_t& version, std::string& error);
// A click or Enter: `fields` are the texts typed in the browser's fields, `action` the button or
// field the page named (empty: only the fields changed). Call web_pane_render afterwards.
bool web_pane_event(Vm& vm, uint64_t id, uint64_t version, const std::string& action,
                    const std::vector<std::pair<std::string, std::string>>& fields, std::string& error);
// ---- decimals (grain_std_decimal.cpp) ----
// op is one of + - * / %. Either side may be an Int or a Decimal, not a Real. False with `error` set on a
// mix with a real, an overflow, or division by zero.
bool decimal_arith(char op, const Value& a, const Value& b, Value& out, std::string& error);
bool decimal_negate(const Value& a, Value& out, std::string& error);
// -1, 0, 1. Both sides Int or Decimal.
int decimal_compare(const Value& a, const Value& b);
std::string decimal_text(const Value& d);
// "19.99", "-0.5", "1e3", "  7 " -> a Decimal.
bool decimal_parse(const std::string& text, Value& out, std::string& error);
Value decimal_from_int(int64_t v);
// Rounded half away from zero (PHP's round(), and money's) to `places` decimals, 0..30.
bool decimal_round(const Value& d, int places, Value& out, std::string& error);
double decimal_to_double(const Value& d);
bool decimal_to_int(const Value& d, int64_t& out);   // truncated toward zero; false if it does not fit
inline bool is_decimal(const Value& v) { return v.type == Type::Decimal; }
// Int or Decimal: what decimal arithmetic accepts.
inline bool is_exact_number(const Value& v) { return v.type == Type::Int || v.type == Type::Decimal; }

using StreamHeaderCallback = std::function<bool(int status, const std::vector<std::pair<std::string, std::string>>& headers)>;
using StreamChunkCallback = std::function<bool(const std::string& chunk)>;

void format_one_sse_event(const Value& ev, std::string& out);

// If the pane defined custom HTTP routes (ui.pane({"routes": {"/path": fn(req)}})),
// delivers the request and fills status, res_body, and content_type. Returns true if handled.
// The handler gets {method, path, query, body, headers (names lowercased), remote}.
bool web_pane_handle_route(Vm& vm, uint64_t id, const std::string& method, const std::string& path,
                           const std::string& query, const std::string& body,
                           const std::vector<std::pair<std::string, std::string>>& headers, const std::string& remote,
                           int& status, std::string& res_body, std::string& content_type, std::string& error,
                           std::vector<std::pair<std::string, std::string>>* res_headers = nullptr,
                           StreamHeaderCallback on_headers = nullptr,
                           StreamChunkCallback on_chunk = nullptr,
                           bool* out_streamed = nullptr);

// True for a pane made by serve_routes: it has routes and no page.
bool web_pane_api_only(Vm& vm, uint64_t id);

// A call made in a worker VM: the function and what it reaches are copied (grain_std_parallel.cpp's
// rules), after which run() needs nothing from the VM they came from, on any thread.
struct WorkerCall {
  std::unique_ptr<Vm> vm;
  Value fn;
  std::vector<Value> args;
  std::vector<std::string> printed;
  Value result;
  std::string error;
  void run();
};
// Copies `fn` out of `vm` (read only: hold whatever lock keeps `vm` still, and only for this call).
std::unique_ptr<WorkerCall> prepare_worker_call(Vm& vm, const Value& fn, std::vector<Value> args, std::string& error);

// Routes that run in worker VMs (ui.pane({..., "workers": true}), serve_routes). prepare picks the
// handler and copies it (call it under the session's lock); the caller then runs the WorkerCall
// with no lock held and hands it to finish for the response. Returns false, with `error` empty,
// when this pane has no such route or does not use workers.
struct RouteJob {
  std::unique_ptr<WorkerCall> call;
  std::vector<std::pair<std::string, std::string>> headers;
  std::shared_ptr<void> writer_state;
  uint64_t writer_id = 0;
  bool streamed = false;
};
bool web_pane_route_prepare(Vm& vm, uint64_t id, const std::string& method, const std::string& path,
                            const std::string& query, const std::string& body,
                            const std::vector<std::pair<std::string, std::string>>& headers, const std::string& remote,
                            RouteJob& job, std::string& error,
                            StreamHeaderCallback on_headers = nullptr,
                            StreamChunkCallback on_chunk = nullptr);
bool web_pane_route_finish(RouteJob& job, int& status, std::string& res_body, std::string& content_type, std::string& error);
// The fleet member a route was given to (`on_machine(member, handler)`), "" for a route of this machine's own or none.
std::string web_pane_route_machine(Vm& vm, uint64_t id, const std::string& method, const std::string& path);

}  // namespace aios::userland::grain

