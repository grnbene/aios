// UDP and TCP for grain, over hal::net - the same code on a hosted machine
// (BSD sockets underneath) and on the Pi (the kernel's own UDP table and TCP
// state machine underneath). Nothing here reaches around the HAL.
//
// Not part of install_builtins(): a VM gets sockets only when its embedder
// says so (`aios grain` on a host, the Pi's console) - the sidecar's
// sandboxed aios_grain_run does not.
#include <memory>
#include <string>
#include <vector>

#include "aios/hal/net.hpp"
#include "aios/userland/grain.hpp"

namespace aios::userland::grain {

namespace {

const char* const kUdp = "udp_socket";
const char* const kTcp = "tcp_socket";

struct UdpHandle {
  std::unique_ptr<hal::UdpSocket> sock;   // null until bound (udp_bind, or the first send)
  bool closed = false;
};
struct TcpHandle {
  std::unique_ptr<hal::Connection> conn;
};
const char* const kTcpListener = "tcp_listener";
struct TcpListenHandle {
  std::unique_ptr<hal::Listener> listener;
};

template <class T>
T* handle(const Value& v, const char* kind, std::string& error, const char* who) {
  if (v.type != Type::Extern || !v.obj) {
    error = std::string(who) + "() wants a " + kind + ", got a " + type_name(v.type);
    return nullptr;
  }
  auto* e = static_cast<ExternObj*>(v.obj.get());
  if (e->kind_name != kind) {
    error = std::string(who) + "() wants a " + kind + ", got a " + e->kind_name;
    return nullptr;
  }
  return static_cast<T*>(e->ptr.get());
}

bool want_port(const Value& v, std::string& error, const char* who, uint16_t& out, bool allow_zero) {
  if (v.type != Type::Int) { error = std::string(who) + "() wants a port number, got a " + type_name(v.type); return false; }
  if (v.i < (allow_zero ? 0 : 1) || v.i > 65535) { error = std::string(who) + "(): port " + std::to_string(v.i) + " is out of range"; return false; }
  out = static_cast<uint16_t>(v.i);
  return true;
}

bool want_text(const Value& v, std::string& error, const char* who) {
  if (v.type == Type::Str) return true;
  error = std::string(who) + "() wants a string, got a " + type_name(v.type);
  return false;
}

int timeout_arg(const std::vector<Value>& args, size_t at, int fallback) {
  return args.size() > at && args[at].is_number() ? static_cast<int>(args[at].as_int()) : fallback;
}

// ---- UDP -------------------------------------------------------------------

Value bi_udp_socket(Vm&, std::vector<Value>&, std::string&) {
  return Value::object(Type::Extern, std::make_shared<ExternObj>(kUdp, std::make_shared<UdpHandle>()));
}

// udp_bind(sock, port) / udp_bind(sock, host, port) -> true
Value bi_udp_bind(Vm&, std::vector<Value>& args, std::string& error) {
  auto* h = handle<UdpHandle>(args[0], kUdp, error, "udp_bind");
  if (!h) return Value::nil();
  if (h->closed) { error = "udp_bind(): the socket is closed"; return Value::nil(); }
  if (h->sock) { error = "udp_bind(): already bound to port " + std::to_string(h->sock->port()); return Value::nil(); }
  std::string host;
  uint16_t port = 0;
  if (args.size() == 2) {
    if (!want_port(args[1], error, "udp_bind", port, true)) return Value::nil();
  } else {
    if (!want_text(args[1], error, "udp_bind") || !want_port(args[2], error, "udp_bind", port, true)) return Value::nil();
    host = args[1].str();
  }
  auto r = hal::udp_open(host, port);
  if (!r.ok()) { error = "udp_bind(): " + r.error().message; return Value::nil(); }
  h->sock = std::move(r.value());
  return Value::boolean(true);
}

// udp_send(sock, host, port, data) -> bytes sent
Value bi_udp_send(Vm&, std::vector<Value>& args, std::string& error) {
  auto* h = handle<UdpHandle>(args[0], kUdp, error, "udp_send");
  if (!h) return Value::nil();
  if (h->closed) { error = "udp_send(): the socket is closed"; return Value::nil(); }
  uint16_t port = 0;
  if (!want_text(args[1], error, "udp_send") || !want_port(args[2], error, "udp_send", port, false)) return Value::nil();
  if (!h->sock) {                              // unbound: any free port, as BSD does on first send
    auto r = hal::udp_open("", 0);
    if (!r.ok()) { error = "udp_send(): " + r.error().message; return Value::nil(); }
    h->sock = std::move(r.value());
  }
  const std::string data = args[3].type == Type::Str ? args[3].str() : args[3].to_text();
  std::string why;
  if (!h->sock->send_to(args[1].str(), port, ByteSpan(reinterpret_cast<const uint8_t*>(data.data()), data.size()), why)) {
    error = "udp_send(): " + why;
    return Value::nil();
  }
  return Value::integer(static_cast<int64_t>(data.size()));
}

// udp_recv(sock, [maxlen=65535], [timeout_ms=1000]) -> {data, host, port, size}, or nil if nothing arrived
Value bi_udp_recv(Vm&, std::vector<Value>& args, std::string& error) {
  auto* h = handle<UdpHandle>(args[0], kUdp, error, "udp_recv");
  if (!h) return Value::nil();
  if (h->closed || !h->sock) { error = h->closed ? "udp_recv(): the socket is closed" : "udp_recv(): bind the socket first (udp_bind)"; return Value::nil(); }
  int64_t maxlen = args.size() > 1 && args[1].is_number() ? args[1].as_int() : 65535;
  if (maxlen <= 0 || maxlen > (1 << 20)) maxlen = 65535;
  auto r = h->sock->recv_from(static_cast<size_t>(maxlen), timeout_arg(args, 2, 1000));
  if (!r.ok()) {
    if (r.error().status == Status::Busy) return Value::nil();
    error = "udp_recv(): " + r.error().message;
    return Value::nil();
  }
  auto out = std::make_shared<MapObj>();
  const auto& d = r.value();
  out->put("data", Value::string(d.data));
  out->put("host", Value::string(d.host));
  out->put("port", Value::integer(d.port));
  out->put("size", Value::integer(static_cast<int64_t>(d.data.size())));
  return Value::object(Type::Map, out);
}

// udp_port(sock) -> the bound local port, or nil
Value bi_udp_port(Vm&, std::vector<Value>& args, std::string& error) {
  auto* h = handle<UdpHandle>(args[0], kUdp, error, "udp_port");
  if (!h) return Value::nil();
  return h->sock ? Value::integer(h->sock->port()) : Value::nil();
}

Value bi_udp_close(Vm&, std::vector<Value>& args, std::string& error) {
  auto* h = handle<UdpHandle>(args[0], kUdp, error, "udp_close");
  if (!h) return Value::nil();
  if (h->sock) h->sock->close();
  h->sock.reset();
  h->closed = true;
  return Value::boolean(true);
}

// ---- TCP -------------------------------------------------------------------

// tcp_connect(host, port, [timeout_ms=5000]) -> socket
Value bi_tcp_connect(Vm&, std::vector<Value>& args, std::string& error) {
  uint16_t port = 0;
  if (!want_text(args[0], error, "tcp_connect") || !want_port(args[1], error, "tcp_connect", port, false)) return Value::nil();
  auto r = hal::net_connect(args[0].str(), port, timeout_arg(args, 2, 5000));
  if (!r.ok()) { error = "tcp_connect(" + args[0].str() + ":" + std::to_string(port) + "): " + r.error().message; return Value::nil(); }
  auto h = std::make_shared<TcpHandle>();
  h->conn = std::move(r.value());
  return Value::object(Type::Extern, std::make_shared<ExternObj>(kTcp, h));
}

// tcp_send(sock, data) -> bytes sent (all of them, or an error)
Value bi_tcp_send(Vm&, std::vector<Value>& args, std::string& error) {
  auto* h = handle<TcpHandle>(args[0], kTcp, error, "tcp_send");
  if (!h) return Value::nil();
  if (!h->conn) { error = "tcp_send(): the socket is closed"; return Value::nil(); }
  const std::string data = args[1].type == Type::Str ? args[1].str() : args[1].to_text();
  if (!h->conn->send_all(ByteSpan(reinterpret_cast<const uint8_t*>(data.data()), data.size()))) {
    error = "tcp_send(): the connection failed";
    return Value::nil();
  }
  return Value::integer(static_cast<int64_t>(data.size()));
}

// tcp_recv(sock, [maxlen=65536], [timeout_ms=5000]) -> what arrived; nil = nothing in time; "" = peer closed
Value bi_tcp_recv(Vm&, std::vector<Value>& args, std::string& error) {
  auto* h = handle<TcpHandle>(args[0], kTcp, error, "tcp_recv");
  if (!h) return Value::nil();
  if (!h->conn) { error = "tcp_recv(): the socket is closed"; return Value::nil(); }
  int64_t maxlen = args.size() > 1 && args[1].is_number() ? args[1].as_int() : 65536;
  if (maxlen <= 0) return Value::string("");
  if (maxlen > (1 << 20)) maxlen = 1 << 20;
  std::string buf(static_cast<size_t>(maxlen), '\0');
  const long n = h->conn->recv_some(MutableByteSpan(reinterpret_cast<uint8_t*>(buf.data()), buf.size()), timeout_arg(args, 2, 5000));
  if (n < 0) { error = "tcp_recv(): the connection failed"; return Value::nil(); }
  if (n == 0) return h->conn->alive() ? Value::nil() : Value::string("");
  buf.resize(static_cast<size_t>(n));
  return Value::string(std::move(buf));
}

// tcp_close(sock_or_listener) -> true
Value bi_tcp_close(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type == Type::Extern && args[0].obj && static_cast<ExternObj*>(args[0].obj.get())->kind_name == kTcpListener) {
    auto* l = handle<TcpListenHandle>(args[0], kTcpListener, error, "tcp_close");
    if (l->listener) l->listener->close();
    l->listener.reset();
    return Value::boolean(true);
  }
  auto* h = handle<TcpHandle>(args[0], kTcp, error, "tcp_close");
  if (!h) return Value::nil();
  if (h->conn) h->conn->close();
  h->conn.reset();
  return Value::boolean(true);
}

// tcp_listen(port, [where="local"]) -> listener. "local" takes connections from this machine only, "all" from
// anywhere; port 0 picks a free one (tcp_port says which).
Value bi_tcp_listen(Vm&, std::vector<Value>& args, std::string& error) {
  uint16_t port = 0;
  if (!want_port(args[0], error, "tcp_listen", port, true)) return Value::nil();
  std::string where = "local";
  if (args.size() > 1) {
    if (!want_text(args[1], error, "tcp_listen")) return Value::nil();
    where = args[1].str();
    if (where != "local" && where != "all") { error = "tcp_listen(): where is \"local\" or \"all\", not \"" + where + "\""; return Value::nil(); }
  }
  auto r = hal::net_listen(port, where == "local");
  if (!r.ok()) { error = "tcp_listen(" + std::to_string(port) + "): " + r.error().message; return Value::nil(); }
  auto h = std::make_shared<TcpListenHandle>();
  h->listener = std::move(r.value());
  return Value::object(Type::Extern, std::make_shared<ExternObj>(kTcpListener, h));
}

// tcp_accept(listener, [timeout_ms=5000]) -> socket (tcp_send/tcp_recv/tcp_close), or nil when none came in time
Value bi_tcp_accept(Vm&, std::vector<Value>& args, std::string& error) {
  auto* l = handle<TcpListenHandle>(args[0], kTcpListener, error, "tcp_accept");
  if (!l) return Value::nil();
  if (!l->listener) { error = "tcp_accept(): the listener is closed"; return Value::nil(); }
  auto r = l->listener->accept(timeout_arg(args, 1, 5000));
  if (!r.ok()) {
    if (r.error().status == Status::Busy) return Value::nil();
    error = "tcp_accept(): " + r.error().message;
    return Value::nil();
  }
  auto h = std::make_shared<TcpHandle>();
  h->conn = std::move(r.value());
  return Value::object(Type::Extern, std::make_shared<ExternObj>(kTcp, h));
}

// tcp_port(listener) -> the port it listens on
Value bi_tcp_port(Vm&, std::vector<Value>& args, std::string& error) {
  auto* l = handle<TcpListenHandle>(args[0], kTcpListener, error, "tcp_port");
  if (!l) return Value::nil();
  if (!l->listener) { error = "tcp_port(): the listener is closed"; return Value::nil(); }
  return Value::integer(l->listener->port());
}

// tcp_peer(sock) -> "host:port" of the other end
Value bi_tcp_peer(Vm&, std::vector<Value>& args, std::string& error) {
  auto* h = handle<TcpHandle>(args[0], kTcp, error, "tcp_peer");
  if (!h) return Value::nil();
  if (!h->conn) { error = "tcp_peer(): the socket is closed"; return Value::nil(); }
  return Value::string(h->conn->peer());
}

}  // namespace

