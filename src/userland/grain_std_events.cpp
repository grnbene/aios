// Server-sent events for grain: a stream a server pushes, delivered to the program as it arrives.
//
//   events("https://host/feed", {"headers": {...}}, fn (data) { ... })   -> an id; events_close(id) stops it
//
// A thread of its own holds the connection (HTTP or HTTPS, a chunked or a close-delimited body), reads the
// stream's events and leaves each one in an inbox; the program's own thread takes them out - through a timer,
// so a window's frame loop, `grain serve`, the web server or `wait()` delivers them - and calls the function
// with each event's data. Nothing asks the server anything after the request: it says when there is news.
// A dropped connection is made again after a second, as a browser's EventSource does; the function is told
// with {"error": ...} only when the stream cannot be had at all (a refused request).
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "aios/channel/http_client.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/net.hpp"
#include "aios/net/tls.hpp"
#include "aios/net/tls_conn.hpp"
#include "aios/userland/grain.hpp"
#include "grain_builtins_internal.hpp"

namespace aios::userland::grain {

std::string grain_http_trust_root();   // grain_std_http.cpp

namespace {

struct Feed {
  std::mutex mu;
  std::deque<std::string> inbox;   // events' data, in order
  std::string error;               // a refused stream, for the program to hear about once
  std::atomic<bool> stop{false};
  std::atomic<bool> connected{false};
};

std::mutex g_feeds_mu;
std::map<int64_t, std::shared_ptr<Feed>> g_feeds;
int64_t g_next_feed = 1;

std::unique_ptr<hal::Connection> open_conn(const channel::HttpEndpoint& ep, std::string& error) {
  if (!ep.secure) {
    auto c = hal::net_connect(ep.host, ep.port, 10000);
    if (!c.ok()) { error = c.error().message; return nullptr; }
    return std::move(*c);
  }
  auto tls = std::make_unique<net::TlsClient>(hal::process_host(), net::default_trust_store(hal::process_host(), grain_http_trust_root()));
  auto r = tls->connect(ep.host, ep.port, 15000);
  if (!r.ok()) { error = r.error().message; return nullptr; }
  return std::make_unique<net::TlsClientConnection>(std::move(tls), ep.host);
}

// One connection's life: the request, the head, then events until it closes. False when the server refused.
bool run_once(Feed& f, const channel::HttpEndpoint& ep, const std::vector<std::pair<std::string, std::string>>& headers) {
  std::string why;
  auto conn = open_conn(ep, why);
  if (!conn) return true;   // unreachable for now: try again
  std::string req = "GET " + (ep.path.empty() ? std::string("/") : ep.path) + " HTTP/1.1\r\nHost: " + ep.host +
                    "\r\nCache-Control: no-cache\r\nUser-Agent: grain\r\n";
  bool has_accept = false;
  for (const auto& [k, v] : headers) {
    if (k.find_first_of("\r\n") != std::string::npos || v.find_first_of("\r\n") != std::string::npos) continue;
    std::string lk = k;
    for (char& c : lk) c = (char)std::tolower((unsigned char)c);
    if (lk == "accept") has_accept = true;
    req += k + ": " + v + "\r\n";
  }
  if (!has_accept) req += "Accept: text/event-stream\r\n";
  req += "\r\n";
  if (!conn->send_all(ByteSpan(reinterpret_cast<const uint8_t*>(req.data()), req.size()))) return true;

  std::string buf;
  auto more = [&]() -> bool {   // whatever arrives next; false at the end
    for (;;) {
      if (f.stop) return false;
      uint8_t tmp[16384];
      const long n = conn->recv_some(MutableByteSpan(tmp, sizeof tmp), 1000);
      if (n > 0) { buf.append(reinterpret_cast<const char*>(tmp), (size_t)n); return true; }
      if (n < 0 || !conn->alive()) return false;
    }
  };
  size_t end;
  while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
    if (buf.size() > 65536 || !more()) return true;
  }
  const std::string head = buf.substr(0, end);
  buf.erase(0, end + 4);
  const size_t sp = head.find(' ');
  const int status = sp == std::string::npos ? 0 : std::atoi(head.c_str() + sp + 1);
  std::string lower_head = head;
  for (char& c : lower_head) c = (char)std::tolower((unsigned char)c);
  if (status != 200) {
    std::lock_guard<std::mutex> lk(f.mu);
    f.error = "the server answered " + std::to_string(status);
    return false;
  }
  const bool chunked = lower_head.find("transfer-encoding: chunked") != std::string::npos;
  f.connected = true;

  std::string text;    // the body, de-chunked
  std::string data;    // the event being read
  bool has_data = false;
  // A chunked body: a size line, that many bytes, a CRLF, again; a zero size ends it.
  enum { kSize, kData, kCrlf } state = kSize;
  size_t chunk_left = 0;
  auto feed_body = [&]() {
    if (!chunked) { text += buf; buf.clear(); return; }
    for (;;) {
      if (state == kSize) {
        const size_t eol = buf.find("\r\n");
        if (eol == std::string::npos) return;
        chunk_left = std::strtoul(buf.substr(0, eol).c_str(), nullptr, 16);
        buf.erase(0, eol + 2);
        if (chunk_left == 0) { buf.clear(); return; }
        state = kData;
      }
      if (state == kData) {
        const size_t take = std::min(chunk_left, buf.size());
        text.append(buf, 0, take);
        buf.erase(0, take);
        chunk_left -= take;
        if (chunk_left > 0) return;
        state = kCrlf;
      }
      if (state == kCrlf) {
        if (buf.size() < 2) return;
        buf.erase(0, 2);
        state = kSize;
      }
    }
  };
  for (;;) {
    feed_body();
    size_t nl;
    while ((nl = text.find('\n')) != std::string::npos) {
      std::string line = text.substr(0, nl);
      text.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) {   // the end of an event
        if (has_data) {
          std::lock_guard<std::mutex> lk(f.mu);
          if (f.inbox.size() < 10000) f.inbox.push_back(data);
        }
        data.clear();
        has_data = false;
      } else if (line.rfind("data:", 0) == 0) {
        std::string d = line.substr(5);
        if (!d.empty() && d[0] == ' ') d.erase(0, 1);
        if (has_data) data += "\n";
        data += d;
        has_data = true;
      }   // comments (": keepalive"), event:, id: and retry: lines are not data
    }
    if (!more()) break;
  }
  f.connected = false;
  return true;
}

