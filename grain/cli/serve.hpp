#pragma once
// `grain serve app.gr` and `./app --web`: a program with a ui.pane, shown in a browser
// instead of a window (serve.cpp). The page, the sessions and the events are
// aios::userland::grain::WebUi; this is only the HTTP in front of it.
#include <functional>
#include <string>

#include "aios/userland/grain.hpp"

namespace grain_cli {

struct ServeOptions {
  std::string host = "127.0.0.1";   // loopback by default: a program may run commands and read files
  int port = 8080;
  bool show_errors = true;          // a program's error appears in its page: this is the developer's own machine
};

// Serves until interrupted. `load_source` gives the program and a string that changes when it does
// (empty `version` is fine for a fixed program); `setup` gives each session's VM its builtins.
int serve_ui(const std::string& name,
             std::function<bool(std::string& source, std::string& version, std::string& error)> load_source,
             std::function<void(aios::userland::grain::Vm&)> setup, const ServeOptions& opt);

}  // namespace grain_cli
