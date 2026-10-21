// Scope analysis: declare every binding, hoist `var` and function declarations, resolve each identifier to its
// binding, and mark the bindings an inner function captures (they live in a heap environment, the rest in
// registers).
#include "compiler.hpp"

namespace aios::js {

namespace {

std::string narrow(const std::u16string& s) {
  std::string o;
  for (char16_t c : s) o += c < 0x80 ? static_cast<char>(c) : '?';
  return o;
}

struct Analyzer {
  Program& prog;
  Analysis& an;
  Scope* cur = nullptr;
  struct Ref { Node* node; Scope* scope; Scope* params_of = nullptr; };   // params_of: the function whose parameter list holds the reference
  Scope* params_of_ = nullptr;
  std::vector<Ref> refs;
  bool failed = false;

  Analyzer(Program& p, Analysis& a) : prog(p), an(a) {}

  void fail(const std::string& msg, uint32_t line) {
    if (!failed) {
      failed = true;
      an.error = msg;
      an.error_line = line;
    }
  }

  Scope* new_scope(Scope::Kind k, Node* node, Scope* parent) {
    an.scopes.emplace_back();
    Scope* s = &an.scopes.back();
    s->kind = k;
    s->node = node;
    s->parent = parent;
    s->strict = parent ? parent->strict : prog.strict;
    if (k == Scope::Kind::Function || k == Scope::Kind::Script) s->func_scope = s;
    else s->func_scope = parent ? parent->func_scope : nullptr;
    return s;
  }

  VarInfo* make_var(Scope* s, const std::u16string& name, VK kind, uint32_t pos) {
    an.vars.emplace_back();
    VarInfo* v = &an.vars.back();
    v->name = name;
    v->kind = kind;
    v->scope = s;
    v->pos = pos;
    v->tdz = kind == VK::Let || kind == VK::Const || kind == VK::Class || kind == VK::ClassName || kind == VK::This;
    v->is_const = kind == VK::Const || kind == VK::ClassName;
    s->vars.push_back(v);
    s->map[name] = v;
    return v;
  }

  static bool is_lexical(VK k) { return k == VK::Let || k == VK::Const || k == VK::Class; }

  // Declare a lexical binding in `s` (the current block / loop / switch / class scope or the function scope).
  void declare_lexical(Scope* s, const std::u16string& name, VK kind, uint32_t pos, uint32_t line) {
    if (s->kind == Scope::Kind::Script) {
      for (auto& g : s->global_lexicals)
        if (g.first == name) { fail("Identifier '" + narrow(name) + "' has already been declared", line); return; }
      for (auto& g : s->global_vars)
        if (g == name) { fail("Identifier '" + narrow(name) + "' has already been declared", line); return; }
      s->global_lexicals.emplace_back(name, kind == VK::Const);
      return;
    }
    auto it = s->map.find(name);
    if (it != s->map.end()) { fail("Identifier '" + narrow(name) + "' has already been declared", line); return; }
    if (s->var_through.count(name)) { fail("Identifier '" + narrow(name) + "' has already been declared", line); return; }
    // `catch (e) { let e }`
    if (s->kind == Scope::Kind::Block && s->parent && s->parent->kind == Scope::Kind::Catch && s->parent->node && s->parent->map.count(name)) {
      fail("Identifier '" + narrow(name) + "' has already been declared", line);
      return;
    }
    // a parameter and a lexical declaration at the top level of a function body
    if (s->kind == Scope::Kind::Function) {
      if (s->map.count(name)) { fail("Identifier '" + narrow(name) + "' has already been declared", line); return; }
    }
    make_var(s, name, kind, pos);
  }

  // Declare a `var` binding (hoisted to the function scope).
  void declare_var(const std::u16string& name, uint32_t pos, uint32_t line, VK kind = VK::Var) {
    // walk from the current scope out to the function scope: no lexical binding of that name may be in the way
    for (Scope* s = cur;; s = s->parent) {
      if (s->kind == Scope::Kind::Script) {
        for (auto& g : s->global_lexicals)
          if (g.first == name) { fail("Identifier '" + narrow(name) + "' has already been declared", line); return; }
        break;
      }
      auto it = s->map.find(name);
      if (it != s->map.end()) {
        VarInfo* v = it->second;
        bool lexical = is_lexical(v->kind) || v->kind == VK::ClassName || (v->kind == VK::Function && s->kind != Scope::Kind::Function);
        if (s->kind == Scope::Kind::Catch && v->kind == VK::Catch) lexical = false;   // Annex B: var e in catch (e)
        if (lexical && s != cur->func_scope) { fail("Identifier '" + narrow(name) + "' has already been declared", line); return; }
        if (lexical && s == cur->func_scope) { fail("Identifier '" + narrow(name) + "' has already been declared", line); return; }
      }
      if (s->kind == Scope::Kind::Function) break;
      s->var_through.insert(name);
    }
    Scope* fs = cur->func_scope;
    if (fs->kind == Scope::Kind::Script) {
      for (auto& g : fs->global_vars)
        if (g == name) return;
      fs->global_vars.push_back(name);
      return;
    }
    auto it = fs->map.find(name);
    if (it != fs->map.end()) {
      if (kind == VK::Function) it->second->kind = VK::Function;
      return;
    }
    make_var(fs, name, kind, pos);
  }

