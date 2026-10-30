#pragma once
// A check's scratch root: a fresh directory under /tmp for each run, removed when the check ends. A fixed root
// (/tmp/aios-asql-round-check) was removed by every run as it started, so two runs at once - two sessions on one
// machine, ctest -j - deleted each other's live store; and a run that died left its store for the next to read.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

struct CheckRoot {
  std::string path;
  // `prefix` names the check: /tmp/aios-asql-round-check becomes /tmp/aios-asql-round-check-Ab12Cd.
  explicit CheckRoot(const std::string& prefix) : path(prefix + "-XXXXXX") {
    if (!mkdtemp(path.data())) {
      std::perror(("mkdtemp " + path).c_str());
      std::exit(1);
    }
  }
  ~CheckRoot() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  CheckRoot(const CheckRoot&) = delete;
  CheckRoot& operator=(const CheckRoot&) = delete;
};
