#pragma once
// Go modules, as a facility of this operating system.
//
// The module proxy protocol spoken over the kernel's own HTTPS, the version
// query answered here, the module zip read here, and — the part that makes it
// worth having — every module's h1 dirhash computed from the bytes that
// arrived and checked against the record the Go checksum database signed. That
// is a different scheme from npm's integrity string and PyPI's sha256: the hash
// is over a canonical listing of the archive's contents, not over the archive,
// so two proxies that pack the same source differently still agree.
//
// What lands is a vendor directory and a go.sum, which is what a Go build wants
// and what real go can check for itself.
#include <cstdint>
#include <string>
#include <vector>

#include "aios/core/result.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/userland/archive.hpp"
#include "aios/userland/sumdb.hpp"

namespace aios::userland {

struct GoModule {
  std::string path;
  std::string version;
  std::string zip_hash;        // h1:… computed here, from the bytes that arrived
  std::string mod_hash;        // h1:… of its go.mod
  std::string origin;          // the proxy that served it
  bool sum_verified = false;   // proved into the signed checksum-database tree
  bool explicitly_asked = false;
  uint64_t bytes = 0;
  uint64_t files = 0;
  std::vector<std::string> packages;
};

struct GoReport {
  std::string proxy;
  std::string sumdb;
  std::string tls;
  std::string trust_anchor;
  std::string target;
  SumTree tree;                // the tree head the sums were proved against
  std::vector<GoModule> modules;
  uint64_t downloaded_bytes = 0;
  uint64_t took_ns = 0;
};

struct GoOptions {
  std::string proxy = "https://proxy.golang.org";
  std::string sumdb = "https://sum.golang.org";
  std::string sumdb_key;       // empty: the key this kernel carries
  bool verify_sums = true;     // off means the proxy is taken at its word, and the report says so
  bool with_dependencies = true;
  int timeout_ms = 120000;
  size_t max_modules = 200;
};

class GoModules {
 public:
  GoModules(Kernel& kernel, Surface* actor = nullptr);

  // Download `spec` ("path", "path@v1.2.3", "path@latest") and everything its
  // go.mod requires into `<dir>/vendor`, writing `<dir>/go.sum` beside it.
  Result<GoReport> download(const std::string& dir, const std::string& spec, const GoOptions& opt = {});
  // What versions the proxy has, without taking any of them.
  Result<std::vector<std::string>> versions(const std::string& path, std::string& latest, const GoOptions& opt = {});
  // Re-hash what is already on disk and hold it against `<dir>/go.sum`.
  Result<GoReport> verify(const std::string& dir, const GoOptions& opt = {});

 private:
  Kernel& kernel_;
  Surface* actor_ = nullptr;
};

// The pieces, testable on their own.
//
// A module path travels in a case-folded form, because the case-insensitive
// filesystems of the world cannot keep Foo and foo apart: an upper-case letter
// becomes an exclamation mark and its lower-case self.
std::string go_escape(const std::string& path);
// dirhash h1: sha256 over a sorted listing of "<sha256 of file>  <name>\n".
std::string go_dirhash(const std::vector<ArchiveEntry>& files);
std::string go_dirhash_one(const std::string& name, ByteSpan content);

struct GoRequire { std::string path, version; bool indirect = false; };
struct GoModFile {
  std::string module;
  std::string go;
  std::vector<GoRequire> require;
};
Result<GoModFile> parse_go_mod(ByteSpan text);

}  // namespace aios::userland
