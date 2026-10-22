#pragma once
// Programs that ship as one file: the grain runtime with a program packed onto
// it (bundle.cpp).
//
// A pack is a little archive - the program's source, and any files it brought
// (assets) - followed by a trailer: the magic "GRAINPK1", the pack's length and
// its SHA-256. `grain build` copies a runtime and appends a pack; that runtime,
// started, finds the pack on its own executable and runs it. On macOS a .app
// keeps the runtime untouched (so it can be signed) and the pack in
// Contents/Resources/program.grainpk.
#include <map>
#include <string>
#include <vector>

#include "aios/userland/grain.hpp"

namespace grain_cli {

struct Pack {
  std::string main_name;                       // the program, e.g. "app.gr"
  std::map<std::string, std::string> files;    // name -> bytes; holds main_name too
};

// The pack this executable carries (appended, or in its .app's Resources);
// false with an empty error when there is none, false with the reason when one
// is there but damaged.
bool find_own_pack(Pack& pack, std::string& error);

// `grain build <file.gr> [-o out] [--include path]... [--runtime grain-binary] [--app]`
int build_command(const std::vector<std::string>& args);

// bundle_file(name) and bundle_files(), for the program a pack runs. `pack`
// must outlive the VM.
void install_bundle_builtins(aios::userland::grain::Vm& vm, const Pack& pack);

}  // namespace grain_cli
