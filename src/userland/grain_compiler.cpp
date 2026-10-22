// grain, stage three: tree to bytecode.
//
// One Scope per function being compiled, chained to its enclosing one. A name
// resolves in three steps and the order matters: a local of this function, an
// upvalue captured from an enclosing one, or a global. Getting that order
// wrong is how a language ends up with a `for` variable that every closure in
// the loop shares.
//
// Jumps are written with a placeholder and patched once the target is known,
// which is why every `if` here is emit-jump, compile, patch rather than
// anything that needs a second pass over the whole chunk.
#include <cstring>

#include "grain_native.hpp"
#include "grain_parser.hpp"

namespace aios::userland::grain {

namespace {

// A local slot in the frame. `depth` is the block nesting it was declared at,
// so leaving a block knows how many to drop; `captured` says a closure took a
// reference to it, so leaving has to close that reference rather than just pop.
struct Local {
  std::string name;
  int depth = 0;
  bool captured = false;
};

struct UpvalueSlot {
  uint8_t index = 0;
  bool from_local = false;
};

class Scope {
 public:
  Scope(Scope* enclosing, std::string name) : enclosing_(enclosing) {
    fn_ = std::make_shared<FnObj>();
    fn_->name = std::move(name);
    // Slot 0 is the function itself. Reserving it means a frame's locals and
    // its stack window start at the same place, which the VM relies on.
    locals_.push_back(Local{"", 0, false});
  }

  Scope* enclosing_ = nullptr;
  std::shared_ptr<FnObj> fn_;
  std::vector<Local> locals_;
  std::vector<UpvalueSlot> upvalues_;
  int depth_ = 0;

  // Loop bookkeeping, so break and continue know where to go. A stack because
  // loops nest, and each level collects the jumps that have to be patched to
  // its own end.
  struct Loop {
    size_t continue_target = 0;
    std::vector<size_t> breaks;
    int depth = 0;
  };
  std::vector<Loop> loops_;
};

}  // namespace

const char* type_name(Type t) {
  switch (t) {
    case Type::Nil: return "nil";
    case Type::Bool: return "bool";
    case Type::Int: return "int";
    case Type::Real: return "real";
    case Type::Str: return "string";
    case Type::List: return "list";
    case Type::Map: return "map";
    case Type::Fn: return "fn";
    case Type::Native: return "native";
    case Type::Extern: return "handle";
    case Type::Id128: return "id128";
    case Type::Capability: return "capability";
    case Type::Channel: return "channel";
    case Type::StateFrame: return "frame";
    case Type::MemoryObject: return "memory";
    case Type::Bytes: return "bytes";
    case Type::Entity: return "entity";
    case Type::BoundMethod: return "method";
  }
  return "?";
}

const char* op_name(Op op) {
  switch (op) {
    case Op::Constant: return "CONST"; case Op::Nil: return "NIL";
    case Op::True: return "TRUE"; case Op::False: return "FALSE"; case Op::Pop: return "POP";
    case Op::GetLocal: return "GETLOCAL"; case Op::SetLocal: return "SETLOCAL";
    case Op::GetGlobal: return "GETGLOBAL"; case Op::SetGlobal: return "SETGLOBAL";
    case Op::DefineGlobal: return "DEFGLOBAL";
    case Op::GetUpvalue: return "GETUPVAL"; case Op::SetUpvalue: return "SETUPVAL";
    case Op::Equal: return "EQ"; case Op::NotEqual: return "NE";
    case Op::Less: return "LT"; case Op::LessEqual: return "LE";
    case Op::Greater: return "GT"; case Op::GreaterEqual: return "GE";
    case Op::Add: return "ADD"; case Op::Subtract: return "SUB";
    case Op::Multiply: return "MUL"; case Op::Divide: return "DIV"; case Op::Modulo: return "MOD";
    case Op::BitAnd: return "BITAND"; case Op::BitOr: return "BITOR"; case Op::BitXor: return "BITXOR";
    case Op::BitNot: return "BITNOT"; case Op::Shl: return "SHL"; case Op::Shr: return "SHR";
    case Op::Dup: return "DUP"; case Op::Dup2: return "DUP2";
    case Op::Negate: return "NEG"; case Op::Not: return "NOT";
    case Op::Jump: return "JUMP"; case Op::JumpIfFalse: return "JUMPF";
    case Op::JumpIfTrue: return "JUMPT"; case Op::Loop: return "LOOP";
    case Op::Call: return "CALL"; case Op::Closure: return "CLOSURE";
    case Op::CloseUpvalue: return "CLOSEUP"; case Op::Return: return "RET";
    case Op::BuildList: return "LIST"; case Op::BuildMap: return "MAP";
    case Op::IndexGet: return "IDXGET"; case Op::IndexSet: return "IDXSET";
    case Op::FieldGet: return "FLDGET"; case Op::FieldSet: return "FLDSET";
    case Op::IterPrep: return "ITERPREP"; case Op::IterNext: return "ITERNEXT";
    case Op::Halt: return "HALT";
  }
  return "?";
}

size_t Chunk::add_constant(const Value& v) {
  // Literals repeat constantly -- every `+ 1`, every "" -- and a pool that
  // grows one entry per mention makes a chunk several times its real size.
  for (size_t i = 0; i < constants.size(); ++i)
    if (constants[i].type == v.type && constants[i].equals(v)) return i;
  constants.push_back(v);
  return constants.size() - 1;
}

namespace {

class Compiler {
 public:
  explicit Compiler(std::vector<Diagnostic>* errors) : errors_(errors) {}

