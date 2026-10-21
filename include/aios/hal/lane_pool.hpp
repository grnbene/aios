#pragma once
// Run a batch of tasks side by side without creating a thread per task.
//
// Hosted builds keep one set of worker threads for the life of the process
// (one fewer than the machine's hardware threads; the caller works too) and
// hand each batch to them; the kernel's compute path used to create and join a
// thread per operation and per slice of each operation - on a training step's
// 426 small operations that was 300-800 us each, a thousand times the
// arithmetic. A task that itself calls run_tasks runs its batch inline (no
// nesting, no deadlock); a batch arriving while the pool is busy with another
// caller's runs inline too rather than wait. Bare-metal builds keep creating
// threads - there a thread is a hand-off to an idle core, and a pool that
// blocks would need mutexes the boards do not have yet.
#include <cstdint>
#include <functional>

namespace aios::hal {

// task(i) for every i in [0, n); returns when all have finished.
void run_tasks(uint32_t n, const std::function<void(uint32_t)>& task);
// How many run at once at most (workers + the caller).
uint32_t task_lanes();

}  // namespace aios::hal
