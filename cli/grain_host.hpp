// Host-only grain natives (sockets, sys_exec) - see grain_host.cpp for why
// they are not part of every VM.
#pragma once

namespace aios::userland::grain { class Vm; }

namespace aios::cli {
void install_grain_host(userland::grain::Vm& vm);
}
