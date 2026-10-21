#pragma once
// The code generator's state. The member functions are in codegen_core.cpp (emission, scopes, statements,
// functions, classes) and codegen_expr.cpp (expressions, patterns).
#include <deque>
#include <unordered_map>

#include "aios/js/context.hpp"
#include "compiler.hpp"

namespace aios::js {

struct Label {
  int32_t pos = -1;
  std::vector<uint32_t> fixups;   // offsets of i32 operands waiting for this label
  int depth = -1;                 // operand-stack depth at the jump sites
};

// A protected range of code, kept as segments so inlined cleanup code can be left out of it.
struct Rec {
  std::vector<std::pair<uint32_t, uint32_t>> segs;
  uint32_t start = 0;
  bool open = false;
};

struct TryCtx {
  Rec a;                 // the try block
  Rec c;                 // the catch block
  bool has_finally = false;
  Node* finally_node = nullptr;
  int env_depth = 0;
};

struct Control {
  enum class Kind : uint8_t { Loop, Switch, Labeled, Try };
  Kind kind = Kind::Loop;
  std::vector<std::u16string> labels;
  Label* brk = nullptr;
  Label* cont = nullptr;
  int brk_env = 0, cont_env = 0;
  TryCtx* try_ctx = nullptr;
  int phase = 0;                 // Try: 0 in the try block, 1 in the catch block
  int iter_tmp = -1;             // a for-of loop: the iterator registers to close on exit
  Rec* iter_rec = nullptr;       // and the range its cleanup handler covers
};

struct OptSite {
  Label* landing;
  int extra_pops;
};

struct FuncState {
  FunctionCode* code = nullptr;
  FuncInfo* info = nullptr;
  Node* node = nullptr;
  Scope* fscope = nullptr;
  FuncState* parent = nullptr;
  Scope* cur_scope = nullptr;
  int env_depth = 0;
  std::vector<Control> control;
  int depth = 0, max_depth = 0;
  bool reachable = true;
  int completion_reg = -1;
  bool is_script = false;
  bool is_derived_ctor = false;
  VarInfo* this_var = nullptr;
  std::unordered_map<std::u16string, uint32_t> atom_k;
  std::unordered_map<uint64_t, uint32_t> num_k;
  uint32_t last_line = 0;
};

struct Compiler {
  Context& ctx;
  Program& prog;
  Analysis& an;
  std::string err;
  uint32_t err_line = 0;
  FuncState* fs = nullptr;
  std::vector<OptSite>* chain = nullptr;
  std::deque<Label>* chain_labels = nullptr;
  std::shared_ptr<const std::u16string> source;
  JsString* filename = nullptr;
  FunctionCode* last_code = nullptr;   // the code object most recently finished by compile_function

  Compiler(Context& c, Program& p, Analysis& a) : ctx(c), prog(p), an(a) {}

  bool failed() const { return !err.empty(); }
  void fail(const std::string& m, uint32_t line) { if (err.empty()) { err = m; err_line = line; } }

  // ---- emission ----
  void emit(OpCode op);
  void emit_u8(OpCode op, uint8_t a);
  void emit_u16(OpCode op, uint16_t a);
  void emit_u32(OpCode op, uint32_t a);
  void emit_i32(OpCode op, int32_t a);
  void emit_u16_u16(OpCode op, uint16_t a, uint16_t b);
  void emit_u32_u8(OpCode op, uint32_t a, uint8_t b);
  void emit_u8_u32(OpCode op, uint8_t a, uint32_t b);
  void emit_u16_u32(OpCode op, uint16_t a, uint32_t b);
  void emit_u16_u16_u32(OpCode op, uint16_t a, uint16_t b, uint32_t c);
  void emit_u16_i32(OpCode op, uint16_t a, Label& l);
  void emit_u32_u8_u16_u16(OpCode op, uint32_t a, uint8_t b, uint16_t c, uint16_t d);
  void jump(OpCode op, Label& l);
  void patch_jump(uint32_t at, int32_t target);
  void bind(Label& l);
  uint32_t pc() const { return static_cast<uint32_t>(fs->code->code.size()); }
  void adj(int d);
  void unreachable() { fs->reachable = false; }
  void mark_line(uint32_t line);

