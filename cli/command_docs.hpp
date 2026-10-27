// What each console command and subcommand does (cli/command_docs.cpp): read by `help`, `help <cmd>`, `man <cmd>`
// and scripts/gen-reference.py.
#pragma once

#include <string>
#include <vector>

#include "out.hpp"

namespace aios::cli {

struct SubDoc {
  const char* words;   // the subcommand and its aliases, separated by |
  const char* usage;
  const char* what;
};

struct CommandDoc {
  const char* name;
  const char* section;
  const char* line;   // the usage shown in the `help` list
  const char* what;
  std::vector<SubDoc> subs;
};

const std::vector<CommandDoc>& command_docs();
// A command by its name or one of its aliases; nullptr when there is none.
const CommandDoc* find_command_doc(const std::string& name);
// One command in full: its line, aliases, and every subcommand.
void print_command_help(Out& o, const CommandDoc& d);

}  // namespace aios::cli
