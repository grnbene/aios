#pragma once
// The display the standalone grain shows panes on, on Windows: the Win32 API
// itself (user32, gdi32 - part of the operating system), so no library is needed.
#include "aios/userland/grain.hpp"

namespace grain_cli {

aios::userland::grain::Display& win32_display();

}  // namespace grain_cli
