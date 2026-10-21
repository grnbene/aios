#undef NDEBUG
// Opens an existing AIOS root in a fresh process and reports what the store
// says about itself. The question it answers is the one that matters after a
// vacuum: can this store still be opened at all?
#include <iostream>
#include <string>
#include "aios/kernel/kernel.hpp"

using namespace aios;

int main(int argc, char** argv) {
  if (argc < 2) { std::cerr << "usage: aios-asql-open-check <root>\n"; return 2; }
  KernelConfig cfg;
  cfg.root = argv[1];
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::cout << "OPEN FAILED: " << k.error().message << "\n"; return 1; }
  auto& store = (*k.value()).store();
  std::cout << "opened " << argv[1] << " objects=" << store.count()
            << " active=" << store.active_count()
            << " cold_blocks=" << store.cold_blocks_count() << "\n";
  return 0;
}
