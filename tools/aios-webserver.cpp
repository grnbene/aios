// aios-webserver: a vhost-aware HTTP(S) edge server built on aios's own HAL
// sockets, TLS server, FastCGI client, and streaming HTTP layer — meant to
// replace nginx for a vhost, terminating TLS itself (SNI-dispatched, so one
// process still serves many domains on :443 the way nginx does), serving
// static files (Angular/JS builds) directly, and streaming PHP requests
// straight through to PHP-FPM over FastCGI. The one property none of the
// pieces it's built from give up: nothing here holds a full request or
// response body in memory — chunks flow client -> [this process] -> php-fpm
// and back, bounded regardless of payload size. That's the actual fix for
// the 499s / large-payload stalls this replaces nginx to get away from.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

#include "aios/core/bytes.hpp"
#include "aios/crypto/x509.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/net.hpp"
#include "aios/net/fastcgi_client.hpp"
#include "aios/net/gzip.hpp"
#include "aios/net/http_stream.hpp"
#include "aios/net/tls.hpp"
#include "aios/net/tls_conn.hpp"
#include "aios/net/tls_sni.hpp"

using namespace aios;

namespace {

bool ieq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
  return true;
}
std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r");
  size_t e = s.find_last_not_of(" \t\r");
  return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

struct VhostConfig {
  std::string server_name;
  uint16_t listen_port = 0;
  bool ssl = false;
  bool loopback_only = false;
  std::string cert_path, key_path;
  std::string root;
  std::string index = "index.html";
  std::string php_host;   // empty = static-only vhost
  uint16_t php_port = 0;
  bool php_off = false;       // isolate PHP (503) while keeping static files live
  bool php_lockdown = false;  // also lock disable_functions, allow_url_fopen=0, and open_basedir=root
  std::vector<std::string> php_admin_values;  // extra PHP_ADMIN_VALUE directives
  bool cors = false;
  std::string cors_origin = "*";
  std::string cors_methods = "GET, POST, PUT, DELETE, OPTIONS, HEAD, PATCH";
  std::string cors_headers = "Authorization, X-Device-UUID, Origin, X-Requested-With, Content-Type, Accept";
  std::string cors_max_age = "1000";
  std::vector<std::pair<std::string, std::string>> extra_headers;
  int64_t max_body_size = 0;  // 0 = unlimited — the whole point of this server
  // `deny <status> <regex>`: a request path matching the pattern (ECMAScript,
  // case-insensitive) is refused with that status before anything is read or
  // run - nginx's `location ~* ... { deny all; }`. A path segment starting
  // with '.' (except /.well-known/) is always refused with 404: a site root
  // that is a git checkout otherwise serves its whole .git directory.
  struct Deny { int status; std::regex re; std::string pattern; };
  std::vector<Deny> denies;

  std::vector<crypto::Certificate> tls_chain;
  crypto::PrivateKey tls_key;
};

