#pragma once
// Running another program is a property of the substrate, not of the kernel:
// a hosted machine has processes, a bare-metal one has whatever it has. The
// service manager above this boundary supervises handles and never names a
// pid, a signal number or a file descriptor.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "aios/core/result.hpp"

namespace aios::hal {

using ProcessHandle = int64_t;
constexpr ProcessHandle kNoProcess = -1;

struct ProcessSpawn {
  std::vector<std::string> argv;
  std::string working_dir;
  std::vector<std::pair<std::string, std::string>> env;
  // Names to REMOVE from whatever environment this process inherits, applied
  // before env is added. Setting a variable is not enough: a child inherits
  // the supervisor's whole environment, and the harm is done by a variable
  // that is PRESENT — one naming another agent's conversation file makes the
  // child answer a question nobody asked. There is no way to express that by
  // assignment, so a service has to be able to say "not this one".
  std::vector<std::string> unset_env;
  // Start from an empty environment (then env is added): for a program that must not see the
  // supervisor's own variables at all, keys and tokens included.
  bool clear_env = false;
  std::string output_path;   // stdout and stderr appended here; empty discards them
};

struct ProcessStatus {
  bool running = false;
  bool exited = false;
  int exit_code = 0;
  int terminating_signal = 0;   // 0 unless the substrate killed it
};

enum class ProcessSignal : uint8_t { Interrupt, Terminate, Kill, Hangup };

Result<ProcessHandle> spawn_process(const ProcessSpawn& spec);
// Reports the child's condition and reaps it once it has finished, so a
// supervisor that keeps asking does not leave a table of dead entries behind.
Result<ProcessStatus> process_status(ProcessHandle h);
// Blocks until the child finishes or the timeout expires. A caller that runs a
// tool to get its answer needs this; a supervisor never does.
Result<ProcessStatus> wait_process(ProcessHandle h, uint32_t timeout_ms);
Result<void> signal_process(ProcessHandle h, ProcessSignal sig);
// Stop asking about this child. A still-running child is killed first: a
// supervisor that forgets a process it started has abandoned it.
Result<void> release_process(ProcessHandle h);

struct ToolResult {
  int exit_code = 0;
  std::string output;   // stdout and stderr, in the order the tool wrote them
};

// Runs a program to completion and gives back what it said. This is how the
// machine reaches a tool the substrate provides — a speech engine, a
// recogniser — without every caller re-deriving fork, redirection and reaping.
Result<ToolResult> run_tool(const std::vector<std::string>& argv, uint32_t timeout_ms,
                            const std::string& working_dir = {});
// The same, with the whole spawn spec (its environment, clear_env); output_path comes from the call.
Result<ToolResult> run_tool(const ProcessSpawn& spec, uint32_t timeout_ms);

// The first of these names that exists as an executable on this substrate, or
// empty when none of them do. A caller uses the answer to say what is missing.
std::string find_program(const std::vector<std::string>& names);

// A path this substrate will let the machine write a scratch file to, unique
// per call. Speech tools trade in files; something has to name them.
Result<std::string> scratch_path(const std::string& suffix);

}  // namespace aios::hal
