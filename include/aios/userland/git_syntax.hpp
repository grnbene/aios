#pragma once
// A file's syntax outline: its named units and their digests (see git_structural.hpp, which makes them, and
// git_blocks.hpp, which keeps them). Plain data, so the repository can hold them without the parser.
#include <cstdint>
#include <string>
#include <vector>

namespace aios::userland {

struct GitSyntaxUnit {
  std::string scope, kind, name;
  uint32_t start = 0, end = 0;          // bytes
  uint32_t line = 0, end_line = 0;      // 1-based, inclusive
  int depth = 0;
  uint64_t digest = 0;                  // of its whole text
  uint64_t shell = 0;                   // of its text without the units under it, whitespace and separators ignored
};
struct GitSyntaxOutline {
  std::string language;
  std::vector<GitSyntaxUnit> units;     // in file order, each followed by the units under it
  uint64_t shell = 0;                   // the file's text outside every top-level unit, the same way
};

}  // namespace aios::userland
