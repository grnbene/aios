#pragma once
// The Rust registry, on the same footing as npm and PyPI: the index document
// over the machine's own TLS, the version chosen here, the .crate archive's
// sha256 checked against the checksum crates.io publishes, and the source
// unpacked through hal::Host into a vendor directory a build can be pointed at.
//
// Vendoring, not building. Compiling Rust is a compiler's job, and this machine
// does not run one; what it can honestly do is put the exact, verified source
// where a compiler will find it.
#include <cstdint>
#include <string>
#include <vector>

#include "aios/core/result.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"

namespace aios::userland {

struct CratePackage {
  std::string name;
  std::string version;
  std::string resolved;
  std::string checksum;        // what the registry published
  uint64_t bytes = 0;
  uint64_t files = 0;
};

struct CrateReport {
  std::string registry;
  std::string tls;
  std::string trust_anchor;
  std::string target;
  std::vector<CratePackage> vendored;
  uint64_t downloaded_bytes = 0;
  uint64_t took_ns = 0;
};

struct CrateOptions {
  std::string registry = "https://crates.io";
  int timeout_ms = 120000;
  bool with_dependencies = true;      // normal dependencies; dev and build ones are the builder's business
  size_t max_packages = 200;
};

class Crates {
 public:
  Crates(Kernel& kernel, Surface* actor = nullptr);
  // Vendor into `<dir>/vendor/<name>-<version>/`.
  Result<CrateReport> vendor(const std::string& dir, const std::string& spec, const CrateOptions& opt = {});
  Result<std::vector<std::string>> versions(const std::string& name, std::string& latest,
                                            const CrateOptions& opt = {});

 private:
  Kernel& kernel_;
  Surface* actor_ = nullptr;
};

}  // namespace aios::userland