  std::shared_ptr<FnObj> compile_script(const std::vector<StmtP>& program, bool keep_last_value) {
    Scope root(nullptr, "<script>");
    scope_ = &root;
    // A trailing bare expression is compiled WITHOUT its Pop and returned, so
    // the value reaches the caller instead of the floor.
    const bool tail_value = keep_last_value && !program.empty() &&
                            program.back()->kind == StmtKind::Expression;
    const size_t n = tail_value ? program.size() - 1 : program.size();
    for (size_t i = 0; i < n; ++i) statement(*program[i]);
    if (tail_value) expression(*program.back()->expr);
    else emit(Op::Nil, 0);
    emit(Op::Return, 0);
    root.fn_->local_slots = (int)root.locals_.size();
    scope_ = nullptr;
    return root.fn_;
  }

 private:
  Chunk& chunk() { return scope_->fn_->chunk; }

  void error(int line, const std::string& msg) {
    if (errors_->size() < 12) errors_->push_back(Diagnostic{line, msg});
  }

  void emit(Op op, int line) { chunk().write((uint8_t)op, line); }
  void emit_byte(uint8_t b, int line) { chunk().write(b, line); }
  void emit_op_arg(Op op, uint8_t arg, int line) { emit(op, line); emit_byte(arg, line); }
  // Constant-pool indexes and literal sizes are two bytes, big-endian: one byte
  // capped a function at 256 constants, which a plain test file with a few
  // hundred string literals already exceeds.
  void emit_op_wide(Op op, size_t arg, int line) {
    emit(op, line);
    emit_byte((uint8_t)((arg >> 8) & 0xff), line);
    emit_byte((uint8_t)(arg & 0xff), line);
  }

  void emit_constant(const Value& v, int line) {
    const size_t k = chunk().add_constant(v);
    if (k > 0xffff) { error(line, "more than 65536 distinct constants in one function"); return; }
    emit_op_wide(Op::Constant, k, line);
  }

  // Jumps are two bytes, big-endian, counted from just after the operand. 64 KiB
  // of body is far past anything a person writes by hand and keeps the
  // instruction fixed-width.
  size_t emit_jump(Op op, int line) {
    emit(op, line);
    emit_byte(0xff, line);
    emit_byte(0xff, line);
    return chunk().code.size() - 2;
  }