// The program's side: called by its timer, it hands each waiting event to the function.
Value method_events_tick(Vm& vm, BoundMethodObj& bm, std::vector<Value>&, std::string& error) {
  auto* rec = static_cast<MapObj*>(bm.receiver.obj.get());
  const int64_t id = rec->find("id")->i;
  std::shared_ptr<Feed> f;
  {
    std::lock_guard<std::mutex> lk(g_feeds_mu);
    auto it = g_feeds.find(id);
    if (it == g_feeds.end()) return Value::nil();
    f = it->second;
  }
  std::deque<std::string> got;
  std::string refused;
  {
    std::lock_guard<std::mutex> lk(f->mu);
    got.swap(f->inbox);
    refused.swap(f->error);
  }
  const Value fn = *rec->find("fn");
  for (const auto& d : got) {
    vm.call(fn, {Value::string(d)}, error);
    if (!error.empty()) return Value::nil();
  }
  if (!refused.empty()) {
    auto m = std::make_shared<MapObj>();
    m->put("error", Value::string(refused));
    vm.call(fn, {Value::object(Type::Map, m)}, error);
  }
  return Value::nil();
}

// events(url, options, fn) -> id
Value bi_events(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (vm.worker) { error = "events(): a worker (parallel_map, spawn) has no loop to deliver events; open the stream in the program itself"; return Value::nil(); }
  if (!want_str(args[0], error, "events")) return Value::nil();
  const Value& fn = args.size() > 2 ? args[2] : args[1];
  if (fn.type != Type::Fn && fn.type != Type::Native && fn.type != Type::BoundMethod) { error = "events() wants a function to give each event to"; return Value::nil(); }
  auto ep = channel::parse_endpoint(args[0].str());
  if (!ep.ok()) { error = "events(\"" + args[0].str() + "\"): " + ep.error().message; return Value::nil(); }
  std::vector<std::pair<std::string, std::string>> headers;
  if (args.size() > 2 && args[1].type == Type::Map) {
    if (Value* h = static_cast<MapObj*>(args[1].obj.get())->find("headers"); h && h->type == Type::Map)
      for (const auto& [k, v] : static_cast<MapObj*>(h->obj.get())->entries) headers.push_back({k, v.to_text()});
  }
  auto f = std::make_shared<Feed>();
  int64_t id;
  {
    std::lock_guard<std::mutex> lk(g_feeds_mu);
    id = g_next_feed++;
    g_feeds[id] = f;
  }
  const channel::HttpEndpoint endpoint = *ep;
  std::thread([f, endpoint, headers]() {
    while (!f->stop) {
      if (!run_once(*f, endpoint, headers)) break;   // refused: told once, not retried
      for (int i = 0; i < 10 && !f->stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));   // then again, as EventSource does
    }
  }).detach();
  // Delivered on the program's thread, by a timer that only looks at the inbox.
  auto rec = std::make_shared<MapObj>();
  rec->put("id", Value::integer(id));
  rec->put("fn", fn);
  Vm::Timer t;
  t.id = vm.next_timer_id++;
  t.period_ms = 20;
  t.due_ms = wall_ms() + 20;
  t.fn = Value::object(Type::BoundMethod, std::make_shared<BoundMethodObj>(Value::object(Type::Map, rec), "events", 0, 0, method_events_tick));
  vm.timers.push_back(t);
  rec->put("timer", Value::integer((int64_t)t.id));
  return Value::integer(id);
}

Value bi_events_close(Vm& vm, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::Int) { error = "events_close() wants the id events() gave"; return Value::nil(); }
  std::shared_ptr<Feed> f;
  {
    std::lock_guard<std::mutex> lk(g_feeds_mu);
    auto it = g_feeds.find(args[0].i);
    if (it == g_feeds.end()) return Value::boolean(false);
    f = it->second;
    g_feeds.erase(it);
  }
  f->stop = true;
  // its timer finds no feed and does nothing; drop it too
  for (auto it = vm.timers.begin(); it != vm.timers.end(); ++it) {
    if (it->fn.type != Type::BoundMethod) continue;
    auto* bm = static_cast<BoundMethodObj*>(it->fn.obj.get());
    if (bm->fn != method_events_tick) continue;
    Value* id = static_cast<MapObj*>(bm->receiver.obj.get())->find("id");
    if (id && id->i == args[0].i) { vm.timers.erase(it); break; }
  }
  return Value::boolean(true);
}

}  // namespace

void install_events_builtins(Vm& vm) {
  vm.define_native("events", 2, 3, bi_events);
  vm.define_native("events_close", 1, 1, bi_events_close);
}

}  // namespace aios::userland::grain
