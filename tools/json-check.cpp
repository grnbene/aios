// Reads JSON documents, one per file named on the command line, and prints "OK <compact dump>" or "ERR"
// for each, so scripts/json-diff.py can hold the in-tree parser against Python's json on the same text.
#include <cstdio>
#include <fstream>
#include <sstream>

#include "aios/core/json.hpp"

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    std::ifstream f(argv[i], std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    auto r = aios::json::parse(ss.str());
    if (r.ok()) std::printf("OK %s\n", r->dump().c_str());
    else std::printf("ERR\n");
  }
  return 0;
}