// The connection under a tcp socket, taken out of it (the socket is then closed to grain): how ws_upgrade turns a
// socket tcp_accept gave into a websocket (grain_std_ws.cpp).
std::unique_ptr<hal::Connection> take_tcp_connection(const Value& v, std::string& error, const char* who) {
  auto* h = handle<TcpHandle>(v, kTcp, error, who);
  if (!h) return nullptr;
  if (!h->conn) { error = std::string(who) + "(): the socket is closed"; return nullptr; }
  return std::move(h->conn);
}

void install_socket_builtins(Vm& vm) {
  vm.define_native("udp_socket", 0, 0, bi_udp_socket);
  vm.define_native("udp_bind", 2, 3, bi_udp_bind);
  vm.define_native("udp_send", 4, 4, bi_udp_send);
  vm.define_native("udp_send_to", 4, 4, bi_udp_send);
  vm.define_native("udp_recv", 1, 3, bi_udp_recv);
  vm.define_native("udp_recv_from", 1, 3, bi_udp_recv);
  vm.define_native("udp_port", 1, 1, bi_udp_port);
  vm.define_native("udp_close", 1, 1, bi_udp_close);
  vm.define_native("tcp_connect", 2, 3, bi_tcp_connect);
  vm.define_native("net_connect", 2, 3, bi_tcp_connect);
  vm.define_native("tcp_send", 2, 2, bi_tcp_send);
  vm.define_native("tcp_recv", 1, 3, bi_tcp_recv);
  vm.define_native("tcp_close", 1, 1, bi_tcp_close);
  vm.define_native("tcp_listen", 1, 2, bi_tcp_listen);
  vm.define_native("tcp_accept", 1, 2, bi_tcp_accept);
  vm.define_native("tcp_port", 1, 1, bi_tcp_port);
  vm.define_native("tcp_peer", 1, 1, bi_tcp_peer);
}

}  // namespace aios::userland::grain
