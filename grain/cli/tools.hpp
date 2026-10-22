#pragma once
// The grain program's developer tools: fmt (tools_fmt.cpp), test
// (tools_test.cpp) and lsp (tools_lsp.cpp). Each takes the arguments after its
// subcommand and returns the exit status.
#include <string>
#include <vector>

#include "aios/userland/grain.hpp"

namespace grain_cli {

int fmt_command(const std::vector<std::string>& args);
int test_command(const std::vector<std::string>& args);
int lsp_command(const std::vector<std::string>& args);

// Sets a VM up as `grain run` does (main.cpp), for the tools that run programs.
void prepare_vm(aios::userland::grain::Vm& vm, const std::vector<std::string>& script_args);

// The text reindented by its brackets (two spaces a level), trailing spaces
// gone, at most two blank lines in a row, one newline at the end. Strings and
// comments are never touched inside. Exposed for the language server.
std::string format_source(const std::string& text);

}  // namespace grain_cli
