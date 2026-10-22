#pragma once
// Private interface between grain's compiler and its native-code subset
// (grain_native.cpp): `native fn` and `extern fn` declarations.
#include <memory>
#include <string>
#include <vector>

#include "aios/userland/grain.hpp"
#include "grain_lexer.hpp"

namespace aios::userland::grain {

// The native functions of one compilation: a later declaration can call an
// earlier one, and itself.
struct NativeModule;
std::shared_ptr<NativeModule> new_native_module();

// Compiles one declaration (the tokens from `native` or `extern` to its closing
// brace, or end of line for an extern). On success `name` is what to bind and
// `fn` the callable; on failure the diagnostics say why.
bool compile_native_decl(NativeModule& mod, const std::vector<Token>& toks, std::string& name, Value& fn,
                         std::vector<Diagnostic>& errors);

}  // namespace aios::userland::grain
