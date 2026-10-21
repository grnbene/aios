// The GPU backends are one file each, beside this one:
//
//   src/compute/cuda_backend.cpp    binds the CUDA driver API and NVRTC
//   src/compute/vulkan_backend.cpp  binds the Vulkan loader and dispatches SPIR-V
//
// What used to be HERE was both of them together: two constructors that wrote
// lane counts, memory sizes and FLOP rates into their own fields on any
// machine, and two execute() methods that ran single-threaded `for` loops over
// system memory under comments calling them SPIR-V and PTX. Nothing in this
// file linked against, loaded, or spoke to a GPU.
//
// The file stays so the target list and every include of backends.hpp keep
// working; the header now declares both classes with runtime-bound state.
#include "aios/compute/backends.hpp"
