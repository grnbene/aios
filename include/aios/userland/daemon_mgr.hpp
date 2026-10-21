#pragma once
// The machine's service manager. A service is a program the machine keeps
// running: it is started in dependency order, its process is supervised, and
// when it dies the manager decides from the unit's restart policy whether to
// bring it back. Everything here is about a real process on the substrate —
// the manager holds handles from hal::process and nothing else.
#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include "aios/core/result.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/process.hpp"
#include "aios/hal/sync.hpp"

namespace aios::userland {

enum class ServiceStatus { Stopped, Running, Failed, Restarting, Exited };
const char* service_status_name(ServiceStatus s);

enum class RestartPolicy { Never, OnFailure, Always };
bool parse_restart_policy(const std::string& s, RestartPolicy& out);
const char* restart_policy_name(RestartPolicy p);

struct ServiceUnit {
  std::string name;
  std::string command;                    // as written, for display
  std::vector<std::string> argv;          // as executed
  std::vector<std::string> dependencies;
  std::vector<std::pair<std::string, std::string>> environment;
  std::vector<std::string> unset_environment;   // stripped before environment is applied
  std::string working_dir;
  std::string output_path;                // where the service's own output goes

  // Whether this service should be brought up when the machine starts.
  // Distinct from the restart policy on purpose: "restart when you die" and
  // "be running in the first place" are different questions, and conflating
  // them is how you end up unable to express either. Nothing this manager
  // starts outlives it — by design — so without this a service was registered
  // for good and running only until the next restart.
  bool at_boot = false;

  RestartPolicy restart = RestartPolicy::OnFailure;
  uint32_t restart_limit = 5;             // restarts allowed inside one window
  uint64_t restart_window_ns = 60000000000ull;
  uint64_t restart_delay_ns = 200000000ull;

  ServiceStatus status = ServiceStatus::Stopped;
  hal::ProcessHandle process = hal::kNoProcess;
  uint64_t started_ns = 0;
  uint64_t stopped_ns = 0;
  uint32_t restart_count = 0;             // over the unit's whole life
  uint32_t restarts_in_window = 0;
  uint64_t window_started_ns = 0;
  uint64_t next_attempt_ns = 0;
  int last_exit_code = 0;
  int last_signal = 0;
  std::string last_error;
};

// What one pass of the supervisor did, so a caller can say what changed
// instead of guessing from a status table.
struct SupervisionEvent {
  std::string service;
  std::string what;      // "exited", "restarted", "gave up"
  int exit_code = 0;
  int signal = 0;
};

class DaemonManager {
 public:
  explicit DaemonManager(hal::Host& host);
  ~DaemonManager();

  Result<void> register_service(const std::string& name, const std::string& command,
                                const std::vector<std::string>& deps = {});
  Result<void> register_unit(const ServiceUnit& unit);
  Result<void> forget_service(const std::string& name);

  // Starts the unit's dependencies first, then the unit. Refuses a cycle.
  Result<void> start_service(const std::string& name);
  Result<void> stop_service(const std::string& name);
  Result<void> restart_service(const std::string& name);

  // One pass over every unit: reap what has died, restart what policy says to
  // restart. A supervisor nobody asks supervises nothing, so something has to
  // call this — the console does, and so does the watchdog.
  std::vector<SupervisionEvent> supervise();

  ServiceStatus query_status(const std::string& name) const;
  Result<ServiceUnit> get_service(const std::string& name) const;
  std::vector<ServiceUnit> list_services() const;

 private:
  Result<void> launch(ServiceUnit& u, uint64_t now_ns);
  Result<void> start_locked(const std::string& name, std::vector<std::string>& visiting);

  hal::Host& host_;
  mutable hal::Mutex mu_;
  std::unordered_map<std::string, ServiceUnit> services_;
};

// Splits a command line the way a shell would for the simple cases: words,
// single and double quotes, backslash escapes. A service unit names a program
// and its arguments; it is not a shell script, and pretending otherwise is how
// a service manager ends up unable to run a path with a space in it.
std::vector<std::string> split_command(const std::string& command);

}  // namespace aios::userland