  void patch_jump(size_t at) {
    const size_t distance = chunk().code.size() - at - 2;
    if (distance > 0xffff) { error(0, "jump too far"); return; }
    chunk().code[at] = (uint8_t)((distance >> 8) & 0xff);
    chunk().code[at + 1] = (uint8_t)(distance & 0xff);
  }

  void emit_loop(size_t target, int line) {
    emit(Op::Loop, line);
    const size_t distance = chunk().code.size() - target + 2;
    if (distance > 0xffff) { error(line, "loop body too large"); return; }
    emit_byte((uint8_t)((distance >> 8) & 0xff), line);
    emit_byte((uint8_t)(distance & 0xff), line);
  }

  // The instruction for a binary operator token - shared by `a op b` and the
  // compound assignments `x op= v`.
  void emit_binop(Tok op, int line) {
    switch (op) {
      case Tok::Plus: emit(Op::Add, line); break;
      case Tok::Minus: emit(Op::Subtract, line); break;
      case Tok::Star: emit(Op::Multiply, line); break;
      case Tok::Slash: emit(Op::Divide, line); break;
      case Tok::Percent: emit(Op::Modulo, line); break;
      case Tok::Amp: emit(Op::BitAnd, line); break;
      case Tok::Pipe: emit(Op::BitOr, line); break;
      case Tok::Caret: emit(Op::BitXor, line); break;
      case Tok::LtLt: emit(Op::Shl, line); break;
      case Tok::GtGt: emit(Op::Shr, line); break;
      case Tok::EqEq: emit(Op::Equal, line); break;
      case Tok::BangEq: emit(Op::NotEqual, line); break;
      case Tok::Lt: emit(Op::Less, line); break;
      case Tok::LtEq: emit(Op::LessEqual, line); break;
      case Tok::Gt: emit(Op::Greater, line); break;
      case Tok::GtEq: emit(Op::GreaterEqual, line); break;
      default: error(line, "unknown operator"); break;
    }
  }

  size_t name_constant(const std::string& name, int line) {
    const size_t k = chunk().add_constant(Value::string(name));
    if (k > 0xffff) { error(line, "too many names in one function"); return 0; }
    return k;
  }

  // --- scopes and names ---------------------------------------------------

  void begin_block() { scope_->depth_++; }

  void end_block(int line) {
    scope_->depth_--;
    while (!scope_->locals_.empty() && scope_->locals_.back().depth > scope_->depth_) {
      // A captured local cannot simply be popped: something still holds a
      // reference to the slot, and that reference has to be given a home of
      // its own before the frame shrinks past it.
      emit(scope_->locals_.back().captured ? Op::CloseUpvalue : Op::Pop, line);
      scope_->locals_.pop_back();
    }
  }

  int add_local(const std::string& name, int line) {
    if (scope_->locals_.size() >= 255) { error(line, "too many locals in one function"); return -1; }
    scope_->locals_.push_back(Local{name, scope_->depth_, false});
    if ((int)scope_->locals_.size() > scope_->fn_->local_slots)
      scope_->fn_->local_slots = (int)scope_->locals_.size();
    return (int)scope_->locals_.size() - 1;
  }

  static int resolve_local(Scope& s, const std::string& name) {
    for (int i = (int)s.locals_.size() - 1; i >= 0; --i)
      if (s.locals_[i].name == name) return i;
    return -1;
  }

  static int add_upvalue(Scope& s, uint8_t index, bool from_local) {
    for (size_t i = 0; i < s.upvalues_.size(); ++i)
      if (s.upvalues_[i].index == index && s.upvalues_[i].from_local == from_local) return (int)i;
    s.upvalues_.push_back(UpvalueSlot{index, from_local});
    s.fn_->upvalue_from_local.push_back(from_local);
    s.fn_->upvalue_index.push_back(index);
    return (int)s.upvalues_.size() - 1;
  }

