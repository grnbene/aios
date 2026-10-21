// profiling driver: compile and run one grain file with no step limit
#include "aios/userland/grain.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
int main(int argc, char** argv) {
  std::ifstream f(argv[1]); std::stringstream ss; ss << f.rdbuf();
  auto c = aios::userland::grain::compile_source(ss.str(), argv[1], false);
  if (!c.ok()) { std::cerr << "compile failed\n"; return 1; }
  aios::userland::grain::Vm vm; aios::userland::grain::install_builtins(vm); vm.step_limit = 0;
  std::vector<std::string> out; vm.sink = &out;
  auto r = vm.run(c.script);
  for (auto& l : out) std::cout << l << "\n";
  if (!r.ok) { std::cerr << r.error << "\n"; return 1; }
}
