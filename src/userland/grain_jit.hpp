// grain -> native code. The interface between the VM (grain_vm.cpp) and the
// AArch64 and x86-64 compilers (grain_jit_arm64.cpp, grain_jit_x64.cpp); on
// other architectures compile() always says no and everything is interpreted.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "aios/userland/grain.hpp"

namespace aios::userland::grain::jit {

// What native code is handed. Field offsets are baked into the generated
// code (see the static_asserts in grain_jit_arm64.cpp).
struct Ctx {
  Vm* vm;                        // + 0
  std::vector<Value>* stack;     // + 8
  uint64_t* steps;               // +16
  const uint64_t* step_limit;    // +24
  void* frames;                  // +32  std::vector<Vm::Frame>*
  void* open;                    // +40  std::vector<std::shared_ptr<Upvalue>>* (open upvalues)
  uint64_t serial;               // +48  the Vm's serial (Chunk::global_cache_owner)
};

// Vm::Frame as the native call path writes it (static_asserts in grain_vm.cpp).
constexpr uint32_t kFrameClosure = 0, kFrameIp = 8, kFrameBase = 16,
                   kFrameOwnedPtr = 24, kFrameOwnedCtrl = 32, kFrameChunk = 40, kFrameSize = 48;
// The deepest call chain the VM allows (Vm::invoke's limit).
constexpr uint32_t kMaxFrames = 200;

// Native entry: runs the function whose frame is on top of the VM's frame
// stack (frame base `base` = stack index of the callee slot) through to its
// Return. 1 = returned normally, 0 = the VM's error is set.
using Entry = int (*)(Ctx*, size_t base);

// Native code for `fn`, or nullptr (not this architecture, or a layout
// check failed). Cached on the FnObj by the caller.
Entry compile(const FnObj& fn);

// Give back compile()'s code (FnObj's destructor calls this).
void release(void* code);

}  // namespace aios::userland::grain::jit
