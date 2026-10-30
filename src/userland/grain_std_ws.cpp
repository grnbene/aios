// WebSockets for grain (RFC 6455): a client for ws:// and wss://, and a server made from any accepted TCP socket.
//
//   let ws = ws_connect("wss://host/feed", {"headers": {...}})
//   ws_send(ws, "hello")                     text; ws_send(ws, data, true) sends binary
//   let m = ws_recv(ws, 5000)                {"type": "text"|"binary", "data": ...}, {"type": "close", "code": n,
//                                            "reason": s} once the other end closes, nil when nothing came in time
//   ws_close(ws)
//
//   let l = tcp_listen(9000)                 a server: the handshake on a socket tcp_accept gave
//   let ws = ws_upgrade(tcp_accept(l, 5000)) -> the same kind of handle, the request's path and headers in ws_info
//
// Pings are answered as they arrive; fragmented messages come back whole. Everything is on the program's own
// thread, like tcp_recv: ws_recv waits at most its timeout. Messages are capped at 16 MB.
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

#include "aios/channel/http_client.hpp"
#include "aios/core/base64.hpp"
#include "aios/crypto/sha1.hpp"
#include "aios/hal/host.hpp"
#include "aios/hal/net.hpp"
#include "aios/net/tls.hpp"
#include "aios/net/tls_conn.hpp"
#include "aios/userland/grain.hpp"
#include "grain_builtins_internal.hpp"

