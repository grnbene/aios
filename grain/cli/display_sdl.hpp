#pragma once
// The SDL2 display the standalone grain shows panes on (display_sdl.cpp).
// Only built when CMake found SDL2 (GRAIN_HAVE_SDL2).
#include "aios/userland/grain.hpp"

namespace grain_cli {

aios::userland::grain::Display& sdl_display();

}  // namespace grain_cli
