// Processes on Windows. Same contract and table-based design as
// posix_process.cpp (which this host does not compile — see CMakeLists.txt):
// a handle maps to a tracked child, refreshed on each query rather than
// reaped by a signal handler.
#include "aios/hal/process.hpp"
#include "aios/hal/sync.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

namespace aios::hal {
namespace {

std::wstring to_wide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? (size_t)n : 0, L'\0');
  if (n > 0) { ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n); w.resize((size_t)n - 1); }
  return w;
}
std::string to_utf8(const std::wstring& w) {
  if (w.empty()) return std::string();
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? (size_t)n : 0, '\0');
  if (n > 0) { ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr); s.resize((size_t)n - 1); }
  return s;
}
std::string win_error() {
  const DWORD err = ::GetLastError();
  char buf[256];
  const DWORD n = ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0, buf, sizeof buf, nullptr);
  std::string msg = n ? std::string(buf, n) : ("error " + std::to_string(err));
  while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
  return msg;
}

std::wstring win_quote(const std::string& a) {
  const std::wstring w = to_wide(a);
  if (!w.empty() && w.find_first_of(L" \t\"") == std::wstring::npos) return w;
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (wchar_t c : w) {
    if (c == L'\\') { ++backslashes; out += c; continue; }
    if (c == L'"') { out.append(backslashes + 1, L'\\'); out += c; backslashes = 0; continue; }
    backslashes = 0;
    out += c;
  }
  out.append(backslashes, L'\\');
  out += L'"';
  return out;
}
std::wstring build_cmdline(const std::vector<std::string>& argv) {
  std::wstring line;
  for (const auto& a : argv) { if (!line.empty()) line += L' '; line += win_quote(a); }
  return line;
}

