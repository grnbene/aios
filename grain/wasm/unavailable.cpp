// What a page cannot do, said as an error where a program asks for it, not left out so that the name is "undefined".
#include <string>
#include <vector>

#include "aios/userland/grain.hpp"

namespace aios::userland::grain {

namespace {
Value bi_process_run(Vm&, std::vector<Value>&, std::string& error) {
  error = "process_run() is not available in a browser: a page cannot start programs";
  return Value::nil();
}
}  // namespace

void install_process_builtins(Vm& vm) { vm.define_native("process_run", 1, 2, bi_process_run); }

}  // namespace aios::userland::grain