namespace aios::userland::grain {

std::string grain_http_trust_root();   // grain_std_http.cpp
std::unique_ptr<hal::Connection> take_tcp_connection(const Value& v, std::string& error, const char* who);   // grain_sockets.cpp

namespace {

const char* const kWs = "websocket";
const size_t kMaxMessage = 16u << 20;
const char* const kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

struct WsHandle {
  std::unique_ptr<hal::Connection> conn;
  std::string buf;          // bytes read past what has been handed out
  bool client = true;       // a client masks what it sends; a server must not
  bool closed = false;      // a close frame went out (or the connection is gone)
  std::string path;         // the server side: what the client asked for
  std::shared_ptr<MapObj> headers;
};

using Clock = std::chrono::steady_clock;

int ms_left(Clock::time_point deadline) {
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
  return left < 0 ? 0 : (int)left;
}

// At least n bytes in h.buf by the deadline. 1: there; 0: timed out; -1: the connection ended.
int fill(WsHandle& h, size_t n, Clock::time_point deadline) {
  while (h.buf.size() < n) {
    char tmp[16384];
    const long r = h.conn->recv_some(MutableByteSpan(reinterpret_cast<uint8_t*>(tmp), sizeof tmp), ms_left(deadline));
    if (r < 0) return -1;
    if (r == 0) {
      if (!h.conn->alive()) return -1;
      if (ms_left(deadline) == 0) return 0;
      continue;
    }
    h.buf.append(tmp, (size_t)r);
  }
  return 1;
}

// The head of an HTTP message, up to and including the blank line; false when it did not come or was too long.
bool read_head(WsHandle& h, std::string& head, Clock::time_point deadline) {
  for (;;) {
    const size_t end = h.buf.find("\r\n\r\n");
    if (end != std::string::npos) {
      head = h.buf.substr(0, end + 4);
      h.buf.erase(0, end + 4);
      return true;
    }
    if (h.buf.size() > 65536) return false;
    if (fill(h, h.buf.size() + 1, deadline) != 1) return false;
  }
}

std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// "Name: value" lines after the first, names lowercased.
std::vector<std::pair<std::string, std::string>> header_lines(const std::string& head) {
  std::vector<std::pair<std::string, std::string>> out;
  size_t at = head.find("\r\n");
  while (at != std::string::npos && at + 2 < head.size()) {
    const size_t next = head.find("\r\n", at + 2);
    const std::string line = head.substr(at + 2, (next == std::string::npos ? head.size() : next) - at - 2);
    const size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string v = line.substr(colon + 1);
      while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
      while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
      out.emplace_back(lower(line.substr(0, colon)), v);
    }
    at = next;
  }
  return out;
}

std::string header(const std::vector<std::pair<std::string, std::string>>& hs, const std::string& name) {
  for (const auto& [k, v] : hs)
    if (k == name) return v;
  return "";
}

std::string accept_key(const std::string& key) {
  const std::string s = key + kGuid;
  const auto d = crypto::sha1(ByteSpan(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
  return base64_encode(ByteSpan(d.data(), d.size()));
}

bool send_frame(WsHandle& h, uint8_t opcode, const std::string& payload) {
  std::string f;
  f.push_back((char)(0x80 | opcode));
  const uint8_t mask_bit = h.client ? 0x80 : 0;
  if (payload.size() < 126) {
    f.push_back((char)(mask_bit | payload.size()));
  } else if (payload.size() <= 0xFFFF) {
    f.push_back((char)(mask_bit | 126));
    f.push_back((char)(payload.size() >> 8));
    f.push_back((char)(payload.size() & 0xFF));
  } else {
    f.push_back((char)(mask_bit | 127));
    for (int i = 7; i >= 0; --i) f.push_back((char)((uint64_t)payload.size() >> (8 * i)));
  }
  if (h.client) {
    uint8_t mask[4];
    hal::process_host().random(MutableByteSpan(mask, 4));
    f.append(reinterpret_cast<const char*>(mask), 4);
    const size_t at = f.size();
    f += payload;
    for (size_t i = 0; i < payload.size(); ++i) f[at + i] = (char)(f[at + i] ^ mask[i % 4]);
  } else {
    f += payload;
  }
  return h.conn->send_all(ByteSpan(reinterpret_cast<const uint8_t*>(f.data()), f.size()));
}

WsHandle* ws_of(const Value& v, std::string& error, const char* who) {
  if (v.type != Type::Extern || !v.obj || static_cast<ExternObj*>(v.obj.get())->kind_name != kWs) {
    error = std::string(who) + "() wants a websocket";
    return nullptr;
  }
  return static_cast<WsHandle*>(static_cast<ExternObj*>(v.obj.get())->ptr.get());
}

Value message(const char* type, std::string data) {
  auto m = std::make_shared<MapObj>();
  m->put("type", Value::string(type));
  m->put("data", Value::string(std::move(data)));
  return Value::object(Type::Map, m);
}

// ws_connect(url, [{headers, protocols, timeout_ms}]) -> websocket
Value bi_ws_connect(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::Str) { error = "ws_connect() wants a ws:// or wss:// url"; return Value::nil(); }
  std::string url = args[0].str();
  if (url.rfind("ws://", 0) == 0) url = "http://" + url.substr(5);
  else if (url.rfind("wss://", 0) == 0) url = "https://" + url.substr(6);
  else { error = "ws_connect(): the url starts ws:// or wss://"; return Value::nil(); }
  auto ep = channel::parse_endpoint(url);
  if (!ep.ok()) { error = "ws_connect(" + args[0].str() + "): " + ep.error().message; return Value::nil(); }
  int timeout = 10000;
  std::string extra;
  if (args.size() > 1 && args[1].type == Type::Map) {
    auto* o = static_cast<MapObj*>(args[1].obj.get());
    if (const Value* t = o->find("timeout_ms"); t && t->is_number()) timeout = (int)t->as_int();
    if (const Value* hs = o->find("headers"); hs && hs->type == Type::Map)
      for (const auto& [k, v] : static_cast<MapObj*>(hs->obj.get())->entries) {
        const std::string val = v.type == Type::Str ? v.str() : v.to_text();
        if (k.find_first_of("\r\n:") != std::string::npos || val.find_first_of("\r\n") != std::string::npos) {
          error = "ws_connect(): a header name or value holds a line break";
          return Value::nil();
        }
        extra += k + ": " + val + "\r\n";
      }
    if (const Value* p = o->find("protocols"); p && p->type == Type::List) {
      std::string list;
      for (const auto& x : static_cast<ListObj*>(p->obj.get())->items) list += (list.empty() ? "" : ", ") + x.to_text();
      if (!list.empty()) extra += "Sec-WebSocket-Protocol: " + list + "\r\n";
    }
  }
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout);

  auto h = std::make_shared<WsHandle>();
  if (!ep->secure) {
    auto c = hal::net_connect(ep->host, ep->port, timeout);
    if (!c.ok()) { error = "ws_connect(" + args[0].str() + "): " + c.error().message; return Value::nil(); }
    h->conn = std::move(*c);
  } else {
    auto tls = std::make_unique<net::TlsClient>(hal::process_host(), net::default_trust_store(hal::process_host(), grain_http_trust_root()));
    auto r = tls->connect(ep->host, ep->port, timeout);
    if (!r.ok()) { error = "ws_connect(" + args[0].str() + "): " + r.error().message; return Value::nil(); }
    h->conn = std::make_unique<net::TlsClientConnection>(std::move(tls), ep->host);
  }

  uint8_t raw_key[16];
  hal::process_host().random(MutableByteSpan(raw_key, sizeof raw_key));
  const std::string key = base64_encode(ByteSpan(raw_key, sizeof raw_key));
  const bool default_port = ep->port == (ep->secure ? 443 : 80);
  const std::string req = "GET " + ep->path + " HTTP/1.1\r\nHost: " + ep->host + (default_port ? "" : ":" + std::to_string(ep->port)) +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                          "\r\nSec-WebSocket-Version: 13\r\n" + extra + "\r\n";
  if (!h->conn->send_all(ByteSpan(reinterpret_cast<const uint8_t*>(req.data()), req.size()))) {
    error = "ws_connect(" + args[0].str() + "): the connection failed";
    return Value::nil();
  }
  std::string head;
  if (!read_head(*h, head, deadline)) { error = "ws_connect(" + args[0].str() + "): no answer to the upgrade"; return Value::nil(); }
  const std::string status = head.substr(0, head.find("\r\n"));
  if (status.size() < 12 || status.compare(9, 3, "101") != 0) { error = "ws_connect(" + args[0].str() + "): the server said " + status; return Value::nil(); }
  const auto hs = header_lines(head);
  if (header(hs, "sec-websocket-accept") != accept_key(key)) { error = "ws_connect(" + args[0].str() + "): the server's Sec-WebSocket-Accept is wrong"; return Value::nil(); }
  h->client = true;
  h->headers = std::make_shared<MapObj>();
  for (const auto& [k, v] : hs) h->headers->put(k, Value::string(v));
  h->path = ep->path;
  return Value::object(Type::Extern, std::make_shared<ExternObj>(kWs, h));
}

// ws_upgrade(sock, [{protocol, headers}]) -> websocket: reads the client's upgrade request on a socket tcp_accept
// gave and answers it. An error (and a 400 to the client) when it is not a websocket request.
Value bi_ws_upgrade(Vm&, std::vector<Value>& args, std::string& error) {
  auto h = std::make_shared<WsHandle>();
  h->conn = take_tcp_connection(args[0], error, "ws_upgrade");
  if (!h->conn) return Value::nil();
  h->client = false;
  std::string head;
  if (!read_head(*h, head, Clock::now() + std::chrono::milliseconds(10000))) { error = "ws_upgrade(): no request came"; return Value::nil(); }
  const auto hs = header_lines(head);
  const std::string first = head.substr(0, head.find("\r\n"));
  const std::string key = header(hs, "sec-websocket-key");
  if (first.rfind("GET ", 0) != 0 || lower(header(hs, "upgrade")) != "websocket" || key.empty()) {
    const std::string no = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    h->conn->send_all(ByteSpan(reinterpret_cast<const uint8_t*>(no.data()), no.size()));
    h->conn->close();
    error = "ws_upgrade(): not a websocket request: " + first;
    return Value::nil();
  }
  std::string extra;
  if (args.size() > 1 && args[1].type == Type::Map) {
    auto* o = static_cast<MapObj*>(args[1].obj.get());
    if (const Value* p = o->find("protocol"); p && p->type == Type::Str) extra += "Sec-WebSocket-Protocol: " + p->str() + "\r\n";
    if (const Value* more = o->find("headers"); more && more->type == Type::Map)
      for (const auto& [k, v] : static_cast<MapObj*>(more->obj.get())->entries) {
        const std::string val = v.type == Type::Str ? v.str() : v.to_text();
        if (k.find_first_of("\r\n:") != std::string::npos || val.find_first_of("\r\n") != std::string::npos) {
          error = "ws_upgrade(): a header name or value holds a line break";
          return Value::nil();
        }
        extra += k + ": " + val + "\r\n";
      }
  }
  const std::string yes = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                          accept_key(key) + "\r\n" + extra + "\r\n";
  if (!h->conn->send_all(ByteSpan(reinterpret_cast<const uint8_t*>(yes.data()), yes.size()))) {
    error = "ws_upgrade(): the connection failed";
    return Value::nil();
  }
  const size_t sp1 = first.find(' '), sp2 = first.find(' ', sp1 + 1);
  h->path = first.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1);
  h->headers = std::make_shared<MapObj>();
  for (const auto& [k, v] : hs) h->headers->put(k, Value::string(v));
  return Value::object(Type::Extern, std::make_shared<ExternObj>(kWs, h));
}

