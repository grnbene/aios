#pragma once
// Calling C from grain (ffi.cpp): ffi_open / ffi_fn / ffi_sym, and raw memory
// (mem_alloc, mem_read, mem_write, cstr_at, bytes_ptr). Only the standalone
// `grain` program has it: this is a way to crash the process from a script, so
// it is not in the language library that the aios desktop and the web server's
// sandboxed handlers run.
#include "aios/userland/grain.hpp"

namespace grain_cli {

void install_ffi(aios::userland::grain::Vm& vm);

}  // namespace grain_cli
