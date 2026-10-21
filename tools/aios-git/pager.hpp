#pragma once
// What a command's output looks like on a terminal: git's pager (less, LESS=FRX) and git's colors for log, show and
// diff. Off the terminal - a pipe, a file, a test's stream - the output is exactly what the command wrote.
#include <memory>
#include <ostream>
#include <streambuf>
#include <string>

#include "cli.hpp"

namespace aios_git {

// The global options that steer it: -p / --paginate, -P / --no-pager.
struct PagerOptions {
  bool force = false;
  bool never = false;
};

// Set up for the command in c.args: `tout` (the stream the command writes, c.out) is pointed through the coloring
// filter and, when one runs, the pager. `real` is where output goes otherwise; `real_is_stdout` says it is the
// process's standard output, the only one a pager can take over. Finished (pager waited for) when destroyed.
class Terminal {
 public:
  Terminal(Ctx& c, std::ostream& tout, std::ostream& real, bool real_is_stdout, const PagerOptions& opt);
  ~Terminal();
  Terminal(const Terminal&) = delete;
  Terminal& operator=(const Terminal&) = delete;

 private:
  void finish();
  std::ostream& tout_;
  std::unique_ptr<std::streambuf> filter_;
  std::streambuf* real_buf_ = nullptr;
  long pager_pid_ = -1;
};

}  // namespace aios_git