// ---- a route's own request as a websocket (ws_accept) ------------------------------------------------

// The server's connection, lent for as long as the route runs.
struct Lent {
  std::mutex mu;
  std::shared_ptr<hal::Connection> conn;   // null once withdrawn
  bool taken = false;
};
std::mutex g_lent_mu;
std::map<std::string, std::shared_ptr<Lent>> g_lent;

// What a websocket made by ws_accept reads and writes through: the lent connection while it is lent.
class LentConn final : public hal::Connection {
 public:
  explicit LentConn(std::shared_ptr<Lent> l) : l_(std::move(l)) {}
  bool send_all(ByteSpan data) override { std::lock_guard<std::mutex> lk(l_->mu); return l_->conn && l_->conn->send_all(data); }
  bool recv_all(MutableByteSpan data, int timeout_ms) override { std::lock_guard<std::mutex> lk(l_->mu); return l_->conn && l_->conn->recv_all(data, timeout_ms); }
  long recv_some(MutableByteSpan data, int timeout_ms) override { std::lock_guard<std::mutex> lk(l_->mu); return l_->conn ? l_->conn->recv_some(data, timeout_ms) : -1; }
  std::string peer() const override { std::lock_guard<std::mutex> lk(l_->mu); return l_->conn ? l_->conn->peer() : ""; }
  bool alive() const override { std::lock_guard<std::mutex> lk(l_->mu); return l_->conn && l_->conn->alive(); }
  void close() override {}   // the server closes it when the route is done
 private:
  std::shared_ptr<Lent> l_;
};

