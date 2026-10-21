#pragma once
// What the console can do with processes on the bare-metal substrate. This
// header exists only where there are page tables to make an address space out
// of; a hosted build has the substrate's own processes and does not need it.
#include <cstdint>
#include <string>
#include <vector>

#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/kernel/surface.hpp"

extern "C" void metal_process_init(size_t pool_bytes);

namespace aios::metal {

struct ProcessReport {
  int pid, ppid;
  std::string name, state, ended;
  uint64_t ticks, syscalls, pages;
  int exit_code;
};

// Load an ELF image and give it an address space, a stack and the authority of
// `actor`. Nothing runs until the scheduler is entered.
// `quiet` discards what the task writes to fd 1/2: the idle learner's own
// workload must not print over whatever the person at the console is doing.
Result<int> process_spawn(const std::string& name, ByteSpan image, const Surface& actor, bool quiet = false);
// Make the console a task, so the scheduler can take the CPU off it too.
void process_become_console(Kernel& kernel, const Surface& console);
// Halt until every task but the console has finished; returns the context
// switches it took to get there.
uint64_t process_wait_all();
// The same, for just these pids: a caller that started its own tasks waits for
// those and reaps those, and leaves everyone else's exit status alone.
uint64_t process_wait(const std::vector<int>& pids);
size_t process_reap(const std::vector<int>& pids);
bool process_any_live();
Result<void> process_signal(int pid, int signal);
std::vector<ProcessReport> process_list();
ProcessReport process_console();
size_t process_reap();
void process_memory(uint64_t* pages_total, uint64_t* pages_live, uint64_t* switches);

}  // namespace aios::metal
