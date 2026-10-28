// http_get / http_post / http_request in a page: the same builtins (grain_std_http.cpp, one copy) over a different
// transport. channel::http_send_noted is the seam: the native one speaks HTTP/1.1 over sockets and TLS, this one
// hands the request to the page.
//
// The page supplies `Module.grainHttp(requestJson) -> answer`, a synchronous function: in a Web Worker that is a
// synchronous XMLHttpRequest, in Node a helper thread (grain/wasm/http-sync.mjs). The request is
//   {"method", "url", "headers": [[name, value], ...], "body": "<base64>", "timeout_ms"}
// and the answer is lines: "ok", status, reason, header count, then name and value lines for each header, then the
// body as base64 - or "error" and a message. A browser follows redirects and undoes gzip itself, so a redirect arrives
// as its final answer and the response has no content-encoding; follow_redirects: false cannot be honoured.
#include <emscripten.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "aios/channel/http_client.hpp"
#include "aios/core/base64.hpp"

namespace aios::channel {

namespace {

EM_JS(char*, host_http, (const char* request), {
  let answer;
  const f = Module.grainHttp;
  if (typeof f !== 'function') {
    answer = 'error\nthis page gave the VM no network (Module.grainHttp is not set)';
  } else {
    try { answer = f(UTF8ToString(request)); }
    catch (e) { answer = 'error\n' + String(e && e.message ? e.message : e); }
  }
  const n = lengthBytesUTF8(answer) + 1;
  const p = _malloc(n);
  stringToUTF8(answer, p, n);
  return p;
});

void json_string(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += (char)c;
    }
  }
  out += '"';
}

// The next line of `text` from `at` (which moves past it).
bool next_line(const std::string& text, size_t& at, std::string& line) {
  if (at > text.size()) return false;
  const size_t nl = text.find('\n', at);
  if (nl == std::string::npos) { line = text.substr(at); at = text.size() + 1; return true; }
  line = text.substr(at, nl - at);
  at = nl + 1;
  return true;
}

}  // namespace

Result<HttpResponse> http_send_noted(const HttpRequest& request, HttpTransportNote*) {
  const auto& ep = request.endpoint;
  std::string url = std::string(ep.secure ? "https" : "http") + "://" + ep.host;
  if (ep.port != (ep.secure ? 443 : 80)) url += ":" + std::to_string(ep.port);
  url += ep.path;

  std::string json = "{\"method\":";
  json_string(json, request.method);
  json += ",\"url\":";
  json_string(json, url);
  json += ",\"headers\":[";
  bool first = true;
  auto header = [&](const std::string& name, const std::string& value) {
    if (!first) json += ',';
    first = false;
    json += '[';
    json_string(json, name);
    json += ',';
    json_string(json, value);
    json += ']';
  };
  bool has_auth = false;
  for (const auto& h : request.headers) {
    header(h.first, h.second);
    if (h.first.size() == 13 && strncasecmp(h.first.c_str(), "authorization", 13) == 0) has_auth = true;
  }
  if (!ep.user.empty() && !has_auth) {
    const std::string cred = ep.user + ":" + ep.password;
    header("Authorization", "Basic " + base64_encode(ByteSpan(reinterpret_cast<const uint8_t*>(cred.data()), cred.size())));
  }
  json += "],\"body\":\"" + base64_encode(ByteSpan(request.body.data(), request.body.size())) + "\",\"timeout_ms\":" +
          std::to_string(request.timeout_ms) + "}";

  char* raw = host_http(json.c_str());
  const std::string answer = raw ? raw : "";
  std::free(raw);

  size_t at = 0;
  std::string line;
  if (!next_line(answer, at, line)) return Result<HttpResponse>(Status::IoError, "the page gave no answer");
  if (line != "ok") {
    std::string why;
    next_line(answer, at, why);
    return Result<HttpResponse>(Status::IoError, why.empty() ? "the request failed" : why);
  }
  HttpResponse r;
  std::string count;
  next_line(answer, at, line);
  r.status = std::atoi(line.c_str());
  next_line(answer, at, r.reason);
  next_line(answer, at, count);
  for (int i = 0, n = std::atoi(count.c_str()); i < n; ++i) {
    std::string name, value;
    next_line(answer, at, name);
    next_line(answer, at, value);
    r.headers.emplace_back(std::move(name), std::move(value));
  }
  std::string body64;
  next_line(answer, at, body64);
  if (!body64.empty()) {
    auto body = base64_decode(body64);
    if (!body.ok()) return Result<HttpResponse>(Status::Corrupt, "the page's body was not base64");
    r.body = std::move(*body);
  }
  return r;
}

}  // namespace aios::channel
