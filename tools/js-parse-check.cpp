// Parses JavaScript files with the in-tree parser and reports success or the first syntax error.
//   aios-js-parse [-s] file.js ...     (-s: parse in strict mode)
// With no error it prints "ok <file> <bytes> <ms>". Exit status is the number of files that failed.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "../src/js/ast.hpp"

using namespace aios::js;

static std::u16string utf8_to_utf16(const std::string& s) {
  std::u16string u;
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = 0xFFFD;
    size_t n = 1;
    if (c < 0x80) cp = c;
    else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); n = 2; }
    else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) { cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); n = 3; }
    else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) { cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); n = 4; }
    if (cp >= 0x10000) { cp -= 0x10000; u += static_cast<char16_t>(0xD800 + (cp >> 10)); u += static_cast<char16_t>(0xDC00 + (cp & 0x3FF)); }
    else u += static_cast<char16_t>(cp);
    i += n;
  }
  return u;
}

int main(int argc, char** argv) {
  bool strict = false;
  int failed = 0;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "-s")) { strict = true; continue; }
    std::ifstream f(argv[i], std::ios::binary);
    if (!f) { std::printf("cannot open %s\n", argv[i]); ++failed; continue; }
    std::stringstream ss;
    ss << f.rdbuf();
    auto src = std::make_shared<const std::u16string>(utf8_to_utf16(ss.str()));
    Program prog;
    ParseError err;
    ParseOptions opt;
    opt.strict = strict;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = parse_program(src, opt, prog, &err);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ok) std::printf("ok   %s  %zu bytes  %.1f ms  %zu nodes\n", argv[i], ss.str().size(), ms, prog.nodes.size());
    else { std::printf("FAIL %s:%u:%u: SyntaxError: %s\n", argv[i], err.line, err.col, err.message.c_str()); ++failed; }
  }
  return failed;
}