  // Walks outward one function at a time. Finding the name two levels up
  // threads an upvalue through the level in between as well -- that middle
  // function has to capture it even though its own body never mentions it.
  static int resolve_upvalue(Scope& s, const std::string& name) {
    if (!s.enclosing_) return -1;
    const int local = resolve_local(*s.enclosing_, name);
    if (local >= 0) {
      s.enclosing_->locals_[local].captured = true;
      return add_upvalue(s, (uint8_t)local, true);
    }
    const int outer = resolve_upvalue(*s.enclosing_, name);
    if (outer >= 0) return add_upvalue(s, (uint8_t)outer, false);
    return -1;
  }

  // --- expressions --------------------------------------------------------

  void expression(const Expr& e) {
    switch (e.kind) {
      case ExprKind::IntLit: emit_constant(Value::integer(e.integer), e.line); return;
      case ExprKind::RealLit: emit_constant(Value::real(e.real), e.line); return;
      case ExprKind::StrLit: emit_constant(Value::string(e.text), e.line); return;
      case ExprKind::BoolLit: emit(e.integer ? Op::True : Op::False, e.line); return;
      case ExprKind::NilLit: emit(Op::Nil, e.line); return;

      case ExprKind::Name: {
        const int local = resolve_local(*scope_, e.text);
        if (local >= 0) { emit_op_arg(Op::GetLocal, (uint8_t)local, e.line); return; }
        const int up = resolve_upvalue(*scope_, e.text);
        if (up >= 0) { emit_op_arg(Op::GetUpvalue, (uint8_t)up, e.line); return; }
        emit_op_wide(Op::GetGlobal, name_constant(e.text, e.line), e.line);
        return;
      }

      case ExprKind::Assign: {
        if (e.op != Tok::End) {                   // x op= v: read x, combine
          const int l = resolve_local(*scope_, e.text);
          if (l >= 0) emit_op_arg(Op::GetLocal, (uint8_t)l, e.line);
          else {
            const int u = resolve_upvalue(*scope_, e.text);
            if (u >= 0) emit_op_arg(Op::GetUpvalue, (uint8_t)u, e.line);
            else emit_op_wide(Op::GetGlobal, name_constant(e.text, e.line), e.line);
          }
          expression(*e.a);
          emit_binop(e.op, e.line);
        } else {
          expression(*e.a);
        }
        const int local = resolve_local(*scope_, e.text);
        if (local >= 0) { emit_op_arg(Op::SetLocal, (uint8_t)local, e.line); return; }
        const int up = resolve_upvalue(*scope_, e.text);
        if (up >= 0) { emit_op_arg(Op::SetUpvalue, (uint8_t)up, e.line); return; }
        emit_op_wide(Op::SetGlobal, name_constant(e.text, e.line), e.line);
        return;
      }

      case ExprKind::Unary:
        expression(*e.a);
        if (e.op == Tok::Minus) emit(Op::Negate, e.line);
        else if (e.op == Tok::Tilde) emit(Op::BitNot, e.line);
        else emit(Op::Not, e.line);
        return;

      case ExprKind::Binary: {
        expression(*e.a);
        expression(*e.b);
        emit_binop(e.op, e.line);
        return;
      }

      case ExprKind::Ternary: {
        expression(*e.a);
        const size_t else_jump = emit_jump(Op::JumpIfFalse, e.line);
        emit(Op::Pop, e.line);
        expression(*e.b);
        const size_t end_jump = emit_jump(Op::Jump, e.line);
        patch_jump(else_jump);
        emit(Op::Pop, e.line);
        expression(*e.c);
        patch_jump(end_jump);
        return;
      }

      // `and` and `or` must not evaluate the right side when the left already
      // decides the answer -- `x != nil and x.name` is the reason.
      case ExprKind::Logical: {
        expression(*e.a);
        const size_t jump = emit_jump(e.op == Tok::And ? Op::JumpIfFalse : Op::JumpIfTrue, e.line);
        emit(Op::Pop, e.line);
        expression(*e.b);
        patch_jump(jump);
        return;
      }

      case ExprKind::Call: {
        expression(*e.a);
        if (e.items.size() > 255) { error(e.line, "more than 255 arguments"); return; }
        for (const auto& arg : e.items) expression(*arg);
        emit_op_arg(Op::Call, (uint8_t)e.items.size(), e.line);
        return;
      }

      case ExprKind::Index:
        expression(*e.a);
        expression(*e.b);
        emit(Op::IndexGet, e.line);
        return;

      case ExprKind::IndexSet:
        expression(*e.a);
        expression(*e.b);
        if (e.op != Tok::End) {                   // a[k] op= v: a and k evaluated once
          emit(Op::Dup2, e.line);
          emit(Op::IndexGet, e.line);
          expression(*e.c);
          emit_binop(e.op, e.line);
        } else {
          expression(*e.c);
        }
        emit(Op::IndexSet, e.line);
        return;

      case ExprKind::Field:
        expression(*e.a);
        emit_op_wide(Op::FieldGet, name_constant(e.text, e.line), e.line);
        return;

      case ExprKind::FieldSet:
        expression(*e.a);
        if (e.op != Tok::End) {                   // a.f op= v: a evaluated once
          emit(Op::Dup, e.line);
          emit_op_wide(Op::FieldGet, name_constant(e.text, e.line), e.line);
          expression(*e.c);
          emit_binop(e.op, e.line);
        } else {
          expression(*e.c);
        }
        emit_op_wide(Op::FieldSet, name_constant(e.text, e.line), e.line);
        return;

      case ExprKind::ListLit: {
        if (e.items.size() > 0xffff) { error(e.line, "list literal longer than 65535"); return; }
        for (const auto& item : e.items) expression(*item);
        emit_op_wide(Op::BuildList, e.items.size(), e.line);
        return;
      }

      case ExprKind::MapLit: {
        if (e.items.size() > 0xffff) { error(e.line, "map literal longer than 65535"); return; }
        for (size_t i = 0; i < e.items.size(); ++i) {
          emit_constant(Value::string(e.keys[i]), e.line);
          expression(*e.items[i]);
        }
        emit_op_wide(Op::BuildMap, e.items.size(), e.line);
        return;
      }

      case ExprKind::Lambda: function(e); return;
    }
  }

