#pragma once
// Scope analysis and code generation: from the parser's tree to FunctionCode.
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.hpp"
#include "bytecode.hpp"

namespace aios::js {

enum class VK : uint8_t { Var, Let, Const, Param, Function, Class, Catch, FuncName, Arguments, This, ClassName, Private, Completion };

struct VarInfo {
  std::u16string name;
  VK kind = VK::Var;
  Scope* scope = nullptr;
  bool captured = false;        // an inner function refers to it: it lives in a heap Env
  bool tdz = false;             // let / const / class: reads before initialisation throw
  bool is_const = false;        // assignment throws (a named function expression's own name: ignored when sloppy)
  bool silent_const = false;    // FuncName in sloppy mode
  bool in_env = false;
  int slot = -1;                // register index, or slot in the scope's Env
  uint32_t pos = 0;
  bool from_block_function = false;   // Annex B: a block-level function declaration
  bool body_only = false;       // declared in the body of a function with parameter expressions: invisible to them
};

struct Scope {
  enum class Kind : uint8_t { Script, Function, Block, Catch, Class, For, Switch };
  Kind kind = Kind::Block;
  Scope* parent = nullptr;
  Scope* func_scope = nullptr;      // the nearest enclosing Function or Script scope (this one, if it is)
  Node* node = nullptr;
  FuncInfo* fn = nullptr;           // for Function scopes
  bool strict = false;
  std::vector<VarInfo*> vars;
  std::unordered_map<std::u16string, VarInfo*> map;
  std::unordered_set<std::u16string> var_through;   // names of `var` declarations hoisted through this block
  std::vector<Node*> hoisted_funcs; // function declarations initialised at scope entry
  int env_size = 0;
  bool materialized = false;        // has an Env at run time
  bool is_arrow = false;
  bool uses_this_var = false;       // `this` is a variable here (a derived constructor)
  bool has_eval = false;
  // For Script scopes: the global declarations.
  std::vector<std::u16string> global_vars;
  std::vector<Node*> global_funcs;
  std::vector<std::pair<std::u16string, bool>> global_lexicals;   // name, is_const
};

struct Analysis {
  std::deque<Scope> scopes;
  std::deque<VarInfo> vars;
  std::string error;
  uint32_t error_line = 0;
  Scope* script = nullptr;
};

bool analyze_program(Program& prog, Analysis& out);

}  // namespace aios::js

namespace aios::js {

class Context;
// Compile a parsed and analysed program into script code (a FunctionCode that returns the completion value).
// On failure returns nullptr with `err` set (an early error the analyser found counts).
FunctionCode* compile_program(Context& ctx, Program& prog, Analysis& an, const std::string& filename, std::string* err, uint32_t* err_line);

}  // namespace aios::js
