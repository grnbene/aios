#pragma once
// aios-git: the `git` command, built on aios' own git library (src/userland/git*.cpp). No program is
// shelled out to. What it does not implement it refuses by name, with a non-zero exit, rather than
// pretending: a caller can always tell "done" from "not done".

#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace aios_git {

// args[0] is the program name ("git" or "aios-git"); the rest is what a user typed after it.
// `env` is the environment the command sees (nullptr: this process's own). `cwd` is where it runs.
int run(const std::vector<std::string>& args,
        const std::string& cwd,
        std::ostream& out,
        std::ostream& err,
        std::istream& in,
        const std::map<std::string, std::string>* env = nullptr);

}  // namespace aios_git