std::string header_of(MapObj* req, const char* name) {
  Value* hs = req->find("headers");
  if (!hs || hs->type != Type::Map) return "";
  Value* v = static_cast<MapObj*>(hs->obj.get())->find(name);
  return v && v->type == Type::Str ? v->str() : "";
}

// ws_accept(req, [{protocol, headers}]) -> websocket: inside an app's route, answers the request as a websocket
// upgrade. An error when it is not one, or the server serving it cannot lend its connection.
Value bi_ws_accept(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::Map) { error = "ws_accept() wants the route's request"; return Value::nil(); }
  auto* req = static_cast<MapObj*>(args[0].obj.get());
  const std::string key = header_of(req, "sec-websocket-key"), token = header_of(req, "x-grain-upgrade");
  if (lower(header_of(req, "upgrade")) != "websocket" || key.empty()) { error = "ws_accept(): the request is not a websocket upgrade"; return Value::nil(); }
  std::shared_ptr<Lent> l;
  {
    std::lock_guard<std::mutex> lk(g_lent_mu);
    auto it = g_lent.find(token);
    if (token.empty() || it == g_lent.end()) { error = "ws_accept(): this server does not hand its connections to routes"; return Value::nil(); }
    l = it->second;
  }
  {
    std::lock_guard<std::mutex> lk(l->mu);
    if (l->taken || !l->conn) { error = "ws_accept(): the request was already answered"; return Value::nil(); }
    l->taken = true;
  }
  std::string extra;
  if (args.size() > 1 && args[1].type == Type::Map) {
    auto* o = static_cast<MapObj*>(args[1].obj.get());
    if (const Value* p = o->find("protocol"); p && p->type == Type::Str) extra += "Sec-WebSocket-Protocol: " + p->str() + "\r\n";
    if (const Value* more = o->find("headers"); more && more->type == Type::Map)
      for (const auto& [k, v] : static_cast<MapObj*>(more->obj.get())->entries) {
        const std::string val = v.type == Type::Str ? v.str() : v.to_text();
        if (k.find_first_of("\r\n:") != std::string::npos || val.find_first_of("\r\n") != std::string::npos) {
          error = "ws_accept(): a header name or value holds a line break";
          return Value::nil();
        }
        extra += k + ": " + val + "\r\n";
      }
  }
  auto h = std::make_shared<WsHandle>();
  h->conn = std::make_unique<LentConn>(l);
  h->client = false;
  const std::string yes = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                          accept_key(key) + "\r\n" + extra + "\r\n";
  if (!h->conn->send_all(ByteSpan(reinterpret_cast<const uint8_t*>(yes.data()), yes.size()))) { error = "ws_accept(): the connection failed"; return Value::nil(); }
  if (Value* p = req->find("path"); p && p->type == Type::Str) h->path = p->str();
  h->headers = std::make_shared<MapObj>();
  if (Value* hs = req->find("headers"); hs && hs->type == Type::Map)
    for (const auto& [k, v] : static_cast<MapObj*>(hs->obj.get())->entries)
      if (k != "x-grain-upgrade") h->headers->put(k, v);
  return Value::object(Type::Extern, std::make_shared<ExternObj>(kWs, h));
}

