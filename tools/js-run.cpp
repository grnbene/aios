// Runs a JavaScript file (or -e source) in the in-tree engine: the standalone way to exercise it.
//   aios-js-run [-e source] [file.js ...]
// console output goes to stdout; the process exits non-zero on an uncaught exception.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#include "aios/js/context.hpp"

using namespace aios::js;

static bool run_source(Context& ctx, const std::string& src, const std::string& name) {
  Value r;
  if (!ctx.eval_script(src, name, &r)) {
    std::fprintf(stderr, "%s\n", ctx.describe_exception(ctx.take_exception()).c_str());
    return false;
  }
  ctx.run_jobs();
  return true;
}

int main(int argc, char** argv) {
  Context ctx;
  ctx.print = [](const std::string& s) { std::fwrite(s.data(), 1, s.size(), stdout); std::fflush(stdout); };
  ctx.now_ms = [] { return std::chrono::duration<double, std::milli>(std::chrono::system_clock::now().time_since_epoch()).count(); };
  int failed = 0;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "-e") && i + 1 < argc) {
      if (!run_source(ctx, argv[++i], "<cmdline>")) ++failed;
      continue;
    }
    std::ifstream f(argv[i], std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", argv[i]); ++failed; continue; }
    std::stringstream ss;
    ss << f.rdbuf();
    if (!run_source(ctx, ss.str(), argv[i])) ++failed;
  }
  return failed ? 1 : 0;
}
