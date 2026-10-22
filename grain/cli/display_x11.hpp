#pragma once
// The display the standalone grain shows panes on, on Linux: the X11 protocol
// spoken directly (display_x11.cpp), so no library is needed.
#include "aios/userland/grain.hpp"

namespace grain_cli {

aios::userland::grain::Display& x11_display();

}  // namespace grain_cli
