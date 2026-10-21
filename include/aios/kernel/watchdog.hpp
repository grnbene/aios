#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "aios/core/result.hpp"

namespace aios {

class Kernel;

struct WatchdogStatus {
  uint64_t checks_performed = 0;
  uint64_t recoveries_triggered = 0;
  uint64_t heap_kib = 0;
  bool lock_pipe_ok = true;
  bool daemon_healthy = true;
  std::string last_remedy;
  std::vector<std::string> event_log;
};

class KernelWatchdog {
 public:
  explicit KernelWatchdog(Kernel& kernel);
  
  WatchdogStatus check_health();
  void log_event(const std::string& msg);
  WatchdogStatus status() const;

 private:
  Kernel& kernel_;
  WatchdogStatus status_;
};

}  // namespace aios
