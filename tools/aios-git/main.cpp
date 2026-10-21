// aios-git's program: arguments in UTF-8, the standard streams in binary (a pack or a bundle on stdout
// must not have its bytes rewritten), then git_cli's run().
#include "git_cli.hpp"

#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace {
#ifdef _WIN32
std::string utf8(const wchar_t* w) {
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s((size_t)(n > 0 ? n - 1 : 0), '\0');
  if (n > 1) ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}
#endif
}  // namespace

int main(int argc, char* argv[]) {
  std::vector<std::string> args;
  std::string cwd;
#ifdef _WIN32
  (void)argc;
  (void)argv;
  int wargc = 0;
  wchar_t** wargv = ::CommandLineToArgvW(::GetCommandLineW(), &wargc);
  for (int i = 0; i < wargc; ++i) args.push_back(utf8(wargv[i]));
  ::LocalFree(wargv);
  wchar_t buf[32768];
  DWORD n = ::GetCurrentDirectoryW(32768, buf);
  cwd = n ? utf8(buf) : std::string(".");
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
#else
  for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
  char buf[4096];
  cwd = ::getcwd(buf, sizeof buf) ? std::string(buf) : std::string(".");
#endif
  std::ios::sync_with_stdio(false);
  int rc = aios_git::run(args, cwd, std::cout, std::cerr, std::cin);
  std::cout.flush();
  std::cerr.flush();
  return rc;
}