std::string strip_quotes(const std::string& s) {
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

std::vector<VhostConfig> parse_config(const std::string& text) {
  std::vector<VhostConfig> vhosts;
  std::istringstream in(text);
  std::string line;
  VhostConfig* cur = nullptr;
  while (std::getline(in, line)) {
    auto hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    line = trim(line);
    if (line.empty()) continue;
    std::istringstream ls(line);
    std::string tok;
    ls >> tok;
    if (tok == "vhost") {
      std::string name;
      ls >> name;
      vhosts.emplace_back();
      cur = &vhosts.back();
      cur->server_name = name;
    } else if (tok == "}") {
      cur = nullptr;
    } else if (cur) {
      if (tok == "listen") {
        int port = 0;
        ls >> port;
        cur->listen_port = uint16_t(port);
        std::string opt;
        while (ls >> opt) {
          if (opt == "ssl") cur->ssl = true;
          if (opt == "loopback") cur->loopback_only = true;
        }
      } else if (tok == "cert") {
        ls >> cur->cert_path;
      } else if (tok == "key") {
        ls >> cur->key_path;
      } else if (tok == "root") {
        ls >> cur->root;
        while (cur->root.size() > 1 && cur->root.back() == '/') cur->root.pop_back();
      } else if (tok == "index") {
        ls >> cur->index;
        while (!cur->index.empty() && cur->index.front() == '/') cur->index.erase(cur->index.begin());
      } else if (tok == "php") {
        std::string hp;
        ls >> hp;
        if (hp == "off") {
          cur->php_off = true;
        } else {
          auto colon = hp.find(':');
          if (hp.rfind("unix:", 0) == 0) {
            // php unix:/run/php/pool.sock - php-fpm's unix socket (FastCgiClient
            // takes the whole "unix:/path" as its host; the port is unused).
            cur->php_host = hp;
            cur->php_port = 0;
          } else if (colon != std::string::npos) {
            cur->php_host = hp.substr(0, colon);
            cur->php_port = uint16_t(std::atoi(hp.substr(colon + 1).c_str()));
          }
        }
      } else if (tok == "php_off") {
        cur->php_off = true;
      } else if (tok == "php_lockdown") {
        cur->php_lockdown = true;
      } else if (tok == "php_admin_value") {
        std::string setting;
        std::getline(ls, setting);
        setting = trim(setting);
        if (!setting.empty() && setting.find('\0') == std::string::npos &&
            setting.find('\r') == std::string::npos && setting.find('\n') == std::string::npos) {
          cur->php_admin_values.push_back(setting);
        }
      } else if (tok == "cors" || tok == "access_control") {
        std::string arg;
        ls >> arg;
        cur->cors = (arg != "off" && arg != "0" && arg != "false");
      } else if (tok == "cors_origin") {
        std::string val;
        std::getline(ls, val);
        val = strip_quotes(trim(val));
        if (!val.empty()) { cur->cors = true; cur->cors_origin = val; }
      } else if (tok == "cors_methods") {
        std::string val;
        std::getline(ls, val);
        val = strip_quotes(trim(val));
        if (!val.empty()) { cur->cors = true; cur->cors_methods = val; }
      } else if (tok == "cors_headers") {
        std::string val;
        std::getline(ls, val);
        val = strip_quotes(trim(val));
        if (!val.empty()) { cur->cors = true; cur->cors_headers = val; }
      } else if (tok == "cors_max_age") {
        std::string val;
        ls >> val;
        if (!val.empty()) { cur->cors = true; cur->cors_max_age = val; }
      } else if (tok == "add_header" || tok == "header") {
        std::string hname, hval;
        ls >> hname;
        std::getline(ls, hval);
        hval = strip_quotes(trim(hval));
        if (!hname.empty() && !hval.empty() &&
            hname.find('\r') == std::string::npos && hname.find('\n') == std::string::npos &&
            hval.find('\r') == std::string::npos && hval.find('\n') == std::string::npos) {
          cur->extra_headers.push_back({hname, hval});
        }
      } else if (tok == "max_body_size") {
        ls >> cur->max_body_size;
      } else if (tok == "deny") {
        int status = 0;
        std::string pattern;
        ls >> status;
        std::getline(ls, pattern);
        pattern = trim(pattern);
        if (status < 400 || status > 599 || pattern.empty()) {
          std::fprintf(stderr, "deny needs a 4xx/5xx status and a pattern: %s\n", line.c_str());
          continue;
        }
        try {
          cur->denies.push_back({status, std::regex(pattern, std::regex::ECMAScript | std::regex::icase), pattern});
        } catch (const std::regex_error& e) {
          std::fprintf(stderr, "deny pattern does not compile (%s): %s\n", e.what(), pattern.c_str());
        }
      }
    }
  }
  return vhosts;
}

std::string guess_mime(const std::string& path) {
  auto dot = path.find_last_of('.');
  std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
  for (auto& c : ext) c = char(std::tolower((unsigned char)c));
  static const std::map<std::string, std::string> table = {
      {"html", "text/html"}, {"htm", "text/html"}, {"css", "text/css"}, {"js", "application/javascript"},
      {"mjs", "application/javascript"}, {"json", "application/json"}, {"png", "image/png"},
      {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"gif", "image/gif"}, {"svg", "image/svg+xml"},
      {"ico", "image/x-icon"}, {"woff", "font/woff"}, {"woff2", "font/woff2"}, {"ttf", "font/ttf"},
      {"map", "application/json"}, {"pdf", "application/pdf"}, {"txt", "text/plain"},
      {"xml", "application/xml"}, {"wasm", "application/wasm"},
  };
  auto it = table.find(ext);
  return it == table.end() ? "application/octet-stream" : it->second;
}

bool valid_header_token(const std::string& header_name) {
  if (header_name.empty()) return false;
  for (unsigned char c : header_name) {
    if (!std::isalnum(c) && c != '-') return false;
  }
  return true;
}

bool has_ctl_or_nul(const std::string& s) {
  for (unsigned char c : s) {
    if (c == 0 || c == '\r' || c == '\n') return true;
  }
  return false;
}

std::string cgi_env_name(const std::string& header_name) {
  std::string out = "HTTP_";
  for (char c : header_name) out += (c == '-') ? '_' : char(std::toupper((unsigned char)c));
  return out;
}

bool exists_as_file(hal::Host& host, const std::string& path) {
  auto st = host.stat(path);
  return st.ok() && !st->is_dir;
}

bool parse_range(const std::string& value, uint64_t file_size, uint64_t& start, uint64_t& end) {
  if (file_size == 0 || value.compare(0, 6, "bytes=") != 0) return false;
  std::string spec = value.substr(6);
  auto dash = spec.find('-');
  if (dash == std::string::npos) return false;
  std::string s1 = spec.substr(0, dash), s2 = spec.substr(dash + 1);
  if (s1.empty() && s2.empty()) return false;
  if (s1.empty()) {
    uint64_t n = std::strtoull(s2.c_str(), nullptr, 10);
    if (n == 0) return false;
    if (n > file_size) n = file_size;
    start = file_size - n;
    end = file_size - 1;
  } else {
    start = std::strtoull(s1.c_str(), nullptr, 10);
    end = s2.empty() ? file_size - 1 : std::strtoull(s2.c_str(), nullptr, 10);
  }
  return start <= end && end < file_size;
}

bool has_response_header(const net::HttpResponseHead& rh, const std::string& name) {
  for (const auto& h : rh.headers) {
    if (ieq(h.name, name)) return true;
  }
  return false;
}

void apply_vhost_headers(net::HttpResponseHead& rh, const VhostConfig& vh) {
  if (vh.cors) {
    if (!has_response_header(rh, "Access-Control-Allow-Origin"))
      rh.headers.push_back({"Access-Control-Allow-Origin", vh.cors_origin});
    if (!has_response_header(rh, "Access-Control-Allow-Methods"))
      rh.headers.push_back({"Access-Control-Allow-Methods", vh.cors_methods});
    if (!has_response_header(rh, "Access-Control-Allow-Headers"))
      rh.headers.push_back({"Access-Control-Allow-Headers", vh.cors_headers});
    if (!has_response_header(rh, "Access-Control-Max-Age"))
      rh.headers.push_back({"Access-Control-Max-Age", vh.cors_max_age});
  }
  for (const auto& [k, v] : vh.extra_headers) {
    if (!has_response_header(rh, k)) rh.headers.push_back({k, v});
  }
}

void write_options_response(hal::Connection& conn, const VhostConfig& vh) {
  net::HttpResponseHead rh;
  rh.status = 204;
  rh.status_text = "No Content";
  rh.headers.push_back({"Access-Control-Allow-Origin", vh.cors_origin});
  rh.headers.push_back({"Access-Control-Allow-Methods", vh.cors_methods});
  rh.headers.push_back({"Access-Control-Allow-Headers", vh.cors_headers});
  rh.headers.push_back({"Access-Control-Max-Age", vh.cors_max_age});
  for (const auto& [k, v] : vh.extra_headers) {
    if (!has_response_header(rh, k)) rh.headers.push_back({k, v});
  }
  net::ResponseWriter w(conn, rh, 0);
  w.suppress_body();
  w.write_head();
  w.finish();
}

void write_simple_response(hal::Connection& conn, int status, const std::string& text, const std::string& body) {
  net::HttpResponseHead rh;
  rh.status = status;
  rh.status_text = text;
  rh.headers.push_back({"Content-Type", "text/plain"});
  rh.headers.push_back({"X-Content-Type-Options", "nosniff"});
  rh.headers.push_back({"Connection", "close"});
  net::ResponseWriter w(conn, rh, int64_t(body.size()));
  if (w.write_head()) w.write_body_chunk(span_of(body));
  w.finish();
}

void serve_static(hal::Host& host, hal::Connection& conn, const net::HttpRequestHead& head, const VhostConfig& vh,
                   std::string path_in_root) {
  for (size_t i = 0; i <= path_in_root.size();) {
    size_t j = path_in_root.find_first_of("/\\", i);
    if (j == std::string::npos) j = path_in_root.size();
    if (path_in_root.compare(i, j - i, "..") == 0) { write_simple_response(conn, 400, "Bad Request", "bad path"); return; }
    i = j + 1;
  }
  std::string fs_path = vh.root + path_in_root;
  if (!exists_as_file(host, fs_path)) fs_path = vh.root + "/" + vh.index;  // SPA fallback
  auto st = host.stat(fs_path);
  if (!st.ok() || st->is_dir) { write_simple_response(conn, 404, "Not Found", "not found"); return; }

  uint64_t file_size = st->size;
  uint64_t start = 0, end = file_size == 0 ? 0 : file_size - 1;
  bool partial = false;
  if (auto range = head.header("Range")) partial = parse_range(*range, file_size, start, end);

  std::string mime = guess_mime(fs_path);
  // Compression changes the byte offsets a Range request refers to, so the
  // two don't mix — serve a Range request uncompressed, same as everywhere
  // else that does this.
  // An empty file stays identity: Content-Encoding: gzip over zero bytes is
  // not a gzip stream, and Safari fails it as "cannot decode raw data".
  std::string encoding = (partial || file_size == 0) ? "" : net::negotiate_content_encoding(head.header("Accept-Encoding").value_or(""), mime);

  net::HttpResponseHead rh;
  rh.status = partial ? 206 : 200;
  rh.status_text = partial ? "Partial Content" : "OK";
  rh.headers.push_back({"Content-Type", mime});
  rh.headers.push_back({"X-Content-Type-Options", "nosniff"});
  rh.headers.push_back({"Accept-Ranges", "bytes"});
  rh.headers.push_back({"Vary", "Accept-Encoding"});
  if (!encoding.empty()) rh.headers.push_back({"Content-Encoding", encoding});
  if (partial)
    rh.headers.push_back({"Content-Range", "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(file_size)});
  apply_vhost_headers(rh, vh);

  int64_t body_len = file_size == 0 ? 0 : int64_t(end - start + 1);
  // Compressed size isn't known up front (it's built incrementally as the
  // file streams past — see gzip.hpp), so that response has to go chunked
  // rather than declare a Content-Length.
  net::ResponseWriter w(conn, rh, encoding.empty() ? std::optional<int64_t>(body_len) : std::nullopt);
  if (head.method == "HEAD") w.suppress_body();
  if (!w.write_head()) return;
  if (head.method != "HEAD" && body_len > 0) {
    constexpr uint64_t kChunk = 256 * 1024;
    net::GzipStreamEncoder gz;
    uint64_t off = start;
    while (off <= end) {
      uint64_t n = std::min<uint64_t>(kChunk, end - off + 1);
      auto data = host.read_range(fs_path, off, n);
      if (!data.ok() || data->empty()) break;
      ByteSpan raw(data->data(), data->size());
      if (encoding == "gzip") {
        auto out = gz.feed(raw);
        if (!out.empty() && !w.write_body_chunk(ByteSpan(out.data(), out.size()))) return;
      } else {
        if (!w.write_body_chunk(raw)) return;
      }
      off += data->size();
    }
    if (encoding == "gzip") {
      auto tail = gz.finish();
      if (!tail.empty()) w.write_body_chunk(ByteSpan(tail.data(), tail.size()));
    }
  }
  w.finish();
}

std::string build_php_admin_value(const VhostConfig& vh) {
  // session.upload_progress.enabled=0 and fatal_error_backtraces=0 close the
  // pre-request-init stale-global-state producer/consumer gadgets in PHP-FPM
  // (GHSA-6wmf-4rv5-p3rp / CVE-2026-109619).
  std::string admin =
      "cgi.fix_pathinfo=0\n"
      "allow_url_include=0\n"
      "enable_dl=0\n"
      "expose_php=0\n"
      "session.upload_progress.enabled=0\n"
      "fatal_error_backtraces=0";
  if (vh.php_lockdown) {
    admin += "\nallow_url_fopen=0";
    admin += "\ndisable_functions=exec,passthru,shell_exec,system,proc_open,popen,pcntl_exec,pcntl_fork,dl";
    if (!vh.root.empty() && vh.root.find('\0') == std::string::npos && vh.root.find('\n') == std::string::npos) {
      admin += "\nopen_basedir=" + vh.root + ":/tmp:/var/tmp:/var/lib/php";
    }
  }
  for (const auto& extra : vh.php_admin_values) {
    admin += "\n" + extra;
  }
  return admin;
}

// Returns false when the connection must be closed (the request body was not read to its end).
bool serve_php(hal::Connection& conn, net::ConnReader& reader, const net::HttpRequestHead& head, const VhostConfig& vh,
               const std::string& script_rel_path, const std::string& query_string, const std::string& client_ip) {
  const std::string script_path = vh.root + script_rel_path;
  net::FcgiParams params = {
      {"SCRIPT_FILENAME", script_path},
      {"SCRIPT_NAME", script_rel_path},
      {"DOCUMENT_URI", script_rel_path},
      {"PATH_INFO", ""},
      {"REQUEST_METHOD", head.method},
      {"QUERY_STRING", query_string},
      {"SERVER_PROTOCOL", head.version},
      {"GATEWAY_INTERFACE", "CGI/1.1"},
      {"SERVER_SOFTWARE", "aios-webserver"},
      {"REMOTE_ADDR", client_ip},
      {"SERVER_NAME", vh.server_name},
      {"REQUEST_URI", head.target},
      {"DOCUMENT_ROOT", vh.root},
      {"HTTPS", vh.ssl ? "on" : ""},
      {"REDIRECT_STATUS", "200"},
      {"PHP_ADMIN_VALUE", build_php_admin_value(vh)},
  };
  if (head.content_length >= 0) params.push_back({"CONTENT_LENGTH", std::to_string(head.content_length)});
  if (auto ct = head.header("Content-Type")) {
    if (!has_ctl_or_nul(*ct)) params.push_back({"CONTENT_TYPE", *ct});
  }
  for (auto& h : head.headers) {
    if (!valid_header_token(h.name) || has_ctl_or_nul(h.value)) continue;
    std::string name = cgi_env_name(h.name);
    // Never forward HTTP_PROXY (httpoxy, CVE-2016-5385) or duplicate CONTENT_*
    if (name == "HTTP_CONTENT_LENGTH" || name == "HTTP_CONTENT_TYPE" || name == "HTTP_PROXY") continue;
    params.push_back({name, h.value});
  }

  net::RequestBodyReader body(reader, head);
  bool body_error = false;   // the client's body could not be read (timeout, reset)
  bool body_too_large = false;   // it ran past the vhost's max_body_size (a chunked body has no length to check up front)
  int64_t streamed_bytes = 0;
  auto next_chunk = [&]() -> ByteBuffer {
    if (body_error || body_too_large) return {};
    auto r = body.next(256 * 1024, 60000);
    if (!r.ok()) { body_error = true; return {}; }
    streamed_bytes += int64_t(r->size());
    if (vh.max_body_size > 0 && streamed_bytes > vh.max_body_size) {
      body_too_large = true;
      return {};
    }
    return std::move(*r);
  };
  // An empty chunk ends php-fpm's stdin, so a body that failed or was cut off must not reach it as if it were whole:
  // the request is dropped instead (php-fpm stops it when the connection goes).
  auto refuse_body = [&]() { return body_error || body_too_large; };

  std::string cgi_head_acc;
  bool header_done = false;
  std::optional<net::ResponseWriter> writer;
  net::HttpResponseHead rh;
  rh.status = 200;
  rh.status_text = "OK";

  // Compression can only be decided once PHP's own Content-Type header has
  // arrived, which is why this lives here rather than being decided up
  // front like the static-file path. GzipStreamEncoder compresses PHP's
  // output incrementally as it streams past, never needing the whole body
  // in memory, while still producing exactly one valid gzip stream (see
  // gzip.hpp for why that — and not one member per chunk — is required).
  bool compress = false;
  net::GzipStreamEncoder gz;

  auto flush_head = [&]() {
    std::istringstream hs(cgi_head_acc);
    std::string hline;
    std::string content_type = "text/html";
    bool upstream_encoded = false;
    while (std::getline(hs, hline)) {
      if (!hline.empty() && hline.back() == '\r') hline.pop_back();
      if (hline.empty()) continue;
      auto colon = hline.find(':');
      if (colon == std::string::npos) continue;
      std::string name = hline.substr(0, colon);
      std::string value = trim(hline.substr(colon + 1));
      if (ieq(name, "Status")) {
        rh.status = std::atoi(value.c_str());
        auto sp = value.find(' ');
        rh.status_text = sp == std::string::npos ? "" : value.substr(sp + 1);
      } else if (!ieq(name, "X-Powered-By")) {
        if (ieq(name, "Content-Type")) content_type = value;
        // PHP itself may already have compressed this response (e.g.
        // zlib.output_compression, or an app calling ob_gzhandler): if it
        // already sent Content-Encoding, recompressing on top would produce
        // gzip-of-gzip — a client decodes one layer per the header and is
        // left with the still-compressed inner stream as garbage. Pass
        // upstream's encoding through untouched instead of renegotiating.
        if (ieq(name, "Content-Encoding")) upstream_encoded = true;
        rh.headers.push_back({name, value});
      }
    }
    bool is_head = head.method == "HEAD";
    if (!upstream_encoded && !is_head && !net::ResponseWriter::status_has_no_body(rh.status)) {
      std::string encoding = net::negotiate_content_encoding(head.header("Accept-Encoding").value_or(""), content_type);
      if (!encoding.empty()) {
        compress = true;
        rh.headers.push_back({"Content-Encoding", encoding});
        rh.headers.push_back({"Vary", "Accept-Encoding"});
      }
    }
    apply_vhost_headers(rh, vh);
    writer.emplace(conn, rh, std::nullopt);  // length unknown up front -> chunked to the client
    if (is_head) writer->suppress_body();
    writer->write_head();
  };

  ByteBuffer body_coalesce;
  bool first_body_emitted = false;

  auto flush_body_coalesce = [&]() {
    if (body_coalesce.empty() || !writer) return;
    writer->write_body_chunk(ByteSpan(body_coalesce.data(), body_coalesce.size()));
    body_coalesce.clear();
  };

  auto emit_body = [&](ByteSpan data, bool flush_now = false) {
    if (data.empty()) return;
    ByteSpan to_send = data;
    ByteBuffer compressed;
    if (compress) {
      const bool need_sync = !first_body_emitted || flush_now;
      compressed = gz.feed(data, need_sync);
      if (compressed.empty()) return;
      to_send = ByteSpan(compressed.data(), compressed.size());
    }
    // Emit the very first slice immediately for low TTFB so client receives initial HTML quickly
    if (!first_body_emitted) {
      first_body_emitted = true;
      writer->write_body_chunk(to_send);
      return;
    }
    if (flush_now) {
      flush_body_coalesce();
      writer->write_body_chunk(to_send);
      return;
    }
    // Coalesce subsequent slices into ~8KB blocks to avoid micro-chunks while streaming smoothly
    if (body_coalesce.size() + to_send.size() >= 8192) {
      flush_body_coalesce();
      if (to_send.size() >= 4096) {
        writer->write_body_chunk(to_send);
      } else {
        body_coalesce.insert(body_coalesce.end(), to_send.begin(), to_send.end());
      }
    } else {
      body_coalesce.insert(body_coalesce.end(), to_send.begin(), to_send.end());
    }
  };

  auto on_output = [&](ByteSpan chunk) {
    if (!header_done) {
      cgi_head_acc.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
      auto pos = cgi_head_acc.find("\r\n\r\n");
      if (pos == std::string::npos) return;
      std::string leftover = cgi_head_acc.substr(pos + 4);
      cgi_head_acc.resize(pos);
      header_done = true;
      flush_head();
      if (!leftover.empty()) emit_body(span_of(leftover), /*flush_now=*/true);
      return;
    }
    emit_body(chunk, /*flush_now=*/!first_body_emitted);
  };

  net::FastCgiClient fcgi(vh.php_host, vh.php_port);
  auto result = fcgi.run(params, next_chunk, on_output, 300000, refuse_body);
  if (body_too_large || body_error) {
    if (!header_done) {
      if (body_too_large) write_simple_response(conn, 413, "Payload Too Large", "body exceeds this vhost's configured limit");
      else write_simple_response(conn, 400, "Bad Request", "the request body could not be read");
    }
    return false;   // the rest of the body is still on the wire: close rather than read it as the next request
  }
  if (!header_done) {
    if (!result.ok) { write_simple_response(conn, 502, "Bad Gateway", "upstream error: " + result.error); return true; }
    flush_head();  // php-fpm returned a body with no header block at all (rare) — send what we have as headers
  }
  if (writer) {
    if (compress) {
      auto tail = gz.finish();
      if (!tail.empty()) {
        body_coalesce.insert(body_coalesce.end(), tail.begin(), tail.end());
      }
    }
    flush_body_coalesce();
    writer->finish();
  }
  return true;
}

std::string percent_decode_once(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    char c = raw[i];
    if (c == '%' && i + 2 < raw.size() && std::isxdigit((unsigned char)raw[i + 1]) && std::isxdigit((unsigned char)raw[i + 2])) {
      c = char(std::strtol(raw.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    }
    out += c;
  }
  return out;
}

bool has_bad_target_bytes(const std::string& raw) {
  if (has_ctl_or_nul(raw)) return true;   // a raw control byte anywhere in the target
  // Decoded, only the path is judged: %0A in a query string is ordinary data (a textarea in a GET form, base64 with
  // newlines), while a decoded CR/LF/NUL in the path is how a request smuggles a header or truncates a file name.
  const std::string path = raw.substr(0, raw.find('?'));
  std::string d1 = percent_decode_once(path);
  if (has_ctl_or_nul(d1)) return true;
  std::string d2 = percent_decode_once(d1);
  if (has_ctl_or_nul(d2)) return true;
  return false;
}

// The path as the deny rules and filesystem resolution see it: %XX decoded
// (up to two passes so double-encoded %252e%252e is caught), '\' normalized to
// '/', and runs of '/' collapsed, so /%2egit/HEAD and //config.php are judged
// as /.git/HEAD and /config.php.
std::string normalized_path(const std::string& raw) {
  std::string decoded = percent_decode_once(percent_decode_once(raw));
  std::string out;
  out.reserve(decoded.size());
  for (char c : decoded) {
    if (c == '\\') c = '/';
    if (c == '/' && !out.empty() && out.back() == '/') continue;
    out += c;
  }
  return out;
}

bool has_dot_dot_segment(const std::string& path) {
  size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') ++i;
    size_t j = i;
    while (j < path.size() && path[j] != '/') ++j;
    std::string_view seg(path.data() + i, j - i);
    if (seg == "." || seg == "..") return true;
    i = j;
  }
  return false;
}

// Blocks CGI argument injection (CVE-2024-4577 / CVE-2012-1823: query string
// without '=' starting with '-' or 0xAD soft-hyphen) and dangerous PHP stream
// wrappers / INI directives in the request URI or query string.
bool is_php_rce_payload(const std::string& raw_target, const std::string& raw_query) {
  if (!raw_query.empty() && raw_query.find('=') == std::string::npos) {
    std::string q = percent_decode_once(percent_decode_once(raw_query));
    size_t idx = 0;
    while (idx < q.size() && (q[idx] == ' ' || q[idx] == '\t' || q[idx] == '+')) ++idx;
    if (idx < q.size()) {
      unsigned char first = (unsigned char)q[idx];
      if (first == '-' || first == 0xAD) return true;
      if (first == 0xC2 && idx + 1 < q.size() && (unsigned char)q[idx + 1] == 0xAD) return true;
    }
  }
  std::string lower = percent_decode_once(percent_decode_once(raw_target));
  for (char& c : lower) c = char(std::tolower((unsigned char)c));
  static const char* const kBlockedTokens[] = {
      "php://",
      "phar://",
      "expect://",
      "data://",
      "glob://",
      "zip://",
      "auto_prepend_file",
      "auto_append_file",
      "allow_url_include",
      "allow_url_fopen",
  };
  for (const char* tok : kBlockedTokens) {
    if (lower.find(tok) != std::string::npos) return true;
  }
  return false;
}

bool ends_with_ci(const std::string& s, const char* suffix) {
  size_t slen = std::strlen(suffix);
  if (s.size() < slen) return false;
  for (size_t i = 0; i < slen; ++i) {
    if (std::tolower((unsigned char)s[s.size() - slen + i]) != std::tolower((unsigned char)suffix[i])) return false;
  }
  return true;
}

bool is_blocked_php_variant(const std::string& path) {
  static const char* const kVariants[] = {
      ".phtml", ".phar", ".phps", ".php3", ".php4", ".php5", ".php7", ".php8", ".inc",
  };
  for (const char* ext : kVariants) {
    if (ends_with_ci(path, ext)) return true;
  }
  return false;
}

bool has_php_content_disposition(const net::HttpRequestHead& head) {
  for (const auto& [name, value] : head.headers) {
    if (!ieq(name, "Content-Disposition")) continue;
    std::string lower = percent_decode_once(value);
    for (char& c : lower) c = char(std::tolower((unsigned char)c));
    if (lower.find(".php") != std::string::npos || lower.find(".phtml") != std::string::npos ||
        lower.find(".phar") != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool is_unbounded_multipart(const net::HttpRequestHead& head) {
  if (!head.chunked && head.content_length >= 0) return false;
  auto ct = head.header("Content-Type");
  if (!ct) return false;
  std::string lower = *ct;
  for (char& c : lower) c = char(std::tolower((unsigned char)c));
  return lower.find("multipart/") != std::string::npos;
}

// The status a vhost refuses this path with, or 0 to serve it.
int denied_status(const VhostConfig& vh, const std::string& path) {
  for (size_t at = path.find("/."); at != std::string::npos; at = path.find("/.", at + 1)) {
    if (path.compare(at, 13, "/.well-known/") != 0) return 404;
  }
  for (const auto& d : vh.denies)
    if (std::regex_search(path, d.re)) return d.status;
  return 0;
}

using VhostResolver = std::function<const VhostConfig*(const std::string& host_header)>;

void handle_http_session(hal::Host& host, std::unique_ptr<hal::Connection> conn, VhostResolver resolve) {
  net::ConnReader reader(*conn);
  for (;;) {
    auto head_r = net::read_request_head(reader, 60000);
    if (!head_r.ok()) break;
    auto& head = *head_r;

    const VhostConfig* vh = resolve(head.host);
    if (!vh) { write_simple_response(*conn, 421, "Misdirected Request", "no matching vhost"); break; }
    if (vh->max_body_size > 0 && head.content_length > vh->max_body_size) {
      write_simple_response(*conn, 413, "Payload Too Large", "body exceeds this vhost's configured limit");
      break;  // body left undrained on the wire — safest to close rather than desync keep-alive framing
    }

    if (head.target.empty() || head.target[0] != '/' || has_bad_target_bytes(head.target)) {
      write_simple_response(*conn, 400, "Bad Request", "bad request target");
      break;
    }

    if (has_php_content_disposition(head)) {
      write_simple_response(*conn, 403, "Forbidden", "forbidden");
      break;
    }

    auto qpos = head.target.find('?');
    std::string query = qpos == std::string::npos ? "" : head.target.substr(qpos + 1);
    std::string raw_path_only = qpos == std::string::npos ? head.target : head.target.substr(0, qpos);
    std::string path_only = normalized_path(raw_path_only);
    if (path_only.empty() || path_only[0] != '/') {
      write_simple_response(*conn, 400, "Bad Request", "bad path");
      break;
    }

    if (int st = denied_status(*vh, path_only)) {
      // write_simple_response says Connection: close, and any body is unread.
      write_simple_response(*conn, st, st == 403 ? "Forbidden" : "Not Found", st == 403 ? "forbidden" : "not found");
      break;
    }

    if (has_dot_dot_segment(path_only) || has_dot_dot_segment(raw_path_only)) {
      write_simple_response(*conn, 400, "Bad Request", "bad path");
      break;
    }

    if (is_blocked_php_variant(path_only)) {
      write_simple_response(*conn, 403, "Forbidden", "forbidden");
      break;
    }

    if (head.method == "OPTIONS" && vh->cors) {   // (a PHP vhost without cors gets its OPTIONS: the app's own CORS rules apply)
      write_options_response(*conn, *vh);
      if (!head.keep_alive || head.content_length > 0 || head.chunked) break;
      continue;
    }

    auto ends_with_php = [](const std::string& p) { return ends_with_ci(p, ".php"); };
    bool is_php = false;
    if (ends_with_php(path_only)) {
      if (vh->php_off) {
        write_simple_response(*conn, 503, "Service Unavailable", "php disabled on this vhost");
        break;
      }
      if (vh->php_host.empty()) {
        // Never leak raw .php source files from a static-only vhost.
        write_simple_response(*conn, 403, "Forbidden", "forbidden");
        break;
      }
      // Equivalent to nginx's `try_files $uri =404;`: never forward a
      // non-existent .php path (e.g. /uploads/avatar.jpg/shell.php) to PHP-FPM.
      if (!exists_as_file(host, vh->root + path_only)) {
        write_simple_response(*conn, 404, "Not Found", "not found");
        break;
      }
      is_php = true;
    } else if (!vh->php_host.empty() && !exists_as_file(host, vh->root + path_only)) {
      // try_files-style fallback for a missing path — but only hand it to
      // FastCGI if the vhost's index is itself a PHP front controller
      // (index.php). A static-site vhost with an incidental php backend
      // (index.html, a couple of .php API endpoints) falls through to
      // serve_static below instead, which has its own SPA-index fallback;
      // running index.html through FastCGI would just get a 403 from
      // php-fpm's security.limit_extensions.
      if (ends_with_php("/" + vh->index)) {
        if (vh->php_off) {
          write_simple_response(*conn, 503, "Service Unavailable", "php disabled on this vhost");
          break;
        }
        std::string idx_rel = "/" + vh->index;
        if (!exists_as_file(host, vh->root + idx_rel)) {
          write_simple_response(*conn, 404, "Not Found", "not found");
          break;
        }
        if (path_only != "/" && query.find("route=") == std::string::npos) {
          std::string route = "route=" + path_only.substr(1);
          query = query.empty() ? std::move(route) : route + "&" + query;
        }
        is_php = true;
        path_only = std::move(idx_rel);
      }
    }

    if (is_php) {
      if (is_php_rce_payload(head.target, query)) {
        write_simple_response(*conn, 403, "Forbidden", "forbidden");
        break;
      }
      if (is_unbounded_multipart(head)) {
        // GHSA-w8hw-w9fx-j79w (CVE-2026-109618): multipart requests without a
        // declared Content-Length bypass PHP's post_max_size check and can force OOM.
        write_simple_response(*conn, 411, "Length Required", "multipart request requires Content-Length");
        break;
      }
      if (!serve_php(*conn, reader, head, *vh, path_only, query, conn->peer())) break;
    } else {
      serve_static(host, *conn, head, *vh, path_only);
    }

    if (!head.keep_alive) break;
  }
  conn->close();
}

}  // namespace

// The server proper. `aios webserver CONFIG` (cli/main.cpp) calls this; so does the standalone
// aios-webserver below. argv[0] is the program name, argv[1] the config file.
int aios_webserver_run(int argc, char** argv) {
  if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h" || std::string(argv[1]) == "help") {
    std::fprintf(stdout,
      "aios-webserver — vhost-aware HTTP(S) edge server with PHP-FPM\n\n"
      "usage: %s <config-file>\n\n"
      "Configuration format:\n"
      "  vhost <domain> {\n"
      "    listen <port> [ssl] [loopback]\n"
      "    cert <cert-file.crt>\n"
      "    key <private-key.key>\n"
      "    root </path/to/web/root>\n"
      "    index <index.html>\n"
      "    php <host:port|unix:/sock|off> # FastCGI PHP-FPM upstream, or 'off' to isolate\n"
      "    php_off                        # Isolate PHP endpoints (503) while serving static files\n"
      "    php_lockdown                   # Lock disable_functions, allow_url_fopen=0, open_basedir\n"
      "    php_admin_value <k=v>          # Extra PHP_ADMIN_VALUE line sent to PHP-FPM\n"
      "    cors [on|off]                  # Enable Access-Control-* headers + 204 OPTIONS preflight\n"
      "    cors_origin <origin>           # Default: *\n"
      "    cors_methods <methods>         # Default: GET, POST, PUT, DELETE, OPTIONS, HEAD, PATCH\n"
      "    cors_headers <headers>         # Allowed request headers\n"
      "    cors_max_age <seconds>         # Default: 1000\n"
      "    add_header <Name> <Value>      # Extra response header on every response\n"
      "    max_body_size <bytes>\n"
      "    deny <status> <path-regex>\n"
      "  }\n",
      argc > 0 ? argv[0] : "aios-webserver");
    return argc < 2 ? 1 : 0;
  }
  std::ifstream f(argv[1]);
  if (!f) { std::fprintf(stderr, "cannot open config: %s\n", argv[1]); return 1; }
  std::stringstream ss;
  ss << f.rdbuf();
  auto vhosts = parse_config(ss.str());
  if (vhosts.empty()) { std::fprintf(stderr, "no vhost blocks found in %s\n", argv[1]); return 1; }

  auto host = hal::make_host();

  for (auto& vh : vhosts) {
    if (!vh.ssl) continue;
    auto cert_raw = host->read_file(vh.cert_path);
    if (!cert_raw.ok()) { std::fprintf(stderr, "vhost %s: cannot read cert %s: %s\n", vh.server_name.c_str(), vh.cert_path.c_str(), cert_raw.error().str().c_str()); return 1; }
    auto chain = crypto::x509_parse_bundle(ByteSpan(cert_raw->data(), cert_raw->size()));
    if (!chain.ok()) { std::fprintf(stderr, "vhost %s: bad cert bundle: %s\n", vh.server_name.c_str(), chain.error().str().c_str()); return 1; }
    vh.tls_chain = std::move(*chain);
    auto key_raw = host->read_file(vh.key_path);
    if (!key_raw.ok()) { std::fprintf(stderr, "vhost %s: cannot read key %s: %s\n", vh.server_name.c_str(), vh.key_path.c_str(), key_raw.error().str().c_str()); return 1; }
    auto key = crypto::x509_parse_private_key(ByteSpan(key_raw->data(), key_raw->size()));
    if (!key.ok()) { std::fprintf(stderr, "vhost %s: bad private key: %s\n", vh.server_name.c_str(), key.error().str().c_str()); return 1; }
    vh.tls_key = std::move(*key);
    std::fprintf(stderr, "aios-webserver: loaded TLS identity for %s (%zu cert(s) in chain)\n", vh.server_name.c_str(), vh.tls_chain.size());
  }

  std::map<uint16_t, std::vector<VhostConfig*>> by_port;
  for (auto& vh : vhosts) by_port[vh.listen_port].push_back(&vh);

  std::vector<std::thread> listener_threads;
  for (auto& [port, group] : by_port) {
    bool ssl = group[0]->ssl;
    bool loopback = group[0]->loopback_only;
    listener_threads.emplace_back([&host, port, group, ssl, loopback]() {
      auto listener = hal::net_listen(port, loopback);
      if (!listener.ok()) { std::fprintf(stderr, "aios-webserver: listen on %u failed: %s\n", port, listener.error().str().c_str()); return; }
      std::fprintf(stderr, "aios-webserver: listening on :%u (%s, %zu vhost(s))\n", port, ssl ? "https" : "http", group.size());
      for (;;) {
        auto conn_r = (*listener)->accept(1000);
        if (!conn_r.ok()) continue;
        std::thread([&host, group, ssl, c = std::move(*conn_r)]() mutable {
          std::string peer = c->peer();
          if (ssl) {
            auto peek = net::peek_client_hello_sni(std::move(c), 10000);
            if (!peek.ok()) return;
            const VhostConfig* vh = group.empty() ? nullptr : group[0];
            for (auto* v : group) if (!v->server_name.empty() && ieq(v->server_name, peek->server_name)) { vh = v; break; }
            if (!vh) return;
            auto tls = std::make_unique<net::TlsServer>(*host, vh->tls_chain, vh->tls_key);
            auto acc = tls->accept(std::move(peek->conn), 20000);
            if (!acc.ok()) return;
            auto adapter = std::make_unique<net::TlsServerConnection>(std::move(tls), peer);
            handle_http_session(*host, std::move(adapter), [vh](const std::string&) { return vh; });
          } else {
            handle_http_session(*host, std::move(c), [&group](const std::string& host_hdr) -> const VhostConfig* {
              for (auto* v : group) if (!v->server_name.empty() && ieq(v->server_name, host_hdr)) return v;
              return group.empty() ? nullptr : group[0];
            });
          }
        }).detach();
      }
    });
  }
  for (auto& t : listener_threads) t.join();
  return 0;
}

#ifndef AIOS_WEBSERVER_NO_MAIN
int main(int argc, char** argv) { return aios_webserver_run(argc, argv); }
#endif
