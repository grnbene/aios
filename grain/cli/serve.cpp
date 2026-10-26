// A small HTTP/1.1 server for one program: GET the page, POST its events. Each connection is
// handled on its own thread and closed after the response; WebUi keeps the sessions. A thread per
// connection rather than a fixed pool: an event stream holds its connection for as long as someone
// watches, and a pool would cap the viewers at its size.
#include "serve.hpp"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#include "aios/hal/sync.hpp"

#include "aios/userland/grain_webui.hpp"

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace grain_cli {

namespace g = aios::userland::grain;

#if defined(_WIN32)

int serve_ui(const std::string&, std::function<bool(std::string&, std::string&, std::string&)>, std::function<void(g::Vm&)>, const ServeOptions&) {
  std::cerr << "grain: serving to a browser is not available on Windows yet\n";
  return 2;
}

#else

namespace {

size_t g_max_body = 1 << 20;

// false: the connection is unusable. too_big: it was a well-formed request with a body past the limit.
bool read_request(int fd, std::string& head, std::string& body, bool& too_big) {
  std::string buf;
  char tmp[4096];
  size_t end = std::string::npos;
  while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
    if (buf.size() > 64 * 1024) return false;
    const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
    if (n <= 0) return false;
    buf.append(tmp, (size_t)n);
  }
  head = buf.substr(0, end);
  body = buf.substr(end + 4);
  // Content-Length, case-insensitively
  size_t want = 0;
  std::string lower = head;
  for (char& c : lower) c = (char)std::tolower((unsigned char)c);
  const size_t cl = lower.find("\r\ncontent-length:");
  if (cl != std::string::npos) want = (size_t)std::strtoull(head.c_str() + cl + 17, nullptr, 10);
  if (want > g_max_body) { too_big = true; return false; }
  while (body.size() < want) {
    const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
    if (n <= 0) return false;
    body.append(tmp, (size_t)n);
  }
  body.resize(want);
  return true;
}

std::string header_of(const std::string& head, const char* name) {
  std::string lower = head;
  for (char& c : lower) c = (char)std::tolower((unsigned char)c);
  const std::string key = std::string("\r\n") + name + ":";
  const size_t at = lower.find(key);
  if (at == std::string::npos) return "";
  size_t b = at + key.size();
  while (b < head.size() && head[b] == ' ') ++b;
  const size_t e = head.find("\r\n", b);
  return head.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

bool send_all(int fd, const std::string& data) {
  size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
    if (n <= 0) return false;
    off += (size_t)n;
  }
  return true;
}

// The reason phrase on a status line: clients mostly ignore it, but "201 Error" reads as a fault.
const char* reason_phrase(int status) {
  switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 418: return "I'm a teapot";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return status >= 500 ? "Server Error" : status >= 400 ? "Client Error" : status >= 300 ? "Redirect" : status >= 200 ? "OK" : "Informational";
  }
}

