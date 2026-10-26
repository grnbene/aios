#include "tools.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>

namespace g = aios::userland::grain;
namespace fs = std::filesystem;

namespace grain_cli {

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitFailed = 2;

bool read_file_lines(const std::string& path, std::vector<std::string>& lines) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }
  return true;
}

void print_debug_help(std::ostream& os) {
  os << "Grain Interactive Debugger Commands:\n"
     << "  s, step             step into next line/statement\n"
     << "  n, next             step over current line\n"
     << "  c, continue         resume execution until next breakpoint or error\n"
     << "  b, break <line>     set a breakpoint at line number\n"
     << "  d, delete [line]    delete breakpoint at line (or all breakpoints)\n"
     << "  p, print <name>     print value of variable in current frame\n"
     << "  locals              display all local variables in current call frame\n"
     << "  globals             display all global variable names\n"
     << "  bt, where           display call stack backtrace\n"
     << "  l, list [start]     display source lines around current line\n"
     << "  h, help             show this help text\n"
     << "  q, quit             terminate debugging session\n";
}

} // namespace

int debug_command(const std::vector<std::string>& args) {
  if (args.empty() || args[0] == "-h" || args[0] == "--help") {
    std::cout << "grain debug <file.gr> [args...]: run script in interactive debugger\n";
    print_debug_help(std::cout);
    return kExitOk;
  }

  std::string script_path = args[0];
  std::vector<std::string> script_args(args.begin() + 1, args.end());

  std::vector<std::string> source_lines;
  if (!read_file_lines(script_path, source_lines)) {
    std::cerr << "grain debug: cannot read " << script_path << "\n";
    return kExitFailed;
  }

  std::string full_source;
  for (const auto& l : source_lines) { full_source += l + "\n"; }

  auto compiled = g::compile_source(full_source, script_path);
  if (!compiled.ok()) {
    std::cerr << "grain debug: script compilation failed:\n";
    for (const auto& d : compiled.errors) {
      std::cerr << "  " << script_path << ":" << d.line << ": " << d.message << "\n";
    }
    return kExitFailed;
  }

  g::Vm vm;
  prepare_vm(vm, script_args);

  vm.jit_enabled = false;          // native code never reaches the line hook
  vm.debug_state.enabled = true;
  vm.debug_state.step_line = true; // pause at first instruction
  vm.debug_state.current_file = script_path;

  auto print_context = [&](int line, int context_lines = 2) {
    int start = std::max(1, line - context_lines);
    int end = std::min(static_cast<int>(source_lines.size()), line + context_lines);
    for (int i = start; i <= end; ++i) {
      char marker = (i == line) ? '>' : ' ';
      std::cout << " " << marker << " " << i << " | " << source_lines[i - 1] << "\n";
    }
  };

  vm.debug_state.on_break = [&](g::Vm& v, const std::string& file, int line) {
    std::cout << "\n[DEBUG] Paused at " << script_path << ":" << line << (file.empty() || file == "<script>" || file == script_path ? "" : " in " + file) << "\n";
    print_context(line);

    for (;;) {
      std::cout << "(grain-dbg) " << std::flush;
      std::string cmd_line;
      if (!std::getline(std::cin, cmd_line)) {
        // EOF reached
        v.debug_state.step_line = false;
        v.debug_state.step_over = false;
        break;
      }
      std::istringstream iss(cmd_line);
      std::string cmd;
      iss >> cmd;

      if (cmd.empty()) continue;

      if (cmd == "s" || cmd == "step") {
        v.debug_state.step_line = true;
        v.debug_state.step_over = false;
        break;
      } else if (cmd == "n" || cmd == "next") {
        v.debug_state.step_line = false;
        v.debug_state.step_over = true;
        v.debug_state.step_over_depth = v.frame_depth();
        break;
      } else if (cmd == "c" || cmd == "continue") {
        v.debug_state.step_line = false;
        v.debug_state.step_over = false;
        break;
      } else if (cmd == "b" || cmd == "break") {
        int bp_line = 0;
        if (iss >> bp_line) {
          v.debug_state.breakpoints.insert(bp_line);
          std::cout << "Breakpoint set at line " << bp_line << "\n";
        } else {
          std::cout << "Usage: b <line>\n";
        }
      } else if (cmd == "d" || cmd == "delete") {
        int bp_line = 0;
        if (iss >> bp_line) {
          v.debug_state.breakpoints.erase(bp_line);
          std::cout << "Breakpoint removed from line " << bp_line << "\n";
        } else {
          v.debug_state.breakpoints.clear();
          std::cout << "All breakpoints cleared\n";
        }
      } else if (cmd == "p" || cmd == "print") {
        std::string var_name;
        iss >> var_name;
        if (var_name.empty()) {
          std::cout << "Usage: p <name>\n";
        } else {
          g::Value* gv = v.find_global(var_name);
          if (gv) {
            std::cout << var_name << " (global) = " << gv->repr() << "\n";
          } else {
            bool found = false;
            for (const auto& [n, val] : v.current_locals()) {
              if (n == var_name) {
                std::cout << var_name << " = " << val.repr() << "\n";
                found = true;
                break;
              }
            }
            if (!found) {
              std::cout << "Variable '" << var_name << "' not found in current scope\n";
            }
          }
        }
      } else if (cmd == "locals") {
        std::cout << "Locals in current frame:\n";
        for (const auto& [n, val] : v.current_locals()) {
          std::cout << "  " << n << " = " << val.repr() << "\n";
        }
      } else if (cmd == "globals") {
        std::cout << "Globals:\n";
        for (const auto& gn : v.global_names()) {
          std::cout << "  " << gn << "\n";
        }
      } else if (cmd == "bt" || cmd == "where") {
        std::cout << "Call stack:\n";
        auto trace = v.stack_trace();
        for (size_t i = 0; i < trace.size(); ++i) {
          std::cout << "  #" << i << " " << trace[i] << "\n";
        }
      } else if (cmd == "l" || cmd == "list") {
        int target = line;
        int arg_line = 0;
        if (iss >> arg_line) target = arg_line;
        print_context(target, 5);
      } else if (cmd == "h" || cmd == "help") {
        print_debug_help(std::cout);
      } else if (cmd == "q" || cmd == "quit") {
        std::cout << "Session terminated by user.\n";
        std::exit(0);
      } else {
        std::cout << "Unknown command '" << cmd << "'. Type 'h' or 'help' for commands.\n";
      }
    }
  };

  std::cout << "Grain Debugger 0.1.0 — debugging " << script_path << "\n";
  std::cout << "Type 'c' to continue, 's' to step, 'h' for help.\n";

  auto res = vm.run(compiled.script);
  if (!res.ok) {
    std::cerr << script_path << ":" << res.line << ": runtime error: " << res.error << "\n";
    for (const auto& t : res.trace) std::cerr << "  in " << t << "\n";
    return kExitFailed;
  }

  std::cout << "[DEBUG] Program finished successfully.\n";
  return kExitOk;
}

} // namespace grain_cli
