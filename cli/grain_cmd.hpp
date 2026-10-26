#pragma once
// The seam between the CLI and grain. Declared apart from commands.cpp so the
// language can be driven from anywhere that has a kernel and somewhere to
// write, including the desktop and a test, not only the shell.
#include <iostream>
#include <string>
#include <vector>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/grain.hpp"
#include "out.hpp"

namespace aios::cli {

Result<void> run_grain_command(Kernel& k, Out& o, const std::vector<std::string>& a);
// A hosted app's VM, as aios sets one up (`aios serve`, grain_app.hpp).
void setup_app_vm(Kernel& k, userland::grain::Vm& vm);

}  // namespace aios::cli