// ws_info(ws) -> {path, headers}: the request a server answered, or the response headers a client got
Value bi_ws_info(Vm&, std::vector<Value>& args, std::string& error) {
  auto* h = ws_of(args[0], error, "ws_info");
  if (!h) return Value::nil();
  auto m = std::make_shared<MapObj>();
  m->put("path", Value::string(h->path));
  m->put("headers", h->headers ? Value::object(Type::Map, h->headers) : Value::object(Type::Map, std::make_shared<MapObj>()));
  return Value::object(Type::Map, m);
}

// ws_send(ws, data, [binary=false]) -> true
Value bi_ws_send(Vm&, std::vector<Value>& args, std::string& error) {
  auto* h = ws_of(args[0], error, "ws_send");
  if (!h) return Value::nil();
  if (h->closed || !h->conn) { error = "ws_send(): the websocket is closed"; return Value::nil(); }
  std::string data;
  bool binary = args.size() > 2 && args[2].truthy();
  if (args[1].type == Type::Str) data = args[1].str();
  else if (args[1].type == Type::Bytes) { data = args[1].to_text(); binary = true; }
  else data = args[1].to_text();
  if (data.size() > kMaxMessage) { error = "ws_send(): a message is at most 16 MB"; return Value::nil(); }
  if (!send_frame(*h, binary ? 0x2 : 0x1, data)) { error = "ws_send(): the connection failed"; return Value::nil(); }
  return Value::boolean(true);
}

// ws_recv(ws, [timeout_ms=5000]) -> {type, data} / {type: "close", code, reason} / nil (nothing in time)
Value bi_ws_recv(Vm&, std::vector<Value>& args, std::string& error) {
  auto* h = ws_of(args[0], error, "ws_recv");
  if (!h) return Value::nil();
  if (!h->conn) { error = "ws_recv(): the websocket is closed"; return Value::nil(); }
  const int timeout = args.size() > 1 && args[1].is_number() ? (int)args[1].as_int() : 5000;
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout);
  auto closed = [&](int code, const std::string& reason) {
    auto m = std::make_shared<MapObj>();
    m->put("type", Value::string("close"));
    m->put("code", Value::integer(code));
    m->put("reason", Value::string(reason));
    h->conn->close();
    h->conn.reset();
    h->closed = true;
    return Value::object(Type::Map, m);
  };
  std::string assembled;
  int first_opcode = -1;
  for (;;) {
    // A frame is read whole before any of it is taken from the buffer, so a timeout part way leaves it for next time.
    const int got = fill(*h, 2, deadline);
    if (got == 0) return Value::nil();
    if (got < 0) return closed(1006, "the connection ended");
    const uint8_t b0 = (uint8_t)h->buf[0], b1 = (uint8_t)h->buf[1];
    const bool fin = b0 & 0x80, masked = b1 & 0x80;
    const int opcode = b0 & 0x0F;
    uint64_t len = b1 & 0x7F;
    size_t at = 2;
    if (len == 126 || len == 127) {
      const size_t n = len == 126 ? 2 : 8;
      const int g = fill(*h, at + n, deadline);
      if (g == 0) return Value::nil();
      if (g < 0) return closed(1006, "the connection ended");
      len = 0;
      for (size_t i = 0; i < n; ++i) len = (len << 8) | (uint8_t)h->buf[at + i];
      at += n;
    }
    if (len > kMaxMessage || assembled.size() + len > kMaxMessage) {
      send_frame(*h, 0x8, std::string("\x03\xf1", 2));   // 1009: too big
      return closed(1009, "a message over 16 MB");
    }
    if (masked == h->client) {   // a server's frames are never masked, a client's always are
      send_frame(*h, 0x8, std::string("\x03\xea", 2));   // 1002: protocol error
      return closed(1002, masked ? "a masked frame from the server" : "an unmasked frame from the client");
    }
    const size_t mask_at = at;
    if (masked) at += 4;
    const int g = fill(*h, at + (size_t)len, deadline);
    if (g == 0) return Value::nil();
    if (g < 0) return closed(1006, "the connection ended");
    std::string payload = h->buf.substr(at, (size_t)len);
    if (masked)
      for (size_t i = 0; i < payload.size(); ++i) payload[i] = (char)(payload[i] ^ h->buf[mask_at + i % 4]);
    h->buf.erase(0, at + (size_t)len);

    if (opcode == 0x9) { if (!h->closed) send_frame(*h, 0xA, payload); continue; }   // ping: pong, keep waiting
    if (opcode == 0xA) continue;                                                       // pong
    if (opcode == 0x8) {
      int code = 1005;
      std::string reason;
      if (payload.size() >= 2) { code = ((uint8_t)payload[0] << 8) | (uint8_t)payload[1]; reason = payload.substr(2); }
      if (!h->closed) send_frame(*h, 0x8, payload.substr(0, 2));   // the closing handshake's answer
      return closed(code, reason);
    }
    if (opcode == 0x1 || opcode == 0x2) {
      if (first_opcode != -1) { send_frame(*h, 0x8, std::string("\x03\xea", 2)); return closed(1002, "a new message inside a fragmented one"); }
      first_opcode = opcode;
    } else if (opcode != 0x0 || first_opcode == -1) {
      send_frame(*h, 0x8, std::string("\x03\xea", 2));
      return closed(1002, "an unknown frame");
    }
    assembled += payload;
    if (fin) return message(first_opcode == 0x2 ? "binary" : "text", std::move(assembled));
  }
}

