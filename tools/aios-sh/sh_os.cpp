// The host under aios-sh: files, pipes and processes, Win32 or POSIX. Nothing above this file knows which.
#include "sh.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char** environ;
#endif

namespace aiossh {

namespace {
std::string lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
}

#ifdef _WIN32
// ---------------------------------------------------------------------------------------------------
// Win32
// ---------------------------------------------------------------------------------------------------
namespace {
std::wstring W(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}
std::string U(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}
// Long paths: \\?\ for absolute drive paths, so a deep node_modules is reachable.
std::wstring WP(const std::string& p) {
  std::string q = p;
  for (auto& c : q) if (c == '/') c = '\\';
  if (q.size() >= 3 && q[1] == ':' && q[2] == '\\' && q.size() > 240) return L"\\\\?\\" + W(q);
  return W(q);
}
HANDLE H(Fd f) { return reinterpret_cast<HANDLE>(f); }
Fd F(HANDLE h) { return reinterpret_cast<Fd>(h); }
// One argv element as CreateProcess's parser (MSVC's) reads it back.
std::string quote_arg_always(const std::string& a) {
  std::string out = "\"";
  size_t bs = 0;
  for (char c : a) {
    if (c == '\\') { ++bs; out += c; continue; }
    if (c == '"') { out.append(bs + 1, '\\'); out += c; bs = 0; continue; }
    bs = 0;
    out += c;
  }
  out.append(bs, '\\');
  out += '"';
  return out;
}
std::string quote_arg(const std::string& a) {
  if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
  return quote_arg_always(a);
}
// cmd.exe's own rules for a .cmd/.bat. An argument with a metacharacter is quoted, not ^-escaped: a batch
// file passes %* on to another command line (npm.cmd does), and quotes survive that where ^ does not.
// What is still outside quotes (after an embedded \") is ^-escaped.
std::string quote_for_cmd(const std::string& a) {
  // An embedded quote is doubled (""), which the C runtime and CommandLineToArgvW read as one quote, so
  // cmd always sees an even number of them and its quoting stays in step for the arguments after.
  std::string q;
  if (!a.empty() && a.find_first_of(" \t\"&|<>()^%!,;=") == std::string::npos) {
    q = a;
  } else {
    q = "\"";
    size_t bs = 0;
    for (char c : a) {
      if (c == '\\') { ++bs; q += c; continue; }
      if (c == '"') { q.append(bs, '\\'); q += "\"\""; bs = 0; continue; }
      bs = 0;
      q += c;
    }
    q.append(bs, '\\');
    q += '"';
  }
  std::string out;
  bool in_quotes = false;
  for (char c : q) {
    // cmd toggles its quoting on every ", backslash or not: mirror it, and never escape the quotes themselves
    if (c == '"') { in_quotes = !in_quotes; out += c; continue; }
    if (!in_quotes && std::strchr("()%!^<>&|", c)) out += '^';
    out += c;
  }
  return out;
}
}  // namespace

namespace os {
Fd std_fd(int which) {
  DWORD id = which == 0 ? STD_INPUT_HANDLE : which == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE;
  HANDLE h = ::GetStdHandle(id);
  return (h == nullptr || h == INVALID_HANDLE_VALUE) ? kNoFd : F(h);
}
bool make_pipe(Fd& r, Fd& w) {
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
  HANDLE hr = nullptr, hw = nullptr;
  if (!::CreatePipe(&hr, &hw, &sa, 1 << 16)) return false;
  r = F(hr); w = F(hw);
  return true;
}
void close_fd(Fd f) { if (f != kNoFd && f != 0) ::CloseHandle(H(f)); }
Fd dup_fd(Fd f) {
  if (f == kNoFd) return kNoFd;
  HANDLE out = nullptr;
  if (!::DuplicateHandle(::GetCurrentProcess(), H(f), ::GetCurrentProcess(), &out, 0, FALSE, DUPLICATE_SAME_ACCESS)) return kNoFd;
  return F(out);
}
long read_fd(Fd f, char* buf, size_t n) {
  if (f == kNoFd) return 0;
  DWORD got = 0;
  if (!::ReadFile(H(f), buf, static_cast<DWORD>(n), &got, nullptr)) {
    DWORD e = ::GetLastError();
    return (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF) ? 0 : -1;
  }
  return static_cast<long>(got);
}
bool write_all(Fd f, const char* p, size_t n) {
  if (f == kNoFd) return true;
  if (is_console(f)) {
    // The console takes UTF-16: written that way, UTF-8 text shows as text whatever the code page.
    std::wstring w = W(std::string(p, n));
    DWORD done = 0;
    return ::WriteConsoleW(H(f), w.data(), static_cast<DWORD>(w.size()), &done, nullptr) != 0;
  }
  while (n) {
    DWORD done = 0;
    if (!::WriteFile(H(f), p, static_cast<DWORD>(std::min<size_t>(n, 1 << 20)), &done, nullptr)) return false;
    p += done; n -= done;
  }
  return true;
}
Fd open_file(const std::string& path, Open how, std::string* err) {
  if (is_null_device(path)) {
    HANDLE h = ::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    return h == INVALID_HANDLE_VALUE ? kNoFd : F(h);
  }
  DWORD access = how == Open::Read ? GENERIC_READ : how == Open::Append ? FILE_APPEND_DATA | SYNCHRONIZE
               : how == Open::ReadWrite ? GENERIC_READ | GENERIC_WRITE : GENERIC_WRITE;
  DWORD disp = how == Open::Read ? OPEN_EXISTING : how == Open::Write ? CREATE_ALWAYS : OPEN_ALWAYS;
  HANDLE h = ::CreateFileW(WP(path).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disp,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (err) *err = error_text(static_cast<int>(::GetLastError()));
    return kNoFd;
  }
  return F(h);
}
bool is_console(Fd f) {
  DWORD mode = 0;
  return f != kNoFd && ::GetConsoleMode(H(f), &mode) != 0;
}
bool exists(const std::string& p) { return ::GetFileAttributesW(WP(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
bool is_dir(const std::string& p) {
  DWORD a = ::GetFileAttributesW(WP(p).c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
bool is_symlink(const std::string& p) {
  DWORD a = ::GetFileAttributesW(WP(p).c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool read_link(const std::string&, std::string* target) { target->clear(); return false; }   // ln copies on Windows
std::string resolved(const std::string& p) { return p; }
bool is_file(const std::string& p) {
  DWORD a = ::GetFileAttributesW(WP(p).c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool is_executable(const std::string& p) {
  if (!is_file(p)) return false;
  std::string ext = lower(p.size() >= 4 ? p.substr(p.size() - 4) : "");
  return ext == ".exe" || ext == ".com" || ext == ".bat" || ext == ".cmd" || ext == ".ps1";
}
uint64_t file_size(const std::string& p) {
  WIN32_FILE_ATTRIBUTE_DATA d{};
  if (!::GetFileAttributesExW(WP(p).c_str(), GetFileExInfoStandard, &d)) return 0;
  return (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}
int64_t mtime(const std::string& p) {
  WIN32_FILE_ATTRIBUTE_DATA d{};
  if (!::GetFileAttributesExW(WP(p).c_str(), GetFileExInfoStandard, &d)) return 0;
  uint64_t t = (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
  return static_cast<int64_t>(t / 10000000ULL) - 11644473600LL;
}
bool list_dir(const std::string& p, std::vector<DirEntry>& out) {
  WIN32_FIND_DATAW fd;
  std::wstring pat = WP(p);
  if (!pat.empty() && pat.back() != L'\\') pat += L'\\';
  pat += L'*';
  HANDLE h = ::FindFirstFileW(pat.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  do {
    std::string n = U(fd.cFileName);
    if (n == "." || n == "..") continue;
    out.push_back({n, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0});
  } while (::FindNextFileW(h, &fd));
  ::FindClose(h);
  return true;
}
bool make_dir(const std::string& p, std::string* err) {
  if (::CreateDirectoryW(WP(p).c_str(), nullptr)) return true;
  if (err) *err = error_text(static_cast<int>(::GetLastError()));
  return false;
}
bool remove_file(const std::string& p, std::string* err) {
  std::wstring w = WP(p);
  if (::DeleteFileW(w.c_str())) return true;
  DWORD e = ::GetLastError();
  if (e == ERROR_ACCESS_DENIED) {   // read-only (git's object files): clear it and try once more
    DWORD a = ::GetFileAttributesW(w.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY) && ::SetFileAttributesW(w.c_str(), a & ~FILE_ATTRIBUTE_READONLY) &&
        ::DeleteFileW(w.c_str()))
      return true;
  }
  if (err) *err = error_text(static_cast<int>(e));
  return false;
}
bool remove_dir(const std::string& p, std::string* err) {
  if (::RemoveDirectoryW(WP(p).c_str())) return true;
  if (err) *err = error_text(static_cast<int>(::GetLastError()));
  return false;
}
bool rename_path(const std::string& from, const std::string& to, std::string* err) {
  if (::MoveFileExW(WP(from).c_str(), WP(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return true;
  if (err) *err = error_text(static_cast<int>(::GetLastError()));
  return false;
}
bool copy_file(const std::string& from, const std::string& to, std::string* err) {
  if (::CopyFileW(WP(from).c_str(), WP(to).c_str(), FALSE)) return true;
  if (err) *err = error_text(static_cast<int>(::GetLastError()));
  return false;
}
bool touch(const std::string& p, std::string* err) {
  HANDLE h = ::CreateFileW(WP(p).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) { if (err) *err = error_text(static_cast<int>(::GetLastError())); return false; }
  FILETIME ft;
  ::GetSystemTimeAsFileTime(&ft);
  ::SetFileTime(h, nullptr, nullptr, &ft);
  ::CloseHandle(h);
  return true;
}
std::string temp_dir() {
  wchar_t b[MAX_PATH + 1];
  DWORD n = ::GetTempPathW(MAX_PATH, b);
  std::string s = U(std::wstring(b, n));
  while (s.size() > 3 && (s.back() == '\\' || s.back() == '/')) s.pop_back();
  return s;
}
std::string home_dir() {
  wchar_t b[32768];
  DWORD n = ::GetEnvironmentVariableW(L"USERPROFILE", b, 32768);
  return n ? U(std::wstring(b, n)) : current_dir();
}
std::string current_dir() {
  wchar_t b[32768];
  DWORD n = ::GetCurrentDirectoryW(32768, b);
  return U(std::wstring(b, n));
}
bool read_whole(const std::string& p, std::string& out, std::string* err) {
  Fd f = open_file(p, Open::Read, err);
  if (f == kNoFd) return false;
  char buf[1 << 16];
  long n;
  while ((n = read_fd(f, buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
  close_fd(f);
  return true;
}
bool write_whole(const std::string& p, const std::string& data, bool append, std::string* err) {
  Fd f = open_file(p, append ? Open::Append : Open::Write, err);
  if (f == kNoFd) return false;
  bool ok = write_all(f, data.data(), data.size());
  close_fd(f);
  return ok;
}

bool spawn(const std::vector<std::string>& argv, const std::string& cwd, const std::vector<std::string>& env,
           Fd in, Fd out, Fd err, Proc& p, std::string* why) {
  std::string prog = argv[0];
  std::string ext = lower(prog.size() >= 4 ? prog.substr(prog.size() - 4) : "");
  std::string line;
  std::wstring app;
  if (ext == ".cmd" || ext == ".bat") {
    // A batch file runs under cmd.exe, which re-parses the line: each argument quoted its way.
    wchar_t comspec[MAX_PATH];
    DWORD n = ::GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
    app = n ? std::wstring(comspec, n) : L"C:\\Windows\\System32\\cmd.exe";
    std::string inner;
    std::string batch = prog;
    for (auto& c : batch) if (c == '/') c = '\\';   // cmd reads C:/x/y.cmd as a switch, not a path
    for (size_t i = 0; i < argv.size(); ++i) inner += (i ? " " : "") + quote_for_cmd(i ? argv[i] : batch);
    line = "cmd.exe /d /s /c \"" + inner + "\"";
  } else if (ext == ".ps1") {
    app = L"";
    line = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File";
    for (const auto& a : argv) line += " " + quote_arg(a);
  } else {
    app = W(prog);
    for (size_t i = 0; i < argv.size(); ++i) line += (i ? " " : "") + quote_arg(argv[i]);
  }
  std::wstring wline = W(line);
  std::vector<wchar_t> wenv;
  for (const auto& kv : env) { std::wstring w = W(kv); wenv.insert(wenv.end(), w.begin(), w.end()); wenv.push_back(0); }
  wenv.push_back(0);
  // The child inherits exactly its three streams: each made inheritable on a private duplicate.
  auto inheritable = [](Fd f) -> HANDLE {
    if (f == kNoFd) return nullptr;
    HANDLE d = nullptr;
    ::DuplicateHandle(::GetCurrentProcess(), H(f), ::GetCurrentProcess(), &d, 0, TRUE, DUPLICATE_SAME_ACCESS);
    return d;
  };
  HANDLE hs[3] = {inheritable(in), inheritable(out), inheritable(err)};
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof si;
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = hs[0];
  si.StartupInfo.hStdOutput = hs[1];
  si.StartupInfo.hStdError = hs[2];
  SIZE_T size = 0;
  ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<unsigned char> attr_buf(size);
  auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
  std::vector<HANDLE> list;
  for (HANDLE h : hs) if (h && std::find(list.begin(), list.end(), h) == list.end()) list.push_back(h);
  bool use_list = ::InitializeProcThreadAttributeList(attrs, 1, 0, &size) && !list.empty() &&
                  ::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list.data(), list.size() * sizeof(HANDLE), nullptr, nullptr);
  si.lpAttributeList = use_list ? attrs : nullptr;
  PROCESS_INFORMATION pi{};
  std::wstring wcwd = WP(cwd);
  BOOL ok = ::CreateProcessW(app.empty() ? nullptr : app.c_str(), wline.data(), nullptr, nullptr, TRUE,
                             CREATE_UNICODE_ENVIRONMENT | (use_list ? EXTENDED_STARTUPINFO_PRESENT : 0), wenv.data(),
                             wcwd.empty() ? nullptr : wcwd.c_str(), &si.StartupInfo, &pi);
  DWORD e = ::GetLastError();
  if (use_list) ::DeleteProcThreadAttributeList(attrs);
  for (HANDLE h : hs) if (h) ::CloseHandle(h);
  if (!ok) { if (why) *why = error_text(static_cast<int>(e)); return false; }
  ::CloseHandle(pi.hThread);
  p.handle = reinterpret_cast<intptr_t>(pi.hProcess);
  p.pid = static_cast<int>(pi.dwProcessId);
  return true;
}
int wait(Proc& p) {
  if (!p.handle) return 127;
  ::WaitForSingleObject(reinterpret_cast<HANDLE>(p.handle), INFINITE);
  DWORD code = 1;
  ::GetExitCodeProcess(reinterpret_cast<HANDLE>(p.handle), &code);
  ::CloseHandle(reinterpret_cast<HANDLE>(p.handle));
  p.handle = 0;
  return static_cast<int>(code & 0xff) == static_cast<int>(code) ? static_cast<int>(code) : static_cast<int>(code & 0xff);
}
void kill_proc(Proc& p) { if (p.handle) ::TerminateProcess(reinterpret_cast<HANDLE>(p.handle), 143); }
int self_pid() { return static_cast<int>(::GetCurrentProcessId()); }
std::string find_program(const std::string& name, const std::string& path_var, const std::string& cwd) {
  std::vector<std::string> exts;
  {
    std::string pe = lower(std::getenv("PATHEXT") ? std::getenv("PATHEXT") : ".COM;.EXE;.BAT;.CMD");
    size_t a = 0;
    while (a <= pe.size()) { size_t b = pe.find(';', a); if (b == std::string::npos) b = pe.size(); if (b > a) exts.push_back(pe.substr(a, b - a)); a = b + 1; }
  }
  auto try_base = [&](const std::string& base) -> std::string {
    std::string l = lower(base);
    for (const auto& e : exts) if (l.size() > e.size() && l.compare(l.size() - e.size(), e.size(), e) == 0 && is_file(base)) return base;
    for (const auto& e : exts) if (is_file(base + e)) return base + e;
    if (is_file(base)) return base;      // an extensionless script: the caller reads its #! line
    return {};
  };
  if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return try_base(host_path(cwd, name));
  size_t a = 0;
  while (a <= path_var.size()) {
    size_t b = path_var.find(';', a);
    if (b == std::string::npos) b = path_var.size();
    std::string d = path_var.substr(a, b - a);
    a = b + 1;
    if (d.empty()) continue;
    if (d.size() > 2 && d.front() == '"' && d.back() == '"') d = d.substr(1, d.size() - 2);
    std::string hit = try_base(d + "\\" + name);
    if (!hit.empty()) return hit;
  }
  return {};
}
std::string self_exe() {
  wchar_t b[32768];
  DWORD n = ::GetModuleFileNameW(nullptr, b, 32768);
  return U(std::wstring(b, n));
}
void sleep_ms(long ms) { ::Sleep(static_cast<DWORD>(ms)); }
std::string error_text(int code) {
  wchar_t* msg = nullptr;
  DWORD n = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                             static_cast<DWORD>(code), MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
  std::string s = n ? U(std::wstring(msg, n)) : "error " + std::to_string(code);
  if (msg) ::LocalFree(msg);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.' || s.back() == ' ')) s.pop_back();
  // The POSIX words scripts and agents grep for.
  if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return "No such file or directory";
  if (code == ERROR_ACCESS_DENIED) return "Permission denied";
  if (code == ERROR_DIR_NOT_EMPTY) return "Directory not empty";
  if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) return "File exists";
  return s;
}
}  // namespace os

#else
// ---------------------------------------------------------------------------------------------------
// POSIX
// ---------------------------------------------------------------------------------------------------
namespace os {
Fd std_fd(int which) { return which; }
// Pipeline stages are threads, so one stage may fork while another is making a pipe: a pipe end that is
// not yet close-on-exec then lives on in that child (a host `git log` holding the write end of some other
// stage's pipe), and that pipe's reader never sees end of input. pipe2 makes the ends close-on-exec at
// once; where there is none, the pipe and its flags are made under the lock spawn forks under.
#if !defined(__linux__) && !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined(__NetBSD__)
static std::mutex fork_mu;
#endif
bool make_pipe(Fd& r, Fd& w) {
  int p[2];
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  if (::pipe2(p, O_CLOEXEC) != 0) return false;
#else
  {
    std::lock_guard<std::mutex> l(fork_mu);
    if (::pipe(p) != 0) return false;
    ::fcntl(p[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(p[1], F_SETFD, FD_CLOEXEC);
  }
#endif
  r = p[0]; w = p[1];
  return true;
}
void close_fd(Fd f) { if (f > 2) ::close(static_cast<int>(f)); }
Fd dup_fd(Fd f) { if (f == kNoFd) return kNoFd; int d = ::fcntl(static_cast<int>(f), F_DUPFD_CLOEXEC, 3); return d < 0 ? kNoFd : d; }
long read_fd(Fd f, char* buf, size_t n) {
  if (f == kNoFd) return 0;
  for (;;) { ssize_t r = ::read(static_cast<int>(f), buf, n); if (r < 0 && errno == EINTR) continue; return static_cast<long>(r); }
}
bool write_all(Fd f, const char* p, size_t n) {
  if (f == kNoFd) return true;
  while (n) {
    ssize_t w = ::write(static_cast<int>(f), p, n);
    if (w < 0) { if (errno == EINTR) continue; return false; }
    p += w; n -= static_cast<size_t>(w);
  }
  return true;
}
Fd open_file(const std::string& path, Open how, std::string* err) {
  int flags = how == Open::Read ? O_RDONLY : how == Open::Write ? (O_WRONLY | O_CREAT | O_TRUNC)
            : how == Open::Append ? (O_WRONLY | O_CREAT | O_APPEND) : (O_RDWR | O_CREAT);
  int f = ::open(path.c_str(), flags | O_CLOEXEC, 0666);
  if (f < 0) { if (err) *err = std::strerror(errno); return kNoFd; }
  return f;
}
bool is_console(Fd f) { return f != kNoFd && ::isatty(static_cast<int>(f)); }
bool exists(const std::string& p) { struct stat s; return ::stat(p.c_str(), &s) == 0; }
bool is_dir(const std::string& p) { struct stat s; return ::stat(p.c_str(), &s) == 0 && S_ISDIR(s.st_mode); }
bool is_symlink(const std::string& p) { struct stat s; return ::lstat(p.c_str(), &s) == 0 && S_ISLNK(s.st_mode); }
bool read_link(const std::string& p, std::string* target) {
  std::string b(4096, '\0');
  ssize_t n = ::readlink(p.c_str(), b.data(), b.size());
  if (n < 0) { target->clear(); return false; }
  target->assign(b.data(), static_cast<size_t>(n));
  return true;
}
std::string resolved(const std::string& p) {
  char real[PATH_MAX];
  if (::realpath(p.c_str(), real)) return real;
  // Not there (yet): resolve the folder it would be in, as realpath -m does for the last part.
  size_t cut = p.find_last_of('/');
  if (cut == std::string::npos || cut == 0 || !::realpath(p.substr(0, cut).c_str(), real)) return p;
  return std::string(real) + p.substr(cut);
}
bool make_link(const std::string& target, const std::string& link, bool symbolic, std::string* err) {
  if ((symbolic ? ::symlink(target.c_str(), link.c_str()) : ::link(target.c_str(), link.c_str())) == 0) return true;
  if (err) *err = std::strerror(errno);
  return false;
}
bool is_file(const std::string& p) { struct stat s; return ::stat(p.c_str(), &s) == 0 && !S_ISDIR(s.st_mode); }
bool is_executable(const std::string& p) { return is_file(p) && ::access(p.c_str(), X_OK) == 0; }
uint64_t file_size(const std::string& p) { struct stat s; return ::stat(p.c_str(), &s) == 0 ? static_cast<uint64_t>(s.st_size) : 0; }
int64_t mtime(const std::string& p) { struct stat s; return ::stat(p.c_str(), &s) == 0 ? static_cast<int64_t>(s.st_mtime) : 0; }
bool list_dir(const std::string& p, std::vector<DirEntry>& out) {
  DIR* d = ::opendir(p.c_str());
  if (!d) return false;
  while (dirent* e = ::readdir(d)) {
    std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    struct stat s, ls;
    std::string full = p + "/" + n;
    bool dir = ::stat(full.c_str(), &s) == 0 && S_ISDIR(s.st_mode);
    bool link = ::lstat(full.c_str(), &ls) == 0 && S_ISLNK(ls.st_mode);
    out.push_back({n, dir, link});
  }
  ::closedir(d);
  return true;
}
bool make_dir(const std::string& p, std::string* err) { if (::mkdir(p.c_str(), 0777) == 0) return true; if (err) *err = std::strerror(errno); return false; }
bool remove_file(const std::string& p, std::string* err) { if (::unlink(p.c_str()) == 0) return true; if (err) *err = std::strerror(errno); return false; }
bool remove_dir(const std::string& p, std::string* err) { if (::rmdir(p.c_str()) == 0) return true; if (err) *err = std::strerror(errno); return false; }
bool rename_path(const std::string& a, const std::string& b, std::string* err) { if (::rename(a.c_str(), b.c_str()) == 0) return true; if (err) *err = std::strerror(errno); return false; }
bool copy_file(const std::string& from, const std::string& to, std::string* err) {
  std::string data;
  if (!read_whole(from, data, err)) return false;
  if (!write_whole(to, data, false, err)) return false;
  struct stat s;
  if (::stat(from.c_str(), &s) == 0) ::chmod(to.c_str(), s.st_mode & 07777);
  return true;
}
bool touch(const std::string& p, std::string* err) {
  int f = ::open(p.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
  if (f < 0) { if (err) *err = std::strerror(errno); return false; }
  ::close(f);
  ::utimes(p.c_str(), nullptr);
  return true;
}
std::string temp_dir() { const char* t = std::getenv("TMPDIR"); return t && *t ? t : "/tmp"; }
std::string home_dir() { const char* h = std::getenv("HOME"); return h ? h : "/"; }
std::string current_dir() { char b[4096]; return ::getcwd(b, sizeof b) ? b : "/"; }
bool read_whole(const std::string& p, std::string& out, std::string* err) {
  Fd f = open_file(p, Open::Read, err);
  if (f == kNoFd) return false;
  char buf[1 << 16];
  long n;
  while ((n = read_fd(f, buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
  close_fd(f);
  return true;
}
bool write_whole(const std::string& p, const std::string& data, bool append, std::string* err) {
  Fd f = open_file(p, append ? Open::Append : Open::Write, err);
  if (f == kNoFd) return false;
  bool ok = write_all(f, data.data(), data.size());
  close_fd(f);
  return ok;
}
bool spawn(const std::vector<std::string>& argv, const std::string& cwd, const std::vector<std::string>& env,
           Fd in, Fd out, Fd err, Proc& p, std::string* why) {
  // Every pipeline stage is a thread, so the child of this fork may only make async-signal-safe calls:
  // anything that allocates is built before it.
  std::vector<char*> av, ev;
  for (const auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
  av.push_back(nullptr);
  for (const auto& e : env) ev.push_back(const_cast<char*>(e.c_str()));
  ev.push_back(nullptr);
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  pid_t pid = ::fork();
#else
  pid_t pid;
  { std::lock_guard<std::mutex> l(fork_mu); pid = ::fork(); }   // not between another thread's pipe() and its fcntl
#endif
  if (pid < 0) { if (why) *why = std::strerror(errno); return false; }
  if (pid == 0) {
    Fd fds[3] = {in, out, err};
    for (int i = 0; i < 3; ++i) {
      if (fds[i] == kNoFd) { int nf = ::open("/dev/null", O_RDWR); ::dup2(nf, i); continue; }
      if (fds[i] != i) ::dup2(static_cast<int>(fds[i]), i);
      else ::fcntl(i, F_SETFD, 0);
    }
    if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) _exit(126);
    ::signal(SIGPIPE, SIG_DFL);
    ::execve(argv[0].c_str(), av.data(), ev.data());
    _exit(errno == ENOENT ? 127 : 126);
  }
  p.pid = pid;
  p.handle = pid;
  return true;
}
int wait(Proc& p) {
  if (!p.pid) return 127;
  int st = 0;
  while (::waitpid(p.pid, &st, 0) < 0 && errno == EINTR) {}
  p.pid = 0;
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
  return 1;
}
void kill_proc(Proc& p) { if (p.pid) ::kill(p.pid, SIGTERM); }
int self_pid() { return static_cast<int>(::getpid()); }
std::string find_program(const std::string& name, const std::string& path_var, const std::string& cwd) {
  if (name.find('/') != std::string::npos) { std::string h = host_path(cwd, name); return is_file(h) ? h : std::string(); }
  size_t a = 0;
  while (a <= path_var.size()) {
    size_t b = path_var.find(':', a);
    if (b == std::string::npos) b = path_var.size();
    std::string d = path_var.substr(a, b - a);
    a = b + 1;
    if (d.empty()) d = ".";
    std::string c = d + "/" + name;
    if (is_executable(c)) return c;
  }
  return {};
}
std::string self_exe() {
  char b[4096];
#ifdef __APPLE__
  uint32_t size = sizeof b;
  if (::_NSGetExecutablePath(b, &size) != 0) return "aios-sh";
  char real[PATH_MAX];
  return ::realpath(b, real) ? std::string(real) : std::string(b);
#else
  ssize_t n = ::readlink("/proc/self/exe", b, sizeof b - 1);
  return n > 0 ? std::string(b, static_cast<size_t>(n)) : "aios-sh";
#endif
}
void sleep_ms(long ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
std::string error_text(int code) { return std::strerror(code); }
}  // namespace os
#endif

namespace os {
int64_t now_seconds() { return static_cast<int64_t>(std::time(nullptr)); }
std::string absolute(const std::string& cwd, const std::string& p) {
#ifdef _WIN32
  std::string s = p;
  for (auto& c : s) if (c == '\\') c = '/';
  bool abs = (s.size() >= 2 && s[1] == ':') || (s.size() >= 2 && s[0] == '/' && s[1] == '/');
  std::string base = cwd;
  for (auto& c : base) if (c == '\\') c = '/';
  std::string joined = abs ? s : (s.size() && s[0] == '/' ? base.substr(0, 2) + s : base + "/" + s);
  // normalise: keep "X:" (or the UNC "//server/share") and resolve . and ..
  std::string head;
  size_t start = 0;
  if (joined.size() >= 2 && joined[1] == ':') { head = std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(joined[0])))) + ":"; start = 2; }
  else if (joined.rfind("//", 0) == 0) { size_t a = joined.find('/', 2); size_t b = a == std::string::npos ? a : joined.find('/', a + 1); head = joined.substr(0, b); start = b == std::string::npos ? joined.size() : b; }
  std::vector<std::string> parts;
  size_t i = start;
  while (i <= joined.size()) {
    size_t j = joined.find('/', i);
    if (j == std::string::npos) j = joined.size();
    std::string seg = joined.substr(i, j - i);
    i = j + 1;
    if (seg.empty() || seg == ".") continue;
    if (seg == "..") { if (!parts.empty()) parts.pop_back(); continue; }
    parts.push_back(seg);
  }
  std::string out = head;
  for (const auto& seg : parts) out += "\\" + seg;
  if (out == head) out += "\\";
  return out;
#else
  std::string joined = (!p.empty() && p[0] == '/') ? p : cwd + "/" + p;
  std::vector<std::string> parts;
  size_t i = 0;
  while (i <= joined.size()) {
    size_t j = joined.find('/', i);
    if (j == std::string::npos) j = joined.size();
    std::string seg = joined.substr(i, j - i);
    i = j + 1;
    if (seg.empty() || seg == ".") continue;
    if (seg == "..") { if (!parts.empty()) parts.pop_back(); continue; }
    parts.push_back(seg);
  }
  std::string out;
  for (const auto& seg : parts) out += "/" + seg;
  return out.empty() ? "/" : out;
#endif
}
}  // namespace os

bool is_null_device(const std::string& p) {
  std::string l = lower(p);
  return l == "/dev/null" || l == "nul" || l == "nul:" || l == "\\\\.\\nul";
}

std::string host_path(const std::string& cwd, const std::string& p) {
#ifdef _WIN32
  if (p.empty()) return cwd;
  if (is_null_device(p)) return "NUL";
  std::string s = p;
  // MSYS spellings: /c/x -> C:/x, /tmp -> the temp dir, /dev/stdin and friends are left to the caller.
  if (s.size() >= 2 && s[0] == '/' && std::isalpha(static_cast<unsigned char>(s[1])) && (s.size() == 2 || s[2] == '/'))
    s = std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(s[1])))) + ":" + (s.size() == 2 ? "/" : s.substr(2));
  else if (s == "/tmp" || s.rfind("/tmp/", 0) == 0)
    s = os::temp_dir() + s.substr(4);
  return os::absolute(cwd, s);
#else
  if (p.empty()) return cwd;
  return os::absolute(cwd, p);
#endif
}

}  // namespace aiossh
