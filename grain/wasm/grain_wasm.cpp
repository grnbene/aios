// Grain in a browser: the language compiled to WebAssembly with Emscripten, driven from JavaScript.
//
// One entry point runs a program and answers with JSON: what it printed, what its last expression was, or the
// compile or run error with its line. The VM is the one the `grain` program runs - the same sources - with the
// portable standard library and no host that a page does not have (no processes, sockets or database driver).
#include <emscripten.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "aios/userland/grain.hpp"

namespace aios::userland::grain { void install_mail_builtins(Vm& vm); }   // unavailable.cpp

namespace {

using namespace aios::userland::grain;

void json_string(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += (char)c;
    }
  }
  out += '"';
}

// The file builtins' storage: Emscripten's in-memory file system, ordinary paths from "/". What a program writes
// lasts as long as the module does (the page), and goes nowhere else.
struct PageFiles final : Files {
  bool read(const std::string& path, std::string& out, std::string& error) override {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) { error = "is a directory"; return false; }
    std::ifstream f(path, std::ios::binary);
    if (!f) { error = std::filesystem::exists(path, ec) ? "cannot open" : "no such file"; return false; }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
  }
  bool write(const std::string& path, const std::string& data, bool append, std::string& error) override {
    std::ofstream f(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!f) { error = "cannot open for writing"; return false; }
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.close();
    if (!f) { error = "write failed"; return false; }
    return true;
  }
  bool list(const std::string& path, std::vector<FileEntry>& out, std::string& error) override {
    std::error_code ec;
    std::filesystem::directory_iterator it(path.empty() ? "." : path, ec), end;
    if (ec) { error = ec.message(); return false; }
    for (; it != end; it.increment(ec)) {
      if (ec) { error = ec.message(); return false; }
      FileEntry e;
      e.name = it->path().filename().string();
      e.dir = it->is_directory(ec);
      e.size = e.dir ? 0 : static_cast<int64_t>(it->file_size(ec));
      out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b) { return a.name < b.name; });
    return true;
  }
  bool remove(const std::string& path, std::string& error) override {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) { error = "no such file"; return false; }
    if (!std::filesystem::remove(path, ec)) { error = ec ? ec.message() : "not removed"; return false; }
    return true;
  }
  bool exists(const std::string& path) override { std::error_code ec; return std::filesystem::is_regular_file(path, ec); }
  bool make_dir(const std::string& path, bool parents, std::string& error) override {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) return true;
    const bool made = parents ? std::filesystem::create_directories(path, ec) : std::filesystem::create_directory(path, ec);
    if (!made || ec) { error = ec ? ec.message() : "not made"; return false; }
    return true;
  }
  bool remove_dir(const std::string& path, std::string& error) override {
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) { error = "no such directory"; return false; }
    if (!std::filesystem::is_empty(path, ec)) { error = ec ? ec.message() : "not empty"; return false; }
    if (!std::filesystem::remove(path, ec)) { error = ec ? ec.message() : "not removed"; return false; }
    return true;
  }
  bool is_dir(const std::string& path) override { std::error_code ec; return std::filesystem::is_directory(path, ec); }
  bool absolute(const std::string& path, std::string& out, std::string& error) override {
    std::error_code ec;
    const auto p = std::filesystem::absolute(path, ec);
    if (ec) { error = ec.message(); return false; }
    out = p.generic_string();
    return true;
  }
};

PageFiles g_files;

char* give(const std::string& s) {
  char* p = static_cast<char*>(std::malloc(s.size() + 1));
  if (p) std::memcpy(p, s.c_str(), s.size() + 1);
  return p;
}

}  // namespace

extern "C" {

// Runs `source` (named `origin` in errors); `step_limit` 0 is no limit. The answer is a JSON object:
//   {"ok": true,  "output": [printed lines], "value": "last expression as text"}
//   {"ok": false, "output": [...], "error": "...", "line": n, "column": n, "stage": "compile" | "run"}
// The caller frees it with grain_free.
EMSCRIPTEN_KEEPALIVE char* grain_run(const char* source, const char* origin, double step_limit) {
  std::string out;
  std::vector<std::string> printed;
  auto lines = [&]() {
    out += "\"output\":[";
    for (size_t i = 0; i < printed.size(); ++i) { if (i) out += ','; json_string(out, printed[i]); }
    out += ']';
  };
  auto compiled = compile_source(source ? source : "", origin && *origin ? origin : "program", true);
  if (!compiled.ok()) {
    const auto& d = compiled.errors.empty() ? Diagnostic(0, "compile error") : compiled.errors[0];
    out = "{\"ok\":false,\"stage\":\"compile\",";
    lines();
    out += ",\"error\":";
    json_string(out, d.message);
    out += ",\"line\":" + std::to_string(d.line) + ",\"column\":" + std::to_string(d.column) + "}";
    return give(out);
  }
  Vm vm;
  install_core_builtins(vm);
  install_http_builtins(vm);     // over the page's own transport (http_browser.cpp)
  install_socket_builtins(vm);   // the ones a page cannot have: each says so
  install_db_builtins(vm);
  install_mail_builtins(vm);
  vm.sink = &printed;
  vm.files = &g_files;
  vm.step_limit = step_limit > 0 ? (uint64_t)step_limit : 0;
  auto res = vm.run(compiled.script);
  if (!res.ok) {
    out = "{\"ok\":false,\"stage\":\"run\",";
    lines();
    out += ",\"error\":";
    json_string(out, res.error);
    out += ",\"line\":" + std::to_string(res.line) + ",\"column\":" + std::to_string(res.column) + "}";
    return give(out);
  }
  out = "{\"ok\":true,";
  lines();
  out += ",\"value\":";
  json_string(out, res.value.to_text());
  out += "}";
  return give(out);
}

EMSCRIPTEN_KEEPALIVE void grain_free(char* p) { std::free(p); }

}  // extern "C"
