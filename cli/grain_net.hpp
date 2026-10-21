#pragma once
// grain's network natives, kept apart from the language so that grain itself
// still has no idea a socket exists.
#include "aios/userland/grain.hpp"

namespace aios::cli {

void install_grain_net(userland::grain::Vm& vm);

}  // namespace aios::cli
