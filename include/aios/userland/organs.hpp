#pragma once
// Organs: primitives this machine grew for itself.
//
// When a dream needs something the machine cannot do — a renderer, a solver, a
// driver for a device it has never seen — the mind writes it in C++, the
// machine builds it and loads it with the substrate's own dynamic loader, and
// keeps it. The next dream has it. The machine's vocabulary grows by accretion
// rather than by release, which is the whole point.
//
// The ABI is one function, text in and text out, so an organ can be written
// without knowing anything about this kernel:
//
//     extern "C" const char* aios_organ(const char* request);
//
// The returned pointer must stay valid until the next call (a static
// std::string is the usual way). An organ that wants to describe itself
// answers the request "describe".
#include <string>
#include <cstdint>
#include "aios/core/result.hpp"

namespace aios::userland {

// Where an organ's source and library live under the kernel root.
std::string organ_dir(const std::string& root, const std::string& name);
std::string organ_library_path(const std::string& root, const std::string& name);

// Call an organ, loading it on first use. NotFound when nothing is built under
// that name; Unsupported on a substrate with no dynamic loader.
Result<std::string> organ_call(const std::string& root, const std::string& name, const std::string& request);

// An organ that draws. Optional second export, for the things the face
// language's per-shape paint verbs cannot reach — a field of noise, a particle
// system, anything that is a million pixels of arithmetic:
//
//     extern "C" void aios_paint(uint32_t* buf, int w, int h, double t);
//
// buf is w*h pixels of 0xAARRGGBB, row-major, the organ's to fill. Unsupported
// when the organ exports no such symbol.
Result<void> organ_paint(const std::string& root, const std::string& name,
                         uint32_t* buf, int w, int h, double t);

// Drop a loaded organ so a rebuilt library can be picked up.
void organ_forget(const std::string& name);

}  // namespace aios::userland
