// Drives the hosted socket path through hal:: the way the ASQL wire and the
// web server use it, and says which kernel path carried the bytes. On a Linux
// kernel that grants io_uring every send and recv below went through a ring
// shared with the kernel — one io_uring_enter where there used to be a recv
// that got EAGAIN and then a poll. The byte-level outcome must be identical
// either way, so the same checks run whichever backend reports.
//
//   net-uring-check            drive, PASS/FAIL
//   AIOS_NET_URING=0 ...       the same drive on the poll path, for comparison
#include "aios/hal/net.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace aios;
using namespace aios::hal;
static bool ok = true;
static void check(bool c, const char* what) { ok = ok && c; std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what); }
static double ms_since(std::chrono::steady_clock::time_point t) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(); }

int main() {
  std::printf("net backend: %s\n", net_backend());
  auto l = net_listen(0, true);
  if (!l.ok()) { std::printf("listen: %s\n", l.error().message.c_str()); return 1; }
  const uint16_t port = (*l)->port();

  // Echo server on its own thread: everything it reads goes straight back.
  // It also holds the socket open for the timeout check, then closes it so the
  // client sees a real FIN.
  constexpr size_t TOTAL = 4u << 20;
  std::thread srv([&] {
    auto c = (*l)->accept(5000);
    if (!c.ok()) return;
    std::vector<unsigned char> buf(65536);
    size_t echoed = 0;
    while (echoed < TOTAL) {
      long n = (*c)->recv_some(MutableByteSpan(buf.data(), buf.size()), 5000);
      if (n <= 0) return;
      if (!(*c)->send_all(ByteSpan(buf.data(), (size_t)n))) return;
      echoed += (size_t)n;
    }
    // Hold quiet for the client's timeout probe, then one byte and FIN.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    unsigned char bye = 0x42; (*c)->send_all(ByteSpan(&bye, 1));
    (*c)->close();
  });

  auto c = net_connect("127.0.0.1", port, 2000);
  if (!c.ok()) { std::printf("connect: %s\n", c.error().message.c_str()); srv.join(); return 1; }
  Connection& conn = **c;

  // 4 MiB through and back, verified byte for byte, in chunks small enough
  // that the echo interleaves with the send.
  std::vector<unsigned char> out(TOTAL), in(TOTAL);
  for (size_t i = 0; i < TOTAL; ++i) out[i] = (unsigned char)(i * 2654435761u >> 13);
  auto t0 = std::chrono::steady_clock::now();
  std::thread writer([&] { for (size_t off = 0; off < TOTAL; off += 32768) conn.send_all(ByteSpan(out.data() + off, std::min<size_t>(32768, TOTAL - off))); });
  bool got = conn.recv_all(MutableByteSpan(in.data(), in.size()), 10000);
  writer.join();
  const double echo_ms = ms_since(t0);
  check(got, "4 MiB echoed back within the timeout");
  check(got && std::memcmp(in.data(), out.data(), TOTAL) == 0, "every byte came back as sent");
  std::printf("        %.1f ms, %.0f MB/s round trip on loopback\n", echo_ms, TOTAL * 2 / 1e6 / (echo_ms / 1e3));

  // Timeout: the peer is quiet for 300 ms; a 50 ms wait must come back empty
  // AND leave the connection alive — this is the path the linked timeout owns.
  t0 = std::chrono::steady_clock::now();
  unsigned char one = 0;
  long n = conn.recv_some(MutableByteSpan(&one, 1), 50);
  const double wait_ms = ms_since(t0);
  check(n == 0, "recv on an idle peer returns 0 (nothing yet)");
  check(conn.alive(), "...and the connection is still alive");
  check(wait_ms >= 40 && wait_ms < 250, "...having waited about the 50 ms asked for");
  std::printf("        waited %.1f ms\n", wait_ms);

  // Then the byte arrives, then FIN: alive() must flip only on the FIN.
  n = conn.recv_some(MutableByteSpan(&one, 1), 2000);
  check(n == 1 && one == 0x42, "the late byte arrives");
  n = conn.recv_some(MutableByteSpan(&one, 1), 2000);
  check(n <= 0 && !conn.alive(), "peer close is seen as dead, not as a timeout");
  srv.join();
  std::printf("NET-URING: %s (%s)\n", ok ? "PASS" : "FAIL", net_backend());
  return ok ? 0 : 1;
}
