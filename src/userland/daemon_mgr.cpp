#include "aios/userland/daemon_mgr.hpp"

#include <algorithm>

namespace aios::userland {

const char* service_status_name(ServiceStatus s) {
  switch (s) {
    case ServiceStatus::Stopped:    return "stopped";
    case ServiceStatus::Running:    return "running";
    case ServiceStatus::Failed:     return "failed";
    case ServiceStatus::Restarting: return "restarting";
    case ServiceStatus::Exited:     return "exited";
  }
  return "unknown";
}

const char* restart_policy_name(RestartPolicy p) {
  switch (p) {
    case RestartPolicy::Never:     return "never";
    case RestartPolicy::OnFailure: return "on-failure";
    case RestartPolicy::Always:    return "always";
  }
  return "unknown";
}

bool parse_restart_policy(const std::string& s, RestartPolicy& out) {
  if (s == "never" || s == "no") { out = RestartPolicy::Never; return true; }
  if (s == "on-failure" || s == "onfailure" || s == "failure") { out = RestartPolicy::OnFailure; return true; }
  if (s == "always") { out = RestartPolicy::Always; return true; }
  return false;
}

std::vector<std::string> split_command(const std::string& command) {
  std::vector<std::string> out;
  std::string word;
  bool have_word = false;
  char quote = 0;
  for (size_t i = 0; i < command.size(); ++i) {
    char ch = command[i];
    if (quote) {
      if (ch == quote) { quote = 0; continue; }
      if (quote == '"' && ch == '\\' && i + 1 < command.size()) { word += command[++i]; continue; }
      word += ch;
      continue;
    }
    if (ch == '\'' || ch == '"') { quote = ch; have_word = true; continue; }
    if (ch == '\\' && i + 1 < command.size()) { word += command[++i]; have_word = true; continue; }
    if (ch == ' ' || ch == '\t' || ch == '\n') {
      if (have_word) { out.push_back(word); word.clear(); have_word = false; }
      continue;
    }
    word += ch;
    have_word = true;
  }
  if (have_word) out.push_back(word);
  return out;
}

DaemonManager::DaemonManager(hal::Host& host) : host_(host) {}

DaemonManager::~DaemonManager() {
  // Nothing this manager started outlives it: a supervisor that exits leaving
  // orphans behind is how a machine ends up with two of everything.
  hal::LockGuard l(mu_);
  for (auto& [name, u] : services_) {
    if (u.process != hal::kNoProcess) (void)hal::release_process(u.process);
    u.process = hal::kNoProcess;
  }
}

Result<void> DaemonManager::register_service(const std::string& name, const std::string& command,
                                             const std::vector<std::string>& deps) {
  ServiceUnit u;
  u.name = name;
  u.command = command;
  u.argv = split_command(command);
  u.dependencies = deps;
  return register_unit(u);
}

Result<void> DaemonManager::register_unit(const ServiceUnit& unit) {
  if (unit.name.empty()) return Result<void>(Status::Invalid, "a service needs a name");
  ServiceUnit u = unit;
  if (u.argv.empty()) u.argv = split_command(u.command);
  if (u.argv.empty()) return Result<void>(Status::Invalid, "service '" + u.name + "' names no program to run");
  if (u.command.empty()) {
    for (const auto& w : u.argv) u.command += (u.command.empty() ? "" : " ") + w;
  }
  hal::LockGuard l(mu_);
  auto it = services_.find(u.name);
  if (it != services_.end() && it->second.status == ServiceStatus::Running) {
    return Result<void>(Status::Conflict, "service '" + u.name + "' is running; stop it before redefining it");
  }
  u.status = ServiceStatus::Stopped;
  u.process = hal::kNoProcess;
  services_[u.name] = u;
  return Ok();
}

Result<void> DaemonManager::forget_service(const std::string& name) {
  hal::LockGuard l(mu_);
  auto it = services_.find(name);
  if (it == services_.end()) return Result<void>(Status::NotFound, "no service '" + name + "'");
  for (const auto& [other, u] : services_) {
    if (other == name) continue;
    if (std::find(u.dependencies.begin(), u.dependencies.end(), name) != u.dependencies.end()) {
      return Result<void>(Status::Conflict, "service '" + other + "' still depends on '" + name + "'");
    }
  }
  if (it->second.process != hal::kNoProcess) (void)hal::release_process(it->second.process);
  services_.erase(it);
  return Ok();
}

Result<void> DaemonManager::launch(ServiceUnit& u, uint64_t now_ns) {
  hal::ProcessSpawn spec;
  spec.argv = u.argv;
  spec.working_dir = u.working_dir;
  spec.env = u.environment;
  spec.unset_env = u.unset_environment;
  spec.output_path = u.output_path;
  auto h = hal::spawn_process(spec);
  if (!h.ok()) {
    u.status = ServiceStatus::Failed;
    u.last_error = h.error().message;
    return h.error();
  }
  u.process = *h;
  u.status = ServiceStatus::Running;
  u.started_ns = now_ns;
  u.stopped_ns = 0;
  u.last_error.clear();
  return Ok();
}

Result<void> DaemonManager::start_locked(const std::string& name, std::vector<std::string>& visiting) {
  auto it = services_.find(name);
  if (it == services_.end()) return Result<void>(Status::NotFound, "no service '" + name + "'");
  ServiceUnit& u = it->second;

  if (u.status == ServiceStatus::Running) {
    auto st = hal::process_status(u.process);
    if (st.ok() && st->running) return Ok();
  }

  if (std::find(visiting.begin(), visiting.end(), name) != visiting.end()) {
    std::string chain;
    for (const auto& v : visiting) chain += v + " → ";
    return Result<void>(Status::Invalid, "dependency cycle: " + chain + name);
  }
  visiting.push_back(name);
  for (const auto& dep : u.dependencies) {
    if (!services_.count(dep)) {
      visiting.pop_back();
      return Result<void>(Status::NotFound, "service '" + name + "' depends on '" + dep + "', which is not registered");
    }
    auto r = start_locked(dep, visiting);
    if (!r.ok()) { visiting.pop_back(); return r; }
  }
  visiting.pop_back();

  // start_locked may have re-entered and moved the table; find the unit again.
  ServiceUnit& unit = services_[name];
  return launch(unit, host_.monotonic_ns());
}

Result<void> DaemonManager::start_service(const std::string& name) {
  hal::LockGuard l(mu_);
  std::vector<std::string> visiting;
  return start_locked(name, visiting);
}

Result<void> DaemonManager::stop_service(const std::string& name) {
  hal::LockGuard l(mu_);
  auto it = services_.find(name);
  if (it == services_.end()) return Result<void>(Status::NotFound, "no service '" + name + "'");
  ServiceUnit& u = it->second;
  // A service stopped by hand is stopped, not crashed: clear the restart
  // accounting so the supervisor does not bring it back a moment later.
  u.status = ServiceStatus::Stopped;
  u.restarts_in_window = 0;
  u.next_attempt_ns = 0;
  if (u.process == hal::kNoProcess) return Ok();

  (void)hal::signal_process(u.process, hal::ProcessSignal::Terminate);
  bool gone = false;
  for (int i = 0; i < 100; ++i) {
    auto st = hal::process_status(u.process);
    if (!st.ok() || !st->running) { gone = true; if (st.ok()) { u.last_exit_code = st->exit_code; u.last_signal = st->terminating_signal; } break; }
    hal::sleep_ms(20);
  }
  if (!gone) (void)hal::signal_process(u.process, hal::ProcessSignal::Kill);
  (void)hal::release_process(u.process);
  u.process = hal::kNoProcess;
  u.stopped_ns = host_.monotonic_ns();
  return Ok();
}

Result<void> DaemonManager::restart_service(const std::string& name) {
  AIOS_TRY(stop_service(name));
  return start_service(name);
}

std::vector<SupervisionEvent> DaemonManager::supervise() {
  std::vector<SupervisionEvent> events;
  hal::LockGuard l(mu_);
  uint64_t now_ns = host_.monotonic_ns();

  for (auto& [name, u] : services_) {
    if (u.status == ServiceStatus::Running && u.process != hal::kNoProcess) {
      auto st = hal::process_status(u.process);
      if (!st.ok()) continue;
      if (st->running) continue;

      u.last_exit_code = st->exit_code;
      u.last_signal = st->terminating_signal;
      u.stopped_ns = now_ns;
      (void)hal::release_process(u.process);
      u.process = hal::kNoProcess;

      bool clean = st->exit_code == 0 && st->terminating_signal == 0;
      u.status = clean ? ServiceStatus::Exited : ServiceStatus::Failed;
      events.push_back(SupervisionEvent{name, "exited", st->exit_code, st->terminating_signal});

      bool wants_restart = u.restart == RestartPolicy::Always ||
                           (u.restart == RestartPolicy::OnFailure && !clean);
      if (!wants_restart) continue;

      if (u.window_started_ns == 0 || now_ns - u.window_started_ns > u.restart_window_ns) {
        u.window_started_ns = now_ns;
        u.restarts_in_window = 0;
      }
      if (u.restarts_in_window >= u.restart_limit) {
        u.status = ServiceStatus::Failed;
        u.last_error = "restarted " + std::to_string(u.restarts_in_window) + " times in the window; not trying again";
        events.push_back(SupervisionEvent{name, "gave up", st->exit_code, st->terminating_signal});
        continue;
      }
      u.status = ServiceStatus::Restarting;
      u.next_attempt_ns = now_ns + u.restart_delay_ns;
      continue;
    }

    if (u.status == ServiceStatus::Restarting && now_ns >= u.next_attempt_ns) {
      u.restarts_in_window++;
      u.restart_count++;
      auto r = launch(u, now_ns);
      events.push_back(SupervisionEvent{name, r.ok() ? "restarted" : "gave up", u.last_exit_code, u.last_signal});
    }
  }
  return events;
}

ServiceStatus DaemonManager::query_status(const std::string& name) const {
  hal::LockGuard l(mu_);
  auto it = services_.find(name);
  return it != services_.end() ? it->second.status : ServiceStatus::Stopped;
}

Result<ServiceUnit> DaemonManager::get_service(const std::string& name) const {
  hal::LockGuard l(mu_);
  auto it = services_.find(name);
  if (it == services_.end()) return Result<ServiceUnit>(Status::NotFound, "no service '" + name + "'");
  return it->second;
}

std::vector<ServiceUnit> DaemonManager::list_services() const {
  hal::LockGuard l(mu_);
  std::vector<ServiceUnit> list;
  for (const auto& [name, s] : services_) list.push_back(s);
  std::sort(list.begin(), list.end(), [](const ServiceUnit& a, const ServiceUnit& b) { return a.name < b.name; });
  return list;
}

}  // namespace aios::userland