  // Walk a binding pattern: declare each name (kind decides where) and register each identifier as a reference.
  void declare_pattern(Node* p, VK kind, Scope* target) {
    if (!p) return;
    switch (p->kind) {
      case NK::Ident:
        if (kind == VK::Var) declare_var(p->str, p->pos, p->line);
        else if (kind == VK::Param) {
          // a duplicate parameter name (sloppy) is the same binding
          if (!target->map.count(p->str)) make_var(target, p->str, VK::Param, p->pos);
        } else if (kind == VK::Catch) {
          if (target->map.count(p->str)) fail("Identifier '" + narrow(p->str) + "' has already been declared", p->line);
          else make_var(target, p->str, VK::Catch, p->pos);
        } else {
          declare_lexical(target, p->str, kind, p->pos, p->line);
        }
        refs.push_back({p, cur, params_of_});
        break;
      case NK::AssignPattern:
        declare_pattern(p->a, kind, target);
        visit(p->b);
        break;
      case NK::Rest:
        declare_pattern(p->a, kind, target);
        break;
      case NK::ArrayPattern:
        for (Node* e : p->list) declare_pattern(e, kind, target);
        break;
      case NK::ObjectPattern:
        for (Node* pr : p->list) {
          if (pr->kind == NK::Rest) { declare_pattern(pr->a, kind, target); continue; }
          if (pr->flags & kFlagComputed) visit(pr->a);
          declare_pattern(pr->b, kind, target);
        }
        break;
      default:
        visit(p);
        break;
    }
  }

  Scope* nearest_non_arrow_function(Scope* s) {
    for (; s; s = s->parent) {
      if (s->kind == Scope::Kind::Function && !s->is_arrow) return s;
      if (s->kind == Scope::Kind::Script) return s;
    }
    return nullptr;
  }

  // `this` / `super` in a derived constructor (or an arrow inside one) is a variable, so that super() can bind it
  // for every closure that sees it.
  void note_this_use(Node* n) {
    Scope* f = nearest_non_arrow_function(cur);
    if (!f || f->kind != Scope::Kind::Function || !f->fn) return;
    if (f->fn->kind != FuncKind::DerivedConstructor) return;
    if (!f->uses_this_var) {
      f->uses_this_var = true;
      make_var(f, u"this", VK::This, n->pos);
    }
    refs.push_back({n, cur, params_of_});
  }

  void visit_list(std::vector<Node*>& l) {
    for (Node* n : l) visit(n);
  }

  void visit_statements(std::vector<Node*>& l) {
    // function declarations are hoisted: declare them all first so that uses earlier in the block resolve
    for (Node* n : l)
      if (n && n->kind == NK::FunctionDecl) declare_function_decl(n);
    for (Node* n : l) {
      if (failed) return;
      if (n && n->kind == NK::FunctionDecl) visit_function(n, n->fn, false);
      else visit(n);
    }
  }