// The spawner's own environment, minus unset_env (case-insensitive, as every
// Windows env var is), plus spec.env (overwriting any name already present).
std::vector<wchar_t> build_env_block(const ProcessSpawn& spec) {
  std::map<std::wstring, std::wstring, bool (*)(const std::wstring&, const std::wstring&)> vars(
      [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
  if (LPWCH block = ::GetEnvironmentStringsW()) {
    for (LPWCH p = block; *p; ) {
      const size_t len = wcslen(p);
      const wchar_t* eq = wcschr(p, L'=');
      if (eq && eq != p) vars[std::wstring(p, eq - p)] = std::wstring(eq + 1);
      p += len + 1;
    }
    ::FreeEnvironmentStringsW(block);
  }
  for (const auto& n : spec.unset_env) vars.erase(to_wide(n));
  for (const auto& [k, v] : spec.env) vars[to_wide(k)] = to_wide(v);
  std::vector<wchar_t> out;
  for (const auto& [k, v] : vars) {
    std::wstring kv = k + L"=" + v;
    out.insert(out.end(), kv.begin(), kv.end());
    out.push_back(L'\0');
  }
  out.push_back(L'\0');
  return out;
}

HANDLE open_output(const std::string& output_path) {
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  if (output_path.empty()) return ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  HANDLE h = ::CreateFileW(to_wide(output_path).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  return h;
}

struct Child {
  HANDLE process = nullptr;
  DWORD pid = 0;
  bool finished = false;
  ProcessStatus last;
};

// Deliberately never destroyed — see posix_process.cpp's identical table for why.
Mutex& table_mutex() { static Mutex* m = new Mutex(); return *m; }
std::map<ProcessHandle, Child>& table() {
  static std::map<ProcessHandle, Child>* t = new std::map<ProcessHandle, Child>();
  return *t;
}
ProcessHandle next_handle() { static ProcessHandle n = 1; return n++; }

void refresh(Child& c) {
  if (c.finished || !c.process) return;
  DWORD code = 0;
  if (!::GetExitCodeProcess(c.process, &code)) { c.finished = true; c.last = ProcessStatus{false, true, 0, 0}; return; }
  if (code == STILL_ACTIVE) { c.last.running = true; return; }
  c.finished = true;
  c.last.running = false;
  c.last.exited = true;
  c.last.exit_code = (int)code;
}

}  // namespace

Result<ProcessHandle> spawn_process(const ProcessSpawn& spec) {
  if (spec.argv.empty()) return Result<ProcessHandle>(Status::Invalid, "a process needs a program to run");

  HANDLE nul_in = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  if (nul_in != INVALID_HANDLE_VALUE) ::SetHandleInformation(nul_in, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  HANDLE out = open_output(spec.output_path);
  if (out != INVALID_HANDLE_VALUE) ::SetHandleInformation(out, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nul_in;
  si.hStdOutput = out;
  si.hStdError = out;

  std::wstring cmdline = build_cmdline(spec.argv);
  std::vector<wchar_t> env = build_env_block(spec);
  std::wstring wd = to_wide(spec.working_dir);

  PROCESS_INFORMATION pi{};
  const BOOL ok = ::CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
      CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env.data(),
      spec.working_dir.empty() ? nullptr : wd.c_str(), &si, &pi);
  if (nul_in != INVALID_HANDLE_VALUE) ::CloseHandle(nul_in);
  if (out != INVALID_HANDLE_VALUE) ::CloseHandle(out);
  if (!ok) return Result<ProcessHandle>(Status::IoError, spec.argv[0] + ": " + win_error());
  ::CloseHandle(pi.hThread);

  LockGuard l(table_mutex());
  ProcessHandle h = next_handle();
  Child c;
  c.process = pi.hProcess;
  c.pid = pi.dwProcessId;
  c.last.running = true;
  table()[h] = c;
  return h;
}

Result<ProcessStatus> process_status(ProcessHandle h) {
  LockGuard l(table_mutex());
  auto it = table().find(h);
  if (it == table().end()) return Result<ProcessStatus>(Status::NotFound, "no such process handle");
  refresh(it->second);
  return it->second.last;
}

Result<ProcessStatus> wait_process(ProcessHandle h, uint32_t timeout_ms) {
  HANDLE process = nullptr;
  {
    LockGuard l(table_mutex());
    auto it = table().find(h);
    if (it == table().end()) return Result<ProcessStatus>(Status::NotFound, "no such process handle");
    if (it->second.finished) return it->second.last;
    process = it->second.process;
  }
  // Waited on outside the lock: a supervisor asking about other children must
  // not stall behind this one, same reasoning as posix_process.cpp's polling wait.
  const DWORD r = ::WaitForSingleObject(process, timeout_ms);
  LockGuard l(table_mutex());
  auto it = table().find(h);
  if (it == table().end()) return Result<ProcessStatus>(Status::NotFound, "no such process handle");
  if (r == WAIT_TIMEOUT) return Result<ProcessStatus>(Status::Busy, "the process is still running");
  refresh(it->second);
  return it->second.last;
}

Result<void> signal_process(ProcessHandle h, ProcessSignal sig) {
  LockGuard l(table_mutex());
  auto it = table().find(h);
  if (it == table().end()) return Result<void>(Status::NotFound, "no such process handle");
  refresh(it->second);
  if (it->second.finished) return Result<void>(Status::Invalid, "the process has already finished");
  if (sig == ProcessSignal::Kill) {
    if (!::TerminateProcess(it->second.process, 1)) return Result<void>(Status::IoError, "TerminateProcess: " + win_error());
    return Ok();
  }
  // No POSIX signal disposition exists here: a Ctrl+Break to the child's own
  // process group (CREATE_NEW_PROCESS_GROUP gave it one) is the nearest
  // cooperative stop Windows has, and is what Interrupt/Terminate/Hangup all
  // get — a console program's default handler for it is to exit, same as the
  // signals it stands in for.
  if (!::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, it->second.pid))
    return Result<void>(Status::IoError, "GenerateConsoleCtrlEvent: " + win_error());
  return Ok();
}

std::string find_program(const std::vector<std::string>& names) {
  const char* path_env = ::getenv("PATH");
  std::string search = path_env ? path_env : "";
  static const char* kExts[] = {"", ".exe", ".cmd", ".bat", ".com"};
  auto try_path = [](const std::string& base) -> std::string {
    for (const char* ext : kExts) {
      std::string candidate = base + ext;
      const DWORD attrs = ::GetFileAttributesW(to_wide(candidate).c_str());
      if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return candidate;
    }
    return {};
  };
  for (const auto& name : names) {
    if (name.find('\\') != std::string::npos || name.find('/') != std::string::npos || (name.size() > 1 && name[1] == ':')) {
      if (auto hit = try_path(name); !hit.empty()) return hit;
      continue;
    }
    size_t at = 0;
    while (at <= search.size()) {
      size_t sep = search.find(';', at);
      if (sep == std::string::npos) sep = search.size();
      std::string dir = search.substr(at, sep - at);
      at = sep + 1;
      if (dir.empty()) continue;
      if (auto hit = try_path(dir + "\\" + name); !hit.empty()) return hit;
    }
  }
  return {};
}

Result<std::string> scratch_path(const std::string& suffix) {
  wchar_t dir[MAX_PATH];
  if (!::GetTempPathW(MAX_PATH, dir)) return Result<std::string>(Status::IoError, "GetTempPath: " + win_error());
  wchar_t name[MAX_PATH];
  if (!::GetTempFileNameW(dir, L"aios", 0, name)) return Result<std::string>(Status::IoError, "GetTempFileName: " + win_error());
  std::string path = to_utf8(name);
  // GetTempFileName had to create the file to guarantee the name; a caller
  // that wants to write it fresh, or wants a particular extension, gets a
  // clean path, same as posix_process.cpp's mkstemp+unlink.
  if (!suffix.empty()) {
    ::DeleteFileW(name);
    path += suffix;
  }
  return path;
}

Result<ToolResult> run_tool(const std::vector<std::string>& argv, uint32_t timeout_ms, const std::string& working_dir) {
  auto out_path = scratch_path(".out");
  if (!out_path.ok()) return out_path.error();

  ProcessSpawn spec;
  spec.argv = argv;
  spec.working_dir = working_dir;
  spec.output_path = *out_path;
  auto h = spawn_process(spec);
  if (!h.ok()) { ::DeleteFileW(to_wide(*out_path).c_str()); return h.error(); }

  auto st = wait_process(*h, timeout_ms);
  if (!st.ok()) {
    (void)signal_process(*h, ProcessSignal::Kill);
    (void)release_process(*h);
    ::DeleteFileW(to_wide(*out_path).c_str());
    return Result<ToolResult>(Status::Busy, argv[0] + " did not finish within " + std::to_string(timeout_ms) + "ms");
  }
  (void)release_process(*h);

  ToolResult r;
  r.exit_code = st->exit_code;
  if (FILE* f = ::_wfopen(to_wide(*out_path).c_str(), L"rb")) {
    char chunk[4096];
    size_t n;
    while ((n = ::fread(chunk, 1, sizeof chunk, f)) > 0) r.output.append(chunk, n);
    ::fclose(f);
  }
  ::DeleteFileW(to_wide(*out_path).c_str());
  // The child's own CRT text-mode-translates "\n" to "\r\n" writing to this
  // (binary-opened, so untouched on the read above) file: every POSIX host's
  // tool output is bare "\n", so a caller comparing or splitting on '\n'
  // alone (as enable_jail's mode_ does) must see the same thing here.
  std::string normalized;
  normalized.reserve(r.output.size());
  for (char c : r.output) if (c != '\r') normalized += c;
  r.output = std::move(normalized);
  return r;
}

Result<void> release_process(ProcessHandle h) {
  LockGuard l(table_mutex());
  auto it = table().find(h);
  if (it == table().end()) return Result<void>(Status::NotFound, "no such process handle");
  refresh(it->second);
  if (!it->second.finished && it->second.process) {
    ::TerminateProcess(it->second.process, 1);
    ::WaitForSingleObject(it->second.process, 5000);
  }
  if (it->second.process) ::CloseHandle(it->second.process);
  table().erase(it);
  return Ok();
}

}  // namespace aios::hal
