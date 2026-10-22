#include "bundle.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>

#include "aios/core/bytes.hpp"
#include "aios/crypto/sha256.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace grain_cli {

namespace g = aios::userland::grain;
namespace fs = std::filesystem;

namespace {

constexpr int kExitOk = 0, kExitUsage = 64, kExitFailed = 1;
constexpr uint32_t kPackVersion = 1;
constexpr size_t kTrailer = 8 + 8 + 32;            // magic, length, SHA-256
constexpr size_t kSearchFromEnd = 4u << 20;        // a code signature appended after the pack is far smaller

// "GRAINPK1", kept out of the runtime's own bytes: the search for a pack scans
// the executable, and the literal would be found in a runtime that carries none.
std::string magic() {
  static const unsigned char enc[8] = {'G' ^ 0x5A, 'R' ^ 0x5A, 'A' ^ 0x5A, 'I' ^ 0x5A, 'N' ^ 0x5A, 'P' ^ 0x5A, 'K' ^ 0x5A, '1' ^ 0x5A};
  std::string m(8, '\0');
  for (int i = 0; i < 8; ++i) m[static_cast<size_t>(i)] = static_cast<char>(enc[i] ^ 0x5A);
  return m;
}

void put_u32(std::string& out, uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); }
void put_u64(std::string& out, uint64_t v) { for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); }
uint64_t get_le(const std::string& s, size_t at, int bytes) {
  uint64_t v = 0;
  for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | static_cast<unsigned char>(s[at + static_cast<size_t>(i)]);
  return v;
}