  void declare_function_decl(Node* n) {
    const std::u16string& name = n->fn->name;
    if (cur->kind == Scope::Kind::Script) {
      // a global function
      for (auto& g : cur->global_lexicals)
        if (g.first == name) { fail("Identifier '" + narrow(name) + "' has already been declared", n->line); return; }
      cur->global_funcs.push_back(n);
      bool have = false;
      for (auto& g : cur->global_vars) if (g == name) have = true;
      if (!have) cur->global_vars.push_back(name);
      return;
    }
    if (cur->kind == Scope::Kind::Function) {
      auto it = cur->map.find(name);
      if (it != cur->map.end()) {
        if (is_lexical(it->second->kind)) { fail("Identifier '" + narrow(name) + "' has already been declared", n->line); return; }
        it->second->kind = VK::Function;
      } else {
        make_var(cur, name, VK::Function, n->pos);
      }
      cur->hoisted_funcs.push_back(n);
      return;
    }
    // a function declared in a block: lexical to the block, and (sloppy mode, Annex B.3.3) also a var of the function
    auto it = cur->map.find(name);
    if (it != cur->map.end()) {
      if (cur->strict || is_lexical(it->second->kind) || it->second->kind != VK::Function) { fail("Identifier '" + narrow(name) + "' has already been declared", n->line); return; }
      cur->hoisted_funcs.push_back(n);
      return;
    }
    if (cur->var_through.count(name)) { fail("Identifier '" + narrow(name) + "' has already been declared", n->line); return; }
    VarInfo* v = make_var(cur, name, VK::Function, n->pos);
    v->from_block_function = true;
    cur->hoisted_funcs.push_back(n);
    if (!cur->strict) {
      // the function-scope var: only if that would not collide with a lexical binding on the way out
      bool clash = false;
      for (Scope* s = cur->parent; s; s = s->parent) {
        if (s->kind == Scope::Kind::Script) {
          for (auto& g : s->global_lexicals) if (g.first == name) clash = true;
          break;
        }
        auto f = s->map.find(name);
        if (f != s->map.end() && (is_lexical(f->second->kind) || (f->second->kind == VK::Function && s->kind != Scope::Kind::Function) || f->second->kind == VK::Param)) { clash = true; break; }
        if (s->kind == Scope::Kind::Function) break;
      }
      if (!clash) {
        Scope* fs = cur->func_scope;
        if (fs->kind == Scope::Kind::Script) {
          bool have = false;
          for (auto& g : fs->global_vars) if (g == name) have = true;
          if (!have) fs->global_vars.push_back(name);
        } else if (!fs->map.count(name)) {
          make_var(fs, name, VK::Var, n->pos);
        }
        n->flags |= kFlagDeclHoisted;   // the declaration also assigns the function-scope var when evaluated
      }
    }
  }

  void visit_function(Node* fnode, FuncInfo* fi, bool is_expr_like) {
    (void)is_expr_like;
    Scope* outer = cur;
    // a named function expression sees its own name through an outer scope owned by its own frame
    Scope* name_scope = nullptr;
    if ((fnode->kind == NK::FunctionExpr) && fi->is_expression && !fi->name.empty() && !fi->name_inferred && fi->kind == FuncKind::Normal) {
      name_scope = new_scope(Scope::Kind::Block, fnode, outer);
      VarInfo* v = make_var(name_scope, fi->name, VK::FuncName, fnode->pos);
      v->is_const = true;
      v->silent_const = !fi->strict;
      v->tdz = false;
      fnode->d = nullptr;
    }
    Scope* s = new_scope(Scope::Kind::Function, fnode, name_scope ? name_scope : outer);
    s->fn = fi;
    s->strict = fi->strict;
    s->is_arrow = fi->kind == FuncKind::Arrow;
    if (name_scope) {
      name_scope->func_scope = s;   // entered by the function's own frame
      name_scope->strict = fi->strict;
    }
    fnode->scope = s;
    Scope* saved = cur;
    cur = s;
    // parameters
    Scope* saved_params_of = params_of_;
    if (!fi->has_simple_params) params_of_ = s;   // references in default values and patterns do not see the body's own declarations
    for (Node* p : fi->params) declare_pattern(p, VK::Param, s);
    params_of_ = saved_params_of;
    // `arguments`
    if (!s->is_arrow && fi->uses_arguments && fi->kind != FuncKind::ClassFieldInit) {
      if (!s->map.count(u"arguments")) make_var(s, u"arguments", VK::Arguments, fnode->pos);
    }
    const size_t body_vars_from = s->vars.size();
    // body
    if (fi->expression_body) {
      visit(fi->body);
    } else if (fi->body) {
      visit_statements(fi->body->list);
    }
    if (!fi->has_simple_params)
      for (size_t i = body_vars_from; i < s->vars.size(); ++i) s->vars[i]->body_only = true;
    cur = saved;
  }

