#pragma once
// What the standalone `grain` program gives a VM from the machine it runs on:
// ordinary files, the `sys` map, the script's arguments, processes and
// sockets. The language owns the natives' semantics; this is only the seam to
// the host operating system, the same seam `aios grain` fills from the kernel.
#include <string>
#include <vector>

#include "aios/userland/grain.hpp"

namespace grain_cli {

// Files on the host's own filesystem, paths relative to the working directory.
aios::userland::grain::Files& host_files();

// `sys`: os, arch, cpus, memory, host, version - read once, before the script runs.
void install_sys(aios::userland::grain::Vm& vm, const std::string& version);

// `args`: the arguments after the script's name, as a list of strings.
void install_args(aios::userland::grain::Vm& vm, const std::vector<std::string>& args);

// sys_exec(cmd) through the host's shell, and exec([program, args...]) without
// one; both return {ok, code, output}.
void install_process(aios::userland::grain::Vm& vm);

}  // namespace grain_cli