std::string sha256_of(const std::string& s) {
  const auto d = aios::crypto::sha256(aios::ByteSpan(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
  return std::string(reinterpret_cast<const char*>(d.data()), d.size());
}

// The pack's bytes: version, entry count, then each entry's name and data.
// The program is the first entry.
std::string serialize(const Pack& p) {
  std::string out;
  put_u32(out, kPackVersion);
  put_u32(out, static_cast<uint32_t>(p.files.size()));
  auto entry = [&](const std::string& name, const std::string& data) {
    put_u32(out, static_cast<uint32_t>(name.size()));
    out += name;
    put_u64(out, data.size());
    out += data;
  };
  entry(p.main_name, p.files.at(p.main_name));
  for (const auto& [name, data] : p.files)
    if (name != p.main_name) entry(name, data);
  const std::string sum = sha256_of(out);   // over the body
  const uint64_t body_len = out.size();
  out += magic();
  put_u64(out, body_len);
  out += sum;
  return out;
}

bool parse(const std::string& body, Pack& p, std::string& error) {
  size_t at = 0;
  auto need = [&](size_t n) { return at + n <= body.size(); };
  if (!need(8)) { error = "the program packed onto this executable is cut short"; return false; }
  const uint32_t version = static_cast<uint32_t>(get_le(body, 0, 4)), count = static_cast<uint32_t>(get_le(body, 4, 4));
  at = 8;
  if (version != kPackVersion) { error = "the program packed onto this executable is format " + std::to_string(version) + "; this grain reads format 1"; return false; }
  for (uint32_t i = 0; i < count; ++i) {
    if (!need(4)) { error = "the packed program is cut short"; return false; }
    const size_t nlen = static_cast<size_t>(get_le(body, at, 4));
    at += 4;
    if (!need(nlen + 8)) { error = "the packed program is cut short"; return false; }
    std::string name = body.substr(at, nlen);
    at += nlen;
    const size_t dlen = static_cast<size_t>(get_le(body, at, 8));
    at += 8;
    if (!need(dlen)) { error = "the packed program is cut short"; return false; }
    if (i == 0) p.main_name = name;
    p.files[name] = body.substr(at, dlen);
    at += dlen;
  }
  if (at != body.size() || p.main_name.empty()) { error = "the packed program has bytes it does not account for"; return false; }
  return true;
}

// A pack ending somewhere in `file` (its magic found searching from the end).
// false with no error: none there.
bool pack_in(const std::string& file, Pack& p, std::string& error) {
  const std::string m = magic();
  const size_t floor = file.size() > kSearchFromEnd ? file.size() - kSearchFromEnd : 0;
  size_t pos = file.size();
  while (pos > floor) {
    const size_t at = file.rfind(m, pos - 1);
    if (at == std::string::npos || at < floor) return false;
    pos = at;
    if (at + kTrailer > file.size()) continue;
    // The length is the pack body's, which ends where the magic starts.
    const uint64_t len = get_le(file, at + 8, 8);
    if (len > at) continue;
    const size_t start = at - static_cast<size_t>(len);
    const std::string body = file.substr(start, static_cast<size_t>(len));
    const std::string sum = file.substr(at + 16, 32);
    if (sha256_of(body) != sum) {
      error = "the program packed onto this executable is damaged (its checksum does not match)";
      return false;
    }
    return parse(body, p, error);
  }
  return false;
}

bool read_all(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

std::string self_path() {
#if defined(_WIN32)
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return "";
    if (n < buf.size()) { buf.resize(n); break; }
    buf.resize(buf.size() * 2);
  }
  return fs::path(buf).u8string();
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buf(size, '\0');
  if (_NSGetExecutablePath(buf.data(), &size) != 0) return "";
  buf.resize(std::strlen(buf.c_str()));
  std::error_code ec;
  const auto canon = fs::canonical(buf, ec);
  return ec ? buf : canon.string();
#else
  std::error_code ec;
  const auto p = fs::read_symlink("/proc/self/exe", ec);
  return ec ? std::string() : p.string();
#endif
}

// The runtime's own bytes without any pack it carries: building from a
// bundled program's executable must not stack a second program onto the first.
std::string bare_runtime(const std::string& file) {
  Pack p;
  std::string err;
  const std::string m = magic();
  if (!pack_in(file, p, err)) return file;
  const size_t at = file.rfind(m);
  const size_t start = at - static_cast<size_t>(get_le(file, at + 8, 8));
  return file.substr(0, start);
}

bool is_pe(const std::string& f) { return f.size() > 2 && f[0] == 'M' && f[1] == 'Z'; }
bool is_macho(const std::string& f) {
  if (f.size() < 4) return false;
  const uint32_t m = static_cast<uint32_t>(get_le(f, 0, 4));
  return m == 0xFEEDFACF || m == 0xCAFEBABE || m == 0xBEBAFECA;
}

bool add_include(const fs::path& path, Pack& pack, std::string& error) {
  std::error_code ec;
  auto store = [&](const fs::path& file) -> bool {
    std::string data;
    if (!read_all(file.string(), data)) { error = "cannot read " + file.string(); return false; }
    std::string name = file.lexically_normal().generic_string();
    if (name.rfind("./", 0) == 0) name = name.substr(2);
    pack.files[name] = std::move(data);
    return true;
  };
  if (fs::is_directory(path, ec)) {
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(path, ec), end; it != end && !ec; it.increment(ec))
      if (it->is_regular_file(ec)) files.push_back(it->path());
    if (ec) { error = path.string() + ": " + ec.message(); return false; }
    std::sort(files.begin(), files.end());
    for (const auto& f : files)
      if (!store(f)) return false;
    return true;
  }
  if (fs::is_regular_file(path, ec)) return store(path);
  error = "no such file or directory: " + path.string();
  return false;
}

bool write_executable(const fs::path& out, const std::string& bytes, std::string& error) {
  std::ofstream f(out, std::ios::binary | std::ios::trunc);
  if (!f) { error = "cannot write " + out.string(); return false; }
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  f.close();
  if (!f) { error = "writing " + out.string() + " failed"; return false; }
#if !defined(_WIN32)
  ::chmod(out.string().c_str(), 0755);
#endif
  return true;
}

std::string info_plist(const std::string& name) {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
         "<plist version=\"1.0\">\n<dict>\n"
         "  <key>CFBundleExecutable</key><string>" + name + "</string>\n"
         "  <key>CFBundleIdentifier</key><string>org.grain-lang.app." + name + "</string>\n"
         "  <key>CFBundleName</key><string>" + name + "</string>\n"
         "  <key>CFBundlePackageType</key><string>APPL</string>\n"
         "  <key>CFBundleShortVersionString</key><string>1.0</string>\n"
         "  <key>NSHighResolutionCapable</key><true/>\n"
         "</dict>\n</plist>\n";
}

}  // namespace

bool find_own_pack(Pack& pack, std::string& error) {
  const std::string exe = self_path();
  if (exe.empty()) return false;
#if defined(__APPLE__)
  // Inside X.app/Contents/MacOS: the pack is a file beside the untouched runtime.
  const fs::path res = fs::path(exe).parent_path().parent_path() / "Resources" / "program.grainpk";
  std::error_code ec;
  if (fs::path(exe).parent_path().filename() == "MacOS" && fs::is_regular_file(res, ec)) {
    std::string file;
    if (!read_all(res.string(), file)) { error = "cannot read " + res.string(); return false; }
    if (pack_in(file, pack, error)) return true;
    if (error.empty()) error = res.string() + " is not a grain program pack";
    return false;
  }
#endif
  std::string file;
  if (!read_all(exe, file)) return false;
  return pack_in(file, pack, error);
}

