// Code generation: expressions and destructuring patterns.
#include <cmath>
#include <cstring>

#include "bigint.hpp"
#include "codegen.hpp"
#include "internal.hpp"

namespace aios::js {

void Compiler::emit_u32_u32(OpCode op, uint32_t a, uint32_t b) {
  fs->code->code.push_back(static_cast<uint8_t>(op));
  for (int i = 0; i < 4; ++i) fs->code->code.push_back(static_cast<uint8_t>(a >> (8 * i)));
  for (int i = 0; i < 4; ++i) fs->code->code.push_back(static_cast<uint8_t>(b >> (8 * i)));
  adj(1);   // only NewRegExp uses this form: it pushes the new object
}

std::u16string Compiler::key_string(Node* key) {
  if (key->kind == NK::Number) return ctx.number_to_string(key->num)->str();
  return key->str;
}

bool Compiler::is_anonymous_fn(Node* n) {
  if (!n || n->has(kFlagParenthesized)) return false;
  if (n->kind == NK::FunctionExpr || n->kind == NK::ArrowFunction) return n->fn && n->fn->name.empty();
  if (n->kind == NK::ClassExpr) return n->str.empty() && n->str2.empty();
  return false;
}

void Compiler::emit_binary_op(Op op) {
  switch (op) {
    case Op::Add: emit(OpCode::Add); break;
    case Op::Sub: emit(OpCode::Sub); break;
    case Op::Mul: emit(OpCode::Mul); break;
    case Op::Div: emit(OpCode::Div); break;
    case Op::Mod: emit(OpCode::Mod); break;
    case Op::Exp: emit(OpCode::Exp); break;
    case Op::Shl: emit(OpCode::Shl); break;
    case Op::Shr: emit(OpCode::Shr); break;
    case Op::UShr: emit(OpCode::UShr); break;
    case Op::BitAnd: emit(OpCode::BitAnd); break;
    case Op::BitOr: emit(OpCode::BitOr); break;
    case Op::BitXor: emit(OpCode::BitXor); break;
    case Op::Eq: emit(OpCode::Eq); break;
    case Op::Ne: emit(OpCode::Ne); break;
    case Op::StrictEq: emit(OpCode::StrictEq); break;
    case Op::StrictNe: emit(OpCode::StrictNe); break;
    case Op::Lt: emit(OpCode::Lt); break;
    case Op::Gt: emit(OpCode::Gt); break;
    case Op::Le: emit(OpCode::Le); break;
    case Op::Ge: emit(OpCode::Ge); break;
    case Op::In: emit(OpCode::In); break;
    case Op::InstanceOf: emit(OpCode::InstanceOf); break;
    default: fail("unsupported binary operator", 0); break;
  }
}

void Compiler::load_this(Node* n) {
  if (n && n->var) load_var_info(n->var, u"this");
  else emit(OpCode::LoadThis);
}

void Compiler::load_private_name(Node* pn) {
  load_var_info(pn->var, u"#" + pn->str);
}

// The value to test is on top of the stack and stays there on the fall-through path. When it is null or undefined the
// whole enclosing chain short-circuits: the landing pad pops whatever sits above the chain's base depth.
void Compiler::optional_site(int extra_items) {
  (void)extra_items;
  if (!chain || !chain_labels) { fail("optional chain outside of a chain", 0); return; }
  chain_labels->emplace_back();
  Label* l = &chain_labels->back();
  chain->push_back({l, 0});
  emit(OpCode::Dup);
  jump(OpCode::JumpIfNullish, *l);
}

uint32_t Compiler::template_site(Node* tpl) {
  FunctionCode::TemplateSite site;
  site.cooked = tpl->tpl->cooked;
  site.raw = tpl->tpl->raw;
  site.ok = tpl->tpl->cooked_ok;
  fs->code->templates.push_back(std::move(site));
  return static_cast<uint32_t>(fs->code->templates.size() - 1);
}

// ---------------------------------------------------------------------------------------------------
// expressions
// ---------------------------------------------------------------------------------------------------

void Compiler::compile_expr_discard(Node* n) {
  if (n->kind == NK::Update && !n->has(kFlagPrefix)) {
    // the old value is not needed: compile as a prefix update
    Node tmp = *n;
    tmp.flags |= kFlagPrefix;
    compile_update(&tmp);
    emit(OpCode::Pop);
    return;
  }
  compile_expr(n);
  emit(OpCode::Pop);
}

void Compiler::compile_expr(Node* n) {
  if (!n || failed()) {
    if (!failed()) fail("missing expression", 0);
    emit(OpCode::Undefined);
    return;
  }
  switch (n->kind) {
    case NK::Number: {
      double d = n->num;
      if (d >= -2147483648.0 && d <= 2147483647.0 && d == static_cast<double>(static_cast<int32_t>(d)) && !(d == 0 && std::signbit(d))) emit_i32(OpCode::Int, static_cast<int32_t>(d));
      else emit_u32(OpCode::Const, k_num(d));
      return;
    }
    case NK::BigIntLit: {
      BigNum v;
      if (!big::string_to_bigint(n->str, &v)) { fail("Invalid BigInt literal", n->line); emit(OpCode::Undefined); return; }
      fs->code->consts.push_back(Value::Big(ctx.heap.make<BigInt>(std::move(v))));
      emit_u32(OpCode::Const, static_cast<uint32_t>(fs->code->consts.size() - 1));
      return;
    }
    case NK::String: emit_u32(OpCode::Const, k_atom(n->str)); return;
    case NK::Null: emit(OpCode::Null); return;
    case NK::Bool: emit(n->num != 0 ? OpCode::True : OpCode::False); return;
    case NK::RegExpLit: emit_u32_u32(OpCode::NewRegExp, k_atom(n->str), k_atom(n->str2)); return;
    case NK::Template: compile_template(n); return;
    case NK::TaggedTemplate: compile_tagged_template(n); return;
    case NK::Ident:
      load_var(n);
      return;
    case NK::This: load_this(n); return;
    case NK::NewTarget: emit(OpCode::LoadNewTarget); return;
    case NK::ArrayLit: compile_array(n); return;
    case NK::ObjectLit: compile_object(n); return;
    case NK::FunctionExpr: case NK::ArrowFunction: compile_closure(n); return;
    case NK::ClassExpr: compile_class(n); return;
    case NK::Member: case NK::Index: compile_member(n); return;
    case NK::Call: compile_call(n); return;
    case NK::New: compile_new(n); return;
    case NK::OptChain: compile_optchain(n); return;
    case NK::Unary: compile_unary(n); return;
    case NK::Update: compile_update(n); return;
    case NK::Binary: compile_binary(n); return;
    case NK::Logical: compile_logical(n); return;
    case NK::Assign: compile_assign(n); return;
    case NK::Conditional: compile_conditional(n); return;
    case NK::Sequence:
      for (size_t i = 0; i + 1 < n->list.size(); ++i) compile_expr_discard(n->list[i]);
      compile_expr(n->list.back());
      return;
    case NK::Yield: compile_yield(n); return;
    case NK::Await:
      compile_expr(n->a);
      emit(OpCode::Await);
      return;
    case NK::PrivateIn:
      load_private_name(n->a);
      compile_expr(n->b);
      emit(OpCode::PrivateIn);
      return;
    case NK::Spread:
      fail("unexpected spread", n->line);
      return;
    default:
      fail("unsupported expression", n->line);
      emit(OpCode::Undefined);
      return;
  }
}

void Compiler::compile_template(Node* n) {
  TemplateData* t = n->tpl;
  emit_u32(OpCode::Const, k_atom(t->cooked[0]));
  for (size_t i = 0; i < n->list.size(); ++i) {
    compile_expr(n->list[i]);
    emit(OpCode::ToString);
    emit(OpCode::Add);
    if (!t->cooked[i + 1].empty()) {
      emit_u32(OpCode::Const, k_atom(t->cooked[i + 1]));
      emit(OpCode::Add);
    }
  }
}

void Compiler::compile_tagged_template(Node* n) {
  compile_callee_this(n->a);
  emit_u32(OpCode::GetTemplateObject, template_site(n->b));
  for (Node* e : n->b->list) compile_expr(e);
  emit_u16(OpCode::Call, static_cast<uint16_t>(1 + n->b->list.size()));
}

void Compiler::compile_array(Node* n) {
  emit_u32(OpCode::NewArray, static_cast<uint32_t>(n->list.size()));
  for (Node* e : n->list) {
    if (!e) emit(OpCode::ArrayHole);
    else if (e->kind == NK::Spread) {
      compile_expr(e->a);
      emit(OpCode::ArraySpread);
    } else {
      compile_expr(e);
      emit(OpCode::ArrayPush);
    }
  }
}

void Compiler::compile_object(Node* n) {
  emit(OpCode::NewObject);
  for (Node* p : n->list) {
    if (failed()) return;
    if (p->has(kFlagSpread)) {
      compile_expr(p->a);
      emit(OpCode::ObjectSpread);
      continue;
    }
    const bool computed = p->has(kFlagComputed);
    if (p->has(kFlagMethod)) {
      const uint32_t kind = p->has(kFlagGetter) ? 1 : p->has(kFlagSetter) ? 2 : 0;
      if (computed) {
        compile_expr(p->a);
        emit(OpCode::ToPropertyKey);
      }
      compile_closure(p->b);
      emit_u8(OpCode::SetHomeObject, computed ? 1 : 0);
      if (computed) {
        emit_u8(OpCode::NameFunction, static_cast<uint8_t>(kind));
        emit_u8(kind == 0 ? OpCode::DefineMethodComputed : kind == 1 ? OpCode::DefineGetterComputed : OpCode::DefineSetterComputed, 1);
      } else {
        emit_u32_u8(kind == 0 ? OpCode::DefineMethod : kind == 1 ? OpCode::DefineGetter : OpCode::DefineSetter, k_atom(key_string(p->a)), 1);
      }
      continue;
    }
    if (p->has(kFlagProto) && !computed) {
      compile_expr(p->b);
      emit(OpCode::SetProto);
      continue;
    }
    if (computed) {
      compile_expr(p->a);
      emit(OpCode::ToPropertyKey);
      compile_expr(p->b);
      if (is_anonymous_fn(p->b)) emit_u8(OpCode::NameFunction, 0);
      emit(OpCode::DefineFieldComputed);
    } else {
      compile_expr(p->b);
      emit_u32(OpCode::DefineField, k_atom(key_string(p->a)));
    }
  }
}

// ---- member access, calls ----

void Compiler::compile_member(Node* n) {
  const bool is_member = n->kind == NK::Member;
  if (n->a->kind == NK::Super) {
    load_this(n->a);
    if (is_member) emit_u32(OpCode::Const, k_atom(n->str));
    else {
      compile_expr(n->b);
      emit(OpCode::ToPropertyKey);
    }
    emit(OpCode::GetSuperProp);
    return;
  }
  compile_expr(n->a);
  if (n->has(kFlagOptional)) optional_site(0);
  if (n == delete_node) {
    if (is_member) emit_u32_u8(OpCode::DeleteProp, k_atom(n->str), strict_mode() ? 1 : 0);
    else {
      compile_expr(n->b);
      emit_u8(OpCode::DeleteElem, strict_mode() ? 1 : 0);
    }
    return;
  }
  if (is_member) {
    if (n->b) {
      load_private_name(n->b);
      emit(OpCode::PrivateGet);
    } else {
      emit_u32(OpCode::GetProp, k_atom(n->str));
    }
    return;
  }
  // a["literal"] is a named property unless the key is an array index
  if (n->b->kind == NK::String && !n->b->str.empty() && !(n->b->str[0] >= u'0' && n->b->str[0] <= u'9') && n->b->str != u"-0") {
    emit_u32(OpCode::GetProp, k_atom(n->b->str));
    return;
  }
  compile_expr(n->b);
  emit(OpCode::GetElem);
}

void Compiler::compile_callee_this(Node* callee) {
  if (callee->kind == NK::Member || callee->kind == NK::Index) {
    if (callee->a->kind == NK::Super) {
      compile_member(callee);
      load_this(callee->a);
      return;
    }
    compile_expr(callee->a);
    if (callee->has(kFlagOptional)) optional_site(0);
    emit(OpCode::Dup);
    if (callee->kind == NK::Member) {
      if (callee->b) {
        load_private_name(callee->b);
        emit(OpCode::PrivateGet);
      } else {
        emit_u32(OpCode::GetProp, k_atom(callee->str));
      }
    } else {
      compile_expr(callee->b);
      emit(OpCode::GetElem);
    }
    emit(OpCode::Swap);
    return;
  }
  compile_expr(callee);
  emit(OpCode::Undefined);
}

void Compiler::compile_args(std::vector<Node*>& list, size_t from, bool* spread) {
  bool any = false;
  for (size_t i = from; i < list.size(); ++i)
    if (list[i]->kind == NK::Spread) any = true;
  *spread = any;
  if (!any) {
    for (size_t i = from; i < list.size(); ++i) compile_expr(list[i]);
    return;
  }
  emit_u32(OpCode::NewArray, static_cast<uint32_t>(list.size() - from));
  for (size_t i = from; i < list.size(); ++i) {
    if (list[i]->kind == NK::Spread) {
      compile_expr(list[i]->a);
      emit(OpCode::ArraySpread);
    } else {
      compile_expr(list[i]);
      emit(OpCode::ArrayPush);
    }
  }
}

// The callee as the source wrote it, for the "x.y is not a function" message; anything it cannot spell is "(intermediate value)".
static std::u16string callee_text(Node* n, int depth = 0) {
  if (!n || depth > 6) return u"(intermediate value)";
  switch (n->kind) {
    case NK::Ident: return n->str;
    case NK::This: return u"this";
    case NK::Member: return n->b ? u"(intermediate value)" : callee_text(n->a, depth + 1) + u"." + n->str;
    case NK::Index: return callee_text(n->a, depth + 1) + u"[...]";
    case NK::Call: return callee_text(n->a, depth + 1) + u"(...)";
    default: return u"(intermediate value)";
  }
}

void Compiler::compile_call(Node* n) {
  mark_line(n->line);
  Node* callee = n->a;
  if (callee->kind == NK::Super) {
    bool spread;
    compile_args(n->list, 0, &spread);
    if (spread) emit(OpCode::SuperCallSpread);
    else emit_u16(OpCode::SuperCall, static_cast<uint16_t>(n->list.size()));
    // bind `this`, then run the field initialisers
    emit(OpCode::Dup);
    VarInfo* v = callee->var;
    if (!v) { fail("super call outside a derived constructor", n->line); return; }
    if (v->in_env) emit_u16_u16(OpCode::InitThisEnv, static_cast<uint16_t>(env_hops(fs->cur_scope, v->scope)), static_cast<uint16_t>(v->slot));
    else emit_u16(OpCode::InitThisLocal, static_cast<uint16_t>(v->slot));
    emit(OpCode::RunFieldInit);
    return;
  }
  compile_callee_this(callee);
  if (n->has(kFlagOptional)) {
    // a?.() : the callee is below `this`; test it by duplicating through a rotation
    emit(OpCode::Swap);
    optional_site(0);
    emit(OpCode::Swap);
  }
  bool spread;
  compile_args(n->list, 0, &spread);
  if (spread) emit(OpCode::CallSpread);
  else {
    emit_u16(OpCode::Call, static_cast<uint16_t>(n->list.size()));
    fs->code->call_sites.push_back({pc(), k_atom(callee_text(callee))});
  }
}

void Compiler::compile_new(Node* n) {
  mark_line(n->line);
  compile_expr(n->a);
  bool spread;
  compile_args(n->list, 0, &spread);
  if (spread) emit(OpCode::NewSpread);
  else emit_u16(OpCode::New, static_cast<uint16_t>(n->list.size()));
}

void Compiler::compile_optchain(Node* n) {
  std::vector<OptSite>* saved = chain;
  std::deque<Label>* saved_labels = chain_labels;
  std::deque<Label> labels;
  std::vector<OptSite> sites;
  chain = &sites;
  chain_labels = &labels;
  const int base = fs->depth;
  const bool deleting = delete_chain;
  delete_chain = false;
  Label Lend;
  compile_expr(n->a);
  chain = saved;
  chain_labels = saved_labels;
  if (!sites.empty()) {
    jump(OpCode::Jump, Lend);
    for (size_t i = 0; i < sites.size(); ++i) {
      Label& l = *sites[i].landing;
      bind(l);
      for (int d = fs->depth; d > base; --d) emit(OpCode::Pop);
      emit(deleting ? OpCode::True : OpCode::Undefined);
      if (i + 1 < sites.size()) jump(OpCode::Jump, Lend);
    }
    bind(Lend);
  }
}

// ---- operators ----

void Compiler::compile_unary(Node* n) {
  switch (n->op) {
    case Op::Neg: compile_expr(n->a); emit(OpCode::Neg); return;
    case Op::Pos: compile_expr(n->a); emit(OpCode::Pos); return;
    case Op::Not: compile_expr(n->a); emit(OpCode::Not); return;
    case Op::BitNot: compile_expr(n->a); emit(OpCode::BitNot); return;
    case Op::Void: compile_expr(n->a); emit(OpCode::Void); return;
    case Op::TypeOf:
      if (n->a->kind == NK::Ident && !n->a->var && n->a->str != u"undefined") {
        emit_u32(OpCode::TypeofGlobal, k_atom(n->a->str));
        return;
      }
      compile_expr(n->a);
      emit(OpCode::TypeOf);
      return;
    case Op::Delete: {
      Node* t = n->a;
      if (t->kind == NK::OptChain) {
        Node* inner = t->a;
        Node* saved_node = delete_node;
        bool saved_chain_flag = delete_chain;
        delete_node = inner;
        delete_chain = true;
        compile_optchain(t);
        delete_node = saved_node;
        delete_chain = saved_chain_flag;
        return;
      }
      if (t->kind == NK::Member || t->kind == NK::Index) {
        if (t->a->kind == NK::Super) {
          load_this(t->a);
          emit(OpCode::Pop);
          emit_u8_u32(OpCode::ThrowError, static_cast<uint8_t>(ErrorKind::ReferenceError), k_atom(u"Unsupported reference to 'super'"));
          emit(OpCode::True);
          return;
        }
        Node* saved_node = delete_node;
        delete_node = t;
        compile_member(t);
        delete_node = saved_node;
        return;
      }
      if (t->kind == NK::Ident) {
        if (t->var) {
          emit(OpCode::False);
        } else {
          emit(OpCode::GlobalObject);
          emit_u32(OpCode::Const, k_atom(t->str));
          emit_u8(OpCode::DeleteElem, 0);
        }
        return;
      }
      compile_expr(t);
      emit(OpCode::Pop);
      emit(OpCode::True);
      return;
    }
    default:
      fail("unsupported unary operator", n->line);
      return;
  }
}

void Compiler::compile_binary(Node* n) {
  compile_expr(n->a);
  compile_expr(n->b);
  emit_binary_op(n->op);
}

void Compiler::compile_logical(Node* n) {
  Label Lend;
  compile_expr(n->a);
  jump(n->op == Op::And ? OpCode::JumpIfFalseKeep : n->op == Op::Or ? OpCode::JumpIfTrueKeep : OpCode::JumpIfNotNullishKeep, Lend);
  compile_expr(n->b);
  bind(Lend);
}

void Compiler::compile_conditional(Node* n) {
  Label Lelse, Lend;
  compile_expr(n->a);
  jump(OpCode::JumpIfFalse, Lelse);
  compile_expr(n->b);
  jump(OpCode::Jump, Lend);
  bind(Lelse);
  compile_expr(n->c);
  bind(Lend);
}

void Compiler::compile_yield(Node* n) {
  const bool async_gen = fs->info && fs->info->is_async;
  if (n->has(kFlagDelegate)) {
    compile_expr(n->a);
    uint32_t tmp = alloc_reg();
    alloc_reg();
    alloc_reg();
    alloc_reg();
    emit_u16(async_gen ? OpCode::AsyncIterInit : OpCode::IterInit, static_cast<uint16_t>(tmp));
    emit_u16(OpCode::YieldStar, static_cast<uint16_t>(tmp));
    return;
  }
  if (n->a) compile_expr(n->a);
  else emit(OpCode::Undefined);
  if (async_gen) {
    emit(OpCode::Await);
    emit(OpCode::AsyncGenYield);
  } else {
    emit(OpCode::Yield);
  }
}

// ---- references ----

Compiler::RefKind Compiler::ref_prepare(Node* t) {
  switch (t->kind) {
    case NK::Ident: return RefKind::Var;
    case NK::Member:
      if (t->a->kind == NK::Super) {
        load_this(t->a);
        emit_u32(OpCode::Const, k_atom(t->str));
        return RefKind::Super;
      }
      compile_expr(t->a);
      if (t->b) {
        load_private_name(t->b);
        return RefKind::Priv;
      }
      return RefKind::Prop;
    case NK::Index:
      if (t->a->kind == NK::Super) {
        load_this(t->a);
        compile_expr(t->b);
        emit(OpCode::ToPropertyKey);
        return RefKind::Super;
      }
      compile_expr(t->a);
      compile_expr(t->b);
      emit(OpCode::ToPropertyKey);
      return RefKind::Elem;
    default:
      fail("invalid assignment target", t->line);
      return RefKind::Var;
  }
}

void Compiler::ref_get(RefKind k, Node* t) {
  switch (k) {
    case RefKind::Var: load_var(t); break;
    case RefKind::Prop:
      emit(OpCode::Dup);
      emit_u32(OpCode::GetProp, k_atom(t->str));
      break;
    case RefKind::Elem:
      emit(OpCode::Dup2);
      emit(OpCode::GetElem);
      break;
    case RefKind::Priv:
      emit(OpCode::Dup2);
      emit(OpCode::PrivateGet);
      break;
    case RefKind::Super:
      emit(OpCode::Dup2);
      emit(OpCode::GetSuperProp);
      break;
  }
}

void Compiler::ref_put(RefKind k, Node* t) {
  switch (k) {
    case RefKind::Var:
      emit(OpCode::Dup);
      store_var(t, StoreMode::Assign);
      break;
    case RefKind::Prop: emit_u32_u8(OpCode::SetProp, k_atom(t->str), strict_mode() ? 1 : 0); break;
    case RefKind::Elem: emit_u8(OpCode::SetElem, strict_mode() ? 1 : 0); break;
    case RefKind::Priv: emit(OpCode::PrivateSet); break;
    case RefKind::Super: emit_u8(OpCode::SetSuperProp, strict_mode() ? 1 : 0); break;
  }
}

void Compiler::compile_assign(Node* n) {
  Node* t = n->a;
  mark_line(n->line);
  if (n->op == Op::Plain) {
    switch (t->kind) {
      case NK::Ident:
        compile_expr(n->b);
        emit(OpCode::Dup);
        store_var(t, StoreMode::Assign);
        return;
      case NK::Member:
        if (t->a->kind == NK::Super) {
          load_this(t->a);
          emit_u32(OpCode::Const, k_atom(t->str));
          compile_expr(n->b);
          emit_u8(OpCode::SetSuperProp, strict_mode() ? 1 : 0);
          return;
        }
        compile_expr(t->a);
        if (t->b) {
          load_private_name(t->b);
          compile_expr(n->b);
          emit(OpCode::PrivateSet);
          return;
        }
        compile_expr(n->b);
        emit_u32_u8(OpCode::SetProp, k_atom(t->str), strict_mode() ? 1 : 0);
        return;
      case NK::Index:
        if (t->a->kind == NK::Super) {
          load_this(t->a);
          compile_expr(t->b);
          emit(OpCode::ToPropertyKey);
          compile_expr(n->b);
          emit_u8(OpCode::SetSuperProp, strict_mode() ? 1 : 0);
          return;
        }
        compile_expr(t->a);
        compile_expr(t->b);
        compile_expr(n->b);
        emit_u8(OpCode::SetElem, strict_mode() ? 1 : 0);
        return;
      case NK::ArrayPattern: case NK::ObjectPattern:
        compile_expr(n->b);
        emit(OpCode::Dup);
        compile_pattern_store(t, StoreMode::Assign);
        return;
      default:
        fail("invalid assignment target", n->line);
        return;
    }
  }
  const bool logical = n->op == Op::And || n->op == Op::Or || n->op == Op::Nullish;
  RefKind k = ref_prepare(t);
  int base_items = k == RefKind::Var ? 0 : k == RefKind::Prop ? 1 : 2;
  ref_get(k, t);
  if (!logical) {
    compile_expr(n->b);
    emit_binary_op(n->op);
    ref_put(k, t);
    return;
  }
  Label Lshort, Lend;
  jump(n->op == Op::And ? OpCode::JumpIfFalseKeep : n->op == Op::Or ? OpCode::JumpIfTrueKeep : OpCode::JumpIfNotNullishKeep, Lshort);
  compile_expr(n->b);
  ref_put(k, t);
  if (base_items == 0) {
    bind(Lshort);
    return;
  }
  jump(OpCode::Jump, Lend);
  bind(Lshort);
  // [operands.. v]: drop the operands under the value
  if (base_items == 1) {
    emit(OpCode::Swap);
    emit(OpCode::Pop);
  } else {
    emit(OpCode::Rot3);
    emit(OpCode::Pop);
    emit(OpCode::Swap);
    emit(OpCode::Pop);
  }
  bind(Lend);
}

void Compiler::compile_update(Node* n) {
  Node* t = n->a;
  const OpCode step = n->op == Op::Inc ? OpCode::Inc : OpCode::Dec;
  const bool prefix = n->has(kFlagPrefix);
  RefKind k = ref_prepare(t);
  ref_get(k, t);
  emit(OpCode::ToNumeric);
  if (prefix) {
    emit(step);
    ref_put(k, t);
    return;
  }
  if (k == RefKind::Var) {
    emit(OpCode::Dup);
    emit(step);
    store_var(t, StoreMode::Assign);
    return;
  }
  uint32_t tmp = alloc_reg();
  emit(OpCode::Dup);
  emit_u16(OpCode::SetLocal, static_cast<uint16_t>(tmp));
  emit(step);
  ref_put(k, t);
  emit(OpCode::Pop);
  emit_u16(OpCode::GetLocal, static_cast<uint16_t>(tmp));
}

// ---------------------------------------------------------------------------------------------------
// patterns
// ---------------------------------------------------------------------------------------------------

void Compiler::compile_default_value(Node* def, Node* target_for_name) {
  (void)target_for_name;
  Label Lskip;
  jump(OpCode::JumpIfNotUndefined, Lskip);
  compile_expr(def);
  bind(Lskip);
}

void Compiler::compile_pattern_store(Node* t, StoreMode mode) {
  if (failed()) return;
  switch (t->kind) {
    case NK::Ident: store_var(t, mode); return;
    case NK::AssignPattern:
      compile_default_value(t->b, t->a);
      compile_pattern_store(t->a, mode);
      return;
    case NK::ArrayPattern: compile_array_pattern(t, mode); return;
    case NK::ObjectPattern: compile_object_pattern(t, mode); return;
    case NK::Member:
      if (t->a->kind == NK::Super) {
        load_this(t->a);
        emit_u32(OpCode::Const, k_atom(t->str));
        emit(OpCode::Rot3);
        emit_u8(OpCode::SetSuperProp, strict_mode() ? 1 : 0);
        emit(OpCode::Pop);
        return;
      }
      compile_expr(t->a);
      if (t->b) {
        load_private_name(t->b);
        emit(OpCode::Rot3);
        emit(OpCode::PrivateSet);
      } else {
        emit(OpCode::Swap);
        emit_u32_u8(OpCode::SetProp, k_atom(t->str), strict_mode() ? 1 : 0);
      }
      emit(OpCode::Pop);
      return;
    case NK::Index:
      if (t->a->kind == NK::Super) {
        load_this(t->a);
        compile_expr(t->b);
        emit(OpCode::ToPropertyKey);
        emit(OpCode::Rot3);
        emit_u8(OpCode::SetSuperProp, strict_mode() ? 1 : 0);
        emit(OpCode::Pop);
        return;
      }
      compile_expr(t->a);
      compile_expr(t->b);
      emit(OpCode::Rot3);
      emit_u8(OpCode::SetElem, strict_mode() ? 1 : 0);
      emit(OpCode::Pop);
      return;
    default:
      fail("invalid destructuring target", t->line);
      emit(OpCode::Pop);
      return;
  }
}

void Compiler::compile_array_pattern(Node* pat, StoreMode mode) {
  uint32_t tmp = alloc_reg();
  alloc_reg();
  alloc_reg();
  alloc_reg();
  emit_u16(OpCode::IterInit, static_cast<uint16_t>(tmp));
  Rec rec;
  Label Lhandler, Lafter;
  const int env = fs->env_depth;
  rec_open(rec);
  for (Node* e : pat->list) {
    if (!e) {
      emit_u16(OpCode::IterNextValue, static_cast<uint16_t>(tmp));
      emit(OpCode::Pop);
      continue;
    }
    if (e->kind == NK::Rest) {
      emit_u16(OpCode::IterRest, static_cast<uint16_t>(tmp));
      compile_pattern_store(e->a, mode);
      continue;
    }
    emit_u16(OpCode::IterNextValue, static_cast<uint16_t>(tmp));
    compile_pattern_store(e, mode);
  }
  rec_close(rec);
  emit_u16(OpCode::IterClose, static_cast<uint16_t>(tmp));
  jump(OpCode::Jump, Lafter);
  bind(Lhandler);
  fs->depth = 1;
  uint32_t etmp = alloc_reg();
  emit_u16(OpCode::SetLocal, static_cast<uint16_t>(etmp));
  emit_u16(OpCode::IterCloseSilent, static_cast<uint16_t>(tmp));
  emit_u16(OpCode::GetLocal, static_cast<uint16_t>(etmp));
  emit(OpCode::Throw);
  add_handlers(rec, static_cast<uint32_t>(Lhandler.pos), env, true);
  bind(Lafter);
}

void Compiler::compile_object_pattern(Node* pat, StoreMode mode) {
  emit(OpCode::RequireObjectCoercible);
  uint32_t vr = alloc_reg();
  emit_u16(OpCode::SetLocal, static_cast<uint16_t>(vr));
  bool has_rest = false;
  for (Node* p : pat->list) if (p->kind == NK::Rest) has_rest = true;
  uint32_t ex = 0;
  if (has_rest) {
    ex = alloc_reg();
    emit_u32(OpCode::NewArray, 0);
    emit_u16(OpCode::SetLocal, static_cast<uint16_t>(ex));
  }
  for (Node* p : pat->list) {
    if (p->kind == NK::Rest) {
      emit_u16(OpCode::GetLocal, static_cast<uint16_t>(vr));
      emit_u16(OpCode::GetLocal, static_cast<uint16_t>(ex));
      emit(OpCode::CopyDataProps);
      compile_pattern_store(p->a, mode);
      continue;
    }
    emit_u16(OpCode::GetLocal, static_cast<uint16_t>(vr));
    if (p->has(kFlagComputed)) {
      compile_expr(p->a);
      emit(OpCode::ToPropertyKey);
      if (has_rest) {
        emit(OpCode::Dup);
        emit_u16(OpCode::GetLocal, static_cast<uint16_t>(ex));
        emit(OpCode::Swap);
        emit(OpCode::ArrayPush);
        emit(OpCode::Pop);
      }
      emit(OpCode::GetElem);
    } else {
      const std::u16string key = key_string(p->a);
      if (has_rest) {
        emit_u16(OpCode::GetLocal, static_cast<uint16_t>(ex));
        emit_u32(OpCode::Const, k_atom(key));
        emit(OpCode::ArrayPush);
        emit(OpCode::Pop);
      }
      const bool index_like = !key.empty() && key[0] >= u'0' && key[0] <= u'9';
      if (index_like) {
        emit_u32(OpCode::Const, k_atom(key));
        emit(OpCode::GetElem);
      } else {
        emit_u32(OpCode::GetProp, k_atom(key));
      }
    }
    compile_pattern_store(p->b, mode);
  }
}

}  // namespace aios::js
