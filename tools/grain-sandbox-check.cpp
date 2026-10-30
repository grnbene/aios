// A grain VM sees the machine's environment, and the MariaDB client, only when its host hands them over.
//
//   1. getenv: a VM whose host did not set allow_env cannot read the process's variables (on the fleet the app host's
//      environment holds AIOS_FLEET_KEY); one that did, can.
//   2. setenv: such a VM changes a table of its own, not the process every app on the host shares.
//   3. The sidecar's sandboxed aios_grain_run has no db_connect / asql_query.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "aios/sidecar/aios_sidecar_c_abi.h"
#include "aios/userland/grain.hpp"

namespace g = aios::userland::grain;

namespace {
int failures = 0;
void expect(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}
std::string run(g::Vm& vm, const char* src) {
  auto c = g::compile_source(src, "check", true);
  if (!c.ok()) return "compile error";
  auto r = vm.run(c.script);
  if (!r.ok) return "error: " + r.error;
  return r.value.type == g::Type::Nil ? "nil" : r.value.str();
}
}  // namespace

int main() {
  ::setenv("GRAIN_SANDBOX_CHECK_SECRET", "s3", 1);
  {
    g::Vm vm;
    g::install_core_builtins(vm);
    expect(run(vm, "getenv(\"GRAIN_SANDBOX_CHECK_SECRET\")") == "nil", "a sandboxed VM cannot read the process environment");
    expect(run(vm, "env(\"GRAIN_SANDBOX_CHECK_SECRET\")") == "nil", "nor through env()");
    run(vm, "setenv(\"GRAIN_SANDBOX_CHECK_SET\", \"mine\")");
    expect(std::getenv("GRAIN_SANDBOX_CHECK_SET") == nullptr, "its setenv does not reach the process");
    expect(run(vm, "getenv(\"GRAIN_SANDBOX_CHECK_SET\")") == "mine", "it reads back what it set itself");
  }
  {
    g::Vm vm;
    g::install_core_builtins(vm);
    vm.allow_env = true;
    expect(run(vm, "getenv(\"GRAIN_SANDBOX_CHECK_SECRET\")") == "s3", "a VM its host allows reads the process environment");
  }
  {
    char out[256] = {0}, err[256] = {0};
    int64_t i = 0;
    double d = 0;
    int rc = aios_grain_run("db_connect(\"127.0.0.1\", 3306, \"u\", \"p\", \"d\")", 100000, &i, &d, out, sizeof out, err, sizeof err);
    expect(rc != 0 && std::string(err).find("'db_connect' is not defined") != std::string::npos, "the sidecar has no db_connect");
    rc = aios_grain_run("asql_query(\"SELECT 1\")", 100000, &i, &d, out, sizeof out, err, sizeof err);
    expect(rc != 0 && std::string(err).find("'asql_query' is not defined") != std::string::npos, "the sidecar has no asql_query");
    rc = aios_grain_run("sidecar_sha256(\"a\")", 100000, &i, &d, out, sizeof out, err, sizeof err);
    expect(rc == 0, "the sidecar keeps its local builtins");
  }
  return failures ? 1 : 0;
}