  // Compiles a function in its own Scope, then emits the instruction that
  // wraps it into a closure over the upvalues it turned out to need. The
  // operand list is written AFTER the opcode because it is only known once the
  // body has been compiled.
  void function(const Expr& e) {
    Scope fn_scope(scope_, e.fn_name);
    fn_scope.fn_->arity = (int)e.params.size();
    Scope* saved = scope_;
    scope_ = &fn_scope;
    begin_block();
    for (const auto& p : e.params) add_local(p.name, p.line);
    for (const auto& s : e.body) statement(*s);
    // A function that runs off its end returns nil rather than whatever was
    // left on the stack.
    emit(Op::Nil, e.line);
    emit(Op::Return, e.line);
    scope_ = saved;

    const size_t k = chunk().add_constant(Value::object(Type::Fn, fn_scope.fn_));
    if (k > 0xffff) { error(e.line, "too many functions in one function"); return; }
    emit_op_wide(Op::Closure, k, e.line);
    for (const auto& uv : fn_scope.upvalues_) {
      emit_byte(uv.from_local ? 1 : 0, e.line);
      emit_byte(uv.index, e.line);
    }
  }

  void compile_entity_handler(const std::string& entity_name, const OnHandler& h) {
    std::string fn_name = entity_name + "." + h.event;
    Scope fn_scope(scope_, fn_name);
    fn_scope.fn_->arity = 2;  // this, param
    Scope* saved = scope_;
    scope_ = &fn_scope;
    begin_block();
    add_local("this", h.line);
    add_local(h.param.empty() ? "_arg" : h.param, h.line);
    for (const auto& s : h.body) statement(*s);
    emit(Op::Nil, h.line);
    emit(Op::Return, h.line);
    scope_ = saved;

    const size_t k = chunk().add_constant(Value::object(Type::Fn, fn_scope.fn_));
    if (k > 0xffff) { error(h.line, "too many functions in one function"); return; }
    emit_op_wide(Op::Closure, k, h.line);
    for (const auto& uv : fn_scope.upvalues_) {
      emit_byte(uv.from_local ? 1 : 0, h.line);
      emit_byte(uv.index, h.line);
    }
  }