  void visit_class(Node* n) {
    Scope* s = new_scope(Scope::Kind::Class, n, cur);
    s->strict = true;
    n->scope = s;
    Scope* saved = cur;
    cur = s;
    if (!n->str.empty()) {
      VarInfo* v = make_var(s, n->str, VK::ClassName, n->pos);
      v->is_const = true;
    }
    // private names this class declares
    for (Node* m : n->list) {
      if (m->kind != NK::Property || !(m->flags & kFlagPrivate)) continue;
      std::u16string nm = u"#" + m->a->str;
      if (!s->map.count(nm)) make_var(s, nm, VK::Private, m->pos);
    }
    if (n->a) visit(n->a);
    bool has_ctor = false;
    for (Node* m : n->list) {
      if (m->kind == NK::Property && (m->flags & kFlagProto) && (m->flags & kFlagMethod)) has_ctor = true;
    }
    if (!has_ctor) {
      // a synthesised constructor: base `constructor() {}`, derived `constructor(...args) { super(...args) }`
      FuncInfo* fi = prog.make_func();
      fi->kind = n->a ? FuncKind::DerivedConstructor : FuncKind::ClassConstructor;
      fi->strict = true;
      fi->src_start = n->fn ? n->fn->src_start : 0;
      fi->src_end = n->fn ? n->fn->src_end : 0;
      fi->name = n->str.empty() ? n->str2 : n->str;
      Node* body = prog.make(NK::Block, n->pos, n->line);
      if (n->a) {
        Node* rest = prog.make(NK::Rest, n->pos, n->line);
        Node* id = prog.make(NK::Ident, n->pos, n->line);
        id->str = u"args";
        rest->a = id;
        fi->params.push_back(rest);
        fi->has_simple_params = false;
        Node* call = prog.make(NK::Call, n->pos, n->line);
        call->a = prog.make(NK::Super, n->pos, n->line);
        Node* spread = prog.make(NK::Spread, n->pos, n->line);
        Node* id2 = prog.make(NK::Ident, n->pos, n->line);
        id2->str = u"args";
        spread->a = id2;
        call->list.push_back(spread);
        Node* es = prog.make(NK::ExprStmt, n->pos, n->line);
        es->a = call;
        body->list.push_back(es);
      }
      fi->body = body;
      Node* f = prog.make(NK::FunctionExpr, n->pos, n->line);
      f->fn = fi;
      Node* m = prog.make(NK::Property, n->pos, n->line);
      m->a = prog.make(NK::String, n->pos, n->line);
      m->a->str = u"constructor";
      m->b = f;
      m->flags |= kFlagMethod | kFlagProto;
      n->list.insert(n->list.begin(), m);
    }
    for (Node* m : n->list) {
      if (failed) break;
      if (m->kind == NK::StaticBlock) {
        Node* f = prog.make(NK::FunctionExpr, m->pos, m->line);
        f->fn = m->fn;
        m->a = f;
        visit_function(f, m->fn, false);
        continue;
      }
      if ((m->flags & kFlagComputed)) visit(m->a);
      if (m->b) {
        // methods, accessors and field initialisers are all functions of the class
        m->b->fn->home_class = n;
        m->b->fn->name = m->b->fn->name;
        visit_function(m->b, m->b->fn, false);
      }
    }
    cur = saved;
  }

