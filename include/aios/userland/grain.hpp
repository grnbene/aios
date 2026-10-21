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
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aios::userland::grain {

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

enum class Type : uint8_t { Nil, Bool, Int, Real, Str, List, Map, Fn, Native, Extern };

const char* type_name(Type t);

struct Value {
  Type type = Type::Nil;
  union {
    bool b;
    int64_t i;
    double d;
  };
  ObjRef obj;  // Str, List, Map, Fn, Native

  Value() : type(Type::Nil), i(0) {}
  static Value nil() { return Value(); }
  static Value boolean(bool v) { Value x; x.type = Type::Bool; x.b = v; return x; }
  static Value integer(int64_t v) { Value x; x.type = Type::Int; x.i = v; return x; }
  static Value real(double v) { Value x; x.type = Type::Real; x.d = v; return x; }
  static Value object(Type t, ObjRef o) { Value x; x.type = t; x.i = 0; x.obj = std::move(o); return x; }
  static Value string(std::string s);
  static Value list(std::vector<Value> v);

  bool is_number() const { return type == Type::Int || type == Type::Real; }
  // Only nil and false are false. An empty string and 0 are values, and a
  // language that calls them false makes `if count` mean two different things
  // depending on whether count is a number or a list.
  bool truthy() const { return !(type == Type::Nil || (type == Type::Bool && !b)); }
  double as_double() const { return type == Type::Int ? (double)i : type == Type::Real ? d : 0.0; }
  int64_t as_int() const { return type == Type::Int ? i : type == Type::Real ? (int64_t)d : 0; }

  const std::string& str() const;
  std::vector<Value>& list_items() const;
  std::string to_text() const;      // what `print` shows
  std::string repr() const;         // what a value inside a list shows
  bool equals(const Value& o) const;
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
};

class StrObj final : public Obj {
 public:
  explicit StrObj(std::string s) : Obj(Type::Str), text(std::move(s)) {}
  std::string text;
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
  std::unordered_map<std::string, size_t> index;   // empty until the map has more than kIndexFrom entries
  Value* find(const std::string& k);
  void put(const std::string& k, Value v);
  bool erase(const std::string& k);
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
  BuildList, BuildMap, IndexGet, IndexSet, FieldGet, FieldSet,
  IterPrep, IterNext,
  Halt
};

const char* op_name(Op op);

struct Chunk {
  std::vector<uint8_t> code;
  std::vector<Value> constants;
  std::vector<int> lines;   // one per byte of `code`, so a fault names a line
  // GetGlobal/SetGlobal inline cache: constant index -> the global's slot in
  // the Vm that last ran this chunk (map nodes never move and globals are
  // never erased, so the pointer stays good). Saves hashing the name on every
  // reference - every recursive call to `fib` looked `fib` up by string.
  mutable std::vector<Value*> global_cache;
  mutable uint64_t global_cache_owner = 0;   // Vm serial, never an address: a freed Vm's address comes back
  size_t add_constant(const Value& v);
  void write(uint8_t byte, int line) { code.push_back(byte); lines.push_back(line); }
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
  std::vector<bool> upvalue_from_local;
  std::vector<uint8_t> upvalue_index;
  // Native code for this function (grain_jit_arm64.cpp), made the first time
  // it is called on a machine that can run it. Null = interpret.
  void* jit_code = nullptr;
  bool jit_tried = false;
};

class Upvalue;
class ClosureObj final : public Obj {
 public:
  ClosureObj() : Obj(Type::Fn) { is_closure = true; }
  std::shared_ptr<FnObj> fn;
  std::vector<std::shared_ptr<Upvalue>> upvalues;
};

// A handle to something the language itself has no words for -- a socket, a
// listener, a file. grain can hold it, pass it and let it go; only the native
// that made it can look inside. `kind` is what makes a native able to refuse a
// listener where a connection was meant, which an opaque pointer alone cannot.
class ExternObj final : public Obj {
 public:
  ExternObj(std::string k, std::shared_ptr<void> p) : Obj(Type::Extern), kind_name(std::move(k)), ptr(std::move(p)) {}
  std::string kind_name;
  std::shared_ptr<void> ptr;
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
  std::string message;
};

// Every stage reports the same way: a result, and the diagnostics that stopped
// it. Nothing throws across a stage boundary, because the REPL has to survive
// a bad line and ask for the next one.
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
    std::vector<std::string> trace;   // innermost call first
  };
  RunResult run(const std::shared_ptr<FnObj>& script);
  Value call(const Value& callee, const std::vector<Value>& args, std::string& error);

  void define_native(const std::string& name, int min_args, int max_args, NativeFn fn);
  void define_global(const std::string& name, Value v);
  Value* find_global(const std::string& name);
  // Drops every global not named in `keep`: a sandbox, since what a program
  // cannot name it cannot call. Only before the first run - the global inline
  // cache assumes globals are never erased once a chunk has looked one up.
  void retain_globals(const std::vector<std::string>& keep);

  // Where `print` goes. Empty by default, which means stdout.
  std::vector<std::string>* sink = nullptr;
  void emit(const std::string& line);

  // Where read_file/write_file/... go. Not owned; null = no file storage.
  Files* files = nullptr;

  // A program that loops forever must not take the console with it.
  uint64_t step_limit = 0;   // 0 = no limit
  uint64_t steps = 0;

  // Compile functions to native code where the machine can run it (AArch64:
  // the Pi's kernel, or any arm64 host). Off = interpret everything, which is
  // also the reference the native code is checked against.
  bool jit_enabled = true;

  // Execute exactly one instruction - the one at `ip` in the current frame -
  // with the interpreter's own semantics. The native code hands every
  // uncommon instruction here, which is what keeps the two identical.
  bool step_one(size_t ip);
  // The native code's Call (instruction at `ip` in the current frame) and
  // Return; see grain_jit_arm64.cpp.
  bool jit_call(size_t ip);
  void jit_return();
  bool jit_get_global(size_t ip);

 private:
  struct Frame;
  struct State;
  std::unique_ptr<State> st_;
  bool execute_loop(size_t target_frame_depth, bool single_step = false);
  bool run_native(void* code, size_t base);
  bool invoke(const Value& callee, uint8_t argc, int line);
  void close_upvalues_from(size_t from);
};

// Installs print/len/str/num/type/push/keys/range/... plus the handful of
// bridges that make this a language ON this machine rather than beside it.
void install_builtins(Vm& vm);

// udp_* / tcp_* over hal::net (grain_sockets.cpp). Separate from
// install_builtins() so a sandboxed VM (the sidecar's aios_grain_run) has no
// network unless its embedder hands it one; `aios grain` and the Pi's
// console do.
void install_socket_builtins(Vm& vm);

// Windowing & Canvas Pane Registration API (register_pane, pane_render, pane_dispatch_event, ...)
void install_window_builtins(Vm& vm);

}  // namespace aios::userland::grain