  // --- statements ---------------------------------------------------------

  void statement(const Stmt& s) {
    switch (s.kind) {
      case StmtKind::Expression:
        expression(*s.expr);
        emit(Op::Pop, s.line);
        return;

      case StmtKind::Let: {
        if (s.expr) expression(*s.expr); else emit(Op::Nil, s.line);
        if (scope_->depth_ == 0 && !scope_->enclosing_) {
          emit_op_wide(Op::DefineGlobal, name_constant(s.name, s.line), s.line);
        } else {
          // The initialiser is already on top of the stack and that IS the
          // slot, so declaring the name afterwards costs no instruction --
          // and means `let x = x` reads the OUTER x, which is what it says.
          add_local(s.name, s.line);
        }
        return;
      }

      case StmtKind::NativeFn: {
        if (!natives_) natives_ = new_native_module();
        std::string name;
        Value fn;
        std::vector<Diagnostic> problems;
        if (!compile_native_decl(*natives_, s.native_tokens, name, fn, problems)) {
          for (const auto& d : problems) error(d.line ? d.line : s.line, d.message);
          return;
        }
        if (name.empty()) return;   // an extern fn declares a C function for native code and binds nothing
        emit_constant(fn, s.line);
        if (scope_->depth_ == 0 && !scope_->enclosing_) emit_op_wide(Op::DefineGlobal, name_constant(name, s.line), s.line);
        else add_local(name, s.line);
        return;
      }

      case StmtKind::Block:
        begin_block();
        for (const auto& b : s.body) statement(*b);
        end_block(s.line);
        return;

      case StmtKind::If: {
        expression(*s.expr);
        const size_t else_jump = emit_jump(Op::JumpIfFalse, s.line);
        emit(Op::Pop, s.line);
        begin_block();
        for (const auto& b : s.body) statement(*b);
        end_block(s.line);
        const size_t end_jump = emit_jump(Op::Jump, s.line);
        patch_jump(else_jump);
        emit(Op::Pop, s.line);
        if (!s.other.empty()) {
          begin_block();
          for (const auto& b : s.other) statement(*b);
          end_block(s.line);
        }
        patch_jump(end_jump);
        return;
      }

      case StmtKind::While: {
        const size_t top = chunk().code.size();
        scope_->loops_.push_back(Scope::Loop{top, {}, scope_->depth_});
        expression(*s.expr);
        const size_t exit = emit_jump(Op::JumpIfFalse, s.line);
        emit(Op::Pop, s.line);
        begin_block();
        for (const auto& b : s.body) statement(*b);
        end_block(s.line);
        emit_loop(top, s.line);
        patch_jump(exit);
        emit(Op::Pop, s.line);
        for (size_t j : scope_->loops_.back().breaks) patch_jump(j);
        scope_->loops_.pop_back();
        return;
      }

      // `for x in seq` keeps the sequence and a cursor in two hidden locals
      // that no name can reach, so the loop cannot be broken by a body that
      // reassigns the thing it is walking.
      case StmtKind::ForIn: {
        begin_block();
        expression(*s.iterable);
        emit(Op::IterPrep, s.line);
        const int seq_slot = add_local("  seq", s.line);
        emit_constant(Value::integer(0), s.line);
        add_local("  cursor", s.line);

        const size_t top = chunk().code.size();
        scope_->loops_.push_back(Scope::Loop{top, {}, scope_->depth_});
        // The operand is where the sequence and its cursor live. IterNext has
        // to be told, because the two hidden locals sit wherever the enclosing
        // function's slots happened to reach.
        emit_op_arg(Op::IterNext, (uint8_t)seq_slot, s.line);
        const size_t exit = emit_jump(Op::JumpIfFalse, s.line);
        emit(Op::Pop, s.line);
        begin_block();
        add_local(s.name, s.line);
        for (const auto& b : s.body) statement(*b);
        end_block(s.line);
        emit_loop(top, s.line);
        patch_jump(exit);
        emit(Op::Pop, s.line);
        for (size_t j : scope_->loops_.back().breaks) patch_jump(j);
        scope_->loops_.pop_back();
        end_block(s.line);
        return;
      }

      case StmtKind::Return: {
        if (!scope_->enclosing_) { error(s.line, "return outside a function"); return; }
        if (s.expr) expression(*s.expr); else emit(Op::Nil, s.line);
        emit(Op::Return, s.line);
        return;
      }

      case StmtKind::Break: {
        if (scope_->loops_.empty()) { error(s.line, "break outside a loop"); return; }
        pop_to_loop_depth(s.line);
        scope_->loops_.back().breaks.push_back(emit_jump(Op::Jump, s.line));
        return;
      }

      case StmtKind::Continue: {
        if (scope_->loops_.empty()) { error(s.line, "continue outside a loop"); return; }
        pop_to_loop_depth(s.line);
        emit_loop(scope_->loops_.back().continue_target, s.line);
        return;
      }

      case StmtKind::EntityDecl: {
        const auto& def = *s.entity_def;
        emit_op_wide(Op::GetGlobal, name_constant("__entity_assemble", s.line), s.line);
        emit_constant(Value::string(def.name), s.line);
        for (const auto& f : def.fields) {
          emit_constant(Value::string(f.first), s.line);
          expression(*f.second);
        }
        emit_op_wide(Op::BuildMap, (uint16_t)def.fields.size(), s.line);
        for (const auto& h : def.handlers) {
          emit_constant(Value::string(h->event), h->line);
          compile_entity_handler(def.name, *h);
        }
        emit_op_wide(Op::BuildMap, (uint16_t)def.handlers.size(), s.line);
        emit_op_arg(Op::Call, 3, s.line);

        if (scope_->depth_ == 0 && !scope_->enclosing_) {
          emit_op_wide(Op::DefineGlobal, name_constant(def.name, s.line), s.line);
        } else {
          add_local(def.name, s.line);
        }
        return;
      }
    }
  }

