#pragma once
// Serving a site written in grain: the web server hands each request that is not
// a static file (or PHP) to the site's `handle(req)` function. See
// grain_serve.cpp for what the script sees and what it may return.
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <memory>

#include "aios/hal/host.hpp"
#include "aios/userland/grain.hpp"

namespace aios::net {

struct GrainSite {
  hal::Host* host = nullptr;
  std::string root;            // the site's document root: all a handler's file access is confined to it
  std::string app;             // the script, relative to root (or absolute)
  uint64_t max_steps = 200000000;   // instructions one request may run; 0 = unlimited
};

struct GrainRequest {
  std::string method, target, path, query, client_ip, host;
  bool https = false;
  std::vector<std::pair<std::string, std::string>> headers;
  // The request body. A small one is read before the handler runs and is `req.body`; a large or
  // chunked one is not, and the handler pulls it with body_read(). Either way read_body is the
  // way to the rest of it: it fills `out` (empty when the body is finished) and returns 0, or
  // 1 when it could not be read and 2 when it ran past the vhost's limit.
  std::vector<uint8_t> body;
  bool body_streamed = false;
  std::function<int(size_t max, std::vector<uint8_t>& out)> read_body;
};

struct GrainResponse {
  int status = 200;
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<uint8_t> body;
  std::string error;           // set when the script failed: the caller answers 500
  bool ui_app = false;         // the script has no handle() but builds a ui.pane: serve it as a web app (grain_webui) instead
  int body_error = 0;          // 1 the request body could not be read, 2 it was too large (400 / 413 rather than 500)
};

// File access for a site's scripts: every path is relative to `root`, and `..` or an absolute path is an error.
std::shared_ptr<aios::userland::grain::Files> make_rooted_files(hal::Host& host, const std::string& root);

// Runs the site's script and calls its `handle(req)`; false (with `error`) when it fails.
bool grain_serve_request(const GrainSite& site, const GrainRequest& req, GrainResponse& out);

}  // namespace aios::net