  void visit(Node* n) {
    if (!n || failed) return;
    switch (n->kind) {
      case NK::Ident:
        refs.push_back({n, cur, params_of_});
        return;
      case NK::PrivateName:
        refs.push_back({n, cur, params_of_});
        return;
      case NK::This:
        note_this_use(n);
        return;
      case NK::Super:
        note_this_use(n);
        return;
      case NK::FunctionExpr:
      case NK::ArrowFunction:
        visit_function(n, n->fn, true);
        return;
      case NK::FunctionDecl:
        // reached only when a declaration sits in a position that did not hoist it (e.g. a labelled declaration)
        declare_function_decl(n);
        visit_function(n, n->fn, false);
        return;
      case NK::ClassExpr:
        visit_class(n);
        return;
      case NK::ClassDecl: {
        declare_lexical(cur, n->str, VK::Class, n->pos, n->line);
        // the outer binding is initialised by the declaration; the class body has its own inner binding
        Node* id = prog.make(NK::Ident, n->pos, n->line);
        id->str = n->str;
        n->b = id;
        refs.push_back({id, cur, params_of_});
        visit_class(n);
        return;
      }
      case NK::Program:
        return;   // handled by the caller
      case NK::VarDecl: {
        VK kind = (n->flags & kFlagVar) ? VK::Var : (n->flags & kFlagConst) ? VK::Const : VK::Let;
        for (Node* d : n->list) {
          declare_pattern(d->a, kind, cur->kind == Scope::Kind::Script || kind == VK::Var ? cur : cur);
          visit(d->b);
        }
        return;
      }
      case NK::Block: {
        Scope* s = new_scope(Scope::Kind::Block, n, cur);
        n->scope = s;
        Scope* saved = cur;
        cur = s;
        visit_statements(n->list);
        cur = saved;
        return;
      }
      case NK::For: {
        Scope* s = new_scope(Scope::Kind::For, n, cur);
        n->scope = s;
        Scope* saved = cur;
        cur = s;
        visit(n->a);
        visit(n->b);
        visit(n->c);
        visit(n->d);
        cur = saved;
        return;
      }
      case NK::ForIn: case NK::ForOf: {
        Scope* s = new_scope(Scope::Kind::For, n, cur);
        n->scope = s;
        Scope* saved = cur;
        cur = s;
        // the right-hand side is evaluated with the loop's bindings in TDZ
        visit(n->b);
        if (n->a && n->a->kind == NK::VarDecl) {
          visit(n->a);
        } else {
          visit(n->a);
        }
        visit(n->c);
        cur = saved;
        return;
      }
      case NK::Switch: {
        visit(n->a);
        Scope* s = new_scope(Scope::Kind::Switch, n, cur);
        n->scope = s;
        Scope* saved = cur;
        cur = s;
        // declarations across all the cases share one scope
        std::vector<Node*> all;
        for (Node* c : n->list) for (Node* st : c->list) all.push_back(st);
        for (Node* st : all) if (st && st->kind == NK::FunctionDecl) declare_function_decl(st);
        for (Node* c : n->list) {
          visit(c->a);
          for (Node* st : c->list) {
            if (st && st->kind == NK::FunctionDecl) visit_function(st, st->fn, false);
            else visit(st);
          }
        }
        cur = saved;
        return;
      }
      case NK::Try: {
        visit(n->a);
        if (n->has(kFlagHasCatch)) {
          Scope* s = new_scope(Scope::Kind::Catch, n, cur);
          Scope* saved = cur;
          cur = s;
          n->scope = s;
          if (n->b) declare_pattern(n->b, VK::Catch, s);
          visit(n->c);
          cur = saved;
        }
        visit(n->d);
        return;
      }
      case NK::Labeled:
        visit(n->a);
        return;
      case NK::Property:
        if (n->flags & kFlagSpread) { visit(n->a); return; }
        if (n->flags & kFlagComputed) visit(n->a);
        visit(n->b);
        return;
      case NK::Member:
        visit(n->a);
        if (n->b) visit(n->b);
        return;
      case NK::Call:
        visit(n->a);
        visit_list(n->list);
        return;
      case NK::Template: case NK::ArrayLit: case NK::ArrayPattern: case NK::Sequence: case NK::New: case NK::Case:
        visit(n->a);
        visit_list(n->list);
        return;
      case NK::ObjectLit: case NK::ObjectPattern:
        visit_list(n->list);
        return;
      case NK::TaggedTemplate:
        visit(n->a);
        visit(n->b);
        return;
      default:
        visit(n->a);
        visit(n->b);
        visit(n->c);
        visit(n->d);
        visit_list(n->list);
        return;
    }
  }

  // ------------------------------------------------------------------ resolution

  void resolve_all() {
    for (Ref& r : refs) {
      Node* n = r.node;
      std::u16string name;
      if (n->kind == NK::This || n->kind == NK::Super) name = u"this";
      else if (n->kind == NK::PrivateName) name = u"#" + n->str;
      else name = n->str;
      VarInfo* found = nullptr;
      Scope* ref_func = r.scope->func_scope;
      for (Scope* s = r.scope; s; s = s->parent) {
        auto it = s->map.find(name);
        if (it != s->map.end()) {
          if (it->second->body_only && r.params_of == s) continue;   // a parameter expression does not see the body's declarations
          found = it->second;
          break;
        }
      }
      if (found) {
        if (found->scope->func_scope != ref_func) found->captured = true;
        n->var = found;
      }
    }
    // storage: captured bindings share their scope's Env
    for (Scope& s : an.scopes) {
      for (VarInfo* v : s.vars) {
        if (v->captured) {
          v->in_env = true;
          v->slot = s.env_size++;
        }
      }
      s.materialized = s.env_size > 0;
    }
  }
};

}  // namespace

bool analyze_program(Program& prog, Analysis& out) {
  Analyzer a(prog, out);
  Scope* s = a.new_scope(Scope::Kind::Script, prog.root, nullptr);
  s->strict = prog.strict;
  out.script = s;
  a.cur = s;
  prog.root->scope = s;
  a.visit_statements(prog.root->list);
  if (a.failed) return false;
  a.resolve_all();
  return !a.failed;
}

}  // namespace aios::js