  // Leaving a loop early still has to unwind the blocks between here and the
  // loop's own level, or the stack grows by one per `continue`.
  void pop_to_loop_depth(int line) {
    const int target = scope_->loops_.back().depth;
    for (int i = (int)scope_->locals_.size() - 1; i >= 0; --i) {
      if (scope_->locals_[i].depth <= target) break;
      emit(scope_->locals_[i].captured ? Op::CloseUpvalue : Op::Pop, line);
    }
  }

  std::vector<Diagnostic>* errors_;
  Scope* scope_ = nullptr;
  std::shared_ptr<NativeModule> natives_;   // the `native fn`s of this compilation, so one can call another
};

}  // namespace

CompileResult compile_source(const std::string& source, const std::string& origin,
                             bool keep_last_value) {
  CompileResult result;
  std::vector<Token> tokens = lex_all(source);
  for (const auto& t : tokens) {
    if (t.kind == Tok::Error) result.errors.push_back(Diagnostic{t.line, t.text});
  }
  if (!result.errors.empty()) return result;

  Parser parser(std::move(tokens), &result.errors);
  std::vector<StmtP> program = parser.parse_program();
  if (!result.errors.empty()) return result;

  Compiler compiler(&result.errors);
  auto script = compiler.compile_script(program, keep_last_value);
  if (!result.errors.empty()) return result;
  script->name = origin.empty() ? "<script>" : origin;
  result.script = script;
  return result;
}

}  // namespace aios::userland::grain
