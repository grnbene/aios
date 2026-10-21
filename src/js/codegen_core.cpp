// Code generation: emission, scopes and variables, statements, functions and classes.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "codegen.hpp"
#include "internal.hpp"

namespace aios::js {

// ---------------------------------------------------------------------------------------------------
// opcode tables
// ---------------------------------------------------------------------------------------------------

namespace {

const char* const kOpNames[] = {
#define X(n, f) #n,
    AIOS_JS_OPCODES(X)
#undef X
};
const char* const kOpFormats[] = {
#define X(n, f) f,
    AIOS_JS_OPCODES(X)
#undef X
};

// Net change in operand-stack depth when the instruction falls through. `a` is the first operand.
int op_effect(OpCode op, uint32_t a, uint32_t b) {
  switch (op) {
    case OpCode::Pop: return -1;
    case OpCode::Dup: return 1;
    case OpCode::Dup2: return 2;
    case OpCode::Undefined: case OpCode::Null: case OpCode::True: case OpCode::False: case OpCode::Int: case OpCode::Const: return 1;
    case OpCode::GetLocal: case OpCode::GetLocalChecked: case OpCode::GetEnv: case OpCode::GetEnvChecked: return 1;
    case OpCode::SetLocal: case OpCode::SetLocalChecked: case OpCode::SetEnv: case OpCode::SetEnvChecked: return -1;
    case OpCode::GetGlobal: case OpCode::TypeofGlobal: return 1;
    case OpCode::SetGlobal: case OpCode::DeclareGlobalFunc: case OpCode::InitGlobalLex: return -1;
    case OpCode::SetProp: case OpCode::GetElem: case OpCode::DeleteElem: case OpCode::In: case OpCode::InstanceOf: return -1;
    case OpCode::SetElem: case OpCode::SetSuperProp: return -2;
    case OpCode::NewObject: case OpCode::NewArray: return 1;
    case OpCode::ArrayPush: case OpCode::ArraySpread: case OpCode::ObjectSpread: case OpCode::SetProto: case OpCode::DefineField: return -1;
    case OpCode::DefineFieldComputed: return -2;
    case OpCode::DefineMethod: case OpCode::DefineGetter: case OpCode::DefineSetter: return -1;
    case OpCode::DefineMethodComputed: case OpCode::DefineGetterComputed: case OpCode::DefineSetterComputed: return -2;
    case OpCode::Closure: case OpCode::NewPrivateName: return 1;
    case OpCode::CreateClass: return -static_cast<int>(b);
    case OpCode::ClassFields: return -1;
    case OpCode::PrivateGet: case OpCode::PrivateIn: case OpCode::GetSuperProp: return -1;
    case OpCode::PrivateSet: return -2;
    case OpCode::PrivateDefine: return -3;
    case OpCode::Call: return -(static_cast<int>(a) + 1);
    case OpCode::CallSpread: return -2;
    case OpCode::New: return -static_cast<int>(a);
    case OpCode::NewSpread: return -1;
    case OpCode::SuperCall: return 1 - static_cast<int>(a);
    case OpCode::Return: case OpCode::Throw: return -1;
    case OpCode::JumpIfFalse: case OpCode::JumpIfTrue: case OpCode::JumpIfFalseKeep: case OpCode::JumpIfTrueKeep:
    case OpCode::JumpIfNotNullishKeep: case OpCode::JumpIfNotUndefined: return -1;
    case OpCode::Add: case OpCode::Sub: case OpCode::Mul: case OpCode::Div: case OpCode::Mod: case OpCode::Exp: case OpCode::Shl: case OpCode::Shr:
    case OpCode::UShr: case OpCode::BitAnd: case OpCode::BitOr: case OpCode::BitXor: case OpCode::Eq: case OpCode::Ne: case OpCode::StrictEq:
    case OpCode::StrictNe: case OpCode::Lt: case OpCode::Gt: case OpCode::Le: case OpCode::Ge:
      return -1;
    case OpCode::LoadThis: case OpCode::LoadThisChecked: case OpCode::LoadNewTarget: case OpCode::LoadFunction: case OpCode::LoadHome:
    case OpCode::GlobalObject: case OpCode::CreateArguments: case OpCode::CreateRest: case OpCode::GetTemplateObject: case OpCode::NewRegExp: return 1;
    case OpCode::InitThisLocal: case OpCode::InitThisEnv: return -1;
    case OpCode::ReturnCtor: return -2;
    case OpCode::CopyDataProps: return -1;
    case OpCode::IterInit: case OpCode::AsyncIterInit: case OpCode::ForInInit: return -1;
    case OpCode::IterNext: case OpCode::IterNextValue: case OpCode::IterRest: case OpCode::IterCallNext: case OpCode::ForInNext: return 1;
    case OpCode::YieldStar: return 1;
    default: return 0;
  }
}

}  // namespace

const char* opcode_name(OpCode op) { return kOpNames[static_cast<int>(op)]; }
const char* opcode_format(OpCode op) { return kOpFormats[static_cast<int>(op)]; }

std::string vm_disassemble(FunctionCode* fc, int indent) {
  std::string out;
  const std::string pad(static_cast<size_t>(indent), ' ');
  out += pad + "== " + (fc->name ? fc->name->utf8() : std::string()) + " nlocals=" + std::to_string(fc->nlocals) + " nparams=" + std::to_string(fc->nparams) +
         " max_stack=" + std::to_string(fc->max_stack) + (fc->strict ? " strict" : "") + "\n";
  const std::vector<uint8_t>& c = fc->code;
  size_t pc = 0;
  while (pc < c.size()) {
    const OpCode op = static_cast<OpCode>(c[pc]);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s%5zu  %s", pad.c_str(), pc, opcode_name(op));
    out += buf;
    size_t p = pc + 1;
    for (const char* f = opcode_format(op); *f; ++f) {
      if (*f == 'B') { out += " " + std::to_string(c[p]); p += 1; }
      else if (*f == 'H') { uint16_t v; std::memcpy(&v, &c[p], 2); out += " " + std::to_string(v); p += 2; }
      else if (*f == 'I') { uint32_t v; std::memcpy(&v, &c[p], 4); out += " " + std::to_string(v); p += 4; }
      else if (*f == 'i') { int32_t v; std::memcpy(&v, &c[p], 4); p += 4; out += " ->" + std::to_string(static_cast<long>(p) + v); }
    }
    out += "\n";
    pc = p;
  }
  for (const Handler& h : fc->handlers)
    out += pad + "  handler [" + std::to_string(h.start) + "," + std::to_string(h.end) + ") -> " + std::to_string(h.target) + " env=" + std::to_string(h.env_depth) + (h.is_finally ? " finally" : "") + "\n";
  for (FunctionCode* f : fc->functions) out += vm_disassemble(f, indent + 2);
  return out;
}

void FunctionCode::trace(Tracer& t) {
  for (const Value& v : consts) t.mark(v);
  for (FunctionCode* f : functions) t.mark(reinterpret_cast<Cell*>(f));
  t.mark(name);
  t.mark(filename);
  for (auto& s : templates) t.mark(s.object);
}

// ---------------------------------------------------------------------------------------------------
// emission
// ---------------------------------------------------------------------------------------------------

void Compiler::adj(int d) {
  fs->depth += d;
  if (fs->depth > fs->max_depth) fs->max_depth = fs->depth;
  if (fs->depth < 0) fs->depth = 0;
}

void Compiler::mark_line(uint32_t line) {
  if (line == 0 || line == fs->last_line) return;
  fs->last_line = line;
  fs->code->lines.push_back({pc(), line});
}

static void put_u16(std::vector<uint8_t>& c, uint32_t v) { c.push_back(static_cast<uint8_t>(v)); c.push_back(static_cast<uint8_t>(v >> 8)); }
static void put_u32(std::vector<uint8_t>& c, uint32_t v) { for (int i = 0; i < 4; ++i) c.push_back(static_cast<uint8_t>(v >> (8 * i))); }

void Compiler::emit(OpCode op) {
  fs->code->code.push_back(static_cast<uint8_t>(op));
  adj(op_effect(op, 0, 0));
  if (op == OpCode::Return || op == OpCode::Throw || op == OpCode::ReturnCtor || op == OpCode::ThrowError || op == OpCode::ThrowConstAssign) fs->reachable = false;
}
void Compiler::emit_u8(OpCode op, uint8_t a) { fs->code->code.push_back(static_cast<uint8_t>(op)); fs->code->code.push_back(a); adj(op_effect(op, a, 0)); }
void Compiler::emit_u16(OpCode op, uint16_t a) { fs->code->code.push_back(static_cast<uint8_t>(op)); put_u16(fs->code->code, a); adj(op_effect(op, a, 0)); }
void Compiler::emit_u32(OpCode op, uint32_t a) { fs->code->code.push_back(static_cast<uint8_t>(op)); put_u32(fs->code->code, a); adj(op_effect(op, a, 0)); }
void Compiler::emit_i32(OpCode op, int32_t a) { fs->code->code.push_back(static_cast<uint8_t>(op)); put_u32(fs->code->code, static_cast<uint32_t>(a)); adj(op_effect(op, static_cast<uint32_t>(a), 0)); }
void Compiler::emit_u16_u16(OpCode op, uint16_t a, uint16_t b) { fs->code->code.push_back(static_cast<uint8_t>(op)); put_u16(fs->code->code, a); put_u16(fs->code->code, b); adj(op_effect(op, a, b)); }
void Compiler::emit_u32_u8(OpCode op, uint32_t a, uint8_t b) { fs->code->code.push_back(static_cast<uint8_t>(op)); put_u32(fs->code->code, a); fs->code->code.push_back(b); adj(op_effect(op, a, b)); }
void Compiler::emit_u8_u32(OpCode op, uint8_t a, uint32_t b) { fs->code->code.push_back(static_cast<uint8_t>(op)); fs->code->code.push_back(a); put_u32(fs->code->code, b); adj(op_effect(op, a, b)); }
void Compiler::emit_u16_u32(OpCode op, uint16_t a, uint32_t b) { fs->code->code.push_back(static_cast<uint8_t>(op)); put_u16(fs->code->code, a); put_u32(fs->code->code, b); adj(op_effect(op, a, b)); }
void Compiler::emit_u16_u16_u32(OpCode op, uint16_t a, uint16_t b, uint32_t c) {
  fs->code->code.push_back(static_cast<uint8_t>(op)); put_u16(fs->code->code, a); put_u16(fs->code->code, b); put_u32(fs->code->code, c); adj(op_effect(op, a, b));
}
void Compiler::emit_u32_u8_u16_u16(OpCode op, uint32_t a, uint8_t b, uint16_t c, uint16_t d) {
  fs->code->code.push_back(static_cast<uint8_t>(op)); put_u32(fs->code->code, a); fs->code->code.push_back(b); put_u16(fs->code->code, c); put_u16(fs->code->code, d);
  adj(op_effect(op, a, b));
}

void Compiler::patch_jump(uint32_t at, int32_t target) {
  const int32_t rel = target - static_cast<int32_t>(at + 4);
  for (int i = 0; i < 4; ++i) fs->code->code[at + static_cast<uint32_t>(i)] = static_cast<uint8_t>(static_cast<uint32_t>(rel) >> (8 * i));
}

void Compiler::jump(OpCode op, Label& l) {
  fs->code->code.push_back(static_cast<uint8_t>(op));
  uint32_t at = pc();
  put_u32(fs->code->code, 0);
  if (l.pos >= 0) patch_jump(at, l.pos);   // a backward jump: the label is already bound
  else l.fixups.push_back(at);
  // depth at the target, and on the fall-through path
  int taken = 0, fall = 0;
  switch (op) {
    case OpCode::Jump: taken = 0; fall = 0; break;
    case OpCode::JumpIfFalse: case OpCode::JumpIfTrue: taken = -1; fall = -1; break;
    case OpCode::JumpIfFalseKeep: case OpCode::JumpIfTrueKeep: case OpCode::JumpIfNotNullishKeep: case OpCode::JumpIfNotUndefined: taken = 0; fall = -1; break;
    case OpCode::JumpIfNullish: taken = -1; fall = -1; break;
    default: break;
  }
  int target_depth = fs->depth + taken;
  if (l.depth < 0) l.depth = target_depth;
  adj(fall);
  if (op == OpCode::Jump) fs->reachable = false;
}

void Compiler::emit_u16_i32(OpCode op, uint16_t a, Label& l) {
  fs->code->code.push_back(static_cast<uint8_t>(op));
  put_u16(fs->code->code, a);
  uint32_t at = pc();
  put_u32(fs->code->code, 0);
  if (l.pos >= 0) patch_jump(at, l.pos);
  else l.fixups.push_back(at);
  int taken = 0, fall = 0;
  switch (op) {
    case OpCode::IterNext: case OpCode::ForInNext: taken = 0; fall = 1; break;
    case OpCode::IterResult: taken = -1; fall = 0; break;
    default: break;
  }
  int target_depth = fs->depth + taken;
  if (l.depth < 0) l.depth = target_depth;
  adj(fall);
}

void Compiler::bind(Label& l) {
  l.pos = static_cast<int32_t>(pc());
  for (uint32_t at : l.fixups) patch_jump(at, l.pos);
  l.fixups.clear();
  if (!fs->reachable) {
    if (l.depth >= 0) fs->depth = l.depth;
    fs->reachable = true;
  } else if (l.depth < 0) {
    l.depth = fs->depth;
  }
}

uint32_t Compiler::k_atom(const std::u16string& s) {
  auto it = fs->atom_k.find(s);
  if (it != fs->atom_k.end()) return it->second;
  fs->code->consts.push_back(Value::String(ctx.heap.intern(s)));
  uint32_t i = static_cast<uint32_t>(fs->code->consts.size() - 1);
  fs->atom_k[s] = i;
  return i;
}

uint32_t Compiler::k_atom_ascii(const char* s) {
  std::u16string u;
  for (; *s; ++s) u += static_cast<char16_t>(static_cast<unsigned char>(*s));
  return k_atom(u);
}

uint32_t Compiler::k_num(double d) {
  uint64_t bits;
  std::memcpy(&bits, &d, 8);
  auto it = fs->num_k.find(bits);
  if (it != fs->num_k.end()) return it->second;
  fs->code->consts.push_back(Value::Number(d));
  uint32_t i = static_cast<uint32_t>(fs->code->consts.size() - 1);
  fs->num_k[bits] = i;
  return i;
}

uint32_t Compiler::alloc_reg() { return fs->code->nlocals++; }

void Compiler::rec_open(Rec& r) {
  if (r.open) return;
  r.start = pc();
  r.open = true;
}

void Compiler::rec_close(Rec& r) {
  if (!r.open) return;
  if (pc() > r.start) r.segs.emplace_back(r.start, pc());
  r.open = false;
}

void Compiler::add_handlers(Rec& r, uint32_t target, int env_depth, bool is_finally) {
  for (auto& s : r.segs) {
    Handler h;
    h.start = s.first;
    h.end = s.second;
    h.target = target;
    h.env_depth = static_cast<uint16_t>(env_depth);
    h.is_finally = is_finally;
    fs->code->handlers.push_back(h);
  }
}

// ---------------------------------------------------------------------------------------------------
// scopes and variables
// ---------------------------------------------------------------------------------------------------

void Compiler::assign_registers(Scope* s) {
  for (VarInfo* v : s->vars)
    if (!v->in_env && v->slot < 0) v->slot = static_cast<int>(alloc_reg());
}

void Compiler::enter_scope(Scope* s) {
  fs->cur_scope = s;
  assign_registers(s);
  if (s->materialized) {
    emit_u16(OpCode::PushEnv, static_cast<uint16_t>(s->env_size));
    ++fs->env_depth;
  }
  for (VarInfo* v : s->vars) {
    if (!v->tdz) continue;
    if (v->in_env) emit_u16_u16(OpCode::SetEnvEmpty, 0, static_cast<uint16_t>(v->slot));
    else emit_u16(OpCode::SetLocalEmpty, static_cast<uint16_t>(v->slot));
  }
}

void Compiler::leave_scope(Scope* s) {
  if (s->materialized) {
    if (fs->reachable) emit(OpCode::PopEnv);
    --fs->env_depth;
  }
  fs->cur_scope = s->parent;
}

void Compiler::pop_envs_to(int depth) {
  for (int d = fs->env_depth; d > depth; --d) emit(OpCode::PopEnv);
}

int Compiler::env_hops(Scope* from, Scope* to) {
  int hops = 0;
  for (Scope* s = from; s && s != to; s = s->parent)
    if (s->materialized) ++hops;
  return hops;
}

VarInfo* Compiler::lookup_var(const std::u16string& name, Scope* from) {
  for (Scope* s = from; s; s = s->parent) {
    auto it = s->map.find(name);
    if (it != s->map.end()) return it->second;
  }
  return nullptr;
}

void Compiler::load_var_info(VarInfo* v, const std::u16string& name) {
  if (!v) {
    emit_u32(OpCode::GetGlobal, k_atom(name));
    return;
  }
  const bool checked = v->tdz;
  if (v->in_env) {
    uint16_t hops = static_cast<uint16_t>(env_hops(fs->cur_scope, v->scope));
    if (checked) emit_u16_u16_u32(OpCode::GetEnvChecked, hops, static_cast<uint16_t>(v->slot), k_atom(name));
    else emit_u16_u16(OpCode::GetEnv, hops, static_cast<uint16_t>(v->slot));
  } else {
    if (checked) emit_u16_u32(OpCode::GetLocalChecked, static_cast<uint16_t>(v->slot), k_atom(name));
    else emit_u16(OpCode::GetLocal, static_cast<uint16_t>(v->slot));
  }
}

void Compiler::load_var_unchecked(VarInfo* v) {
  if (v->in_env) emit_u16_u16(OpCode::GetEnv, static_cast<uint16_t>(env_hops(fs->cur_scope, v->scope)), static_cast<uint16_t>(v->slot));
  else emit_u16(OpCode::GetLocal, static_cast<uint16_t>(v->slot));
}

void Compiler::load_var(Node* ident) {
  VarInfo* v = ident->var;
  if (!v && ident->str == u"undefined") {
    emit(OpCode::Undefined);
    return;
  }
  load_var_info(v, ident->str);
}

void Compiler::store_var_info(VarInfo* v, const std::u16string& name, StoreMode mode, Node* where) {
  (void)where;
  if (!v) {
    if (mode == StoreMode::InitLexical) emit_u32(OpCode::InitGlobalLex, k_atom(name));
    else emit_u32_u8(OpCode::SetGlobal, k_atom(name), mode == StoreMode::Assign && fs->code->strict ? 1 : 0);
    return;
  }
  if (mode == StoreMode::Assign) {
    if (v->is_const) {
      emit(OpCode::Pop);
      if (!v->silent_const) emit_u32(OpCode::ThrowConstAssign, k_atom(name));
      return;
    }
    if (v->tdz) {
      if (v->in_env) emit_u16_u16_u32(OpCode::SetEnvChecked, static_cast<uint16_t>(env_hops(fs->cur_scope, v->scope)), static_cast<uint16_t>(v->slot), k_atom(name));
      else emit_u16_u32(OpCode::SetLocalChecked, static_cast<uint16_t>(v->slot), k_atom(name));
      return;
    }
  }
  if (v->in_env) emit_u16_u16(OpCode::SetEnv, static_cast<uint16_t>(env_hops(fs->cur_scope, v->scope)), static_cast<uint16_t>(v->slot));
  else emit_u16(OpCode::SetLocal, static_cast<uint16_t>(v->slot));
}

void Compiler::store_var(Node* ident, StoreMode mode) { store_var_info(ident->var, ident->str, mode, ident); }

// ---------------------------------------------------------------------------------------------------
// statements
// ---------------------------------------------------------------------------------------------------

void Compiler::hoist_functions(Scope* s) {
  for (Node* f : s->hoisted_funcs) {
    compile_closure(f);
    auto it = s->map.find(f->fn->name);
    VarInfo* v = it != s->map.end() ? it->second : nullptr;
    store_var_info(v, f->fn->name, StoreMode::InitVar, f);
  }
}

void Compiler::compile_statements(std::vector<Node*>& list) {
  for (Node* n : list) {
    if (failed()) return;
    compile_statement(n);
  }
}

void Compiler::compile_block(Node* n) {
  Scope* s = n->scope;
  if (!s || s->vars.empty()) {
    // no bindings: no scope work at all
    if (s) {
      Scope* saved = fs->cur_scope;
      fs->cur_scope = s;
      compile_statements(n->list);
      fs->cur_scope = saved;
    } else {
      compile_statements(n->list);
    }
    return;
  }
  enter_scope(s);
  hoist_functions(s);
  compile_statements(n->list);
  leave_scope(s);
}

void Compiler::compile_var_decl(Node* n) {
  const bool is_var = n->has(kFlagVar);
  for (Node* d : n->list) {
    Node* target = d->a;
    if (!d->b) {
      if (is_var) continue;   // hoisted: nothing to do
      emit(OpCode::Undefined);
      compile_pattern_store(target, StoreMode::InitLexical);
      continue;
    }
    mark_line(d->line);
    compile_expr(d->b);
    compile_pattern_store(target, is_var ? StoreMode::InitVar : StoreMode::InitLexical);
  }
}

void Compiler::compile_function_decl_stmt(Node* n) {
  // Hoisting already created and stored the function. Annex B: a block-level declaration also assigns the
  // function-scope var when the declaration is evaluated.
  if (!n->has(kFlagDeclHoisted)) return;
  const std::u16string& name = n->fn->name;
  VarInfo* block_var = lookup_var(name, fs->cur_scope);
  load_var_info(block_var, name);
  // the function-scope binding: look up from the function scope outward
  Scope* from = fs->cur_scope;
  while (from && from->kind != Scope::Kind::Function && from->kind != Scope::Kind::Script) from = from->parent;
  VarInfo* outer = from && from->kind == Scope::Kind::Function ? lookup_var(name, from) : nullptr;
  if (outer == block_var) { emit(OpCode::Pop); return; }
  store_var_info(outer, name, StoreMode::InitVar, n);
}

void Compiler::compile_if(Node* n) {
  Label Lelse, Lend;
  compile_expr(n->a);
  jump(OpCode::JumpIfFalse, Lelse);
  compile_statement(n->b);
  if (n->c) {
    jump(OpCode::Jump, Lend);
    bind(Lelse);
    compile_statement(n->c);
    bind(Lend);
  } else {
    bind(Lelse);
  }
}

void Compiler::compile_while(Node* n, std::vector<std::u16string>* labels) {
  Label Lcont, Lend;
  Control c;
  c.kind = Control::Kind::Loop;
  if (labels) c.labels = *labels;
  c.brk = &Lend;
  c.cont = &Lcont;
  c.brk_env = c.cont_env = fs->env_depth;
  bind(Lcont);
  compile_expr(n->a);
  jump(OpCode::JumpIfFalse, Lend);
  fs->control.push_back(c);
  compile_statement(n->b);
  fs->control.pop_back();
  jump(OpCode::Jump, Lcont);
  bind(Lend);
}

void Compiler::compile_do_while(Node* n, std::vector<std::u16string>* labels) {
  Label Ltop, Lcont, Lend;
  Control c;
  c.kind = Control::Kind::Loop;
  if (labels) c.labels = *labels;
  c.brk = &Lend;
  c.cont = &Lcont;
  c.brk_env = c.cont_env = fs->env_depth;
  bind(Ltop);
  fs->control.push_back(c);
  compile_statement(n->a);
  fs->control.pop_back();
  bind(Lcont);
  compile_expr(n->b);
  jump(OpCode::JumpIfTrue, Ltop);
  bind(Lend);
}

void Compiler::compile_for(Node* n, std::vector<std::u16string>* labels) {
  Scope* ls = n->scope;
  const bool scoped = ls && !ls->vars.empty();
  if (scoped) enter_scope(ls);
  else if (ls) fs->cur_scope = ls;
  if (n->a) {
    if (n->a->kind == NK::VarDecl) compile_var_decl(n->a);
    else compile_expr_discard(n->a);
  }
  Label Ltest, Lcont, Lend;
  const bool per_iter = scoped && ls->materialized;
  if (per_iter) emit(OpCode::CopyEnv);
  Control c;
  c.kind = Control::Kind::Loop;
  if (labels) c.labels = *labels;
  c.brk = &Lend;
  c.cont = &Lcont;
  c.brk_env = c.cont_env = fs->env_depth;
  bind(Ltest);
  if (n->b) {
    compile_expr(n->b);
    jump(OpCode::JumpIfFalse, Lend);
  }
  fs->control.push_back(c);
  compile_statement(n->d);
  fs->control.pop_back();
  bind(Lcont);
  if (per_iter) emit(OpCode::CopyEnv);
  if (n->c) compile_expr_discard(n->c);
  jump(OpCode::Jump, Ltest);
  bind(Lend);
  if (scoped) leave_scope(ls);
  else if (ls) fs->cur_scope = ls->parent;
}

void Compiler::compile_for_in_of(Node* n, std::vector<std::u16string>* labels) {
  const bool is_of = n->kind == NK::ForOf;
  const bool is_await = n->has(kFlagAwait);
  Scope* ls = n->scope;
  const bool scoped = ls && !ls->vars.empty();
  Node* left = n->a;
  // the right-hand side sees the loop's bindings in their temporal dead zone
  if (scoped) enter_scope(ls);
  else if (ls) fs->cur_scope = ls;
  compile_expr(n->b);
  if (scoped) leave_scope(ls);
  else if (ls) fs->cur_scope = ls->parent;
  uint32_t tmp = alloc_reg();
  alloc_reg();
  alloc_reg();
  alloc_reg();
  if (!is_of) emit_u16(OpCode::ForInInit, static_cast<uint16_t>(tmp));
  else emit_u16(is_await ? OpCode::AsyncIterInit : OpCode::IterInit, static_cast<uint16_t>(tmp));
  Label Lnext, Lcont, Lend, Lhandler, Lafter;
  Control c;
  c.kind = Control::Kind::Loop;
  if (labels) c.labels = *labels;
  c.brk = &Lend;
  c.cont = &Lcont;
  c.brk_env = fs->env_depth;
  Rec rec;
  if (is_of) {
    c.iter_tmp = static_cast<int>(tmp);
    c.iter_rec = &rec;
  }
  bind(Lnext);
  if (!is_of) emit_u16_i32(OpCode::ForInNext, static_cast<uint16_t>(tmp), Lend);
  else if (is_await) {
    emit_u16(OpCode::IterCallNext, static_cast<uint16_t>(tmp));
    emit(OpCode::Await);
    emit_u16_i32(OpCode::IterResult, static_cast<uint16_t>(tmp), Lend);
    emit_u16(OpCode::AwaitIfSync, static_cast<uint16_t>(tmp));
  } else {
    emit_u16_i32(OpCode::IterNext, static_cast<uint16_t>(tmp), Lend);
  }
  // the value is on the stack; the iteration's own bindings
  const int outside_env = fs->env_depth;
  if (scoped) enter_scope(ls);
  else if (ls) fs->cur_scope = ls;
  c.cont_env = fs->env_depth;
  if (is_of) rec_open(rec);
  fs->control.push_back(c);
  fs->control.back().iter_rec = is_of ? &rec : nullptr;
  if (left->kind == NK::VarDecl) {
    Node* d = left->list[0];
    StoreMode mode = left->has(kFlagVar) ? StoreMode::InitVar : StoreMode::InitLexical;
    compile_pattern_store(d->a, mode);
  } else {
    compile_pattern_store(left, StoreMode::Assign);
  }
  compile_statement(n->c);
  fs->control.pop_back();
  bind(Lcont);
  if (scoped) leave_scope(ls);
  else if (ls) fs->cur_scope = ls->parent;
  if (is_of) rec_close(rec);
  jump(OpCode::Jump, Lnext);
  bind(Lend);
  if (is_of) {
    jump(OpCode::Jump, Lafter);
    // an exception out of the body closes the iterator, then continues unwinding
    bind(Lhandler);
    uint32_t etmp = alloc_reg();
    fs->depth = 1;
    emit_u16(OpCode::SetLocal, static_cast<uint16_t>(etmp));
    emit_u16(OpCode::IterCloseSilent, static_cast<uint16_t>(tmp));
    emit_u16(OpCode::GetLocal, static_cast<uint16_t>(etmp));
    emit(OpCode::Throw);
    add_handlers(rec, static_cast<uint32_t>(Lhandler.pos), outside_env, true);
    bind(Lafter);
  }
}

void Compiler::compile_switch(Node* n, std::vector<std::u16string>* labels) {
  compile_expr(n->a);
  uint32_t tmp = alloc_reg();
  emit_u16(OpCode::SetLocal, static_cast<uint16_t>(tmp));
  Scope* ss = n->scope;
  const bool scoped = ss && !ss->vars.empty();
  if (scoped) {
    enter_scope(ss);
    hoist_functions(ss);
  } else if (ss) {
    fs->cur_scope = ss;
  }
  Label Lend;
  std::vector<Label> case_labels(n->list.size());
  int default_index = -1;
  for (size_t i = 0; i < n->list.size(); ++i) {
    Node* c = n->list[i];
    if (c->has(kFlagDefaultCase)) { default_index = static_cast<int>(i); continue; }
    emit_u16(OpCode::GetLocal, static_cast<uint16_t>(tmp));
    compile_expr(c->a);
    emit(OpCode::StrictEq);
    jump(OpCode::JumpIfTrue, case_labels[i]);
  }
  if (default_index >= 0) jump(OpCode::Jump, case_labels[static_cast<size_t>(default_index)]);
  else jump(OpCode::Jump, Lend);
  Control c;
  c.kind = Control::Kind::Switch;
  if (labels) c.labels = *labels;
  c.brk = &Lend;
  c.brk_env = fs->env_depth;
  fs->control.push_back(c);
  for (size_t i = 0; i < n->list.size(); ++i) {
    bind(case_labels[i]);
    compile_statements(n->list[i]->list);
  }
  fs->control.pop_back();
  bind(Lend);
  if (scoped) leave_scope(ss);
  else if (ss) fs->cur_scope = ss->parent;
}

void Compiler::compile_labeled(Node* n) {
  std::vector<std::u16string> labels;
  Node* s = n;
  while (s->kind == NK::Labeled) { labels.push_back(s->str); s = s->a; }
  switch (s->kind) {
    case NK::For: compile_for(s, &labels); return;
    case NK::ForIn: case NK::ForOf: compile_for_in_of(s, &labels); return;
    case NK::While: compile_while(s, &labels); return;
    case NK::DoWhile: compile_do_while(s, &labels); return;
    case NK::Switch: compile_switch(s, &labels); return;
    default: break;
  }
  Label Lend;
  Control c;
  c.kind = Control::Kind::Labeled;
  c.labels = labels;
  c.brk = &Lend;
  c.brk_env = fs->env_depth;
  fs->control.push_back(c);
  compile_statement(s);
  fs->control.pop_back();
  bind(Lend);
}

// Close every open protected range at or above `index` (inlined cleanup runs outside them), returning what to reopen.
static void collect_open(FuncState* fs, int index, std::vector<Rec*>& out) {
  for (size_t i = static_cast<size_t>(index < 0 ? 0 : index); i < fs->control.size(); ++i) {
    Control& c = fs->control[i];
    if (c.try_ctx) {
      if (c.try_ctx->a.open) out.push_back(&c.try_ctx->a);
      if (c.try_ctx->c.open) out.push_back(&c.try_ctx->c);
    }
    if (c.iter_rec && c.iter_rec->open) out.push_back(c.iter_rec);
  }
}

void Compiler::inline_finally(Control& c, int index) {
  TryCtx* t = c.try_ctx;
  pop_envs_to(t->env_depth);
  std::vector<Control> saved(fs->control.begin() + index, fs->control.end());
  fs->control.resize(static_cast<size_t>(index));
  Scope* saved_scope = fs->cur_scope;
  int saved_env = fs->env_depth;
  // inside the finally block we are at the try statement's scope level
  fs->env_depth = t->env_depth;
  compile_block(t->finally_node);
  fs->env_depth = saved_env;
  fs->cur_scope = saved_scope;
  fs->control.insert(fs->control.end(), saved.begin(), saved.end());
}

// Emit the cleanup for leaving every control construct above `target_index` (and the target's own iterator, for a
// break out of it), then (optionally) a jump to the target's label.
void Compiler::emit_exit_to(int target_index, bool is_continue, bool include_target_cleanup) {
  std::vector<Rec*> suspended;
  collect_open(fs, target_index < 0 ? 0 : (include_target_cleanup ? target_index : target_index + 1), suspended);
  for (Rec* r : suspended) rec_close(*r);
  const int top = static_cast<int>(fs->control.size()) - 1;
  for (int i = top; i > target_index; --i) {
    Control& c = fs->control[static_cast<size_t>(i)];
    if (c.kind == Control::Kind::Try && c.try_ctx && c.try_ctx->has_finally) {
      inline_finally(c, i);
    } else if (c.kind == Control::Kind::Loop && c.iter_tmp >= 0) {
      pop_envs_to(c.brk_env);
      emit_u16(OpCode::IterClose, static_cast<uint16_t>(c.iter_tmp));
    }
  }
  if (target_index >= 0 && include_target_cleanup) {
    Control& t = fs->control[static_cast<size_t>(target_index)];
    if (t.kind == Control::Kind::Loop && t.iter_tmp >= 0) {
      pop_envs_to(t.brk_env);
      emit_u16(OpCode::IterClose, static_cast<uint16_t>(t.iter_tmp));
    }
  }
  if (target_index >= 0) {
    Control& t = fs->control[static_cast<size_t>(target_index)];
    pop_envs_to(is_continue ? t.cont_env : t.brk_env);
    jump(OpCode::Jump, is_continue ? *t.cont : *t.brk);
  }
  for (Rec* r : suspended) rec_open(*r);
}

void Compiler::compile_break_continue(Node* n) {
  const bool is_break = n->kind == NK::Break;
  int target = -1;
  for (int i = static_cast<int>(fs->control.size()) - 1; i >= 0; --i) {
    Control& c = fs->control[static_cast<size_t>(i)];
    if (n->str.empty()) {
      if (c.kind == Control::Kind::Loop || (is_break && c.kind == Control::Kind::Switch)) { target = i; break; }
    } else {
      bool has = false;
      for (auto& l : c.labels) if (l == n->str) has = true;
      if (has && (is_break || c.kind == Control::Kind::Loop)) { target = i; break; }
    }
  }
  if (target < 0) { fail("Illegal break or continue statement", n->line); return; }
  emit_exit_to(target, !is_break, is_break);
}

void Compiler::compile_return(Node* n) {
  const bool derived = fs->is_derived_ctor;
  if (n->a) compile_expr(n->a);
  else emit(OpCode::Undefined);
  bool needs_cleanup = false;
  for (Control& c : fs->control)
    if ((c.kind == Control::Kind::Try && c.try_ctx && c.try_ctx->has_finally) || (c.kind == Control::Kind::Loop && c.iter_tmp >= 0)) needs_cleanup = true;
  if (needs_cleanup) {
    uint32_t rv = alloc_reg();
    emit_u16(OpCode::SetLocal, static_cast<uint16_t>(rv));
    emit_exit_to(-1, false, false);
    emit_u16(OpCode::GetLocal, static_cast<uint16_t>(rv));
  }
  if (derived && fs->this_var) {
    load_var_unchecked(fs->this_var);
    emit(OpCode::ReturnCtor);
  } else {
    emit(OpCode::Return);
  }
}

void Compiler::compile_try(Node* n) {
  TryCtx t;
  t.has_finally = n->d != nullptr;
  t.finally_node = n->d;
  t.env_depth = fs->env_depth;
  const bool has_catch = n->has(kFlagHasCatch);
  Label Lcatch, Lfinally, Lend;
  // the try block
  Control c;
  c.kind = Control::Kind::Try;
  c.try_ctx = &t;
  c.phase = 0;
  fs->control.push_back(c);
  rec_open(t.a);
  compile_block(n->a);
  rec_close(t.a);
  fs->control.pop_back();
  if (t.has_finally) compile_block(n->d);
  jump(OpCode::Jump, Lend);

  if (has_catch) {
    bind(Lcatch);
    fs->depth = 1;   // the exception
    Control cc;
    cc.kind = Control::Kind::Try;
    cc.try_ctx = &t;
    cc.phase = 1;
    Scope* cs = n->scope;
    const bool scoped = cs && !cs->vars.empty();
    if (scoped) enter_scope(cs);
    else if (cs) fs->cur_scope = cs;
    if (n->b) compile_pattern_store(n->b, StoreMode::InitLexical);
    else emit(OpCode::Pop);
    if (t.has_finally) { fs->control.push_back(cc); rec_open(t.c); }
    compile_block(n->c);
    if (t.has_finally) { rec_close(t.c); fs->control.pop_back(); }
    if (scoped) leave_scope(cs);
    else if (cs) fs->cur_scope = cs->parent;
    if (t.has_finally) compile_block(n->d);
    jump(OpCode::Jump, Lend);
  }
  if (t.has_finally) {
    bind(Lfinally);
    fs->depth = 1;
    uint32_t etmp = alloc_reg();
    emit_u16(OpCode::SetLocal, static_cast<uint16_t>(etmp));
    compile_block(n->d);
    emit_u16(OpCode::GetLocal, static_cast<uint16_t>(etmp));
    emit(OpCode::Throw);
  }
  // handlers, inner first: the catch handler, then the finally handler over the try and catch blocks
  if (has_catch) add_handlers(t.a, static_cast<uint32_t>(Lcatch.pos), t.env_depth, false);
  if (t.has_finally) {
    if (!has_catch) add_handlers(t.a, static_cast<uint32_t>(Lfinally.pos), t.env_depth, true);
    else {
      // an exception in the try block goes to the catch block; the finally handler covers the try block too, but
      // only after the catch handler has declined it, which a catch-all never does: it matters for the sentinel a
      // generator's return() throws, which only finally handlers see
      add_handlers(t.a, static_cast<uint32_t>(Lfinally.pos), t.env_depth, true);
      add_handlers(t.c, static_cast<uint32_t>(Lfinally.pos), t.env_depth, true);
    }
  }
  bind(Lend);
}

void Compiler::compile_statement(Node* n) {
  if (!n || failed()) return;
  mark_line(n->line);
  switch (n->kind) {
    case NK::ExprStmt:
      compile_expr(n->a);
      if (fs->completion_reg >= 0) emit_u16(OpCode::SetLocal, static_cast<uint16_t>(fs->completion_reg));
      else emit(OpCode::Pop);
      return;
    case NK::VarDecl: compile_var_decl(n); return;
    case NK::FunctionDecl: compile_function_decl_stmt(n); return;
    case NK::ClassDecl:
      compile_class(n);
      store_var(n->b, StoreMode::InitLexical);
      return;
    case NK::Return: compile_return(n); return;
    case NK::If: compile_if(n); return;
    case NK::For: compile_for(n, nullptr); return;
    case NK::ForIn: case NK::ForOf: compile_for_in_of(n, nullptr); return;
    case NK::While: compile_while(n, nullptr); return;
    case NK::DoWhile: compile_do_while(n, nullptr); return;
    case NK::Break: case NK::Continue: compile_break_continue(n); return;
    case NK::Throw:
      compile_expr(n->a);
      emit(OpCode::Throw);
      return;
    case NK::Try: compile_try(n); return;
    case NK::Switch: compile_switch(n, nullptr); return;
    case NK::Labeled: compile_labeled(n); return;
    case NK::Block: compile_block(n); return;
    case NK::Empty: return;
    case NK::Debugger: emit(OpCode::Debugger); return;
    default:
      fail("unsupported statement", n->line);
      return;
  }
}

// ---------------------------------------------------------------------------------------------------
// functions
// ---------------------------------------------------------------------------------------------------

void Compiler::function_prologue(Node* fnode, FuncInfo* fi) {
  Scope* fsc = fnode->scope;
  fs->fscope = fsc;
  // parameter registers
  uint32_t nparams_slots = 0;
  for (Node* p : fi->params) if (p->kind != NK::Rest) ++nparams_slots;
  fs->code->nlocals = nparams_slots;
  fs->code->nparams = nparams_slots;
  {
    uint32_t i = 0;
    for (Node* p : fi->params) {
      if (p->kind == NK::Rest) break;
      if (p->kind == NK::Ident && p->var && !p->var->in_env && p->var->slot < 0 && p->var->kind == VK::Param && p->var->scope == fsc) p->var->slot = static_cast<int>(i);
      ++i;
    }
  }
  // a named function expression's own name
  Scope* ns = (fsc->parent && fsc->parent->kind == Scope::Kind::Block && fsc->parent->node == fnode) ? fsc->parent : nullptr;
  if (ns) {
    fs->cur_scope = ns;
    assign_registers(ns);
    if (ns->materialized) {
      emit_u16(OpCode::PushEnv, static_cast<uint16_t>(ns->env_size));
      ++fs->env_depth;
    }
    for (VarInfo* v : ns->vars) {
      emit(OpCode::LoadFunction);
      store_var_info(v, v->name, StoreMode::InitLexical, fnode);
    }
  }
  enter_scope(fsc);
  fs->cur_scope = fsc;
  if (fsc->uses_this_var) {
    auto it = fsc->map.find(u"this");
    if (it != fsc->map.end()) fs->this_var = it->second;
  }
  // arguments
  if (!fsc->is_arrow) {
    auto it = fsc->map.find(u"arguments");
    if (it != fsc->map.end() && it->second->kind == VK::Arguments) {
      emit(OpCode::CreateArguments);
      store_var_info(it->second, u"arguments", StoreMode::InitVar, fnode);
    }
  }
  // parameters
  {
    uint32_t i = 0;
    for (Node* p : fi->params) {
      if (p->kind == NK::Rest) {
        emit_u16(OpCode::CreateRest, static_cast<uint16_t>(nparams_slots));
        compile_pattern_store(p->a, StoreMode::InitVar);
        break;
      }
      if (p->kind == NK::Ident) {
        VarInfo* v = p->var;
        if (!(v && !v->in_env && v->slot == static_cast<int>(i))) {
          emit_u16(OpCode::GetLocal, static_cast<uint16_t>(i));
          store_var(p, StoreMode::InitVar);
        }
      } else if (p->kind == NK::AssignPattern) {
        emit_u16(OpCode::GetLocal, static_cast<uint16_t>(i));
        compile_default_value(p->b, p->a);
        compile_pattern_store(p->a, StoreMode::InitVar);
      } else {
        emit_u16(OpCode::GetLocal, static_cast<uint16_t>(i));
        compile_pattern_store(p, StoreMode::InitVar);
      }
      ++i;
    }
  }
  hoist_functions(fsc);
  if (fi->is_generator) emit(OpCode::InitialYield);
}

uint32_t Compiler::compile_function(Node* fnode, FuncInfo* fi, bool is_script) {
  FuncState st;
  st.parent = fs;
  st.info = fi;
  st.node = fnode;
  st.is_script = is_script;
  FunctionCode* fc = ctx.heap.make<FunctionCode>();
  st.code = fc;
  fc->name = ctx.heap.intern(fi ? fi->name : std::u16string());
  fc->source = source;
  fc->filename = filename;
  if (fi) {
    fc->src_start = fi->src_start;
    fc->src_end = fi->src_end;
    fc->length = fi->length;
    fc->strict = fi->strict;
    fc->is_arrow = fi->kind == FuncKind::Arrow;
    fc->is_async = fi->is_async;
    fc->is_generator = fi->is_generator;
    fc->is_method = fi->kind == FuncKind::Method || fi->kind == FuncKind::Getter || fi->kind == FuncKind::Setter || fi->kind == FuncKind::ClassFieldInit;
    fc->is_class_constructor = fi->kind == FuncKind::ClassConstructor || fi->kind == FuncKind::DerivedConstructor;
    fc->is_derived = fi->kind == FuncKind::DerivedConstructor;
    fc->is_getter_or_setter = fi->kind == FuncKind::Getter || fi->kind == FuncKind::Setter;
    fc->uses_arguments = fi->uses_arguments;
    for (Node* p : fi->params) if (p->kind == NK::Rest) fc->has_rest = true;
    fc->has_simple_params = fi->has_simple_params;
    fc->line = fi->line;
    st.is_derived_ctor = fc->is_derived;
  }
  fc->is_script = is_script;
  if (!fi) fc->strict = prog.strict;
  FuncState* saved = fs;
  fs = &st;
  Scope* saved_chain_scope = nullptr;
  (void)saved_chain_scope;
  std::vector<OptSite>* saved_chain = chain;
  std::deque<Label>* saved_chain_labels = chain_labels;
  chain = nullptr;
  chain_labels = nullptr;
  if (is_script) {
    st.fscope = an.script;
    st.cur_scope = an.script;
    st.completion_reg = static_cast<int>(alloc_reg());
    emit(OpCode::Undefined);
    emit_u16(OpCode::SetLocal, static_cast<uint16_t>(st.completion_reg));
    // global declarations
    Scope* sc = an.script;
    for (auto& g : sc->global_lexicals) emit_u32_u8(OpCode::DeclareGlobalLex, k_atom(g.first), g.second ? 1 : 0);
    for (auto& g : sc->global_vars) emit_u32(OpCode::DeclareGlobalVar, k_atom(g));
    for (Node* f : sc->global_funcs) {
      compile_closure(f);
      emit_u32(OpCode::DeclareGlobalFunc, k_atom(f->fn->name));
    }
    compile_statements(prog.root->list);
    emit_u16(OpCode::GetLocal, static_cast<uint16_t>(st.completion_reg));
    emit(OpCode::Return);
  } else {
    function_prologue(fnode, fi);
    if (fi->expression_body) {
      compile_expr(fi->body);
      emit(OpCode::Return);
    } else {
      compile_statements(fi->body->list);
    }
    if (fs->reachable) {
      if (fs->is_derived_ctor && fs->this_var) {
        emit(OpCode::Undefined);
        load_var_unchecked(fs->this_var);
        emit(OpCode::ReturnCtor);
      } else {
        emit(OpCode::Undefined);
        emit(OpCode::Return);
      }
    }
  }
  fc->max_stack = static_cast<uint32_t>(st.max_depth + 8);
  chain = saved_chain;
  chain_labels = saved_chain_labels;
  fs = saved;
  last_code = fc;
  if (saved) {
    saved->code->functions.push_back(fc);
    return static_cast<uint32_t>(saved->code->functions.size() - 1);
  }
  return 0;
}

void Compiler::compile_closure(Node* fnode) {
  uint32_t idx = compile_function(fnode, fnode->fn);
  emit_u32(OpCode::Closure, idx);
}

// ---------------------------------------------------------------------------------------------------
// classes
// ---------------------------------------------------------------------------------------------------

void Compiler::compile_class(Node* n) {
  Scope* cs = n->scope;
  enter_scope(cs);
  // private names are created once per class evaluation
  for (VarInfo* v : cs->vars) {
    if (v->kind != VK::Private) continue;
    emit_u32(OpCode::NewPrivateName, k_atom(v->name.substr(1)));
    store_var_info(v, v->name, StoreMode::InitLexical, n);
  }
  const bool has_super = n->a != nullptr;
  if (has_super) compile_expr(n->a);
  Node* ctor_member = nullptr;
  for (Node* m : n->list)
    if (m->kind == NK::Property && (m->flags & kFlagProto) && (m->flags & kFlagMethod)) ctor_member = m;
  const std::u16string class_name = !n->str.empty() ? n->str : n->str2;
  mark_line(n->line);
  uint32_t idx = compile_function(ctor_member->b, ctor_member->b->fn);
  fs->code->functions[idx]->name = ctx.heap.intern(class_name);
  fs->code->functions[idx]->src_start = n->fn ? n->fn->src_start : 0;
  fs->code->functions[idx]->src_end = n->fn ? n->fn->src_end : 0;
  uint32_t ctor_reg = alloc_reg(), proto_reg = alloc_reg();
  emit_u32_u8_u16_u16(OpCode::CreateClass, idx, has_super ? 1 : 0, static_cast<uint16_t>(ctor_reg), static_cast<uint16_t>(proto_reg));

  struct StaticItem { Node* member; int key_reg; };
  std::vector<StaticItem> statics;
  bool any_instance = false;
  uint32_t fields_reg = 0;
  auto ensure_fields = [&]() {
    if (any_instance) return;
    any_instance = true;
    fields_reg = alloc_reg();
    emit_u32(OpCode::NewArray, 0);
    emit_u16(OpCode::SetLocal, static_cast<uint16_t>(fields_reg));
  };
  auto push_field_entry = [&](std::function<void()> key, std::function<void()> value, int kind) {
    emit_u16(OpCode::GetLocal, static_cast<uint16_t>(fields_reg));
    key();
    emit(OpCode::ArrayPush);
    value();
    emit(OpCode::ArrayPush);
    emit_i32(OpCode::Int, kind);
    emit(OpCode::ArrayPush);
    emit(OpCode::Pop);
  };
  auto load_private = [&](Node* key_node) {
    VarInfo* v = lookup_var(u"#" + key_node->str, fs->cur_scope);
    load_var_info(v, u"#" + key_node->str);
  };

  for (Node* m : n->list) {
    if (failed()) return;
    if (m == ctor_member) continue;
    if (m->kind == NK::StaticBlock) {
      statics.push_back({m, -1});
      continue;
    }
    const bool is_static = m->has(kFlagStatic);
    const bool is_private = m->has(kFlagPrivate);
    if (m->has(kFlagMethod)) {
      uint32_t kind = m->has(kFlagGetter) ? 1 : m->has(kFlagSetter) ? 2 : 0;
      if (is_private) {
        // private method / accessor: defined on instances during construction (or on the class, if static)
        if (is_static) {
          emit_u16(OpCode::GetLocal, static_cast<uint16_t>(ctor_reg));
          load_private(m->a);
          emit_u16(OpCode::GetLocal, static_cast<uint16_t>(is_static ? ctor_reg : proto_reg));
          compile_closure(m->b);
          emit_u8(OpCode::SetHomeObject, 0);
          emit(OpCode::Swap);
          emit(OpCode::Pop);
          emit_u8(OpCode::PrivateDefine, static_cast<uint8_t>(kind == 0 ? 1 : kind == 1 ? 2 : 3));
        } else {
          ensure_fields();
          push_field_entry([&] { load_private(m->a); },
                           [&] {
                             emit_u16(OpCode::GetLocal, static_cast<uint16_t>(proto_reg));
                             compile_closure(m->b);
                             emit_u8(OpCode::SetHomeObject, 0);
                             emit(OpCode::Swap);
                             emit(OpCode::Pop);
                           },
                           kind == 0 ? 2 : kind == 1 ? 3 : 4);
        }
        continue;
      }
      uint32_t target_reg = is_static ? ctor_reg : proto_reg;
      emit_u16(OpCode::GetLocal, static_cast<uint16_t>(target_reg));
      const bool computed = m->has(kFlagComputed);
      if (computed) {
        compile_expr(m->a);
        emit(OpCode::ToPropertyKey);
      }
      compile_closure(m->b);
      emit_u8(OpCode::SetHomeObject, computed ? 1 : 0);
      if (computed) emit_u8(OpCode::NameFunction, static_cast<uint8_t>(kind));
      OpCode op;
      if (computed) op = kind == 0 ? OpCode::DefineMethodComputed : kind == 1 ? OpCode::DefineGetterComputed : OpCode::DefineSetterComputed;
      else op = kind == 0 ? OpCode::DefineMethod : kind == 1 ? OpCode::DefineGetter : OpCode::DefineSetter;
      if (computed) emit_u8(op, 0);
      else {
        std::u16string key = m->a->kind == NK::Number ? std::u16string() : m->a->str;
        if (m->a->kind == NK::Number) key = ctx.number_to_string(m->a->num)->str();
        emit_u32_u8(op, k_atom(key), 0);
      }
      emit(OpCode::Pop);
      continue;
    }
    // a field
    int key_reg = -1;
    if (m->has(kFlagComputed)) {
      compile_expr(m->a);
      emit(OpCode::ToPropertyKey);
      key_reg = static_cast<int>(alloc_reg());
      emit_u16(OpCode::SetLocal, static_cast<uint16_t>(key_reg));
    }
    if (is_static) {
      statics.push_back({m, key_reg});
      continue;
    }
    ensure_fields();
    auto key_emit = [&] {
      if (is_private) load_private(m->a);
      else if (key_reg >= 0) emit_u16(OpCode::GetLocal, static_cast<uint16_t>(key_reg));
      else {
        std::u16string key = m->a->kind == NK::Number ? ctx.number_to_string(m->a->num)->str() : m->a->str;
        emit_u32(OpCode::Const, k_atom(key));
      }
    };
    push_field_entry(key_emit,
                     [&] {
                       if (m->b) {
                         emit_u16(OpCode::GetLocal, static_cast<uint16_t>(proto_reg));
                         compile_closure(m->b);
                         emit_u8(OpCode::SetHomeObject, 0);
                         emit(OpCode::Swap);
                         emit(OpCode::Pop);
                       } else {
                         emit(OpCode::Undefined);
                       }
                     },
                     is_private ? 1 : 0);
  }
  // the class's own name, inside the class, is bound once the constructor exists
  if (!n->str.empty()) {
    auto it = cs->map.find(n->str);
    if (it != cs->map.end() && it->second->kind == VK::ClassName) {
      emit_u16(OpCode::GetLocal, static_cast<uint16_t>(ctor_reg));
      store_var_info(it->second, n->str, StoreMode::InitLexical, n);
    }
  }
  if (any_instance) {
    emit_u16(OpCode::GetLocal, static_cast<uint16_t>(fields_reg));
    emit_u16(OpCode::ClassFields, static_cast<uint16_t>(ctor_reg));
  }
  // static fields and blocks, in order, with the class as `this`
  for (auto& s : statics) {
    Node* m = s.member;
    if (m->kind == NK::StaticBlock) {
      emit_u16(OpCode::GetLocal, static_cast<uint16_t>(ctor_reg));
      compile_closure(m->a);
      emit_u8(OpCode::SetHomeObject, 0);
      emit(OpCode::Swap);
      emit(OpCode::Pop);
      emit_u16(OpCode::GetLocal, static_cast<uint16_t>(ctor_reg));
      emit_u16(OpCode::Call, 0);
      emit(OpCode::Pop);
      continue;
    }
    const bool is_private = m->has(kFlagPrivate);
    emit_u16(OpCode::GetLocal, static_cast<uint16_t>(ctor_reg));
    if (is_private) load_private(m->a);
    else if (s.key_reg >= 0) emit_u16(OpCode::GetLocal, static_cast<uint16_t>(s.key_reg));
    else {
      std::u16string key = m->a->kind == NK::Number ? ctx.number_to_string(m->a->num)->str() : m->a->str;
      emit_u32(OpCode::Const, k_atom(key));
    }
    if (m->b) {
      emit_u16(OpCode::GetLocal, static_cast<uint16_t>(ctor_reg));
      compile_closure(m->b);
      emit_u8(OpCode::SetHomeObject, 0);
      emit(OpCode::Swap);
      emit(OpCode::Pop);
      emit_u16(OpCode::GetLocal, static_cast<uint16_t>(ctor_reg));
      emit_u16(OpCode::Call, 0);
    } else {
      emit(OpCode::Undefined);
    }
    if (is_private) emit_u8(OpCode::PrivateDefine, 0);
    else {
      emit(OpCode::DefineFieldComputed);
      emit(OpCode::Pop);
    }
  }
  leave_scope(cs);
  emit_u16(OpCode::GetLocal, static_cast<uint16_t>(ctor_reg));
}

// ---------------------------------------------------------------------------------------------------
// entry point
// ---------------------------------------------------------------------------------------------------

FunctionCode* compile_program(Context& ctx, Program& prog, Analysis& an, const std::string& filename, std::string* err, uint32_t* err_line) {
  Compiler c(ctx, prog, an);
  c.source = prog.source;
  c.filename = ctx.heap.intern_utf8(filename);
  c.compile_function(prog.root, nullptr, true);
  if (c.failed()) {
    if (err) *err = c.err;
    if (err_line) *err_line = c.err_line;
    return nullptr;
  }
  return c.last_code;
}

}  // namespace aios::js
