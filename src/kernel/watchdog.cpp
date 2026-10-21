#include "aios/kernel/watchdog.hpp"
#include "aios/kernel/kernel.hpp"
// The lock-reclaim half of the watchdog is about host processes, so it is
// host-only: <fstream> and the POSIX process calls do not exist on the metal,
// and including them here silently broke the bare-metal link (the image failed
// at ld with libstdc++ filebuf/locale pulled in, long after this compiled fine).
#include <chrono>
#include <cstdlib>
#if !defined(AIOS_METAL)
#include <cerrno>
#include <signal.h>
#endif

namespace aios {

KernelWatchdog::KernelWatchdog(Kernel& kernel) : kernel_(kernel) {}

void KernelWatchdog::log_event(const std::string& msg) {
  status_.event_log.push_back(msg);
  if (status_.event_log.size() > 50) {
    status_.event_log.erase(status_.event_log.begin());
  }
}

WatchdogStatus KernelWatchdog::status() const {
  return status_;
}

WatchdogStatus KernelWatchdog::check_health() {
  status_.checks_performed++;
  auto mem_stats = kernel_.memory().stats();
  status_.heap_kib = mem_stats.bytes / 1024;
  
  // The front door's pipe lock (scripts/aos.sh): $AOS_RUN/exchange.lock, the
  // run directory being AOS_RUN or <root>/run as everywhere else on this
  // machine. Only a lock whose recorded owner is dead is reclaimed; one with
  // no owner recorded is left to aos.sh, which knows how long it waits.
#if !defined(AIOS_METAL)
  const std::string run_env_s = kernel_.host().env("AOS_RUN");
  const char* run_env = run_env_s.empty() ? nullptr : run_env_s.c_str();
  const std::string run = (run_env && *run_env) ? std::string(run_env)
                        : (kernel_.config().root.empty() ? std::string("run") : kernel_.config().root + "/run");
  const std::string lock_dir = run + "/exchange.lock";
  status_.lock_pipe_ok = true;
  if (kernel_.host().exists(lock_dir)) {
    std::string pid_file = lock_dir + "/pid";
    long pid = -1;
    auto raw = kernel_.host().read_file(pid_file);
    if (raw.ok()) {
      std::string text((const char*)raw->data(), raw->size());
      size_t at = text.find_first_of("0123456789");
      if (at != std::string::npos) {
        pid = 0;
        for (; at < text.size() && text[at] >= '0' && text[at] <= '9'; ++at) pid = pid * 10 + (text[at] - '0');
      }
    }
    // kill(pid, 0): ESRCH is "no such process" on every POSIX host (a /proc
    // test called every pid dead on a host without /proc, macOS).
    const bool dead_pid = pid > 0 && !kernel_.host().process_alive(pid);
    if (dead_pid) {
      (void)kernel_.host().remove(pid_file);
      (void)kernel_.host().remove(lock_dir);
      status_.recoveries_triggered++;
      status_.last_remedy = "reclaimed " + lock_dir + " from dead PID " + std::to_string(pid);
      log_event("WATCHDOG: " + status_.last_remedy);
    } else {
      status_.lock_pipe_ok = pid > 0;   // held by a live owner, or by one not recorded
    }
  }
#endif

  // Self-heal memory fabric if objects exceed threshold
  if (mem_stats.objects > 500) {
    auto compact_res = kernel_.memory().compact_garbage(
        [this](const EntityID& e) { return kernel_.entities().exists(e); });
    if (compact_res.objects_released > 0) {
      status_.recoveries_triggered++;
      status_.last_remedy = "compacted " + std::to_string(compact_res.objects_released) + " orphaned memory objects (" + std::to_string(compact_res.bytes_reclaimed) + " bytes)";
      log_event("WATCHDOG: " + status_.last_remedy);
    }
  }

  return status_;
}

}  // namespace aios
