#pragma once
// Bytecode for the JavaScript VM, and the compiled-function record.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aios/js/js.hpp"

namespace aios::js {

// Operand formats, for the disassembler and the encoder: u8, u16, u32, i32 (a relative jump, from the end of the instruction).
#define AIOS_JS_OPCODES(X)                                                                                         \
  X(Nop, "")                                                                                                        \
  X(Pop, "")                                                                                                        \
  X(Dup, "")                                                                                                        \
  X(Dup2, "")                                                                                                       \
  X(Swap, "")                                                                                                       \
  X(Rot3, "")      /* a b c -> b c a (the bottom item to the top) */                                                                              \
  X(Rot4, "")      /* a b c d -> b c d a */                                                                          \
  X(Undefined, "")                                                                                                  \
  X(Null, "")                                                                                                       \
  X(True, "")                                                                                                       \
  X(False, "")                                                                                                      \
  X(Int, "i")                                                                                                       \
  X(Const, "I")                                                                                                     \
  X(GetLocal, "H")                                                                                                  \
  X(SetLocal, "H")        /* pops */                                                                                \
  X(GetLocalChecked, "HI")                                                                                          \
  X(SetLocalChecked, "HI")                                                                                          \
  X(SetLocalEmpty, "H")                                                                                             \
  X(GetEnv, "HH")                                                                                                   \
  X(SetEnv, "HH")                                                                                                   \
  X(GetEnvChecked, "HHI")                                                                                           \
  X(SetEnvChecked, "HHI")                                                                                           \
  X(SetEnvEmpty, "HH")                                                                                              \
  X(PushEnv, "H")                                                                                                   \
  X(PopEnv, "")                                                                                                     \
  X(CopyEnv, "")                                                                                                    \
  X(GetGlobal, "I")                                                                                                 \
  X(SetGlobal, "IB")                                                                                                \
  X(TypeofGlobal, "I")                                                                                              \
  X(DeclareGlobalVar, "I")                                                                                          \
  X(DeclareGlobalFunc, "I")  /* pops the function */                                                               \
  X(DeclareGlobalLex, "IB")                                                                                         \
  X(InitGlobalLex, "I")                                                                                             \
  X(GetProp, "I")                                                                                                   \
  X(SetProp, "IB")        /* [obj val] -> [val]; B = strict */                                                      \
  X(GetElem, "")                                                                                                    \
  X(SetElem, "B")         /* [obj key val] -> [val] */                                                              \
  X(DeleteProp, "IB")                                                                                               \
  X(DeleteElem, "B")                                                                                                \
  X(GetSuperProp, "")     /* [this key] -> [value] (home object from the frame) */                             \
  X(SetSuperProp, "B")    /* [this key val] -> [val] */                                                                  \
  X(In, "")                                                                                                         \
  X(InstanceOf, "")                                                                                                 \
  X(NewObject, "")                                                                                                  \
  X(NewArray, "I")                                                                                                  \
  X(ArrayPush, "")                                                                                                  \
  X(ArrayHole, "")                                                                                                  \
  X(ArraySpread, "")                                                                                                \
  X(ObjectSpread, "")                                                                                               \
  X(SetProto, "")         /* [obj proto] -> [obj] */                                                                \
  X(DefineField, "I")     /* [obj val] -> [obj] */                                                                  \
  X(DefineFieldComputed, "")  /* [obj key val] -> [obj] */                                                          \
  X(DefineMethod, "IB")   /* [obj fn] -> [obj]; B bit0 = enumerable */                                              \
  X(DefineMethodComputed, "B")                                                                                      \
  X(DefineGetter, "IB")                                                                                             \
  X(DefineGetterComputed, "B")                                                                                      \
  X(DefineSetter, "IB")                                                                                             \
  X(DefineSetterComputed, "B")                                                                                      \
  X(SetHomeObject, "B")   /* [obj (key) fn] -> same; B = 1 when a key sits between */                                                                \
  X(NameFunction, "B")    /* [key fn] -> [key fn]; names an anonymous fn from key; B: 0 plain 1 get 2 set */       \
  X(Closure, "I")                                                                                                   \
  X(CreateClass, "IBHH")  /* [super?] -> []; funcIdx, hasSuper, ctor tmp, proto tmp */                              \
  X(ClassFields, "H")     /* [fn] -> []; installs the field initialiser on the ctor in tmp */                       \
  X(NewPrivateName, "I")  /* pushes a fresh private name (description const) */                                     \
  X(PrivateGet, "")       /* [obj name] -> [val] */                                                                 \
  X(PrivateSet, "")       /* [obj name val] -> [val] */                                                             \
  X(PrivateDefine, "B")   /* [obj name val] -> [] ; B: 0 field 1 method 2 getter 3 setter */                        \
  X(PrivateIn, "")        /* [name obj] -> [bool] */                                                                \
  X(Call, "H")                                                                                                      \
  X(CallSpread, "")                                                                                                 \
  X(New, "H")                                                                                                       \
  X(NewSpread, "")                                                                                                  \
  X(SuperCall, "H")                                                                                                 \
  X(SuperCallSpread, "")                                                                                            \
  X(Return, "")                                                                                                     \
  X(Throw, "")                                                                                                      \
  X(ThrowError, "BI")     /* kind, message const */                                                                 \
  X(ThrowConstAssign, "I")                                                                                          \
  X(Jump, "i")                                                                                                      \
  X(JumpIfFalse, "i")                                                                                               \
  X(JumpIfTrue, "i")                                                                                                \
  X(JumpIfFalseKeep, "i") /* if falsy: jump keeping the value; else pop */                                          \
  X(JumpIfTrueKeep, "i")                                                                                            \
  X(JumpIfNotNullishKeep, "i")                                                                                      \
  X(JumpIfNullish, "i")   /* pops; jumps if the value was null or undefined */                                                            \
  X(JumpIfNotUndefined, "i") /* if not undefined: jump keeping it; else pop (default values) */                                               \
  X(Add, "") X(Sub, "") X(Mul, "") X(Div, "") X(Mod, "") X(Exp, "")                                                \
  X(Shl, "") X(Shr, "") X(UShr, "") X(BitAnd, "") X(BitOr, "") X(BitXor, "")                                        \
  X(Eq, "") X(Ne, "") X(StrictEq, "") X(StrictNe, "") X(Lt, "") X(Gt, "") X(Le, "") X(Ge, "")                       \
  X(Neg, "") X(Pos, "") X(Not, "") X(BitNot, "") X(TypeOf, "") X(Inc, "") X(Dec, "")                               \
  X(ToNumeric, "") X(ToPropertyKey, "") X(ToString, "")                                                             \
  X(LoadThis, "") X(LoadThisChecked, "") X(LoadNewTarget, "") X(LoadFunction, "") X(LoadHome, "")                   \
  X(InitThisLocal, "H")   /* [obj] -> []: binds this after super(); ReferenceError if already bound */              \
  X(InitThisEnv, "HH")                                                                                              \
  X(RunFieldInit, "")     /* [obj] -> [obj]: runs the field initialisers of the active constructor on obj */                                                                                                       \
  X(ReturnCtor, "")       /* [value this?] derived-constructor return */                                            \
  X(GlobalObject, "")                                                                                               \
  X(CreateArguments, "") X(CreateRest, "H")                                                                         \
  X(GetTemplateObject, "I") X(NewRegExp, "II")                                                                      \
  X(RequireObjectCoercible, "")                                                                                     \
  X(CopyDataProps, "")    /* [src excludedKeysArray] -> [newObject] */                                              \
  X(IterInit, "H")        /* [iterable] -> [], tmp..tmp+2 = iterator, next, done */                                 \
  X(AsyncIterInit, "H")                                                                                             \
  X(IterNext, "Hi")       /* push next value, or jump when done */                                                  \
  X(IterNextValue, "H")   /* destructuring: value or undefined when exhausted */                                    \
  X(IterRest, "H")        /* push an array of the rest */                                                           \
  X(IterClose, "H")       /* call return() if not done; errors propagate */                                         \
  X(IterCloseSilent, "H") /* same, ignoring any error (used while an exception is pending) */                       \
  X(IterCallNext, "H")    /* push the raw next() result (for await) */                                              \
  X(IterResult, "Hi")     /* [result] -> value, or jump when done */                                                \
  X(ForInInit, "H")                                                                                                 \
  X(ForInNext, "Hi")                                                                                                \
  X(InitialYield, "")                                                                                               \
  X(Yield, "")            /* [value] -> [sent value] */                                                             \
  X(YieldStar, "H")       /* yield* with the iterator record in tmp */                                              \
  X(Await, "")                                                                                                      \
  X(AwaitIfSync, "H")     /* for await over a sync iterable: the value is awaited too (async-from-sync) */          \
  X(AsyncGenYield, "")                                                                                              \
  X(Debugger, "")                                                                                                   \
  X(Void, "")             /* pop, push undefined */                                                                 \
  X(CheckPrivateBrand, "")                                                                                          \
  X(End, "")

enum class OpCode : uint8_t {
#define X(n, f) n,
  AIOS_JS_OPCODES(X)
#undef X
  Count
};

const char* opcode_name(OpCode op);
const char* opcode_format(OpCode op);

struct Handler {
  uint32_t start = 0, end = 0;     // pc range protected
  uint32_t target = 0;             // where to continue; the exception is pushed
  uint16_t env_depth = 0;          // environments to unwind to
  bool is_finally = false;         // run for a generator return() as well as for throws
};

struct LineEntry {
  uint32_t pc;
  uint32_t line;
};

struct FunctionCode final : Cell {
  FunctionCode() : Cell(CellKind::Code) {}
  std::vector<uint8_t> code;
  std::vector<Value> consts;
  std::vector<FunctionCode*> functions;     // nested function templates
  std::vector<Handler> handlers;
  std::vector<LineEntry> lines;
  std::vector<std::pair<uint32_t, uint32_t>> call_sites;   // pc after a Call -> consts index of the callee's text, for "a.b is not a function"
  JsString* name = nullptr;
  uint32_t nparams = 0;                     // declared parameters (for `length` see `length`)
  uint32_t length = 0;
  uint32_t nlocals = 0;                     // frame registers, parameters first
  uint32_t max_stack = 0;
  bool strict = false;
  bool is_arrow = false;
  bool is_async = false;
  bool is_generator = false;
  bool is_method = false;
  bool is_class_constructor = false;
  bool is_derived = false;
  bool is_getter_or_setter = false;
  bool uses_arguments = false;
  bool has_rest = false;
  bool has_simple_params = true;
  bool is_script = false;
  bool needs_home = false;
  std::shared_ptr<const std::u16string> source;
  uint32_t src_start = 0, src_end = 0;      // Function.prototype.toString
  uint32_t line = 0;
  JsString* filename = nullptr;
  // Template objects are created once per site.
  struct TemplateSite { std::vector<std::u16string> cooked; std::vector<std::u16string> raw; std::vector<bool> ok; Object* object = nullptr; };
  std::vector<TemplateSite> templates;
  struct RegexSite { JsString* pattern; JsString* flags; };

  void trace(Tracer& t) override;
  size_t extra_bytes() const override { return code.capacity() + consts.capacity() * sizeof(Value); }
};

}  // namespace aios::js
