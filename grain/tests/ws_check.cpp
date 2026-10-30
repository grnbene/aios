// WebSockets: a grain server (tcp_listen + ws_upgrade) and a grain client (ws_connect) in two VMs on two threads,
// over loopback. Text, binary, a 70 KB message (the 64-bit length), the request's path and headers, and the closing
// handshake both ways.
#include <cstdio>
#include <string>
#include <thread>

#include "aios/userland/grain.hpp"

namespace g = aios::userland::grain;

static int failures = 0;
static void check(const char* what, bool ok) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}

static g::Vm::RunResult run(g::Vm& vm, const std::string& src) {
  auto c = g::compile_source(src, "<ws_check>", true);
  if (!c.ok()) { std::printf("does not compile: %s\n", c.errors[0].message.c_str()); return {}; }
  return vm.run(c.script);
}

static void setup(g::Vm& vm) {
  g::install_core_builtins(vm);
  g::install_socket_builtins(vm);
  g::install_http_builtins(vm);
}

int main() {
  g::Vm server, client;
  setup(server);
  setup(client);

  auto port = run(server, "let l = tcp_listen(0)\ntcp_port(l)");
  if (!port.ok) { std::printf("tcp_listen: %s\n", port.error.c_str()); return 1; }
  const std::string p = std::to_string(port.value.as_int());

  g::Vm::RunResult served;
  std::thread t([&] {
    served = run(server,
        "let ws = ws_upgrade(tcp_accept(l, 5000))\n"
        "let info = ws_info(ws)\n"
        "let got = []\n"
        "while true {\n"
        "  let m = ws_recv(ws, 5000)\n"
        "  if m == nil { break }\n"
        "  if m.type == \"close\" { push(got, \"close \" + str(m.code)); break }\n"
        "  push(got, m.type + \" \" + str(len(m.data)))\n"
        "  ws_send(ws, m.data, m.type == \"binary\")\n"
        "}\n"
        "info.path + \"|\" + info.headers[\"x-who\"] + \"|\" + join(got, \",\")");
  });

  auto c = run(client,
      "let ws = ws_connect(\"ws://127.0.0.1:" + p + "/feed?x=1\", {\"headers\": {\"X-Who\": \"grain\"}})\n"
      "let out = []\n"
      "ws_send(ws, \"hello\")\n"
      "let a = ws_recv(ws, 5000)\n"
      "push(out, a.type + \":\" + a.data)\n"
      "ws_send(ws, \"\\x00\\x01\\xff\", true)\n"
      "let b = ws_recv(ws, 5000)\n"
      "push(out, b.type + \":\" + hex_encode(b.data))\n"
      "let big = \"\"\n"
      "for i in 0..7000 { big = big + \"0123456789\" }\n"
      "ws_send(ws, big)\n"
      "let c = ws_recv(ws, 5000)\n"
      "push(out, str(c.data == big))\n"
      "ws_close(ws, 1000, \"done\")\n"
      "let d = ws_recv(ws, 5000)\n"
      "push(out, d.type + \":\" + str(d.code))\n"
      "join(out, \",\")");
  t.join();

  check("the client ran", c.ok);
  if (!c.ok) std::printf("  client: %s\n", c.error.c_str());
  check("the server ran", served.ok);
  if (!served.ok) std::printf("  server: %s\n", served.error.c_str());
  const std::string cs = c.ok ? c.value.to_text() : "";
  const std::string ss = served.ok ? served.value.to_text() : "";
  std::printf("  client saw: %s\n  server saw: %s\n", cs.c_str(), ss.c_str());
  check("text and binary echoed, 70 KB intact, close answered", cs == "text:hello,binary:0001ff,true,close:1000");
  check("the server got the path, a header, every message and the close",
        ss == "/feed?x=1|grain|text 5,binary 3,text 70000,close 1000");

  g::Vm bad;
  setup(bad);
  auto r = run(bad, "ws_connect(\"http://127.0.0.1:1/\")");
  check("ws_connect wants ws:// or wss://", !r.ok);
  return failures ? 1 : 0;
}
