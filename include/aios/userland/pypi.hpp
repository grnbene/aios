#pragma once
// pip, as a facility of this operating system.
//
// The same shape as the npm client next door, over the same TLS: the package
// document from the index, the version chosen here, the wheel's sha256 checked
// against what the index published, the archive opened by this machine and the
// tree written through hal::Host. Wheels only — an sdist has to be BUILT, and
// building means running someone's setup.py, which this machine cannot do and
// would not want to.
#include <cstdint>
#include <string>
#include <vector>

#include "aios/core/result.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"

namespace aios::userland {

struct PyPackage {
  std::string name;
  std::string version;
  std::string wheel;           // the filename, which names the tags it is for
  std::string resolved;        // where it came from
  std::string sha256;          // what the index said it would hash to
  uint64_t bytes = 0;
  uint64_t files = 0;
};

struct PyReport {
  std::string index;
  std::string tls;
  std::string trust_anchor;
  std::string target;          // where the packages landed
  std::string environment;     // the interpreter and platform they were chosen for
  std::vector<PyPackage> installed;
  uint64_t downloaded_bytes = 0;
  uint64_t took_ns = 0;
};

struct PyOptions {
  std::string index = "https://pypi.org";
  std::string python = "3.12";      // the interpreter the wheels must fit
  std::string platform;             // empty: taken from this machine's own architecture
  int timeout_ms = 120000;
  bool with_dependencies = true;
  size_t max_packages = 200;
};

class PyPi {
 public:
  PyPi(Kernel& kernel, Surface* actor = nullptr);

  // Install into `dir` (a site-packages-shaped directory). `spec` is "name",
  // "name==1.2.3" or "name>=1.2"; empty installs what `<dir>/requirements.txt`
  // asks for.
  Result<PyReport> install(const std::string& dir, const std::string& spec, const PyOptions& opt = {});
  Result<std::vector<std::string>> versions(const std::string& name, std::string& latest,
                                            const PyOptions& opt = {});

 private:
  Kernel& kernel_;
  Surface* actor_ = nullptr;
};

// PEP 440 ordering: release segments, then pre/post/dev, so 1.10 > 1.9 and
// 2.0rc1 < 2.0. Returns -1, 0 or 1.
int pep440_compare(const std::string& a, const std::string& b);
// The highest version in `available` that the specifier set admits
// ("", ">=1.2,<2", "==1.2.3", "~=1.4.2").
Result<std::string> pep440_pick(const std::vector<std::string>& available, const std::string& specifiers);
// Does a wheel's filename fit this interpreter and machine?
bool wheel_fits(const std::string& filename, const std::string& python, const std::string& platform);

}  // namespace aios::userland
