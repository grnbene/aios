// The console on Windows: CONIN$ and CONOUT$, opened for this one question, so a
// prompt reaches the person even while stdin and stdout carry a protocol.
#include "aios/hal/tty.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace aios::hal {
namespace {

std::wstring widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
  if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}
std::string narrow(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)(n > 0 ? n : 0), '\0');
  if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

struct Handle {
  HANDLE h;
  explicit Handle(HANDLE x) : h(x) {}
  ~Handle() { if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
};

}  // namespace

Result<std::string> tty_ask(const std::string& question, bool echo) {
  Handle in(::CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
  Handle out(::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
  if (in.h == INVALID_HANDLE_VALUE || out.h == INVALID_HANDLE_VALUE)
    return Result<std::string>(Status::NotFound, "no console to ask");

  std::wstring wp = widen(question);
  DWORD wrote = 0;
  ::WriteConsoleW(out.h, wp.data(), (DWORD)wp.size(), &wrote, nullptr);

  DWORD mode = 0;
  bool restore = false;
  if (!echo && ::GetConsoleMode(in.h, &mode)) {
    restore = ::SetConsoleMode(in.h, (mode | ENABLE_LINE_INPUT) & ~(DWORD)ENABLE_ECHO_INPUT) != 0;
  }
  std::wstring line;
  bool failed = false;
  for (;;) {
    wchar_t buf[256];
    DWORD got = 0;
    if (!::ReadConsoleW(in.h, buf, 256, &got, nullptr)) { failed = true; break; }
    if (got == 0) break;
    bool done = false;
    for (DWORD i = 0; i < got; ++i) {
      if (buf[i] == L'\r' || buf[i] == L'\n') { done = true; break; }
      line += buf[i];
    }
    if (done) break;
  }
  if (restore) {
    ::SetConsoleMode(in.h, mode);
    ::WriteConsoleW(out.h, L"\r\n", 2, &wrote, nullptr);
  }
  if (failed) return Result<std::string>(Status::IoError, "reading the console failed");
  return narrow(line);
}

}  // namespace aios::hal