  uint32_t k_atom(const std::u16string& s);
  uint32_t k_atom_ascii(const char* s);
  uint32_t k_num(double d);
  uint32_t alloc_reg();
  void rec_open(Rec& r);
  void rec_close(Rec& r);
  void add_handlers(Rec& r, uint32_t target, int env_depth, bool is_finally);

  // ---- scopes and variables ----
  void assign_registers(Scope* s);
  void enter_scope(Scope* s);
  void leave_scope(Scope* s);
  void pop_envs_to(int depth);
  int env_hops(Scope* from, Scope* to);
  void load_var(Node* ident);
  void load_var_info(VarInfo* v, const std::u16string& name);
  void load_var_unchecked(VarInfo* v);
  enum class StoreMode { Assign, InitVar, InitLexical };
  void store_var(Node* ident, StoreMode mode);
  void store_var_info(VarInfo* v, const std::u16string& name, StoreMode mode, Node* where);
  VarInfo* lookup_var(const std::u16string& name, Scope* from);

  // ---- statements ----
  void compile_statements(std::vector<Node*>& list);
  void compile_statement(Node* n);
  void compile_block(Node* n);
  void compile_var_decl(Node* n);
  void compile_if(Node* n);
  void compile_for(Node* n, std::vector<std::u16string>* labels);
  void compile_for_in_of(Node* n, std::vector<std::u16string>* labels);
  void compile_while(Node* n, std::vector<std::u16string>* labels);
  void compile_do_while(Node* n, std::vector<std::u16string>* labels);
  void compile_switch(Node* n, std::vector<std::u16string>* labels);
  void compile_try(Node* n);
  void compile_labeled(Node* n);
  void compile_break_continue(Node* n);
  void compile_return(Node* n);
  void emit_exit_to(int target_index, bool is_continue, bool include_target_cleanup);
  void emit_return_value_cleanup();
  void inline_finally(Control& c, int index);
  void hoist_functions(Scope* s);
  void compile_function_decl_stmt(Node* n);

  // ---- functions and classes ----
  uint32_t compile_function(Node* fnode, FuncInfo* fi, bool is_script = false);
  void compile_closure(Node* fnode);
  void compile_class(Node* n);
  void function_prologue(Node* fnode, FuncInfo* fi);

  // ---- expressions ----
  void compile_expr(Node* n);
  void compile_expr_discard(Node* n);
  void compile_member(Node* n);
  void compile_call(Node* n);
  void compile_new(Node* n);
  void compile_args(std::vector<Node*>& list, size_t from, bool* spread);
  void compile_unary(Node* n);
  void compile_update(Node* n);
  void compile_binary(Node* n);
  void compile_logical(Node* n);
  void compile_assign(Node* n);
  void compile_conditional(Node* n);
  void compile_array(Node* n);
  void compile_object(Node* n);
  void compile_template(Node* n);
  void compile_tagged_template(Node* n);
  void compile_optchain(Node* n);
  void compile_yield(Node* n);
  void compile_callee_this(Node* callee);   // pushes [fn this]
  void load_this(Node* n);
  void load_private_name(Node* pn);
  void optional_site(int extra_items);       // value to test on top; short-circuits the enclosing ?. chain
  std::u16string key_string(Node* key);
  uint32_t template_site(Node* tpl);
  // References: a target prepared once, read and written (compound assignment, update).
  enum class RefKind { Var, Prop, Elem, Priv, Super };
  RefKind ref_prepare(Node* t);
  void ref_get(RefKind k, Node* t);          // duplicates the prepared operands and pushes the current value
  void ref_put(RefKind k, Node* t);          // [operands.. value] -> [value]
  // Patterns: the value to bind is on the stack and is consumed.
  void compile_pattern_store(Node* target, StoreMode mode);
  void compile_array_pattern(Node* pat, StoreMode mode);
  void compile_object_pattern(Node* pat, StoreMode mode);
  void compile_default_value(Node* def, Node* target_for_name);
  bool is_anonymous_fn(Node* n);
  void emit_binary_op(Op op);
  bool strict_mode() const { return fs->code->strict; }
  void emit_u32_u32(OpCode op, uint32_t a, uint32_t b);

  Node* delete_node = nullptr;               // the member being deleted at the end of an optional chain
  bool delete_chain = false;
};

}  // namespace aios::js