void answer(int fd, g::WebUi& ui) {
  std::string head, body;
  bool too_big = false;
  if (!read_request(fd, head, body, too_big)) {
    if (too_big) {
      static const char refuse[] = "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      (void)::send(fd, refuse, sizeof refuse - 1, 0);
    }
    ::close(fd);
    return;
  }
  const size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
  g::WebUi::Request req;
  req.method = head.substr(0, sp1);
  std::string target = sp1 == std::string::npos || sp2 == std::string::npos ? "/" : head.substr(sp1 + 1, sp2 - sp1 - 1);
  req.path = target.substr(0, target.find('?'));
  req.query = target.find('?') == std::string::npos ? "" : target.substr(target.find('?') + 1);
  req.cookie = header_of(head, "cookie");
  req.xhr = header_of(head, "x-grain-ui") == "1";
  req.tree = header_of(head, "accept").find("application/x-grain-ui") != std::string::npos;
  req.stream = header_of(head, "accept").find("text/event-stream") != std::string::npos;
  req.body = body;
  for (size_t at = head.find("\r\n"); at != std::string::npos;) {
    const size_t b = at + 2, e = head.find("\r\n", b);
    const std::string line = head.substr(b, e == std::string::npos ? std::string::npos : e - b);
    const size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string name = line.substr(0, colon), value = line.substr(colon + 1);
      for (char& c : name) c = (char)std::tolower((unsigned char)c);
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
      while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
      req.headers.push_back({name, value});
    }
    at = e;
  }
  sockaddr_storage peer{};
  socklen_t peer_len = sizeof peer;
  if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peer_len) == 0) {
    char ip[INET6_ADDRSTRLEN] = {0};
    if (peer.ss_family == AF_INET) ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&peer)->sin_addr, ip, sizeof ip);
    else if (peer.ss_family == AF_INET6) ::inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(&peer)->sin6_addr, ip, sizeof ip);
    req.remote = ip;
  }
  if (req.stream && req.method == "GET") {
    // The page asked to be told when the data changes: hold the connection and send each change as it happens.
    g::WebUi::Stream stream;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string frame, error;
    if (ui.open_stream(req, stream, headers, frame, error)) {
      std::string out = "HTTP/1.1 200 OK\r\n";
      for (const auto& h : headers) out += h.first + ": " + h.second + "\r\n";
      out += "Connection: close\r\n\r\n" + frame;
      bool alive = send_all(fd, out);
      while (alive && ui.next_frame(stream, 15000, frame)) alive = send_all(fd, frame.empty() ? std::string(": keepalive\n\n") : frame);
      ui.close_stream(stream);
      ::close(fd);
      return;
    }
  }
  bool headers_sent = false;
  auto on_headers = [&](int status, const std::vector<std::pair<std::string, std::string>>& headers) -> bool {
    if (headers_sent) return true;
    headers_sent = true;
    const char* reason = reason_phrase(status);
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
    for (const auto& h : headers) out += h.first + ": " + h.second + "\r\n";
    out += "Connection: close\r\n\r\n";
    return send_all(fd, out);
  };
  auto on_chunk = [&](const std::string& chunk) -> bool {
    if (!headers_sent) {
      on_headers(200, {{"Content-Type", "text/event-stream; charset=utf-8"}, {"Cache-Control", "no-cache"}});
    }
    return send_all(fd, chunk);
  };

  g::WebUi::Response res;
  if (req.path == "/favicon.ico") { res.status = 204; }
  else res = ui.handle(req, on_headers, on_chunk);

  if (headers_sent) {
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
    return;
  }

  const char* reason = reason_phrase(res.status);
  std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " + reason + "\r\n";
  for (const auto& h : res.headers) out += h.first + ": " + h.second + "\r\n";
  out += "Content-Length: " + std::to_string(res.body.size()) + "\r\nConnection: close\r\n\r\n";
  if (req.method != "HEAD") out += res.body;
  send_all(fd, out);
  ::shutdown(fd, SHUT_RDWR);
  ::close(fd);
}

}  // namespace

int serve_ui(const std::string& name,
             std::function<bool(std::string&, std::string&, std::string&)> load_source,
             std::function<void(g::Vm&)> setup, const ServeOptions& opt) {
  g_max_body = opt.max_body;
  g::WebUi::Host host;
  host.origin = name;
  host.show_errors = opt.show_errors;
  host.load_source = load_source;
  host.current_version = [load_source](std::string& version, std::string& error) {
    std::string source;
    return load_source(source, version, error);   // a program on disk is small; reading it is the version check
  };
  host.setup = [setup](g::Vm& vm) -> std::shared_ptr<void> { setup(vm); return nullptr; };
  auto ui = std::make_shared<g::WebUi>(std::move(host));

  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) { std::cerr << "grain: cannot open a socket\n"; return 2; }
  const int yes = 1;
  ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
  sockaddr_in addr;
  std::memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)opt.port);
  if (opt.host.empty() || opt.host == "0.0.0.0" || opt.host == "*") {
    addr.sin_addr.s_addr = INADDR_ANY;
  } else if (::inet_pton(AF_INET, opt.host.c_str(), &addr.sin_addr) != 1) {
    std::cerr << "grain: " << opt.host << " is not an IPv4 address\n";
    return 64;
  }
  if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(listener, 64) != 0) {
    std::cerr << "grain: cannot listen on " << opt.host << ":" << opt.port << "\n";
    return 2;
  }
  if (opt.host == "0.0.0.0" || opt.host == "*" || opt.host.empty()) {
    std::cout << "grain: " << name << " is listening on all IPs at port " << opt.port << "  (Ctrl-C to stop)\n" << std::flush;
  } else {
    std::cout << "grain: " << name << " is at http://" << opt.host << ":" << opt.port << "/  (Ctrl-C to stop)\n" << std::flush;
  }
  for (;;) {
    const int fd = ::accept(listener, nullptr, nullptr);
    if (fd < 0) continue;
    aios::hal::Thread conn([fd, ui]() { answer(fd, *ui); });
    if (!conn.joinable()) {   // no thread to be had (the process is at its limit): this client is turned away, not left hanging
      std::cerr << "grain: cannot start a thread for a connection; closing it\n";
      ::close(fd);
      continue;
    }
    conn.detach();
  }
}

#endif

}  // namespace grain_cli
