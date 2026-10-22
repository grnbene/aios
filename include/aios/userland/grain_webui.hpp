#pragma once
// A grain program with a ui.pane, served to browsers (grain_webui.cpp).
//
// The same program a desktop window runs: `ui.pane({"view": fn () { ... }})` and
// `run_panes()`. Each browser gets its own copy of the program (a session, kept by a
// cookie), shows what `view` returns as HTML, and sends clicks, Enter and field text
// back; the program's own functions run on the server and the page redraws.
//
// This is the host-independent part: it takes a request and gives a response, so the
// standalone `grain serve` and aios-webserver's `grain_ui` directive are thin wrappers.
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "aios/userland/grain.hpp"

namespace aios::userland::grain {

class WebUi {
 public:
  struct Host {
    // A string that changes when the program does (cheap: a file's mtime and size). A session whose program changed starts over.
    std::function<bool(std::string& version, std::string& error)> current_version;
    // The program's source and its version, read when a session starts.
    std::function<bool(std::string& source, std::string& version, std::string& error)> load_source;
    // Gives a new session's VM its builtins and its files. The returned object lives as long as the session.
    std::function<std::shared_ptr<void>(Vm&)> setup;
    std::string origin = "app.gr";      // the program's name, for error messages
    uint64_t max_steps = 200000000;     // instructions one event or render may run; 0 = unlimited
    size_t max_sessions = 256;          // the least recently used goes first
    int idle_seconds = 1800;            // a session untouched this long is dropped
    bool show_errors = false;           // put a program's error in the page (for development); otherwise only the log
  };

  struct Request {
    std::string method;
    std::string path;
    std::string cookie;      // the Cookie header
    bool xhr = false;        // the request carries "X-Grain-UI: 1", which a cross-site page cannot add
    bool https = false;
    std::string body;        // application/x-www-form-urlencoded, for POST
  };

  struct Response {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
  };

  explicit WebUi(Host host);
  ~WebUi();

  // GET anything but /_ui/...: the page. POST /_ui/event: an event, answered with the redrawn widgets.
  Response handle(const Request& req);

  size_t sessions() const;
  // Where errors are reported (stderr when empty).
  std::function<void(const std::string&)> log;

 private:
  struct Session;
  Host host_;
  mutable std::mutex mu_;
  std::map<std::string, std::shared_ptr<Session>> sessions_;

  std::shared_ptr<Session> session_for(const Request& req, std::string& new_id, std::string& error);
  std::shared_ptr<Session> make_session(std::string& error);
  void reap_locked();
  void report(const std::string& msg) const;
};

}  // namespace aios::userland::grain
