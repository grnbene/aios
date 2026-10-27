// Sockets for grain, over the machine's own network face.
//
// This is hal::net and nothing else -- no libcurl, no BSD sockets reached
// around the HAL, no second networking stack. A grain program that listens is
// using the same `net_listen` the kernel's own node transport uses, which is
// what makes "a web server inside the OS" mean something.
//
// Reading is the only awkward part. `recv_all` fills exactly the span it is
// given, which is right for a body of known length and useless for a request
// header of unknown length, and `pending()` is not implemented on every host.
// So headers are read a byte at a time until the marker appears -- a few
// hundred syscalls per request, nothing against a network round trip -- and the
// body, whose length the header just gave us, is read in one.
#include "grain_net.hpp"

#include <memory>
#include <string>
#include <vector>

#include "aios/hal/net.hpp"
#include "aios/userland/grain.hpp"
#include "aios/userland/runtime_registry.hpp"

namespace aios::cli {

namespace {

using namespace aios::userland::grain;

// The two things a handle can be. Named kinds, so passing a listener where a
// connection belongs is refused by name rather than by crash.
const char* const kListener = "listener";
const char* const kConnection = "connection";

std::shared_ptr<void> handle_of(const Value& v, const char* want, std::string& error, const char* who) {
  if (v.type != Type::Extern || !v.obj) {
    error = std::string(who) + "() wants a " + want + ", got a " + type_name(v.type);
    return nullptr;
  }
  auto* e = static_cast<ExternObj*>(v.obj.get());
  if (e->kind_name != want) {
    error = std::string(who) + "() wants a " + want + ", got a " + e->kind_name;
    return nullptr;
  }
  if (!e->ptr) { error = std::string(who) + "() was given a closed " + want; return nullptr; }
  return e->ptr;
}

Value make_handle(const char* kind, std::shared_ptr<void> p) {
  return Value::object(Type::Extern, std::make_shared<ExternObj>(kind, std::move(p)));
}

Value net_listen(Vm&, std::vector<Value>& args, std::string& error) {
  if (!args[0].is_number()) { error = "listen() wants a port number"; return Value::nil(); }
  const int64_t port = args[0].as_int();
  if (port < 1 || port > 65535) { error = "listen(): port " + std::to_string(port) + " is out of range"; return Value::nil(); }
  // Loopback unless the program says otherwise. A server written at a prompt
  // should not become reachable from the network by default.
  const bool loopback = args.size() < 2 || args[1].truthy();
  auto l = hal::net_listen((uint16_t)port, loopback);
  if (!l.ok()) { error = "listen(" + std::to_string(port) + "): " + l.error().message; return Value::nil(); }
  aios::userland::runtime::add_port((int)(*l)->port(), "listen");   // grain ps shows it
  return make_handle(kListener, std::shared_ptr<void>(l->release()));
}

// nil on timeout rather than an error: waiting and getting nobody is the
// normal case in an accept loop, not a fault.
Value net_accept(Vm&, std::vector<Value>& args, std::string& error) {
  auto h = handle_of(args[0], kListener, error, "accept");
  if (!h) return Value::nil();
  const int timeout = args.size() > 1 ? (int)args[1].as_int() : 1000;
  auto c = std::static_pointer_cast<hal::Listener>(h)->accept(timeout);
  if (!c.ok()) return Value::nil();
  return make_handle(kConnection, std::shared_ptr<void>(c->release()));
}

Value net_peer(Vm&, std::vector<Value>& args, std::string& error) {
  auto h = handle_of(args[0], kConnection, error, "peer");
  if (!h) return Value::nil();
  return Value::string(std::static_pointer_cast<hal::Connection>(h)->peer());
}

// Reads until `marker` is seen, or until `limit` bytes have arrived without
// it. The limit is what stops a peer that never sends the marker from growing
// this buffer until the machine gives up.
Value net_read_until(Vm&, std::vector<Value>& args, std::string& error) {
  auto h = handle_of(args[0], kConnection, error, "read_until");
  if (!h) return Value::nil();
  if (args[1].type != Type::Str || args[1].str().empty()) { error = "read_until() wants a marker string"; return Value::nil(); }
  const std::string marker = args[1].str();
  const size_t limit = args.size() > 2 ? (size_t)args[2].as_int() : 65536;
  const int timeout = args.size() > 3 ? (int)args[3].as_int() : 15000;
  auto conn = std::static_pointer_cast<hal::Connection>(h);

  std::string out;
  char byte = 0;
  while (out.size() < limit) {
    if (!conn->recv_all(MutableByteSpan((uint8_t*)&byte, 1), timeout)) {
      // The peer hanging up mid-request is ordinary. Hand back what arrived
      // and let the program decide whether it is enough.
      return out.empty() ? Value::nil() : Value::string(out);
    }
    out.push_back(byte);
    if (out.size() >= marker.size() &&
        out.compare(out.size() - marker.size(), marker.size(), marker) == 0)
      return Value::string(out);
  }
  error = "read_until(): " + std::to_string(limit) + " bytes without the marker";
  return Value::nil();
}

// Exactly n bytes, which is what a Content-Length body is.
Value net_read(Vm&, std::vector<Value>& args, std::string& error) {
  auto h = handle_of(args[0], kConnection, error, "read");
  if (!h) return Value::nil();
  const int64_t n = args[1].as_int();
  if (n < 0 || n > (1 << 24)) { error = "read(): " + std::to_string(n) + " bytes is out of range"; return Value::nil(); }
  if (n == 0) return Value::string("");
  const int timeout = args.size() > 2 ? (int)args[2].as_int() : 15000;
  std::string out((size_t)n, '\0');
  auto conn = std::static_pointer_cast<hal::Connection>(h);
  if (!conn->recv_all(MutableByteSpan((uint8_t*)out.data(), (size_t)n), timeout)) return Value::nil();
  return Value::string(out);
}

Value net_write(Vm&, std::vector<Value>& args, std::string& error) {
  auto h = handle_of(args[0], kConnection, error, "write");
  if (!h) return Value::nil();
  if (args[1].type != Type::Str) { error = "write() wants a string"; return Value::nil(); }
  const std::string& body = args[1].str();
  auto conn = std::static_pointer_cast<hal::Connection>(h);
  return Value::boolean(conn->send_all(ByteSpan((const uint8_t*)body.data(), body.size())));
}

// Closing is explicit AND automatic: this drops the handle now, and letting
// the last reference go drops it anyway. A server that forgets one connection
// in an error path must not leak a descriptor per request.
Value net_close(Vm&, std::vector<Value>& args, std::string& error) {
  if (args[0].type != Type::Extern || !args[0].obj) { error = "close() wants a handle"; return Value::nil(); }
  auto* e = static_cast<ExternObj*>(args[0].obj.get());
  if (!e->ptr) return Value::boolean(false);
  if (e->kind_name == kListener) {
    auto l = std::static_pointer_cast<hal::Listener>(e->ptr);
    aios::userland::runtime::forget_port((int)l->port());
    l->close();
  }
  else if (e->kind_name == kConnection) std::static_pointer_cast<hal::Connection>(e->ptr)->close();
  e->ptr.reset();
  return Value::boolean(true);
}

Value net_port(Vm&, std::vector<Value>& args, std::string& error) {
  auto h = handle_of(args[0], kListener, error, "port");
  if (!h) return Value::nil();
  return Value::integer((int64_t)std::static_pointer_cast<hal::Listener>(h)->port());
}

}  // namespace

void install_grain_net(userland::grain::Vm& vm) {
  vm.define_native("listen", 1, 2, net_listen);
  vm.define_native("accept", 1, 2, net_accept);
  vm.define_native("peer", 1, 1, net_peer);
  vm.define_native("read_until", 2, 4, net_read_until);
  vm.define_native("read", 2, 3, net_read);
  vm.define_native("write", 2, 2, net_write);
  vm.define_native("close", 1, 1, net_close);
  vm.define_native("port", 1, 1, net_port);
}

}  // namespace aios::cli
