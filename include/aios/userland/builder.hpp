#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "aios/core/result.hpp"
#include "aios/kernel/kernel.hpp"

namespace aios::userland {

struct BuildResult {
  bool success = false;
  int exit_code = 0;
  std::string output;
  std::string errors;
  uint64_t build_duration_ms = 0;
};

struct TestResult {
  bool success = false;
  int total_tests = 0;
  int passed_tests = 0;
  int failed_tests = 0;
  std::string summary;
  std::vector<std::string> failed_test_names;
};

class BuilderManager {
 public:
  explicit BuilderManager(Kernel& kernel);

  BuildResult run_build(const std::string& target = "");
  TestResult run_tests();
  BuildResult auto_remediate_and_commit(const std::string& commit_msg);
  // `git -C <repo> add -A` then `git -C <repo> commit -m <msg>`, run with no
  // shell: the message is one argv element, never parsed by sh.
  BuildResult commit_all(const std::string& commit_msg, const std::string& repo = "aios");

 private:
  Kernel& kernel_;
};

}  // namespace aios::userland
