#pragma once
// A grain program with a ui.pane, served to browsers and native clients (grain_webui.cpp).
//
// The program is the one a desktop window runs. Its `view` is shown, and when its data changes - because
// someone pressed a button, because a timer (`every`, `after`) fired, because another viewer changed shared
// data - every screen showing it is updated. Nothing in the program says how: no sockets, no hooks, no polling.
// The data is the program's own variables; what is on screen follows it.
//
// Behind that: each visitor gets a copy of the program (a session, kept by a cookie), or, for a pane made with
// "shared": true, all of them share one. A ticker fires timers and re-renders; a change is pushed to browsers
// over server-sent events (and a native client polls for it cheaply).
//
// This is the host-independent part: a request in, a response out. The standalone `grain serve` and
// aios-webserver's `grain_ui` directive are thin wrappers.
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "aios/hal/sync.hpp"
#include "aios/userland/grain.hpp"

namespace aios::userland::grain {

class WebUi {
 public:
  struct Request;
  struct Response;
  struct Host {
    // A string that changes when the program does (cheap: a file's mtime and size). A session whose program changed starts over.
    std::function<bool(std::string& version, std::string& error)> current_version;
    // The program's source and its version, read when a session starts.
    std::function<bool(std::string& source, std::string& version, std::string& error)> load_source;
    // Gives a new session's VM its builtins and its files. The returned object lives as long as the session.
    std::function<std::shared_ptr<void>(Vm&)> setup;
    std::string origin = "app.gr";      // the program's name, for error messages
    std::string cookie_name = "grain_sid";   // one per program, so two apps on a host keep their own sessions
    uint64_t max_steps = 200000000;     // instructions one event, render or timer may run; 0 = unlimited
    size_t max_sessions = 256;          // the least recently used goes first
    int idle_seconds = 1800;            // a session with nobody watching and untouched this long is dropped
    bool show_errors = false;           // put a program's error in the page (for development); otherwise only the log
    // A route given to another machine (on_machine): the request, answered there. Returns false when `member` is this
    // machine (the route's own handler answers here). Unset: every route answers here.
    std::function<bool(const std::string& member, const Request& req, Response& res)> forward;
    // The same for a request whose answer is a stream (Accept: text/event-stream): the member's route writes, and
    // each piece comes through on_headers / on_chunk as it is written. false: not carried (answered as above).
    std::function<bool(const std::string& member, const Request& req, StreamHeaderCallback on_headers, StreamChunkCallback on_chunk)> forward_stream;
    // Whether `member` (an id or a prefix) is this machine. A pane given to it (ui.pane "machine") is answered here.
    // Unset: no member is.
    std::function<bool(const std::string& member)> is_here;
  };

  struct Request {
    std::string method;
    std::string path;
    std::string query;       // after the '?'
    std::string cookie;      // the Cookie header
    bool xhr = false;        // the request carries "X-Grain-UI: 1", which a cross-site page cannot add
    bool https = false;
    bool tree = false;       // Accept: application/x-grain-ui - the widgets as data for a native client, not HTML
    bool stream = false;     // Accept: text/event-stream - the caller wants updates pushed (use open_stream)
    std::string body;        // application/x-www-form-urlencoded, for POST
    std::vector<std::pair<std::string, std::string>> headers;   // every header, names lowercased: a pane's routes read them
    std::string remote;      // the client's address, as the server saw it
  };

  struct Response {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
  };

  struct Session;

  // An open server-sent-events stream. The caller loops on next_frame() and writes each frame to the client.
  struct Stream {
    std::shared_ptr<Session> session;
    uint64_t seen = 0;
    bool tree = false;
  };

  explicit WebUi(Host host);
  ~WebUi();

  // GET: the page (or, for a native client, the widgets as data; `?poll=N` answers 204 until something changed
  // since N). POST with X-Grain-UI: 1: an event, answered with the redrawn widgets.
  Response handle(const Request& req, StreamHeaderCallback on_headers = nullptr, StreamChunkCallback on_chunk = nullptr);

  // For a request with `stream` set: opens the stream. `headers` go on the 200 (Content-Type: text/event-stream
  // and a Set-Cookie for a new session); `first` is the current frame. False with `error` when there is none.
  bool open_stream(const Request& req, Stream& out, std::vector<std::pair<std::string, std::string>>& headers,
                   std::string& first, std::string& error);
  // Waits up to timeout_ms for the data to change. True with a frame to send; True with an empty frame when nothing
  // changed (send a comment to keep the connection alive); False when the stream should end.
  bool next_frame(Stream& st, int timeout_ms, std::string& frame);
  void close_stream(Stream& st);

  size_t sessions() const;
  // Where errors are reported (stderr when empty).
  std::function<void(const std::string&)> log;

 private:
  Host host_;
  mutable std::mutex mu_;
  std::map<std::string, std::shared_ptr<Session>> sessions_;
  std::shared_ptr<Session> shared_;     // the one copy, for a pane with "shared": true
  bool member_known_ = false;           // given_member(): learnt for member_version_ of the program
  std::string member_version_, member_;
  aios::hal::Thread ticker_;
  bool stop_ = false;
  std::condition_variable stop_cv_;

  std::shared_ptr<Session> session_for(const Request& req, std::string& new_id, std::string& error);
  std::shared_ptr<Session> make_session(std::string& error);
  void reap_locked();
  void report(const std::string& msg) const;
  void start_ticker();
  void tick();
  std::string given_member(const Request& req);   // the member a ui.pane "machine" gives the pane to, "" for here
  bool refresh_locked(Session& s);           // re-render; true (and a new seq) when what is on screen changed
  void frame_locked(Session& s, bool tree, std::string& out);
};

}  // namespace aios::userland::grain
