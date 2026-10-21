// Drives the real fleet hub with a member that stops keeping up - the way the
// Pi did on 2026-09-27, when its Things pulls held its one loop and it read
// its hub link only every few seconds: it attaches polled (no reader thread,
// frames read only when poll() runs, as on the board) and polls every 5 s.
// A peer streams at it over a circuit, another opens a circuit to it, and then
// a new member tries to attach. A hub that writes to members inline under its
// roster lock stops for everyone once the slow member's socket is full; this
// reports how long the newcomer took, or that it never got in.
//
//   LD_PRELOAD=/tmp/smallrcv.so aios-relay-stuck-member-drive
// (the preload gives client sockets a small receive window, like the board's)
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "aios/core/ids.hpp"
#include "aios/fleet/relay.hpp"

using namespace aios;
using clk = std::chrono::steady_clock;

static Id128 id_of(uint8_t n) {
  Id128 id{};
  id.hi = 0x5100000000000000ull | n;
  id.lo = 0x1234ull * n;
  return id;
}

int main() {
  const std::string key = "local-drive-key";
  RelayHub hub(key);
  auto port = hub.start(0);
  if (!port.ok()) { std::printf("hub did not start\n"); return 1; }
  const uint16_t p = *port;

  auto slow = std::make_shared<RelayClient>(key, id_of(1));
  if (!slow->attach_polled("127.0.0.1", p).ok()) { std::printf("slow member did not attach\n"); return 1; }
  std::atomic<bool> run{true};
  std::thread slow_loop([&] {
    while (run) {
      slow->poll();
      for (int i = 0; i < 50 && run; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });

  RelayClient a(key, id_of(2));
  if (!a.attach("127.0.0.1", p).ok()) { std::printf("A did not attach\n"); return 1; }
  std::thread flood([&] {
    auto c = a.open(id_of(1));
    if (!c.ok()) { std::printf("A: open failed: %s\n", c.error().message.c_str()); return; }
    std::vector<uint8_t> chunk(64 * 1024, 'x');
    size_t sent = 0;
    for (int i = 0; i < 64 && run; ++i) {
      if (!(*c)->send_all(ByteSpan(chunk.data(), chunk.size()))) break;
      sent += chunk.size();
    }
    std::printf("A: streamed %zu KB at the slow member\n", sent / 1024);
  });
  std::this_thread::sleep_for(std::chrono::seconds(3));

  RelayClient c(key, id_of(3));
  if (!c.attach("127.0.0.1", p).ok()) std::printf("C did not attach\n");
  std::thread opener([&] {
    auto t0 = clk::now();
    auto r = c.open(id_of(1));
    std::printf("C: open to the slow member %s after %lld ms\n", r.ok() ? "answered" : ("failed (" + r.error().message + ")").c_str(),
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count());
  });
  std::this_thread::sleep_for(std::chrono::seconds(3));

  std::atomic<int> d_ok{-1};
  std::atomic<long long> d_ms{0};
  std::thread newcomer([&] {
    RelayClient d(key, id_of(4));
    auto t0 = clk::now();
    auto r = d.attach("127.0.0.1", p);
    d_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count();
    d_ok = r.ok() ? 1 : 0;
    d.stop();
  });
  for (int i = 0; i < 300 && d_ok < 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  if (d_ok < 0) std::printf("NEWCOMER: still not attached after 30 s - the hub is stuck behind the slow member\n");
  else std::printf("NEWCOMER: %s in %lld ms; hub members now: %zu\n", d_ok ? "attached" : "refused", d_ms.load(), hub.attached());
  std::fflush(stdout);
  // A stuck hub never lets these threads finish; the verdict is already out.
  std::_Exit(d_ok == 1 ? 0 : 2);
}