int build_command(const std::vector<std::string>& a) {
  std::string main_path, out_path, runtime_path;
  std::vector<std::string> includes;
  bool app = false;
  for (size_t i = 0; i < a.size(); ++i) {
    if ((a[i] == "-o" || a[i] == "--output") && i + 1 < a.size()) out_path = a[++i];
    else if (a[i] == "--include" && i + 1 < a.size()) includes.push_back(a[++i]);
    else if (a[i] == "--runtime" && i + 1 < a.size()) runtime_path = a[++i];
    else if (a[i] == "--app") app = true;
    else if (main_path.empty() && a[i].rfind("-", 0) != 0) main_path = a[i];
    else { std::cerr << "grain build: unexpected " << a[i] << "\n"; return kExitUsage; }
  }
  if (main_path.empty()) {
    std::cerr << "grain build <file.gr> [-o out] [--include file-or-dir]... [--runtime grain-binary] [--app]\n";
    return kExitUsage;
  }
  // The program must compile: a build that ships a syntax error is not a build.
  Pack pack;
  std::string src;
  if (!read_all(main_path, src)) { std::cerr << "grain build: cannot read " << main_path << "\n"; return kExitUsage; }
  auto compiled = g::compile_source(src, main_path);
  if (!compiled.ok()) {
    for (const auto& d : compiled.errors) std::cerr << main_path << ":" << d.line << ": " << d.message << "\n";
    return kExitFailed;
  }
  pack.main_name = fs::path(main_path).filename().generic_string();
  pack.files[pack.main_name] = src;
  std::string error;
  for (const auto& inc : includes) {
    if (!add_include(inc, pack, error)) { std::cerr << "grain build: " << error << "\n"; return kExitFailed; }
  }

  if (runtime_path.empty()) runtime_path = self_path();
  std::string runtime;
  if (runtime_path.empty() || !read_all(runtime_path, runtime)) {
    std::cerr << "grain build: cannot read the runtime " << runtime_path << "\n";
    return kExitFailed;
  }
  runtime = bare_runtime(runtime);
  const std::string packed = serialize(pack);
  const std::string stem = fs::path(main_path).stem().string();

  if (app) {
    if (!is_macho(runtime)) { std::cerr << "grain build --app makes a macOS application, and the runtime is not a macOS executable\n"; return kExitUsage; }
    const fs::path bundle = out_path.empty() ? fs::path(stem + ".app") : fs::path(out_path);
    const std::string name = bundle.stem().string();
    std::error_code ec;
    fs::create_directories(bundle / "Contents" / "MacOS", ec);
    fs::create_directories(bundle / "Contents" / "Resources", ec);
    if (ec) { std::cerr << "grain build: " << bundle.string() << ": " << ec.message() << "\n"; return kExitFailed; }
    if (!write_executable(bundle / "Contents" / "MacOS" / name, runtime, error) ||
        !write_executable(bundle / "Contents" / "Resources" / "program.grainpk", packed, error)) {
      std::cerr << "grain build: " << error << "\n";
      return kExitFailed;
    }
    std::ofstream(bundle / "Contents" / "Info.plist") << info_plist(name);
    std::cout << "built " << bundle.string() << " (" << pack.files.size() << " file(s); the runtime is unmodified, so the app can be signed)\n";
    return kExitOk;
  }

  fs::path out = out_path.empty() ? fs::path(stem) : fs::path(out_path);
  if (is_pe(runtime) && out.extension() != ".exe") out += ".exe";
  if (!write_executable(out, runtime + packed, error)) { std::cerr << "grain build: " << error << "\n"; return kExitFailed; }
  std::cout << "built " << out.string() << " (" << (runtime.size() + packed.size()) / 1024 << " KB, " << pack.files.size() << " file(s))\n";
  if (is_macho(runtime))
    std::cerr << "note: a single-file macOS program runs as built but cannot be notarized; for distribution use --app\n";
  return kExitOk;
}

namespace {
const Pack* g_pack = nullptr;

g::Value bi_bundle_file(g::Vm&, std::vector<g::Value>& args, std::string& error) {
  if (args[0].type != g::Type::Str) { error = std::string("bundle_file() wants a string, got a ") + g::type_name(args[0].type); return g::Value::nil(); }
  auto it = g_pack->files.find(args[0].str());
  if (it == g_pack->files.end()) { error = "bundle_file(): \"" + args[0].str() + "\" was not packed into this program (grain build --include)"; return g::Value::nil(); }
  return g::Value::string(it->second);
}

g::Value bi_bundle_files(g::Vm&, std::vector<g::Value>&, std::string&) {
  std::vector<g::Value> names;
  for (const auto& [name, data] : g_pack->files) names.push_back(g::Value::string(name));
  return g::Value::list(std::move(names));
}
}  // namespace

void install_bundle_builtins(g::Vm& vm, const Pack& pack) {
  g_pack = &pack;
  vm.define_native("bundle_file", 1, 1, bi_bundle_file);
  vm.define_native("bundle_files", 0, 0, bi_bundle_files);
}

}  // namespace grain_cli
