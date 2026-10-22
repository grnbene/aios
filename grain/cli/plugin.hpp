#pragma once
// Plugins written in C or C++ (plugin.cpp, include/grain_plugin.h): load_plugin(path)
// opens a shared library and lets it register natives. Standalone grain only.
#include "aios/userland/grain.hpp"

namespace grain_cli {

void install_plugins(aios::userland::grain::Vm& vm);

}  // namespace grain_cli