// ws_close(ws, [code=1000], [reason]) -> true. The other end's answer is read by ws_recv, or not at all.
Value bi_ws_close(Vm&, std::vector<Value>& args, std::string& error) {
  auto* h = ws_of(args[0], error, "ws_close");
  if (!h) return Value::nil();
  if (!h->conn || h->closed) { if (h->conn) { h->conn->close(); h->conn.reset(); } return Value::boolean(true); }
  const int code = args.size() > 1 && args[1].is_number() ? (int)args[1].as_int() : 1000;
  std::string payload;
  payload.push_back((char)(code >> 8));
  payload.push_back((char)(code & 0xFF));
  if (args.size() > 2 && args[2].type == Type::Str) payload += args[2].str().substr(0, 123);
  send_frame(*h, 0x8, payload);
  h->closed = true;
  return Value::boolean(true);
}

}  // namespace

std::string offer_connection(std::shared_ptr<hal::Connection> conn) {
  uint8_t raw[16];
  hal::process_host().random(MutableByteSpan(raw, sizeof raw));
  static const char* const hex = "0123456789abcdef";
  std::string token;
  for (uint8_t b : raw) { token.push_back(hex[b >> 4]); token.push_back(hex[b & 15]); }
  auto l = std::make_shared<Lent>();
  l->conn = std::move(conn);
  std::lock_guard<std::mutex> lk(g_lent_mu);
  g_lent[token] = l;
  return token;
}

bool withdraw_connection(const std::string& token) {
  std::shared_ptr<Lent> l;
  {
    std::lock_guard<std::mutex> lk(g_lent_mu);
    auto it = g_lent.find(token);
    if (it == g_lent.end()) return false;
    l = it->second;
    g_lent.erase(it);
  }
  std::lock_guard<std::mutex> lk(l->mu);
  l->conn.reset();
  return l->taken;
}

void install_ws_builtins(Vm& vm) {
  vm.define_native("ws_accept", 1, 2, bi_ws_accept);
  vm.define_native("ws_connect", 1, 2, bi_ws_connect);
  vm.define_native("ws_upgrade", 1, 2, bi_ws_upgrade);
  vm.define_native("ws_info", 1, 1, bi_ws_info);
  vm.define_native("ws_send", 2, 3, bi_ws_send);
  vm.define_native("ws_recv", 1, 2, bi_ws_recv);
  vm.define_native("ws_close", 1, 3, bi_ws_close);
}

}  // namespace aios::userland::grain
