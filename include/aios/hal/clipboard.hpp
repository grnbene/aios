#pragma once
// The clipboard belongs to whoever owns the display. On a hosted substrate that
// is the host's window system, and the machine must share ITS clipboard or
// copying out of this desktop into anything else is impossible. On bare metal
// there is no other program to share with, so the machine keeps its own.
#include <string>
#include "aios/core/result.hpp"

namespace aios::hal {

Result<std::string> clipboard_get();
Result<void> clipboard_set(const std::string& text);

}  // namespace aios::hal
