#pragma once
// npm, as a facility of this operating system.
//
// Not a program the machine shells out to: the registry protocol spoken over
// the kernel's own HTTPS, the package document read here, the version chosen
// here, the tarball's integrity checked against the hash the registry
// published, gunzipped and untarred by this machine, and the tree written
// through hal::Host. Dependencies are resolved transitively, so `npm install`
// of one package lands the packages it needs with it.
#include <cstdint>
#include <string>
#include <vector>

#include "aios/core/result.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"

namespace aios::userland {

struct NpmPackage {
  std::string name;
  std::string version;
  std::string resolved;        // the tarball this machine actually fetched
  std::string integrity;       // what the registry said it would hash to
  uint64_t bytes = 0;          // the tarball as it arrived
  uint64_t files = 0;          // what came out of it
  bool already_present = false;
};

struct NpmReport {
  std::string registry;
  std::string tls;             // the transport this machine negotiated
  std::string trust_anchor;
  std::vector<NpmPackage> installed;
  uint64_t downloaded_bytes = 0;
  uint64_t took_ns = 0;
  std::string root;            // where node_modules is
};

struct NpmOptions {
  std::string registry = "https://registry.npmjs.org";
  int timeout_ms = 120000;
  bool with_dependencies = true;
  size_t max_packages = 400;   // a runaway dependency graph stops, and says so
};

class Npm {
 public:
  // Reaching the registry spends network authority and writing the tree spends
  // storage authority; both are checked through the Surface before anything
  // leaves or lands.
  Npm(Kernel& kernel, Surface* actor = nullptr);

  // Install into `<dir>/node_modules`. `spec` is "name" or "name@version" or
  // "name@^1.2.3"; empty installs what `<dir>/package.json` asks for.
  Result<NpmReport> install(const std::string& dir, const std::string& spec, const NpmOptions& opt = {});
  // What the registry knows about a package, without installing it.
  Result<std::vector<std::string>> versions(const std::string& name, std::string& latest,
                                            const NpmOptions& opt = {});

 private:
  Kernel& kernel_;
  Surface* actor_ = nullptr;
};

// The pieces the installer needs, useful on their own and testable on their own.
Result<ByteBuffer> gunzip(ByteSpan gz);
struct TarEntry { std::string path; ByteBuffer data; uint32_t mode = 0644; bool is_dir = false; };
Result<std::vector<TarEntry>> untar(ByteSpan tar);
// The highest version in `available` that `range` admits. Supports exact, "*",
// "latest", ^, ~, >=, and x-ranges; anything else is refused by name.
Result<std::string> semver_pick(const std::vector<std::string>& available, const std::string& range);
int semver_compare(const std::string& a, const std::string& b);

}  // namespace aios::userland
