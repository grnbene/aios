#pragma once
// The syntax tree the parser builds and the compiler walks. Nodes live in an arena owned by the Program.
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace aios::js {

enum class NK : uint8_t {
  // expressions
  Number, BigIntLit, String, Template, TaggedTemplate, RegExpLit, Ident, This, Super, Null, Bool,
  ArrayLit, ObjectLit, Property, FunctionExpr, ArrowFunction, ClassExpr,
  Member, Index, Call, New, OptChain,
  Unary, Update, Binary, Logical, Assign, Conditional, Sequence, Spread, Yield, Await,
  NewTarget, PrivateName, PrivateIn,
  // patterns
  ArrayPattern, ObjectPattern, AssignPattern, Rest,
  // statements
  VarDecl, Declarator, FunctionDecl, ClassDecl, Return, If, For, ForIn, ForOf, While, DoWhile,
  Break, Continue, Throw, Try, Switch, Case, Labeled, Block, ExprStmt, Empty, Debugger, With,
  StaticBlock, Program,
};

// Operators (Unary, Binary, Logical, Assign, Update). Stored in Node::op.
enum class Op : uint8_t {
  None,
  // unary
  Neg, Pos, Not, BitNot, TypeOf, Void, Delete,
  // update
  Inc, Dec,
  // binary
  Add, Sub, Mul, Div, Mod, Exp, Shl, Shr, UShr, BitAnd, BitOr, BitXor,
  Eq, Ne, StrictEq, StrictNe, Lt, Gt, Le, Ge, In, InstanceOf,
  // logical
  And, Or, Nullish,
  // assignment (Assign holds the binary op it combines with in `op2`; Plain means =)
  Plain,
};

enum NodeFlags : uint32_t {
  kFlagComputed = 1u << 0,       // Property/ClassMember/Member key is [expr]
  kFlagShorthand = 1u << 1,      // { a } in an object literal
  kFlagMethod = 1u << 2,
  kFlagGetter = 1u << 3,
  kFlagSetter = 1u << 4,
  kFlagStatic = 1u << 5,
  kFlagSpread = 1u << 6,         // { ...x } in an object literal
  kFlagPrefix = 1u << 7,         // ++x
  kFlagOptional = 1u << 8,       // a?.b, a?.(), a?.[b]
  kFlagDelegate = 1u << 9,       // yield*
  kFlagAwait = 1u << 10,         // for await
  kFlagParenthesized = 1u << 11,
  kFlagField = 1u << 12,         // class field
  kFlagHasCatch = 1u << 13,
  kFlagVar = 1u << 14,           // VarDecl kinds
  kFlagLet = 1u << 15,
  kFlagConst = 1u << 16,
  kFlagProto = 1u << 17,         // { __proto__: x } sets the prototype
  kFlagDefaultCase = 1u << 18,
  kFlagHasDefaultInit = 1u << 19,// {a = 1} in an object pattern shorthand
  kFlagComputedKeyDone = 1u << 20,
  kFlagUseStrict = 1u << 21,
  kFlagTrailingComma = 1u << 22,
  kFlagPrivate = 1u << 23,       // #name class member
  kFlagLegacyOctal = 1u << 24,
  kFlagAnonymousFn = 1u << 25,   // a function/class expression with no name of its own
  kFlagDeclHoisted = 1u << 26,
};

struct Node;
struct FuncInfo;
struct Scope;
struct VarInfo;

struct TemplateData {
  std::vector<std::u16string> cooked;   // per quasi; valid only where cooked_ok
  std::vector<std::u16string> raw;
  std::vector<bool> cooked_ok;
};

struct Node {
  NK kind;
  Op op = Op::None;
  Op op2 = Op::None;                // the binary operator of a compound assignment
  uint32_t flags = 0;
  uint32_t pos = 0;                 // source offset of the node's first token
  uint32_t line = 0;
  double num = 0;
  Node* a = nullptr;
  Node* b = nullptr;
  Node* c = nullptr;
  Node* d = nullptr;
  std::vector<Node*> list;
  std::u16string str;               // identifier / string value / label / regexp pattern
  std::u16string str2;              // regexp flags
  FuncInfo* fn = nullptr;           // FunctionExpr / ArrowFunction / FunctionDecl / the constructor of a class
  TemplateData* tpl = nullptr;
  // filled by the analyser
  Scope* scope = nullptr;           // the scope this node opens (function, block, catch, class, for)
  VarInfo* var = nullptr;           // for Ident: the binding it resolves to (null: global)
  explicit Node(NK k) : kind(k) {}
  bool has(uint32_t f) const { return (flags & f) != 0; }
};

enum class FuncKind : uint8_t { Normal, Arrow, Method, Getter, Setter, ClassConstructor, DerivedConstructor, ClassFieldInit };

struct FuncInfo {
  std::u16string name;
  FuncKind kind = FuncKind::Normal;
  bool is_async = false;
  bool is_generator = false;
  bool strict = false;
  bool expression_body = false;      // arrow with a concise body: `body` is an expression
  bool has_simple_params = true;     // only plain identifiers, no defaults/rest/patterns
  bool is_expression = false;        // a function expression (its own name is visible inside)
  bool name_inferred = false;        // the name came from the binding it was assigned to: not visible inside
  bool is_class_static_block = false;
  bool uses_arguments = false;
  bool uses_this = false;            // this / super / new.target inside (arrows propagate to the enclosing function)
  bool has_direct_eval = false;
  std::vector<Node*> params;         // Ident / patterns / AssignPattern / Rest
  Node* body = nullptr;              // a Block, or an expression for a concise arrow
  uint32_t src_start = 0, src_end = 0;
  uint32_t line = 0;
  uint32_t length = 0;               // the `length` property: params before the first default or rest
  Node* home_class = nullptr;
};

struct Program {
  std::deque<Node> nodes;
  std::deque<FuncInfo> funcs;
  std::deque<TemplateData> templates;
  Node* root = nullptr;
  std::shared_ptr<const std::u16string> source;
  bool strict = false;
  Node* make(NK k, uint32_t pos, uint32_t line) {
    nodes.emplace_back(k);
    Node* n = &nodes.back();
    n->pos = pos;
    n->line = line;
    return n;
  }
  FuncInfo* make_func() { funcs.emplace_back(); return &funcs.back(); }
  TemplateData* make_template() { templates.emplace_back(); return &templates.back(); }
};

struct ParseError {
  std::string message;
  uint32_t line = 0, col = 0;
};

enum class ParseMode : uint8_t { Script, FunctionBody /* new Function */, Eval };

struct ParseOptions {
  bool strict = false;               // start in strict mode
  bool in_function = false;          // allow `return`, `new.target` (eval inside a function)
  bool allow_super_property = false;
  bool allow_super_call = false;
  bool allow_new_target = false;
};

// Parse `source` into `out` (out.root set on success). On a syntax error, `err` describes it and false is returned.
bool parse_program(const std::shared_ptr<const std::u16string>& source, const ParseOptions& opt, Program& out, ParseError* err);
// Parse the parameter list and the body of a Function(...) call, as one function expression.
bool parse_function_source(const std::shared_ptr<const std::u16string>& source, const std::u16string& params, const std::u16string& body, bool is_async,
                           bool is_generator, Program& out, ParseError* err);
// Parse a regular-expression body for early errors.
}  // namespace aios::js
