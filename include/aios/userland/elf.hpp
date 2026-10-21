#pragma once
// ELF64, read as the bytes that arrived.
//
// This is what stands between a file on this machine's disk and a program it
// can run: the header validated rather than assumed, the program headers
// bounds-checked against the file they claim to live in, and the loadable
// segments handed back with the permissions they ask for. Nothing here maps
// anything — mapping is the address space's business, and it differs between
// the substrate that has page tables and the one that does not.
#include <cstdint>
#include <string>
#include <vector>

#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"

namespace aios::userland {

struct ElfSegment {
  uint64_t vaddr = 0;
  uint64_t file_offset = 0;
  uint64_t file_size = 0;
  uint64_t mem_size = 0;        // larger than file_size when the segment has .bss behind it
  bool readable = false, writable = false, executable = false;
  std::string rights() const { return std::string(readable ? "r" : "-") + (writable ? "w" : "-") + (executable ? "x" : "-"); }
};

struct ElfImage {
  uint64_t entry = 0;
  std::string machine;          // "x86-64"
  std::string kind;             // "executable" | "shared object"
  bool static_linked = true;    // false when it asks for an interpreter
  std::string interpreter;
  std::vector<ElfSegment> segments;
  uint64_t lowest = 0, highest = 0;   // the span the image wants in memory
  uint64_t bytes = 0;
};

// Read and validate. Every refusal names what was wrong with the file rather
// than reporting that it "is not an ELF".
Result<ElfImage> elf_read(ByteSpan bytes);

struct ElfFile {
  std::string path;
  ByteBuffer bytes;
  ElfImage image;
};

// The same, for a file on this machine — read through the Surface, so a program
// is only loadable by an entity that may read this machine's storage.
Result<ElfFile> elf_open(Kernel& kernel, Surface* actor, const std::string& path);

}  // namespace aios::userland
